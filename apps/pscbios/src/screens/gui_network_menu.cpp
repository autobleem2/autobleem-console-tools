//
// GuiNetworkMenu: the WiFi settings.
//
#include "gui_network_menu.h"
#include "gui_ssid_scan_menu.h" // and GuiTimezoneSelect
#include "pscbios_busy.h"
#include "../pscbios.h"
#include "gui/gui.h"
#include "gui/screens/gui_confirm.h"
#include "gui/screens/gui_keyboard.h"

using namespace std;

//*******************************
// GuiNetworkMenu::init
//*******************************
void GuiNetworkMenu::init() {
    GuiMenuBase::init();
    // ssid.cfg if there is one, else the SSID an earlier setup left in wpa_supplicant.conf
    if (!config.load(SsidConfig::defaultPath()))
        config.ssid = SsidConfig::ssidFromWpaSupplicant(SsidConfig::wpaSupplicantPath());
    refresh();
    fill();
}

//*******************************
// GuiNetworkMenu::refresh / fill
//*******************************
// the connection: wpa_supplicant's state (a short STATUS) and the interface's address - "Connected, 192.168.1.23",
// "Looking for the network", "Not connected", "-" without a WiFi dongle
void GuiNetworkMenu::refresh() {
    lastRefresh = gui->platform().ticks();
    ConsoleBackend &console = PscBios::get().console();
    const string iface = console.wifiInterface(); // any name, not only wlan0
    if (iface.empty()) {
        connection = "-";
    } else {
        string ip = console.ipOf(iface);
        WpaStatus status;
        if (!console.wifiStatus(status)) {
            connection = ip.empty() ? _("Not connected") : ip;
        } else if (status.wpaState == "COMPLETED") {
            if (ip.empty())
                ip = status.ipAddress;
            connection = ip.empty() ? wifiStageText(WifiConnectStage::GettingAddress) : _("Connected") + ", " + ip;
        } else {
            connection = wpaStateText(status.wpaState);
            if (connection.empty())
                connection = _("Not connected");
        }
    }
    timezone = console.timezone();
}

GuiNetworkMenu::Row GuiNetworkMenu::rowAt(int index) const {
    if (index < 0 || index >= static_cast<int>(rows.size()))
        return Row::Connection;
    return rows[static_cast<size_t>(index)];
}

bool GuiNetworkMenu::skipSelectingThisLineWhenMovingByOne(int index) {
    const Row row = rowAt(index);
    return row == Row::Connection || row == Row::Message;
}

// the rows again; the selection stays on the row it was on (the message row coming or going moves the actions)
void GuiNetworkMenu::fill() {
    const bool had = !rows.empty();
    const Row was = rowAt(selected);
    lines.clear();
    values.clear();
    rows.clear();
    auto row = [&](Row kind, const string &label, const string &value) {
        rows.push_back(kind);
        lines.push_back(label);
        values.push_back(value);
    };
    row(Row::Ssid, _("SSID:"), config.ssid);
    row(Row::Password, _("Password:"),
        displayAsterisksInsteadOfPassword ? string(config.password.size(), '*') : config.password);
    row(Row::DriverMode, _("Driver mode:"), config.driverMode);
    row(Row::TimeZone, _("Timezone:"), timezone);
    row(Row::Connection, _("Connection:"), connection);
    if (!message_.empty())
        row(Row::Message, message_, "");
    row(Row::WriteFile, _("Write Configuration/Restart Network"), "");
    row(Row::InitNetwork, _("Restart Network"), "");
    for (size_t i = 0; had && i < rows.size(); i++)
        if (rows[i] == was)
            selected = static_cast<int>(i);
    if (selected >= static_cast<int>(rows.size()))
        selected = 0;
}

