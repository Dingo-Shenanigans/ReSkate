#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_challenge_runtime.h"
#include "challenge_attempts.h"
#include "Extension/Throwdowns/virtual_player_names.h"
#include "local_entitlement_trigger_runtime.h"
#include <algorithm>
#include <mutex>
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_challenge.h"

namespace dingosdk::profile_runtime {
// Decoded challenge entry/exit expressions and the native activity model catalog.

ChallengeRuntime& challenge_runtime() { static auto* value = new ChallengeRuntime; return *value; }

std::atomic_bool& hidden_challenges() { static std::atomic_bool value{}; return value; }

std::string_view challenge_title_fallback(std::string_view key) {
    const auto& labels = challenge_runtime().title_fallbacks;
    const auto found = labels.find(key);
    return found == labels.end() ? std::string_view{} : found->second;
}

thread_local bool select_authored_challenge{};

thread_local ChallengeCompletionDelivery* challenge_completion_delivery{};

bool local_challenges_active() {
    return local_runtime().active.load(std::memory_order_acquire) && challenge_runtime().policy.enabled;
}

const profile::ChallengeDefinition* local_challenge(std::string_view id) {
    const auto& catalog = challenge_runtime().policy.catalog;
    const auto it = catalog.find(id);
    return it == catalog.end() ? nullptr : &it->second;
}

bool challenge_native_array(const void* wrapper, std::uintptr_t& data, std::uint32_t& count, std::uint32_t maximum) {
    return game::native_array<std::uint64_t>(wrapper, data, count, maximum);
}

bool challenge_player_id(const void* wrapper, std::string& value) {
    std::uintptr_t pointer{}; std::uint8_t first{};
    if (!read(reinterpret_cast<std::uintptr_t>(wrapper), pointer) || !read(pointer, first)) return false;
    // The local session has no authenticated backend player ID. Its valid
    // CString is empty, and must be preserved in the native callback payload.
    if (!first) { value.clear(); return true; }
    return identifier(wrapper, value);
}

bool challenge_native_strings(const void* wrapper, std::vector<std::string>& values, std::uint32_t maximum,
    bool players ) {
    std::uintptr_t data{}; std::uint32_t count{};
    if (!challenge_native_array(wrapper, data, count, maximum)) return false;
    std::set<std::string> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string id;
        const auto value = reinterpret_cast<const void*>(data + i * 8);
        if (!(players ? challenge_player_id(value, id) : identifier(value, id)) || !seen.insert(id).second) return false;
        values.push_back(std::move(id));
    }
    return true;
}

namespace {
// The local player's open attempts (challenge_attempts.h), under a lock of their own: begin and end
// requests come on the game's threads, and the client thread asks.
struct Attempts {
    std::mutex mutex;
    OpenAttempts open;
};
Attempts& attempts() {
    static auto* value = new Attempts;
    return *value;
}
AttemptFallback attempt_fallback;
// A begin or end request seen, whether the local challenges take it or the game does.
void note_attempt(bool ending, const void* id_value) noexcept {
    PreserveError preserve;
    attempt_fallback.guard([&] {
        auto& a = attempts();
        std::lock_guard lock(a.mutex);
        a.open.observe(ending, [&](std::string& id) { return identifier(id_value, id); });
    });
}
} // namespace

bool local_challenge_attempt_open() noexcept {
    // The active flag is published only after all profile hooks enabled successfully. Prepared
    // originals alone are not proof: initialization failure deliberately retains their relays.
    if (attempt_fallback.unknown()) return true;
    bool open = true;
    attempt_fallback.guard([&] {
        const bool active = local_runtime().active.load(std::memory_order_acquire);
        if (!active) return;
        const auto& f = challenge_runtime().f;
        if (!challenge_requests_watched(active, f.begin != nullptr, f.end != nullptr)) return;
        auto& a = attempts();
        std::lock_guard lock(a.mutex);
        open = a.open.open();
    });
    return open || attempt_fallback.unknown();
}

void challenge_attempts_before_level_transition(unsigned next) noexcept {
    if (next != 3 && next != 14 && next != 22 && next != 24 && next != 25) return;
    attempt_fallback.guard([&] {
        auto& a = attempts();
        std::lock_guard lock(a.mutex);
        a.open.clear();
        attempt_fallback.cleared();
    });
}

