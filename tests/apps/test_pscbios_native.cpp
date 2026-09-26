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
    void setScanEvents(bool send) { scanEvents_ = send; } // false: a SCAN is answered, its end never announced
    // an event to every ATTACHed client, as wpa_supplicant sends them
    void sendEvent(const string &event) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &client : attached_)
            sendto(fd_, event.data(), event.size(), 0, reinterpret_cast<struct sockaddr *>(&client.first),
                   client.second);
    }
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
            if (cmd == "SCAN" && scanEvents_) {
                std::lock_guard<std::mutex> lock(mutex_);
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
    std::atomic<bool> scanEvents_{true};
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
    NativePaths paths;
    paths.sysClassNet = tmp.at("net");
    paths.wpaRunDir = tmp.at("run");
    paths.wpaSupplicantConf = tmp.at("etc/wpa_supplicant.conf");
    paths.ctrlGroup = "";
    paths.supplicantStartMs = 300;
    paths.kernelMarker = tmp.at("bin/abnet");
    paths.settime = tmp.at("bin/settime");
    paths.timezoneFile = tmp.at("etc/timezone");
    paths.localtime = tmp.at("etc/localtime");
    paths.zoneinfoDir = tmp.at("zoneinfo");
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
                                            "SET_NETWORK 3 psk \"pa$s word\"", "ENABLE_NETWORK 3",
                                            "SET update_config 1", "SAVE_CONFIG"});

    fake.clearCommands();
    REQUIRE(client.configure("Cafe Corner", "")); // an open network
    CHECK(fake.commands() == vector<string>{"REMOVE_NETWORK all", "ADD_NETWORK", "SET_NETWORK 3 ssid \"Cafe Corner\"",
                                            "SET_NETWORK 3 key_mgmt NONE", "ENABLE_NETWORK 3", "SET update_config 1",
                                            "SAVE_CONFIG"});

    fake.clearCommands();
    fake.reply("SET_NETWORK 3 psk \"12345678\"", "FAIL\n");
    CHECK_FALSE(client.configure("Home", "12345678"));
    // the reason, then the step, never the password
    CHECK(client.lastError() == "wpa_supplicant refused the network (SET_NETWORK psk: FAIL)");
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
    NativeBackend backend(paths, runs.runner());

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

    // Bluetooth is BlueZ's, and without a bus there is none
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() == "no Bluetooth support");
}

TEST_CASE("NativeBackend picks the ethernet dongle by what it is, under any name") {
    TempDir tmp("native_eth");
    NativePaths paths = pathsIn(tmp);
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner());
    CHECK(backend.ethernetInterface().empty());
    makeInterface(tmp, "lo", "0x9");
    makeInterface(tmp, "rndis0", "0x1003"); // the AutoBleem kernel's USB gadget: not a dongle
    tmp.makeSubDir("net/rndis0/device");
    makeInterface(tmp, "sit0", "0x80"); // a tunnel: no device, not ARPHRD_ETHER
    tmp.writeFile("net/sit0/type", "776\n");
    makeInterface(tmp, "wlan0", "0x1003", "wireless"); // the WiFi dongle is not the wired one
    tmp.makeSubDir("net/wlan0/device");
    CHECK(backend.ethernetInterface().empty());
    makeInterface(tmp, "enx00e04c680001", "0x1003"); // a USB ethernet dongle, named by its address
    tmp.writeFile("net/enx00e04c680001/type", "1\n");
    tmp.makeSubDir("net/enx00e04c680001/device");
    CHECK(backend.ethernetInterface() == "enx00e04c680001");
    CHECK(backend.wifiInterface() == "wlan0");
}

