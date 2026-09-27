//
// GuiPadConfig: the pad test and mapping wizard.
//
#include "gui_pad_config.h"
#include "core/game_controller_db.h"
#include "gui/gui.h"
#include "core/services/environment.h"
#include "gui/screens/gui_keyboard.h"

#include <ableem/engine/log.h>

#include <algorithm>
#include <cstdlib>

using namespace std;
using ableem::Color;
using ableem::ControllerState;
using ableem::Joystick;
using ableem::Rect;

//*******************************
// GuiPadConfig::init
//*******************************
void GuiPadConfig::init() {
    padImage = ableem::Texture::loadFile(renderer, Env::getAppDir() + sep + "DS3.png");
    elements = PadMapping::standardElements();
    chooseFont();
    if (Joystick::count() == 0) {
        showPopup(_("NO GAMEPADS CONNECTED"));
    } else {
        openJoystick(0);
    }
}

//*******************************
// GuiPadConfig::chooseFont
//*******************************
// The left column is the facts (4 rows), the message (3 rows wrapped) and, while mapping, the 26 entries in
// two columns of 13 - 20 rows. The classic font at the theme's size (18 at most - a smaller one reads
// better next to the numbers) unless that runs past the footer, then the largest size down to 12 at
// which every row fits the panel's content.
void GuiPadConfig::chooseFont() {
    const ableem::Rect content = gui->classicContent();
    const int themeSize = min(18, static_cast<int>(app.theme().classic().font.size.value));
    pageFont = gui->assets().classicFontAtSize(themeSize);
    for (int size = themeSize; size >= 12; size--) {
        pageFont = gui->assets().classicFontAtSize(size);
        if (PageRows * pageFont.lineHeight() <= content.h - 8)
            break;
    }
}

//*******************************
// GuiPadConfig::openJoystick / nextJoystick / joystickTitle
//*******************************
void GuiPadConfig::openJoystick(int index) {
    joystick.close();
    if (index >= 0 && index < Joystick::count())
        joystick.open(index);
}

void GuiPadConfig::nextJoystick() {
    int count = Joystick::count();
    if (count == 0) {
        joystick.close();
        return;
    }
    openJoystick((joystick.index() + 1) % count);
}

string GuiPadConfig::joystickTitle() const {
    if (!joystick.isOpen())
        return _("NO GAME CONTROLLERS OPENED");
    string name = joystick.name();
    if (joystick.isGameController())
        name += " - SDL2:" + Joystick::controllerNameForIndex(joystick.index());
    return "#" + to_string(joystick.index() + 1) + "/" + to_string(Joystick::count()) + "/ " + name;
}

//*******************************
// GuiPadConfig::startMapping / cancelMapping
//*******************************
void GuiPadConfig::startMapping() {
    joysticksAtStart = Joystick::count();
    originalMapping = joystick.isGameController() ? gui->input().mappingForDeviceIndex(joystick.index()) : "";
    for (PadMapping::Element &element : elements)
        element.value.clear();
    // reopened so that the raw view starts from a clean state, then a snapshot of the pad at rest
    openJoystick(joystick.index());
    joystick.update();
    initialState = joystick.state();
    gui->input().flushEvents();
    stage = Stage::Mapping;
    current = 0;
    pending.clear();
    holdSince = 0;
    stepDeadline = gui->platform().ticks() + PadMapping::StepTimeoutMs;
}

void GuiPadConfig::cancelMapping() {
    pending.clear();
    holdSince = 0;
    for (PadMapping::Element &element : elements)
        element.value.clear();
    if (!originalMapping.empty())
        gui->input().addMapping(originalMapping);
    gui->input().flushEvents();
    stage = Stage::Test;
}

//*******************************
// GuiPadConfig::takeInput
//*******************************
// the input moved (pending) has been let go: it is the element's, and the next one is asked for - taken on
// release, so that Circle held for the hold-to-exit is never mapped, and the next element never sees it
void GuiPadConfig::takeInput(const string &value) {
    elements[current].value = value;
    advance();
}

