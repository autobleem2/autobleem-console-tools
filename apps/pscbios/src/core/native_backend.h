//
// NativeBackend: the console asked directly instead of through the kernel's /bin/abnet - the network
// interfaces from sysfs, their addresses from getifaddrs, WiFi through wpa_supplicant's control socket
// (WpaCtrlClient), Bluetooth through BlueZ over D-Bus (BluezClient). Any interface name, not only wlan0: a
// wireless interface is one sysfs gives a `wireless` or `phy80211` entry. The timezone and kernelInstalled() are
// still another backend's (AbnetBackend on the console) - docs/native-backend-plan.md. Linux only.
//
#pragma once

#include "bluez_client.h"
#include "console_backend.h"
#include "wpa_ctrl_client.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

//******************
// NativePaths
//******************
// where the console keeps what the backend reads and writes - replaced by a temp tree in the tests
struct NativePaths {
    std::string sysClassNet = "/sys/class/net";
    std::string wpaRunDir = "/var/run/wpa_supplicant";          // wpa_supplicant's ctrl_interface DIR
    std::string wpaSupplicantConf = "/etc/wpa_supplicant.conf"; // what dhcpcd's hook starts it with
    std::string dhcpcdConf = "/etc/dhcpcd.conf";
    std::string kernelConfigDir = "/etc/autobleem"; // dhcpcd.conf.wext / dhcpcd.conf.nl80211
    // the ctrl_interface GROUP written into a new wpa_supplicant.conf - left out when the system has no such
    // group (wpa_supplicant refuses an unknown one), and when empty
    std::string ctrlGroup = "netdev";
    int supplicantStartMs = 6000; // how long a started wpa_supplicant is waited for (its socket appearing)
    std::string powerSupplyDir = "/sys/class/power_supply"; // the pads' batteries (hid-sony, hid-playstation)
    int btScanMs = 8000;                                    // btScan()'s discovery
    BluezTimeouts btTimeouts;
};

//******************
// NativeBackend
//******************
class NativeBackend : public ConsoleBackend {
public:
    // runs a program and waits: System::runAndWait on the console, a recording fake in the tests
    using CommandRunner = std::function<int(const std::string &exe, const std::vector<std::string> &args)>;

    // the console: its paths, System::runAndWait, the system bus (DbusBluezBus - where libdbus was found at build
    // time), an AbnetBackend for the timezone
    NativeBackend();
    // bluez connects the BluezClient's bus when Bluetooth is first asked for (and again after a failure);
    // none = Bluetooth unavailable, with that as the reason
    NativeBackend(NativePaths paths, CommandRunner run, std::unique_ptr<ConsoleBackend> rest,
                  BluezBusFactory bluez = nullptr);
    ~NativeBackend() override;

    bool kernelInstalled() override { return rest_->kernelInstalled(); }

    // network
    bool interfaceFound(const std::string &iface) override;
    bool wlanOn() override; // some wireless interface exists and is up
    bool isUp(const std::string &iface) override;
    std::string ipOf(const std::string &iface) override;
    std::vector<std::string> scanSsids() override;
    void configureWifi(const std::string &ssid, const std::string &password, const std::string &driverMode) override;
    void restartNetwork() override;

    // bluetooth, through BlueZ: an adapter that is there and powered (powered on once, if it is not);
    // "hci0 <alias> <address>"; btScan discovers for btScanMs; btPair pairs, trusts and connects (blocking - the
    // screens of step 4 drive bluez() themselves); each false/empty with the reason in btLastError()
    bool btUp() override;
    std::string btName() override;
    std::vector<BtDevice> btScan() override;
    std::vector<BtDevice> btPairedDevices() override;
    bool btPair(const std::string &mac) override;
    bool btRemove(const std::string &mac) override;
    BtBattery btBattery(const std::string &mac);
    // the client, connected on first use; nullptr (and btLastError()) when the system bus cannot be reached
    BluezClient *bluez();
    const std::string &btLastError() const { return btError_; }

    // time: the other backend's
    std::string timezone() override { return rest_->timezone(); }
    void setTimezone(const std::string &zone) override { rest_->setTimezone(zone); }
    std::vector<std::string> listTimezones() override { return rest_->listTimezones(); }

    // every interface sysfs lists ("lo" included), sorted
    std::vector<std::string> interfaces();
    bool isWireless(const std::string &iface);
    std::vector<std::string> wirelessInterfaces(); // sorted
    // the one WiFi is set up on: the first wireless interface that is up, else the first there is; "" for none
    std::string wifiInterface();
    // the connection's state from wpa_supplicant; false when it is not running on the interface
    bool wifiStatus(const std::string &iface, WpaStatus &out);

    // why the last WiFi action did not do what was asked ("" after one that did); Bluetooth's is btLastError()
    const std::string &lastError() const { return lastError_; }

    // the file written when no wpa_supplicant is running: ctrl_interface, update_config=1 and the network
    static std::string supplicantConfText(const std::string &runDir, const std::string &group, const std::string &ssid,
                                          const std::string &password);

private:
    bool writeSupplicantConf(const std::string &ssid, const std::string &password);
    void selectDriverMode(const std::string &driverMode);
    // dhcpcd takes the interface again; its 10-wpa_supplicant hook starts wpa_supplicant from the conf file
    void rebindDhcpcd(const std::string &iface);
    bool waitForSupplicant(const std::string &iface);
    std::string readSys(const std::string &iface, const std::string &file);
    // the client's error as btLastError(); drops the client when the bus went away, so the next call reconnects
    void takeBtError();
    static BtDevice toBtDevice(const BluezDevice &device);

    NativePaths paths_;
    CommandRunner run_;
    std::unique_ptr<ConsoleBackend> rest_;
    BluezBusFactory bluezFactory_;
    std::unique_ptr<BluezClient> bluez_;
    bool triedPowerOn_ = false;
    std::string lastError_;
    std::string btError_;
};
