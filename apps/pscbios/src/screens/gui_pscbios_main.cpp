//
// GuiPscBiosMain: the hardware information screen.
//
#include "gui_pscbios_main.h"
#include "../pscbios.h"
#include "gui/gui.h"
#include "core/services/environment.h"
#include "core/model/pad_assignment.h"

#include <ableem/ui/joystick.h>

#include <ctime>

using namespace std;

//*******************************
// psPlayerSlotLabel (local)
//*******************************
// the enum's UI text, literal _() calls at each branch so tools/lang_tools.py's extract (which only
// recognises a literal string inside _(...), not a runtime value) picks up the three keys.
static string psPlayerSlotLabel(PsPlayerSlot slot) {
    switch (slot) {
    case PsPlayerSlot::Player1:
        return _("Player 1");
    case PsPlayerSlot::Player2:
        return _("Player 2");
    case PsPlayerSlot::Unused:
    default:
        return _("not used by the PS1 emulator");
    }
}

//*******************************
// GuiPscBiosMain::init
//*******************************
void GuiPscBiosMain::init() {
    kernel = PscBios::get().console().kernelInstalled();
    GuiFactsPage::init();
}

//*******************************
// GuiPscBiosMain::clockText
//*******************************
string GuiPscBiosMain::clockText() {
    time_t t = time(nullptr);
    string clock = asctime(localtime(&t));
    while (!clock.empty() && (clock.back() == '\n' || clock.back() == '\r'))
        clock.pop_back();
    return clock;
}

//*******************************
// GuiPscBiosMain::collect
//*******************************
// the sections; the keys are the 2020 tool's (translated), a trailing colon dropped where the heading
// band or the value column makes it redundant
vector<abgui::FactsSection> GuiPscBiosMain::collect() {
    auto plain = [](string text) {
        while (!text.empty() && (text.back() == ':' || text.back() == ' '))
            text.pop_back();
        return text;
    };
    vector<InfoSection> sections;
    // the package the user installed (the launcher's VERSION), the version every screen shows
    sections.push_back({plain(_("AutoBleem")), {{plain(_("Version")), Env::productVersion()}}});
    if (kernel) {
        status.refresh(PscBios::get().console());
        sections.push_back(
            {_("System"), {{plain(_("Current Time:")), clockText()}, {plain(_("Timezone:")), status.timezone}}});
        // the dongles under whatever name the kernel gave them (the interface row), Bluetooth with the reason
        // it is not there when BlueZ or the system bus does not answer
        InfoSection wifi{
            plain(_("WiFi information:")),
            {{plain(_("Dongle status:")), NetworkStatus::dongleStatus(status.wirelessFound, status.wirelessActive)}}};
        if (status.wirelessFound)
            wifi.rows.push_back({plain(_("Interface details:")), status.wirelessIface});
        wifi.rows.push_back(
            {plain(_("IP configuration:")), NetworkStatus::addressText(status.wirelessFound, status.wirelessAddr)});
        sections.push_back(wifi);
        InfoSection eth{plain(_("Ethernet information:")),
                        {{plain(_("Dongle status:")), NetworkStatus::dongleStatus(status.ethFound, status.ethActive)}}};
        if (status.ethFound)
            eth.rows.push_back({plain(_("Interface details:")), status.ethIface});
        eth.rows.push_back(
            {plain(_("IP configuration:")), NetworkStatus::addressText(status.ethFound, status.ethAddr)});
        sections.push_back(eth);
        sections.push_back({plain(_("Bluetooth information:")),
                            {{plain(_("Dongle status:")), status.btStatusText()},
                             {plain(_("Interface details:")), status.btDetails()}}});
    }

    const int joysticks = ableem::Joystick::count();
    InfoSection pads{plain(_("Game Controller information:")), {}};
    pads.rows.push_back(
        {plain(_("Game Controllers number:")), to_string(gui->input().activePadCount()) + "/" + to_string(joysticks)});
    string mappingFile = gui->input().currentMappingPath();
    pads.rows.push_back({plain(_("Game controller DB:")), mappingFile.empty() ? _("SDL's built-in") : mappingFile});
    // taking Options -> "Swap Player 1 / Player 2" (C11, config.ini "padswap") into account, exactly as
    // gui_hardware_info.cpp's own Controllers section does - core/model/pad_assignment.h's 3-arg
    // psPlayerSlot is the one source of the swap rule; a lone pad stays Player 1 regardless.
    bool padSwap = app.config().inifile.values["padswap"] == "true";
    for (int i = 0; i < joysticks; i++) {
        string name = ableem::Joystick::nameForIndex(i);
        if (ableem::Joystick::isGameControllerAtIndex(i))
            name += _(" - Mapping (Available):") + " " + ableem::Joystick::controllerNameForIndex(i);
        else
            name += _(" - Mapping (Not found)");
        // joysticks are enumerated in the same ascending SDL device-index order pcsx-ab/pcsx-abnxt
        // assign PS1 ports 1/2 by (see core/model/pad_assignment.h), so this position is that port
        PsPlayerSlot slot = psPlayerSlot(i, joysticks, padSwap);
        string playerLabel = psPlayerSlotLabel(slot);
        if (i < 2)
            pads.rows.push_back({playerLabel, name});
        else
            pads.rows.push_back({_("Controller") + " " + to_string(i + 1), name + " (" + playerLabel + ")"});
    }
    sections.push_back(pads);
    return sectionsOf(sections);
}