//*******************************
// GuiPadConfig::circleNow / checkHoldToExit / holdHint / refreshOtherPads
//*******************************
string GuiPadConfig::circleNow() {
    if (!joystick.isOpen())
        return "";
    const string line =
        stage == Stage::Mapping ? originalMapping : gui->input().mappingForDeviceIndex(joystick.index());
    return PadMapping::circleInput(elements, line);
}

// TOOLS-9: keeps `others` (one raw handle + rest sample + hold timer per connected pad OTHER than the one
// being watched) in step with the actual device list - called once a frame, but it only opens/closes
// handles when the set of indices actually changed, so an already-watched pad's rest sample (and its hold
// timer) is never reset just because render() ran again
void GuiPadConfig::refreshOtherPads() {
    const int count = Joystick::count();
    const int mine = joystick.isOpen() ? joystick.index() : -1;
    // drop anything no longer connected, or that is now the pad being watched
    for (auto it = others.begin(); it != others.end();) {
        if (it->first >= count || it->first == mine)
            it = others.erase(it);
        else
            ++it;
    }
    // add anything new
    for (int i = 0; i < count; i++) {
        if (i == mine || others.count(i))
            continue;
        OtherPad pad;
        pad.handle.reset(new ableem::Joystick());
        if (!pad.handle->open(i))
            continue;
        pad.handle->update();
        pad.rest = pad.handle->state();
        others.emplace(i, std::move(pad));
    }
}

bool GuiPadConfig::checkHoldToExit() {
    refreshOtherPads();
    const unsigned int now = gui->platform().ticks();
    bool exit = false;

    // the pad being watched: Circle if this session (or the pad's own line) knows it, else anything held
    bool heldMine = false;
    if (joystick.isOpen()) {
        const string circle = circleNow();
        if (!circle.empty())
            heldMine = PadMapping::inputHeld(circle, initialState, joystick.state());
        else if (stage == Stage::Mapping)
            heldMine = !pending.empty(); // no mapping to name Circle: whatever is being held
    }
    if (PadMapping::advanceHold(holdSince, heldMine, now))
        exit = true;

    // TOOLS-9: every OTHER connected pad gets the same check, each with its own independent timer - a
    // second controller's Circle (or, unmapped, anything held) held 2 s also leaves the wizard
    for (auto &entry : others) {
        OtherPad &pad = entry.second;
        pad.handle->update();
        const string line = gui->input().mappingForDeviceIndex(entry.first);
        const string circle = PadMapping::rawInput(line, "b");
        bool held = !circle.empty() ? PadMapping::inputHeld(circle, pad.rest, pad.handle->state())
                                    : PadMapping::anythingHeld(pad.rest, pad.handle->state());
        if (PadMapping::advanceHold(pad.holdSince, held, now))
            exit = true;
    }
    return exit;
}

string GuiPadConfig::holdHint() {
    if (!joystick.isOpen())
        return "";
    if (!circleNow().empty())
        return "   |@O| " + _("Hold 2 s: Exit");
    return stage == Stage::Mapping ? "   " + _("Hold any button 2 s: Exit") : "";
}

void GuiPadConfig::advance() {
    stepDeadline = gui->platform().ticks() + PadMapping::StepTimeoutMs;
    // the input just taken (or skipped) has settled: whatever the pad reads right now becomes the fresh
    // rest for the NEXT step, instead of the rest sampled once at the very start of the whole session
    // (TOOLS-9: a stale rest, drifted from the button-mapping steps before it, is what made analog mapping
    // read as random - see PadMapping::AxisThreshold's comment)
    if (joystick.isOpen()) {
        joystick.update();
        initialState = joystick.state();
    }
    if (current + 1 < elements.size()) {
        current++;
    } else {
        finishMapping();
    }
}

