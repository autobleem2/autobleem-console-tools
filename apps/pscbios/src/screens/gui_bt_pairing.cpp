//
// GuiBtPairing: the Bluetooth controller pairing screen.
//
#include "gui_bt_pairing.h"
#include "pscbios_busy.h"
#include "gui/gui.h"
#include "gui/screens/gui_confirm.h"
#include "pscbios.h"

using namespace std;

ConsoleBackend &GuiBtPairing::backend() {
    return PscBios::get().console();
}

//*******************************
// GuiBtPairing::init
//*******************************
void GuiBtPairing::init() {
    GuiMenuBase::init();
    hasAdapter_ = backend().btUp();
    adapterError_ = hasAdapter_ ? "" : backend().btLastError();
    if (hasAdapter_)
        devices_.updatePaired(backend().btPairedDevices());
    lastRefresh_ = gui->platform().ticks();
    rebuild();
}

//*******************************
// GuiBtPairing::rebuild / deviceIndex
//*******************************
void GuiBtPairing::rebuild() {
    lines.clear();
    if (!hasAdapter_) {
        lines.push_back(_("No Bluetooth adapter found."));
        lines.push_back(_("Plug a USB Bluetooth dongle into the console, then open this screen again."));
        if (!adapterError_.empty())
            lines.push_back(adapterError_); // why: no adapter, or the system bus / BlueZ not answering
    } else {
        lines.push_back(_("Scan for controllers"));
        for (const BtRow &row : devices_.rows())
            lines.push_back(row.device.name);
        if (!message_.empty())
            lines.push_back(message_);
    }
    if (selected >= getVerticalSize())
        selected = 0;
    computePagePosition();
}

int GuiBtPairing::deviceIndex(int line) const {
    if (!hasAdapter_ || line < 1 || line > static_cast<int>(devices_.rows().size()))
        return -1;
    return line - 1;
}

//*******************************
// GuiBtPairing::render / renderLineIndexOnRow
//*******************************
// the paired devices re-read every RefreshInterval, so a pad that connects, drops or runs down shows it
void GuiBtPairing::render() {
    if (hasAdapter_ && !busy_ && gui->platform().ticks() - lastRefresh_ >= RefreshInterval)
        refreshPaired();
    GuiStringMenu::render();
}

// a device: its name at the left (elided to leave room), its state at the right edge; the other rows whole
void GuiBtPairing::renderLineIndexOnRow(int index, int row) {
    const int width = gui->classicContent().w - 64;
    const int device = deviceIndex(index);
    if (device < 0) {
        gui->text().renderTextLine(gui->text().elide(font, lines[index], width), row, yoffset, XALIGN_LEFT, 0, font);
        return;
    }
    const BtRow &r = devices_.rows()[static_cast<size_t>(device)];
    const string state = BtDeviceList::stateText(r);
    const int stateWidth = gui->text().textWidth(font, state);
    gui->text().renderTextLine(gui->text().elide(font, r.device.name, width - stateWidth - 32), row, yoffset,
                               XALIGN_LEFT, 0, font);
    gui->text().renderRowValue(state, row, yoffset, 0, font);
}

//*******************************
// GuiBtPairing::refreshPaired
//*******************************
void GuiBtPairing::refreshPaired() {
    lastRefresh_ = gui->platform().ticks();
    devices_.updatePaired(backend().btPairedDevices());
    rebuild();
}

//*******************************
// GuiBtPairing::scan
//*******************************
void GuiBtPairing::scan() {
    app.audio().cursor.play();
    message_.clear();
    rebuild();
    busy_ = true;
    busyFooter_ = "|@O| " + _("Stop");
    vector<BtDevice> found;
    string error;
    bool stopped = false;
    {
        BusyWork busy(*gui, backend(), _("Scanning for controllers..."), [this]() { render(); }, true);
        found = backend().btScan();
        error = backend().btLastError();
        stopped = busy.stopped();
        if (error.empty())
            devices_.setScanned(found);
        devices_.updatePaired(backend().btPairedDevices());
    }
    busy_ = false;
    busyFooter_.clear();
    if (!error.empty())
        message_ = _("Bluetooth scan failed") + ": " + error;
    else if (found.empty() && !stopped)
        message_ = _("No controllers found - is yours in pairing mode?");
    lastRefresh_ = gui->platform().ticks();
    rebuild();
}

//*******************************
// GuiBtPairing::pair
//*******************************
string GuiBtPairing::stageMessage(BtPairStage stage, const string &name) {
    switch (stage) {
    case BtPairStage::Idle:
    case BtPairStage::Discovering:
        return _("Looking for") + " " + name + "...";
    case BtPairStage::Connecting:
        return _("Connecting") + " " + name + "...";
    default:
        return _("Pairing") + " " + name + "...";
    }
}