//*******************************
// GuiNetworkMenu::renderLineIndexOnRow
//*******************************
// the label at the left, the value at the row's right edge; the message row across the whole row
void GuiNetworkMenu::renderLineIndexOnRow(int index, int row) {
    const int width = gui->classicContent().w - 64;
    if (rowAt(index) == Row::Message) {
        gui->text().renderTextLine(gui->text().elide(font, lines[index], width), row, yoffset, XALIGN_LEFT, 0, font);
        return;
    }
    gui->text().renderTextLine(lines[index], row, yoffset, XALIGN_LEFT, 0, font);
    if (!values[index].empty()) {
        const int labelWidth = gui->text().textWidth(font, lines[index]);
        gui->text().renderRowValue(gui->text().elide(font, values[index], width - labelWidth - 32), row, yoffset, 0,
                                   font);
    }
}

//*******************************
// GuiNetworkMenu::render
//*******************************
void GuiNetworkMenu::render() {
    if (!busy_ && gui->platform().ticks() - lastRefresh >= RefreshInterval)
        refresh();
    fill();
    GuiStringMenu::render();
}

//*******************************
// GuiNetworkMenu::getStatusLine
//*******************************
string GuiNetworkMenu::getStatusLine() {
    if (!busyFooter_.empty())
        return busyFooter_;
    switch (rowAt(selected)) {
    case Row::Ssid:
        return "|@X| " + _("Edit SSID") + "   |@T| " + _("Scan SSID") + "   |@O| " + _("Back");
    case Row::Password:
        return "|@X| " + _("Edit Password") + "   |@O| " + _("Back");
    case Row::WriteFile:
        return "|@X| " + _("Write Config/Restart Network") + "   |@O| " + _("Back");
    case Row::InitNetwork:
        return "|@X| " + _("Restart Network") + "   |@O| " + _("Back");
    case Row::DriverMode:
    case Row::TimeZone:
        return "|@X| " + _("Change") + "   |@O| " + _("Back");
    default:
        return "|@O| " + _("Back");
    }
}

//*******************************
// GuiNetworkMenu::doCircle_Pressed / doTriangle_Pressed
//*******************************
void GuiNetworkMenu::doCircle_Pressed() {
    app.audio().cancel.play();
    menuVisible = false;
}

void GuiNetworkMenu::doTriangle_Pressed() {
    if (rowAt(selected) == Row::Ssid) {
        app.audio().cursor.play();
        scanSsid();
    }
}

//*******************************
// GuiNetworkMenu::doCross_Pressed
//*******************************
void GuiNetworkMenu::doCross_Pressed() {
    app.audio().cursor.play();
    switch (rowAt(selected)) {
    case Row::Ssid:
        editSsid();
        break;
    case Row::Password:
        editPassword();
        break;
    case Row::DriverMode:
        config.driverMode = config.driverMode == "wext" ? "nl80211" : "wext";
        break;
    case Row::WriteFile: {
        if (!writeConfig())
            break;
        GuiConfirm confirm(*gui);
        confirm.label = _("Restart Networking Now?");
        confirm.show();
        if (confirm.result)
            restartNetwork(); // and follows the connection
        else
            followConnection(); // wpa_supplicant took the network already
        break;
    }
    case Row::InitNetwork:
        restartNetwork();
        break;
    case Row::TimeZone:
        pickTimezone();
        break;
    default:
        break;
    }
    refresh();
    fill();
    render();
}

//*******************************
// the actions
//*******************************
void GuiNetworkMenu::editSsid() {
    GuiKeyboard keyboard(*gui);
    keyboard.label = _("Enter SSID");
    keyboard.result = config.ssid;
    keyboard.show();
    if (!keyboard.cancelled)
        config.ssid = keyboard.result;
}

void GuiNetworkMenu::editPassword() {
    GuiKeyboard keyboard(*gui);
    keyboard.label = _("Enter Password");
    keyboard.result = config.password;
    keyboard.displayAsterisksInstead = displayAsterisksInsteadOfPassword;
    keyboard.show();
    if (!keyboard.cancelled)
        config.password = keyboard.result;
}