//*******************************
// GuiPadConfig::finishMapping
//*******************************
// every element asked for: the list is merged, given to SDL for the test stage, and the pad reopened so
// that its controller view uses it
void GuiPadConfig::finishMapping() {
    finals = PadMapping::finalElements(elements, ableem::Platform::osName());
    // TOOLS-9: every element skipped or timed out - nothing to test or save, and no empty/broken mapping is
    // written; back to Test with a message instead
    if (PadMapping::isEmptyMapping(finals)) {
        PLOG_INFO << "Mapping wizard: nothing was mapped, nothing saved";
        cancelMapping();
        showPopup(_("Nothing was mapped. Nothing was saved."));
        return;
    }
    string line = PadMapping::mappingLine(joystick.guid(), PadMapping::cleanName(joystick.name()), finals);
    PLOG_INFO << "New mapping: " << line;
    if (gui->input().addMapping(line))
        openJoystick(joystick.index());
    gui->input().flushEvents();
    stage = Stage::Save;
}

//*******************************
// GuiPadConfig::saveMapping
//*******************************
void GuiPadConfig::saveMapping() {
    GuiKeyboard keyboard(*gui);
    keyboard.result = PadMapping::cleanName(joystick.name());
    keyboard.label = _("Enter name for new gamepad");
    keyboard.show();
    string name = PadMapping::cleanName(keyboard.result);
    if (keyboard.cancelled || name.empty()) {
        cancelMapping();
        return;
    }
    string line = PadMapping::mappingLine(joystick.guid(), name, finals);
    if (gui->input().addMapping(line)) {
        // the file the launcher loads from: the one probePads() found, else the shipped one
        string path = gui->input().currentMappingPath();
        if (path.empty())
            path = Env::getPathToGameControllerDb();
        GameControllerDb db;
        db.load(path);
        db.replaceMapping(joystick.guid(), ableem::Platform::osName(), line);
        if (db.save(path)) {
            PLOG_INFO << "Mapping stored in " << path;
            showPopup(_("Mapping stored to database"));
        } else {
            showPopup(_("Error Storing mapping to database"));
        }
    } else {
        showPopup(_("Error Storing mapping to database"));
    }
    stage = Stage::Test;
}

//*******************************
// GuiPadConfig::showPopup / renderPopup
//*******************************
// TOOLS-9: a small message box in the shared classic look (PanelStyle, as GuiConfirm draws its dialog),
// over the wizard's own already-rendered frame - never Gui::drawText()'s full-screen splash, which replaced
// the whole window and is what the owner meant by "it looks awful". No blocking delay() either: the wizard
// keeps rendering (and handling Quit/other events) while the popup counts down on its own.
void GuiPadConfig::showPopup(const string &message, unsigned int durationMs) {
    popupMessage = message;
    popupUntil = gui->platform().ticks() + durationMs;
}

void GuiPadConfig::renderPopup() {
    if (popupMessage.empty())
        return;
    if (gui->platform().ticks() >= popupUntil) {
        popupMessage.clear();
        return;
    }
    const PanelStyle style = gui->panelStyle();
    style.dim(renderer);
    const int width = 800;
    const ableem::Font &font = gui->assets().themeFonts[FONT_22_MED];
    const int textWidth = width - 2 * (PanelStyle::RowInset + 8);
    const int textHeight = gui->text().wrappedHeight(font, popupMessage, textWidth);
    const int height = PanelStyle::HeaderHeight + 12 + textHeight + 24;
    const ableem::Rect panel((SCREEN_WIDTH - width) / 2, (SCREEN_HEIGHT - height) / 2, width, height);
    style.sheet(renderer, panel);
    const int y = style.header(*gui, panel, _("Gamepad configuration"));
    gui->text().renderWrappedText(font, popupMessage, panel.x + PanelStyle::RowInset + 8, y, textWidth, style.text);
}

