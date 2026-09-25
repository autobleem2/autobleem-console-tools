//
// pscbios_core's native backend (Unix only): WpaCtrlClient's parsing and its requests against a fake
// wpa_supplicant - a Unix datagram socket in a temp dir answering canned replies - and NativeBackend's
// interfaces from a fake sysfs tree, its WiFi set-up with and without a running wpa_supplicant, and its
// Bluetooth over a scripted BlueZ (fake_bluez_bus.h).
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"

#include "fake_bluez_bus.h"

#include "core/native_backend.h"
#include "core/wpa_ctrl_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using std::string;
using std::vector;

//*******************************
// FakeWpaSupplicant
//*******************************
// the server side of <dir>/<iface>: records every command, answers from `replies` ("OK\n" for anything not
// in it, nothing at all when `silent`), keeps the ATTACHed clients and, after a SCAN, sends them the event
namespace {
class FakeWpaSupplicant {
public:
    explicit FakeWpaSupplicant(string path) : path_(std::move(path)) {
        fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path_.c_str());
        unlink(path_.c_str());
        bound_ = bind(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0;
        thread_ = std::thread([this]() { serve(); });
    }
    ~FakeWpaSupplicant() {
        stop_ = true;
        thread_.join();
        close(fd_);
        unlink(path_.c_str());
    }
    FakeWpaSupplicant(const FakeWpaSupplicant &) = delete;
    FakeWpaSupplicant &operator=(const FakeWpaSupplicant &) = delete;

    bool bound() const { return bound_; }
    void reply(const string &cmd, const string &answer) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[cmd] = answer;
    }
    void setSilent(bool silent) { silent_ = silent; }
    // what the client sent, ATTACH/DETACH left out
    vector<string> commands() {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }
    void clearCommands() {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.clear();
    }

private:
    void serve() {
        while (!stop_) {
            struct pollfd p{};
            p.fd = fd_;
            p.events = POLLIN;
            if (poll(&p, 1, 20) <= 0)
                continue;
            char buffer[4096];
            struct sockaddr_un from{};
            socklen_t fromLength = sizeof(from);
            ssize_t n =
                recvfrom(fd_, buffer, sizeof(buffer), 0, reinterpret_cast<struct sockaddr *>(&from), &fromLength);
            if (n < 0)
                continue;
            string cmd(buffer, static_cast<size_t>(n));
            string answer = "OK\n";
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cmd == "ATTACH") {
                    attached_.emplace_back(from, fromLength);
                } else if (cmd != "DETACH") {
                    commands_.push_back(cmd);
                    auto it = replies_.find(cmd);
                    if (it != replies_.end())
                        answer = it->second;
                }
            }
            if (silent_)
                continue;
            sendto(fd_, answer.data(), answer.size(), 0, reinterpret_cast<struct sockaddr *>(&from), fromLength);
            if (cmd == "SCAN") {
                const string event = "<2>CTRL-EVENT-SCAN-RESULTS ";
                for (auto &client : attached_)
                    sendto(fd_, event.data(), event.size(), 0, reinterpret_cast<struct sockaddr *>(&client.first),
                           client.second);
            }
        }
    }

    string path_;
    int fd_ = -1;
    bool bound_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> silent_{false};
    std::mutex mutex_;
    std::map<string, string> replies_;
    vector<string> commands_;
    vector<std::pair<struct sockaddr_un, socklen_t>> attached_;
    std::thread thread_;
};

const char *const ScanResults = "bssid / frequency / signal level / flags / ssid\n"
                                "aa:bb:cc:00:00:01\t2437\t-71\t[WPA2-PSK-CCMP][ESS]\tHome Network\n"
                                "aa:bb:cc:00:00:02\t5180\t-48\t[WPA2-PSK-CCMP][ESS]\tHome Network\n"
                                "aa:bb:cc:00:00:03\t2412\t-60\t[ESS]\tCafe Corner\n"
                                "aa:bb:cc:00:00:04\t2462\t-40\t[WPA2-PSK-CCMP][ESS]\t\n"
                                "aa:bb:cc:00:00:05\t2462\t-42\t[WPA2-PSK-CCMP][ESS]\t\\x00\\x00\\x00\n"
                                "aa:bb:cc:00:00:06\t2472\t-80\t[WPA-PSK-TKIP][ESS]\tCaf\\xc3\\xa9 \\\"Bar\\\"\\\\\n";

