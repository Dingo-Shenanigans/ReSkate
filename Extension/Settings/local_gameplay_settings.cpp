#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "local_gameplay_settings.h"
#include "local_user_settings.h"
#include "Extension/Trainer/trainer_gamestate.h"
#include <optional>
#include <string_view>
#include <array>
#include <atomic>

namespace dingosdk::profile_runtime {
// Applied gameplay values live in an ECS cache even when the user-value

// service does not exist. Hook the applied cache, never the pending UI edits.

// See analysis/native-game-settings.md for the two distinct native boundaries.

GameplaySettingFunctions& gameplay_settings() { static GameplaySettingFunctions f; return f; }

// UI edits and repeated backend initialization share the same setters. Only

// the reviewed menu actions authorize a durable edit; initialization restores.

unsigned gameplay_expression_kind(std::uintptr_t vm) {
    std::uintptr_t instance{}, resource{}, current{}; std::uint32_t key{};
    std::array<std::uint32_t, 10> layout{};
    if (!vm || !read(vm + 0x30, instance) || !read(instance, resource) ||
        !read(vm + 0x38, current) || current != resource || !read(resource + 0x10, key) ||
        !read(resource + 0x20, layout)) return 0;
    if ((key == 0x6777763e && layout == std::array<std::uint32_t, 10>{112,2696,1010,2329,11,0,35,1638514,6881314,66563}) ||
        (key == 0xff4a234e && layout == std::array<std::uint32_t, 10>{80,416,184,359,6,0,6,327698,2949131,1}) ||
        (key == 0xff2662d5 && layout == std::array<std::uint32_t, 10>{96,2000,661,1338,6,0,23,983108,5046294,2}) ||
        (key == 0x82c1331d && layout == std::array<std::uint32_t, 10>{64,496,202,472,9,0,5,28,1507339,66305}) ||
        (key == 0x6e9f5108 && layout == std::array<std::uint32_t, 10>{96,1928,746,1444,11,0,14,1638495,7208982,17041665})) return 1;
    if ((key == 0x7a55b1d7 && layout == std::array<std::uint32_t, 10>{224,5624,721,2953,19,0,17,2293843,6488140,66561}) ||
        (key == 0x375c4ce9 && layout == std::array<std::uint32_t, 10>{112,1552,1014,2506,33,0,7,983209,8454179,264449})) return 2;
    return 0;
}

bool gameplay_user_edit() {
    if (gameplay_expression_kind(executing_expression) == 1) return true;
    auto* scope = expression_scope;
    for (unsigned depth = 0; scope && depth < 32; ++depth, scope = scope->parent)
        if (scope->vm != executing_expression && gameplay_expression_kind(scope->vm) == 1) return true;
    return false;
}

bool gameplay_setting_key(const void* reference, std::string& key) {
    std::uintptr_t asset{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(reference), asset)) return false; // every script settings get
    asset &= ~std::uintptr_t{4};
    return asset && identifier(reinterpret_cast<const void*>(asset + 0x60), key);
}

// Every script settings get the game makes (several a frame from its expressions) asks this. One
// store lookup per setting asset and profile change, cached on the asking thread in a small
// direct-mapped table: a hit is two peeks, a copy of the name and compares, with no heap string,
// store lock or map walk (~0.5% of a multiplayer client frame before, profiled 2026-10-02). A hit
// needs the same asset and the same whole name, so a reused address cannot answer for another
// setting (many share their first bytes: egf_enable..., egf_exp_...).
const dingosdk::Json* saved_gameplay_json(const void* reference) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return nullptr;
    std::uintptr_t asset{}, name{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(reference), asset)) return nullptr;
    asset &= ~std::uintptr_t{4};
    if (!asset || !memory::peek(asset + 0x60, name) || !name) return nullptr;
    char text[256];
    const auto length = memory::peek_cstring(name, text, sizeof(text));
    if (length <= 0) return nullptr;
    const std::string_view key(text, static_cast<std::size_t>(length));
    struct Entry { std::uintptr_t asset{}; std::uint64_t changes{}; std::string key; std::optional<dingosdk::Json> value; };
    thread_local std::array<Entry, 256> cache;
    const auto changes = profile::Store::changes();
    auto& entry = cache[((asset >> 4) ^ (asset >> 12)) & 255];
    if (entry.asset != asset || entry.changes != changes || entry.key != key) {
        for (const unsigned char c : key) if (c < 32 || c == 127) return nullptr;
        entry.changes = 0;
        entry.value = s.store->user_value(key);
        entry.key = key;
        entry.asset = asset;
        entry.changes = changes;
    }
    return entry.value ? &*entry.value : nullptr;
}

void save_gameplay_setting(const void* reference, const dingosdk::Json& value) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try {
        std::string key;
        if (!gameplay_setting_key(reference, key)) return;
        if (s.store->user_value(key) == value) return;
        s.store->set_user_value(key, value);
        dingosdk::logging::event(dingosdk::logging::Channel::settings, dingosdk::Json{{"event", "local_gameplay_setting_saved"}, {"key", key}}.dump().c_str());
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::settings, "{\"event\":\"local_gameplay_setting_save_failed\"}"); }
}

