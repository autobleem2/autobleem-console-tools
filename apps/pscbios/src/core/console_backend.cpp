//
// ConsoleBackend: what every backend shares (the blocking pairing over the state machine), and the dev host's
// stand-in (the console's backend is NativeBackend, native_backend.*).
//
#include "console_backend.h"
#include "core/main.h"

#include <ableem/engine/log.h>

#include <chrono>
#include <thread>

using namespace std;

const char *btPairStageName(BtPairStage stage) {
    switch (stage) {
    case BtPairStage::Idle:
        return "Idle";
    case BtPairStage::Discovering:
        return "Discovering";
    case BtPairStage::Trusting:
        return "Trusting";
    case BtPairStage::Pairing:
        return "Pairing";
    case BtPairStage::WaitingForPaired:
        return "WaitingForPaired";
    case BtPairStage::Connecting:
        return "Connecting";
    case BtPairStage::Done:
        return "Done";
    case BtPairStage::Failed:
        return "Failed";
    }
    return "?";
}

//*******************************
// ConsoleBackend::btPair
//*******************************
bool ConsoleBackend::btPair(const string &mac) {
    if (!btBeginPair(mac))
        return false;
    BtPairStage stage = btPumpPair();
    while (stage != BtPairStage::Done && stage != BtPairStage::Failed) {
        if (!keepWaiting()) {
            btCancelPair();
            return false;
        }
        stage = btPumpPair();
    }
    return stage == BtPairStage::Done;
}

