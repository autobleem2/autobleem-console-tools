//
// GuiNetworkHub: the Network & Controllers hub. See the header.
//
#include "gui_network_hub.h"
#include "gui_gamepad_menu.h"
#include "gui_network_menu.h"
#include "../pscbios.h"
#include "gui/gui.h"
#include "gui/screens/gui_action_menu.h"
#include "gui/screens/gui_text_page.h"

using namespace std;

namespace {
enum Item { WifiNetwork = 0, BluetoothPairing, DualShock3, Mapping };

// the Wi-Fi screen needs the platform's network tools: the AutoBleem kernel's abnet on the console,
// NetworkManager's nmcli on a Pi or the PC stick
void showNoNetworkTools(ableem::GuiBase &gui) {
    GuiTextPage page(gui);
    page.title = _("Wi-Fi network");
    page.lines = {_("The Wi-Fi settings need the system's network tools, which are not there."),
                  _("On the console they come with the AutoBleem kernel; elsewhere with NetworkManager.")};
    page.show();
}
} // namespace

//*******************************
// showNetworkHub
//*******************************
void showNetworkHub(ableem::GuiBase &gui) {
    GuiActionMenu menu(gui);
    menu.title = _("Network & Controllers");
    menu.items = {{_("Wi-Fi network"), _("Choose a network, enter its password, set the time zone")},
                  {_("Bluetooth controllers"), _("Pair a DualShock 4 or another Bluetooth gamepad")},
                  {_("DualShock 3 pairing"), _("How a DualShock 3 pairs over its USB cable")},
                  {_("Controller mapping"), _("Test a controller and map its buttons")}};
    for (;;) {
        menu.show();
        switch (menu.result) {
        case WifiNetwork:
            if (PscBios::get().console().kernelInstalled()) {
                GuiNetworkMenu network(gui);
                network.show();
            } else {
                showNoNetworkTools(gui);
            }
            break;
        case BluetoothPairing:
            GuiGamepadMenu::showBluetoothPairing(gui);
            break;
        case DualShock3:
            GuiGamepadMenu::showDualShock3Page(gui);
            break;
        case Mapping:
            GuiGamepadMenu::showWizard(gui);
            break;
        default:
            return; // Circle
        }
    }
}
