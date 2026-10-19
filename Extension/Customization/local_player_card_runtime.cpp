#include "Engine/Core/Log/logging.h"
#include "local_customization_runtime.h"
#include "local_player_card_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_player_card.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Multiplayer/Steam/steam_social.h"

namespace dingosdk::profile_runtime {
// RIP Card is account-wide and uses the general recipe manager, not CAS presets.

PlayerCardRuntime& player_card_runtime() { static auto* p = new PlayerCardRuntime; return *p; }

namespace {
constexpr std::string_view card_name_key = "ReSkate.PlayerCardName";
// Steam persona names allow 32 characters; keep custom card names to the same bound.
constexpr std::size_t max_card_name = 32;

// Trimmed, printable UTF-8 of at most max_card_name code points.
std::optional<std::string> card_name(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    std::size_t count{};
    for (std::size_t i = 0; i < text.size(); ++count) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
            (lead & 0xf0) == 0xe0 ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!length || text.size() - i < length) return std::nullopt;
        std::uint32_t point = length == 1 ? lead : lead & (0x7f >> length);
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) return std::nullopt;
            point = point << 6 | (next & 0x3f);
        }
        if ((length == 3 && (point < 0x800 || (point >= 0xd800 && point <= 0xdfff))) ||
            (length == 4 && (point < 0x10000 || point > 0x10ffff)) || point < 32 || (point >= 0x7f && point < 0xa0))
            return std::nullopt;
        i += length;
    }
    if (count > max_card_name) return std::nullopt;
    return std::string(text);
}

std::string saved_card_name() {
    const auto saved = local_runtime().store->user_value(card_name_key);
    if (!saved || !saved->is_string()) return {};
    return card_name(saved->string()).value_or(std::string{});
}
}

void initialize_player_card_functions(std::uintptr_t base) {
    auto& f = player_card_runtime().functions;
    f.construct_recipe = reinterpret_cast<decltype(f.construct_recipe)>(base + card_recipe_ctor_contract.rva);
    f.destroy_recipe = reinterpret_cast<decltype(f.destroy_recipe)>(base + card_recipe_destroy_contract.rva);
    f.get_recipe = reinterpret_cast<decltype(f.get_recipe)>(base + card_recipe_get_contract.rva);
    f.set_recipe = reinterpret_cast<decltype(f.set_recipe)>(base + card_recipe_set_contract.rva);
    f.ready = reinterpret_cast<decltype(f.ready)>(base + card_recipe_ready_contract.rva);
    f.construct_info = reinterpret_cast<decltype(f.construct_info)>(base + card_info_ctor_contract.rva);
}

bool read_player_card(std::uintptr_t manager, profile::CosmeticLoadout& result) {
    PlayerCardRecipeGuard native;
    if (!player_card_runtime().functions.get_recipe(manager, player_card_template, 1, native.words.data())) return false;
    profile::CosmeticRecipe recipe{player_card_template, 1, {}, {}};
    std::uint32_t count{};
    if (!cosmetic_words(native.words[0], 0, recipe.scalar_bits) ||
        !cosmetic_array(native.words[1], sizeof(CosmeticNativeItem), 3, count) || count != 3) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        CosmeticNativeItem item{}; profile::CosmeticSlot slot;
        if (!read(native.words[1] + i * sizeof(item), item) ||
            !cosmetic_text(reinterpret_cast<std::uintptr_t>(item.asset), slot.asset) ||
            !cosmetic_words(reinterpret_cast<std::uintptr_t>(item.parameters), 0, slot.parameter_bits)) return false;
        if (slot.asset.empty())
            for (const auto& [key, info] : cosmetic_runtime().items)
                if (info.hash == item.hash) { slot.asset = key; break; }
        if (slot.asset.empty() || item.hash != cosmetic_hash(slot.asset)) return false;
        slot.slot = item.slot; recipe.items.push_back(std::move(slot));
    }
    result.recipes.push_back(std::move(recipe));
    profile::validate_player_card(result);
    return true;
}

