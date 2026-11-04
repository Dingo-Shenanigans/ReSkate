#pragma once
#include "share_code.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Who sees whose modded outfit with Skate Synchros (contrib/outfit-sharing.md). Both ends opt in:
// a wearer sends its outfit only to people it chose, and a viewer asks only people it chose. With
// either switch off nothing is sent or asked for, and other players' items it lacks are stand-ins.
namespace dingosdk::multiplayer::synchros {

struct Settings {
    // Wearer side.
    bool share{};                 // Share my outfit
    bool share_with_friends{true};// ...with Steam friends (codes work either way)
    std::string own_code;         // twelve symbols (read_code); empty until share is first turned on
    // Players who proved they hold this player's code, by SteamID64. New code does not clear them;
    // removing one from the list does.
    std::set<std::uint64_t> paired;

    // Viewer side.
    bool show{};                  // Show shared outfits
    std::vector<std::string> entered; // codes entered, twelve symbols each
    // Neither sent to nor shown, whatever else says.
    std::set<std::uint64_t> blocked;
};

// What this PC knows about one other player in the session.
struct Peer {
    std::uint64_t steam_id{};
    bool friend_{};               // a Steam friend of this player (k_EFriendRelationshipFriend)
};

// Whether this player's outfit files may be sent to `peer` when it asks. A peer proves a code
// before it is paired (accept_proof); that is the only way in for someone who is not a friend.
inline bool may_send(const Settings& s, const Peer& peer) noexcept {
    if (!s.share || !peer.steam_id || s.blocked.contains(peer.steam_id)) return false;
    return (s.share_with_friends && peer.friend_) || s.paired.contains(peer.steam_id);
}

// Whether this player asks `peer` for its outfit. A friend is asked (they decide whether to
// answer); someone who is not is asked only when this player entered a code, and then with the
// proofs of those codes (proofs_for).
inline bool may_ask(const Settings& s, const Peer& peer) noexcept {
    if (!s.show || !peer.steam_id || s.blocked.contains(peer.steam_id)) return false;
    return peer.friend_ || !s.entered.empty();
}

// The proofs this viewer sends `wearer`: one for each code it entered. The wearer finds its own
// among them, or none. Entering a code never says whose it is, so every one is offered.
inline std::vector<Proof> proofs_for(const Settings& s, std::uint64_t session, std::uint64_t wearer, std::uint64_t viewer) {
    std::vector<Proof> out;
    for (const auto& code : s.entered) out.push_back(code_proof(code_key(code), session, wearer, viewer));
    return out;
}

// The wearer's side of a request: whether one of the viewer's proofs is of this player's current
// code. When it is, the viewer is remembered as paired (and may_send lets it in from then on).
inline bool accept_proof(Settings& s, const std::vector<Proof>& proofs, std::uint64_t session,
                         std::uint64_t wearer, std::uint64_t viewer) {
    if (!s.share || s.own_code.empty() || !viewer || s.blocked.contains(viewer)) return false;
    const auto own = code_key(s.own_code);
    for (const auto& proof : proofs)
        if (proof_matches(own, proof, session, wearer, viewer)) {
            s.paired.insert(viewer);
            return true;
        }
    return false;
}

// At most this many codes are kept, and offered per request.
inline constexpr std::size_t max_entered_codes = 64;

// Adding a code someone gave this player. Empty when added, otherwise why not.
inline std::string enter_code(Settings& s, std::string_view typed) {
    const auto symbols = read_code(typed);
    if (!symbols) return "That is not a share code. Check it for a typo.";
    if (*symbols == s.own_code) return "That is your own code.";
    if (std::find(s.entered.begin(), s.entered.end(), *symbols) != s.entered.end()) return "You already have that code.";
    if (s.entered.size() >= max_entered_codes) return "You have the most codes there can be. Remove one first.";
    s.entered.push_back(*symbols);
    return {};
}

// A new code for this player: nobody new can pair with the old one. Players already paired stay.
inline std::string renew_code(Settings& s) {
    const auto shown = make_code();
    s.own_code = *read_code(shown);
    return shown;
}

// Blocking a player stops their outfit at once and forgets their pairing.
inline void block(Settings& s, std::uint64_t player) {
    s.blocked.insert(player);
    s.paired.erase(player);
}
} // namespace dingosdk::multiplayer::synchros