// a fake /sys/class/net entry: `flags` as the kernel writes it, and the wireless marker when asked
void makeInterface(const TempDir &tmp, const string &name, const string &flags, const string &wirelessMarker = "") {
    tmp.makeSubDir("net/" + name);
    tmp.writeFile("net/" + name + "/flags", flags + "\n");
    tmp.writeFile("net/" + name + "/operstate", "unknown\n");
    if (!wirelessMarker.empty())
        tmp.makeSubDir("net/" + name + "/" + wirelessMarker);
}

// the paths of a NativeBackend inside `tmp`: net/ is sysfs, run/ the ctrl_interface dir, etc/ the config
NativePaths pathsIn(const TempDir &tmp) {
    tmp.makeSubDir("net");
    tmp.makeSubDir("run");
    tmp.makeSubDir("etc/autobleem");
    tmp.writeFile("etc/autobleem/dhcpcd.conf.wext", "env wpa_supplicant_driver=wext\n");
    tmp.writeFile("etc/autobleem/dhcpcd.conf.nl80211", "env wpa_supplicant_driver=nl80211\n");
    NativePaths paths;
    paths.sysClassNet = tmp.at("net");
    paths.wpaRunDir = tmp.at("run");
    paths.wpaSupplicantConf = tmp.at("etc/wpa_supplicant.conf");
    paths.dhcpcdConf = tmp.at("etc/dhcpcd.conf");
    paths.kernelConfigDir = tmp.at("etc/autobleem");
    paths.ctrlGroup = "";
    paths.supplicantStartMs = 300;
    return paths;
}

struct RecordedRuns {
    vector<string> lines; // "exe arg arg"
    NativeBackend::CommandRunner runner() {
        return [this](const string &exe, const vector<string> &args) {
            string line = exe;
            for (const string &arg : args)
                line += " " + arg;
            lines.push_back(line);
            return 0;
        };
    }
};
} // namespace

//*******************************
// WpaCtrlClient: parsing
//*******************************
TEST_CASE("WpaCtrlClient parses SCAN_RESULTS: one entry per SSID, the strongest, hidden ones left out") {
    vector<WifiNetwork> networks = WpaCtrlClient::parseScanResults(ScanResults);
    REQUIRE(networks.size() == 3);
    CHECK(networks[0].ssid == "Home Network"); // strongest first
    CHECK(networks[0].bssid == "aa:bb:cc:00:00:02");
    CHECK(networks[0].frequency == 5180);
    CHECK(networks[0].signal == -48);
    CHECK(networks[0].flags == "[WPA2-PSK-CCMP][ESS]");
    CHECK(networks[1].ssid == "Cafe Corner");
    CHECK(networks[2].ssid == "Caf\xc3\xa9 \"Bar\"\\"); // wpa_supplicant's escapes undone
    CHECK(WpaCtrlClient::parseScanResults("bssid / frequency / signal level / flags / ssid\n").empty());
    CHECK(WpaCtrlClient::parseScanResults("").empty());
}

TEST_CASE("WpaCtrlClient parses STATUS") {
    WpaStatus status = WpaCtrlClient::parseStatus("bssid=aa:bb:cc:00:00:02\nfreq=5180\nssid=Home Network\nid=0\n"
                                                  "mode=station\nwpa_state=COMPLETED\nip_address=192.168.1.40\n"
                                                  "address=00:c0:ca:11:22:33\n");
    CHECK(status.wpaState == "COMPLETED");
    CHECK(status.ssid == "Home Network");
    CHECK(status.ipAddress == "192.168.1.40");
    status = WpaCtrlClient::parseStatus("wpa_state=SCANNING\naddress=00:c0:ca:11:22:33\n");
    CHECK(status.wpaState == "SCANNING");
    CHECK(status.ssid.empty());
    CHECK(status.ipAddress.empty());
}