// The player's own stance (Gameplay Preferences' Default Stance Is Goofy), seen as the game reads
// or changes it: a plugin's flick pictures follow it. Its setting is found once by its key
// (one naming both "stance" and "goofy"); every other setting asset is checked once per thread.
namespace {
std::atomic<std::uintptr_t> stance_asset{};
std::atomic<int> stance_value{-1};
void note_stance(const void* reference, bool value) noexcept {
    std::uintptr_t asset{};
    if (!reference || !memory::peek(reinterpret_cast<std::uintptr_t>(reference), asset)) return;
    asset &= ~std::uintptr_t{4};
    if (!asset) return;
    if (const auto known = stance_asset.load(std::memory_order_relaxed); known != asset) {
        if (known) return;
        thread_local std::array<std::uintptr_t, 256> checked{};
        auto& slot = checked[((asset >> 4) ^ (asset >> 12)) & 255];
        if (slot == asset) return;
        slot = asset;
        PreserveError preserve;
        try {
            std::string key;
            if (!gameplay_setting_key(reference, key)) return;
            std::string lower;
            for (const unsigned char c : key) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            if (lower.find("goofy") == std::string::npos || lower.find("stance") == std::string::npos) return;
            stance_asset.store(asset, std::memory_order_relaxed);
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::settings,
                                   "Player stance: read from the game's \"{}\" setting.", key);
        } catch (...) { return; }
    }
    if (stance_value.exchange(value ? 1 : 0, std::memory_order_relaxed) != (value ? 1 : 0))
        dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::settings,
                               "Player stance: {}.", value ? "goofy" : "regular");
}
}

std::optional<bool> game_stance_goofy() noexcept {
    const auto value = stance_value.load(std::memory_order_relaxed);
    if (value < 0) return {};
    return value == 1;
}

bool gameplay_get_bool(std::uintptr_t m, const void* a) {
    // The trainer's grind settings ("Grinds feel like Skate 3"): forced for as long as the trainer holds them.
    // Never to the game's own Settings screen: it shows, and would save, the player's own value.
    if (const auto forced = dingosdk::trainer::forced_gameplay_bool(a); forced && !gameplay_user_edit()) {
        note_stance(a, *forced);
        return *forced;
    }
    if (const auto value = saved_gameplay_setting<bool>(a)) {
        note_stance(a, *value);
        return *value;
    }
    const bool value = gameplay_settings().get_bool(m, a);
    note_stance(a, value);
    return value;
}

float gameplay_get_number(std::uintptr_t m, const void* a) {
    if (const auto value = saved_gameplay_setting<float>(a)) return *value;
    return gameplay_settings().get_number(m, a);
}

std::int32_t gameplay_get_integer(std::uintptr_t m, const void* a) {
    if (const auto value = saved_gameplay_setting<std::int32_t>(a)) return *value;
    return gameplay_settings().get_integer(m, a);
}

void* gameplay_get_string(std::uintptr_t m, void* out, const void* a) {
    // The original constructs the output CString; assignment may then replace it.
    auto* result = gameplay_settings().get_string(m, out, a);
    if (const auto value = saved_gameplay_setting<std::string>(a))
        user_values().assign_string(out, value->c_str(), static_cast<std::uint32_t>(value->size()));
    return result;
}

void gameplay_set_bool(std::uintptr_t m, const void* a, bool v) {
    if (local_runtime().active.load(std::memory_order_acquire)) {
        PreserveError preserve; std::string key;
    }
    if (!gameplay_user_edit()) {
        const bool applied = saved_gameplay_setting<bool>(a).value_or(v);
        gameplay_settings().set_bool(m, a, applied);
        note_stance(a, applied);
        return;
    }
    gameplay_settings().set_bool(m, a, v);
    note_stance(a, v);
    save_gameplay_setting(a, v);
}

void gameplay_set_number(const void* a, float v) {
    if (!gameplay_user_edit()) {
        gameplay_settings().set_number(a, saved_gameplay_setting<float>(a).value_or(v)); return;
    }
    gameplay_settings().set_number(a, v);
    save_gameplay_setting(a, v);
}

void gameplay_set_integer(std::uintptr_t m, const void* a, std::int32_t v) {
    if (!gameplay_user_edit()) {
        gameplay_settings().set_integer(m, a, saved_gameplay_setting<std::int32_t>(a).value_or(v)); return;
    }
    gameplay_settings().set_integer(m, a, v);
    save_gameplay_setting(a, v);
}

void gameplay_set_string(std::uintptr_t m, const void* a, const void* v) {
    const bool edit = gameplay_user_edit();
    if (!edit) {
        if (const auto saved = saved_gameplay_setting<std::string>(a)) {
            const char* text = saved->c_str(); gameplay_settings().set_string(m, a, &text); return;
        }
    }
    gameplay_settings().set_string(m, a, v);
    if (!edit || !local_runtime().active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try {
        std::uintptr_t text{}; std::string value;
        if (!read(reinterpret_cast<std::uintptr_t>(v), text)) return;
        for (std::size_t i = 0; i <= 4096; ++i) {
            char c{}; if (!read(text + i, c)) return;
            if (!c) { save_gameplay_setting(a, value); return; }
            value += c;
        }
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::settings, "{\"event\":\"local_gameplay_setting_save_failed\"}"); }
}
}