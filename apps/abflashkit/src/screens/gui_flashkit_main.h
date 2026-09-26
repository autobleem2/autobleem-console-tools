//
// GuiFlashKitMain: ABFlashKit's one screen - a GuiActionMenu of the four actions and About (Flash kernel, Full
// backup, Restore mode, Back up games; Circle quits) over the theme's background, and the FlashUi the
// actions report through while they run: every status line is the spinner's message over the dimmed menu,
// with the progress bar under it while a step can measure itself, a question is a confirm dialog, a pause
// or a step run in the background keeps the spinner turning.
//
#pragma once

#include "gui/gui_screen.h"
#include "gui/screens/gui_action_menu.h"
#include "core/flash_actions.h"

#include <functional>

//********************
// GuiFlashKitMain
//********************
class GuiFlashKitMain : public GuiScreen, public FlashUi {
public:
    explicit GuiFlashKitMain(ableem::GuiBase &_gui) : GuiScreen(_gui), menu(_gui) {}

    void init() override;
    void render() override;
    void loop() override;

    // FlashUi
    void status(const std::string &text) override;
    bool confirm(const std::string &question) override;
    void wait(int ms) override;
    void progress(int done, int total) override;
    // the job on a thread while this one keeps the spinner turning
    void runInBackground(const std::function<void()> &job) override;

private:
    GuiActionMenu menu;
    bool busy = false;                       // the spinner is up (an action is running)
    int progressDone = 0, progressTotal = 0; // the bar under the spinner, kept across the status lines
    unsigned int lastProgressFrame = 0;      // when a progress step was last drawn
    void run(FlashKitActions::Outcome (FlashKitActions::*action)());
    void showAbout(); // GuiAbout with ABFlashKit's credits
    std::string lastStatus;
};
