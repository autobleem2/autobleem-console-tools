//
// pscbios_core: the dev host's backend and the main screen's facts, ssid.cfg, gamecontrollerdb.txt editing
// and the mapping wizard's logic - everything PSC-Bios does that is not a screen.
//
#include "doctest/doctest.h"

#include "support/env_fixture.h"
#include "support/temp_dir.h"

#include "fake_bluez_bus.h"

#include "core/bt_device_list.h"
#include "core/console_backend.h"
#include "core/game_controller_db.h"
#include "core/network_status.h"
#include "core/nm_backend.h"
#include "core/pad_mapping.h"
#include "core/ssid_config.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

using std::string;
using std::vector;
using std::map;

TEST_CASE("FakeBackend pairs and removes Bluetooth controllers, and can report no adapter") {
    FakeBackend fake;
    CHECK(fake.btUp());
    CHECK(fake.btScan().size() == 3);
    REQUIRE(fake.btPairedDevices().size() == 1); // the Xbox pad is already paired
    CHECK(fake.btPairedDevices()[0].name == "Xbox Wireless Controller");

    CHECK(fake.btPair("00:1B:DC:0F:11:22")); // pair a DS4
    CHECK(fake.btPairedDevices().size() == 2);
    CHECK(fake.btRemove("A0:AB:51:33:44:55")); // forget the Xbox pad
    CHECK(fake.btPairedDevices().size() == 1);
    CHECK_FALSE(fake.btPair("no:such:mac"));

    fake.btAdapter_ = false;
    CHECK_FALSE(fake.btUp());
    CHECK(fake.btName().empty());
    CHECK(fake.btLastError() == "no Bluetooth adapter");
    CHECK(fake.btScan().size() == 3); // the fake still lists its devices; the screen gates on btUp()
}

TEST_CASE("NetworkStatus gathers the main screen's facts from the backend") {
    FakeBackend fake;
    NetworkStatus status;
    status.refresh(fake);
    CHECK(status.wirelessIface == "wlan0"); // the backend's WiFi interface, whatever its name
    CHECK(status.wirelessFound);
    CHECK(status.wirelessActive);
    CHECK(status.wirelessAddr == "192.168.1.23");
    CHECK_FALSE(status.ethFound);
    CHECK(status.ethIface.empty());
    CHECK(status.btActive);
    CHECK(status.btStatusText() == "Found/Active");
    CHECK(status.btDetails() == "hci0 PSC-BT 00:11:22:33:44:55");
    CHECK(status.timezone == "Europe/Warsaw");
    CHECK(NetworkStatus::dongleStatus(true, false) == "Found/Not active");
    CHECK(NetworkStatus::addressText(false, "") == "-");
    CHECK(NetworkStatus::addressText(true, "") == "Waiting for IP Address...");
    CHECK(NetworkStatus::addressText(true, "10.0.0.2") == "10.0.0.2");

    // no adapter: "Not found", and the reason as the details
    fake.btAdapter_ = false;
    status.refresh(fake);
    CHECK(status.btStatusText() == "Not found/Not active");
    CHECK(status.btDetails() == "no Bluetooth adapter");
    // the bus or BlueZ not answering: unavailable, with the reason
    status.btError = "BlueZ does not answer: org.freedesktop.DBus.Error.ServiceUnknown";
    CHECK(status.btStatusText() == "Unavailable");
    CHECK(status.btDetails() == status.btError);
    fake.btAdapter_ = true;

    fake.setTimezone("UTC");
    fake.configureWifi("Home Network", "secret");
    CHECK(fake.configuredSsid == "Home Network");
    CHECK(fake.timezone() == "UTC");
}

//*******************************
// FakeBackend: the state machines and the wait hook
//*******************************
TEST_CASE("FakeBackend pairs through the pumped state machine, and refuses the 8BitDo pad with a reason") {
    FakeBackend fake;
    REQUIRE(fake.btBeginPair("00:1B:DC:0F:11:22"));
    CHECK(fake.btPumpPair() == BtPairStage::Done);
    CHECK(fake.btPairConnected());
    CHECK(fake.btBattery("00:1B:DC:0F:11:22").percent == 80);

    REQUIRE(fake.btBeginPair("E4:17:D8:AA:BB:CC"));
    CHECK(fake.btPumpPair() == BtPairStage::Failed);
    CHECK(fake.btLastError() ==
          "the controller refused the pairing (org.bluez.Error.AuthenticationFailed: Authentication Failed)");

    CHECK_FALSE(fake.btBeginPair("no:such:mac"));
    CHECK(fake.btLastError() == "the controller is not known (no:such:mac)");
}

TEST_CASE("FakeBackend: a wait hook that says stop cancels a slow pairing and a slow scan") {
    FakeBackend fake(true); // the dev host's: every action takes its time
    int asked = 0;
    fake.setWaitHook([&]() { return ++asked < 2; });
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(fake.btPair("00:1B:DC:0F:11:22"));
    CHECK(fake.btLastError() == "cancelled");
    CHECK(fake.scanNetworks().empty());
    CHECK(fake.lastError() == "cancelled");
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(waited < 1500); // neither ran its full time
}

TEST_CASE("FakeBackend plays a WiFi connection out: connected, or a wrong password for Neighbour 5G") {
    FakeBackend fake;
    vector<WifiNetwork> networks = fake.scanNetworks();
    REQUIRE(networks.size() == 3);
    CHECK(networks[0].ssid == "Home Network");
    CHECK(networks[0].secured());
    CHECK_FALSE(networks[1].secured()); // Cafe Corner is open

    fake.beginWifiConnect("Home Network");
    const WifiConnectWatch &ok = fake.pumpWifiConnect();
    CHECK(ok.stage() == WifiConnectStage::Connected);
    CHECK(ok.text() == "Connected, 192.168.1.23");

    fake.beginWifiConnect("Neighbour 5G");
    const WifiConnectWatch &wrong = fake.pumpWifiConnect();
    CHECK(wrong.stage() == WifiConnectStage::Failed);
    CHECK(wrong.failure() == WifiFailure::WrongPassword);
    CHECK(wrong.text().compare(0, 16, "wrong password (") == 0);
    WpaStatus status;
    REQUIRE(fake.wifiStatus(status));
    CHECK(status.wpaState == "DISCONNECTED");
}

