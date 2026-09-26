//
// NetworkStatus: what the main screen shows about the network - the WiFi and ethernet dongles, their
// addresses, the Bluetooth dongle, the timezone - refreshed from the ConsoleBackend every so often (sysfs, a
// getifaddrs and a D-Bus round trip to BlueZ each time, so not every frame). The WiFi and ethernet dongles are
// the backend's wifiInterface()/ethernetInterface() - whatever the kernel named them.
//
#pragma once

#include <string>

class ConsoleBackend;

//******************
// NetworkStatus
//******************
struct NetworkStatus {
    std::string wirelessIface; // "" without a WiFi dongle
    bool wirelessFound = false;
    bool wirelessActive = false;
    std::string wirelessAddr;
    std::string ethIface;
    bool ethFound = false;
    bool ethActive = false;
    std::string ethAddr;
    bool btActive = false;
    std::string btName;
    std::string btError; // why there is no Bluetooth (no adapter, no system bus, BlueZ not answering); "" when up
    std::string timezone;

    void refresh(ConsoleBackend &backend);

    // "<Found|Not found>/<Active|Not active>", translated - the dongle line of the main screen
    static std::string dongleStatus(bool found, bool active);
    // the address, or the translated "Waiting for IP Address..." when there is none yet, "-" for no dongle
    static std::string addressText(bool found, const std::string &address);
    // the Bluetooth dongle line: Found/Active when up, Not found/Not active without an adapter, else
    // "Unavailable" (the reason is the details row - btDetails())
    std::string btStatusText() const;
    std::string btDetails() const { return btActive ? btName : btError; }
};
