//
// NativeBackend: sysfs, getifaddrs, wpa_supplicant's control socket and BlueZ over D-Bus.
//
#include "native_backend.h"
#ifdef PSCBIOS_HAVE_DBUS
#include "bluez_dbus_bus.h"
#endif
#include "core/main.h"
#include "core/services/system.h"

#include <ableem/engine/log.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <dirent.h>
#include <grp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

namespace {
int runViaSystem(const string &exe, const vector<string> &args) {
    return System::runAndWait(exe, args);
}

unique_ptr<BluezBus> systemBus(BusError &error) {
#ifdef PSCBIOS_HAVE_DBUS
    return DbusBluezBus::connectSystem(error);
#else
    error.message = "PSC-Bios was built without libdbus-1";
    return nullptr;
#endif
}
} // namespace

//*******************************
// NativeBackend::NativeBackend
//*******************************
NativeBackend::NativeBackend() : run_(runViaSystem), bluezFactory_(systemBus) {}

NativeBackend::NativeBackend(NativePaths paths, CommandRunner run, BluezBusFactory bluez)
    : paths_(std::move(paths)), run_(std::move(run)), bluezFactory_(std::move(bluez)) {}

NativeBackend::~NativeBackend() = default;

//*******************************
// NativeBackend::kernelInstalled
//*******************************
bool NativeBackend::kernelInstalled() {
    return DirEntry::exists(paths_.kernelMarker);
}

//*******************************
// NativeBackend::interfaces / isWireless / wirelessInterfaces / wifiInterface
//*******************************
vector<string> NativeBackend::interfaces() {
    vector<string> names;
    DIR *dir = opendir(paths_.sysClassNet.c_str());
    if (dir == nullptr)
        return names;
    // every entry: real sysfs has symlinks here, a test tree directories
    for (struct dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        if (entry->d_name[0] != '.')
            names.emplace_back(entry->d_name);
    }
    closedir(dir);
    sort(names.begin(), names.end());
    return names;
}

bool NativeBackend::isWireless(const string &iface) {
    string base = paths_.sysClassNet + sep + iface + sep;
    return DirEntry::exists(base + "wireless") || DirEntry::exists(base + "phy80211");
}

vector<string> NativeBackend::wirelessInterfaces() {
    vector<string> out;
    for (const string &iface : interfaces())
        if (isWireless(iface))
            out.push_back(iface);
    return out;
}

string NativeBackend::wifiInterface() {
    vector<string> wireless = wirelessInterfaces();
    for (const string &iface : wireless)
        if (isUp(iface))
            return iface;
    return wireless.empty() ? "" : wireless.front();
}

//*******************************
// NativeBackend::ethernetInterface
//*******************************
string NativeBackend::ethernetInterface() {
    auto startsWith = [](const string &name, const char *prefix) {
        return name.compare(0, strlen(prefix), prefix) == 0;
    };
    for (const string &iface : interfaces()) {
        if (iface == "lo" || startsWith(iface, "rndis") || startsWith(iface, "bnep") || isWireless(iface))
            continue;
        string type = readSys(iface, "type");
        if (!type.empty() && type != "1") // ARPHRD_ETHER
            continue;
        if (DirEntry::exists(paths_.sysClassNet + sep + iface + sep + "device"))
            return iface;
    }
    return "";
}

//*******************************
// NativeBackend::interfaceFound / wlanOn / isUp / ipOf
//*******************************
bool NativeBackend::interfaceFound(const string &iface) {
    return !iface.empty() && DirEntry::exists(paths_.sysClassNet + sep + iface);
}

bool NativeBackend::wlanOn() {
    vector<string> wireless = wirelessInterfaces();
    return any_of(wireless.begin(), wireless.end(), [this](const string &iface) { return isUp(iface); });
}

string NativeBackend::readSys(const string &iface, const string &file) {
    ifstream in(paths_.sysClassNet + sep + iface + sep + file);
    string value;
    getline(in, value);
    return Strings::trim(value);
}

// up = administratively up (IFF_UP in `flags`, what `ifconfig` without -a lists - as abnet's is_up did);
// a WiFi interface still looking for its network is up but its operstate is "dormant" or "down"
bool NativeBackend::isUp(const string &iface) {
    if (!interfaceFound(iface))
        return false;
    string flags = readSys(iface, "flags");
    if (!flags.empty())
        return (strtol(flags.c_str(), nullptr, 16) & IFF_UP) != 0;
    return readSys(iface, "operstate") == "up";
}

