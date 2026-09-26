//
// GuiBtPairing: pair a DualShock 4 or other standard Bluetooth gamepad. A compact list - a "Scan for
// controllers" action on top, then the discovered/known devices, each with its live state at the right edge
// (new / discovering... / pairing... / connecting... / paired / connected / dropped / failed, and the battery),
// re-read every RefreshInterval; the last failure and its reason is the last row. Cross pairs a device that is not
// paired, Triangle removes a pairing, Circle goes back. The scan and the pairing run under the busy spinner and
// Circle stops them; the pairing's stage is what the spinner says. DualShock 3 is NOT here: it pairs over USB
// through the sixaxis plugin (see the DualShock 3 page). Needs a Bluetooth adapter (btUp()); without one the
// screen says so, and why.
//
#pragma once

#include "core/bt_device_list.h"
#include "core/console_backend.h"
#include "gui/menus/gui_string_menu.h"

#include <string>
#include <vector>

//********************
// GuiBtPairing
//********************
class GuiBtPairing : public GuiStringMenu {
public:
    explicit GuiBtPairing(ableem::GuiBase &_gui) : GuiStringMenu(_gui) {}

    void init() override;
    void render() override;
    void renderLineIndexOnRow(int index, int row) override;
    std::string getTitle() override { return _("Bluetooth controller pairing"); }
    std::string getStatusLine() override;

    void doCross_Pressed() override;
    void doTriangle_Pressed() override;
    void doCircle_Pressed() override;

    static const unsigned int RefreshInterval = 2000; // ms between re-reads of the paired devices

private:
    ConsoleBackend &backend();
    void rebuild(); // lines from hasAdapter_ + devices_ + message_
    void scan();
    void pair(const BtDevice &device);
    void remove(const BtDevice &device);
    void refreshPaired();            // the paired devices' connection and battery, again
    int deviceIndex(int line) const; // the device on `line` of `lines`, -1 for the other rows
    static std::string stageMessage(BtPairStage stage, const std::string &name);

    bool hasAdapter_ = false;
    std::string adapterError_; // why there is no adapter (btLastError()), shown under the two info rows
    BtDeviceList devices_;     // rows 1..N (row 0 is the Scan action)
    std::string message_;      // the last failure and why: the last row
    std::string busyFooter_;   // the footer while busy ("|@O| Cancel"), drawn on the spinner's backdrop
    bool busy_ = false;
    unsigned int lastRefresh_ = 0;
};