//*******************************
// GuiPadConfig::pictureRect
//*******************************
// the DualShock picture (drawn 420x425 in the 2020 tool) at the right of the panel's content, scaled down
// when the content is not that tall; the overlays below are in the picture's own 420x425 coordinates
ableem::Rect GuiPadConfig::pictureRect() const {
    const ableem::Rect content = gui->classicContent();
    int h = min(425, content.h - 8);
    int w = 420 * h / 425;
    return Rect(content.x + content.w - PanelStyle::RowInset - 8 - w, content.y + (content.h - h) / 2, w, h);
}

//*******************************
// GuiPadConfig::renderPadPicture
//*******************************
// the DualShock picture with the pressed buttons and the sticks' positions drawn over it; while mapping,
// the element being asked for is what lights up instead
void GuiPadConfig::renderPadPicture(const ControllerState &liveState) {
    const Rect picture = pictureRect();
    if (padImage.valid())
        renderer.copy(padImage, nullptr, &picture);
    // the overlays were placed on the picture drawn at (430, 200) 420x425
    const float k = static_cast<float>(picture.h) / 425.0f;
    auto at = [&](int x, int y, int w, int h) {
        return Rect(picture.x + static_cast<int>((x - 430) * k), picture.y + static_cast<int>((y - 200) * k),
                    max(1, static_cast<int>(w * k)), max(1, static_cast<int>(h * k)));
    };

    ControllerState state = liveState;
    bool showSticks = true;
    if (stage == Stage::Mapping) {
        renderer.setDrawColor(Color(255, 0, 0, 100));
        state = ControllerState();
        const PadMapping::Element &element = elements[current];
        if (element.button >= 0)
            state.buttons[element.button] = true;
        if (element.axis >= 0)
            state.axes[element.axis] = element.negative ? -32767 : 32767;
        showSticks = element.scan == PadMapping::Scan::Analog;
    } else {
        renderer.setDrawColor(Color(0, 255, 0, 150));
    }
    renderer.setBlendMode(ableem::BlendMode::Blend);

    // where each of SDL's 15 standard buttons is on the picture
    static const int positions[15][4] = {{745, 481, 30, 30}, {777, 449, 30, 30}, {713, 449, 30, 30}, {745, 416, 30, 30},
                                         {586, 454, 32, 21}, {624, 472, 32, 32}, {660, 454, 32, 21}, {548, 492, 61, 61},
                                         {670, 492, 61, 61}, {498, 298, 51, 27}, {730, 298, 51, 27}, {508, 432, 22, 26},
                                         {508, 471, 22, 26}, {486, 452, 26, 23}, {526, 452, 26, 23}};
    for (int i = 0; i < 15; i++)
        if (state.buttons[i])
            renderer.fillRect(at(positions[i][0], positions[i][1], positions[i][2], positions[i][3]));

    if (showSticks) {
        renderer.fillRect(at(571 + state.axes[0] * 20 / 32768, 515 + state.axes[1] * 20 / 32768, 16, 16));
        renderer.fillRect(at(693 + state.axes[2] * 20 / 32768, 515 + state.axes[3] * 20 / 32768, 16, 16));
    }
    // the triggers fill up from the bottom
    int left = state.axes[4] * 47 / 32768;
    renderer.fillRect(at(498, 237 + 46 - left, 51, 1 + left));
    int right = state.axes[5] * 47 / 32768;
    renderer.fillRect(at(730, 237 + 46 - right, 51, 1 + right));
}

