//
// PadMapping: the wizard's logic.
//
#include "pad_mapping.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace std;
using ableem::JoystickState;

//*******************************
// PadMapping::standardElements
//*******************************
vector<PadMapping::Element> PadMapping::standardElements() {
    auto digital = [](const char *name, int button) {
        Element e;
        e.apiName = name;
        e.scan = Scan::Digital;
        e.button = button;
        return e;
    };
    auto trigger = [](const char *name, int axis) {
        Element e;
        e.apiName = name;
        e.scan = Scan::Trigger;
        e.axis = axis;
        return e;
    };
    auto analog = [](const char *name, int axis, bool negative) {
        Element e;
        e.apiName = name;
        e.scan = Scan::Analog;
        e.axis = axis;
        e.negative = negative;
        return e;
    };
    // the button numbers are SDL_GameControllerButton's, the axes SDL_GameControllerAxis's
    return {digital("a", 0),
            digital("b", 1),
            digital("x", 2),
            digital("y", 3),
            digital("back", 4),
            digital("guide", 5),
            digital("start", 6),
            digital("leftstick", 7),
            digital("rightstick", 8),
            digital("leftshoulder", 9),
            digital("rightshoulder", 10),
            trigger("lefttrigger", 4),
            trigger("righttrigger", 5),
            digital("dpup", 11),
            digital("dpdown", 12),
            digital("dpleft", 13),
            digital("dpright", 14),
            analog("-leftx", 0, true),
            analog("+leftx", 0, false),
            analog("-lefty", 1, true),
            analog("+lefty", 1, false),
            analog("-rightx", 2, true),
            analog("+rightx", 2, false),
            analog("-righty", 3, true),
            analog("+righty", 3, false)};
}

//*******************************
// PadMapping::detectChange
//*******************************
string PadMapping::detectChange(const JoystickState &initial, const JoystickState &now, const vector<Element> &taken,
                                Scan scan) {
    auto isTaken = [&](const string &raw) {
        for (const Element &element : taken)
            if (element.value == raw)
                return true;
        return false;
    };
    auto button = [&]() {
        size_t buttons = min(initial.buttons.size(), now.buttons.size());
        for (size_t i = 0; i < buttons; i++)
            if (now.buttons[i] != initial.buttons[i] && !isTaken("b" + to_string(i)))
                return "b" + to_string(i);
        return string();
    };
    // a hat is reported by where it points now, one direction at a time (a diagonal is not a mapping)
    auto hat = [&]() {
        for (size_t i = 0; i < now.hats.size(); i++) {
            unsigned h = now.hats[i];
            if (h != ableem::Joystick::HatUp && h != ableem::Joystick::HatDown && h != ableem::Joystick::HatLeft &&
                h != ableem::Joystick::HatRight)
                continue;
            string raw = "h" + to_string(i) + "." + to_string(h);
            if (!isTaken(raw))
                return raw;
        }
        return string();
    };
    // the axis moved furthest past AxisThreshold, not the first: pushing a stick one way moves its other
    // axis a little too, and a diagonal-ish push must still name the axis the user meant
    auto axis = [&](bool sticksOnly) {
        string best;
        int bestDelta = AxisThreshold;
        size_t axes = min(initial.axes.size(), now.axes.size());
        for (size_t i = 0; i < axes; i++) {
            int delta = now.axes[i] - initial.axes[i];
            if (abs(delta) <= bestDelta)
                continue;
            const bool middle = abs(initial.axes[i]) < RestTolerance;
            if (sticksOnly && !middle)
                continue; // rested at one end: a trigger, never a stick
            // rested in the middle: a stick, one half of it; at one end: a trigger, the whole axis
            string raw = middle ? string(delta > 0 ? "+" : "-") + "a" + to_string(i) : "a" + to_string(i);
            if (isTaken(raw))
                continue;
            best = raw;
            bestDelta = abs(delta);
        }
        return best;
    };
    string result;
    switch (scan) {
    case Scan::Digital:
        result = button();
        if (result.empty())
            result = hat();
        if (result.empty())
            result = axis(false);
        break;
    case Scan::Analog:
        result = axis(true);
        break;
    case Scan::Trigger:
        result = axis(false);
        if (result.empty())
            result = button();
        break;
    }
    return result;
}

