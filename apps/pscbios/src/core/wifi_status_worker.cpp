//
// WifiStatusWorker: see the header.
//
#include "wifi_status_worker.h"
#include "console_backend.h"
#include "core/main.h"

#include <utility>

using namespace std;

//*******************************
// WifiStatusWorker::read
//*******************************
// the connection: wpa_supplicant's state (a short STATUS) and the interface's address - "Connected, 192.168.1.23",
// "Looking for the network", "Not connected", "-" without a WiFi dongle
WifiStatusSnapshot WifiStatusWorker::read(ConsoleBackend &backend) {
    WifiStatusSnapshot snapshot;
    const string iface = backend.wifiInterface(); // any name, not only wlan0
    if (iface.empty()) {
        snapshot.connection = "-";
    } else {
        string ip = backend.ipOf(iface);
        WpaStatus status;
        if (!backend.wifiStatus(status)) {
            snapshot.connection = ip.empty() ? _("Not connected") : ip;
        } else if (status.wpaState == "COMPLETED") {
            if (ip.empty())
                ip = status.ipAddress;
            snapshot.connection =
                ip.empty() ? wifiStageText(WifiConnectStage::GettingAddress) : _("Connected") + ", " + ip;
        } else {
            snapshot.connection = wpaStateText(status.wpaState);
            if (snapshot.connection.empty())
                snapshot.connection = _("Not connected");
        }
    }
    snapshot.timezone = backend.timezone();
    return snapshot;
}

//*******************************
// WifiStatusWorker::start / take / wait
//*******************************
void WifiStatusWorker::start(ConsoleBackend &backend) {
    if (active_)
        return;
    active_ = true;
    done_ = false;
    thread_ = thread([this, &backend]() {
        WifiStatusSnapshot snapshot = read(backend);
        {
            lock_guard<mutex> lock(mutex_);
            result_ = std::move(snapshot);
        }
        done_ = true;
    });
}

bool WifiStatusWorker::take(WifiStatusSnapshot &out) {
    if (!active_ || !done_)
        return false;
    thread_.join();
    active_ = false;
    lock_guard<mutex> lock(mutex_);
    out = std::move(result_);
    return true;
}

void WifiStatusWorker::wait() {
    if (!active_)
        return;
    thread_.join();
    active_ = false;
}