namespace {
bool player_card_item_owned(const profile::CosmeticSlot& slot, const profile::Snapshot& snapshot) {
    const auto it = cosmetic_runtime().items.find(slot.asset);
    if (it == cosmetic_runtime().items.end() || !installed_item_owned(snapshot, slot.asset, it->second.hash, it->second.build_kit))
        return false;
    // Each card category is also its recipe slot hash in the installed template.
    return std::find(it->second.categories.begin(), it->second.categories.end(), slot.slot) !=
        it->second.categories.end();
}
}

bool player_card_owned(const profile::CosmeticLoadout& value) {
    const auto snapshot_shared = local_runtime().store->shared_snapshot();
    const auto& snapshot = *snapshot_shared;
    for (const auto& slot : value.recipes.front().items)
        if (!player_card_item_owned(slot, snapshot)) return false;
    return true;
}

void apply_player_card(std::uintptr_t manager, const profile::CosmeticLoadout& value) {
    CosmeticBorrowedArray<std::uint32_t> empty(0);
    CosmeticBorrowedArray<CosmeticNativeItem> items(3);
    const auto& saved = value.recipes.front().items;
    for (std::size_t i = 0; i < saved.size(); ++i)
        items.data()[i] = {saved[i].asset.c_str(), empty.data(), cosmetic_hash(saved[i].asset), saved[i].slot};
    const std::array<std::uintptr_t, 4> native{reinterpret_cast<std::uintptr_t>(empty.data()),
        reinterpret_cast<std::uintptr_t>(items.data()), reinterpret_cast<std::uintptr_t>(empty.data()), 0};
    // Committed mode 1 also refreshes preview mode 2 through the native setter.
    player_card_runtime().functions.set_recipe(manager, player_card_template, 1, native.data());
}

std::uint64_t local_player_info_hook() {
    if (const auto other = player_info_override.load(std::memory_order_relaxed))
        if (const auto info = other()) return info;
    auto& s = local_runtime(); auto& p = player_card_runtime();
    const auto original = p.functions.get_local_info();
    if (!s.active.load(std::memory_order_acquire) || original) return original;
    PreserveError preserve;
    // Only the local-player expression receives this local profile record. No native_mutex: the
    // handle and model are atomics, written on the client thread that runs these scripts, and
    // the record is checked against the live model below (profiled 2026-10-01: waiting on the
    // lock held by other threads was the largest ReSkate cost while skating and teleporting).
    const auto handle = p.info_handle.load(std::memory_order_acquire);
    std::uintptr_t manager{}, model{};
    if (!handle || !memory::peek(s.base + addr::engine::ui_manager, manager) || !manager ||
        !memory::peek(manager + 0x140, model) || model != p.data_model.load(std::memory_order_acquire)) return 0;
    return game::native_data().models.value(model, handle, 0, 0) ? handle : 0;
}

