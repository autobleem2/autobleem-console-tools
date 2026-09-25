//
// pscbios_core's BluezClient against a scripted BlueZ (fake_bluez_bus.h): GetManagedObjects taken apart, the
// batteries in a fake power_supply tree, the agent's yes/no, and the pairing's state machine - the steps the old
// `bt` wrapper learned on the console, each failure with the D-Bus error behind it. Built on every host.
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"

#include "fake_bluez_bus.h"

#include "core/bluez_client.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using std::string;
using std::vector;
using namespace fakebluez;

namespace {
const char *const Ds4 = "1C:A0:B8:12:34:56";
const char *const Other = "E4:17:D8:AA:BB:CC";

BluezTimeouts fastTimeouts() {
    BluezTimeouts t;
    t.discoverMs = 400;
    t.pairMs = 600;
    t.pairedWaitMs = 300;
    t.connectMs = 400;
    t.pollMs = 1;
    return t;
}

std::shared_ptr<State> consoleWithAdapter(bool powered = true) {
    auto state = std::make_shared<State>();
    state->objects["/org/bluez"]["org.bluez.AgentManager1"];
    state->objects[Adapter] = adapter("00:1A:7D:DA:71:13", powered);
    return state;
}

std::unique_ptr<BluezClient> clientFor(const std::shared_ptr<State> &state, const string &powerSupply = "") {
    return std::unique_ptr<BluezClient>(
        new BluezClient(makeBus(state), powerSupply.empty() ? "/nonexistent" : powerSupply, fastTimeouts()));
}

bool called(const std::shared_ptr<State> &state, const string &prefix) {
    return std::any_of(state->calls.begin(), state->calls.end(),
                       [&](const string &c) { return c.compare(0, prefix.size(), prefix) == 0; });
}

// the calls with their first word only, in order - the shape of a sequence
vector<string> verbs(const std::shared_ptr<State> &state) {
    vector<string> out;
    for (const string &c : state->calls)
        out.push_back(c.substr(0, c.find(' ')));
    return out;
}
} // namespace

//*******************************
// parsing
//*******************************
TEST_CASE("BluezClient takes the first adapter and only its devices out of GetManagedObjects") {
    DbusManagedObjects objects;
    objects["/org/bluez/hci1"] = adapter("00:00:00:00:00:01", false);
    objects["/org/bluez/hci0"] = adapter("00:1A:7D:DA:71:13", true);
    objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller", true, true);
    objects[devicePath(Other)] = device(Other, "");
    objects[devicePath("AA:AA:AA:AA:AA:AA", "/org/bluez/hci1")] =
        device("AA:AA:AA:AA:AA:AA", "Elsewhere", false, false, "/org/bluez/hci1");
    objects[devicePath(Ds4)]["org.bluez.Device1"]["RSSI"] = DbusValue::ofInt(-62);
    objects[devicePath(Ds4)]["org.bluez.Battery1"]["Percentage"] = DbusValue::ofInt(80);

    BluezAdapter a;
    REQUIRE(BluezClient::parseAdapter(objects, a));
    CHECK(a.path == "/org/bluez/hci0");
    CHECK(a.hciName() == "hci0");
    CHECK(a.address == "00:1A:7D:DA:71:13");
    CHECK(a.alias == "PSC");
    CHECK(a.powered);

    vector<BluezDevice> devices = BluezClient::parseDevices(objects, a.path);
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].address == Ds4); // sorted by address
    CHECK(devices[0].displayName() == "Wireless Controller");
    CHECK(devices[0].paired);
    CHECK(devices[0].trusted);
    CHECK(devices[0].connected);
    CHECK(devices[0].icon == "input-gaming");
    CHECK(devices[0].modalias == "usb:v054Cp09CCd0100");
    CHECK(devices[0].deviceClass == 0x002508);
    CHECK(devices[0].hasRssi);
    CHECK(devices[0].rssi == -62);
    CHECK(devices[0].batteryPercent == 80);
    CHECK(devices[1].address == Other);
    CHECK(devices[1].displayName() == "E4-17-D8-AA-BB-CC"); // no Name: BlueZ's alias
    CHECK_FALSE(devices[1].paired);
    CHECK_FALSE(devices[1].hasRssi);
    CHECK(devices[1].batteryPercent == -1);

    BluezDevice bare;
    bare.address = Other;
    CHECK(bare.displayName() == Other);

    DbusManagedObjects none;
    none["/org/bluez"]["org.bluez.AgentManager1"];
    CHECK_FALSE(BluezClient::parseAdapter(none, a));
    CHECK(a.path.empty());
    CHECK(BluezClient::devicePathSuffix("1c:a0:b8:12:34:56") == "/dev_1C_A0_B8_12_34_56");
}

