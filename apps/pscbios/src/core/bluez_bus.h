//
// BluezBus: the few D-Bus operations BluezClient needs from BlueZ (org.bluez on the system bus), as an interface -
// DbusBluezBus (bluez_dbus_bus.*, libdbus-1) is the console's, the tests script a fake. Everything BlueZ answers
// arrives here already unpacked into plain values (DbusValue), so the device list, the pairing's state machine
// and the agent's answers are BluezClient's and are tested without a bus. Nothing here blocks for longer than
// the timeout it is given.
//
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

//******************
// DbusValue
//******************
// one property value out of GetManagedObjects (a variant): what BlueZ's Adapter1/Device1/Battery1 properties
// are - booleans, integers of every width, strings and object paths; arrays (UUIDs) keep their string elements
struct DbusValue {
    enum class Type { None, Bool, Int, String, ObjectPath, Array };
    Type type = Type::None;
    bool boolean = false;
    int64_t integer = 0;
    std::string text;               // String and ObjectPath
    std::vector<std::string> items; // Array (of strings/paths; anything else in it is left out)

    static DbusValue ofBool(bool value);
    static DbusValue ofInt(int64_t value);
    static DbusValue ofString(const std::string &value);
    static DbusValue ofPath(const std::string &value);
};

using DbusProperties = std::map<std::string, DbusValue>;          // property name -> value
using DbusInterfaces = std::map<std::string, DbusProperties>;     // interface -> its properties
using DbusManagedObjects = std::map<std::string, DbusInterfaces>; // object path -> its interfaces

//******************
// BusError
//******************
// a D-Bus error as BlueZ sent it ("org.bluez.Error.AuthenticationFailed", "Authentication Failed"), or one of
// the transport's own (the bus not reachable, a timeout: org.freedesktop.DBus.Error.*)
struct BusError {
    std::string name;
    std::string message;
    bool empty() const { return name.empty() && message.empty(); }
    void clear() {
        name.clear();
        message.clear();
    }
    std::string text() const; // "name: message", or whichever of the two there is
};

//******************
// AgentRequest / AgentReply
//******************
// a call BlueZ made to the agent object (org.bluez.Agent1): the method, the device it is about (an object path,
// "" for Release/Cancel) and its other argument
struct AgentRequest {
    std::string method;   // "RequestConfirmation", "AuthorizeService", "RequestPinCode", "Release", ...
    std::string device;   // "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
    uint32_t passkey = 0; // RequestConfirmation, DisplayPasskey
    std::string text;     // AuthorizeService's UUID, DisplayPinCode's PIN
};

struct AgentReply {
    bool ok = true;
    std::string errorName; // "org.bluez.Error.Rejected" when !ok
    std::string errorMessage;
    std::string pinCode;  // RequestPinCode's answer
    uint32_t passkey = 0; // RequestPasskey's answer

    static AgentReply success() { return AgentReply(); }
    static AgentReply rejected(const std::string &why);
};

//******************
// BluezBus
//******************
class BluezBus {
public:
    using AgentHandler = std::function<AgentReply(const AgentRequest &)>;
    enum class AsyncState { None, Pending, Succeeded, Failed };

    virtual ~BluezBus() = default;

    // org.freedesktop.DBus.ObjectManager.GetManagedObjects on org.bluez's root: every adapter and device
    virtual bool getManagedObjects(DbusManagedObjects &out, BusError &error) = 0;
    // org.freedesktop.DBus.Properties.Set of a boolean (Adapter1.Powered, Device1.Trusted)
    virtual bool setBool(const std::string &path, const std::string &interface, const std::string &property, bool value,
                         BusError &error) = 0;
    // a method without arguments (StartDiscovery, StopDiscovery, Disconnect)
    virtual bool call(const std::string &path, const std::string &interface, const std::string &method,
                      BusError &error) = 0;
    // a method taking one object path (Adapter1.RemoveDevice, AgentManager1.RequestDefaultAgent/UnregisterAgent)
    virtual bool callWithPath(const std::string &path, const std::string &interface, const std::string &method,
                              const std::string &argument, BusError &error) = 0;
    // Adapter1.SetDiscoveryFilter({"Transport": transport})
    virtual bool setDiscoveryFilter(const std::string &adapter, const std::string &transport, BusError &error) = 0;

    // the agent: the object exported at agentPath (every Agent1 call on it goes to handler, while pump() runs),
    // then AgentManager1.RegisterAgent(agentPath, capability); unregisterAgent is UnregisterAgent plus the export
    // removed - the export goes even when BlueZ refuses (it may have been restarted and forgotten us)
    virtual bool registerAgent(const std::string &agentPath, const std::string &capability, AgentHandler handler,
                               BusError &error) = 0;
    virtual bool unregisterAgent(const std::string &agentPath, BusError &error) = 0;

    // one call without arguments, not waited for (Device1.Pair, Device1.Connect - they take seconds, and BlueZ
    // calls the agent meanwhile): its state is asyncState(), advanced by pump(). One at a time; a new one
    // replaces (and forgets) the last.
    virtual bool startAsync(const std::string &path, const std::string &interface, const std::string &method,
                            int timeoutMs, BusError &error) = 0;
    // Pending until the reply arrived: Succeeded, or Failed with the error in `error`
    virtual AsyncState asyncState(BusError &error) = 0;
    virtual void cancelAsync() = 0;

    // reads and writes the connection for up to timeoutMs, handing any incoming agent call to its handler
    virtual void pump(int timeoutMs) = 0;
};

// connects a BluezBus: nullptr, with the reason in `error`, when the system bus cannot be reached
using BluezBusFactory = std::function<std::unique_ptr<BluezBus>(BusError &error)>;