TEST_CASE("WpaCtrlClient quotes an SSID or sends it in hex, and checks a password") {
    CHECK(WpaCtrlClient::ssidValue("Home Network") == "\"Home Network\"");
    CHECK(WpaCtrlClient::ssidValue("My \"Net\"") == "4d7920224e657422"); // a quote: hex, nothing to escape
    CHECK(WpaCtrlClient::ssidValue("Caf\xc3\xa9") == "436166c3a9");      // UTF-8: hex
    CHECK(WpaCtrlClient::ssidValue("").empty());
    CHECK(WpaCtrlClient::ssidValue(string(33, 'x')).empty());

    CHECK(WpaCtrlClient::pskValue("pa$s \"word\"") == "\"pa$s \"word\"\"");
    CHECK(WpaCtrlClient::pskValue("short").empty());
    CHECK(WpaCtrlClient::pskValue(string(64, 'x')).empty());
    CHECK(WpaCtrlClient::pskValue(string(64, 'a')) == string(64, 'a')); // a raw key goes unquoted
    CHECK(WpaCtrlClient::pskValue("zażółć gęślą").empty());             // not ASCII
}

//*******************************
// WpaCtrlClient: against the fake
//*******************************
TEST_CASE("WpaCtrlClient without a wpa_supplicant does not connect") {
    TempDir tmp("wpa_none");
    WpaCtrlClient client("wlan0", tmp.path());
    CHECK_FALSE(client.socketExists());
    CHECK_FALSE(client.open());
    CHECK_FALSE(client.lastError().empty());
    string reply;
    CHECK_FALSE(client.request("PING", reply));
}

TEST_CASE("WpaCtrlClient scans, reads the results and the status") {
    TempDir tmp("wpa_scan");
    FakeWpaSupplicant fake(tmp.at("wlan0"));
    REQUIRE(fake.bound());
    fake.reply("SCAN_RESULTS", ScanResults);
    fake.reply("STATUS", "wpa_state=COMPLETED\nssid=Home Network\nip_address=10.0.0.7\n");

    WpaCtrlClient client("wlan0", tmp.path());
    REQUIRE(client.open());
    CHECK(client.scan(2000)); // the event came
    vector<WifiNetwork> networks = client.scanResults();
    REQUIRE(networks.size() == 3);
    CHECK(networks[0].ssid == "Home Network");
    WpaStatus status;
    REQUIRE(client.status(status));
    CHECK(status.wpaState == "COMPLETED");
    CHECK(status.ipAddress == "10.0.0.7");
    CHECK(fake.commands() == vector<string>{"SCAN", "SCAN_RESULTS", "STATUS"});
}

TEST_CASE("WpaCtrlClient configures a network in wpa_supplicant's order") {
    TempDir tmp("wpa_conf");
    FakeWpaSupplicant fake(tmp.at("wlan0"));
    REQUIRE(fake.bound());
    fake.reply("ADD_NETWORK", "3\n");
    WpaCtrlClient client("wlan0", tmp.path());

    REQUIRE(client.configure("My \"Net\"", "pa$s word"));
    CHECK(fake.commands() == vector<string>{"REMOVE_NETWORK all", "ADD_NETWORK", "SET_NETWORK 3 ssid 4d7920224e657422",
                                            "SET_NETWORK 3 psk \"pa$s word\"", "ENABLE_NETWORK 3", "SAVE_CONFIG"});

    fake.clearCommands();
    REQUIRE(client.configure("Cafe Corner", "")); // an open network
    CHECK(fake.commands() == vector<string>{"REMOVE_NETWORK all", "ADD_NETWORK", "SET_NETWORK 3 ssid \"Cafe Corner\"",
                                            "SET_NETWORK 3 key_mgmt NONE", "ENABLE_NETWORK 3", "SAVE_CONFIG"});

    fake.clearCommands();
    fake.reply("SET_NETWORK 3 psk \"12345678\"", "FAIL\n");
    CHECK_FALSE(client.configure("Home", "12345678"));
    CHECK(client.lastError() == "SET_NETWORK psk: FAIL");              // the step, never the password
    CHECK(fake.commands().back() == "SET_NETWORK 3 psk \"12345678\""); // nothing after the refusal

    fake.clearCommands();
    CHECK_FALSE(client.configure("Home", "short")); // refused before anything is sent
    CHECK(fake.commands().empty());
}

