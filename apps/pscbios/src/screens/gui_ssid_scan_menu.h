//
// GuiSsidScanMenu: the networks in range, one to pick - each SSID with its signal at the right edge ("Good,
// -60 dBm", "open" for a network without a password), strongest first. The scan itself runs before (the WiFi
// screen's, under the spinner); `networks` is its result. `newSsid` is the pick when not `cancelled`.
//
#pragma once

#include "core/wifi_connect.h"
#include "gui/menus/gui_string_menu.h"

#include <string>
#include <vector>

//********************
// GuiSsidScanMenu
//********************
class GuiSsidScanMenu : public GuiStringMenu {
public:
    explicit GuiSsidScanMenu(ableem::GuiBase &_gui) : GuiStringMenu(_gui) {}

    void init() override;
    void renderLineIndexOnRow(int index, int row) override;
    std::string getTitle() override { return _("Select WiFi Network to connect"); }
    std::string getStatusLine() override;
    void doCircle_Pressed() override;
    void doCross_Pressed() override;

    std::vector<WifiNetwork> networks; // given before show()
    std::string newSsid;

    // the value column: "Excellent, -48 dBm", with ", open" for a network without a password
    static std::string signalColumn(const WifiNetwork &network);
};

//********************
// GuiTimezoneSelect
//********************
// the system's timezone names, one to pick. `newTimezone` is the pick when not `cancelled`.
class GuiTimezoneSelect : public GuiStringMenu {
public:
    explicit GuiTimezoneSelect(ableem::GuiBase &_gui) : GuiStringMenu(_gui) {}

    void init() override;
    std::string getTitle() override { return _("Select Time Zone for Your Location"); }
    void doCircle_Pressed() override;
    void doCross_Pressed() override;

    std::string newTimezone;
};
