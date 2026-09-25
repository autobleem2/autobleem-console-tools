//
// BluezClient: BlueZ (bluetoothd) as PSC-Bios needs it - the adapter, the devices it knows, discovery, pairing a
// gamepad and forgetting one - over a BluezBus (DbusBluezBus on the console: libdbus-1 on the system bus).
//
// Pairing is a state machine the caller drives: beginPair(mac), then pump() until the stage is Done or Failed -
// a screen calls pump() once a frame under its busy spinner, pair() does the same in a loop for a caller that
// can block. The steps are those the old `bt` bluetoothctl wrapper learned on the console: discover until the
// device is known (bluetoothd forgets an unpaired device soon after a scan ends), trust it BEFORE pairing (a
// DualShock 4 opens its HID channels the moment it is paired, and an untrusted device has BlueZ ask the agent to
// authorise them), Pair (AlreadyExists is fine), wait for Paired, Connect. For the pairing's duration PSC-Bios
// has an agent of its own (NoInputNoOutput, made the default) that says yes to that one device only; it is
// unregistered afterwards, and BlueZ's previous default - abbtagent, the payload's cable-pairing agent - is the
// default again. Every failure leaves the D-Bus error's name and message in lastError(), and in the log.
//
// The battery: /sys/class/power_supply/{sony_controller_battery_,ps-controller-battery-}<mac> (hid-sony,
// hid-playstation), else BlueZ's Battery1.Percentage.
//
#pragma once

#include "bluez_bus.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

//******************
// BluezAdapter / BluezDevice / BtBattery
//******************
struct BluezAdapter {
    std::string path;    // "/org/bluez/hci0"
    std::string address; // "00:1A:7D:DA:71:13"
    std::string name;    // the system's name ("psc")
    std::string alias;   // what it is shown as (defaults to name)
    bool powered = false;
    bool discovering = false;
    std::string hciName() const; // "hci0" - the path's last element
};

struct BluezDevice {
    std::string path;    // "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
    std::string address; // "AA:BB:CC:DD:EE:FF"
    std::string name;    // "" until the device told it
    std::string alias;   // BlueZ's: the name, or the address with dashes
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    std::string modalias; // "usb:v054Cp09CCd0100"
    std::string icon;     // "input-gaming"
    uint32_t deviceClass = 0;
    bool hasRssi = false;    // RSSI is there only while discovery sees the device
    int rssi = 0;            // dBm
    int batteryPercent = -1; // org.bluez.Battery1, -1 without one

    // the name to show: Name, else Alias, else the address
    std::string displayName() const;
};

struct BtBattery {
    int percent = -1;   // -1: nothing known
    std::string status; // sysfs's "Charging", "Discharging", "Full", ... ("" from BlueZ)
    bool known() const { return percent >= 0; }
};

//******************
// BluezTimeouts
//******************
struct BluezTimeouts {
    int discoverMs = 15000;  // a pairing's discovery until the device is known
    int pairMs = 30000;      // Device1.Pair's reply
    int pairedWaitMs = 5000; // Paired=true after Pair returned
    int connectMs = 15000;   // Device1.Connect's reply
    int pollMs = 500;        // how often the device list is read again while waiting for it
};

//******************
// BtPairStage
//******************
enum class BtPairStage { Idle, Discovering, Trusting, Pairing, WaitingForPaired, Connecting, Done, Failed };
const char *btPairStageName(BtPairStage stage); // "Discovering", ... for the log

//******************
// BluezClient
//******************
class BluezClient {
public:
    static const char *const AgentPath;       // "/org/autobleem/pscbios/agent"
    static const char *const AgentCapability; // "NoInputNoOutput"
    static const char *const DefaultPowerSupplyDir;

    explicit BluezClient(std::unique_ptr<BluezBus> bus, std::string powerSupplyDir = DefaultPowerSupplyDir,
                         BluezTimeouts timeouts = BluezTimeouts());
    ~BluezClient(); // a pairing still running is cancelled (the agent unregistered), our discovery stopped
    BluezClient(const BluezClient &) = delete;
    BluezClient &operator=(const BluezClient &) = delete;

