#include "network_object_runtime.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Multiplayer/Session/object_state.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "local_buildkit_limits.h"
#include "local_placements_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include <algorithm>
#include <atomic>
#include <tuple>

namespace dingosdk::profile_runtime {
namespace network_objects_detail {
using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
struct Live {
    std::uint64_t entity{}, delete_sent{}, delete_started{}, refresh{};
    profile::PlacedObject applied;
};
struct Create {
    Key key;
    profile::PlacedObject object;
    std::vector<std::uint64_t> before; // sorted: the native entities when it was sent
    std::uint64_t sent{};
    std::uint32_t source{};
    bool existed(std::uint64_t entity) const { return std::binary_search(before.begin(), before.end(), entity); }
};
struct State {
    std::string map, status;
    std::map<Key, profile::PlacedObject> desired;
    std::map<Key, Live> live;
    std::set<Key> dirty;
    std::optional<Create> creating;
    std::map<std::uint32_t, Create> late_creates;
    std::set<Key> failed_creates;
    std::uint64_t next_poll{};
    std::optional<Key> move_cursor;
    std::optional<Key> dirty_cursor;
    std::map<Key, profile::PlacedObject> stage; // also in `desired`
};
// The style stage's owner. Steam ids are far larger, so no player has it.
constexpr std::uint64_t stage_owner = 1;
State &state() {
    static State value;
    return value;
}
// Other players' objects are still being created: the client tick keeps calling
// update_network_objects (tick_network_objects) instead of waiting for the 500 ms
// customization update, which paced a big park at two objects a second.
std::atomic<bool> creates_pending{};
std::atomic<std::uint32_t> extra_budget{};
// Do not reuse a tag after level teardown: a late queued message must be rejected.
std::uint32_t next_source{};
void refresh_budget() {
    const auto &s = state();
    auto count = s.desired.size();
    for (const auto &[key, live] : s.live) {
        (void)live;
        if (!s.desired.contains(key))
            ++count;
    }
    if (s.creating && !s.desired.contains(s.creating->key) && !s.live.contains(s.creating->key))
        ++count;
    for (const auto &[source, create] : s.late_creates) {
        (void)source;
        if (!s.desired.contains(create.key) && !s.live.contains(create.key)) ++count;
    }
    extra_budget.store(static_cast<std::uint32_t>(count), std::memory_order_release);
}
} // namespace network_objects_detail
std::uint32_t network_object_extra_budget() noexcept {
    return network_objects_detail::extra_budget.load(std::memory_order_acquire);
}
void reset_network_object_world() {
    network_objects_detail::state() = {};
    network_objects_detail::creates_pending.store(false, std::memory_order_release);
    network_objects_detail::refresh_budget();
}
void retire_local_network_object(std::uint64_t entity, const profile::PlacedObject& object) {
    if (!entity) return;
    // Zero owner/epoch cannot be received from the network. Keep these handles
    // excluded from autosave until native deletion is actually acknowledged.
    network_objects_detail::state().live.try_emplace(
        network_objects_detail::Key{0, 0, entity},
        network_objects_detail::Live{entity, 0, 0, 0, object});
    network_objects_detail::refresh_budget();
}
bool network_objects_inflight() {
    const auto &s = network_objects_detail::state();
    const auto now = GetTickCount64();
    return s.creating.has_value() || std::any_of(s.live.begin(), s.live.end(),
                                                 [&](const auto &row) {
                                                     return (std::get<0>(row.first) == 0 || row.second.delete_sent) &&
                                                            (!row.second.delete_started ||
                                                             now <= row.second.delete_started + 10000);
                                                 });
}
std::uint64_t network_object_creation_owner(std::uint32_t source) {
    const auto &s = network_objects_detail::state();
    if (s.creating && source == s.creating->source) return std::get<0>(s.creating->key);
    const auto late = s.late_creates.find(source);
    return late == s.late_creates.end() ? 0 : std::get<0>(late->second.key);
}
bool network_object_observe(std::uint64_t entity, const profile::PlacedObject &object, std::uint32_t source) {
    auto &s = network_objects_detail::state();
    for (const auto &[key, live] : s.live) {
        (void)key;
        if (live.entity == entity)
            return true;
    }
    if (s.creating && source == s.creating->source && !s.creating->existed(entity) &&
        s.creating->object.item == object.item) {
        auto applied = object;
        applied.id = s.creating->object.id;
        s.live[s.creating->key] = {entity, 0, 0, 0, std::move(applied)};
        s.dirty.insert(s.creating->key);
        s.creating.reset();
        network_objects_detail::refresh_budget();
        return true;
    }
    if (const auto late = s.late_creates.find(source);
        late != s.late_creates.end() && !late->second.existed(entity) &&
        late->second.object.item == object.item) {
        auto applied = object;
        applied.id = late->second.object.id;
        s.live[late->second.key] = {entity, 0, 0, 0, std::move(applied)};
        s.dirty.insert(late->second.key);
        s.failed_creates.erase(late->second.key);
        s.late_creates.erase(late);
        network_objects_detail::refresh_budget();
        return true;
    }
    return false;
}
void update_network_objects() {
    auto &s = network_objects_detail::state();
    auto &r = placements_runtime();
    const auto now = GetTickCount64();
    if (!s.creating && s.live.empty() && s.desired.empty()) {
        s.status = "0 remote objects synced.";
        network_objects_detail::creates_pending.store(false, std::memory_order_release);
        return;
    }
    if (s.creating) {
        if (now <= s.creating->sent + 10000) return;
        // Stop blocking local edits, but retain the exact source ownership so a
        // delayed callback can never leak the remote object into local autosave.
        s.status = "A network object was not acknowledged; local editing remains available.";
        s.failed_creates.insert(s.creating->key);
        s.late_creates.emplace(s.creating->source, std::move(*s.creating));
        s.creating.reset();
        network_objects_detail::refresh_budget();
    }
    if (park_editor_owns_placements() || r.clearing ||
        !network_object_native_ready() || now < r.map_since + 3000 || now < s.next_poll)
        return;
    s.next_poll = now + 50;
    const auto channel = placement_client_channel();
    if (!channel)
        return;
    auto table = placement_manager_ids(r.manager);
    if (!table)
        return;
    // Sorted by entity for lookups: a big park has thousands of rows.
    auto &native = *table;
    std::stable_sort(native.begin(), native.end(), [](const auto &a, const auto &b) { return a.entity < b.entity; });
    const auto native_row = [&](std::uint64_t entity) -> const PlacementId * {
        const auto it = std::lower_bound(native.begin(), native.end(), entity,
                                         [](const PlacementId &row, std::uint64_t value) { return row.entity < value; });
        return it != native.end() && it->entity == entity ? &*it : nullptr;
    };
    std::vector<std::uint32_t> erase;
    unsigned failed_deletes{};
    for (auto it = s.live.begin(); it != s.live.end();) {
        auto &live = it->second;
        const auto found = native_row(live.entity);
        if (!found) {
            it = s.live.erase(it);
            continue;
        }
        const auto wanted = s.desired.find(it->first);
        if (s.map != r.map || wanted == s.desired.end() || wanted->second.item != live.applied.item) {
            if (live.delete_started && now > live.delete_started + 10000) {
                ++failed_deletes;
                ++it;
                continue;
            }
            if (!live.delete_sent || now > live.delete_sent + 1000) {
                erase.push_back(found->id);
                live.delete_sent = now;
                if (!live.delete_started) live.delete_started = now;
                if (erase.size() >= 64)
                    break;
            }
        }
        ++it;
    }
    network_objects_detail::refresh_budget();
    if (!erase.empty()) {
        send_placement_delete(channel, erase);
        network_objects_detail::creates_pending.store(true, std::memory_order_release);
        return;
    }
    // Clear session copies before restoring the guest's personal layout.
    // Waiting on r.restore above would deadlock those two operations.
    if (network_objects_inflight() || s.map != r.map || r.inflight || !r.restore.empty())
        return;
    unsigned missing{};
    unsigned failed{};
    for (const auto &[key, object] : s.desired) {
        if (s.live.contains(key))
            continue;
        if (s.failed_creates.contains(key)) {
            ++failed;
            continue;
        }
        const auto item = cosmetic_runtime().items.find(object.item);
        if (item == cosmetic_runtime().items.end() || !item->second.build_kit) {
            ++missing;
            continue;
        }
        if (network_objects_detail::next_source == 0x00ffffff) {
            s.status = "Network object request IDs exhausted; restart the game.";
            return;
        }
        network_objects_detail::Create create{
            key, object, {}, now, network_object_source_prefix | ++network_objects_detail::next_source};
        create.before.reserve(native.size());
        for (const auto &row : native)
            create.before.push_back(row.entity); // already sorted
        s.creating = std::move(create);
        // Reserve native capacity for peer copies before their create request.
        // Their presence must not consume the saved local object's budget.
        update_buildkit_limits();
        if (!queue_placement_create(object, item->second.hash, s.creating->source)) {
            s.creating.reset();
            network_objects_detail::refresh_budget();
            // Each attempt uses a request id: while creating fails, try twice a second.
            s.next_poll = now + 500;
        }
        // One create in flight at a time (the native queue holds one request and
        // the server tick takes one per tick); the next goes out once it is in.
        network_objects_detail::creates_pending.store(true, std::memory_order_release);
        return;
    }
    network_objects_detail::creates_pending.store(false, std::memory_order_release);
    s.status = std::to_string(s.live.size()) + " remote objects synced.";
    if (missing)
        s.status += " " + std::to_string(missing) + " assets are not installed locally.";
    if (failed)
        s.status += " " + std::to_string(failed) + " creations timed out without blocking local editing.";
    if (failed_deletes)
        s.status += " " + std::to_string(failed_deletes) + " removals timed out; reload to clear them.";
}
void update_network_object_moves() {
    auto &s = network_objects_detail::state();
    if (s.map != placements_runtime().map || !network_object_native_ready() || s.live.empty())
        return;
    const auto now = GetTickCount64();
    // An object marked dirty, or whose target differs from what was last applied, is moved at once; the
    // rest are put back where their owner has them every 10 s (the game's own tools can
    // move a copy, and a move made before its physics body existed placed only the
    // model), not every second as before. A failed move retries a second later.
    const auto move = [&](auto it, bool dirty) {
        auto &live = it->second;
        const auto wanted = s.desired.find(it->first);
        if (!live.delete_sent && wanted != s.desired.end() && wanted->second.item == live.applied.item &&
            (dirty || wanted->second != live.applied || now >= live.refresh)) {
            try {
                move_network_object(live.entity, wanted->second);
                live.applied = wanted->second;
                live.refresh = now + 10000;
            } catch (...) {
                s.status = "Waiting for a remote object's native transform.";
                live.refresh = now + 1000;
            }
        }
    };
    // Fresh edits go first. The walk over the rest of the park catches targets a
    // failed move left behind, without making a recently moved object wait
    // behind thousands of props.
    unsigned used{};
    while (!s.dirty.empty() && used < 128) {
        auto next = s.dirty_cursor ? s.dirty.upper_bound(*s.dirty_cursor) : s.dirty.begin();
        if (next == s.dirty.end())
            next = s.dirty.begin();
        const auto key = *next;
        s.dirty_cursor = key;
        s.dirty.erase(next);
        if (auto found = s.live.find(key); found != s.live.end()) {
            move(found, true);
            ++used;
        }
    }
    auto it = s.move_cursor ? s.live.upper_bound(*s.move_cursor) : s.live.begin();
    if (it == s.live.end())
        it = s.live.begin();
    const auto budget = std::min<std::size_t>(128 - used, s.live.size());
    for (std::size_t i = 0; i < budget; ++i) {
        move(it, false);
        s.move_cursor = it->first;
        if (++it == s.live.end())
            it = s.live.begin();
    }
}
} // namespace dingosdk::profile_runtime