void* challenge_request(bool ending, void* out, const void* id_value, const void* player_value,
    const void* values, const void* callback) {
    auto& r = challenge_runtime(); auto& s = local_runtime();
    const auto original = ending ? r.f.end : r.f.begin;
    if (!local_challenges_active()) {
        note_attempt(ending, id_value);
        return original(out, id_value, player_value, values, callback);
    }
    PreserveError preserve;
    std::lock_guard lock(s.native_mutex);
    PendingChallenge request; request.ending = ending;
    if (!identifier(id_value, request.id) || !local_challenge(request.id)) {
        note_attempt(ending, id_value);
        if (r.forwarded_requests++ < 16)
            dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_native_forward"}, {"ending", ending},
                {"id", request.id}, {"selected", r.selected}}.dump().c_str());
        return original(out, id_value, player_value, values, callback);
    }
    game::NativeDelegateGuard retained;
    game::native_data().values.copy_delegate(&retained.value, callback);
    request.callback = retained.value;
    struct RequestReference {
        std::uintptr_t value{};
        ~RequestReference() { if (value) challenge_runtime().f.release_request(&value); }
    } ticket;
    try {
        if (!challenge_player_id(player_value, request.player)) throw std::runtime_error("Invalid challenge player");
        if (ending) {
            std::uintptr_t data{}; std::uint32_t count{};
            if (!challenge_native_array(values, data, count, 16) || !count)
                throw std::runtime_error("Invalid challenge participants");
            bool found{}; std::set<std::string> unique;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::string player; std::vector<std::string> goals;
                const auto entry = data + i * 16;
                if (!challenge_player_id(reinterpret_cast<const void*>(entry), player) || !unique.insert(player).second ||
                    !challenge_native_strings(reinterpret_cast<const void*>(entry + 8), goals, 256))
                    throw std::runtime_error("Invalid challenge participant result");
                request.players.push_back(player);
                if (player == request.player) { found = true; request.goals = std::move(goals); }
            }
            const auto attempt = r.attempts.find(request.id);
            if (!found || attempt == r.attempts.end()) throw std::runtime_error("Challenge has not begun locally");
            request.attempt = attempt->second;
            // ServerMpActivityBase's leave/remove/participant handlers end the leaver's attempt
            // (their entry plus the host's). With a virtual leaver the local attempt goes on.
            std::uintptr_t resource{};
            std::uint32_t graph{};
            if (executing_expression && read(executing_expression + 0x38, resource) && resource) read(resource + 0x10, graph);
            constexpr std::array<std::uint32_t, 4> leave_graphs{0xad0980ba, 0x93a7f66f, 0x973edf07, 0xfc3cbd3a};
            request.virtual_leave = std::ranges::find(leave_graphs, graph) != leave_graphs.end() &&
                std::ranges::any_of(request.players, [](const std::string &p) { return multiplayer::is_virtual_eid(p); });
        } else if (!challenge_native_strings(values, request.players, 16, true)) {
            throw std::runtime_error("Invalid challenge participants");
        }
        // SendEndAmpEvent checks the *start request reference* at +b8 before
        // submitting results. A success callback with a null return reference
        // leaves that guard false forever. Use the same typed, reference-counted
        // cancellation ticket as the native begin/end endpoints. Its factory
        // allocates only local state; it does not contact a backend.
        std::uintptr_t allocator{};
        if (!read(s.base + addr::engine::ui_allocator, allocator) || !allocator)
            throw std::runtime_error("Challenge request allocator unavailable");
        r.f.create_request(&ticket.value, allocator);
        if (!ticket.value) throw std::runtime_error("Challenge request allocation failed");
    } catch (const std::exception& error) {
        request.failed = true;
        dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_request_rejected"}, {"id", request.id},
            {"reason", error.what()}}.dump().c_str());
    }
    // A virtual participant's leave ends its entry, not the local attempt, which goes on. A begin the
    // game was refused opens no attempt (no end follows one), so it never holds score lines down.
    if (ending ? !request.virtual_leave : !request.failed) note_attempt(ending, id_value);
    // No native pointer into the expression stack survives this call. Retain
    // the VM delegate, then commit and acknowledge on the client update thread.
    r.pending.push_back(std::move(request)); retained.value = 0;
    // Transfer the factory's owned reference to the VM. The game retains it in
    // StartEventAmpCallback/EndEventAmpCallback and releases it normally.
    *static_cast<std::uintptr_t*>(out) = ticket.value;
    ticket.value = 0;
    return out;
}

void* challenge_begin_hook(void* out, const void* id, const void* player, const void* values, const void* callback) {
    return challenge_request(false, out, id, player, values, callback);
}

void* challenge_end_hook(void* out, const void* id, const void* player, const void* values, const void* callback) {
    return challenge_request(true, out, id, player, values, callback);
}

bool challenge_available_hook(const void* value) {
    if (local_challenges_active()) {
        PreserveError preserve;
        std::string id;
        if (identifier(value, id)) if (const auto* entry = local_challenge(id))
            return entry->available && !hidden_challenges().load(std::memory_order_acquire);
    }
    return challenge_runtime().f.available(value);
}

std::uint64_t challenge_lookup_hook(std::uintptr_t owner, const void* value) {
    // Native SelectActivity returns its "populate from authored prefab" flag
    // when the catalog lookup misses. Keep that path for complete goal/rule data.
    if (select_authored_challenge) return 0;
    return challenge_runtime().f.lookup(owner, value);
}

