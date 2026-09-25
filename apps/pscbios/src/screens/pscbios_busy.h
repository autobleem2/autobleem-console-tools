//
// BusyWork: a slow console call under the standard busy spinner (Gui::beginBusy) - for as long as it lives the
// backend's wait hook is its tick(): the spinner turns, the pad is read, and a cancellable one ends the wait when
// Circle is pressed (a Quit - the power button, a window closed - ends it too, and is handed on to the screen's
// loop). The screen is the backdrop, dimmed: `redraw` renders it, so a screen that shows "|@O| Cancel" in its
// footer while busy has the hint on the spinner's backdrop. setMessage() changes what the spinner says (a
// pairing's stage), over a fresh backdrop.
//
#pragma once

#include "core/console_backend.h"

#include <functional>
#include <string>

class Gui;

//******************
// BusyWork
//******************
class BusyWork {
public:
    BusyWork(Gui &gui, ConsoleBackend &backend, const std::string &message, std::function<void()> redraw,
             bool cancellable);
    ~BusyWork();
    BusyWork(const BusyWork &) = delete;
    BusyWork &operator=(const BusyWork &) = delete;

    void setMessage(const std::string &message);
    // a frame of the spinner and the pad read; false once the user asked to stop (from then on, every time)
    bool tick();
    bool stopped() const { return stopped_; }
    // a loop of its own (a state machine pumped between frames): tick, then wait the rest of a ~20 ms frame
    bool frame();

private:
    Gui &gui_;
    ConsoleBackend &backend_;
    std::function<void()> redraw_;
    bool cancellable_;
    bool stopped_ = false;
    std::string message_;
};
