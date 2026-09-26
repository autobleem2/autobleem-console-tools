//
// NativeBackend: the console asked directly instead of through the kernel's /bin/abnet - the network
// interfaces from sysfs, their addresses from getifaddrs, WiFi through wpa_supplicant's control socket
// (WpaCtrlClient), Bluetooth through BlueZ over D-Bus (BluezClient). Any interface name, not only wlan0: a
// wireless interface is one sysfs gives a `wireless` or `phy80211` entry. The timezone is /etc/timezone and the
// zoneinfo tables, set through the kernel's settime (run directly, no shell). No script of any kind is asked -
// docs/native-backend-plan.md. Linux only.
//
#pragma once

#include "bluez_client.h"
#include "console_backend.h"
#include "wpa_ctrl_client.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class WpaEventMonitor;

//******************
// NativePaths
//******************
// where the console keeps what the backend reads and writes - replaced by a temp tree in the tests
struct NativePaths {
    std::string sysClassNet = "/sys/class/net";
    std::string wpaRunDir = "/var/run/wpa_supplicant";          // wpa_supplicant's ctrl_interface DIR
    std::string wpaSupplicantConf = "/etc/wpa_supplicant.conf"; // what dhcpcd's hook starts it with
    // the ctrl_interface GROUP written into a new wpa_supplicant.conf - left out when the system has no such
    // group (wpa_supplicant refuses an unknown one), and when empty
    std::string ctrlGroup = "netdev";
    int supplicantStartMs = 6000;  // how long a started wpa_supplicant is waited for (its socket appearing)
    int wpaStatusTimeoutMs = 1000; // a STATUS a screen repeats: never longer than this
    WifiConnectTimeouts wifiTimeouts;
    std::string powerSupplyDir = "/sys/class/power_supply"; // the pads' batteries (hid-sony, hid-playstation)
    int btScanMs = 8000;                                    // btScan()'s discovery
    BluezTimeouts btTimeouts;
    // the status reads a screen repeats (btUp, btPairedDevices): GetManagedObjects waits no longer than
    // btStatusTimeoutMs, and after one that got no reply BlueZ is not asked again for btRetryMs - a bluetoothd that
    // holds its name but never answers costs a screen one short stall, not one per refresh
    int btStatusTimeoutMs = 1500;
    int btRetryMs = 15000;
    // the AutoBleem kernel: its overlay's /bin/abnet is what the 2020 tool (and AbnetBackend) checked - only
    // looked at, never run; the overlay also brings settime and dhcpcd's wpa_supplicant hook
    std::string kernelMarker = "/bin/abnet";
    std::string settime = "/bin/settime";            // the kernel's `settime tzone <zone>` (a bash script)
    std::string timezoneFile = "/etc/timezone";      // what `settime tz` printed (it is a cat of this file)
    std::string localtime = "/etc/localtime";        // the zone's link, when /etc/timezone says nothing
    std::string zoneinfoDir = "/usr/share/zoneinfo"; // zone1970.tab / zone.tab - timedatectl's list
};

//******************
// NativeBackend
//******************
class NativeBackend : public ConsoleBackend {
public:
    // runs a program and waits: System::runAndWait on the console (the wait hook asked every 100 ms meanwhile), a
    // recording fake in the tests
    using CommandRunner = std::function<int(const std::string &exe, const std::vector<std::string> &args)>;

    // the console: its paths, System::runAndWait, the system bus (DbusBluezBus - where libdbus was found at build
    // time)
    NativeBackend();
    // bluez connects the BluezClient's bus when Bluetooth is first asked for (and again after a failure);
    // none = Bluetooth unavailable, with that as the reason
    NativeBackend(NativePaths paths, CommandRunner run, BluezBusFactory bluez = nullptr);
    ~NativeBackend() override;

    void setWaitHook(WaitHook hook) override;
    bool kernelInstalled() override;

