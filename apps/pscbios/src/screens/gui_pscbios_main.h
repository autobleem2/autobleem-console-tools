//
// GuiPscBiosMain: the hardware information screen PSC-Bios opens on - a GuiFactsPage of the time and
// timezone, the WiFi, ethernet and Bluetooth dongles with their addresses, the connected pads and whether
// each has a mapping. Circle closes the tool - the only key: the WiFi settings and the gamepads are the quick
// menu's "network" entry (showNetworkHub), and the About that Triangle opened here is gone (the owner,
// 2026-10-06: it crashed, and the rest is already in the quick menu).
//
#pragma once

#include "gui/screens/gui_facts_page.h"
#include "core/network_status.h"

//********************
// GuiPscBiosMain
//********************
class GuiPscBiosMain : public GuiFactsPage {
public:
    explicit GuiPscBiosMain(ableem::GuiBase &_gui) : GuiFactsPage(_gui) { refreshInterval = 2000; }

    void init() override;

protected:
    std::string title() override { return _("Playstation Classic Hardware Information"); }
    std::vector<abgui::FactsSection> collect() override;

private:
    NetworkStatus status;
    bool kernel = false; // the AutoBleem kernel: the network rows need it

    static std::string clockText(); // the local time as asctime() prints it, without its newline
};