// the pairing pumped once a frame under the spinner, which says its stage; the row shows it too (the backdrop)
void GuiBtPairing::pair(const BtDevice &device) {
    app.audio().cursor.play();
    const string mac = device.mac;
    message_.clear();
    devices_.pairStage(mac, BtPairStage::Discovering);
    rebuild();
    busy_ = true;
    busyFooter_ = "|@O| " + _("Cancel");
    BtPairStage stage = BtPairStage::Failed;
    bool stopped = false;
    {
        BusyWork busy(
            *gui, backend(), stageMessage(BtPairStage::Discovering, device.name), [this]() { render(); }, true);
        if (backend().btBeginPair(mac)) {
            BtRowState shown = BtRowState::Discovering;
            for (stage = backend().btPumpPair(); stage != BtPairStage::Done && stage != BtPairStage::Failed;
                 stage = backend().btPumpPair()) {
                devices_.pairStage(mac, stage);
                const BtRow *row = devices_.find(mac);
                if (row != nullptr && row->state != shown) {
                    shown = row->state;
                    rebuild();
                    busy.setMessage(stageMessage(stage, device.name));
                }
                if (!busy.frame()) {
                    backend().btCancelPair();
                    stage = BtPairStage::Failed;
                    break;
                }
            }
        }
        stopped = busy.stopped();
    }
    busy_ = false;
    busyFooter_.clear();
    const bool paired = stage == BtPairStage::Done;
    const bool connected = paired && backend().btPairConnected();
    const string error = backend().btLastError();
    devices_.pairFinished(mac, paired, connected, stopped ? "" : error);
    if (stopped && !paired)
        message_ = _("Pairing cancelled");
    else if (!paired)
        message_ = _("Pairing failed") + ": " + error;
    else if (!connected && !error.empty())
        message_ = _("Paired, but not connected") + ": " + error; // the pad connects itself on its PS button
    if (paired)
        refreshPaired(); // its battery
    rebuild();
}

//*******************************
// GuiBtPairing::remove
//*******************************
void GuiBtPairing::remove(const BtDevice &device) {
    GuiConfirm confirm(*gui);
    confirm.label = _("Remove pairing for") + " " + device.name + "?";
    confirm.show();
    if (!confirm.result)
        return;
    message_.clear();
    busy_ = true;
    bool removed = false;
    string error;
    {
        BusyWork busy(*gui, backend(), _("Removing") + " " + device.name + "...", [this]() { render(); }, false);
        removed = backend().btRemove(device.mac);
        error = backend().btLastError();
    }
    busy_ = false;
    devices_.removed(device.mac, removed, error);
    if (!removed)
        message_ = _("Removing the pairing failed") + ": " + error;
    refreshPaired();
}

//*******************************
// GuiBtPairing::doCross_Pressed / doTriangle_Pressed / doCircle_Pressed
//*******************************
void GuiBtPairing::doCross_Pressed() {
    if (!hasAdapter_)
        return; // the info rows do nothing
    if (selected == 0) {
        scan();
        return;
    }
    const int idx = deviceIndex(selected);
    if (idx < 0)
        return;
    const BtRow row = devices_.rows()[static_cast<size_t>(idx)];
    if (row.device.paired)
        return; // Triangle removes it; a paired pad connects itself on its PS button
    pair(row.device);
}

void GuiBtPairing::doTriangle_Pressed() {
    const int idx = deviceIndex(selected);
    if (idx < 0)
        return;
    const BtRow row = devices_.rows()[static_cast<size_t>(idx)];
    if (!row.device.paired)
        return;
    app.audio().cursor.play();
    remove(row.device);
}

void GuiBtPairing::doCircle_Pressed() {
    app.audio().cancel.play();
    menuVisible = false;
}

//*******************************
// GuiBtPairing::getStatusLine
//*******************************
string GuiBtPairing::getStatusLine() {
    if (!busyFooter_.empty())
        return busyFooter_;
    if (!hasAdapter_)
        return "|@O| " + _("Back");
    if (selected == 0)
        return "|@X| " + _("Scan") + "   |@O| " + _("Back");
    const int idx = deviceIndex(selected);
    if (idx < 0)
        return "|@O| " + _("Back");
    if (devices_.rows()[static_cast<size_t>(idx)].device.paired)
        return "|@T| " + _("Remove") + "   |@O| " + _("Back");
    return "|@X| " + _("Pair") + "   |@O| " + _("Back");
}
