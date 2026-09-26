//
// FakeBluezBus: a scripted BlueZ for BluezClient's tests - the managed objects a test sets up, every call recorded
// as a line of text, an error per method on request, devices that appear a few pumps into a discovery, and Pair /
// Connect answered a few pumps after they were started, with the agent asked in between as bluetoothd would.
// The state is shared, so a test keeps its handle after the client took the bus.
//
#pragma once

#include "core/bluez_bus.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fakebluez {

const char *const Adapter = "/org/bluez/hci0";

inline DbusInterfaces adapter(const std::string &address, bool powered) {
    DbusInterfaces ifaces;
    DbusProperties &p = ifaces["org.bluez.Adapter1"];
    p["Address"] = DbusValue::ofString(address);
    p["Name"] = DbusValue::ofString("psc");
    p["Alias"] = DbusValue::ofString("PSC");
    p["Powered"] = DbusValue::ofBool(powered);
    p["Discovering"] = DbusValue::ofBool(false);
    ifaces["org.freedesktop.DBus.Introspectable"];
    return ifaces;
}

inline std::string devicePath(const std::string &address, const std::string &adapterPath = Adapter) {
    std::string path = adapterPath + "/dev_" + address;
    for (char &c : path)
        if (c == ':')
            c = '_';
    return path;
}

inline DbusInterfaces device(const std::string &address, const std::string &name, bool paired = false,
                             bool connected = false, const std::string &adapterPath = Adapter) {
    DbusInterfaces ifaces;
    DbusProperties &p = ifaces["org.bluez.Device1"];
    p["Address"] = DbusValue::ofString(address);
    if (!name.empty())
        p["Name"] = DbusValue::ofString(name);
    std::string alias = address;
    for (char &c : alias)
        if (c == ':')
            c = '-';
    p["Alias"] = DbusValue::ofString(name.empty() ? alias : name);
    p["Adapter"] = DbusValue::ofPath(adapterPath);
    p["Paired"] = DbusValue::ofBool(paired);
    p["Trusted"] = DbusValue::ofBool(paired);
    p["Connected"] = DbusValue::ofBool(connected);
    p["Icon"] = DbusValue::ofString("input-gaming");
    p["Class"] = DbusValue::ofInt(0x002508);
    p["Modalias"] = DbusValue::ofString("usb:v054Cp09CCd0100");
    DbusValue uuids;
    uuids.type = DbusValue::Type::Array;
    uuids.items = {"00001124-0000-1000-8000-00805f9b34fb"};
    p["UUIDs"] = uuids;
    return ifaces;
}

struct State {
    DbusManagedObjects objects;
    std::vector<std::string> calls;         // "Pair /org/bluez/hci0/dev_...", "Set Trusted=true ...", ...
    std::map<std::string, BusError> errors; // method (or "Set <property>") -> the error it answers with
    DbusManagedObjects discoverable;        // joins `objects` appearAfterPumps pumps into a discovery
    int appearAfterPumps = 2;
    bool discovering = false;
    int discoveryPumps = 0;

    // the agent calls bluetoothd makes during Pair, in order: (method, device path)
    std::vector<std::pair<std::string, std::string>> agentCallsDuringPair;
    std::vector<AgentReply> agentReplies; // what the client answered them
    bool pairSetsPaired = true;           // a Pair that succeeds leaves Paired=true
    int asyncPumps = 3;                   // pumps until a started Pair/Connect answers
    bool asyncNeverAnswers = false;
    // models CancelPairing losing its race with Pair()/Connect() finishing on BlueZ's side: cancelAsync() (what
    // giving up on the client's own wait for the reply does) lands the property change anyway, as if bluetoothd
    // had already committed it a moment before the cancel arrived
    bool completesDespiteCancel = false;
    int callTimeout = 5000;           // what setCallTimeout() left
    std::vector<int> refreshTimeouts; // the timeout each GetManagedObjects was made with
    std::function<bool()> waitHook;   // what setWaitHook() was given

    BluezBus::AgentHandler agent;
    std::string agentPath;
    std::string pendingMethod, pendingPath;
    int pendingPumps = 0;
    BluezBus::AsyncState asyncState = BluezBus::AsyncState::None;
    BusError asyncError;

    DbusProperties *props(const std::string &path, const std::string &iface) {
        auto object = objects.find(path);
        if (object == objects.end())
            return nullptr;
        auto i = object->second.find(iface);
        return i == object->second.end() ? nullptr : &i->second;
    }
    bool failWith(const std::string &key, BusError &error) {
        auto it = errors.find(key);
        if (it == errors.end())
            return false;
        error = it->second;
        return true;
    }
};

class FakeBluezBus : public BluezBus {
public:
    explicit FakeBluezBus(std::shared_ptr<State> state) : s_(std::move(state)) {}

