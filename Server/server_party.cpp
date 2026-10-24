#include "server_host.h"
#include "server_text.h"

// Parties on a dedicated server. The server owns them (PartyBook): players ask through the
// game's Social menu or the ReSkate menus (Packet::party) or in chat (/party), and every
// roster tells everyone who is in which party.
namespace dingosdk::server {
namespace {
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
} // namespace

void Host::send_party(Guest &to, PartyAction action, std::uint64_t player) {
    auto notice = packet(PacketKind::party, now_);
    notice.party_action = action;
    notice.party_player = player;
    send_packet(to, notice, true, false);
}
void Host::party_notice(std::uint32_t party, std::string_view text, std::uint64_t except) {
    const auto *details = parties_.party(party);
    if (!details) return;
    for (const auto member : details->members)
        if (member != except)
            if (auto *guest = find(member); guest && guest->handshaken) send_chat(text, guest);
}
std::string Host::party_status(std::uint64_t id) const {
    const auto name = [&](std::uint64_t player) {
        const auto found = guests_.find(player);
        return found == guests_.end() ? std::to_string(player) : guest_name(*found->second);
    };
    if (id) {
        const auto *details = parties_.party(parties_.party_of(id));
        if (!details) return "You're not in a party. Invite someone with /party invite <player>.";
        std::string text = "Your party (" + std::to_string(details->members.size()) + "/" +
                           std::to_string(parties_.limit()) + (details->open ? ", open" : ", invite-only") + "):";
        for (const auto member : details->members)
            text += (member == details->members.front() ? " " : ", ") + name(member) + (member == details->leader ? " (leader)" : "");
        return text;
    }
    if (parties_.parties().empty()) return "No parties.";
    std::string text;
    for (const auto &[party, details] : parties_.parties()) {
        text += "Party " + std::to_string(party) + (details.open ? " (open):" : ":");
        for (const auto member : details.members)
            text += " " + name(member) + (member == details.leader ? "*" : "");
        text += "\n";
    }
    text.pop_back();
    return text;
}

void Host::party_request(Guest &guest, PartyAction action, std::uint64_t player) {
    if (!config_.parties) return reply(guest, "Parties are off on this server.");
    const auto me = guest.member.id;
    const auto my_name = guest_name(guest);
    Guest *other{};
    if (player) {
        other = find(player);
        if (!other || !other->handshaken) return reply(guest, "That player is not on the server.");
    }
    const auto their_name = other ? guest_name(*other) : std::string{};
    // Not into one party with a player a plugin hides from them, or from whom they are hidden
    // (plugin_hide): their games would have a party member they cannot see.
    if (other && (action == PartyAction::invite || action == PartyAction::accept || action == PartyAction::join) && !hidden_.empty()) {
        const auto with = [&](std::uint64_t who) {
            std::vector<std::uint64_t> group{who};
            if (const auto *details = parties_.party(parties_.party_of(who))) group.assign(details->members.begin(), details->members.end());
            return group;
        };
        for (const auto a : with(me))
            for (const auto b : with(player))
                if (hidden_between(a, b)) return reply(guest, "That party is not open to you right now.");
    }
    using R = PartyBook::Result;
    auto result = R::ok;
    switch (action) {
    case PartyAction::invite:
        result = parties_.invite(me, player, now_);
        if (result == R::ok) {
            send_party(*other, PartyAction::invited, me);
            reply(guest, "Invited " + their_name + " to your party.");
            log_("[party] " + my_name + " invited " + their_name);
        }
        break;
    case PartyAction::accept:
    case PartyAction::join: {
        const auto before = parties_.party_of(me);
        result = action == PartyAction::accept ? parties_.accept(me, player, now_) : parties_.join(me, player, now_);
        if (result == R::ok) {
            const auto party = parties_.party_of(me);
            if (before && before != party) party_notice(before, my_name + " left the party.");
            party_notice(party, my_name + " joined the party.", me);
            reply(guest, "You joined " + their_name + "'s party.");
            log_("[party] " + my_name + " joined " + their_name + "'s party");
        } else if (result == R::closed) {
            // Ask the leader instead: they can invite.
            const auto *details = parties_.party(parties_.party_of(player));
            if (auto *leader = details ? find(details->leader) : nullptr) {
                send_chat(my_name + " would like to join your party. Invite them from their player card or with /party invite " +
                              my_name, leader);
                reply(guest, "That party is invite-only; its leader was asked to invite you.");
                return;
            }
        }
        break;
    }
    case PartyAction::decline:
        result = parties_.decline(me, player);
        if (result == R::ok) send_chat(my_name + " declined your party invite.", other);
        break;
    case PartyAction::leave: {
        const auto party = parties_.party_of(me);
        result = parties_.leave(me);
        if (result == R::ok) {
            party_notice(party, my_name + " left the party.");
            reply(guest, "You left the party.");
        }
        break;
    }
    case PartyAction::kick: {
        const auto party = parties_.party_of(me);
        result = parties_.kick(me, player);
        if (result == R::ok) {
            send_chat("You were removed from the party.", other);
            party_notice(party, their_name + " was removed from the party.");
        }
        break;
    }
    case PartyAction::promote:
        result = parties_.promote(me, player);
        if (result == R::ok) party_notice(parties_.party_of(me), their_name + " now leads the party.");
        break;
    case PartyAction::open:
    case PartyAction::close: {
        const auto *before = parties_.party(parties_.party_of(me));
        const bool was_open = before && before->open;
        result = parties_.set_open(me, action == PartyAction::open);
        if (result == R::ok && was_open != (action == PartyAction::open))
            party_notice(parties_.party_of(me), action == PartyAction::open ? "The party is open: anyone can join."
                                                                           : "The party is invite-only.");
        break;
    }
    case PartyAction::invited:
    case PartyAction::withdrawn: return; // the server's own notices
    }
    if (result != R::ok) reply(guest, refusal(result));
    if (parties_.revision() != party_revision_) roster_dirty_ = true;
}

void Host::party_left(std::uint64_t id, const std::string &name) {
    const auto party = parties_.party_of(id);
    parties_.remove(id);
    if (party) party_notice(party, name + " left the server.");
}

void Host::tick_parties() {
    for (const auto &lapsed : parties_.expire(now_)) {
        if (auto *to = find(lapsed.to)) send_party(*to, PartyAction::withdrawn, lapsed.from);
        if (auto *from = find(lapsed.from); from && from->handshaken)
            if (const auto *to = find(lapsed.to)) send_chat("Your party invite to " + guest_name(*to) + " expired.", from);
    }
    for (const auto &withdrawn : parties_.take_withdrawn())
        if (auto *to = find(withdrawn.to)) send_party(*to, PartyAction::withdrawn, withdrawn.from);
    if (parties_.revision() != party_revision_) {
        party_revision_ = parties_.revision();
        roster_dirty_ = true;
    }
}

void Host::party_command(Guest &guest, std::string_view line) {
    if (!config_.parties) return reply(guest, "Parties are off on this server.");
    const auto [first, rest] = split(line);
    const auto verb = lower(first);
    const auto target = [&]() -> std::uint64_t {
        auto *other = match_player(rest);
        if (!other) reply(guest, rest.empty() ? "Name a player." : "No single player matches \"" + std::string(rest) + "\".");
        return other ? other->member.id : 0;
    };
    // The newest invite this player holds (for /party accept and decline without a name).
    const auto newest_invite = [&]() -> std::uint64_t {
        std::uint64_t from{}, latest{};
        for (const auto &invite : parties_.invites())
            if (invite.to == guest.member.id && invite.expires >= latest) from = invite.from, latest = invite.expires;
        if (!from) reply(guest, "You have no party invites.");
        return from;
    };
    if (verb.empty() || verb == "status" || verb == "list") return reply(guest, party_status(guest.member.id));
    if (verb == "help" || verb == "?")
        return reply(guest, "/party invite|join|kick|promote <player>, /party accept|decline [player], /party leave, "
                            "/party open|close, /p <message> talks to your party");
    if (verb == "invite") { if (const auto id = target()) party_request(guest, PartyAction::invite, id); return; }
    if (verb == "join") { if (const auto id = target()) party_request(guest, PartyAction::join, id); return; }
    if (verb == "kick" || verb == "remove") { if (const auto id = target()) party_request(guest, PartyAction::kick, id); return; }
    if (verb == "promote" || verb == "lead" || verb == "leader") {
        if (const auto id = target()) party_request(guest, PartyAction::promote, id);
        return;
    }
    if (verb == "accept" || verb == "decline") {
        const auto id = rest.empty() ? newest_invite() : target();
        if (id) party_request(guest, verb == "accept" ? PartyAction::accept : PartyAction::decline, id);
        return;
    }
    if (verb == "leave") return party_request(guest, PartyAction::leave, 0);
    if (verb == "open" || verb == "close") return party_request(guest, verb == "open" ? PartyAction::open : PartyAction::close, 0);
    if (verb == "say" || verb == "chat") return party_chat(guest, rest);
    reply(guest, "Unknown party command. Type /party help.");
}

void Host::party_chat(Guest &guest, std::string_view text) {
    const auto party = parties_.party_of(guest.member.id);
    if (!party) return reply(guest, "You're not in a party.");
    const auto line = clean_chat_text(text);
    if (line.empty()) return reply(guest, "/p <message>");
    // Relayed as the sender's own chat line, only to the rest of their party.
    auto message = packet(PacketKind::chat, now_);
    message.source = guest.member.id;
    message.epoch = guest.member.epoch;
    message.text = clean_chat_text("[Party] " + line);
    log_("[party chat] " + guest_name(guest) + ": " + line);
    for (const auto member : parties_.party(party)->members)
        if (member != guest.member.id)
            if (auto *to = find(member); to && to->handshaken) send_packet(*to, message, true, false);
}
} // namespace dingosdk::server
