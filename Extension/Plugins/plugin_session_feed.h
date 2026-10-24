#pragma once
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// What the session and the plugins hand each other (Extension/Multiplayer/Session/session.cpp
// and plugin_game_runtime.cpp): other players' skaters as the session last heard them, published
// each rendered frame; and the short tags plugins put beside players' nametags. Header only, so
// neither depends on the other. Any thread.
namespace dingosdk::plugin_feed {
struct Peer {
    std::uint64_t id{};
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1}; // x, y, z, w
    double age_ms{};                          // since its pose arrived, when published
};
// Where a joint of another player's skater is in the world, as the session last drew it.
struct JointPlace {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    float scale{1};
};
struct Tag {
    std::string text;
    std::uint32_t colour{0xffffffffu}; // IM_COL32 layout
};
// A skater's body as drawn, as capsules (a sphere when both ends meet), for plugins' touching and
// their raycast at players. `part` indexes body_parts.
struct Capsule {
    std::uint8_t part{};
    std::array<float, 3> a{}, b{};
    float radius{};
};
struct Body {
    std::uint64_t player{}; // 0: the local skater
    std::vector<Capsule> capsules;
};
inline constexpr std::array<const char*, 7> body_parts{"head", "body", "left_arm", "right_arm", "left_leg", "right_leg", "board"};
// The joints each limb's capsules run between (Extension/Skater/skater_skeleton.h): hips to neck,
// shoulder to elbow to hand, hip to knee to foot.
struct Bone {
    std::uint8_t part;
    std::uint16_t from, to;
    float radius;
};
inline constexpr std::array<Bone, 9> body_bones{{{1, 7, 101, 0.16f},
                                                 {2, 276, 277, 0.06f}, {2, 277, 278, 0.06f},
                                                 {3, 47, 48, 0.06f}, {3, 48, 49, 0.06f},
                                                 {4, 341, 342, 0.08f}, {4, 342, 343, 0.08f},
                                                 {5, 8, 9, 0.08f}, {5, 9, 10, 0.08f}}};
inline constexpr std::uint16_t head_joint = 103, neck_joint = 101;
// The body from where its joints are in the world (`place(joint)`, empty when unknown) and where its
// board is (its entity's position and rotation): the head from the head joint 0.16 m on away from the
// neck; the board along its length (its local Z), as the players' collision has it (remote_collision.cpp).
template <class Place>
Body body_of(std::uint64_t player, Place&& place, const std::optional<std::pair<std::array<float, 3>, std::array<float, 4>>>& board) {
    Body body{player, {}};
    for (const auto& bone : body_bones) {
        const std::optional<std::array<float, 3>> a = place(bone.from), b = place(bone.to);
        if (a && b) body.capsules.push_back({bone.part, *a, *b, bone.radius});
    }
    const std::optional<std::array<float, 3>> head = place(head_joint), neck = place(neck_joint);
    if (head && neck) {
        std::array<float, 3> up{(*head)[0] - (*neck)[0], (*head)[1] - (*neck)[1], (*head)[2] - (*neck)[2]};
        const float length = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (length > 1e-4f)
            for (auto& v : up) v *= 0.16f / length;
        body.capsules.push_back({0, *head, {(*head)[0] + up[0], (*head)[1] + up[1], (*head)[2] + up[2]}, 0.11f});
    }
    if (board) {
        const auto& [at, q] = *board;
        // The deck's local Z turned into the world: q * (0, 0, 1) and the 0.08 m above it, q * (0, 1, 0).
        const std::array<float, 3> z{2 * (q[0] * q[2] + q[3] * q[1]), 2 * (q[1] * q[2] - q[3] * q[0]), 1 - 2 * (q[0] * q[0] + q[1] * q[1])};
        const std::array<float, 3> y{2 * (q[0] * q[1] - q[3] * q[2]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] + q[3] * q[0])};
        Capsule deck{6, {}, {}, 0.12f};
        for (std::size_t i = 0; i < 3; ++i) {
            deck.a[i] = at[i] + 0.08f * y[i] - 0.3f * z[i];
            deck.b[i] = at[i] + 0.08f * y[i] + 0.3f * z[i];
        }
        body.capsules.push_back(deck);
    }
    for (const auto& c : body.capsules)
        for (std::size_t i = 0; i < 3; ++i)
            if (!std::isfinite(c.a[i]) || !std::isfinite(c.b[i])) return {player, {}};
    return body;
}
// The joints body_of reads.
inline std::vector<std::uint16_t> body_joints() {
    std::vector<std::uint16_t> joints{head_joint, neck_joint};
    for (const auto& bone : body_bones) {
        joints.push_back(bone.from);
        joints.push_back(bone.to);
    }
    return joints;
}