    // network
    bool interfaceFound(const std::string &iface) override;
    bool wlanOn(); // some wireless interface exists and is up
    bool isUp(const std::string &iface) override;
    std::string ipOf(const std::string &iface) override;
    std::vector<WifiNetwork> scanNetworks() override;
    void configureWifi(const std::string &ssid, const std::string &password) override;
    void restartNetwork() override;
    bool wifiStatus(WpaStatus &out) override; // of wifiInterface()
    // the connection followed on an attached control connection (re-attached when wpa_supplicant restarts), STATUS
    // every 500 ms and the interface's address
    void beginWifiConnect(const std::string &ssid) override;
    const WifiConnectWatch &pumpWifiConnect() override;

    // bluetooth, through BlueZ: an adapter that is there and powered (powered on once, if it is not);
    // "hci0 <alias> <address>"; btScan discovers for btScanMs (the hook can end it early); the pairing is
    // BluezClient's state machine; each false/empty with the reason in btLastError()
    bool btUp() override;
    std::string btName() override;
    std::vector<BtDevice> btScan() override;
    std::vector<BtDevice> btPairedDevices() override;
    bool btBeginPair(const std::string &mac) override;
    BtPairStage btPumpPair() override;
    BtPairStage btCancelPair() override;
    bool btPairConnected() const override;
    bool btRemove(const std::string &mac) override;
    BtBattery btBattery(const std::string &mac) override;
    // the client, connected on first use; nullptr (and btLastError()) when the system bus cannot be reached
    BluezClient *bluez();
    std::string btLastError() const override { return btError_; }

    // time: /etc/timezone (else the zone /etc/localtime links to); the kernel's `settime tzone <zone>` - run
    // directly, a zone from the list only; the zones of zone1970.tab and zone.tab whose file is there, and UTC -
    // what timedatectl list-timezones answered
    std::string timezone() override;
    void setTimezone(const std::string &zone) override;
    std::vector<std::string> listTimezones() override;

    // every interface sysfs lists ("lo" included), sorted
    std::vector<std::string> interfaces();
    bool isWireless(const std::string &iface);
    std::vector<std::string> wirelessInterfaces(); // sorted
    // the one WiFi is set up on: the first wireless interface that is up, else the first there is; "" for none
    std::string wifiInterface() override;
    // a wired interface: the first (sorted) that is ARPHRD_ETHER, has a `device` (hardware, not a bridge or a
    // tunnel), is not wireless and is not the AutoBleem kernel's USB gadget (rndis*) or Bluetooth PAN (bnep*)
    std::string ethernetInterface() override;
    // the connection's state from wpa_supplicant (wpaStatusTimeoutMs); false when it is not running on the interface
    bool wifiStatus(const std::string &iface, WpaStatus &out);

    // why the last WiFi or timezone action did not do what was asked ("" after one that did); Bluetooth's is
    // btLastError()
    std::string lastError() const override { return lastError_; }

    // the file written when no wpa_supplicant is running: ctrl_interface, update_config=1 and the network
    static std::string supplicantConfText(const std::string &runDir, const std::string &group, const std::string &ssid,
                                          const std::string &password);

private:
    bool writeSupplicantConf(const std::string &ssid, const std::string &password);
    // dhcpcd takes the interface again; its 10-wpa_supplicant hook starts wpa_supplicant from the conf file
    void rebindDhcpcd(const std::string &iface);
    bool waitForSupplicant(const std::string &iface);
    std::string readSys(const std::string &iface, const std::string &file);
    // the client's error as btLastError(); drops the client when the bus went away, so the next call reconnects
    void takeBtError();
    // a status read of BlueZ: the short timeout, and none at all for a while after one that got no reply
    bool statusRefresh(BluezClient &client);
    BtDevice toBtDevice(const BluezDevice &device);
    static long long steadyMs();

    NativePaths paths_;
    CommandRunner run_;
    BluezBusFactory bluezFactory_;
    std::unique_ptr<BluezClient> bluez_;
    bool triedPowerOn_ = false;
    long long btRetryAt_ = 0; // no status read of BlueZ before this (steadyMs) - it did not answer the last one
    std::string btStaleError_;
    std::string lastError_;
    std::string btError_;
    // the WiFi connection being followed
    std::unique_ptr<WifiConnectWatch> watch_;
    std::unique_ptr<WpaEventMonitor> monitor_;
    unsigned long monitorInode_ = 0; // the socket the monitor is attached to (a restarted wpa_supplicant has another)
    std::string watchIface_;
    long long lastStatusPoll_ = 0;
};
