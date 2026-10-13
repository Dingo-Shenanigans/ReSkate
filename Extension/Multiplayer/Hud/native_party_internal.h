#pragma once
#include "native_party.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Native party state shared by native_party.cpp and native_party_hooks.cpp.
namespace dingosdk::multiplayer::native_party_detail {
using Address = std::uintptr_t;
using Handle = std::uint64_t;
namespace engine = addr::engine;
namespace party = addr::native_party;
// UI models, registries and arrays of the live game: guarded copies, not a system
// call per read (the lookup hooks run for UI queries every frame).
template<class T> T read(Address address) {
    T result{};
    if (!memory::peek(address, result)) throw std::runtime_error("Native party memory unavailable.");
    return result;
}
void require(bool ok, const char *text);
// UIPlayerInfo.Profile.Group (GameGroupStatus, analysis/party-re/native-core.md 1.4): a player's own
// party as the Social menu's buttons read it (ui-actions.md 1.2).
struct GroupStatus {
    std::int32_t join{};       // +0x00: 0 closed, 1 the leader's friends, 2 anyone
    std::int32_t invite = 2;   // +0x04: who may invite this player: anyone
    std::uint64_t id{};        // +0x08: stable party id (native_party_group_id)
    std::uint8_t members{};    // +0x10
    std::uint8_t limit{};      // +0x11
    bool leader{};             // +0x12
    std::uint64_t leader_id{}; // +0x18
    bool unlocked = true;      // +0x20: parties are available (Join needs it)
    bool operator==(const GroupStatus &) const = default;
};
// colour: the party colour index (members 0..N-1 in roster order, -1 otherwise); friend_of: a
// Steam friend's record; group: the player's own party.
struct Record { PartyPlayer player; Handle handle{}; std::uint32_t player_id{}; std::int32_t colour = -1; bool friend_of{};
    GroupStatus group; };
struct Published { Address manager{}; std::array<Record, max_players> records; std::vector<Record> friends;
    Record server; // the dedicated server's card (set_native_party_server): in no list
    std::uint64_t social_revision{}; unsigned capacity = max_players; bool overlay = true;
    // Built by index() once per publication, for the lookup hooks: the first record
    // with a handle for each player ID, in records and in friends, and the first
    // remote record with a handle for each slot (index + 1; 0 = none).
    std::unordered_map<std::uint64_t, std::size_t> record_ids, friend_ids;
    std::array<std::uint16_t, max_remote_players> slot_records{};
    void index() {
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto &entry = records[i];
            if (!entry.handle) continue;
            record_ids.try_emplace(entry.player.id, i);
            if (!entry.player.local && entry.player.slot < max_remote_players && !slot_records[entry.player.slot])
                slot_records[entry.player.slot] = static_cast<std::uint16_t>(i + 1);
        }
        for (std::size_t i = 0; i < friends.size(); ++i)
            if (friends[i].handle) friend_ids.try_emplace(friends[i].player.id, i);
    }
    const Record *record(std::uint64_t id) const {
        const auto found = record_ids.find(id);
        return found == record_ids.end() ? nullptr : &records[found->second];
    }
    // A record of the local player's party (the lookups the game's party queries answer).
    const Record *member(std::uint64_t id) const {
        const auto found = record(id);
        return found && found->player.member ? found : nullptr;
    }
    const Record *friend_record(std::uint64_t id) const {
        const auto found = friend_ids.find(id);
        return found == friend_ids.end() ? nullptr : &friends[found->second];
    }
    // The dedicated server's card, by its id: what it placed asks for its owner by id.
    const Record *server_record(std::uint64_t id) const {
        return server.handle && server.player.id == id ? &server : nullptr;
    }
    const Record *slot_record(std::size_t slot) const {
        return slot < slot_records.size() && slot_records[slot] ? &records[std::size_t{slot_records[slot]} - 1] : nullptr;
    }
};
struct Position { bool valid{}; ULONGLONG sampled{}; Transform root; };
struct MapMarker {
    std::uint64_t id{}, epoch{};
    Handle poi{}, widget{}, display{}, data{};
    bool registered{};
    std::uint32_t key{}; // MapObjectData.ElementKey, unique while registered
    std::string title;   // the name last published (Title is only republished when it changes)
    bool card{};         // built with the live game's focus commands (the player card)
    // The transform last published; unchanged positions are not published again.
    std::optional<Transform> placed;
};
// A registered model type found by its hash (native_party.cpp, type_with_hash),
// reused while the manager's type registry is unchanged and the entry still holds it.
struct TypeEntry { Address manager{}, begin{}, end{}, entry{}, type{}; ULONGLONG retry_at{}; };
struct State {
    Address base{};
    bool attempted{};
    std::atomic<bool> installed{};
    std::atomic<bool> roster_active{};
    std::atomic<bool> challenge_coop{}; // the last challenge started here runs coop (set_native_challenge_coop)
    std::atomic<std::shared_ptr<const Published>> published;
    std::mutex position_mutex, status_mutex;
    std::array<Position, max_remote_players> positions;
    std::string status = "Native party: waiting for a lobby.";
    // Publication is serialized by the client callback and the model lock.
    Address manager{};
    std::map<std::pair<std::uint64_t, std::uint64_t>, Handle> handles;
    std::set<Handle> owned;
    std::set<std::uint64_t> owned_ids;
    ULONGLONG next_update{};
    // set_native_party_server: the session's dedicated server, and whether it changed since
    // the last publication.
    std::uint64_t server_id{};
    std::string server_name;
    bool server_changed{};
    // From the last publication: whether the party menu's slots were bound (until they
    // are, it is retried every 500 ms) and the local player card it was given.
    bool party_ready{};
    Address local_info{};
    std::uint64_t (*get_info)(Address, std::uint64_t){};
    Handle (*find_model)(Address, Address, std::uint64_t, std::uint32_t, std::uint8_t){};
    void (*is_member)(const std::uint64_t *, bool *){};
    void (*member_ids)(Address){};
    void (*members_with_leader)(Address){};
    Address (*weak_player)(Address, std::uint64_t){};
    Address (*weak_entity)(Address, const std::uint64_t *){};
    bool (*world_transform)(Address, void *){};
    std::uint64_t (*player_id)(Address){};
    bool (*can_spectate)(std::uint32_t){};
    bool (*can_teleport)(std::uint32_t){};
    void (*spectate)(Address, bool){};
    bool (*spectate_active)(){};
    void (*spectate_clear)(Address){};
    Address (*spectate_name)(Address){};
    std::uint32_t (*teleport)(Address, const void *, Address, std::uint64_t, std::uint8_t, Address, Address){};
    std::atomic<std::uint64_t> spectating{}, spectating_epoch{};
    // After a teleport ends Spectate, the native view re-issues Spectate for the
    // same player while closing; ignore those until this tick count.
    std::atomic<std::uint64_t> spectate_blocked_until{};
    bool camera_owned{};
    ULONGLONG camera_started{};
    // The native Spectate UI flag is synced at 10 Hz while spectating, and at once
    // when the spectated player changes (tick_native_party_actions).
    std::uint64_t synced_spectate{};
    ULONGLONG next_spectate_sync{};
    std::string action_status;
    Address map_manager{}, map_models{};
    Handle map_list{};
    Handle map_prototype{};
    std::array<MapMarker, max_remote_players> markers;
    ULONGLONG next_map_update{};
    std::atomic<unsigned> map_count{};
    // One bit per remote slot: the slot has a party marker (native_party_map_icon_visible).
    std::array<std::atomic<std::uint64_t>, (max_remote_players + 63) / 64> map_slots{};
    // type_with_hash results by type hash (client thread, under the model lock).
    std::map<std::uint32_t, TypeEntry> types;
    // Party markers (DingoMapPOIType 2, PartyMember) in the map's POI registry, like the live
    // game's PartyMemberMapObjectComponent: named, and focusable for its teleport. Off, players
    // show as the game's plain player dots instead (native_player_ui.cpp), as the live game
    // shows non-party players. `mp map-markers on|off`.
    std::atomic<bool> map_markers{true};
    // The live game's party-marker focus commands (DataModelContextualCommand delegates: a
    // script function's type object): OnFocus = ui/features/map/OnPartyMemberFocus (the player
    // card with Fast Travel and View Profile), OnUnFocus = ui/features/map/HideMapCursorPanel.
    // Level-resident; found per map manager (analysis/map-player-markers.md).
    Address map_on_unfocus{}, map_on_unfocus_record{};
    std::atomic<Address> map_on_focus{};
    std::atomic<bool> focus_scan_running{};
    // Player-card invite cooldowns to clear on the next tick (native_party_hooks.cpp): the
    // record handles whose CountdownDataModel says a cooldown nobody started.
    std::mutex cooldown_mutex;
    std::vector<Handle> stale_cooldowns;
    std::atomic<std::uint64_t> focus_scan_generation{};
    ULONGLONG focus_scan_retry{};
    unsigned focus_scan_attempts{}; // at most two scans until a found command goes stale
    ULONGLONG unfocus_search_at{};  // next registry walk for OnUnFocus while none is known
};
State &state();
void status(std::string message);
Address model_type(Address manager, Handle handle);
Handle singleton(Address manager, std::uint32_t hash, std::uint16_t size);
Handle field(Address manager, Handle parent, unsigned index, std::uint32_t hash, std::uint16_t offset);
template<class T> void publish_value(Address manager, Handle handle, Address type, const T &value) {
    require(handle && model_type(manager, handle) == type, "Native party value type differs.");
    game::native_data().models.publish(manager, handle, type, &value);
}
struct NativeText {
    Address value{};
    explicit NativeText(const std::string &text) {
        game::native_data().values.assign(&value, text.c_str(), static_cast<std::uint32_t>(text.size()));
    }
    ~NativeText() { if (value) reinterpret_cast<void (*)(Address *)>(state().base + engine::native_text_release)(&value); }
    NativeText(const NativeText &) = delete;
};
// native_party_hooks.cpp
void install(Address base);
// native_party_requests.cpp: the game's party buttons and group queries, answered by the SDK.
void install_requests(Address base);
bool post_event(std::uintptr_t type, const void *payload);
// When the local player last invited `id` through the game's Invite (ms, steady clock; 0 = never).
std::uint64_t last_native_invite(std::uint64_t id);
// A registered model type by its hash (native_party.cpp), or 0.
Address registered_model_type(Address manager, std::uint32_t hash);
// The game's party notifications (joined, left, new leader) for what changed between two publications.
void post_party_changes(const Published *previous, const Published &next);
} // namespace dingosdk::multiplayer::native_party_detail
