#include "session_internal.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include <algorithm>

// Parties. Whoever hosts owns them, in a PartyBook: a dedicated server (Server/server_party.cpp)
// or, in a lobby, the host's game (the second half of this file, which answers requests the way
// the server does). Nobody is in a party until they form one, and every roster says who is in
// which. The native party UI, map markers and coop challenges read them from here.
namespace dingosdk::multiplayer::session_detail {
namespace {
// An invite the host passed on lapses there after a minute; drop ours a little later.
constexpr std::uint64_t invite_lifetime_us = 65000000;
std::string_view refusal(PartyBook::Result result) {
    switch (result) {
    case PartyBook::Result::ok: return {};
    case PartyBook::Result::self: return "That's you.";
    case PartyBook::Result::same_party: return "You're already in a party together.";
    case PartyBook::Result::full: return "That party is full.";
    case PartyBook::Result::not_leader: return "Only the party leader can do that.";
    case PartyBook::Result::not_member: return "You're not in a party with them.";
    case PartyBook::Result::no_invite: return "That invite has expired.";
    case PartyBook::Result::closed: return "That party is invite-only.";
    case PartyBook::Result::no_party: return "They're not in a party.";
    case PartyBook::Result::busy: return "They already have too many party invites.";
    case PartyBook::Result::renewed: return "They already have your party invite; it stays open for another minute.";
    }
    return "That can't be done.";
}
std::string player_name(Session &s, std::uint64_t id) {
    if (const auto *peer = find_peer(s, id); peer && !peer->member.name.empty()) return peer->member.name;
    return s.transport.name(id);
}
// The host itself, or a guest it has admitted.
bool in_lobby(Session &s, std::uint64_t id) {
    if (id == s.transport.status().local_id) return true;
    const auto *peer = find_peer(s, id);
    return peer && peer->handshaken;
}
// A line only `to` reads: in the host's own chat, or sent to the guest as the host's notice.
void tell(Session &s, std::uint64_t to, std::string_view text, std::uint64_t now) {
    if (to == s.transport.status().local_id) return add_chat(s, 0, "ReSkate", std::string(text));
    auto notice = packet(s, PacketKind::admin, now);
    notice.text = clean_chat_text(text);
    send_packet(s, to, notice, true, false);
}
// An invite for `to`, or word that one can no longer be accepted.
void notify(Session &s, std::uint64_t to, PartyAction action, std::uint64_t player, std::uint64_t now) {
    auto notice = packet(s, PacketKind::party, now);
    notice.party_action = action;
    notice.party_player = player;
    if (to == s.transport.status().local_id) receive_party(s, notice, now);
    else send_packet(s, to, notice, true, false);
}
void party_notice(Session &s, std::uint32_t party, std::string_view text, std::uint64_t now, std::uint64_t except = 0) {
    const auto *details = s.parties.party(party);
    if (!details) return;
    for (const auto member : details->members)
        if (member != except && in_lobby(s, member)) tell(s, member, text, now);
}
} // namespace

std::uint32_t party_of(const Session &s, std::uint64_t id) {
    if (!id) return 0;
    if (id == s.transport.status().local_id) return s.local_party;
    for (const auto &peer : active_peers(s))
        if (peer.handshaken && peer.member.id == id) return peer.member.party;
    return 0;
}
bool party_member(const Session &s, std::uint64_t id) {
    return s.local_party && id != s.transport.status().local_id && party_of(s, id) == s.local_party;
}
std::uint64_t party_leader(const Session &s) {
    if (!s.local_party) return 0;
    if (s.local_party_leader) return s.transport.status().local_id;
    for (const auto &peer : active_peers(s))
        if (peer.handshaken && peer.member.party == s.local_party && peer.member.party_leader) return peer.member.id;
    return 0;
}
void set_local_party(Session &s, const Member &local) {
    const bool joined = local.party && local.party != s.local_party;
    if (local.party != s.local_party || local.party_leader != s.local_party_leader || local.party_open != s.local_party_open)
        ++s.party_revision;
    s.local_party = local.party;
    s.local_party_leader = local.party_leader;
    s.local_party_open = local.party_open;
    // Invites into the party we are now in are moot.
    if (joined)
        std::erase_if(s.party_invites, [&](const auto &invite) { return party_of(s, invite.from) == local.party; });
}
void receive_party(Session &s, const Packet &p, std::uint64_t now) {
    const auto from = p.party_player;
    auto *inviter = find_peer(s, from);
    const auto name = inviter && !inviter->member.name.empty() ? inviter->member.name : s.transport.name(from);
    if (p.party_action == PartyAction::invited && ignored(s, from)) return; // (an ignored player's invite is dropped unseen)
    if (p.party_action == PartyAction::invited) {
        std::erase_if(s.party_invites, [&](const auto &invite) { return invite.from == from; });
        if (s.party_invites.size() >= 8) s.party_invites.erase(s.party_invites.begin());
        s.party_invites.push_back({from, now});
        ++s.party_revision;
        // The game's own invite toast (Accept / Decline) when it can show one; chat either way.
        const bool toast = post_native_party_invite(from);
        add_chat(s, 0, "ReSkate", name + " invited you to their party. " +
                 (toast ? "Accept it from the notification, the Multiplayer menu or /party accept."
                        : "Accept it in the Multiplayer menu or type /party accept."));
    } else if (p.party_action == PartyAction::withdrawn) {
        const auto before = s.party_invites.size();
        std::erase_if(s.party_invites, [&](const auto &invite) { return invite.from == from; });
        if (s.party_invites.size() != before) ++s.party_revision;
    }
    publish(s);
}
void expire_party_invites(Session &s, std::uint64_t now) {
    const auto before = s.party_invites.size();
    std::erase_if(s.party_invites, [&](const auto &invite) {
        return now < invite.received || now - invite.received > invite_lifetime_us || !find_peer(s, invite.from);
    });
    if (s.party_invites.size() != before) ++s.party_revision;
}
std::string send_party_request(Session &s, PartyAction action, std::uint64_t player) {
    if (s.mode != Mode::host && s.mode != Mode::join) return "Parties need a multiplayer session.";
    if (!valid_party_request(action, player) || action == PartyAction::invited || action == PartyAction::withdrawn)
        return "That party request isn't valid.";
    if (player && !find_peer(s, player)) return "That player is not in the session.";
    const auto now = now_us();
    if (s.mode == Mode::host) {
        host_party_request(s, s.transport.status().local_id, action, player, now);
        return {};
    }
    auto request = packet(s, PacketKind::party, now);
    request.party_action = action;
    request.party_player = player;
    if (!send_packet(s, s.host_id, request, true, false))
        return dedicated_host(s) ? "Could not reach the server." : "Could not reach the host.";
    return {};
}

// ---------------------------------------------------------------------------
// A lobby's host. The same answers, word for word, as a dedicated server gives
// (Server/server_party.cpp), so a party works the same wherever it is formed.
void host_party_request(Session &s, std::uint64_t me, PartyAction action, std::uint64_t player, std::uint64_t now) {
    if (s.mode != Mode::host || !in_lobby(s, me)) return;
    if (player && !in_lobby(s, player)) return tell(s, me, "That player is not in the lobby.", now);
    const auto my_name = player_name(s, me);
    const auto their_name = player ? player_name(s, player) : std::string{};
    auto &parties = s.parties;
    using R = PartyBook::Result;
    auto result = R::ok;
    switch (action) {
    case PartyAction::invite:
        result = parties.invite(me, player, now);
        if (result == R::ok) {
            notify(s, player, PartyAction::invited, me, now);
            tell(s, me, "Invited " + their_name + " to your party.", now);
        }
        break;
    case PartyAction::accept:
    case PartyAction::join: {
        const auto before = parties.party_of(me);
        result = action == PartyAction::accept ? parties.accept(me, player, now) : parties.join(me, player, now);
        if (result == R::ok) {
            const auto party = parties.party_of(me);
            if (before && before != party) party_notice(s, before, my_name + " left the party.", now);
            party_notice(s, party, my_name + " joined the party.", now, me);
            tell(s, me, "You joined " + their_name + "'s party.", now);
        } else if (result == R::closed) {
            // Ask the leader instead: they can invite.
            const auto *details = parties.party(parties.party_of(player));
            if (details && in_lobby(s, details->leader)) {
                tell(s, details->leader, my_name + " would like to join your party. Invite them from their player card "
                                         "or with /party invite " + my_name, now);
                return tell(s, me, "That party is invite-only; its leader was asked to invite you.", now);
            }
        }
        break;
    }
    case PartyAction::decline:
        result = parties.decline(me, player);
        if (result == R::ok) tell(s, player, my_name + " declined your party invite.", now);
        break;
    case PartyAction::leave: {
        const auto party = parties.party_of(me);
        result = parties.leave(me);
        if (result == R::ok) {
            party_notice(s, party, my_name + " left the party.", now);
            tell(s, me, "You left the party.", now);
        }
        break;
    }
    case PartyAction::kick: {
        const auto party = parties.party_of(me);
        result = parties.kick(me, player);
        if (result == R::ok) {
            tell(s, player, "You were removed from the party.", now);
            party_notice(s, party, their_name + " was removed from the party.", now);
        }
        break;
    }
    case PartyAction::promote:
        result = parties.promote(me, player);
        if (result == R::ok) party_notice(s, parties.party_of(me), their_name + " now leads the party.", now);
        break;
    case PartyAction::open:
    case PartyAction::close: {
        const auto *before = parties.party(parties.party_of(me));
        const bool was_open = before && before->open;
        result = parties.set_open(me, action == PartyAction::open);
        if (result == R::ok && was_open != (action == PartyAction::open))
            party_notice(s, parties.party_of(me), action == PartyAction::open ? "The party is open: anyone can join."
                                                                             : "The party is invite-only.", now);
        break;
    }
    case PartyAction::invited:
    case PartyAction::withdrawn: return; // the host's own notices
    }
    if (result != R::ok) tell(s, me, refusal(result), now);
    if (parties.revision() != s.parties_revision) s.roster_dirty = true;
}
void host_party_chat(Session &s, std::uint64_t from, std::string_view text, std::uint64_t now) {
    if (s.mode != Mode::host) return;
    const auto party = s.parties.party_of(from);
    if (!party) return tell(s, from, "You're not in a party.", now);
    const auto line = clean_chat_text(text);
    if (line.empty()) return tell(s, from, "/p <message>", now);
    // Relayed as the sender's own chat line, only to the rest of their party.
    const auto local = s.transport.status().local_id;
    auto message = packet(s, PacketKind::chat, now);
    message.text = clean_chat_text("[Party] " + line);
    if (from != local) {
        const auto *sender = find_peer(s, from);
        if (!sender) return;
        message.source = from;
        message.epoch = sender->member.epoch;
    }
    for (const auto member : s.parties.party(party)->members) {
        if (member == from) continue;
        if (member == local) add_chat(s, from, player_name(s, from), message.text);
        else send_packet(s, member, message, true, false);
    }
}
void tick_host_parties(Session &s, std::uint64_t now) {
    if (s.mode != Mode::host) return;
    auto &parties = s.parties;
    // A player who left the lobby is out of their party, and their invites go with them.
    std::vector<std::uint64_t> gone;
    const auto check = [&](std::uint64_t id) {
        if (!in_lobby(s, id) && std::find(gone.begin(), gone.end(), id) == gone.end()) gone.push_back(id);
    };
    for (const auto &[id, details] : parties.parties())
        for (const auto member : details.members) check(member);
    for (const auto &invite : parties.invites()) { check(invite.from); check(invite.to); }
    for (const auto id : gone) {
        const auto party = parties.party_of(id);
        const auto name = player_name(s, id);
        parties.remove(id);
        if (party) party_notice(s, party, name + " left the lobby.", now);
    }
    for (const auto &lapsed : parties.expire(now)) {
        if (in_lobby(s, lapsed.to)) notify(s, lapsed.to, PartyAction::withdrawn, lapsed.from, now);
        if (in_lobby(s, lapsed.from) && in_lobby(s, lapsed.to))
            tell(s, lapsed.from, "Your party invite to " + player_name(s, lapsed.to) + " expired.", now);
    }
    for (const auto &withdrawn : parties.take_withdrawn())
        if (in_lobby(s, withdrawn.to)) notify(s, withdrawn.to, PartyAction::withdrawn, withdrawn.from, now);
    if (parties.revision() != s.parties_revision) s.roster_dirty = true;
}
void fill_roster_parties(Session &s, std::vector<Member> &members) {
    for (auto &m : members) {
        const auto party = s.parties.party_of(m.id);
        const auto *details = s.parties.party(party);
        m.party = party;
        m.party_leader = details && details->leader == m.id;
        m.party_open = m.party_leader && details->open;
    }
    // The book holds only players in the lobby (tick_host_parties), so this only guards the
    // roster's rules: every listed party has two members and one leader.
    for (auto &m : members)
        if (m.party && std::count_if(members.begin(), members.end(), [&](const Member &o) { return o.party == m.party; }) < 2)
            m.party = 0, m.party_leader = m.party_open = false;
    for (auto &m : members)
        if (m.party && std::none_of(members.begin(), members.end(),
                                    [&](const Member &o) { return o.party == m.party && o.party_leader; }))
            m.party_leader = true; // the first listed member of a party missing its leader
    s.parties_revision = s.parties.revision();
}
} // namespace dingosdk::multiplayer::session_detail