bool challenge_select_hook(std::uintptr_t owner, const void* value) {
    auto& r = challenge_runtime();
    std::string id;
    if (!local_challenges_active() || !identifier(value, id) || !local_challenge(id)) return r.f.select(owner, value);
    std::lock_guard lock(local_runtime().native_mutex);
    struct Restore { bool old{select_authored_challenge}; ~Restore() { select_authored_challenge = old; } } restore;
    select_authored_challenge = true;
    r.selected = id;
    return r.f.select(owner, value);
}

std::uintptr_t challenge_owner() {
    const auto base = local_runtime().base;
    std::uintptr_t context{}, vtable{}, backlink{}, manager{}, buckets{};
    std::uint32_t offset{}, count{}; std::uint8_t ready{};
    if (!read(base + addr::local_challenge::challenge_context, context) || !context || !read(base + addr::local_challenge::challenge_owner_offset, offset) || offset > 0x1000000) return 0;
    const auto owner = context + offset;
    if (!read(owner, vtable) || vtable != base + addr::local_challenge::challenge_owner_vtable || !read(owner + 0x18, backlink) || backlink != context ||
        !read(owner + 0x268, ready) || ready != 1 || !read(owner + 0x1a8, manager) || !manager ||
        !read(owner + 0x38, buckets) || !buckets || !read(owner + 0x40, count) || !count || count > 0x100000) return 0;
    return owner;
}

// Called for every catalog entry on each catalog poll: peeked (no system call per read).
std::uint64_t challenge_map_lookup(std::uintptr_t map, const std::string& id) {
    std::uintptr_t buckets{}, node{}; std::uint32_t count{};
    if (!memory::peek(map, buckets) || !memory::peek(map + 8, count) || !count || count > 0x100000 ||
        !memory::peek(buckets + (game::native_name_hash(id) % count) * 8, node)) return 0;
    std::string key;
    for (unsigned visited = 0; node && visited < 2048; ++visited) {
        std::uint64_t value{};
        if (!identifier(reinterpret_cast<void*>(node), key) || !memory::peek(node + 8, value)) return 0;
        if (key == id) return value;
        if (!memory::peek(node + 16, node)) return 0;
    }
    return 0;
}

bool challenge_map_type_contract(std::uintptr_t base) {
    std::uint16_t counts_size{}, activity_size{};
    struct Field { std::uint64_t hash, offset; std::uintptr_t type; } field{};
    return read(base + addr::local_challenge::challenge_counts_size, counts_size) && counts_size == sizeof(ChallengeCounts) &&
        read(base + addr::local_challenge::activity_data_size, activity_size) && activity_size == 0x60 &&
        read(base + addr::local_challenge::activity_criteria_field, field) && field.hash == 0x571954a7 &&
        field.offset == 0x30 && field.type == base + addr::engine::uint64_type;
}

void challenge_counts_hook(const void* neighborhood, bool visible_only, ChallengeCounts* output) {
    // The retail function returns without writing when its owner/index is not
    // ready or the neighborhood has no entry. Never expose VM stack garbage.
    if (output && local_challenges_active()) *output = {};
    challenge_runtime().f.counts(neighborhood, visible_only, output);
}

bool hydrate_challenge_neighborhoods(std::uintptr_t owner) {
    auto& r = challenge_runtime();
    std::uintptr_t buckets{};
    std::uint32_t bucket_count{};
    if (!read(owner + 0x100, buckets) || !buckets || !read(owner + 0x108, bucket_count) ||
        !bucket_count || bucket_count > 0x100000) return false;
    std::map<std::string, std::vector<std::uint64_t>, std::less<>> groups;
    for (const auto id : profile::neighborhood_ids) groups[std::string(id)];
    for (const auto& [id, entry] : r.policy.catalog) {
        if (entry.neighborhood.empty()) continue;
        auto& handles = groups[entry.neighborhood];
        const auto owned = r.catalog_activities.find(id);
        if (owned != r.catalog_activities.end() && owned->second &&
            challenge_map_lookup(owner + 0x38, id) == owned->second) handles.push_back(owned->second);
    }
    bool changed{};
    for (const auto& [id, handles] : groups) {
        const auto* key = id.c_str();
        // Native find-or-insert clones the borrowed key and owns the vector.
        // Its result is {node, bucket, inserted}; the node's vector starts at +8.
        std::array<std::uintptr_t, 3> result{};
        r.f.index_insert(owner + 0x100, result.data(), &key);
        if (!result[0]) continue;
        changed |= (result[2] & 0xff) != 0;
        const auto vector = result[0] + 8;
        std::uintptr_t begin{}, end{}, capacity{};
        if (!read(vector, begin) || !read(vector + 8, end) || !read(vector + 16, capacity) ||
            end < begin || capacity < end || capacity - begin > 0x20000 || (end - begin) % 8 ||
            (!begin && (end || capacity))) continue;
        std::vector<std::uint64_t> current((end - begin) / 8);
        if (!current.empty() && !read_bytes(begin, current.data(), current.size() * 8)) continue;
        std::set<std::uint64_t> present(current.begin(), current.end());
        for (const auto handle : handles) {
            if (present.insert(handle).second) { r.f.index_append(vector, &handle); changed = true; }
        }
    }
    return changed;
}

