#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_challenge_runtime.h"
#include "local_user_settings.h"

namespace dingosdk::profile_runtime {
// Gameplay settings use the typed user-value service directly, bypassing the

// Boolean expression wrappers. Keep its native error/callback ownership contract.

UserValueFunctions& user_values() { static UserValueFunctions f; return f; }

void* user_get_bool(std::uintptr_t s, void* e, const void* k, void* o) { return user_value_get<bool>(s,e,k,o,user_values().get_bool); }

void* user_get_number(std::uintptr_t s, void* e, const void* k, void* o) { return user_value_get<double>(s,e,k,o,user_values().get_number); }

void* user_get_integer(std::uintptr_t s, void* e, const void* k, void* o) { return user_value_get<std::int64_t>(s,e,k,o,user_values().get_integer); }

void* user_get_string(std::uintptr_t s, void* e, const void* k, void* o) { return user_value_get<std::string>(s,e,k,o,user_values().get_string); }

bool user_value_save(std::uintptr_t service, const void* key, const dingosdk::Json& value, const void* callback) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return false;
    PreserveError preserve;
    std::uintptr_t dispatcher{};
    if (!read(service + 0x158, dispatcher) || !dispatcher) return false;
    bool saved{};
    std::string id;
    try {
        if (!identifier(key, id)) throw std::runtime_error("Invalid user setting key");
        s.store->set_user_value(id, value);
        saved = true;
        dingosdk::logging::event(dingosdk::logging::Channel::settings, dingosdk::Json{{"event", "local_user_value_saved"}, {"key", id}}.dump().c_str());
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::settings, "{\"event\":\"local_user_value_save_failed\",\"acknowledged\":false}"); }
    alignas(8) std::array<std::byte, 40> error{};
    if (saved) s.success(error.data());
    else challenge_runtime().f.error(error.data(), 0xdc41216b, "Could not save the local setting");
    struct Release { void* error; ~Release() { local_runtime().destroy_string(error); } } release{error.data()};
    // Native success moves the error CString into a queued callback; failure
    // copies it. Destroying the remaining CString once is correct in both cases.
    (saved ? user_values().success_callback : user_values().error_callback)(dispatcher, callback, error.data());
    return true;
}

void user_set_bool(std::uintptr_t s, const void* k, bool v, const void* c) {
    if (!user_value_save(s,k,v,c)) user_values().set_bool(s,k,v,c);
}

void user_set_number(std::uintptr_t s, const void* k, double v, const void* c) {
    if (!user_value_save(s,k,v,c)) user_values().set_number(s,k,v,c);
}

void user_set_integer(std::uintptr_t s, const void* k, std::int64_t v, const void* c) {
    if (!user_value_save(s,k,v,c)) user_values().set_integer(s,k,v,c);
}

bool user_text_value(std::uintptr_t s, const void* k, const char* text, const void* c) {
    if (!local_runtime().active.load(std::memory_order_acquire)) return false;
    std::string value;
    for (std::size_t i = 0; i <= 4096; ++i) {
        char ch{};
        if (!read(reinterpret_cast<std::uintptr_t>(text) + i, ch)) return user_value_save(s,k,nullptr,c);
        if (!ch) return user_value_save(s,k,value,c);
        value += ch;
    }
    return user_value_save(s,k,nullptr,c);
}

void user_set_text(std::uintptr_t s, const void* k, const char* v, const void* c) {
    if (!user_text_value(s,k,v,c)) user_values().set_text(s,k,v,c);
}

void user_set_string(std::uintptr_t s, const void* k, const void* v, const void* c) {
    std::uintptr_t text{};
    if (local_runtime().active.load(std::memory_order_acquire) && read(reinterpret_cast<std::uintptr_t>(v), text) &&
        user_text_value(s,k,reinterpret_cast<const char*>(text),c)) return;
    user_values().set_string(s,k,v,c);
}
}