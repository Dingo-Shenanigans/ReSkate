#include "native_party_internal.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "Engine/Core/Log/logging.h"
#include <algorithm>
#include <cstring>

namespace dingosdk::multiplayer {
using namespace native_party_detail;
namespace {
// The authored Party panel has eight rows, and the party colour list eight colours.
constexpr unsigned authored_party_slots = 8;
// The type an entry of the registry holds when it holds the type named by hash.
Address registered_type(Address entry, std::uint32_t hash) {
    const auto first = read<Address>(entry + 8), last = read<Address>(entry + 16);
    if (!first || last <= first) return 0;
    const auto type = read<Address>(first);
    return type && read<std::uint32_t>(read<Address>(type)) == hash ? type : 0;
}
Address type_with_hash(Address manager, std::uint32_t hash) {
    // Registered native model types, not a process-memory/string scan. Dynamic
    // UI schemas are registered again when their asset partition is loaded.
    const auto begin = read<Address>(manager + 0x600), end = read<Address>(manager + 0x608);
    require(begin && end >= begin && (end - begin) % 72 == 0 && (end - begin) / 72 <= 65536,
            "Native party model type registry differs.");
    // The registry holds thousands of types. While it is the same array, the entry
    // found last time is checked again instead (the same test the walk applies);
    // a type not found is looked for again once the array changes, or after 1 s.
    auto &cached = state().types[hash];
    const bool same = cached.manager == manager && cached.begin == begin && cached.end == end;
    if (same && cached.type && registered_type(cached.entry, hash) == cached.type) return cached.type;
    const auto now = GetTickCount64();
    if (same && !cached.type && now < cached.retry_at) return 0;
    for (auto at = begin; at < end; at += 72) {
        if (const auto type = registered_type(at, hash)) {
            cached = {manager, begin, end, at, type};
            return type;
        }
    }
    cached = {manager, begin, end, 0, 0, now + 1000};
    return 0;
}
} // namespace
namespace native_party_detail {
Address registered_model_type(Address manager, std::uint32_t hash) { return type_with_hash(manager, hash); }
void require(bool ok, const char *text) { if (!ok) throw std::runtime_error(text); }
State &state() { static auto *s = new State; return *s; }
void status(std::string message) {
    auto &s = state();
    std::lock_guard lock(s.status_mutex);
    if (message == s.status) return;
    s.status = std::move(message);
    logging::write(logging::Level::info, logging::Channel::ui, s.status);
}
Address model_type(Address manager, Handle handle) {
    const auto base = state().base;
    const auto record = reinterpret_cast<Address (*)(Address, Handle, std::uint8_t)>(base + party::model_record)(manager, handle, 0);
    if (!record) return 0;
    return reinterpret_cast<Address (*)(Address, Handle, Address, std::uint8_t)>(base + party::model_type)(manager, handle, record, 0);
}
Handle singleton(Address manager, std::uint32_t hash, std::uint16_t size) {
    const auto type = type_with_hash(manager, hash);
    if (!type) return 0;
    require(read<std::uint16_t>(read<Address>(type) + 6) == size, "Native party singleton size differs.");
    return reinterpret_cast<Handle (*)(Address, Address, std::uint8_t, std::uint8_t)>(state().base + party::model_singleton)(
        manager, type, 0, 1);
}
Handle field(Address manager, Handle parent, unsigned index, std::uint32_t hash, std::uint16_t offset) {
    const auto type = model_type(manager, parent);
    require(type, "Native party model handle expired.");
    const auto meta = read<Address>(type);
    require(index < read<std::uint16_t>(meta + 0x2a), "Native party field count differs.");
    const auto entry = read<Address>(meta + 0x60) + index * 24;
    require(read<std::uint32_t>(entry) == hash && read<std::uint16_t>(entry + 8) == offset,
            "Native party field schema differs.");
    const auto result = game::native_data().models.field(manager, parent, index, UINT32_MAX, false);
    require(result && model_type(manager, result) == read<Address>(entry + 16), "Native party child type differs.");
    return result;
}
} // namespace native_party_detail
namespace {
template<std::size_t N, class T> void put(std::array<std::byte, N> &bytes, std::size_t offset, T value) {
    require(offset + sizeof(value) <= N, "Native party record bounds differ.");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
// A player's own party from the roster. `shown` false (the local party of a lobby shown without
// the party count preference) reports none, as before.
GroupStatus group_status(const PartyRoster &roster, unsigned capacity, const PartyPlayer &player, bool shown) {
    GroupStatus g;
    g.limit = static_cast<std::uint8_t>(std::clamp(capacity, 1U, 255U));
    if (!player.party || !shown) return g;
    std::uint8_t members{};
    std::uint64_t leader{};
    for (const auto &other : roster)
        if (other.id && other.party == player.party) {
            ++members;
            if (other.leader_of_party) leader = other.id;
        }
    if (members < 2) return g;
    g.id = native_party_group_id(player.party);
    g.members = members;
    g.leader = player.leader_of_party;
    g.leader_id = leader;
    g.join = player.party_open ? 2 : 1;
    return g;
}
// The dedicated server's card is no player's: its player id is none the roster gives (those
// count up from 0x7e000000), and not 0, which the game takes for the local player's without
// its own service (a placed object's menu then offered Edit on what the server placed).
constexpr std::uint32_t server_player_id = 0x7effffff;
Handle player_record(Address manager, const PartyPlayer &player, std::uint32_t player_id,
                     bool party = true, bool friend_record = false, bool online = true, std::int32_t colour = -1,
                     const GroupStatus &group = {}) {
    auto &s = state(); auto &n = game::native_data().models;
    const auto key = std::pair{player.id, player.epoch};
    auto found = s.handles.find(key);
    Handle handle = found == s.handles.end() ? 0 : found->second;
    if (!handle || !n.value(manager, handle, 0, 0)) {
        require(s.handles.size() < 8192, "Native social record limit reached; reload the map to refresh UI models.");
        // Distinct from the native service namespace: these IDs never register
        // an EA account or a ClientPlayer. Native UI lookups are adapted below.
        handle = n.create(manager, s.base + engine::player_info_type, player.id,
            game::native_name_hash("ReSkate.SteamParty") ^ static_cast<std::uint32_t>(player.epoch) ^
            static_cast<std::uint32_t>(player.epoch >> 32), false, 2);
        require(handle, "Cannot create native party player info.");
        s.handles[key] = handle; s.owned.insert(handle); s.owned_ids.insert(player.id);
    }
    require(model_type(manager, handle) == s.base + engine::player_info_type, "Native party player info type differs.");
    alignas(8) std::array<std::byte, 0x108> info{};
    reinterpret_cast<void (*)(void *)>(s.base + engine::card_info_construct)(info.data());
    put(info, 0x00, group.join); put(info, 0x04, group.invite); put(info, 0x08, group.id);
    put(info, 0x10, group.members); put(info, 0x11, group.limit); put(info, 0x12, group.leader);
    put(info, 0x18, group.leader_id); put(info, 0x20, group.unlocked);
    // Social actions (Invite) need the player "unlocked" (+0x95); friends are IsFriend (+0x104).
    put(info, 0x95, std::uint8_t{1});
    put(info, 0x104, friend_record);
    const NativeText text(player.name.empty() ? "Skater" : player.name);
    put(info, 0x28, text.value); put(info, 0x38, text.value);
    put(info, 0x40, player.id); put(info, 0x4c, player_id);
    put(info, 0xa0, handle);
    put(info, 0xf0, std::uint32_t{player.local ? 0U : party ? 1U : online ? 4U : 5U});
    put(info, 0xcc, std::uint32_t{friend_record ? (party ? 3U : 2U) : 1U});
    put(info, 0xd8, std::uint32_t{online ? 0U : 2U});
    // Player network (EadpPlayerNetwork): every ReSkate player is on Steam; left at 0 the
    // UI shows the EA logo beside the name.
    put(info, 0xf4, std::uint32_t{4});
    const auto card_part = [&](std::uint32_t hash) {
        return !player.local && profile_runtime::reserved_cosmetic_hash(hash) ? 0U : hash;
    };
    put(info, 0xd0, card_part(player.card.background)); put(info, 0xc4, card_part(player.card.emblem));
    put(info, 0xd4, card_part(player.card.title));
    // The party colour index (map dots, waypoints, the party list and voice HUD pick their
    // colour from PartyMemberColorsList by it) and the mic state the voice HUD shows.
    put(info, 0xc0, party ? colour : std::int32_t{-1});
    put(info, 0xdc, std::int32_t{player.mic});
    put(info, 0x100, player.present); put(info, 0x101, party);
    put(info, 0x105, player.local); put(info, 0x107, player.present);
    n.publish(manager, handle, s.base + engine::player_info_type, info.data());
    require(n.value(manager, handle, 0, 0), "Native party player info publication failed.");
    return handle;
}
void social_list(Address ui, Address manager, const Published &published, bool friends = false) {
    auto &n = game::native_data().models;
    const auto handle = read<Handle>(ui + (friends ? 0xf8 : 0xf0));
    require(handle && model_type(manager, handle) == state().base + party::player_list_type,
            "Native social player list type differs.");
    struct Ref { Address value{}; Handle context{}; };
    game::NativeArrayStorage<Ref, 2304> next;
    Address data{}; std::uint32_t count{};
    const auto value = n.value(manager, handle, 0, 0);
    require(value && game::native_array<Ref>(reinterpret_cast<const void *>(value), data, count, 2304),
            "Native social player list exceeds bound.");
    for (unsigned i = 0; i < count; ++i) {
        const auto item = read<Ref>(data + i * sizeof(Ref));
        if (!state().owned.contains(item.context)) {
            require(next.count < 248, "Native social list has too many non-Steam records.");
            next.values[next.count++] = item;
        }
    }
    if (friends) {
        for (const auto &record : published.friends) if (record.handle) next.values[next.count++] = {0, record.handle};
    } else for (const auto &record : published.records) {
        if (record.handle && !record.player.local) {
            // Native ModelRef is a reference-counted boxed value plus a model
            // handle. The native UI roster uses a null boxed value here.
            next.values[next.count++] = {0, record.handle};
        }
    }
    const auto pointer = next.values.data();
    n.publish(manager, handle, state().base + party::player_list_type, &pointer);
    if (friends) {
        const auto root = singleton(manager, 0xdd652425, 40);
        if (root) {
            game::NativeArrayStorage<Handle, 2304> handles;
            for (unsigned i = 0; i < next.count; ++i) handles.values[handles.count++] = next.values[i].context;
            const auto values = handles.values.data();
            publish_value(manager, field(manager, root, 0, 0x7138dc31, 0), state().base + party::uint64_array_type, values);
            publish_value(manager, field(manager, root, 1, 0xa19aaa9b, 8), state().base + engine::bool_type, true);
            const auto online = static_cast<std::int32_t>(std::count_if(published.friends.begin(), published.friends.end(),
                [&](const auto &record) {
                    const auto info = n.value(manager, record.handle, 0, 0);
                    return info && read<std::uint32_t>(info + 0xd8) == 0;
                }));
            publish_value(manager, field(manager, root, 4, 0xbe820091, 32), state().base + engine::int32_type, online);
        }
    }
}
void friends_view(Address manager, const Published &published) {
    auto &s = state(); auto &n = game::native_data().models;
    // The current Friends tab observes UIFriendsService's sorted list, not
    // UIPlayerManager.Friends or the legacy FriendsData singleton alone.
    const auto service = read<Address>(s.base + party::friends_service);
    if (!service || read<Address>(service) != s.base + party::friends_service_vtable) return;
    const auto handle = read<Handle>(service + 0x120);
    require(handle && model_type(manager, handle) == s.base + party::sorted_friends_type,
            "Native sorted friends model differs.");
    Address rows{}; std::uint32_t count{};
    const auto value = n.value(manager, handle, 0, 0);
    require(value && game::native_array<Handle>(reinterpret_cast<const void *>(value), rows, count, 2304),
            "Native sorted friends list exceeds bound.");
    game::NativeArrayStorage<Handle, 2304> next;
    for (unsigned i = 0; i < count; ++i) {
        const auto item = read<Handle>(rows + i * sizeof(Handle));
        if (!s.owned.contains(item)) {
            require(next.count < 248, "Native sorted friends list has too many non-Steam records.");
            next.values[next.count++] = item;
        }
    }
    // Authored FriendsFilter: 0 = all, 1 = EA-only, 2 = platform-only.
    const auto filter = read<std::uint32_t>(service + 0x130);
    if (filter == 0 || filter == 2)
        for (const auto &record : published.friends) if (record.handle) next.values[next.count++] = record.handle;
    const auto pointer = next.values.data();
    n.publish(manager, handle, s.base + party::sorted_friends_type, &pointer);
}
bool party_list(Address manager, const Published &published) {
    const auto root = singleton(manager, 0xf86b63a2, 104);
    if (!root) return false;
    auto &n = game::native_data().models; const auto base = state().base;
    // The party's stable id (the local player's own Group).
    std::uint64_t party_id{};
    for (const auto &record : published.records)
        if (record.handle && record.player.local) party_id = record.group.id;
    publish_value(manager, field(manager, root, 2, 0xda8be596, 16), base + engine::uint64_type, party_id);
    const auto list = field(manager, root, 0, 0xeec2d797, 0);
    Address rows{}; std::uint32_t count{};
    using Row = std::array<std::byte, 112>;
    const auto value = n.value(manager, list, 0, 0);
    // Social records, map markers and our multiplayer roster cover the full network membership.
    require(value && game::native_array<Row>(reinterpret_cast<const void *>(value), rows, count, authored_party_slots) &&
            count == authored_party_slots, "Native party requires its authored eight member slots.");
    // Do not replace an unrelated native party or its callbacks. Only empty
    // slots and records previously projected by this adapter are eligible.
    for (unsigned i = 0; i < count; ++i) {
        const auto id = read<std::uint64_t>(rows + i * 112 + 72);
        require(!id || state().owned_ids.contains(id), "An existing native party owns the party menu.");
    }
    // Party members only (the roster lists them first, leader first); the rest of the
    // session are other players, not the party.
    std::array<const Record *, authored_party_slots> members{};
    unsigned used{};
    for (const auto &record : published.records)
        if (record.handle && record.player.member && used < members.size()) members[used++] = &record;
    const Record empty;
    for (unsigned i = 0; i < count; ++i) {
        const auto member = n.field(manager, list, UINT32_MAX, i, false);
        const auto type = model_type(manager, member);
        require(type && read<std::uint32_t>(read<Address>(type)) == 0xd0ffd455 &&
                read<std::uint16_t>(read<Address>(type) + 6) == 112,
                "Native party member element differs.");
        const auto &record = i < used ? *members[i] : empty;
        const NativeText name(record.handle ? record.player.name : "");
        publish_value(manager, field(manager, member, 1, 0x1011d206, 48), base + engine::string_type, name.value);
        publish_value(manager, field(manager, member, 3, 0x94877505, 64), base + engine::bool_type, false);
        publish_value(manager, field(manager, member, 4, 0xb9c67172, 65), base + engine::bool_type, record.player.local);
        publish_value(manager, field(manager, member, 5, 0x3bd2a3cb, 72), base + engine::persona_id_type, record.player.id);
        publish_value(manager, field(manager, member, 6, 0x81d5aa57, 80), base + engine::uint64_type, record.handle);
        publish_value(manager, field(manager, member, 7, 0x9d501f88, 88), base + engine::bool_type, false);
    }
    return true;
}
// A field of a model, published only while this build's schema has it where expected (else skipped).
template<class T> bool publish_field(Address manager, Handle parent, unsigned index, std::uint32_t hash,
                                     std::uint16_t offset, const T &value) {
    const auto type = model_type(manager, parent);
    if (!type) return false;
    const auto meta = read<Address>(type);
    if (index >= read<std::uint16_t>(meta + 0x2a)) return false;
    const auto entry = read<Address>(meta + 0x60) + index * 24;
    if (read<std::uint32_t>(entry) != hash || read<std::uint16_t>(entry + 8) != offset) return false;
    const auto child = game::native_data().models.field(manager, parent, index, UINT32_MAX, false);
    const auto child_type = read<Address>(entry + 16);
    if (!child || model_type(manager, child) != child_type) return false;
    game::native_data().models.publish(manager, child, child_type, &value);
    return true;
}
// The local player's own UIPlayerInfo is the game's: its Profile.Group is published field by field
// (our records carry theirs in the record itself, player_record).
void party_profile(Address manager, Handle info, const GroupStatus &group) {
    if (!info || model_type(manager, info) != state().base + engine::player_info_type) return;
    const auto base = state().base;
    const auto profile = field(manager, info, 1, 0x8f991be2, 0);
    const auto g = field(manager, profile, 8, 0x0c470c21, 0);
    // IsPartyLeader_Predicate reads UIPlayerInfo.Profile.Group.MemberCount
    // and IsLeader. IsPartyMember alone only decorates the roster row.
    publish_value(manager, field(manager, g, 2, 0xbed2dcf6, 8), base + engine::uint64_type, group.id);
    publish_value(manager, field(manager, g, 3, 0x33d68284, 16), base + party::uint8_type, group.members);
    // MaxMemberCount drives the "members / limit" party indicator and the Full checks: the party's cap.
    publish_value(manager, field(manager, g, 4, 0xd6604a2a, 17), base + party::uint8_type, group.limit);
    publish_value(manager, field(manager, g, 5, 0xc261d182, 18), base + engine::bool_type, group.leader);
    publish_value(manager, field(manager, g, 7, 0x9bcfd36c, 24), base + engine::persona_id_type, group.leader_id);
    // Join / invite permission and "parties unlocked" (ui-actions.md 1.2); skipped if moved.
    publish_field(manager, g, 0, 0xed3e9214, 0, group.join);
    publish_field(manager, g, 1, 0xb0c50f57, 4, group.invite);
    publish_field(manager, g, 8, 0xa3e14bc2, 32, group.unlocked);
    publish_value(manager, field(manager, info, 18, 0x11cca848, 257), base + engine::bool_type, group.members > 1);
}
} // namespace
void set_native_party_server(std::uint64_t id, std::string_view name) noexcept {
    try {
        auto &s = state();
        if (s.server_id == id && s.server_name == name) return;
        s.server_id = id;
        s.server_name = name;
        s.server_changed = true;
    } catch (...) {}
}
void update_native_party(std::uintptr_t base, const PartyRoster &roster, unsigned capacity,
                         bool overlay) noexcept {
    auto &s = state();
    try {
        const bool active = std::any_of(roster.begin(), roster.end(), [](const auto &p) { return p.id != 0; });
        const auto steam = steam_social_snapshot();
        if (!active && !s.manager && steam->friends.empty()) return;
        if (!s.attempted) {
            HookBatch batch; // (its hooks in one pause of the game's threads)
            install(base);
        }
        if (!s.installed.load(std::memory_order_acquire)) return;
        const auto now = GetTickCount64();
        auto previous = s.published.load(std::memory_order_acquire);
        bool changed = !previous || previous->social_revision != steam->revision ||
                       previous->capacity != capacity || previous->overlay != overlay;
        changed |= std::exchange(s.server_changed, false);
        bool refreshed{}, talking{};
        if (previous) for (std::size_t i = 0; i < roster.size(); ++i) {
            const auto &old = previous->records[i].player;
            changed |= roster[i].id != old.id || roster[i].epoch != old.epoch || roster[i].name != old.name ||
                       roster[i].present != old.present || roster[i].leader != old.leader || roster[i].card != old.card ||
                       roster[i].member != old.member || roster[i].party != old.party ||
                       roster[i].leader_of_party != old.leader_of_party;
            // Not a native change, but the lookups and markers find players by these.
            refreshed |= roster[i].slot != old.slot || roster[i].local != old.local;
            talking |= roster[i].mic != old.mic;
        }
        const auto ui = read<Address>(base + engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        // Publishing reaches native observers and rewrites the social lists under
        // the model lock: only on a change (roster, Steam friends, limits or a new
        // UI model manager), plus a slow re-publish restoring anything the game
        // replaced in its own lists meanwhile.
        refreshed |= previous && previous->manager != manager;
        // A new local player card also needs the party fields.
        const auto local_info = profile_runtime::player_card_runtime().functions.get_local_info
            ? profile_runtime::local_player_info_hook() : 0;
        refreshed |= previous && local_info != s.local_info;
        if (!changed && !refreshed && !talking && now < s.next_update) return;
        const auto ui_early = read<Address>(base + engine::ui_manager);
        const auto manager_early = ui_early ? read<Address>(ui_early + 0x140) : 0;
        if (!changed && !refreshed && talking && previous && manager_early && previous->manager == manager_early) {
            // Only who is talking changed: republish those players' records, nothing else.
            game::ModelWriteLock lock(manager_early);
            auto next = std::make_shared<Published>(*previous);
            for (std::size_t i = 0; i < roster.size(); ++i) {
                auto &record = next->records[i];
                if (!record.handle || record.player.mic == roster[i].mic) continue;
                record.player.mic = roster[i].mic;
                record.handle = player_record(manager_early, record.player, record.player_id, record.player.member,
                                              record.friend_of, true, record.colour, record.group);
            }
            next->index();
            s.published.store(next, std::memory_order_release);
            return;
        }
        // The party menu's assets load when it is first opened: until its slots are
        // bound, try again soon.
        s.next_update = now + (s.party_ready ? 5000 : 500);
        if (!manager) { s.published.store({}); s.roster_active = false; return; }
        const auto native_client = read<Address>(base + party::party_client);
        if (native_client && read<Address>(native_client + 0x70)) {
            s.published.store({}); s.roster_active = false;
            throw std::runtime_error("An existing native EA party owns the social UI.");
        }
        {
        game::ModelWriteLock lock(manager);
        if (manager != s.manager) {
            s.manager = manager; s.handles.clear(); s.owned.clear(); s.owned_ids.clear();
            s.published.store({});
        }
        auto next = std::make_shared<Published>(); next->manager = manager; next->social_revision = steam->revision;
        next->capacity = capacity; next->overlay = overlay;
        std::int32_t colours{};
        for (std::size_t i = 0; i < roster.size(); ++i) {
            if (!roster[i].id) continue;
            auto &record = next->records[i]; record.player = roster[i];
            record.player_id = 0x7e000000U + static_cast<std::uint32_t>(i);
            // The game's colour list is sized for its eight-member parties: a bigger one repeats them.
            record.colour = roster[i].member ? colours++ % static_cast<std::int32_t>(authored_party_slots) : -1;
            record.friend_of = std::any_of(steam->friends.begin(), steam->friends.end(), [&](const auto &friend_info) {
                return friend_info.id == roster[i].id;
            });
            record.group = group_status(roster, capacity, roster[i], overlay || !roster[i].member);
            const auto &old = previous ? previous->records[i] : Record{};
            if (previous && previous->manager == manager && old.handle &&
                old.player.id == roster[i].id && old.player.epoch == roster[i].epoch &&
                old.player.name == roster[i].name && old.player.present == roster[i].present &&
                old.player.local == roster[i].local && old.player.card == roster[i].card &&
                old.player.member == roster[i].member && old.player.mic == roster[i].mic &&
                old.colour == record.colour && old.friend_of == record.friend_of && old.group == record.group &&
                old.player.party_open == roster[i].party_open &&
                previous->social_revision == steam->revision &&
                game::native_data().models.value(manager, old.handle, 0, 0))
                record.handle = old.handle;
            else record.handle = player_record(manager, roster[i], record.player_id, roster[i].member,
                record.friend_of, true, record.colour, record.group);
        }
        if (previous && previous->manager == manager && previous->social_revision == steam->revision && !changed)
            next->friends = previous->friends;
        else for (const auto &friend_info : steam->friends) {
            const auto member = std::find_if(next->records.begin(), next->records.end(), [&](const auto &r) {
                return r.handle && r.player.id == friend_info.id;
            });
            if (member != next->records.end()) next->friends.push_back(*member);
            else {
                Record record;
                record.player.id = friend_info.id; record.player.name = friend_info.name;
                // Not in the session: nobody can invite them to a party here.
                GroupStatus away;
                away.invite = 0;
                record.handle = player_record(manager, record.player, 0, false, true, friend_info.online, -1, away);
                next->friends.push_back(std::move(record));
            }
        }
        if (s.server_id) {
            auto &server = next->server;
            server.player.id = s.server_id;
            server.player.name = s.server_name;
            const auto &old = previous ? previous->server : Record{};
            server.handle = previous && previous->manager == manager && old.handle && old.player.id == s.server_id &&
                                    old.player.name == s.server_name && game::native_data().models.value(manager, old.handle, 0, 0)
                                ? old.handle
                                : player_record(manager, server.player, server_player_id, false, false, true);
        }
        // Make the identity lookup available before publishing arrays that can
        // immediately invoke native UI observers.
        next->index();
        s.published.store(next, std::memory_order_release);
        s.roster_active.store(active, std::memory_order_release);
        GroupStatus local_group;
        for (const auto &record : next->records)
            if (record.handle && record.player.local) local_group = record.group;
        party_profile(manager, local_info, local_group);
        social_list(ui, manager, *next);
        social_list(ui, manager, *next, true);
        friends_view(manager, *next);
        const bool party_ready = party_list(manager, *next);
        s.party_ready = party_ready;
        s.local_info = local_info;
        const auto count = std::count_if(next->records.begin(), next->records.end(), [](const auto &r) { return r.handle != 0; });
        const auto members = std::count_if(next->records.begin(), next->records.end(),
                                           [](const auto &r) { return r.handle != 0 && r.player.member; });
        status(!active ? "Native party: disconnected." : "Native party: " + std::to_string(count) + " players, " +
            std::to_string(members) + " in your party; social list bound; " +
            (party_ready ? "party slots bound." : "waiting for party menu assets."));
        }
        // The game's own roster observers populate dependent native views.
        // Dispatch after releasing the model lock and only on roster changes.
        if (changed) {
            reinterpret_cast<void (*)()>(base + party::roster_changed)();
            // The party notifications (PartyService rebuilds its rows from them, without our names:
            // republish shortly after).
            const auto published = s.published.load(std::memory_order_acquire);
            if (published) {
                post_party_changes(previous && previous->manager == published->manager ? previous.get() : nullptr, *published);
                s.next_update = std::min<ULONGLONG>(s.next_update, GetTickCount64() + 300);
            }
        }
    } catch (const std::exception &e) { status(e.what()); }
}
void update_party_position(const Pose *pose) noexcept {
    auto &s = state(); std::lock_guard lock(s.position_mutex);
    auto &position = s.positions[peer_slot];
    // Playback poses were fully validated when admitted to PoseBuffer. This
    // adapter consumes only the root, so do not walk another 412 transforms.
    position.valid = pose && valid_transform(pose->root); position.sampled = GetTickCount64();
    if (position.valid) position.root = pose->root;
}
std::uint64_t native_party_player_info(std::uintptr_t manager, std::size_t slot) noexcept {
    try {
        const auto published = state().published.load(std::memory_order_acquire);
        if (!published || published->manager != manager) return 0;
        if (const auto record = published->slot_record(slot)) {
            game::ModelWriteLock lock(manager);
            return game::native_data().models.value(manager, record->handle, 0, 0) ? record->handle : 0;
        }
    } catch (...) {}
    return 0;
}
std::string native_party_status() {
    auto &s = state(); std::lock_guard lock(s.status_mutex);
    return s.status + " Map party markers: " + std::to_string(s.map_count.load()) + ".";
}
void set_native_party_map_markers(bool enabled) noexcept { state().map_markers.store(enabled, std::memory_order_release); }
bool native_party_map_markers() noexcept { return state().map_markers.load(std::memory_order_acquire); }
bool native_party_map_icon_visible(std::size_t slot) noexcept {
    return slot < max_remote_players && ((state().map_slots[slot / 64].load() >> (slot % 64)) & 1) != 0;
}
} // namespace dingosdk::multiplayer