TEST_CASE("NativeBackend: the kernel, and the timezone without timedatectl or a shell") {
    TempDir tmp("native_tz");
    NativePaths paths = pathsIn(tmp);
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner());
    CHECK_FALSE(backend.kernelInstalled());
    tmp.writeFile("bin/abnet", "#!/bin/bash\n"); // only looked at, never run
    CHECK(backend.kernelInstalled());

    // the zones: both tables, only those whose file is there, and UTC
    tmp.writeFile("zoneinfo/zone1970.tab", "# comment\n"
                                           "PL\t+5215+02100\tEurope/Warsaw\n"
                                           "JP\t+353916+1394441\tAsia/Tokyo\n"
                                           "XX\t+0000+00000\tNowhere/Gone\tno file\n");
    tmp.writeFile("zoneinfo/zone.tab", "IE\t+5320-00615\tEurope/Dublin\nPL\t+5215+02100\tEurope/Warsaw\n");
    for (const char *zone : {"Europe/Warsaw", "Asia/Tokyo", "Europe/Dublin", "UTC"})
        tmp.writeFile(string("zoneinfo/") + zone, "TZif");
    CHECK(backend.listTimezones() == vector<string>{"Asia/Tokyo", "Europe/Dublin", "Europe/Warsaw", "UTC"});

    // the current one: /etc/timezone (what `settime tz` printed), else /etc/localtime's link
    CHECK(backend.timezone().empty());
    REQUIRE(symlink((paths.zoneinfoDir + "/Europe/Dublin").c_str(), paths.localtime.c_str()) == 0);
    CHECK(backend.timezone() == "Europe/Dublin");
    tmp.writeFile("etc/timezone", "Europe/Warsaw\n");
    CHECK(backend.timezone() == "Europe/Warsaw");

    // set through the kernel's settime, run directly - a zone from the list only
    backend.setTimezone("Asia/Tokyo");
    CHECK(backend.lastError().empty());
    CHECK(runs.lines == vector<string>{paths.settime + " tzone Asia/Tokyo"});
    runs.lines.clear();
    backend.setTimezone("../../etc/passwd");
    backend.setTimezone("Nowhere/Gone");
    CHECK(runs.lines.empty());
    CHECK(backend.lastError() == "unknown timezone (Nowhere/Gone)");
}

TEST_CASE("NativeBackend says why settime failed") {
    TempDir tmp("native_tzfail");
    NativePaths paths = pathsIn(tmp);
    tmp.writeFile("zoneinfo/zone.tab", "PL\t+5215+02100\tEurope/Warsaw\n");
    tmp.writeFile("zoneinfo/Europe/Warsaw", "TZif");
    NativeBackend backend(paths, [](const string &, const vector<string> &) { return 1; });
    backend.setTimezone("Europe/Warsaw");
    CHECK(backend.lastError() == "the timezone could not be changed (" + paths.settime + " tzone Europe/Warsaw: 1)");
}

TEST_CASE("NativeBackend configures WiFi through a running wpa_supplicant") {
    TempDir tmp("native_conf");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "wlan0", "0x1003", "wireless");
    FakeWpaSupplicant fake(tmp.at("run/wlan0"));
    REQUIRE(fake.bound());
    fake.reply("SCAN_RESULTS", ScanResults);
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner());

    vector<WifiNetwork> networks = backend.scanNetworks(); // strongest first, one per SSID
    REQUIRE(networks.size() == 3);
    CHECK(networks[0].ssid == "Home Network");
    CHECK(networks[0].signal == -48);
    CHECK(networks[1].ssid == "Cafe Corner");
    CHECK_FALSE(networks[1].secured());
    CHECK(networks[2].ssid == "Caf\xc3\xa9 \"Bar\"\\");
    CHECK(backend.lastError().empty());

    fake.clearCommands();
    fake.reply("ADD_NETWORK", "0\n");
    backend.configureWifi("Home Network", "secret123");
    CHECK(backend.lastError().empty());
    CHECK(fake.commands() == vector<string>{"REMOVE_NETWORK all", "ADD_NETWORK", "SET_NETWORK 0 ssid \"Home Network\"",
                                            "SET_NETWORK 0 psk \"secret123\"", "ENABLE_NETWORK 0",
                                            "SET update_config 1", "SAVE_CONFIG"});
    CHECK_FALSE(ableem::DirEntry::exists(paths.wpaSupplicantConf)); // wpa_supplicant writes its own file
    CHECK(runs.lines.empty());                                      // nothing restarted

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
    NativeBackend backend(paths, runs.runner());

    backend.configureWifi("Home Network", "secret123");
    CHECK(backend.lastError().empty());
    CHECK(tmp.readFile("etc/wpa_supplicant.conf") == "ctrl_interface=DIR=" + paths.wpaRunDir +
                                                         "\n"
                                                         "update_config=1\n"
                                                         "\n"
                                                         "network={\n"
                                                         "\tssid=\"Home Network\"\n"
                                                         "\tpsk=\"secret123\"\n"
                                                         "}\n");
    CHECK(runs.lines == vector<string>{"dhcpcd -n wlan0"});

    runs.lines.clear();
    backend.configureWifi("Home Network", "short"); // refused, the file kept
    CHECK_FALSE(backend.lastError().empty());
    CHECK(tmp.readFile("etc/wpa_supplicant.conf").find("secret123") != string::npos);
    CHECK(runs.lines.empty());

    // a scan with no wpa_supplicant starts it; one that never comes up gives up after supplicantStartMs
    runs.lines.clear();
    CHECK(backend.scanNetworks().empty());
    CHECK(runs.lines == vector<string>{"dhcpcd -n wlan0"});
    CHECK(backend.lastError() == "wpa_supplicant did not start (wlan0)");

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
    NativeBackend backend(paths, runs.runner());
    CHECK_FALSE(backend.wlanOn());
    CHECK(backend.scanNetworks().empty());
    CHECK(backend.lastError() == "no WiFi interface");
    backend.configureWifi("Home Network", "secret123");
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
    NativeBackend backend(paths, runs.runner(), [&](BusError &) {
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
    CHECK(backend.btLastError() == "the controller did not answer - is it in pairing mode? "
                                   "(org.bluez.Error.AuthenticationTimeout: Authentication Timeout)");

    CHECK(backend.btRemove("A0:AB:51:33:44:55"));
    CHECK_FALSE(backend.btRemove("A0:AB:51:33:44:55"));
    CHECK(backend.btLastError() == "the controller is not known (A0:AB:51:33:44:55)");
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
    CHECK(backend.btLastError() == "BlueZ (bluetoothd) is not running (org.freedesktop.DBus.Error.ServiceUnknown: "
                                   "The name org.bluez was not provided by any .service files)");
    CHECK(backend.btScan().empty());
    CHECK(backend.btPairedDevices().empty());
}

TEST_CASE("NativeBackend without a system bus: no Bluetooth, and why") {
    TempDir tmp("native_nobus");
    NativePaths paths = pathsIn(tmp);
    RecordedRuns runs;
    int attempts = 0;
    NativeBackend backend(paths, runs.runner(), [&](BusError &error) {
        ++attempts;
        error.name = "org.freedesktop.DBus.Error.FileNotFound";
        error.message = "Failed to connect to socket /var/run/dbus/system_bus_socket: No such file or directory";
        return std::unique_ptr<BluezBus>();
    });
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() ==
          "the system bus cannot be reached (org.freedesktop.DBus.Error.FileNotFound: Failed to connect to socket "
          "/var/run/dbus/system_bus_socket: No such file or directory)");
    CHECK(backend.btName().empty());
    CHECK_FALSE(backend.btPair("1C:A0:B8:12:34:56"));
    CHECK_FALSE(backend.btRemove("1C:A0:B8:12:34:56"));
    CHECK(backend.btScan().empty());
    CHECK(attempts == 5); // tried again each time: dbus-daemon may come up later
}

