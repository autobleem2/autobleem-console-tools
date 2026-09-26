//
// WpaCtrlClient: wpa_supplicant's control socket (/var/run/wpa_supplicant/<iface>), through hostap's own client
// (third_party/wpa_ctrl, vendored). What PSC-Bios asks it: a scan and its results, the connection's state, and
// the one network to join - which wpa_supplicant then writes into its configuration itself (SAVE_CONFIG).
// Every request has a timeout; nothing here can block for ever. Linux only (not built on Windows).
//
#pragma once

#include "wifi_connect.h" // WifiNetwork, WpaStatus

#include <functional>
#include <string>
#include <vector>

struct wpa_ctrl;

//******************
// WpaEventMonitor
//******************
// a second connection to the control socket, ATTACHed: wpa_supplicant sends it its events ("<3>CTRL-EVENT-...")
// - the scan's end, a connection, a network given a rest after a wrong key. Read without blocking the caller for
// longer than it asks.
class WpaEventMonitor {
public:
    explicit WpaEventMonitor(std::string socketPath);
    ~WpaEventMonitor();
    WpaEventMonitor(const WpaEventMonitor &) = delete;
    WpaEventMonitor &operator=(const WpaEventMonitor &) = delete;

    bool open(int timeoutMs = 1000); // connect + ATTACH; false when there is no wpa_supplicant to hear
    bool isOpen() const { return ctrl_ != nullptr; }
    // the events waiting, after waiting up to waitMs for the first; false when the connection failed
    bool read(std::vector<std::string> &events, int waitMs);

private:
    std::string socketPath_;
    wpa_ctrl *ctrl_ = nullptr;
};

//******************
// WpaCtrlClient
//******************
class WpaCtrlClient {
public:
    static const char *const DefaultRunDir; // "/var/run/wpa_supplicant"
    static const int DefaultTimeoutMs = 3000;
    static const int ScanTimeoutMs = 8000; // a scan of every channel by a USB dongle takes 2-5 s

    // nothing is opened yet; runDir is the ctrl_interface DIR (a temp dir in the tests)
    explicit WpaCtrlClient(std::string iface, std::string runDir = DefaultRunDir, int timeoutMs = DefaultTimeoutMs);
    ~WpaCtrlClient();
    WpaCtrlClient(const WpaCtrlClient &) = delete;
    WpaCtrlClient &operator=(const WpaCtrlClient &) = delete;

    // <runDir>/<iface>: there while a wpa_supplicant manages the interface
    std::string socketPath() const;
    bool socketExists() const;
    // connects; false (and lastError()) when there is no wpa_supplicant to talk to
    bool open();
    bool isOpen() const { return ctrl_ != nullptr; }
    void close();

    // one command, its reply as sent (usually ending in "\n"); false on a send error or a timeout
    bool request(const std::string &cmd, std::string &reply);

    // SCAN, then wait up to timeoutMs for the scan's end (CTRL-EVENT-SCAN-RESULTS on a WpaEventMonitor); true when
    // the results are fresh. A busy scanner (FAIL-BUSY: one is running) is waited for the same way. Either way
    // SCAN_RESULTS answers with what wpa_supplicant has. The wait hook is asked every 50 ms; false ends the wait
    bool scan(int timeoutMs = ScanTimeoutMs);
    void setWaitHook(std::function<bool()> hook) { waitHook_ = std::move(hook); }
    // SCAN_RESULTS: hidden networks left out, one entry per SSID (the strongest), strongest first
    std::vector<WifiNetwork> scanResults();
    bool status(WpaStatus &out);
    // the one network: REMOVE_NETWORK all, ADD_NETWORK, SET_NETWORK ssid / psk (key_mgmt NONE for an empty
    // password), ENABLE_NETWORK, SAVE_CONFIG. False, with lastError() (the reason, then the step and the reply in
    // brackets - never the password), at the first step refused.
    bool configure(const std::string &ssid, const std::string &password);
    // TERMINATE: wpa_supplicant exits (dhcpcd's hook starts it again)
    bool terminate();

    // a reason for the user, translated, with wpa_supplicant's reply or the socket in brackets
    const std::string &lastError() const { return lastError_; }

    // the text of the replies, and the values sent - static, so tested without a socket
    static std::vector<WifiNetwork> parseScanResults(const std::string &reply);
    static WpaStatus parseStatus(const std::string &reply);
    // wpa_supplicant's printf_encode undone: \\ \" \e \n \r \t \xNN
    static std::string decodeSsid(const std::string &encoded);
    // the value of `SET_NETWORK <id> ssid` and of `ssid=` in the file: "\"Name\"" for plain printable ASCII,
    // else the bytes in hex (what wpa_supplicant takes unquoted) - so no SSID needs escaping
    static std::string ssidValue(const std::string &ssid);
    // the value of `psk`: "\"passphrase\"" (8..63 printable ASCII) or the 64 hex digits of a raw key; "" for
    // a password wpa_supplicant would refuse
    static std::string pskValue(const std::string &password);

private:
    // `reason` is what lastError() says when the reply is not OK; `logged` names the step (never the values)
    bool expectOk(const std::string &cmd, const std::string &logged, const std::string &reason);

    std::string iface_;
    std::string runDir_;
    int timeoutMs_;
    wpa_ctrl *ctrl_ = nullptr;
    std::string lastError_;
    std::function<bool()> waitHook_;
};
