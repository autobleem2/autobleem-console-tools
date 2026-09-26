//
// PSC-Bios as an extension of the launcher (the launcher's docs/extensions-plan.md): the hardware
// configuration - WiFi, timezone, Bluetooth, the pads. The Extensions list runs it (run(): its facts page);
// the System menu's and the Quick menu's Network & Controllers item opens it at its hub (runEntry("network"),
// extension.ini's Provides=network, SDK ABI 4). It ships with the console package in Extensions/pscbios/ (the
// owner, 2026-09-24; until then it was an App, a program of its own in Apps/pscbios/) and, since 2026-09-26,
// with the Pi and PC-stick packages. Nothing runs in the background.
//
#include "pscbios.h"
#include "core/nm_backend.h"
#include "screens/gui_network_hub.h"
#include "screens/gui_pscbios_main.h"

#include "core/main.h"
#include "core/services/environment.h"
#include "gui/extension.h"
#include "gui/gui.h"

#include <functional>
#include <memory>

using namespace std;

namespace {
// the platform's backend: a fake with canned answers on a dev host, the kernel's abnet/settime scripts on the
// console, NetworkManager + BlueZ on a Pi and the PC stick
unique_ptr<ConsoleBackend> makeBackend() {
#if defined(AB_DEBUG_HOST)
    return make_unique<FakeBackend>();
#elif defined(AB_PLATFORM_PSC)
    unique_ptr<ConsoleBackend> console = make_unique<AbnetBackend>();
    if (!console->kernelInstalled())
        PLOG_WARNING << "No AutoBleem kernel (" << AbnetBackend::Abnet << " missing): the network rows are off";
    return console;
#else
    unique_ptr<ConsoleBackend> console = make_unique<NmBackend>();
    if (!console->kernelInstalled())
        PLOG_WARNING << "No NetworkManager (nmcli missing): the network rows are off";
    return console;
#endif
}
} // namespace

//******************
// PscBiosExtension
//******************
class PscBiosExtension : public Extension {
public:
    explicit PscBiosExtension(ExtensionHost &host) : host(host) {}

    void run() override {
        withTool([]() {
            GuiPscBiosMain mainScreen(*Gui::getInstance());
            mainScreen.show();
        });
    }

    bool runEntry(const string &entry) override {
        if (entry != "network")
            return false;
        withTool([]() { showNetworkHub(*Gui::getInstance()); });
        return true;
    }

private:
    // the tool's own files (DS3.png, the bt helper, ssid.cfg off the console) are found through
    // Env::getAppDir(), as they were when it was a program started in its folder: that is the extension's
    // folder while it runs, and whatever it was before afterwards
    void withTool(const function<void()> &screens) {
        const string previousAppDir = Env::getAppDir();
        Env::setAppDir(host.folder());
        PLOG_INFO << "PSC-Bios, folder " << host.folder();
        {
            PscBios tool(makeBackend());
            screens();
        }
        Env::setAppDir(previousAppDir);
    }

    ExtensionHost &host;
};

AB_EXTENSION(PscBiosExtension)
