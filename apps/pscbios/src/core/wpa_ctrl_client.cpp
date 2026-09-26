//
// WpaCtrlClient: the requests, the waiting for a scan, and the parsing of the replies.
//
#include "wpa_ctrl_client.h"
#include "core/main.h"

#include <ableem/engine/log.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <map>
#include <sstream>
#include <utility>

#include <sys/select.h>
#include <sys/stat.h>

#include "wpa_ctrl.h"

using namespace std;

const char *const WpaCtrlClient::DefaultRunDir = "/var/run/wpa_supplicant";

namespace {
const size_t ReplyBufferSize = 16384; // SCAN_RESULTS of a crowded block of flats is a few KB

string withoutTrailingNewlines(string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

string firstWord(const string &cmd) {
    return cmd.substr(0, cmd.find(' '));
}

bool isPrintableAscii(const string &s) {
    return all_of(s.begin(), s.end(), [](char c) { return c >= 32 && c <= 126; });
}

long long millisecondsSince(chrono::steady_clock::time_point start) {
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
}
} // namespace

//*******************************
// WpaCtrlClient::WpaCtrlClient / ~WpaCtrlClient
//*******************************
WpaCtrlClient::WpaCtrlClient(string iface, string runDir, int timeoutMs)
    : iface_(std::move(iface)), runDir_(std::move(runDir)), timeoutMs_(timeoutMs) {}

WpaCtrlClient::~WpaCtrlClient() {
    close();
}

//*******************************
// WpaCtrlClient::socketPath / socketExists
//*******************************
string WpaCtrlClient::socketPath() const {
    return DirEntry::removeSeparatorFromEndOfPath(runDir_) + sep + iface_;
}

bool WpaCtrlClient::socketExists() const {
    struct stat st{};
    return stat(socketPath().c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

//*******************************
// WpaEventMonitor
//*******************************
WpaEventMonitor::WpaEventMonitor(string socketPath) : socketPath_(std::move(socketPath)) {}

WpaEventMonitor::~WpaEventMonitor() {
    if (ctrl_ != nullptr) {
        wpa_ctrl_detach(ctrl_);
        wpa_ctrl_close(ctrl_);
    }
}

bool WpaEventMonitor::open(int timeoutMs) {
    if (ctrl_ != nullptr)
        return true;
    struct stat st{};
    if (stat(socketPath_.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode))
        return false;
    ctrl_ = wpa_ctrl_open(socketPath_.c_str());
    if (ctrl_ == nullptr)
        return false;
    wpa_ctrl_set_timeout(ctrl_, timeoutMs);
    if (wpa_ctrl_attach(ctrl_) != 0) {
        wpa_ctrl_close(ctrl_);
        ctrl_ = nullptr;
        return false;
    }
    return true;
}

bool WpaEventMonitor::read(vector<string> &events, int waitMs) {
    if (ctrl_ == nullptr)
        return false;
    const int fd = wpa_ctrl_get_fd(ctrl_);
    for (int wait = waitMs;; wait = 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv{};
        tv.tv_sec = wait / 1000;
        tv.tv_usec = (wait % 1000) * 1000;
        const int ready = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0)
            return ready == 0 || errno == EINTR; // nothing (more) waiting; a signal is nothing too
        char event[4096];
        size_t length = sizeof(event) - 1;
        if (wpa_ctrl_recv(ctrl_, event, &length) != 0)
            return false;
        events.emplace_back(event, length);
    }
}

//*******************************
// WpaCtrlClient::open / close
//*******************************
bool WpaCtrlClient::open() {
    if (ctrl_ != nullptr)
        return true;
    if (!socketExists()) {
        lastError_ = _("wpa_supplicant is not running") + " (" + socketPath() + ")";
        return false;
    }
    ctrl_ = wpa_ctrl_open(socketPath().c_str());
    if (ctrl_ == nullptr) {
        lastError_ = _("wpa_supplicant does not answer") + " (" + socketPath() + ": " + strerror(errno) + ")";
        PLOG_WARNING << lastError_;
        return false;
    }
    wpa_ctrl_set_timeout(ctrl_, timeoutMs_);
    return true;
}

void WpaCtrlClient::close() {
    if (ctrl_ != nullptr) {
        wpa_ctrl_close(ctrl_);
        ctrl_ = nullptr;
    }
}

//*******************************
// WpaCtrlClient::request
//*******************************
bool WpaCtrlClient::request(const string &cmd, string &reply) {
    reply.clear();
    if (!open())
        return false;
    vector<char> buffer(ReplyBufferSize);
    size_t length = buffer.size() - 1;
    int result = wpa_ctrl_request(ctrl_, cmd.c_str(), cmd.size(), buffer.data(), &length, nullptr);
    if (result != 0) {
        // -2 is the timeout; either way the connection may be stale (wpa_supplicant restarted) - reopen next time
        lastError_ = _("wpa_supplicant does not answer") + " (" + firstWord(cmd) + ": " +
                     (result == -2 ? "no reply within " + to_string(timeoutMs_) + " ms" : string("socket error")) + ")";
        close();
        return false;
    }
    reply.assign(buffer.data(), length);
    return true;
}

bool WpaCtrlClient::expectOk(const string &cmd, const string &logged, const string &reason) {
    string reply;
    if (!request(cmd, reply)) {
        // lastError_ says wpa_supplicant does not answer
    } else if (withoutTrailingNewlines(reply) != "OK") {
        lastError_ = reason + " (" + logged + ": " + withoutTrailingNewlines(reply) + ")";
    } else {
        return true;
    }
    PLOG_WARNING << "wpa_supplicant on " << iface_ << ": " << lastError_;
    return false;
}

//*******************************
// WpaCtrlClient::scan
//*******************************
bool WpaCtrlClient::scan(int timeoutMs) {
    if (!open())
        return false;
    // a second connection hears the events: the one requests go through never asks for them
    WpaEventMonitor monitor(socketPath());
    const bool listening = monitor.open(timeoutMs_);
    string reply;
    bool started = request("SCAN", reply);
    reply = withoutTrailingNewlines(reply);
    if (started && reply != "OK" && reply != "FAIL-BUSY") {
        lastError_ = _("the scan failed") + " (SCAN: " + reply + ")";
        started = false;
    }
    bool finished = false;
    bool stopped = false;
    auto start = chrono::steady_clock::now();
    while (started && listening && !finished) {
        if (millisecondsSince(start) >= timeoutMs) {
            lastError_ = _("the scan did not finish in time");
            break;
        }
        if (waitHook_ && !waitHook_()) {
            lastError_ = _("cancelled");
            stopped = true;
            break;
        }
        vector<string> events;
        if (!monitor.read(events, 50)) {
            lastError_ = _("wpa_supplicant does not answer") + " (" + socketPath() + ")";
            break;
        }
        for (const string &event : events) {
            if (event.find("CTRL-EVENT-SCAN-RESULTS") != string::npos) {
                finished = true;
            } else if (event.find("CTRL-EVENT-SCAN-FAILED") != string::npos) {
                lastError_ = _("the scan failed") + " (" + WifiConnectWatch::eventText(event) + ")";
                started = false;
            }
        }
    }
    if (!finished && started && !stopped) {
        PLOG_WARNING << "wpa_supplicant on " << iface_ << ": " << lastError_ << " - using the results it has";
    }
    return finished;
}

//*******************************
// WpaCtrlClient::scanResults / status
//*******************************
vector<WifiNetwork> WpaCtrlClient::scanResults() {
    string reply;
    if (!request("SCAN_RESULTS", reply))
        return {};
    return parseScanResults(reply);
}

bool WpaCtrlClient::status(WpaStatus &out) {
    string reply;
    if (!request("STATUS", reply))
        return false;
    out = parseStatus(reply);
    return true;
}

//*******************************
// WpaCtrlClient::configure
//*******************************
bool WpaCtrlClient::configure(const string &ssid, const string &password) {
    string ssidArg = ssidValue(ssid);
    if (ssidArg.empty()) {
        lastError_ = _("an SSID is 1 to 32 bytes");
        return false;
    }
    string pskArg;
    if (!password.empty()) {
        pskArg = pskValue(password);
        if (pskArg.empty()) {
            lastError_ = _("a WiFi password is 8 to 63 letters, digits or signs");
            return false;
        }
    }
    const string refused = _("wpa_supplicant refused the network");
    if (!expectOk("REMOVE_NETWORK all", "REMOVE_NETWORK", refused))
        return false;
    string reply;
    if (!request("ADD_NETWORK", reply))
        return false;
    string id = withoutTrailingNewlines(reply);
    if (id.empty() || !all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        lastError_ = refused + " (ADD_NETWORK: " + id + ")";
        return false;
    }
    if (!expectOk("SET_NETWORK " + id + " ssid " + ssidArg, "SET_NETWORK ssid", refused))
        return false;
    if (pskArg.empty()) {
        if (!expectOk("SET_NETWORK " + id + " key_mgmt NONE", "SET_NETWORK key_mgmt", refused))
            return false;
    } else if (!expectOk("SET_NETWORK " + id + " psk " + pskArg, "SET_NETWORK psk", refused)) {
        return false; // never logged with the password: `logged` names the step only
    }
    if (!expectOk("ENABLE_NETWORK " + id, "ENABLE_NETWORK", refused))
        return false;
    // the network is in use from here on; SAVE_CONFIG only makes it survive a reboot. wpa_supplicant refuses it
    // unless update_config is on, and a file without the line (Buildroot's example, what the payload shipped
    // until 2026-09-26) has it off - so switch it on first (2.10 answers OK; an older one's refusal is left to
    // SAVE_CONFIG to report, the network stays)
    string ignored;
    request("SET update_config 1", ignored);
    if (!expectOk("SAVE_CONFIG", "SAVE_CONFIG", _("the WiFi settings could not be saved")))
        return false;
    PLOG_INFO << "wpa_supplicant on " << iface_ << ": network " << id << " set to \"" << ssid << "\" and saved";
    return true;
}

bool WpaCtrlClient::terminate() {
    bool ok = expectOk("TERMINATE", "TERMINATE", _("wpa_supplicant does not answer"));
    close();
    return ok;
}

//*******************************
// WpaCtrlClient::parseScanResults
//*******************************
// "bssid / frequency / signal level / flags / ssid" then one tab-separated line per BSS
vector<WifiNetwork> WpaCtrlClient::parseScanResults(const string &reply) {
    map<string, WifiNetwork> strongest;
    istringstream in(reply);
    string line;
    while (getline(in, line)) {
        line = withoutTrailingNewlines(line);
        vector<string> fields;
        size_t from = 0;
        for (size_t tab = line.find('\t'); tab != string::npos && fields.size() < 4; tab = line.find('\t', from)) {
            fields.push_back(line.substr(from, tab - from));
            from = tab + 1;
        }
        if (fields.size() < 4)
            continue; // the header, or a line without an SSID column
        fields.push_back(line.substr(from));
        WifiNetwork network;
        network.bssid = fields[0];
        network.frequency = Strings::toInt(fields[1]);
        network.signal = Strings::toInt(fields[2]);
        network.flags = fields[3];
        network.ssid = decodeSsid(fields[4]);
        // a hidden network announces an empty SSID, or one of NUL bytes
        if (network.ssid.find_first_not_of('\0') == string::npos)
            continue;
        auto it = strongest.find(network.ssid);
        if (it == strongest.end() || network.signal > it->second.signal)
            strongest[network.ssid] = network;
    }
    vector<WifiNetwork> out;
    for (auto &entry : strongest)
        out.push_back(entry.second);
    stable_sort(out.begin(), out.end(), [](const WifiNetwork &a, const WifiNetwork &b) { return a.signal > b.signal; });
    return out;
}

//*******************************
// WpaCtrlClient::parseStatus
//*******************************
WpaStatus WpaCtrlClient::parseStatus(const string &reply) {
    WpaStatus status;
    istringstream in(reply);
    string line;
    while (getline(in, line)) {
        line = withoutTrailingNewlines(line);
        size_t eq = line.find('=');
        if (eq == string::npos)
            continue;
        string key = line.substr(0, eq);
        string value = line.substr(eq + 1);
        if (key == "wpa_state")
            status.wpaState = value;
        else if (key == "ssid")
            status.ssid = decodeSsid(value);
        else if (key == "ip_address")
            status.ipAddress = value;
    }
    return status;
}

//*******************************
// WpaCtrlClient::decodeSsid
//*******************************
string WpaCtrlClient::decodeSsid(const string &encoded) {
    string out;
    for (size_t i = 0; i < encoded.size(); i++) {
        char c = encoded[i];
        if (c != '\\' || i + 1 >= encoded.size()) {
            out += c;
            continue;
        }
        char next = encoded[++i];
        switch (next) {
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'e':
            out += '\033';
            break;
        case 'x':
            if (i + 2 < encoded.size() && hexDigit(encoded[i + 1]) >= 0 && hexDigit(encoded[i + 2]) >= 0) {
                out += static_cast<char>(hexDigit(encoded[i + 1]) * 16 + hexDigit(encoded[i + 2]));
                i += 2;
            } else {
                out += next;
            }
            break;
        default: // \\ and \" - and anything else, kept as the character itself
            out += next;
            break;
        }
    }
    return out;
}

//*******************************
// WpaCtrlClient::ssidValue / pskValue
//*******************************
string WpaCtrlClient::ssidValue(const string &ssid) {
    if (ssid.empty() || ssid.size() > 32)
        return "";
    if (isPrintableAscii(ssid) && ssid.find('"') == string::npos)
        return "\"" + ssid + "\"";
    static const char *const digits = "0123456789abcdef";
    string hex;
    for (unsigned char c : ssid) {
        hex += digits[c >> 4];
        hex += digits[c & 0x0f];
    }
    return hex;
}

string WpaCtrlClient::pskValue(const string &password) {
    if (password.size() == 64 && all_of(password.begin(), password.end(), [](char c) { return hexDigit(c) >= 0; }))
        return password; // a raw 256-bit key, unquoted
    if (password.size() < 8 || password.size() > 63 || !isPrintableAscii(password))
        return "";
    return "\"" + password + "\""; // wpa_supplicant takes everything up to the last quote
}