//*******************************
// WifiConnectWatch
//*******************************
TEST_CASE("WifiConnectWatch follows wpa_supplicant's states to Connected") {
    WifiConnectWatch watch("Home", 0);
    CHECK(watch.stage() == WifiConnectStage::Starting);
    watch.supplicantRunning(true, 100);
    CHECK(watch.stage() == WifiConnectStage::Searching);
    WpaStatus status;
    status.wpaState = "ASSOCIATING";
    watch.onStatus(status, "", 600);
    CHECK(watch.stage() == WifiConnectStage::Associating);
    status.wpaState = "4WAY_HANDSHAKE";
    watch.onStatus(status, "", 1100);
    CHECK(watch.stage() == WifiConnectStage::Handshake);
    CHECK(watch.text() == "Checking the password");
    status.wpaState = "COMPLETED";
    status.ssid = "Home";
    watch.onStatus(status, "", 1600);
    CHECK(watch.stage() == WifiConnectStage::GettingAddress);
    CHECK_FALSE(watch.finished());
    watch.onStatus(status, "192.168.1.40", 2100); // getifaddrs' address
    CHECK(watch.stage() == WifiConnectStage::Connected);
    CHECK(watch.address() == "192.168.1.40");
    CHECK(watch.text() == "Connected, 192.168.1.40");
    watch.onEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home\" reason=WRONG_KEY", 2200);
    CHECK(watch.stage() == WifiConnectStage::Connected); // finished is finished

    // COMPLETED on another network (what an earlier set-up left) is not ours
    WifiConnectWatch other("Home", 0);
    status.ssid = "Neighbour";
    other.onStatus(status, "10.0.0.2", 100);
    CHECK(other.stage() == WifiConnectStage::Associating);
    // STATUS's own ip_address will do when getifaddrs has none yet
    status.ssid = "Home";
    status.ipAddress = "10.0.0.3";
    other.onStatus(status, "", 200);
    CHECK(other.address() == "10.0.0.3");
}

TEST_CASE("WifiConnectWatch knows a wrong password from wpa_supplicant's events") {
    std::string detail;
    CHECK(WifiConnectWatch::failureOfEvent(
              "<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home\" auth_failures=1 duration=10 reason=WRONG_KEY",
              detail) == WifiFailure::WrongPassword);
    CHECK(detail == "CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home\" auth_failures=1 duration=10 reason=WRONG_KEY");
    CHECK(WifiConnectWatch::failureOfEvent("<3>WPA: 4-Way Handshake failed - pre-shared key may be incorrect",
                                           detail) == WifiFailure::WrongPassword);
    CHECK(WifiConnectWatch::failureOfEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"x\" reason=CONN_FAILED",
                                           detail) == WifiFailure::AssociationRejected);
    CHECK(WifiConnectWatch::failureOfEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"x\" reason=AUTH_FAILED",
                                           detail) == WifiFailure::AuthenticationRejected);
    CHECK(WifiConnectWatch::failureOfEvent("<3>CTRL-EVENT-SCAN-RESULTS ", detail) == WifiFailure::None);
    CHECK(WifiConnectWatch::eventText("<2>CTRL-EVENT-CONNECTED - Connection to aa:bb completed\n") ==
          "CTRL-EVENT-CONNECTED - Connection to aa:bb completed");

    WifiConnectWatch watch("Home", 0);
    watch.supplicantRunning(true, 10);
    watch.onEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home\" auth_failures=1 duration=10 reason=WRONG_KEY",
                  500);
    CHECK(watch.stage() == WifiConnectStage::Failed);
    CHECK(watch.failure() == WifiFailure::WrongPassword);
    CHECK(watch.text() == "wrong password (CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home\" auth_failures=1 "
                          "duration=10 reason=WRONG_KEY)");
}

