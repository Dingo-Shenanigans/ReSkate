#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_native_settings.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_native_settings.h"
#include "Engine/Game/Build/20260929/named_settings.h"
#include "local_user_settings.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <atomic>
#include <cstring>
#include <optional>
#include <string_view>
#include <array>

namespace dingosdk::profile_runtime {
namespace native_types = addr::named_settings;
// Modern DingoUISettingAsset controls use DingoProfileSettingsManager's boxed

// option groups, independently of the legacy DelMar/ECS settings scripts.

NativeSettingsFunctions& native_settings() { static NativeSettingsFunctions f; return f; }

unsigned native_settings_location(std::uintptr_t group) {
    auto& s = local_runtime();
    if (!group || !s.active.load(std::memory_order_acquire)) return 0;
    // Asked on every settings get the game's scripts make: peeked, no system call per read.
    std::uintptr_t manager{}, begin{}, end{}, candidate{};
    if (!memory::peek(s.base + addr::local_native_settings::profile_settings_manager, manager) || !manager ||
        !memory::peek(manager, begin) || !memory::peek(manager + 8, end) ||
        !begin || end < begin || end - begin != 3 * sizeof(std::uintptr_t)) return 0;
    // LocalPerDevice (0) already has working native disk storage. Only the two
    // cloud groups need the offline provider; retain their separate key spaces.
    for (unsigned location : {1u, 2u})
        if (memory::peek(begin + location * 8, candidate) && candidate == group) return location;
    return 0;
}

// Runs on every settings get the game's scripts make: peeked, not read (one ReadProcessMemory
// call per read, ~0.15% of a multiplayer client frame, profiled 2026-10-02).
std::optional<dingosdk::Json> native_setting_json(const NativeSettingValue* input) {
    NativeSettingValue value{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(input), value) || !value.data) return {};
    const auto type = value.type - local_runtime().base;
    switch (type) {
    case native_types::native_bool: {
        std::uint8_t byte{};
        if (memory::peek(reinterpret_cast<std::uintptr_t>(value.data), byte) && byte <= 1) return byte != 0;
        return {};
    }
    case native_types::native_uint32: return native_scalar<std::uint32_t>(value.data);
    case native_types::native_int32: return native_scalar<std::int32_t>(value.data);
    case native_types::native_float32: return native_scalar<float>(value.data);
    case native_types::native_cstring: {
        std::uintptr_t text{};
        if (!memory::peek(reinterpret_cast<std::uintptr_t>(value.data), text)) return {};
        char buffer[4097];
        const auto length = memory::peek_cstring(text, buffer, sizeof(buffer));
        if (length < 0) return {};
        return std::string(buffer, static_cast<std::size_t>(length));
    }
    }
    return {};
}

namespace {
// The saved option for a native setting key, or null: one store lookup per key and profile change,
// cached on the asking thread in a small direct-mapped table (valid until this thread's next call).
// A hit needs the same key address, location and whole key text (keys can share an address and
// their first bytes: CRAS_HasSeen...Tab), with no heap string, store lock or map walk. May throw.
const dingosdk::Json* saved_native_option(unsigned location, const char* key) {
    const auto address = reinterpret_cast<std::uintptr_t>(key);
    char text[256];
    const auto length = memory::peek_cstring(address, text, sizeof(text));
    if (length <= 0) return nullptr;
    const std::string_view name(text, static_cast<std::size_t>(length));
    struct Entry { std::uintptr_t key{}; unsigned location{}; std::uint64_t changes{}; std::string name; std::optional<dingosdk::Json> value; };
    thread_local std::array<Entry, 256> cache;
    const auto changes = profile::Store::changes();
    auto& entry = cache[((address >> 3) ^ (address >> 11) ^ location) & 255];
    if (entry.key != address || entry.location != location || entry.changes != changes || entry.name != name) {
        for (const unsigned char c : name) if (c < 32 || c == 127) return nullptr;
        entry.changes = 0;
        entry.value = local_runtime().store->native_profile_option(location, name);
        entry.name = name;
        entry.key = address;
        entry.location = location;
        entry.changes = changes;
    }
    return entry.value ? &*entry.value : nullptr;
}
}