bool publish_player_card(const profile::CosmeticLoadout& value) {
    auto& s = local_runtime(); auto& p = player_card_runtime(); auto& n = game::native_data().models;
    std::uintptr_t ui_manager{}, model{};
    if (!read(s.base + addr::engine::ui_manager, ui_manager) || !ui_manager ||
        !read(ui_manager + 0x140, model) || model != p.data_model) return false;
    game::ModelWriteLock model_lock(model);
    const auto original = p.functions.get_local_info();
    std::uint64_t handle = original;
    if (!handle) {
        handle = p.info_handle;
        if (handle && !n.value(model, handle, 0, 0)) { handle = 0; p.info_handle = 0; }
        if (!handle) {
            // Native offline players have no service identity. Keep the fallback
            // in its own local model namespace instead of changing player IDs.
            handle = n.create(model, s.base + addr::engine::player_info_type, 0, cosmetic_hash("ReSkate.LocalPlayerInfo"), false, 2);
            if (!handle) return false;
            alignas(8) std::array<std::byte, 0x108> info{};
            p.functions.construct_info(info.data());
            // Native UIPlayerInfo.IsLocal: enables the card's local progression
            // subscriptions (neighborhood badges, collection score, stance).
            info[0x105] = std::byte{1};
            // UIPlayerInfo carries its own model handle; widgets that receive
            // the record by value (throwdown leaderboard rows) bind their
            // inline profile through it and show a blank name without it.
            std::memcpy(info.data() + 0xa0, &handle, sizeof(handle));
            // Native publication returns false for an unchanged value too.
            n.publish(model, handle, s.base + addr::engine::player_info_type, info.data());
            if (!n.value(model, handle, 0, 0)) return false;
            p.info_handle = handle;
        }
    }
    if (!original) {
        const auto social = multiplayer::steam_social_snapshot();
        const auto base_info = n.field(model, handle, 1, UINT32_MAX, false);
        if (!base_info) return false;
        std::uintptr_t text{};
        // The custom name is presentation only; multiplayer reads Steam directly.
        const auto custom = saved_card_name();
        const auto& name = custom.empty() ? social->local.name : custom;
        game::native_data().values.assign(&text, name.empty() ? "Skater" : name.c_str(),
            static_cast<std::uint32_t>(name.empty() ? 6 : name.size()));
        struct ReleaseText { std::uintptr_t& value; ~ReleaseText() { local_runtime().destroy_string(&value); } } release{text};
        // DisplayName/PersonaName are part of the nested profile. The offline
        // backend ID remains empty; it is still required by local challenge callbacks.
        for (unsigned index : {0U, 4U}) {
            const auto field = n.field(model, base_info, index, UINT32_MAX, false);
            if (!field) return false;
            n.publish(model, field, s.base + addr::engine::string_type, &text);
        }
        // Native player id and persona id: throwdown leaderboard rows match
        // their participant against these, as party records already do.
        if (const auto player_id = multiplayer::local_native_player_id()) {
            const std::uint64_t persona = social->local.id;
            if (const auto field = n.field(model, base_info, 2, UINT32_MAX, false))
                n.publish(model, field, s.base + addr::engine::uint32_type, &player_id);
            if (const auto field = n.field(model, base_info, 3, UINT32_MAX, false); field && persona)
                n.publish(model, field, s.base + addr::engine::persona_id_type, &persona);
        }
        const std::uint32_t connection = social->local.online ? 0U : 2U;
        const auto presence = n.field(model, handle, 7, UINT32_MAX, false);
        n.publish(model, presence, s.base + addr::local_player_card::connection_state_type, &connection);
        // Every ReSkate player is a Steam player: the logo beside names is Steam's, not EA's.
        if (const auto network = n.field(model, handle, 10, UINT32_MAX, false)) {
            static bool logged_network{};
            if (!logged_network) {
                logged_network = true;
                std::uint32_t previous{};
                if (const auto record = n.value(model, handle, 0, 0)) read(record + 0xf4, previous);
                dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::progression,
                    "Player card: network {} shown as Steam.", previous);
            }
            const auto steam = addr::local_player_card::steam_network;
            n.publish(model, network, s.base + addr::local_player_card::player_network_type, &steam);
        }
    }
    {
        // In-throwdown / throwdown-host flags. The throwdown Quit action does
        // nothing unless the first is set; the second is only for the local
        // player's own drop (a joined one is left, not cancelled).
        const bool throwdown = multiplayer::local_throwdown_active();
        const bool host = multiplayer::local_throwdown_host();
        static bool logged_state{};
        if (const auto field = n.field(model, handle, 11, UINT32_MAX, false))
            n.publish(model, field, s.base + addr::engine::bool_type, &throwdown);
        if (const auto field = n.field(model, handle, 12, UINT32_MAX, false))
            n.publish(model, field, s.base + addr::engine::bool_type, &host);
        if (throwdown != logged_state) {
            logged_state = throwdown;
            std::array<std::uint8_t, 2> stored{};
            const auto record = n.value(model, handle, 0, 0);
            if (record) { read(record + 0xfc, stored[0]); read(record + 0xff, stored[1]); }
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::progression,
                "Throwdown flags: active={} native-record={} stored={}/{}", throwdown, original != 0, stored[0], stored[1]);
        }
    }
    for (const auto& slot : value.recipes.front().items) {
        const unsigned index = slot.slot == 42677725U ? 3 : slot.slot == 782673846U ? 25 : 26;
        const auto field = game::native_data().models.field(model, handle, index, 0xffffffffU, false);
        const auto hash = cosmetic_hash(slot.asset);
        std::uint32_t actual{};
        if (!field) return false;
        if (read(n.value(model, field, 0, 0), actual) && actual == hash) continue;
        n.publish(model, field, s.base + addr::engine::uint32_type, &hash);
        if (!read(n.value(model, field, 0, 0), actual) || actual != hash) return false;
    }
    return true;
}