string NativeBackend::ipOf(const string &iface) {
    struct ifaddrs *all = nullptr;
    if (iface.empty() || getifaddrs(&all) != 0)
        return "";
    string address;
    for (struct ifaddrs *a = all; a != nullptr && address.empty(); a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET || iface != a->ifa_name)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        const auto *in = reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr);
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr)
            address = text;
    }
    freeifaddrs(all);
    return address;
}

//*******************************
// NativeBackend::wifiStatus
//*******************************
bool NativeBackend::wifiStatus(const string &iface, WpaStatus &out) {
    WpaCtrlClient client(iface, paths_.wpaRunDir);
    return client.socketExists() && client.status(out);
}

//*******************************
// NativeBackend::scanSsids
//*******************************
vector<string> NativeBackend::scanSsids() {
    lastError_.clear();
    string iface = wifiInterface();
    if (iface.empty()) {
        lastError_ = _("no WiFi interface");
        return {};
    }
    WpaCtrlClient client(iface, paths_.wpaRunDir);
    if (!client.socketExists()) {
        // nothing ever configured: wpa_supplicant has not been started. It is what scans, so start it - with
        // no network in its file when there is no file yet (one that is there is left as it is)
        PLOG_INFO << "no wpa_supplicant on " << iface << " - starting it to scan";
        if (DirEntry::fileSize(paths_.wpaSupplicantConf) <= 0 && !writeSupplicantConf("", ""))
            return {};
        rebindDhcpcd(iface);
        if (!waitForSupplicant(iface))
            return {};
    }
    client.scan();
    vector<string> ssids;
    for (const WifiNetwork &network : client.scanResults())
        ssids.push_back(network.ssid);
    if (ssids.empty() && !client.lastError().empty())
        lastError_ = client.lastError();
    sort(ssids.begin(), ssids.end());
    ssids.erase(unique(ssids.begin(), ssids.end()), ssids.end());
    return ssids;
}

//*******************************
// NativeBackend::configureWifi
//*******************************
void NativeBackend::configureWifi(const string &ssid, const string &password, const string &driverMode) {
    lastError_.clear();
    selectDriverMode(driverMode);
    string iface = wifiInterface();
    if (!iface.empty()) {
        WpaCtrlClient client(iface, paths_.wpaRunDir);
        if (client.socketExists() && client.open()) {
            // wpa_supplicant is running: it takes the network and writes its file itself (SAVE_CONFIG)
            if (!client.configure(ssid, password))
                lastError_ = client.lastError();
            return;
        }
    }
    // no wpa_supplicant yet: the file it will be started with, then dhcpcd takes the interface again and its
    // hook starts it. Without a WiFi interface the file is only written, for the dongle plugged in later.
    if (!writeSupplicantConf(ssid, password))
        return;
    if (!iface.empty())
        rebindDhcpcd(iface);
}

//*******************************
// NativeBackend::restartNetwork
//*******************************
// what abnet restart did: wpa_supplicant stopped (TERMINATE, where abnet did a killall), dhcpcd (the
// dhclient service) restarted - its hook starts wpa_supplicant again, with a changed driver mode too
void NativeBackend::restartNetwork() {
    lastError_.clear();
    for (const string &iface : wirelessInterfaces()) {
        WpaCtrlClient client(iface, paths_.wpaRunDir);
        if (client.socketExists() && client.open())
            client.terminate();
    }
    int status = run_("systemctl", {"restart", "dhclient"});
    if (status != 0) {
        lastError_ = "systemctl restart dhclient: " + to_string(status);
        PLOG_WARNING << lastError_;
    }
}

//*******************************
// NativeBackend::supplicantConfText / writeSupplicantConf
//*******************************
string NativeBackend::supplicantConfText(const string &runDir, const string &group, const string &ssid,
                                         const string &password) {
    string text = "ctrl_interface=DIR=" + runDir + (group.empty() ? "" : " GROUP=" + group) + "\n";
    text += "update_config=1\n";
    if (ssid.empty())
        return text;
    text += "\nnetwork={\n";
    text += "\tssid=" + WpaCtrlClient::ssidValue(ssid) + "\n";
    if (password.empty())
        text += "\tkey_mgmt=NONE\n";
    else
        text += "\tpsk=" + WpaCtrlClient::pskValue(password) + "\n";
    return text + "}\n";
}

