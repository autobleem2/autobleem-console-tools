//
// WifiConnectWatch: a connection followed from wpa_supplicant's states and events to Connected or a reason.
//
#include "wifi_connect.h"
#include "core/main.h"

#include <ableem/engine/log.h>

#include <utility>

using namespace std;

namespace {
bool contains(const string &text, const char *part) {
    return text.find(part) != string::npos;
}
} // namespace

//*******************************
// WifiNetwork
//*******************************
bool WifiNetwork::secured() const {
    return contains(flags, "WPA") || contains(flags, "WEP") || contains(flags, "RSN") || contains(flags, "SAE");
}

string WifiNetwork::signalText(int dbm) {
    if (dbm >= -55)
        return _("Excellent");
    if (dbm >= -67)
        return _("Good");
    if (dbm >= -75)
        return _("Fair");
    return _("Weak");
}

//*******************************
// the texts
//*******************************
string wpaStateText(const string &wpaState) {
    if (wpaState == "COMPLETED")
        return _("Connected");
    if (wpaState == "INTERFACE_DISABLED")
        return _("WiFi is off");
    if (wpaState == "DISCONNECTED" || wpaState == "INACTIVE")
        return _("Not connected");
    if (wpaState.empty())
        return "";
    return wifiStageText(WifiConnectWatch::stageOfState(wpaState));
}

string wifiFailureText(WifiFailure failure) {
    switch (failure) {
    case WifiFailure::None:
        return "";
    case WifiFailure::NoInterface:
        return _("no WiFi interface");
    case WifiFailure::NoSupplicant:
        return _("wpa_supplicant did not start");
    case WifiFailure::WrongPassword:
        return _("wrong password");
    case WifiFailure::NetworkNotFound:
        return _("the network was not found");
    case WifiFailure::AssociationRejected:
        return _("the access point refused the connection");
    case WifiFailure::AuthenticationRejected:
        return _("the access point refused the authentication");
    case WifiFailure::NoAddress:
        return _("no address from the network (DHCP)");
    case WifiFailure::Timeout:
        return _("the connection timed out");
    case WifiFailure::Cancelled:
        return _("cancelled");
    }
    return "";
}

string wifiStageText(WifiConnectStage stage) {
    switch (stage) {
    case WifiConnectStage::Starting:
        return _("Starting WiFi");
    case WifiConnectStage::Searching:
        return _("Looking for the network");
    case WifiConnectStage::Associating:
        return _("Connecting");
    case WifiConnectStage::Handshake:
        return _("Checking the password");
    case WifiConnectStage::GettingAddress:
        return _("Getting an address");
    case WifiConnectStage::Connected:
        return _("Connected");
    case WifiConnectStage::Failed:
        return _("Not connected");
    }
    return "";
}

//*******************************
// WifiConnectWatch
//*******************************
WifiConnectWatch::WifiConnectWatch(string ssid, long long startMs, WifiConnectTimeouts timeouts)
    : ssid_(std::move(ssid)), startMs_(startMs), timeouts_(timeouts) {}

