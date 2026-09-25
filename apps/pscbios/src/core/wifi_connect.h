//
// The WiFi side of PSC-Bios as the screens see it, on every host: a network from a scan, wpa_supplicant's STATUS,
// and WifiConnectWatch - the state machine that follows a connection after the settings were written or the
// network restarted, from what wpa_supplicant reports (its STATUS wpa_state, its events on an attached control
// connection, the interface's address) to Connected or a reason it failed: a wrong password
// (CTRL-EVENT-SSID-TEMP-DISABLED reason=WRONG_KEY), a network that is not there, an access point that refuses us,
// no address from DHCP, a timeout. Fed by NativeBackend on the console and by the FakeBackend on a dev host; the
// times are the caller's (milliseconds on any steady clock), so it is tested without waiting.
//
#pragma once

#include <string>

//******************
// WifiNetwork
//******************
// one network of a scan (SCAN_RESULTS' line, the strongest of its access points)
struct WifiNetwork {
    std::string bssid; // "aa:bb:cc:dd:ee:ff"
    int frequency = 0; // MHz
    int signal = 0;    // dBm, e.g. -48 (higher is stronger)
    std::string flags; // "[WPA2-PSK-CCMP][ESS]"
    std::string ssid;  // decoded (wpa_supplicant escapes it with \xNN, \\, \")

    bool secured() const; // WPA/WPA2/WEP in the flags: a password is needed
    // "Excellent" / "Good" / "Fair" / "Weak", translated, from the signal
    static std::string signalText(int dbm);
};

//******************
// WpaStatus
//******************
// what STATUS answers that the screens show
struct WpaStatus {
    std::string wpaState;  // "COMPLETED", "SCANNING", "DISCONNECTED", "4WAY_HANDSHAKE", ...
    std::string ssid;      // "" while not associated
    std::string ipAddress; // "" until dhcpcd got one
};
// the connection row of the WiFi screen for a wpa_state: "Connected", "Looking for the network", "Checking the
// password", "Not connected", ... translated; "" for a state it does not know
std::string wpaStateText(const std::string &wpaState);

//******************
// WifiFailure
//******************
enum class WifiFailure {
    None,
    NoInterface,            // no WiFi dongle
    NoSupplicant,           // wpa_supplicant never came up
    WrongPassword,          // reason=WRONG_KEY, or "4-Way Handshake failed - pre-shared key may be incorrect"
    NetworkNotFound,        // no access point with that SSID answers
    AssociationRejected,    // CTRL-EVENT-ASSOC-REJECT / reason=CONN_FAILED
    AuthenticationRejected, // CTRL-EVENT-AUTH-REJECT / reason=AUTH_FAILED
    NoAddress,              // connected, but DHCP gave no address
    Timeout,                // none of the above, and no connection in time
    Cancelled               // the user stopped waiting
};
std::string wifiFailureText(WifiFailure failure); // "wrong password", ... translated

//******************
// WifiConnectStage
//******************
enum class WifiConnectStage { Starting, Searching, Associating, Handshake, GettingAddress, Connected, Failed };
std::string wifiStageText(WifiConnectStage stage); // "Starting WiFi", "Looking for the network", ... translated

//******************
// WifiConnectTimeouts
//******************
struct WifiConnectTimeouts {
    int startMs = 12000;    // wpa_supplicant's control socket appearing (dhcpcd's hook starts it)
    int totalMs = 45000;    // Connected, from the start
    int notFoundEvents = 3; // CTRL-EVENT-NETWORK-NOT-FOUND events (one per scan) before giving up on the SSID
};

//******************
// WifiConnectWatch
//******************
class WifiConnectWatch {
public:
    WifiConnectWatch(std::string ssid, long long startMs, WifiConnectTimeouts timeouts = WifiConnectTimeouts());

    // what was seen, each at the time it was seen
    void supplicantRunning(bool running, long long nowMs);  // its control socket is there (or went)
    void onEvent(const std::string &line, long long nowMs); // "<3>CTRL-EVENT-..." from an attached connection
    void onStatus(const WpaStatus &status, const std::string &address, long long nowMs); // address: getifaddrs'
    void tick(long long nowMs);                                                          // the timeouts
    void fail(WifiFailure failure, const std::string &detail = "");
    void cancel() { fail(WifiFailure::Cancelled); }

    const std::string &ssid() const { return ssid_; }
    WifiConnectStage stage() const { return stage_; }
    bool finished() const { return stage_ == WifiConnectStage::Connected || stage_ == WifiConnectStage::Failed; }
    WifiFailure failure() const { return failure_; }
    const std::string &detail() const { return detail_; } // the event or reply behind the failure
    const std::string &address() const { return address_; }
    // what the screen shows: the stage while it runs, "Connected, 192.168.1.23", or the reason (+ the detail)
    std::string text() const;

    // an event that ends the attempt: its failure (and the event, trimmed, as `detail`), else None
    static WifiFailure failureOfEvent(const std::string &line, std::string &detail);
    // wpa_state -> stage (Searching for anything not under way)
    static WifiConnectStage stageOfState(const std::string &wpaState);
    // the event without its "<N>" priority prefix
    static std::string eventText(const std::string &line);

private:
    std::string ssid_;
    long long startMs_;
    WifiConnectTimeouts timeouts_;
    WifiConnectStage stage_ = WifiConnectStage::Starting;
    WifiFailure failure_ = WifiFailure::None;
    std::string detail_;
    std::string address_;
    std::string lastRejection_; // an ASSOC-REJECT/AUTH-REJECT seen, for a timeout's reason
    WifiFailure lastRejectionKind_ = WifiFailure::None;
    int notFound_ = 0;
};