TEST_CASE("BluezClient reads a pad's battery from power_supply, else from BlueZ") {
    TempDir tmp("bluez_battery");
    tmp.makeSubDir("ps");
#ifndef _WIN32 // the sysfs names carry the address with its colons, which no Windows file name can
    tmp.makeSubDir("ps/sony_controller_battery_1c:a0:b8:12:34:56");
    tmp.writeFile("ps/sony_controller_battery_1c:a0:b8:12:34:56/capacity", "75\n");
    tmp.writeFile("ps/sony_controller_battery_1c:a0:b8:12:34:56/status", "Discharging\n");
    tmp.makeSubDir("ps/ps-controller-battery-e4:17:d8:aa:bb:cc");
    tmp.writeFile("ps/ps-controller-battery-e4:17:d8:aa:bb:cc/capacity", "100\n");
    tmp.writeFile("ps/ps-controller-battery-e4:17:d8:aa:bb:cc/status", "Full\n");

    BtBattery ds4 = BluezClient::readSysfsBattery(tmp.at("ps"), Ds4); // upper-case asked, lower-case on disk
    CHECK(ds4.percent == 75);
    CHECK(ds4.status == "Discharging");
    BtBattery dualsense = BluezClient::readSysfsBattery(tmp.at("ps"), Other);
    CHECK(dualsense.percent == 100);
    CHECK(dualsense.status == "Full");
    CHECK_FALSE(BluezClient::readSysfsBattery(tmp.at("ps"), "00:11:22:33:44:55").known());
#endif

    // no power_supply entry: Battery1.Percentage
    auto state = consoleWithAdapter();
    state->objects[devicePath("00:11:22:33:44:55")] = device("00:11:22:33:44:55", "8BitDo", true, true);
    state->objects[devicePath("00:11:22:33:44:55")]["org.bluez.Battery1"]["Percentage"] = DbusValue::ofInt(40);
    auto client = clientFor(state, tmp.at("ps"));
    REQUIRE(client->refresh());
    CHECK(client->battery("00:11:22:33:44:55").percent == 40);
    CHECK(client->battery("00:11:22:33:44:55").status.empty());
#ifndef _WIN32
    CHECK(client->battery(Ds4).percent == 75); // sysfs wins
#endif
}

//*******************************
// the agent
//*******************************
TEST_CASE("BluezClient's agent says yes to the device being paired only, and only while it is") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->asyncNeverAnswers = true; // stay in Pairing
    auto client = clientFor(state);

    AgentRequest confirm;
    confirm.method = "RequestConfirmation";
    confirm.device = devicePath(Ds4);
    CHECK_FALSE(client->answerAgent(confirm).ok); // not pairing anything

    REQUIRE(client->beginPair("1c:a0:b8:12:34:56")); // any case
    CHECK(client->pairingAddress() == Ds4);
    CHECK(client->agentRegistered());
    CHECK(client->answerAgent(confirm).ok);

    AgentRequest authorize = confirm;
    authorize.method = "AuthorizeService";
    authorize.text = "00001124-0000-1000-8000-00805f9b34fb";
    CHECK(client->answerAgent(authorize).ok);
    AgentRequest authorization = confirm;
    authorization.method = "RequestAuthorization";
    CHECK(client->answerAgent(authorization).ok);

    AgentRequest pin = confirm;
    pin.method = "RequestPinCode";
    AgentReply pinReply = client->answerAgent(pin);
    CHECK(pinReply.ok);
    CHECK(pinReply.pinCode == "0000");
    AgentRequest passkey = confirm;
    passkey.method = "RequestPasskey";
    CHECK(client->answerAgent(passkey).ok);
    CHECK(client->answerAgent(passkey).passkey == 0);

    AgentRequest stranger = confirm;
    stranger.device = devicePath(Other);
    AgentReply no = client->answerAgent(stranger);
    CHECK_FALSE(no.ok);
    CHECK(no.errorName == "org.bluez.Error.Rejected");
    AgentRequest otherAdapter = confirm;
    otherAdapter.device = devicePath(Ds4, "/org/bluez/hci1");
    CHECK_FALSE(client->answerAgent(otherAdapter).ok);
    AgentRequest unknown = confirm;
    unknown.method = "SomethingNew";
    CHECK_FALSE(client->answerAgent(unknown).ok);

    AgentRequest cancel;
    cancel.method = "Cancel";
    CHECK(client->answerAgent(cancel).ok);
    AgentRequest release;
    release.method = "Release";
    CHECK(client->answerAgent(release).ok);
    CHECK_FALSE(client->agentRegistered()); // BlueZ let go: nothing to unregister

    client->cancelPair();
    CHECK(client->pairStage() == BtPairStage::Failed);
    CHECK_FALSE(client->answerAgent(confirm).ok);
}

