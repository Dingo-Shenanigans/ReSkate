#pragma once
#include "share_code.h"
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Who sees whose modded outfit with Skate Synchros (contrib/outfit-sharing.md). Codes are the only
// way in, Steam friends or not:
//   - a player's personal code: whoever enters it sees that player's outfit (one way);
//   - a group code: everyone who has it sees everyone else's who has it.
// Both ends opt in. A player sends its outfit only with "Share my outfit" on, and asks for others'
// only with "Show shared outfits" on; with either off nothing is sent or asked for, and other
// players' items it lacks are stand-ins as before. Blocking wins over everything.
namespace dingosdk::multiplayer::synchros {

inline constexpr std::size_t max_entered_codes = 64, max_groups = 32, max_group_name = 32;

struct Group {
    std::string name;     // the player's own label for it ("Sk8 Crew"); not sent anywhere
    std::string code;     // twelve symbols (read_code with CodeKind::group)
    bool created{};       // made on this PC: only here is "New group code" offered
    bool operator==(const Group&) const = default;
};

struct Settings {
    bool share{};                       // Share my outfit
    bool show{};                        // Show shared outfits
    std::string own_code;               // twelve symbols; made when sharing is first turned on
    // Players who proved they hold this player's personal code, by SteamID64. A new code does not
    // clear them; removing one from the list does.
    std::set<std::uint64_t> paired;
    std::vector<std::string> entered;   // personal codes others gave this player, twelve symbols each
    std::vector<Group> groups;
    std::set<std::uint64_t> blocked;    // neither sent to nor shown, whatever else says
};

// What one session knows of who proved a group code to this player: only for that session, so
// leaving a group (or a member's new code) takes effect the next time.
struct SessionGrants {
    std::uint64_t session{};
    std::set<std::uint64_t> by_group;
};

// Whether this player's outfit files may be sent to `viewer`.
inline bool may_send(const Settings& s, const SessionGrants& grants, std::uint64_t viewer) noexcept {
    if (!s.share || !viewer || s.blocked.contains(viewer)) return false;
    return s.paired.contains(viewer) || grants.by_group.contains(viewer);
}

// Whether this player asks `wearer` for its outfit. A code never says whose it is, so a player with
// any codes or groups asks everyone in the session (who answers only if one of them is theirs).
inline bool may_ask(const Settings& s, std::uint64_t wearer) noexcept {
    if (!s.show || !wearer || s.blocked.contains(wearer)) return false;
    return !s.entered.empty() || !s.groups.empty();
}

// The proofs a viewer sends `wearer`: one for each personal code it entered, one for each group.
struct Proofs {
    std::vector<Proof> personal, group;
};
inline Proofs proofs_for(const Settings& s, std::uint64_t session, std::uint64_t wearer, std::uint64_t viewer) {
    Proofs out;
    for (const auto& code : s.entered) out.personal.push_back(code_proof(code_key(code, CodeKind::personal), session, wearer, viewer));
    for (const auto& group : s.groups) out.group.push_back(code_proof(code_key(group.code, CodeKind::group), session, wearer, viewer));
    return out;
}

// The wearer's side of a request. A proof of its personal code pairs the viewer for good; a proof
// of a group code both hold lets the viewer in for this session. Whether either did.
inline bool accept_proofs(Settings& s, SessionGrants& grants, const Proofs& proofs, std::uint64_t session,
                          std::uint64_t wearer, std::uint64_t viewer) {
    if (!s.share || !viewer || s.blocked.contains(viewer)) return false;
    if (grants.session != session) grants = {session, {}};
    bool in = false;
    if (!s.own_code.empty()) {
        const auto own = code_key(s.own_code, CodeKind::personal);
        for (const auto& proof : proofs.personal)
            if (proof_matches(own, proof, session, wearer, viewer)) { s.paired.insert(viewer); in = true; break; }
    }
    for (const auto& group : s.groups) {
        const auto key = code_key(group.code, CodeKind::group);
        for (const auto& proof : proofs.group)
            if (proof_matches(key, proof, session, wearer, viewer)) { grants.by_group.insert(viewer); in = true; break; }
    }
    return in;
}

// Adding a personal code someone gave this player. Empty when added, otherwise why not.
inline std::string enter_code(Settings& s, std::string_view typed) {
    if (typed_kind(typed) == CodeKind::group) return "That is a group code. Join it as a group instead.";
    const auto symbols = read_code(typed, CodeKind::personal);
    if (!symbols) return "That is not a share code. Check it for a typo.";
    if (*symbols == s.own_code) return "That is your own code.";
    if (std::find(s.entered.begin(), s.entered.end(), *symbols) != s.entered.end()) return "You already have that code.";
    if (s.entered.size() >= max_entered_codes) return "You have the most codes there can be. Remove one first.";
    s.entered.push_back(*symbols);
    return {};
}

// A new personal code: nobody new can pair with the old one. Players already paired stay.
inline std::string renew_code(Settings& s) {
    const auto shown = make_code(CodeKind::personal);
    s.own_code = *read_code(shown, CodeKind::personal);
    return shown;
}

// Making a group: its code, as shown, for the player to give out. Empty name: "My group".
inline std::string create_group(Settings& s, std::string name) {
    if (s.groups.size() >= max_groups) return {};
    if (name.empty()) name = "My group";
    if (name.size() > max_group_name) name.resize(max_group_name);
    const auto shown = make_code(CodeKind::group);
    s.groups.push_back({std::move(name), *read_code(shown, CodeKind::group), true});
    return shown;
}

// Joining a group someone gave the code of. Empty when joined, otherwise why not.
inline std::string join_group(Settings& s, std::string_view typed, std::string name) {
    if (typed_kind(typed) == CodeKind::personal) return "That is a personal code. Add it as a code instead.";
    const auto symbols = read_code(typed, CodeKind::group);
    if (!symbols) return "That is not a group code. Check it for a typo.";
    for (const auto& group : s.groups)
        if (group.code == *symbols) return "You are already in that group (" + group.name + ").";
    if (s.groups.size() >= max_groups) return "You are in the most groups there can be. Leave one first.";
    if (name.empty()) name = "Group";
    if (name.size() > max_group_name) name.resize(max_group_name);
    s.groups.push_back({std::move(name), *symbols, false});
    return {};
}

// A new code for a group this player made, to give to everyone still meant to be in it: whoever
// keeps only the old one no longer matches anyone who moved to the new. Nothing when this PC did
// not make the group (without a server that is a courtesy, not a lock: anyone with the code could
// make a group of their own with it).
inline std::string renew_group_code(Settings& s, std::size_t index) {
    if (index >= s.groups.size() || !s.groups[index].created) return {};
    const auto shown = make_code(CodeKind::group);
    s.groups[index].code = *read_code(shown, CodeKind::group);
    return shown;
}

inline void leave_group(Settings& s, std::size_t index) {
    if (index < s.groups.size()) s.groups.erase(s.groups.begin() + static_cast<std::ptrdiff_t>(index));
}

// Blocking a player stops their outfit at once, both ways, and forgets their pairing.
inline void block(Settings& s, SessionGrants& grants, std::uint64_t player) {
    s.blocked.insert(player);
    s.paired.erase(player);
    grants.by_group.erase(player);
}
} // namespace dingosdk::multiplayer::synchros
