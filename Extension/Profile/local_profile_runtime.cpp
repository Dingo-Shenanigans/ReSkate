#include "Extension/News/live_news.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Customization/local_cosmetic_catalog.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "Extension/Music/local_music_ui.h"
#include "Extension/Music/local_music_shelf.h"
#include "Extension/Customization/item_browser.h"
#include "Extension/Music/local_music_playback.h"
#include "Extension/News/local_news_runtime.h"
#include "Extension/Objects/local_buildkit_labels.h"
#include "Extension/Objects/local_buildkit_limits.h"
#include "Extension/Objects/local_object_runtime.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "runtime_internal.h"
#include "Extension/Progression/local_challenge_runtime.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Extension/Progression/local_neighborhood_runtime.h"
#include "Extension/Progression/local_rip_score_runtime.h"
#include "Extension/Rendering/local_graphics_controls.h"
#include "Extension/Settings/local_gameplay_settings.h"
#include "Extension/Settings/local_native_settings.h"
#include "Extension/Settings/local_user_settings.h"
#include "Extension/Travel/local_location_travel.h"
#include "Extension/World/local_park_rotation.h"
#include "Extension/World/local_world_controls.h"
#include "Extension/World/local_world_layers.h"

namespace dingosdk::profile_runtime {
LocalRuntime& local_runtime() { static auto* s = new LocalRuntime; return *s; }

thread_local bool hydrating{};

// Runs inside the game's expression VM hooks and the challenge map walks, many times a frame:
// peeked, not read, which cost one ReadProcessMemory call per character (profiled 2026-10-01).
bool identifier(const void* wrapper, std::string& value) {
    std::uintptr_t p{};
    if (!memory::peek(reinterpret_cast<std::uintptr_t>(wrapper), p)) return false;
    value.clear();
    char text[256];
    const auto length = memory::peek_cstring(p, text, sizeof(text));
    if (length <= 0) return false;
    for (std::ptrdiff_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127) return false;
    }
    value.assign(text, static_cast<std::size_t>(length));
    return true;
}

bool entitlements_ready_hook(std::uintptr_t manager) {
    auto& s = local_runtime();
    // Active is published only after the validated JSON profile has loaded and
    // all hooks are installed. UI consumers may now query that local provider.
    // This is readiness, not ownership: unknown IDs still use the native lookup.
    if (s.active.load(std::memory_order_acquire)) {
        PreserveError preserve;
        if (!s.logged_entitlements_ready.exchange(true))
            dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_entitlements_ready\",\"source\":\"saved_profile\"}");
        return true;
    }
    return s.entitlements_ready(manager);
}

bool has_entitlement_hook(std::uintptr_t manager, const void* wrapper) {
    auto& s = local_runtime();
    if (s.active.load(std::memory_order_acquire)) {
        PreserveError preserve;
        try {
            std::string id;
            if (identifier(wrapper, id)) {
                const auto saved = s.store->entitlement(id);
                if (saved) {
                    std::lock_guard lock(s.native_mutex);
                    if (s.logged_entitlements.size() < 256 &&
                        s.logged_entitlements.try_emplace(id, *saved).second) {
                        std::ostringstream event;
                        event << "{\"event\":\"local_profile_entitlement\",\"id\":" << std::quoted(id)
                              << ",\"owned\":" << (*saved ? "true" : "false");
                        if (id.starts_with("unlock_busstop_")) append_entitlement_trigger_context(event);
                        event << '}';
                        dingosdk::logging::event(dingosdk::logging::Channel::profile, event.str().c_str());
                    }
                    return *saved;
                }
            }
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_entitlement_failed\"}"); }
        // Offline readiness can expose this lookup before the online manager
        // exists. Unknown IDs are unowned; the native lookup dereferences +1e0
        // without a null guard and is only safe with an actual manager.
        if (!manager) return false;
    }
    return s.has_entitlement(manager, wrapper);
}

