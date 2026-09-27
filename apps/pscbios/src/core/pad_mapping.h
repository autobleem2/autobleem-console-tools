//
// PadMapping: the mapping wizard's logic without the screen - the 25 things a standard game controller
// has, in the order the user is asked for them; which raw input just moved; the merge of the two halves
// of each stick axis into one; the gamecontrollerdb line that comes out.
//
// A raw input is written the way SDL's mapping strings do: "b<n>" a button, "h<n>.<mask>" a hat
// direction, "+a<n>"/"-a<n>" half an axis that rests in the middle (a stick), "a<n>" a whole axis that
// rests at one end (a trigger), and "a<n>~" a stick axis the pad reports inverted.
//
#pragma once

#include <ableem/ui/input.h>    // Key
#include <ableem/ui/joystick.h> // JoystickState, a plain struct - no SDL behind it

#include <string>
#include <vector>

//******************
// PadMapping
//******************
class PadMapping {
public:
    enum class Scan { Digital, Analog, Trigger };

    struct Element {
        std::string apiName; // "a", "dpup", "-leftx", "lefttrigger", ...
        Scan scan = Scan::Digital;
        int button = -1;       // the standard button (0..14) it lights up on the pad picture while asked for
        int axis = -1;         // or the standard axis (0..5)
        bool negative = false; // ... and towards which end
        std::string value;     // the raw input found for it, "" until then
    };

    // the 25 elements the wizard asks for, in order; every value empty
    static std::vector<Element> standardElements();

    // the raw input that moved between `initial` and `now`, a button first, then a hat, then an axis (a
    // change of more than AxisThreshold); "" when nothing did, or when the input is already the value of
    // one of `taken` (an element cannot be mapped twice)
    static std::string detectChange(const ableem::JoystickState &initial, const ableem::JoystickState &now,
                                    const std::vector<Element> &taken);
    // the raw inputs still away from their rest position - what the wizard waits to clear before asking
    // for the next element
    static bool anythingHeld(const ableem::JoystickState &initial, const ableem::JoystickState &now);

    // the list to write: each stick axis' two halves merged into one "a<n>" (with "~" when the halves
    // came out inverted) when they are the two halves of one raw axis, both dropped when one is missing,
    // kept as two half-axis keys otherwise; every unmapped element dropped; "platform:<platform>" last
    static std::vector<Element> finalElements(const std::vector<Element> &scanned, const std::string &platform);

    // the raw input a mapping line gives an element: rawInput("...,a:b0,b:b1,...", "b") -> "b1"; "" when the
    // line has no such element
    static std::string rawInput(const std::string &mappingLine, const std::string &apiName);
    // the raw input (in the format above) is away from its rest in `now` - a button pressed, the hat in that
    // direction, a half axis past HeldThreshold on its side, a whole axis that far from where it rested in
    // `initial`. False for "" or an input the pad does not have
    static bool inputHeld(const std::string &raw, const ableem::JoystickState &initial,
                          const ableem::JoystickState &now);
    // the raw input that is Circle ("b") on this pad: what the wizard mapped it to this time, else what the
    // pad's mapping line says, "" when neither knows - what the wizard's hold-to-exit watches
    static std::string circleInput(const std::vector<Element> &elements, const std::string &mappingLine);

    // Power (Key::Sleep) or a keyboard's Esc/Backspace leaves the wizard at once. The wizard turns
    // keyboardAsPad off while it shows (ableem::Input::setKeyboardAsPad(false)/setRawKeyboard(true), the
    // way GuiKeyboard does), so these arrive as keys, never remapped into a Button::Circle event - a real
    // pad's own Circle only ever reaches the wizard as a button (Test stage: it is opened as a
    // GameController too, for the picture; SDL keeps sending its events even though Input let its own pads
    // go), and only the 2 s hold (HoldToExitMs, via inputHeld on the raw joystick) leaves for that; a short
    // press is mapped/tested as usual
    static bool isExitKey(ableem::Key key);

    static const unsigned HoldToExitMs = 2000;   // Circle held this long leaves the wizard
    static const unsigned StepTimeoutMs = 10000; // a mapping step this long unresolved is skipped/taken as-is

    // advances a hold-to-exit timer in place: `holdSince` (0 = not currently counting) is driven from
    // whether the watched input is held right now and the current tick count; returns true once it has been
    // held continuously for HoldToExitMs. Pure and pad-agnostic, so the wizard can run one of these per
    // connected pad (TOOLS-9: the 2 s exit used to work only from the pad being mapped)
    static bool advanceHold(unsigned &holdSince, bool heldNow, unsigned nowTicks);

    // "<guid>,<name>,<apiName>:<value>,...," as SDL reads it
    static std::string mappingLine(const std::string &guid, const std::string &name,
                                   const std::vector<Element> &finals);
    // a pad name SDL will take inside the line: letters, digits and spaces only
    static std::string cleanName(const std::string &name);
    // true when `finals` has nothing but the trailing "platform" entry - every element was skipped/timed
    // out, so there is nothing worth writing to the database
    static bool isEmptyMapping(const std::vector<Element> &finals);

    // TOOLS-9: AxisThreshold used to be 32000 out of the ~32767 range - a ~97.5% deflection that many real
    // sticks never reach (calibration, deadzone, a worn or cheap pad's reduced travel), so a deliberate move
    // often registered as nothing. It is now measured against a rest sampled fresh for each step (see
    // GuiPadConfig::advance()), so ~50% of the range is already a clear, deliberate move.
    static const int AxisThreshold = 16384; // how far an axis must travel from its step's rest to count as "moved"
    // TOOLS-9: 600 (under 2% of range) misread an ordinary stick's centre drift (common on worn/cheap pads)
    // as a trigger resting at an extreme, which then mapped the whole axis instead of a half - "maps
    // something at random". A trigger's true rest is tens of thousands of units out, so this can grow a lot
    // and still tell the two apart cleanly.
    static const int RestTolerance = 3000; // an axis within this of 0 rested in the middle (a stick)
    // TOOLS-9: 20000 (61% of range) let "let go" fire while a stick was still far from centre; the very next
    // step's freshly-sampled rest could then be taken mid-drift. Lowered so "let go" means actually close to
    // rest again before the next step starts measuring from it.
    static const int HeldThreshold = 8000; // an axis still this far from its rest is still held

private:
    static void mergeAxis(std::vector<Element> &elements, const std::string &apiName, size_t minus, size_t plus);
};