//*******************************
// GuiPadConfig::renderElements
//*******************************
// the mapping entries as "name  value" rows in two columns from y; returns the y below them
int GuiPadConfig::renderElements(int x, int y, int width) {
    const vector<PadMapping::Element> &list = stage == Stage::Mapping ? elements : finals;
    const ableem::Color secondary = gui->panelStyle().secondary;
    const ableem::Color text = gui->panelStyle().text;
    const int perColumn = (static_cast<int>(list.size()) + 1) / 2;
    const int columnWidth = (width - 16) / 2;
    const int top = y;
    for (size_t i = 0; i < list.size(); i++) {
        const PadMapping::Element &element = list[i];
        const int column = static_cast<int>(i) / perColumn;
        const int cx = x + column * (columnWidth + 16);
        const int cy = top + (static_cast<int>(i) % perColumn) * pageFont.lineHeight();
        const bool asking = stage == Stage::Mapping && i == current;
        if (asking)
            gui->panelStyle().selection(renderer, Rect(cx - 8, cy, columnWidth + 8, pageFont.lineHeight()));
        gui->text().renderText_WithColor(pageFont, element.apiName, cx, cy, asking ? text : secondary, XALIGN_LEFT);
        gui->text().renderText_WithColor(pageFont, element.value, cx + columnWidth * 6 / 10, cy, text, XALIGN_LEFT);
    }
    return top + perColumn * pageFont.lineHeight();
}

