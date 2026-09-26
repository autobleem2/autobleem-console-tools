//
// NmBackend: PSC-Bios's ConsoleBackend on a Raspberry Pi and the PC stick - NetworkManager (nmcli) for the
// network, timedatectl for the time zone, BlueZ's bluetoothctl for the adapter and, for pairing, the same
// shipped `bt` wrapper the console uses. The launcher runs as root there, so nothing asks for a password.
//
// The screens name the interfaces "wlan0" and "eth0", which is what the console calls them. A PC names them
// after the slot (wlp2s0, enp3s0), so here those two names mean "the first Wi-Fi device" and "the first
// Ethernet device" NetworkManager reports; a real device of that name wins.
//
#pragma once

#include "console_backend.h"

#include <string>
#include <vector>

//******************
// NmBackend
//******************
class NmBackend : public ConsoleBackend {
public:
    NmBackend();
    using Runner = std::string (*)(const std::string &cmd);
    using LinesRunner = std::vector<std::string> (*)(const std::string &cmd);
    // the tests: a scripted shell, whether nmcli is there, a fixed name for the bt helper
    NmBackend(Runner run, LinesRunner runLines, bool nmcli, std::string btHelper = "bt");

    bool kernelInstalled() override { return nmcli_; } // here: NetworkManager's nmcli is there
    bool interfaceFound(const std::string &iface) override;
    bool wlanOn() override;
    bool isUp(const std::string &iface) override;
    std::string ipOf(const std::string &iface) override;
    std::vector<std::string> scanSsids() override;
    // remembered only: NetworkManager connects and keeps the credentials itself - restartNetwork() connects
    void configureWifi(const std::string &ssid, const std::string &password, const std::string &driverMode) override;
    void restartNetwork() override;
    bool btUp() override; // an adapter is there (it is switched on when a scan or a pairing needs it)
    std::string btName() override;
    std::vector<BtDevice> btScan() override;
    std::vector<BtDevice> btPairedDevices() override;
    bool btPair(const std::string &mac) override;
    bool btRemove(const std::string &mac) override;
    std::string timezone() override;
    void setTimezone(const std::string &zone) override;
    std::vector<std::string> listTimezones() override;
    bool hasDriverMode() override { return false; }
    bool keepsWifiSettings() override { return true; }
    std::string currentSsid() override;

    //*******************************
    // the parsing, pure for the tests
    //*******************************
    struct Device {
        std::string name;  // "wlan0"
        std::string type;  // "wifi", "ethernet", "loopback", "wifi-p2p", ...
        std::string state; // "connected", "disconnected", "unavailable", "unmanaged", "connecting (...)"...
    };
    // `nmcli -t -f DEVICE,TYPE,STATE device status`
    static std::vector<Device> parseDevices(const std::vector<std::string> &lines);
    // one line of nmcli's terse output: fields split at ':', with its "\:" and "\\" escapes undone
    static std::vector<std::string> splitTerse(const std::string &line);
    // `nmcli -g IP4.ADDRESS device show <dev>`: "192.168.68.144/24 | 10.0.0.2/8" -> "192.168.68.144"
    static std::string firstAddress(const std::string &text);
    // `bluetoothctl show`: "<Name> <address>", " (off)" appended while it is not powered; "" without a controller
    static std::string controllerName(const std::vector<std::string> &showLines);
    // a shell argument in single quotes
    static std::string shellQuoted(const std::string &value);

    static const char *const PasswordVariable; // the environment variable the password reaches nmcli through

private:
    std::vector<Device> devices();
    // the device the screens' "wlan0"/"eth0" stands for, "" when there is none
    std::string resolve(const std::string &iface);

    Runner run_;
    LinesRunner runLines_;
    bool nmcli_;
    std::string btHelper_;
    std::string pendingSsid_, pendingPassword_;
};