void hydrate_challenge_catalog(std::uintptr_t owner) {
    auto& r = challenge_runtime(); auto& n = game::native_data().models; const auto base = local_runtime().base;
    std::uintptr_t manager{}, context{};
    if (!read(owner + 0x1a8, manager) || !read(owner + 0x18, context)) return;
    if (r.last_owner != owner) {
        r.catalog_groups.clear(); r.catalog_activities.clear(); r.catalog_goals.clear(); r.changed_groups.clear();
        r.map_progress_revision = UINT64_MAX;
    }
    const auto create = [&](std::uintptr_t type, const void* data) {
        // The real activity catalog's allocation expands to this
        // anonymous allocation. A named (0, 1) allocation instead reuses one
        // model per type and makes every challenge display the last record.
        const auto handle = n.create(manager, base + type, UINT32_MAX, UINT32_MAX, false, 1);
        if (!handle) throw std::runtime_error("Challenge model creation failed");
        // Publish returns whether a value changed, not whether assignment
        // succeeded. A fresh empty criterion group already equals its default.
        n.publish(manager, handle, base + type, data);
        return handle;
    };
    const auto insert = [&](std::size_t offset, const std::string& id, std::uint64_t handle) {
        const char* key = id.c_str(); std::array<std::uintptr_t, 4> result{};
        r.f.insert(owner + offset, result.data(), 0, &key, &handle);
    };
    ChallengeEmptyArray empty;
    const auto array = reinterpret_cast<std::uintptr_t>(&empty.element);
    const char* blank = "";
    std::size_t added{};
    bool refreshed{};
    const auto snapshot_shared = local_runtime().store->shared_snapshot();
    const auto& snapshot = *snapshot_shared;
    const auto hidden = snapshot.bool_options.find(profile::hide_challenges_option);
    const bool visible = hidden == snapshot.bool_options.end() || !hidden->second;
    const auto populate_group = [&](const profile::ChallengeDefinition& entry, std::uint64_t existing) {
        // A present group with zero criteria means completed to the retail count
        // function. Absent metadata must stay an absent group until learned.
        if (entry.goals.empty()) return std::uint64_t{};
        ChallengeArray<std::uint64_t, 256> required, optional;
        auto& handles = r.catalog_goals[entry.id]; handles.clear();
        const auto completed = profile::challenge_completed_criteria(snapshot, entry.id);
        for (const auto& goal : entry.goals) {
            // Map-only criterion values. Actual playable rules/thresholds still
            // come from the selected authored prefab, never cached rewards.
            const ChallengeCriterionValue value{0, goal.id.c_str(), 0, array, 0,
                goal.description_key.c_str(), goal.title_key.c_str(), goal.feed_key.c_str(),
                goal.short_description_key.c_str(), 0,
                std::find(completed.begin(), completed.end(), goal.id) != completed.end(), goal.optional, {}};
            const auto handle = create(addr::local_challenge::criterion_type, &value);
            auto& list = goal.optional ? optional : required;
            list.values[list.count++] = handle;
            handles.emplace_back(goal.id, handle);
        }
        struct Group { const char* title; const std::uint64_t* criteria; const char* other_title;
            const std::uint64_t* other_criteria; } group{blank, required.values.data(), blank, optional.values.data()};
        if (existing) { n.publish(manager, existing, base + addr::local_challenge::criterion_group_type, &group); return existing; }
        return create(addr::local_challenge::criterion_group_type, &group);
    };
    for (const auto& [id, entry] : r.policy.catalog) {
        if (const auto activity = challenge_map_lookup(owner + 0x38, id)) {
            if (r.catalog_activities.contains(id) && r.catalog_activities.at(id) == activity &&
                r.catalog_groups.contains(id) && r.changed_groups.contains(id)) {
                auto& group = r.catalog_groups.at(id);
                group = populate_group(entry, group);
                // Learning the first goals creates the previously absent group.
                // Field 14 is ActivityData's UInt64 criterion-group handle.
                const auto field = game::native_data().models.field(manager, activity, 14, UINT32_MAX, false);
                if (field) n.publish(manager, field, base + addr::engine::uint64_type, &group);
                const auto value = n.value(manager, activity, 0, 0);
                std::uint64_t published{};
                if (read(value + 0x30, published) && published == group) r.changed_groups.erase(id);
                refreshed = true;
            }
            continue;
        }
        auto type = challenge_map_lookup(owner + 0x60, entry.type);
        if (!type) {
            static const std::map<std::string, std::uint32_t> types{{"OTS", 1}, {"Slam", 2}, {"Line", 3},
                {"Session", 4}, {"Stunt", 5}, {"OTL", 6}, {"Speedline", 7}};
            struct Type { const char* name; std::uint32_t type, padding{}; } data{entry.type.c_str(), types.at(entry.type)};
            type = create(addr::local_challenge::activity_kind_type, &data); insert(0x60, entry.type, type);
        }
        const auto criteria = populate_group(entry, 0);
        r.catalog_groups[id] = criteria;
        // Reflected field records (0x82dc240..): +0x18 SeasonId, +0x38 ActivityTitle and +0x40
        // Description (the map card and details page show them localized).
        struct Data {
            std::uintptr_t array; std::uint64_t type; const char* id; const char* season;
            std::int64_t expires{}; std::uintptr_t added_array; std::uint64_t criteria;
            const char* title; const char* description; const char* neighborhood; std::uint32_t priority{1};
            std::uint8_t added_flag{}, is_new{}, visible{1}, flag{}, available{}, extra{}, another_flag{};
            std::uint8_t padding[5]{};
        } data{array, type, id.c_str(), blank, 0, array, criteria,
            entry.title_key.empty() ? id.c_str() : entry.title_key.c_str(), entry.description_key.c_str(),
            entry.neighborhood.c_str(), 1,
            0, 0, static_cast<std::uint8_t>(visible), 0, static_cast<std::uint8_t>(visible && entry.available)};
        // Current reflected ActivityData is 60h, not the older 58h record.
        // Both native arrays need valid count prefixes even while empty.
        static_assert(sizeof(Data) == 0x60 && offsetof(Data, id) == 0x10 &&
            offsetof(Data, added_array) == 0x28 && offsetof(Data, criteria) == 0x30 &&
            offsetof(Data, available) == 0x58);
        const auto activity = create(addr::local_challenge::activity_data_type, &data);
        insert(0x38, id, activity); r.catalog_activities[id] = activity; ++added;
    }
    // The native count/list functions consume a separate neighborhood index,
    // not the ID catalog. Reuse the real insertion/vector helpers, preserve
    // foreign entries and deduplicate handles on every retry.
    refreshed |= hydrate_challenge_neighborhoods(owner);
    // Restored and newly earned results must reach every map entry, including
    // challenges that have not been selected since launching the game.
    if (r.map_progress_revision != snapshot.revision || refreshed) {
        game::ModelWriteLock model_lock(manager);
        for (const auto& [id, entry] : r.policy.catalog) {
            const auto handle = challenge_map_lookup(owner + 0x38, id);
            if (!handle) continue;
            const auto value = n.value(manager, handle, 0, 0);
            const auto set_flag = [&](std::size_t offset, unsigned index, bool next) {
                std::uint8_t current{};
                if (!read(value + offset, current) || static_cast<bool>(current) == next) return;
                const auto field = game::native_data().models.field(manager, handle, index, UINT32_MAX, false);
                if (field) { n.publish(manager, field, base + addr::engine::bool_type, &next); refreshed = true; }
            };
            // Reflected ActivityData visibility and availability fields. Keep
            // goal models and completion handling alive while markers are hidden.
            set_flag(0x56, 12, visible);
            set_flag(0x58, 7, visible && entry.available);
        }
        for (const auto& [id, goals] : r.catalog_goals) {
            const auto saved = profile::challenge_completed_criteria(snapshot, id);
            for (const auto& [goal, handle] : goals) {
                const auto value = n.value(manager, handle, 0, 0);
                std::uint8_t was{};
                const bool complete = std::find(saved.begin(), saved.end(), goal) != saved.end();
                if (!read(value + 0x4c, was) || static_cast<bool>(was) == complete) continue;
                const auto field = game::native_data().models.field(manager, handle, 11, UINT32_MAX, false);
                if (field) { n.publish(manager, field, base + addr::engine::bool_type, &complete); refreshed = true; }
            }
        }
        r.map_progress_revision = snapshot.revision;
    }
    if (added || refreshed) {
        // Same ActivityListUpdated notification as the real catalog callback.
        std::uint64_t payload{}; const std::array<std::uint32_t, 3> options{0, 1, 0};
        r.f.dispatch(context, base + addr::local_challenge::activity_list_updated_type, &payload, options.data(), 0);
        dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_catalog"}, {"added", added},
            {"progress_refreshed", refreshed}, {"owner", owner}}.dump().c_str());
    }
    r.last_owner = owner;
}