std::uintptr_t onboarding_manager(std::uintptr_t context) {
    // The native context accessor uses this registered system offset.
    // Resolve it afresh; the level owns the system and its entity handle table.
    std::uint32_t offset{}, bucket_count{};
    std::uintptr_t owner{}, buckets{};
    std::uint8_t initialized{};
    if (context < 0x10000 || !read(local_runtime().base + addr::profile::onboarding_system_offset, offset) ||
        offset > 0x1000000) return 0;
    const auto manager = context + offset;
    if (!read(manager + 0x18, owner) || owner != context ||
        !read(manager + 0x90, initialized) || initialized != 1 ||
        !read(manager + 0x58, buckets) || buckets < 0x10000 ||
        !read(manager + 0x60, bucket_count) || !bucket_count || bucket_count > 1048576) return 0;
    return manager;
}

void apply_onboarding_event(std::uintptr_t manager, const profile::PlayEvent& event) {
    if (!event.id.ends_with(":start") && !event.id.ends_with(":complete")) return;
    auto& s = local_runtime();
    alignas(8) std::array<std::byte, 40> error{};
    s.success(error.data());
    struct Release {
        DestroyString destroy; void* error;
        ~Release() { destroy(error); }
    } release{s.destroy_string, error.data()};
    const char* id = event.id.c_str();
    // Unlike the history cache, this consumer publishes the runtime component's
    // started/completed fields and the ECS data-change notification.
    s.apply_onboarding(manager, error.data(), &id, event.timestamp, event.count);
}

void register_onboarding_hook(std::uintptr_t manager, const void* wrapper, std::uint64_t handle) {
    auto& s = local_runtime();
    s.register_onboarding(manager, wrapper, handle);
    if (!s.active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try {
        std::lock_guard lock(s.native_mutex);
        std::uintptr_t context{};
        std::string id;
        if (!(handle >> 32) || !read(manager + 0x18, context) ||
            onboarding_manager(context) != manager || !identifier(wrapper, id)) return;
        const auto snapshot = s.store->mission_state();
        unsigned applied{};
        for (const auto* suffix : {":start", ":complete"}) {
            const auto found = snapshot->play_events.find(id + suffix);
            if (found == snapshot->play_events.end()) continue;
            apply_onboarding_event(manager, found->second);
            ++applied;
        }
        if (applied && s.logged_onboarding[id] != handle) {
            s.logged_onboarding[id] = handle;
            std::ostringstream event;
            event << "{\"event\":\"local_profile_onboarding_replayed\",\"id\":" << std::quoted(id)
                << ",\"events\":" << applied << '}';
            dingosdk::logging::event(dingosdk::logging::Channel::profile, event.str().c_str());
        }
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_onboarding_replay_failed\"}"); }
}

bool valid_manager(std::uintptr_t manager) {
    PreserveError preserve;
    std::uintptr_t vtable{}, context{}, begin{}, end{}, buckets{};
    std::uint32_t bucket_count{};
    std::uint8_t initialized{};
    return read(manager, vtable) && vtable == local_runtime().base + addr::profile::play_event_manager_vtable &&
        read(manager + 0xc0, initialized) && initialized == 1 &&
        read(manager + 0x18, context) && context >= 0x10000 &&
        read(manager + 0x38, begin) && read(manager + 0x40, end) && end >= begin &&
        (end - begin) % 0x40 == 0 && (end - begin) / 0x40 <= 4096 &&
        read(manager + 0x58, buckets) && buckets >= 0x10000 &&
        read(manager + 0x60, bucket_count) && bucket_count > 0 && bucket_count <= 1048576;
}

void cache_event(std::uintptr_t manager, const profile::PlayEvent& event) {
    const NativeEvent native(event);
    local_runtime().cache(manager, &native);
}

void refresh_graphs(std::uintptr_t manager) {
    // The native acknowledgement takes a synchronous snapshot of this vector.
    std::uintptr_t begin{}, end{};
    if (!read(manager + 0x38, begin) || !read(manager + 0x40, end) || end < begin ||
        (end - begin) % 0x40 || (end - begin) / 0x40 > 4096) throw std::runtime_error("Invalid graph vector");
    for (auto at = begin; at != end; at += 0x40) local_runtime().refresh_graph(at);
}