namespace {
// Options the game keeps on this device (location 0) whose default ReSkate changes. The game
// saves these itself, so nothing is stored here but the fact that the default was applied:
// once per profile, the first time the game asks for the option. After that the option is the
// player's, to switch in the game's own menus.
//   Accessibility_Party_Menu_Narration: on in the game's data, so the party menu was read
//   aloud to every new player until they found the switch.
struct OwnDefault {
    std::string_view key;
    bool value;
    const char* applied; // the ReSkate preference that records it
};
constexpr OwnDefault own_defaults[]{{"Accessibility_Party_Menu_Narration", false, "Defaults.PartyMenuNarration"}};

bool per_device_group(std::uintptr_t group) {
    auto& s = local_runtime();
    std::uintptr_t manager{}, begin{}, end{}, first{};
    return group && s.active.load(std::memory_order_acquire) &&
        memory::peek(s.base + addr::local_native_settings::profile_settings_manager, manager) && manager &&
        memory::peek(manager, begin) && memory::peek(manager + 8, end) && begin && end >= begin &&
        end - begin == 3 * sizeof(std::uintptr_t) && memory::peek(begin, first) && first == group;
}
// Runs on every get of an option outside the cloud groups: the key is compared first, and
// only a key from the table costs anything more.
void apply_own_default(std::uintptr_t group, const char* key, const NativeSettingValue* result) {
    char text[64];
    const auto length = memory::peek_cstring(reinterpret_cast<std::uintptr_t>(key), text, sizeof(text));
    if (length <= 0) return;
    const std::string_view name(text, static_cast<std::size_t>(length));
    for (const auto& own : own_defaults) {
        if (name != own.key) continue;
        if (!per_device_group(group) || local_preference(own.applied).value_or(false)) return;
        NativeSettingValue native{};
        const auto current = native_setting_json(result);
        if (!current || !current->is_boolean() || !memory::peek(reinterpret_cast<std::uintptr_t>(result), native)) return;
        if (current->get<bool>() != own.value && !restore_native_scalar<bool>(group, key, native.type, dingosdk::Json(own.value))) return;
        set_local_preference(own.applied, true);
        dingosdk::logging::event(dingosdk::logging::Channel::settings,
            dingosdk::Json{{"event", "local_native_default_applied"}, {"key", std::string(own.key)}, {"value", own.value}}.dump().c_str());
        return;
    }
}

// The game's camera height button writes UseHighCam (a cloud option, true is the high camera)
// as the opposite of the height it reads. The listener may keep another height
// (first person in the camera cycle, Extension/Skater/client_debug.cpp).
std::atomic<CameraHeightWrite> height_write_listener{};
// True when the game's write of `value` is to be replaced by `high`.
bool replace_camera_height(const char* key, std::uintptr_t type, const NativeSettingValue* value, bool& high) noexcept {
    constexpr std::string_view camera_height_key = "UseHighCam";
    const auto listener = height_write_listener.load(std::memory_order_acquire);
    if (!listener || type != local_runtime().base + native_types::native_bool) return false;
    char text[camera_height_key.size() + 1];
    if (memory::peek_cstring(reinterpret_cast<std::uintptr_t>(key), text, sizeof(text)) !=
            static_cast<std::ptrdiff_t>(camera_height_key.size()) || camera_height_key != text) return false;
    NativeSettingValue native{};
    std::uint8_t byte{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(value), native) ||
        !memory::peek(reinterpret_cast<std::uintptr_t>(native.data), byte) || byte > 1) return false;
    high = listener(byte != 0);
    return high != (byte != 0);
}
}

void set_camera_height_write_listener(CameraHeightWrite listener) noexcept {
    height_write_listener.store(listener, std::memory_order_release);
}

bool restore_native_setting(std::uintptr_t group, const char* key, std::uintptr_t type,
    const dingosdk::Json& saved) {
    const auto base = local_runtime().base;
    switch (type - base) {
    case native_types::native_bool: return restore_native_scalar<bool>(group, key, type, saved);
    case native_types::native_uint32: return restore_native_scalar<std::uint32_t>(group, key, type, saved);
    case native_types::native_int32: return restore_native_scalar<std::int32_t>(group, key, type, saved);
    case native_types::native_float32: return restore_native_scalar<float>(group, key, type, saved);
    case native_types::native_cstring: {
        if (!saved.is_string()) return false;
        const auto& value = saved.string();
        // CString cloning needs engine-owned storage, not a std::string buffer.
        const char* text = reinterpret_cast<const char*>(base + addr::engine::empty_cstring);
        user_values().assign_string(&text, value.c_str(), static_cast<std::uint32_t>(value.size()));
        struct Release { const char** text; ~Release() { local_runtime().destroy_string(text); } } release{&text};
        const NativeSettingValue borrowed{type, &text};
        return native_settings().set(group, key, type, &borrowed);
    }
    }
    return false;
}

const NativeSettingValue* native_setting_get(std::uintptr_t group, const char* key) {
    auto* result = native_settings().get(group, key);
    PreserveError preserve;
    try {
        const auto location = native_settings_location(group);
        if (!location) {
            apply_own_default(group, key, result);
            return result;
        }
        const auto* saved = saved_native_option(location, key);
        if (!saved) return result;
        const auto current = native_setting_json(result);
        if (!current || *current == *saved) return result;
        // Copied before the restore: a get it causes on this thread reuses the cache.
        const auto wanted = *saved;
        std::string name;
        (void)identifier(&key, name);
        NativeSettingValue native{};
        if (memory::peek(reinterpret_cast<std::uintptr_t>(result), native) && restore_native_setting(group, key, native.type, wanted))
            dingosdk::logging::event(dingosdk::logging::Channel::settings, dingosdk::Json{{"event","local_native_setting_restored"},{"location",location},{"key",name}}.dump().c_str());
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::settings, "{\"event\":\"local_native_setting_restore_failed\"}"); }
    return result;
}

bool native_setting_set(std::uintptr_t group, const char* key, std::uintptr_t type, const NativeSettingValue* value) {
    bool high{};
    const NativeSettingValue kept{type, &high};
    // The original setter clones the boxed value, so `kept` only needs to outlive the call.
    if (replace_camera_height(key, type, value, high)) value = &kept;
    const bool changed = native_settings().set(group, key, type, value);
    PreserveError preserve;
    try {
        const auto location = native_settings_location(group); std::string id;
        if (changed && location && identifier(&key, id)) {
            // Read back the accepted native value, including type validation.
            if (const auto saved = native_setting_json(native_settings().get(group, key))) {
                local_runtime().store->set_native_profile_option(location, id, *saved);
                dingosdk::logging::event(dingosdk::logging::Channel::settings, dingosdk::Json{{"event","local_native_setting_saved"},{"location",location},{"key",id}}.dump().c_str());
            }
        }
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::settings, "{\"event\":\"local_native_setting_save_failed\"}"); }
    return changed;
}
}