void update_challenge_catalog() {
    auto& r = challenge_runtime();
    if (!local_challenges_active() || GetTickCount64() < r.next_catalog_poll) return;
    r.next_catalog_poll = GetTickCount64() + 1000;
    const auto owner = challenge_owner();
    if (owner) hydrate_challenge_catalog(owner);
}

void update_selected_challenge_progress(bool learn ) {
    auto& r = challenge_runtime(); auto& s = local_runtime(); auto& n = game::native_data().models;
    if (!local_challenges_active() || GetTickCount64() < r.next_progress_poll) return;
    r.next_progress_poll = GetTickCount64() + 250;
    const auto owner = challenge_owner(); if (!owner) return;
    std::uintptr_t manager{}; std::uint64_t wrapper{}, selected{}, group{};
    if (!read(owner + 0x1a8, manager) || !read(owner + 0x148, wrapper) || !wrapper) return;
    std::string id;
    std::vector<profile::ChallengeGoal> authored;
    std::set<std::string> unique;
    {
        game::ModelWriteLock model_lock(manager);
        if (!read(n.value(manager, wrapper, 0, 0), selected) || !selected) return;
        const auto data = n.value(manager, selected, 0, 0);
        if (!identifier(reinterpret_cast<const void*>(data + 0x10), id) || !local_challenge(id) ||
            !read(data + 0x30, group) || !group) return;
        const auto& definition = *local_challenge(id);
        const auto fill_text = [&](std::uint64_t model, std::uintptr_t address, unsigned index,
            const std::string& key, std::string_view placeholder = {}) {
            if (key.empty()) return;
            std::uintptr_t pointer{}; std::uint8_t first{};
            if (!read(address, pointer)) return;
            std::string current;
            if (pointer && (!read(pointer, first) || (first &&
                !identifier(reinterpret_cast<void*>(address), current)))) return;
            // Keep authored text when present; only repair empty/ID placeholders.
            if (!current.empty() && current != placeholder) return;
            const auto field = game::native_data().models.field(manager, model, index, UINT32_MAX, false);
            const char* text = key.c_str();
            if (field) n.publish(manager, field, s.base + addr::engine::string_type, &text);
        };
        fill_text(selected, data + 0x40, 2, definition.description_key, id);
        const auto saved = s.store->completed_challenge_criteria(id);
        const auto group_data = n.value(manager, group, 0, 0);
        for (const auto offset : {8U, 24U}) {
            std::uintptr_t entries{}; std::uint32_t count{};
            if (!challenge_native_array(reinterpret_cast<void*>(group_data + offset), entries, count, 256)) return;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint64_t handle{}; std::string goal; std::uint8_t completed{};
                if (!read(entries + i * 8, handle) || !handle) return;
                const auto value = n.value(manager, handle, 0, 0);
                if (!identifier(reinterpret_cast<void*>(value + 8), goal) || !read(value + 0x4c, completed) ||
                    !unique.insert(goal).second || authored.size() >= 256) return;
                profile::ChallengeGoal observed{goal, offset == 24};
                const auto cached = std::find_if(definition.goals.begin(), definition.goals.end(),
                    [&](const auto& entry) { return entry.id == goal; });
                const auto text = [&](std::size_t offset, unsigned index, std::string& destination,
                    const std::string* fallback) {
                    // Learn authored localization keys as well as IDs so uncached
                    // activities retain their descriptions on the map after restart.
                    std::string key;
                    if (identifier(reinterpret_cast<void*>(value + offset), key)) destination = std::move(key);
                    else if (fallback) {
                        destination = *fallback;
                        fill_text(handle, value + offset, index, destination);
                    }
                };
                const auto* metadata = cached == definition.goals.end() ? nullptr : &*cached;
                text(0x30, 1, observed.title_key, metadata ? &metadata->title_key : nullptr);
                text(0x38, 3, observed.feed_key, metadata ? &metadata->feed_key : nullptr);
                text(0x40, 2, observed.short_description_key, metadata ? &metadata->short_description_key : nullptr);
                text(0x28, 4, observed.description_key, metadata ? &metadata->description_key : nullptr);
                authored.push_back(std::move(observed));
                if (completed || std::find(saved.begin(), saved.end(), goal) == saved.end()) continue;
                // Native progress hydration uses GetModelField here.
                // FindModel instead expects type metadata, not a model handle.
                const auto field = game::native_data().models.field(manager, handle, 11, UINT32_MAX, false);
                const bool yes = true;
                if (field) n.publish(manager, field, s.base + addr::engine::bool_type, &yes);
            }
        }
    }
    if (!learn) return;
    // Wait for the same complete list on two polls, then retain the current
    // authored identities. This also covers newly installed uncached challenges.
    // Disk IO happens after releasing the native model lock.
    if (r.observed_id != id || r.observed_goals != authored) {
        r.observed_id = id; r.observed_goals = std::move(authored); return;
    }
    if (!authored.empty() && local_challenge(id)->goals != authored && s.store->remember_challenge_goals(id, authored)) {
        r.policy.catalog.at(id).goals = std::move(authored);
        r.changed_groups.insert(id); r.next_catalog_poll = 0;
        dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_goals_learned"}, {"id", id},
            {"goals", r.policy.catalog.at(id).goals.size()}}.dump().c_str());
    }
}