namespace detail {
inline std::mutex mutex;
inline std::vector<Peer> peers;
inline std::vector<Body> bodies; // every other player's, then the local skater's
inline std::chrono::steady_clock::time_point bodies_asked{};
inline std::set<std::pair<std::string, std::uint64_t>> hidden; // (plugin, player)
inline std::map<std::pair<std::string, std::uint64_t>, Tag> tags; // (plugin, player)
// (player, joint): when it was last asked for, and where it is.
inline std::map<std::pair<std::uint64_t, std::uint16_t>, std::chrono::steady_clock::time_point> joints_asked;
inline std::map<std::pair<std::uint64_t, std::uint16_t>, JointPlace> joints;
}

// Where a joint of another player is (joint_place), asked for by the plugins' side. Asking keeps it
// published for two seconds, so the first ask comes back empty; at most 64 are kept.
inline std::optional<JointPlace> joint_place(std::uint64_t player, std::uint16_t joint) {
    std::lock_guard lock(detail::mutex);
    const std::pair key{player, joint};
    if (detail::joints_asked.size() < 64 || detail::joints_asked.contains(key)) detail::joints_asked[key] = std::chrono::steady_clock::now();
    const auto at = detail::joints.find(key);
    if (at == detail::joints.end()) return std::nullopt;
    return at->second;
}
// The joints asked for in the last two seconds, for the session to publish.
inline std::vector<std::pair<std::uint64_t, std::uint16_t>> joints_wanted() {
    std::lock_guard lock(detail::mutex);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(detail::joints_asked, [&](const auto& asked) { return now - asked.second > std::chrono::seconds(2); });
    std::vector<std::pair<std::uint64_t, std::uint16_t>> result;
    for (const auto& [key, when] : detail::joints_asked) result.push_back(key);
    return result;
}
inline void publish_joints(std::map<std::pair<std::uint64_t, std::uint16_t>, JointPlace> joints) {
    std::lock_guard lock(detail::mutex);
    detail::joints = std::move(joints);
}

// The bodies of the skaters in the session (body_of), asked for by the plugins' side: asking keeps
// them published for two seconds, so the first ask comes back empty.
inline std::vector<Body> latest_bodies() {
    std::lock_guard lock(detail::mutex);
    detail::bodies_asked = std::chrono::steady_clock::now();
    return detail::bodies;
}
inline bool bodies_wanted() {
    std::lock_guard lock(detail::mutex);
    return std::chrono::steady_clock::now() - detail::bodies_asked < std::chrono::seconds(2);
}
inline void publish_bodies(std::vector<Body> bodies) {
    std::lock_guard lock(detail::mutex);
    detail::bodies = std::move(bodies);
}
// Another player's skater, board, sound, nametag and map marker not shown on this screen while any
// plugin hides them; at most 256 are kept.
inline bool hide(const std::string& plugin, std::uint64_t player, bool on) {
    std::lock_guard lock(detail::mutex);
    if (!on) detail::hidden.erase({plugin, player});
    else if (detail::hidden.size() < 256 || detail::hidden.contains({plugin, player})) detail::hidden.insert({plugin, player});
    else return false;
    return true;
}
inline bool hidden(std::uint64_t player) {
    std::lock_guard lock(detail::mutex);
    for (const auto& [owner, who] : detail::hidden)
        if (who == player) return true;
    return false;
}
inline std::vector<std::uint64_t> hidden_by(const std::string& plugin) {
    std::lock_guard lock(detail::mutex);
    std::vector<std::uint64_t> out;
    for (const auto& [owner, who] : detail::hidden)
        if (owner == plugin) out.push_back(who);
    return out;
}
inline void release_hidden(const std::string& plugin) {
    std::lock_guard lock(detail::mutex);
    std::erase_if(detail::hidden, [&](const auto& item) { return item.first == plugin; });
}

inline void publish_peers(std::vector<Peer> peers) {
    std::lock_guard lock(detail::mutex);
    detail::peers = std::move(peers);
}
inline std::vector<Peer> latest_peers() {
    std::lock_guard lock(detail::mutex);
    return detail::peers;
}
inline void set_tag(const std::string& plugin, std::uint64_t player, std::string text, std::uint32_t colour) {
    std::lock_guard lock(detail::mutex);
    if (text.empty()) detail::tags.erase({plugin, player});
    else if (detail::tags.size() < 1024 || detail::tags.contains({plugin, player})) detail::tags[{plugin, player}] = {std::move(text), colour};
}
inline void release_tags(const std::string& plugin) {
    std::lock_guard lock(detail::mutex);
    std::erase_if(detail::tags, [&](const auto& item) { return item.first.first == plugin; });
}
// The tag a plugin gave this player (the first, when several did).
inline std::optional<Tag> tag_of(std::uint64_t player) {
    std::lock_guard lock(detail::mutex);
    for (const auto& [key, tag] : detail::tags)
        if (key.second == player) return tag;
    return std::nullopt;
}
}
