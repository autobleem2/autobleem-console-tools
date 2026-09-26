//
// GuiPadConfig: the pad test and mapping wizard. Shows one joystick raw (its axes, buttons and hats as
// numbers, and a DualShock picture lit by the mapped view); Reset (or Start on a keyboard) moves to the
// next pad, Open (or Return) starts mapping: each of the 25 standard inputs is highlighted on the picture
// in turn and the raw input the user moves is taken for it (Open skips one). At the end the mapping is
// added to SDL for a test, and Open once more names the pad and writes it to the gamecontrollerdb.txt in
// use. Power (or Escape) cancels a mapping, or closes the screen - and so does holding the pad's own Circle
// for PadMapping::HoldToExitMs (a short press of it is mapped as usual): the way out for a player with no
// front buttons and no keyboard. Circle is the pad's mapping's "b" (or what this session mapped it to);
// while mapping a pad SDL has no mapping for, any input held that long leaves.
//
#pragma once

#include "gui/gui_screen.h"
#include "core/pad_mapping.h"

#include <ableem/ui/joystick.h>
#include <ableem/ui/texture.h>
#include <ableem/ui/types.h>

#include <string>
#include <vector>

//********************
// GuiPadConfig
//********************
class GuiPadConfig : public GuiScreen {
public:
    using GuiScreen::GuiScreen;

    void init() override;
    void render() override;
    void loop() override;

private:
    enum class Stage { Test, Mapping, Save };
    Stage stage = Stage::Test;

    ableem::Joystick joystick;
    ableem::JoystickState initialState; // the pad at rest, when the mapping started
    std::vector<PadMapping::Element> elements;
    std::vector<PadMapping::Element> finals;
    size_t current = 0;                         // the element being asked for
    std::string originalMapping;                // SDL's mapping before the wizard, put back on cancel
    int joysticksAtStart = 0;                   // a pad plugged or pulled mid-mapping cancels it
    std::string pending;                        // the input just moved while mapping, taken when it is let go
    unsigned int holdSince = 0;                 // when Circle (see the top) went down and stayed down, 0: it is not
    ableem::Texture padImage;                   // DS3.png, from the tool's own folder
    ableem::Font pageFont;                      // the classic font at a size every row of this page fits at (see init)
    static constexpr int PageRows = 4 + 3 + 13; // the facts, the message, the 26 mapping entries in two columns

    void openJoystick(int index);
    void nextJoystick();
    void startMapping();
    void cancelMapping();
    void takeInput(const std::string &value);
    std::string circleNow(); // the raw input that is Circle on this pad now, "" when not known
    bool checkHoldToExit();  // true when Circle has been held long enough: the screen is to close
    std::string holdHint();  // the footer's hold-to-exit hint, "" when there is no Circle to hold
    void advance();
    void finishMapping();
    void saveMapping();

    ableem::Rect pictureRect() const;
    void renderPadPicture(const ableem::ControllerState &state);
    int renderElements(int x, int y, int width);
    void chooseFont();
    std::string joystickTitle() const;
};
