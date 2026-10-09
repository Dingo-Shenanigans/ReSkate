#include "session_internal.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_audio.h"
#include "Extension/Multiplayer/Remote/native_vfx.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/physics_tuning.h"
#include "Engine/Vfs/mod_scoring.h"
#include "Extension/Multiplayer/Remote/remote_collision.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dingosdk::multiplayer {
using namespace session_detail;
namespace {
// Guests upload their layouts to the host, which alone decides what everyone
// else sees. Unless guests may place objects, each shared layout stays frozen,
// so a patched client cannot place, move or delete objects for anyone else.
// Uploads are still accepted meanwhile, keeping the owner's revisions in step.
void publish_guest_objects(Session &s) {
    if (s.mode != Mode::host || s.object_placement != ObjectPlacement::everyone) return;
    for (auto &peer : active_peers(s))
        if (peer.handshaken && peer.objects.revision() != peer.shared_from) {
            auto layout = peer.objects.layout();
            std::erase_if(layout, [&](const auto &object) { return peer.cleared.contains(object.id); });
            // The host's limit on each guest's objects, which a patched client cannot place past.
            layout = limited_layout(std::move(layout), peer.shared.objects(), s.object_limit);
            peer.shared.replace(layout);
            peer.shared_from = peer.objects.revision();
        }
}
// The objects a receiver should render or forward for this owner.
const ObjectState &visible_objects(const Session &s, const Peer &peer) {
    return s.mode == Mode::host ? peer.shared : peer.objects;
}
void send_object_states(Session &s, std::uint64_t now) {
    struct Source { std::uint64_t id, epoch; const ObjectState *state; };
    std::vector<Source> sources{{s.transport.status().local_id, s.epoch, &s.local_objects}};
    if (s.mode == Mode::host)
        for (const auto &peer : active_peers(s))
            if (peer.handshaken && peer.world_ready) sources.push_back({peer.member.id, peer.member.epoch, &peer.shared});
    for (auto &peer : active_peers(s)) {
        if (!peer.handshaken || !peer.world_ready || (s.mode == Mode::join && peer.member.id != s.host_id)) continue;
        auto &delivery = peer.object_delivery;
        const auto find = [&](std::uint64_t id, std::uint64_t epoch) {
            return std::find_if(sources.begin(), sources.end(), [&](const auto &source) { return source.id == id && source.epoch == epoch; });
        };
        std::erase_if(delivery.sent, [&](const auto &row) { return find(row.first, row.second.first) == sources.end(); });
        if (!delivery.chunks.empty() && find(delivery.source, delivery.epoch) == sources.end()) delivery.chunks.clear();
        // Bounded reliable chunks share the ordered control lane with admission,
        // roster and travel. A newly admitted owner always precedes its objects.
        for (unsigned budget = 0; budget < 4; ++budget) {
            if (delivery.chunks.empty()) {
                for (std::size_t attempt = 0; attempt < sources.size(); ++attempt) {
                    const auto &source = sources[delivery.cursor++ % sources.size()];
                    if (source.id == peer.member.id || !source.state->revision()) continue;
                    const auto previous = delivery.sent.find(source.id);
                    const auto since = previous != delivery.sent.end() && previous->second.first == source.epoch ? previous->second.second : 0;
                    delivery.chunks = source.state->updates(since);
                    if (delivery.chunks.empty()) continue;
                    delivery.source = source.id; delivery.epoch = source.epoch; delivery.next = 0;
                    break;
                }
                if (delivery.chunks.empty()) break;
            }
            auto update = packet(s, PacketKind::objects, now);
            update.source = delivery.source; update.epoch = delivery.epoch;
            update.objects = delivery.chunks[delivery.next];
            if (!send_packet(s, peer.member.id, update, true, false)) {
                s.transport.disconnect(peer.member.id, "Cannot deliver shared object state. Join again.");
                break;
            }
            if (++delivery.next == delivery.chunks.size()) {
                delivery.sent[delivery.source] = {delivery.epoch, update.objects.revision};
                delivery.chunks.clear();
            }
        }
    }
}
} // namespace
namespace session_detail {
Packet packet(Session &s, PacketKind kind, std::uint64_t now) {
    Packet p;
    p.kind = kind;
    p.sequence = ++s.sequence;
    p.session = s.secret;
    p.epoch = s.epoch;
    p.map = s.map;
    p.time_us = now;
    p.source = s.transport.status().local_id;
    p.world = s.world;
    p.tps = s.tps;
    p.pose_interval_us = multiplayer_pose_interval(s.tps);
    p.build = supported_build::game_sha256_bytes;
    return p;
}
void reset_direct(Session &s, Peer &p, std::uint64_t now) {
    p.sender = {};
    p.receiver = {};
    p.budget = {};
    p.pose_delivery = {};
    p.direct_ready = false;
    p.connected_at = p.last_direct_hello = p.last_direct_pose = 0;
    p.next_dial = now + 3000000;
    s.last_routes = 0;
}
void disconnect(Session &s, std::uint64_t id, const std::string &reason) {
    if (s.mode == Mode::join) {
        if (id == s.host_id)
            stop(s, reason);
        else {
            s.transport.disconnect(id, reason.c_str());
            if (auto *p = find_peer(s, id))
                reset_direct(s, *p, s.network_now);
        }
        return;
    }
    s.transport.disconnect(id, reason.c_str());
    for (std::size_t i = 0; i < s.used_slots; ++i)
        if (s.peers[i].member.id == id) {
            s.roster_dirty |= s.peers[i].handshaken;
            // Never admitted: it held a player slot meanwhile. An ID that keeps failing waits
            // longer each time before its connection is taken again.
            if (s.mode == Mode::host && !s.peers[i].handshaken) {
                const auto failures = s.join_backoff.failed(id, s.network_now);
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Multiplayer: {} did not finish joining ({}), attempt {}.", id, reason, failures);
            }
            reset_peer(s, i);
            break;
        }
    s.status = reason;
}
bool send_packet(Session &s, std::uint64_t id, const Packet &p, bool reliable, bool fresh,
                 std::span<const std::uint8_t> raw, std::span<const std::uint8_t> wire) {
    auto *peer = find_peer(s, id);
    if (!peer)
        return false;
    auto update = raw.empty() ? peer->sender.prepare(p) : peer->sender.prepare(p, raw, wire);
    // A packet that cannot be built for anyone is its source's fault, never this recipient's:
    // nothing is sent, and the recipient is not treated as unreachable.
    if (update.bytes.empty())
        return true;
    // A stream and its reliable delta references must always use the same lane.
    if (!s.transport.send(id, update.bytes, reliable || update.establishes_baseline(), fresh, traffic_lane(p.kind)))
        return false;
    peer->sender.sent(p, std::move(update));
    return true;
}
void send_required(Session &s, std::uint64_t id, const std::vector<std::uint8_t> &bytes) {
    if (bytes.empty())
        return;
    const auto p = decode_wire(bytes);
    if (!p || !send_packet(s, id, *p, true, false))
        s.transport.disconnect(id, "Cannot deliver required session data. Join again.");
}
void send_world_state(Session &s, std::uint64_t now) {
    auto state = packet(s, PacketKind::world_state, now);
    state.destination = s.map_name;
    state.world_ready = s.host_world_ready;
    const auto bytes = encode_wire(state);
    for (const auto &peer : active_peers(s))
        if (peer.handshaken) send_required(s, peer.member.id, bytes);
    s.last_world_state = now;
}
void begin_host_world(Session &s, std::string_view destination, std::uint64_t now) {
    if (!destination.empty() && !valid_map_destination(destination))
        throw std::runtime_error("The new host map is not a supported native destination.");
    clear_world(s, now);
    if (++s.world == 0) throw std::runtime_error("Map transition counter exhausted.");
    s.map_name = destination;
    s.map = destination.empty() ? 0 : map_hash(destination);
    s.travelling = true;
    s.host_world_ready = false;
    s.travel_started = now;
    s.status = "Changing maps. Players remain connected while loading...";
    send_world_state(s, now);
    if (s.public_host)
        s.lobbies.update_host(false, static_cast<unsigned>(s.transport.status().peers.size()) + 1,
                              s.map_name, now);
}
std::string send_chat(Session &s, std::string_view typed) {
    if (s.mode != Mode::host && s.mode != Mode::join) return "Chat needs a multiplayer session.";
    auto text = clean_chat_text(typed);
    if (text.empty()) return "Type a message first.";
    const auto now = now_us();
    switch (s.local_chat_rate.accept(now, text)) {
    case ChatRate::Verdict::repeated: return "You just said that.";
    case ChatRate::Verdict::too_fast: return "Slow down: one message every couple of seconds.";
    case ChatRate::Verdict::accepted: break;
    }
    auto message = packet(s, PacketKind::chat, now);
    message.text = text;
    broadcast(s, message, true, false, now);
    const auto local = s.transport.status().local_id;
    // The host passes a guest's line on, and could pass on anything under anyone's name. So a
    // guest whose line shows a badge of the backend's also sends it straight to every player
    // Steam connects them to: the copy is not shown, it is what lets that player know the
    // host's one is ours (chat proofs, session_receive.cpp). Older builds ignore it.
    if (s.mode == Mode::join && own_tag_shown() && identity_mark(local))
        for (const auto &peer : active_peers(s))
            if (peer.handshaken && peer.direct_ready && peer.member.id != s.host_id)
                send_packet(s, peer.member.id, message, true, false);
    add_chat(s, local, s.transport.name(local), std::move(text), true);
    return {};
}
std::string send_chat_command(Session &s, std::string_view typed) {
    if (!dedicated_host(s)) return "That command needs a dedicated server.";
    auto text = clean_chat_text(typed);
    if (text.size() < 2 || text.front() != '/') return "Type a command after the /.";
    const auto now = now_us();
    switch (s.local_chat_rate.accept(now, text)) {
    case ChatRate::Verdict::repeated: return "You just sent that.";
    case ChatRate::Verdict::too_fast: return "Slow down: one message every couple of seconds.";
    case ChatRate::Verdict::accepted: break;
    }
    auto message = packet(s, PacketKind::chat, now);
    message.text = std::move(text);
    if (!send_packet(s, s.host_id, message, true, false)) return "Could not reach the server.";
    // Party chat: the server relays it to the rest of the party, not back to us.
    const std::string_view sent = message.text;
    // An answer to the vote typed in chat shows on the vote card like one given there.
    if (s.vote.id && s.vote.outcome == vote_running) {
        if (s.vote.kind == server_vote_poll) {
            // "/2" or "/vote 2"
            const auto number = sent.starts_with("/vote ") ? sent.substr(6) : sent.substr(1);
            if (number.size() == 1 && number[0] >= '1' && static_cast<std::size_t>(number[0] - '0') <= s.vote.answers.size())
                s.vote_mine = static_cast<std::uint8_t>(number[0] - '0');
        } else if (sent == "/yes" || sent == "/y" || sent == "/vote yes" || sent == "/vote y") s.vote_mine = 1;
        else if (sent == "/no" || sent == "/n" || sent == "/vote no" || sent == "/vote n") s.vote_mine = 2;
    }
    if (sent.starts_with("/p ") && s.local_party) {
        const auto local = s.transport.status().local_id;
        add_chat(s, local, s.transport.name(local), "[Party] " + std::string(sent.substr(3)), true);
    }
    return {};
}
std::string answer_server_poll(Session &s, std::size_t answer) {
    if (!dedicated_host(s) || !s.vote.id || s.vote.outcome != vote_running || s.vote.kind != server_vote_poll)
        return "No poll is running.";
    if (answer >= s.vote.answers.size()) return "The poll has " + std::to_string(s.vote.answers.size()) + " answers.";
    if (s.vote_mine == answer + 1) return {};
    // Sent as the chat command, as an answer to a vote is.
    auto message = packet(s, PacketKind::chat, now_us());
    message.text = "/" + std::to_string(answer + 1);
    if (!send_packet(s, s.host_id, message, true, false)) return "Could not reach the server.";
    s.vote_mine = static_cast<std::uint8_t>(answer + 1);
    return {};
}
std::string cast_server_vote(Session &s, bool yes) {
    if (!dedicated_host(s) || !s.vote.id || s.vote.outcome != vote_running) return "No vote is running.";
    if (s.vote.kind == server_vote_poll) return "This is a poll: answer it on its card, or with /1, /2...";
    if (s.vote.target == s.transport.status().local_id) return "You cannot vote on your own kick.";
    if (s.vote_mine == (yes ? 1 : 2)) return {};
    // The server takes the answer as the chat command (server_votes.cpp); it is not a chat
    // line, so the pace kept for those does not hold it back.
    auto message = packet(s, PacketKind::chat, now_us());
    message.text = yes ? "/yes" : "/no";
    if (!send_packet(s, s.host_id, message, true, false)) return "Could not reach the server.";
    s.vote_mine = yes ? 1 : 2;
    return {};
}
std::string send_party_chat(Session &s, std::string_view typed) {
    if (s.mode != Mode::host && s.mode != Mode::join) return "Chat needs a multiplayer session.";
    if (!s.local_party) return "You're not in a party.";
    const auto text = clean_chat_text(typed);
    if (text.empty()) return "Type a message first.";
    const auto now = now_us();
    switch (s.local_chat_rate.accept(now, text)) {
    case ChatRate::Verdict::repeated: return "You just said that.";
    case ChatRate::Verdict::too_fast: return "Slow down: one message every couple of seconds.";
    case ChatRate::Verdict::accepted: break;
    }
    const auto local = s.transport.status().local_id;
    if (s.mode == Mode::host) {
        host_party_chat(s, local, text, now);
    } else {
        // The host relays it to the rest of the party, not back to us.
        auto message = packet(s, PacketKind::chat, now);
        message.text = clean_chat_text("/p " + text);
        if (!send_packet(s, s.host_id, message, true, false)) return "Could not reach the host.";
    }
    add_chat(s, local, s.transport.name(local), "[Party] " + text, true);
    return {};
}
void send_throwdown(Session &s, std::vector<std::uint8_t> message) {
    if ((s.mode != Mode::host && s.mode != Mode::join) || message.empty() || message.size() > max_throwdown_message) return;
    const auto now = now_us();
    auto p = packet(s, PacketKind::throwdown, now);
    p.throwdown = std::move(message);
    broadcast(s, p, true, false, now);
}
// This game's pose to a dedicated server: its differences from a pose of its own the server has
// said it holds (pose_batch.h), as the server sends everyone else's.
void upload_pose(Session &s, const Packet &packet, std::uint64_t now) {
    auto &kept = s.own_poses;
    if (!kept.empty() && kept.back().sequence >= packet.sequence) return;
    kept.push_back({packet.sequence, packet.time_us, pose_codec::quantize(packet.pose)});
    while (kept.size() > 64) kept.pop_front();
    const auto find = [&](std::uint32_t sequence) -> std::optional<pose_batch::KeptView> {
        for (auto it = kept.rbegin(); it != kept.rend(); ++it)
            if (it->sequence == sequence) return pose_batch::KeptView{it->sequence, it->time_us, &it->pose};
        return {};
    };
    const auto emit = [&](std::span<const std::uint8_t> message, bool reliable) {
        return s.transport.send(s.host_id, message, reliable, !reliable, TrafficLane::gameplay);
    };
    s.pose_upload.begin(s.world, s.map);
    s.pose_upload.add(packet.source, packet.epoch, *find(packet.sequence), find, false, packet.player_collision, pose_batch::Rate::full, now, emit);
    s.pose_upload.flush(emit);
}
void broadcast(Session &s, const Packet &packet, bool reliable, bool fresh, std::uint64_t now,
               std::uint64_t except) {
    const auto *source = find_peer(s, packet.source);
    // One encoding per pose rate (full, 100 ms, 200 ms), each of the same packet with only
    // the interval it carries changed. Recipients holding the same delta reference share one
    // patch (DeltaCache).
    struct Encoded {
        bool ready{};
        std::vector<std::uint8_t> raw, wire;
        DeltaCache deltas;
    };
    std::array<Encoded, 3> encoded;
    // Every recipient's update is built first and all go to Steam in one call. The delta
    // references a DeltaCache points into stay valid until the senders record the sends.
    struct Outgoing {
        Peer *peer{};
        WireUpdate update;
        PoseDelivery *delivery{};
        std::uint32_t interval{};
    };
    std::vector<Outgoing> outgoing;
    unsigned direct_sent{};
    const bool gameplay = packet.kind == PacketKind::pose || packet.kind == PacketKind::audio ||
                          packet.kind == PacketKind::voice || packet.kind == PacketKind::cosmetics ||
                          packet.kind == PacketKind::effects;
    for (auto &p : active_peers(s)) {
        if (!p.handshaken || p.member.id == except ||
            (s.mode == Mode::host && gameplay && !p.world_ready) ||
            (s.mode == Mode::join && p.member.id != s.host_id && (!p.direct_ready || dedicated_host(s))))
            continue;
        // Chat and throwdown messages always travel through the host, which relays
        // them once to everyone else; a direct copy as well would deliver them twice.
        if ((packet.kind == PacketKind::chat || packet.kind == PacketKind::throwdown || packet.kind == PacketKind::effects) && s.mode == Mode::join &&
            p.member.id != s.host_id)
            continue;
        if (packet.kind == PacketKind::voice) {
            if (!s.voice_policy.accepts(packet.voice) || (s.mode == Mode::join && !s.roster_sequence)) continue;
            if (s.mode == Mode::join && p.member.id != s.host_id) continue;
            // Each listener fades voices by their own hearing distance, so the host
            // forwards proximity voice to anyone within its voice range (a host
            // setting). Unknown positions are forwarded rather than cut.
            if (s.mode == Mode::host && packet.voice.distance > 0.f) {
                const auto *root = source ? (source->latest_root ? &*source->latest_root : nullptr)
                                          : (s.local_root ? &*s.local_root : nullptr);
                const bool known = root && p.latest_root && p.pose_arrival && now - p.pose_arrival <= 3000000 &&
                    (!source || (source->pose_arrival && now - source->pose_arrival <= 3000000));
                if (known && voice_gain(root->position, p.latest_root->position, s.voice_range) <= 0.f) continue;
            }
        }
        if (s.mode == Mode::join && p.member.id != s.host_id &&
            (packet.kind == PacketKind::pose || packet.kind == PacketKind::audio) &&
            direct_sent++ >= s.direct_upload.limit)
            continue;
        // Keep admission/roster/cosmetics host-controlled. Only frequent gameplay
        // streams move off the host after the destination confirms direct poses.
        if (s.mode == Mode::host && source &&
            (packet.kind == PacketKind::pose || packet.kind == PacketKind::audio) &&
            !needs_relay(p.direct_routes, p.route_reported, source->member, now))
            continue;
        if (packet.kind == PacketKind::pose && dedicated_host(s) && p.member.id == s.host_id) {
            upload_pose(s, packet, now);
            continue;
        }
        // This game's skater's sound to a dedicated server (sound_codec.h): what changed since
        // the last samples it was sent.
        if (packet.kind == PacketKind::audio && dedicated_host(s) && p.member.id == s.host_id) {
            s.sound_upload.begin(s.world, s.map);
            s.sound_upload.add(packet.source, packet.epoch, packet.sequence, packet.time_us, packet.audio, now);
            if (s.sound_upload.pending())
                s.sound_upload.sent(s.transport.send(s.host_id, s.sound_upload.message(), true, fresh, traffic_lane(packet.kind)));
            continue;
        }
        PoseDelivery *delivery{};
        std::uint32_t interval = multiplayer_pose_interval(s.tps);
        if (packet.kind == PacketKind::pose) {
            auto entry = std::find_if(p.pose_delivery.begin(), p.pose_delivery.end(),
                [&](const auto &v) { return v.source == packet.source; });
            if (entry == p.pose_delivery.end())
                entry = p.pose_delivery.size() < max_players
                    ? p.pose_delivery.insert(p.pose_delivery.end(), PoseDelivery{})
                    : std::min_element(p.pose_delivery.begin(), p.pose_delivery.end(),
                          [](const auto &a, const auto &b) { return a.last_sent < b.last_sent; });
            delivery = &*entry;
            if (delivery->source != packet.source || delivery->epoch != packet.epoch)
                *delivery = {packet.source, packet.epoch, 0, interval};
            // Full upstream rate lets the host forward to nearby recipients even
            // when its own skater is far from this guest.
            if (!(s.mode == Mode::join && p.member.id == s.host_id) && p.latest_root &&
                p.pose_arrival && now >= p.pose_arrival && now - p.pose_arrival <= 1000000) {
                float distance{};
                for (unsigned i = 0; i < 3; ++i) {
                    const auto d = p.latest_root->position[i] - packet.pose.root.position[i];
                    distance += d * d;
                }
                interval = pose_interval(distance, delivery->interval_us, s.distances, s.tps);
            }
            if (delivery->interval_us != interval) delivery->next_source_time = 0;
            delivery->interval_us = interval;
            // Source capture already caps full-rate poses. A second arrival-time
            // throttle discards good updates when frames or relay arrivals jitter.
            if (interval > multiplayer_pose_interval(s.tps) && packet.time_us < delivery->next_source_time)
                continue;
        }
        const auto variant = interval == 200000 ? 2U : interval == 100000 ? 1U : 0U;
        auto &data = encoded[variant];
        if (!data.ready) {
            // Only a pose's encoding carries its interval.
            data.raw = packet.kind == PacketKind::pose ? encode(packet, true, interval) : encode(packet, true);
            data.wire = encode_wire_bytes(data.raw);
            data.ready = true;
        }
        auto update = p.sender.prepare(packet, data.raw, data.wire, data.deltas);
        // Unbuildable for anyone (see send_packet): skipped, and no recipient is dropped for it.
        if (update.bytes.empty())
            continue;
        outgoing.push_back({&p, std::move(update), delivery, interval});
    }
    if (outgoing.empty())
        return;
    std::vector<TransportSend> sends;
    sends.reserve(outgoing.size());
    // A stream and its reliable delta references must always use the same lane.
    for (const auto &item : outgoing)
        sends.push_back({item.peer->member.id, item.update.bytes, reliable || item.update.establishes_baseline(), fresh,
                         traffic_lane(packet.kind)});
    s.transport.send_batch(sends);
    for (std::size_t i = 0; i < outgoing.size(); ++i) {
        auto &item = outgoing[i];
        if (sends[i].sent) {
            item.peer->sender.sent(packet, std::move(item.update));
            if (item.delivery) {
                item.delivery->last_sent = now;
                advance_pose_deadline(item.delivery->next_source_time, packet.time_us, item.interval);
            }
        } else if (reliable && !fresh)
            s.transport.disconnect(item.peer->member.id, "Cannot deliver required session data. Join again.");
    }
}
void sync_objects(Session &s, const NativeFrame &local, std::uint64_t now) {
    if ((s.mode != Mode::host && s.mode != Mode::join) || !world_playing(s, local) || now < s.next_object_update) return;
    s.next_object_update = now + 100000;
    const auto captured = capture_local_network_objects();
    if (s.clear_pending && clear_lobby_guest_objects()) s.clear_pending = false;
    if (captured) s.local_objects.replace(captured->objects);
    publish_guest_objects(s);
    send_object_states(s, now);
    if (!captured) {
        // The native runtime may reset its remote objects meanwhile (a level reload).
        s.object_owners_valid = false;
        return;
    }
    // Handing the owners over copies every remote object into a new native table. Do it
    // when an owner, its revision or the map changed; and once a second anyway, since the
    // runtime can reset its table itself (a reload keeps the map name).
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> current;
    for (const auto &peer : active_peers(s))
        if (const auto &objects = visible_objects(s, peer); peer.handshaken && peer.world_ready && objects.revision())
            current.emplace_back(peer.member.id, peer.member.epoch, objects.revision());
    if (s.object_owners_valid && current == s.object_owners_sent && captured->map == s.object_map_sent &&
        now >= s.object_owners_at && now - s.object_owners_at < 1000000)
        return;
    std::vector<NetworkObjectOwner> owners;
    for (const auto &peer : active_peers(s))
        if (const auto &objects = visible_objects(s, peer); peer.handshaken && peer.world_ready && objects.revision())
            owners.push_back({peer.member.id, peer.member.epoch, objects.layout()});
    set_remote_network_objects(captured->map, owners);
    s.object_owners_sent = std::move(current);
    s.object_map_sent = captured->map;
    s.object_owners_valid = true;
    s.object_owners_at = now;
}
void refresh_host_choices(Session &s, std::uint64_t now) {
    if (s.mode != Mode::host || now < s.next_park_update) return;
    s.next_park_update = now + 100000;
    const auto parks = local_profile_parks().choices;
    if (parks != s.parks) { s.parks = parks; s.roster_dirty = true; }
    if (s.force_world_layers) {
        const auto layers = local_profile_world_layers().choices;
        if (layers != s.layers) { s.layers = layers; s.roster_dirty = true; }
    }
}
void send_roster(Session &s, std::uint64_t now) {
    auto p = packet(s, PacketKind::roster, now);
    p.voice_policy = s.voice_policy;
    p.voice_range = s.voice_range;
    p.distances = s.distances;
    p.object_placement = s.object_placement;
    p.object_limit = s.object_limit;
    p.guest_noclip = s.guest_noclip;
    p.guest_no_bail = s.guest_no_bail;
    p.guest_boosts = s.guest_boosts;
    p.enforce_tuning = s.enforce_tuning;
    p.object_clears = s.object_clears.value_or(0);
    p.force_world_layers = s.force_world_layers;
    s.layers = local_profile_world_layers().choices;
    p.layers = pack_world_layers(s.layers);
    s.parks = local_profile_parks().choices;
    p.parks = s.parks;
    p.capacity = s.capacity;
    // Names come from the transport's short-lived name cache, not a Steam call per player.
    // Steam names are cleaned to what a roster may carry: one that is not would be refused
    // by every guest, and by this host's own encoder.
    p.members.push_back({p.source, s.epoch, clean_roster_name(s.transport.name(p.source))});
    p.members.back().scoring = s.local_scoring;
    for (auto &peer : active_peers(s))
        if (peer.handshaken) {
            peer.member.name = clean_roster_name(s.transport.name(peer.member.id));
            p.members.push_back(peer.member);
        }
    // Parties are the ones the lobby's players formed (session_party.cpp), as on a dedicated
    // server. The host's own copy of each player follows the roster it sends.
    fill_roster_parties(s, p.members);
    for (auto &peer : active_peers(s))
        if (peer.handshaken)
            for (const auto &m : p.members)
                if (m.id == peer.member.id) {
                    if (peer.member.party != m.party || peer.member.party_leader != m.party_leader ||
                        peer.member.party_open != m.party_open) ++s.party_revision;
                    peer.member.party = m.party;
                    peer.member.party_leader = m.party_leader;
                    peer.member.party_open = m.party_open;
                }
    set_local_party(s, p.members.front());
    broadcast(s, p, true, false, now);
    s.roster_dirty = false;
    s.last_roster = now;
    publish_chat(s); // the host's "/" argument lists (players) follow its own roster
}
void update_physics_tuning(Session &s, const NativeFrame &local, std::uint64_t now) {
    // A guest whose host sets everyone's physics: the player's own edits stand down from the
    // moment of joining (the roster says otherwise, if it does) and through travel, and what
    // the host shares beyond its tuning is theirs. A dedicated server shares the game's own.
    const bool enforced = s.mode == Mode::join && s.enforce_tuning;
    set_session_tuning_enforced(enforced);
    if (enforced && s.host_extras && !dedicated_host(s)) set_host_physics_extras(*s.host_extras);
    else set_host_physics_extras({});
    if (s.mode == Mode::host) {
        physics_tuning::release(s.base);
        if (!s.enforce_tuning) {
            s.sent_tuning.reset();
            s.tuning_packet.clear();
            s.sent_extras = 0;
            s.extras_packet.clear();
            return;
        }
        // The host's physics beyond its tuning: small, so a look four times a second, and sent
        // whenever they change (the first time even when they are the game's own, so a guest
        // never keeps what an earlier spell of enforcement left it).
        if (now >= s.next_extras_check) {
            s.next_extras_check = now + 250000;
            std::vector<std::uint8_t> extras;
            if (local_physics_extras(s.sent_extras, extras)) {
                if (extras.size() > max_physics_extras) extras.clear();
                logging::log(logging::Level::info, logging::Channel::runtime,
                             "Multiplayer: sending your other physics changes to guests ({} bytes).", extras.size());
                auto p = packet(s, PacketKind::physics_extras, now);
                p.extras = std::move(extras);
                s.extras_packet = encode_wire(p);
                broadcast(s, p, true, false, now);
            }
        }
        // Edits to the tuning are rare: a look every 5 s (a 17 KB copy) is enough.
        if (now < s.next_tuning_check) return;
        s.next_tuning_check = now + 5000000;
        physics_tuning::prepare();
        const auto differences = physics_tuning::local_differences(s.base, max_physics_tuning);
        if (!differences || (s.sent_tuning && *s.sent_tuning == differences->bytes)) return;
        if (differences->left_out)
            logging::log(logging::Level::warning, logging::Channel::runtime,
                         "Multiplayer: {} of your changed physics tuning curves do not fit in the session's tuning and are left out.",
                         differences->left_out);
        logging::log(logging::Level::info, logging::Channel::runtime,
                     "Multiplayer: sending your physics tuning to guests ({} changed groups of values, {} curves, {} bytes).",
                     differences->runs, differences->curves, differences->bytes.size());
        s.sent_tuning = differences->bytes;
        auto p = packet(s, PacketKind::physics_tuning, now);
        p.tuning = differences->bytes;
        s.tuning_packet = encode_wire(p);
        broadcast(s, p, true, false, now);
        return;
    }
    if (s.mode == Mode::join && s.roster_sequence && s.enforce_tuning) {
        if (!world_playing(s, local)) return; // kept as it is while travelling
        physics_tuning::prepare();
        if (dedicated_host(s))
            physics_tuning::enforce(s.base, local.entity, {});
        else if (s.host_tuning)
            physics_tuning::enforce(s.base, local.entity, *s.host_tuning);
        return;
    }
    physics_tuning::release(s.base);
}
namespace {
std::string scoring_mod_names(const std::vector<std::string> &mods) {
    std::string names;
    for (const auto &mod : mods) names += (names.empty() ? "" : ", ") + mod;
    return clean_chat_text(names);
}
} // namespace
std::string own_scoring_notice() {
    const auto names = scoring_mod_names(mods::scoring_state().mods);
    return "Your mods change scoring or physics" + (names.empty() ? std::string() : " (" + names + ")") +
           ", so throwdowns and coop challenges are off for you in this session. Turn them off and restart Skate to take part.";
}
void update_scoring(Session &s, std::uint64_t now) {
    if ((s.mode != Mode::host && s.mode != Mode::join) || now < s.next_scoring_check) return;
    s.next_scoring_check = now + 1000000; // it changes at most when mods are applied
    const auto state = mods::scoring_state();
    if (!state.known) return; // the launch's check is still reading the mods
    if (s.mode == Mode::host) {
        const bool flagged = s.score_check && state.fingerprint;
        if (flagged == s.local_scoring) return;
        s.local_scoring = flagged;
        s.roster_dirty = true;
        if (flagged) add_chat(s, 0, "ReSkate", own_scoring_notice());
        return;
    }
    const auto *host = find_peer(s, s.host_id);
    if (!host || !host->handshaken) return;
    std::pair report{state.fingerprint, scoring_mod_names(state.mods)};
    if (s.scoring_sent == report) return;
    auto p = packet(s, PacketKind::scoring, now);
    p.scoring = report.first;
    p.text = report.second;
    send_required(s, s.host_id, encode_wire(p));
    logging::log(logging::Level::info, logging::Channel::runtime, "Multiplayer: told the host our trick scoring is {}.",
                 report.first ? "changed by " + (report.second.empty() ? std::string("mods") : report.second) : "the game's own");
    s.scoring_sent = std::move(report);
}
void judge_scoring(Session &s, Peer &peer) {
    const bool flagged = s.score_check && peer.scoring && *peer.scoring;
    if (flagged == peer.member.scoring) return;
    peer.member.scoring = flagged;
    s.roster_dirty = true;
    const auto name = peer.member.name.empty() ? s.transport.name(peer.member.id) : peer.member.name;
    logging::log(logging::Level::info, logging::Channel::runtime, "Multiplayer: {}'s mods {} (scoring {:016x}{}).", name,
                 flagged ? "change scoring or physics: out of throwdowns and coop challenges" : "are no longer checked",
                 peer.scoring.value_or(0), peer.scoring_mods.empty() ? "" : ", " + peer.scoring_mods);
    if (flagged)
        add_chat(s, 0, "ReSkate", name + "'s mods change scoring or physics" +
                                      (peer.scoring_mods.empty() ? std::string() : " (" + peer.scoring_mods + ")") +
                                      ", so they are out of throwdowns and coop challenges.");
}
void send_local(Session &s, const NativeFrame &local, std::uint64_t now, std::uint64_t captured_at,
                bool pose_captured) {
    if (!world_playing(s, local))
        return;
    s.local_root = local.pose.root;
    auto deliver = [&](Packet &p, bool reliable, bool fresh) {
        if (s.mode == Mode::echo) {
            auto update = s.peers[0].sender.prepare(p);
            bool missing{};
            auto echoed = s.peers[0].receiver.receive(update.bytes, missing);
            s.peers[0].sender.sent(p, std::move(update));
            if (!echoed || !accept_data(s.peers[0], std::move(*echoed), now))
                throw std::runtime_error("Local Echo packet could not be decoded.");
        } else
            broadcast(s, p, reliable, fresh, now);
        return p.kind == PacketKind::cosmetics ? encode_wire(p) : std::vector<std::uint8_t>{};
    };
    if (now - s.last_cosmetic_capture >= 500000) {
        s.last_cosmetic_capture = now;
        auto appearance = capture_cosmetics(s.base, local, s.cosmetic_capture_status);
        // The player's choices to go without their tag or their animated items travel with their
        // outfit, so a change is sent like one and reaches players who join later.
        if (appearance) {
            appearance->hide_tag = !own_tag_shown();
            appearance->hide_items = !own_items_shown();
            appearance->marks = developer_hoodie_detail::own_styles.load();
        }
        if (appearance && (!s.sent_appearance || *appearance != *s.sent_appearance)) {
            auto p = packet(s, PacketKind::cosmetics, now);
            p.appearance = *appearance;
            s.cosmetic_packet = deliver(p, true, false);
            s.sent_appearance = appearance;
        }
    }
    if (now < s.next_send)
        return;
    // A callback can cross the deadline after its cheap root capture. Wait for
    // the next callback instead of accidentally publishing an empty rig.
    if (!pose_captured)
        return;
    advance_pose_deadline(s.next_send, now, multiplayer_pose_interval(s.tps));
    auto p = packet(s, PacketKind::pose, now);
    // A few guarded reads, at the pose rate.
    s.local_player_collision = local_allows_player_collision(s.base, local.entity);
    p.player_collision = s.local_player_collision;
    if (captured_at) p.time_us = captured_at;
    p.pose = local.pose;
    ++s.local_pose_count;
    if (s.pose_dump.is_open()) {
        // A record: when it was captured (8 bytes), its length (4), and the compact encoding.
        const auto raw = encode(p, true);
        const std::uint64_t time = p.time_us;
        const auto length = static_cast<std::uint32_t>(raw.size());
        s.pose_dump.write(reinterpret_cast<const char *>(&time), sizeof time);
        s.pose_dump.write(reinterpret_cast<const char *>(&length), sizeof length);
        s.pose_dump.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        ++s.pose_dump_count;
        if (now >= s.pose_dump_until) {
            s.pose_dump.close();
            logging::log(logging::Level::info, logging::Channel::runtime, "Multiplayer: pose dump finished, {} poses.", s.pose_dump_count);
        }
    }
    deliver(p, false, true);
    auto samples = drain_audio_capture(now);
    if (!samples.empty()) {
        auto a = packet(s, PacketKind::audio, now);
        a.audio = std::move(samples);
        if (s.pose_dump.is_open()) {
            // The skater's sound is recorded with the poses, in the same records.
            const auto raw = encode(a, true);
            const std::uint64_t time = a.time_us;
            const auto length = static_cast<std::uint32_t>(raw.size());
            s.pose_dump.write(reinterpret_cast<const char *>(&time), sizeof time);
            s.pose_dump.write(reinterpret_cast<const char *>(&length), sizeof length);
            s.pose_dump.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        }
        deliver(a, std::any_of(a.audio.begin(), a.audio.end(), [](const auto &sample) { return sample.event; }), true);
    }
    // This skater's contacts with the world, for the sparks and dust others see on it.
    if (auto impacts = drain_impacts(); !impacts.empty() && s.sync_effects) {
        auto e = packet(s, PacketKind::effects, now);
        e.impacts = std::move(impacts);
        deliver(e, false, true);
    }
}
} // namespace session_detail
} // namespace dingosdk::multiplayer
