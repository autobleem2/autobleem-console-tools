//
// DbusBluezBus: libdbus-1 on the system bus, no main loop.
//
#include "bluez_dbus_bus.h"

#include <ableem/engine/log.h>

#include <dbus/dbus.h>

#include <chrono>
#include <thread>
#include <utility>

using namespace std;

namespace {
const char *const Bluez = "org.bluez";
const char *const AgentManager1 = "org.bluez.AgentManager1";
const char *const AgentManagerPath = "/org/bluez";
const char *const Agent1 = "org.bluez.Agent1";

//******************
// ScopedError
//******************
// a DBusError freed on every path; take() moves it into a BusError
struct ScopedError {
    DBusError e;
    ScopedError() { dbus_error_init(&e); }
    ~ScopedError() { dbus_error_free(&e); }
    ScopedError(const ScopedError &) = delete;
    ScopedError &operator=(const ScopedError &) = delete;
    bool isSet() const { return dbus_error_is_set(&e) != 0; }
    void take(BusError &out) const {
        out.name = e.name != nullptr ? e.name : "";
        out.message = e.message != nullptr ? e.message : "";
    }
};

// one variant's value; nested containers other than an array of strings/paths come back as None
DbusValue readVariant(DBusMessageIter *variant) {
    DbusValue value;
    DBusMessageIter inner;
    dbus_message_iter_recurse(variant, &inner);
    int type = dbus_message_iter_get_arg_type(&inner);
    switch (type) {
    case DBUS_TYPE_BOOLEAN: {
        dbus_bool_t b = FALSE;
        dbus_message_iter_get_basic(&inner, &b);
        return DbusValue::ofBool(b != 0);
    }
    case DBUS_TYPE_BYTE: {
        unsigned char v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(v);
    }
    case DBUS_TYPE_INT16: {
        dbus_int16_t v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(v);
    }
    case DBUS_TYPE_UINT16: {
        dbus_uint16_t v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(v);
    }
    case DBUS_TYPE_INT32: {
        dbus_int32_t v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(v);
    }
    case DBUS_TYPE_UINT32: {
        dbus_uint32_t v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(v);
    }
    case DBUS_TYPE_INT64: {
        dbus_int64_t v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(v);
    }
    case DBUS_TYPE_UINT64: {
        dbus_uint64_t v = 0;
        dbus_message_iter_get_basic(&inner, &v);
        return DbusValue::ofInt(static_cast<int64_t>(v));
    }
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH: {
        const char *s = nullptr;
        dbus_message_iter_get_basic(&inner, &s);
        return type == DBUS_TYPE_STRING ? DbusValue::ofString(s != nullptr ? s : "")
                                        : DbusValue::ofPath(s != nullptr ? s : "");
    }
    case DBUS_TYPE_ARRAY: {
        value.type = DbusValue::Type::Array;
        DBusMessageIter element;
        dbus_message_iter_recurse(&inner, &element);
        for (int t = dbus_message_iter_get_arg_type(&element); t != DBUS_TYPE_INVALID;
             dbus_message_iter_next(&element), t = dbus_message_iter_get_arg_type(&element)) {
            if (t != DBUS_TYPE_STRING && t != DBUS_TYPE_OBJECT_PATH)
                continue;
            const char *s = nullptr;
            dbus_message_iter_get_basic(&element, &s);
            value.items.emplace_back(s != nullptr ? s : "");
        }
        return value;
    }
    default:
        return value;
    }
}

// a{sv}: the properties of one interface
void readProperties(DBusMessageIter *array, DbusProperties &out) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(array, &entry);
    while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv;
        dbus_message_iter_recurse(&entry, &kv);
        if (dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_STRING) {
            const char *name = nullptr;
            dbus_message_iter_get_basic(&kv, &name);
            dbus_message_iter_next(&kv);
            if (name != nullptr && dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_VARIANT)
                out[name] = readVariant(&kv);
        }
        dbus_message_iter_next(&entry);
    }
}

// a{sa{sv}}: the interfaces of one object
void readInterfaces(DBusMessageIter *array, DbusInterfaces &out) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(array, &entry);
    while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv;
        dbus_message_iter_recurse(&entry, &kv);
        if (dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_STRING) {
            const char *name = nullptr;
            dbus_message_iter_get_basic(&kv, &name);
            dbus_message_iter_next(&kv);
            if (name != nullptr && dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_ARRAY)
                readProperties(&kv, out[name]);
        }
        dbus_message_iter_next(&entry);
    }
}

