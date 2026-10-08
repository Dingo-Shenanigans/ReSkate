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
constexpr std::string_view help_text =
    "status | net [player] | players | say <text> | msg <player> <text> | msg-party <player> <text> | msg-admins <text> | kick <player> | ban <player or SteamID64> [name] | unban <SteamID64> | bans\n"
    "map <name, e.g. San Vansterdam> | maps | name <text> | password <text|off> | welcome <text|off> | listed on|off\n"
    "voice on|off | voice-range <50-1000> | distances <full> <half> <half-return> <low> | crowd <n>|off | rate <KB/s> | bone-scale <1-8>|off\n"
    "placement everyone|admins|nobody | objects <number>|off | clear-objects | noclip on|off | nobail on|off | boosts on|off | tuning on|off\n"
    "tpall [player] | tphere <player> | votes [map|kick|tod on|off|<percent>] | vote-cancel\n"
    "map-pool [add|remove <map>|clear] | rotation [<minutes>|off]\n"
    "park <lot> <layout> | layer-sync on|off | layer <key> default|on|off | tod <time|default>\n"
    "activity-log on|off | announce-throwdowns on|off | parties [on|off] | party-size <2-8> | speed-check off|warn|kick\n"
    "score-check [off|warn|kick] | score-allow [<fingerprint>|remove <fingerprint>]\n"
    "reserved [slots <n> | add|remove <SteamID64>] | admin add|remove <SteamID64> | admins | update | quit";
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
    if (!transport_.host(capacity())) {
        error = transport_.status().detail;
        return false;
    }
    id_ = transport_.status().local_id;
    secret_ = nonce();
    epoch_ = nonce();
    world_ = 1;
    map_ = map_hash(map_destination(config_.map));
    password_ = config_.password.empty() ? std::nullopt : password_key(config_.password, secret_);
    voice_policy_ = {config_.voice_chat, 1};
    apply_layers();
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
// How finely a player's rotations are sent, by how far away the recipient is (coarsen_rotations):
// within the full rate's reach, the half rate's, the low rate's, and out of sight. The
// nearest is 0.04 degrees a step, which nothing shows; the low rate's a third of a degree,
// at 170 m and more; out of sight only where the skater is matters. A bone that turns by less
// than a step is not sent at all, and a far skater's fingers and face never turn by more.
constexpr std::array<unsigned, 4> pose_precision{4, 6, 7, 11};
constexpr std::array<const char *, 4> pose_precision_names{"near", "mid", "far", "out of sight"};
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
void Host::broadcast(const Packet &packet, bool reliable, bool fresh, std::uint64_t except) {
    if (packet.kind == PacketKind::pose)
        if (auto *from = find(packet.source)) measure_pose(*from, packet);
    const auto *source = find(packet.source);
    // Recipients holding the same delta reference share one patch and compression
    // (DeltaCache): every update is built first, then all are sent and recorded.
    struct Encoded { Packet packet; std::vector<std::uint8_t> raw, wire; DeltaCache deltas; };
    std::array<Encoded, 12> encoded; // by the interval the pose carries, then by precision
    struct Outgoing { Guest *guest; std::uint64_t id; WireUpdate update; PoseDelivery *delivery; std::uint32_t interval; unsigned variant; };
    std::vector<Outgoing> outgoing;
    outgoing.reserve(guests_.size());
    const bool gameplay = packet.kind == PacketKind::pose || packet.kind == PacketKind::audio ||
                          packet.kind == PacketKind::voice || packet.kind == PacketKind::cosmetics;
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
        // Frequent gameplay streams skip the server once the receiver confirms a direct route.
        if (source && (packet.kind == PacketKind::pose || packet.kind == PacketKind::audio) &&
            !needs_relay(p.direct_routes, p.route_reported, source->member, now_))
            continue;
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
                const auto sight = static_cast<float>(config_.distances.low_rate_start) * (delivery->precision == 3 ? 1.4f : 1.5f);
                out_of_sight = config_.distances.valid() && distance > sight * sight;
                precision = out_of_sight ? 3U : delivery->by_distance == 200000 ? 2U : delivery->by_distance == 100000 ? 1U : 0U;
            } else {
                delivery->by_distance = 0;
                precision = delivery->precision;
            }
            // By distance only, which changes seldom: a difference from a reference of another
            // precision carries every bone again, so the reference is renewed with it.
            if (precision != delivery->precision) {
                p.sender.forget(packet.source, PacketKind::pose);
                delivery->precision = static_cast<std::uint8_t>(precision);
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
        }
        const auto variant = (interval == 200000 ? 2U : interval == 100000 ? 1U : 0U) * 4 + precision;
        auto &data = encoded[variant];
        if (data.raw.empty()) {
            data.packet = packet;
            data.packet.pose_interval_us = interval;
            if (packet.kind == PacketKind::pose) {
                // What a mod resized on its player's skater reaches the others only as far as the server allows.
                if (config_.bone_scale_limit >= 1.f) limit_bone_scale(data.packet.pose, config_.bone_scale_limit);
                coarsen_rotations(data.packet.pose, pose_precision[precision]);
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
                    ++pose_sizes_.sent[out.variant % 4];
                    pose_sizes_.sent_bytes[out.variant % 4] += out.update.bytes.size();
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
    p.distances = config_.distances;
    p.object_placement = config_.object_placement;
    p.object_limit = config_.object_limit;
    p.guest_noclip = config_.noclip;
    p.guest_no_bail = config_.no_bail;
    p.guest_boosts = config_.boosts;
    p.enforce_tuning = config_.enforce_tuning;
    p.server_votes = enabled_votes();
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
            }
        }
        return check_speed(source, p.time_us); // last: a speed-check kick frees `source`
    }
    return true;
}
void Host::receive(std::uint64_t peer, std::span<const std::uint8_t> bytes, std::uint64_t received_at) {
    auto *link = find(peer);
    if (!link) return;
    // Counted by arrival: after the server was held up, everyone's packets are read at once.
    if (!link->budget.accept(received_at ? received_at : now_, bytes.size(), 1U))
        return drop(peer, "Peer exceeded the multiplayer packet limit.");
    bool missing_reference{};
    const auto decoded = link->receiver.receive(bytes, missing_reference, world_);
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
        if (arrived) meet_later(*link);
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
        if (link->objects.receive(p.objects) == ObjectState::Result::invalid)
            return drop(peer, "Invalid shared object revision or layout.");
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
        broadcast(p, false, true, p.source);
        return;
    }
    if (accept_data(*link, p))
        broadcast(p, p.kind == PacketKind::audio && std::any_of(p.audio.begin(), p.audio.end(),
                                                                [](const auto &sample) { return sample.event; }),
                  true, p.source);
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