void deliver_local_challenge_completion(std::uintptr_t vm, std::uint32_t pc) noexcept try {
    auto* delivery = challenge_completion_delivery;
    if (!delivery || delivery->delivered || pc || !local_challenges_active()) return;
    PreserveError preserve;
    // EndChallengeV2Callback sends OnAmpActivityEndRecieved_Global using the
    // participant's backend EID. The native sender rejects an empty EID
    // before sending anything. Only the local empty-EID callback gets this
    // client-context delivery, after its normal interpreter has returned.
    constexpr std::array<std::uint32_t, 10> expected{64, 72, 46, 123, 7, 0, 1, 327689, 1441792, 66560};
    constexpr std::array<unsigned char, 16> event_guid{
        0x24,0xc2,0x5b,0xb4,0x9f,0x0d,0x03,0x73,0x28,0x6a,0xeb,0x83,0x76,0xeb,0xe2,0xb6};
    std::uintptr_t resource{}, instance{}, instance_resource{}, type{}, metadata{}, context{};
    std::uint32_t key{}, hash{}, realm_offset{}, realm{};
    std::uint16_t flags{}, size{};
    std::int32_t code{};
    std::array<std::uint32_t, 10> layout{};
    std::array<std::uintptr_t, 7> pages{};
    std::array<unsigned char, 16> guid{};
    std::string reason;
    if (!read(vm + 0x38, resource) || !read(resource + 0x10, key) || key != 0x388b3076) return;
    delivery->blocked_at = "graph_layout";
    if (!read(resource + 0x20, layout) || layout != expected ||
        !read(vm + 0x30, instance) || !read(instance, instance_resource) || instance_resource != resource) return;
    delivery->blocked_at = "event_metadata";
    // The interpreter places the page table after the graph's aligned frame size
    // (resource +20): 40h for this callback, unlike SendEndAmpEvent's 60h.
    const auto page_offset = (layout[0] + 15U) & ~15U;
    if (!read(instance + page_offset, pages) || !read(pages[0] + 16, type) || !read(type, metadata) ||
        !read(metadata, hash) || hash != 619632508U || !read(metadata + 4, flags) || (flags & 0x3e0) != 0x40 ||
        !read(metadata + 6, size) || size != 16 || !read(metadata + 8, guid) || guid != event_guid) return;
    delivery->blocked_at = "event_payload";
    if (!read(pages[2], code) || code != delivery->error_code ||
        !challenge_player_id(reinterpret_cast<void*>(pages[2] + 8), reason)) return;
    delivery->blocked_at = "client_context";
    const auto owner = challenge_owner();
    if (!owner || !read(owner + 0x18, context) || !context ||
        !read(local_runtime().base + addr::engine::context_type_offset, realm_offset) || realm_offset > 0x1000000 ||
        !read(context + realm_offset, realm) || realm != 0xbf0f9789) return;
    // Publish copies the callback's original ErrorCode/Reason into the native
    // event queue. Do not synthesize reward data or retain VM page pointers.
    const std::array<std::uint32_t, 3> options{0, 1, 0};
    delivery->delivered = true;
    challenge_runtime().f.dispatch(context, type, reinterpret_cast<void*>(pages[2]), options.data(), 0);
    dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_completion_delivered"}, {"error_code", code}}.dump().c_str());
} catch (...) {
    dingosdk::logging::event(dingosdk::logging::Channel::progression, "{\"event\":\"local_challenge_completion_delivery_failed\"}");
}