//*******************************
// GuiPadConfig::render
//*******************************
void GuiPadConfig::render() {
    if (stage != Stage::Test && Joystick::count() != joysticksAtStart) {
        cancelMapping();
        showPopup(_("Gamepad configuration changed. Mapping interrupted."));
    }
    joystick.update();

    renderer.clear();
    gui->renderBackground();
    gui->renderTextBar();
    gui->renderHeader(_("Gamepad configuration details"));
    TextRenderer &text = gui->text();
    const PanelStyle style = gui->panelStyle();
    const ableem::Rect content = gui->classicContent();
    const Rect picture = pictureRect();
    const int x = content.x + PanelStyle::RowInset + 8;
    const int width = picture.x - 24 - x;
    const int lineHeight = pageFont.lineHeight();
    int y = content.y + 4;
    auto row = [&](const string &line, const ableem::Color &color) {
        text.renderText_WithColor(pageFont, text.elide(pageFont, line, width), x, y, color, XALIGN_LEFT);
        y += lineHeight;
    };

    if (stage == Stage::Mapping) {
        if (pending.empty()) {
            pending = PadMapping::detectChange(initialState, joystick.state(), elements);
            if (!pending.empty())
                app.audio().cursor.play();
        } else if (!PadMapping::anythingHeld(initialState, joystick.state())) {
            const string taken = pending;
            pending.clear();
            takeInput(taken);
        }
        // TOOLS-9: every step is skippable (the console's own Open button, always reachable regardless of
        // what the pad under test has) OR times out - a pad missing a button/stick must still reach the
        // end. A candidate already seen (pending) is taken as-is rather than discarded, so a reading that
        // never fully "lets go" cannot hang the wizard either.
        if (stage == Stage::Mapping && gui->platform().ticks() >= stepDeadline) {
            if (!pending.empty()) {
                const string taken = pending;
                pending.clear();
                takeInput(taken);
            } else {
                elements[current].value.clear();
                advance();
            }
        }
    }
    if (checkHoldToExit()) {
        PLOG_INFO << "Circle held: leaving the mapping wizard";
        app.audio().cancel.play();
        if (stage != Stage::Test)
            cancelMapping();
        menuVisible = false;
        return;
    }

    // the facts: the pad, its inputs, the raw buttons and hats, the axes eight to a row
    const ableem::JoystickState &state = joystick.state();
    // style.text, not style.secondary, for every row below that carries a translated label: the label can
    // carry a diacritic (Finnish "syötteet", "Painikkeet", "Ristiohjaimet", German umlauts, ...) and its
    // mark is only 1-2px at this panel's font sizes - legible in the bright colour, but the dim grey
    // (PanelStyle::secondary, ~100,100,100) was too low-contrast to show it at all next to the panel
    // background (TOOLS-7: "Ohjaimen syötteet:" read as "Ohjaimen syotteet:"). Only a row that is purely
    // untranslated data - the axes dump below ("#1: 000 ..."), and renderElements()'s apiName column (raw
    // SDL tokens like "a"/"dpup"/"lefttrigger", never translated) - stays dim.
    row(joystickTitle(), style.text);
    row(_("Gamepad input configuration:") + " A:" + to_string(state.axes.size()) +
            "  B:" + to_string(state.buttons.size()) + " D:" + to_string(state.hats.size()),
        style.text);
    string buttons = _("Buttons:") + " ";
    for (bool pressed : state.buttons)
        buttons += string(pressed ? "1" : "0") + " ";
    buttons += " " + _("Hats:") + " ";
    for (unsigned hat : state.hats)
        buttons += to_string(hat) + " ";
    row(buttons, style.text);
    string axes;
    for (size_t i = 0; i < state.axes.size(); i++) {
        int percent = state.axes[i] * 100 / 32767;
        string padded = to_string(abs(percent));
        padded.insert(0, 3 - min<size_t>(3, padded.size()), '0');
        axes += string(i < 9 ? " " : "") + "#" + to_string(i + 1) + ":" + (percent < 0 ? "-" : " ") + padded + " ";
        if ((i + 1) % 8 == 0 || i + 1 == state.axes.size()) {
            row(axes, style.secondary);
            axes.clear();
        }
    }

    // the message for the stage, wrapped to the column
    y += lineHeight / 2;
    string first, second;
    switch (stage) {
    case Stage::Test:
        first = _("NOTE: Make sure none of the buttons are pressed before mapping and all analog sticks are in default "
                  "position.");
        second = _("You can test your controller");
        break;
    case Stage::Mapping:
        switch (elements[current].scan) {
        case PadMapping::Scan::Digital:
            first = _("Press a button highlighted or (OPEN) if not avaliable.");
            break;
        case PadMapping::Scan::Trigger:
            first = _("Press a trigger highlighted fully or (OPEN) if not avaliable.");
            break;
        case PadMapping::Scan::Analog:
            first = _("Move your sticks to state shown or press (OPEN) if stick position not avaliable.");
            break;
        }
        second = _("Updating mapping");
        {
            // TOOLS-9: a visible countdown for the step's timeout, next to the existing skip hint below
            const unsigned int now = gui->platform().ticks();
            const unsigned int left = stepDeadline > now ? stepDeadline - now : 0;
            const int secondsLeft = static_cast<int>((left + 999) / 1000);
            second += "   " + _("Skip in") + " " + to_string(secondsLeft) + "s";
        }
        break;
    case Stage::Save:
        first = _("Mapping Complete - press (OPEN) to save. (POWER) to cancel.");
        second = _("Please test a new mapping");
        break;
    }
    // both lines are translated labels (never raw data), so both use style.text - the same reasoning as
    // the rows above.
    y += max(lineHeight, text.renderWrappedText(pageFont, first, x, y, width, style.text));
    y += max(lineHeight, text.renderWrappedText(pageFont, second, x, y, width, style.text));
    y += lineHeight / 2;

    if (stage != Stage::Test)
        renderElements(x, y, width);

    renderPadPicture(joystick.controllerState());

    // while Circle is held, how far the hold-to-exit has come: a bar across the foot of the content
    if (holdSince != 0) {
        const unsigned int held = min(+PadMapping::HoldToExitMs, gui->platform().ticks() - holdSince);
        const Rect track(content.x + PanelStyle::RowInset, content.y + content.h - 6,
                         content.w - 2 * PanelStyle::RowInset, 4);
        renderer.setBlendMode(ableem::BlendMode::Blend);
        renderer.setDrawColor(Color(style.text.r, style.text.g, style.text.b, 60));
        renderer.fillRect(track);
        renderer.setDrawColor(Color(style.text.r, style.text.g, style.text.b, 220));
        renderer.fillRect(Rect(track.x, track.y, static_cast<int>(track.w * held / PadMapping::HoldToExitMs), track.h));
    }

    // the console's front buttons, as chips
    if (stage == Stage::Test)
        gui->renderStatus("|@Reset| " + _("Next pad") + "   |@Open| " + _("Update mapping") + "   |@Power| " +
                          _("Exit") + holdHint());
    else if (stage == Stage::Mapping)
        gui->renderStatus("|@Open| " + _("Skip / No button on controller") + "   |@Power| " + _("Cancel mapping") +
                          holdHint());
    else
        gui->renderStatus("|@Open| " + _("Save") + "   |@Power| " + _("Cancel mapping") + holdHint());
    renderPopup();
    renderer.present();
}

