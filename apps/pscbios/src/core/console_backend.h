//
// ConsoleBackend: everything PSC-Bios asks the console for - the network interfaces, WiFi, Bluetooth, the
// timezone. The interface is what the screens use; NativeBackend (native_backend.h: sysfs, wpa_supplicant's
// control socket, BlueZ over D-Bus, the kernel's settime) is the console, FakeBackend a dev host with canned
// answers (the ProcessRunner pattern: the extension picks one, nothing else knows the difference).
//
// Nothing here may freeze a screen (docs/native-backend-plan.md, step 4). A call that waits - a scan, a pairing's
// D-Bus calls, wpa_supplicant starting, a program run - calls the wait hook every few tens of milliseconds while
// it waits: the screen's busy spinner turns there and the pad is read, and a hook that answers false (Circle)
// ends the wait early where that means something. The long ones are also state machines the screen pumps once a
// frame: the pairing (btBeginPair/btPumpPair) and a WiFi connection (beginWifiConnect/pumpWifiConnect). The
// status reads a screen repeats (btUp, wifiStatus) use short timeouts and stop asking a daemon that did not
// answer for a while.
//
#pragma once

#include "wifi_connect.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// called while the backend waits; false = the user asked to stop (the wait ends, the call says "cancelled")
using WaitHook = std::function<bool()>;

//******************
// BtBattery
//******************
struct BtBattery {
    int percent = -1;   // -1: nothing known
    std::string status; // sysfs's "Charging", "Discharging", "Full", ... ("" from BlueZ)
    bool known() const { return percent >= 0; }
};

//******************
// BtDevice
//******************
// a Bluetooth device the pairing screen lists: its address, its name, whether it is paired and connected, and
// its battery while it reports one.
struct BtDevice {
    std::string mac;  // "AA:BB:CC:DD:EE:FF"
    std::string name; // "Wireless Controller" (may be the mac again when the name is unknown)
    bool paired = false;
    bool connected = false;
    BtBattery battery;
};

//******************
// BtPairStage
//******************
enum class BtPairStage { Idle, Discovering, Trusting, Pairing, WaitingForPaired, Connecting, Done, Failed };
const char *btPairStageName(BtPairStage stage); // "Discovering", ... for the log

//******************
// ConsoleBackend
//******************
class ConsoleBackend {
public:
    virtual ~ConsoleBackend() = default;

    // the screen's hook while one of its calls waits (nullptr: none - the call just waits)
    virtual void setWaitHook(WaitHook hook) { waitHook_ = std::move(hook); }

    virtual bool kernelInstalled() = 0; // the AutoBleem kernel, whose overlay the network set-up and settime need

    // network - the interfaces by what they are, under whatever name the kernel gave them
    virtual std::string wifiInterface() = 0;     // the WiFi dongle's interface ("wlan0", "wlx00c0ca..."); "" for none
    virtual std::string ethernetInterface() = 0; // a wired dongle's ("eth0", "usb0", "enx..."); "" for none
    virtual bool interfaceFound(const std::string &iface) = 0;
    virtual bool isUp(const std::string &iface) = 0;
    virtual std::string ipOf(const std::string &iface) = 0; // "" while there is none
    // the networks in range, strongest first, one per SSID (hidden ones left out); empty without WiFi, or when the
    // hook stopped it before anything came back
    virtual std::vector<WifiNetwork> scanNetworks() = 0;
    virtual void configureWifi(const std::string &ssid, const std::string &password) = 0;
    virtual void restartNetwork() = 0;
    // the connection now (wpa_supplicant's STATUS, a short timeout): false when nothing manages the WiFi interface
    virtual bool wifiStatus(WpaStatus &out) = 0;
    // following a connection to `ssid` after configureWifi/restartNetwork: begin, then pump once a frame until
    // finished(); the watch stays readable until the next begin
    virtual void beginWifiConnect(const std::string &ssid) = 0;
    virtual const WifiConnectWatch &pumpWifiConnect() = 0;
    // why the last WiFi or timezone action (scanNetworks, configureWifi, restartNetwork, setTimezone) did not do
    // what was asked - a reason for the user, the technical detail after it in brackets; "" after one that did
    virtual std::string lastError() const { return ""; }

    // bluetooth
    virtual bool btUp() = 0; // a Bluetooth adapter (usually a USB dongle) is present and up
    virtual std::string btName() = 0;

    // bluetooth controller pairing (needs btUp()). DualShock 3 pairs over USB through the sixaxis plugin and is
    // not listed here - this is for DualShock 4 and other standard Bluetooth gamepads.
    virtual std::vector<BtDevice> btScan() = 0;          // discovery for a few seconds (the hook can end it early)
    virtual std::vector<BtDevice> btPairedDevices() = 0; // devices already paired/known, with their batteries
    // the pairing as a state machine: begin (false: it failed at once, btLastError() says why), then pump once a
    // frame until Done or Failed; Done with btPairConnected() false is paired but not connected (btLastError())
    virtual bool btBeginPair(const std::string &mac) = 0;
    virtual BtPairStage btPumpPair() = 0;
    virtual void btCancelPair() = 0;
    virtual bool btPairConnected() const = 0;
    // begin + pump until it ends, the hook asked between the pumps (Circle cancels); true when paired
    bool btPair(const std::string &mac);
    virtual bool btRemove(const std::string &mac) = 0; // unpair/forget; true on success
    virtual BtBattery btBattery(const std::string &mac) = 0;
    // why the last Bluetooth call failed or found no adapter ("" after one that worked): no adapter, the system
    // bus or BlueZ not answering, a pairing refused - a reason for the user, the D-Bus error after it
    virtual std::string btLastError() const { return ""; }

