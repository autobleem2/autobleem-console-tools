//
// GuiPadConfig: the pad test and mapping wizard. Shows one joystick raw (its axes, buttons and hats as
// numbers, and a DualShock picture lit by the mapped view). Everything is done from the pad itself - a Pi
// has no front buttons (the console's and a keyboard's still work, as shortcuts):
// - Test: a button pressed on another pad shows that pad; Cross held 2 s starts mapping (a pad SDL has no
//   mapping for starts by itself after a short countdown).
// - Mapping: each of the 25 standard inputs is highlighted on the picture in turn and the raw input the user
//   moves is taken for it once let go and settled; a step skips itself after PadMapping::StepTimeoutMs
//   (the countdown is a popup at the top), and once Circle is mapped a press of it skips at once.
// - Save: the mapping is added to SDL for a test; Cross names the pad and writes it to the
//   gamecontrollerdb.txt in use, Circle cancels.
// Holding Circle for PadMapping::HoldToExitMs leaves the screen from any stage. Circle and Cross are what
// this session mapped them to, else the pad's own mapping's "b"/"a"; a pad with no mapping leaves by any
// button held that long. Open (Return) = Cross, Power (Escape) = cancel/leave, Reset = next pad.
//
#pragma once

#include "gui/gui_screen.h"
#include "core/pad_mapping.h"

#include <ableem/ui/joystick.h>
#include <ableem/ui/texture.h>
#include <ableem/ui/types.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

//********************
// GuiPadConfig
//********************
class GuiPadConfig : public GuiScreen {
public:
    using GuiScreen::GuiScreen;

    void init() override;
    // before each frame: the pad read, the wizard's step, the pad's own controls; false once a 2 s hold left
    bool prepareFrame() override;
    void draw() override; // the frame's picture; the screen stack clears and presents
    void loop() override;

private:
    enum class Stage { Test, Mapping, Save };
    Stage stage = Stage::Test;

    ableem::Joystick joystick;
    ableem::JoystickState initialState; // the pad at rest, resampled fresh for every step (see advance())
    std::vector<PadMapping::Element> elements;
    std::vector<PadMapping::Element> finals;
    size_t current = 0;                         // the element being asked for
    std::string originalMapping;                // SDL's mapping before the wizard, put back on cancel
    int joysticksAtStart = 0;                   // a pad plugged or pulled mid-mapping cancels it
    std::string pending;                        // the input just moved while mapping, taken when it is let go
    unsigned int holdSince = 0;                 // when Circle (see the top) went down and stayed down, 0: it is not
    unsigned int stepDeadline = 0;              // ticks() when the current step is skipped/taken even if unresolved
    unsigned int releasedSince = 0;             // when `pending` was let go, 0: still held (see PadMapping::SettleMs)
    bool circleWasHeld = false;                 // this session's Circle was down last frame (a release skips/cancels)
    bool crossWasHeld = false;                  // the same for Cross (a release saves)
    unsigned int crossSince = 0;                // Test: when Cross went down and stayed down (held 2 s: map the pad)
    bool crossArmed = false;                    // Test: Cross was held 2 s - mapping starts when it is let go
    unsigned int autoMapAt = 0;                 // Test: ticks() when a pad with no mapping starts mapping, 0: never
    static constexpr unsigned AutoMapDelayMs = 3000;
    ableem::Texture padImage;                   // DS3.png, from the tool's own folder
    ableem::Font pageFont;                      // the classic font at a size every row of this page fits at (see init)
    static constexpr int PageRows = 4 + 3 + 13; // the facts, the message, the 26 mapping entries in two columns

    // TOOLS-9: a small popup drawn over the wizard's own already-rendered frame (PanelStyle, the same look
    // GuiConfirm uses) instead of Gui::drawText()'s full-screen splash - see showPopup()/renderPopup()
    std::string popupMessage;
    unsigned int popupUntil = 0; // ticks() when the popup clears itself

    // TOOLS-9: the 2 s hold-to-exit must work from ANY connected pad, not only the one being mapped - one
    // raw handle + rest sample + timer per other connected joystick index, rebuilt only when the set of
    // connected pads changes (never every frame: that would keep resetting the rest sample and a hold could
    // never be measured)
    struct OtherPad {
        std::unique_ptr<ableem::Joystick> handle;
        ableem::JoystickState rest;
        unsigned int holdSince = 0;
    };
    std::map<int, OtherPad> others;

    void openJoystick(int index);
    void nextJoystick();
    void startMapping();
    void cancelMapping();
    void takeInput(const std::string &value);
    std::string circleNow(); // the raw input that is Circle on this pad now, "" when not known
    std::string elementNow(const std::string &apiName); // the same for any element ("a": Cross)
    // true once, on the frame `raw` is let go after being held (`wasHeld` carries the last frame)
    bool releasedNow(const std::string &raw, bool &wasHeld);
    void padControls();       // the pad's own Cross/Circle for each stage - no front buttons needed
    void switchToPressedPad(); // Test: a button pressed on another pad shows that pad
    void armAutoMap();         // Test: a pad SDL has no mapping for starts mapping by itself shortly
    void renderTopPopup(const std::string &message); // a one-line popup at the top, over the frame
    bool checkHoldToExit();  // true when Circle has been held long enough on ANY connected pad
    std::string holdHint();  // the footer's hold-to-exit hint, "" when there is no Circle to hold
    void refreshOtherPads(); // keeps `others` in step with Joystick::count() and the pad being watched
    void advance();
    void finishMapping();
    void saveMapping();
    void showPopup(const std::string &message, unsigned int durationMs = 2000);
    void renderPopup();

    ableem::Rect pictureRect() const;
    void renderPadPicture(const ableem::ControllerState &state);
    int renderElements(int x, int y, int width);
    void chooseFont();
    std::string joystickTitle() const;
};