TEST_CASE("WifiConnectWatch gives each way of not connecting its reason") {
    WifiConnectTimeouts t;
    t.startMs = 100;
    t.totalMs = 1000;
    t.notFoundEvents = 2;

    WifiConnectWatch never("Home", 0, t); // wpa_supplicant never came up
    never.tick(99);
    CHECK_FALSE(never.finished());
    never.tick(100);
    CHECK(never.failure() == WifiFailure::NoSupplicant);

    WifiConnectWatch notFound("Home", 0, t); // the network is not there
    notFound.supplicantRunning(true, 10);
    notFound.onEvent("<3>CTRL-EVENT-NETWORK-NOT-FOUND", 200);
    CHECK_FALSE(notFound.finished());
    notFound.onEvent("<3>CTRL-EVENT-NETWORK-NOT-FOUND", 400);
    CHECK(notFound.failure() == WifiFailure::NetworkNotFound);

    WifiConnectWatch searching("Home", 0, t); // an older wpa_supplicant says nothing: the time runs out searching
    searching.supplicantRunning(true, 10);
    searching.tick(1000);
    CHECK(searching.failure() == WifiFailure::NetworkNotFound);

    WifiConnectWatch rejected("Home", 0, t); // refused, tried again, never in
    rejected.supplicantRunning(true, 10);
    rejected.onEvent("<3>CTRL-EVENT-ASSOC-REJECT bssid=aa:bb:cc:dd:ee:ff status_code=17", 300);
    CHECK_FALSE(rejected.finished());
    rejected.tick(1000);
    CHECK(rejected.failure() == WifiFailure::AssociationRejected);
    CHECK(rejected.detail() == "CTRL-EVENT-ASSOC-REJECT bssid=aa:bb:cc:dd:ee:ff status_code=17");

    WifiConnectWatch noDhcp("Home", 0, t); // in, but no address
    WpaStatus completed;
    completed.wpaState = "COMPLETED";
    noDhcp.onStatus(completed, "", 50);
    noDhcp.tick(1000);
    CHECK(noDhcp.failure() == WifiFailure::NoAddress);
    CHECK(noDhcp.text() == "no address from the network (DHCP)");

    WifiConnectWatch stuck("Home", 0, t); // associating for ever
    WpaStatus associating;
    associating.wpaState = "ASSOCIATING";
    stuck.onStatus(associating, "", 50);
    stuck.tick(1000);
    CHECK(stuck.failure() == WifiFailure::Timeout);

    WifiConnectWatch cancelled("Home", 0, t);
    cancelled.cancel();
    CHECK(cancelled.failure() == WifiFailure::Cancelled);

    // a restart under the watch: back to Starting until it is up again, not a failure
    WifiConnectWatch restarted("Home", 0, t);
    restarted.supplicantRunning(true, 10);
    restarted.supplicantRunning(false, 50);
    CHECK(restarted.stage() == WifiConnectStage::Starting);
    restarted.supplicantRunning(true, 90);
    CHECK(restarted.stage() == WifiConnectStage::Searching);
}

TEST_CASE("The WiFi texts: the connection row, the signal, the stages") {
    CHECK(wpaStateText("COMPLETED") == "Connected");
    CHECK(wpaStateText("SCANNING") == "Looking for the network");
    CHECK(wpaStateText("4WAY_HANDSHAKE") == "Checking the password");
    CHECK(wpaStateText("ASSOCIATING") == "Connecting");
    CHECK(wpaStateText("DISCONNECTED") == "Not connected");
    CHECK(wpaStateText("INTERFACE_DISABLED") == "WiFi is off");
    CHECK(wpaStateText("").empty());
    CHECK(WifiNetwork::signalText(-40) == "Excellent");
    CHECK(WifiNetwork::signalText(-60) == "Good");
    CHECK(WifiNetwork::signalText(-70) == "Fair");
    CHECK(WifiNetwork::signalText(-85) == "Weak");
    WifiNetwork n;
    n.flags = "[WPA2-PSK-CCMP][ESS]";
    CHECK(n.secured());
    n.flags = "[WEP][ESS]";
    CHECK(n.secured());
    n.flags = "[ESS]";
    CHECK_FALSE(n.secured());
    CHECK(wifiFailureText(WifiFailure::WrongPassword) == "wrong password");
    CHECK(wifiStageText(WifiConnectStage::GettingAddress) == "Getting an address");
}

//*******************************
// BtDeviceList
//*******************************
namespace {
BtDevice btDevice(const string &mac, const string &name, bool paired, bool connected, int battery = -1) {
    BtDevice d;
    d.mac = mac;
    d.name = name;
    d.paired = paired;
    d.connected = connected;
    d.battery.percent = battery;
    d.battery.status = battery >= 0 ? "Discharging" : "";
    return d;
}
} // namespace

TEST_CASE("BtDeviceList merges the scan with the paired pads, and sees one drop") {
    BtDeviceList list;
    list.updatePaired({btDevice("AA", "Xbox", true, true, 55)});
    REQUIRE(list.rows().size() == 1);
    CHECK(list.rows()[0].state == BtRowState::Connected);
    CHECK(BtDeviceList::stateText(list.rows()[0]) == "connected, battery 55%");

    list.setScanned({btDevice("BB", "Wireless Controller", false, false)});
    REQUIRE(list.rows().size() == 2); // the paired pad the scan missed stays
    CHECK(list.find("BB")->state == BtRowState::New);
    CHECK(BtDeviceList::stateText(*list.find("BB")) == "new");

    // the Xbox pad goes out of range: dropped; back again: connected
    list.updatePaired({btDevice("AA", "Xbox", true, false)});
    CHECK(list.find("AA")->state == BtRowState::Dropped);
    CHECK(BtDeviceList::stateText(*list.find("AA")) == "dropped");
    list.updatePaired({btDevice("AA", "Xbox", true, true, 50)});
    CHECK(list.find("AA")->state == BtRowState::Connected);

    // forgotten elsewhere: back to new
    list.updatePaired({});
    CHECK(list.find("AA")->state == BtRowState::New);
    CHECK_FALSE(list.find("AA")->device.paired);

    // a paired pad never seen connected is just paired
    list.updatePaired({btDevice("CC", "8BitDo", true, false)});
    CHECK(list.find("CC")->state == BtRowState::Paired);
    CHECK(list.indexOf("CC") == 2);
    CHECK(list.indexOf("ZZ") == -1);
}

