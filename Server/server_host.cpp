#include "server_host.h"
#include "server_text.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Multiplayer/Net/block_codec.h"
#include "Extension/Multiplayer/Net/pose_delta.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/World/world_names.h"
#include "Engine/Game/World/park_randomization.h"
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <fstream>
#include <random>
#endif
#include <algorithm>
#include <cmath>
#include <array>
#include <charconv>
#include <ctime>
#include <stdexcept>

namespace dingosdk::server {
namespace {
std::uint64_t nonce() {
    std::uint64_t value{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !value)
        throw std::runtime_error("Cannot generate a session code.");
#else
    // Preferred: OS entropy; fallback to std::random_device for containers without getrandom.
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.read(reinterpret_cast<char *>(&value), sizeof(value)) && value) return value;
    std::random_device device;
    // Four rounds of sixteen bits each. Stopping as soon as `value` was
    // non-zero would leave a session code with sixteen bits of entropy.
    for (int i = 0; i < 4; ++i)
        value = (value << 16) ^ static_cast<std::uint64_t>(device() & 0xFFFF);
    if (!value) throw std::runtime_error("Cannot generate a session code.");
#endif
    return value;
}
} // namespace

Host::Host(ServerConfig &config, SteamTransport &transport, Log log)
    : config_(config), transport_(transport), log_(std::move(log)),
      activity_([this](const std::string &text) { if (config_.activity_log) log_(text); },
                [this](std::uint64_t id) {
                    const auto *guest = find(id);
                    return guest && guest->handshaken ? guest_name(*guest) : std::string{};
                },
                [this](const std::string &text) { if (config_.announce_throwdowns) send_chat(text); }) {
    parties_.set_limit(config_.party_size);
}

