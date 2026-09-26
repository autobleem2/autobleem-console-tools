//
// NmBackend: nmcli and timedatectl for a Raspberry Pi and the PC stick; Bluetooth over BlueZ/D-Bus (shared
// with NativeBackend - see its comments for the pairing state machine, the retry back-off and the battery).
//
#include "nm_backend.h"
#ifdef PSCBIOS_HAVE_DBUS
#include "bluez_dbus_bus.h"
#endif
#include "core/main.h"
#include "core/services/environment.h"
#include "core/services/system.h"

#include <ableem/engine/log.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

using namespace std;

const char *const NmBackend::PasswordVariable = "AB_WIFI_PSK";

namespace {
string runViaSystem(const string &cmd) {
    return System::execUnixCommand(cmd.c_str());
}
vector<string> runLinesViaSystem(const string &cmd) {
    return System::execUnixCommandLines(cmd);
}

vector<string> sortedUnique(vector<string> list) {
    list.erase(remove(list.begin(), list.end(), string()), list.end());
    sort(list.begin(), list.end());
    list.erase(unique(list.begin(), list.end()), list.end());
    return list;
}

#ifdef PSCBIOS_HAVE_DBUS
unique_ptr<BluezBus> systemBus(BusError &error) {
    return DbusBluezBus::connectSystem(error);
}
#endif
} // namespace

//*******************************
// NmBackend::NmBackend
//*******************************
NmBackend::NmBackend()
    : run_(runViaSystem), runLines_(runLinesViaSystem),
      nmcli_(DirEntry::exists("/usr/bin/nmcli") || DirEntry::exists("/bin/nmcli")),
#ifdef PSCBIOS_HAVE_DBUS
      bluezFactory_(systemBus) {
#else
      bluezFactory_(nullptr) {
#endif
}

NmBackend::NmBackend(Runner run, LinesRunner runLines, bool nmcli, BluezBusFactory bluez)
    : run_(run), runLines_(runLines), nmcli_(nmcli), bluezFactory_(std::move(bluez)) {}

long long NmBackend::steadyMs() {
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

//*******************************
// the parsers
//*******************************
vector<string> NmBackend::splitTerse(const string &line) {
    vector<string> fields(1);
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            fields.back() += line[++i];
        } else if (c == ':') {
            fields.emplace_back();
        } else {
            fields.back() += c;
        }
    }
    return fields;
}

vector<NmBackend::Device> NmBackend::parseDevices(const vector<string> &lines) {
    vector<Device> devices;
    for (const string &line : lines) {
        vector<string> f = splitTerse(line);
        if (f.size() < 3 || f[0].empty())
            continue;
        devices.push_back({f[0], f[1], f[2]});
    }
    return devices;
}

string NmBackend::firstAddress(const string &text) {
    string first = text.substr(0, text.find('|'));
    first = first.substr(0, first.find('/'));
    return Strings::trim(first);
}

