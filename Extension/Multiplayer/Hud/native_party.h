#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cstdint>
#include <string>

namespace dingosdk::multiplayer {
struct PartyPlayer {
    std::uint64_t id{}, epoch{};
    std::string name;
    // Membership survives map travel and temporary pose loss.
    bool local{}, leader{}, present{};
    std::size_t slot = max_remote_players;
    PlayerCard card;
    // In the local player's party (the local player: in a party at all). Everyone else in
    // the session still gets a native player record (names, cards, nametags) but is shown
    // as an online player, not a party member. `leader` is the party's leader.
    bool member{};
    // The player's own party (0 = none), and whether they lead it: parties other than the
    // local player's show on player cards (joining them) and the game's group queries.
    std::uint32_t party{};
    bool leader_of_party{}, party_open{}; // party_open: anyone may join that party
    // Voice (UIPlayerInfo PlayerMic): -1 no voice, 0 muted, 1 on, 2 talking (the party HUD).
    std::int8_t mic = -1;
};
using PartyRoster = std::array<PartyPlayer, max_players>;
// capacity is the session player limit the native party indicator shows beside
// the member count; pass max_players when no session owns the roster. When
// overlay is false the party group is published as empty, which is how the game
// already hides that indicator outside a party.
void update_native_party(std::uintptr_t base, const PartyRoster &roster, unsigned capacity,
                         bool overlay = true) noexcept;
void update_party_position(const Pose *pose) noexcept;
// Points the party Spectate camera at a session player's skater from code (the
// throwdown turn camera). 0 stops a spectate this function started. Game thread.
void spectate_party_member(std::uint64_t id) noexcept;
// Whether the party Spectate camera follows a player now: the chat rises above the game's
// spectate prompts meanwhile (session_view.cpp, chat_overlay.cpp).
bool native_party_spectating() noexcept;
// Returns only a record owned by this adapter, validated against this manager.
std::uint64_t native_party_player_info(std::uintptr_t manager, std::size_t slot) noexcept;
std::string native_party_status();
// Whether players can form parties: in a session, a lobby or a dedicated server. Outside one
// the game's party buttons are refused.
void set_native_party_changes(bool allowed) noexcept;
// A party invite from `from` (a session player) as the game's own invite toast, whose Accept and
// Decline come back through the request hooks. False when it could not be shown. Game thread.
bool post_native_party_invite(std::uint64_t from) noexcept;
// The game's group id for a ReSkate party number (stable across promotions; 0 = none).
constexpr std::uint64_t native_party_group_id(std::uint32_t party) noexcept {
    return party ? 0x5253500000000000ULL | party : 0;
}
bool native_party_map_icon_visible(std::size_t slot) noexcept;
// Selectable party markers on the pause map (off by default; see native_party_internal.h).
void set_native_party_map_markers(bool enabled) noexcept;
bool native_party_map_markers() noexcept;
// Every challenge start (StartChallenge, any thread): whether this copy runs coop. A coop copy
// shows its coop objective ("all players complete the base challenge") on the objective list and
// finish screen even when no Coop button was pressed here (a guest's copy, or a leader's Solo).
void set_native_challenge_coop(bool coop) noexcept;
// Shared interpreter interception: the profile runtime already owns both VM hooks.
void execute_native_party_expression(std::uintptr_t vm, std::uint32_t pc, std::uintptr_t profiler,
                                    void (*runner)(std::uintptr_t, std::uint32_t, std::uintptr_t));
// Runs at the same verified client/camera phase as native debug camera controls.
void tick_native_party_actions(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase) noexcept;
} // namespace dingosdk::multiplayer