TEST_CASE("BtDeviceList follows a pairing's stages and how it ended") {
    BtDeviceList list;
    list.setScanned({btDevice("BB", "Wireless Controller", false, false)});
    list.pairStage("BB", BtPairStage::Discovering);
    CHECK(BtDeviceList::stateText(*list.find("BB")) == "discovering...");
    list.pairStage("BB", BtPairStage::WaitingForPaired);
    CHECK(BtDeviceList::stateText(*list.find("BB")) == "pairing...");
    list.updatePaired({}); // a refresh meanwhile does not undo the stage
    CHECK(list.find("BB")->state == BtRowState::Pairing);
    list.pairStage("BB", BtPairStage::Connecting);
    CHECK(BtDeviceList::stateText(*list.find("BB")) == "connecting...");

    list.pairFinished("BB", false, false, "the controller refused the pairing (org.bluez.Error.AuthenticationFailed)");
    CHECK(list.find("BB")->state == BtRowState::Failed);
    CHECK(BtDeviceList::stateText(*list.find("BB")) == "failed");
    CHECK_FALSE(list.find("BB")->error.empty());

    list.pairStage("BB", BtPairStage::Pairing); // tried again: the error goes
    CHECK(list.find("BB")->error.empty());
    list.pairFinished("BB", false, false, ""); // cancelled: new again
    CHECK(list.find("BB")->state == BtRowState::New);

    list.pairFinished("BB", true, false, "the controller is off or out of range (...)"); // paired, not connected
    CHECK(list.find("BB")->state == BtRowState::Paired);
    list.pairFinished("BB", true, true, "");
    CHECK(list.find("BB")->state == BtRowState::Connected);

    list.removed("BB", false, "the controller could not be removed (org.bluez.Error.Failed: nope)");
    CHECK(list.find("BB")->state == BtRowState::Connected);
    CHECK_FALSE(list.find("BB")->error.empty());
    list.removed("BB", true, "");
    CHECK(list.find("BB")->state == BtRowState::New);
}

TEST_CASE("BtDeviceList's battery text") {
    BtBattery b;
    CHECK(BtDeviceList::batteryText(b).empty());
    b.percent = 80;
    CHECK(BtDeviceList::batteryText(b) == "battery 80%");
    b.status = "Charging";
    CHECK(BtDeviceList::batteryText(b) == "charging 80%");
    b.status = "Full";
    CHECK(BtDeviceList::batteryText(b) == "battery full");
}

TEST_CASE("SsidConfig reads and writes the two-line ssid.cfg") {
    TempDir tmp("ssid");
    SsidConfig cfg;
    CHECK_FALSE(cfg.load(tmp.at("ssid.cfg")));

    tmp.writeFile("ssid.cfg", "Home\r\nsecret\r\n"); // CRLF survives
    REQUIRE(cfg.load(tmp.at("ssid.cfg")));
    CHECK(cfg.ssid == "Home");
    CHECK(cfg.password == "secret");

    REQUIRE(cfg.save(tmp.at("out.cfg")));
    CHECK(tmp.readFile("out.cfg") == "Home\nsecret\n");

    cfg.password.clear();
    CHECK_FALSE(cfg.save(tmp.at("out2.cfg"))); // no password: nothing written
}

TEST_CASE("SsidConfig reads a legacy ssid.cfg with a driver-mode third line, and ignores it") {
    TempDir tmp("ssid-legacy");
    SsidConfig cfg;
    tmp.writeFile("ssid.cfg", "Home\nsecret\nnl80211\n");
    REQUIRE(cfg.load(tmp.at("ssid.cfg")));
    CHECK(cfg.ssid == "Home");
    CHECK(cfg.password == "secret");

    REQUIRE(cfg.save(tmp.at("out.cfg"))); // written back with two lines only
    CHECK(tmp.readFile("out.cfg") == "Home\nsecret\n");
}

TEST_CASE("SsidConfig falls back to the SSID in wpa_supplicant.conf, except the old tool's \"1\"") {
    TempDir tmp("wpa");
    tmp.writeFile("wpa.conf", "network={\n    ssid=\"Cafe\"\n    psk=\"x\"\n}\n");
    CHECK(SsidConfig::ssidFromWpaSupplicant(tmp.at("wpa.conf")) == "Cafe");
    tmp.writeFile("wpa1.conf", "network={\n ssid=\"1\"\n}\n");
    CHECK(SsidConfig::ssidFromWpaSupplicant(tmp.at("wpa1.conf")) == "");
    CHECK(SsidConfig::ssidFromWpaSupplicant(tmp.at("missing.conf")) == "");
}

TEST_CASE("SsidConfig's paths follow the kernel config dir, and the app dir without one") {
    EnvFixture env;
    env.setAppDir("/apps/pscbios");
    ableem::Environment::setKernelConfigDir("");
    CHECK(SsidConfig::defaultPath() == "/apps/pscbios/ssid.cfg");
    CHECK(SsidConfig::wpaSupplicantPath() == "/apps/pscbios/wpa_supplicant.conf");
    ableem::Environment::setKernelConfigDir("/etc/autobleem");
    CHECK(SsidConfig::defaultPath() == "/etc/autobleem/ssid.cfg");
    CHECK(SsidConfig::wpaSupplicantPath() == "/etc/wpa_supplicant.conf");
}

TEST_CASE("GameControllerDb replaces a mapping by (platform, guid) and appends a new one under the marker") {
    TempDir tmp("gcdb");
    tmp.writeFile("db.txt", "# Game Controller DB\n"
                            "\n"
                            "0001,Pad One,a:b0,platform:Linux,\n"
                            "0001,Pad One,a:b1,platform:Windows,\n"
                            "#AutoBleem\n"
                            "0002,Pad Two,a:b2,platform:Linux,\n");
    GameControllerDb db;
    db.load(tmp.at("db.txt"));
    REQUIRE(db.lines().size() == 5); // the blank line is dropped
    CHECK(db.lines()[0].comment);
    CHECK(db.lines()[1].guid == "0001");
    CHECK(db.lines()[1].platform == "Linux");
    CHECK(db.lines()[2].platform == "Windows");

    db.replaceMapping("0001", "Linux", "0001,Pad One Fixed,a:b9,platform:Linux,");
    db.replaceMapping("0003", "Linux", "0003,Pad Three,a:b3,platform:Linux,");
    REQUIRE(db.save(tmp.at("db.txt")));
    CHECK(tmp.readFile("db.txt") == "# Game Controller DB\n"
                                    "0001,Pad One Fixed,a:b9,platform:Linux,\n"
                                    "0001,Pad One,a:b1,platform:Windows,\n"
                                    "#AutoBleem\n"
                                    "0002,Pad Two,a:b2,platform:Linux,\n"
                                    "0003,Pad Three,a:b3,platform:Linux,\n");
}

