//
// BluezClient: the adapter, the device list, discovery and the pairing's state machine over a BluezBus.
//
#include "bluez_client.h"
#include "core/main.h"

#include <ableem/engine/log.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <utility>

using namespace std;

namespace {
const char *const Adapter1 = "org.bluez.Adapter1";
const char *const Device1 = "org.bluez.Device1";
const char *const Battery1 = "org.bluez.Battery1";
const char *const AgentManager1 = "org.bluez.AgentManager1";
const char *const AgentManagerPath = "/org/bluez";

string upper(string text) {
    for (char &c : text)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return text;
}

string lower(string text) {
    for (char &c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

bool endsWith(const string &text, const string &end) {
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

// a property of the given type, or the fallback
string textOf(const DbusProperties &props, const string &name) {
    auto it = props.find(name);
    if (it == props.end())
        return "";
    if (it->second.type == DbusValue::Type::String || it->second.type == DbusValue::Type::ObjectPath)
        return it->second.text;
    return "";
}

bool boolOf(const DbusProperties &props, const string &name) {
    auto it = props.find(name);
    return it != props.end() && it->second.type == DbusValue::Type::Bool && it->second.boolean;
}

bool intOf(const DbusProperties &props, const string &name, int64_t &out) {
    auto it = props.find(name);
    if (it == props.end() || it->second.type != DbusValue::Type::Int)
        return false;
    out = it->second.integer;
    return true;
}

string readLine(const string &path) {
    ifstream in(path);
    string line;
    if (!getline(in, line))
        return "";
    while (!line.empty() && isspace(static_cast<unsigned char>(line.back())))
        line.pop_back();
    return line;
}
} // namespace

//*******************************
// DbusValue / BusError / AgentReply
//*******************************
DbusValue DbusValue::ofBool(bool value) {
    DbusValue v;
    v.type = Type::Bool;
    v.boolean = value;
    return v;
}

DbusValue DbusValue::ofInt(int64_t value) {
    DbusValue v;
    v.type = Type::Int;
    v.integer = value;
    return v;
}

DbusValue DbusValue::ofString(const string &value) {
    DbusValue v;
    v.type = Type::String;
    v.text = value;
    return v;
}

DbusValue DbusValue::ofPath(const string &value) {
    DbusValue v;
    v.type = Type::ObjectPath;
    v.text = value;
    return v;
}

string BusError::text() const {
    if (name.empty())
        return message;
    if (message.empty())
        return name;
    return name + ": " + message;
}

AgentReply AgentReply::rejected(const string &why) {
    AgentReply reply;
    reply.ok = false;
    reply.errorName = "org.bluez.Error.Rejected";
    reply.errorMessage = why;
    return reply;
}

//*******************************
// BluezAdapter / BluezDevice
//*******************************
string BluezAdapter::hciName() const {
    size_t slash = path.rfind('/');
    return slash == string::npos ? path : path.substr(slash + 1);
}

string BluezDevice::displayName() const {
    if (!name.empty())
        return name;
    if (!alias.empty())
        return alias;
    return address;
}

//*******************************
// BluezClient
//*******************************
const char *const BluezClient::AgentPath = "/org/autobleem/pscbios/agent";
const char *const BluezClient::AgentCapability = "NoInputNoOutput";
const char *const BluezClient::DefaultPowerSupplyDir = "/sys/class/power_supply";
const char *const BluezClient::CancelledError = "org.autobleem.Error.Cancelled";

BluezClient::BluezClient(unique_ptr<BluezBus> bus, string powerSupplyDir, BluezTimeouts timeouts)
    : bus_(std::move(bus)), powerSupplyDir_(std::move(powerSupplyDir)), timeouts_(timeouts) {}

BluezClient::~BluezClient() {
    if (stage_ != BtPairStage::Idle && stage_ != BtPairStage::Done && stage_ != BtPairStage::Failed)
        cancelPair();
    else
        finishPair();
    if (ourDiscovery_)
        stopDiscovery();
}

//*******************************
// BluezClient::parseAdapter / parseDevices
//*******************************
bool BluezClient::parseAdapter(const DbusManagedObjects &objects, BluezAdapter &out) {
    out = BluezAdapter();
    // the map is ordered by path: hci0 before hci1
    for (const auto &object : objects) {
        auto adapter = object.second.find(Adapter1);
        if (adapter == object.second.end())
            continue;
        const DbusProperties &props = adapter->second;
        out.path = object.first;
        out.address = textOf(props, "Address");
        out.name = textOf(props, "Name");
        out.alias = textOf(props, "Alias");
        out.powered = boolOf(props, "Powered");
        out.discovering = boolOf(props, "Discovering");
        return true;
    }
    return false;
}

vector<BluezDevice> BluezClient::parseDevices(const DbusManagedObjects &objects, const string &adapterPath) {
    vector<BluezDevice> devices;
    for (const auto &object : objects) {
        auto device = object.second.find(Device1);
        if (device == object.second.end())
            continue;
        const DbusProperties &props = device->second;
        string adapter = textOf(props, "Adapter");
        if (!adapterPath.empty() && adapter != adapterPath)
            continue;
        BluezDevice d;
        d.path = object.first;
        d.address = upper(textOf(props, "Address"));
        if (d.address.empty())
            continue;
        d.name = textOf(props, "Name");
        d.alias = textOf(props, "Alias");
        d.paired = boolOf(props, "Paired");
        d.trusted = boolOf(props, "Trusted");
        d.connected = boolOf(props, "Connected");
        d.modalias = textOf(props, "Modalias");
        d.icon = textOf(props, "Icon");
        int64_t value = 0;
        if (intOf(props, "Class", value))
            d.deviceClass = static_cast<uint32_t>(value);
        if (intOf(props, "RSSI", value)) {
            d.hasRssi = true;
            d.rssi = static_cast<int>(value);
        }
        auto battery = object.second.find(Battery1);
        if (battery != object.second.end() && intOf(battery->second, "Percentage", value))
            d.batteryPercent = static_cast<int>(value);
        devices.push_back(d);
    }
    sort(devices.begin(), devices.end(),
         [](const BluezDevice &a, const BluezDevice &b) { return a.address < b.address; });
    return devices;
}

//*******************************
// BluezClient::readSysfsBattery / devicePathSuffix
//*******************************
BtBattery BluezClient::readSysfsBattery(const string &powerSupplyDir, const string &mac) {
    BtBattery battery;
    string address = lower(mac);
    // hid-sony (DualShock 3/4) and hid-playstation (DualSense, and the DualShock 4 on newer kernels)
    for (const string &prefix : {string("sony_controller_battery_"), string("ps-controller-battery-")}) {
        string dir = powerSupplyDir + "/" + prefix + address;
        string capacity = readLine(dir + "/capacity");
        if (capacity.empty())
            continue;
        char *end = nullptr;
        long percent = strtol(capacity.c_str(), &end, 10);
        if (end == capacity.c_str())
            continue;
        battery.percent = static_cast<int>(max(0L, min(100L, percent)));
        battery.status = readLine(dir + "/status");
        return battery;
    }
    return battery;
}

string BluezClient::devicePathSuffix(const string &mac) {
    string suffix = "/dev_" + upper(mac);
    replace(suffix.begin(), suffix.end(), ':', '_');
    return suffix;
}

//*******************************
// BluezClient::reasonFor
//*******************************
string BluezClient::reasonFor(const BusError &error) {
    const string &n = error.name;
    const string &m = error.message;
    if (n == CancelledError)
        return _("cancelled");
    if (n == "org.bluez.Error.AuthenticationFailed")
        return _("the controller refused the pairing");
    if (n == "org.bluez.Error.AuthenticationRejected")
        return _("the pairing was rejected");
    if (n == "org.bluez.Error.AuthenticationCanceled")
        return _("the pairing was cancelled");
    if (n == "org.bluez.Error.AuthenticationTimeout" || n == "org.bluez.Error.ConnectionAttemptFailed")
        return _("the controller did not answer - is it in pairing mode?");
    if (n == "org.bluez.Error.Failed" &&
        (m.find("page-timeout") != string::npos || m.find("Host is down") != string::npos))
        return _("the controller is off or out of range");
    if (n == "org.bluez.Error.NotReady")
        return _("the Bluetooth adapter is not ready");
    if (n == "org.bluez.Error.InProgress")
        return _("Bluetooth is busy with something else");
    if (n == "org.bluez.Error.DoesNotExist" || n == "org.freedesktop.DBus.Error.UnknownObject")
        return _("the controller is not known");
    if (n == "org.freedesktop.DBus.Error.NoReply" || n == "org.freedesktop.DBus.Error.Timeout" ||
        n == "org.freedesktop.DBus.Error.TimedOut")
        return _("BlueZ did not answer in time");
    if (n == "org.freedesktop.DBus.Error.ServiceUnknown" || n == "org.freedesktop.DBus.Error.NameHasNoOwner")
        return _("BlueZ (bluetoothd) is not running");
    if (n == "org.freedesktop.DBus.Error.Disconnected" || n == "org.freedesktop.DBus.Error.NoServer")
        return _("the system bus went away");
    if (n == "org.freedesktop.DBus.Error.AccessDenied")
        return _("the system bus refused PSC-Bios");
    return "";
}

//*******************************
// BluezClient::fail
//*******************************
bool BluezClient::fail(const string &reason, const BusError &error, const string &subject) {
    lastBusError_ = error;
    const string known = reasonFor(error);
    lastError_ = (known.empty() ? reason : known) + (error.empty() ? string() : " (" + error.text() + ")");
    PLOG_WARNING << "bluetooth: " << (subject.empty() ? string() : subject + ": ") << reason
                 << (error.empty() ? string() : " - " + error.text());
    return false;
}

//*******************************
// BluezClient::refresh / findDevice
//*******************************
bool BluezClient::refresh(int timeoutMs) {
    DbusManagedObjects objects;
    BusError error;
    const int previousTimeout = bus_->callTimeout();
    if (timeoutMs > 0)
        bus_->setCallTimeout(timeoutMs);
    const bool answered = bus_->getManagedObjects(objects, error);
    if (timeoutMs > 0)
        bus_->setCallTimeout(previousTimeout);
    if (!answered) {
        adapter_ = BluezAdapter();
        devices_.clear();
        return fail(_("BlueZ does not answer"), error);
    }
    if (!parseAdapter(objects, adapter_)) {
        devices_.clear();
        return true;
    }
    devices_ = parseDevices(objects, adapter_.path);
    return true;
}

const BluezDevice *BluezClient::findDevice(const string &mac) const {
    string address = upper(mac);
    for (const BluezDevice &d : devices_)
        if (d.address == address)
            return &d;
    return nullptr;
}

//*******************************
// BluezClient::setPowered / startDiscovery / stopDiscovery / scan
//*******************************
bool BluezClient::setPowered(bool on) {
    if (!hasAdapter() && !refresh())
        return false;
    if (!hasAdapter())
        return fail(_("no Bluetooth adapter"));
    BusError error;
    if (!bus_->setBool(adapter_.path, Adapter1, "Powered", on, error))
        return fail(on ? _("the Bluetooth adapter cannot be switched on")
                       : _("the Bluetooth adapter cannot be switched off"),
                    error);
    adapter_.powered = on;
    PLOG_INFO << "bluetooth: " << adapter_.hciName() << " powered " << (on ? "on" : "off");
    return true;
}

bool BluezClient::startDiscovery() {
    if (!hasAdapter() && !refresh())
        return false;
    if (!hasAdapter())
        return fail(_("no Bluetooth adapter"));
    if (!adapter_.powered && !setPowered(true))
        return false;
    BusError error;
    // the gamepads are classic Bluetooth; a filter BlueZ refuses (an old one) only means LE devices too
    if (!bus_->setDiscoveryFilter(adapter_.path, "bredr", error)) {
        PLOG_WARNING << "bluetooth: SetDiscoveryFilter: " << error.text();
    }
    error.clear();
    if (!bus_->call(adapter_.path, Adapter1, "StartDiscovery", error)) {
        if (error.name == "org.bluez.Error.InProgress") {
            PLOG_INFO << "bluetooth: a discovery is already running";
            return true;
        }
        return fail(_("the search for controllers cannot start"), error);
    }
    ourDiscovery_ = true;
    PLOG_INFO << "bluetooth: discovery started on " << adapter_.hciName();
    return true;
}

bool BluezClient::stopDiscovery() {
    if (!ourDiscovery_)
        return true;
    ourDiscovery_ = false;
    BusError error;
    if (!bus_->call(adapter_.path, Adapter1, "StopDiscovery", error)) {
        // NotReady/Failed "No discovery started": it ended by itself
        PLOG_INFO << "bluetooth: StopDiscovery: " << error.text();
        return false;
    }
    PLOG_INFO << "bluetooth: discovery stopped";
    return true;
}

bool BluezClient::scan(int durationMs, const function<bool()> &keepGoing) {
    lastError_.clear();
    lastBusError_.clear();
    if (!startDiscovery())
        return false;
    auto start = chrono::steady_clock::now();
    while (msSince(start) < durationMs) {
        bus_->pump(50);
        if (keepGoing && !keepGoing()) {
            PLOG_INFO << "bluetooth: the scan was stopped after " << msSince(start) << " ms";
            break; // what was found so far is the answer
        }
    }
    stopDiscovery();
    return refresh();
}

//*******************************
// BluezClient::removeDevice / disconnect / battery
//*******************************
bool BluezClient::removeDevice(const string &mac) {
    lastError_.clear();
    lastBusError_.clear();
    if (!refresh())
        return false;
    const BluezDevice *device = findDevice(mac);
    if (device == nullptr)
        return fail(_("the controller is not known") + " (" + mac + ")", BusError(), mac);
    BusError error;
    if (!bus_->callWithPath(adapter_.path, Adapter1, "RemoveDevice", device->path, error))
        return fail(_("the controller could not be removed"), error, mac);
    PLOG_INFO << "bluetooth: removed " << mac;
    refresh();
    return true;
}

bool BluezClient::disconnect(const string &mac) {
    lastError_.clear();
    lastBusError_.clear();
    if (findDevice(mac) == nullptr && !refresh())
        return false;
    const BluezDevice *device = findDevice(mac);
    if (device == nullptr)
        return fail(_("the controller is not known") + " (" + mac + ")", BusError(), mac);
    BusError error;
    if (!bus_->call(device->path, Device1, "Disconnect", error))
        return fail(_("the controller could not be disconnected"), error, mac);
    return true;
}

BtBattery BluezClient::battery(const string &mac) const {
    BtBattery battery = readSysfsBattery(powerSupplyDir_, mac);
    if (battery.known())
        return battery;
    const BluezDevice *device = findDevice(mac);
    if (device != nullptr && device->batteryPercent >= 0)
        battery.percent = device->batteryPercent;
    return battery;
}

//*******************************
// BluezClient: the pairing
//*******************************
long long BluezClient::msSince(chrono::steady_clock::time_point start) const {
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
}

void BluezClient::enter(BtPairStage stage) {
    PLOG_INFO << "bluetooth: pairing " << pairMac_ << ": " << btPairStageName(stage);
    stage_ = stage;
    stageStart_ = chrono::steady_clock::now();
    lastPoll_ = stageStart_ - chrono::milliseconds(timeouts_.pollMs); // the first pump reads the list at once
}

// true once every pollMs - the device list is read again then
bool BluezClient::pollDue() {
    if (msSince(lastPoll_) < timeouts_.pollMs)
        return false;
    lastPoll_ = chrono::steady_clock::now();
    return true;
}

bool BluezClient::isPairingDevice(const string &devicePath) const {
    if (pairMac_.empty() || stage_ == BtPairStage::Idle || stage_ == BtPairStage::Done || stage_ == BtPairStage::Failed)
        return false;
    return endsWith(devicePath, devicePathSuffix(pairMac_)) &&
           (adapter_.path.empty() || devicePath.compare(0, adapter_.path.size() + 1, adapter_.path + "/") == 0);
}

AgentReply BluezClient::answerAgent(const AgentRequest &request) {
    const string &method = request.method;
    if (method == "Release") {
        // BlueZ let go of us (bluetoothd stopping, or our own UnregisterAgent)
        PLOG_INFO << "bluetooth: agent released";
        agentRegistered_ = false;
        return AgentReply::success();
    }
    if (method == "Cancel") {
        PLOG_INFO << "bluetooth: agent request cancelled by BlueZ";
        return AgentReply::success();
    }
    if (method == "DisplayPinCode" || method == "DisplayPasskey")
        return AgentReply::success(); // nothing to show on a pad
    if (!isPairingDevice(request.device)) {
        PLOG_WARNING << "bluetooth: agent " << method << " for " << request.device << " rejected (pairing "
                     << (pairMac_.empty() ? "nothing" : pairMac_) << ")";
        return AgentReply::rejected("PSC-Bios is not pairing this device");
    }
    PLOG_INFO << "bluetooth: agent " << method << " for " << pairMac_ << " accepted";
    if (method == "RequestPinCode") {
        AgentReply reply;
        reply.pinCode = "0000"; // a pad has no keypad; legacy-pairing ones use 0000
        return reply;
    }
    if (method == "RequestPasskey")
        return AgentReply::success(); // passkey 0
    if (method == "RequestConfirmation" || method == "RequestAuthorization" || method == "AuthorizeService")
        return AgentReply::success();
    return AgentReply::rejected("unknown agent method " + method);
}

bool BluezClient::beginPair(const string &mac) {
    if (stage_ != BtPairStage::Idle && stage_ != BtPairStage::Done && stage_ != BtPairStage::Failed)
        cancelPair();
    lastError_.clear();
    lastBusError_.clear();
    pairMac_ = upper(mac);
    pairConnected_ = false;
    stage_ = BtPairStage::Idle;
    PLOG_INFO << "bluetooth: pairing " << pairMac_;

    if (!refresh() || !hasAdapter()) {
        if (lastError_.empty())
            fail(_("no Bluetooth adapter"), BusError(), pairMac_);
        stage_ = BtPairStage::Failed;
        return false;
    }
    if (!adapter_.powered && !setPowered(true)) {
        stage_ = BtPairStage::Failed;
        return false;
    }
    // our agent, the default one, before anything can ask
    BusError error;
    if (!bus_->registerAgent(
            AgentPath, AgentCapability, [this](const AgentRequest &request) { return answerAgent(request); }, error)) {
        fail(_("the pairing agent cannot be registered"), error, pairMac_);
        stage_ = BtPairStage::Failed;
        return false;
    }
    agentRegistered_ = true;
    error.clear();
    if (!bus_->callWithPath(AgentManagerPath, AgentManager1, "RequestDefaultAgent", AgentPath, error)) {
        fail(_("the pairing agent cannot be registered"), error, pairMac_);
        finishPair();
        stage_ = BtPairStage::Failed;
        return false;
    }

    if (findDevice(pairMac_) != nullptr) {
        enter(BtPairStage::Trusting);
        return true;
    }
    // bluetoothd forgets an unpaired device soon after a discovery ends: look for it again
    if (!startDiscovery()) {
        finishPair();
        stage_ = BtPairStage::Failed;
        return false;
    }
    enter(BtPairStage::Discovering);
    return true;
}

void BluezClient::finishPair() {
    if (ourDiscovery_)
        stopDiscovery();
    if (!agentRegistered_)
        return;
    agentRegistered_ = false;
    BusError error;
    if (!bus_->unregisterAgent(AgentPath, error)) {
        PLOG_WARNING << "bluetooth: UnregisterAgent: " << error.text();
    } else {
        PLOG_INFO << "bluetooth: pairing agent unregistered";
    }
}

void BluezClient::cancelPair() {
    bus_->cancelAsync();
    if (stage_ != BtPairStage::Idle && stage_ != BtPairStage::Done && stage_ != BtPairStage::Failed) {
        fail(_("cancelled"), BusError(), pairMac_);
        stage_ = BtPairStage::Failed;
    }
    finishPair();
}

BtPairStage BluezClient::pump(int timeoutMs) {
    bus_->pump(timeoutMs);
    switch (stage_) {
    case BtPairStage::Discovering:
        pumpDiscovering();
        break;
    case BtPairStage::Trusting: {
        BusError error;
        const BluezDevice *device = findDevice(pairMac_);
        if (device == nullptr) {
            fail(_("the controller is not known") + " (" + pairMac_ + ")", BusError(), pairMac_);
        } else if (!bus_->setBool(device->path, Device1, "Trusted", true, error)) {
            fail(_("the pairing failed"), error, pairMac_);
        } else if (!bus_->startAsync(device->path, Device1, "Pair", timeouts_.pairMs, error)) {
            fail(_("the pairing failed"), error, pairMac_);
        } else {
            enter(BtPairStage::Pairing);
            break;
        }
        stage_ = BtPairStage::Failed;
        finishPair();
        break;
    }
    case BtPairStage::Pairing:
        pumpPairing();
        break;
    case BtPairStage::WaitingForPaired:
        pumpWaitingForPaired();
        break;
    case BtPairStage::Connecting:
        pumpConnecting();
        break;
    case BtPairStage::Idle:
    case BtPairStage::Done:
    case BtPairStage::Failed:
        break;
    }
    return stage_;
}

void BluezClient::pumpDiscovering() {
    if (pollDue() && refresh() && findDevice(pairMac_) != nullptr) {
        stopDiscovery();
        enter(BtPairStage::Trusting);
        return;
    }
    if (!lastError_.empty() || msSince(stageStart_) >= timeouts_.discoverMs) {
        if (lastError_.empty())
            fail(_("the controller was not found - is it in pairing mode?"), BusError(), pairMac_);
        stage_ = BtPairStage::Failed;
        finishPair();
    }
}

void BluezClient::pumpPairing() {
    BusError error;
    BluezBus::AsyncState state = bus_->asyncState(error);
    if (state == BluezBus::AsyncState::Pending) {
        // libdbus times a pending call out itself; this is the backstop
        if (msSince(stageStart_) < timeouts_.pairMs + 2000)
            return;
        bus_->cancelAsync();
        error.name = "org.freedesktop.DBus.Error.NoReply";
        error.message = "no answer to Pair";
        state = BluezBus::AsyncState::Failed;
    }
    if (state == BluezBus::AsyncState::Succeeded || error.name == "org.bluez.Error.AlreadyExists") {
        if (!error.empty()) {
            PLOG_INFO << "bluetooth: " << pairMac_ << " was paired already";
        }
        enter(BtPairStage::WaitingForPaired);
        return;
    }
    fail(_("the pairing failed"), error, pairMac_);
    stage_ = BtPairStage::Failed;
    finishPair();
}

void BluezClient::pumpWaitingForPaired() {
    if (!pollDue()) {
        if (msSince(stageStart_) >= timeouts_.pairedWaitMs) {
            fail(_("the pairing failed"), BusError{"", "Paired did not become true"}, pairMac_);
            stage_ = BtPairStage::Failed;
            finishPair();
        }
        return;
    }
    if (!refresh()) {
        stage_ = BtPairStage::Failed;
        finishPair();
        return;
    }
    const BluezDevice *device = findDevice(pairMac_);
    if (device != nullptr && device->paired) {
        if (device->connected) {
            pairConnected_ = true;
            enter(BtPairStage::Done);
            finishPair();
            return;
        }
        BusError error;
        if (!bus_->startAsync(device->path, Device1, "Connect", timeouts_.connectMs, error)) {
            // paired all the same: the pad connects itself on its PS button
            fail(_("paired, but the controller did not connect"), error, pairMac_);
            enter(BtPairStage::Done);
            finishPair();
            return;
        }
        enter(BtPairStage::Connecting);
        return;
    }
    if (msSince(stageStart_) >= timeouts_.pairedWaitMs) {
        fail(_("the pairing failed"), BusError{"", "Paired did not become true"}, pairMac_);
        stage_ = BtPairStage::Failed;
        finishPair();
    }
}

void BluezClient::pumpConnecting() {
    BusError error;
    BluezBus::AsyncState state = bus_->asyncState(error);
    if (state == BluezBus::AsyncState::Pending) {
        if (msSince(stageStart_) < timeouts_.connectMs + 2000)
            return;
        bus_->cancelAsync();
        error.name = "org.freedesktop.DBus.Error.NoReply";
        error.message = "no answer to Connect";
        state = BluezBus::AsyncState::Failed;
    }
    if (state == BluezBus::AsyncState::Succeeded || error.name == "org.bluez.Error.AlreadyConnected") {
        pairConnected_ = true;
    } else {
        // paired all the same: lastError() says why it is not connected, the pairing still counts
        fail(_("paired, but the controller did not connect"), error, pairMac_);
    }
    refresh();
    const BluezDevice *device = findDevice(pairMac_);
    if (device != nullptr && device->connected)
        pairConnected_ = true;
    enter(BtPairStage::Done);
    finishPair();
}

bool BluezClient::pair(const string &mac, const function<void(BtPairStage)> &progress) {
    if (!beginPair(mac))
        return false;
    while (stage_ != BtPairStage::Done && stage_ != BtPairStage::Failed) {
        pump(50);
        if (progress)
            progress(stage_);
    }
    return stage_ == BtPairStage::Done;
}
