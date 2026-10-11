#include "Engine/Core/Log/logging.h"
#include "runtime_internal.h"

namespace dingosdk {
using namespace profile_runtime;
namespace profile_runtime { std::string binding_feedback; }
ControllerBindingsModel local_profile_controller_bindings() {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    ControllerBindingsModel result;
    result.status = binding_feedback;
    if (!s.active || !s.store) return result;
    result.freecam_controller = s.store->freecam_controller();
    result.freecam_controller_combo = s.store->freecam_controller_binding();
    result.freecam_combo = s.store->freecam_binding();
    result.tp_to_freecam_combo = s.store->tp_to_freecam_binding();
    result.noclip_combo = s.store->noclip_binding();
    result.forward_velocity_combo = s.store->forward_velocity_binding();
    result.up_velocity_combo = s.store->up_velocity_binding();
    result.offboard_up_velocity_combo = s.store->offboard_up_velocity_binding();
    result.vote_yes_combo = s.store->vote_binding(true);
    result.vote_no_combo = s.store->vote_binding(false);
    for (std::size_t i = 0; i < action_binds.size(); ++i) result.action_combos[i] = s.store->action_binding(action_binds[i].key);
    result.available = true;
    return result;
}
bool set_local_noclip_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_noclip_binding(combo);
        binding_feedback = combo ? "Noclip binding saved." : "Noclip binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","noclip"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool local_freecam_controller() {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    return s.active && s.store && s.store->freecam_controller();
}
bool set_local_freecam_controller(bool value) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store) return false;
    try {
        s.store->save_freecam_controller(value);
        return true;
    } catch (...) { return false; }
}

bool set_local_freecam_controller_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_freecam_controller_binding(combo);
        binding_feedback = combo ? "Freecam Controller binding saved." : "Freecam Controller binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","freecam_controller"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_freecam_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_freecam_binding(combo);
        binding_feedback = combo ? "Freecam binding saved." : "Freecam binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","freecam"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_action_binding(std::size_t index, std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || index >= action_binds.size() || !valid_action_binding(combo)) return false;
    try {
        const auto& slot = action_binds[index];
        s.store->save_action_binding(slot.key, combo);
        binding_feedback = std::string(slot.label) + (combo ? " binding saved." : " binding cleared.");
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action",std::string(slot.key)},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_vote_binding(bool yes, std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_vote_binding(yes, combo);
        binding_feedback = std::string(yes ? "Vote yes" : "Vote no") + (combo ? " binding saved." : " binding cleared.");
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action",yes ? "vote_yes" : "vote_no"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_tp_to_freecam_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_tp_to_freecam_binding(combo);
        binding_feedback = combo ? "TP to Freecam binding saved." : "TP to Freecam binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","tp_to_freecam"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_forward_velocity_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_forward_velocity_binding(combo);
        binding_feedback = combo ? "Forward Boost binding saved." : "Forward Boost binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","forward_velocity"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_offboard_up_velocity_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_offboard_up_velocity_binding(combo);
        binding_feedback = combo ? "Off-board Up Boost binding saved." : "Off-board Up Boost binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","offboard_up_velocity"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

bool set_local_up_velocity_binding(std::uint32_t combo) {
    auto& s = local_runtime(); std::lock_guard lock(s.native_mutex);
    if (!s.active || !s.store || !valid_action_binding(combo)) return false;
    try {
        s.store->save_up_velocity_binding(combo);
        binding_feedback = combo ? "Up Boost binding saved." : "Up Boost binding cleared.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","controller_binding_saved"},{"action","up_velocity"},{"combo",combo}}.dump().c_str());
        return true;
    } catch (...) { binding_feedback = "Could not save the binding. See the console log."; return false; }
}

namespace profile_runtime {
std::optional<bool> local_preference(std::string_view key) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store) return {};
        const auto value = s.store->user_value("ReSkate." + std::string(key));
        if (value && value->is_boolean()) return value->get<bool>();
    } catch (...) { /* A preference is never worth failing a caller over. */ }
    return {};
}

void set_local_preference(std::string_view key, bool value) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store) {
            dingosdk::logging::event(dingosdk::logging::Channel::profile,
                "{\"event\":\"local_preference_save_failed\",\"reason\":\"profile_unavailable\"}");
            return;
        }
        const auto full_key = "ReSkate." + std::string(key);
        s.store->set_user_value(full_key, value);
        const auto saved = s.store->user_value(full_key);
        if (!saved || !saved->is_boolean() || saved->get<bool>() != value) {
            dingosdk::logging::event(dingosdk::logging::Channel::profile,
                dingosdk::Json{{"event", "local_preference_save_failed"},
                    {"key", full_key}, {"reason", "readback_mismatch"}}.dump().c_str());
            return;
        }
        dingosdk::logging::event(dingosdk::logging::Channel::profile,
            dingosdk::Json{{"event", "local_preference_saved"},
                {"key", full_key}, {"value", value}}.dump().c_str());
    } catch (...) {
        dingosdk::logging::event(dingosdk::logging::Channel::profile,
            "{\"event\":\"local_preference_save_failed\",\"reason\":\"exception\"}");
    }
}

std::optional<Json> local_value(std::string_view key) noexcept {
    try {
        auto& s = local_runtime();
        if (s.store) return s.store->user_value("ReSkate." + std::string(key));
    } catch (...) { /* An unreadable value just means the default. */ }
    return {};
}

void set_local_values(const std::vector<std::pair<std::string, Json>>& values) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store) return;
        std::vector<std::pair<std::string, Json>> prefixed;
        prefixed.reserve(values.size());
        for (const auto& [key, value] : values) prefixed.emplace_back("ReSkate." + key, value);
        s.store->set_user_values(prefixed);
    } catch (...) { /* Saving is best effort; the runtime values already applied. */ }
}

// Shared with the overlay through the model; clamped to a legible range.
std::atomic<float>& menu_scale_cache() noexcept { static std::atomic<float> value{0}; return value; }

float local_menu_scale() noexcept {
    auto& cache = menu_scale_cache();
    if (const auto value = cache.load(std::memory_order_relaxed); value > 0) return value;
    float value = default_menu_scale;
    try {
        auto& s = local_runtime();
        if (s.store)
            if (const auto saved = s.store->user_value("ReSkate.MenuScale"); saved && saved->is_number())
                value = std::clamp(static_cast<float>(saved->get<double>()), min_menu_scale, max_menu_scale);
    } catch (...) { /* An unreadable preference just means the default scale. */ }
    cache.store(value, std::memory_order_relaxed);
    return value;
}

bool set_local_menu_scale(float value) noexcept {
    if (!(value >= min_menu_scale && value <= max_menu_scale)) return false;
    menu_scale_cache().store(value, std::memory_order_relaxed);
    try {
        auto& s = local_runtime();
        if (s.store) s.store->set_user_value("ReSkate.MenuScale", static_cast<double>(value));
    } catch (...) { /* Saving is best effort; the runtime value already applied. */ }
    return true;
}
} // namespace profile_runtime
}