void update_challenge_requests() {
    auto& r = challenge_runtime(); auto& s = local_runtime();
    if (!local_challenges_active()) return;
    auto pending = std::move(r.pending); r.pending.clear();
    for (auto& request : pending) {
        game::NativeDelegateGuard retained; retained.value = request.callback;
        try {
            if (request.failed) throw std::runtime_error("Invalid local challenge request");
            if (request.ending && request.virtual_leave) {
                // Only a virtual participant left: the local attempt is still running.
                std::erase_if(request.players, [](const std::string &p) { return !multiplayer::is_virtual_eid(p); });
            } else if (request.ending) s.store->finish_challenge(request.id, request.attempt, request.goals);
            else { request.attempt = s.store->begin_challenge(request.id); r.attempts[request.id] = request.attempt; }
        } catch (const std::exception& error) {
            request.failed = true;
            dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_save_failed"}, {"id", request.id}, {"reason", error.what()}}.dump().c_str());
        }
        if (request.ending && !request.failed && !request.virtual_leave) {
            r.next_catalog_poll = 0;
            r.next_progress_poll = 0;
            try { update_selected_challenge_progress(false); }
            catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::progression, "{\"event\":\"local_challenge_progress_refresh_failed\"}"); }
        }
        alignas(8) std::array<std::byte, 40> error{};
        if (request.failed) r.f.error(error.data(), 0xc5365a61, "ReSkate could not save challenge progress");
        else s.success(error.data());
        struct Release { void* error; ~Release() { local_runtime().destroy_string(error); } } release{error.data()};
        ChallengeCompletionDelivery delivery;
        if (retained.value) {
            if (request.ending) {
                // Native callback receives Array<CString> with a count prefix.
                struct Players { std::uint32_t capacity{16}, count{}; std::array<const char*, 16> values{}; } players;
                players.count = static_cast<std::uint32_t>(request.players.size());
                for (std::size_t i = 0; i < request.players.size(); ++i) players.values[i] = request.players[i].c_str();
                const auto values = players.values.data();
                // The local player's own result: its empty EID, beside any virtual coop participants.
                const bool local_delivery = request.player.empty() && !request.virtual_leave &&
                    std::ranges::count(request.players, std::string{}) == 1 &&
                    std::ranges::all_of(request.players, [](const std::string &p) { return p.empty() || multiplayer::is_virtual_eid(p); });
                std::memcpy(&delivery.error_code, error.data() + 32, 4);
                ChallengeCompletionScope scope(local_delivery ? &delivery : nullptr);
                r.f.invoke_end(&retained.value, error.data(), &values);
                if (local_delivery && !delivery.delivered)
                    dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", "local_challenge_completion_blocked"},
                        {"id", request.id}, {"stage", delivery.blocked_at}}.dump().c_str());
            } else game::native_data().values.invoke(&retained.value, error.data());
        }
        dingosdk::logging::event(dingosdk::logging::Channel::progression, dingosdk::Json{{"event", request.ending ? "local_challenge_ended" : "local_challenge_began"},
            {"id", request.id}, {"attempt", request.attempt}, {"goals", request.goals}, {"players", request.players},
            {"virtual_leave", request.virtual_leave},
            {"callback_present", retained.value != 0}, {"local_delivery", delivery.delivered},
            {"success", !request.failed}}.dump().c_str());
    }
}