//*******************************
// PadMapping::wholeTrigger / anyPressed
//*******************************
string PadMapping::wholeTrigger(const string &pending, JoystickState &rest, const JoystickState &now) {
    if (pending.size() < 3 || (pending[0] != '+' && pending[0] != '-') || pending[1] != 'a')
        return pending;
    int n = atoi(pending.c_str() + 2);
    if (n < 0 || n >= static_cast<int>(now.axes.size()) || n >= static_cast<int>(rest.axes.size()))
        return pending;
    const int farEnd = 32767 - RestTolerance;
    const bool otherEnd = pending[0] == '+' ? now.axes[n] < -farEnd : now.axes[n] > farEnd;
    if (!otherEnd)
        return pending;
    rest.axes[n] = now.axes[n];
    return "a" + to_string(n);
}

bool PadMapping::hasFreeAxis(const vector<Element> &elements, size_t axisCount) {
    for (size_t i = 0; i < axisCount; i++) {
        const string n = "a" + to_string(i);
        bool used = false;
        for (const Element &e : elements)
            if (e.value == n || e.value == "+" + n || e.value == "-" + n || e.value == n + "~")
                used = true;
        if (!used)
            return true;
    }
    return false;
}

bool PadMapping::anyPressed(const JoystickState &now) {
    for (bool pressed : now.buttons)
        if (pressed)
            return true;
    for (unsigned hat : now.hats)
        if (hat != 0)
            return true;
    return false;
}

//*******************************
// PadMapping::anythingHeld
//*******************************
bool PadMapping::anythingHeld(const JoystickState &initial, const JoystickState &now) {
    size_t buttons = min(initial.buttons.size(), now.buttons.size());
    for (size_t i = 0; i < buttons; i++)
        if (now.buttons[i] != initial.buttons[i])
            return true;
    size_t hats = min(initial.hats.size(), now.hats.size());
    for (size_t i = 0; i < hats; i++)
        if (now.hats[i] != initial.hats[i])
            return true;
    size_t axes = min(initial.axes.size(), now.axes.size());
    for (size_t i = 0; i < axes; i++)
        if (abs(now.axes[i] - initial.axes[i]) > HeldThreshold)
            return true;
    return false;
}

//*******************************
// PadMapping::rawInput / inputHeld / circleInput
//*******************************
string PadMapping::rawInput(const string &mappingLine, const string &apiName) {
    const string key = "," + apiName + ":";
    size_t at = mappingLine.find(key);
    if (at == string::npos)
        return "";
    at += key.size();
    return mappingLine.substr(at, mappingLine.find(',', at) - at);
}

bool PadMapping::inputHeld(const string &raw, const JoystickState &initial, const JoystickState &now) {
    if (raw.size() < 2)
        return false;
    auto index = [](const string &digits) { return digits.empty() ? -1 : atoi(digits.c_str()); };
    if (raw[0] == 'b') {
        int n = index(raw.substr(1));
        return n >= 0 && n < static_cast<int>(now.buttons.size()) && now.buttons[n];
    }
    if (raw[0] == 'h') {
        size_t dot = raw.find('.');
        if (dot == string::npos)
            return false;
        int n = index(raw.substr(1, dot - 1));
        unsigned mask = static_cast<unsigned>(atoi(raw.c_str() + dot + 1));
        return n >= 0 && n < static_cast<int>(now.hats.size()) && (now.hats[n] & mask) != 0;
    }
    const char sign = raw[0] == '+' || raw[0] == '-' ? raw[0] : 0;
    const string axis = sign ? raw.substr(1) : raw;
    if (axis.empty() || axis[0] != 'a')
        return false;
    int n = index(axis.substr(1, axis.find('~') == string::npos ? string::npos : axis.find('~') - 1));
    if (n < 0 || n >= static_cast<int>(now.axes.size()))
        return false;
    if (sign == '+')
        return now.axes[n] > HeldThreshold;
    if (sign == '-')
        return now.axes[n] < -HeldThreshold;
    int rest = n < static_cast<int>(initial.axes.size()) ? initial.axes[n] : 0;
    return abs(now.axes[n] - rest) > HeldThreshold;
}

string PadMapping::circleInput(const vector<Element> &elements, const string &mappingLine) {
    return elementInput(elements, mappingLine, "b");
}

