#include "network_object_runtime.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/local_placements.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Multiplayer/Session/object_state.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "local_buildkit_limits.h"
#include "local_placements_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <set>
#include <tuple>

namespace dingosdk::profile_runtime {
namespace network_objects_detail {
using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
struct Live {
    std::uint64_t entity{}, delete_sent{}, delete_started{}, refresh{};
    profile::PlacedObject applied;
    // Whether the native ghost is forced on it, and when that is next made sure of.
    bool ghosted{};
    std::uint64_t ghost_refresh{};
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
};
State &state() {
    static State value;
    return value;
}
// The players whose objects are solid for the local player. Not part of State: it outlives
// a change of world. Under the native lock, like State.
std::set<std::uint64_t> &solid_owners() {
    static std::set<std::uint64_t> value;
    return value;
}
// How the client knows a placed object: the object's number, which is the index part of the
// server's entity for it (bits 8 up; the low byte is that entity's generation). The client's
// copy has an entity of its own, but its component (+0x24) and its record (+0x28) carry this
// number (measured 2026-10-13: server entities 0x25'00000503 and 0x25'00001104 were objects
// 5 and 0x11 on the client).
std::uint32_t object_number(std::uint64_t server_entity) noexcept {
    return static_cast<std::uint32_t>(server_entity >> 8) & 0xffffff;
}
// The numbers of the objects other players placed, and who placed each, for the game's "is
// this object mine?" (object_owned_hook): every copy has the local player as its native owner,
// so the game would offer the owner's menu on all of them. The game asks as each object appears
// on the client, so a number is here from the moment the server's entity is known. Written by
// the object update, read by the client's scripts.
std::mutex remote_objects_mutex;
std::map<std::uint32_t, std::uint64_t> remote_objects;
std::size_t published_objects{}; // how many objects there were when remote_objects was last rebuilt
// What the local player chose for single objects with the game's own menu (Disable, and
// enabling one again): that is the game's ghost native, called on the client. A choice stands
// over the owner's setting until that setting is changed. Under remote_objects_mutex.
std::map<std::uint32_t, bool> object_choices;
void publish_remote_objects(const State &s) {
    std::map<std::uint32_t, std::uint64_t> numbers;
    for (const auto &[key, live] : s.live)
        if (std::get<0>(key)) numbers[object_number(live.entity)] = std::get<0>(key);
    std::lock_guard lock(remote_objects_mutex);
    remote_objects = std::move(numbers);
    std::erase_if(object_choices, [](const auto &choice) { return !remote_objects.contains(choice.first); });
    published_objects = s.live.size();
}
void add_remote_object(const Key &key, std::uint64_t entity) {
    if (!std::get<0>(key)) return;
    std::lock_guard lock(remote_objects_mutex);
    remote_objects[object_number(entity)] = std::get<0>(key);
}
// The number of the placed object an entity is part of, as the game's own checks find it.
std::optional<std::uint32_t> client_object_number(std::uint64_t entity) noexcept {
    namespace placements = addr::local_placements;
    const auto base = local_runtime().base;
    std::array<std::uint64_t, 2> root{};
    reinterpret_cast<void (*)(std::uint64_t *, std::uint64_t, bool)>(base + placements::object_root)(root.data(), entity, true);
    std::array<std::uint64_t, 8> found{}; // [1] valid, [2] the component
    reinterpret_cast<void (*)(const std::uint64_t *, std::uint64_t *)>(base + placements::object_component)(root.data(), found.data());
    std::uint32_t number{};
    if (!found[1] || !found[2] || !memory::peek(static_cast<std::uintptr_t>(found[2]) + 0x24, number)) return std::nullopt;
    return number;
}
void (*object_ghost_original)(const std::uint64_t *, bool){};
void object_ghost_hook(const std::uint64_t *entity, bool ghost) {
    object_ghost_original(entity, ghost);
    try {
        if (!entity) return;
        const auto number = client_object_number(*entity);
        if (!number) return;
        std::lock_guard lock(remote_objects_mutex);
        if (remote_objects.contains(*number)) object_choices[*number] = ghost;
    } catch (...) {}
}
// Another player's object is not the local player's to edit: the game's own check for the
// card's Edit button does not ask whose the object is (every object is its owner's to see,
// where the game runs its own service).
bool (*objects_editable_original)(const std::uint64_t *const *){};
bool objects_editable_hook(const std::uint64_t *const *entities) {
    const bool editable = objects_editable_original(entities);
    if (!editable || !entities) return editable;
    try {
        const std::uint64_t *first{};
        std::uint32_t count{};
        if (!memory::peek(reinterpret_cast<std::uintptr_t>(entities), first) || !first ||
            !memory::peek(reinterpret_cast<std::uintptr_t>(first) - 4, count)) return editable;
        count &= 0x7fffffff;
        for (std::uint32_t i = 0; i < count && i < 64; ++i) {
            const auto number = client_object_number(first[i]);
            if (!number) continue;
            std::unique_lock lock(remote_objects_mutex, std::try_to_lock);
            if (lock.owns_lock() && remote_objects.contains(*number)) return false;
        }
    } catch (...) {}
    return editable;
}
bool (*object_owned_original)(const std::uint64_t *){};
bool object_owned_hook(const std::uint64_t *entity) {
    const bool owned = object_owned_original(entity);
    if (!owned || !entity) return owned;
    try {
        const auto number = client_object_number(*entity);
        if (!number) return owned;
        std::unique_lock lock(remote_objects_mutex, std::try_to_lock);
        return !(lock.owns_lock() && remote_objects.contains(*number));
    } catch (...) {
        return owned;
    }
}
void install_object_owned_hook() noexcept {
    static bool attempted{};
    if (std::exchange(attempted, true)) return;
    namespace placements = addr::local_placements;
    const auto base = local_runtime().base;
    std::array<unsigned char, 16> owned{}, root{}, component{};
    auto *const target = reinterpret_cast<void *>(base + placements::object_owned);
    if (!memory::read(base + placements::object_owned, owned) || owned != placements::object_owned_prefix ||
        !memory::read(base + placements::object_root, root) || root != placements::object_root_prefix ||
        !memory::read(base + placements::object_component, component) || component != placements::object_component_prefix ||
        hook_prepare(target, reinterpret_cast<void *>(&object_owned_hook), reinterpret_cast<void **>(&object_owned_original)) != HookOk ||
        hook_enable(target) != HookOk) {
        logging::write(logging::Level::warning, logging::Channel::runtime,
                       "Object owners: the game's owner check could not be replaced; other players' objects offer the owner's menu.");
        return;
    }
    std::array<unsigned char, 16> editable{};
    auto *const editable_target = reinterpret_cast<void *>(base + placements::objects_editable);
    if (!memory::read(base + placements::objects_editable, editable) || editable != placements::objects_editable_prefix ||
        hook_prepare(editable_target, reinterpret_cast<void *>(&objects_editable_hook),
                     reinterpret_cast<void **>(&objects_editable_original)) != HookOk ||
        hook_enable(editable_target) != HookOk)
        logging::write(logging::Level::warning, logging::Channel::runtime,
                       "Object owners: the game's edit check could not be replaced; other players' objects offer Edit.");
    // The same functions tell which object the menu's Disable was pressed on.
    std::array<unsigned char, 16> ghost{};
    auto *const ghost_target = reinterpret_cast<void *>(base + placements::object_ghost);
    if (!memory::read(base + placements::object_ghost, ghost) || ghost != placements::object_ghost_prefix ||
        hook_prepare(ghost_target, reinterpret_cast<void *>(&object_ghost_hook), reinterpret_cast<void **>(&object_ghost_original)) != HookOk ||
        hook_enable(ghost_target) != HookOk)
        logging::write(logging::Level::warning, logging::Channel::runtime,
                       "Object ghosting: the menu's own Disable could not be followed; an object disabled there turns solid again.");
}
// Makes placed object `number` a ghost for the local player, or lets it be solid again,
// through the game's own record of it (local_placements.h). False when the client has no
// record of it yet (it comes a moment after the object), or when this build's functions are
// not the ones expected: objects then stay solid, as they always were.
bool set_object_ghost(std::uint32_t number, bool ghost) noexcept {
    namespace placements = addr::local_placements;
    static const bool known = [] {
        std::array<unsigned char, 16> lookup{};
        const auto base = local_runtime().base;
        const bool same = memory::read(base + placements::realm_context, lookup) && lookup == placements::realm_context_prefix;
        if (!same)
            logging::write(logging::Level::warning, logging::Channel::runtime,
                           "Object ghosting: the game's functions differ; other players' objects stay solid.");
        return same;
    }();
    if (!known) return false;
    const auto base = local_runtime().base;
    // The records are the client's, and this runs in the local server's context, where the
    // game's own function (which takes the calling thread's context) finds no record store.
    // So the client's context is asked for by name, the record is marked here, and the store
    // is told to decide every record again, which the client does on its next update.
    std::uintptr_t client{};
    reinterpret_cast<std::uintptr_t *(*)(std::uintptr_t *, std::uint32_t)>(base + placements::realm_context)(
        &client, placements::client_realm);
    std::uint32_t offset{};
    std::uintptr_t first{}, last{};
    std::uint8_t ready{};
    if (!client || !memory::peek(base + placements::client_ready_offset, offset) || offset > 0x1000000) return false;
    const auto store = client + offset;
    if (!memory::peek(store + 0x100, ready) || !ready || !memory::peek(store + 0x48, first) || !memory::peek(store + 0x50, last) ||
        !first || last < first || (last - first) % 0x50 || (last - first) / 0x50 > 65536)
        return false;
    // An object can have several records under its number; the active one carries the flags.
    std::uintptr_t record{};
    for (auto at = first; at != last && !record; at += 0x50) {
        std::uint32_t held{};
        std::uint8_t active{};
        if (memory::peek(at + 0x28, held) && held == number && memory::peek(at + 0x3a, active) && active) record = at;
    }
    if (!record) return false;
    // As the game's own native sets them: a forced ghost, or none (and solid forced only on a
    // record that was not a forced ghost, which ReSkate never asks for).
    auto *const bytes = reinterpret_cast<std::uint8_t *>(record);
    if ((bytes[0x3b] != 0) != ghost) {
        bytes[0x3c] = 0;
        bytes[0x3b] = ghost;
        *reinterpret_cast<std::uint8_t *>(store + 0xfb) = 1;
    }
    return true;
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
std::uint64_t network_object_owner_of(std::uint64_t client_entity) noexcept {
    using namespace network_objects_detail;
    try {
        if (!client_entity) return 0;
        const auto number = client_object_number(client_entity);
        if (!number) return 0;
        std::unique_lock lock(remote_objects_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return 0;
        const auto remote = remote_objects.find(*number);
        return remote == remote_objects.end() ? 0 : remote->second;
    } catch (...) {
        return 0;
    }
}
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
        network_objects_detail::add_remote_object(s.creating->key, entity);
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
        network_objects_detail::add_remote_object(late->second.key, entity);
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
    if (s.live.empty() && network_objects_detail::published_objects) network_objects_detail::publish_remote_objects(s);
    if (s.map != placements_runtime().map || !network_object_native_ready() || s.live.empty())
        return;
    const auto now = GetTickCount64();
    // An object whose target differs from what was last applied is moved at once; the
    // rest are put back where their owner has them every 10 s (the game's own tools can
    // move a copy, and a move made before its physics body existed placed only the
    // model), not every second as before. A failed move retries a second later.
    network_objects_detail::install_object_owned_hook();
    const auto move = [&](auto it) {
        auto &live = it->second;
        const auto wanted = s.desired.find(it->first);
        // Another player's object is a ghost unless they were made solid. Set when that
        // changes, and a ghost again with each refresh: one forced before the game had its
        // record of the object did nothing.
        if (const auto owner = std::get<0>(it->first); owner && !live.delete_sent && now >= live.ghost_refresh) {
            const auto number = network_objects_detail::object_number(live.entity);
            bool ghost = !network_objects_detail::solid_owners().contains(owner);
            {
                std::lock_guard choices(network_objects_detail::remote_objects_mutex);
                if (const auto chosen = network_objects_detail::object_choices.find(number);
                        chosen != network_objects_detail::object_choices.end()) ghost = chosen->second;
            }
            // The client's record of a new object comes a moment after the object: try again soon.
            const bool set = network_objects_detail::set_object_ghost(number, ghost);
            if (set) live.ghosted = ghost;
            live.ghost_refresh = now + (set ? 10000 : 1000);
        }
        if (!live.delete_sent && wanted != s.desired.end() && wanted->second.item == live.applied.item &&
            (wanted->second != live.applied || now >= live.refresh)) {
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
            move(found);
            ++used;
        }
    }
    auto it = s.move_cursor ? s.live.upper_bound(*s.move_cursor) : s.live.begin();
    if (it == s.live.end())
        it = s.live.begin();
    const auto budget = std::min<std::size_t>(128 - used, s.live.size());
    for (std::size_t i = 0; i < budget; ++i) {
        move(it);
        s.move_cursor = it->first;
        if (++it == s.live.end())
            it = s.live.begin();
    }
    // When an object went: its number must not name whatever takes it next.
    if (s.live.size() != network_objects_detail::published_objects) network_objects_detail::publish_remote_objects(s);
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
        if (same && total == s.desired.size() &&
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
    s.desired.clear(); // Retain live handles until native deletion is acknowledged.
    s.dirty.clear();
    profile_runtime::network_objects_detail::refresh_budget();
}
void set_network_object_owner_solid(std::uint64_t owner, bool solid) {
    using namespace profile_runtime;
    if (!owner) return;
    std::lock_guard lock(local_runtime().native_mutex);
    auto &owners = network_objects_detail::solid_owners();
    if (solid == owners.contains(owner)) return;
    if (solid) owners.insert(owner);
    else owners.erase(owner);
    // Their objects go first in the next pass.
    auto &s = network_objects_detail::state();
    std::lock_guard choices(network_objects_detail::remote_objects_mutex);
    for (auto &[key, live] : s.live)
        if (std::get<0>(key) == owner) {
            network_objects_detail::object_choices.erase(network_objects_detail::object_number(live.entity));
            live.ghost_refresh = 0;
            s.dirty.insert(key);
        }
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