namespace dingosdk {
// Everything the other players have placed that is shown here, all of them together. Each
// owner has a limit of its own; without one for the session, every player a session can hold
// could add a full layout to the native world.
constexpr std::size_t max_remote_objects = 16384;
void set_remote_network_objects(std::string_view map, std::span<const NetworkObjectOwner> owners) {
    using namespace profile_runtime;
    if (owners.size() > multiplayer::max_remote_players)
        return;
    for (const auto &owner : owners)
        if (!owner.owner || !owner.epoch || owner.objects.size() > multiplayer::max_owned_objects)
            return;
    {
        // The session hands over every owner's objects ten times a second. When they
        // are exactly the targets already held (the usual case), there is nothing to
        // rebuild: tens of thousands of rows in a full lobby.
        std::lock_guard lock(local_runtime().native_mutex);
        auto &s = network_objects_detail::state();
        std::size_t total{};
        bool same = s.map == map;
        for (const auto &owner : owners) {
            for (const auto &object : owner.objects) {
                if (total == max_remote_objects) break;
                ++total;
                if (!same) continue;
                const auto found = s.desired.find({owner.owner, owner.epoch, object.id});
                same = found != s.desired.end() && found->second.id == object.id && found->second.item == object.item &&
                       found->second.position == object.position && found->second.rotation == object.rotation &&
                       found->second.scale == object.scale;
            }
        }
        if (same && total + s.stage.size() == s.desired.size() &&
            std::all_of(s.failed_creates.begin(), s.failed_creates.end(),
                        [&](const auto &key) { return s.desired.contains(key); }))
            return;
    }
    std::map<network_objects_detail::Key, profile::PlacedObject> desired;
    for (const auto &owner : owners) {
        for (const auto &object : owner.objects) {
            // Owners come in a stable order: past the limit, the later ones' objects are left out.
            if (desired.size() == max_remote_objects)
                break;
            if (!multiplayer::valid_network_object(object))
                return;
            if (!desired
                     .emplace(network_objects_detail::Key{owner.owner, owner.epoch, object.id},
                               profile::PlacedObject{object.id, object.item, object.position, object.rotation,
                                                     object.scale})
                     .second)
                return;
        }
    }
    std::lock_guard lock(local_runtime().native_mutex);
    auto &s = network_objects_detail::state();
    desired.insert(s.stage.begin(), s.stage.end());
    for (const auto &[key, object] : desired) {
        const auto found = s.live.find(key);
        if (found != s.live.end() && found->second.applied != object)
            s.dirty.insert(key);
    }
    std::erase_if(s.failed_creates, [&](const auto &key) { return !desired.contains(key); });
    s.map = map;
    s.desired = std::move(desired);
    network_objects_detail::refresh_budget();
}
void clear_remote_network_objects() {
    std::lock_guard lock(profile_runtime::local_runtime().native_mutex);
    auto &s = profile_runtime::network_objects_detail::state();
    s.desired = s.stage; // Retain live handles until native deletion is acknowledged.
    s.dirty.clear();
    profile_runtime::network_objects_detail::refresh_budget();
}
void remove_remote_network_objects(std::uint64_t owner, std::uint64_t epoch) {
    if (!owner || !epoch)
        return;
    std::lock_guard lock(profile_runtime::local_runtime().native_mutex);
    auto &s = profile_runtime::network_objects_detail::state();
    std::erase_if(s.desired, [&](const auto &row) {
        return std::get<0>(row.first) == owner && std::get<1>(row.first) == epoch;
    });
    std::erase_if(s.dirty,
                  [&](const auto &key) { return std::get<0>(key) == owner && std::get<1>(key) == epoch; });
    profile_runtime::network_objects_detail::refresh_budget();
}
bool set_stage_objects(std::string_view map, std::span<const profile::PlacedObject> objects) {
    using namespace profile_runtime;
    using network_objects_detail::Key, network_objects_detail::stage_owner;
    std::lock_guard lock(local_runtime().native_mutex);
    auto &s = network_objects_detail::state();
    if (map.empty() || placements_runtime().map != map) return false;
    // Other players' objects on another map: not the stage's to replace.
    if (s.map != map && std::ranges::any_of(s.desired, [](const auto &row) { return std::get<0>(row.first) != stage_owner; }))
        return false;
    std::map<Key, profile::PlacedObject> stage;
    for (const auto &object : objects) stage.emplace(Key{stage_owner, 1, object.id}, object);
    for (const auto &[key, object] : s.stage)
        if (!stage.contains(key)) s.desired.erase(key);
    // Moved again even when unchanged: a move made before an object's physics body exists places only its model.
    for (const auto &[key, object] : stage) {
        if (s.live.contains(key)) s.dirty.insert(key);
        s.desired[key] = object;
    }
    s.stage = std::move(stage);
    s.map = map;
    network_objects_detail::refresh_budget();
    network_objects_detail::creates_pending.store(true, std::memory_order_release);
    return true;
}
std::optional<profile::PlacedObject> stage_object(std::uint64_t id) {
    std::lock_guard lock(profile_runtime::local_runtime().native_mutex);
    const auto &s = profile_runtime::network_objects_detail::state();
    const auto live = s.live.find({profile_runtime::network_objects_detail::stage_owner, 1, id});
    if (live == s.live.end() || live->second.delete_sent) return std::nullopt;
    return live->second.applied;
}
std::optional<std::string> stage_map() {
    using namespace profile_runtime;
    std::lock_guard lock(local_runtime().native_mutex);
    if (!local_runtime().active.load(std::memory_order_acquire) || !placement_session_ready() || !network_object_native_ready())
        return std::nullopt;
    return placements_runtime().map;
}
std::string network_object_status() {
    std::lock_guard lock(profile_runtime::local_runtime().native_mutex);
    return profile_runtime::network_objects_detail::state().status;
}
void tick_network_objects() noexcept {
    using namespace profile_runtime;
    if (!network_objects_detail::creates_pending.load(std::memory_order_acquire))
        return;
    PreserveError preserve;
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        if (!local_runtime().active.load(std::memory_order_acquire) ||
            cosmetic_runtime().update_thread != GetCurrentThreadId())
            return;
        // The order update_placement_restore gives these; update_network_objects
        // itself still waits 50 ms between polls.
        if (!placement_session_ready() || park_editor_owns_placements() || placements_runtime().clearing)
            return;
        update_network_objects();
    } catch (...) {
    }
}
} // namespace dingosdk