DBusHandlerResult agentCallback(DBusConnection * /*connection*/, DBusMessage *message, void *self) {
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL || !dbus_message_has_interface(message, Agent1))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    static_cast<DbusBluezBus *>(self)->handleAgentMessage(message);
    return DBUS_HANDLER_RESULT_HANDLED;
}

bool outOfMemory(BusError &error) {
    error.name = "org.freedesktop.DBus.Error.NoMemory";
    error.message = "out of memory";
    return false;
}
} // namespace

//*******************************
// DbusBluezBus::connectSystem
//*******************************
unique_ptr<BluezBus> DbusBluezBus::connectSystem(BusError &error, int callTimeoutMs) {
    ScopedError e;
    // private: closing it is ours, and nothing else in the process shares it
    DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e.e);
    if (connection == nullptr) {
        e.take(error);
        if (error.empty())
            error.message = "cannot connect to the system bus";
        PLOG_WARNING << "bluetooth: no system bus: " << error.text();
        return nullptr;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    return unique_ptr<BluezBus>(new DbusBluezBus(connection, callTimeoutMs));
}

DbusBluezBus::DbusBluezBus(DBusConnection *connection, int callTimeoutMs)
    : connection_(connection), callTimeoutMs_(callTimeoutMs) {}

DbusBluezBus::~DbusBluezBus() {
    cancelAsync();
    dropExport();
    dbus_connection_close(connection_);
    dbus_connection_unref(connection_);
}

//*******************************
// DbusBluezBus::callBlocking
//*******************************
DBusMessage *DbusBluezBus::callBlocking(DBusMessage *message, BusError &error) {
    error.clear();
    if (message == nullptr) {
        outOfMemory(error);
        return nullptr;
    }
    ScopedError e;
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection_, message, callTimeoutMs_, &e.e);
    dbus_message_unref(message);
    if (reply == nullptr) {
        e.take(error);
        if (error.empty())
            error.name = "org.freedesktop.DBus.Error.NoReply";
        return nullptr;
    }
    return reply;
}

//*******************************
// DbusBluezBus::getManagedObjects
//*******************************
bool DbusBluezBus::getManagedObjects(DbusManagedObjects &out, BusError &error) {
    out.clear();
    DBusMessage *reply = callBlocking(
        dbus_message_new_method_call(Bluez, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"), error);
    if (reply == nullptr)
        return false;
    DBusMessageIter it;
    // a{oa{sa{sv}}}
    if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&it, &entry);
        while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter kv;
            dbus_message_iter_recurse(&entry, &kv);
            if (dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_OBJECT_PATH) {
                const char *path = nullptr;
                dbus_message_iter_get_basic(&kv, &path);
                dbus_message_iter_next(&kv);
                if (path != nullptr && dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_ARRAY)
                    readInterfaces(&kv, out[path]);
            }
            dbus_message_iter_next(&entry);
        }
    }
    dbus_message_unref(reply);
    return true;
}

//*******************************
// DbusBluezBus::setBool / call / callWithPath / setDiscoveryFilter
//*******************************
bool DbusBluezBus::setBool(const string &path, const string &interface, const string &property, bool value,
                           BusError &error) {
    DBusMessage *message = dbus_message_new_method_call(Bluez, path.c_str(), "org.freedesktop.DBus.Properties", "Set");
    if (message == nullptr)
        return outOfMemory(error);
    DBusMessageIter it, variant;
    const char *iface = interface.c_str();
    const char *name = property.c_str();
    dbus_bool_t b = value ? TRUE : FALSE;
    dbus_message_iter_init_append(message, &it);
    if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface) ||
        !dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name) ||
        !dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &b) ||
        !dbus_message_iter_close_container(&it, &variant)) {
        dbus_message_unref(message);
        return outOfMemory(error);
    }
    DBusMessage *reply = callBlocking(message, error);
    if (reply == nullptr)
        return false;
    dbus_message_unref(reply);
    return true;
}