bool hydrate(std::uintptr_t manager) {
    if (hydrating) return false;
    struct Guard { Guard() { hydrating = true; } ~Guard() { hydrating = false; } } guard;
    auto& s = local_runtime();
    const auto snapshot = s.store->mission_state();
    for (const auto& [id, event] : snapshot->play_events) { (void)id; cache_event(manager, event); }
    // Use the normal response consumer for ready publication and graph refresh.
    // It consumes its native Error and a null shared list (no invented refcounts).
    alignas(8) std::array<std::byte, 40> error{};
    std::array<std::uintptr_t, 2> empty_list{};
    s.success(error.data());
    s.complete_load(&manager, error.data(), empty_list.data());
    dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_hydrated\"}");
    return true;
}

void load_events_hook(std::uintptr_t manager) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire) || !valid_manager(manager)) { s.load(manager); return; }
    PreserveError preserve;
    try { std::lock_guard lock(s.native_mutex); hydrate(manager); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_load_failed\"}"); }
}

bool ready_hook(std::uintptr_t manager) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire) || !valid_manager(manager)) return s.ready(manager);
    PreserveError preserve;
    try {
        std::lock_guard lock(s.native_mutex);
        if (hydrating) return false;
        std::uint8_t loaded{};
        return read(manager + 0xb8, loaded) && (loaded == 1 || hydrate(manager));
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_load_failed\"}"); return false; }
}

void send_event_hook(std::uintptr_t manager, const void* wrapper) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire) || !valid_manager(manager)) { s.send(manager, wrapper); return; }
    PreserveError preserve;
    try {
        std::lock_guard lock(s.native_mutex);
        std::string id;
        if (!identifier(wrapper, id)) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_event_rejected\"}"); return; }
        std::uint8_t loaded{};
        if (!read(manager + 0xb8, loaded) || (!loaded && !hydrate(manager))) return;
        // Acknowledge only after the durable transaction. Native cache insertion
        // publishes the same absolute count used by the backend load consumer.
        const auto event = s.store->record_play_event(id);
        cache_event(manager, event);
        refresh_graphs(manager);
        std::uintptr_t context{};
        if (read(manager + 0x18, context))
            if (const auto onboarding = onboarding_manager(context)) apply_onboarding_event(onboarding, event);
        dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_event_saved\"}");
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_save_failed\",\"acknowledged\":false}"); }
}

bool get_bool_hook(std::uint64_t user, const void* key, bool fallback) {
    auto& s = local_runtime();
    if (s.active.load(std::memory_order_acquire)) {
        PreserveError preserve;
        try {
            std::string id;
            if (identifier(key, id)) if (const auto value = s.store->bool_option(id)) return *value;
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_option_read_failed\"}"); }
    }
    return s.get_bool(user, key, fallback);
}

void set_bool_hook(std::uint64_t user, const void* key, bool value) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) { s.set_bool(user, key, value); return; }
    PreserveError preserve;
    if (gameplay_expression_kind(executing_expression) == 2 && !gameplay_user_edit()) return;
    try {
        std::string id;
        if (identifier(key, id)) { s.store->set_bool_option(id, value); }
        else dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_option_rejected\"}");
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_save_failed\",\"acknowledged\":false}"); }
}

bool saved_quest(std::string_view id, std::int32_t& value) noexcept {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return false;
    try {
        const auto saved = s.store->quest_state(id);
        if (saved && (*saved == 0 || *saved == 5)) { value = *saved; return true; }
    } catch (...) {}
    return false;
}

bool save_quest_completion(std::string_view id) noexcept {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return false;
    try { s.store->set_quest_state(id, 5); return true; }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_quest_save_failed\"}"); return false; }
}

bool matches(std::uintptr_t base, const Fingerprint& f) {
    std::array<unsigned char, 32> bytes{};
    return read(base + f.rva, bytes) && bytes == f.bytes;
}
}