bool NativeBackend::writeSupplicantConf(const string &ssid, const string &password) {
    if (!ssid.empty() && WpaCtrlClient::ssidValue(ssid).empty()) {
        lastError_ = _("an SSID is 1 to 32 bytes");
        return false;
    }
    if (!password.empty() && WpaCtrlClient::pskValue(password).empty()) {
        lastError_ = _("a WiFi password is 8 to 63 letters, digits or signs");
        return false;
    }
    string group = paths_.ctrlGroup;
    if (!group.empty() && getgrnam(group.c_str()) == nullptr)
        group.clear(); // wpa_supplicant would refuse the whole control interface over an unknown group
    ofstream out(paths_.wpaSupplicantConf, ios::binary | ios::trunc);
    if (!DirEntry::checkWritable(out, paths_.wpaSupplicantConf)) {
        lastError_ = _("cannot write") + " " + paths_.wpaSupplicantConf;
        return false;
    }
    out << supplicantConfText(paths_.wpaRunDir, group, ssid, password);
    out.close();
    chmod(paths_.wpaSupplicantConf.c_str(), 0600); // it holds the password
    if (!out) {
        lastError_ = _("cannot write") + " " + paths_.wpaSupplicantConf;
        return false;
    }
    PLOG_INFO << "wrote " << paths_.wpaSupplicantConf << (ssid.empty() ? " (no network)" : " for \"" + ssid + "\"");
    return true;
}

//*******************************
// NativeBackend::selectDriverMode
//*******************************
// dhcpcd.conf sets the driver dhcpcd's hook starts wpa_supplicant with (env wpa_supplicant_driver): one of
// the kernel's two variants copied over it, as abnet driver_mode did
void NativeBackend::selectDriverMode(const string &driverMode) {
    string mode = driverMode == "wext" ? "wext" : "nl80211";
    string source = paths_.kernelConfigDir + sep + "dhcpcd.conf." + mode;
    if (!DirEntry::exists(source)) {
        PLOG_WARNING << "no " << source << " - the driver mode is left as it is";
        return;
    }
    if (!DirEntry::copyFile(source, paths_.dhcpcdConf)) {
        PLOG_WARNING << "cannot copy " << source << " to " << paths_.dhcpcdConf;
    }
}

//*******************************
// NativeBackend::rebindDhcpcd / waitForSupplicant
//*******************************
void NativeBackend::rebindDhcpcd(const string &iface) {
    int status = run_("dhcpcd", {"-n", iface});
    if (status != 0) {
        PLOG_WARNING << "dhcpcd -n " << iface << ": " << status;
    }
}

