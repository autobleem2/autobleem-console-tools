//
// BtDeviceList: the pairing screen's rows and their states.
//
#include "bt_device_list.h"
#include "core/main.h"

#include <algorithm>

using namespace std;

//*******************************
// BtDeviceList: lookups
//*******************************
int BtDeviceList::indexOf(const string &mac) const {
    for (size_t i = 0; i < rows_.size(); i++)
        if (rows_[i].device.mac == mac)
            return static_cast<int>(i);
    return -1;
}

const BtRow *BtDeviceList::find(const string &mac) const {
    int i = indexOf(mac);
    return i < 0 ? nullptr : &rows_[static_cast<size_t>(i)];
}

BtRow *BtDeviceList::row(const string &mac) {
    int i = indexOf(mac);
    return i < 0 ? nullptr : &rows_[static_cast<size_t>(i)];
}

bool BtDeviceList::underWay(BtRowState state) {
    return state == BtRowState::Discovering || state == BtRowState::Pairing || state == BtRowState::Connecting;
}

BtRowState BtDeviceList::settledState(const BtRow &row) {
    if (!row.device.paired)
        return row.error.empty() ? BtRowState::New : BtRowState::Failed;
    if (row.device.connected)
        return BtRowState::Connected;
    return row.wasConnected ? BtRowState::Dropped : BtRowState::Paired;
}

//*******************************
// BtDeviceList::setScanned / updatePaired
//*******************************
void BtDeviceList::setScanned(const vector<BtDevice> &found) {
    vector<BtRow> next;
    for (const BtDevice &d : found) {
        BtRow r;
        if (const BtRow *known = find(d.mac))
            r = *known;
        r.device.mac = d.mac;
        r.device.name = d.name;
        r.device.paired = d.paired;
        r.device.connected = d.connected;
        if (d.battery.known() || !d.connected)
            r.device.battery = d.battery;
        r.wasConnected = r.wasConnected || d.connected;
        if (!underWay(r.state))
            r.state = settledState(r);
        next.push_back(r);
    }
    // paired devices the scan did not see (switched off, out of range) stay listed
    for (const BtRow &r : rows_)
        if (r.device.paired &&
            none_of(next.begin(), next.end(), [&](const BtRow &n) { return n.device.mac == r.device.mac; }))
            next.push_back(r);
    rows_ = next;
}

void BtDeviceList::updatePaired(const vector<BtDevice> &paired) {
    for (BtRow &r : rows_) {
        auto it = find_if(paired.begin(), paired.end(), [&](const BtDevice &d) { return d.mac == r.device.mac; });
        if (underWay(r.state))
            continue; // the pairing's own stages say more
        if (it == paired.end()) {
            r.device.paired = false; // forgotten (here, or by another tool)
            r.device.connected = false;
            r.device.battery = BtBattery();
            r.wasConnected = false;
        } else {
            r.device.paired = true;
            r.device.connected = it->connected;
            r.device.battery = it->battery;
            if (!it->name.empty())
                r.device.name = it->name;
            r.wasConnected = r.wasConnected || it->connected;
        }
        r.state = settledState(r);
    }
    for (const BtDevice &d : paired) {
        if (find(d.mac) != nullptr)
            continue;
        BtRow r;
        r.device = d;
        r.wasConnected = d.connected;
        r.state = settledState(r);
        rows_.push_back(r);
    }
}

//*******************************
// BtDeviceList: the pairing and the removal
//*******************************
void BtDeviceList::pairStage(const string &mac, BtPairStage stage) {
    BtRow *r = row(mac);
    if (r == nullptr)
        return;
    switch (stage) {
    case BtPairStage::Idle:
    case BtPairStage::Discovering:
        r->state = BtRowState::Discovering;
        break;
    case BtPairStage::Trusting:
    case BtPairStage::Pairing:
    case BtPairStage::WaitingForPaired:
        r->state = BtRowState::Pairing;
        break;
    case BtPairStage::Connecting:
        r->state = BtRowState::Connecting;
        break;
    case BtPairStage::Done:
    case BtPairStage::Failed:
        break; // pairFinished says how it ended
    }
    r->error.clear();
}

void BtDeviceList::pairFinished(const string &mac, bool paired, bool connected, const string &error) {
    BtRow *r = row(mac);
    if (r == nullptr)
        return;
    r->device.paired = paired;
    r->device.connected = paired && connected;
    r->wasConnected = r->device.connected;
    r->error = error; // a pairing that did not connect keeps its reason too
    // failed with its reason; a cancelled one (no error) is back to what it was, new
    r->state = settledState(*r);
}

void BtDeviceList::removed(const string &mac, bool ok, const string &error) {
    BtRow *r = row(mac);
    if (r == nullptr)
        return;
    if (ok) {
        r->device.paired = false;
        r->device.connected = false;
        r->device.battery = BtBattery();
        r->wasConnected = false;
        r->error.clear();
    } else {
        r->error = error;
    }
    r->state = settledState(*r); // a paired pad whose removal failed stays paired; the error is shown apart
}

//*******************************
// BtDeviceList::stateText / batteryText
//*******************************
string BtDeviceList::batteryText(const BtBattery &battery) {
    if (!battery.known())
        return "";
    const string percent = to_string(battery.percent) + "%";
    if (battery.status == "Charging")
        return _("charging") + " " + percent;
    if (battery.status == "Full")
        return _("battery full");
    return _("battery") + " " + percent;
}

string BtDeviceList::stateText(const BtRow &row) {
    string text;
    switch (row.state) {
    case BtRowState::New:
        return _("new");
    case BtRowState::Discovering:
        return _("discovering...");
    case BtRowState::Pairing:
        return _("pairing...");
    case BtRowState::Connecting:
        return _("connecting...");
    case BtRowState::Failed:
        return _("failed");
    case BtRowState::Dropped:
        return _("dropped");
    case BtRowState::Paired:
        text = _("paired");
        break;
    case BtRowState::Connected:
        text = _("connected");
        break;
    }
    const string battery = batteryText(row.device.battery);
    return battery.empty() ? text : text + ", " + battery;
}
