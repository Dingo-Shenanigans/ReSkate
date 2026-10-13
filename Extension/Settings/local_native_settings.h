#pragma once
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
struct NativeSettingValue { std::uintptr_t type; const void* data; };

static_assert(sizeof(NativeSettingValue) == 16);

struct NativeSettingsFunctions {
    const NativeSettingValue* (*get)(std::uintptr_t, const char*){};
    bool (*set)(std::uintptr_t, const char*, std::uintptr_t, const NativeSettingValue*){};
};

NativeSettingsFunctions& native_settings();

unsigned native_settings_location(std::uintptr_t group);

template<class T> std::optional<dingosdk::Json> native_scalar(const void* data) {
    T value{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(data), value)) return {};
    if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(value)) return {};
    if constexpr (std::is_same_v<T, std::uint64_t>) if (value > INT64_MAX) return {};
    return dingosdk::Json(value);
}

std::optional<dingosdk::Json> native_setting_json(const NativeSettingValue* input);

template<class T> bool restore_native_scalar(std::uintptr_t group, const char* key,
    std::uintptr_t type, const dingosdk::Json& saved) {
    if constexpr (std::is_same_v<T, bool>) { if (!saved.is_boolean()) return false; }
    else if constexpr (std::is_integral_v<T>) {
        if (!saved.is_number_integer() || saved < (std::numeric_limits<T>::lowest)() ||
            saved > (std::numeric_limits<T>::max)()) return false;
    } else {
        if (!saved.is_number_float() || !std::isfinite(saved.get<double>()) ||
            std::abs(saved.get<double>()) > (std::numeric_limits<T>::max)()) return false;
    }
    const T value = saved.get<T>();
    const NativeSettingValue borrowed{type, &value};
    // The original setter clones the boxed value into its existing node.
    return native_settings().set(group, key, type, &borrowed);
}

bool restore_native_setting(std::uintptr_t group, const char* key, std::uintptr_t type,
    const dingosdk::Json& saved);

// Apply a persisted cloud-profile option to the live settings group now. The
// normal get hook remains the fallback for settings changed before game state
// is ready.
bool apply_native_profile_option(unsigned location, std::string_view key,
    const dingosdk::Json& value) noexcept;

const NativeSettingValue* native_setting_get(std::uintptr_t group, const char* key);

bool native_setting_set(std::uintptr_t group, const char* key, std::uintptr_t type, const NativeSettingValue* value);
}
