//
// BluezClient: the adapter, the device list, discovery and the pairing's state machine over a BluezBus.
//
#include "bluez_client.h"

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

const char *btPairStageName(BtPairStage stage) {
    switch (stage) {
    case BtPairStage::Idle:
        return "Idle";
    case BtPairStage::Discovering:
        return "Discovering";
    case BtPairStage::Trusting:
        return "Trusting";
    case BtPairStage::Pairing:
        return "Pairing";
    case BtPairStage::WaitingForPaired:
        return "WaitingForPaired";
    case BtPairStage::Connecting:
        return "Connecting";
    case BtPairStage::Done:
        return "Done";
    case BtPairStage::Failed:
        return "Failed";
    }
    return "?";
}

//*******************************
// BluezClient
//*******************************
const char *const BluezClient::AgentPath = "/org/autobleem/pscbios/agent";
const char *const BluezClient::AgentCapability = "NoInputNoOutput";
const char *const BluezClient::DefaultPowerSupplyDir = "/sys/class/power_supply";

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
// BluezClient::fail
//*******************************
// lastError() = what + the D-Bus error, logged; always false, for `return fail(...)`
bool BluezClient::fail(const string &what, const BusError &error) {
    lastBusError_ = error;
    lastError_ = error.empty() ? what : what + ": " + error.text();
    PLOG_WARNING << "bluetooth: " << lastError_;
    return false;
}

//*******************************
// BluezClient::refresh / findDevice
//*******************************
bool BluezClient::refresh() {
    DbusManagedObjects objects;
    BusError error;
    if (!bus_->getManagedObjects(objects, error)) {
        adapter_ = BluezAdapter();
        devices_.clear();
        return fail("BlueZ does not answer", error);
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
        return fail("no Bluetooth adapter");
    BusError error;
    if (!bus_->setBool(adapter_.path, Adapter1, "Powered", on, error))
        return fail(string("cannot power the adapter ") + (on ? "on" : "off"), error);
    adapter_.powered = on;
    PLOG_INFO << "bluetooth: " << adapter_.hciName() << " powered " << (on ? "on" : "off");
    return true;
}

bool BluezClient::startDiscovery() {
    if (!hasAdapter() && !refresh())
        return false;
    if (!hasAdapter())
        return fail("no Bluetooth adapter");
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
        return fail("cannot start the discovery", error);
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

bool BluezClient::scan(int durationMs, const function<void()> &progress) {
    lastError_.clear();
    lastBusError_.clear();
    if (!startDiscovery())
        return false;
    auto start = chrono::steady_clock::now();
    while (msSince(start) < durationMs) {
        bus_->pump(50);
        if (progress)
            progress();
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
        return fail(mac + " is not known");
    BusError error;
    if (!bus_->callWithPath(adapter_.path, Adapter1, "RemoveDevice", device->path, error))
        return fail("cannot remove " + mac, error);
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
        return fail(mac + " is not known");
    BusError error;
    if (!bus_->call(device->path, Device1, "Disconnect", error))
        return fail("cannot disconnect " + mac, error);
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
            fail("no Bluetooth adapter");
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
        fail("cannot register the pairing agent", error);
        stage_ = BtPairStage::Failed;
        return false;
    }
    agentRegistered_ = true;
    error.clear();
    if (!bus_->callWithPath(AgentManagerPath, AgentManager1, "RequestDefaultAgent", AgentPath, error)) {
        fail("cannot make the pairing agent the default", error);
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
        fail("pairing " + pairMac_ + " cancelled");
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
            fail(pairMac_ + " is gone");
        } else if (!bus_->setBool(device->path, Device1, "Trusted", true, error)) {
            fail("cannot trust " + pairMac_, error);
        } else if (!bus_->startAsync(device->path, Device1, "Pair", timeouts_.pairMs, error)) {
            fail("cannot pair " + pairMac_, error);
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
            fail(pairMac_ + " was not found - is it in pairing mode?");
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
    fail("pairing " + pairMac_ + " failed", error);
    stage_ = BtPairStage::Failed;
    finishPair();
}

void BluezClient::pumpWaitingForPaired() {
    if (!pollDue()) {
        if (msSince(stageStart_) >= timeouts_.pairedWaitMs) {
            fail(pairMac_ + " did not become paired");
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
            fail("paired, but cannot connect " + pairMac_, error);
            enter(BtPairStage::Done);
            finishPair();
            return;
        }
        enter(BtPairStage::Connecting);
        return;
    }
    if (msSince(stageStart_) >= timeouts_.pairedWaitMs) {
        fail(pairMac_ + " did not become paired");
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
        fail("paired, but the connection failed", error);
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
