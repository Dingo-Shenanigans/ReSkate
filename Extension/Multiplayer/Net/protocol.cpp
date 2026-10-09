#include "protocol.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
constexpr std::uint32_t magic = 0x31504d52; // RMP1, little endian
constexpr std::size_t header_size = packet_header_size;
std::size_t utf8_length(std::string_view text, std::size_t at) noexcept; // defined with the text checks below
std::uint8_t encode_park(unsigned lot, std::string_view choice) {
    if (choice.empty() || choice == "empty") return 0;
    for (unsigned family = 0; family < park_families.size(); ++family)
        for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant)
            if (choice == park_id(family, variant))
                return static_cast<std::uint8_t>(1 + family * 16 + variant - 1);
    throw std::invalid_argument("Invalid park selection");
}
std::string decode_park(unsigned lot, unsigned code) {
    if (!code) return "empty";
    const auto family = (code - 1) / 16, variant = (code - 1) % 16 + 1;
    if (family >= park_families.size() || variant > park_lots[lot].counts[family])
        throw std::invalid_argument("Invalid park selection");
    return park_id(family, variant);
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    void integer(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void transform(const Transform &t) {
        for (float v : t.position)
            integer(std::bit_cast<std::uint32_t>(v), 4);
        for (float v : t.rotation)
            integer(std::bit_cast<std::uint32_t>(v), 4);
        for (float v : t.scale)
            integer(std::bit_cast<std::uint32_t>(v), 4);
    }
    void compact_transform(const Transform &t) {
        unsigned largest{};
        for (unsigned i = 1; i < 4; ++i)
            if (std::abs(t.rotation[i]) > std::abs(t.rotation[largest]))
                largest = i;
        const bool wide =
            std::any_of(t.position.begin(), t.position.end(), [](float v) { return std::abs(v) > 32.767f; });
        const bool scale = t.scale != std::array<float, 3>{1, 1, 1};
        integer((largest << 2) | (wide ? 1U : 0U) | (scale ? 2U : 0U), 1);
        for (float v : t.position) {
            if (wide)
                integer(std::bit_cast<std::uint32_t>(v), 4);
            else
                integer(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 1000.f))), 2);
        }
        float norm{};
        for (float v : t.rotation)
            norm += v * v;
        const float factor = (t.rotation[largest] < 0 ? -1.f : 1.f) * 46339.5358f / std::sqrt(norm);
        for (unsigned i = 0; i < 4; ++i)
            if (i != largest)
                integer(static_cast<std::uint16_t>(
                            static_cast<std::int16_t>(std::lround(t.rotation[i] * factor))),
                        2);
        if (scale)
            for (float v : t.scale)
                integer(std::bit_cast<std::uint32_t>(v), 4);
    }
    void words(const std::vector<std::uint32_t> &values) {
        integer(values.size(), 2);
        for (auto value : values)
            integer(value, 4);
    }
    void recipe(const CosmeticRecipe &r) {
        integer(r.key, 4);
        integer(r.version, 4);
        words(r.scalars);
        integer(r.items.size(), 2);
        for (const auto &item : r.items) {
            integer(item.slot, 4);
            integer(item.asset.size(), 2);
            bytes.insert(bytes.end(), item.asset.begin(), item.asset.end());
            words(item.parameters);
        }
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    std::uint64_t integer(unsigned width) {
        if (at > bytes.size() || width > bytes.size() - at)
            throw std::runtime_error("Truncated packet");
        std::uint64_t result{};
        for (unsigned i = 0; i < width; ++i)
            result |= std::uint64_t{bytes[at++]} << (8 * i);
        return result;
    }
    float number() { return std::bit_cast<float>(static_cast<std::uint32_t>(integer(4))); }
    Transform transform() {
        Transform t;
        for (auto &v : t.position)
            v = number();
        for (auto &v : t.rotation)
            v = number();
        for (auto &v : t.scale)
            v = number();
        if (!valid_transform(t))
            throw std::runtime_error("Invalid transform");
        return t;
    }
    Transform compact_transform() {
        Transform t;
        const auto flags = integer(1);
        if (flags & ~15ULL)
            throw std::runtime_error("Unknown packed transform flags");
        for (auto &v : t.position)
            v = (flags & 1) ? number() : static_cast<std::int16_t>(integer(2)) / 1000.f;
        const unsigned largest = static_cast<unsigned>(flags >> 2);
        float norm{};
        for (unsigned i = 0; i < 4; ++i)
            if (i != largest) {
                t.rotation[i] = static_cast<std::int16_t>(integer(2)) / 46339.5358f;
                norm += t.rotation[i] * t.rotation[i];
            }
        if (norm > 1.0001f)
            throw std::runtime_error("Invalid packed quaternion");
        t.rotation[largest] = std::sqrt(std::max(0.f, 1.f - norm));
        if (flags & 2)
            for (auto &v : t.scale)
                v = number();
        if (!valid_transform(t))
            throw std::runtime_error("Invalid packed transform");
        return t;
    }
    std::vector<std::uint32_t> words(std::size_t limit) {
        const auto count = integer(2);
        if (count > limit || count > (bytes.size() - at) / 4)
            throw std::runtime_error("Invalid cosmetic parameter count");
        std::vector<std::uint32_t> result;
        result.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(static_cast<std::uint32_t>(integer(4)));
        return result;
    }
    CosmeticRecipe recipe() {
        CosmeticRecipe r;
        r.key = static_cast<std::uint32_t>(integer(4));
        r.version = static_cast<std::uint32_t>(integer(4));
        r.scalars = words(max_cosmetic_scalars);
        const auto count = integer(2);
        if (!count || count > max_cosmetic_slots)
            throw std::runtime_error("Invalid cosmetic slot count");
        r.items.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            CosmeticSlot item;
            item.slot = static_cast<std::uint32_t>(integer(4));
            const auto length = integer(2);
            if (length > max_cosmetic_asset || length > bytes.size() - at)
                throw std::runtime_error("Invalid cosmetic asset length");
            item.asset.assign(reinterpret_cast<const char *>(bytes.data() + at),
                              static_cast<std::size_t>(length));
            at += static_cast<std::size_t>(length);
            item.parameters = words(max_cosmetic_parameters);
            r.items.push_back(std::move(item));
        }
        return r;
    }
};
} // namespace
bool valid_roster(std::span<const Member> members, unsigned capacity) noexcept {
    if (capacity < 2 || capacity > max_players || members.empty() || members.size() > max_players)
        return false;
    // A lobby holds no more than it was opened for. A dedicated server can hold a few more than
    // it says: its reserved players and admins join past its limit (33 of 32).
    if (members.size() > capacity && !game_server_steam_id(members[0].id)) return false;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto &m = members[i];
        // The host comes first, and may be a dedicated server rather than a player.
        const bool server = i == 0 && game_server_steam_id(m.id);
        const bool identity = individual_steam_id(m.id) || server;
        if (!identity || !m.epoch || m.name.size() > 128 || (i == 0 && m.admin))
            return false;
        // A server is in no party; only a party's leader leads or opens it.
        if ((server && m.party) || (!m.party && (m.party_leader || m.party_open)) || (m.party_open && !m.party_leader))
            return false;
        // Names reach the game's own UI: whole UTF-8 characters only, and no controls.
        for (std::size_t at = 0; at < m.name.size();) {
            const auto length = utf8_length(m.name, at);
            const auto c = static_cast<unsigned char>(m.name[at]);
            if (!length || c < 32 || c == 127)
                return false;
            at += length;
        }
        unsigned leaders = m.party_leader ? 1U : 0U, size = 1;
        for (std::size_t j = 0; j < members.size(); ++j) {
            if (j < i && members[j].id == m.id)
                return false;
            if (j != i && m.party && members[j].party == m.party) {
                leaders += members[j].party_leader ? 1U : 0U;
                ++size;
            }
        }
        // Each party has one leader and at least one other member.
        if (m.party && (leaders != 1 || size < 2))
            return false;
    }
    return true;
}
bool valid_party_request(PartyAction action, std::uint64_t player) noexcept {
    switch (action) {
    case PartyAction::invite:
    case PartyAction::accept:
    case PartyAction::decline:
    case PartyAction::join:
    case PartyAction::kick:
    case PartyAction::promote:
    case PartyAction::invited:
    case PartyAction::withdrawn: return individual_steam_id(player);
    case PartyAction::leave:
    case PartyAction::open:
    case PartyAction::close: return player == 0;
    }
    return false;
}
bool valid_routes(std::span<const Member> members) noexcept {
    if (members.size() > max_remote_players)
        return false;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto &m = members[i];
        if (!individual_steam_id(m.id) || !m.epoch || !m.name.empty() || m.admin)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (members[j].id == m.id)
                return false;
    }
    return true;
}
bool valid_appearance(const Appearance &a) noexcept {
    if (a.skater.key != skater_recipe_key || a.skater.version != 2 || a.board.key != board_recipe_key ||
        a.board.version != 1)
        return false;
    std::size_t size = header_size + 13 + mark_items * 8;
    for (const auto *r : {&a.skater, &a.board}) {
        if (r->scalars.size() > max_cosmetic_scalars || r->items.empty() ||
            r->items.size() > max_cosmetic_slots)
            return false;
        size += 12 + r->scalars.size() * 4;
        for (std::size_t i = 0; i < r->items.size(); ++i) {
            const auto &item = r->items[i];
            if (!item.slot || item.asset.size() > max_cosmetic_asset ||
                item.parameters.size() > max_cosmetic_parameters)
                return false;
            for (unsigned char c : item.asset)
                if (c < 32 || c == 127)
                    return false;
            for (std::size_t j = 0; j < i; ++j)
                if (r->items[j].slot == item.slot)
                    return false;
            size += 8 + item.asset.size() + item.parameters.size() * 4;
        }
    }
    return size <= max_appearance_bytes;
}
bool valid_transform(const Transform &t) noexcept {
    for (float v : t.position)
        if (!std::isfinite(v) || std::abs(v) > 1000000)
            return false;
    for (float v : t.scale)
        if (!std::isfinite(v) || v < .0001f || v > 100)
            return false;
    float norm{};
    for (float v : t.rotation) {
        if (!std::isfinite(v))
            return false;
        norm += v * v;
    }
    return norm > .8f && norm < 1.2f;
}
bool valid_pose(const Pose &p) noexcept {
    return valid_transform(p.root) && p.skater.size() <= max_skater_bones &&
           p.board.size() <= max_board_bones &&
           std::all_of(p.skater.begin(), p.skater.end(), valid_transform) &&
           std::all_of(p.board.begin(), p.board.end(), valid_transform);
}
void offset_pose(Pose &pose, const std::array<float, 3> &offset) {
    // Both native rigs reserve bone 0 and put their world-space anchor in
    // bone 1. Child bones stay local. Board packet index 0 is the separate
    // entity transform, so its rig anchor is packet index 2.
    for (std::size_t i = 0; i < offset.size(); ++i) {
        pose.root.position[i] += offset[i];
        if (pose.skater.size() > 1)
            pose.skater[1].position[i] += offset[i];
        if (!pose.board.empty())
            pose.board.front().position[i] += offset[i];
        if (pose.board.size() > 2)
            pose.board[2].position[i] += offset[i];
    }
}
bool valid_map_destination(std::string_view value) noexcept {
    const auto split = value.find('|');
    return !value.empty() && value.size() <= 256 && split != std::string_view::npos && split > 0 &&
           value.find('|', split + 1) == std::string_view::npos &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string_view greeting_error(const Packet &p, std::uint64_t session, std::uint64_t map,
                                const std::array<std::uint8_t, 32> &build,
                                std::uint64_t peer_epoch) noexcept {
    if ((p.kind != PacketKind::hello && p.kind != PacketKind::welcome && p.kind != PacketKind::challenge &&
         p.kind != PacketKind::peer_hello && p.kind != PacketKind::peer_welcome) ||
        !p.session || !p.epoch || p.session != session)
        return "Join code or multiplayer protocol did not match.";
    if (p.build != build)
        return "Both players need the same supported game build.";
    if (!map || p.map != map)
        return "Map mismatch. Load the same map on both machines, then join again.";
    if (peer_epoch && p.epoch != peer_epoch)
        return "Peer changed level or session. Join again.";
    return {};
}
namespace {
void coarsen(Transform &t, unsigned bits) noexcept {
    if (!bits) return;
    unsigned largest{};
    float norm{};
    for (unsigned i = 0; i < 4; ++i) {
        norm += t.rotation[i] * t.rotation[i];
        if (std::abs(t.rotation[i]) > std::abs(t.rotation[largest])) largest = i;
    }
    if (!(norm > 0.f) || !std::isfinite(norm)) return;
    // As Writer::compact_transform packs it: the three smaller components, the largest positive.
    constexpr float scale = 46339.5358f;
    const float factor = (t.rotation[largest] < 0 ? -1.f : 1.f) * scale / std::sqrt(norm);
    const float step = static_cast<float>(1U << bits);
    const float most = std::floor(32767.f / step) * step; // a rounded value must still fit 16 bits
    float sum{};
    for (unsigned i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float packed = std::clamp(std::round(t.rotation[i] * factor / step) * step, -most, most);
        t.rotation[i] = packed / scale;
        sum += t.rotation[i] * t.rotation[i];
    }
    t.rotation[largest] = std::sqrt(std::max(0.f, 1.f - sum));
}
} // namespace
void limit_bone_scale(Pose &pose, float limit) noexcept {
    if (!(limit >= 1.f)) return;
    const auto hold = [&](Transform &t) {
        for (float &axis : t.scale) axis = std::isfinite(axis) ? std::clamp(axis, 1.f / limit, limit) : 1.f;
    };
    hold(pose.root);
    for (auto &t : pose.skater) hold(t);
    for (auto &t : pose.board) hold(t);
}
void limit_bone_reach(Pose &pose, float limit) noexcept {
    if (!(limit > 0.f)) return;
    const auto hold = [](std::array<float, 3> &position, const std::array<float, 3> &from, float most) {
        float squared{};
        for (unsigned i = 0; i < 3; ++i) squared += (position[i] - from[i]) * (position[i] - from[i]);
        if (!(squared > most * most)) return;
        const float factor = most / std::sqrt(squared);
        for (unsigned i = 0; i < 3; ++i) position[i] = from[i] + (position[i] - from[i]) * factor;
    };
    constexpr std::array<float, 3> parent{};
    // Bone 1 is the rig's place in the world (offset_pose): it goes with the root.
    for (std::size_t i = 0; i < pose.skater.size(); ++i) {
        if (i == 1) continue;
        const bool free = std::find(free_skater_bones.begin(), free_skater_bones.end(), i) != free_skater_bones.end();
        hold(pose.skater[i].position, parent, free ? std::max(limit, free_bone_reach) : limit);
    }
    // The board is its own actor (0) with its rig's place in the world (2): wherever it was
    // left, but in one piece.
    for (std::size_t i = 1; i < pose.board.size(); ++i) {
        if (i == 2) hold(pose.board[i].position, pose.board[0].position, std::max(limit, 5.f));
        else hold(pose.board[i].position, parent, limit);
    }
}
void coarsen_rotations(Pose &pose, unsigned bits) noexcept {
    bits = std::min(bits, 12U);
    coarsen(pose.root, std::min(bits, 6U)); // which way they face matters from further off than a finger does
    for (auto &t : pose.skater) coarsen(t, bits);
    for (auto &t : pose.board) coarsen(t, bits);
}
std::vector<std::uint8_t> encode(const Packet &p, bool compact_pose) {
    return encode(p, compact_pose, p.pose_interval_us);
}
std::vector<std::uint8_t> encode(const Packet &p, bool compact_pose, std::uint32_t pose_interval_us) {
    if (!p.session || !p.epoch || (!p.world && p.kind != PacketKind::map_request) ||
        (p.kind == PacketKind::pose && (!p.map || !valid_pose(p.pose))) ||
        (p.kind == PacketKind::cosmetics && (!p.map || !valid_appearance(p.appearance))) ||
        (p.kind == PacketKind::audio && (!p.map || !valid_audio_batch(p.audio))))
        throw std::invalid_argument("Invalid multiplayer packet");
    if (p.kind == PacketKind::roster && (!p.map || !valid_roster(p.members, p.capacity) || !p.distances.valid() || !p.voice_policy.revision))
        throw std::invalid_argument("Invalid session roster");
    if (p.kind == PacketKind::routes && (!p.map || !valid_routes(p.members)))
        throw std::invalid_argument("Invalid direct route report");
    if (p.kind == PacketKind::objects && (!p.map || !p.source || !valid_object_chunk(p.objects)))
        throw std::invalid_argument("Invalid object update");
    if (p.kind == PacketKind::voice && (!p.map || !p.source || !valid_voice(p.voice)))
        throw std::invalid_argument("Invalid voice packet");
    if (p.kind == PacketKind::effects && (!p.map || !p.source || !valid_impacts(p.impacts)))
        throw std::invalid_argument("Invalid effects packet");
    if (p.kind == PacketKind::chat && (!p.source || !valid_chat_text(p.text)))
        throw std::invalid_argument("Invalid chat message");
    if (p.kind == PacketKind::admin && (!p.source || !valid_admin_text(p.text)))
        throw std::invalid_argument("Invalid admin message");
    if (p.kind == PacketKind::throwdown && (!p.source || p.throwdown.empty() || p.throwdown.size() > max_throwdown_message))
        throw std::invalid_argument("Invalid throwdown message");
    if (p.kind == PacketKind::physics_tuning && (!p.source || p.tuning.size() > max_physics_tuning))
        throw std::invalid_argument("Invalid physics tuning");
    if (p.kind == PacketKind::physics_extras && (!p.source || p.extras.size() > max_physics_extras))
        throw std::invalid_argument("Invalid physics extras");
    if (p.kind == PacketKind::party && (!p.source || !valid_party_request(p.party_action, p.party_player)))
        throw std::invalid_argument("Invalid party message");
    if (p.kind == PacketKind::scoring && (!p.source || (!p.text.empty() && !valid_admin_text(p.text))))
        throw std::invalid_argument("Invalid scoring report");
    if (p.kind == PacketKind::teleport &&
        (!p.source || std::any_of(p.teleport.begin(), p.teleport.end(), [](float v) { return !std::isfinite(v) || std::abs(v) > 1e6f; })))
        throw std::invalid_argument("Invalid teleport");
    if (p.kind == PacketKind::bans &&
        (!p.source || p.bans.size() > max_ban_rows || p.ban_total < p.bans.size() ||
         std::any_of(p.bans.begin(), p.bans.end(), [](const auto &ban) {
             return !individual_steam_id(ban.id) || !valid_member_name(ban.name);
         })))
        throw std::invalid_argument("Invalid ban list");
    if (p.kind == PacketKind::maps &&
        (!p.source || p.maps.size() > max_server_maps || !valid_map_pool(p.map_pool, p.maps.size()) ||
         p.map_rotation > max_map_rotation ||
         std::any_of(p.maps.begin(), p.maps.end(), [](const auto &asset) { return !valid_map_asset(asset); })))
        throw std::invalid_argument("Invalid server map list");
    if (p.kind == PacketKind::hello && !p.text.empty() && !valid_member_name(p.text))
        throw std::invalid_argument("Invalid player name");
    if (p.kind == PacketKind::roster && !valid_voice_range(p.voice_range))
        throw std::invalid_argument("Invalid voice range");
    if (p.kind == PacketKind::map_offer &&
        (!valid_map_destination(p.destination) || p.map != map_hash(p.destination)))
        throw std::invalid_argument("Invalid host map destination");
    if ((p.kind == PacketKind::map_offer || p.kind == PacketKind::world_state) && !valid_map_label(p.map_label))
        throw std::invalid_argument("Invalid map name");
    if (p.kind == PacketKind::world_state &&
        (p.destination.empty() ? (p.map != 0 || p.world_ready)
                               : (!valid_map_destination(p.destination) || p.map != map_hash(p.destination))))
        throw std::invalid_argument("Invalid world transition");
    const bool packed = compact_pose && p.kind == PacketKind::pose;
    const bool greeting = p.kind == PacketKind::hello || p.kind == PacketKind::welcome ||
                          p.kind == PacketKind::challenge || p.kind == PacketKind::peer_hello ||
                          p.kind == PacketKind::peer_welcome || p.kind == PacketKind::map_request ||
                          p.kind == PacketKind::map_offer;
    if (!greeting && p.kind != PacketKind::pose && p.kind != PacketKind::away &&
        p.kind != PacketKind::cosmetics && p.kind != PacketKind::audio && p.kind != PacketKind::roster &&
        p.kind != PacketKind::routes && p.kind != PacketKind::world_state && p.kind != PacketKind::world_ready &&
        p.kind != PacketKind::objects && p.kind != PacketKind::voice && p.kind != PacketKind::chat &&
        p.kind != PacketKind::admin && p.kind != PacketKind::bans && p.kind != PacketKind::maps &&
        p.kind != PacketKind::throwdown && p.kind != PacketKind::teleport && p.kind != PacketKind::physics_tuning &&
        p.kind != PacketKind::party && p.kind != PacketKind::scoring && p.kind != PacketKind::physics_extras &&
        p.kind != PacketKind::effects)
        throw std::invalid_argument("Unknown packet kind");
    const auto payload = greeting ? 72
                         : p.kind == PacketKind::away
                             ? 0
                             : 44 + (p.pose.skater.size() + p.pose.board.size()) * 40;
    Writer w;
    w.bytes.reserve(header_size + payload);
    w.integer(magic, 4);
    w.integer(protocol_version, 2);
    w.integer(packed ? 8U : static_cast<unsigned>(p.kind), 2);
    w.integer(payload, 4);
    w.integer(p.sequence, 4);
    w.integer(p.session, 8);
    w.integer(p.map, 8);
    w.integer(p.epoch, 8);
    w.integer(p.time_us, 8);
    w.integer(p.source, 8);
    w.integer(p.world, 8);
    if (greeting) {
        w.bytes.insert(w.bytes.end(), p.build.begin(), p.build.end());
        w.integer(p.challenge, 8);
        w.bytes.insert(w.bytes.end(), p.proof.begin(), p.proof.end());
        if (p.kind == PacketKind::map_offer) {
            w.integer(p.map_authorized ? 1 : 0, 1);
            w.integer(p.destination.size(), 2);
            w.bytes.insert(w.bytes.end(), p.destination.begin(), p.destination.end());
            w.integer(p.map_label.size(), 1);
            w.bytes.insert(w.bytes.end(), p.map_label.begin(), p.map_label.end());
        }
        if (p.kind == PacketKind::hello) {
            w.integer(p.text.size(), 1);
            w.bytes.insert(w.bytes.end(), p.text.begin(), p.text.end());
        }
    } else if (p.kind == PacketKind::world_state) {
        w.bytes.insert(w.bytes.end(), p.build.begin(), p.build.end());
        w.integer(p.world_ready ? 1 : 0, 1);
        w.integer(p.destination.size(), 2);
        w.bytes.insert(w.bytes.end(), p.destination.begin(), p.destination.end());
        w.integer(p.map_label.size(), 1);
        w.bytes.insert(w.bytes.end(), p.map_label.begin(), p.map_label.end());
    } else if (p.kind == PacketKind::world_ready) {
        w.integer(p.world_ready ? 1 : 0, 1);
    } else if (p.kind == PacketKind::pose) {
        w.integer(p.pose.skater.size(), 2);
        w.integer(p.pose.board.size(), 2);
        if (!valid_pose_interval(pose_interval_us))
            throw std::invalid_argument("Invalid pose update interval");
        // Version 19 stores TPS, preserving fractional-millisecond rates such
        // as 30/60/120 without silently rounding them to 33/16/8 ms.
        w.integer((1000000U / pose_interval_us) | (p.player_collision ? 0x8000U : 0U), 2);
        const auto transform = [&](const Transform &t) {
            if (packed)
                w.compact_transform(t);
            else
                w.transform(t);
        };
        transform(p.pose.root);
        for (const auto &t : p.pose.skater)
            transform(t);
        for (const auto &t : p.pose.board)
            transform(t);
    } else if (p.kind == PacketKind::cosmetics) {
        w.recipe(p.appearance.skater);
        w.recipe(p.appearance.board);
        w.integer(p.appearance.card.background, 4);
        w.integer(p.appearance.card.emblem, 4);
        w.integer(p.appearance.card.title, 4);
        w.integer((p.appearance.hide_tag ? 1 : 0) | (p.appearance.hide_items ? 2 : 0), 1);
        for (const auto &style : p.appearance.marks) {
            w.integer(static_cast<std::uint8_t>(style.mode), 1);
            for (const auto part : style.from) w.integer(part, 1);
            for (const auto part : style.to) w.integer(part, 1);
            w.integer(style.speed, 1);
        }
    } else if (p.kind == PacketKind::audio) {
        w.integer(p.audio.size(), 2);
        AudioState previous;
        for (const auto &s : p.audio) {
            w.integer(s.age_us | (s.event ? 0x80000000U : 0), 4);
            std::uint64_t values{}, selectors{}, flags{};
            for (std::size_t i = 0; i < audio_float_count; ++i)
                if (std::bit_cast<std::uint32_t>(s.state.values[i]) != std::bit_cast<std::uint32_t>(previous.values[i]))
                    values |= 1ULL << i;
            for (std::size_t i = 0; i < audio_selector_count; ++i)
                if (s.state.selectors[i] != previous.selectors[i]) selectors |= 1ULL << i;
            for (std::size_t i = 0; i < audio_flag_count; ++i)
                flags |= std::uint64_t{s.state.flags[i]} << i;
            w.integer(values, 8);
            w.integer(selectors, 4);
            w.integer(flags, 6);
            for (std::size_t i = 0; i < audio_float_count; ++i)
                if (values & (1ULL << i)) w.integer(std::bit_cast<std::uint32_t>(s.state.values[i]), 4);
            for (std::size_t i = 0; i < audio_selector_count; ++i)
                if (selectors & (1ULL << i)) w.integer(s.state.selectors[i], 4);
            previous = s.state;
        }
    }
    if (p.kind == PacketKind::voice) {
        w.integer(std::bit_cast<std::uint32_t>(p.voice.distance), 4);
        w.integer(std::bit_cast<std::uint32_t>(p.voice.gain), 4);
        w.integer(p.voice.policy_revision, 4);
        w.integer(p.voice.bytes.size(), 2);
        w.bytes.insert(w.bytes.end(), p.voice.bytes.begin(), p.voice.bytes.end());
    }
    if (p.kind == PacketKind::chat || p.kind == PacketKind::admin) {
        w.integer(p.text.size(), 2);
        w.bytes.insert(w.bytes.end(), p.text.begin(), p.text.end());
    }
    if (p.kind == PacketKind::throwdown) {
        w.integer(p.throwdown.size(), 2);
        w.bytes.insert(w.bytes.end(), p.throwdown.begin(), p.throwdown.end());
    }
    if (p.kind == PacketKind::teleport)
        for (const auto v : p.teleport) w.integer(std::bit_cast<std::uint32_t>(v), 4);
    if (p.kind == PacketKind::party) {
        w.integer(static_cast<std::uint8_t>(p.party_action), 1);
        w.integer(p.party_player, 8);
    }
    if (p.kind == PacketKind::physics_tuning) {
        w.integer(p.tuning.size(), 2);
        w.bytes.insert(w.bytes.end(), p.tuning.begin(), p.tuning.end());
    }
    if (p.kind == PacketKind::effects) {
        w.integer(p.impacts.size(), 1);
        for (const auto &impact : p.impacts) {
            for (const auto v : impact.position) w.integer(std::bit_cast<std::uint32_t>(v), 4);
            for (const auto v : impact.velocity)
                w.integer(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 100.f))), 2);
            for (const auto v : impact.normal)
                w.integer(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(v * 127.f))), 1);
            w.integer(impact.material, 2);
        }
    }
    if (p.kind == PacketKind::physics_extras) {
        w.integer(p.extras.size(), 2);
        w.bytes.insert(w.bytes.end(), p.extras.begin(), p.extras.end());
    }
    if (p.kind == PacketKind::scoring) {
        w.integer(p.scoring, 8);
        w.integer(p.text.size(), 2);
        w.bytes.insert(w.bytes.end(), p.text.begin(), p.text.end());
    }
    if (p.kind == PacketKind::maps) {
        w.integer(p.maps.size(), 2);
        for (const auto &asset : p.maps) {
            w.integer(asset.size(), 1);
            w.bytes.insert(w.bytes.end(), asset.begin(), asset.end());
        }
        w.integer(p.map_pool.size(), 2);
        for (const auto index : p.map_pool) w.integer(index, 2);
        w.integer(p.map_rotation, 2);
    }
    if (p.kind == PacketKind::bans) {
        w.integer(p.ban_total, 4);
        w.integer(p.bans.size(), 2);
        for (const auto &ban : p.bans) {
            w.integer(ban.id, 8);
            w.integer(static_cast<std::uint64_t>(ban.added), 8);
            w.integer(ban.name.size(), 1);
            w.bytes.insert(w.bytes.end(), ban.name.begin(), ban.name.end());
        }
    }
    if (p.kind == PacketKind::roster) {
        w.integer(p.capacity, 1);
        w.integer(p.members.size(), 1);
        for (const auto &m : p.members) {
            w.integer(m.id, 8);
            w.integer(m.epoch, 8);
            w.integer(m.name.size(), 1);
            w.bytes.insert(w.bytes.end(), m.name.begin(), m.name.end());
            w.integer((m.admin ? 1U : 0U) | (m.party_leader ? 2U : 0U) | (m.party_open ? 4U : 0U) | (m.speeding ? 8U : 0U) |
                          (m.scoring ? 16U : 0U), 1);
            w.integer(m.party, 4);
        }
        w.integer(p.distances.full_rate_return, 2);
        w.integer(p.distances.half_rate_start, 2);
        w.integer(p.distances.half_rate_return, 2);
        w.integer(p.distances.low_rate_start, 2);
        for (unsigned lot = 0; lot < p.parks.size(); ++lot)
            w.integer(encode_park(lot, p.parks[lot]), 1);
        w.integer(p.object_clears, 4);
        if (!valid_multiplayer_tps(p.tps)) throw std::invalid_argument("Invalid session TPS");
        w.integer(p.tps, 1);
        w.integer(p.chat_badge & 0xffffff, 3); // red, green, blue
        w.integer(p.chat_text & 0xffffff, 3);
        // Short text: its length in `width` bytes, then the text.
        const auto text = [&w](std::string_view value, unsigned width) {
            w.integer(value.size(), width);
            w.bytes.insert(w.bytes.end(), value.begin(), value.end());
        };
        w.integer(p.vote.id, 4);
        if (p.vote.id) {
            if (!valid_server_vote(p.vote) || p.vote.outcome > vote_cancelled) throw std::invalid_argument("Invalid server vote");
            w.integer(p.vote.kind, 1);
            w.integer(p.vote.outcome, 1);
            w.integer(p.vote.yes, 2);
            w.integer(p.vote.no, 2);
            w.integer(p.vote.needed, 2);
            w.integer(p.vote.seconds, 2);
            w.integer(p.vote.starter, 8);
            w.integer(p.vote.target, 8);
            w.integer(p.vote.label.size(), 1);
            w.bytes.insert(w.bytes.end(), p.vote.label.begin(), p.vote.label.end());
            w.integer(p.vote.answers.size(), 1);
            for (std::size_t i = 0; i < p.vote.answers.size(); ++i) {
                text(p.vote.answers[i], 1);
                w.integer(p.vote.counts[i], 2);
            }
        }
        w.integer(static_cast<std::uint8_t>(p.object_placement), 1);
        if (!valid_object_limit(p.object_limit)) throw std::invalid_argument("Invalid object limit");
        w.integer(p.object_limit, 2);
        w.integer(p.object_scaling ? 1 : 0, 1);
        w.integer(p.sync_effects ? 1 : 0, 1);
        w.integer(p.force_world_layers, 1);
        // One mode per world-layer catalog row; both peers read the same catalog
        // from the same game build.
        // No layers (a dedicated server without the catalog) means every layer at its default.
        if (p.layers.size() > 0xffff || (p.layers.empty() && p.force_world_layers))
            throw std::runtime_error("Invalid world layer state");
        w.integer(p.layers.size(), 2);
        for (const auto &choice : p.layers) {
            if (choice >= world_layer_modes.size()) throw std::runtime_error("Invalid world layer mode");
            w.integer(choice, 1);
        }
        w.integer(p.voice_policy.allowed, 1);
        w.integer(p.voice_policy.revision, 4);
        w.integer(static_cast<std::uint16_t>(std::lround(p.voice_range)), 2);
        if (p.server_votes > 7) throw std::invalid_argument("Invalid server votes");
        w.integer((p.guest_noclip ? 1U : 0U) | (p.guest_no_bail ? 2U : 0U) | (p.guest_boosts ? 4U : 0U) |
                      (static_cast<unsigned>(p.server_votes) << 3) | (p.enforce_tuning ? 64U : 0U), 1);
        if (p.server_polls > static_cast<std::uint8_t>(ServerPolls::everyone) || p.server_custom_votes.size() > server_custom_vote_limit ||
            !std::all_of(p.server_custom_votes.begin(), p.server_custom_votes.end(), [](const auto &v) { return valid_server_custom_vote(v); }))
            throw std::invalid_argument("Invalid server votes");
        w.integer(p.server_polls, 1);
        w.integer(p.server_custom_votes.size(), 1);
        for (const auto &vote : p.server_custom_votes) {
            text(vote.name, 1);
            text(vote.description, 1);
            w.integer(vote.choices.size(), 1);
            for (const auto &choice : vote.choices) text(choice, 1);
        }
        w.integer(p.announcement.id, 4);
        if (p.announcement.id) {
            if (!valid_chat_text(p.announcement.text)) throw std::invalid_argument("Invalid server announcement");
            w.integer(p.announcement.seconds, 2);
            text(p.announcement.text, 1);
        }
    }
    if (p.kind == PacketKind::routes) {
        w.integer(p.members.size(), 1);
        for (const auto &m : p.members) {
            w.integer(m.id, 8);
            w.integer(m.epoch, 8);
        }
    }
    if (p.kind == PacketKind::objects) {
        const auto &chunk = p.objects;
        w.integer(chunk.base, 8); w.integer(chunk.revision, 8);
        w.integer(chunk.part, 2); w.integer(chunk.parts, 2);
        w.integer(chunk.objects.size(), 2); w.integer(chunk.removed.size(), 2);
        for (const auto &object : chunk.objects) {
            w.integer(object.id, 8); w.integer(object.item.size(), 2);
            w.bytes.insert(w.bytes.end(), object.item.begin(), object.item.end());
            for (float value : object.position) w.integer(std::bit_cast<std::uint32_t>(value), 4);
            for (float value : object.rotation) w.integer(std::bit_cast<std::uint32_t>(value), 4);
            w.integer(std::bit_cast<std::uint32_t>(object.scale), 4);
        }
        for (const auto id : chunk.removed) w.integer(id, 8);
    }
    const auto actual_payload = w.bytes.size() - header_size;
    for (unsigned i = 0; i < 4; ++i)
        w.bytes[8 + i] = static_cast<std::uint8_t>(actual_payload >> (8 * i));
    return std::move(w.bytes);
}
static bool read_map_label(Reader &r, std::span<const std::uint8_t> bytes, Packet &p) {
    const auto length = r.integer(1);
    if (length > max_member_name || length > bytes.size() - r.at) return false;
    p.map_label.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
    r.at += static_cast<std::size_t>(length);
    return valid_map_label(p.map_label);
}
// Short text written by encode's `text`: nothing when it is longer than `limit` or the packet.
static std::optional<std::string> read_text(Reader &r, std::span<const std::uint8_t> bytes, unsigned width, std::size_t limit) {
    const auto length = r.integer(width);
    if (length > limit || length > bytes.size() - r.at) return {};
    std::string value(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
    r.at += static_cast<std::size_t>(length);
    return value;
}
std::optional<Packet> decode(std::span<const std::uint8_t> bytes) noexcept {
    try {
        if (bytes.size() < header_size || bytes.size() > max_packet)
            return {};
        Reader r{bytes};
        if (r.integer(4) != magic || r.integer(2) != protocol_version)
            return {};
        Packet p;
        const auto kind = r.integer(2);
        const bool packed = kind == 8;
        p.kind = packed ? PacketKind::pose : static_cast<PacketKind>(kind);
        if (r.integer(4) != bytes.size() - header_size)
            return {};
        p.sequence = static_cast<std::uint32_t>(r.integer(4));
        p.session = r.integer(8);
        p.map = r.integer(8);
        p.epoch = r.integer(8);
        p.time_us = r.integer(8);
        p.source = r.integer(8);
        p.world = r.integer(8);
        if (!p.session || !p.epoch || (!p.world && p.kind != PacketKind::map_request))
            return {};
        if (p.kind == PacketKind::hello || p.kind == PacketKind::welcome || p.kind == PacketKind::challenge ||
            p.kind == PacketKind::peer_hello || p.kind == PacketKind::peer_welcome ||
            p.kind == PacketKind::map_request || p.kind == PacketKind::map_offer) {
            for (auto &v : p.build)
                v = static_cast<std::uint8_t>(r.integer(1));
            p.challenge = r.integer(8);
            for (auto &v : p.proof)
                v = static_cast<std::uint8_t>(r.integer(1));
            if (p.kind == PacketKind::map_offer) {
                const auto authorized = r.integer(1), length = r.integer(2);
                if (authorized > 1 || length > 256 || length > bytes.size() - r.at)
                    return {};
                p.map_authorized = authorized != 0;
                p.destination.assign(reinterpret_cast<const char *>(bytes.data() + r.at),
                                     static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                if (!valid_map_destination(p.destination) || p.map != map_hash(p.destination) || !read_map_label(r, bytes, p))
                    return {};
            }
            if (p.kind == PacketKind::hello) {
                const auto length = r.integer(1);
                if (length > max_member_name || length > bytes.size() - r.at) return {};
                p.text.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                if (!p.text.empty() && !valid_member_name(p.text)) return {};
            }
        } else if (p.kind == PacketKind::world_state || p.kind == PacketKind::world_ready) {
            if (p.kind == PacketKind::world_state)
                for (auto &v : p.build) v = static_cast<std::uint8_t>(r.integer(1));
            const auto ready = r.integer(1);
            if (ready > 1) return {};
            p.world_ready = ready != 0;
            if (p.kind == PacketKind::world_state) {
                const auto length = r.integer(2);
                if (length > 256 || length > bytes.size() - r.at) return {};
                p.destination.assign(reinterpret_cast<const char *>(bytes.data() + r.at),
                                     static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                if (p.destination.empty() ? (p.map != 0 || p.world_ready)
                                          : (!valid_map_destination(p.destination) || p.map != map_hash(p.destination)))
                    return {};
                if (!read_map_label(r, bytes, p)) return {};
            }
        } else if (p.kind == PacketKind::pose) {
            const auto skater = r.integer(2), board = r.integer(2);
            const auto rate = static_cast<unsigned>(r.integer(2));
            const auto tps = rate & 0x7fffU;
            p.player_collision = (rate & 0x8000U) != 0;
            if (!valid_multiplayer_tps(tps) && tps != 10 && tps != 5) return {};
            p.pose_interval_us = 1000000U / tps;
            if (!p.map || skater > max_skater_bones || board > max_board_bones ||
                !valid_pose_interval(p.pose_interval_us) ||
                (!packed && bytes.size() != header_size + 46 + (skater + board) * 40))
                return {};
            const auto transform = [&] { return packed ? r.compact_transform() : r.transform(); };
            p.pose.root = transform();
            p.pose.skater.reserve(static_cast<std::size_t>(skater));
            p.pose.board.reserve(static_cast<std::size_t>(board));
            for (std::uint64_t i = 0; i < skater; ++i)
                p.pose.skater.push_back(transform());
            for (std::uint64_t i = 0; i < board; ++i)
                p.pose.board.push_back(transform());
        } else if (p.kind == PacketKind::cosmetics) {
            p.appearance.skater = r.recipe();
            p.appearance.board = r.recipe();
            p.appearance.card.background = static_cast<std::uint32_t>(r.integer(4));
            p.appearance.card.emblem = static_cast<std::uint32_t>(r.integer(4));
            p.appearance.card.title = static_cast<std::uint32_t>(r.integer(4));
            const auto flags = r.integer(1);
            p.appearance.hide_tag = (flags & 1) != 0;
            p.appearance.hide_items = (flags & 2) != 0;
            bool styled = true;
            for (auto &style : p.appearance.marks) {
                style.mode = static_cast<MarkMode>(r.integer(1));
                for (auto &part : style.from) part = static_cast<std::uint8_t>(r.integer(1));
                for (auto &part : style.to) part = static_cast<std::uint8_t>(r.integer(1));
                style.speed = static_cast<std::uint8_t>(r.integer(1));
                styled = styled && valid_mark_style(style);
            }
            if (flags > 3 || !styled || !p.map || !valid_appearance(p.appearance))
                return {};
        } else if (p.kind == PacketKind::audio) {
            const auto count = r.integer(2);
            if (!p.map || !count || count > max_audio_samples)
                return {};
            p.audio.resize(static_cast<std::size_t>(count));
            AudioState previous;
            for (auto &s : p.audio) {
                const auto age = static_cast<std::uint32_t>(r.integer(4));
                s.age_us = age & 0x7fffffffU;
                s.event = (age & 0x80000000U) != 0;
                const auto values = r.integer(8), selectors = r.integer(4), flags = r.integer(6);
                if ((values >> audio_float_count) || (selectors >> audio_selector_count) ||
                    (flags >> audio_flag_count)) return {};
                s.state = previous;
                for (std::size_t i = 0; i < audio_float_count; ++i)
                    if (values & (1ULL << i)) s.state.values[i] = r.number();
                for (std::size_t i = 0; i < audio_selector_count; ++i)
                    if (selectors & (1ULL << i)) s.state.selectors[i] = static_cast<std::uint32_t>(r.integer(4));
                for (std::size_t i = 0; i < audio_flag_count; ++i)
                    s.state.flags[i] = static_cast<std::uint8_t>((flags >> i) & 1);
                previous = s.state;
            }
            if (!valid_audio_batch(p.audio))
                return {};
        } else if (p.kind == PacketKind::voice) {
            p.voice.distance = r.number();
            p.voice.gain = r.number();
            p.voice.policy_revision = static_cast<std::uint32_t>(r.integer(4));
            const auto count = r.integer(2);
            if (!p.map || !p.source || !count || count > max_voice_bytes || count != bytes.size() - r.at)
                return {};
            p.voice.bytes.assign(bytes.begin() + r.at, bytes.end());
            r.at = bytes.size();
            if (!valid_voice(p.voice)) return {};
        } else if (p.kind == PacketKind::chat) {
            const auto length = r.integer(2);
            if (!p.source || !length || length > multiplayer_chat_max_bytes || length != bytes.size() - r.at)
                return {};
            p.text.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
            r.at = bytes.size();
            if (!valid_chat_text(p.text)) return {};
        } else if (p.kind == PacketKind::throwdown) {
            const auto length = r.integer(2);
            if (!p.source || !length || length > max_throwdown_message || length != bytes.size() - r.at)
                return {};
            p.throwdown.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.at), bytes.end());
            r.at = bytes.size();
        } else if (p.kind == PacketKind::physics_tuning) {
            const auto length = r.integer(2);
            if (!p.source || length > max_physics_tuning || length != bytes.size() - r.at)
                return {};
            p.tuning.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.at), bytes.end());
            r.at = bytes.size();
        } else if (p.kind == PacketKind::effects) {
            const auto count = r.integer(1);
            if (!p.map || !p.source || !count || count > max_impacts || count * impact_wire_size != bytes.size() - r.at)
                return {};
            p.impacts.resize(static_cast<std::size_t>(count));
            for (auto &impact : p.impacts) {
                for (auto &v : impact.position) v = r.number();
                for (auto &v : impact.velocity)
                    v = static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>(r.integer(2)))) / 100.f;
                for (auto &v : impact.normal)
                    v = static_cast<float>(static_cast<std::int8_t>(static_cast<std::uint8_t>(r.integer(1)))) / 127.f;
                impact.material = static_cast<std::uint16_t>(r.integer(2));
            }
            if (!valid_impacts(p.impacts)) return {};
        } else if (p.kind == PacketKind::physics_extras) {
            const auto length = r.integer(2);
            if (!p.source || length > max_physics_extras || length != bytes.size() - r.at)
                return {};
            p.extras.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.at), bytes.end());
            r.at = bytes.size();
        } else if (p.kind == PacketKind::scoring) {
            p.scoring = r.integer(8);
            const auto length = r.integer(2);
            if (!p.source || length > max_admin_text || length != bytes.size() - r.at) return {};
            p.text.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
            r.at = bytes.size();
            if (!p.text.empty() && !valid_admin_text(p.text)) return {};
        } else if (p.kind == PacketKind::party) {
            const auto action = r.integer(1);
            p.party_action = static_cast<PartyAction>(action);
            p.party_player = r.integer(8);
            if (!p.source || !valid_party_request(p.party_action, p.party_player)) return {};
        } else if (p.kind == PacketKind::teleport) {
            if (!p.source) return {};
            for (auto &v : p.teleport) {
                v = r.number();
                if (!std::isfinite(v) || std::abs(v) > 1e6f) return {};
            }
        } else if (p.kind == PacketKind::admin) {
            const auto length = r.integer(2);
            if (!p.source || !length || length > max_admin_text || length != bytes.size() - r.at)
                return {};
            p.text.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
            r.at = bytes.size();
            if (!valid_admin_text(p.text)) return {};
        } else if (p.kind == PacketKind::maps) {
            const auto count = r.integer(2);
            if (!p.source || count > max_server_maps) return {};
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto length = r.integer(1);
                if (length > max_map_asset || length > bytes.size() - r.at) return {};
                std::string asset(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                if (!valid_map_asset(asset)) return {};
                p.maps.push_back(std::move(asset));
            }
            const auto pooled = r.integer(2);
            if (pooled > p.maps.size()) return {};
            for (std::uint64_t i = 0; i < pooled; ++i) p.map_pool.push_back(static_cast<std::uint16_t>(r.integer(2)));
            p.map_rotation = static_cast<std::uint16_t>(r.integer(2));
            if (!valid_map_pool(p.map_pool, p.maps.size()) || p.map_rotation > max_map_rotation) return {};
        } else if (p.kind == PacketKind::bans) {
            p.ban_total = static_cast<std::uint32_t>(r.integer(4));
            const auto count = r.integer(2);
            if (!p.source || count > max_ban_rows || count > p.ban_total) return {};
            for (std::uint64_t i = 0; i < count; ++i) {
                MultiplayerBan ban;
                ban.id = r.integer(8);
                ban.added = static_cast<std::int64_t>(r.integer(8));
                const auto length = r.integer(1);
                if (length > max_member_name || length > bytes.size() - r.at) return {};
                ban.name.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                if (!individual_steam_id(ban.id) || !valid_member_name(ban.name)) return {};
                p.bans.push_back(std::move(ban));
            }
        } else if (p.kind == PacketKind::roster) {
            p.capacity = static_cast<unsigned>(r.integer(1));
            const auto count = r.integer(1);
            if (!p.map || count > max_players)
                return {};
            for (std::uint64_t i = 0; i < count; ++i) {
                Member m;
                m.id = r.integer(8);
                m.epoch = r.integer(8);
                const auto length = r.integer(1);
                if (length > 128 || length > bytes.size() - r.at)
                    return {};
                m.name.assign(reinterpret_cast<const char *>(bytes.data() + r.at),
                              static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                const auto flags = r.integer(1);
                if (flags > 31) return {};
                m.admin = (flags & 1) != 0;
                m.speeding = (flags & 8) != 0;
                m.scoring = (flags & 16) != 0;
                m.party_leader = (flags & 2) != 0;
                m.party_open = (flags & 4) != 0;
                m.party = static_cast<std::uint32_t>(r.integer(4));
                p.members.push_back(std::move(m));
            }
            p.distances.full_rate_return = static_cast<int>(r.integer(2));
            p.distances.half_rate_start = static_cast<int>(r.integer(2));
            p.distances.half_rate_return = static_cast<int>(r.integer(2));
            p.distances.low_rate_start = static_cast<int>(r.integer(2));
            for (unsigned lot = 0; lot < p.parks.size(); ++lot)
                p.parks[lot] = decode_park(lot, static_cast<unsigned>(r.integer(1)));
            p.object_clears = static_cast<std::uint32_t>(r.integer(4));
            p.tps = static_cast<unsigned>(r.integer(1));
            if (!valid_multiplayer_tps(p.tps)) return {};
            p.chat_badge = 0xff000000U | static_cast<std::uint32_t>(r.integer(3));
            p.chat_text = 0xff000000U | static_cast<std::uint32_t>(r.integer(3));
            p.vote.id = static_cast<std::uint32_t>(r.integer(4));
            if (p.vote.id) {
                p.vote.kind = static_cast<std::uint8_t>(r.integer(1));
                p.vote.outcome = static_cast<std::uint8_t>(r.integer(1));
                p.vote.yes = static_cast<std::uint16_t>(r.integer(2));
                p.vote.no = static_cast<std::uint16_t>(r.integer(2));
                p.vote.needed = static_cast<std::uint16_t>(r.integer(2));
                p.vote.seconds = static_cast<std::uint16_t>(r.integer(2));
                p.vote.starter = r.integer(8);
                p.vote.target = r.integer(8);
                const auto length = r.integer(1);
                if (p.vote.outcome > vote_cancelled || length > max_vote_label || length > bytes.size() - r.at) return {};
                p.vote.label.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                const auto answers = r.integer(1);
                if (answers > max_vote_answers) return {};
                for (std::uint64_t i = 0; i < answers; ++i) {
                    auto answer = read_text(r, bytes, 1, max_vote_answer);
                    if (!answer) return {};
                    p.vote.answers.push_back(std::move(*answer));
                    p.vote.counts.push_back(static_cast<std::uint16_t>(r.integer(2)));
                }
                if (!valid_server_vote(p.vote)) return {};
            }
            const auto placement = r.integer(1);
            if (!valid_object_placement(placement)) return {};
            p.object_placement = static_cast<ObjectPlacement>(placement);
            const auto object_limit = r.integer(2);
            if (!valid_object_limit(object_limit)) return {};
            p.object_limit = static_cast<unsigned>(object_limit);
            const auto scaling = r.integer(1);
            if (scaling > 1) return {};
            p.object_scaling = scaling != 0;
            const auto shared_effects = r.integer(1);
            if (shared_effects > 1) return {};
            p.sync_effects = shared_effects != 0;
            const auto forced = r.integer(1);
            if (forced > 1) return {};
            p.force_world_layers = forced != 0;
            const auto layer_count = r.integer(2);
            if (layer_count != world_layers().size() && (layer_count || p.force_world_layers)) return {};
            p.layers.resize(world_layers().size());
            for (std::uint64_t i = 0; i < layer_count; ++i) {
                auto &choice = p.layers[static_cast<std::size_t>(i)];
                const auto mode = r.integer(1);
                if (mode >= world_layer_modes.size()) return {};
                choice = static_cast<std::uint8_t>(mode);
            }
            const auto voice_allowed = r.integer(1);
            p.voice_policy.revision = static_cast<std::uint32_t>(r.integer(4));
            if (voice_allowed > 1 || !p.voice_policy.revision) return {};
            p.voice_policy.allowed = voice_allowed != 0;
            p.voice_range = static_cast<float>(r.integer(2));
            if (!valid_voice_range(p.voice_range)) return {};
            const auto tools = r.integer(1);
            if (tools > 127) return {};
            p.guest_noclip = (tools & 1) != 0;
            p.guest_no_bail = (tools & 2) != 0;
            p.guest_boosts = (tools & 4) != 0;
            p.server_votes = static_cast<std::uint8_t>((tools >> 3) & 7);
            p.enforce_tuning = (tools & 64) != 0;
            const auto polls = r.integer(1), custom = r.integer(1);
            if (polls > static_cast<std::uint8_t>(ServerPolls::everyone) || custom > server_custom_vote_limit) return {};
            p.server_polls = static_cast<std::uint8_t>(polls);
            for (std::uint64_t i = 0; i < custom; ++i) {
                ServerCustomVote vote;
                auto name = read_text(r, bytes, 1, server_vote_name_bytes);
                auto description = name ? read_text(r, bytes, 1, server_vote_description_bytes) : std::nullopt;
                if (!description) return {};
                vote.name = std::move(*name);
                vote.description = std::move(*description);
                const auto choices = r.integer(1);
                if (choices > server_vote_max_choices) return {};
                for (std::uint64_t c = 0; c < choices; ++c) {
                    auto choice = read_text(r, bytes, 1, server_vote_name_bytes);
                    if (!choice) return {};
                    vote.choices.push_back(std::move(*choice));
                }
                if (!valid_server_custom_vote(vote)) return {};
                p.server_custom_votes.push_back(std::move(vote));
            }
            p.announcement.id = static_cast<std::uint32_t>(r.integer(4));
            if (p.announcement.id) {
                p.announcement.seconds = static_cast<std::uint16_t>(r.integer(2));
                auto line = read_text(r, bytes, 1, multiplayer_chat_max_bytes);
                if (!line || !valid_chat_text(*line)) return {};
                p.announcement.text = std::move(*line);
            }
            if (!valid_roster(p.members, p.capacity) || !p.distances.valid())
                return {};
        } else if (p.kind == PacketKind::routes) {
            const auto count = r.integer(1);
            if (!p.map || count > max_remote_players)
                return {};
            for (std::uint64_t i = 0; i < count; ++i)
                p.members.push_back({r.integer(8), r.integer(8), {}});
            if (!valid_routes(p.members))
                return {};
        } else if (p.kind == PacketKind::objects) {
            auto &chunk = p.objects;
            chunk.base = r.integer(8); chunk.revision = r.integer(8);
            chunk.part = static_cast<std::uint16_t>(r.integer(2));
            chunk.parts = static_cast<std::uint16_t>(r.integer(2));
            const auto count = r.integer(2), removed = r.integer(2);
            if (!p.map || !p.source || count + removed > object_chunk_entries) return {};
            for (std::uint64_t i = 0; i < count; ++i) {
                NetworkObject object;
                object.id = r.integer(8);
                const auto length = r.integer(2);
                if (length > 256 || length > bytes.size() - r.at) return {};
                object.item.assign(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                for (float &value : object.position) value = r.number();
                for (float &value : object.rotation) value = r.number();
                object.scale = r.number();
                chunk.objects.push_back(std::move(object));
            }
            for (std::uint64_t i = 0; i < removed; ++i) chunk.removed.push_back(r.integer(8));
            if (!valid_object_chunk(chunk)) return {};
        } else if (p.kind != PacketKind::away)
            return {};
        if (r.at != bytes.size())
            return {};
        return p;
    } catch (...) {
        return {};
    }
}
namespace {
// Length of the UTF-8 sequence starting at `at`, 0 when it is malformed,
// overlong, a surrogate or past U+10FFFF.
std::size_t utf8_length(std::string_view text, std::size_t at) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned lead = byte(at);
    if (lead < 0x80) return 1;
    std::size_t length{};
    std::uint32_t code{}, minimum{};
    if ((lead & 0xE0) == 0xC0) { length = 2; code = lead & 0x1F; minimum = 0x80; }
    else if ((lead & 0xF0) == 0xE0) { length = 3; code = lead & 0x0F; minimum = 0x800; }
    else if ((lead & 0xF8) == 0xF0) { length = 4; code = lead & 0x07; minimum = 0x10000; }
    else return 0;
    if (at + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80) return 0;
        code = (code << 6) | (byte(at + i) & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
    return length;
}
bool chat_control(std::string_view text, std::size_t at, std::size_t length) noexcept {
    const auto lead = static_cast<unsigned char>(text[at]);
    // C0 controls and DEL, and the C1 range (U+0080..U+009F, encoded C2 80..C2 9F).
    return (length == 1 && (lead < 0x20 || lead == 0x7F)) ||
           (length == 2 && lead == 0xC2 && static_cast<unsigned char>(text[at + 1]) < 0xA0);
}
} // namespace
bool valid_chat_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > multiplayer_chat_max_bytes) return false;
    bool visible{};
    for (std::size_t at = 0; at < text.size();) {
        const auto length = utf8_length(text, at);
        if (!length || chat_control(text, at, length)) return false;
        visible |= text[at] != ' ';
        at += length;
    }
    return visible;
}
bool valid_server_vote_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= server_vote_name_bytes && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool valid_server_custom_vote(const ServerCustomVote &vote) noexcept {
    if (!valid_server_vote_name(vote.name) || vote.choices.size() > server_vote_max_choices) return false;
    if (!vote.description.empty() &&
        (vote.description.size() > server_vote_description_bytes || !valid_chat_text(vote.description)))
        return false;
    return std::all_of(vote.choices.begin(), vote.choices.end(), [](const std::string &c) { return valid_server_vote_name(c); });
}
bool valid_server_vote(const ServerVote &vote) noexcept {
    if (vote.label.size() > max_vote_label || vote.answers.size() > max_vote_answers || vote.answers.size() != vote.counts.size())
        return false;
    // A poll has two answers at least; nothing else has any.
    if ((vote.kind == server_vote_poll) != !vote.answers.empty() || (vote.kind == server_vote_poll && vote.answers.size() < 2))
        return false;
    return std::all_of(vote.answers.begin(), vote.answers.end(), [](const std::string &answer) {
        return answer.size() <= max_vote_answer && valid_chat_text(answer);
    });
}
bool valid_member_name(std::string_view text) noexcept {
    if (text.size() > max_member_name) return false;
    for (std::size_t at = 0; at < text.size();) {
        const auto length = utf8_length(text, at);
        if (!length || chat_control(text, at, length)) return false;
        at += length;
    }
    return true;
}
bool valid_map_asset(std::string_view asset) noexcept {
    return !asset.empty() && asset.size() <= max_map_asset &&
           std::all_of(asset.begin(), asset.end(), [](char c) { return c > 32 && c < 127 && c != '|'; });
}
bool valid_map_label(std::string_view label) noexcept {
    return label.empty() || (label.size() <= max_member_name && valid_member_name(label));
}
bool valid_map_pool(std::span<const std::uint16_t> pool, std::size_t maps) noexcept {
    if (pool.size() > maps) return false;
    for (std::size_t i = 0; i < pool.size(); ++i)
        if (pool[i] >= maps || std::find(pool.begin(), pool.begin() + i, pool[i]) != pool.begin() + i) return false;
    return true;
}
bool valid_admin_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > max_admin_text) return false;
    for (std::size_t at = 0; at < text.size();) {
        const auto length = utf8_length(text, at);
        if (!length || chat_control(text, at, length)) return false;
        at += length;
    }
    return true;
}
std::string clean_chat_text(std::string_view text) {
    std::string result;
    for (std::size_t at = 0; at < text.size();) {
        const auto length = utf8_length(text, at);
        if (!length) { ++at; continue; }
        if (chat_control(text, at, length)) {
            // Tabs and line breaks read as a space; other controls vanish.
            if (text[at] == '\t' || text[at] == '\n' || text[at] == '\r') result += ' ';
            at += length;
            continue;
        }
        if (result.size() + length > multiplayer_chat_max_bytes) break;
        result.append(text.substr(at, length));
        at += length;
    }
    const auto first = result.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return result.substr(first, result.find_last_not_of(' ') - first + 1);
}
std::string clean_roster_name(std::string_view text) {
    auto name = clean_chat_text(text);
    if (name.size() > 128) {
        std::size_t cut = 128;
        while (cut && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    return name;
}
bool newer_sequence(std::uint32_t a, std::uint32_t b) noexcept {
    const auto d = a - b;
    return d && d < 0x80000000u;
}
std::uint64_t map_hash(std::string_view value) noexcept {
    std::uint64_t result = 14695981039346656037ull;
    for (unsigned char c : value) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c == '\\')
            c = '/';
        result = (result ^ c) * 1099511628211ull;
    }
    return result;
}
std::array<float, 16> to_matrix(const Transform &t) {
    const auto q = interpolate(t, t, 0).rotation;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {(1 - 2 * y * y - 2 * z * z) * t.scale[0],
            (2 * x * y + 2 * z * w) * t.scale[0],
            (2 * x * z - 2 * y * w) * t.scale[0],
            0,
            (2 * x * y - 2 * z * w) * t.scale[1],
            (1 - 2 * x * x - 2 * z * z) * t.scale[1],
            (2 * y * z + 2 * x * w) * t.scale[1],
            0,
            (2 * x * z + 2 * y * w) * t.scale[2],
            (2 * y * z - 2 * x * w) * t.scale[2],
            (1 - 2 * x * x - 2 * y * y) * t.scale[2],
            0,
            t.position[0],
            t.position[1],
            t.position[2],
            0};
}
Transform from_matrix(const std::array<float, 16> &input) {
    auto m = input;
    Transform t;
    t.position = {m[12], m[13], m[14]};
    for (unsigned i = 0; i < 3; ++i) {
        const auto k = i * 4;
        t.scale[i] = std::sqrt(m[k] * m[k] + m[k + 1] * m[k + 1] + m[k + 2] * m[k + 2]);
        if (t.scale[i] < .0001f)
            throw std::invalid_argument("Degenerate pose matrix");
        for (unsigned j = 0; j < 3; ++j)
            m[k + j] /= t.scale[i];
    }
    const float trace = m[0] + m[5] + m[10];
    auto &q = t.rotation;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s, .25f * s};
    } else if (m[0] > m[5] && m[0] > m[10]) {
        const float s = std::sqrt(1 + m[0] - m[5] - m[10]) * 2;
        q = {.25f * s, (m[4] + m[1]) / s, (m[8] + m[2]) / s, (m[6] - m[9]) / s};
    } else if (m[5] > m[10]) {
        const float s = std::sqrt(1 + m[5] - m[0] - m[10]) * 2;
        q = {(m[4] + m[1]) / s, .25f * s, (m[9] + m[6]) / s, (m[8] - m[2]) / s};
    } else {
        const float s = std::sqrt(1 + m[10] - m[0] - m[5]) * 2;
        q = {(m[8] + m[2]) / s, (m[9] + m[6]) / s, .25f * s, (m[1] - m[4]) / s};
    }
    if (!valid_transform(t))
        throw std::invalid_argument("Invalid pose matrix");
    return t;
}
std::optional<Invite> parse_invite(std::string_view text) noexcept {
    const auto split = text.find('-');
    if (split == text.npos || split == 0 || text.size() - split - 1 != 16)
        return {};
    Invite i;
    const auto a = std::from_chars(text.data(), text.data() + split, i.steam_id);
    const auto b = std::from_chars(text.data() + split + 1, text.data() + text.size(), i.secret, 16);
    if (a.ec != std::errc{} || a.ptr != text.data() + split || b.ec != std::errc{} ||
        b.ptr != text.data() + text.size() || !i.secret ||
        (!individual_steam_id(i.steam_id) && !game_server_steam_id(i.steam_id)))
        return {};
    return i;
}
std::string format_invite(Invite i) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = std::to_string(i.steam_id) + "-";
    for (int n = 15; n >= 0; --n)
        out += hex[(i.secret >> (n * 4)) & 15];
    return out;
}
} // namespace dingosdk::multiplayer