// the scan under the spinner over this screen (Circle stops it), then the picker - or why there is nothing to pick
void GuiNetworkMenu::scanSsid() {
    ConsoleBackend &console = PscBios::get().console();
    message_.clear();
    busy_ = true;
    busyFooter_ = "|@O| " + _("Stop");
    vector<WifiNetwork> networks;
    string error;
    bool stopped = false;
    {
        BusyWork busy(*gui, console, _("Scanning networks"), [this]() { render(); }, true);
        networks = console.scanNetworks();
        error = console.lastError();
        stopped = busy.stopped();
    }
    busy_ = false;
    busyFooter_.clear();
    if (stopped)
        return;
    if (networks.empty()) {
        // the reason, when there is one: no WiFi dongle, wpa_supplicant not starting or not answering
        message_ = _("No networks found") + (error.empty() ? string() : ": " + error);
        return;
    }
    GuiSsidScanMenu scan(*gui);
    scan.networks = networks;
    scan.show();
    if (!scan.cancelled)
        config.ssid = scan.newSsid;
}

bool GuiNetworkMenu::writeConfig() {
    message_.clear();
    if (config.ssid.empty() || config.password.empty()) {
        message_ = _("Enter the SSID and the password first");
        return false;
    }
    if (!config.save(SsidConfig::defaultPath())) {
        message_ = _("WiFi configuration failed") + ": " + _("cannot write") + " (" + SsidConfig::defaultPath() + ")";
        return false;
    }
    ConsoleBackend &console = PscBios::get().console();
    busy_ = true;
    {
        BusyWork busy(*gui, console, _("Saving the WiFi settings"), [this]() { render(); }, false);
        console.configureWifi(config.ssid, config.password, config.driverMode);
    }
    busy_ = false;
    if (!console.lastError().empty()) {
        message_ = _("WiFi configuration failed") + ": " + console.lastError();
        return false;
    }
    return true;
}

// wpa_supplicant stopped and dhcpcd restarted (its hook starts wpa_supplicant again) under the spinner, then the
// connection followed
void GuiNetworkMenu::restartNetwork() {
    message_.clear();
    ConsoleBackend &console = PscBios::get().console();
    busy_ = true;
    {
        BusyWork busy(*gui, console, _("Reinitializing Network"), [this]() { render(); }, false);
        console.restartNetwork();
    }
    busy_ = false;
    if (!console.lastError().empty()) {
        message_ = _("Network restart failed") + ": " + console.lastError();
        return;
    }
    followConnection();
}

// the connection's stages on the spinner ("Home Network: Checking the password"), pumped once a frame until it is
// connected or failed; Circle stops following it (the connection goes on without us)
void GuiNetworkMenu::followConnection() {
    if (config.ssid.empty())
        return;
    ConsoleBackend &console = PscBios::get().console();
    busy_ = true;
    busyFooter_ = "|@O| " + _("Stop");
    WifiConnectWatch result("", 0);
    {
        console.beginWifiConnect(config.ssid);
        BusyWork busy(
            *gui, console, config.ssid + ": " + wifiStageText(WifiConnectStage::Starting), [this]() { render(); },
            true);
        for (;;) {
            const WifiConnectWatch &watch = console.pumpWifiConnect();
            if (watch.finished()) {
                result = watch;
                break;
            }
            busy.setMessage(config.ssid + ": " + wifiStageText(watch.stage()));
            if (!busy.frame()) {
                result = watch;
                result.cancel();
                break;
            }
        }
    }
    busy_ = false;
    busyFooter_.clear();
    if (result.stage() == WifiConnectStage::Failed && result.failure() != WifiFailure::Cancelled)
        message_ = _("Could not connect to") + " " + config.ssid + ": " + result.text();
    refresh();
}

void GuiNetworkMenu::pickTimezone() {
    GuiTimezoneSelect zones(*gui);
    zones.show();
    if (zones.cancelled)
        return;
    message_.clear();
    ConsoleBackend &console = PscBios::get().console();
    busy_ = true;
    {
        BusyWork busy(*gui, console, _("Changing the timezone"), [this]() { render(); }, false);
        console.setTimezone(zones.newTimezone);
    }
    busy_ = false;
    if (!console.lastError().empty())
        message_ = _("Timezone change failed") + ": " + console.lastError();
}