    // time
    virtual std::string timezone() = 0;
    virtual void setTimezone(const std::string &zone) = 0;
    virtual std::vector<std::string> listTimezones() = 0; // sorted, unique

    // what differs between the console (its own settings file) and NetworkManager (NmBackend, a Pi and the PC
    // stick, which keeps the credentials itself)
    virtual bool keepsWifiSettings() { return false; } // it stores the credentials itself: no ssid.cfg
    virtual std::string currentSsid() { return ""; }   // the network it is connected to, "" when unknown

protected:
    // the hook once; true when there is none. Every wait the backend does goes through here
    bool keepWaiting() const { return !waitHook_ || waitHook_(); }
    WaitHook waitHook_;
};

//******************
// FakeBackend
//******************
// a dev host: a WiFi dongle on 192.168.1.23, no ethernet, a Bluetooth dongle, three networks to pick from,
// Europe/Warsaw and a short timezone list; configure/restart/setTimezone only log and update what the fake then
// reports (so the screens show their effect). `slow` (the dev host's, not the tests') makes every action take
// about the time the console's does, through the wait hook, so the spinner, the stages and a cancel can be seen:
// a Bluetooth scan 4 s, a pairing 4 s in three stages (the 8BitDo pad's is refused), a WiFi connection 4-5 s
// ("Neighbour 5G" ends in a wrong password), and the Xbox pad drops its connection a few refreshes into the
// pairing screen.
class FakeBackend : public ConsoleBackend {
public:
    explicit FakeBackend(bool slow = false) : slow_(slow) {}

    bool kernelInstalled() override { return true; }
    std::string wifiInterface() override { return "wlan0"; }
    std::string ethernetInterface() override { return ""; }
    bool interfaceFound(const std::string &iface) override { return iface == "wlan0"; }
    bool isUp(const std::string &iface) override { return iface == "wlan0"; }
    std::string ipOf(const std::string &iface) override { return iface == "wlan0" ? "192.168.1.23" : ""; }
    std::vector<WifiNetwork> scanNetworks() override;
    void configureWifi(const std::string &ssid, const std::string &password) override;
    void restartNetwork() override;
    bool wifiStatus(WpaStatus &out) override;
    void beginWifiConnect(const std::string &ssid) override;
    const WifiConnectWatch &pumpWifiConnect() override;
    std::string lastError() const override { return lastError_; }
    bool btUp() override { return btAdapter_; }
    std::string btName() override { return btAdapter_ ? "hci0 PSC-BT 00:11:22:33:44:55" : ""; }
    std::vector<BtDevice> btScan() override;
    std::vector<BtDevice> btPairedDevices() override;
    bool btBeginPair(const std::string &mac) override;
    BtPairStage btPumpPair() override;
    void btCancelPair() override;
    bool btPairConnected() const override { return pairStage_ == BtPairStage::Done; }
    bool btRemove(const std::string &mac) override;
    BtBattery btBattery(const std::string &mac) override;
    std::string btLastError() const override; // "no Bluetooth adapter" while btAdapter_ is cleared
    std::string timezone() override { return timezone_; }
    void setTimezone(const std::string &zone) override;
    std::vector<std::string> listTimezones() override;

    std::string configuredSsid, configuredPassword; // what configureWifi() was given
    int restarts = 0;
    bool btAdapter_ = true; // a test can clear this to exercise the "no adapter" path

private:
    // `ms` of work (none unless slow_), the hook asked every 20 ms; false when it said stop
    bool work(int ms);
    long long nowMs() const;
    BtDevice *device(const std::string &mac);

    bool slow_;
    std::string timezone_ = "Europe/Warsaw";
    std::string lastError_, btError_;
    std::string connectedSsid_ = "Home Network";
    std::unique_ptr<WifiConnectWatch> watch_;
    long long watchStart_ = 0;
    std::string pairMac_;
    BtPairStage pairStage_ = BtPairStage::Idle;
    long long pairStart_ = 0;
    int pairedReads_ = 0;
    // the fake's nearby devices; btScan reveals them, a pairing / btRemove flip `paired`
    std::vector<BtDevice> btDevices_ = {
        {"00:1B:DC:0F:11:22", "Wireless Controller", false, false, {}}, // a DS4 in pairing mode
        {"E4:17:D8:AA:BB:CC", "8BitDo Pro 2", false, false, {}},
        {"A0:AB:51:33:44:55", "Xbox Wireless Controller", true, true, {55, "Discharging"}} // paired in a past session
    };
};