bool NativeBackend::waitForSupplicant(const string &iface) {
    WpaCtrlClient client(iface, paths_.wpaRunDir);
    auto start = chrono::steady_clock::now();
    while (!client.socketExists()) {
        auto waited = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
        if (waited >= paths_.supplicantStartMs) {
            lastError_ = _("wpa_supplicant did not start on") + " " + iface;
            PLOG_WARNING << lastError_;
            return false;
        }
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    return true;
}

//*******************************
// NativeBackend: Bluetooth
//*******************************
BluezClient *NativeBackend::bluez() {
    if (bluez_)
        return bluez_.get();
    if (!bluezFactory_) {
        btError_ = "no Bluetooth support";
        return nullptr;
    }
    BusError error;
    unique_ptr<BluezBus> bus = bluezFactory_(error);
    if (!bus) {
        btError_ = _("the system bus cannot be reached") + (error.empty() ? string() : ": " + error.text());
        return nullptr;
    }
    bluez_ = make_unique<BluezClient>(std::move(bus), paths_.powerSupplyDir, paths_.btTimeouts);
    return bluez_.get();
}

void NativeBackend::takeBtError() {
    if (!bluez_)
        return;
    btError_ = bluez_->lastError();
    // the bus went away (dbus-daemon restarted): a new connection next time
    const string &name = bluez_->lastBusError().name;
    if (name == "org.freedesktop.DBus.Error.Disconnected" || name == "org.freedesktop.DBus.Error.NoServer")
        bluez_.reset();
}

BtDevice NativeBackend::toBtDevice(const BluezDevice &device) {
    BtDevice d;
    d.mac = device.address;
    d.name = device.displayName();
    d.paired = device.paired;
    d.connected = device.connected;
    return d;
}

bool NativeBackend::btUp() {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return false;
    if (!client->refresh()) {
        takeBtError();
        return false;
    }
    if (!client->hasAdapter()) {
        btError_ = _("no Bluetooth adapter");
        return false;
    }
    if (!client->adapter().powered && !triedPowerOn_) {
        // once: an adapter that is off is switched on, but one that will not come on is not asked every refresh
        triedPowerOn_ = true;
        if (!client->setPowered(true))
            takeBtError();
    }
    if (!client->adapter().powered && btError_.empty())
        btError_ = _("the Bluetooth adapter is off");
    return client->adapter().powered;
}

string NativeBackend::btName() {
    BluezClient *client = bluez();
    if (client == nullptr || !client->hasAdapter())
        return "";
    const BluezAdapter &adapter = client->adapter();
    string alias = adapter.alias.empty() ? adapter.name : adapter.alias;
    return adapter.hciName() + " " + (alias.empty() ? string() : alias + " ") + adapter.address;
}

vector<BtDevice> NativeBackend::btScan() {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return {};
    if (!client->scan(paths_.btScanMs)) {
        takeBtError();
        return {};
    }
    vector<BtDevice> out;
    for (const BluezDevice &device : client->devices())
        out.push_back(toBtDevice(device));
    return out;
}

vector<BtDevice> NativeBackend::btPairedDevices() {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return {};
    if (!client->refresh()) {
        takeBtError();
        return {};
    }
    vector<BtDevice> out;
    for (const BluezDevice &device : client->devices())
        if (device.paired)
            out.push_back(toBtDevice(device));
    return out;
}

bool NativeBackend::btPair(const string &mac) {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return false;
    bool paired = client->pair(mac);
    takeBtError(); // after a pairing that did not connect, too: it says why
    return paired;
}

bool NativeBackend::btRemove(const string &mac) {
    btError_.clear();
    BluezClient *client = bluez();
    if (client == nullptr)
        return false;
    bool removed = client->removeDevice(mac);
    takeBtError();
    return removed;
}

BtBattery NativeBackend::btBattery(const string &mac) {
    BluezClient *client = bluez();
    if (client == nullptr)
        return BluezClient::readSysfsBattery(paths_.powerSupplyDir, mac);
    return client->battery(mac);
}

//*******************************
// NativeBackend: the timezone
//*******************************
// what `settime tz` printed: it is `cat /etc/timezone`. A console where that file says nothing still has the
// zone in /etc/localtime's link (settime makes both).
string NativeBackend::timezone() {
    ifstream in(paths_.timezoneFile);
    string zone;
    getline(in, zone);
    zone = Strings::trim(zone);
    if (!zone.empty())
        return zone;
    char target[512] = {};
    ssize_t n = readlink(paths_.localtime.c_str(), target, sizeof(target) - 1);
    if (n <= 0)
        return "";
    string link(target, static_cast<size_t>(n));
    const string prefix = paths_.zoneinfoDir + "/";
    size_t at = link.find(prefix);
    return at == string::npos ? "" : link.substr(at + prefix.size());
}

// `settime tzone <zone>`, the kernel's script, run as a program (its #! line) - it replaces /etc/localtime's
// link and writes /etc/timezone. Only a zone the list offers: the script puts it in a path unchecked.
void NativeBackend::setTimezone(const string &zone) {
    lastError_.clear();
    vector<string> zones = listTimezones();
    if (zone.empty() || zone.find("..") != string::npos || !binary_search(zones.begin(), zones.end(), zone)) {
        lastError_ = _("unknown timezone") + " " + zone;
        PLOG_WARNING << lastError_;
        return;
    }
    int status = run_(paths_.settime, {"tzone", zone});
    if (status != 0) {
        lastError_ = paths_.settime + " tzone " + zone + ": " + to_string(status);
        PLOG_WARNING << lastError_;
    }
}

// timedatectl list-timezones, without timedatectl: the third column of the zoneinfo tables (zone1970.tab, and
// zone.tab, which older systemd read and which has the links too), each only while its file is there, and UTC
vector<string> NativeBackend::listTimezones() {
    vector<string> zones;
    for (const char *table : {"zone1970.tab", "zone.tab"}) {
        ifstream in(paths_.zoneinfoDir + sep + table);
        string line;
        while (getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            size_t first = line.find('\t');
            size_t second = first == string::npos ? string::npos : line.find('\t', first + 1);
            if (second == string::npos)
                continue;
            size_t end = line.find('\t', second + 1);
            string zone = Strings::trim(line.substr(second + 1, end == string::npos ? string::npos : end - second - 1));
            if (!zone.empty() && zone.find("..") == string::npos && DirEntry::exists(paths_.zoneinfoDir + sep + zone))
                zones.push_back(zone);
        }
    }
    if (DirEntry::exists(paths_.zoneinfoDir + sep + "UTC"))
        zones.emplace_back("UTC");
    sort(zones.begin(), zones.end());
    zones.erase(unique(zones.begin(), zones.end()), zones.end());
    return zones;
}
