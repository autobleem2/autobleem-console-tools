//
// NmBackend: PSC-Bios's ConsoleBackend on a Raspberry Pi and the PC stick - NetworkManager (nmcli) for the
// network, timedatectl for the time zone, BlueZ over D-Bus (the same BluezClient NativeBackend uses on the
// console) for the adapter and pairing. The launcher runs as root there, so nothing asks for a password.
// No shell script of any kind is asked (docs/native-backend-plan.md's rule extends here too): pairing goes
// through BluezClient, never the `bt` bluetoothctl wrapper (gone since the native backend work).
//
#pragma once

#include "bluez_client.h"
#include "console_backend.h"

#include <memory>
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
    // the tests: a scripted shell for nmcli/timedatectl, whether nmcli is there, and a BlueZ bus factory
    // (nullptr: no Bluetooth support, as on a host with no libdbus-1)
    NmBackend(Runner run, LinesRunner runLines, bool nmcli, BluezBusFactory bluez = nullptr);

    bool kernelInstalled() override { return nmcli_; } // here: NetworkManager's nmcli is there
    bool interfaceFound(const std::string &iface) override;
    bool isUp(const std::string &iface) override;
    std::string ipOf(const std::string &iface) override;
    std::string wifiInterface() override;
    std::string ethernetInterface() override;
    std::vector<WifiNetwork> scanNetworks() override;
    // remembered only: NetworkManager connects and keeps the credentials itself - restartNetwork() connects
    void configureWifi(const std::string &ssid, const std::string &password) override;
    void restartNetwork() override;
    bool wifiStatus(WpaStatus &out) override;
    void beginWifiConnect(const std::string &ssid) override;
    const WifiConnectWatch &pumpWifiConnect() override;
    std::string lastError() const override { return lastError_; }

    // bluetooth, through the same BluezClient the console uses
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
    std::string btLastError() const override { return btError_; }

    std::string timezone() override;
    void setTimezone(const std::string &zone) override;
    std::vector<std::string> listTimezones() override;

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
    // a shell argument in single quotes
    static std::string shellQuoted(const std::string &value);

    static const char *const PasswordVariable; // the environment variable the password reaches nmcli through

private:
    std::vector<Device> devices();
    // the device by type ("wifi"/"ethernet"): the first that is up, else the first there is; "" for none
    std::string deviceOfType(const std::string &type);
    // BlueZ, connected on first use (mirrors NativeBackend - see its comments)
    BluezClient *bluez();
    void takeBtError();
    bool statusRefresh(BluezClient &client);
    BtDevice toBtDevice(const BluezDevice &device);
    static long long steadyMs();

    Runner run_;
    LinesRunner runLines_;
    bool nmcli_;
    BluezBusFactory bluezFactory_;
    std::unique_ptr<BluezClient> bluez_;
    bool triedPowerOn_ = false;
    long long btRetryAt_ = 0;
    std::string btStaleError_;
    std::string lastError_;
    std::string btError_;
    std::string pendingSsid_, pendingPassword_;
    // the outcome of the last configureWifi()/restartNetwork() (nmcli does the whole connection attempt in
    // one blocking call, unlike wpa_supplicant's event stream): beginWifiConnect/pumpWifiConnect just report it
    bool lastConnectOk_ = false;
    std::string lastConnectSsid_, lastConnectDetail_;
    std::unique_ptr<WifiConnectWatch> watch_;
};