    // GetManagedObjects: the first adapter (by path) and its devices. False, with lastError(), when BlueZ does
    // not answer; true with no adapter when it answers without one (hasAdapter() says which)
    bool refresh();
    bool hasAdapter() const { return !adapter_.path.empty(); }
    const BluezAdapter &adapter() const { return adapter_; }
    const std::vector<BluezDevice> &devices() const { return devices_; } // sorted by address
    const BluezDevice *findDevice(const std::string &mac) const;         // any case; nullptr when unknown

    bool setPowered(bool on);
    // discovery (BR/EDR - the gamepads PSC-Bios pairs are classic Bluetooth). InProgress (someone else's
    // discovery) counts as started; then stopDiscovery() leaves it alone
    bool startDiscovery();
    bool stopDiscovery();
    bool discoveryIsOurs() const { return ourDiscovery_; }
    // discovery for durationMs, pumped (progress each round), then stopped and the list read
    bool scan(int durationMs, const std::function<void()> &progress = nullptr);
    bool removeDevice(const std::string &mac);
    bool disconnect(const std::string &mac);
    BtBattery battery(const std::string &mac) const;

    // pairing
    bool beginPair(const std::string &mac); // false (Failed, lastError()) when it cannot even start
    BtPairStage pump(int timeoutMs = 50);   // one round: the bus read, the stage advanced; returns it
    void cancelPair();                      // Failed, "cancelled"; the agent unregistered
    BtPairStage pairStage() const { return stage_; }
    const std::string &pairingAddress() const { return pairMac_; }
    bool pairConnected() const { return pairConnected_; } // Done: whether Connect succeeded too
    // beginPair + pump until Done/Failed; true when paired (lastError() then says why it did not connect, if not)
    bool pair(const std::string &mac, const std::function<void(BtPairStage)> &progress = nullptr);

    // the agent's answer to BlueZ (public for the tests): yes only to the device being paired, while it is
    AgentReply answerAgent(const AgentRequest &request);
    bool agentRegistered() const { return agentRegistered_; }

    const std::string &lastError() const { return lastError_; }
    const BusError &lastBusError() const { return lastBusError_; } // the D-Bus error behind lastError(), if any
    BluezBus &bus() { return *bus_; }

    // GetManagedObjects' reply taken apart - static, tested without a bus
    static bool parseAdapter(const DbusManagedObjects &objects, BluezAdapter &out); // the first Adapter1
    static std::vector<BluezDevice> parseDevices(const DbusManagedObjects &objects, const std::string &adapterPath);
    // the power_supply entry of a pad's battery; percent -1 when there is none
    static BtBattery readSysfsBattery(const std::string &powerSupplyDir, const std::string &mac);
    // "AA:BB:..." -> "/dev_AA_BB_..." (the end of the device's object path)
    static std::string devicePathSuffix(const std::string &mac);

private:
    bool fail(const std::string &what, const BusError &error = BusError());
    void finishPair();
    void enter(BtPairStage stage);
    long long msSince(std::chrono::steady_clock::time_point start) const;
    bool pollDue();
    bool isPairingDevice(const std::string &devicePath) const;
    void pumpDiscovering();
    void pumpPairing();
    void pumpWaitingForPaired();
    void pumpConnecting();

    std::unique_ptr<BluezBus> bus_;
    std::string powerSupplyDir_;
    BluezTimeouts timeouts_;
    BluezAdapter adapter_;
    std::vector<BluezDevice> devices_;
    bool ourDiscovery_ = false;
    bool agentRegistered_ = false;

    BtPairStage stage_ = BtPairStage::Idle;
    std::string pairMac_;
    bool pairConnected_ = false;
    std::chrono::steady_clock::time_point stageStart_;
    std::chrono::steady_clock::time_point lastPoll_;

    std::string lastError_;
    BusError lastBusError_;
};