TEST_CASE("GameControllerDb adds the marker to a file without one, and starts empty without a file") {
    TempDir tmp("gcdb2");
    tmp.writeFile("plain.txt", "0001,Pad,a:b0,platform:Linux,\n");
    GameControllerDb db;
    db.load(tmp.at("plain.txt"));
    REQUIRE(db.lines().size() == 2);
    CHECK(db.lines()[1].text == "#AutoBleem");

    GameControllerDb fresh;
    fresh.load(tmp.at("absent.txt"));
    REQUIRE(fresh.lines().size() == 1);
    CHECK(fresh.lines()[0].text == "#AutoBleem");
    CHECK(GameControllerDb::platformOf("x,y,a:b0,platform:Mac OS X") == "Mac OS X");
    CHECK(GameControllerDb::platformOf("x,y,a:b0") == "");
    CHECK(GameControllerDb::guidOf("abcd,y,a:b0") == "abcd");
}

//*******************************
// PadMapping
//*******************************
namespace {
ableem::JoystickState rest(int axes, int buttons, int hats) {
    ableem::JoystickState s;
    s.axes.assign(axes, 0);
    s.buttons.assign(buttons, false);
    s.hats.assign(hats, ableem::Joystick::HatCentered);
    return s;
}
} // namespace

TEST_CASE("PadMapping asks for the 25 standard inputs in the wizard's order") {
    vector<PadMapping::Element> elements = PadMapping::standardElements();
    REQUIRE(elements.size() == 25);
    CHECK(elements[0].apiName == "a");
    CHECK(elements[0].button == 0);
    CHECK(elements[11].apiName == "lefttrigger");
    CHECK(elements[11].scan == PadMapping::Scan::Trigger);
    CHECK(elements[11].axis == 4);
    CHECK(elements[17].apiName == "-leftx");
    CHECK(elements[17].negative);
    CHECK(elements[24].apiName == "+righty");
    for (const PadMapping::Element &e : elements)
        CHECK(e.value.empty());
}

TEST_CASE("PadMapping::detectChange names the raw input that moved, a button before a hat before an axis") {
    ableem::JoystickState initial = rest(6, 12, 1);
    ableem::JoystickState now = initial;
    vector<PadMapping::Element> taken;
    CHECK(PadMapping::detectChange(initial, now, taken) == "");

    now.buttons[3] = true;
    CHECK(PadMapping::detectChange(initial, now, taken) == "b3");

    now = initial;
    now.hats[0] = ableem::Joystick::HatLeft;
    CHECK(PadMapping::detectChange(initial, now, taken) == "h0.8");
    now.hats[0] = ableem::Joystick::HatUp | ableem::Joystick::HatLeft; // a diagonal is not a mapping
    CHECK(PadMapping::detectChange(initial, now, taken) == "");

    now = initial;
    now.axes[1] = -32700; // a stick pushed up from the middle
    CHECK(PadMapping::detectChange(initial, now, taken) == "-a1");
    now.axes[1] = 32700;
    CHECK(PadMapping::detectChange(initial, now, taken) == "+a1");
    now.axes[1] = 20000; // not far enough
    CHECK(PadMapping::detectChange(initial, now, taken) == "");

    initial.axes[4] = -32768; // a trigger rests at one end
    now = initial;
    now.axes[4] = 32767;
    CHECK(PadMapping::detectChange(initial, now, taken) == "a4");

    // an input already given to an element is not offered again
    PadMapping::Element used;
    used.value = "a4";
    taken.push_back(used);
    CHECK(PadMapping::detectChange(initial, now, taken) == "");
}

TEST_CASE("PadMapping::anythingHeld is what the wizard waits to clear") {
    ableem::JoystickState initial = rest(2, 2, 1);
    ableem::JoystickState now = initial;
    CHECK_FALSE(PadMapping::anythingHeld(initial, now));
    now.axes[0] = 25000;
    CHECK(PadMapping::anythingHeld(initial, now));
    now.axes[0] = 10000;
    CHECK_FALSE(PadMapping::anythingHeld(initial, now));
    now.buttons[1] = true;
    CHECK(PadMapping::anythingHeld(initial, now));
}

TEST_CASE("PadMapping::finalElements merges stick halves, drops the unmapped and adds the platform") {
    vector<PadMapping::Element> scanned = PadMapping::standardElements();
    auto set = [&](const char *name, const char *value) {
        for (PadMapping::Element &e : scanned)
            if (e.apiName == name)
                e.value = value;
    };
    set("a", "b0");
    set("b", "b1");
    set("dpup", "h0.1");
    set("lefttrigger", "a2");
    set("-leftx", "-a0");
    set("+leftx", "+a0"); // a normal stick axis
    set("-lefty", "+a1");
    set("+lefty", "-a1"); // an inverted one
    set("-rightx", "-a3");
    set("+rightx", "b7");  // halves that cannot be merged stay as half keys
    set("-righty", "-a4"); // and a half alone goes

    vector<PadMapping::Element> finals = PadMapping::finalElements(scanned, "Linux");
    string line = PadMapping::mappingLine("0300abcd", "My Pad", finals);
    CHECK(line == "0300abcd,My Pad,a:b0,b:b1,lefttrigger:a2,dpup:h0.1,-rightx:-a3,+rightx:b7,leftx:a0,lefty:a1~,"
                  "platform:Linux,");
}

