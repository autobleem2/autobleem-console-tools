//
// PSC-Bios as an extension of the launcher (the launcher's docs/extensions-plan.md): the console's hardware
// configuration - WiFi, timezone, the pads - shown from the System menu's Hardware Information item, or from
// the Extensions list. It ships with the console package in Extensions/pscbios/ (the owner, 2026-09-24; until
// then it was an App, a program of its own in Apps/pscbios/). Nothing runs in the background.
//
#include "pscbios.h"
#include "screens/gui_pscbios_main.h"
#ifdef PSCBIOS_NATIVE_BACKEND
#include "core/native_backend.h"
#endif

#include "core/main.h"
#include "core/services/environment.h"
#include "gui/extension.h"
#include "gui/gui.h"

#include <memory>

using namespace std;

//******************
// PscBiosExtension
//******************
class PscBiosExtension : public Extension {
public:
    explicit PscBiosExtension(ExtensionHost &host) : host(host) {}

    void run() override {
        // the tool's own files (DS3.png, ssid.cfg off the console) are found through
        // Env::getAppDir(), as they were when it was a program started in its folder: that is the extension's
        // folder while it runs, and whatever it was before afterwards
        const string previousAppDir = Env::getAppDir();
        Env::setAppDir(host.folder());

        // the console asked directly (sysfs, wpa_supplicant's socket, BlueZ over D-Bus, settime) - no shell
        // scripts; a fake with canned answers on a dev host (and wherever the native backend is not built)
#if defined(PSCBIOS_NATIVE_BACKEND) && !defined(AB_DEBUG_HOST)
        unique_ptr<ConsoleBackend> console = make_unique<NativeBackend>();
        if (!console->kernelInstalled()) {
            PLOG_WARNING << "No AutoBleem kernel (" << NativePaths().kernelMarker
                         << " missing): the network rows are off";
        }
#else
        unique_ptr<ConsoleBackend> console = make_unique<FakeBackend>();
#endif
        PLOG_INFO << "PSC-Bios, folder " << host.folder();
        {
            PscBios tool(std::move(console));
            GuiPscBiosMain mainScreen(*Gui::getInstance());
            mainScreen.show();
        }
        Env::setAppDir(previousAppDir);
    }

private:
    ExtensionHost &host;
};

AB_EXTENSION(PscBiosExtension)
