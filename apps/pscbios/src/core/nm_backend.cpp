//
// NmBackend: nmcli, timedatectl and bluetoothctl for a Raspberry Pi and the PC stick.
//
#include "nm_backend.h"
#include "core/main.h"
#include "core/services/environment.h"
#include "core/services/system.h"

#include <ableem/engine/log.h>

#include <algorithm>
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
// bluetoothctl waits for ever for a bluetoothd that is not running ("Waiting to connect to bluetoothd...")
const string Bluetoothctl = "timeout 5 bluetoothctl";

// what bluetoothctl colours its lines with, gone
string withoutAnsi(const string &text) {
    string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && !(text[i] >= '@' && text[i] <= '~'))
                i++;
            continue;
        }
        if (text[i] != '\x01' && text[i] != '\x02') // readline's invisible-text markers
            out += text[i];
    }
    return out;
}

vector<string> sortedUnique(vector<string> list) {
    list.erase(remove(list.begin(), list.end(), string()), list.end());
    sort(list.begin(), list.end());
    list.erase(unique(list.begin(), list.end()), list.end());
    return list;
}
} // namespace

//*******************************
// NmBackend::NmBackend
//*******************************
NmBackend::NmBackend()
    : run_(runViaSystem), runLines_(runLinesViaSystem),
      nmcli_(DirEntry::exists("/usr/bin/nmcli") || DirEntry::exists("/bin/nmcli")),
      btHelper_(Env::getAppDir() + sep + "bt") {}

NmBackend::NmBackend(Runner run, LinesRunner runLines, bool nmcli, string btHelper)
    : run_(run), runLines_(runLines), nmcli_(nmcli), btHelper_(std::move(btHelper)) {}

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

string NmBackend::controllerName(const vector<string> &showLines) {
    string address, name;
    bool powered = true;
    for (const string &raw : showLines) {
        string line = Strings::trim(withoutAnsi(raw));
        if (line.compare(0, 11, "Controller ") == 0 && address.empty()) {
            address = line.substr(11);
            address = address.substr(0, address.find(' '));
        } else if (line.compare(0, 6, "Name: ") == 0 && name.empty()) {
            name = Strings::trim(line.substr(6));
        } else if (line.compare(0, 9, "Powered: ") == 0) {
            powered = Strings::trim(line.substr(9)) == "yes";
        }
    }
    if (address.empty())
        return "";
    string text = name.empty() ? address : name + " " + address;
    return powered ? text : text + " (off)";
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
// NmBackend::devices / resolve
//*******************************
vector<NmBackend::Device> NmBackend::devices() {
    if (!nmcli_)
        return {};
    return parseDevices(runLines_("nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null"));
}

string NmBackend::resolve(const string &iface) {
    vector<Device> list = devices();
    for (const Device &d : list)
        if (d.name == iface)
            return d.name;
    string type = iface.compare(0, 4, "wlan") == 0 ? "wifi" : iface.compare(0, 3, "eth") == 0 ? "ethernet" : "";
    for (const Device &d : list)
        if (!type.empty() && d.type == type)
            return d.name;
    return "";
}

//*******************************
// network
//*******************************
bool NmBackend::interfaceFound(const string &iface) {
    return !resolve(iface).empty();
}

bool NmBackend::wlanOn() {
    // "unavailable": the radio is off (or rfkill-blocked); "unmanaged": NetworkManager does not drive it
    for (const Device &d : devices())
        if (d.type == "wifi")
            return d.state != "unavailable" && d.state != "unmanaged";
    return false;
}

bool NmBackend::isUp(const string &iface) {
    string name = resolve(iface);
    for (const Device &d : devices())
        if (d.name == name)
            return d.state == "connected";
    return false;
}

string NmBackend::ipOf(const string &iface) {
    string name = resolve(iface);
    if (name.empty())
        return "";
    return firstAddress(run_("nmcli -g IP4.ADDRESS device show " + shellQuoted(name) + " 2>/dev/null"));
}

vector<string> NmBackend::scanSsids() {
    if (resolve("wlan0").empty())
        return {};
    vector<string> ssids;
    for (const string &line : runLines_("nmcli -t -f SSID device wifi list --rescan yes 2>/dev/null"))
        ssids.push_back(splitTerse(line)[0]); // a hidden network has no name: dropped
    return sortedUnique(ssids);
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

void NmBackend::configureWifi(const string &ssid, const string &password, const string &) {
    pendingSsid_ = ssid;
    pendingPassword_ = password;
}

// the password goes through the environment, not the command line: the command is logged
void NmBackend::restartNetwork() {
    string device = resolve("wlan0");
    if (device.empty()) {
        PLOG_WARNING << "no Wi-Fi device to connect with";
        return;
    }
    if (pendingSsid_.empty()) {
        run_("nmcli --wait 30 device connect " + shellQuoted(device) + " 2>&1");
        return;
    }
    string cmd = "nmcli --wait 30 device wifi connect " + shellQuoted(pendingSsid_);
    if (!pendingPassword_.empty())
        cmd += string(" password \"$") + PasswordVariable + "\"";
    cmd += " ifname " + shellQuoted(device) + " 2>&1";
#ifndef _WIN32
    if (!pendingPassword_.empty())
        setenv(PasswordVariable, pendingPassword_.c_str(), 1);
#endif
    string answer = run_(cmd);
#ifndef _WIN32
    unsetenv(PasswordVariable);
#endif
    PLOG_INFO << "nmcli: " << answer;
    pendingSsid_.clear();
    pendingPassword_.clear();
}

//*******************************
// bluetooth
//*******************************
bool NmBackend::btUp() {
    return !btName().empty();
}

string NmBackend::btName() {
    return controllerName(runLines_(Bluetoothctl + " show 2>/dev/null"));
}

// the bt helper switches the adapter on (and out of rfkill's block) before a scan or a pairing
vector<BtDevice> NmBackend::btScan() {
    if (!btUp())
        return {};
    return parseBtHelperLines(runLines_("sh " + shellQuoted(btHelper_) + " scan"), false);
}

vector<BtDevice> NmBackend::btPairedDevices() {
    if (!btUp())
        return {};
    return parseBtHelperLines(runLines_("sh " + shellQuoted(btHelper_) + " paired"), true);
}

bool NmBackend::btPair(const string &mac) {
    return run_("sh " + shellQuoted(btHelper_) + " pair " + shellQuoted(mac)) == "ok";
}

bool NmBackend::btRemove(const string &mac) {
    return run_("sh " + shellQuoted(btHelper_) + " remove " + shellQuoted(mac)) == "ok";
}

//*******************************
// time
//*******************************
string NmBackend::timezone() {
    return run_("timedatectl show -p Timezone --value 2>/dev/null");
}

void NmBackend::setTimezone(const string &zone) {
    run_("timedatectl set-timezone " + shellQuoted(zone) + " 2>&1");
}

vector<string> NmBackend::listTimezones() {
    return sortedUnique(runLines_("timedatectl list-timezones 2>/dev/null"));
}