namespace dingosdk {
using namespace profile_runtime;

void set_local_location_travel_queue(LocalLocationTravelQueue queue) noexcept {
    location_travel_queue.store(queue, std::memory_order_release);
}
void update_local_customization() noexcept {
    PreserveError preserve;
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return;
    std::lock_guard lock(s.native_mutex);
    try {
        auto& c = cosmetic_runtime();
        const auto thread = GetCurrentThreadId();
        if (!c.update_thread) c.update_thread = thread;
        if (c.update_thread != thread) return;
        if (!c.catalog_failed && refresh_cosmetic_catalog()) {
            publish_cosmetic_catalog();
            publish_cosmetic_inventory();
        }
    } catch (...) {
        cosmetic_runtime().catalog_failed = true;
        dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_cosmetic_catalog_failed\"}");
    }
    try { update_challenge_catalog(); update_challenge_requests(); update_selected_challenge_progress(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_challenge_update_failed\"}"); }
    try { update_location_travel(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_location_travel_failed\"}"); }
    try { update_position_teleport(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"position_teleport_failed\"}"); }
    try { update_placement_restore(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_placement_restore_failed\"}"); }
    try { update_buildkit_limits(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_buildkit_limits_failed\"}"); }
    try { update_rip_score(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_rip_score_failed\"}"); }
    try { update_news_subscriptions(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_news_failed\",\"operation\":\"update\"}"); }
    try { update_object_categories(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_object_categories_failed\",\"operation\":\"update\"}"); }
    update_music_catalog();
    update_music_shelf();
    item_browser::update(s.base);
    try {
        if (cosmetic_runtime().update_thread == GetCurrentThreadId() && !cosmetic_runtime().items.empty())
            update_player_card();
    } catch (...) {
        player_card_runtime().failed = true;
        dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_player_card_failed\"}");
    }
}
void local_profile_before_level_transition(unsigned next) noexcept {
    PreserveError preserve;
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return;
    std::lock_guard lock(s.native_mutex);
    music_shelf_before_level_transition(next);
    item_browser::before_level_transition(next, s.base);
    news_runtime().pending.before_transition(next);
    object_runtime().pending.before_transition(next);
    auto& placements = placements_runtime();
    if (next == 3 || next == 14 || next == 22 || next == 24 || next == 25) {
        placements.creates_enabled = false;
        placements.creates.clear();
        placements.creation.reset();
    } else if (next == 13 || next == 21) placements.creates_enabled = true;
}
std::uint32_t local_customization_selected_preset() noexcept {
    PreserveError preserve;
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return 0;
    try { return s.store->selected_cosmetic_preset(); } catch (...) { return 0; }
}
bool local_customization_outfits_loadable() noexcept {
    PreserveError preserve;
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return true;
    std::lock_guard lock(s.native_mutex);
    try {
        if (!cosmetic_runtime().items.empty() || refresh_cosmetic_catalog()) return true;
        // Nothing saved is nothing to check: a new profile's slots are built
        // from the game's own defaults and need no catalog.
        const auto snapshot = s.store->shared_snapshot();
        return snapshot->customization.value("loadouts", dingosdk::Json::object()).empty();
    } catch (...) { return true; }
}
void observe_local_customization_selection(std::int32_t index) noexcept {
    PreserveError preserve;
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire) || index < 0 || index >= 10) return;
    std::lock_guard lock(s.native_mutex);
    auto& c = cosmetic_runtime();
    if (c.update_thread != GetCurrentThreadId() || c.observed_selection == index) return;
    c.observed_selection = index;
    try { s.store->set_selected_cosmetic_preset(static_cast<std::uint32_t>(index)); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_cosmetic_selection_save_failed\"}"); }
}

