//
// BusyWork: the spinner, the pad and the backend's wait hook for the length of one slow call.
//
#include "pscbios_busy.h"
#include "gui/gui.h"

#include <ableem/engine/log.h>

#include <utility>

using namespace std;

BusyWork::BusyWork(Gui &gui, ConsoleBackend &backend, const string &message, function<void()> redraw, bool cancellable)
    : gui_(gui), backend_(backend), redraw_(std::move(redraw)), cancellable_(cancellable), message_(message) {
    gui_.input().flushEvents(); // the press that started it is not the one that stops it
    gui_.beginBusy(message_, redraw_);
    backend_.setWaitHook([this]() { return tick(); });
}

BusyWork::~BusyWork() {
    backend_.setWaitHook(nullptr);
    gui_.endBusy();
}

void BusyWork::setMessage(const string &message) {
    if (message == message_)
        return;
    message_ = message;
    gui_.endBusy();
    gui_.beginBusy(message_, redraw_);
}

bool BusyWork::tick() {
    gui_.busyTick();
    if (stopped_)
        return false;
    ableem::Event e;
    while (gui_.input().poll(e)) {
        if (e.type == ableem::Event::Type::Quit) {
            // the screen's loop has to see it: it closes the screen (and the power button's unwinds them all)
            gui_.input().inject(e);
            stopped_ = true;
            PLOG_INFO << "busy: \"" << message_ << "\" stopped by a quit";
            break;
        }
        if (cancellable_ && e.type == ableem::Event::Type::ButtonDown && e.button == ableem::Button::Circle) {
            stopped_ = true;
            PLOG_INFO << "busy: \"" << message_ << "\" stopped by the user";
        }
    }
    return !stopped_;
}

bool BusyWork::frame() {
    const unsigned int start = gui_.platform().ticks();
    const bool going = tick();
    const unsigned int spent = gui_.platform().ticks() - start;
    if (spent < 20)
        gui_.platform().delay(20 - spent);
    return going;
}