string WifiConnectWatch::eventText(const string &line) {
    string text = line;
    if (!text.empty() && text[0] == '<') {
        size_t end = text.find('>');
        if (end != string::npos)
            text = text.substr(end + 1);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

WifiFailure WifiConnectWatch::failureOfEvent(const string &line, string &detail) {
    const string text = eventText(line);
    WifiFailure failure = WifiFailure::None;
    if (contains(text, "CTRL-EVENT-SSID-TEMP-DISABLED")) {
        // wpa_supplicant gives the network a rest after failing it: the reason is why
        if (contains(text, "reason=WRONG_KEY"))
            failure = WifiFailure::WrongPassword;
        else if (contains(text, "reason=AUTH_FAILED"))
            failure = WifiFailure::AuthenticationRejected;
        else if (contains(text, "reason=CONN_FAILED"))
            failure = WifiFailure::AssociationRejected;
    } else if (contains(text, "4-Way Handshake failed") && contains(text, "pre-shared key may be incorrect")) {
        failure = WifiFailure::WrongPassword;
    }
    if (failure != WifiFailure::None)
        detail = text;
    return failure;
}

WifiConnectStage WifiConnectWatch::stageOfState(const string &wpaState) {
    if (wpaState == "AUTHENTICATING" || wpaState == "ASSOCIATING" || wpaState == "ASSOCIATED")
        return WifiConnectStage::Associating;
    if (wpaState == "4WAY_HANDSHAKE" || wpaState == "GROUP_HANDSHAKE")
        return WifiConnectStage::Handshake;
    if (wpaState == "COMPLETED")
        return WifiConnectStage::GettingAddress;
    return WifiConnectStage::Searching; // SCANNING, DISCONNECTED, INACTIVE, INTERFACE_DISABLED, ...
}

void WifiConnectWatch::fail(WifiFailure failure, const string &detail) {
    if (finished())
        return;
    stage_ = WifiConnectStage::Failed;
    failure_ = failure;
    detail_ = detail;
    PLOG_WARNING << "wifi: connecting to \"" << ssid_ << "\" failed: " << wifiFailureText(failure)
                 << (detail.empty() ? "" : " (" + detail + ")");
}

void WifiConnectWatch::supplicantRunning(bool running, long long nowMs) {
    if (finished())
        return;
    if (running && stage_ == WifiConnectStage::Starting)
        stage_ = WifiConnectStage::Searching;
    else if (!running && stage_ != WifiConnectStage::Starting)
        stage_ = WifiConnectStage::Starting; // restarted under us (the network restart): wait for it again
    tick(nowMs);
}

void WifiConnectWatch::onEvent(const string &line, long long nowMs) {
    if (finished())
        return;
    const string text = eventText(line);
    string detail;
    WifiFailure failure = failureOfEvent(text, detail);
    if (failure != WifiFailure::None) {
        fail(failure, detail);
        return;
    }
    if (contains(text, "CTRL-EVENT-ASSOC-REJECT")) {
        lastRejection_ = text; // wpa_supplicant tries again; the reason if it never gets further
        lastRejectionKind_ = WifiFailure::AssociationRejected;
    } else if (contains(text, "CTRL-EVENT-AUTH-REJECT")) {
        lastRejection_ = text;
        lastRejectionKind_ = WifiFailure::AuthenticationRejected;
    } else if (contains(text, "CTRL-EVENT-NETWORK-NOT-FOUND")) {
        if (++notFound_ >= timeouts_.notFoundEvents) {
            fail(WifiFailure::NetworkNotFound, text);
            return;
        }
    } else if (contains(text, "CTRL-EVENT-CONNECTED")) {
        if (stage_ != WifiConnectStage::Connected)
            stage_ = address_.empty() ? WifiConnectStage::GettingAddress : WifiConnectStage::Connected;
    } else if (contains(text, "CTRL-EVENT-SCAN-STARTED") && stage_ == WifiConnectStage::Starting) {
        stage_ = WifiConnectStage::Searching;
    }
    tick(nowMs);
}

void WifiConnectWatch::onStatus(const WpaStatus &status, const string &address, long long nowMs) {
    if (finished())
        return;
    WifiConnectStage stage = stageOfState(status.wpaState);
    // COMPLETED on another network (one an earlier configuration left) is not ours yet
    if (stage == WifiConnectStage::GettingAddress && !status.ssid.empty() && status.ssid != ssid_)
        stage = WifiConnectStage::Associating;
    string ip = !address.empty() ? address : status.ipAddress;
    if (stage == WifiConnectStage::GettingAddress && !ip.empty()) {
        address_ = ip;
        stage_ = WifiConnectStage::Connected;
        PLOG_INFO << "wifi: connected to \"" << ssid_ << "\", " << address_;
        return;
    }
    stage_ = stage; // a STATUS answered, so it runs: Starting is over too
    tick(nowMs);
}

void WifiConnectWatch::tick(long long nowMs) {
    if (finished())
        return;
    const long long waited = nowMs - startMs_;
    if (stage_ == WifiConnectStage::Starting && waited >= timeouts_.startMs) {
        fail(WifiFailure::NoSupplicant);
        return;
    }
    if (waited < timeouts_.totalMs)
        return;
    switch (stage_) {
    case WifiConnectStage::GettingAddress:
        fail(WifiFailure::NoAddress);
        break;
    case WifiConnectStage::Handshake:
        fail(WifiFailure::WrongPassword, "4WAY_HANDSHAKE");
        break;
    case WifiConnectStage::Searching:
        if (lastRejectionKind_ != WifiFailure::None)
            fail(lastRejectionKind_, lastRejection_);
        else
            fail(WifiFailure::NetworkNotFound);
        break;
    default:
        if (lastRejectionKind_ != WifiFailure::None)
            fail(lastRejectionKind_, lastRejection_);
        else
            fail(WifiFailure::Timeout);
        break;
    }
}

string WifiConnectWatch::text() const {
    if (stage_ == WifiConnectStage::Connected)
        return _("Connected") + (address_.empty() ? string() : ", " + address_);
    if (stage_ == WifiConnectStage::Failed)
        return wifiFailureText(failure_) + (detail_.empty() ? string() : " (" + detail_ + ")");
    return wifiStageText(stage_);
}