//*******************************
// pairing
//*******************************
TEST_CASE("BluezClient pairs a known pad: agent, trust, pair, connect, agent gone") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->agentCallsDuringPair = {{"RequestConfirmation", devicePath(Ds4)}, {"AuthorizeService", devicePath(Ds4)}};
    auto client = clientFor(state);

    vector<BtPairStage> seen;
    CHECK(client->pair(Ds4, [&](BtPairStage s) {
        if (seen.empty() || seen.back() != s)
            seen.push_back(s);
    }));
    CHECK(client->pairStage() == BtPairStage::Done);
    CHECK(client->pairConnected());
    CHECK(client->lastError().empty());
    CHECK(verbs(state) ==
          vector<string>{"RegisterAgent", "RequestDefaultAgent", "Set", "Pair", "Connect", "UnregisterAgent"});
    CHECK(state->calls[0] == "RegisterAgent /org/autobleem/pscbios/agent NoInputNoOutput");
    CHECK(state->calls[1] == "RequestDefaultAgent /org/bluez /org/autobleem/pscbios/agent");
    CHECK(state->calls[2] == "Set Trusted=true " + devicePath(Ds4)); // trusted BEFORE the pairing
    REQUIRE(state->agentReplies.size() == 2);
    CHECK(state->agentReplies[0].ok);
    CHECK(state->agentReplies[1].ok);
    CHECK_FALSE(client->agentRegistered());
    CHECK(seen.front() == BtPairStage::Pairing);
    CHECK(seen.back() == BtPairStage::Done);
    const BluezDevice *d = client->findDevice(Ds4);
    REQUIRE(d != nullptr);
    CHECK(d->paired);
    CHECK(d->connected);
}

TEST_CASE("BluezClient discovers an unknown pad before pairing it, and stops its discovery") {
    auto state = consoleWithAdapter();
    state->discoverable[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    auto client = clientFor(state);

    REQUIRE(client->beginPair(Ds4));
    CHECK(client->pairStage() == BtPairStage::Discovering);
    CHECK(client->discoveryIsOurs());
    while (client->pairStage() != BtPairStage::Done && client->pairStage() != BtPairStage::Failed)
        client->pump(1);
    CHECK(client->pairStage() == BtPairStage::Done);
    CHECK(verbs(state) == vector<string>{"RegisterAgent", "RequestDefaultAgent", "SetDiscoveryFilter", "StartDiscovery",
                                         "StopDiscovery", "Set", "Pair", "Connect", "UnregisterAgent"});
    CHECK(called(state, "SetDiscoveryFilter /org/bluez/hci0 bredr"));
    CHECK_FALSE(state->discovering);
}

TEST_CASE("BluezClient gives up on a pad that never shows up, and cleans up") {
    auto state = consoleWithAdapter();
    auto client = clientFor(state);
    CHECK_FALSE(client->pair(Ds4));
    CHECK(client->pairStage() == BtPairStage::Failed);
    CHECK(client->lastError() == "the controller was not found - is it in pairing mode?");
    CHECK(called(state, "StopDiscovery"));
    CHECK(called(state, "UnregisterAgent"));
    CHECK_FALSE(called(state, "Pair"));
}

TEST_CASE("BluezClient reports a refused pairing with BlueZ's error") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->errors["Pair"] = {"org.bluez.Error.AuthenticationFailed", "Authentication Failed"};
    auto client = clientFor(state);
    CHECK_FALSE(client->pair(Ds4));
    CHECK(client->pairStage() == BtPairStage::Failed);
    CHECK(client->lastError() ==
          "the controller refused the pairing (org.bluez.Error.AuthenticationFailed: Authentication Failed)");
    CHECK(client->lastBusError().name == "org.bluez.Error.AuthenticationFailed");
    CHECK(state->calls.back() == "UnregisterAgent /org/autobleem/pscbios/agent");
    CHECK_FALSE(called(state, "Connect"));
}