TEST_CASE("WpaCtrlClient gives up on a wpa_supplicant that does not answer") {
    TempDir tmp("wpa_silent");
    FakeWpaSupplicant fake(tmp.at("wlan0"));
    REQUIRE(fake.bound());
    fake.setSilent(true);
    WpaCtrlClient client("wlan0", tmp.path(), 200);
    auto start = std::chrono::steady_clock::now();
    string reply;
    CHECK_FALSE(client.request("PING", reply));
    CHECK_FALSE(client.scan(300));
    auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(waited < 3000);
    CHECK_FALSE(client.lastError().empty());
}

//*******************************
// NativeBackend
//*******************************
TEST_CASE("NativeBackend finds the interfaces in sysfs, a wireless one by any name") {
    TempDir tmp("native_ifaces");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "lo", "0x9");
    makeInterface(tmp, "eth0", "0x1002");                        // down
    makeInterface(tmp, "wlx00c0ca112233", "0x1003", "wireless"); // up
    makeInterface(tmp, "wlan1", "0x1002", "phy80211");           // down
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), std::make_unique<FakeBackend>());

    CHECK(backend.interfaces() == vector<string>{"eth0", "lo", "wlan1", "wlx00c0ca112233"});
    CHECK(backend.wirelessInterfaces() == vector<string>{"wlan1", "wlx00c0ca112233"});
    CHECK(backend.interfaceFound("eth0"));
    CHECK_FALSE(backend.interfaceFound("wlan0"));
    CHECK_FALSE(backend.interfaceFound(""));
    CHECK(backend.isUp("wlx00c0ca112233"));
    CHECK(backend.isUp("lo"));
    CHECK_FALSE(backend.isUp("eth0"));
    CHECK_FALSE(backend.isUp("wlan0")); // not there
    CHECK(backend.wlanOn());
    CHECK(backend.wifiInterface() == "wlx00c0ca112233"); // the one that is up

    tmp.writeFile("net/wlx00c0ca112233/flags", "0x1002\n");
    CHECK_FALSE(backend.wlanOn());             // wireless, but none up
    CHECK(backend.wifiInterface() == "wlan1"); // then the first
    CHECK(backend.ipOf("no-such-interface").empty());
    CHECK(backend.ipOf("lo") == "127.0.0.1"); // getifaddrs: the machine's own loopback

    // the timezone is the other backend's; Bluetooth is BlueZ's, and without a bus there is none
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() == "no Bluetooth support");
    CHECK(backend.timezone() == "Europe/Warsaw");
    CHECK(backend.kernelInstalled());
}

TEST_CASE("NativeBackend configures WiFi through a running wpa_supplicant") {
    TempDir tmp("native_conf");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "wlan0", "0x1003", "wireless");
    FakeWpaSupplicant fake(tmp.at("run/wlan0"));
    REQUIRE(fake.bound());
    fake.reply("SCAN_RESULTS", ScanResults);
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), std::make_unique<FakeBackend>());

    vector<string> expected{"Home Network", "Cafe Corner", "Caf\xc3\xa9 \"Bar\"\\"};
    std::sort(expected.begin(), expected.end()); // sorted as std::string sorts (char's sign differs per CPU)
    CHECK(backend.scanSsids() == expected);

    fake.clearCommands();
    fake.reply("ADD_NETWORK", "0\n");
    backend.configureWifi("Home Network", "secret123", "wext");
    CHECK(backend.lastError().empty());
    CHECK(fake.commands() == vector<string>{"REMOVE_NETWORK all", "ADD_NETWORK", "SET_NETWORK 0 ssid \"Home Network\"",
                                            "SET_NETWORK 0 psk \"secret123\"", "ENABLE_NETWORK 0", "SAVE_CONFIG"});
    CHECK(tmp.readFile("etc/dhcpcd.conf") == "env wpa_supplicant_driver=wext\n"); // the driver variant copied
    CHECK_FALSE(ableem::DirEntry::exists(paths.wpaSupplicantConf));               // wpa_supplicant writes its own file
    CHECK(runs.lines.empty());                                                    // nothing restarted

    WpaStatus status;
    fake.reply("STATUS", "wpa_state=COMPLETED\nssid=Home Network\n");
    REQUIRE(backend.wifiStatus("wlan0", status));
    CHECK(status.wpaState == "COMPLETED");

    fake.clearCommands();
    backend.restartNetwork();
    CHECK(fake.commands() == vector<string>{"TERMINATE"});
    CHECK(runs.lines == vector<string>{"systemctl restart dhclient"});
}