    bool getManagedObjects(DbusManagedObjects &out, BusError &error) override {
        s_->refreshTimeouts.push_back(s_->callTimeout);
        if (s_->failWith("GetManagedObjects", error))
            return false;
        out = s_->objects;
        return true;
    }
    bool setBool(const std::string &path, const std::string &iface, const std::string &property, bool value,
                 BusError &error) override {
        s_->calls.push_back("Set " + property + "=" + (value ? "true " : "false ") + path);
        if (s_->failWith("Set " + property, error))
            return false;
        DbusProperties *p = s_->props(path, iface);
        if (p == nullptr) {
            error = {"org.freedesktop.DBus.Error.UnknownObject", "no " + path};
            return false;
        }
        (*p)[property] = DbusValue::ofBool(value);
        return true;
    }
    bool call(const std::string &path, const std::string &iface, const std::string &method, BusError &error) override {
        s_->calls.push_back(method + " " + path);
        if (s_->failWith(method, error))
            return false;
        if (method == "StartDiscovery") {
            s_->discovering = true;
            s_->discoveryPumps = 0;
        } else if (method == "StopDiscovery") {
            s_->discovering = false;
        } else if (method == "Disconnect") {
            if (DbusProperties *p = s_->props(path, iface))
                (*p)["Connected"] = DbusValue::ofBool(false);
        }
        return true;
    }
    bool callWithPath(const std::string &path, const std::string & /*iface*/, const std::string &method,
                      const std::string &argument, BusError &error) override {
        s_->calls.push_back(method + " " + path + " " + argument);
        if (s_->failWith(method, error))
            return false;
        if (method == "RemoveDevice")
            s_->objects.erase(argument);
        return true;
    }
    bool setDiscoveryFilter(const std::string &adapter, const std::string &transport, BusError &error) override {
        s_->calls.push_back("SetDiscoveryFilter " + adapter + " " + transport);
        return !s_->failWith("SetDiscoveryFilter", error);
    }
    bool registerAgent(const std::string &agentPath, const std::string &capability, AgentHandler handler,
                       BusError &error) override {
        s_->calls.push_back("RegisterAgent " + agentPath + " " + capability);
        if (s_->failWith("RegisterAgent", error))
            return false;
        s_->agent = std::move(handler);
        s_->agentPath = agentPath;
        return true;
    }
    bool unregisterAgent(const std::string &agentPath, BusError &error) override {
        s_->calls.push_back("UnregisterAgent " + agentPath);
        s_->agent = nullptr;
        s_->agentPath.clear();
        return !s_->failWith("UnregisterAgent", error);
    }
    bool startAsync(const std::string &path, const std::string & /*iface*/, const std::string &method,
                    int /*timeoutMs*/, BusError &error) override {
        s_->calls.push_back(method + " " + path);
        if (s_->failWith("start " + method, error))
            return false;
        s_->pendingMethod = method;
        s_->pendingPath = path;
        s_->pendingPumps = 0;
        s_->asyncState = AsyncState::Pending;
        s_->asyncError.clear();
        return true;
    }
    AsyncState asyncState(BusError &error) override {
        error = s_->asyncError;
        return s_->asyncState;
    }
    void cancelAsync() override {
        if (s_->completesDespiteCancel && s_->asyncState == AsyncState::Pending) {
            if (DbusProperties *p = s_->props(s_->pendingPath, "org.bluez.Device1")) {
                if (s_->pendingMethod == "Pair" && s_->pairSetsPaired)
                    (*p)["Paired"] = DbusValue::ofBool(true);
                if (s_->pendingMethod == "Connect")
                    (*p)["Connected"] = DbusValue::ofBool(true);
            }
        }
        s_->asyncState = AsyncState::None;
        s_->pendingMethod.clear();
    }
    void pump(int /*timeoutMs*/) override {
        State &s = *s_;
        if (s.discovering && ++s.discoveryPumps >= s.appearAfterPumps) {
            for (auto &object : s.discoverable)
                s.objects[object.first] = object.second;
            s.discoverable.clear();
        }
        if (s.asyncState != AsyncState::Pending || s.asyncNeverAnswers)
            return;
        ++s.pendingPumps;
        bool rejected = false;
        if (s.pendingMethod == "Pair" && s.pendingPumps == 1) {
            // bluetoothd asks the default agent while the pairing runs
            for (auto &request : s.agentCallsDuringPair) {
                AgentRequest r;
                r.method = request.first;
                r.device = request.second;
                AgentReply reply = s.agent ? s.agent(r) : AgentReply::rejected("no agent");
                s.agentReplies.push_back(reply);
                if (!reply.ok && request.second == s.pendingPath)
                    rejected = true;
            }
        }
        if (rejected) {
            s.asyncState = AsyncState::Failed;
            s.asyncError = {"org.bluez.Error.AuthenticationRejected", "Authentication Rejected"};
            return;
        }
        if (s.pendingPumps < s.asyncPumps)
            return;
        BusError error;
        if (s.failWith(s.pendingMethod, error)) {
            s.asyncState = AsyncState::Failed;
            s.asyncError = error;
            return;
        }
        s.asyncState = AsyncState::Succeeded;
        if (DbusProperties *p = s.props(s.pendingPath, "org.bluez.Device1")) {
            if (s.pendingMethod == "Pair" && s.pairSetsPaired)
                (*p)["Paired"] = DbusValue::ofBool(true);
            if (s.pendingMethod == "Connect")
                (*p)["Connected"] = DbusValue::ofBool(true);
        }
    }

    void setCallTimeout(int timeoutMs) override { s_->callTimeout = timeoutMs; }
    int callTimeout() const override { return s_->callTimeout; }
    void setWaitHook(std::function<bool()> hook) override { s_->waitHook = std::move(hook); }

private:
    std::shared_ptr<State> s_;
};

inline std::unique_ptr<BluezBus> makeBus(const std::shared_ptr<State> &state) {
    return std::unique_ptr<BluezBus>(new FakeBluezBus(state));
}

} // namespace fakebluez