void initialize_challenge_functions(std::uintptr_t base) {
    auto& r = challenge_runtime();
    r.policy = profile::challenge_policy(*local_runtime().store->shared_snapshot());
    hidden_challenges().store(local_runtime().store->bool_option(profile::hide_challenges_option).value_or(false),
        std::memory_order_release);
    r.title_fallbacks.clear();
    // A challenge whose title the game cannot translate (retail had it from the backend) shows
    // its id made readable: "Plot-009-OTS-01" -> "Plot 9 OTS 1", "OTSChallenge001" -> "OTS Challenge 1".
    const auto readable = [](std::string_view id) {
        std::string title;
        const auto is_upper = [](char c) { return c >= 'A' && c <= 'Z'; };
        const auto is_lower = [](char c) { return c >= 'a' && c <= 'z'; };
        const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
        for (std::size_t i = 0; i < id.size(); ++i) {
            const char c = id[i] == '_' || id[i] == '-' ? ' ' : id[i];
            const char previous = title.empty() ? ' ' : title.back();
            // A word starts at a letter-digit change and at the capital that begins "Challenge" in "OTSChallenge".
            const bool boundary = c != ' ' && previous != ' ' &&
                ((is_digit(c) != is_digit(previous)) ||
                 (is_upper(c) && is_upper(previous) && i + 1 < id.size() && is_lower(id[i + 1])) ||
                 (is_upper(c) && is_lower(previous)));
            if (boundary) title += ' ';
            // Leading zeros of a number: "009" -> "9".
            if (c == '0' && (title.empty() || title.back() == ' ') && i + 1 < id.size() && is_digit(id[i + 1])) continue;
            if (c != ' ' || (!title.empty() && title.back() != ' ')) title += c;
        }
        while (!title.empty() && title.back() == ' ') title.pop_back();
        return title;
    };
    for (const auto& [id, entry] : r.policy.catalog) {
        std::string title = readable(id);
        r.title_fallbacks.emplace(id, title);
        if (!entry.title_key.empty()) r.title_fallbacks.emplace(entry.title_key, std::move(title));
    }
    r.f.invoke_end = reinterpret_cast<decltype(r.f.invoke_end)>(base + challenge_end_invoke_contract.rva);
    r.f.error = reinterpret_cast<decltype(r.f.error)>(base + challenge_error_contract.rva);
    r.f.index_insert = reinterpret_cast<decltype(r.f.index_insert)>(base + challenge_index_insert_contract.rva);
    r.f.index_append = reinterpret_cast<decltype(r.f.index_append)>(base + challenge_index_append_contract.rva);
    r.f.insert = reinterpret_cast<decltype(r.f.insert)>(base + challenge_insert_contract.rva);
    r.f.dispatch = reinterpret_cast<decltype(r.f.dispatch)>(base + challenge_dispatch_contract.rva);
    r.f.create_request = reinterpret_cast<decltype(r.f.create_request)>(base + challenge_request_create_contract.rva);
    r.f.release_request = reinterpret_cast<decltype(r.f.release_request)>(base + object_release_contract.rva);
}
}