bool DbusBluezBus::call(const string &path, const string &interface, const string &method, BusError &error) {
    DBusMessage *reply =
        callBlocking(dbus_message_new_method_call(Bluez, path.c_str(), interface.c_str(), method.c_str()), error);
    if (reply == nullptr)
        return false;
    dbus_message_unref(reply);
    return true;
}

bool DbusBluezBus::callWithPath(const string &path, const string &interface, const string &method,
                                const string &argument, BusError &error) {
    DBusMessage *message = dbus_message_new_method_call(Bluez, path.c_str(), interface.c_str(), method.c_str());
    if (message == nullptr)
        return outOfMemory(error);
    const char *arg = argument.c_str();
    if (!dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &arg, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return outOfMemory(error);
    }
    DBusMessage *reply = callBlocking(message, error);
    if (reply == nullptr)
        return false;
    dbus_message_unref(reply);
    return true;
}

bool DbusBluezBus::setDiscoveryFilter(const string &adapter, const string &transport, BusError &error) {
    DBusMessage *message =
        dbus_message_new_method_call(Bluez, adapter.c_str(), "org.bluez.Adapter1", "SetDiscoveryFilter");
    if (message == nullptr)
        return outOfMemory(error);
    // a{sv} = {"Transport": <s transport>}
    DBusMessageIter it, dict, entry, variant;
    const char *key = "Transport";
    const char *value = transport.c_str();
    dbus_message_iter_init_append(message, &it);
    if (!dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict) ||
        !dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING, &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value) ||
        !dbus_message_iter_close_container(&entry, &variant) || !dbus_message_iter_close_container(&dict, &entry) ||
        !dbus_message_iter_close_container(&it, &dict)) {
        dbus_message_unref(message);
        return outOfMemory(error);
    }
    DBusMessage *reply = callBlocking(message, error);
    if (reply == nullptr)
        return false;
    dbus_message_unref(reply);
    return true;
}

//*******************************
// DbusBluezBus::registerAgent / unregisterAgent / handleAgentMessage
//*******************************
bool DbusBluezBus::registerAgent(const string &agentPath, const string &capability, AgentHandler handler,
                                 BusError &error) {
    error.clear();
    dropExport();
    static const DBusObjectPathVTable vtable = {nullptr, agentCallback, nullptr, nullptr, nullptr, nullptr};
    ScopedError e;
    if (!dbus_connection_try_register_object_path(connection_, agentPath.c_str(), &vtable, this, &e.e)) {
        e.take(error);
        if (error.empty())
            return outOfMemory(error);
        return false;
    }
    agentPath_ = agentPath;
    agentHandler_ = std::move(handler);

    DBusMessage *message = dbus_message_new_method_call(Bluez, AgentManagerPath, AgentManager1, "RegisterAgent");
    if (message == nullptr) {
        dropExport();
        return outOfMemory(error);
    }
    const char *path = agentPath.c_str();
    const char *cap = capability.c_str();
    if (!dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &cap, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        dropExport();
        return outOfMemory(error);
    }
    DBusMessage *reply = callBlocking(message, error);
    if (reply == nullptr) {
        dropExport();
        return false;
    }
    dbus_message_unref(reply);
    return true;
}

bool DbusBluezBus::unregisterAgent(const string &agentPath, BusError &error) {
    bool ok = callWithPath(AgentManagerPath, AgentManager1, "UnregisterAgent", agentPath, error);
    // BlueZ calls Release on the way out; answer it before the object goes
    pump(0);
    dropExport();
    return ok;
}

void DbusBluezBus::dropExport() {
    if (agentPath_.empty())
        return;
    dbus_connection_unregister_object_path(connection_, agentPath_.c_str());
    agentPath_.clear();
    agentHandler_ = nullptr;
}