string NmBackend::shellQuoted(const string &value) {
    string out = "'";
    for (char c : value) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

//*******************************
// NmBackend::devices / deviceOfType
//*******************************
vector<NmBackend::Device> NmBackend::devices() {
    if (!nmcli_)
        return {};
    return parseDevices(runLines_("nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null"));
}

string NmBackend::deviceOfType(const string &type) {
    string any;
    for (const Device &d : devices()) {
        if (d.type != type)
            continue;
        if (d.state == "connected")
            return d.name;
        if (any.empty())
            any = d.name;
    }
    return any;
}

//*******************************
// network
//*******************************
string NmBackend::wifiInterface() {
    return deviceOfType("wifi");
}

string NmBackend::ethernetInterface() {
    return deviceOfType("ethernet");
}

bool NmBackend::interfaceFound(const string &iface) {
    for (const Device &d : devices())
        if (d.name == iface)
            return true;
    return false;
}

bool NmBackend::isUp(const string &iface) {
    for (const Device &d : devices())
        if (d.name == iface)
            return d.state == "connected";
    return false;
}

string NmBackend::ipOf(const string &iface) {
    if (iface.empty())
        return "";
    return firstAddress(run_("nmcli -g IP4.ADDRESS device show " + shellQuoted(iface) + " 2>/dev/null"));
}

// SIGNAL is a 0-100 percentage; approximated to dBm (0% -> -100 dBm, 100% -> -50 dBm) for signalText()'s
// thresholds - nmcli never reports a real dBm figure
vector<WifiNetwork> NmBackend::scanNetworks() {
    lastError_.clear();
    string iface = wifiInterface();
    if (iface.empty()) {
        lastError_ = _("no WiFi interface");
        return {};
    }
    vector<WifiNetwork> out;
    for (const string &line : runLines_("nmcli -t -f SSID,SIGNAL,SECURITY device wifi list --rescan yes 2>/dev/null")) {
        vector<string> f = splitTerse(line);
        if (f.empty() || f[0].empty())
            continue; // a hidden network has no name: dropped
        WifiNetwork n;
        n.ssid = f[0];
        int percent = f.size() > 1 ? atoi(f[1].c_str()) : 0;
        n.signal = percent / 2 - 100;
        n.flags = f.size() > 2 && f[2] != "--" ? "[" + f[2] + "]" : "";
        out.push_back(n);
    }
    sort(out.begin(), out.end(), [](const WifiNetwork &a, const WifiNetwork &b) { return a.signal > b.signal; });
    return out;
}

string NmBackend::currentSsid() {
    if (!nmcli_)
        return "";
    for (const string &line : runLines_("nmcli -t -f ACTIVE,SSID device wifi list --rescan no 2>/dev/null")) {
        vector<string> f = splitTerse(line);
        if (f.size() >= 2 && f[0] == "yes")
            return f[1];
    }
    return "";
}

void NmBackend::configureWifi(const string &ssid, const string &password) {
    lastError_.clear();
    pendingSsid_ = ssid;
    pendingPassword_ = password;
}

// the password goes through the environment, not the command line: the command is logged. nmcli does the
// whole connection attempt in this one (blocking) call - there is no wpa_supplicant event stream to follow
// afterwards, so beginWifiConnect/pumpWifiConnect just report what happened here
void NmBackend::restartNetwork() {
    lastError_.clear();
    string device = wifiInterface();
    if (device.empty()) {
        lastError_ = _("no WiFi interface");
        return;
    }
    string ssid = pendingSsid_.empty() ? currentSsid() : pendingSsid_;
    string cmd;
    if (pendingSsid_.empty()) {
        cmd = "nmcli --wait 30 device connect " + shellQuoted(device) + " 2>&1";
    } else {
        cmd = "nmcli --wait 30 device wifi connect " + shellQuoted(pendingSsid_);
        if (!pendingPassword_.empty())
            cmd += string(" password \"$") + PasswordVariable + "\"";
        cmd += " ifname " + shellQuoted(device) + " 2>&1";
    }
#ifndef _WIN32
    if (!pendingPassword_.empty())
        setenv(PasswordVariable, pendingPassword_.c_str(), 1);
#endif
    string answer = run_(cmd);
#ifndef _WIN32
    unsetenv(PasswordVariable);
#endif
    PLOG_INFO << "nmcli: " << answer;
    lastConnectSsid_ = ssid;
    lastConnectDetail_ = answer;
    lastConnectOk_ = answer.find("Error:") == string::npos && !answer.empty();
    if (!lastConnectOk_)
        lastError_ = _("the network could not be joined") + (answer.empty() ? string() : " (" + answer + ")");
    pendingSsid_.clear();
    pendingPassword_.clear();
}

bool NmBackend::wifiStatus(WpaStatus &out) {
    out = WpaStatus();
    string iface = wifiInterface();
    if (iface.empty())
        return false;
    out.ipAddress = ipOf(iface);
    out.ssid = currentSsid();
    out.wpaState = isUp(iface) ? "COMPLETED" : "DISCONNECTED";
    return true;
}

// nmcli already finished the attempt (in restartNetwork(), above) by the time this is called: the watch is
// resolved at once, so followConnection()'s pump loop sees it finished on the very first pumpWifiConnect()
void NmBackend::beginWifiConnect(const string &ssid) {
    const long long now = steadyMs();
    watch_ = make_unique<WifiConnectWatch>(ssid, now);
    if (ssid.empty()) {
        watch_->fail(WifiFailure::NoInterface);
        return;
    }
    if (ssid == lastConnectSsid_ && lastConnectOk_) {
        WpaStatus status;
        status.wpaState = "COMPLETED";
        status.ssid = ssid;
        status.ipAddress = ipOf(wifiInterface());
        watch_->onStatus(status, status.ipAddress, now);
    } else if (ssid == lastConnectSsid_) {
        watch_->fail(WifiFailure::AssociationRejected, lastConnectDetail_);
    } else {
        // followConnection() called without a preceding restartNetwork() (e.g. after Write only): report
        // whatever the interface is on now
        WpaStatus status;
        if (wifiStatus(status) && status.ssid == ssid && !status.ipAddress.empty())
            watch_->onStatus(status, status.ipAddress, now);
        else
            watch_->fail(WifiFailure::Timeout);
    }
}

const WifiConnectWatch &NmBackend::pumpWifiConnect() {
    if (!watch_)
        beginWifiConnect(lastConnectSsid_);
    watch_->tick(steadyMs());
    return *watch_;
}

//*******************************
// bluetooth (mirrors NativeBackend's - see its comments)
//*******************************
BluezClient *NmBackend::bluez() {
    if (bluez_)
        return bluez_.get();
    if (!bluezFactory_) {
        btError_ = _("no Bluetooth support");
        return nullptr;
    }
    BusError error;
    unique_ptr<BluezBus> bus = bluezFactory_(error);
    if (!bus) {
        btError_ = _("the system bus cannot be reached") + (error.empty() ? string() : " (" + error.text() + ")");
        return nullptr;
    }
    bus->setWaitHook([this]() { return keepWaiting(); });
    bluez_ = make_unique<BluezClient>(std::move(bus));
    return bluez_.get();
}

void NmBackend::takeBtError() {
    if (!bluez_)
        return;
    btError_ = bluez_->lastError();
    const string &name = bluez_->lastBusError().name;
    if (name == "org.freedesktop.DBus.Error.Disconnected" || name == "org.freedesktop.DBus.Error.NoServer")
        bluez_.reset();
}

BtDevice NmBackend::toBtDevice(const BluezDevice &device) {
    BtDevice d;
    d.mac = device.address;
    d.name = device.displayName();
    d.paired = device.paired;
    d.connected = device.connected;
    if (device.paired && bluez_)
        d.battery = bluez_->battery(device.address);
    return d;
}

bool NmBackend::statusRefresh(BluezClient &client) {
    const long long now = steadyMs();
    if (now < btRetryAt_) {
        btError_ = btStaleError_;
        return false;
    }
    if (client.refresh(1500))
        return true;
    const string name = client.lastBusError().name;
    takeBtError();
    if (name == "org.freedesktop.DBus.Error.NoReply" || name == "org.freedesktop.DBus.Error.Timeout" ||
        name == "org.freedesktop.DBus.Error.TimedOut") {
        btRetryAt_ = now + 15000;
        btStaleError_ = btError_;
    }
    return false;
}

bool NmBackend::btUp() {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return false;
    if (!statusRefresh(*client))
        return false;
    if (!client->hasAdapter()) {
        btError_ = _("no Bluetooth adapter");
        return false;
    }
    if (!client->adapter().powered && !triedPowerOn_) {
        triedPowerOn_ = true;
        if (!client->setPowered(true))
            takeBtError();
    }
    if (!client->adapter().powered && btError_.empty())
        btError_ = _("the Bluetooth adapter is off");
    return client->adapter().powered;
}

string NmBackend::btName() {
    BluezClient *client = bluez();
    if (client == nullptr || !client->hasAdapter())
        return "";
    const BluezAdapter &adapter = client->adapter();
    string alias = adapter.alias.empty() ? adapter.name : adapter.alias;
    return adapter.hciName() + " " + (alias.empty() ? string() : alias + " ") + adapter.address;
}

vector<BtDevice> NmBackend::btScan() {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return {};
    if (!client->scan(8000, [this]() { return keepWaiting(); })) {
        takeBtError();
        return {};
    }
    vector<BtDevice> out;
    for (const BluezDevice &device : client->devices())
        out.push_back(toBtDevice(device));
    return out;
}

vector<BtDevice> NmBackend::btPairedDevices() {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return {};
    if (!statusRefresh(*client))
        return {};
    vector<BtDevice> out;
    for (const BluezDevice &device : client->devices())
        if (device.paired)
            out.push_back(toBtDevice(device));
    return out;
}

bool NmBackend::btBeginPair(const string &mac) {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return false;
    if (!client->beginPair(mac)) {
        takeBtError();
        return false;
    }
    return true;
}

BtPairStage NmBackend::btPumpPair() {
    BluezClient *client = bluez();
    if (client == nullptr)
        return BtPairStage::Failed;
    BtPairStage stage = client->pump();
    if (stage == BtPairStage::Failed)
        takeBtError();
    return stage;
}

BtPairStage NmBackend::btCancelPair() {
    if (!bluez_)
        return BtPairStage::Failed;
    const BtPairStage stage = bluez_->cancelPair();
    takeBtError();
    return stage;
}

bool NmBackend::btPairConnected() const {
    return bluez_ && bluez_->pairConnected();
}

bool NmBackend::btRemove(const string &mac) {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return false;
    if (!client->removeDevice(mac)) {
        takeBtError();
        return false;
    }
    return true;
}

BtBattery NmBackend::btBattery(const string &mac) {
    BluezClient *client = bluez();
    return client != nullptr ? client->battery(mac) : BtBattery();
}

//*******************************
// time
//*******************************
string NmBackend::timezone() {
    return run_("timedatectl show -p Timezone --value 2>/dev/null");
}

void NmBackend::setTimezone(const string &zone) {
    lastError_.clear();
    string answer = run_("timedatectl set-timezone " + shellQuoted(zone) + " 2>&1");
    if (!answer.empty())
        lastError_ = _("the timezone could not be changed") + " (" + answer + ")";
}

vector<string> NmBackend::listTimezones() {
    return sortedUnique(runLines_("timedatectl list-timezones 2>/dev/null"));
}