//*******************************
// FakeBackend: the clock and the work
//*******************************
long long FakeBackend::nowMs() const {
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

bool FakeBackend::work(int ms) {
    if (!slow_)
        return keepWaiting();
    const long long until = nowMs() + ms;
    while (nowMs() < until) {
        if (!keepWaiting())
            return false;
        this_thread::sleep_for(chrono::milliseconds(20));
    }
    return true;
}

BtDevice *FakeBackend::device(const string &mac) {
    for (BtDevice &d : btDevices_)
        if (d.mac == mac)
            return &d;
    return nullptr;
}

//*******************************
// FakeBackend: WiFi
//*******************************
vector<WifiNetwork> FakeBackend::scanNetworks() {
    PLOG_INFO << "fake scanNetworks";
    lastError_.clear();
    if (!work(2500)) {
        lastError_ = _("cancelled");
        return {};
    }
    auto network = [](const string &ssid, int signal, const string &flags) {
        WifiNetwork n;
        n.ssid = ssid;
        n.signal = signal;
        n.flags = flags;
        return n;
    };
    return {network("Home Network", -48, "[WPA2-PSK-CCMP][ESS]"), network("Cafe Corner", -63, "[ESS]"),
            network("Neighbour 5G", -79, "[WPA2-PSK-CCMP][ESS]")};
}

void FakeBackend::configureWifi(const string &ssid, const string &password) {
    PLOG_INFO << "fake configureWifi \"" << ssid << "\" (password of " << password.size() << " chars)";
    lastError_.clear();
    work(600);
    configuredSsid = ssid;
    configuredPassword = password;
}

void FakeBackend::restartNetwork() {
    PLOG_INFO << "fake restartNetwork";
    lastError_.clear();
    work(1500);
    restarts++;
}

bool FakeBackend::wifiStatus(WpaStatus &out) {
    out = WpaStatus();
    out.wpaState = connectedSsid_.empty() ? "DISCONNECTED" : "COMPLETED";
    out.ssid = connectedSsid_;
    out.ipAddress = connectedSsid_.empty() ? "" : "192.168.1.23";
    return true;
}

void FakeBackend::beginWifiConnect(const string &ssid) {
    watchStart_ = nowMs();
    watch_ = make_unique<WifiConnectWatch>(ssid, watchStart_);
    connectedSsid_.clear();
}

// a connection played out on the clock: wpa_supplicant up, the scan, the association, the handshake, the address -
// or, for "Neighbour 5G", the handshake refused as wpa_supplicant reports a wrong key. Without `slow` all at once
const WifiConnectWatch &FakeBackend::pumpWifiConnect() {
    if (!watch_)
        beginWifiConnect(configuredSsid);
    WifiConnectWatch &watch = *watch_;
    const long long now = nowMs();
    const long long elapsed = slow_ ? now - watchStart_ : 100000;
    auto status = [&](const char *state, const string &ip) {
        WpaStatus s;
        s.wpaState = state;
        s.ssid = string(state) == "COMPLETED" ? watch.ssid() : "";
        s.ipAddress = ip;
        watch.onStatus(s, ip, now);
    };
    if (elapsed >= 300)
        watch.supplicantRunning(true, now);
    if (elapsed >= 300 && elapsed < 1300)
        status("SCANNING", "");
    if (elapsed >= 1300 && elapsed < 2300)
        status("ASSOCIATING", "");
    if (elapsed >= 2300 && elapsed < 3300)
        status("4WAY_HANDSHAKE", "");
    if (elapsed >= 3300) {
        if (watch.ssid() == "Neighbour 5G") {
            watch.onEvent("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Neighbour 5G\" auth_failures=1 duration=10 "
                          "reason=WRONG_KEY",
                          now);
        } else if (elapsed < 4300) {
            status("COMPLETED", "");
        } else {
            status("COMPLETED", "192.168.1.23");
            connectedSsid_ = watch.ssid();
        }
    }
    watch.tick(watchStart_ + (slow_ ? elapsed : 0));
    return watch;
}

//*******************************
// FakeBackend: Bluetooth
//*******************************
vector<BtDevice> FakeBackend::btScan() {
    PLOG_INFO << "fake btScan";
    btError_.clear();
    work(4000);        // a cancel only ends it early: what was found is shown
    return btDevices_; // a scan reveals everything nearby, paired or not
}

vector<BtDevice> FakeBackend::btPairedDevices() {
    // the Xbox pad goes out of range a few refreshes into the pairing screen (the dev host's "dropped")
    if (slow_ && ++pairedReads_ == 4) {
        if (BtDevice *xbox = device("A0:AB:51:33:44:55")) {
            xbox->connected = false;
            xbox->battery = BtBattery();
        }
    }
    vector<BtDevice> out;
    for (const BtDevice &d : btDevices_)
        if (d.paired)
            out.push_back(d);
    return out;
}

bool FakeBackend::btBeginPair(const string &mac) {
    PLOG_INFO << "fake btBeginPair " << mac;
    btError_.clear();
    pairMac_ = mac;
    if (device(mac) == nullptr) {
        btError_ = _("the controller is not known") + " (" + mac + ")";
        pairStage_ = BtPairStage::Failed;
        return false;
    }
    pairStart_ = nowMs();
    pairStage_ = BtPairStage::Discovering;
    return true;
}

BtPairStage FakeBackend::btPumpPair() {
    if (pairStage_ == BtPairStage::Done || pairStage_ == BtPairStage::Failed || pairStage_ == BtPairStage::Idle)
        return pairStage_;
    if (slow_)
        this_thread::sleep_for(chrono::milliseconds(15)); // a pump reads the bus for a moment
    const long long elapsed = slow_ ? nowMs() - pairStart_ : 100000;
    BtDevice *d = device(pairMac_);
    if (elapsed < 1000) {
        pairStage_ = BtPairStage::Discovering;
    } else if (elapsed < 2500) {
        pairStage_ = BtPairStage::Pairing;
    } else if (pairMac_ == "E4:17:D8:AA:BB:CC") {
        btError_ =
            _("the controller refused the pairing") + " (org.bluez.Error.AuthenticationFailed: Authentication Failed)";
        pairStage_ = BtPairStage::Failed;
    } else if (elapsed < 3500) {
        pairStage_ = BtPairStage::Connecting;
    } else {
        d->paired = true;
        d->connected = true;
        d->battery = {80, "Discharging"};
        pairStage_ = BtPairStage::Done;
    }
    return pairStage_;
}

void FakeBackend::btCancelPair() {
    if (pairStage_ == BtPairStage::Done || pairStage_ == BtPairStage::Failed || pairStage_ == BtPairStage::Idle)
        return;
    btError_ = _("cancelled");
    pairStage_ = BtPairStage::Failed;
}

bool FakeBackend::btRemove(const string &mac) {
    PLOG_INFO << "fake btRemove " << mac;
    btError_.clear();
    work(700);
    if (BtDevice *d = device(mac)) {
        d->paired = false;
        d->connected = false;
        d->battery = BtBattery();
        return true;
    }
    btError_ = _("the controller is not known") + " (" + mac + ")";
    return false;
}

BtBattery FakeBackend::btBattery(const string &mac) {
    BtDevice *d = device(mac);
    return d != nullptr ? d->battery : BtBattery();
}

string FakeBackend::btLastError() const {
    return btAdapter_ ? btError_ : _("no Bluetooth adapter");
}

//*******************************
// FakeBackend: the timezone
//*******************************
void FakeBackend::setTimezone(const string &zone) {
    PLOG_INFO << "fake setTimezone " << zone;
    lastError_.clear();
    work(800);
    timezone_ = zone;
}

vector<string> FakeBackend::listTimezones() {
    return {"Africa/Cairo",      "America/Chicago", "America/Los_Angeles", "America/New_York",
            "America/Sao_Paulo", "Asia/Tokyo",      "Asia/Shanghai",       "Australia/Sydney",
            "Europe/Berlin",     "Europe/London",   "Europe/Madrid",       "Europe/Paris",
            "Europe/Rome",       "Europe/Warsaw",   "Pacific/Auckland",    "UTC"};
}