bool initialize_local_profile(std::uintptr_t base, bool authored_offline,
    const std::filesystem::path& path) {
    auto& s = local_runtime();
    if (!authored_offline) return false;
    if (s.attempted) return s.active.load() && s.base == base;
    s.attempted = true;
    std::vector<void*> created;
    bool enable_attempted{};
    try {
        if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != base) return false;
        std::array<wchar_t, 32768> exe{};
        const auto n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (!n || n >= exe.size()) return false;
        launcher::validate_game_file(exe.data());
        for (const auto& f : {load_contract, ready_contract, send_contract, get_bool_contract,
            user_get_bool_contract, user_get_number_contract, user_get_integer_contract, user_get_string_contract,
            user_set_bool_contract, user_set_number_contract, user_set_integer_contract, user_set_text_contract,
            user_set_string_contract, user_success_callback_contract, user_error_callback_contract,
            gameplay_get_bool_contract, gameplay_get_number_contract, gameplay_get_integer_contract, gameplay_get_string_contract,
            gameplay_set_bool_contract, gameplay_set_number_contract, gameplay_set_integer_contract, gameplay_set_string_contract,
            native_setting_get_contract, native_setting_set_contract,
            set_bool_contract, cache_contract, complete_load_contract, success_contract, refresh_graph_contract,
            register_onboarding_contract, apply_onboarding_contract, destroy_string_contract, onboarding_accessor_contract,
            has_entitlement_contract, entitlements_ready_contract, neighborhood_update_contract, neighborhood_publish_contract,
            neighborhood_ctor_contract, model_manager_contract, model_create_contract, model_value_contract,
            model_find_contract, model_lock_contract, model_unlock_contract,
            execute_expression_contract, execute_profiled_expression_contract,
            trigger_online_contract, trigger_session_contract,
            load_cosmetic_contract, save_cosmetic_contract, copy_cosmetic_contract,
            cosmetic_allocate_contract, cosmetic_construct_contract, cosmetic_decode_contract,
            cosmetic_create_catalog_contract, cosmetic_complete_catalog_contract, cosmetic_ui_ready_contract,
            cosmetic_control_destroy_contract, cosmetic_control_free_contract, cosmetic_adapter_allocate_contract,
            cosmetic_adapter_aligned_contract, cosmetic_adapter_free_contract, cosmetic_populate_inventory_contract,
            card_recipe_ctor_contract, card_recipe_destroy_contract, card_recipe_get_contract,
            card_recipe_set_contract, card_recipe_ready_contract, card_info_ctor_contract,
            card_model_field_contract, card_local_info_contract,
            news_first_contract, news_list_contract, news_ctor_contract, news_destroy_contract,
            news_array_ctor_contract, news_array_destroy_contract, news_append_contract, news_assign_contract,
            buildkit_text_exists_contract, buildkit_text_translate_contract,
            buildkit_settings_lookup_contract, buildkit_grabber_settings_contract,
            placement_server_create_contract, placement_message_contract, placement_apply_contract,
            placement_construct_contract, placement_destroy_contract, placement_resolve_contract, placement_valid_contract, placement_query_contract, placement_pose_contract, placement_context_contract, placement_accessor_contract, placement_connection_contract, placement_channel_contract,
            placement_delete_ctor_contract, placement_message_destroy_contract, placement_send_contract,
            placement_teleport_contract, placement_transition_contract,
            news_subscribe_contract, news_delegate_copy_contract, news_delegate_destroy_contract,
            news_invoke_contract, news_reference_contract,
            object_subscribe_contract, object_allocate_contract, object_ctor_contract,
            object_register_contract, object_parse_contract, object_release_contract,
            challenge_begin_contract, challenge_end_contract, challenge_available_contract, challenge_select_contract, challenge_lookup_contract, challenge_end_invoke_contract, challenge_error_contract, challenge_counts_contract, challenge_index_insert_contract, challenge_index_append_contract, challenge_insert_contract, challenge_dispatch_contract, challenge_request_create_contract,
            rip_score_context_contract})
            if (!matches(base, f)) return false;
        if (!neighborhood_type_contract(base) || !rip_score_type_contract(base) ||
            !challenge_map_type_contract(base)) return false;
        if (!prepare_main_mission_override(base)) return false;
        if (!game::initialize_native_data(base)) return false;
        s.base = base;
        s.store = std::make_unique<profile::Store>(path);
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::profile,
            L"Local profile loaded: " + path.filename().wstring()); // the folder holds the Windows user name
        buildkit_limits_runtime().config = buildkit_limits_from(*s.store->shared_snapshot());
        buildkit_limits_runtime().lookup = reinterpret_cast<decltype(buildkit_limits_runtime().lookup)>(base + buildkit_settings_lookup_contract.rva);
        s.cache = reinterpret_cast<Cache>(base + cache_contract.rva);
        s.complete_load = reinterpret_cast<CompleteLoad>(base + complete_load_contract.rva);
        s.success = reinterpret_cast<Success>(base + success_contract.rva);
        s.refresh_graph = reinterpret_cast<RefreshGraph>(base + refresh_graph_contract.rva);
        s.apply_onboarding = reinterpret_cast<ApplyOnboarding>(base + apply_onboarding_contract.rva);
        s.destroy_string = reinterpret_cast<DestroyString>(base + destroy_string_contract.rva);
        s.copy_cosmetic = reinterpret_cast<CopyCosmeticRecipes>(base + copy_cosmetic_contract.rva);
        initialize_cosmetic_catalog_functions(base);
        if (!matches(base, starter_profile_contract)) throw std::runtime_error("Starter profile contract mismatch");
        cosmetic_runtime().initialize_starter = reinterpret_cast<decltype(cosmetic_runtime().initialize_starter)>(base + starter_profile_contract.rva);
        initialize_player_card_functions(base);
        initialize_news_functions(base);
        news::start_live_news();
        initialize_object_functions(base);
        initialize_challenge_functions(base);
        for (const auto& f : {travel_location_ctor_contract, travel_access_ctor_contract, travel_request_contract,
                             travel_prepare_contract, travel_attribute_request_contract})
            if (!matches(base, f)) throw std::runtime_error("Location travel native contract mismatch");
        if (!location_travel_type_contract(base)) throw std::runtime_error("Location travel value contract mismatch");
        auto& travel = location_travel_runtime();
        travel.policy = location_travel_policy((*s.store->shared_snapshot()).extensions);
        travel.construct_location = reinterpret_cast<decltype(travel.construct_location)>(base + travel_location_ctor_contract.rva);
        travel.construct_access = reinterpret_cast<decltype(travel.construct_access)>(base + travel_access_ctor_contract.rva);
        const bool music_ready = initialize_music_functions(base);
        if (!music_ready) dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"native_music_ui_contract_mismatch\"}");
        rip_score_runtime().get_context = reinterpret_cast<decltype(rip_score_runtime().get_context)>(base + rip_score_context_contract.rva);
        auto hook = [&](const Fingerprint& f, auto detour, auto& original) {
            void* trampoline{};
            auto* target = reinterpret_cast<void*>(base + f.rva);
            const auto status = hook_prepare(target, reinterpret_cast<void*>(detour), &trampoline);
            if (status != HookOk)
                throw std::runtime_error("Cannot prepare local profile hook at RVA " + std::to_string(f.rva) +
                    ": " + hook_status_string(status));
            created.push_back(target);
            if (!trampoline) throw std::runtime_error("Missing local profile trampoline");
            original = reinterpret_cast<std::remove_reference_t<decltype(original)>>(trampoline);
        };
        hook(load_contract, &load_events_hook, s.load);
        hook(travel_request_contract, &location_travel_request_hook, travel.request);
        hook(travel_prepare_contract, &prepare_location_travel_hook, travel.prepare_request);
        hook(travel_attribute_request_contract, &location_travel_attribute_request_hook, travel.attribute_request);
        hook(ready_contract, &ready_hook, s.ready);
        hook(send_contract, &send_event_hook, s.send);
        hook(get_bool_contract, &get_bool_hook, s.get_bool);
        hook(set_bool_contract, &set_bool_hook, s.set_bool);
        auto& values = user_values();
        values.assign_string = reinterpret_cast<decltype(values.assign_string)>(base + news_assign_contract.rva);
        values.success_callback = reinterpret_cast<decltype(values.success_callback)>(base + user_success_callback_contract.rva);
        values.error_callback = reinterpret_cast<decltype(values.error_callback)>(base + user_error_callback_contract.rva);
        hook(user_get_bool_contract, &user_get_bool, values.get_bool);
        hook(user_get_number_contract, &user_get_number, values.get_number);
        hook(user_get_integer_contract, &user_get_integer, values.get_integer);
        hook(user_get_string_contract, &user_get_string, values.get_string);
        hook(user_set_bool_contract, &user_set_bool, values.set_bool);
        hook(user_set_number_contract, &user_set_number, values.set_number);
        hook(user_set_integer_contract, &user_set_integer, values.set_integer);
        hook(user_set_text_contract, &user_set_text, values.set_text);
        hook(user_set_string_contract, &user_set_string, values.set_string);
        auto& gameplay = gameplay_settings();
        hook(gameplay_get_bool_contract, &gameplay_get_bool, gameplay.get_bool);
        hook(gameplay_get_number_contract, &gameplay_get_number, gameplay.get_number);
        hook(gameplay_get_integer_contract, &gameplay_get_integer, gameplay.get_integer);
        hook(gameplay_get_string_contract, &gameplay_get_string, gameplay.get_string);
        hook(gameplay_set_bool_contract, &gameplay_set_bool, gameplay.set_bool);
        hook(gameplay_set_number_contract, &gameplay_set_number, gameplay.set_number);
        hook(gameplay_set_integer_contract, &gameplay_set_integer, gameplay.set_integer);
        hook(gameplay_set_string_contract, &gameplay_set_string, gameplay.set_string);
        auto& native = native_settings();
        hook(native_setting_get_contract, &native_setting_get, native.get);
        hook(native_setting_set_contract, &native_setting_set, native.set);
        hook(register_onboarding_contract, &register_onboarding_hook, s.register_onboarding);
        hook(has_entitlement_contract, &has_entitlement_hook, s.has_entitlement);
        hook(entitlements_ready_contract, &entitlements_ready_hook, s.entitlements_ready);
        hook(execute_expression_contract, &execute_expression_hook, s.execute_expression);
        hook(execute_profiled_expression_contract, &execute_profiled_expression_hook, s.execute_profiled_expression);
        hook(trigger_online_contract, &trigger_online_hook, s.trigger_online);
        hook(trigger_session_contract, &trigger_session_hook, s.trigger_session);
        hook(load_cosmetic_contract, &load_cosmetic_hook, s.load_cosmetic);
        hook(save_cosmetic_contract, &save_cosmetic_hook, s.save_cosmetic);
        hook(card_local_info_contract, &local_player_info_hook, player_card_runtime().functions.get_local_info);
        hook(news_first_contract, &news_first_hook, news_runtime().functions.first);
        hook(news_list_contract, &news_list_hook, news_runtime().functions.list);
        hook(news_subscribe_contract, &news_subscribe_hook, news_runtime().functions.subscribe);
        hook(object_subscribe_contract, &object_categories_hook, object_runtime().functions.subscribe);
        if (music_ready) {
            hook(music_ui_initialize_contract, &music_ui_initialize_hook, music_ui_runtime().functions.initialize);
            if (music_ui_runtime().functions.favorite_apply)
                hook(addr::local_music::favorite_change_contract, &music_favorite_change_hook,
                    music_ui_runtime().functions.favorite_change);
            std::array<unsigned char, 32> construct_bytes{};
            if (read(base + music_model_construct_contract.rva, construct_bytes) &&
                construct_bytes == music_model_construct_contract.bytes)
                hook(music_model_construct_contract, &music_model_construct_hook, music_model_construct_original);
            else dingosdk::logging::event(dingosdk::logging::Channel::music,
                "{\"event\":\"music_model_construct_contract_mismatch\"}");
        }
        const bool playback_ready = initialize_music_playback(base,
            local_preference("MusicShuffle").value_or(false));
        if (playback_ready)
            hook(addr::local_music::playback_select_next_contract,
                &music_select_next_hook, music_select_next_original());
        else dingosdk::logging::event(dingosdk::logging::Channel::music,
            "{\"event\":\"music_playback_order_contract_mismatch\"}");
        hook(buildkit_text_exists_contract, &buildkit_text_exists, buildkit_text_functions().exists);
        hook(buildkit_text_translate_contract, &buildkit_text_translate, buildkit_text_functions().translate);
        hook(buildkit_grabber_settings_contract, &buildkit_grabber_settings_hook, buildkit_limits_runtime().grabber_settings);
        auto& placements = placements_runtime();
        placements.server_create = reinterpret_cast<decltype(placements.server_create)>(base + placement_server_create_contract.rva);
        placements.teleport = reinterpret_cast<decltype(placements.teleport)>(base + placement_teleport_contract.rva);
        placements.transition_ctor = reinterpret_cast<decltype(placements.transition_ctor)>(base + placement_transition_contract.rva);
        placements.transition_destroy = reinterpret_cast<decltype(placements.transition_destroy)>(base + object_release_contract.rva);
        placements.delete_ctor = reinterpret_cast<decltype(placements.delete_ctor)>(base + placement_delete_ctor_contract.rva);
        placements.message_destroy = reinterpret_cast<decltype(placements.message_destroy)>(base + placement_message_destroy_contract.rva);
        placements.send = reinterpret_cast<decltype(placements.send)>(base + placement_send_contract.rva);

        placements.resolve = reinterpret_cast<decltype(placements.resolve)>(base + placement_resolve_contract.rva);
        placements.valid = reinterpret_cast<decltype(placements.valid)>(base + placement_valid_contract.rva);
        placements.query = reinterpret_cast<decltype(placements.query)>(base + placement_query_contract.rva);
        placements.pose = reinterpret_cast<decltype(placements.pose)>(base + placement_pose_contract.rva);
        placements.context = reinterpret_cast<decltype(placements.context)>(base + placement_context_contract.rva);
        hook(placement_construct_contract, &placement_construct_hook, placements.construct);
        hook(placement_destroy_contract, &placement_destroy_hook, placements.destroy);
        hook(placement_message_contract, &placement_message_hook, placements.message);
        hook(placement_apply_contract, &placement_apply_hook, placements.apply);
        hook(challenge_begin_contract, &challenge_begin_hook, challenge_runtime().f.begin);
        hook(challenge_end_contract, &challenge_end_hook, challenge_runtime().f.end);
        hook(challenge_available_contract, &challenge_available_hook, challenge_runtime().f.available);
        hook(challenge_select_contract, &challenge_select_hook, challenge_runtime().f.select);
        hook(challenge_lookup_contract, &challenge_lookup_hook, challenge_runtime().f.lookup);
        hook(challenge_counts_contract, &challenge_counts_hook, challenge_runtime().f.counts);
        auto& neighborhoods = neighborhood_runtime();
        neighborhoods.construct = reinterpret_cast<ConstructNeighborhood>(base + neighborhood_ctor_contract.rva);
        hook(neighborhood_publish_contract, &publish_neighborhood_hook, game::native_data().models.publish);
        hook(neighborhood_update_contract, &update_neighborhoods_hook, neighborhoods.update);
        for (auto* target : created) {
            enable_attempted = true;
            if (hook_enable(target) != HookOk) throw std::runtime_error("Cannot enable local profile hook");
        }
        if (playback_ready) activate_music_playback();
        initialize_placement_store(path);
        initialize_park_editor(path.parent_path());
        set_park_mods_root(mods::engine_data_root());
        s.active.store(true, std::memory_order_release);
        start_world_layers();
        start_park_rotation();
        start_world_controls();
        start_graphics_controls();
        set_local_profile_event_provider(base, true);
        set_local_object_browser_provider(base, &local_object_browser_ready);
        set_main_mission_profile_provider(base, &saved_quest, &save_quest_completion);
        dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_initialized\",\"active\":true,\"schema\":1,\"format\":\"sqlite\"}");
        return true;
    } catch (const std::exception& e) {
        s.active.store(false, std::memory_order_release);
        deactivate_music_playback();
        set_local_profile_event_provider(base, false);
        set_local_object_browser_provider(base, nullptr);
        for (auto* target : created) {
            if (enable_attempted) hook_disable(target);
            else hook_remove(target);
        }
        // Retain attempted trampolines/store for any already-entered detours.
        if (!enable_attempted) s.store.reset();
        dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"local_profile_initialized\",\"active\":false}");
        dingosdk::logging::log(dingosdk::logging::Level::error, dingosdk::logging::Channel::profile, "Profile initialization failed: {}", e.what());
        return false;
    }
}

}
