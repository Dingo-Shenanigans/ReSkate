#include "session_internal.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Engine/Game/Build/supported_build.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace dingosdk::multiplayer {
using namespace session_detail;
namespace {
Peer &reserve(Session &s, std::uint64_t id, std::uint64_t now) {
    if (auto *p = find_peer(s, id))
        return *p;
    for (std::size_t slot = 0; slot < s.peers.size(); ++slot)
        if (auto &p = s.peers[slot]; !p.member.id) {
            p.member.id = id;
            p.last_packet = now;
            note_slot(s, slot);
            return p;
        }
    throw std::runtime_error("Session has no free player slots.");
}
void defer_cosmetics(Peer &link, const Packet &packet, std::uint64_t now) {
    auto &pending = link.pending_cosmetics;
    std::erase_if(pending, [&](const auto &item) { return now - item.received > 10000000; });
    const auto found = std::find_if(pending.begin(), pending.end(), [&](const auto &item) {
        return item.packet.source == packet.source && item.packet.epoch == packet.epoch;
    });
    if (found != pending.end()) {
        if (newer_sequence(packet.sequence, found->packet.sequence)) *found = {packet, now};
    } else {
        if (pending.size() >= max_players) pending.erase(pending.begin());
        pending.push_back({packet, now});
    }
}
void receive_cosmetics(Session &s, const NativeFrame &local, std::uint64_t now) {
    for (auto &link : active_peers(s)) {
        auto pending = std::exchange(link.pending_cosmetics, {});
        const bool direct = s.mode == Mode::join && link.member.id != s.host_id;
        for (auto &item : pending) {
            const auto &p = item.packet;
            if (p.world != s.world || now - item.received > 10000000) continue;
            auto *source = find_peer(s, p.source);
            // Lanes may deliver an outfit before welcome/roster/readiness. Keep
            // the bounded decoded value until those independent messages arrive;
            // never apply it before the normal identity/world/admission checks.
            if (!link.handshaken || !local.ready || s.awaiting_map || !s.host_world_ready ||
                !s.started_map || (s.mode == Mode::host && !link.world_ready) ||
                (direct && !link.direct_ready) || !source || !source->handshaken ||
                source->member.epoch != p.epoch) {
                link.pending_cosmetics.push_back(std::move(item));
                continue;
            }
            if (p.map == s.map &&
                routed_source(p, source->member, link.member.id, s.mode == Mode::host, s.host_id, direct) &&
                accept_data(*source, p, now) && s.mode == Mode::host)
                broadcast(s, p, true, false, now, p.source);
        }
    }
}
// Chat proofs. A guest takes chat from the host alone, which passes every other guest's lines
// on, and a host can pass on what it likes under any of its players' names. That is no worse
// than a name on a line, until the line shows a badge of the backend's (Dev, Creator, Homie):
// then it is a line a developer never wrote, with their badge on it. So such a guest also
// sends each line straight to the players Steam connects them to (send_chat), and here a line
// the host passed on as theirs shows their badge only when that copy says the same. One
// without it waits a moment for it, then shows as any other player's would: from an older
// build, a party line (which the host writes), or not theirs at all.
constexpr std::uint64_t chat_proof_wait = 500000, chat_proof_life = 10000000;
constexpr std::size_t chat_proofs_kept = 8;
// Whether a line the host passed on as this player's would show a badge that rests on who
// they are: another guest's, known to Steam, on one of the backend's lists and showing it.
bool owes_proof(const Session &s, const Peer &sender) {
    // Their own copy only comes over a direct connection: without one there is none to wait for.
    return s.mode == Mode::join && sender.member.id != s.host_id && sender.direct_ready && shows_tag(sender) &&
           identity_mark(sender.member.id);
}
std::string chat_name(Session &s, const Peer &sender) {
    return sender.member.name.empty() ? s.transport.name(sender.member.id) : sender.member.name;
}
// Shows the lines that have waited long enough (or all of them), without the badge.
void release_chat(Session &s, Peer &sender, std::uint64_t now, bool all) {
    auto &waiting = sender.chat_waiting;
    auto line = waiting.begin();
    for (; line != waiting.end() && (all || now < line->at || now - line->at >= chat_proof_wait); ++line)
        add_chat(s, sender.member.id, chat_name(s, sender), std::move(line->text), false, false);
    waiting.erase(waiting.begin(), line);
}
// The host's copy of a line: shown at once when the sender's own copy is here, else it waits.
void relayed_chat(Session &s, Peer &sender, const Packet &p, std::uint64_t now) {
    auto &proofs = sender.chat_proofs;
    const auto proof = std::find_if(proofs.begin(), proofs.end(), [&](const auto &copy) {
        return copy.sequence == p.sequence && copy.text == p.text && now >= copy.at && now - copy.at <= chat_proof_life;
    });
    const bool proven = proof != proofs.end();
    if (proven) proofs.erase(proof);
    // The sender's copies come in the order of their lines: with this one's here, or none
    // owed for it, nothing is on its way for the lines before it.
    if (proven || p.text.starts_with("[Party] ")) {
        release_chat(s, sender, now, true);
        add_chat(s, sender.member.id, chat_name(s, sender), p.text, false, proven);
        return;
    }
    if (sender.chat_waiting.size() >= chat_proofs_kept) release_chat(s, sender, now, true);
    sender.chat_waiting.push_back({p.sequence, p.text, now});
}
// The sender's own copy of a line, over their own connection. Never shown by itself.
void proven_chat(Session &s, Peer &sender, const Packet &p, std::uint64_t now) {
    if (!owes_proof(s, sender)) return;
    auto &waiting = sender.chat_waiting;
    const auto held = std::find_if(waiting.begin(), waiting.end(), [&](const auto &line) {
        return line.sequence == p.sequence && line.text == p.text;
    });
    if (held != waiting.end()) {
        for (auto line = waiting.begin(); line != held; ++line)
            add_chat(s, sender.member.id, chat_name(s, sender), std::move(line->text), false, false);
        add_chat(s, sender.member.id, chat_name(s, sender), std::move(held->text));
        waiting.erase(waiting.begin(), held + 1);
        return;
    }
    auto &proofs = sender.chat_proofs;
    std::erase_if(proofs, [&](const auto &copy) { return now < copy.at || now - copy.at > chat_proof_life; });
    if (proofs.size() >= chat_proofs_kept) proofs.erase(proofs.begin());
    proofs.push_back({p.sequence, p.text, now});
}
// A hitch leaves a backlog of pose updates, all of which would be decoded in one frame.
// Every pose delta references a reliable baseline, never another delta, so older deltas
// of one stream in a batch can be dropped undecoded. Keep the newest ones playback may
// still interpolate between (its delay plus a frame each side) among those whose baseline
// is already here; baselines and every other kind are always read.
std::vector<bool> stale_pose_deltas(Session &s, const std::vector<TransportMessage> &messages) {
    std::vector<bool> stale(messages.size());
    struct Stream {
        std::uint64_t link{}, source{};
        unsigned kept{}, keep{};
    };
    std::vector<Stream> streams;
    for (auto i = messages.size(); i-- > 0;) {
        const auto &bytes = messages[i].bytes;
        // Sparse pose patches (delta_codec.cpp): "RMS1"/"RMS2", source at 4, kind at 20.
        if (bytes.size() <= 34 || bytes[0] != 'R' || bytes[1] != 'M' || bytes[2] != 'S' ||
            (bytes[3] != '1' && bytes[3] != '2'))
            continue;
        std::uint64_t source{};
        for (unsigned b = 0; b < 8; ++b)
            source |= std::uint64_t{bytes[4 + b]} << (8 * b);
        if ((bytes[20] | (unsigned{bytes[21]} << 8)) != static_cast<unsigned>(PacketKind::pose))
            continue;
        // One whose baseline has not arrived (it may be in this batch) is left to decode
        // or fail as before, and does not use up the count.
        std::uint64_t epoch{};
        std::uint32_t reference{};
        for (unsigned b = 0; b < 8; ++b)
            epoch |= std::uint64_t{bytes[12 + b]} << (8 * b);
        for (unsigned b = 0; b < 4; ++b)
            reference |= std::uint32_t{bytes[22 + b]} << (8 * b);
        const auto *link = find_peer(s, messages[i].peer);
        if (!link || !link->receiver.holds(source, PacketKind::pose, epoch, reference))
            continue;
        auto stream = std::find_if(streams.begin(), streams.end(),
                                   [&](const auto &v) { return v.link == messages[i].peer && v.source == source; });
        if (stream == streams.end()) {
            const auto *peer = find_peer(s, source);
            const std::uint32_t interval =
                peer && peer->received_pose_interval ? peer->received_pose_interval : 50000;
            const auto delay = std::max<std::uint32_t>(100000, interval + 50000);
            streams.push_back({messages[i].peer, source, 0, std::clamp<unsigned>(delay / interval + 2, 2, 16)});
            stream = std::prev(streams.end());
        }
        if (stream->kept >= stream->keep)
            stale[i] = true;
        else
            ++stream->kept;
    }
    return stale;
}
} // namespace
namespace session_detail {
void apply_roster(Session &s, const Packet &p, std::uint64_t now) {
    auto *host = find_peer(s, s.host_id);
    if (!host || !host->handshaken ||
        !trusted_roster(p, s.host_id, host->member.epoch, s.transport.status().local_id, s.epoch))
        throw std::runtime_error("Host supplied an invalid player roster.");
    if (s.roster_sequence && !newer_sequence(p.sequence, s.roster_sequence))
        return;
    s.roster_sequence = p.sequence;
    s.server_admin = false;
    for (const auto &m : p.members)
        if (m.id == s.transport.status().local_id) {
            s.server_admin = m.admin && game_server_steam_id(s.host_id);
            set_local_party(s, m);
            s.local_speeding = m.speeding && game_server_steam_id(s.host_id);
            if (m.scoring && !s.local_scoring) add_chat(s, 0, "ReSkate", own_scoring_notice());
            s.local_scoring = m.scoring;
        }
    if (s.voice_policy != p.voice_policy) s.voice.reset();
    s.voice_policy = p.voice_policy;
    if (s.tps != p.tps) {
        s.tps = p.tps;
        s.next_send = 0;
        for (auto &peer : active_peers(s)) peer.pose_delivery = {};
    }
    apply_object_placement(s, p.object_placement);
    apply_object_limit(s, p.object_limit); // after server_admin, which exempts an admin
    apply_guest_tools(s, p.guest_noclip, p.guest_no_bail, p.guest_boosts);
    s.enforce_tuning = p.enforce_tuning;
    s.server_votes = dedicated_host(s) ? p.server_votes : 0;
    publish_chat(s); // the "/" list follows the server's votes, its players and its maps
    if (s.object_clears && *s.object_clears != p.object_clears) s.clear_pending = true;
    s.object_clears = p.object_clears;
    s.force_world_layers = p.force_world_layers;
    s.layers = unpack_world_layers(p.layers);
    apply_host_world_layers(s.force_world_layers, s.layers);
    apply_distances(s, p.distances);
    apply_host_park_choices(p.parks);
    MemberSlots old{};
    for (std::size_t i = 0; i < old.size(); ++i)
        old[i] = s.peers[i].member;
    const auto next = roster_slots(old, p.members, s.transport.status().local_id);
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (old[i].id != next[i].id || old[i].epoch != next[i].epoch) {
            if (old[i].id && old[i].id != s.host_id)
                s.transport.disconnect(old[i].id, "Player left or rejoined the host roster.");
            release_chat(s, s.peers[i], now, true); // what they said just before leaving
            reset_peer(s, i);
            s.peers[i].last_packet = now;
        }
        // Someone the host just flagged (or who arrives flagged) for mods that change scoring or physics.
        if (next[i].id && next[i].scoring && !(old[i].id == next[i].id && old[i].scoring)) {
            const auto name = next[i].name.empty() ? s.transport.name(next[i].id) : next[i].name;
            add_chat(s, 0, "ReSkate", name + "'s mods change scoring or physics, so they are out of throwdowns and coop challenges.");
        }
        s.peers[i].member = next[i];
        s.peers[i].handshaken = next[i].id != 0;
        if (next[i].id) note_slot(s, i);
    }
    s.capacity = p.capacity;
    s.roster_voice_range = p.voice_range;
    ++s.party_revision; // anyone's party may have changed
    // A dedicated server knows players only by the name each sent in their hello.
    for (auto &peer : active_peers(s))
        if (peer.member.id && peer.member.name.empty() && individual_steam_id(peer.member.id))
            peer.member.name = s.transport.name(peer.member.id);
    s.transport.allow_peers(p.members);
    s.last_routes = 0;
    s.status = "Connected through Steam. Network updates: " + std::to_string(s.tps) + " TPS.";
}
bool accept_data(Peer &peer, const Packet &p, std::uint64_t now) {
    bool accepted{};
    if (p.kind == PacketKind::cosmetics) {
        accepted = peer.outfit_budget.accept(now) && peer.appearance.push(p);
        if (accepted) {
            peer.cosmetic_packet = encode_wire(p);
            ++peer.cosmetic_revision;
        }
    } else if (p.kind == PacketKind::audio)
        accepted = peer.sound_budget.accept(now, p.audio.size()) && peer.audio.push(p, now);
    else if (p.kind == PacketKind::pose)
        accepted = peer.poses.push_validated(p, now);
    if (accepted) {
        peer.last_packet = now;
        if (p.kind == PacketKind::pose) {
            peer.pose_arrival = now;
            peer.latest_root = p.pose.root;
            peer.player_collision = p.player_collision;
            peer.received_pose_interval = p.pose_interval_us;
            ++peer.pose_count;
        }
    }
    return accepted;
}
bool accept_data(Peer &peer, Packet &&p, std::uint64_t now) {
    if (p.kind != PacketKind::pose)
        return accept_data(peer, std::as_const(p), now);
    const auto root = p.pose.root;
    const auto interval = p.pose_interval_us;
    const bool collision = p.player_collision;
    if (!peer.poses.push_validated(std::move(p), now))
        return false;
    peer.player_collision = collision;
    peer.last_packet = now;
    peer.pose_arrival = now;
    peer.latest_root = root;
    peer.received_pose_interval = interval;
    ++peer.pose_count;
    return true;
}
void networking(Session &s, const NativeFrame &local, std::uint64_t now) {
    s.network_now = now;
    s.join_backoff.prune(now);
    trim_slots(s);
    if (world_playing(s, local)) s.local_root = local.pose.root;
    for (auto &peer : active_peers(s))
        if (!peer.chat_waiting.empty()) release_chat(s, peer, now, false);
    s.transport.poll();
    if (s.mode == Mode::join && s.transport.status().telemetry) {
        const auto &t = s.transport.status();
        const auto peers = active_peers(s);
        const auto active =
            static_cast<unsigned>(std::count_if(peers.begin(), peers.end(), [&](const auto &p) {
                return p.member.id != s.host_id && p.direct_ready;
            }));
        s.direct_upload.update(now, t.queue_us, t.skipped, active);
    }
    const auto links = s.transport.status().peers;
    const auto physical = [&](std::uint64_t id) {
        return std::find_if(links.begin(), links.end(), [id](const auto &v) { return v.id == id; });
    };
    for (auto &p : active_peers(s)) {
        const auto id = p.member.id;
        if (!id)
            continue;
        const bool direct = s.mode == Mode::join && id != s.host_id;
        if (physical(id) == links.end()) {
            if (!direct) {
                disconnect(s, id,
                           s.mode == Mode::join ? s.transport.status().detail : "A player disconnected.");
                if (s.mode == Mode::off)
                    return;
            } else {
                if (p.connected_at || p.direct_ready)
                    reset_direct(s, p, now);
                if (p.handshaken && now >= p.next_dial &&
                    dial_peer(s.transport.status().local_id, id, s.host_id)) {
                    p.next_dial = now + 3000000;
                    s.transport.connect_peer(id);
                }
            }
        }
    }
    if (s.mode == Mode::join && physical(s.host_id) == links.end()) {
        stop(s, s.transport.status().detail);
        return;
    }
    // The backend's bans (reskate_banned) arrive while a session runs: a banned player stops
    // hosting theirs, and nobody stays with a banned host. A banned guest is for the host or
    // the server to turn away (below, and Host::tick), which a server may choose not to.
    if (s.mode == Mode::host && reskate_banned(s.transport.status().local_id)) {
        stop(s, std::string(banned_notice));
        return;
    }
    if (s.mode == Mode::join && reskate_banned(s.host_id)) {
        stop(s, "This host is banned from ReSkate multiplayer.");
        return;
    }
    for (const auto &link : links) {
        if (s.mode == Mode::host && s.banned.contains(link.id)) {
            s.transport.disconnect(link.id, "You were kicked from this session.");
            continue;
        }
        if (s.mode == Mode::host && is_banned(s, link.id)) {
            s.transport.disconnect(link.id, "You are banned from this host's lobbies.");
            continue;
        }
        if (s.mode == Mode::host && reskate_banned(link.id)) {
            s.transport.disconnect(link.id, banned_notice.data());
            continue;
        }
        auto *p = find_peer(s, link.id);
        if (!p && s.mode == Mode::host && s.join_backoff.waiting(link.id, now)) {
            s.transport.disconnect(link.id, "Too many failed attempts to join. Wait a little and try again.");
            continue;
        }
        if (!p && s.mode == Mode::host)
            p = &reserve(s, link.id, now);
        if (!p) {
            // The initial host is reserved here; all other identities need a roster.
            if (s.mode == Mode::join && link.id == s.host_id)
                p = &reserve(s, link.id, now);
            else {
                s.transport.disconnect(link.id, "Identity is not in the host roster.");
                continue;
            }
        }
        if (link.connected && !p->connected_at)
            p->connected_at = now;
        if (s.mode == Mode::join && link.id != s.host_id && world_playing(s, local) && link.connected && p->handshaken &&
            !p->direct_ready && now - p->last_direct_hello >= 1000000) {
            send_required(s, link.id, encode_wire(packet(s, PacketKind::peer_hello, now)));
            p->last_direct_hello = now;
        }
    }
    auto *host = find_peer(s, s.host_id);
    if (s.mode == Mode::join && host && host->connected_at && !host->handshaken &&
        (s.awaiting_map || !local.ready) && now - s.last_map_request >= 1000000) {
        auto request = packet(s, PacketKind::map_request, now);
        request.map = s.join_destination.empty() ? 0 : map_hash(s.join_destination);
        if (s.password && host->password_challenge && request.map) {
            request.challenge = host->password_challenge;
            request.proof = password_proof(*s.password, s.secret, request.map, s.host_id, request.source,
                                          host->member.epoch, s.epoch, request.challenge);
        }
        send_required(s, s.host_id, encode_wire(request));
        s.last_map_request = now;
    }
    if (s.mode == Mode::join && host && host->connected_at && !host->handshaken && local.ready &&
        !s.awaiting_map &&
        now - s.last_hello >= 1000000) {
        auto hello = packet(s, PacketKind::hello, now);
        // Our name, for a dedicated server that has no Steam friends list to look it up.
        hello.text = s.transport.name(hello.source);
        while (!hello.text.empty() && !valid_member_name(hello.text)) hello.text.pop_back();
        if (s.password && host->password_challenge) {
            hello.challenge = host->password_challenge;
            hello.proof = password_proof(*s.password, s.secret, s.map, s.host_id, hello.source,
                                         host->member.epoch, s.epoch, hello.challenge);
        }
        send_required(s, s.host_id, encode_wire(hello));
        s.last_hello = now;
    }
    const auto messages = s.transport.receive();
    const auto stale = stale_pose_deltas(s, messages);
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto &message = messages[index];
        auto *link = find_peer(s, message.peer);
        if (!link)
            continue;
        const bool direct_link = s.mode == Mode::join && message.peer != s.host_id;
        if (!link->budget.accept(message.arrived ? message.arrived : now, message.bytes.size(),
                                 s.mode == Mode::join && !direct_link ? max_remote_players : 1U)) {
            disconnect(s, message.peer, "Peer exceeded the multiplayer packet limit.");
            if (s.mode == Mode::off)
                return;
            continue;
        }
        if (stale[index])
            continue;
        bool missing_reference{};
        auto decoded = link->receiver.receive(message.bytes, missing_reference, s.world);
        if (missing_reference)
            continue;
        if (!decoded || decoded->session != s.secret ||
            ((s.mode == Mode::host || direct_link) && decoded->source != message.peer)) {
            disconnect(s, message.peer, "Join code, sender identity, or multiplayer protocol did not match.");
            if (s.mode == Mode::off)
                return;
            continue;
        }
        const auto &p = *decoded;
        // A dedicated server sends no poses; its roster and control traffic keep it alive.
        if (dedicated_host(s) && message.peer == s.host_id) link->last_packet = now;
        if (p.kind == PacketKind::world_state) {
            if (s.mode != Mode::join || message.peer != s.host_id || p.source != s.host_id ||
                !link->handshaken || p.epoch != link->member.epoch ||
                p.build != supported_build::game_sha256_bytes) {
                disconnect(s, message.peer, "Only the admitted host may change the room's map.");
                if (s.mode == Mode::off) return;
                continue;
            }
            if (p.world < s.world || (p.world == s.world && s.world_state_sequence &&
                                     !newer_sequence(p.sequence, s.world_state_sequence)))
                continue;
            if (p.world > s.world) {
                clear_world(s, now);
                s.world = p.world;
                s.travelling = s.awaiting_map = s.join_map_authorized = true;
                s.host_world_ready = false;
                s.travel_started = s.join_started = now;
                s.join_destination.clear();
                s.map_name.clear();
                s.map_label.clear();
                s.map = 0;
                s.map_load_submitted = false;
                s.last_map_load_check = 0;
            }
            if (!s.join_destination.empty() && !p.destination.empty() && s.map != p.map) {
                stop(s, "The host changed destinations without starting a new map transition.");
                return;
            }
            s.world_state_sequence = p.sequence;
            if (!p.destination.empty()) {
                s.join_destination = s.map_name = p.destination;
                s.map_label = p.map_label;
                s.map = p.map;
            }
            s.host_world_ready = p.world_ready;
            link->world_ready = p.world_ready;
            link->last_packet = now;
            if (s.travelling)
                s.status = p.world_ready ? "Host map ready. Waiting for local loading..."
                                        : "The host is changing maps. Your session stays connected.";
            continue;
        }
        if (p.kind == PacketKind::world_ready) {
            if (s.mode != Mode::host || !link->handshaken || p.source != message.peer ||
                p.epoch != link->member.epoch) {
                disconnect(s, message.peer, "Invalid map readiness message.");
                if (s.mode == Mode::off) return;
                continue;
            }
            if (p.world != s.world || p.map != s.map ||
                (link->ready_sequence && !newer_sequence(p.sequence, link->ready_sequence)))
                continue;
            link->ready_sequence = p.sequence;
            link->last_packet = now;
            const bool arrived = p.world_ready && !link->world_ready;
            link->world_ready = p.world_ready;
            if (p.world_ready) link->travel_since = 0;
            if (arrived) {
                send_required(s, message.peer, s.cosmetic_packet);
                for (const auto &other : active_peers(s))
                    if (other.handshaken && other.member.id != message.peer)
                        send_required(s, message.peer, other.cosmetic_packet);
            }
            continue;
        }
        if (p.kind == PacketKind::map_request) {
            if (s.mode != Mode::host || p.source != message.peer ||
                p.build != supported_build::game_sha256_bytes ||
                (link->member.epoch && link->member.epoch != p.epoch)) {
                disconnect(s, message.peer, "Invalid host map request. Update ReSkate and join again.");
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            if (link->handshaken || !local.ready || !valid_map_destination(s.map_name))
                continue;
            if (p.map && p.map != s.map) {
                disconnect(s, message.peer, "The host changed maps while you were joining. Join again.");
                continue;
            }
            link->member.epoch = p.epoch;
            bool authorized = !s.password;
            if (s.password) {
                if (!link->password_challenge)
                    link->password_challenge = nonce();
                if (p.challenge == link->password_challenge) {
                    const auto proof = password_proof(*s.password, s.secret, s.map, s.host_id, message.peer,
                                                      s.epoch, p.epoch, link->password_challenge);
                    if (!proof_matches(proof, p.proof)) {
                        disconnect(s, message.peer, "Incorrect lobby password.");
                        continue;
                    }
                    authorized = true;
                }
            }
            const bool newly_authorized = authorized && !link->map_authorized;
            link->map_authorized |= authorized;
            if (newly_authorized || !link->last_map_offer || now - link->last_map_offer >= 1000000) {
                auto offer = packet(s, PacketKind::map_offer, now);
                offer.destination = s.map_name;
                offer.challenge = link->password_challenge;
                offer.map_authorized = authorized;
                send_required(s, message.peer, encode_wire(offer));
                link->last_map_offer = now;
            }
            continue;
        }
        if (p.kind == PacketKind::map_offer) {
            if (link->handshaken || p.world < s.world) continue;
            if (s.mode != Mode::join || message.peer != s.host_id || p.source != s.host_id ||
                p.build != supported_build::game_sha256_bytes ||
                (link->member.epoch && p.epoch != link->member.epoch) ||
                (!s.join_destination.empty() && map_hash(s.join_destination) != p.map) ||
                (!p.map_authorized && !p.challenge)) {
                disconnect(s, message.peer, "Host map or session changed. Join again.");
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            if (link->handshaken)
                continue;
            if (p.challenge && !s.password) {
                stop(s, "This lobby requires a password. Use Join with password and try again.");
                return;
            }
            const bool new_challenge = link->password_challenge != p.challenge;
            link->member.epoch = p.epoch;
            link->password_challenge = p.challenge;
            link->map_authorized |= p.map_authorized;
            s.join_destination = p.destination;
            s.map_label = p.map_label;
            s.world = p.world;
            s.join_map_authorized |= p.map_authorized;
            if (new_challenge)
                s.last_map_request = 0;
            if (!s.join_map_authorized)
                s.status = "Checking lobby password before loading the host's map...";
            continue;
        }
        // A map reload can return to the same asset. The generation prevents
        // late poses, audio, cosmetics and control from reviving the old world.
        if (p.world != s.world) continue;
        if (p.kind == PacketKind::peer_hello || p.kind == PacketKind::peer_welcome) {
            if (!world_playing(s, local)) continue;
            if (!direct_link || !link->handshaken ||
                !greeting_error(p, s.secret, local.ready ? s.map : 0, supported_build::game_sha256_bytes,
                                link->member.epoch)
                     .empty()) {
                disconnect(s, message.peer, "Direct peer is not admitted to this session.");
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            const bool first = !link->direct_ready;
            link->direct_ready = true;
            link->world_ready = true;
            if (p.kind == PacketKind::peer_hello)
                send_required(s, message.peer, encode_wire(packet(s, PacketKind::peer_welcome, now)));
            if (first) {
                send_required(s, message.peer, s.cosmetic_packet);
                s.last_routes = 0;
            }
            continue;
        }
        if (p.kind == PacketKind::challenge) {
            if (s.mode != Mode::join || message.peer != s.host_id || p.source != s.host_id || !p.challenge ||
                !greeting_error(p, s.secret, local.ready ? s.map : 0, supported_build::game_sha256_bytes,
                                link->member.epoch)
                     .empty()) {
                disconnect(s, message.peer, "Invalid lobby password challenge.");
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            if (!s.password) {
                stop(s, "This lobby requires a password. Enter it in Join and try again.");
                return;
            }
            if (!link->handshaken) {
                link->member.epoch = p.epoch;
                link->password_challenge = p.challenge;
                s.last_hello = 0;
            }
            continue;
        }
        if (p.kind == PacketKind::hello || p.kind == PacketKind::welcome) {
            if (!world_playing(s, local)) continue;
            if (direct_link || p.source != message.peer ||
                (s.mode == Mode::host ? p.kind != PacketKind::hello : p.kind != PacketKind::welcome)) {
                disconnect(s, message.peer, "Unexpected multiplayer handshake direction.");
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            const auto error =
                greeting_error(p, s.secret, local.ready ? s.map : 0, supported_build::game_sha256_bytes,
                               link->handshaken ? link->member.epoch : 0);
            if (!error.empty()) {
                disconnect(s, message.peer, std::string(error));
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            if (s.mode == Mode::host && s.password && !link->handshaken) {
                if (!link->password_challenge)
                    link->password_challenge = nonce();
                if (p.challenge != link->password_challenge) {
                    auto challenge = packet(s, PacketKind::challenge, now);
                    challenge.challenge = link->password_challenge;
                    send_required(s, message.peer, encode_wire(challenge));
                    continue;
                }
                const auto proof = password_proof(*s.password, s.secret, s.map, s.host_id, message.peer,
                                                  s.epoch, p.epoch, link->password_challenge);
                if (!proof_matches(proof, p.proof)) {
                    disconnect(s, message.peer, "Incorrect lobby password.");
                    continue;
                }
            }
            const bool joined = !link->handshaken;
            link->member.epoch = p.epoch;
            link->handshaken = true;
            link->world_ready = true;
            link->travel_since = 0;
            link->last_packet = now;
            if (joined) s.join_backoff.joined(message.peer);
            if (s.mode == Mode::host) {
                send_required(s, message.peer, encode_wire(packet(s, PacketKind::welcome, now)));
                if (joined) {
                    send_roster(s, now);
                    if (!s.tuning_packet.empty()) send_required(s, message.peer, s.tuning_packet);
                    if (!s.extras_packet.empty()) send_required(s, message.peer, s.extras_packet);
                    send_required(s, message.peer, s.cosmetic_packet);
                    for (const auto &other : active_peers(s))
                        if (other.handshaken && other.member.id != message.peer)
                            send_required(s, message.peer, other.cosmetic_packet);
                }
            } else if (joined)
                send_required(s, message.peer, s.cosmetic_packet);
            s.status = "Connected through Steam. Network updates: " + std::to_string(s.tps) + " TPS.";
            continue;
        }
        if (p.kind == PacketKind::cosmetics) {
            defer_cosmetics(*link, p, now);
            continue;
        }
        // A guest's report of how its mods change trick scoring; read before the map check, so one
        // sent while the guest is still loading the host's map counts.
        if (p.kind == PacketKind::scoring) {
            if (s.mode == Mode::host && !direct_link && link->handshaken && p.source == message.peer &&
                p.epoch == link->member.epoch && link->scoring_budget.accept(now, 4)) {
                link->last_packet = now;
                link->scoring = p.scoring;
                link->scoring_mods = p.text;
                judge_scoring(s, *link);
            }
            continue;
        }
        if (!link->handshaken || p.map != s.map || (direct_link && !link->direct_ready))
            continue;
        if (p.kind == PacketKind::routes) {
            const bool valid =
                s.mode == Mode::host && p.source == message.peer && p.epoch == link->member.epoch;
            if (!valid) {
                disconnect(s, message.peer, "Invalid direct route report.");
                if (s.mode == Mode::off)
                    return;
                continue;
            }
            if (!link->route_reported || newer_sequence(p.sequence, link->route_sequence)) {
                link->direct_routes.clear();
                // Reports can cross a roster update during a departure/rejoin.
                // Discard stale entries, never disconnect healthy reporters.
                for (const auto &m : p.members) {
                    const auto *other = find_peer(s, m.id);
                    if (other && other->handshaken && other->member.epoch == m.epoch && m.id != message.peer)
                        link->direct_routes.push_back(m);
                }
                link->route_reported = now;
                link->route_sequence = p.sequence;
            }
            continue;
        }
        if (p.kind == PacketKind::roster) {
            if (s.mode != Mode::join || message.peer != s.host_id) {
                disconnect(s, message.peer, "Only the host may publish the player roster.");
                continue;
            }
            apply_roster(s, p, now);
            continue;
        }
        // Only the host (or dedicated server) moves players: tpall and tphere.
        if (p.kind == PacketKind::teleport) {
            if (s.mode == Mode::join && message.peer == s.host_id && p.source == s.host_id &&
                dingosdk::teleport_local_skater(p.teleport))
                add_chat(s, 0, "ReSkate", dedicated_host(s) ? "A server admin teleported you." : "The host teleported you.");
            continue;
        }
        // The host's physics tuning (Extension/Skater/physics_tuning.h), for its guests only.
        if (p.kind == PacketKind::physics_tuning) {
            if (s.mode == Mode::join && message.peer == s.host_id && p.source == s.host_id && !dedicated_host(s))
                s.host_tuning = std::move(p.tuning);
            continue;
        }
        // And the physics its tuning does not carry (Engine/Game/Multiplayer/session_physics.h).
        if (p.kind == PacketKind::physics_extras) {
            if (s.mode == Mode::join && message.peer == s.host_id && p.source == s.host_id && !dedicated_host(s))
                s.host_extras = p.extras;
            continue;
        }
        if (p.kind == PacketKind::maps) {
            if (dedicated_host(s) && message.peer == s.host_id && p.source == s.host_id) {
                s.server_maps = p.maps;
                s.server_map_pool.clear();
                for (const auto entry : p.map_pool) s.server_map_pool.push_back(p.maps[entry]);
                s.server_map_rotation = p.map_rotation;
                publish(s);
                publish_chat(s);
            }
            continue;
        }
        if (p.kind == PacketKind::bans) {
            if (dedicated_host(s) && message.peer == s.host_id && p.source == s.host_id) {
                s.server_bans = p.bans;
                s.server_ban_total = p.ban_total;
                publish(s);
            }
            continue;
        }
        // Whoever hosts telling us about a party invite; or, hosting a lobby, a guest asking
        // for something to be done with their party.
        if (p.kind == PacketKind::party) {
            if (s.mode == Mode::join) {
                if (message.peer == s.host_id && p.source == s.host_id) receive_party(s, p, now);
            } else if (s.mode == Mode::host) {
                auto *sender = find_peer(s, p.source);
                if (!direct_link && sender && sender->handshaken && message.peer == p.source &&
                    routed_source(p, sender->member, message.peer, true, s.host_id) && sender->party_budget.accept(now, 20))
                    host_party_request(s, p.source, p.party_action, p.party_player, now);
            }
            continue;
        }
        // A dedicated server's answer to one of our admin requests, or a lobby host's to a
        // party request: a line for this player only.
        if (p.kind == PacketKind::admin) {
            if (s.mode == Mode::join && message.peer == s.host_id && p.source == s.host_id)
                add_chat(s, 0, dedicated_host(s) ? "Server" : "ReSkate", p.text);
            continue;
        }
        // Chat is accepted while either side is still loading a map: it needs
        // only an authenticated sender, never the world.
        if (p.kind == PacketKind::chat) {
            auto *sender = find_peer(s, p.source);
            if (!sender || !sender->handshaken) continue;
            // Another guest's own copy of a line they said: proof of the host's, never a line.
            if (direct_link) {
                if (routed_source(p, sender->member, message.peer, false, s.host_id, true)) proven_chat(s, *sender, p, now);
                continue;
            }
            if (!routed_source(p, sender->member, message.peer, s.mode == Mode::host, s.host_id))
                continue;
            // A dedicated server speaks (welcome message, its console) as "Server".
            const bool server = dedicated_host(s) && sender->member.id == s.host_id;
            // Everyone holds everyone to the same pace, so a modified client cannot flood.
            if (!server && sender->chat_rate.accept(now, p.text, 1) != ChatRate::Verdict::accepted) continue;
            sender->last_packet = now;
            // "/p": party chat, which a lobby's host relays to the sender's party and nobody else.
            if (s.mode == Mode::host && p.text.starts_with("/p ")) {
                host_party_chat(s, sender->member.id, std::string_view(p.text).substr(3), now);
                continue;
            }
            if (owes_proof(s, *sender)) relayed_chat(s, *sender, p, now);
            else add_chat(s, sender->member.id, server ? std::string("Server") : chat_name(s, *sender), p.text);
            if (s.mode == Mode::host) broadcast(s, p, true, false, now, p.source);
            continue;
        }
        // Throwdown messages are relayed like chat, but only within one map: a drop
        // belongs to the world it was placed in.
        if (p.kind == PacketKind::throwdown) {
            auto *sender = find_peer(s, p.source);
            if (direct_link || !sender || !sender->handshaken || p.map != s.map || p.world != s.world ||
                !routed_source(p, sender->member, message.peer, s.mode == Mode::host, s.host_id))
                continue;
            if (!sender->throwdown_budget.accept(now, throwdown_burst)) continue;
            sender->last_packet = now;
            // A player whose mods change scoring or physics takes part in nothing linked: the host
            // passes none of theirs on.
            if (sender->member.scoring) continue;
            if (s.throwdown_inbox.size() < 256) s.throwdown_inbox.emplace_back(p.source, p.throwdown);
            if (s.mode == Mode::host) broadcast(s, p, true, false, now, p.source);
            continue;
        }
        // A host-ready notice and its fresh cosmetics can share one receive
        // batch. Accept them once our target skater is loaded, even though the
        // local transition is finalized on the next tick.
        if (!local.ready || s.awaiting_map || !s.host_world_ready || !s.started_map ||
            (s.mode == Mode::host && !link->world_ready)) continue;
        auto *source = find_peer(s, p.source);
        if (!source || !source->handshaken ||
            !routed_source(p, source->member, message.peer, s.mode == Mode::host, s.host_id, direct_link))
            continue;
        if (p.kind == PacketKind::away) {
            disconnect(s, message.peer, "A player ended their session.");
            if (s.mode == Mode::off)
                return;
            continue;
        }
        if (direct_link && p.kind == PacketKind::pose)
            link->last_direct_pose = now;
        if (p.kind == PacketKind::objects) {
            if (direct_link) { disconnect(s, message.peer, "Object updates must use the host route."); continue; }
            const auto result = source->objects.receive(p.objects);
            if (result == ObjectState::Result::invalid) {
                disconnect(s, message.peer, "Invalid shared object revision or layout.");
                if (s.mode == Mode::off) return;
            } else source->last_packet = now;
            continue;
        }
        if (p.kind == PacketKind::voice) {
            if (!s.voice_policy.accepts(p.voice) || (s.mode == Mode::join && !s.roster_sequence)) continue;
            if (direct_link || (source->received_voice && !newer_sequence(p.sequence, source->voice_sequence))) continue;
            if (!source->voice_budget.accept(now, p.voice.bytes.size())) continue;
            source->received_voice = true;
            source->voice_sequence = p.sequence;
            source->last_packet = now;
            s.voice.receive(p);
            if (s.mode == Mode::host) broadcast(s, p, false, true, now, p.source);
            continue;
        }
        // The host forwards what it accepts, so it keeps the packet; a guest moves the
        // pose into playback instead of copying it.
        if (s.mode != Mode::host)
            accept_data(*source, std::move(*decoded), now);
        else if (accept_data(*source, p, now))
            broadcast(s, p, p.kind == PacketKind::cosmetics ||
                (p.kind == PacketKind::audio && std::any_of(p.audio.begin(), p.audio.end(),
                    [](const auto &sample) { return sample.event; })),
                p.kind != PacketKind::cosmetics, now, p.source);
    }
    receive_cosmetics(s, local, now);
    for (std::size_t i = 0; i < s.used_slots; ++i) {
        const auto &p = s.peers[i];
        if (!p.member.id)
            continue;
        if (s.mode == Mode::join && p.member.id != s.host_id) {
            if (world_playing(s, local) && p.connected_at && !p.direct_ready && now - p.connected_at > 8000000)
                disconnect(s, p.member.id, "Direct handshake timed out; using host forwarding.");
            continue;
        }
        // Authorized arrivals get time for native loading; this has an absolute
        // deadline, so requests cannot keep an unused slot reserved indefinitely.
        const std::uint64_t handshake_timeout = p.map_authorized ? 180000000 : 8000000;
        if (p.handshaken && (s.travelling || p.travel_since)) {
            if (p.travel_since && now - p.travel_since > 180000000) {
                disconnect(s, p.member.id, "A player could not finish loading the new map.");
                if (s.mode == Mode::off) return;
            }
            continue;
        }
        if ((!p.handshaken && p.connected_at && now - p.connected_at > handshake_timeout) ||
            (p.handshaken && now - p.last_packet > 10000000)) {
            disconnect(s, p.member.id, "A player timed out while waiting for gameplay data.");
            if (s.mode == Mode::off)
                return;
        }
    }
    if (s.mode == Mode::join && world_playing(s, local) && now - s.last_routes >= 250000) {
        if (const auto *host_peer = find_peer(s, s.host_id); host_peer && host_peer->handshaken) {
            auto report = packet(s, PacketKind::routes, now);
            for (const auto &p : active_peers(s))
                if (p.member.id != s.host_id && p.direct_ready && p.last_direct_pose &&
                    now - p.last_direct_pose <= 500000)
                    report.members.push_back({p.member.id, p.member.epoch, {}});
            // A report can expire safely: failed/skipped reports leave relay enabled.
            if (send_packet(s, s.host_id, report, false, true))
                s.last_routes = now;
        }
    }
    update_scoring(s, now); // before the roster, so a host's own flag goes out with it
    tick_host_parties(s, now); // and the lobby's parties, which the roster carries
    if (s.mode == Mode::host && world_playing(s, local) && (s.roster_dirty || now - s.last_roster > 2000000))
        send_roster(s, now);
    const auto peers = active_peers(s);
    if (s.mode == Mode::host && (s.travelling || !s.last_world_state ||
        std::any_of(peers.begin(), peers.end(), [](const auto &peer) { return peer.handshaken && !peer.world_ready; })) &&
        (!s.last_world_state || now - s.last_world_state >= 1000000) && (s.travelling || s.world > 1))
        send_world_state(s, now);
    if (s.mode == Mode::join && s.world > 1 && (s.travelling || !s.last_world_ready) &&
        (!s.last_world_ready || now - s.last_world_ready >= 1000000)) {
        if (const auto *host_peer = find_peer(s, s.host_id); host_peer && host_peer->handshaken) {
            auto ready = packet(s, PacketKind::world_ready, now);
            ready.world_ready = local.ready && !s.awaiting_map;
            send_required(s, s.host_id, encode_wire(ready));
            s.last_world_ready = now;
        }
    }
    if (s.public_host)
        s.lobbies.update_host(world_playing(s, local), static_cast<unsigned>(s.transport.status().peers.size()) + 1,
                              s.map_name, now);
    if (s.mode == Mode::host && !s.travelling && s.travel_started &&
        std::none_of(peers.begin(), peers.end(), [](const auto &peer) { return peer.handshaken && !peer.world_ready; })) {
        s.travel_started = 0;
        s.status = "Map changed. All connected players are ready.";
    }
}
} // namespace session_detail
} // namespace dingosdk::multiplayer