TEST_CASE("BluezClient's agent turning another device down does not stop the pairing") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->objects[devicePath(Other)] = device(Other, "Somebody's phone");
    state->agentCallsDuringPair = {{"RequestAuthorization", devicePath(Other)},
                                   {"RequestConfirmation", devicePath(Ds4)}};
    auto client = clientFor(state);
    CHECK(client->pair(Ds4));
    REQUIRE(state->agentReplies.size() == 2);
    CHECK_FALSE(state->agentReplies[0].ok);
    CHECK(state->agentReplies[1].ok);
}

TEST_CASE("BluezClient takes AlreadyExists as paired, and a pad that is connected already as done") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller", true, true);
    state->errors["Pair"] = {"org.bluez.Error.AlreadyExists", "Already Exists"};
    auto client = clientFor(state);
    CHECK(client->pair(Ds4));
    CHECK(client->pairConnected());
    CHECK_FALSE(called(state, "Connect")); // connected already
}

TEST_CASE("BluezClient counts a pairing whose connection failed as paired, and says why") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->errors["Connect"] = {"org.bluez.Error.Failed", "br-connection-page-timeout"};
    auto client = clientFor(state);
    CHECK(client->pair(Ds4));
    CHECK(client->pairStage() == BtPairStage::Done);
    CHECK_FALSE(client->pairConnected());
    // the reason BlueZ's error stands for, the error after it
    CHECK(client->lastError() ==
          "the controller is off or out of range (org.bluez.Error.Failed: br-connection-page-timeout)");
    CHECK(called(state, "UnregisterAgent"));
}

TEST_CASE("BluezClient fails when Pair returns but the pad never becomes paired") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->pairSetsPaired = false;
    auto client = clientFor(state);
    CHECK_FALSE(client->pair(Ds4));
    CHECK(client->lastError() == "the pairing failed (Paired did not become true)");
}

TEST_CASE("BluezClient gives up on a Pair that is never answered") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->asyncNeverAnswers = true;
    BluezTimeouts t = fastTimeouts();
    t.pairMs = 50; // + the 2 s backstop
    BluezClient client(makeBus(state), "/nonexistent", t);
    CHECK_FALSE(client.pair(Ds4));
    CHECK(client.lastBusError().name == "org.freedesktop.DBus.Error.NoReply");
    CHECK(called(state, "UnregisterAgent"));
}

