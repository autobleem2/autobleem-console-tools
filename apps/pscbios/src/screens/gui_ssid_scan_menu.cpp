//
// GuiSsidScanMenu / GuiTimezoneSelect: the two pickers.
//
#include "gui_ssid_scan_menu.h"
#include "../pscbios.h"
#include "gui/gui.h"

using namespace std;

//*******************************
// GuiSsidScanMenu
//*******************************
void GuiSsidScanMenu::init() {
    GuiMenuBase::init();
    lines.clear();
    for (const WifiNetwork &network : networks)
        lines.push_back(network.ssid);
}

string GuiSsidScanMenu::signalColumn(const WifiNetwork &network) {
    string text = WifiNetwork::signalText(network.signal) + ", " + to_string(network.signal) + " dBm";
    if (!network.flags.empty() && !network.secured())
        text += ", " + _("open");
    return text;
}

// the SSID at the left (elided to leave room), its signal at the right edge
void GuiSsidScanMenu::renderLineIndexOnRow(int index, int row) {
    const string value = signalColumn(networks[static_cast<size_t>(index)]);
    const int width = gui->classicContent().w - 64 - gui->text().textWidth(font, value) - 32;
    gui->text().renderTextLine(gui->text().elide(font, lines[index], width), row, yoffset, XALIGN_LEFT, 0, font);
    gui->text().renderRowValue(value, row, yoffset, 0, font);
}

string GuiSsidScanMenu::getStatusLine() {
    string menu;
    if (!lines.empty())
        menu += "|@X| " + _("Select") + "   ";
    return menu + "|@O| " + _("Back");
}

void GuiSsidScanMenu::doCross_Pressed() {
    if (lines.empty())
        return;
    app.audio().cursor.play();
    newSsid = lines[selected];
    cancelled = false;
    menuVisible = false;
}

void GuiSsidScanMenu::doCircle_Pressed() {
    app.audio().cancel.play();
    cancelled = true;
    menuVisible = false;
}

//*******************************
// GuiTimezoneSelect
//*******************************
// the zoneinfo tables read - a file, quick; no spinner needed
void GuiTimezoneSelect::init() {
    GuiMenuBase::init();
    lines = PscBios::get().console().listTimezones();
}

void GuiTimezoneSelect::doCross_Pressed() {
    if (lines.empty())
        return;
    app.audio().cursor.play();
    newTimezone = lines[selected];
    cancelled = false;
    menuVisible = false;
}

void GuiTimezoneSelect::doCircle_Pressed() {
    app.audio().cancel.play();
    cancelled = true;
    menuVisible = false;
}