TEST_CASE("PadMapping::cleanName keeps what SDL accepts in a mapping line") {
    CHECK(PadMapping::cleanName("Sony PLAYSTATION(R)3 Controller, v2") == "Sony PLAYSTATIONR3 Controller v2");
}

//*******************************
// NmBackend - the answers are what a Raspberry Pi 400 (Raspberry Pi OS trixie, NetworkManager 1.52.1,
// BlueZ 5.82) printed on 2026-09-26. Ported against the new ConsoleBackend shape 2026-09-26: no
// hasDriverMode()/wlanOn() any more (X6 - the interface has neither), scanNetworks() returns
// vector<WifiNetwork> from SSID,SIGNAL,SECURITY (not a bare SSID list), configureWifi() takes no driver
// mode, and Bluetooth goes through the same BluezClient/fake bus NativeBackend's tests use - never the
// deleted `bt` bluetoothctl wrapper.
//*******************************
namespace {
const char *const DeviceStatus = "nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null";
const vector<string> PiDevices = {"wlan0:wifi:connected", "lo:loopback:connected (externally)",
                                  "p2p-dev-wlan0:wifi-p2p:disconnected", "eth0:ethernet:unavailable"};

// the Runner/LinesRunner test seam: NmBackend takes plain function pointers (no captures), so the script
// is global state a ScriptedShell resets before each test
vector<string> ran;
map<string, string> answers;
map<string, vector<string>> listings;

string scriptedRun(const string &cmd) {
    ran.push_back(cmd);
    auto it = answers.find(cmd);
    return it != answers.end() ? it->second : string();
}
vector<string> scriptedRunLines(const string &cmd) {
    ran.push_back(cmd);
    auto it = listings.find(cmd);
    return it != listings.end() ? it->second : vector<string>();
}
struct ScriptedShell {
    ScriptedShell() {
        ran.clear();
        answers.clear();
        listings.clear();
    }
};
} // namespace

TEST_CASE("NmBackend::splitTerse undoes nmcli's escapes") {
    CHECK(NmBackend::splitTerse("wlan0:wifi:connected") == vector<string>{"wlan0", "wifi", "connected"});
    CHECK(NmBackend::splitTerse("yes:My\\:Net\\\\5G:63") == vector<string>{"yes", "My:Net\\5G", "63"});
    CHECK(NmBackend::splitTerse("no::92:WPA2") == vector<string>{"no", "", "92", "WPA2"});
    CHECK(NmBackend::splitTerse("") == vector<string>{""});
}

TEST_CASE("NmBackend reads the devices, addresses and time zone the way a Pi 400 answers") {
    ScriptedShell shell;
    listings[DeviceStatus] = PiDevices;
    answers["nmcli -g IP4.ADDRESS device show 'wlan0' 2>/dev/null"] = "192.168.68.144/24";
    answers["nmcli -g IP4.ADDRESS device show 'eth0' 2>/dev/null"] = "";
    answers["timedatectl show -p Timezone --value 2>/dev/null"] = "Europe/Dublin";
    listings["timedatectl list-timezones 2>/dev/null"] = {"Africa/Abidjan", "Africa/Accra", "Europe/Dublin",
                                                          "Africa/Accra"};
    // SSID,SIGNAL,SECURITY - a hidden network (no name) dropped; SIGNAL is nmcli's 0-100 percentage,
    // approximated to dBm (percent/2 - 100), sorted strongest first
    listings["nmcli -t -f SSID,SIGNAL,SECURITY device wifi list --rescan yes 2>/dev/null"] = {
        "DecoMeshArt:88:WPA2", "DecoMeshArt_Guest:70:--", ":50:WPA2", "FRITZ!Box 4040 BN:60:WPA2",
        "Mark's Office 2.4ghz (Private):40:WPA2"};
    listings["nmcli -t -f ACTIVE,SSID device wifi list --rescan no 2>/dev/null"] = {
        "no:DecoMeshArt", "no:DecoMeshArt_Guest", "no:", "yes:DecoMeshArt", "no:SKY45280"};
    NmBackend backend(scriptedRun, scriptedRunLines, true, nullptr);

    CHECK(backend.kernelInstalled()); // here: nmcli is there
    CHECK(backend.keepsWifiSettings());
    CHECK(backend.interfaceFound("wlan0"));
    CHECK(backend.interfaceFound("eth0"));
    CHECK(backend.isUp("wlan0"));
    CHECK_FALSE(backend.isUp("eth0")); // unplugged: "unavailable"
    CHECK(backend.ipOf("wlan0") == "192.168.68.144");
    CHECK(backend.ipOf("eth0") == "");
    CHECK(backend.timezone() == "Europe/Dublin");
    CHECK(backend.listTimezones() == vector<string>{"Africa/Abidjan", "Africa/Accra", "Europe/Dublin"});

    vector<WifiNetwork> networks = backend.scanNetworks();
    REQUIRE(networks.size() == 4); // the hidden one dropped
    CHECK(networks[0].ssid == "DecoMeshArt");
    CHECK(networks[0].signal == 88 / 2 - 100);
    CHECK(networks[0].flags == "[WPA2]");
    CHECK(networks[0].secured());
    CHECK(networks[1].ssid == "DecoMeshArt_Guest");
    CHECK(networks[1].flags.empty()); // SECURITY "--" -> no flags
    CHECK_FALSE(networks[1].secured());
    CHECK(networks[2].ssid == "FRITZ!Box 4040 BN");
    CHECK(networks[3].ssid == "Mark's Office 2.4ghz (Private)");
    CHECK(backend.currentSsid() == "DecoMeshArt");

    ran.clear();
    backend.setTimezone("America/New_York");
    CHECK(ran == vector<string>{"timedatectl set-timezone 'America/New_York' 2>&1"});
}

