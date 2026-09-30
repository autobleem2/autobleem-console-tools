//
// GuiNetworkMenu: the WiFi settings - the SSID (typed, or picked from a scan with each network's signal), the
// password, the timezone, the connection as wpa_supplicant has it now (re-read every
// RefreshInterval on a worker thread - the frame never waits for it), and the two actions: writing the settings and restarting the network, or a restart alone.
// Every action that waits runs under the busy spinner, and after a write or a restart the connection is followed
// to Connected or the reason it failed (a wrong password, the network not found, no address...) - Circle stops
// following it. The last failure and its reason is a row of its own under the connection. Option rows in the
// shared look: the label at the left, the value at the row's right edge; a compact panel.
//
#pragma once

#include "gui/menus/gui_string_menu.h"
#include "core/network_status.h"
#include "core/ssid_config.h"
#include "core/wifi_status_worker.h"

#include <string>
#include <vector>

//********************
// GuiNetworkMenu
//********************
class GuiNetworkMenu : public GuiStringMenu {
public:
    explicit GuiNetworkMenu(ableem::GuiBase &_gui) : GuiStringMenu(_gui) {}

    void init() override;
    bool prepareFrame() override; // the status re-read when due and the rows filled, before the frame
    void renderLineIndexOnRow(int index, int row) override;
    std::string getTitle() override { return _("Edit Network WPA WiFi Credentials"); }
    std::string getStatusLine() override;
    bool skipSelectingThisLineWhenMovingByOne(int index) override;

    void doCircle_Pressed() override;
    void doCross_Pressed() override;
    void doTriangle_Pressed() override;

    static const unsigned int RefreshInterval = 2000; // ms between re-reads of the connection and timezone

private:
    enum class Row { Ssid, Password, TimeZone, Connection, Message, Spacer, WriteFile, InitNetwork };
    SsidConfig config;
    std::string connection, timezone;
    std::string message_;            // the last failure and why ("" for none): the Message row
    std::vector<Row> rows;           // what each of `lines` is
    std::vector<std::string> values; // one per row of `lines`, "" for an action row
    std::string busyFooter_;         // the footer while busy, drawn on the spinner's backdrop
    bool busy_ = false;
    bool acting_ = false; // an action (and the screens it shows) is running: no background read meanwhile
    WifiStatusWorker worker_; // the periodic read; wait()ed for before the backend is used here, joined on destruction
    unsigned int lastRefresh = 0;

    Row rowAt(int index) const;
    void refresh();     // the connection and the timezone from the console, now (after an action)
    void refreshInBackground(); // the same from the worker: started when due, taken when finished
    void fill();        // the rows from the current values
    bool writeConfig(); // false when nothing was written, or the console refused it (message_ says why)
    void restartNetwork();
    void followConnection(); // the connection to config.ssid, under the spinner, until it is up or fails
    void editSsid();
    void editPassword();
    void scanSsid();
    void pickTimezone();
};