Host::Guest *Host::find(std::uint64_t id) {
    const auto found = guests_.find(id);
    return found == guests_.end() ? nullptr : found->second.get();
}
Packet Host::packet(PacketKind kind, std::uint64_t now) {
    Packet p;
    p.kind = kind;
    p.sequence = ++sequence_;
    p.session = secret_;
    p.epoch = epoch_;
    p.map = map_;
    p.time_us = now;
    p.source = id_;
    p.world = world_;
    p.tps = config_.tps;
    p.pose_interval_us = multiplayer_pose_interval(config_.tps);
    p.build = supported_build::game_sha256_bytes;
    return p;
}
std::string Host::guest_name(const Guest &g) const {
    return g.member.name.empty() ? std::to_string(g.member.id) : g.member.name;
}
// The name a joining player is known by here. It is the one their game sent, which the
// server cannot check against Steam, so it is made safe to show and to type: never blank,
// never the server's or ReSkate's own, never read as a SteamID64 by kick or ban (which take
// a number as one), and never the same as another player's.
std::string Host::player_name(std::string_view wanted, std::uint64_t id) const {
    const auto fallback = "Player " + std::to_string(id % 10000);
    std::string name(trim(wanted));
    cut_text(name, max_member_name);
    // Names show in every player's roster, nametags and party UI.
    if (text::contains_bad_words(name)) name = text::mask_bad_words(name);
    const auto folded = lower(name);
    const auto first = split(name).first;
    const bool digits = !first.empty() && std::all_of(first.begin(), first.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (name.empty() || folded == "server" || folded == "reskate" || folded == lower(config_.name)) name = fallback;
    else if (digits) name = "Player " + name;
    const auto taken = [&](const std::string &candidate) {
        return std::any_of(guests_.begin(), guests_.end(), [&](const auto &entry) {
            return entry.first != id && entry.second->handshaken && lower(entry.second->member.name) == lower(candidate);
        });
    };
    auto unique = name;
    for (unsigned copy = 2; taken(unique) && copy < 1000; ++copy) unique = name + " (" + std::to_string(copy) + ")";
    cut_text(unique, 128);
    return unique;
}
bool Host::is_admin(std::uint64_t id) const {
    return std::find(config_.admins.begin(), config_.admins.end(), id) != config_.admins.end();
}
bool Host::is_banned(std::uint64_t id) const {
    return std::any_of(config_.bans.begin(), config_.bans.end(), [&](const auto &ban) { return ban.id == id; });
}
void Host::save() {
    try {
        save_config(config_);
    } catch (const std::exception &e) {
        log_(std::string("Could not save the config: ") + e.what());
    }
}
std::string Host::invite() const { return format_invite({id_, secret_}); }
std::string Host::map_name() const { return map_label(config_.map); }
std::string Host::wire_map_label() const { // players without the map's mod still see its name
    auto label = map_name();
    cut_text(label, max_member_name);
    return valid_map_label(label) ? label : std::string{};
}
unsigned Host::players() const {
    return static_cast<unsigned>(std::count_if(guests_.begin(), guests_.end(), [](const auto &g) { return g.second->handshaken; }));
}

bool Host::start(std::string &error) {
    transport_.set_send_rate(static_cast<int>(config_.send_rate * 1024));
    transport_.set_packing(config_.pack_ms);
    if (config_.steam_debug)
        log_(transport_.set_steam_debug(true) ? "Steam networking debug output is on (\"steam_debug\"): its lines are marked [direct] Steam:."
                                             : "steam_debug: this Steam has no debug output.");
    if (!transport_.host(connection_capacity())) {
        error = transport_.status().detail;
        return false;
    }
    direct_port_ = 0;
    if (!config_.use_steam_relay) {
        const auto port = config_.port;
        if (transport_.listen_direct(port)) {
            direct_port_ = port;
            log_("Connection: direct, on UDP port " + std::to_string(port) +
                 ". The port must be open to the internet; players it does not reach come through Steam's relays.");
        } else {
            log_("Connection: direct was asked for, but Steam could not listen on UDP port " + std::to_string(port) +
                 " (in use, or not allowed here). Players connect through Steam's relays only.");
        }
    } else {
        log_("Connection: through Steam's relays (\"use_steam_relay\": false lets players connect straight to the server).");
    }
    id_ = transport_.status().local_id;
    secret_ = nonce();
    epoch_ = nonce();
    world_ = 1;
    map_ = map_hash(map_destination(config_.map));
    password_ = config_.password.empty() ? std::nullopt : password_key(config_.password, secret_);
    voice_policy_ = {config_.voice_chat, 1};
    apply_layers();
    // The threads that share each pass's sending with this one ("threads": this many in all;
    // 0: one for each of the machine's processors but one, which is left to Steam's own).
    {
        const auto cores = std::max(1U, std::thread::hardware_concurrency());
        const auto wanted = config_.threads ? config_.threads : std::clamp(cores - 1, 1U, 8U);
        workers_.reset();
        if (wanted > 1) workers_ = std::make_unique<WorkerPool>(wanted - 1);
        log_("Threads: " + std::to_string(1 + (workers_ ? workers_->threads() : 0)) + " share the sending of each pass (\"threads\"; this machine has " +
             std::to_string(cores) + " processors).");
    }
    running_ = true;
    roster_dirty_ = true;
    return true;
}
void Host::stop(const std::string &reason) {
    if (!running_) return;
    const auto away = packet(PacketKind::away, now_us());
    for (auto &[id, guest] : guests_)
        if (guest->handshaken) send_packet(*guest, away, true, false);
    for (auto it = guests_.begin(); it != guests_.end();) transport_.disconnect((it++)->first, reason.c_str());
    guests_.clear();
    transport_.stop();
    running_ = false;
}
void Host::apply_layers() {
    layers_ = default_world_layers();
    if (!config_.world_layer_sync) return;
    for (std::size_t i = 0; i < world_layers().size(); ++i) {
        const auto found = config_.layers.find(world_layers()[i].key);
        if (found != config_.layers.end() && valid_world_layer_mode(found->second)) layers_[i] = found->second;
    }
}

// ---- Sending ---------------------------------------------------------------------------------
namespace {
// How often a stream's whole state is sent again. A whole pose measured 3.7 KB against 0.7 KB
// for a difference from one, however old the reference (net's pose size line), so they are
// kept rare: at every 8 seconds they were a seventh of everything a busy server sent.
constexpr std::uint64_t whole_state_refresh = 60000000;
// Rotations were rounded more coarsely for far recipients for a while (coarsen_rotations).
// Measured on a full server it saved 0 to 7% of a pose (24% out of sight), because a pose is
// sent as a difference from a reference up to a minute old, from which every animated bone
// has turned by far more than any step; and renewing the reference whenever a recipient
// crossed from one precision to another cost more than that saved. So poses go as they come.
constexpr std::array<const char *, 4> pose_rate_names{"full rate", "half rate", "low rate", "out of sight"};
// How long a kept pose stays (KeptPose::keep), in microseconds: by the slowest rate it went out at.
constexpr std::array<std::uint64_t, 4> pose_kept_for{1500000, 3000000, 5000000, 15000000};
constexpr std::array<const char *, 6> traffic_names{"poses", "sound", "voice", "outfits", "objects", "other"};
constexpr std::size_t traffic_kind(PacketKind kind) noexcept {
    return kind == PacketKind::pose ? 0 : kind == PacketKind::audio ? 1 : kind == PacketKind::voice ? 2
         : kind == PacketKind::cosmetics ? 3 : kind == PacketKind::objects ? 4 : 5;
}
} // namespace
bool Host::send_packet(Guest &g, const Packet &p, bool reliable, bool fresh, std::span<const std::uint8_t> raw,
                       std::span<const std::uint8_t> wire) {
    auto update = raw.empty() ? g.sender.prepare(p) : g.sender.prepare(p, raw, wire);
    // A packet that cannot be built for anyone is its source's fault, never this recipient's:
    // nothing is sent, and the recipient is not treated as unreachable.
    if (update.bytes.empty()) return true;
    // A stream and its reliable delta references must always use the same lane.
    if (!transport_.send(g.member.id, update.bytes, reliable || update.establishes_baseline(), fresh, traffic_lane(p.kind)))
        return false;
    for (auto *counted : {&g.traffic, &traffic_}) {
        counted->total.out[traffic_kind(p.kind)] += update.bytes.size();
        counted->total.snapshots += update.establishes_baseline();
    }
    g.sender.sent(p, std::move(update));
    return true;
}
void Host::send_required(Guest &g, const std::vector<std::uint8_t> &bytes) {
    if (bytes.empty()) return;
    const auto p = decode_wire(bytes);
    if (!p || !send_packet(g, *p, true, false))
        transport_.disconnect(g.member.id, "Cannot deliver required session data. Join again.");
}
void Host::measure_pose(Guest &from, const Packet &packet) {
    auto raw = encode(packet, true);
    // As DeltaSender would send it: the patch packed, behind its 34 byte header.
    const auto cost = [&](const Guest::Earlier &reference) -> std::uint64_t {
        if (reference.raw.empty()) return 0;
        const auto patch = pose_delta::encode(raw, reference.raw);
        return patch.empty() ? 0 : 34 + compress_block(patch).bytes.size();
    };
    if (packet.sequence % 4 == 0) {
        const auto last = cost(from.pose_last), quarter = cost(from.pose_quarter), second = cost(from.pose_second);
        if (last && quarter && second) {
            ++pose_sizes_.samples;
            // What changed since the pose before, field by field, as pose_delta walks them.
            try {
                constexpr auto prefix = packet_header_size + 6;
                const auto bones = pose_delta::count(raw);
                if (bones && bones == pose_delta::count(from.pose_last.raw)) {
                    pose_delta::Cursor now_at{raw, prefix}, before_at{from.pose_last.raw, prefix};
                    pose_sizes_.bones += bones;
                    for (unsigned bone = 0; bone < bones; ++bone) {
                        const auto current = pose_delta::fields(now_at), earlier = pose_delta::fields(before_at);
                        // Fields: flags, position (12 bytes as floats, 6 in mm), rotation, scale.
                        for (unsigned field = 1; field < 4; ++field) {
                            if (std::equal(current[field].begin(), current[field].end(), earlier[field].begin(), earlier[field].end())) continue;
                            auto &kind = pose_sizes_.changed[field == 1 ? (current[1].size() == 12 ? 0 : 1) : field];
                            ++kind.fields;
                            kind.bytes += current[field].size();
                        }
                    }
                }
            } catch (...) {
            }
            pose_sizes_.whole += encode_wire_bytes(raw).size() + 4;
            pose_sizes_.last += last;
            pose_sizes_.quarter += quarter;
            pose_sizes_.second += second;
        }
    }
    // Each kept reference is replaced once it is its age old, as a whole state sent that often would be.
    for (auto [kept, age] : {std::pair{&from.pose_quarter, 250000ULL}, std::pair{&from.pose_second, 1000000ULL}})
        if (kept->raw.empty() || packet.time_us < kept->time || packet.time_us - kept->time >= age) *kept = {raw, packet.time_us};
    from.pose_last = {std::move(raw), packet.time_us};
}
Host::Guest::KeptPose *Host::keep_pose(Guest &from, const Packet &packet) {
    auto &kept = from.kept_poses;
    std::erase_if(kept, [&](const Guest::KeptPose &pose) { return now_ - pose.kept_at > pose_kept_for[std::min<std::size_t>(pose.keep, 3)]; });
    while (kept.size() >= 96) kept.pop_front();
    if (!kept.empty() && kept.back().sequence == packet.sequence) return &kept.back();
    // What a mod resized on its player's skater reaches the others only as far as the server allows.
    if (config_.bone_scale_limit >= 1.f) {
        auto pose = packet.pose;
        limit_bone_scale(pose, config_.bone_scale_limit);
        kept.push_back({packet.sequence, packet.time_us, now_, 0, pose_codec::quantize(pose)});
    } else {
        kept.push_back({packet.sequence, packet.time_us, now_, 0, pose_codec::quantize(packet.pose)});
    }
    return &kept.back();
}
// Sends each player the poses queued for them this pass (broadcast): their differences from a
// pose of the same player the recipient is known to hold, packed several to a message.
// What one player is sent of the pass: the ack of their own poses, the sound they hear and the
// poses of everyone they see. It reads the other players (the poses kept of them) and writes
// only this player's own state and `sent`, so the players are done on several threads at once
// (flush_poses); what is everyone's is added up afterwards from `sent`.
void Host::flush_player(std::uint64_t id, Guest &g, Flushed &sent) {
    // One at a time into the transport, in this player's order.
    const auto send = [&](std::span<const std::uint8_t> message, bool reliable, bool fresh, TrafficLane lane) {
        std::lock_guard lock(send_mutex_);
        return transport_.send(id, message, reliable, fresh, lane);
    };
    const auto count = [&](PacketKind kind, std::size_t bytes) {
        g.traffic.total.out[traffic_kind(kind)] += bytes;
        sent.traffic.out[traffic_kind(kind)] += bytes;
    };
    // Which of their own pose messages arrived, once for all read this pass.
    if (std::exchange(g.upload_ack_due, false)) {
        const auto ack = g.upload_ack.bytes();
        if (send(ack, false, true, TrafficLane::gameplay)) count(PacketKind::pose, ack.size());
    }
    // The sound of the skaters they can hear, in one message, reliably: each builds on the
    // last, and many samples are a pulse that must not be lost.
    if (!g.queued_sound.empty()) {
        const auto sounds = std::exchange(g.queued_sound, {});
        if (g.handshaken && g.world_ready) {
            g.sound_sender.begin(world_, map_);
            for (const auto &q : sounds)
                if (const auto *from = find(q.source)) g.sound_sender.add(q.source, from->member.epoch, q.sequence, q.time_us, *q.samples, now_);
            if (g.sound_sender.pending()) {
                const auto message = g.sound_sender.message();
                const bool went = send(message, true, true, traffic_lane(PacketKind::audio));
                if (went) count(PacketKind::audio, message.size());
                g.sound_sender.sent(went);
            }
        }
    }
    if (g.queued_poses.empty()) return;
    const auto queued = std::exchange(g.queued_poses, {});
    if (!g.handshaken || !g.world_ready) return;
    const auto emit = [&](std::span<const std::uint8_t> message, bool reliable) {
        if (!send(message, reliable, !reliable, TrafficLane::gameplay)) return false;
        count(PacketKind::pose, message.size());
        return true;
    };
    g.pose_sender.begin(world_, map_);
    for (const auto &q : queued) {
        auto *from = find(q.source);
        if (!from) continue;
        const auto find_kept = [&](std::uint32_t sequence) -> std::optional<pose_batch::KeptView> {
            for (auto it = from->kept_poses.rbegin(); it != from->kept_poses.rend(); ++it)
                if (it->sequence == sequence) return pose_batch::KeptView{it->sequence, it->time_us, &it->pose};
            return {};
        };
        const auto pose = find_kept(q.sequence);
        if (!pose) continue;
        const auto added = g.pose_sender.add(q.source, from->member.epoch, *pose, find_kept, q.hold_fingers, q.collision, q.rate, now_, emit);
        if (added.did == pose_batch::Sender::Did::whole) {
            // Kept long enough for the ack of it to find it here (marked once every player is done).
            sent.whole.emplace_back(from, q.sequence);
            ++g.traffic.total.snapshots;
            ++sent.traffic.snapshots;
            ++sent.sizes.whole_sent;
            sent.sizes.whole_sent_bytes += added.bytes;
        } else if (added.did == pose_batch::Sender::Did::difference) {
            const auto tier = std::min<std::size_t>(q.tier, 3);
            ++sent.sizes.sent[tier];
            sent.sizes.sent_bytes[tier] += added.bytes + 8;
            sent.sizes.held += q.hold_fingers;
        }
    }
    g.pose_sender.flush(emit);
}
void Host::flush_poses() {
    std::vector<std::pair<std::uint64_t, Guest *>> players;
    std::size_t poses{};
    for (auto &[id, guest] : guests_) {
        if (!guest->upload_ack_due && guest->queued_sound.empty() && guest->queued_poses.empty()) continue;
        players.emplace_back(id, guest.get());
        poses += guest->queued_poses.size();
    }
    std::vector<Flushed> sent(players.size());
    const std::function<void(std::size_t)> job = [&](std::size_t index) {
        flush_player(players[index].first, *players[index].second, sent[index]);
    };
    // Waking the threads costs more than a pass with little in it.
    if (workers_ && poses >= 256) workers_->run(players.size(), job);
    else
        for (std::size_t index = 0; index < players.size(); ++index) job(index);
    for (const auto &done : sent) {
        for (std::size_t kind = 0; kind < done.traffic.out.size(); ++kind) traffic_.total.out[kind] += done.traffic.out[kind];
        traffic_.total.snapshots += done.traffic.snapshots;
        pose_sizes_.whole_sent += done.sizes.whole_sent;
        pose_sizes_.whole_sent_bytes += done.sizes.whole_sent_bytes;
        pose_sizes_.held += done.sizes.held;
        for (std::size_t tier = 0; tier < done.sizes.sent.size(); ++tier) {
            pose_sizes_.sent[tier] += done.sizes.sent[tier];
            pose_sizes_.sent_bytes[tier] += done.sizes.sent_bytes[tier];
        }
        for (const auto &[from, sequence] : done.whole)
            for (auto &kept : from->kept_poses)
                if (kept.sequence == sequence) kept.keep = 3;
    }
}
// A player said which messages of poses they read in full: the poses in those are ones they hold.
void Host::pose_ack(Guest &g, const pose_batch::Ack &ack) { g.pose_sender.ack(ack); }
void Host::broadcast(const Packet &packet, bool reliable, bool fresh, std::uint64_t except) {
    // A pose is rounded once and kept (keep_pose); each recipient is then sent its differences
    // from a pose of this player they hold, several players to a message (flush_poses).
    auto *kept = packet.kind == PacketKind::pose ? [&]() -> Guest::KeptPose * {
        auto *from = find(packet.source);
        return from ? keep_pose(*from, packet) : nullptr;
    }() : nullptr;
    const auto *source = find(packet.source);
    // Recipients holding the same delta reference share one patch and compression
    // (DeltaCache): every update is built first, then all are sent and recorded.
    struct Encoded { Packet packet; std::vector<std::uint8_t> raw, wire; DeltaCache deltas; };
    std::array<Encoded, 12> encoded; // by the interval the pose carries, then by precision
    struct Outgoing { Guest *guest; std::uint64_t id; WireUpdate update; PoseDelivery *delivery; std::uint32_t interval; unsigned variant; };
    std::vector<Outgoing> outgoing;
    outgoing.reserve(guests_.size());
    std::shared_ptr<const std::vector<AudioSample>> sound; // a skater's samples, shared by everyone sent them
    const bool gameplay = packet.kind == PacketKind::pose || packet.kind == PacketKind::audio ||
                          packet.kind == PacketKind::voice || packet.kind == PacketKind::cosmetics ||
                          packet.kind == PacketKind::effects;
    for (auto &[id, guest] : guests_) {
        auto &p = *guest;
        if (!p.handshaken || id == except || (gameplay && !p.world_ready)) continue;
        if (packet.kind == PacketKind::voice) {
            if (!voice_policy_.accepts(packet.voice)) continue;
            // Each listener fades voices by their own hearing distance; the server
            // forwards proximity voice within its range. Unknown positions go through.
            if (packet.voice.distance > 0.f && source) {
                const bool known = source->latest_root && p.latest_root && p.pose_arrival &&
                                   now_ - p.pose_arrival <= 3000000 && source->pose_arrival &&
                                   now_ - source->pose_arrival <= 3000000;
                if (known && voice_gain(source->latest_root->position, p.latest_root->position, config_.voice_range) <= 0.f)
                    continue;
            }
        }
        // Not before they have been shown this player (introduce).
        if (!p.unmet.empty() && (packet.kind == PacketKind::pose || packet.kind == PacketKind::audio || packet.kind == PacketKind::cosmetics) &&
            p.unmet.contains(packet.source))
            continue;
        // A skater's effects go to those near enough to see them: within 150 m.
        if (packet.kind == PacketKind::effects && source && source->latest_root && p.latest_root) {
            float distance{};
            for (unsigned i = 0; i < 3; ++i) {
                const auto d = p.latest_root->position[i] - source->latest_root->position[i];
                distance += d * d;
            }
            if (distance > 150.f * 150.f) continue;
        }
        // A skater's sound goes to those who would hear it: within the full pose rate's reach,
        // and in a crowd only from the nearest (the same players sent at the full rate).
        // A game stops a sound it hears nothing more of after a second.
        if (packet.kind == PacketKind::audio && source && source->latest_root && source->pose_arrival &&
            now_ - source->pose_arrival <= 1000000 && p.latest_root && p.pose_arrival && now_ - p.pose_arrival <= 1000000) {
            float distance{};
            for (unsigned i = 0; i < 3; ++i) {
                const auto d = p.latest_root->position[i] - source->latest_root->position[i];
                distance += d * d;
            }
            const auto reach = static_cast<float>(config_.distances.half_rate_start);
            if (distance > p.crowd.half || (config_.distances.valid() && distance > reach * reach)) continue;
        }
        // Poses and sound go through the server to everyone, whether or not two games have linked
        // to each other: what they send each other directly is the older, larger format, and
        // games of 1.1.6 as first released discard most of it (they took its first four bytes
        // for sound_codec's).
        // Sound is sent with everyone else's they hear, once a pass (flush_poses).
        if (packet.kind == PacketKind::audio) {
            if (!sound) sound = std::make_shared<const std::vector<AudioSample>>(packet.audio);
            p.queued_sound.push_back({packet.source, packet.time_us, packet.sequence, sound});
            continue;
        }
        PoseDelivery *delivery{};
        std::uint32_t slot{}; // how often this source's poses go to them, when not every `interval`
        bool out_of_sight{};
        unsigned precision{};
        std::uint32_t interval = multiplayer_pose_interval(config_.tps);
        if (packet.kind == PacketKind::pose) {
            auto entry = std::find_if(p.pose_delivery.begin(), p.pose_delivery.end(),
                                      [&](const auto &v) { return v.source == packet.source; });
            if (entry == p.pose_delivery.end())
                entry = std::min_element(p.pose_delivery.begin(), p.pose_delivery.end(),
                                         [](const auto &a, const auto &b) { return a.last_sent < b.last_sent; });
            delivery = &*entry;
            if (delivery->source != packet.source || delivery->epoch != packet.epoch)
                *delivery = {packet.source, packet.epoch, 0, interval, 0, 0};
            if (p.latest_root && p.pose_arrival && now_ >= p.pose_arrival && now_ - p.pose_arrival <= 1000000) {
                float distance{};
                for (unsigned i = 0; i < 3; ++i) {
                    const auto d = p.latest_root->position[i] - packet.pose.root.position[i];
                    distance += d * d;
                }
                delivery->by_distance = pose_interval(distance, delivery->by_distance, config_.distances, config_.tps);
                interval = crowd_interval(delivery->by_distance, distance, p.crowd);
                // Half as far again as the low rate starts: nothing of a skater can be made out
                // there, only where they are (their dot, the map).
                // (A little nearer to come back into sight than to leave it.)
                const auto sight = static_cast<float>(config_.distances.low_rate_start) * ((delivery->precision & 1) ? 1.4f : 1.5f);
                out_of_sight = config_.distances.valid() && distance > sight * sight;
                // Too far to make out a hand: the fingers are not sent (pose_codec's Fingers). A
                // little nearer to get them back than to lose them.
                const auto hands = static_cast<float>(config_.finger_distance) * ((delivery->precision & 2) ? 0.9f : 1.1f);
                const bool held = config_.finger_distance && packet.pose.skater.size() == pose_codec::finger_skeleton && distance > hands * hands;
                precision = held ? 1U : 0U;
                delivery->precision = static_cast<std::uint8_t>((out_of_sight ? 1U : 0U) | (held ? 2U : 0U)); // which side of each they are on
            } else {
                delivery->by_distance = 0;
            }
            // Standing still for a few seconds: five poses a second carry it to anyone. The
            // rate is back at once when they move (the change of rate sends the next pose).
            if (source && source->moved_at && now_ - source->moved_at > 3000000) interval = std::max<std::uint32_t>(interval, 200000);
            if (delivery->interval_us != interval) delivery->next_source_time = 0;
            delivery->interval_us = interval;
            if (interval > multiplayer_pose_interval(config_.tps) && packet.time_us < delivery->next_source_time) continue;
            // Out of sight, one pose a second is sent (as low-rate poses, a game holds a skater
            // it hears nothing of for several seconds). On a full server most players are out
            // of each other's sight, and at five a second each they were most of what was sent.
            slot = out_of_sight && interval == 200000 ? 1000000U : interval;
            if (kept) {
                const std::uint8_t tier = slot == 1000000 ? 3 : interval == 200000 ? 2 : interval == 100000 ? 1 : 0;
                p.queued_poses.push_back({packet.source, packet.sequence,
                                          tier >= 2 ? pose_batch::Rate::low : tier == 1 ? pose_batch::Rate::half : pose_batch::Rate::full,
                                          packet.player_collision, precision == 1, tier});
                kept->keep = std::max(kept->keep, tier);
                delivery->last_sent = now_;
                // The next slot on the source's own clock: everyone sent a source at a slower rate
                // is sent the same poses of it, so few of them need keeping to build on.
                delivery->next_source_time = (packet.time_us / slot + 1) * slot;
                continue;
            }
        }
        const auto variant = (interval == 200000 ? 2U : interval == 100000 ? 1U : 0U) * 4 + precision;
        auto &data = encoded[variant];
        if (data.raw.empty()) {
            data.packet = packet;
            data.packet.pose_interval_us = interval;
            if (packet.kind == PacketKind::pose) {
                // What a mod resized on its player's skater reaches the others only as far as the server allows.
                if (config_.bone_scale_limit >= 1.f) limit_bone_scale(data.packet.pose, config_.bone_scale_limit);
            }
            data.raw = encode(data.packet, true);
            data.wire = encode_wire_bytes(data.raw);
        }
        outgoing.push_back({&p, id, p.sender.prepare(data.packet, data.raw, data.wire, data.deltas), delivery, slot ? slot : interval, variant});
    }
    for (auto &out : outgoing) {
        // A packet that cannot be built for anyone is its source's fault, never this
        // recipient's: nothing is sent, and the recipient is not treated as unreachable.
        bool sent = true;
        if (!out.update.bytes.empty()) {
            const auto &sending = encoded[out.variant].packet;
            const bool whole = out.update.establishes_baseline();
            sent = transport_.send(out.id, out.update.bytes, reliable || whole, fresh, traffic_lane(sending.kind));
            if (sent) {
                for (auto *counted : {&out.guest->traffic, &traffic_}) {
                    counted->total.out[traffic_kind(sending.kind)] += out.update.bytes.size();
                    counted->total.snapshots += whole;
                }
                if (sending.kind == PacketKind::pose && !whole) {
                    ++pose_sizes_.sent[out.variant / 4];
                    pose_sizes_.sent_bytes[out.variant / 4] += out.update.bytes.size();
                }
                out.guest->sender.sent(sending, std::move(out.update));
            }
        }
        if (sent && out.delivery) {
            out.delivery->last_sent = now_;
            // The next slot on the source's own clock, not this long after this send: everyone
            // sent a source at a slower rate is then sent the same packets of it, and shares
            // their encoding and patch.
            out.delivery->next_source_time = (packet.time_us / out.interval + 1) * out.interval;
        }
        // Refused by a full queue: a connection that is only behind keeps its player (a chat
        // line or an outfit change is lost to them); one that takes nothing for a while does not.
        if (reliable && !fresh) {
            auto &since = out.guest->undelivered_since;
            if (sent) since = 0;
            else if (!since) since = now_;
            else if (now_ - since > 15000000) transport_.disconnect(out.id, "Cannot deliver required session data. Join again.");
        }
    }
}
// A player who has just arrived in this world is shown the others a few at a time (introduce).
void Host::meet_later(Guest &guest) {
    guest.unmet.clear();
    for (const auto &[id, other] : guests_)
        if (other->handshaken && id != guest.member.id) guest.unmet.insert(id);
    guest.next_introduction = now_;
}
// Shows each arriving player one more of the others: that player's outfit, after which their
// poses and sound follow. The nearest first, about seven a second, and none while what
// they have already been sent is still on its way. A busy server's eighty outfits and
// streams in one burst is what a joining game, building every skater at once, fell over on.
void Host::introduce() {
    for (auto &[id, guest] : guests_) {
        auto &g = *guest;
        if (g.unmet.empty() || !g.handshaken || !g.world_ready || now_ < g.next_introduction) continue;
        if (transport_.pending(id) > 96 * 1024) continue;
        const Guest *next{};
        float nearest{};
        for (auto it = g.unmet.begin(); it != g.unmet.end();) {
            const auto *other = find(*it);
            if (!other || !other->handshaken) {
                it = g.unmet.erase(it);
                continue;
            }
            ++it;
            float distance = std::numeric_limits<float>::max();
            if (g.latest_root && other->latest_root) {
                distance = 0;
                for (unsigned i = 0; i < 3; ++i) {
                    const auto d = g.latest_root->position[i] - other->latest_root->position[i];
                    distance += d * d;
                }
            }
            if (!next || distance < nearest) {
                next = other;
                nearest = distance;
            }
        }
        if (!next) continue;
        g.unmet.erase(next->member.id);
        g.next_introduction = now_ + 150000;
        send_required(g, next->cosmetic_packet);
    }
}
void Host::send_roster() {
    auto p = packet(PacketKind::roster, now_);
    p.voice_policy = voice_policy_;
    p.voice_range = config_.voice_range;
    p.chat_badge = parse_colour(config_.chat_color).value_or(multiplayer::default_server_chat_badge);
    p.chat_text = parse_colour(config_.chat_text_color).value_or(multiplayer::default_server_chat_text);
    p.vote = vote_shown_;
    if (vote_ && vote_shown_.id == vote_->id && vote_->ends > now_)
        p.vote.seconds = static_cast<std::uint16_t>(std::min<std::uint64_t>((vote_->ends - now_ + 999999) / 1000000, 65535));
    p.distances = config_.distances;
    p.object_placement = config_.object_placement;
    p.object_limit = config_.object_limit;
    p.object_scaling = config_.object_scaling;
    p.sync_effects = config_.sync_effects;
    p.guest_noclip = config_.noclip;
    p.guest_no_bail = config_.no_bail;
    p.guest_boosts = config_.boosts;
    p.enforce_tuning = config_.enforce_tuning;
    p.server_votes = enabled_votes();
    p.server_polls = static_cast<std::uint8_t>(enabled_polls());
    p.server_custom_votes = custom_votes();
    p.announcement = announcement_;
    if (announcement_.id)
        p.announcement.seconds = static_cast<std::uint16_t>(announcement_until_ > now_ ? (announcement_until_ - now_ + 999999) / 1000000 : 0);
    p.object_clears = object_clears_;
    // Without the catalog (or with sync off) no layers are sent: every player keeps their own.
    p.force_world_layers = config_.world_layer_sync && !world_layers().empty();
    if (p.force_world_layers) p.layers = pack_world_layers(layers_);
    else p.layers.clear();
    p.parks = config_.parks;
    p.capacity = capacity();
    p.members.push_back({id_, epoch_, config_.name, false});
    for (auto &[id, guest] : guests_)
        if (guest->handshaken) {
            guest->member.admin = is_admin(id);
            const auto party = parties_.party_of(id);
            const auto *details = parties_.party(party);
            guest->member.party = party;
            guest->member.party_leader = details && details->leader == id;
            guest->member.party_open = guest->member.party_leader && details->open;
            guest->member.speeding = guest->speeding;
            guest->member.scoring = guest->scoring_flagged;
            p.members.push_back(guest->member);
        }
    // Parties hold only handshaken players (party_request), so this only guards the roster's
    // rules: every listed party has two members and one leader.
    for (auto &m : p.members)
        if (m.party && std::count_if(p.members.begin(), p.members.end(), [&](const Member &o) { return o.party == m.party; }) < 2)
            m.party = 0, m.party_leader = m.party_open = false;
    for (auto &m : p.members)
        if (m.party && std::none_of(p.members.begin(), p.members.end(),
                                    [&](const Member &o) { return o.party == m.party && o.party_leader; }))
            m.party_leader = true; // the first listed member of a party missing its leader
    broadcast(p, true, false);
    roster_dirty_ = false;
    last_roster_ = now_;
}
void Host::send_world_state() {
    auto state = packet(PacketKind::world_state, now_);
    state.destination = map_destination(config_.map);
    state.map_label = wire_map_label();
    state.world_ready = true; // the server has nothing to load
    const auto bytes = encode_wire(state);
    for (auto &[id, guest] : guests_)
        if (guest->handshaken) send_required(*guest, bytes);
    last_world_state_ = now_;
}
void Host::send_chat(std::string_view text, Guest *only) {
    auto message = packet(PacketKind::chat, now_);
    message.text = clean_chat_text(text);
    if (message.text.empty()) return;
    if (only) send_packet(*only, message, true, false);
    else broadcast(message, true, false);
}
void Host::send_bans(Guest &admin) {
    auto list = packet(PacketKind::bans, now_);
    list.ban_total = static_cast<std::uint32_t>(config_.bans.size());
    // Newest first; a very long list sends only its newest rows.
    for (auto ban = config_.bans.rbegin(); ban != config_.bans.rend() && list.bans.size() < max_ban_rows; ++ban) {
        auto row = *ban;
        if (const auto seen = seen_names_.find(row.id); row.name.empty() && seen != seen_names_.end()) row.name = seen->second;
        while (!row.name.empty() && !valid_member_name(row.name)) row.name.pop_back();
        if (row.name.size() > max_member_name) row.name.resize(max_member_name);
        while (!valid_member_name(row.name)) row.name.pop_back();
        if (individual_steam_id(row.id)) list.bans.push_back(std::move(row));
    }
    if (send_packet(admin, list, true, false)) admin.bans_sent = bans_revision_;
}
void Host::send_maps(Guest &guest) { // admins: every map and the pool; players: the pool
    auto list = packet(PacketKind::maps, now_);
    const auto add = [&](const ServerLevel &level) {
        if (valid_map_asset(level.asset) && list.maps.size() < max_server_maps) list.maps.push_back(level.asset);
    };
    if (is_admin(guest.member.id)) {
        for (const auto &level : levels()) add(level);
        if (!config_.map_pool.empty())
            for (const auto *level : pool_levels(config_)) {
                const auto at = std::find(list.maps.begin(), list.maps.end(), level->asset);
                if (at != list.maps.end()) list.map_pool.push_back(static_cast<std::uint16_t>(at - list.maps.begin()));
            }
    } else {
        for (const auto *level : pool_levels(config_)) add(*level);
    }
    list.map_rotation = static_cast<std::uint16_t>(std::min(config_.map_rotation, max_map_rotation));
    if (send_packet(guest, list, true, false)) guest.maps_sent = true;
}
void Host::change_map(std::string_view map) {
    if (!valid_map_destination(map_destination(map_setting(map))))
        throw std::invalid_argument("That map does not name a destination.");
    if (!installed_map(map)) throw std::invalid_argument("This server does not have that map.");
    // Everything tied to the old world goes; admission and player slots stay.
    for (auto &[id, guest] : guests_) {
        auto &g = *guest;
        auto old = std::exchange(g, Guest{});
        g.sender.set_refresh(whole_state_refresh, true); // as for a new player (tick): a new world's streams start over
        g.traffic = old.traffic;
        g.member = std::move(old.member);
        g.handshaken = old.handshaken;
        g.map_authorized = old.map_authorized;
        g.password_challenge = old.password_challenge;
        g.connected_at = old.connected_at;
        // Rate limits and anti-cheat flags belong to the player, not the world: clients report
        // their scoring only once per session. The speed measurement starts over.
        g.budget = old.budget;
        g.chat_rate = old.chat_rate;
        g.admin_budget = old.admin_budget;
        g.throwdown_budget = old.throwdown_budget;
        g.party_budget = old.party_budget;
        g.scoring_budget = old.scoring_budget;
        g.speeding = old.speeding;
        g.scoring = old.scoring;
        g.scoring_mods = std::move(old.scoring_mods);
        g.scoring_flagged = old.scoring_flagged;
        g.world_ready = false;
        g.travel_since = g.handshaken ? now_ : 0;
        g.last_packet = now_;
    }
    if (++world_ == 0) throw std::runtime_error("Map transition counter exhausted.");
    activity_.clear(); // throwdowns end with the world
    config_.map = map_setting(map);
    map_ = map_hash(map_destination(config_.map));
    map_since_ = now_;
    rotation_warned_ = false;
    travel_started_ = now_;
    last_world_state_ = 0;
    send_world_state();
    roster_dirty_ = true;
}
void Host::drop(std::uint64_t id, const std::string &reason, const std::string &detail) {
    transport_.disconnect(id, reason.c_str());
    const auto found = guests_.find(id);
    if (found == guests_.end()) return;
    const auto name = guest_name(*found->second);
    if (found->second->handshaken) {
        roster_dirty_ = true;
        log_(name + " left (" + reason + ")" + (detail.empty() ? std::string{} : " [" + detail + "]"));
        activity_.left(id);
    } else {
        // Never admitted: it held a player slot meanwhile, so it shows in the log, and an ID
        // that keeps failing waits longer each time before its connection is taken again.
        const auto failures = join_backoff_.failed(id, now_);
        log_("[join] " + std::to_string(id) + " did not finish joining (" + reason + ")" +
             (failures > 1 ? ", attempt " + std::to_string(failures) : std::string{}) +
             (detail.empty() ? std::string{} : " [" + detail + "]"));
    }
    guests_.erase(found);
    for (auto &[other, guest] : guests_) {
        guest->pose_sender.forget(id);
        guest->sound_sender.forget(id);
    }
    party_left(id, name);
    // Their vote cooldown stays (tick() drops it once it has run out): leaving and coming
    // back does not let a player start another vote sooner.
    // One voter fewer, or the player a kick vote was about. Counted in tick(): a passing kick
    // vote drops another player, which must not happen inside a caller's walk over guests_.
    vote_recount_ = true;
}

// ---- Receiving -------------------------------------------------------------------------------
bool Host::accept_data(Guest &source, const Packet &p) {
    bool accepted{};
    if (p.kind == PacketKind::cosmetics) {
        accepted = source.outfit_budget.accept(now_) && source.appearance.push(p);
        if (accepted) source.cosmetic_packet = encode_wire(p);
    } else if (p.kind == PacketKind::audio)
        accepted = source.sound_budget.accept(now_, p.audio.size()) && source.audio.push(p, now_);
    else if (p.kind == PacketKind::effects)
        accepted = config_.sync_effects && source.effect_budget.accept(now_);
    else if (p.kind == PacketKind::pose)
        accepted = source.poses.push_validated(p, now_);
    // The server never plays anything back: keep only what ordering needs.
    if (source.poses.size() > 4 || source.audio.size() > 16) {
        source.poses.clear();
        source.audio.clear();
    }
    if (!accepted) return false;
    source.last_packet = now_;
    if (p.kind == PacketKind::pose) {
        source.pose_arrival = now_;
        source.latest_root = p.pose.root;
        {
            const auto &root = p.pose.root, &was = source.still_at;
            float moved{}, facing{};
            for (unsigned i = 0; i < 3; ++i) moved += (root.position[i] - was.position[i]) * (root.position[i] - was.position[i]);
            for (unsigned i = 0; i < 4; ++i) facing += root.rotation[i] * was.rotation[i];
            if (!source.moved_at || moved > 0.05f * 0.05f || std::abs(facing) < 0.9995f) {
                source.still_at = root;
                source.moved_at = now_;
                active(source);
            }
        }
        return check_speed(source, p.time_us); // last: a speed-check kick frees `source`
    }
    return true;
}
// A player's own poses (pose_batch.h): each is rebuilt from one of theirs held here and taken as
// the packet it would have been. A message read in full is acked (flush_poses); one with a pose
// that could not be rebuilt is not, so their game goes on building on what is held here.
void Host::receive_poses(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at) {
    auto *link = find(peer);
    if (!link || !link->handshaken) return;
    const auto batch = pose_batch::read(bytes);
    if (!batch || batch->world != static_cast<std::uint32_t>(world_) || batch->map != static_cast<std::uint32_t>(map_)) return;
    bool complete = true, first = true;
    for (const auto &entry : batch->entries) {
        if (entry.whole) {
            // Only themselves, and one stream at a time: a new one replaces the last.
            if (entry.source != peer || entry.epoch != link->member.epoch) {
                complete = false;
                continue;
            }
            if (!link->upload_streams.contains(entry.stream)) link->upload_streams.clear();
        }
        const auto rebuilt = pose_batch::rebuild(link->upload_streams, entry);
        if (!rebuilt) {
            complete = false;
            continue;
        }
        if (rebuilt->repeat) continue;
        Packet p;
        p.kind = PacketKind::pose;
        p.session = secret_;
        p.map = map_;
        p.world = world_;
        p.source = peer;
        p.epoch = rebuilt->epoch;
        p.sequence = rebuilt->sequence;
        p.time_us = rebuilt->time_us;
        p.player_collision = rebuilt->collision;
        p.pose = pose_codec::restore(rebuilt->pose);
        // The message's bytes are counted once, with its first pose.
        receive(peer, first ? bytes : bytes.first(0), received_at, &p);
        first = false;
        link = find(peer); // a pose can be what gets its player removed
        if (!link) return;
    }
    if (complete) {
        link->upload_ack.note(batch->number);
        link->upload_ack_due = true;
    }
}
// A player's own skater's sound (sound_codec.h), taken as the packet it would have been.
void Host::receive_sound(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at) {
    auto *link = find(peer);
    if (!link || !link->handshaken) return;
    if (link->sound_in.size() > 2) link->sound_in.clear(); // only their own, one stream at a time
    const auto heard = sound_codec::read(link->sound_in, bytes, world_, map_);
    if (!heard) return;
    bool first = true;
    for (const auto &one : *heard) {
        if (one.source != peer || one.epoch != link->member.epoch || !valid_audio_batch(one.samples)) continue;
        Packet p;
        p.kind = PacketKind::audio;
        p.session = secret_;
        p.map = map_;
        p.world = world_;
        p.source = peer;
        p.epoch = one.epoch;
        p.sequence = one.sequence;
        p.time_us = one.time_us;
        p.audio = one.samples;
        receive(peer, first ? bytes : bytes.first(0), received_at, &p);
        first = false;
        link = find(peer);
        if (!link) return;
    }
}
void Host::receive(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at, const Packet *ready) {
    auto *link = find(peer);
    if (!link) return;
    if (!ready) {
        // Counted by arrival: after the server was held up, everyone's packets are read at once.
        if (!link->budget.accept(received_at ? received_at : now_, bytes.size(), 1U))
            return drop(peer, "Peer exceeded the multiplayer packet limit.");
        // Which messages of poses they read (pose_batch.h): not a packet, and says nothing else.
        if (const auto ack = pose_batch::Ack::read(bytes)) {
            if (link->handshaken) pose_ack(*link, *ack);
            return;
        }
        if (pose_batch::is_batch(bytes)) return receive_poses(peer, bytes, received_at);
        if (sound_codec::is_sound(bytes)) return receive_sound(peer, bytes, received_at);
    }
    bool missing_reference{};
    const auto decoded = ready ? std::optional<Packet>(*ready) : link->receiver.receive(bytes, missing_reference, world_);
    if (missing_reference) return;
    if (!decoded || decoded->session != secret_ || decoded->source != peer)
        return drop(peer, "Join code, sender identity, or multiplayer protocol did not match.");
    const auto &p = *decoded;
    link->traffic.total.in[traffic_kind(p.kind)] += bytes.size();
    traffic_.total.in[traffic_kind(p.kind)] += bytes.size();
    switch (p.kind) {
    case PacketKind::world_state: return drop(peer, "Only the server may change the room's map.");
    // Sent once the player's game knows, and again if a mod enabled later changes it; not tied to
    // a world, so a report crossing a map change still counts.
    case PacketKind::scoring: {
        if (!link->handshaken || p.epoch != link->member.epoch || !link->scoring_budget.accept(now_, 4)) return;
        link->last_packet = now_;
        if (link->scoring == p.scoring && link->scoring_mods == p.text) return;
        link->scoring = p.scoring;
        link->scoring_mods = p.text;
        check_scoring(*link);
        return;
    }
    case PacketKind::world_ready: {
        if (!link->handshaken || p.epoch != link->member.epoch) return drop(peer, "Invalid map readiness message.");
        if (p.world != world_ || p.map != map_ || (link->ready_sequence && !newer_sequence(p.sequence, link->ready_sequence)))
            return;
        link->ready_sequence = p.sequence;
        link->last_packet = now_;
        const bool arrived = p.world_ready && !link->world_ready;
        if (!p.world_ready && link->world_ready) link->loading_since = now_;
        if (arrived && config_.activity_log)
            if (const auto since = link->travel_since ? link->travel_since : link->loading_since; since && now_ > since)
                log_("[map] " + guest_name(*link) + " finished loading (" + std::to_string((now_ - since) / 1000000) + " s)");
        link->world_ready = p.world_ready;
        if (p.world_ready) link->travel_since = link->loading_since = 0;
        if (arrived) {
            meet_later(*link);
            active(*link); // the time away starts once they are in
        }
        return;
    }
    case PacketKind::map_request: {
        if (p.build != supported_build::game_sha256_bytes || (link->member.epoch && link->member.epoch != p.epoch))
            return drop(peer, "Invalid map request. Update ReSkate to the server's version and join again.");
        if (link->handshaken) return;
        if (p.map && p.map != map_) return drop(peer, "The server changed maps while you were joining. Join again.");
        link->member.epoch = p.epoch;
        bool authorized = !password_;
        if (password_) {
            if (!link->password_challenge) link->password_challenge = nonce();
            if (p.challenge == link->password_challenge) {
                const auto proof = password_proof(*password_, secret_, map_, id_, peer, epoch_, p.epoch, link->password_challenge);
                if (!proof_matches(proof, p.proof)) return drop(peer, "Incorrect server password.");
                authorized = true;
            }
        }
        const bool newly_authorized = authorized && !link->map_authorized;
        link->map_authorized |= authorized;
        if (newly_authorized || !link->last_map_offer || now_ - link->last_map_offer >= 1000000) {
            auto offer = packet(PacketKind::map_offer, now_);
            offer.destination = map_destination(config_.map);
            offer.map_label = wire_map_label();
            offer.challenge = link->password_challenge;
            offer.map_authorized = authorized;
            send_required(*link, encode_wire(offer));
            link->last_map_offer = now_;
        }
        return;
    }
    case PacketKind::map_offer:
        if (link->handshaken || p.world < world_) return;
        return drop(peer, "Only the server offers maps.");
    default: break;
    }
    // A map reload can return to the same asset. The generation keeps late
    // poses, audio, cosmetics and control from reviving the old world.
    if (p.world != world_) return;
    switch (p.kind) {
    case PacketKind::peer_hello:
    case PacketKind::peer_welcome: return drop(peer, "Direct peer is not admitted to this session.");
    case PacketKind::challenge: return drop(peer, "Invalid lobby password challenge.");
    case PacketKind::welcome: return drop(peer, "Unexpected multiplayer handshake direction.");
    case PacketKind::hello: {
        const auto error = greeting_error(p, secret_, map_, supported_build::game_sha256_bytes,
                                          link->handshaken ? link->member.epoch : 0);
        if (!error.empty()) return drop(peer, std::string(error));
        if (password_ && !link->handshaken) {
            if (!link->password_challenge) link->password_challenge = nonce();
            if (p.challenge != link->password_challenge) {
                auto challenge = packet(PacketKind::challenge, now_);
                challenge.challenge = link->password_challenge;
                send_required(*link, encode_wire(challenge));
                return;
            }
            const auto proof = password_proof(*password_, secret_, map_, id_, peer, epoch_, p.epoch, link->password_challenge);
            if (!proof_matches(proof, p.proof)) return drop(peer, "Incorrect server password.");
        }
        const bool joined = !link->handshaken;
        link->member.epoch = p.epoch;
        if (joined) {
            link->member.name = player_name(p.text, peer);
            if (seen_names_.size() >= 4096) seen_names_.erase(seen_names_.begin());
            seen_names_[peer] = link->member.name;
            join_backoff_.joined(peer);
        }
        link->handshaken = true;
        link->world_ready = true;
        link->travel_since = 0;
        link->last_packet = now_;
        send_required(*link, encode_wire(packet(PacketKind::welcome, now_)));
        if (joined) {
            send_roster();
            meet_later(*link);
            if (!config_.welcome.empty()) send_chat(config_.welcome, link);
            log_(guest_name(*link) + " joined (" + std::to_string(peer) + (is_admin(peer) ? ", admin" : "") + "), " +
                 std::to_string(players()) + "/" + std::to_string(config_.max_players) + " players" +
                 (link->connected_at && now_ > link->connected_at
                      ? ", loaded in " + std::to_string((now_ - link->connected_at) / 1000000) + " s" : ""));
        }
        return;
    }
    case PacketKind::cosmetics: {
        auto &pending = link->pending_cosmetics;
        std::erase_if(pending, [&](const auto &item) { return now_ - item.received > 10000000; });
        const auto found = std::find_if(pending.begin(), pending.end(), [&](const auto &item) {
            return item.packet.source == p.source && item.packet.epoch == p.epoch;
        });
        if (found != pending.end()) {
            if (newer_sequence(p.sequence, found->packet.sequence)) *found = {p, now_};
        } else {
            if (pending.size() >= 4) pending.erase(pending.begin());
            pending.push_back({p, now_});
        }
        return;
    }
    default: break;
    }
    if (!link->handshaken || p.map != map_) return;
    if (p.kind == PacketKind::routes) {
        if (p.epoch != link->member.epoch) return drop(peer, "Invalid direct route report.");
        if (!link->route_reported || newer_sequence(p.sequence, link->route_sequence)) {
            link->direct_routes.clear();
            for (const auto &m : p.members) {
                const auto *other = find(m.id);
                if (other && other->handshaken && other->member.epoch == m.epoch && m.id != peer)
                    link->direct_routes.push_back(m);
            }
            link->route_reported = now_;
            link->route_sequence = p.sequence;
        }
        return;
    }
    if (p.kind == PacketKind::roster) return drop(peer, "Only the server may publish the player roster.");
    if (p.kind == PacketKind::teleport) return drop(peer, "Only the server may teleport players.");
    // A listen host's; the server's physics are the game's own.
    if (p.kind == PacketKind::physics_tuning || p.kind == PacketKind::physics_extras) return;
    if (p.kind == PacketKind::chat) {
        active(*link);
        if (!routed_source(p, link->member, peer, true, id_) ||
            link->chat_rate.accept(now_, p.text, 1) != ChatRate::Verdict::accepted)
            return;
        link->last_packet = now_;
        // "/" starts a command (votes; any server command for admins), answered to the sender only.
        if (p.text.front() == '/') {
            log_("[command] " + guest_name(*link) + ": /" + loggable(std::string_view(p.text).substr(1)));
            chat_command(*link, std::string_view(p.text).substr(1));
            return;
        }
        log_("[chat] " + guest_name(*link) + ": " + p.text);
        broadcast(p, true, false, p.source);
        return;
    }
    // Linked throwdowns (Extension/Throwdowns/throwdown_relay.cpp): opaque to the
    // server, relayed like chat to everyone else in the same world.
    if (p.kind == PacketKind::throwdown) {
        if (p.world != world_ || !routed_source(p, link->member, peer, true, id_) || !link->throwdown_budget.accept(now_, 60))
            return;
        // A player whose tricks score differently takes part in nothing linked: their scores,
        // attempts and drops go nowhere, whatever the other players' games would do with them.
        if (link->scoring_flagged) return;
        link->last_packet = now_;
        if (config_.activity_log || config_.announce_throwdowns)
            activity_.throwdown(peer, p.throwdown, link->latest_root ? &link->latest_root->position : nullptr, now_);
        broadcast(p, true, false, p.source);
        return;
    }
    if (p.kind == PacketKind::party) {
        if (!routed_source(p, link->member, peer, true, id_) || !link->party_budget.accept(now_, 20)) return;
        link->last_packet = now_;
        party_request(*link, p.party_action, p.party_player);
        return;
    }
    if (p.kind == PacketKind::admin) {
        if (!routed_source(p, link->member, peer, true, id_) || !link->admin_budget.accept(now_, 30)) return;
        link->last_packet = now_;
        std::string answer;
        if (!is_admin(peer)) answer = "You are not an admin on this server.";
        else {
            log_("[admin] " + guest_name(*link) + ": " + loggable(p.text));
            answer = command(p.text, peer);
        }
        if (auto *still = find(peer)) {
            auto reply = packet(PacketKind::admin, now_);
            reply.text = clean_chat_text(answer.empty() ? std::string("Done.") : answer);
            if (reply.text.empty()) reply.text = "Done.";
            send_packet(*still, reply, true, false);
        }
        return;
    }
    if (!link->world_ready) return;
    if (!routed_source(p, link->member, peer, true, id_)) return;
    if (p.kind == PacketKind::away) return drop(peer, "A player ended their session.");
    if (p.kind == PacketKind::objects) {
        const auto before = link->objects.revision();
        if (link->objects.receive(p.objects) == ObjectState::Result::invalid)
            return drop(peer, "Invalid shared object revision or layout.");
        if (link->objects.revision() != before) active(*link);
        link->last_packet = now_;
        return;
    }
    if (p.kind == PacketKind::voice) {
        if (!voice_policy_.accepts(p.voice)) return;
        if (link->received_voice && !newer_sequence(p.sequence, link->voice_sequence)) return;
        if (!link->voice_budget.accept(now_, p.voice.bytes.size())) return;
        link->received_voice = true;
        link->voice_sequence = p.sequence;
        link->last_packet = now_;
        active(*link);
        broadcast(p, false, true, p.source);
        return;
    }
    if (!accept_data(*link, p)) return;
    // A pose waits until the pass has read everything (relay_poses).
    if (p.kind == PacketKind::pose) {
        link->relay_poses.push_back(p);
        return;
    }
    // A contact's effect is only worth showing as it happens: with the server behind, a
    // player's backlog of them is not passed on.
    if (p.kind == PacketKind::effects && ++link->effects_pass > 2) {
        ++shed_;
        return;
    }
    broadcast(p, p.kind == PacketKind::audio && std::any_of(p.audio.begin(), p.audio.end(),
                                                            [](const auto &sample) { return sample.event; }),
              true, p.source);
}
// The poses read this pass go on to everyone. A pass normally reads one or two of each player's.
// More means the server is behind (a stall, or more players than it can carry), and passing
// every one of them on is what kept it behind: each pass then had the whole of the last
// pass's arrivals to send, took as long again, and sent poses whole because the ones they
// would have built on had gone stale, which is more work still. So of a player's backlog only
// the newest goes on. Their skater is where they are now; the others see fewer poses of them
// for a moment, which their games already smooth over.
void Host::relay_poses() {
    std::vector<Packet> poses;
    for (auto &[id, guest] : guests_) {
        guest->effects_pass = 0;
        if (guest->relay_poses.empty()) continue;
        auto own = std::exchange(guest->relay_poses, {});
        const std::size_t first = own.size() > 3 ? own.size() - 1 : 0;
        shed_ += first;
        for (std::size_t index = first; index < own.size(); ++index) poses.push_back(std::move(own[index]));
    }
    for (const auto &pose : poses) broadcast(pose, false, true, pose.source);
    if (shed_ != shed_logged_ && now_ - shed_log_at_ >= 30000000) {
        log_("[network] The server is behind: " + std::to_string(shed_ - shed_logged_) +
             " poses and effects that arrived late were not passed on. Players see each other at a lower rate until it catches up" +
             (shed_log_at_ ? "" : "; if this keeps coming, the server has more players than its CPU can carry") + ".");
        shed_logged_ = shed_;
        shed_log_at_ = now_;
    }
}
void Host::receive_cosmetics() {
    for (auto &[id, guest] : guests_) {
        auto &link = *guest;
        auto pending = std::exchange(link.pending_cosmetics, {});
        for (auto &item : pending) {
            const auto &p = item.packet;
            if (p.world != world_ || now_ - item.received > 10000000) continue;
            // Lanes may deliver an outfit before the hello or readiness; keep it until then.
            if (!link.handshaken || !link.world_ready) {
                link.pending_cosmetics.push_back(std::move(item));
                continue;
            }
            if (p.map == map_ && routed_source(p, link.member, id, true, id_) && accept_data(link, p))
                broadcast(p, true, false, p.source);
        }
    }
}

// Players who have been away longer than the server allows ("afk_kick_minutes") are removed,
// with a warning a minute before. Away is doing nothing a player at their game does: not
// moving, speaking, typing in chat or changing their objects. Admins stay, and so does anyone
// whose game is still loading the map.
void Host::remove_away() {
    if (!config_.afk_kick) return;
    const auto limit = static_cast<std::uint64_t>(config_.afk_kick) * 60000000;
    std::vector<std::uint64_t> away;
    for (auto &[id, guest] : guests_) {
        auto &g = *guest;
        if (!g.handshaken || !g.world_ready || !g.active_at || is_admin(id)) continue;
        const auto idle = now_ - g.active_at;
        if (idle >= limit) away.push_back(id);
        else if (!g.away_warned && limit > 60000000 && idle >= limit - 60000000) {
            g.away_warned = true;
            reply(g, "You have been away a while: move or say something within a minute to stay on the server.");
        }
    }
    for (const auto id : away) {
        if (const auto *g = find(id)) log_("[afk] " + guest_name(*g) + " was removed after " + std::to_string(config_.afk_kick) + " min away.");
        drop(id, "You were removed from the server for being away too long. You can join again.");
    }
}

// ---- Objects ---------------------------------------------------------------------------------
void Host::sync_objects() {
    if (now_ < next_object_update_) return;
    next_object_update_ = now_ + 100000;
    // Guests upload their layouts; the server alone decides what everyone else
    // sees, and freezes the layouts of players who may not build right now.
    for (auto &[id, guest] : guests_)
        if (guest->handshaken && guest->objects.revision() != guest->shared_from && now_ >= guest->objects_held_until &&
            (config_.object_placement == ObjectPlacement::everyone ||
             (config_.object_placement == ObjectPlacement::host_only && is_admin(id)))) {
            auto layout = guest->objects.layout();
            std::erase_if(layout, [&](const auto &object) { return guest->cleared.contains(object.id); });
            // No more of a player's objects than the server allows each of them; admins are not limited.
            if (!is_admin(id)) layout = limited_layout(std::move(layout), guest->shared.objects(), config_.object_limit);
            // Nobody places objects by the hundred, minute after minute: a game that does is
            // spawning and removing them to animate them. Theirs are deleted for everyone, and
            // nothing they place is shared for a minute (or they would be back at once).
            if (!is_admin(id)) {
                if (now_ - guest->placed_since >= 60000000) {
                    guest->placed_since = now_;
                    guest->placed = 0;
                }
                for (const auto &object : layout) guest->placed += !guest->shared.objects().contains(object.id);
                const auto burst = 2 * (config_.object_limit ? config_.object_limit : max_owned_objects) + 100;
                if (guest->placed > burst) {
                    guest->objects_held_until = now_ + 60000000;
                    guest->placed_since = guest->objects_held_until;
                    guest->placed = 0;
                    const auto deleted = guest->shared.objects().size();
                    for (const auto *state : {&guest->objects, &guest->shared})
                        for (const auto &[object, value] : state->objects()) {
                            (void)value;
                            guest->cleared.insert(object);
                        }
                    if (guest->shared.revision()) guest->shared.replace({});
                    guest->shared_from = guest->objects.revision();
                    log_("[objects] " + guest_name(*guest) + " (" + std::to_string(id) + ") placed over " + std::to_string(burst) +
                         " objects in a minute: their " + std::to_string(deleted) + " objects were deleted, and none of theirs are shared for a minute.");
                    reply(*guest, "You placed objects faster than the server allows. Yours were deleted for everyone, and none you place are shared for a minute.");
                    continue;
                }
            }
            // With scaling off, a player's objects reach everyone else at their own size, whatever
            // that player's game made of them.
            if (!config_.object_scaling && !is_admin(id))
                for (auto &object : layout) object.scale = 1.f;
            if (config_.activity_log) activity_.objects(id, guest->shared.layout(), layout);
            guest->shared.replace(layout);
            guest->shared_from = guest->objects.revision();
        }
    struct Source { std::uint64_t id, epoch; const ObjectState *state; };
    std::vector<Source> sources;
    for (const auto &[id, guest] : guests_)
        if (guest->handshaken && guest->world_ready) sources.push_back({id, guest->member.epoch, &guest->shared});
    for (auto &[id, guest] : guests_) {
        auto &peer = *guest;
        if (!peer.handshaken || !peer.world_ready || sources.empty()) continue;
        // Objects wait for a connection that is behind: they must arrive, so they would only
        // queue up behind the poses and push those out.
        if (transport_.pending(id) > 128 * 1024) continue;
        auto &delivery = peer.object_delivery;
        const auto find_source = [&](std::uint64_t source, std::uint64_t epoch) {
            return std::find_if(sources.begin(), sources.end(), [&](const auto &s) { return s.id == source && s.epoch == epoch; });
        };
        std::erase_if(delivery.sent, [&](const auto &row) { return find_source(row.first, row.second.first) == sources.end(); });
        if (!delivery.chunks.empty() && find_source(delivery.source, delivery.epoch) == sources.end()) delivery.chunks.clear();
        for (unsigned budget = 0; budget < 4; ++budget) {
            if (delivery.chunks.empty()) {
                for (std::size_t attempt = 0; attempt < sources.size(); ++attempt) {
                    const auto &source = sources[delivery.cursor++ % sources.size()];
                    if (source.id == id || !source.state->revision()) continue;
                    const auto previous = delivery.sent.find(source.id);
                    const auto since = previous != delivery.sent.end() && previous->second.first == source.epoch
                                           ? previous->second.second : 0;
                    delivery.chunks = source.state->updates(since);
                    if (delivery.chunks.empty()) continue;
                    delivery.source = source.id;
                    delivery.epoch = source.epoch;
                    delivery.next = 0;
                    break;
                }
                if (delivery.chunks.empty()) break;
            }
            auto update = packet(PacketKind::objects, now_);
            update.source = delivery.source;
            update.epoch = delivery.epoch;
            update.objects = delivery.chunks[delivery.next];
            if (!send_packet(peer, update, true, false)) {
                // Refused for now (a full queue): the same part is tried again. Only a
                // connection that takes none for a long while is given up on.
                if (!delivery.failing_since) delivery.failing_since = now_;
                if (now_ - delivery.failing_since > 30000000)
                    transport_.disconnect(id, "Cannot deliver shared object state. Join again.");
                break;
            }
            delivery.failing_since = 0;
            if (++delivery.next == delivery.chunks.size()) {
                delivery.sent[delivery.source] = {delivery.epoch, update.objects.revision};
                delivery.chunks.clear();
            }
        }
    }
}

// `net`: how the connections and the server's own loop are doing right now. With a player's
// name or SteamID64, that player alone. In-game admins get the first line: a reply has to fit
// one chat-sized message.
std::string Host::network_report(std::string_view player, bool console) {
    const auto links = transport_.links();
    const auto &net = transport_.status();
    const auto kb = [](float bytes) { return std::to_string(static_cast<int>(bytes / 1024.f + .5f)); };
    const auto percent = [](float quality) { return quality < 0 ? std::string("?") : std::to_string(static_cast<int>(quality * 100.f + .5f)) + "%"; };
    const auto row = [&](const multiplayer::TransportLink &link) {
        const auto *guest = find(link.id);
        std::string name = guest ? guest_name(*guest) : std::to_string(link.id);
        if (name.size() > 20) name.resize(20);
        name.resize(21, ' ');
        if (!link.measured) return "  " + name + (link.connected ? "not measured yet" : "connecting");
        const auto silent = guest && guest->last_packet && now_ > guest->last_packet ? (now_ - guest->last_packet) / 100000 : 0; // tenths of a second
        return "  " + name + "ping " + std::to_string(link.ping_ms) + " ms, quality " + percent(link.quality_local) + " here / " +
               percent(link.quality_remote) + " there, queued " + std::to_string(link.pending_bytes) + " B (" +
               std::to_string(link.queue_us / 1000) + " ms), out " + kb(link.out_bps) + " of " + kb(static_cast<float>(link.send_rate)) +
               " KB/s, in " + kb(link.in_bps) + " KB/s, last heard " + std::to_string(silent / 10) + "." + std::to_string(silent % 10) + " s ago" +
               (guest && !guest->handshaken ? ", joining" : guest && !guest->world_ready ? ", loading" : "");
    };
    // By what it carries, in KB/s: the last whole half minute, or the one so far.
    const bool whole = traffic_window_us_ != 0;
    const double seconds = whole ? static_cast<double>(traffic_window_us_) / 1e6
                                 : std::max(1.0, static_cast<double>(now_ - traffic_mark_) / 1e6);
    const auto rate = [&](const Counted &c, bool out, std::size_t kind) {
        const auto bytes = whole ? (out ? c.last.out[kind] : c.last.in[kind])
                                 : out ? c.total.out[kind] - c.mark.out[kind] : c.total.in[kind] - c.mark.in[kind];
        return static_cast<double>(bytes) / 1024.0 / seconds;
    };
    const auto number = [](double value) {
        char text[32];
        std::snprintf(text, sizeof(text), value < 10 ? "%.1f" : "%.0f", value);
        return std::string(text);
    };
    const auto by_kind = [&](const Counted &c) {
        std::string text = "KB/s out / in:";
        for (std::size_t kind = 0; kind < traffic_names.size(); ++kind)
            text += std::string(kind ? ", " : " ") + traffic_names[kind] + " " + number(rate(c, true, kind)) + " / " + number(rate(c, false, kind));
        const auto snapshots = whole ? c.last.snapshots : c.total.snapshots - c.mark.snapshots;
        return text + "; " + number(static_cast<double>(snapshots) / seconds) + " whole states a second";
    };
    if (!player.empty()) {
        const auto wanted = lower(player);
        for (const auto &link : links) {
            const auto *guest = find(link.id);
            if (std::to_string(link.id) == player || (guest && lower(guest_name(*guest)).starts_with(wanted)))
                return row(link).substr(2) + (guest ? "\n" + by_kind(guest->traffic) : std::string{}) +
                       (link.direct ? std::string("\nroute: direct, not through Steam's relays")
                                    : "\nroute: through Steam's relay " + (link.relay.empty() ? std::string("(unknown)") : link.relay) +
                                          " on this side and " + (link.remote_relay.empty() ? std::string("(unknown)") : link.remote_relay) + " on theirs");
        }
        return "No connected player matches \"" + std::string(player) + "\".";
    }
    float out{}, in{};
    std::uint64_t queued{}, longest_queue{};
    std::size_t waiting{}, poor{};
    int worst_ping{};
    for (const auto &link : links) {
        if (!link.measured) continue;
        out += link.out_bps;
        in += link.in_bps;
        queued += static_cast<std::uint64_t>(std::max(0, link.pending_bytes));
        longest_queue = std::max(longest_queue, link.queue_us);
        waiting += link.queue_us > 50000;
        poor += (link.quality_local >= 0 && link.quality_local < .9f) || (link.quality_remote >= 0 && link.quality_remote < .9f);
        worst_ping = std::max(worst_ping, link.ping_ms);
    }
    std::string text = std::to_string(players()) + " players (" + std::to_string(links.size()) + " connections), " + std::to_string(config_.tps) +
                       " TPS | " + kb(out) + " KB/s out, " + kb(in) + " KB/s in | worst ping " + std::to_string(worst_ping) + " ms | " +
                       std::to_string(queued / 1024) + " KB queued, longest wait " + std::to_string(longest_queue / 1000) + " ms, " +
                       std::to_string(waiting) + " waiting over 50 ms | " + std::to_string(poor) + " under 90% quality";
    if (!console) return text;
    {
        // The relay locations in use, most connections first: everyone through one far away is a route problem.
        std::map<std::string, unsigned> relays;
        for (const auto &link : links)
            if (link.measured)
                ++relays[link.direct ? std::string("direct")
                                     : (link.relay.empty() ? "?" : link.relay) + "-" + (link.remote_relay.empty() ? "?" : link.remote_relay)];
        std::vector<std::pair<std::string, unsigned>> order(relays.begin(), relays.end());
        std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        text += "\nSteam relay network: " + transport_.relay_status();
        text += "\nrelays (ours-theirs):";
        for (std::size_t i = 0; i < std::min<std::size_t>(order.size(), 10); ++i)
            text += " " + order[i].first + " x" + std::to_string(order[i].second);
    }
    text += "\nloop: " + loop_report();
    text += "\n" + by_kind(traffic_);
    {
        std::uint64_t poses{};
        for (const auto count : pose_sizes_.sent) poses += count;
        if (poses) {
            text += "\npose sizes:";
            for (std::size_t i = 0; i < pose_sizes_.sent.size(); ++i)
                if (pose_sizes_.sent[i])
                    text += std::string(" ") + pose_rate_names[i] + " " + std::to_string(pose_sizes_.sent_bytes[i] / pose_sizes_.sent[i]) + " B x" +
                            std::to_string(pose_sizes_.sent[i]);
            text += "; " + std::to_string(pose_sizes_.held * 100 / poses) + "% without fingers; whole poses " + std::to_string(pose_sizes_.whole_sent) +
                    (pose_sizes_.whole_sent ? " at " + std::to_string(pose_sizes_.whole_sent_bytes / pose_sizes_.whole_sent) + " B" : std::string{});
        }
    }
    {
        // Who uploads most, and what most of it is: one player's outfit or objects can cost
        // every other player's connection.
        struct Upload { const Guest *guest; double total; std::size_t most; };
        std::vector<Upload> uploads;
        for (const auto &[id, guest] : guests_) {
            Upload upload{guest.get(), 0, 0};
            for (std::size_t kind = 0; kind < traffic_names.size(); ++kind) {
                upload.total += rate(guest->traffic, false, kind);
                if (rate(guest->traffic, false, kind) > rate(guest->traffic, false, upload.most)) upload.most = kind;
            }
            uploads.push_back(upload);
        }
        std::stable_sort(uploads.begin(), uploads.end(), [](const auto &a, const auto &b) { return a.total > b.total; });
        if (!uploads.empty()) text += "\nmost uploaded:";
        for (std::size_t i = 0; i < std::min<std::size_t>(uploads.size(), 6); ++i)
            text += std::string(i ? ", " : " ") + guest_name(*uploads[i].guest) + " " + number(uploads[i].total) + " KB/s (" +
                    traffic_names[uploads[i].most] + " " + number(rate(uploads[i].guest->traffic, false, uploads[i].most)) + ")";
    }
    text += "\nshed while behind: " + std::to_string(shed_) + " poses and effects not passed on (since the server started)";
    text += "\nsends: " + std::to_string(net.send_failures) + " failed, " + std::to_string(net.skipped) + " skipped, " +
            std::to_string(net.dropped) + " dropped (since the server started)";
    // The connections doing worst first: a long queue, poor delivery, silence, then ping.
    auto order = links;
    const auto badness = [&](const multiplayer::TransportLink &link) {
        const auto *guest = find(link.id);
        const double silent = guest && guest->handshaken && now_ > guest->last_packet ? static_cast<double>(now_ - guest->last_packet) / 1000.0 : 0;
        const double lost = 1.0 - std::min(link.quality_local < 0 ? 1.f : link.quality_local, link.quality_remote < 0 ? 1.f : link.quality_remote);
        return static_cast<double>(link.queue_us) / 1000.0 + lost * 2000.0 + silent + link.ping_ms;
    };
    std::stable_sort(order.begin(), order.end(), [&](const auto &a, const auto &b) { return badness(a) > badness(b); });
    const std::size_t shown = std::min<std::size_t>(order.size(), 12);
    if (shown) text += "\nworst " + std::to_string(shown) + " of " + std::to_string(order.size()) + " (net <player> for one):";
    for (std::size_t i = 0; i < shown; ++i) text += "\n" + row(order[i]);
    return text;
}

// ---- Tick ------------------------------------------------------------------------------------
namespace {
std::uint64_t clock_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace
std::string Host::loop_report() const {
    // The last whole minute when there is one, or the minute so far.
    const auto &l = last_loop_.passes ? last_loop_ : loop_;
    const auto seconds = std::max<std::uint64_t>(1, last_loop_.passes ? 60 : (clock_us() - l.since) / 1000000);
    const auto ms = [](std::uint64_t us) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(us) / 1000.0);
        return std::string(text);
    };
    return std::to_string(l.passes / seconds) + " passes/s, " +
           std::to_string(l.passes ? l.busy_us * 100 / (seconds * 1000000) : 0) + "% busy, longest pass " + ms(l.longest_pass_us) +
           " ms, longest gap " + ms(l.longest_gap_us) + " ms (worst since start: " + ms(worst_loop_.longest_pass_us) + " ms pass, " +
           ms(worst_loop_.longest_gap_us) + " ms gap)";
}
void Host::tick(std::uint64_t now) {
    if (!running_) return;
    now_ = now;
    // Timed on a clock of its own, from the start of one pass to the start of the next.
    struct Timed {
        Host &host;
        std::uint64_t started = clock_us();
        explicit Timed(Host &h) : host(h) {
            auto &l = host.loop_;
            if (!l.since) l.since = started;
            if (host.pass_started_) {
                const auto gap = started - host.pass_started_;
                l.longest_gap_us = std::max(l.longest_gap_us, gap);
                host.worst_loop_.longest_gap_us = std::max(host.worst_loop_.longest_gap_us, gap);
            }
            host.pass_started_ = started;
        }
        ~Timed() {
            auto &l = host.loop_;
            const auto took = clock_us() - started;
            ++l.passes;
            l.busy_us += took;
            l.longest_pass_us = std::max(l.longest_pass_us, took);
            host.worst_loop_.longest_pass_us = std::max(host.worst_loop_.longest_pass_us, took);
            if (started - l.since >= 60000000) {
                host.last_loop_ = l;
                l = {};
            }
        }
    } timed{*this};
    transport_.poll();
    const auto links = transport_.status().peers;
    const auto linked = [&](std::uint64_t id) {
        return std::any_of(links.begin(), links.end(), [id](const auto &v) { return v.id == id; });
    };
    for (auto it = guests_.begin(); it != guests_.end();) {
        const auto id = (it++)->first;
        if (linked(id)) continue;
        // Steam closed it: the log says how (the player's game closing it, or the connection
        // failing at one end or the other), which "Disconnected." alone does not.
        const auto how = transport_.take_closed(id);
        drop(id, "Disconnected.", how);
    }
    for (const auto &link : links) {
        if (kicked_.contains(link.id)) {
            transport_.disconnect(link.id, "You were kicked from this server.");
            continue;
        }
        if (is_banned(link.id)) {
            transport_.disconnect(link.id, "You are banned from this server.");
            continue;
        }
        // The backend's list (global_bans.h), which can reach a player who is already on.
        if (config_.global_bans && multiplayer::reskate_banned(link.id)) {
            transport_.disconnect(link.id, multiplayer::banned_notice.data());
            continue;
        }
        auto *guest = find(link.id);
        if (!guest && join_backoff_.waiting(link.id, now_)) {
            transport_.disconnect(link.id, "Too many failed attempts to join. Wait a little and try again.");
            continue;
        }
        if (!guest) {
            if (!individual_steam_id(link.id) || !may_join(config_, link.id, guests_.size())) {
                transport_.disconnect(link.id, "The server is full.");
                continue;
            }
            auto created = std::make_unique<Guest>();
            created->sender.set_refresh(whole_state_refresh, true);
            created->member.id = link.id;
            created->last_packet = now_;
            guest = created.get();
            guests_.emplace(link.id, std::move(created));
        }
        if (link.connected && !guest->connected_at) guest->connected_at = now_;
    }
    for (const auto &message : transport_.receive()) receive(message.peer, message.bytes, message.arrived);
    relay_poses();
    flush_poses();
    receive_cosmetics();
    introduce();
    for (auto it = guests_.begin(); it != guests_.end();) {
        auto &g = *(it++)->second;
        // Authorized arrivals get time for loading; this deadline is absolute, so
        // requests cannot hold an unused slot indefinitely.
        const std::uint64_t handshake_timeout = g.map_authorized ? 180000000 : 8000000;
        if (g.handshaken && g.travel_since) {
            if (now_ - g.travel_since > 180000000) drop(g.member.id, "Could not finish loading the new map.");
            continue;
        }
        // The connection is still open but nothing came over it: what Steam measures of it says
        // whether the link went bad or the player's game just stopped sending.
        if ((!g.handshaken && g.connected_at && now_ - g.connected_at > handshake_timeout) ||
            (g.handshaken && now_ - g.last_packet > 10000000))
            drop(g.member.id, "Timed out waiting for gameplay data.",
                 "nothing for " + std::to_string((now_ - g.last_packet) / 1000000) + " s; " + transport_.link_report(g.member.id));
    }
    for (auto note : transport_.take_direct_notes()) {
        while (!note.empty() && (note.back() == '\n' || note.back() == '\r')) note.pop_back();
        log_("[direct] " + note);
    }
    // Steam's relay network, whenever what it says of itself changes: every connection goes
    // through it, and joins time out while it is not ready.
    if (now_ >= next_relay_check_) {
        next_relay_check_ = now_ + 10000000;
        if (auto status = transport_.relay_status(); status != relay_status_) {
            log_("[steam] Relay network: " + status);
            relay_status_ = std::move(status);
        }
    }
    // Every half minute, what was sent and received in it (net).
    if (now_ >= traffic_mark_ + 30000000) {
        const auto roll = [](Counted &c) {
            for (std::size_t i = 0; i < c.total.out.size(); ++i) {
                c.last.out[i] = c.total.out[i] - c.mark.out[i];
                c.last.in[i] = c.total.in[i] - c.mark.in[i];
            }
            c.last.snapshots = c.total.snapshots - c.mark.snapshots;
            c.mark = c.total;
        };
        roll(traffic_);
        for (auto &[id, guest] : guests_) roll(guest->traffic);
        traffic_window_us_ = traffic_mark_ ? now_ - traffic_mark_ : 0;
        traffic_mark_ = now_;
    }
    // In a crowd, how far the full and half pose rates reach for each player (crowd_limits).
    if (now_ >= next_crowd_) {
        next_crowd_ = now_ + 500000;
        std::vector<float> squared;
        for (auto &[id, guest] : guests_) {
            auto &g = *guest;
            g.crowd = {};
            if (!g.latest_root || !config_.crowd_budget || guests_.size() * config_.tps <= config_.crowd_budget) continue;
            squared.clear();
            for (const auto &[other_id, other] : guests_) {
                if (other_id == id || !other->handshaken || !other->world_ready) continue;
                float distance{};
                if (other->latest_root)
                    for (unsigned i = 0; i < 3; ++i) {
                        const auto d = g.latest_root->position[i] - other->latest_root->position[i];
                        distance += d * d;
                    }
                squared.push_back(distance); // not placed yet: counted as beside them
            }
            g.crowd = crowd_limits(squared, config_.tps, config_.crowd_budget);
        }
    }
    tick_parties();
    // Once a minute while anyone is on: how the connections are doing as a whole. A queue that
    // grows with the player count, or sends that fail, is the server not getting data out.
    if (config_.activity_log && now_ >= next_network_log_ && !guests_.empty()) {
        next_network_log_ = now_ + 60000000;
        const auto &net = transport_.status();
        if (net.telemetry)
            log_("[network] " + std::to_string(guests_.size()) + " players, " + std::to_string(static_cast<int>(net.outgoing_bps / 1024.f + .5f)) +
                 " KB/s out, " + std::to_string(static_cast<int>(net.incoming_bps / 1024.f + .5f)) + " KB/s in, worst ping " +
                 std::to_string(net.ping_ms) + " ms, " + std::to_string(net.pending_bytes / 1024) + " KB queued (longest " +
                 std::to_string(net.queue_us / 1000) + " ms), " + std::to_string(net.send_failures) + " failed sends, " +
                 std::to_string(net.skipped) + " skipped, " + std::to_string(net.dropped) + " dropped | loop " + loop_report());
    }
    if (roster_dirty_ || now_ - last_roster_ > 2000000) send_roster();
    for (auto &[id, guest] : guests_)
        if (guest->handshaken) {
            if (is_admin(id) && guest->bans_sent != bans_revision_) send_bans(*guest);
            // Admins pick maps from it; with map votes on, everyone completes /vote map from it.
            if (!guest->maps_sent && (is_admin(id) || config_.votes.map.enabled)) send_maps(*guest);
        }
    const bool loading = std::any_of(guests_.begin(), guests_.end(),
                                     [](const auto &g) { return g.second->handshaken && !g.second->world_ready; });
    if (world_ > 1 && (loading || !last_world_state_) && (!last_world_state_ || now_ - last_world_state_ >= 1000000))
        send_world_state();
    if (travel_started_ && !loading) {
        travel_started_ = 0;
        log_("Everyone has loaded " + map_name() + ".");
    }
    sync_objects();
    activity_.tick(now_);
    if (std::exchange(vote_recount_, false)) check_vote(false);
    if (vote_ && now_ >= vote_->ends) check_vote(true);
    // A finished vote has been shown long enough.
    if (!vote_ && vote_shown_.id && now_ >= vote_shown_until_) {
        vote_shown_ = {};
        roster_dirty_ = true;
    }
    tick_rotation();
    tick_announcements();
    remove_away();
    std::erase_if(vote_cooldowns_, [&](const auto &entry) { return now_ >= entry.second; });
    join_backoff_.prune(now_);
}

} // namespace dingosdk::server
