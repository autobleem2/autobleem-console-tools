//
// NetworkStatus: one round of questions to the console.
//
#include "network_status.h"
#include "console_backend.h"
#include "core/main.h"

using namespace std;

//*******************************
// NetworkStatus::refresh
//*******************************
void NetworkStatus::refresh(ConsoleBackend &backend) {
    wirelessIface = backend.wifiInterface();
    wirelessFound = !wirelessIface.empty();
    wirelessActive = wirelessFound && backend.isUp(wirelessIface);
    wirelessAddr = wirelessFound ? backend.ipOf(wirelessIface) : "";
    ethIface = backend.ethernetInterface();
    ethFound = !ethIface.empty();
    ethActive = ethFound && backend.isUp(ethIface);
    ethAddr = ethFound ? backend.ipOf(ethIface) : "";
    btActive = backend.btUp();
    btName = btActive ? backend.btName() : "";
    btError = btActive ? "" : backend.btLastError();
    timezone = backend.timezone();
}

//*******************************
// NetworkStatus::dongleStatus / btStatusText / addressText
//*******************************
string NetworkStatus::dongleStatus(bool found, bool active) {
    return (found ? _("Found") : _("Not found")) + "/" + (active ? _("Active") : _("Not active"));
}

string NetworkStatus::btStatusText() const {
    if (btActive || btError.empty() || btError == _("no Bluetooth adapter"))
        return dongleStatus(btActive, btActive);
    return _("Unavailable");
}

string NetworkStatus::addressText(bool found, const string &address) {
    if (!found)
        return "-";
    return address.empty() ? _("Waiting for IP Address...") : address;
}