void update_player_card() {
    auto& s = local_runtime(); auto& p = player_card_runtime(); auto& f = p.functions;
    if (p.failed || !f.ready || !f.get_local_info) return;
    const auto now = GetTickCount64();
    if (now < p.next_poll) return;
    p.next_poll = now + 250;
    std::uintptr_t manager{}, table{}, model{};
    if (!read(s.base + addr::engine::appearance_manager, manager) || !manager || !read(manager, table) ||
        table != s.base + addr::local_player_card::appearance_manager_vtable || !f.ready(manager) || !read(manager + 0xf0, model) || !model) return;
    if (manager != p.recipe_manager || model != p.data_model) {
        p.recipe_manager = manager; p.data_model = model;
        p.restored = false; p.observed.reset(); p.info_handle = 0;
        p.display_ready = false; p.display_pending_logged = false;
    }
    profile::CosmeticLoadout current;
    if (!read_player_card(manager, current)) return;
    if (!p.restored) {
        auto restore = s.store->player_card();
        if (restore) {
            // A saved part the game cannot show now (not installed, not available
            // or no longer fitting its slot) shows the game's own part for that
            // slot. The saved card keeps it until the player changes the card.
            const auto snapshot_shared = s.store->shared_snapshot();
            const auto& snapshot = *snapshot_shared;
            const auto& defaults = current.recipes.front().items;
            for (auto& slot : restore->recipes.front().items) {
                if (player_card_item_owned(slot, snapshot)) continue;
                const auto fallback = std::find_if(defaults.begin(), defaults.end(),
                    [&](const profile::CosmeticSlot& item) { return item.slot == slot.slot; });
                if (fallback == defaults.end()) { restore.reset(); break; }
                slot = *fallback;
            }
        }
        if (restore) {
            apply_player_card(manager, *restore);
            current = {};
            if (!read_player_card(manager, current) || current != *restore)
                throw std::runtime_error("RIP Card restore did not match");
            p.observed = current;
            dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_player_card_restored\"}");
        }
        p.restored = true;
    }
    if ((!p.observed || *p.observed != current) && player_card_owned(current)) {
        s.store->save_player_card(current);
        p.observed = current;
        dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_player_card_saved\"}");
    }
    // UI records may appear later than recipes (or be rebuilt when menus open).
    const bool ready = publish_player_card(current);
    if (ready && !p.display_ready) dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_player_card_display_published\"}");
    if (!ready && !p.display_pending_logged) {
        dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_player_card_display_pending\"}");
        p.display_pending_logged = true;
    }
    p.display_ready = ready;
}
}

namespace dingosdk {
using namespace profile_runtime;
PlayerCardModel local_profile_player_card() {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(s.native_mutex);
    return {true, saved_card_name(), player_card_runtime().name_feedback};
}

bool set_local_player_card_name(std::string_view name) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(s.native_mutex);
    auto& p = player_card_runtime();
    const auto value = card_name(name);
    if (!value) {
        p.name_feedback = "Card names are limited to 32 printable characters.";
        return false;
    }
    try { s.store->set_user_value(card_name_key, *value); }
    catch (...) {
        p.name_feedback = "Couldn't save the card name. Try again.";
        dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_player_card_name_failed\"}");
        return false;
    }
    p.next_poll = 0; // Republish on the next customization update.
    p.name_feedback = value->empty() ? "Card name reset to your Steam name." :
        "Card name saved. Multiplayer still shows your Steam name.";
    dingosdk::logging::event(dingosdk::logging::Channel::customization, value->empty() ?
        "{\"event\":\"local_player_card_name_reset\"}" : "{\"event\":\"local_player_card_name_saved\"}");
    return true;
}
}
