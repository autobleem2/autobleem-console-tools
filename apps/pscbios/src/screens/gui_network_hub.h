//
// GuiNetworkHub: "Network & Controllers" - what the launcher's System menu and Quick menu open PSC-Bios at
// (Extension::runEntry("network"), extension.ini's Provides=network). A compact action menu of the four
// setup screens: the Wi-Fi network (with the time zone), Bluetooth controller pairing, the DualShock 3 page
// and the controller mapping wizard. Circle goes back to the launcher.
//
#pragma once

#include <ableem/ui/gui_base.h>

//********************
// showNetworkHub
//********************
// shows the hub until Circle; each item's screen returns to it
void showNetworkHub(ableem::GuiBase &gui);
