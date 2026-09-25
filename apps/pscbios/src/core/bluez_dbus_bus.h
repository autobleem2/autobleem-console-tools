//
// DbusBluezBus: BluezBus over libdbus-1 on the system bus - a private connection of PSC-Bios's own (the launcher
// has none), driven without a main loop and from the main thread only: a call is sent and its reply waited for
// by reading the connection in 40 ms slices, the wait hook asked between them (the screen's spinner turns, a
// Circle ends the wait), up to the call timeout; pump() reads the connection and dispatches the agent's calls for
// the asynchronous Pair/Connect. The thin layer; the logic is BluezClient's.
// Built where libdbus-1 is (PSCBIOS_HAVE_DBUS) - always for the console.
//
#pragma once

#include "bluez_bus.h"

#include <functional>
#include <memory>
#include <string>

struct DBusConnection;
struct DBusMessage;
struct DBusPendingCall;

//******************
// DbusBluezBus
//******************
class DbusBluezBus : public BluezBus {
public:
    static const int DefaultCallTimeoutMs = 5000;

    // the system bus, or nullptr with the reason ("org.freedesktop.DBus.Error.FileNotFound: ...")
    static std::unique_ptr<BluezBus> connectSystem(BusError &error, int callTimeoutMs = DefaultCallTimeoutMs);
    ~DbusBluezBus() override;
    DbusBluezBus(const DbusBluezBus &) = delete;
    DbusBluezBus &operator=(const DbusBluezBus &) = delete;

    bool getManagedObjects(DbusManagedObjects &out, BusError &error) override;
    bool setBool(const std::string &path, const std::string &interface, const std::string &property, bool value,
                 BusError &error) override;
    bool call(const std::string &path, const std::string &interface, const std::string &method,
              BusError &error) override;
    bool callWithPath(const std::string &path, const std::string &interface, const std::string &method,
                      const std::string &argument, BusError &error) override;
    bool setDiscoveryFilter(const std::string &adapter, const std::string &transport, BusError &error) override;
    bool registerAgent(const std::string &agentPath, const std::string &capability, AgentHandler handler,
                       BusError &error) override;
    bool unregisterAgent(const std::string &agentPath, BusError &error) override;
    bool startAsync(const std::string &path, const std::string &interface, const std::string &method, int timeoutMs,
                    BusError &error) override;
    AsyncState asyncState(BusError &error) override;
    void cancelAsync() override;
    void pump(int timeoutMs) override;
    void setCallTimeout(int timeoutMs) override { callTimeoutMs_ = timeoutMs; }
    int callTimeout() const override { return callTimeoutMs_; }
    void setWaitHook(std::function<bool()> hook) override { waitHook_ = std::move(hook); }

    // an Agent1 call arriving on the connection (libdbus's object-path callback)
    void handleAgentMessage(DBusMessage *message);

private:
    DbusBluezBus(DBusConnection *connection, int callTimeoutMs);
    // sends `message` (and unrefs it), waits for the reply - the connection read and the hook asked meanwhile;
    // nullptr with `error` set on an error reply, none in time, a closed connection or a hook that said stop
    DBusMessage *callBlocking(DBusMessage *message, BusError &error);
    void dropExport();

    DBusConnection *connection_;
    int callTimeoutMs_;
    DBusPendingCall *pending_ = nullptr;
    AsyncState asyncState_ = AsyncState::None;
    BusError asyncError_;
    std::string agentPath_; // exported while not empty
    AgentHandler agentHandler_;
    std::function<bool()> waitHook_;
};
