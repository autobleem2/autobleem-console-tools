//
// NativeBackend: the console asked directly instead of through the kernel's /bin/abnet - the network
// interfaces from sysfs, their addresses from getifaddrs, WiFi through wpa_supplicant's control socket
// (WpaCtrlClient). Any interface name, not only wlan0: a wireless interface is one sysfs gives a `wireless`
// or `phy80211` entry. Bluetooth and the timezone are still another backend's (AbnetBackend on the console)
// until BlueZ is spoken to over D-Bus (docs/native-backend-plan.md, step 2). Linux only.
//
#pragma once

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
};

//******************
// NativeBackend
//******************
class NativeBackend : public ConsoleBackend {
public:
    // runs a program and waits: System::runAndWait on the console, a recording fake in the tests
    using CommandRunner = std::function<int(const std::string &exe, const std::vector<std::string> &args)>;

    // the console: its paths, System::runAndWait, an AbnetBackend for Bluetooth and the timezone
    NativeBackend();
    NativeBackend(NativePaths paths, CommandRunner run, std::unique_ptr<ConsoleBackend> rest);

    bool kernelInstalled() override { return rest_->kernelInstalled(); }

    // network
    bool interfaceFound(const std::string &iface) override;
    bool wlanOn() override; // some wireless interface exists and is up
    bool isUp(const std::string &iface) override;
    std::string ipOf(const std::string &iface) override;
    std::vector<std::string> scanSsids() override;
    void configureWifi(const std::string &ssid, const std::string &password, const std::string &driverMode) override;
    void restartNetwork() override;

    // bluetooth and time: the other backend's
    bool btUp() override { return rest_->btUp(); }
    std::string btName() override { return rest_->btName(); }
    std::vector<BtDevice> btScan() override { return rest_->btScan(); }
    std::vector<BtDevice> btPairedDevices() override { return rest_->btPairedDevices(); }
    bool btPair(const std::string &mac) override { return rest_->btPair(mac); }
    bool btRemove(const std::string &mac) override { return rest_->btRemove(mac); }
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

    // why the last WiFi action did not do what was asked ("" after one that did)
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

    NativePaths paths_;
    CommandRunner run_;
    std::unique_ptr<ConsoleBackend> rest_;
    std::string lastError_;
};