//*******************************
// NativeBackend: nothing freezes a screen
//*******************************
TEST_CASE("NativeBackend's scan asks the wait hook, and a stop ends it before its timeout") {
    TempDir tmp("native_scanstop");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "wlan0", "0x1003", "wireless");
    FakeWpaSupplicant fake(tmp.at("run/wlan0"));
    REQUIRE(fake.bound());
    fake.setScanEvents(false); // a scan that never ends by itself
    fake.reply("SCAN_RESULTS", ScanResults);
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner());
    int asked = 0;
    backend.setWaitHook([&]() { return ++asked < 4; });
    const auto start = std::chrono::steady_clock::now();
    vector<WifiNetwork> networks = backend.scanNetworks();
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(asked == 4);
    CHECK(waited < 2000);        // WpaCtrlClient::ScanTimeoutMs is 8 s
    CHECK(networks.size() == 3); // what wpa_supplicant has, all the same
    CHECK(backend.lastError() == "cancelled");
}

TEST_CASE("NativeBackend follows a connection to the end: connected, or a wrong password") {
    TempDir tmp("native_follow");
    NativePaths paths = pathsIn(tmp);
    makeInterface(tmp, "wlan0", "0x1003", "wireless");
    FakeWpaSupplicant fake(tmp.at("run/wlan0"));
    REQUIRE(fake.bound());
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner());

    auto follow = [&](int maxMs) -> const WifiConnectWatch & {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const WifiConnectWatch &watch = backend.pumpWifiConnect();
            auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (watch.finished() || ms > maxMs)
                return watch;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };

    fake.reply("STATUS", "wpa_state=COMPLETED\nssid=Home Network\nip_address=10.0.0.7\n");
    backend.beginWifiConnect("Home Network");
    const WifiConnectWatch &ok = follow(3000);
    CHECK(ok.stage() == WifiConnectStage::Connected);
    CHECK(ok.address() == "10.0.0.7");

    fake.reply("STATUS", "wpa_state=4WAY_HANDSHAKE\nssid=Home Network\n");
    backend.beginWifiConnect("Home Network");
    backend.pumpWifiConnect(); // attached
    fake.sendEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home Network\" auth_failures=1 duration=10 "
                   "reason=WRONG_KEY");
    const WifiConnectWatch &wrong = follow(3000);
    CHECK(wrong.stage() == WifiConnectStage::Failed);
    CHECK(wrong.failure() == WifiFailure::WrongPassword);
    CHECK(wrong.text().compare(0, 16, "wrong password (") == 0);

    WpaStatus status;
    fake.reply("STATUS", "wpa_state=SCANNING\n");
    REQUIRE(backend.wifiStatus(status));
    CHECK(status.wpaState == "SCANNING");
}

