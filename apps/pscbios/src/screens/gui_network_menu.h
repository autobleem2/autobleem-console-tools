//
// GuiNetworkMenu: the WiFi settings - the SSID (typed, or picked from a scan with each network's signal), the
// password, the driver mode, the timezone, the connection as wpa_supplicant has it now (re-read every
// RefreshInterval), and the two actions: writing the settings and restarting the network, or a restart alone.
// Every action that waits runs under the busy spinner, and after a write or a restart the connection is followed
// to Connected or the reason it failed (a wrong password, the network not found, no address...) - Circle stops
// following it. The last failure and its reason is a row of its own under the connection. Option rows in the
// shared look: the label at the left, the value at the row's right edge; a compact panel.
//
#pragma once

#include "gui/menus/gui_string_menu.h"
#include "core/network_status.h"
#include "core/ssid_config.h"

#include <string>
#include <vector>

//********************
// GuiNetworkMenu
//********************
class GuiNetworkMenu : public GuiStringMenu {
public:
    explicit GuiNetworkMenu(ableem::GuiBase &_gui) : GuiStringMenu(_gui) {}

    void init() override;
    void render() override;
    void renderLineIndexOnRow(int index, int row) override;
    std::string getTitle() override { return _("Edit Network WPA WiFi Credentials"); }
    std::string getStatusLine() override;
    bool skipSelectingThisLineWhenMovingByOne(int index) override;

    void doCircle_Pressed() override;
    void doCross_Pressed() override;
    void doTriangle_Pressed() override;

    bool displayAsterisksInsteadOfPassword = false;

    static const unsigned int RefreshInterval = 2000; // ms between re-reads of the connection and timezone

private:
    enum class Row { Ssid, Password, DriverMode, TimeZone, Connection, Message, WriteFile, InitNetwork };
    SsidConfig config;
    std::string connection, timezone;
    std::string message_;            // the last failure and why ("" for none): the Message row
    std::vector<Row> rows;           // what each of `lines` is
    std::vector<std::string> values; // one per row of `lines`, "" for an action row
    std::string busyFooter_;         // the footer while busy, drawn on the spinner's backdrop
    bool busy_ = false;
    unsigned int lastRefresh = 0;

    Row rowAt(int index) const;
    void refresh();     // the connection and the timezone from the console
    void fill();        // the rows from the current values
    bool writeConfig(); // false when nothing was written, or the console refused it (message_ says why)
    void restartNetwork();
    void followConnection(); // the connection to config.ssid, under the spinner, until it is up or fails
    void editSsid();
    void editPassword();
    void scanSsid();
    void pickTimezone();
};
