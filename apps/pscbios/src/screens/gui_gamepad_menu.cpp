//
// GuiGamepadMenu: the gamepad section.
//
#include "gui_gamepad_menu.h"
#include "gui_bt_pairing.h"
#include "gui_pad_config.h"
#include "pscbios_pages.h"
#include "gui/gui.h"
#include "gui/screens/gui_text_page.h"

using namespace std;

//*******************************
// GuiGamepadMenu::init
//*******************************
void GuiGamepadMenu::init() {
    GuiMenuBase::init();
    lines = {_("Setup/Test Game Controller or update mapping"), _("Connect DualShock3 by Bluetooth (Wireless mode)"),
             _("Connect other gamepad by Bluetooth")};
}

//*******************************
// GuiGamepadMenu::doCircle_Pressed / doCross_Pressed
//*******************************
void GuiGamepadMenu::doCircle_Pressed() {
    app.audio().cancel.play();
    menuVisible = false;
}

void GuiGamepadMenu::doCross_Pressed() {
    app.audio().cursor.play();
    switch (selected) {
    case Mapping:
        showWizard(*gui);
        break;
    case DualShock:
        showDualShock3Page(*gui);
        break;
    case BluetoothPairing:
        showBluetoothPairing(*gui);
        break;
    default:
        break;
    }
}

//*******************************
// GuiGamepadMenu::showWizard
//*******************************
// the wizard reads the pads raw, so Input lets go of them first and takes them back (with whatever mapping
// the wizard added) after
void GuiGamepadMenu::showWizard(ableem::GuiBase &gui) {
    gui.input().flushPads();
    {
        GuiPadConfig wizard(gui);
        wizard.show();
    }
    gui.input().probePads();
    gui.input().flushEvents();
}

//*******************************
// GuiGamepadMenu::showDualShock3Page / showBluetoothPairing
//*******************************
void GuiGamepadMenu::showDualShock3Page(ableem::GuiBase &gui) {
    GuiTextPage page(gui);
    page.title = _("DualShock3/SixAxis Wireless Pairing");
    page.lines = dualshock3PairingLines();
    page.show();
}

void GuiGamepadMenu::showBluetoothPairing(ableem::GuiBase &gui) {
    GuiBtPairing pairing(gui);
    pairing.show();
}
