//
// GuiFlashKitMain: the one screen.
//
#include "gui_flashkit_main.h"
#include "../abflashkit_app.h"
#include "core/version.h"
#include "gui/gui.h"
#include "gui/screens/gui_confirm.h"

#include <ableem/engine/log.h>

#include <atomic>
#include <thread>

using namespace std;

namespace {
enum Action { Flash = 0, FullBackup, Restore, BackupGames };
}

//*******************************
// GuiFlashKitMain::init
//*******************************
void GuiFlashKitMain::init() {
    menu.title = "ABFlashKit";
    menu.subtitle = Version::VERSION;
    menu.circleLabel = _("Quit");
    menu.items = {
        {_("Flash Kernel"), _("Back the console up to the stick, install the AutoBleem kernel and reboot")},
        {_("Full backup"), _("All four partitions to LBOOT.EPB on the stick, for a restore later")},
        {_("Restore Mode"), _("Reboot into Sony's recovery, which restores the console from LBOOT.EPB")},
        {_("Back up games"), _("Copy the console's built-in games to the Games Backup folder on the stick")},
    };
}

//*******************************
// GuiFlashKitMain::render
//*******************************
void GuiFlashKitMain::render() {
    menu.render();
}

//*******************************
// the FlashUi: every step as the spinner's message over the menu, a confirm dialog, a pause
//*******************************
void GuiFlashKitMain::status(const string &text) {
    PLOG_INFO << text;
    lastStatus = text;
    if (busy)
        gui->endBusy();
    gui->beginBusy(text, [this]() { render(); });
    busy = true;
    if (progressTotal > 0)
        gui->setBusyProgress(progressDone, progressTotal); // the bar stays through the messages
    gui->busyTick();
}

bool GuiFlashKitMain::confirm(const string &question) {
    if (busy) {
        gui->endBusy();
        busy = false;
    }
    GuiConfirm dialog(*gui);
    dialog.label = question;
    dialog.show();
    if (!lastStatus.empty())
        status(lastStatus); // the spinner comes back with the step in progress
    return dialog.result;
}

void GuiFlashKitMain::progress(int done, int total) {
    const bool shownOrHidden = total != progressTotal || done == 0 || done == total;
    progressDone = done;
    progressTotal = total;
    if (!busy)
        return;
    // a frame waits for the display, so drawing every step of a 1000-step bar would take longer than the
    // work it shows: a step is drawn when the bar appears, fills or goes, otherwise at most every 40 ms
    const unsigned int now = gui->platform().ticks();
    if (!shownOrHidden && now - lastProgressFrame < 40)
        return;
    lastProgressFrame = now;
    gui->setBusyProgress(done, total);
    gui->busyTick();
}

void GuiFlashKitMain::runInBackground(const function<void()> &job) {
    // the job on a thread, the spinner on this one: nothing but the job touches what it touches, and this
    // loop only draws until it is done
    atomic<bool> finished(false);
    thread worker([&job, &finished] {
        job();
        finished = true;
    });
    while (!finished) {
        if (busy)
            gui->busyTick();
        gui->platform().delay(20);
    }
    worker.join();
}

void GuiFlashKitMain::wait(int ms) {
    const unsigned int until = gui->platform().ticks() + ms;
    while (gui->platform().ticks() < until) {
        if (busy)
            gui->busyTick();
        gui->platform().delay(20);
    }
}

//*******************************
// GuiFlashKitMain::run
//*******************************
void GuiFlashKitMain::run(FlashKitActions::Outcome (FlashKitActions::*action)()) {
    AbFlashKit &tool = AbFlashKit::get();
    FlashKitActions actions(tool.flasher(), tool.led(), *this, tool.paths());
    lastStatus.clear();
    progressDone = progressTotal = 0;
    FlashKitActions::Outcome outcome = (actions.*action)();
    PLOG_INFO << "Action " << FlashKitActions::name(outcome);
    // the last status stays readable for a moment before the menu is back
    wait(1500);
    if (busy) {
        gui->endBusy();
        busy = false;
    }
    // a reboot has been issued on the console; on a dev host there is nothing to reboot, the tool closes
    if (outcome == FlashKitActions::Outcome::Rebooting)
        menuVisible = false;
    gui->input().flushEvents();
}

//*******************************
// GuiFlashKitMain::loop
//*******************************
void GuiFlashKitMain::loop() {
    menuVisible = true;
    AbFlashKit::get().led().setMode(LedMode::Green);
    while (menuVisible) {
        menu.show();
        switch (menu.result) {
        case Flash:
            run(&FlashKitActions::flash);
            break;
        case FullBackup:
            run(&FlashKitActions::fullBackup);
            break;
        case Restore:
            run(&FlashKitActions::restore);
            break;
        case BackupGames:
            run(&FlashKitActions::backupGames);
            break;
        default:
            menuVisible = false; // Circle, or the window closed
            break;
        }
    }
    AbFlashKit::get().led().setMode(LedMode::Off);
}