TEST_CASE("NativeBackend writes wpa_supplicant.conf and has dhcpcd start it when none runs") {
    TempDir tmp("native_file");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "wlan0", "0x1003", "wireless");
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), std::make_unique<FakeBackend>());

    backend.configureWifi("Home Network", "secret123", "nl80211");
    CHECK(backend.lastError().empty());
    CHECK(tmp.readFile("etc/wpa_supplicant.conf") == "ctrl_interface=DIR=" + paths.wpaRunDir +
                                                         "\n"
                                                         "update_config=1\n"
                                                         "\n"
                                                         "network={\n"
                                                         "\tssid=\"Home Network\"\n"
                                                         "\tpsk=\"secret123\"\n"
                                                         "}\n");
    CHECK(tmp.readFile("etc/dhcpcd.conf") == "env wpa_supplicant_driver=nl80211\n");
    CHECK(runs.lines == vector<string>{"dhcpcd -n wlan0"});

    runs.lines.clear();
    backend.configureWifi("Home Network", "short", "nl80211"); // refused, the file kept
    CHECK_FALSE(backend.lastError().empty());
    CHECK(tmp.readFile("etc/wpa_supplicant.conf").find("secret123") != string::npos);
    CHECK(runs.lines.empty());

    // a scan with no wpa_supplicant starts it; one that never comes up gives up after supplicantStartMs
    runs.lines.clear();
    CHECK(backend.scanSsids().empty());
    CHECK(runs.lines == vector<string>{"dhcpcd -n wlan0"});
    CHECK(backend.lastError() == "wpa_supplicant did not start on wlan0");

    CHECK(NativeBackend::supplicantConfText("/var/run/wpa_supplicant", "netdev", "Cafe Corner", "") ==
          "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\nupdate_config=1\n\nnetwork={\n"
          "\tssid=\"Cafe Corner\"\n\tkey_mgmt=NONE\n}\n");
    CHECK(NativeBackend::supplicantConfText("/run/wpa", "", "", "") ==
          "ctrl_interface=DIR=/run/wpa\nupdate_config=1\n");
}

TEST_CASE("NativeBackend without a WiFi interface scans nothing and only writes the file") {
    TempDir tmp("native_nowifi");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "eth0", "0x1003");
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), std::make_unique<FakeBackend>());
    CHECK_FALSE(backend.wlanOn());
    CHECK(backend.scanSsids().empty());
    backend.configureWifi("Home Network", "secret123", "wext");
    CHECK(ableem::DirEntry::exists(paths.wpaSupplicantConf)); // for the dongle plugged in later
    CHECK(runs.lines.empty());
}