// ---- Objects ---------------------------------------------------------------------------------
void Host::sync_objects() {
    if (now_ < next_object_update_) return;
    next_object_update_ = now_ + 100000;
    // Guests upload their layouts; the server alone decides what everyone else
    // sees, and freezes the layouts of players who may not build right now.
    for (auto &[id, guest] : guests_)
        if (guest->handshaken && guest->objects.revision() != guest->shared_from &&
            (config_.object_placement == ObjectPlacement::everyone ||
             (config_.object_placement == ObjectPlacement::host_only && is_admin(id)))) {
            auto layout = guest->objects.layout();
            std::erase_if(layout, [&](const auto &object) { return guest->cleared.contains(object.id); });
            // No more of a player's objects than the server allows each of them; admins are not limited.
            if (!is_admin(id)) layout = limited_layout(std::move(layout), guest->shared.objects(), config_.object_limit);
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
                       "\nroute: through Steam's relay " + (link.relay.empty() ? std::string("(unknown)") : link.relay) +
                       " on this side and " + (link.remote_relay.empty() ? std::string("(unknown)") : link.remote_relay) + " on theirs";
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
            if (link.measured) ++relays[(link.relay.empty() ? "?" : link.relay) + "-" + (link.remote_relay.empty() ? "?" : link.remote_relay)];
        std::vector<std::pair<std::string, unsigned>> order(relays.begin(), relays.end());
        std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        text += "\nrelays (ours-theirs):";
        for (std::size_t i = 0; i < std::min<std::size_t>(order.size(), 10); ++i)
            text += " " + order[i].first + " x" + std::to_string(order[i].second);
    }
    text += "\nloop: " + loop_report();
    text += "\n" + by_kind(traffic_);
    if (pose_sizes_.samples) {
        const auto each = [&](std::uint64_t sum) { return std::to_string(sum / pose_sizes_.samples) + " B"; };
        text += "\npose size as sent:";
        for (std::size_t i = 0; i < pose_sizes_.sent.size(); ++i)
            if (pose_sizes_.sent[i])
                text += std::string(" ") + pose_precision_names[i] + " " + std::to_string(pose_sizes_.sent_bytes[i] / pose_sizes_.sent[i]) + " B x" +
                        std::to_string(pose_sizes_.sent[i]);
        text += "; as it arrives " + each(pose_sizes_.last) + " against the pose before, whole " + each(pose_sizes_.whole);
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
                // Full for them: for everyone, or with only the reserved slots left.
                transport_.disconnect(link.id, guests_.size() >= config_.max_players || !individual_steam_id(link.id)
                                                   ? "The server is full."
                                                   : "The server is full: the slots that are left are reserved.");
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
    tick_rotation();
    std::erase_if(vote_cooldowns_, [&](const auto &entry) { return now_ >= entry.second; });
    join_backoff_.prune(now_);
}

// ---- Commands --------------------------------------------------------------------------------
std::string Host::command(std::string_view line, std::uint64_t admin) {
    const bool console = admin == 0;
    auto [action, argument] = split(line);
    const auto verb = lower(action);
    // The in-game menu's names for the same settings.
    const std::string name = verb == "voice-allow" ? "voice" : verb == "object-placement" ? "placement"
                           : verb == "object-limit" ? "objects"
                           : verb == "world-layer-sync" ? "layer-sync" : verb == "noclip-allow" ? "noclip"
                           : verb == "nobail-allow" ? "nobail" : verb == "boosts-allow" ? "boosts"
                           : verb == "tuning-enforce" ? "tuning" : verb;
    const auto target = [&](std::string_view text) -> Guest * {
        // A SteamID64 (optionally followed by the player's session epoch, as the
        // in-game menu sends it), or the start of a connected player's name.
        // Every name starts with "", so a bare `kick` would pick the only player.
        if (text.empty()) return nullptr;
        const auto [first, rest] = split(text);
        (void)rest;
        if (const auto id = number(first)) return find(*id);
        Guest *match{};
        for (auto &[id, guest] : guests_)
            if (guest->handshaken && lower(guest->member.name).starts_with(lower(text))) {
                if (match) return nullptr;
                match = guest.get();
            }
        return match;
    };
    const auto changed = [&](std::string text) {
        ++bans_revision_; // cheap: admins only get a fresh list when it moved
        save();
        roster_dirty_ = true;
        if (!console) log_(text); // the console logs its own replies
        return text;
    };
    if (name.empty() || name == "help") return std::string(help_text);
    if (name == "status")
        return config_.name + " | " + map_name() + " | " + std::to_string(players()) + "/" +
               std::to_string(config_.max_players) + " players | " + std::to_string(config_.tps) + " TPS | voice " +
               (voice_policy_.allowed ? "on" : "off") + " (" + std::to_string(static_cast<int>(config_.voice_range)) +
               " m) | password " + (password_ ? "on" : "off") + " | code " + invite();
    if (name == "net") return network_report(argument, console);
    if (name == "players") {
        std::string text = std::to_string(players()) + " players";
        for (const auto &[id, guest] : guests_)
            if (guest->handshaken)
                text += "\n  " + std::to_string(id) + "  " + guest_name(*guest) + (is_admin(id) ? "  (admin)" : "");
        return text;
    }
    if (name == "say") {
        if (!console) return "Use chat to talk to everyone.";
        if (argument.empty()) return "say <text>";
        send_chat(argument);
        return "[chat] Server: " + clean_chat_text(argument);
    }
    if (name == "msg" || name == "msg-party" || name == "msg-admins") {
        // Direct messages from the console or an admin, marked "[DM from ...]" so nobody takes them for chat.
        const auto *sender = console ? nullptr : find(admin);
        const std::string from = sender ? guest_name(*sender) : "Server";
        const bool to_admins = name == "msg-admins";
        const auto [who, text] = to_admins ? std::pair<std::string_view, std::string_view>{{}, trim(argument)} : split(argument);
        if (text.empty()) return to_admins ? "msg-admins <text>" : name + " <player> <text>";
        std::vector<Guest *> recipients;
        std::string scope, label;
        if (to_admins) {
            scope = label = "admins";
            for (auto &[id, guest] : guests_)
                if (guest->handshaken && is_admin(id)) recipients.push_back(guest.get());
        } else {
            auto *guest = match_player(who);
            if (!guest) return "No single connected player matches \"" + std::string(who) + "\".";
            label = guest_name(*guest);
            if (name == "msg") {
                recipients.push_back(guest);
            } else {
                const auto *details = parties_.party(parties_.party_of(guest->member.id));
                if (!details) return label + " is not in a party.";
                scope = "party";
                label += "'s party";
                for (const auto member : details->members)
                    if (auto *found = find(member); found && found->handshaken) recipients.push_back(found);
            }
        }
        if (recipients.empty()) return "No admins are online.";
        const auto message = dm_line(from, scope, text, multiplayer_chat_max_bytes);
        for (auto *guest : recipients) send_chat(message, guest);
        const auto done = "Sent to " + label + (recipients.size() > 1 || to_admins ? " (" + std::to_string(recipients.size()) + " players)" : "") + ".";
        if (!console) log_("[dm] " + from + " -> " + label + ": " + clean_chat_text(text));
        return done;
    }
    if (name == "kick") {
        auto *guest = target(argument);
        if (!guest || !guest->handshaken) return "No single connected player matches \"" + std::string(argument) + "\".";
        // Admins answer to the console, not to each other.
        if (!console && is_admin(guest->member.id)) return "Admins cannot kick other admins.";
        const auto label = guest_name(*guest);
        kicked_.insert(guest->member.id);
        drop(guest->member.id, "You were kicked from this server.");
        return changed(label + " was kicked until the server restarts.");
    }
    if (name == "ban") {
        auto [who, reason] = split(argument);
        auto *guest = target(who);
        std::uint64_t id = guest ? guest->member.id : number(who).value_or(0);
        if (!individual_steam_id(id)) return "Enter a connected player or a SteamID64 (17 digits starting 7656119).";
        if (id == admin) return "You cannot ban yourself.";
        if (!console && is_admin(id)) return "Admins cannot ban other admins.";
        if (is_banned(id)) return std::to_string(id) + " is already banned.";
        auto label = guest ? guest->member.name : clean_chat_text(reason);
        cut_text(label, 64);
        config_.bans.push_back({id, label, static_cast<std::int64_t>(std::time(nullptr))});
        if (guest) drop(id, "You were banned from this server.");
        return changed((label.empty() ? std::to_string(id) : label) + " was banned.");
    }
    if (name == "unban") {
        const auto id = number(argument).value_or(0);
        const auto found = std::find_if(config_.bans.begin(), config_.bans.end(), [&](const auto &b) { return b.id == id; });
        if (found == config_.bans.end()) return "That SteamID64 is not banned.";
        const auto label = found->name.empty() ? std::to_string(id) : found->name;
        config_.bans.erase(found);
        kicked_.erase(id);
        return changed(label + " was unbanned.");
    }
    if (name == "bans") {
        std::string text = std::to_string(config_.bans.size()) + " banned";
        for (const auto &ban : config_.bans) text += "\n  " + std::to_string(ban.id) + "  " + ban.name;
        return text;
    }
    if (name == "map") {
        // The map as it will be stored must still name a destination: one that does not would
        // leave the server unable to tell players where to go, and unable to start again.
        if (argument.empty() || !valid_map_destination(map_destination(argument)) ||
            !valid_map_destination(map_destination(map_setting(argument))))
            return "No single map is called \"" + std::string(argument) + "\". Type maps for the list.";
        // Only a map the server has: the game's own, or one from a mod in its Mods folder.
        if (!installed_map(argument))
            return "This server does not have that map. Put the map's mod folder in Mods next to the server, then restart it.";
        if (map_hash(map_destination(argument)) == map_) return "The server is already on that map.";
        change_map(argument);
        save();
        return changed("Changing map to " + map_name());
    }
    if (name == "maps") {
        std::string text = std::to_string(levels().size()) + " maps (custom maps come from Mods next to the server)";
        for (const auto &level : levels())
            text += "\n  " + level.name + (same_map(level.asset) ? "  (now)" : "") +
                    (!config_.map_pool.empty() && in_map_pool(config_, level.asset) ? "  (pool)" : "");
        return text;
    }
    if (name == "map-pool") { // map-pool [add|remove <map>|clear]
        const auto [what_text, map] = split(argument);
        const auto what = lower(what_text);
        if (what.empty()) return pool_text();
        if (what == "clear") {
            config_.map_pool.clear();
            resend_maps();
            return changed("The map pool is cleared: players vote between every map, and the rotation goes through them all.");
        }
        if (what != "add" && what != "remove") return "map-pool [add|remove <map>|clear]";
        const auto *level = find_level(map);
        if (!level || !valid_map_destination(map_destination(level->asset)))
            return "No single map is called \"" + std::string(map) + "\". Type maps for the list.";
        const auto pooled = [&](const std::string &entry) { return find_level(entry) == level; };
        const bool listed = std::any_of(config_.map_pool.begin(), config_.map_pool.end(), pooled);
        if (what == "add") {
            if (listed || config_.map_pool.empty()) return level->name + " is already in the map pool.";
            config_.map_pool.push_back(level->name);
        } else {
            if (config_.map_pool.empty()) // every map: keep all the others
                for (const auto *other : pool_levels(config_)) config_.map_pool.push_back(other->name);
            else if (!listed) return level->name + " is not in the map pool.";
            if (std::all_of(config_.map_pool.begin(), config_.map_pool.end(), pooled))
                return "The map pool needs at least one map. map-pool clear allows every map again.";
            std::erase_if(config_.map_pool, pooled);
        }
        resend_maps();
        return changed(level->name + (what == "add" ? " added to" : " removed from") + " the map pool.");
    }
    if (name == "rotation") { // rotation [<minutes>|off]
        if (argument.empty()) return rotation_text();
        const auto value = lower(argument);
        const auto minutes = value == "off" ? std::optional<std::uint64_t>(0) : number(value);
        if (!minutes || *minutes > max_map_rotation) return "rotation <1-1440 minutes>|off";
        config_.map_rotation = static_cast<unsigned>(*minutes);
        map_since_ = now_;
        rotation_warned_ = false;
        resend_maps();
        return changed(rotation_text());
    }
    if (name == "name") {
        if (!valid_server_name(argument)) return std::string("Server names are ") + server_name_rule + ".";
        config_.name = argument;
        if (text::contains_bad_words(config_.name))
            return changed("Server renamed to " + config_.name +
                           ". That name contains blocked words, so the server stays out of the server browser.");
        return changed("Server renamed to " + config_.name + ".");
    }
    if (name == "password") {
        if (argument.size() > 64) return "Passwords are at most 64 characters.";
        config_.password = argument == "off" ? std::string{} : std::string(argument);
        erase_key(password_);
        password_ = config_.password.empty() ? std::nullopt : password_key(config_.password, secret_);
        return changed(config_.password.empty() ? "Password removed. Anyone can join."
                                                : "Password set. Players already here stay; new ones need it.");
    }
    if (name == "welcome") {
        if (argument != "off" && !argument.empty() && !valid_chat_text(argument)) return "The welcome message is one chat line.";
        config_.welcome = argument == "off" ? std::string{} : std::string(argument);
        return changed(config_.welcome.empty() ? "Welcome message removed." : "Welcome message set.");
    }
    if (name == "announce-throwdowns") {
        const auto value = on_off(argument);
        if (!value) return std::string("announce-throwdowns on|off (now ") + (config_.announce_throwdowns ? "on" : "off") + ")";
        config_.announce_throwdowns = *value;
        return changed(*value ? "Placed throwdowns are announced in chat." : "Placed throwdowns are no longer announced.");
    }
    if (name == "parties") {
        const auto value = on_off(argument);
        if (argument.empty()) return std::string(config_.parties ? "Parties are on.\n" : "Parties are off.\n") + party_status(0);
        if (!value) return "parties on|off";
        config_.parties = *value;
        if (!*value) {
            for (auto &[id, guest] : guests_) parties_.remove(id);
            parties_.take_withdrawn();
        }
        return changed(*value ? "Players can form parties." : "Parties are off; every party was ended.");
    }
    if (name == "speed-check") {
        const auto value = lower(argument);
        if (value != "off" && value != "warn" && value != "kick")
            return "speed-check off|warn|kick (now " + config_.speed_check + ")";
        config_.speed_check = value;
        if (value == "off")
            for (auto &[id, guest] : guests_) {
                guest->speed.restart();
                guest->speeding = false;
            }
        return changed(value == "off" ? "Game speed is no longer checked."
                       : value == "kick" ? "Players whose game runs fast are kicked."
                                         : "Players whose game runs fast are taken out of throwdowns and challenges.");
    }
    if (name == "score-check") {
        const auto value = lower(argument);
        if (value.empty()) {
            std::string text = "score-check " + config_.score_check + " (off|warn|kick)";
            for (const auto &[id, guest] : guests_) {
                if (!guest->handshaken) continue;
                text += "\n  " + guest_name(*guest) + ": ";
                if (!guest->scoring) text += "not reported";
                else if (!*guest->scoring) text += "the game's own scoring";
                else
                    text += scoring_text(*guest->scoring) + (guest->scoring_mods.empty() ? "" : " (" + guest->scoring_mods + ")") +
                            (guest->scoring_flagged ? ", out of throwdowns" : ", allowed");
            }
            return text;
        }
        if (value != "off" && value != "warn" && value != "kick") return "score-check off|warn|kick (now " + config_.score_check + ")";
        config_.score_check = value;
        // Kicking changes the guest list: collect first.
        std::vector<std::uint64_t> ids;
        for (const auto &[id, guest] : guests_) ids.push_back(id);
        for (const auto id : ids)
            if (auto *guest = find(id)) check_scoring(*guest);
        return changed(value == "off" ? "Mods that change scoring or physics are no longer checked."
                       : value == "kick" ? "Players whose mods change scoring or physics are kicked."
                                         : "Players whose mods change scoring or physics are taken out of throwdowns and challenges.");
    }
    if (name == "score-allow") {
        auto [what, rest] = split(argument);
        if (what.empty()) {
            std::string text = "score-allow <fingerprint> | score-allow remove <fingerprint>. Accepted besides the game's own:";
            if (config_.score_allow.empty()) text += " none";
            for (const auto fingerprint : config_.score_allow) text += "\n  " + scoring_text(fingerprint);
            return text;
        }
        const bool remove = lower(what) == "remove";
        const auto fingerprint = parse_scoring(remove ? rest : what);
        if (!fingerprint) return "A fingerprint is 16 hex digits, as score-check lists it.";
        const auto found = std::find(config_.score_allow.begin(), config_.score_allow.end(), *fingerprint);
        if (remove) {
            if (found == config_.score_allow.end()) return scoring_text(*fingerprint) + " was not accepted.";
            config_.score_allow.erase(found);
        } else if (found == config_.score_allow.end()) {
            config_.score_allow.push_back(*fingerprint);
        }
        std::vector<std::uint64_t> ids;
        for (const auto &[id, guest] : guests_) ids.push_back(id);
        for (const auto id : ids)
            if (auto *guest = find(id)) check_scoring(*guest);
        return changed(remove ? "Scoring " + scoring_text(*fingerprint) + " is no longer accepted."
                              : "Scoring " + scoring_text(*fingerprint) + " is accepted like the game's own.");
    }
    if (name == "rate") {
        // rate <KB/s>: what the server may send each player, from now on and to those on.
        const auto value = number(argument);
        if (!value || *value < 128 || *value > 16384)
            return "rate <128-16384> (KB/s for each player, now " + std::to_string(config_.send_rate) + ")";
        config_.send_rate = static_cast<unsigned>(*value);
        const bool applied = transport_.set_send_rate(static_cast<int>(config_.send_rate * 1024));
        return changed("Each player is sent at most " + std::to_string(config_.send_rate) + " KB/s" +
                       (applied ? "." : ": players who join from now on. Steam did not change the connections already open."));
    }
    if (name == "bone-scale") {
        // bone-scale <1-8>|off: how far a mod may resize part of a skater for the other players.
        float value{};
        const bool off = argument == "off" || argument == "0";
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
        if (!off && (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !(value >= 1.f && value <= 8.f)))
            return "bone-scale <1-8>|off: 1 shows every skater at the game's own proportions, off allows anything (now " +
                   (config_.bone_scale_limit >= 1.f ? std::to_string(config_.bone_scale_limit).substr(0, 4) : std::string("off")) + ")";
        config_.bone_scale_limit = off ? 0.f : value;
        // Whole states go again so that nobody keeps a reference with the old sizes in it.
        for (auto &[id, guest] : guests_)
            for (const auto &[other, unused] : guests_) guest->sender.forget(other, PacketKind::pose);
        return changed(off ? std::string("Mods may resize skaters' body parts freely.")
                           : value == 1.f ? std::string("Skaters show at the game's own proportions: resized body parts are not passed on.")
                                          : "Resized body parts show at up to " + std::to_string(value).substr(0, 4) + "x.");
    }
    if (name == "crowd") {
        // crowd <poses a second>|off: the most one player is sent (crowd_limits).
        auto value = number(argument);
        if (argument == "off") value = 0;
        if (!value || *value > max_crowd_budget || !valid_crowd_budget(static_cast<unsigned>(*value)))
            return "crowd <" + std::to_string(min_crowd_budget) + "-" + std::to_string(max_crowd_budget) + ">|off (now " +
                   (config_.crowd_budget ? std::to_string(config_.crowd_budget) : std::string("off")) + ")";
        config_.crowd_budget = static_cast<unsigned>(*value);
        next_crowd_ = 0;
        return changed(*value ? "Each player is sent at most " + std::to_string(*value) + " poses a second: about " +
                                    std::to_string(*value / config_.tps) + " players near them at the full rate."
                              : std::string("No crowd limit: every player near is sent at the full rate."));
    }
    if (name == "party-size") {
        const auto value = number(argument);
        if (!value || *value < 2 || *value > 8) return "party-size <2-8> (now " + std::to_string(config_.party_size) + ")";
        config_.party_size = static_cast<unsigned>(*value);
        parties_.set_limit(config_.party_size);
        return changed("Parties hold up to " + std::to_string(config_.party_size) + " players. Larger ones stay until members leave.");
    }
    if (name == "activity-log") {
        const auto value = on_off(argument);
        if (!value) return std::string("activity-log on|off (now ") + (config_.activity_log ? "on" : "off") + ")";
        config_.activity_log = *value;
        if (!*value) activity_.clear();
        return changed(*value ? "Player activity (throwdowns, objects, loading) is logged."
                              : "Player activity is no longer logged.");
    }
    if (name == "listed") {
        const auto value = on_off(argument);
        if (!value) return "listed on|off";
        config_.listed = *value;
        return changed(*value ? "The server is listed in the server browser." : "The server is hidden; players need the code.");
    }
    if (name == "tps") {
        return "Dedicated servers run at " + std::to_string(dedicated_tps) + " TPS for now; it cannot be changed.";
    }
    if (name == "voice") {
        const auto value = on_off(argument);
        if (!value) return "voice on|off";
        config_.voice_chat = *value;
        if (voice_policy_.allowed != *value) {
            voice_policy_.allowed = *value;
            if (!++voice_policy_.revision) ++voice_policy_.revision;
        }
        return changed(*value ? "Voice chat allowed." : "Voice chat disabled for everyone.");
    }
    if (name == "voice-range") {
        float range{};
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), range);
        if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || !valid_voice_range(range))
            return "Choose a voice range from 50 to 1000 m.";
        config_.voice_range = range;
        return changed("Voice range set to " + std::to_string(static_cast<int>(range)) + " m.");
    }
    if (name == "distances") {
        MultiplayerDistances value;
        auto rest = argument;
        for (auto *field : {&value.full_rate_return, &value.half_rate_start, &value.half_rate_return, &value.low_rate_start}) {
            const auto [token, remaining] = split(rest);
            const auto result = std::from_chars(token.data(), token.data() + token.size(), *field);
            if (token.empty() || result.ec != std::errc{} || result.ptr != token.data() + token.size())
                return "distances <full> <half> <half-return> <low> (whole metres)";
            rest = remaining;
        }
        if (!rest.empty() || !value.valid())
            return "Use ordered distances: full < half <= half-return < low (at most 10000 m).";
        config_.distances = value;
        for (auto &[id, guest] : guests_) guest->pose_delivery = {};
        return changed("TPS distances updated.");
    }
    if (name == "placement") {
        // The protocol's "host only" is admins only here: the server has no skater of its own.
        const auto policy = parse_object_placement(argument == "admins" ? "host" : argument, config_.object_placement);
        if (!policy) return "placement everyone|admins|nobody";
        config_.object_placement = *policy;
        return changed(*policy == ObjectPlacement::everyone ? "Everyone can place objects."
                       : *policy == ObjectPlacement::host_only ? "Only admins can place objects. Everyone else's are frozen."
                                                               : "Object placement is off. Existing objects stay.");
    }
    if (name == "objects") {
        const auto limit = parse_object_limit(argument);
        if (!limit) return "objects <1-" + std::to_string(max_object_limit) + ">|off";
        config_.object_limit = *limit;
        for (auto &[id, guest] : guests_) guest->shared_from = 0; // look at every layout again
        return changed(*limit ? "Each player can place up to " + std::to_string(*limit) + " objects. Admins are not limited."
                              : std::string("Players can place as many objects as they like."));
    }
    if (name == "votes") {
        // votes | votes <map|kick|tod> on|off|<percent> | votes seconds|cooldown <n>
        const auto [what_text, value_text] = split(argument);
        const auto what = lower(what_text);
        const auto describe = [&](const char *label, const VoteSetting &v) {
            return std::string(label) + ": " + (v.enabled ? "on, " + std::to_string(v.percent) + "% to pass" : "off");
        };
        if (what.empty())
            return describe("map votes", config_.votes.map) + "\n" + describe("kick votes", config_.votes.kick) + "\n" +
                   describe("time of day votes", config_.votes.time) +
                   (config_.world_layer_sync ? "" : " (needs layer-sync on)") + "\nvotes last " +
                   std::to_string(config_.votes.seconds) + " s; a player waits " + std::to_string(config_.votes.cooldown) +
                   " s between votes" + (vote_ ? "\nrunning: a vote to " + vote_->label : std::string{});
        const auto value = lower(value_text);
        if (what == "seconds" || what == "cooldown") {
            const auto n = number(value);
            const bool seconds = what == "seconds";
            if (!n || (seconds ? *n < 10 || *n > 300 : *n > 3600))
                return seconds ? "votes seconds <10-300>" : "votes cooldown <0-3600>";
            (seconds ? config_.votes.seconds : config_.votes.cooldown) = static_cast<unsigned>(*n);
            return changed(seconds ? "Votes now last " + std::to_string(*n) + " s."
                                   : "Players now wait " + std::to_string(*n) + " s between votes.");
        }
        VoteSetting *setting = what == "map" ? &config_.votes.map : what == "kick" ? &config_.votes.kick
                             : what == "tod" || what == "time" ? &config_.votes.time : nullptr;
        if (!setting) return "votes [map|kick|tod on|off|<percent>] | votes seconds <n> | votes cooldown <n>";
        const auto label = what == "map" ? std::string("Map votes") : what == "kick" ? std::string("Kick votes")
                                                                                     : std::string("Time of day votes");
        if (const auto toggle = on_off(value)) {
            setting->enabled = *toggle;
            if (!*toggle && vote_ && vote_setting(vote_->kind).enabled == false) cancel_vote("that vote was switched off");
            return changed(label + (*toggle ? " are on (" + std::to_string(setting->percent) + "% to pass)." : " are off."));
        }
        const auto percent = number(value.ends_with("%") ? std::string_view(value).substr(0, value.size() - 1) : std::string_view(value));
        if (!percent || *percent < 1 || *percent > 100) return "votes " + what + " on|off|<1-100>";
        setting->percent = static_cast<unsigned>(*percent);
        return changed(label + " now need " + std::to_string(*percent) + "% to pass.");
    }
    if (name == "vote-cancel") {
        if (!vote_) return "No vote is running.";
        cancel_vote(console ? "the server cancelled it" : "an admin cancelled it");
        return "Vote cancelled.";
    }
    if (name == "tpall" || name == "tphere") {
        // Where they go: the admin who asked, or (tpall from the console) the named player.
        Guest *to{};
        std::vector<Guest *> movers;
        if (name == "tpall") {
            to = argument.empty() ? (console ? nullptr : find(admin)) : target(argument);
            if (!to || !to->handshaken)
                return console && argument.empty() ? "tpall <player>: everyone goes to that player."
                                                   : "No single connected player matches \"" + std::string(argument) + "\".";
            for (auto &[id, guest] : guests_)
                if (guest->handshaken && guest->world_ready && guest.get() != to) movers.push_back(guest.get());
        } else {
            if (console) return "tphere is for admins in the game; the console can use tpall <player>.";
            to = find(admin);
            auto *who = target(argument);
            if (!who || !who->handshaken) return "No single connected player matches \"" + std::string(argument) + "\".";
            if (who == to) return "That is you.";
            movers.push_back(who);
        }
        if (!to || !to->latest_root) return "There is no position for " + (to ? guest_name(*to) : std::string("you")) + " yet.";
        if (movers.empty()) return "Nobody else is in the world.";
        const auto at = to->latest_root->position;
        unsigned sent{};
        for (std::size_t i = 0; i < movers.size(); ++i) {
            // A ring around them, so nobody lands inside anyone else.
            const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(movers.size());
            auto p = packet(PacketKind::teleport, now_);
            p.teleport = {at[0] + 2.5f * std::cos(angle), at[1] + 1.0f, at[2] + 2.5f * std::sin(angle)};
            if (send_packet(*movers[i], p, true, false)) ++sent;
        }
        const auto text = movers.size() == 1 && sent ? guest_name(*movers[0]) + " was teleported to " + guest_name(*to) + "."
                                                     : std::to_string(sent) + " player(s) teleported to " + guest_name(*to) + ".";
        if (!console) log_(text);
        return text;
    }
    if (name == "noclip" || name == "nobail" || name == "boosts") {
        auto &allowed = name == "noclip" ? config_.noclip : name == "nobail" ? config_.no_bail : config_.boosts;
        const auto value = argument == "toggle" ? std::optional<bool>(!allowed) : on_off(argument);
        if (!value) return name + " on|off (now " + (allowed ? "on" : "off") + ")";
        allowed = *value;
        const std::string tool = name == "noclip" ? "Noclip and teleporting" : name == "nobail" ? "No Bail" : "Boosts";
        return changed(tool + (*value ? (name == "boosts" ? " are" : " is") + std::string(" allowed for everyone.")
                                      : (name == "boosts" ? " are" : " is") + std::string(" off for players; admins keep it.")));
    }
    if (name == "tuning") {
        const auto value = argument == "toggle" ? std::optional<bool>(!config_.enforce_tuning) : on_off(argument);
        if (!value) return std::string("tuning on|off (now ") + (config_.enforce_tuning ? "on" : "off") + ")";
        config_.enforce_tuning = *value;
        return changed(*value ? "Players skate with the game's own physics tuning."
                              : "Players skate with their own physics tuning.");
    }
    if (name == "clear-objects") {
        std::size_t removed{};
        for (auto &[id, guest] : guests_) {
            if (!guest->handshaken) continue;
            for (const auto *state : {&guest->objects, &guest->shared})
                for (const auto &[object, value] : state->objects()) {
                    (void)value;
                    guest->cleared.insert(object);
                }
            removed += guest->shared.objects().size();
            if (guest->shared.revision()) guest->shared.replace({});
            guest->shared_from = guest->objects.revision();
        }
        ++object_clears_;
        return changed("Deleted " + std::to_string(removed) + " placed object" + (removed == 1 ? "." : "s."));
    }
    if (name == "park") {
        const auto [lot_name, layout] = split(argument);
        const auto lot = std::find_if(park_lots.begin(), park_lots.end(), [&](const auto &l) { return l.key == lot_name; });
        if (lot == park_lots.end()) return "park construction|historic|financial <layout, e.g. skatepark_01, or empty>";
        const auto index = static_cast<unsigned>(lot - park_lots.begin());
        if (layout.empty() || !valid_park(index, layout)) return "That is not a layout for this lot.";
        config_.parks[index] = layout;
        return changed(std::string(lot->label) + " now shows " + park_label(layout) + ".");
    }
    if ((name == "layer-sync" || name == "layer" || name == "layers" || name == "tod") && world_layers().empty())
        return "World layers need world-layers.json next to the server (copy it from a player's "
               "%LOCALAPPDATA%\\ReSkate\\cache folder for the same game build).";
    if (name == "layer-sync") {
        const auto value = on_off(argument);
        if (!value) return "layer-sync on|off";
        config_.world_layer_sync = *value;
        apply_layers();
        return changed(*value ? "Everyone now follows the server's world layers." : "Players choose their own world layers.");
    }
    if (name == "layers") {
        // Several at once, as key=mode pairs: the in-game time of day sends seven.
        std::vector<std::pair<std::string, std::string>> changes;
        for (auto rest = argument; !rest.empty();) {
            const auto [pair, remaining] = split(rest);
            rest = remaining;
            const auto equals = pair.find('=');
            if (equals == std::string_view::npos) return "layers <key>=default|on|off ...";
            const auto key = pair.substr(0, equals), mode = pair.substr(equals + 1);
            if (std::none_of(world_layers().begin(), world_layers().end(), [&](const auto &l) { return l.key == key; }))
                return "No world layer is called \"" + std::string(key) + "\".";
            if (!valid_world_layer_mode(mode)) return "layers <key>=default|on|off ...";
            changes.emplace_back(key, mode);
        }
        if (changes.empty()) return "layers <key>=default|on|off ...";
        for (const auto &[key, mode] : changes) {
            if (mode == "default") config_.layers.erase(key);
            else config_.layers[key] = mode;
        }
        apply_layers();
        return changed(std::to_string(changes.size()) + " world layer" + (changes.size() == 1 ? "" : "s") + " changed" +
                       (config_.world_layer_sync ? "." : ". Turn on layer-sync to apply them to everyone."));
    }
    if (name == "tod") {
        // Every map's seven time layers ("<map>_tod_<n>_<name>"): one on and the rest off, or
        // all back to the level's own. Set for every map, so it holds across map changes.
        static constexpr std::array<std::string_view, 8> times{"default", "morning", "noon", "afternoon",
                                                               "evening", "night", "weatherday", "weathernight"};
        const auto wanted = lower(argument);
        const auto found = std::find(times.begin(), times.end(), wanted);
        if (found == times.end()) return "tod default|morning|noon|afternoon|evening|night|weatherday|weathernight";
        const auto slot = static_cast<char>('0' + (found - times.begin()));
        unsigned count{};
        for (const auto &layer : world_layers()) {
            const auto at = layer.key.find("_tod_");
            if (at == std::string::npos || at + 5 >= layer.key.size()) continue;
            if (slot == '0') config_.layers.erase(layer.key);
            else config_.layers[layer.key] = layer.key[at + 5] == slot ? "on" : "off";
            ++count;
        }
        if (!count) return "world-layers.json has no time-of-day layers.";
        apply_layers();
        return changed("Time of day set to " + std::string(*found) +
                       (config_.world_layer_sync ? " for everyone." : ". Turn on layer-sync to apply it to everyone."));
    }
    if (name == "layer") {
        const auto [key, mode] = split(argument);
        const auto found = std::find_if(world_layers().begin(), world_layers().end(), [&](const auto &l) { return l.key == key; });
        if (found == world_layers().end()) return "No world layer is called \"" + std::string(key) + "\".";
        if (!valid_world_layer_mode(mode)) return "layer <key> default|on|off";
        if (mode == "default") config_.layers.erase(std::string(key));
        else config_.layers[std::string(key)] = mode;
        apply_layers();
        return changed(found->label + " set to " + std::string(mode) +
                       (config_.world_layer_sync ? "." : ". Turn on layer-sync to apply it to everyone."));
    }
    if (name == "reserved") {
        // reserved | reserved slots <n> | reserved add|remove <player or SteamID64>
        if (!console) return "Only the server console manages reserved slots.";
        const auto [sub, who] = split(argument);
        if (sub.empty()) {
            std::string text = std::to_string(config_.reserved_slots) + " of " + std::to_string(config_.max_players) +
                               " slots are reserved for " + std::to_string(config_.reserved.size()) + " players and the admins";
            for (const auto id : config_.reserved) {
                const auto *guest = find(id);
                text += "\n  " + std::to_string(id) + (guest ? "  " + guest_name(*guest) : std::string{});
            }
            return text;
        }
        if (sub == "slots") {
            const auto slots = number(who);
            if (!slots || *slots >= config_.max_players) return "reserved slots <0-" + std::to_string(config_.max_players - 1) + ">";
            config_.reserved_slots = static_cast<unsigned>(*slots);
            return changed(*slots ? std::to_string(*slots) + " slots are kept for reserved players and admins."
                                  : std::string("No slots are reserved."));
        }
        auto *guest = target(who);
        const auto id = guest ? guest->member.id : number(who).value_or(0);
        if (!individual_steam_id(id) || (sub != "add" && sub != "remove")) return "reserved | reserved slots <n> | reserved add|remove <player or SteamID64>";
        const bool listed = std::find(config_.reserved.begin(), config_.reserved.end(), id) != config_.reserved.end();
        if (sub == "add") {
            if (!listed && config_.reserved.size() >= 1024) return "The reserved list is full.";
            if (!listed) config_.reserved.push_back(id);
            return changed(std::to_string(id) + " has a reserved slot." +
                           (config_.reserved_slots ? std::string{} : " No slots are reserved yet: reserved slots <n>."));
        }
        std::erase(config_.reserved, id);
        return changed(std::to_string(id) + " no longer has a reserved slot.");
    }
    if (name == "admins" || name == "admin") {
        if (!console) return "Only the server console manages admins.";
        const auto [sub, who] = split(argument);
        if (name == "admins" || sub.empty()) {
            std::string text = std::to_string(config_.admins.size()) + " admins";
            for (const auto id : config_.admins) {
                const auto *guest = find(id);
                text += "\n  " + std::to_string(id) + (guest ? "  " + guest_name(*guest) : std::string{});
            }
            return text;
        }
        auto *guest = target(who);
        const auto id = guest ? guest->member.id : number(who).value_or(0);
        if (!individual_steam_id(id)) return "admin add|remove <player or SteamID64>";
        if (sub == "add") {
            if (!is_admin(id)) config_.admins.push_back(id);
            resend_maps();
            return changed(std::to_string(id) + " is an admin.");
        }
        if (sub == "remove") {
            std::erase(config_.admins, id);
            resend_maps();
            return changed(std::to_string(id) + " is no longer an admin.");
        }
        return "admin add|remove <player or SteamID64>";
    }
    return "Unknown command \"" + std::string(action) + "\". Type help.";
}
// A player's mods change how tricks score (their report; Engine/Vfs/mod_scoring.h): flagged
// players are taken out of linked throwdowns and coop challenges (the roster's scoring flag, and
// the server relays none of theirs), or kicked. The game keeps a scoring mod's points until it
// restarts, so the flag lasts for the session.
void Host::check_scoring(Guest &guest) {
    if (!guest.handshaken) return;
    const bool changed = guest.scoring && *guest.scoring &&
                         std::find(config_.score_allow.begin(), config_.score_allow.end(), *guest.scoring) == config_.score_allow.end();
    const auto name = guest_name(guest);
    if (config_.score_check == "off" || !changed) {
        if (!guest.scoring_flagged) return;
        guest.scoring_flagged = false;
        roster_dirty_ = true;
        log_("[anticheat] " + name + " may take part in throwdowns again (score-check " + config_.score_check + ").");
        return;
    }
    if (guest.scoring_flagged) return;
    const auto mods = guest.scoring_mods.empty() ? std::string("their mods") : guest.scoring_mods;
    log_("[anticheat] " + name + "'s mods change scoring or physics: " + mods + " (scoring " + scoring_text(*guest.scoring) + ").");
    if (config_.score_check == "kick")
        return drop(guest.member.id, "Your mods change scoring or physics (" + mods + "). Turn them off and restart Skate to play here.");
    // Every player's game says so in chat when the roster flags someone, the player themself included.
    guest.scoring_flagged = true;
    roster_dirty_ = true;
}
// A speedhack runs the player's game clock, and so their pose timestamps, faster than real
// time. Flagged players are taken out of linked throwdowns and coop challenges (the roster's
// speeding flag: every client drops them from those), or kicked; the flag clears after a minute
// of normal speed.
bool Host::check_speed(Guest &guest, std::uint64_t sent) {
    if (config_.speed_check == "off" || !guest.handshaken || !guest.world_ready || !sent) return true;
    if (!guest.speed.sample(sent, now_)) return true;
    const auto name = guest_name(guest);
    const auto speed = guest.speed.speed();
    if (guest.speed.flagged() && !guest.speeding) {
        char text[160];
        std::snprintf(text, sizeof text, "%s's game is running at %.2fx speed (a speed hack?).", name.c_str(), speed);
        log_(std::string("[anticheat] ") + text);
        if (config_.speed_check == "kick") {
            drop(guest.member.id, "Your game is running faster than normal. Turn off speed hacks to play here.");
            return false;
        }
        guest.speeding = true;
        guest.speed_normal_since = 0;
        roster_dirty_ = true;
        send_chat("The server measured your game running faster than normal: throwdowns and challenges are off for you until it's back to normal speed.", &guest);
        // Only the player is told in the game; for the admins it is in the log.
        return true;
    }
    if (!guest.speeding) return true;
    if (speed < SpeedCheck::limit) {
        if (!guest.speed_normal_since) guest.speed_normal_since = now_;
        if (now_ - guest.speed_normal_since >= 60000000) {
            guest.speeding = false;
            roster_dirty_ = true;
            log_("[anticheat] " + name + "'s game speed is back to normal.");
            send_chat("Your game speed is back to normal: throwdowns and challenges are on again.", &guest);
        }
    } else guest.speed_normal_since = 0;
    return true;
}
} // namespace dingosdk::server
