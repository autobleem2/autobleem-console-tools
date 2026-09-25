//
// ConsoleBackend: the dev host's stand-in (the console's backend is NativeBackend, native_backend.*).
//
#include "console_backend.h"
#include "core/main.h"

#include <ableem/engine/log.h>

using namespace std;

//*******************************
// FakeBackend
//*******************************
void FakeBackend::configureWifi(const string &ssid, const string &password, const string &driverMode) {
    PLOG_INFO << "fake configureWifi \"" << ssid << "\" (password of " << password.size() << " chars), driver "
              << driverMode;
    configuredSsid = ssid;
    configuredPassword = password;
    configuredDriverMode = driverMode;
}

void FakeBackend::restartNetwork() {
    PLOG_INFO << "fake restartNetwork";
    restarts++;
}

vector<BtDevice> FakeBackend::btScan() {
    PLOG_INFO << "fake btScan";
    return btDevices_; // a scan reveals everything nearby, paired or not
}

vector<BtDevice> FakeBackend::btPairedDevices() {
    vector<BtDevice> out;
    for (const BtDevice &d : btDevices_)
        if (d.paired)
            out.push_back(d);
    return out;
}

bool FakeBackend::btPair(const string &mac) {
    PLOG_INFO << "fake btPair " << mac;
    for (BtDevice &d : btDevices_)
        if (d.mac == mac) {
            d.paired = true;
            d.connected = true;
            return true;
        }
    return false;
}

bool FakeBackend::btRemove(const string &mac) {
    PLOG_INFO << "fake btRemove " << mac;
    for (BtDevice &d : btDevices_)
        if (d.mac == mac) {
            d.paired = false;
            d.connected = false;
            return true;
        }
    return false;
}

void FakeBackend::setTimezone(const string &zone) {
    PLOG_INFO << "fake setTimezone " << zone;
    timezone_ = zone;
}

vector<string> FakeBackend::listTimezones() {
    return {"Africa/Cairo",      "America/Chicago", "America/Los_Angeles", "America/New_York",
            "America/Sao_Paulo", "Asia/Tokyo",      "Asia/Shanghai",       "Australia/Sydney",
            "Europe/Berlin",     "Europe/London",   "Europe/Madrid",       "Europe/Paris",
            "Europe/Rome",       "Europe/Warsaw",   "Pacific/Auckland",    "UTC"};
}

string FakeBackend::btLastError() const {
    return btAdapter_ ? "" : _("no Bluetooth adapter");
}