TEST_CASE("NativeBackend's connection watch: no wpa_supplicant, and no WiFi at all") {
    TempDir tmp("native_follow_none");
    NativePaths paths = pathsIn(tmp);
    paths.wifiTimeouts.startMs = 100;
    makeInterface(tmp, "wlan0", "0x1003", "wireless");
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner());
    backend.beginWifiConnect("Home Network");
    CHECK(backend.pumpWifiConnect().stage() == WifiConnectStage::Starting);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(backend.pumpWifiConnect().failure() == WifiFailure::NoSupplicant);
    WpaStatus status;
    CHECK_FALSE(backend.wifiStatus(status));

    TempDir bare("native_follow_nowifi");
    NativePaths barePaths = pathsIn(bare);
    NativeBackend noWifi(barePaths, runs.runner());
    noWifi.beginWifiConnect("Home Network");
    CHECK(noWifi.pumpWifiConnect().failure() == WifiFailure::NoInterface);
}

TEST_CASE("NativeBackend stops asking a bluetoothd that does not answer, for a while") {
    TempDir tmp("native_bt_hung");
    NativePaths paths = pathsIn(tmp);
    paths.btStatusTimeoutMs = 700;
    paths.btRetryMs = 200;
    auto state = std::make_shared<fakebluez::State>();
    state->objects[fakebluez::Adapter] = fakebluez::adapter("00:1A:7D:DA:71:13", true);
    state->errors["GetManagedObjects"] = {"org.freedesktop.DBus.Error.NoReply", "no reply within 700 ms"};
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), [&](BusError &) { return fakebluez::makeBus(state); });

    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() == "BlueZ did not answer in time (org.freedesktop.DBus.Error.NoReply: no reply within "
                                   "700 ms)");
    CHECK(state->refreshTimeouts == vector<int>{700}); // the status read's own timeout
    CHECK_FALSE(backend.btUp());                       // not asked again yet: the same reason
    CHECK(backend.btPairedDevices().empty());
    CHECK(state->refreshTimeouts.size() == 1);
    CHECK_FALSE(backend.btLastError().empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    state->errors.clear(); // it came back
    CHECK(backend.btUp());
    CHECK(state->refreshTimeouts.size() == 2);
}

TEST_CASE("NativeBackend: the paired pads with their batteries, and a pairing the hook cancels") {
    TempDir tmp("native_bt_live");
    NativePaths paths = pathsIn(tmp);
    paths.powerSupplyDir = tmp.at("ps");
    paths.btTimeouts.pollMs = 1;
    tmp.makeSubDir("ps/sony_controller_battery_a0:ab:51:33:44:55");
    tmp.writeFile("ps/sony_controller_battery_a0:ab:51:33:44:55/capacity", "40\n");
    tmp.writeFile("ps/sony_controller_battery_a0:ab:51:33:44:55/status", "Charging\n");
    auto state = std::make_shared<fakebluez::State>();
    state->objects["/org/bluez"]["org.bluez.AgentManager1"];
    state->objects[fakebluez::Adapter] = fakebluez::adapter("00:1A:7D:DA:71:13", true);
    state->objects[fakebluez::devicePath("A0:AB:51:33:44:55")] =
        fakebluez::device("A0:AB:51:33:44:55", "Wireless Controller", true, true);
    state->objects[fakebluez::devicePath("1C:A0:B8:12:34:56")] =
        fakebluez::device("1C:A0:B8:12:34:56", "Wireless Controller");
    state->asyncNeverAnswers = true;
    RecordedRuns runs;
    NativeBackend backend(paths, runs.runner(), [&](BusError &) { return fakebluez::makeBus(state); });

    vector<BtDevice> paired = backend.btPairedDevices();
    REQUIRE(paired.size() == 1);
    CHECK(paired[0].battery.percent == 40);
    CHECK(paired[0].battery.status == "Charging");

    REQUIRE(backend.btBeginPair("1C:A0:B8:12:34:56"));
    CHECK(backend.btPumpPair() == BtPairStage::Pairing);
    CHECK(state->waitHook); // the bus's waits go through the backend's hook
    int asked = 0;
    backend.setWaitHook([&]() { return ++asked < 3; });
    CHECK(state->waitHook() == true); // the bus asks the backend, which asks the screen
    backend.btCancelPair();
    CHECK(backend.btLastError() == "cancelled");
    CHECK(state->calls.back() == "UnregisterAgent /org/autobleem/pscbios/agent");
    CHECK_FALSE(backend.btPair("1C:A0:B8:12:34:56")); // the blocking form: the hook says stop between the pumps
    CHECK(backend.btLastError() == "cancelled");
}