//*******************************
// GuiPadConfig::loop
//*******************************
// the console's front buttons drive this screen (the pad under test is not to be trusted): Power, Reset
// and Open; on a keyboard Escape, Start (Space) and Return do the same. Input hands the power button over
// as a key for the duration instead of powering off.
//
// keyboardAsPad is off here, the way GuiKeyboard turns it off: the screen's own job is reading a *pad*
// raw, so a keyboard's Esc/Backspace must read as keys, never remapped into the very Button::Circle event
// a real pad also sends (Test stage: the wizard opens it as a GameController too, for the picture, and SDL
// keeps delivering its events even though Input let its own pads go - see PadMapping::isExitKey). Without
// this, a short press of a real pad's Circle looked exactly like a keyboard's Esc and closed the wizard at
// once, instead of only the 2 s hold doing that (checkHoldToExit, below).
void GuiPadConfig::loop() {
    menuVisible = true;
    ableem::Input &input = gui->input();
    const bool keyboardAsPad = input.keyboardAsPad();
    const bool rawKeyboard = input.rawKeyboard();
    input.setKeyboardAsPad(false);
    input.setRawKeyboard(true);
    input.setPowerKeyAsKey(true);
    while (menuVisible) {
        render();
        if (!menuVisible)
            break; // Circle held: render() closed the screen
        Event e;
        while (gui->input().poll(e)) {
            if (e.type == Event::Type::Quit)
                menuVisible = false;
            // Power, or a keyboard's Esc/Backspace (isExitKey) - never a pad's own Circle: that only
            // leaves by the 2 s hold, above
            bool power = e.type == Event::Type::KeyDown && PadMapping::isExitKey(e.key);
            bool reset = e.type == Event::Type::KeyDown && e.key == Key::Reset;
#ifdef AB_DEBUG_HOST
            // a dev host's keyboard stands in for Reset through keyboard-as-pad (Space = Start). Never on a
            // device: there a Start (or a Select arriving as one) is the pad under test, and it switched the
            // screen to the next pad in the middle of a DS4 test (2026-09-26)
            reset = reset || (e.type == Event::Type::ButtonDown && e.button == Button::Start);
#endif
            bool open = e.type == Event::Type::KeyDown && (e.key == Key::Open || e.key == Key::Return);
            if (power) {
                app.audio().cursor.play();
                if (stage == Stage::Test)
                    menuVisible = false;
                else
                    cancelMapping();
            } else if (reset) {
                if (stage == Stage::Test && Joystick::count() > 0) {
                    app.audio().cursor.play();
                    nextJoystick();
                }
            } else if (open) {
                switch (stage) {
                case Stage::Test:
                    if (Joystick::count() > 0) {
                        app.audio().cursor.play();
                        startMapping();
                    }
                    break;
                case Stage::Mapping:
                    app.audio().cursor.play();
                    elements[current].value.clear(); // no such input on this pad
                    advance();
                    break;
                case Stage::Save:
                    app.audio().cursor.play();
                    saveMapping();
                    break;
                }
            } else if (e.type == Event::Type::PadAdded || e.type == Event::Type::PadRemoved) {
                if (stage == Stage::Test) {
                    showPopup(_("Gamepad configuration changed."));
                    if (!joystick.isOpen() || joystick.index() >= Joystick::count())
                        openJoystick(0);
                }
            }
        }
    }
    input.setPowerKeyAsKey(false);
    input.setKeyboardAsPad(keyboardAsPad);
    input.setRawKeyboard(rawKeyboard);
    joystick.close();
}