TEST_CASE("NmBackend maps the screens' wlan0/eth0 to a PC's slot-named devices") {
    ScriptedShell shell;
    listings[DeviceStatus] = {"enp3s0:ethernet:connected", "wlp2s0:wifi:disconnected", "lo:loopback:unmanaged"};
    answers["nmcli -g IP4.ADDRESS device show 'enp3s0' 2>/dev/null"] = "10.0.2.15/24 | 10.0.3.1/16";
    NmBackend backend(scriptedRun, scriptedRunLines, true, nullptr);
    // wifiInterface()/ethernetInterface() are what the screens actually ask for - the real slot names,
    // never a hard-coded "wlan0"/"eth0"
    CHECK(backend.wifiInterface() == "wlp2s0");
    CHECK(backend.ethernetInterface() == "enp3s0");
    CHECK(backend.interfaceFound("wlp2s0"));
    CHECK_FALSE(backend.isUp("wlp2s0"));
    CHECK(backend.isUp("enp3s0"));
    CHECK(backend.ipOf("enp3s0") == "10.0.2.15");
    CHECK_FALSE(backend.interfaceFound("usb0"));

    // no Wi-Fi device at all: scanNetworks() reports it and asks nothing else
    listings[DeviceStatus] = {"enp3s0:ethernet:connected"};
    ran.clear();
    CHECK(backend.scanNetworks().empty());
    CHECK(backend.lastError() == "no WiFi interface");
    CHECK(ran == vector<string>{DeviceStatus});
}

TEST_CASE("NmBackend connects on restartNetwork, the password through the environment, never the command") {
    ScriptedShell shell;
    listings[DeviceStatus] = PiDevices;
    NmBackend backend(scriptedRun, scriptedRunLines, true, nullptr);
    // an empty answer is not success (restartNetwork() requires !answer.empty() && no "Error:") - nmcli's
    // real success line names the connection it just brought up
    const string ok = "Device 'wlan0' successfully activated with '11111111-2222-3333-4444-555555555555'.";

    backend.configureWifi("Mark's Office", "s3cr3t pass"); // remembered only - no driver mode argument any more
    for (const string &cmd : ran)
        CHECK(cmd.find("connect") == string::npos);
    ran.clear();
    answers["nmcli --wait 30 device wifi connect 'Mark'\\''s Office' password \"$AB_WIFI_PSK\" ifname 'wlan0' "
            "2>&1"] = ok;
    backend.restartNetwork();
    REQUIRE_FALSE(ran.empty());
    CHECK(ran.back() ==
          "nmcli --wait 30 device wifi connect 'Mark'\\''s Office' password \"$AB_WIFI_PSK\" ifname 'wlan0' 2>&1");
    for (const string &cmd : ran)
        CHECK(cmd.find("s3cr3t") == string::npos);
    CHECK(backend.lastError().empty());

    // an open network: no password argument; a restart with nothing new reconnects the device
    backend.configureWifi("Cafe", "");
    answers["nmcli --wait 30 device wifi connect 'Cafe' ifname 'wlan0' 2>&1"] = ok;
    backend.restartNetwork();
    CHECK(ran.back() == "nmcli --wait 30 device wifi connect 'Cafe' ifname 'wlan0' 2>&1");
    answers["nmcli --wait 30 device connect 'wlan0' 2>&1"] = ok;
    backend.restartNetwork();
    CHECK(ran.back() == "nmcli --wait 30 device connect 'wlan0' 2>&1");

    // nmcli's own "Error:" in the answer: the network could not be joined, the answer kept as the detail
    backend.configureWifi("Neighbour 5G", "wrongpass");
    answers["nmcli --wait 30 device wifi connect 'Neighbour 5G' password \"$AB_WIFI_PSK\" ifname 'wlan0' 2>&1"] =
        "Error: Connection activation failed: Secrets were required, but not provided.";
    backend.restartNetwork();
    CHECK(backend.lastError() ==
          "the network could not be joined (Error: Connection activation failed: Secrets were required, "
          "but not provided.)");

    // beginWifiConnect/pumpWifiConnect just report restartNetwork()'s own outcome - nmcli already waited
    // out the whole attempt, there is no wpa_supplicant event stream to follow afterwards
    backend.configureWifi("Mark's Office", "s3cr3t pass");
    answers["nmcli --wait 30 device wifi connect 'Mark'\\''s Office' password \"$AB_WIFI_PSK\" ifname 'wlan0' "
            "2>&1"] = ok;
    answers["nmcli -g IP4.ADDRESS device show 'wlan0' 2>/dev/null"] = "192.168.68.144/24"; // onStatus needs an address to reach Connected
    backend.restartNetwork();
    backend.beginWifiConnect("Mark's Office");
    const WifiConnectWatch &watch = backend.pumpWifiConnect();
    CHECK(watch.finished());
    CHECK(watch.stage() == WifiConnectStage::Connected);
}

TEST_CASE("NmBackend without nmcli asks nothing of the network") {
    ScriptedShell shell;
    listings[DeviceStatus] = PiDevices;
    NmBackend backend(scriptedRun, scriptedRunLines, false, nullptr);
    CHECK_FALSE(backend.kernelInstalled());
    CHECK_FALSE(backend.interfaceFound("wlan0"));
    CHECK(backend.currentSsid().empty());
    CHECK(ran.empty());
}

