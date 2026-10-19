#include "Engine/Core/Log/logging.h"
#include "runtime_internal.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

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
bool local_profile_store_ready() noexcept {
    try { return local_runtime().store != nullptr; } catch (...) { return false; }
}

namespace {
// ReSkate's own preferences and values are saved on a thread of their own, in the order they were
// set. They are set from the game's threads: a menu click on the render thread, a console command
// or a session's slider on the client thread, and a save (a database transaction) held that
// frame up. Until one is saved, it is what reads of it give.
struct PendingValues {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::vector<std::pair<std::string, Json>>> queue;
    std::map<std::string, Json, std::less<>> latest; // set and not saved yet, by full key
    bool started{};
};
PendingValues& pending_values() {
    static auto* pending = new PendingValues; // process lifetime, as its writer
    return *pending;
}
void save_values(std::vector<std::pair<std::string, Json>> values) {
    auto& s = local_runtime();
    try {
        s.store->set_user_values(values);
        for (const auto& [key, value] : values) {
            if (!value.is_boolean()) continue;
            const auto saved = s.store->user_value(key);
            if (!saved || !saved->is_boolean() || saved->get<bool>() != value.get<bool>())
                dingosdk::logging::event(dingosdk::logging::Channel::profile,
                    dingosdk::Json{{"event", "local_preference_save_failed"}, {"key", key}, {"reason", "readback_mismatch"}}.dump().c_str());
            else
                dingosdk::logging::event(dingosdk::logging::Channel::profile,
                    dingosdk::Json{{"event", "local_preference_saved"}, {"key", key}, {"value", value.get<bool>()}}.dump().c_str());
        }
    } catch (...) {
        dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_preference_save_failed\",\"reason\":\"exception\"}");
    }
}
void writer() {
    auto& p = pending_values();
    for (;;) {
        std::vector<std::pair<std::string, Json>> values;
        {
            std::unique_lock lock(p.mutex);
            p.wake.wait(lock, [&] { return !p.queue.empty(); });
            values = std::move(p.queue.front());
            p.queue.pop_front();
        }
        save_values(values);
        std::lock_guard lock(p.mutex);
        // (A value set again meanwhile is still waiting: it stays.)
        for (const auto& [key, value] : values)
            if (const auto found = p.latest.find(key); found != p.latest.end() && found->second == value) p.latest.erase(found);
    }
}
// Full keys ("ReSkate." ...). Saved in place when no writer could be started.
void queue_values(std::vector<std::pair<std::string, Json>> values) {
    auto& p = pending_values();
    {
        std::lock_guard lock(p.mutex);
        if (!p.started) {
            try {
                std::thread(writer).detach();
                p.started = true;
            } catch (...) {}
        }
        if (p.started) {
            for (const auto& [key, value] : values) p.latest[key] = value;
            p.queue.push_back(std::move(values));
            p.wake.notify_one();
            return;
        }
    }
    save_values(std::move(values));
}
std::optional<Json> pending_value(std::string_view full_key) {
    auto& p = pending_values();
    std::lock_guard lock(p.mutex);
    if (const auto found = p.latest.find(full_key); found != p.latest.end()) return found->second;
    return {};
}
} // namespace

std::optional<bool> local_preference(std::string_view key) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store) return {};
        const auto full_key = "ReSkate." + std::string(key);
        if (const auto waiting = pending_value(full_key); waiting && waiting->is_boolean()) return waiting->get<bool>();
        const auto value = s.store->user_value(full_key);
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
        queue_values({{"ReSkate." + std::string(key), Json(value)}});
    } catch (...) {
        dingosdk::logging::event(dingosdk::logging::Channel::profile,
            "{\"event\":\"local_preference_save_failed\",\"reason\":\"exception\"}");
    }
}

std::optional<Json> local_value(std::string_view key) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store) return {};
        const auto full_key = "ReSkate." + std::string(key);
        if (auto waiting = pending_value(full_key)) return waiting;
        return s.store->user_value(full_key);
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
        queue_values(std::move(prefixed));
    } catch (...) { /* Saving is best effort; the runtime values already applied. */ }
}

// Shared with the overlay through the model; clamped to a legible range. The window and the full
// screen menu each keep their own (ReSkate.MenuScale, ReSkate.MenuScaleFullscreen).
std::atomic<float>& menu_scale_cache(bool fullscreen) noexcept {
    static std::atomic<float> window{0}, full{0};
    return fullscreen ? full : window;
}
const char* menu_scale_key(bool fullscreen) noexcept { return fullscreen ? "ReSkate.MenuScaleFullscreen" : "ReSkate.MenuScale"; }

float local_menu_scale(bool fullscreen) noexcept {
    auto& cache = menu_scale_cache(fullscreen);
    if (const auto value = cache.load(std::memory_order_relaxed); value > 0) return value;
    float value = default_menu_scale;
    try {
        auto& s = local_runtime();
        if (s.store)
            if (const auto saved = s.store->user_value(menu_scale_key(fullscreen)); saved && saved->is_number())
                value = std::clamp(static_cast<float>(saved->get<double>()), min_menu_scale, max_menu_scale);
    } catch (...) { /* An unreadable preference just means the default scale. */ }
    cache.store(value, std::memory_order_relaxed);
    return value;
}

std::atomic<int>& menu_fullscreen_cache() noexcept { static std::atomic<int> value{-1}; return value; }

bool local_menu_fullscreen() noexcept {
    auto& cache = menu_fullscreen_cache();
    if (const auto value = cache.load(std::memory_order_relaxed); value >= 0) return value != 0;
    bool value = false;
    try {
        auto& s = local_runtime();
        if (s.store)
            if (const auto saved = s.store->user_value("ReSkate.MenuFullscreen"); saved && saved->is_boolean())
                value = saved->get<bool>();
    } catch (...) { /* An unreadable preference just means a window. */ }
    cache.store(value ? 1 : 0, std::memory_order_relaxed);
    return value;
}

void set_local_menu_fullscreen(bool fullscreen) noexcept {
    menu_fullscreen_cache().store(fullscreen ? 1 : 0, std::memory_order_relaxed);
    try {
        auto& s = local_runtime();
        if (s.store) s.store->set_user_value("ReSkate.MenuFullscreen", fullscreen);
    } catch (...) { /* Saving is best effort; the runtime value already applied. */ }
}

bool set_local_menu_scale(float value, bool fullscreen) noexcept {
    if (!(value >= min_menu_scale && value <= max_menu_scale)) return false;
    menu_scale_cache(fullscreen).store(value, std::memory_order_relaxed);
    try {
        auto& s = local_runtime();
        if (s.store) s.store->set_user_value(menu_scale_key(fullscreen), static_cast<double>(value));
    } catch (...) { /* Saving is best effort; the runtime value already applied. */ }
    return true;
}
} // namespace profile_runtime
}