string PadMapping::elementInput(const vector<Element> &elements, const string &mappingLine, const string &apiName) {
    for (const Element &e : elements)
        if (e.apiName == apiName && !e.value.empty())
            return e.value;
    return rawInput(mappingLine, apiName);
}

bool PadMapping::isExitKey(ableem::Key key) {
    return key == ableem::Key::Sleep || key == ableem::Key::Escape || key == ableem::Key::Backspace;
}

//*******************************
// PadMapping::advanceHold
//*******************************
bool PadMapping::advanceHold(unsigned &holdSince, bool heldNow, unsigned nowTicks) {
    if (!heldNow) {
        holdSince = 0;
        return false;
    }
    if (holdSince == 0)
        holdSince = nowTicks == 0 ? 1 : nowTicks;
    return nowTicks - holdSince >= HoldToExitMs;
}

//*******************************
// PadMapping::isEmptyMapping
//*******************************
bool PadMapping::isEmptyMapping(const vector<Element> &finals) {
    for (const Element &element : finals)
        if (element.apiName != "platform")
            return false;
    return true;
}

//*******************************
// PadMapping::mergeAxis
//*******************************
// the "-x" and "+x" halves of a stick axis: both mapped to the two halves of the same raw axis become one
// "a<n>" element (inverted, "~", when the pad's + is our -); a half without its partner drops both (a
// stick that only goes one way is no stick); anything else - halves on different axes, a half on a button -
// is kept as the two half-axis keys SDL also understands ("-leftx:-a0,+leftx:b5")
void PadMapping::mergeAxis(vector<Element> &elements, const string &apiName, size_t minus, size_t plus) {
    string m = elements[minus].value, p = elements[plus].value;
    if (m.empty() || p.empty()) {
        elements[minus].value.clear();
        elements[plus].value.clear();
        return;
    }
    bool halves = m.size() >= 3 && p.size() >= 3 && (m[0] == '+' || m[0] == '-') && (p[0] == '+' || p[0] == '-') &&
                  m[1] == 'a' && p[1] == 'a';
    if (!halves || m.substr(2) != p.substr(2))
        return;
    Element merged;
    merged.apiName = apiName;
    merged.scan = Scan::Analog;
    merged.value = "a" + m.substr(2);
    if (p[0] == '-' && m[0] == '+')
        merged.value += "~";
    elements[minus].value.clear();
    elements[plus].value.clear();
    elements.push_back(merged);
}

//*******************************
// PadMapping::finalElements
//*******************************
vector<PadMapping::Element> PadMapping::finalElements(const vector<Element> &scanned, const string &platform) {
    vector<Element> elements = scanned;
    if (elements.size() >= 25) {
        // a pad with no left stick drives it from the d-pad: each direction is also that half of the stick,
        // at full deflection ("-leftx:h0.8"), so a game that only reads the stick still moves
        if (elements[17].value.empty() && elements[18].value.empty() && elements[19].value.empty() &&
            elements[20].value.empty()) {
            elements[17].value = elements[15].value; // -leftx: dpleft
            elements[18].value = elements[16].value; // +leftx: dpright
            elements[19].value = elements[13].value; // -lefty: dpup
            elements[20].value = elements[14].value; // +lefty: dpdown
        }
        mergeAxis(elements, "leftx", 17, 18);
        mergeAxis(elements, "lefty", 19, 20);
        mergeAxis(elements, "rightx", 21, 22);
        mergeAxis(elements, "righty", 23, 24);
    }
    vector<Element> finals;
    for (const Element &element : elements)
        if (!element.value.empty())
            finals.push_back(element);
    Element platformElement;
    platformElement.apiName = "platform";
    platformElement.value = platform;
    finals.push_back(platformElement);
    return finals;
}

//*******************************
// PadMapping::mappingLine / cleanName
//*******************************
string PadMapping::mappingLine(const string &guid, const string &name, const vector<Element> &finals) {
    string line = guid + "," + name + ",";
    for (const Element &element : finals)
        line += element.apiName + ":" + element.value + ",";
    return line;
}

string PadMapping::cleanName(const string &name) {
    string clean;
    for (char c : name)
        if (isalnum(static_cast<unsigned char>(c)) || isspace(static_cast<unsigned char>(c)))
            clean += c;
    return clean;
}