void DbusBluezBus::handleAgentMessage(DBusMessage *message) {
    AgentRequest request;
    const char *member = dbus_message_get_member(message);
    request.method = member != nullptr ? member : "";
    // (o), (o u), (o s), (o u q) - the device first, then its number or text
    DBusMessageIter it;
    if (dbus_message_iter_init(message, &it)) {
        for (int t = dbus_message_iter_get_arg_type(&it); t != DBUS_TYPE_INVALID;
             dbus_message_iter_next(&it), t = dbus_message_iter_get_arg_type(&it)) {
            if (t == DBUS_TYPE_OBJECT_PATH && request.device.empty()) {
                const char *s = nullptr;
                dbus_message_iter_get_basic(&it, &s);
                request.device = s != nullptr ? s : "";
            } else if (t == DBUS_TYPE_UINT32) {
                dbus_uint32_t v = 0;
                dbus_message_iter_get_basic(&it, &v);
                request.passkey = v;
            } else if (t == DBUS_TYPE_STRING && request.text.empty()) {
                const char *s = nullptr;
                dbus_message_iter_get_basic(&it, &s);
                request.text = s != nullptr ? s : "";
            }
        }
    }
    AgentReply answer = agentHandler_ ? agentHandler_(request) : AgentReply::rejected("no agent");
    DBusMessage *reply = nullptr;
    if (!answer.ok) {
        reply = dbus_message_new_error(message, answer.errorName.c_str(), answer.errorMessage.c_str());
    } else {
        reply = dbus_message_new_method_return(message);
        if (reply != nullptr && request.method == "RequestPinCode") {
            const char *pin = answer.pinCode.c_str();
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
        } else if (reply != nullptr && request.method == "RequestPasskey") {
            dbus_uint32_t passkey = answer.passkey;
            dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
        }
    }
    if (reply == nullptr)
        return;
    if (!dbus_message_get_no_reply(message))
        dbus_connection_send(connection_, reply, nullptr);
    dbus_message_unref(reply);
    dbus_connection_flush(connection_);
}

//*******************************
// DbusBluezBus::startAsync / asyncState / cancelAsync / pump
//*******************************
bool DbusBluezBus::startAsync(const string &path, const string &interface, const string &method, int timeoutMs,
                              BusError &error) {
    error.clear();
    cancelAsync();
    DBusMessage *message = dbus_message_new_method_call(Bluez, path.c_str(), interface.c_str(), method.c_str());
    if (message == nullptr)
        return outOfMemory(error);
    DBusPendingCall *pending = nullptr;
    bool sent = dbus_connection_send_with_reply(connection_, message, &pending, timeoutMs) != 0;
    dbus_message_unref(message);
    if (!sent)
        return outOfMemory(error);
    if (pending == nullptr) {
        error.name = "org.freedesktop.DBus.Error.Disconnected";
        error.message = "the system bus connection is closed";
        return false;
    }
    dbus_connection_flush(connection_);
    pending_ = pending;
    asyncState_ = AsyncState::Pending;
    asyncError_.clear();
    return true;
}

BluezBus::AsyncState DbusBluezBus::asyncState(BusError &error) {
    if (pending_ != nullptr && dbus_pending_call_get_completed(pending_)) {
        DBusMessage *reply = dbus_pending_call_steal_reply(pending_);
        dbus_pending_call_unref(pending_);
        pending_ = nullptr;
        ScopedError e;
        if (reply == nullptr) {
            asyncError_.name = "org.freedesktop.DBus.Error.NoReply";
            asyncState_ = AsyncState::Failed;
        } else if (dbus_set_error_from_message(&e.e, reply)) {
            e.take(asyncError_);
            asyncState_ = AsyncState::Failed;
        } else {
            asyncState_ = AsyncState::Succeeded;
        }
        if (reply != nullptr)
            dbus_message_unref(reply);
    }
    error = asyncError_;
    return asyncState_;
}

void DbusBluezBus::cancelAsync() {
    if (pending_ != nullptr) {
        dbus_pending_call_cancel(pending_);
        dbus_pending_call_unref(pending_);
        pending_ = nullptr;
    }
    asyncState_ = AsyncState::None;
    asyncError_.clear();
}

void DbusBluezBus::pump(int timeoutMs) {
    if (!dbus_connection_read_write_dispatch(connection_, timeoutMs)) {
        // disconnected: nothing will ever arrive - do not let a caller's loop spin
        if (timeoutMs > 0)
            this_thread::sleep_for(chrono::milliseconds(timeoutMs));
        return;
    }
    while (dbus_connection_get_dispatch_status(connection_) == DBUS_DISPATCH_DATA_REMAINS)
        dbus_connection_dispatch(connection_);
}
