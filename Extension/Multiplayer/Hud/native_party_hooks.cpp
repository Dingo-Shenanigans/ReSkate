#include "native_party_internal.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Engine/Core/Platform/memory.h"
#include "follow_camera.h"
#include "Engine/Game/Build/20260929/profile.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>

namespace dingosdk::multiplayer {
using namespace native_party_detail;
namespace {
Handle find_model_hook(Address manager, Address type, std::uint64_t id,
                       std::uint32_t name_space, std::uint8_t realm) {
    auto &s = state();
    const auto native = s.find_model(manager, type, id, name_space, realm);
    // UpdatePartyList uses FindModel directly, bypassing UIPlayerManager's
    // getter. Preserve real native records and expose only our admitted
    // roster/friends to the exact client UIInfo identity lookup.
    if (native || !id || realm != 0 || type != s.base + engine::player_info_type ||
        name_space != 0xa941e23f || !s.installed.load(std::memory_order_acquire)) return native;
    try {
        const auto p = s.published.load(std::memory_order_acquire);
        if (!p || p->manager != manager) return 0;
        const auto find = [&](const Record *record) -> Handle {
            if (!record) return 0;
            game::ModelWriteLock lock(manager);
            return game::native_data().models.value(manager, record->handle, 0, 0) ? record->handle : 0;
        };
        if (const auto handle = find(p->record(id))) return handle;
        return find(p->friend_record(id));
    } catch (...) { return 0; }
}
std::uint64_t get_info_hook(Address ui, std::uint64_t id) {
    try {
    const auto published = state().published.load(std::memory_order_acquire);
    if (state().installed.load(std::memory_order_acquire) && published && id) {
        const auto record = published->record(id);
        if (const auto found = record ? record : published->friend_record(id)) {
            Address manager{};
            if (!memory::peek(ui + 0x140, manager) || manager != published->manager) return 0;
            game::ModelWriteLock lock(manager);
            return game::native_data().models.value(manager, found->handle, 0, 0) ? found->handle : 0;
        }
    }
    } catch (...) { return 0; }
    return state().get_info(ui, id);
}
void is_member_hook(const std::uint64_t *id, bool *result) {
    state().is_member(id, result);
    const auto published = state().published.load(std::memory_order_acquire);
    if (!state().installed.load(std::memory_order_acquire) || !published || !id || !result) return;
    if (published->member(*id)) *result = true;
}
void member_ids_hook(Address output) {
    state().member_ids(output);
    const auto published = state().published.load(std::memory_order_acquire);
    if (!state().installed.load(std::memory_order_acquire) || !published) return;
    try {
        Address data{}; std::uint32_t count{};
        require(game::native_array<std::uint64_t>(reinterpret_cast<const void *>(output), data, count, 64),
                "Native party query array exceeds bound.");
        // The IDs already present, read once; appended IDs are added here too.
        std::array<std::uint64_t, 64> ids{};
        require(!count || memory::peek_bytes(data, ids.data(), count * sizeof(std::uint64_t)),
                "Native party memory unavailable.");
        auto known = count;
        for (const auto &record : published->records) {
            if (!record.handle || !record.player.member) continue;
            if (std::find(ids.begin(), ids.begin() + known, record.player.id) != ids.begin() + known) continue;
            const auto slot = reinterpret_cast<Address (*)(Address, Address)>(state().base + party::array_append)(output, 0);
            require(slot, "Native party ID allocation failed.");
            reinterpret_cast<void (*)(Address)>(state().base + party::id_construct)(slot);
            std::memcpy(reinterpret_cast<void *>(slot), &record.player.id, sizeof(record.player.id));
            // Append can reallocate; never reuse the old borrowed array pointer.
            require(game::native_array<std::uint64_t>(reinterpret_cast<const void *>(output), data, count, 64),
                    "Native party ID allocation changed its array.");
            if (known < ids.size()) ids[known++] = record.player.id;
        }
    } catch (const std::exception &e) { status(e.what()); }
}
void members_with_leader_hook(Address output) {
    state().members_with_leader(output);
    const auto published = state().published.load(std::memory_order_acquire);
    if (!state().installed.load(std::memory_order_acquire) || !published) return;
    try {
        struct Member { std::uint64_t id; bool leader; std::array<std::byte, 7> padding; };
        static_assert(sizeof(Member) == 16);
        Address data{}; std::uint32_t count{};
        require(game::native_array<Member>(reinterpret_cast<const void *>(output), data, count, 64),
                "Native party member array exceeds bound.");
        // The members already present, read once; this loop appends the rest.
        std::array<Member, 64> members{};
        require(!count || memory::peek_bytes(data, members.data(), count * sizeof(Member)),
                "Native party memory unavailable.");
        for (const auto &record : published->records) {
            if (!record.handle || !record.player.member) continue;
            if (std::any_of(members.begin(), members.begin() + count,
                            [&](const Member &member) { return member.id == record.player.id; }))
                continue;
            require(count < 64, "Native party member limit reached.");
            // Same allocator/header and POD copy as the native members query. The array
            // belongs to the caller; no C++ allocation crosses this ABI.
            const auto capacity = read<std::uint32_t>(data - 8);
            require(count <= capacity, "Native party member capacity differs.");
            if (count == capacity) {
                const auto grown = count ? count * 2 : 1;
                const auto block = reinterpret_cast<Address (*)(Address, std::size_t, unsigned, Address)>(
                    state().base + party::array_allocate)(output, grown * sizeof(Member) + 8, 8, 0);
                require(block, "Cannot allocate native party members.");
                const auto next = block + 8;
                std::memcpy(reinterpret_cast<void *>(next), reinterpret_cast<const void *>(data), count * sizeof(Member));
                const auto empty = reinterpret_cast<Address (*)()>(state().base + party::empty_array)();
                if (data != empty) reinterpret_cast<void (*)(Address, Address, std::size_t)>(
                    state().base + party::array_free)(output, data - 8, count * sizeof(Member) + 8);
                *reinterpret_cast<Address *>(output) = next;
                *reinterpret_cast<std::uint32_t *>(block) = grown;
                data = next;
            }
            const Member member{record.player.id, record.player.leader, {}};
            std::memcpy(reinterpret_cast<void *>(data + count * sizeof(Member)), &member, sizeof(member));
            *reinterpret_cast<std::uint32_t *>(data - 4) = ++count;
            members[count - 1] = member;
        }
    } catch (const std::exception &e) { status(e.what()); }
}
struct Target { Record record; Position position; };
std::optional<Target> target(std::uint64_t id, bool native_id = false) {
    const auto published = state().published.load(std::memory_order_acquire);
    if (!published || !id) return {};
    const auto eligible = [&](const Record &record) {
        return record.handle && !record.player.local && record.player.slot < max_remote_players &&
               (native_id ? record.player_id == id : record.player.id == id);
    };
    // A native ID is its record's index (update_native_party); an EA ID is indexed.
    // The walk remains for the case the indexed record is not a remote player's.
    const Record *found{};
    if (native_id) {
        if (id >= 0x7e000000U && id - 0x7e000000U < published->records.size() &&
            eligible(published->records[id - 0x7e000000U]))
            found = &published->records[id - 0x7e000000U];
    } else if (const auto record = published->record(id); record && eligible(*record)) {
        found = record;
    } else if (record) {
        for (const auto &candidate : published->records)
            if (eligible(candidate)) { found = &candidate; break; }
    }
    if (!found) return {};
    std::lock_guard lock(state().position_mutex);
    return Target{*found, state().positions[found->player.slot]};
}
bool available(const Target &value) {
    return value.record.player.present && value.position.valid &&
           GetTickCount64() - value.position.sampled < 1000;
}
// A script function delegate: its value is the function's type object, whose first qword is the
// function's type record {u32 hash, u16 flags (kind in bits 5-9, 24 = function), ...}.
constexpr std::uint32_t on_party_member_focus = 0xae7e231f, hide_map_cursor_panel = 0xf1365116;
bool function_delegate(Address value, std::uint32_t hash, Address *record = nullptr) noexcept {
    Address rec{};
    std::uint32_t actual{};
    std::uint16_t flags{};
    if (!value || (value & 1) || !memory::read(value, rec) || !rec || !memory::read(rec, actual) || actual != hash ||
        !memory::read(rec + 4, flags) || ((flags >> 5) & 0x1f) != 24)
        return false;
    if (record) *record = rec;
    return true;
}
// Another function's record with the same shape as HideMapCursorPanel's: flags and size, the
// parameter layout (+0x28..+0x2f) and the generic thunk (+0x30). Both take one UInt64 context.
bool same_function_shape(Address record, Address reference) noexcept {
    std::array<std::uint8_t, 4> a{}, b{};
    std::array<std::uint8_t, 16> c{}, d{};
    return memory::read(record + 4, a) && memory::read(reference + 4, b) && a == b &&
           memory::read(record + 0x28, c) && memory::read(reference + 0x28, d) && c == d;
}
// OnPartyMemberFocus has no live POI to borrow it from: find its type object on the heap on a
// worker, never on the game thread and only in a world. Profiled 2026-10-01: a scan of a
// 12 GB game takes a minute or more of a whole core, and while it runs every ReadProcessMemory
// on the game thread slows down. Both scans therefore stay at background priority and read only
// pages already in the working set; a scan that read every committed page faulted gigabytes
// in (the game grew from 12 to 14.4 GB) at normal priority for several minutes.
void find_party_focus(State &s) {
    const auto now = GetTickCount64();
    if (s.map_on_focus.load(std::memory_order_acquire) || !s.map_on_unfocus_record || now < s.focus_scan_retry ||
        s.focus_scan_attempts >= 2 || s.focus_scan_running.exchange(true))
        return;
    s.focus_scan_retry = now + 15000;
    ++s.focus_scan_attempts;
    const auto reference = s.map_on_unfocus_record;
    const auto generation = s.focus_scan_generation.load();
    try {
        std::thread([&s, reference, generation] {
            NativeTypeQuery query{on_party_member_focus, 0, 24};
            find_native_types({&query, 1}, true);
            Address record{};
            const bool found = query.object && function_delegate(query.object, on_party_member_focus, &record) &&
                               same_function_shape(record, reference);
            if (found && s.focus_scan_generation.load() == generation) s.map_on_focus.store(query.object, std::memory_order_release);
            logging::write(found ? logging::Level::info : logging::Level::warning, logging::Channel::ui,
                           found ? "Party map markers: the player card command was found; markers open the game's card."
                                 : "Party map markers: the player card command was not found; markers stay without it.");
            s.focus_scan_running.store(false, std::memory_order_release);
        }).detach();
    } catch (...) { s.focus_scan_running.store(false, std::memory_order_release); }
}
void update_map_markers() {
    auto &s = state(); auto &n = game::native_data().models;
    const auto now = GetTickCount64();
    if (now < s.next_map_update) return;
    s.next_map_update = now + 100;
    for (auto &word : s.map_slots) word.store(0);
    const auto published = s.published.load(std::memory_order_acquire);
    const auto map = read<Address>(s.base + party::map_manager);
    if (!map || !published) { s.map_count = 0; return; }
    require(read<Address>(map) == s.base + party::map_manager_vtable, "Native party map manager type differs.");
    const auto manager = read<Address>(map + 0x18);
    if (manager != published->manager) { s.map_count = 0; return; }
    game::ModelWriteLock lock(manager);
    const auto list = read<Handle>(map + 0x20);
    if (map != s.map_manager || manager != s.map_models || list != s.map_list) {
        // Old map registrations belong to its destroyed manager. Never call
        // removal through an old pointer after native map/model replacement.
        s.map_manager = map; s.map_models = manager; s.map_list = list; s.markers = {}; s.map_prototype = 0;
        // The map screen gets a new manager each time it opens; the focus commands are the
        // level's and stay valid (checked before every use), so only OnUnFocus is found again.
        s.map_on_unfocus = s.map_on_unfocus_record = 0;
    }
    auto remove = [&](MapMarker &marker) {
        if (marker.registered && n.value(manager, marker.poi, 0, 0))
            reinterpret_cast<void (*)(Address, Handle)>(s.base + party::map_unregister)(map, marker.poi);
        marker = {};
    };
    if (!s.map_markers.load(std::memory_order_acquire)) {
        for (auto &marker : s.markers) remove(marker);
        s.map_count = 0;
        return;
    }
    // The game's own user marker supplies its authored PlayerDisplay widget,
    // including the asset reference and native sizing/selection behavior.
    // Copy typed models under the native lock; never memcpy a widget object.
    Handle prototype = s.map_prototype;
    if (prototype && (!n.value(manager, prototype, 0, 0) || model_type(manager, prototype) != s.base + party::map_poi_type))
        prototype = 0;
    const auto table = read<Address>(map + 0x48);
    const auto buckets = read<std::uint32_t>(map + 0x50), count = read<std::uint32_t>(map + 0x54);
    require(table && buckets <= 65536 && count <= 20000, "Native map POI registry exceeds bound.");
    // The game's own POIs also lend their OnUnFocus (HideMapCursorPanel): stops, fast-travel
    // points and friends all use it (MapObjectData.InteractHandler.Focusable.OnUnFocus @264).
    if (s.map_on_unfocus && !function_delegate(s.map_on_unfocus, hide_map_cursor_panel)) s.map_on_unfocus = s.map_on_unfocus_record = 0;
    // A found OnFocus that no longer checks out belonged to an unloaded level: look again.
    if (const auto stale = s.map_on_focus.load(std::memory_order_acquire);
        stale && !function_delegate(stale, on_party_member_focus)) {
        s.map_on_focus.store(0, std::memory_order_release);
        ++s.focus_scan_generation;
        s.focus_scan_attempts = 0;
    }
    // Walking the registry is not free while the map is open: for OnUnFocus, every 2 s at most.
    const auto search_now = GetTickCount64();
    const bool search_unfocus = !s.map_on_unfocus && search_now >= s.unfocus_search_at;
    if (search_unfocus) s.unfocus_search_at = search_now + 2000;
    unsigned visited{};
    for (unsigned i = 0; i < buckets && (!prototype || (search_unfocus && !s.map_on_unfocus)); ++i) {
        auto node = read<Address>(table + i * 8);
        while (node && visited++ < count) {
            const auto handle = read<Handle>(node);
            if (model_type(manager, handle) == s.base + party::map_poi_type) {
                const auto value = n.value(manager, handle, 0, 0);
                const auto kind = value ? read<std::uint32_t>(value + 404) : 0U;
                if (value && kind == 1 && !prototype) prototype = handle;
                if (value && search_unfocus && (kind == 5 || kind == 13 || kind == 16) && !s.map_on_unfocus) {
                    const auto unfocus = read<Address>(value + 264);
                    Address record{};
                    if (function_delegate(unfocus, hide_map_cursor_panel, &record)) {
                        s.map_on_unfocus = unfocus;
                        s.map_on_unfocus_record = record;
                    }
                }
                if (prototype && (!search_unfocus || s.map_on_unfocus)) break;
            }
            node = read<Address>(node + 8);
        }
    }
    s.map_prototype = prototype;
    // The player card only matters on a party member's marker: no scan in solo play.
    bool party_member{};
    for (std::size_t slot = 0; slot < s.markers.size() && !party_member; ++slot)
        if (const auto record = published->slot_record(slot); record && record->player.member) party_member = true;
    if (party_member) find_party_focus(s);
    // The live game's focus commands, if both are known and still intact.
    const auto on_focus = s.map_on_focus.load(std::memory_order_acquire);
    const bool card = on_focus && s.map_on_unfocus && function_delegate(on_focus, on_party_member_focus) &&
                      function_delegate(s.map_on_unfocus, hide_map_cursor_panel);
    unsigned active{};
    std::array<std::uint64_t, (max_remote_players + 63) / 64> slots{};
    for (std::size_t slot = 0; slot < s.markers.size(); ++slot) {
        auto &marker = s.markers[slot];
        const auto record = published->slot_record(slot);
        const auto value = record ? target(record->player.id) : std::optional<Target>{};
        // Party markers are for the local player's party; everyone
        // else shows as the game's plain player dot (native_player_ui.cpp).
        if (!value || !available(*value) || !record->player.member) { remove(marker); continue; }
        if (marker.id != record->player.id || marker.epoch != record->player.epoch ||
            (marker.poi && !n.value(manager, marker.poi, 0, 0)) || (marker.poi && card && !marker.card))
            remove(marker); // rebuilt below, with the player card once its commands are known
        if (!marker.poi) {
            if (!prototype) continue;
            struct BuildGuard { MapMarker &marker; bool done{}; ~BuildGuard() { if (!done) marker = {}; } } guard{marker};
            const auto prototype_value = n.value(manager, prototype, 0, 0);
            const auto widget_source = read<Handle>(prototype_value + 344);
            const auto widget_type = model_type(manager, widget_source);
            require(widget_type && read<std::uint32_t>(read<Address>(widget_type)) == 0xf88f43c6 &&
                    read<std::uint16_t>(read<Address>(widget_type) + 6) == 240,
                    "Native map player widget schema differs.");
            const auto display_source = read<Handle>(n.value(manager, widget_source, 0, 0) + 16);
            const auto display_type = model_type(manager, display_source);
            require(display_type && read<std::uint32_t>(read<Address>(display_type)) == 0xe65fdda8 &&
                    read<std::uint16_t>(read<Address>(display_type) + 6) == 160,
                    "Native map player display schema differs.");
            const auto ns = game::native_name_hash("ReSkate.PartyMap") ^ static_cast<std::uint32_t>(record->player.epoch) ^
                            static_cast<std::uint32_t>(record->player.epoch >> 32);
            auto create = [&](Address type) {
                const auto h = n.create(manager, type, record->player.id, ns, false, 2);
                require(h, "Cannot create native party map model."); return h;
            };
            marker = {record->player.id, record->player.epoch, create(s.base + party::map_poi_type),
                      create(widget_type), create(display_type), create(s.base + party::map_data_type), false};
            // Re-read borrowed sources after creation, which can move model storage.
            n.publish(manager, marker.display, display_type, reinterpret_cast<const void *>(n.value(manager, display_source, 0, 0)));
            n.publish(manager, marker.widget, widget_type, reinterpret_cast<const void *>(n.value(manager, widget_source, 0, 0)));
            publish_value(manager, field(manager, marker.display, 1, 0x1f7d0f7d, 8), s.base + engine::bool_type, false);
            publish_value(manager, field(manager, marker.display, 3, 0x81d5aa57, 32), s.base + engine::uint64_type, record->handle);
            const auto content = field(manager, marker.widget, 0, 0x85321504, 0);
            const auto normal = field(manager, content, 0, 0x716496c8, 0);
            // Widget reference = native asset + ModelRef. Validate the context
            // field before redirecting only this owned widget's view model.
            const auto reference_type = model_type(manager, normal);
            const auto meta = read<Address>(reference_type);
            require(read<std::uint16_t>(meta + 6) == 24 && read<std::uint16_t>(meta + 0x2a) <= 8,
                    "Native map widget reference size differs.");
            const auto fields = read<Address>(meta + 0x60);
            Handle context{};
            for (unsigned i = 0; i < read<std::uint16_t>(meta + 0x2a); ++i) {
                const auto f = fields + i * 24;
                if (read<std::uint16_t>(f + 8) == 8 && read<Address>(f + 16) == s.base + engine::reference_type)
                    context = n.field(manager, normal, i, UINT32_MAX, false);
            }
            struct Ref { Address value{}; Handle handle{}; };
            publish_value(manager, context, s.base + engine::reference_type, Ref{0, marker.display});
            alignas(16) std::array<std::byte, 416> poi{};
            reinterpret_cast<void (*)(void *)>(s.base + party::poi_construct)(poi.data());
            // Default construction gives safe empty delegates and references. With the live
            // game's focus commands (InteractHandler.Focusable: OnUnFocus @264, OnFocus @272),
            // focusing the marker shows the player card with Fast Travel and View Profile. They are
            // untagged (borrowed) delegates: publish copies them, and they are cleared again before
            // the local value's destructor.
            if (card) {
                const auto unfocus = s.map_on_unfocus;
                std::memcpy(poi.data() + 264, &unfocus, sizeof unfocus);
                std::memcpy(poi.data() + 272, &on_focus, sizeof on_focus);
            }
            n.publish(manager, marker.poi, s.base + party::map_poi_type, poi.data());
            std::memset(poi.data() + 264, 0, 16);
            reinterpret_cast<void (*)(void *)>(s.base + party::poi_destroy)(poi.data());
            marker.card = card;
            publish_value(manager, field(manager, marker.poi, 0, 0xc1d9bffe, 404), s.base + party::map_poi_kind_type, std::uint32_t{2});
            // The map's list tells its items apart by MapObjectData.ElementKey (the list's identity
            // selector, CoreMap_ItemIdentitySelector); the game gives every POI a unique one
            // (native NextUniqueId, always >= 0x7fffffff). A default-constructed POI has 0, and
            // markers sharing it shared one list widget: the list's focus drifted off its data and,
            // as party markers sort ahead of every fast-travel kind, every stop shifted (the wrong
            // stop travelled to, the last ones not selectable). Keys from a private range below the
            // game's, set before registration and kept until unregistered.
            static std::uint32_t next_key = 0x52530000;
            if (++next_key >= 0x7fffffffU) next_key = 0x52530001;
            marker.key = next_key;
            publish_value(manager, field(manager, marker.poi, 8, 0x0fc37633, 400), s.base + engine::uint32_type, marker.key);
            publish_value(manager, field(manager, marker.poi, 5, 0x9df3c20e, 352), s.base + engine::uint64_type, marker.data);
            publish_value(manager, field(manager, marker.poi, 6, 0x5e7ee0fd, 336), s.base + engine::reference_type, Ref{0, marker.widget});
            publish_value(manager, field(manager, marker.poi, 4, 0x19ff199a, 392), s.base + party::vec2_type, std::array<float, 2>{128, 128});
            publish_value(manager, field(manager, marker.poi, 11, 0x86c837b9, 412), s.base + engine::bool_type, true);
            guard.done = true;
        }
        // Position and heading only when they moved: each publish notifies the map.
        if (!marker.placed || *marker.placed != value->position.root) {
            const auto world = to_matrix(value->position.root);
            publish_value(manager, field(manager, marker.poi, 9, 0x24939145, 0), s.base + party::matrix_type, world);
            const float rotation = std::atan2(world[2], world[0]) + 3.141592654f;
            publish_value(manager, field(manager, marker.poi, 10, 0x421c13b7, 408), s.base + engine::float_type, rotation);
            publish_value(manager, field(manager, marker.display, 4, 0x549cb2ed, 40), s.base + engine::float_type, -rotation);
            marker.placed = value->position.root;
        }
        if (marker.title != record->player.name) {
            const NativeText name(record->player.name);
            publish_value(manager, field(manager, marker.poi, 1, 0x44688629, 368), s.base + engine::string_type, name.value);
            marker.title = record->player.name;
        }
        if (!marker.registered) {
            reinterpret_cast<void (*)(Address, Handle)>(s.base + party::map_register)(map, marker.poi);
            marker.registered = true;
        }
        ++active;
        slots[slot / 64] |= std::uint64_t{1} << (slot % 64);
    }
    s.map_count = active;
    for (std::size_t i = 0; i < slots.size(); ++i) s.map_slots[i].store(slots[i]);
}
struct VmScope {
    std::uint32_t hash{};
    std::array<std::pair<Address, std::uint64_t>, 8> weak_arguments{};
    std::uint64_t selected{};
};
thread_local VmScope *current_vm{};
// The native Spectate command resolves the player's weak reference, copies it
// and immediately passes the copy on, often outside any action script. Our
// players' references stay null, so the copy cannot be matched by address or
// contents. Remember the last ReSkate player resolved on this thread instead.
struct RecentReference { std::uint64_t id{}, at{}; };
thread_local RecentReference recent_reference{};
constexpr std::uint64_t recent_reference_ms = 250;
void remember_reference(Address output, std::uint64_t id) {
    if (target(id) && !read<Address>(output)) recent_reference = {id, GetTickCount64()};
}
bool action_expression(std::uint32_t hash) {
    switch (hash) {
    case 0x57a3de89: case 0xe86b5cc2: // Native Spectate/Teleport button predicates.
    case 0xe04bde5d: case 0x75c4c61e: // Inspect->Spectate and focused-player selection.
    case 0xf9e3a04d: case 0x6dc1bace: // Teleport confirmation and next player.
    case 0xcf0a17f2: case 0x1b2dd9f9: // Party map focus and spectated-player details.
    case 0xee062494:                  // IsUserInServer (the Join and Accept flows).
    case 0xa7a7fbed:                  // InviteToPartyButton_Predicate (diagnosis only).
        return true;
    default: return false;
    }
}
void bind_weak_argument(Address output, std::uint64_t id) {
    // Keep the native null weak reference intact. The UI's position and ID
    // queries are adapted for this exact argument within its expression call;
    // no fabricated ClientPlayer or ref-counted engine object is ever supplied.
    if (!current_vm || !target(id) || read<Address>(output)) return;
    current_vm->selected = id;
    for (auto &entry : current_vm->weak_arguments) {
        if (!entry.first || entry.first == output) { entry = {output, id}; return; }
    }
}
std::optional<Target> weak_target(Address argument) {
    if (!current_vm) return {};
    for (const auto &[address, id] : current_vm->weak_arguments)
        if (address == argument) return target(id);
    return {};
}
Address weak_player_hook(Address output, std::uint64_t id) {
    const auto result = state().weak_player(output, id);
    try { remember_reference(output, id); bind_weak_argument(output, id); } catch (...) {}
    return result;
}
Address weak_entity_hook(Address output, const std::uint64_t *id) {
    const auto result = state().weak_entity(output, id);
    try { if (id) { remember_reference(output, *id); bind_weak_argument(output, *id); } } catch (...) {}
    return result;
}
bool world_transform_hook(Address argument, void *output) {
    if (const auto value = weak_target(argument)) {
        if (!available(*value) || !output) return false;
        const auto world = to_matrix(value->position.root);
        std::memcpy(output, world.data(), sizeof(world));
        return true;
    }
    return state().world_transform(argument, output);
}
std::uint64_t player_id_hook(Address argument) {
    if (const auto value = weak_target(argument)) return value->record.player_id;
    return state().player_id(argument);
}
bool can_spectate_hook(std::uint32_t id) {
    if (const auto value = target(id, true)) return available(*value);
    return state().can_spectate(id);
}
bool can_teleport_hook(std::uint32_t id) {
    if (const auto value = target(id, true)) return available(*value);
    return state().can_teleport(id);
}
void select_spectator(const Target &value) {
    if (!available(value)) return;
    state().spectating_epoch.store(value.record.player.epoch, std::memory_order_relaxed);
    state().spectating.store(value.record.player.id, std::memory_order_release);
}
void spectate_hook(Address argument, bool immediate) {
    if (GetTickCount64() < state().spectate_blocked_until.load(std::memory_order_acquire)) return;
    auto value = weak_target(argument);
    // A null reference right after resolving one of our players is that player.
    // A live native reference is never redirected.
    if (!value && argument && !read<Address>(argument) && recent_reference.id &&
        GetTickCount64() - recent_reference.at <= recent_reference_ms)
        value = target(recent_reference.id);
    if (value) { select_spectator(*value); return; }
    state().spectate(argument, immediate);
}
bool spectate_active_hook() {
    if (state().spectating.load(std::memory_order_acquire)) return true;
    return state().spectate_active();
}
void spectate_clear_hook(Address manager) {
    state().spectating.store(0, std::memory_order_release);
    state().spectate_clear(manager);
}
Address spectate_name_hook(Address output) {
    if (const auto value = target(state().spectating.load(std::memory_order_acquire))) {
        game::native_data().values.assign(reinterpret_cast<void *>(output), value->record.player.name.c_str(),
                                         static_cast<std::uint32_t>(value->record.player.name.size()));
        return output;
    }
    return state().spectate_name(output);
}
std::uint32_t teleport_hook(Address reason, const void *matrix, Address destination,
    std::uint64_t id, std::uint8_t mode, Address stance, Address transition) {
    if (const auto value = target(id)) {
        if (!available(*value)) return UINT32_MAX;
        // Same native teleport path as live players, including ground checks,
        // streaming, board state and physics reset. This local pose has already
        // been admitted by our lobby; bypass only the absent EA ClientPlayer
        // lookup. ID zero is the function's native world-position overload.
        alignas(16) auto world = to_matrix(value->position.root);
        world[12] += world[0] * 2.0f; world[13] += 1.0f; world[14] += world[2] * 2.0f;
        const auto result = state().teleport(reason, world.data(), destination, 0, mode, stance, transition);
        // Teleporting from Spectate closes the native view without its clear
        // call. End ours too; the next tick restores the original camera.
        if (result != UINT32_MAX) {
            recent_reference = {};
            state().spectate_blocked_until.store(GetTickCount64() + 1000, std::memory_order_release);
            state().spectating.store(0, std::memory_order_release);
        }
        return result;
    }
    // RequestTeleport's last-but-one argument is three option bytes: on board, stance and
    // lip mount. While the local player waits for their S.K.A.T.E. turn, the turn start's
    // teleport of the local skater (id 0) lands it on foot. Kept in a static: the native
    // copies the options into its components, but must never see a dead buffer.
    if (!id && stance && throwdown_relay_waits_offboard()) {
        static thread_local std::array<std::uint8_t, 3> on_foot{};
        if (memory::peek_bytes(stance, on_foot.data(), on_foot.size()) && on_foot[0]) {
            on_foot[0] = 0;
            return state().teleport(reason, matrix, destination, id, mode, reinterpret_cast<Address>(on_foot.data()),
                                    transition);
        }
    }
    return state().teleport(reason, matrix, destination, id, mode, stance, transition);
}
void adapt_predicate(Address vm, Address resource, std::uint32_t hash) {
    if (hash != 0x57a3de89 && hash != 0xe86b5cc2) return;
    const bool spectate = hash == 0x57a3de89;
    const auto header = read<std::uint32_t>(resource + 0x20);
    require(header == (spectate ? 64U : 80U) &&
            read<std::uint32_t>(resource + 0x24) == (spectate ? 384U : 496U),
            "Native social action expression layout differs.");
    const auto registers = read<Address>(vm + 0x30) + ((header + 15U) & ~15U);
    const auto constants = read<Address>(registers);
    const auto arguments = read<Address>(registers + 8);
    const auto locals = read<Address>(registers + 16);
    const auto value = target(read<std::uint64_t>(locals + 8));
    if (!value) return;
    // Verified opcode 0x32 stores through the output pointer in parameter 1.
    // Use the authored EnableAllSocialActions branch's result, without changing
    // that global setting or enabling any action for unrelated players.
    const auto constant = constants + (spectate ? 0x10c : 0x164);
    const auto result = read<std::uint8_t>(constant + (available(*value) ? 0 : 1));
    const auto output = read<Address>(arguments + (spectate ? 0x10 : 0x18));
    require(result <= 1 && output, "Native social action result binding differs.");
    SIZE_T written{};
    require(WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(output), &result, 1, &written) && written == 1,
            "Cannot publish native social action availability.");
}
// friends/expressions/IsUserInServer (0xee062494): IsValid(weak_player(EAId)), false for every
// ReSkate player (no native player). The Social menu's Join and an invite's Accept then take the
// "another server" path (modals, presence lookups) instead of joining: a session player is on
// this server. Port 0 is the EAId, port 8 the bool result (analysis/party-re/ui-actions.md 4.3).
void adapt_in_server(Address vm, Address resource) {
    require(read<std::uint32_t>(resource + 0x20) == 0x40 && read<std::uint32_t>(resource + 0x24) == 8,
            "IsUserInServer expression layout differs.");
    const auto registers = read<Address>(vm + 0x30) + 0x40;
    const auto arguments = read<Address>(registers + 8);
    const auto input = read<Address>(arguments), output = read<Address>(arguments + 8);
    const auto published = state().published.load(std::memory_order_acquire);
    if (!published || !input || !output) return;
    std::uint64_t id{};
    if (!memory::peek(input, id)) return;
    const auto record = published->record(id);
    if (!record || record->player.local) return;
    const std::uint8_t yes = 1;
    SIZE_T written{};
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(output), &yes, 1, &written);
}
// InviteToPartyButton_Predicate (0xa7a7fbed): why the player card's Invite is disabled, in the
// graph's own terms (its debug line "Full {0} Permission {1} Cooldown {2} Enabled {3} Available
// {4} Unlocked {5}", whose LogInfo is a retail stub). Logged for session players when it changes.
void diagnose_invite(Address vm, Address resource) {
    if (read<std::uint32_t>(resource + 0x20) != 0x40 || read<std::uint32_t>(resource + 0x24) != 0x438) return;
    const auto registers = read<Address>(vm + 0x30) + 0x40;
    const auto locals = read<Address>(registers + 16);
    if (!locals) return;
    const auto target = read<std::uint64_t>(locals + 0x130);
    const auto published = state().published.load(std::memory_order_acquire);
    const auto record = published ? published->record(target) : nullptr;
    if (!record || record->player.local) return;
    // Negated where the graph ORs the negation: each is true when it disables the button.
    const std::array<std::uint8_t, 6> reasons{read<std::uint8_t>(locals + 0x20e), read<std::uint8_t>(locals + 0x211),
        read<std::uint8_t>(locals + 0x214), read<std::uint8_t>(locals + 0x23b), read<std::uint8_t>(locals + 0x23e),
        read<std::uint8_t>(locals + 0x240)};
    static std::mutex mutex;
    static std::map<std::uint64_t, std::array<std::uint8_t, 6>> seen;
    std::lock_guard lock(mutex);
    auto &last = seen[target];
    if (last == reasons) return;
    last = reasons;
    status("Party invite button for " + record->player.name + ": full " + std::to_string(reasons[0]) +
           ", permission " + std::to_string(reasons[1]) + ", cooldown " + std::to_string(reasons[2]) +
           ", parties/gate " + std::to_string(reasons[3]) + ", availability " + std::to_string(reasons[4]) +
           ", unlocked " + std::to_string(reasons[5]) + " (1 = that disables it).");
    // A cooldown the local player never started (no invite from here for a minute): the card's
    // CountdownDataModel (keyed by this record) is cleared on the next tick, which re-evaluates
    // the button. Not here: publishing inside the predicate would re-enter it.
    if (reasons[2]) {
        const auto sent = last_native_invite(target);
        const auto now = GetTickCount64();
        if (!sent || now - sent > 60000) {
            std::lock_guard cooldowns(state().cooldown_mutex);
            if (state().stale_cooldowns.size() < 64) state().stale_cooldowns.push_back(record->handle);
        }
    }
}
// CountdownDataModel (TypeNameHash 0x63a14e6c): {TargetTime i64 (0x1acddb6a), IsActive bool (0x369babfe)}.
void clear_stale_cooldowns() {
    auto &s = state();
    std::vector<Handle> handles;
    {
        std::lock_guard lock(s.cooldown_mutex);
        handles.swap(s.stale_cooldowns);
    }
    if (handles.empty()) return;
    const auto published = s.published.load(std::memory_order_acquire);
    if (!published || !published->manager) return;
    const auto manager = published->manager;
    auto &n = game::native_data().models;
    game::ModelWriteLock lock(manager);
    const auto type = registered_model_type(manager, 0x63a14e6c);
    if (!type) { status("Party invite cooldown: the countdown model type is not registered."); return; }
    const auto meta = read<Address>(type);
    const auto count = read<std::uint16_t>(meta + 0x2a);
    const auto fields = read<Address>(meta + 0x60);
    for (const auto handle : handles) {
        const auto countdown = s.find_model(manager, type, handle, 0, 0);
        if (!countdown || !n.value(manager, countdown, 0, 0)) {
            status("Party invite cooldown: no countdown model for that player (the button reads a stale value).");
            continue;
        }
        for (unsigned i = 0; i < count && fields; ++i) {
            const auto entry = fields + i * 24;
            if (read<std::uint32_t>(entry) != 0x369babfe) continue;
            const auto child = n.field(manager, countdown, i, UINT32_MAX, false);
            const auto child_type = read<Address>(entry + 16);
            if (!child || model_type(manager, child) != child_type) break;
            const bool inactive = false;
            n.publish(manager, child, child_type, &inactive);
            status("Party invite cooldown cleared for a session player (nobody had invited them).");
            break;
        }
    }
}
// ActivitiesChallengeStartConfigDataModel (TypeNameHash 0x8317d7c9): {IsCoop bool (0xc93bc955),
// InQueue, ChallengeEndedByHost}. The Solo/Coop buttons write IsCoop, and the objective list
// (ActivityCriteriaListHUDRule.PostPush) and the finish screen (ActivityRewardMenuRule_Challenge
// .FillContent) leave the coop objective out unless it is set when they build their rows. A copy
// the SDK started pressed no Coop button here, so IsCoop still says solo and the row never comes
// (analysis/coop-challenges-re/coop_criteria_hud.md). It is set just before either graph runs.
constexpr std::uint32_t graph_objective_list = 0xeae864ca, graph_challenge_rewards = 0x1e01dd61;
void show_coop_objectives() {
    auto &s = state();
    const auto published = s.published.load(std::memory_order_acquire);
    if (!published || !published->manager) return;
    const auto manager = published->manager;
    auto &n = game::native_data().models;
    game::ModelWriteLock lock(manager);
    const auto type = registered_model_type(manager, 0x8317d7c9);
    if (!type) { status("Coop challenge objectives: the challenge start model is not registered."); return; }
    const auto model = reinterpret_cast<Handle (*)(Address, Address, std::uint8_t, std::uint8_t)>(s.base + party::model_singleton)(
        manager, type, 0, 1);
    const auto meta = read<Address>(type);
    const auto count = read<std::uint16_t>(meta + 0x2a);
    const auto fields = read<Address>(meta + 0x60);
    for (unsigned i = 0; model && i < count && fields; ++i) {
        const auto entry = fields + i * 24;
        if (read<std::uint32_t>(entry) != 0xc93bc955) continue;
        const auto child = n.field(manager, model, i, UINT32_MAX, false);
        const auto child_type = read<Address>(entry + 16);
        if (!child || model_type(manager, child) != child_type) break;
        const auto value = n.value(manager, child, 0, 0);
        if (value && read<std::uint8_t>(value)) return; // already coop (the Coop button)
        const bool coop = true;
        n.publish(manager, child, child_type, &coop);
        logging::write(logging::Level::info, logging::Channel::ui,
                       "Coop challenge objectives: this copy was started without the Coop button; marked coop so the "
                       "all-players objective shows.");
        return;
    }
    status("Coop challenge objectives: the challenge start model has no IsCoop field.");
}
void run_expression(Address vm, std::uint32_t pc, Address profiler, void (*runner)(Address, std::uint32_t, Address)) {
    if (!state().installed.load(std::memory_order_acquire)) { runner(vm, pc, profiler); return; }
    // These are the exact two reads performed by the native VM entry point.
    // The common path avoids RPM, schema scans and locks for unrelated scripts.
    const auto resource = *reinterpret_cast<const Address *>(vm + 0x38);
    const auto hash = *reinterpret_cast<const std::uint32_t *>(resource + 0x10);
    if ((hash == graph_objective_list || hash == graph_challenge_rewards) &&
        state().challenge_coop.load(std::memory_order_acquire)) {
        try { show_coop_objectives(); } catch (const std::exception &e) { status(e.what()); }
    }
    if (!state().roster_active.load(std::memory_order_acquire)) { runner(vm, pc, profiler); return; }
    if (!action_expression(hash)) { runner(vm, pc, profiler); return; }
    VmScope scope; scope.hash = hash;
    auto *previous = current_vm; current_vm = &scope;
    struct Restore { VmScope *previous; ~Restore() { current_vm = previous; } } restore{previous};
    runner(vm, pc, profiler);
    try {
        if (hash == 0xee062494) { adapt_in_server(vm, resource); return; }
        if (hash == 0xa7a7fbed) { diagnose_invite(vm, resource); return; }
        adapt_predicate(vm, resource, hash);
        // Inspect's native command can stop before the absent replicated-camera
        // request. It still resolves the exact selected player's EAId operand.
        if (hash == 0xe04bde5d && scope.selected)
            if (const auto value = target(scope.selected)) select_spectator(*value);
    } catch (const std::exception &e) { status(e.what()); }
}
} // namespace
namespace native_party_detail {
void install(Address base) {
    auto &s = state(); s.attempted = true; s.base = base;
    for (const auto &contract : party::native_party_contracts) {
        std::array<unsigned char, 32> bytes{};
        require(memory::read(base + contract.rva, bytes) && bytes == contract.bytes,
                "Native party function fingerprint differs; adapters were not installed.");
    }
    const auto &find_contract = addr::profile::model_find_contract;
    std::array<unsigned char, 32> find_bytes{};
    require(memory::read(base + find_contract.rva, find_bytes) && find_bytes == find_contract.bytes,
            "Native social model lookup fingerprint differs.");
    require(game::native_data().models.publish && game::native_data().values.assign,
            "Native party model API unavailable.");
    struct Hook { Address rva; void *replacement; void **original; };
    const std::array hooks{
        Hook{engine::model_find, reinterpret_cast<void *>(&find_model_hook), reinterpret_cast<void **>(&s.find_model)},
        Hook{party::get_info, reinterpret_cast<void *>(&get_info_hook), reinterpret_cast<void **>(&s.get_info)},
        Hook{party::is_member, reinterpret_cast<void *>(&is_member_hook), reinterpret_cast<void **>(&s.is_member)},
        Hook{party::member_ids, reinterpret_cast<void *>(&member_ids_hook), reinterpret_cast<void **>(&s.member_ids)},
        Hook{party::members_with_leader, reinterpret_cast<void *>(&members_with_leader_hook), reinterpret_cast<void **>(&s.members_with_leader)},
        Hook{party::weak_player, reinterpret_cast<void *>(&weak_player_hook), reinterpret_cast<void **>(&s.weak_player)},
        Hook{party::weak_entity, reinterpret_cast<void *>(&weak_entity_hook), reinterpret_cast<void **>(&s.weak_entity)},
        Hook{party::world_transform, reinterpret_cast<void *>(&world_transform_hook), reinterpret_cast<void **>(&s.world_transform)},
        Hook{party::player_id, reinterpret_cast<void *>(&player_id_hook), reinterpret_cast<void **>(&s.player_id)},
        Hook{party::can_spectate, reinterpret_cast<void *>(&can_spectate_hook), reinterpret_cast<void **>(&s.can_spectate)},
        Hook{party::can_teleport, reinterpret_cast<void *>(&can_teleport_hook), reinterpret_cast<void **>(&s.can_teleport)},
        Hook{party::spectate, reinterpret_cast<void *>(&spectate_hook), reinterpret_cast<void **>(&s.spectate)},
        Hook{party::spectate_active, reinterpret_cast<void *>(&spectate_active_hook), reinterpret_cast<void **>(&s.spectate_active)},
        Hook{party::spectate_clear, reinterpret_cast<void *>(&spectate_clear_hook), reinterpret_cast<void **>(&s.spectate_clear)},
        Hook{party::spectate_name, reinterpret_cast<void *>(&spectate_name_hook), reinterpret_cast<void **>(&s.spectate_name)},
        Hook{party::teleport, reinterpret_cast<void *>(&teleport_hook), reinterpret_cast<void **>(&s.teleport)}};
    // All adapters are gated until every trampoline has been enabled.
    std::size_t prepared{};
    for (const auto &hook : hooks) {
        if (hook_prepare(reinterpret_cast<void *>(base + hook.rva), hook.replacement, hook.original) != HookOk) {
            while (prepared) hook_remove(reinterpret_cast<void *>(base + hooks[--prepared].rva));
            throw std::runtime_error("Cannot prepare native party hooks.");
        }
        ++prepared;
    }
    // One transaction (one suspension of every game thread) for all sixteen.
    for (const auto &hook : hooks)
        require(hook_queue_enable(reinterpret_cast<void *>(base + hook.rva)) == HookOk,
                "Cannot enable native party hooks; restart ReSkate.");
    require(hook_apply_queued() == HookOk, "Cannot enable native party hooks; restart ReSkate.");
    s.installed.store(true, std::memory_order_release);
    try { install_requests(base); } catch (const std::exception &e) { status(e.what()); }
}
} // namespace native_party_detail
void set_native_challenge_coop(bool coop) noexcept { state().challenge_coop.store(coop, std::memory_order_release); }
void execute_native_party_expression(std::uintptr_t vm, std::uint32_t pc, std::uintptr_t profiler,
                                    void (*runner)(std::uintptr_t, std::uint32_t, std::uintptr_t)) {
    run_expression(vm, pc, profiler, runner);
}
bool native_party_spectating() noexcept { return state().spectating.load(std::memory_order_acquire) != 0; }
void spectate_party_member(std::uint64_t id) noexcept {
    try {
        auto &s = state();
        static std::uint64_t started{};
        if (!id) {
            if (started && s.spectating.load(std::memory_order_acquire) == started)
                s.spectating.store(0, std::memory_order_release);
            started = 0;
            return;
        }
        if (s.spectating.load(std::memory_order_acquire) == id) return;
        if (const auto value = target(id); value && available(*value)) {
            select_spectator(*value);
            started = id;
        }
    } catch (...) {}
}
void tick_native_party_actions(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase) noexcept {
    auto &s = state();
    if (!s.installed.load(std::memory_order_acquire)) return;
    try { update_map_markers(); } catch (const std::exception &e) { status(e.what()); }
    try { clear_stale_cooldowns(); } catch (const std::exception &e) { status(e.what()); }
    try {
        auto id = s.spectating.load(std::memory_order_acquire);
        const auto selected = target(id);
        if (id && (!ready || !selected || !available(*selected) ||
            selected->record.player.epoch != s.spectating_epoch.load(std::memory_order_relaxed))) {
            // Say why, so a cancelled Spectate never looks like an ignored click.
            const auto reason = !ready ? "Spectate cancelled: your skater is not in control (loading or menus)."
                : !selected || selected->record.player.epoch != s.spectating_epoch.load(std::memory_order_relaxed)
                    ? "Spectate cancelled: that player left or rejoined."
                    : "Spectate cancelled: no recent position for that player.";
            if (s.action_status != reason) {
                s.action_status = reason;
                logging::write(logging::Level::info, logging::Channel::ui, reason);
            }
            id = 0; s.spectating.store(0, std::memory_order_release);
        }
        auto &n = game::native_data().models;
        const auto published = s.published.load(std::memory_order_acquire);
        const auto sync_at = GetTickCount64();
        if ((id || s.camera_owned) && published && (id != s.synced_spectate || sync_at >= s.next_spectate_sync)) {
            s.synced_spectate = id;
            s.next_spectate_sync = sync_at + 100;
            const auto ui = read<Address>(base + engine::ui_manager);
            if (ui && read<Address>(ui + 0x140) == published->manager) {
                game::ModelWriteLock lock(published->manager);
                const auto model = singleton(published->manager, 0x24b7d6db, 184);
                if (model) {
                    const auto active = field(published->manager, model, 1, 0xd126a5a6, 8);
                    const auto value = n.value(published->manager, active, 0, 0);
                    const bool shown = value && read<bool>(value);
                    if (s.camera_owned && value && !shown) {
                        id = 0; s.spectating.store(0, std::memory_order_release);
                    }
                    // Published only when the flag differs.
                    if (!value || shown != (id != 0))
                        publish_value(published->manager, active, base + engine::bool_type, id != 0);
                }
            }
        }
        if (!id && !s.camera_owned) return;
        std::optional<std::array<float, 16>> camera;
        float fov{};
        // Framed the way the game's own camera frames the local skater (follow_camera.h).
        if (id && selected) camera = follow_camera(id, selected->position.root, fov);
        std::string detail;
        const bool applied = dingosdk::update_party_camera(base, client, ready, camera_phase, camera ? &*camera : nullptr, detail, fov);
        if (!applied && detail.empty()) detail = "Spectate camera helper is busy or not initialized.";
        if (applied) {
            s.camera_owned = id != 0;
            s.camera_started = 0;
        } else if (id) {
            // Activation failures must also request restoration: the native
            // mode switch can succeed even if its subsequent guard fails.
            s.camera_owned = true;
            if (!s.camera_started) s.camera_started = GetTickCount64();
            if (GetTickCount64() - s.camera_started > 5000) s.spectating.store(0, std::memory_order_release);
        }
        if (!detail.empty() && detail != s.action_status) {
            s.action_status = detail;
            logging::write(logging::Level::info, logging::Channel::ui, detail);
        }
    } catch (const std::exception &e) {
        s.spectating.store(0, std::memory_order_release);
        status(e.what());
    }
}
} // namespace dingosdk::multiplayer
