#include "native_party_internal.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "follow_camera.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Objects/object_prompt.h"
#include "Engine/Game/Build/20260929/profile.h"
#include <format>
#include <map>
#include <set>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>
#include <type_traits>

namespace dingosdk::multiplayer {
using namespace native_party_detail;
namespace {
// Above zero on the thread that is running the placed-object prompt's graph (object_prompt.h).
thread_local int object_prompt_running{};
// That run's VM and graph resource: the object the card is for is one of its locals.
thread_local Address object_prompt_vm{}, object_prompt_resource{};
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
        if (const auto handle = find(p->friend_record(id))) return handle;
        return find(p->server_record(id));
    } catch (...) { return 0; }
}
std::uint64_t get_info_hook(Address ui, std::uint64_t id) {
    try {
    const auto published = state().published.load(std::memory_order_acquire);
    if (state().installed.load(std::memory_order_acquire) && published && id) {
        auto record = published->record(id);
        if (!record) record = published->friend_record(id);
        if (const auto found = record ? record : published->server_record(id)) {
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
// Other players' QuickDrop fast travel flags (Extension/Objects/network_object_runtime.h). Their flag
// objects are made here like any placed object, but the flag's map component registers a map point
// only for the local player's own flags, so each one gets a PersonalFastTravel POI (kind 16) of
// ReSkate's own, read off the game's own in a running game on 2026-11-03:
// - widget: MapObjectViewModel (0xf88f43c6, 240 bytes) on DecoratedIcon_Widget, its view model at
//   +16 a DecoratedIcon model (0xf12b24a5, 208 bytes: the icon texture img_Icon_Shape_FastTravel_256
//   at +0, references to a frame model (0x19770b6a, 208) at +24 and a tint model (0x26d4c4e0, 288)
//   at +40). A flag's frame is img_Frame_Shape_Location_256 filled with Fillstyle_PremiumGradient
//   (the gold) and its tints are plain white; a bus stop (kind 5) has the same widget, icon and
//   texture with its own frame and the location colours.
// - details: PersonalFastTravel data (0x7b558c03, 80 bytes): a 3x4 rotation, the flag's place at
//   +48, the flag's id at +64 and a 1 at +72 on the player's own. With those two the card's Remove
//   deleted the player's own flag; without them it does nothing.
// - card: OnFocus 0xbd83711c (ui/features/map/OnPersonalFastTravelPointFocus: Set Waypoint, Fast
//   Travel to the point, Remove), OnUnFocus HideMapCursorPanel.
// With a flag of the player's own on the map, a remote flag's models are copies of its; without
// one, they are made from a bus stop's, with the flag's frame and tints, and the card command is
// found on the heap (as the party card's is). Titled with the owner's name; Entity stays empty.
constexpr std::string_view fast_travel_flag_item = "own_bkinteractive_generic_fasttravelflag_00001";
constexpr std::uint32_t personal_fast_travel = 16, bus_stop = 5;
constexpr std::uint32_t flag_card_focus = 0xbd83711c; // ui/features/map/OnPersonalFastTravelPointFocus
constexpr std::uint32_t bus_card_focus = 0xac5c80a0;  // a bus stop's OnFocus
// The model of a type of `hash` and `size`: whether `type` is it.
bool model_schema(Address type, std::uint32_t hash, std::uint16_t size) {
    return type && read<std::uint32_t>(read<Address>(type)) == hash && read<std::uint16_t>(read<Address>(type) + 6) == size;
}
template <std::size_t N> std::array<std::byte, N> model_bytes(Address manager, Handle handle) {
    std::array<std::byte, N> bytes{};
    const auto at = game::native_data().models.value(manager, handle, 0, 0);
    require(at && memory::read_bytes(at, bytes.data(), bytes.size()), "Native map flag model unavailable.");
    return bytes;
}
template <std::size_t N, class T> void put_at(std::array<std::byte, N> &bytes, std::size_t offset, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    require(offset + sizeof(value) <= N, "Native map flag model bounds differ.");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void update_flag_markers() {
    auto &s = state(); auto &n = game::native_data().models;
    const auto now = GetTickCount64();
    if (now < s.next_flag_update) return;
    s.next_flag_update = now + 250;
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    const auto map = read<Address>(s.base + party::map_manager);
    if (!map || read<Address>(map) != s.base + party::map_manager_vtable) { s.flag_markers.clear(); s.flag_map = 0; return; }
    const auto manager = read<Address>(map + 0x18);
    if (!manager || !n.value || !n.create || !n.publish) return;
    // A new map screen holds none of the old registrations: never unregister through it.
    if (map != s.flag_map || manager != s.flag_models) { s.flag_map = map; s.flag_models = manager; s.flag_markers.clear(); }
    // Where each flag is and who placed it, read before the model lock.
    std::map<Key, std::array<float, 3>> wanted;
    for (const auto &flag : remote_network_objects_of(fast_travel_flag_item)) wanted[{flag.owner, flag.epoch, flag.id}] = flag.position;
    const bool test = s.flag_markers_test.load(std::memory_order_acquire);
    const auto published = s.published.load(std::memory_order_acquire);
    game::ModelWriteLock lock(manager);
    auto remove = [&](FlagMarker &marker) {
        if (marker.registered && n.value(manager, marker.poi, 0, 0))
            reinterpret_cast<void (*)(Address, Handle)>(s.base + party::map_unregister)(map, marker.poi);
        marker = {};
    };
    // A flag of the local player's own (and, testing, where to put the copies), or else a bus stop.
    std::set<Handle> ours;
    for (const auto &[key, marker] : s.flag_markers) ours.insert(marker.poi);
    const auto table = read<Address>(map + 0x48);
    const auto buckets = read<std::uint32_t>(map + 0x50), count = read<std::uint32_t>(map + 0x54);
    require(table && buckets <= 65536 && count <= 20000, "Native map POI registry exceeds bound.");
    Handle donor{}, stop{};
    // HideMapCursorPanel, which a card's POI needs as its OnUnFocus: a bus stop's may be another
    // command, so it is taken from any POI that has it, as party markers do.
    Address hide_cursor{};
    std::uint64_t test_id{};
    std::optional<std::array<float, 3>> me; // the player's own marker (kind 1), for a test flag when they have none
    unsigned visited{};
    for (unsigned i = 0; i < buckets; ++i)
        for (auto node = read<Address>(table + i * 8); node && visited++ < count; node = read<Address>(node + 8)) {
            const auto handle = read<Handle>(node);
            if (ours.contains(handle) || model_type(manager, handle) != s.base + party::map_poi_type) continue;
            const auto value = n.value(manager, handle, 0, 0);
            const auto kind = value ? read<std::uint32_t>(value + 404) : 0u;
            if (!hide_cursor && (kind == bus_stop || kind == 13 || kind == personal_fast_travel) &&
                function_delegate(read<Address>(value + 264), hide_map_cursor_panel))
                hide_cursor = read<Address>(value + 264);
            if (kind == personal_fast_travel) {
                if (!donor) donor = handle;
                if (test) wanted[{0, 0, ++test_id}] = {read<float>(value + 48) + 6, read<float>(value + 52), read<float>(value + 56)};
            } else if (kind == 1 && !me) {
                me = std::array<float, 3>{read<float>(value + 48) + 6, read<float>(value + 52), read<float>(value + 56)};
            } else if (kind == bus_stop && !stop) {
                // Only one on the flag's widget, with its icon model.
                const auto view = read<Handle>(value + 344);
                const auto view_value = view && model_schema(model_type(manager, view), 0xf88f43c6, 240) ? n.value(manager, view, 0, 0) : 0;
                if (view_value && model_schema(model_type(manager, read<Handle>(view_value + 16)), 0xf12b24a5, 208)) stop = handle;
            }
        }
    // Testing without a flag of the player's own: one 6 m from them, made from a bus stop.
    if (test && !test_id && me) wanted[{0, 0, 1}] = *me;
    for (auto it = s.flag_markers.begin(); it != s.flag_markers.end();) {
        if (wanted.contains(it->first) && it->second.poi && n.value(manager, it->second.poi, 0, 0)) { ++it; continue; }
        remove(it->second);
        it = s.flag_markers.erase(it);
    }
    if (wanted.empty()) return;
    const auto source = donor ? donor : stop;
    if (!source) {
        if (!std::exchange(s.flag_donor_logged, true))
            logging::log(logging::Level::info, logging::Channel::ui,
                "Map: {} fast travel flag(s) of other players are not shown yet: the map has no flag or bus stop to make them from.", wanted.size());
        return;
    }
    s.flag_donor_logged = false;
    const auto source_value = n.value(manager, source, 0, 0);
    const auto view_source = read<Handle>(source_value + 344);
    const auto view_type = model_type(manager, view_source);
    require(model_schema(view_type, 0xf88f43c6, 240), "Native map flag widget schema differs.");
    const auto icon_source = read<Handle>(n.value(manager, view_source, 0, 0) + 16);
    const auto icon_type = model_type(manager, icon_source);
    require(model_schema(icon_type, 0xf12b24a5, 208), "Native map flag icon schema differs.");
    // The card: a flag of the player's own lends its command, which is kept for when there is none.
    auto on_unfocus = read<Address>(source_value + 264);
    if (!function_delegate(on_unfocus, hide_map_cursor_panel)) on_unfocus = hide_cursor;
    const bool unfocus = function_delegate(on_unfocus, hide_map_cursor_panel);
    if (donor) {
        const auto focus = read<Address>(source_value + 272);
        if (function_delegate(focus, flag_card_focus)) s.flag_card = focus;
    }
    auto on_focus = s.flag_card;
    if (on_focus && !function_delegate(on_focus, flag_card_focus)) { // an unloaded level's
        s.flag_card = 0;
        on_focus = 0;
    }
    const bool card = on_focus && unfocus;
    // Before the player has placed a flag this session the flag card is not loaded: a bus stop's card
    // then, with a copy of its data at the flag's place (see below). Tested 2026-11-03: its Fast Travel
    // goes to the place in that data, so to the flag.
    const auto stop_focus = !donor ? read<Address>(source_value + 272) : 0;
    const auto stop_data = !donor ? read<std::uint64_t>(source_value + 352) : 0;
    const auto stop_data_type = stop_data ? model_type(manager, stop_data) : 0;
    const bool bus_card = !card && unfocus && function_delegate(stop_focus, bus_card_focus) &&
                          model_schema(stop_data_type, 0xb92656e0, 80);
    for (const auto &[key, at] : wanted) {
        auto &marker = s.flag_markers[key];
        const auto owner = std::get<0>(key);
        if (marker.poi && card && !marker.card) remove(marker); // rebuilt with the card once its command is known
        if (!marker.poi) {
            struct BuildGuard { FlagMarker &marker; bool done{}; ~BuildGuard() { if (!done) marker = {}; } } guard{marker};
            // What was being made when something failed, for the log.
            const char *step = "its models";
            try {
            const auto ns = game::native_name_hash("ReSkate.FlagMap") ^ static_cast<std::uint32_t>(owner) ^
                            static_cast<std::uint32_t>(owner >> 32) ^ static_cast<std::uint32_t>(std::get<1>(key));
            auto create = [&](Address type) {
                const auto h = n.create(manager, type, std::get<2>(key), ns, false, 2);
                require(h, "Cannot create native flag map model."); return h;
            };
            const auto icon_value = n.value(manager, icon_source, 0, 0);
            const auto frame_source = read<Handle>(icon_value + 32), tint_source = read<Handle>(icon_value + 48);
            const auto frame_type = model_type(manager, frame_source), tint_type = model_type(manager, tint_source);
            require(model_schema(frame_type, 0x19770b6a, 208) && model_schema(tint_type, 0x26d4c4e0, 288), "Native map flag icon parts differ.");
            // The details' type is registered only while a flag's data is loaded: without any, the POI goes
            // without details. Neither it nor the card's command is loaded before the player has placed a flag in
            // this session; afterwards both stay (tested 2026-11-03: a copy kept its card and Fast Travel once
            // the player's own flag was removed).
            auto details_type = donor ? model_type(manager, read<Handle>(source_value + 352)) : registered_model_type(manager, 0x7b558c03);
            if (!model_schema(details_type, 0x7b558c03, 80)) details_type = 0;
            // With the bus stop's card, a copy of its data (0xb92656e0, laid out as a flag's: a 3x4
            // rotation, the place at +48): its Fast Travel goes to the place in it.
            const bool bus_details = !details_type && bus_card;
            if (bus_details) details_type = stop_data_type;
            marker.poi = create(s.base + party::map_poi_type);
            marker.view = create(view_type);
            marker.icon = create(icon_type);
            marker.frame = create(frame_type);
            marker.tint = create(tint_type);
            if (details_type) marker.details = create(details_type);
            // Re-read borrowed sources after creation, which can move model storage.
            auto frame = model_bytes<208>(manager, frame_source);
            auto tint = model_bytes<288>(manager, tint_source);
            auto view = model_bytes<240>(manager, view_source);
            if (!donor) {
                // A bus stop's, made a flag's: the flag's frame (its gold fill) and plain white tints.
                const menu_data::Context context(s.base, manager);
                const auto anchor = read<Address>(n.value(manager, icon_source, 0, 0));
                const auto shape = menu_data::named_asset(context, anchor, "UI/Textures/Icons/Locations/FrameShape/img_Frame_Shape_Location_256");
                const auto gold = menu_data::named_asset(context, anchor, "UI/Features/Rarity/FillStyle/Fillstyle_PremiumGradient");
                if (shape) {
                    put_at(frame, 40, std::uint64_t{0});
                    put_at(frame, 48, shape);
                    put_at(frame, 72, std::uint64_t{0x200000002});
                }
                if (gold) put_at(frame, 80, gold);
                put_at(frame, 112, std::uint64_t{0x3f80000000000000});
                const auto empty = read<std::uint64_t>(n.value(manager, tint_source, 0, 0) + 24); // the engine's empty text
                // Six tints of 48 bytes: white, no colour list, empty names, as the game's own flag has
                // them (its last 32 bytes were not read, so they stay the bus stop's).
                for (std::size_t entry = 0; entry < 6; ++entry) {
                    const auto base = entry * 48;
                    put_at(tint, base, std::uint64_t{0x3f8000003f800000});
                    put_at(tint, base + 8, std::uint64_t{0x3f800000});
                    if (entry == 5) break;
                    put_at(tint, base + 16, std::uint64_t{0});
                    put_at(tint, base + 24, empty);
                    put_at(tint, base + 32, std::uint64_t{0});
                    put_at(tint, base + 40, entry == 0 ? std::uint64_t{0x130} : entry % 2 ? empty : std::uint64_t{0});
                }
                // The view's fields the game's own flag leaves empty.
                put_at(view, 64, std::uint64_t{0});
                put_at(view, 72, std::uint64_t{0});
                put_at(view, 136, std::uint64_t{0});
                put_at(view, 160, std::uint32_t{0});
                put_at(view, 168, std::uint64_t{0});
                put_at(view, 220, std::uint32_t{1});
                logging::log(logging::Level::info, logging::Channel::ui, "Map flags: made from a bus stop's widget (frame shape {}, gold fill {}).",
                    shape ? "found" : "missing", gold ? "found" : "missing");
            }
            step = "the frame";
            n.publish(manager, marker.frame, frame_type, frame.data());
            n.publish(manager, marker.tint, tint_type, tint.data());
            // The icon: the source's, linked to this marker's own frame and tint. Their {type, handle}
            // references sit at +24 and +40; the engine's typed copy takes them with the rest.
            step = "the icon";
            auto icon = model_bytes<208>(manager, icon_source);
            put_at(icon, 32, marker.frame);
            put_at(icon, 48, marker.tint);
            n.publish(manager, marker.icon, icon_type, icon.data());
            // Publication also returns false for a value it already has, so what it holds is checked instead.
            const auto icon_now = n.value(manager, marker.icon, 0, 0);
            if (!icon_now || read<Handle>(icon_now + 32) != marker.frame || read<Handle>(icon_now + 48) != marker.tint)
                throw std::runtime_error(std::format("Native map flag icon links were not kept (frame {:#x} tint {:#x}).",
                    icon_now ? read<Handle>(icon_now + 32) : 0, icon_now ? read<Handle>(icon_now + 48) : 0));
            step = "the widget";
            n.publish(manager, marker.view, view_type, view.data());
            // The widget's view model: this marker's own icon model, by the same reference walk as a party marker's.
            step = "the widget's view model link";
            const auto content = field(manager, marker.view, 0, 0x85321504, 0);
            const auto normal = field(manager, content, 0, 0x716496c8, 0);
            const auto meta = read<Address>(model_type(manager, normal));
            require(read<std::uint16_t>(meta + 6) == 24 && read<std::uint16_t>(meta + 0x2a) <= 8, "Native map flag widget reference size differs.");
            const auto fields = read<Address>(meta + 0x60);
            Handle context{};
            for (unsigned i = 0; i < read<std::uint16_t>(meta + 0x2a); ++i) {
                const auto f = fields + i * 24;
                if (read<std::uint16_t>(f + 8) == 8 && read<Address>(f + 16) == s.base + engine::reference_type)
                    context = n.field(manager, normal, i, UINT32_MAX, false);
            }
            struct Ref { Address value{}; Handle handle{}; };
            publish_value(manager, context, s.base + engine::reference_type, Ref{0, marker.icon});
            // The details: a level 3x4 rotation and the flag's place; no flag id, no "yours".
            std::array<std::byte, 80> details{};
            step = "the details";
            if (bus_details) { // the stop's own fields after the place stay; its rotation is made level
                details = model_bytes<80>(manager, stop_data);
                std::memset(details.data(), 0, 48);
            }
            put_at(details, 0, 1.f);
            put_at(details, 20, 1.f);
            put_at(details, 40, 1.f);
            put_at(details, 48, at);
            if (details_type) n.publish(manager, marker.details, details_type, details.data());
            step = "the map point";
            alignas(16) std::array<std::byte, 416> poi{};
            reinterpret_cast<void (*)(void *)>(s.base + party::poi_construct)(poi.data());
            if (card || bus_card) {
                const auto focus = card ? on_focus : stop_focus;
                std::memcpy(poi.data() + 264, &on_unfocus, sizeof on_unfocus);
                std::memcpy(poi.data() + 272, &focus, sizeof focus);
            }
            n.publish(manager, marker.poi, s.base + party::map_poi_type, poi.data());
            std::memset(poi.data() + 264, 0, 16);
            reinterpret_cast<void (*)(void *)>(s.base + party::poi_destroy)(poi.data());
            marker.card = card;
            publish_value(manager, field(manager, marker.poi, 0, 0xc1d9bffe, 404), s.base + party::map_poi_kind_type, personal_fast_travel);
            // A unique ElementKey from a private range below the game's (see update_map_markers).
            static std::uint32_t next_key = 0x5e530000;
            if (++next_key >= 0x7fffffffU) next_key = 0x5e530001;
            marker.key = next_key;
            publish_value(manager, field(manager, marker.poi, 8, 0x0fc37633, 400), s.base + engine::uint32_type, marker.key);
            if (marker.details) publish_value(manager, field(manager, marker.poi, 5, 0x9df3c20e, 352), s.base + engine::uint64_type, marker.details);
            publish_value(manager, field(manager, marker.poi, 6, 0x5e7ee0fd, 336), s.base + engine::reference_type, Ref{0, marker.view});
            publish_value(manager, field(manager, marker.poi, 4, 0x19ff199a, 392), s.base + party::vec2_type, std::array<float, 2>{160, 160});
            publish_value(manager, field(manager, marker.poi, 11, 0x86c837b9, 412), s.base + engine::bool_type, false);
            const std::array<float, 16> world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, at[0], at[1], at[2], 1};
            publish_value(manager, field(manager, marker.poi, 9, 0x24939145, 0), s.base + party::matrix_type, world);
            } catch (const std::exception &e) {
                throw std::runtime_error(std::format("Map flag: making {} failed: {}", step, e.what()));
            }
            guard.done = true;
            logging::log(logging::Level::info, logging::Channel::ui, "Map: a fast travel flag of {} at ({:.1f}, {:.1f}, {:.1f}), from {}{}{}.",
                owner ? std::to_string(owner) : std::string("the test"), at[0], at[1], at[2], donor ? "a flag of yours" : "a bus stop",
                card ? "" : bus_card ? " (with a bus stop's card: no flag placed yet)" : " (no card)", marker.details ? "" : ", without details (none loaded)");
        }
        // Titled with whoever placed it, as the session names them.
        std::string title = owner ? "Fast Travel Flag" : "Test flag";
        if (const auto record = owner && published ? published->record(owner) : nullptr; record && !record->player.name.empty())
            title = record->player.name;
        if (marker.title != title) {
            const NativeText name(title);
            publish_value(manager, field(manager, marker.poi, 1, 0x44688629, 368), s.base + engine::string_type, name.value);
            marker.title = title;
        }
        if (!marker.registered) {
            reinterpret_cast<void (*)(Address, Handle)>(s.base + party::map_register)(map, marker.poi);
            marker.registered = true;
        }
    }
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
std::uint32_t teleport_request(Address reason, const void *matrix, Address destination,
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
    // A plugin match's countdown (kit::match().input_lock) lands the skater on the board.
    if(!id && stance && dingosdk::throwdowns::kit::match().input_lock) {
        static thread_local std::array<std::uint8_t,3> on_board{};
        if(memory::peek_bytes(stance,on_board.data(),on_board.size())) {
            on_board[0]=1;
            return state().teleport(reason,matrix,destination,id,mode,reinterpret_cast<Address>(on_board.data()),transition);
        }
    }
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
// Every teleport request (RequestTeleport: Fast Travel, a party teleport, a plugin's) is logged
// with where it goes and what the game answered. Reported 2026-11-04: Fast Travel back to the
// player's own flag sometimes leaves a black screen. The fade waits for the request it was given;
// a refused one (no request id) or one that never finishes would leave it black, and this tells
// those apart in a player's log.
std::uint32_t teleport_hook(Address reason, const void *matrix, Address destination,
    std::uint64_t id, std::uint8_t mode, Address stance, Address transition) {
    const auto result = teleport_request(reason, matrix, destination, id, mode, stance, transition);
    try {
        std::array<float, 16> world{};
        std::array<std::uint8_t, 3> options{};
        const bool placed = matrix && memory::peek_bytes(reinterpret_cast<Address>(matrix), world.data(), sizeof world);
        const bool optioned = stance && memory::peek_bytes(stance, options.data(), options.size());
        logging::log(logging::Level::info, logging::Channel::player,
            "Teleport: request {} to {}, {}mode {}, options {}{}.",
            result == UINT32_MAX ? std::string("refused") : std::format("{}", result),
            placed ? std::format("({:.1f}, {:.1f}, {:.1f})", world[12], world[13], world[14]) : std::string("no place"),
            id ? std::format("player {:#x}, ", id) : std::string(), mode,
            optioned ? std::format("board {} stance {} lip {}", options[0], options[1], options[2]) : std::string("none"),
            transition ? "" : ", no transition");
    } catch (...) {}
    return result;
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
// profile_runtime::player_info_override: while that prompt runs, the info of whoever placed the
// object nearest the skater, when that is another player the party HUD has a record for.
std::uint64_t object_owner_info() noexcept {
    if (!object_prompt_running) return 0;
    try {
        auto &s = state();
        if (!s.installed.load(std::memory_order_acquire) || !object_prompt_vm || !object_prompt_resource) return 0;
        // The object the card is for: the script's local 0x380, read from the card's model at
        // its start, well before it asks whose card to show (flow_buildkit aa058fc: frame
        // 0xc0, constants 0x1be0; a script of another shape is left alone).
        std::uint32_t frame{}, constants{};
        Address registers{}, locals{};
        std::uint64_t entity{};
        if (!memory::peek(object_prompt_resource + 0x20, frame) || frame != 0xc0 ||
            !memory::peek(object_prompt_resource + 0x24, constants) || constants != 0x1be0 ||
            !memory::peek(object_prompt_vm + 0x30, registers) || !registers ||
            !memory::peek(registers + 0xc0 + 16, locals) || !locals || !memory::peek(locals + 0x380, entity)) return 0;
        const auto owner = profile_runtime::network_object_owner_of(entity);
        Address ui{};
        if (!owner || !memory::peek(s.base + engine::ui_manager, ui) || !ui) return 0;
        // A dedicated server's own objects (its parks) name the server: it is in no player list,
        // so its card is not one get_info finds.
        if (const auto published = s.published.load(std::memory_order_acquire);
                published && published->server.handle && published->server.player.id == owner) {
            Address manager{};
            if (!memory::peek(ui + 0x140, manager) || manager != published->manager) return 0;
            game::ModelWriteLock lock(manager);
            return game::native_data().models.value(manager, published->server.handle, 0, 0) ? published->server.handle : 0;
        }
        return get_info_hook(ui, owner);
    } catch (...) {
        return 0;
    }
}
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
    struct Prompt {
        bool on;
        explicit Prompt(bool running, Address vm, Address resource) : on(running) {
            if (!on) return;
            ++object_prompt_running;
            object_prompt_vm = vm;
            object_prompt_resource = resource;
        }
        ~Prompt() {
            if (!on) return;
            --object_prompt_running;
            object_prompt_vm = object_prompt_resource = 0;
        }
    } prompt(hash == object_prompt::graph, vm, resource);
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
    profile_runtime::player_info_override.store(&object_owner_info, std::memory_order_release);
    try { install_requests(base); } catch (const std::exception &e) { status(e.what()); }
}
} // namespace native_party_detail
void set_native_challenge_coop(bool coop) noexcept { state().challenge_coop.store(coop, std::memory_order_release); }
void execute_native_party_expression(std::uintptr_t vm, std::uint32_t pc, std::uintptr_t profiler,
                                    void (*runner)(std::uintptr_t, std::uint32_t, std::uintptr_t)) {
    run_expression(vm, pc, profiler, runner);
}
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
bool party_spectating() noexcept {
    auto &s = state();
    return s.spectating.load(std::memory_order_acquire) != 0 || s.camera_owned;
}
void tick_native_party_actions(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase) noexcept {
    auto &s = state();
    if (!s.installed.load(std::memory_order_acquire)) return;
    try { update_map_markers(); } catch (const std::exception &e) { status(e.what()); }
    try { update_flag_markers(); } catch (const std::exception &e) {
        // A marker that could not be made is tried again in a while, not every pass: each try makes models.
        state().next_flag_update = GetTickCount64() + 5000;
        status(e.what());
    }
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
