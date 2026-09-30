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
    holdSince = 0;
    crossSince = 0;
    crossArmed = false;
    circleWasHeld = crossWasHeld = false;
    if (joystick.isOpen()) {
        joystick.update();
        initialState = joystick.state();
    }
    if (stage == Stage::Test)
        armAutoMap();
}

void GuiPadConfig::armAutoMap() {
    autoMapAt = joystick.isOpen() && !joystick.isGameController() ? gui->platform().ticks() + AutoMapDelayMs : 0;
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
    releasedSince = 0;
    autoMapAt = 0;
    holdSince = 0;
    crossSince = 0;
    crossArmed = false;
    circleWasHeld = crossWasHeld = false;
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
    circleWasHeld = crossWasHeld = false;
    crossSince = 0;
    crossArmed = false;
    armAutoMap();
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
    return elementNow("b");
}

string GuiPadConfig::elementNow(const string &apiName) {
    if (!joystick.isOpen())
        return "";
    const string line =
        stage == Stage::Mapping ? originalMapping : gui->input().mappingForDeviceIndex(joystick.index());
    return PadMapping::elementInput(elements, line, apiName);
}

bool GuiPadConfig::releasedNow(const string &raw, bool &wasHeld) {
    const bool held = !raw.empty() && PadMapping::inputHeld(raw, initialState, joystick.state());
    const bool released = wasHeld && !held;
    wasHeld = held;
    return released;
}

//*******************************
// GuiPadConfig::padControls / switchToPressedPad
//*******************************
// what the pad under test does besides being tested or mapped - so that neither a Pi (no front buttons) nor
// a console without a keyboard needs anything but the pad. Presses are taken on release: Circle held for
// the 2 s exit must never also skip or cancel, and Cross held into the next stage must not carry over.
void GuiPadConfig::padControls() {
    const unsigned int now = gui->platform().ticks();
    switch (stage) {
    case Stage::Test: {
        if (autoMapAt != 0) {
            // an unmapped pad in use (the user holding a button to leave) holds the countdown back
            if (PadMapping::anyPressed(joystick.state()))
                autoMapAt = now + AutoMapDelayMs;
            else if (now >= autoMapAt) {
                app.audio().cursor.play();
                startMapping();
            }
            break;
        }
        const string cross = elementNow("a");
        const bool held = !cross.empty() && PadMapping::inputHeld(cross, initialState, joystick.state());
        // held 2 s arms it; mapping starts once Cross is let go - started while still held, the pad's rest
        // would be sampled with Cross down, and its release would read as an input that never lets go
        if (crossArmed) {
            if (!held) {
                crossArmed = false;
                startMapping();
            }
        } else if (PadMapping::advanceHold(crossSince, held, now)) {
            crossSince = 0;
            crossArmed = true;
            app.audio().cursor.play();
        }
        break;
    }
    case Stage::Mapping: {
        // Circle skips a step once this session has mapped it - never the pad's old mapping, which may be
        // the very thing being fixed; before that the step's timeout is the only skip
        string circle;
        for (const PadMapping::Element &e : elements)
            if (e.apiName == "b")
                circle = e.value;
        if (releasedNow(circle, circleWasHeld)) {
            app.audio().cursor.play();
            elements[current].value.clear();
            advance();
        }
        break;
    }
    case Stage::Save:
        if (releasedNow(elementNow("b"), circleWasHeld)) {
            app.audio().cancel.play();
            cancelMapping();
        } else if (releasedNow(elementNow("a"), crossWasHeld)) {
            app.audio().cursor.play();
            saveMapping();
        }
        break;
    }
}

