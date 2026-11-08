#include "Engine/Core/Log/logging.h"
#include "runtime_internal.h"
#include "Extension/Rendering/replay_export.h"
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

// One of the game's own gameplay options as the local profile holds it (local_profile_runtime.h).
std::optional<double> local_game_option(std::string_view key) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store || !s.active.load(std::memory_order_acquire)) return {};
        const auto saved = s.store->user_value(key);
        if (!saved) return {};
        if (saved->is_boolean()) return saved->get<bool>() ? 1.0 : 0.0;
        if (saved->is_number()) return saved->get<double>();
    } catch (...) {}
    return {};
}

bool set_local_game_option(std::string_view key, double value, bool boolean) noexcept {
    try {
        auto& s = local_runtime();
        if (!s.store || !s.active.load(std::memory_order_acquire)) return false;
        // A value keeps the type the game saved it with (its getters answer only with a value of their own type).
        const auto saved = s.store->user_value(key);
        if (boolean || (saved && saved->is_boolean())) s.store->set_user_value(key, value != 0);
        else if (saved && saved->is_number_float()) s.store->set_user_value(key, value);
        else s.store->set_user_value(key, static_cast<std::int64_t>(value));
        return true;
    } catch (...) { return false; }
}

// Shared with the overlay through the model; clamped to a legible range. One scale serves both of the
// menu's layouts (ReSkate.MenuScale).
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

namespace {
// A saved number of the menu's (its layout, the sheet's width, the window's size), read once and then
// served from `cache` (below zero: not read yet). Out of range reads as `fallback`.
float menu_number(std::atomic<float>& cache, const char* key, float fallback, float low, float high) noexcept {
    if (const auto value = cache.load(std::memory_order_relaxed); value >= 0) return value;
    float value = fallback;
    try {
        auto& s = local_runtime();
        if (s.store)
            if (const auto saved = s.store->user_value(key); saved && saved->is_number()) {
                const auto read = static_cast<float>(saved->get<double>());
                if (read >= low && read <= high) value = read;
            }
    } catch (...) { /* An unreadable preference just means the default. */ }
    cache.store(value, std::memory_order_relaxed);
    return value;
}
void set_menu_number(std::atomic<float>& cache, const char* key, float value) noexcept {
    cache.store(value, std::memory_order_relaxed);
    try {
        auto& s = local_runtime();
        if (s.store) s.store->set_user_value(key, static_cast<double>(value));
    } catch (...) { /* Saving is best effort; the runtime value already applied. */ }
}
std::atomic<float>& menu_layout_cache() noexcept { static std::atomic<float> value{-1}; return value; }
std::atomic<float>& park_editor_layout_cache() noexcept { static std::atomic<float> value{-1}; return value; }
std::atomic<float>& menu_sheet_width_cache() noexcept { static std::atomic<float> value{-1}; return value; }
std::atomic<float>& menu_hub_width_cache() noexcept { static std::atomic<float> value{-1}; return value; }
std::atomic<float>& menu_hub_height_cache() noexcept { static std::atomic<float> value{-1}; return value; }
std::atomic<float>& replay_export_quality_cache() noexcept { static std::atomic<float> value{-1}; return value; }
}

int local_menu_layout() noexcept {
    return static_cast<int>(menu_number(menu_layout_cache(), "ReSkate.MenuLayout", static_cast<float>(menu_layout_sheet),
                                        static_cast<float>(menu_layout_sheet), static_cast<float>(menu_layout_hub)));
}

bool set_local_menu_layout(int layout) noexcept {
    if (layout != menu_layout_sheet && layout != menu_layout_hub) return false;
    set_menu_number(menu_layout_cache(), "ReSkate.MenuLayout", static_cast<float>(layout));
    return true;
}

int local_park_editor_layout() noexcept {
    return static_cast<int>(menu_number(park_editor_layout_cache(), "ReSkate.ParkEditorLayout",
                                        static_cast<float>(park_editor_layout_hotbar),
                                        static_cast<float>(park_editor_layout_hotbar),
                                        static_cast<float>(park_editor_layout_rail)));
}

bool set_local_park_editor_layout(int layout) noexcept {
    if (layout != park_editor_layout_hotbar && layout != park_editor_layout_rail) return false;
    set_menu_number(park_editor_layout_cache(), "ReSkate.ParkEditorLayout", static_cast<float>(layout));
    return true;
}

// 0 is the default size: what an unsaved or reset size reads as, so the default can change later.
float local_menu_sheet_width() noexcept {
    return menu_number(menu_sheet_width_cache(), "ReSkate.MenuSheetWidth", 0, min_menu_sheet_width, max_menu_sheet_width);
}

bool set_local_menu_sheet_width(float width) noexcept {
    if (width != 0 && !(width >= min_menu_sheet_width && width <= max_menu_sheet_width)) return false;
    set_menu_number(menu_sheet_width_cache(), "ReSkate.MenuSheetWidth", width);
    return true;
}

std::pair<float, float> local_menu_hub_size() noexcept {
    const float width = menu_number(menu_hub_width_cache(), "ReSkate.MenuHubWidth", 0, min_menu_hub_width, max_menu_hub_side);
    const float height = menu_number(menu_hub_height_cache(), "ReSkate.MenuHubHeight", 0, min_menu_hub_height, max_menu_hub_side);
    // Half a saved size is no size: both or the default.
    return width > 0 && height > 0 ? std::pair{width, height} : std::pair{0.0f, 0.0f};
}

bool set_local_menu_hub_size(float width, float height) noexcept {
    const bool reset = width == 0 && height == 0;
    if (!reset && !(width >= min_menu_hub_width && width <= max_menu_hub_side && height >= min_menu_hub_height && height <= max_menu_hub_side))
        return false;
    set_menu_number(menu_hub_width_cache(), "ReSkate.MenuHubWidth", width);
    set_menu_number(menu_hub_height_cache(), "ReSkate.MenuHubHeight", height);
    return true;
}

// Saved as a number like the menu's: the setting's place in replay_export::Quality.
int local_replay_export_quality() noexcept {
    constexpr auto none = static_cast<float>(replay_export::quality_game), best = static_cast<float>(replay_export::quality_count - 1);
    return static_cast<int>(menu_number(replay_export_quality_cache(), "ReSkate.ReplayExportQuality", none, none, best));
}

bool set_local_replay_export_quality(int quality) noexcept {
    if (quality < replay_export::quality_game || quality >= replay_export::quality_count) return false;
    set_menu_number(replay_export_quality_cache(), "ReSkate.ReplayExportQuality", static_cast<float>(quality));
    return true;
}
} // namespace profile_runtime
}
