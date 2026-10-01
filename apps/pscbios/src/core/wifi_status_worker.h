//
// WifiStatusWorker: the WiFi screen's periodic status read (the connection row and the timezone) run on a thread
// of its own. The read spawns nmcli/timedatectl on a Raspberry Pi (blocking popen calls, a few hundred ms in all),
// which on the UI thread froze the screen every refresh (BUG-33). The screen starts a read when one is due and
// takes the finished result on a later frame; it never waits for it, except before it uses the backend itself
// (wait()) and when it goes away (the destructor).
//
// The thread touches the ConsoleBackend and its own result only - never a screen or SDL. The backend is not
// thread-safe, so the owner must not call it while a read is running: wait() first.
//
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class ConsoleBackend;

//******************
// WifiStatusSnapshot
//******************
struct WifiStatusSnapshot {
    std::string connection; // the Connection row: "Connected, 192.168.1.23", "Not connected", "-" ...
    std::string timezone;
};

//******************
// WifiStatusWorker
//******************
class WifiStatusWorker {
public:
    WifiStatusWorker() = default;
    WifiStatusWorker(const WifiStatusWorker &) = delete;
    WifiStatusWorker &operator=(const WifiStatusWorker &) = delete;
    ~WifiStatusWorker() { wait(); }

    // the read itself, on the calling thread: what the worker runs, and what a screen calls after an action
    static WifiStatusSnapshot read(ConsoleBackend &backend);

    // starts a read on the thread; nothing when one is running or its result was not taken yet. The backend must
    // outlive the read - wait() (or the destructor) before it goes
    void start(ConsoleBackend &backend);
    // the finished read's result (true, once), false while it runs or when none was started
    bool take(WifiStatusSnapshot &out);
    // the read still going, or finished and not taken
    bool active() const { return active_; }
    // blocks until the running read ends and drops its result; the backend is free to use afterwards
    void wait();

private:
    std::thread thread_;
    bool active_ = false; // the owner's thread only
    std::atomic<bool> done_{false};
    std::mutex mutex_;
    WifiStatusSnapshot result_;
};