TEST_CASE("BluezClient powers the adapter on to pair, and says why it cannot start") {
    auto state = consoleWithAdapter(false);
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    auto client = clientFor(state);
    CHECK(client->pair(Ds4));
    CHECK(state->calls.front() == "Set Powered=true /org/bluez/hci0");

    // no adapter at all
    auto bare = std::make_shared<State>();
    auto noAdapter = clientFor(bare);
    CHECK_FALSE(noAdapter->pair(Ds4));
    CHECK(noAdapter->lastError() == "no Bluetooth adapter");
    CHECK(bare->calls.empty()); // no agent registered for nothing

    // bluetoothd not running: the bus's answer
    auto gone = std::make_shared<State>();
    gone->errors["GetManagedObjects"] = {"org.freedesktop.DBus.Error.ServiceUnknown",
                                         "The name org.bluez was not provided by any .service files"};
    auto unreachable = clientFor(gone);
    CHECK_FALSE(unreachable->pair(Ds4));
    CHECK(unreachable->pairStage() == BtPairStage::Failed);
    CHECK(unreachable->lastError() ==
          "BlueZ (bluetoothd) is not running (org.freedesktop.DBus.Error.ServiceUnknown: The name org.bluez was not "
          "provided by any .service files)");

    // the agent refused: nothing else is tried
    auto refused = consoleWithAdapter();
    refused->errors["RegisterAgent"] = {"org.bluez.Error.AlreadyExists", "Already Exists"};
    auto noAgent = clientFor(refused);
    CHECK_FALSE(noAgent->pair(Ds4));
    CHECK(noAgent->lastError() ==
          "the pairing agent cannot be registered (org.bluez.Error.AlreadyExists: Already Exists)");
    CHECK(verbs(refused) == vector<string>{"RegisterAgent"});

    // not made the default: the agent goes again
    auto notDefault = consoleWithAdapter();
    notDefault->errors["RequestDefaultAgent"] = {"org.bluez.Error.DoesNotExist", "Does Not Exist"};
    auto client2 = clientFor(notDefault);
    CHECK_FALSE(client2->pair(Ds4));
    CHECK(verbs(notDefault) == vector<string>{"RegisterAgent", "RequestDefaultAgent", "UnregisterAgent"});
}

TEST_CASE("BluezClient cancels a pairing: the call dropped, the agent unregistered") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->asyncNeverAnswers = true;
    auto client = clientFor(state);
    REQUIRE(client->beginPair(Ds4));
    client->pump(1); // trust, Pair started
    CHECK(client->pairStage() == BtPairStage::Pairing);
    client->cancelPair();
    CHECK(client->pairStage() == BtPairStage::Failed);
    CHECK(client->lastError() == "cancelled");
    CHECK(state->calls.back() == "UnregisterAgent /org/autobleem/pscbios/agent");
    CHECK(state->asyncState == BluezBus::AsyncState::None);
}

TEST_CASE("BluezClient: the destructor ends a pairing still running") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->asyncNeverAnswers = true;
    {
        auto client = clientFor(state);
        REQUIRE(client->beginPair(Ds4));
    }
    CHECK(state->calls.back() == "UnregisterAgent /org/autobleem/pscbios/agent");
}

//*******************************
// scan, remove, disconnect
//*******************************
TEST_CASE("BluezClient scans: discovery started, devices found, discovery stopped") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Other)] = device(Other, "8BitDo Pro 2", true);
    state->discoverable[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    auto client = clientFor(state);
    int rounds = 0;
    REQUIRE(client->scan(30, [&]() {
        ++rounds;
        return true;
    }));
    CHECK(rounds > 0);
    CHECK(client->devices().size() == 2);
    CHECK(client->findDevice(Ds4) != nullptr);
    CHECK(verbs(state) == vector<string>{"SetDiscoveryFilter", "StartDiscovery", "StopDiscovery"});

    // someone else's discovery running: joined, and left running
    state->calls.clear();
    state->errors["StartDiscovery"] = {"org.bluez.Error.InProgress", "Operation already in progress"};
    REQUIRE(client->scan(5));
    CHECK_FALSE(called(state, "StopDiscovery"));

    // a refused filter only means LE devices too
    state->calls.clear();
    state->errors.clear();
    state->errors["SetDiscoveryFilter"] = {"org.freedesktop.DBus.Error.UnknownMethod", "no such method"};
    CHECK(client->scan(5));

    state->errors["StartDiscovery"] = {"org.bluez.Error.NotReady", "Resource Not Ready"};
    CHECK_FALSE(client->scan(5));
    CHECK(client->lastError() == "the Bluetooth adapter is not ready (org.bluez.Error.NotReady: Resource Not Ready)");
}