//*******************************
// NmBackend: Bluetooth, over the same BluezClient/fake bus as NativeBackend (fake_bluez_bus.h) - never
// bluetoothctl or the deleted `bt` wrapper
//*******************************
TEST_CASE("NmBackend's Bluetooth goes through the shared BluezClient, exactly like NativeBackend's") {
    ScriptedShell shell;
    listings[DeviceStatus] = PiDevices;

    auto state = std::make_shared<fakebluez::State>();
    state->objects["/org/bluez"]["org.bluez.AgentManager1"];
    state->objects[fakebluez::Adapter] = fakebluez::adapter("DC:A6:32:E3:04:97", true);
    state->objects[fakebluez::devicePath("A0:AB:51:33:44:55")] =
        fakebluez::device("A0:AB:51:33:44:55", "Xbox Wireless Controller", true, true);
    state->discoverable[fakebluez::devicePath("00:1B:DC:0F:11:22")] =
        fakebluez::device("00:1B:DC:0F:11:22", "Wireless Controller");
    int connects = 0;
    NmBackend backend(scriptedRun, scriptedRunLines, true,
                      [&](BusError &) {
                          ++connects;
                          return fakebluez::makeBus(state);
                      });

    CHECK(backend.btUp());
    CHECK(backend.btName() == "hci0 PSC DC:A6:32:E3:04:97"); // fakebluez::adapter()'s fixed Alias "PSC"

    vector<BtDevice> paired = backend.btPairedDevices();
    REQUIRE(paired.size() == 1);
    CHECK(paired[0].mac == "A0:AB:51:33:44:55");
    CHECK(paired[0].connected);

    vector<BtDevice> found = backend.btScan();
    REQUIRE(found.size() == 2);
    bool sawNew = false;
    for (const BtDevice &d : found)
        if (d.mac == "00:1B:DC:0F:11:22")
            sawNew = true;
    CHECK(sawNew);

    CHECK(backend.btPair("00:1B:DC:0F:11:22"));
    CHECK(backend.btLastError().empty());
    CHECK(backend.btPairedDevices().size() == 2);
    CHECK(connects == 1); // one bus connection for all of it, exactly like NativeBackend

    CHECK(backend.btRemove("A0:AB:51:33:44:55"));
    CHECK_FALSE(backend.btRemove("A0:AB:51:33:44:55"));
    CHECK(backend.btLastError() == "the controller is not known (A0:AB:51:33:44:55)");
}

TEST_CASE("NmBackend without a BlueZ bus factory: no Bluetooth support") {
    ScriptedShell shell;
    listings[DeviceStatus] = PiDevices;
    NmBackend backend(scriptedRun, scriptedRunLines, true, nullptr); // a host with no libdbus-1
    CHECK_FALSE(backend.btUp());
    CHECK(backend.btLastError() == "no Bluetooth support");
}

TEST_CASE("PadMapping: the raw input behind Circle, and whether it is held (the wizard's hold-to-exit)") {
    const string line = "030000004c0500006802000011010000,PS3 Controller,a:b0,b:b1,x:b3,y:b2,dpup:h0.1,"
                        "lefttrigger:a2,-leftx:-a0,platform:Linux,";
    CHECK(PadMapping::rawInput(line, "b") == "b1");
    CHECK(PadMapping::rawInput(line, "a") == "b0");
    CHECK(PadMapping::rawInput(line, "dpup") == "h0.1");
    CHECK(PadMapping::rawInput(line, "back") == "");
    CHECK(PadMapping::rawInput("", "b") == "");

    // this session's mapping wins over the pad's old line; neither: unknown
    vector<PadMapping::Element> elements = PadMapping::standardElements();
    CHECK(PadMapping::circleInput(elements, line) == "b1");
    CHECK(PadMapping::circleInput(elements, "") == "");
    for (PadMapping::Element &e : elements)
        if (e.apiName == "b")
            e.value = "b7";
    CHECK(PadMapping::circleInput(elements, line) == "b7");

    ableem::JoystickState initial = rest(3, 8, 1);
    initial.axes[2] = -32768; // a trigger at rest
    ableem::JoystickState now = initial;
    CHECK_FALSE(PadMapping::inputHeld("b1", initial, now));
    now.buttons[1] = true;
    CHECK(PadMapping::inputHeld("b1", initial, now));
    CHECK_FALSE(PadMapping::inputHeld("b9", initial, now)); // no such button
    now.hats[0] = 1;
    CHECK(PadMapping::inputHeld("h0.1", initial, now));
    CHECK_FALSE(PadMapping::inputHeld("h0.4", initial, now));
    now.axes[0] = -30000;
    CHECK(PadMapping::inputHeld("-a0", initial, now));
    CHECK_FALSE(PadMapping::inputHeld("+a0", initial, now));
    CHECK_FALSE(PadMapping::inputHeld("a2", initial, now));
    now.axes[2] = 32767;
    CHECK(PadMapping::inputHeld("a2", initial, now));
    CHECK_FALSE(PadMapping::inputHeld("", initial, now));
    CHECK_FALSE(PadMapping::inputHeld("x1", initial, now));
}

TEST_CASE("PadMapping::isExitKey is Power or a keyboard's Esc/Backspace, never a pad button") {
    CHECK(PadMapping::isExitKey(ableem::Key::Sleep));
    CHECK(PadMapping::isExitKey(ableem::Key::Escape));
    CHECK(PadMapping::isExitKey(ableem::Key::Backspace));
    // a real pad's Circle is a Button, never a Key - the wizard's loop() only calls isExitKey on a
    // KeyDown, so a short press of a real pad's Circle cannot reach it at all; only these keys, or the
    // 2 s hold (PadMapping::inputHeld on the raw joystick), leave the wizard
    CHECK_FALSE(PadMapping::isExitKey(ableem::Key::Return));
    CHECK_FALSE(PadMapping::isExitKey(ableem::Key::Reset));
    CHECK_FALSE(PadMapping::isExitKey(ableem::Key::Open));
    CHECK_FALSE(PadMapping::isExitKey(ableem::Key::Other));
}