//*******************************
// NativeBackend: Bluetooth
//*******************************
TEST_CASE("NativeBackend's Bluetooth goes through BlueZ") {
    TempDir tmp("native_bt");
    NativePaths paths = pathsIn(tmp);
    paths.powerSupplyDir = tmp.at("ps");
    paths.btScanMs = 20;
    paths.btTimeouts.discoverMs = 300;
    paths.btTimeouts.pairedWaitMs = 300;
    paths.btTimeouts.pollMs = 1;
    tmp.makeSubDir("ps/sony_controller_battery_1c:a0:b8:12:34:56");
    tmp.writeFile("ps/sony_controller_battery_1c:a0:b8:12:34:56/capacity", "60\n");

    auto state = std::make_shared<fakebluez::State>();
    state->objects["/org/bluez"]["org.bluez.AgentManager1"];
    state->objects[fakebluez::Adapter] = fakebluez::adapter("00:1A:7D:DA:71:13", false);
    state->objects[fakebluez::devicePath("A0:AB:51:33:44:55")] =
        fakebluez::device("A0:AB:51:33:44:55", "Xbox Wireless Controller", true, true);
    state->discoverable[fakebluez::devicePath("1C:A0:B8:12:34:56")] =
        fakebluez::device("1C:A0:B8:12:34:56", "Wireless Controller");
    int connects = 0;
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), std::make_unique<FakeBackend>(), [&](BusError &) {
        ++connects;
        return fakebluez::makeBus(state);
    });

    CHECK(backend.btUp()); // off: powered on, once
    CHECK(state->calls == vector<string>{"Set Powered=true /org/bluez/hci0"});
    CHECK(backend.btName() == "hci0 PSC 00:1A:7D:DA:71:13");

    vector<BtDevice> paired = backend.btPairedDevices();
    REQUIRE(paired.size() == 1);
    CHECK(paired[0].mac == "A0:AB:51:33:44:55");
    CHECK(paired[0].name == "Xbox Wireless Controller");
    CHECK(paired[0].connected);

    vector<BtDevice> found = backend.btScan();
    REQUIRE(found.size() == 2);
    CHECK(found[0].mac == "1C:A0:B8:12:34:56");
    CHECK_FALSE(found[0].paired);

    CHECK(backend.btPair("1C:A0:B8:12:34:56"));
    CHECK(backend.btLastError().empty());
    CHECK(backend.btPairedDevices().size() == 2);
    CHECK(backend.btBattery("1C:A0:B8:12:34:56").percent == 60);

    state->errors["Pair"] = {"org.bluez.Error.AuthenticationTimeout", "Authentication Timeout"};
    state->objects[fakebluez::devicePath("E4:17:D8:AA:BB:CC")] = fakebluez::device("E4:17:D8:AA:BB:CC", "8BitDo");
    CHECK_FALSE(backend.btPair("E4:17:D8:AA:BB:CC"));
    CHECK(backend.btLastError() ==
          "pairing E4:17:D8:AA:BB:CC failed: org.bluez.Error.AuthenticationTimeout: Authentication Timeout");

    CHECK(backend.btRemove("A0:AB:51:33:44:55"));
    CHECK_FALSE(backend.btRemove("A0:AB:51:33:44:55"));
    CHECK(backend.btLastError() == "A0:AB:51:33:44:55 is not known");
    CHECK(connects == 1); // one connection for all of it

    // the adapter switched off behind our back: not powered on again, and said so
    (*state->props(fakebluez::Adapter, "org.bluez.Adapter1"))["Powered"] = DbusValue::ofBool(false);
    state->calls.clear();
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() == "the Bluetooth adapter is off");
    CHECK(state->calls.empty());

    // bluetoothd stopped
    state->errors["GetManagedObjects"] = {"org.freedesktop.DBus.Error.ServiceUnknown",
                                          "The name org.bluez was not "
                                          "provided by any .service files"};
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() == "BlueZ does not answer: org.freedesktop.DBus.Error.ServiceUnknown: The name "
                                   "org.bluez was not provided by any .service files");
    CHECK(backend.btScan().empty());
    CHECK(backend.btPairedDevices().empty());
}

TEST_CASE("NativeBackend without a system bus: no Bluetooth, and why") {
    TempDir tmp("native_nobus");
    NativePaths paths = pathsIn(tmp);
    RecordedRuns runs;
    int attempts = 0;
    NativeBackend backend(paths, runs.runner(), std::make_unique<FakeBackend>(), [&](BusError &error) {
        ++attempts;
        error.name = "org.freedesktop.DBus.Error.FileNotFound";
        error.message = "Failed to connect to socket /var/run/dbus/system_bus_socket: No such file or directory";
        return std::unique_ptr<BluezBus>();
    });
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() ==
          "the system bus cannot be reached: org.freedesktop.DBus.Error.FileNotFound: Failed to connect to socket "
          "/var/run/dbus/system_bus_socket: No such file or directory");
    CHECK(backend.btName().empty());
    CHECK_FALSE(backend.btPair("1C:A0:B8:12:34:56"));
    CHECK_FALSE(backend.btRemove("1C:A0:B8:12:34:56"));
    CHECK(backend.btScan().empty());
    CHECK(attempts == 5); // tried again each time: dbus-daemon may come up later
}