TEST_CASE("BluezClient removes and disconnects a device by its address") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller", true, true);
    auto client = clientFor(state);
    REQUIRE(client->disconnect(Ds4));
    CHECK(called(state, "Disconnect " + devicePath(Ds4)));
    REQUIRE(client->removeDevice("1c:a0:b8:12:34:56"));
    CHECK(called(state, "RemoveDevice /org/bluez/hci0 " + devicePath(Ds4)));
    CHECK(client->findDevice(Ds4) == nullptr);
    CHECK_FALSE(client->removeDevice(Ds4));
    CHECK(client->lastError() == "the controller is not known (" + string(Ds4) + ")");

    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller", true, false);
    state->errors["RemoveDevice"] = {"org.bluez.Error.Failed", "nope"};
    CHECK_FALSE(client->removeDevice(Ds4));
    CHECK(client->lastError() == "the controller could not be removed (org.bluez.Error.Failed: nope)");
}

TEST_CASE("BluezClient's scan ends early when it is asked to stop, and keeps what it found") {
    auto state = consoleWithAdapter();
    state->discoverable[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->appearAfterPumps = 1;
    auto client = clientFor(state);
    int rounds = 0;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(client->scan(10000, [&]() { return ++rounds < 3; })); // Circle on the third frame
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(waited < 5000);
    CHECK(rounds == 3);
    CHECK(client->findDevice(Ds4) != nullptr);
    CHECK(called(state, "StopDiscovery")); // ours, stopped
}

TEST_CASE("BluezClient reads the status with a short timeout, and puts the bus's back") {
    auto state = consoleWithAdapter();
    auto client = clientFor(state);
    REQUIRE(client->refresh(1500));
    REQUIRE(client->refresh());
    CHECK(state->refreshTimeouts == vector<int>{1500, 5000});
    CHECK(state->callTimeout == 5000);
}

TEST_CASE("BluezClient says what a D-Bus error means, and the error after it") {
    auto reason = [](const char *name, const char *message = "") { return BluezClient::reasonFor({name, message}); };
    CHECK(reason("org.bluez.Error.AuthenticationFailed") == "the controller refused the pairing");
    CHECK(reason("org.bluez.Error.AuthenticationRejected") == "the pairing was rejected");
    CHECK(reason("org.bluez.Error.AuthenticationCanceled") == "the pairing was cancelled");
    CHECK(reason("org.bluez.Error.AuthenticationTimeout") == "the controller did not answer - is it in pairing mode?");
    CHECK(reason("org.bluez.Error.ConnectionAttemptFailed") ==
          "the controller did not answer - is it in pairing mode?");
    CHECK(reason("org.bluez.Error.Failed", "Host is down") == "the controller is off or out of range");
    CHECK(reason("org.bluez.Error.Failed", "br-connection-page-timeout") == "the controller is off or out of range");
    CHECK(reason("org.bluez.Error.Failed", "something else").empty()); // the step's own reason then
    CHECK(reason("org.bluez.Error.NotReady") == "the Bluetooth adapter is not ready");
    CHECK(reason("org.bluez.Error.InProgress") == "Bluetooth is busy with something else");
    CHECK(reason("org.bluez.Error.DoesNotExist") == "the controller is not known");
    CHECK(reason("org.freedesktop.DBus.Error.NoReply") == "BlueZ did not answer in time");
    CHECK(reason("org.freedesktop.DBus.Error.ServiceUnknown") == "BlueZ (bluetoothd) is not running");
    CHECK(reason("org.freedesktop.DBus.Error.Disconnected") == "the system bus went away");
    CHECK(reason("org.freedesktop.DBus.Error.AccessDenied") == "the system bus refused PSC-Bios");
    CHECK(reason(BluezClient::CancelledError) == "cancelled");
    CHECK(reason("").empty());
}

TEST_CASE("BluezClient: a wait the user stopped fails the pairing as cancelled") {
    auto state = consoleWithAdapter();
    state->objects[devicePath(Ds4)] = device(Ds4, "Wireless Controller");
    state->errors["Set Trusted"] = {BluezClient::CancelledError, "stopped by the user"}; // Circle during the call
    auto client = clientFor(state);
    CHECK_FALSE(client->pair(Ds4));
    CHECK(client->lastError() == "cancelled (org.autobleem.Error.Cancelled: stopped by the user)");
    CHECK(state->calls.back() == "UnregisterAgent /org/autobleem/pscbios/agent");
}