void GuiPadConfig::switchToPressedPad() {
    if (stage != Stage::Test)
        return;
    for (auto &entry : others) {
        const ableem::JoystickState &state = entry.second.handle->state();
        const ableem::JoystickState &rest = entry.second.rest;
        size_t buttons = min(state.buttons.size(), rest.buttons.size());
        for (size_t i = 0; i < buttons; i++)
            if (state.buttons[i] && !rest.buttons[i]) {
                const int index = entry.first;
                app.audio().cursor.play();
                openJoystick(index); // `others` is rebuilt around it on the next frame
                return;
            }
    }
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
        else
            heldMine = PadMapping::anyPressed(joystick.state()); // an unmapped pad: any button held
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
    pending.clear();
    releasedSince = 0;
    // a stick step on a pad with no axis left to give (no sticks, or only a d-pad reported as axes - the
    // PSC's own controller) is skipped at once instead of waiting out its timeout; finalElements() then
    // drives the left stick from the d-pad
    size_t next = current + 1;
    while (next < elements.size() && elements[next].scan == PadMapping::Scan::Analog &&
           !PadMapping::hasFreeAxis(elements, initialState.axes.size()))
        elements[next++].value.clear();
    if (next < elements.size()) {
        current = next;
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
    circleWasHeld = crossWasHeld = false;
    crossSince = 0;
    crossArmed = false;
    autoMapAt = 0;
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

// one line on a small sheet at the top of the screen, over the header - the screen under it stays
// readable (no dim), it is only a countdown
void GuiPadConfig::renderTopPopup(const string &message) {
    const PanelStyle style = gui->panelStyle();
    const ableem::Font &font = gui->assets().themeFonts[FONT_22_MED];
    const int width = gui->text().textWidth(font, message) + 2 * (PanelStyle::RowInset + 8);
    const int height = font.lineHeight() + 20;
    style.sheet(renderer, ableem::Rect((SCREEN_WIDTH - width) / 2, 12, width, height));
    gui->text().renderText(font, message, 0, 22, XALIGN_CENTER, &style.text);
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
    style.sheet(gui->uiContext(), panel);
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
// GuiPadConfig::prepareFrame
//*******************************
// the wizard's step for this frame, before it is drawn (render() = this, then the screen stack's frame of draw()): the
// pad read, the input being mapped taken, the pad's own controls - and false, with no frame, once a 2 s hold left
bool GuiPadConfig::prepareFrame() {
    if (stage != Stage::Test && Joystick::count() != joysticksAtStart) {
        cancelMapping();
        showPopup(_("Gamepad configuration changed. Mapping interrupted."));
    }
    joystick.update();

    if (stage == Stage::Mapping) {
        const PadMapping::Scan scan = elements[current].scan;
        if (pending.empty()) {
            pending = PadMapping::detectChange(initialState, joystick.state(), elements, scan);
            releasedSince = 0;
            if (!pending.empty())
                app.audio().cursor.play();
        } else {
            if (scan == PadMapping::Scan::Trigger) {
                // a pad with both a button and an axis for L2 gives the button first: the axis, once it
                // crosses, is the better mapping (an analog trigger)
                if (pending[0] == 'b') {
                    const string axis = PadMapping::detectChange(initialState, joystick.state(), elements, scan);
                    if (!axis.empty() && axis[0] != 'b')
                        pending = axis;
                }
                pending = PadMapping::wholeTrigger(pending, initialState, joystick.state());
            }
            // taken once let go and settled for SettleMs - a stick springing back must neither be the next
            // step's rest nor its input
            if (PadMapping::anythingHeld(initialState, joystick.state())) {
                releasedSince = 0;
            } else if (releasedSince == 0) {
                releasedSince = gui->platform().ticks();
            } else if (gui->platform().ticks() - releasedSince >= PadMapping::SettleMs) {
                const string taken = pending;
                pending.clear();
                takeInput(taken);
            }
        }
        // TOOLS-9: every step is skippable (Circle once mapped, the console's Open) OR times out - a pad missing a button/stick must still reach the
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
        return false;
    }
    padControls();
    switchToPressedPad();
    return true;
}

//*******************************
// GuiPadConfig::draw
//*******************************
// the frame's picture: the screen stack clears before it and presents after it
void GuiPadConfig::draw() {
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
            first = _("Press the highlighted button, then let it go.");
            break;
        case PadMapping::Scan::Trigger:
            first = _("Press the highlighted trigger fully, then let it go.");
            break;
        case PadMapping::Scan::Analog:
            first = _("Move the stick as shown, then let it go.");
            break;
        }
        second = _("Not on your pad? The step skips itself when the time runs out.");
        break;
    case Stage::Save:
        first = _("Mapping complete - test your pad, then save it or cancel.");
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

    // while Circle (or, in Test, Cross) is held, how far the hold has come: a bar across the foot of the content
    const unsigned int since = holdSince != 0 ? holdSince : crossSince;
    if (since != 0) {
        const unsigned int held = min(+PadMapping::HoldToExitMs, gui->platform().ticks() - since);
        const Rect track(content.x + PanelStyle::RowInset, content.y + content.h - 6,
                         content.w - 2 * PanelStyle::RowInset, 4);
        style.progress(renderer, track, held, PadMapping::HoldToExitMs, abgui::Tone::Text, 60, abgui::Tone::Text, 220);
    }

    // the pad's own buttons (the console's front buttons and a keyboard still work, unlisted)
    const bool circleMapped = !elementNow("b").empty();
    if (stage == Stage::Test) {
        string hints = !elementNow("a").empty() ? "|@X| " + _("Hold 2 s: Map pad") : "";
        if (circleMapped)
            hints += "   |@O| " + _("Hold 2 s: Exit");
        else
            hints += "   " + _("Hold any button 2 s: Exit");
        if (Joystick::count() > 1)
            hints += "   " + _("Other pad: press a button");
        gui->renderStatus(hints);
    } else if (stage == Stage::Mapping) {
        bool circleThisSession = false;
        for (const PadMapping::Element &e : elements)
            if (e.apiName == "b" && !e.value.empty())
                circleThisSession = true;
        gui->renderStatus((circleThisSession ? "|@O| " + _("Skip") + "   " : string()) + holdHint());
    } else {
        gui->renderStatus("|@X| " + _("Save") + "   |@O| " + _("Cancel mapping"));
    }

    // the step's countdown (and a pad with no mapping's), a popup at the top
    const unsigned int now = gui->platform().ticks();
    auto secondsTo = [&](unsigned int at) { return to_string((at > now ? at - now : 0) / 1000 + 1); };
    if (stage == Stage::Mapping)
        renderTopPopup(_("Skip in") + " " + secondsTo(stepDeadline) + " s");
    else if (stage == Stage::Test && crossArmed)
        renderTopPopup(_("Release to start mapping"));
    else if (stage == Stage::Test && autoMapAt != 0)
        renderTopPopup(_("Mapping starts in") + " " + secondsTo(autoMapAt) + " s");
    renderPopup();
}

//*******************************
// GuiPadConfig::loop
//*******************************
// the pad itself drives this screen (padControls(), from render()'s prepareFrame()); the console's front buttons -
// Power, Reset and Open - and a keyboard's Escape, Start (Space) and Return are shortcuts handled here. Input
// hands the power button over as a key for the duration instead of powering off.
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
