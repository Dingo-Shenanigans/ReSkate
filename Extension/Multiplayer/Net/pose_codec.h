#pragma once
#include "protocol.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// A pose as the differences from an earlier one, each in as few bits as it needs.
//
// Measured on a minute of recorded play (tools/research, pose-dump): of a skater's 413 bones 87
// ever turn and about 70 turn in any one pose, by a degree or so; 40 of the 87 are fingers. The
// older encoding sends each changed field whole (6 bytes a rotation) and leaves the rest to a
// general compressor: 661 bytes a pose. Here a pose is first rounded to whole steps (QuantPose),
// and what is sent is, for each bone that changed, the small integer it changed by: about 310
// bytes against a pose 67 ms older, 215 without the fingers.
//
// Both ends work on the rounded integers, never on the floats, so a chain of differences cannot
// drift: applying a difference to the reference gives exactly the sender's rounded pose.
namespace dingosdk::multiplayer::pose_codec {
// Rotations are rounded to 2^rotation_shift of the 16-bit form a compact pose packs them in:
// 0.079 degrees. Positions to a millimetre.
inline constexpr unsigned rotation_shift = 5;
inline constexpr int rotation_bits = 16 - rotation_shift + 1; // a whole component, signed
inline constexpr float rotation_scale = 46339.5358f;         // as Writer::compact_transform
inline constexpr std::int32_t position_limit = 1000000000;  // mm: valid_transform's million metres

struct QuantBone {
    std::int16_t rotation[3]{};  // the three smaller components, in steps
    std::uint8_t largest{3};     // which component is left out (and is positive)
    bool scaled{};
    std::int32_t position[3]{};  // mm
    float scale[3]{1, 1, 1};     // sent whole, and only when it is not 1: mods aside it always is
    bool operator==(const QuantBone &) const = default;
};
// The root, the skater's bones, then the board's.
struct QuantPose {
    std::uint16_t skater{}, board{};
    std::vector<QuantBone> bones;
    bool operator==(const QuantPose &) const = default;
};
// The skater's finger bones, as indices into QuantPose::bones (the root is 0): two runs of
// twenty. Only in the skeleton this was measured on; any other bone count has none.
inline constexpr std::size_t finger_skeleton = 395;
inline constexpr bool finger_bone(const QuantPose &pose, std::size_t bone) noexcept {
    return pose.skater == finger_skeleton && ((bone >= 52 && bone < 72) || (bone >= 281 && bone < 301));
}

inline QuantBone quantize(const Transform &t) noexcept {
    QuantBone bone;
    unsigned largest{};
    float norm{};
    for (unsigned i = 0; i < 4; ++i) {
        norm += t.rotation[i] * t.rotation[i];
        if (std::abs(t.rotation[i]) > std::abs(t.rotation[largest])) largest = i;
    }
    const float factor = norm > 0.f && std::isfinite(norm) ? (t.rotation[largest] < 0 ? -1.f : 1.f) * rotation_scale / std::sqrt(norm) : 0.f;
    const float step = static_cast<float>(1U << rotation_shift);
    const auto most = static_cast<long>(32767 >> rotation_shift);
    bone.largest = static_cast<std::uint8_t>(largest);
    unsigned out{};
    for (unsigned i = 0; i < 4; ++i)
        if (i != largest) bone.rotation[out++] = static_cast<std::int16_t>(std::clamp(std::lround(t.rotation[i] * factor / step), -most, most));
    for (unsigned i = 0; i < 3; ++i) {
        const auto mm = std::isfinite(t.position[i]) ? std::llround(static_cast<double>(t.position[i]) * 1000.0) : 0LL;
        bone.position[i] = static_cast<std::int32_t>(std::clamp<long long>(mm, -position_limit, position_limit));
    }
    bone.scaled = t.scale != std::array<float, 3>{1, 1, 1};
    if (bone.scaled) std::copy(t.scale.begin(), t.scale.end(), bone.scale);
    return bone;
}
inline Transform restore(const QuantBone &bone) noexcept {
    Transform t;
    const float step = static_cast<float>(1U << rotation_shift) / rotation_scale;
    float sum{};
    unsigned in{};
    for (unsigned i = 0; i < 4; ++i) {
        if (i == bone.largest) continue;
        t.rotation[i] = static_cast<float>(bone.rotation[in++]) * step;
        sum += t.rotation[i] * t.rotation[i];
    }
    t.rotation[bone.largest & 3] = std::sqrt(std::max(0.f, 1.f - sum));
    for (unsigned i = 0; i < 3; ++i) t.position[i] = static_cast<float>(static_cast<double>(bone.position[i]) / 1000.0);
    if (bone.scaled) std::copy(bone.scale, bone.scale + 3, t.scale.begin());
    return t;
}
inline QuantPose quantize(const Pose &pose) {
    QuantPose out;
    out.skater = static_cast<std::uint16_t>(pose.skater.size());
    out.board = static_cast<std::uint16_t>(pose.board.size());
    out.bones.reserve(1 + pose.skater.size() + pose.board.size());
    out.bones.push_back(quantize(pose.root));
    for (const auto &t : pose.skater) out.bones.push_back(quantize(t));
    for (const auto &t : pose.board) out.bones.push_back(quantize(t));
    return out;
}
inline Pose restore(const QuantPose &pose) {
    Pose out;
    if (pose.bones.size() != std::size_t{1} + pose.skater + pose.board) return out;
    out.root = restore(pose.bones[0]);
    out.skater.reserve(pose.skater);
    out.board.reserve(pose.board);
    for (std::size_t i = 0; i < pose.skater; ++i) out.skater.push_back(restore(pose.bones[1 + i]));
    for (std::size_t i = 0; i < pose.board; ++i) out.board.push_back(restore(pose.bones[1 + pose.skater + i]));
    return out;
}

namespace detail {
struct BitWriter {
    std::vector<std::uint8_t> bytes;
    std::uint64_t hold{};
    unsigned held{};
    void put(std::uint64_t value, unsigned bits) { // the low `bits` of value, at most 32
        hold |= (value & ((std::uint64_t{1} << bits) - 1)) << held;
        held += bits;
        while (held >= 8) {
            bytes.push_back(static_cast<std::uint8_t>(hold));
            hold >>= 8;
            held -= 8;
        }
    }
    // A count of 1 or more, shorter the smaller it is (Elias gamma): bones that changed come in
    // runs, and the step from one to the next is nearly always 1, which is a single bit.
    void gap(std::uint32_t value) {
        const auto width = static_cast<unsigned>(std::bit_width(value));
        put(0, width - 1);
        put(1, 1);
        put(value, width - 1);
    }
    std::vector<std::uint8_t> finish() {
        if (held) bytes.push_back(static_cast<std::uint8_t>(hold));
        hold = held = 0;
        return std::move(bytes);
    }
};
struct BitReader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    std::uint64_t hold{};
    unsigned held{};
    std::uint64_t get(unsigned bits) {
        while (held < bits) {
            if (at >= bytes.size()) throw std::runtime_error("Truncated pose");
            hold |= std::uint64_t{bytes[at++]} << held;
            held += 8;
        }
        const auto value = hold & ((std::uint64_t{1} << bits) - 1);
        hold >>= bits;
        held -= bits;
        return value;
    }
    std::uint32_t gap() {
        unsigned width{1};
        while (!get(1))
            if (++width > 32) throw std::runtime_error("Invalid pose gap");
        return static_cast<std::uint32_t>((std::uint64_t{1} << (width - 1)) | get(width - 1));
    }
};
constexpr std::uint32_t zigzag(std::int64_t value) noexcept {
    return static_cast<std::uint32_t>(value >= 0 ? static_cast<std::uint64_t>(value) * 2 : static_cast<std::uint64_t>(-value) * 2 - 1);
}
constexpr std::int64_t unzigzag(std::uint64_t value) noexcept {
    return (value & 1) ? -static_cast<std::int64_t>((value + 1) / 2) : static_cast<std::int64_t>(value / 2);
}
// A signed number as its length (a fixed few bits) and then that many bits.
inline void small(BitWriter &w, std::int64_t value, unsigned length_bits) {
    const auto coded = zigzag(value);
    const auto width = static_cast<unsigned>(std::bit_width(coded));
    w.put(width, length_bits);
    if (width > 32 - 1) {
        w.put(coded & 0xffff, 16);
        w.put(coded >> 16, width - 16);
    } else {
        w.put(coded, width);
    }
}
inline std::int64_t small(BitReader &r, unsigned length_bits) {
    const auto width = static_cast<unsigned>(r.get(length_bits));
    if (width > 32) throw std::runtime_error("Invalid pose number");
    if (width > 31) {
        const auto low = r.get(16);
        return unzigzag(low | (r.get(width - 16) << 16));
    }
    return unzigzag(r.get(width));
}
inline void whole_rotation(BitWriter &w, const QuantBone &bone) {
    w.put(bone.largest, 2);
    for (const auto component : bone.rotation) w.put(zigzag(component), rotation_bits);
}
inline void whole_rotation(BitReader &r, QuantBone &bone) {
    bone.largest = static_cast<std::uint8_t>(r.get(2));
    for (auto &component : bone.rotation) component = static_cast<std::int16_t>(unzigzag(r.get(rotation_bits)));
}
inline void scale(BitWriter &w, const QuantBone &bone) {
    w.put(bone.scaled, 1);
    if (bone.scaled)
        for (const float axis : bone.scale) {
            const auto bits = std::bit_cast<std::uint32_t>(axis);
            w.put(bits & 0xffff, 16);
            w.put(bits >> 16, 16);
        }
}
inline void scale(BitReader &r, QuantBone &bone) {
    bone.scaled = r.get(1) != 0;
    for (float &axis : bone.scale) {
        if (!bone.scaled) {
            axis = 1;
            continue;
        }
        const auto low = static_cast<std::uint32_t>(r.get(16));
        axis = std::bit_cast<float>(low | (static_cast<std::uint32_t>(r.get(16)) << 16));
        if (!std::isfinite(axis) || std::abs(axis) > 1000.f) throw std::runtime_error("Invalid pose scale");
    }
}
inline bool same_rotation(const QuantBone &a, const QuantBone &b) noexcept {
    return a.largest == b.largest && std::equal(a.rotation, a.rotation + 3, b.rotation);
}
inline bool same_position(const QuantBone &a, const QuantBone &b) noexcept { return std::equal(a.position, a.position + 3, b.position); }
inline bool same_scale(const QuantBone &a, const QuantBone &b) noexcept {
    return a.scaled == b.scaled && (!a.scaled || std::equal(a.scale, a.scale + 3, b.scale));
}
} // namespace detail

// How a pose's fingers are sent (finger_bone). `held`: not at all, the receiver keeps the ones it
// has (a player too far away to make out a hand). `whole`: every finger bone in full, whatever
// the reference holds: the first poses after `held`, until the receiver is known to have them,
// since its copy of the reference has old fingers in it.
enum class Fingers : std::uint8_t { sent, held, whole };

// The whole pose, needing no reference: the first of a stream, and whenever the receiver holds
// nothing recent enough to build on.
inline std::vector<std::uint8_t> encode_whole(const QuantPose &pose) {
    detail::BitWriter w;
    w.put(1, 1);
    w.put(rotation_shift, 3);
    w.put(pose.skater, 10);
    w.put(pose.board, 7);
    for (const auto &bone : pose.bones) {
        detail::whole_rotation(w, bone);
        for (const auto mm : bone.position) detail::small(w, mm, 6);
        detail::scale(w, bone);
    }
    return w.finish();
}
// The pose as its differences from `reference`, which the receiver must hold exactly. Empty when
// the two are not the same skeleton: send it whole.
inline std::vector<std::uint8_t> encode_delta(const QuantPose &pose, const QuantPose &reference, Fingers fingers = Fingers::sent) {
    if (pose.skater != reference.skater || pose.board != reference.board || pose.bones.size() != reference.bones.size()) return {};
    detail::BitWriter w;
    w.put(0, 1);
    w.put(rotation_shift, 3);
    w.put(static_cast<unsigned>(fingers), 2);
    const auto count = pose.bones.size();
    const auto listed = [&](std::size_t bone) { return fingers == Fingers::sent || !finger_bone(pose, bone); };
    // Three lists of the bones that changed, each as how many and then the steps between them.
    const auto list = [&](auto &&differs, auto &&write) {
        std::uint32_t changed{};
        for (std::size_t bone = 0; bone < count; ++bone) changed += listed(bone) && differs(pose.bones[bone], reference.bones[bone]);
        w.put(changed, 10);
        std::size_t last{};
        for (std::size_t bone = 0; bone < count; ++bone) {
            if (!listed(bone) || !differs(pose.bones[bone], reference.bones[bone])) continue;
            w.gap(static_cast<std::uint32_t>(bone + 1 - last));
            last = bone + 1;
            write(pose.bones[bone], reference.bones[bone]);
        }
    };
    list([](const QuantBone &a, const QuantBone &b) { return !detail::same_rotation(a, b); }, [&](const QuantBone &now, const QuantBone &was) {
        // A different component left out: the three sent are other ones, so they go whole.
        const bool flipped = now.largest != was.largest;
        w.put(flipped, 1);
        if (flipped) return detail::whole_rotation(w, now);
        for (unsigned i = 0; i < 3; ++i) detail::small(w, std::int64_t{now.rotation[i]} - was.rotation[i], 4);
    });
    list([](const QuantBone &a, const QuantBone &b) { return !detail::same_position(a, b); }, [&](const QuantBone &now, const QuantBone &was) {
        for (unsigned i = 0; i < 3; ++i) detail::small(w, std::int64_t{now.position[i]} - was.position[i], 6);
    });
    list([](const QuantBone &a, const QuantBone &b) { return !detail::same_scale(a, b); },
         [&](const QuantBone &now, const QuantBone &) { detail::scale(w, now); });
    if (fingers == Fingers::whole)
        for (std::size_t bone = 0; bone < count; ++bone) {
            if (!finger_bone(pose, bone)) continue;
            detail::whole_rotation(w, pose.bones[bone]);
            for (const auto mm : pose.bones[bone].position) detail::small(w, mm, 6);
            detail::scale(w, pose.bones[bone]);
        }
    return w.finish();
}
inline bool whole(std::span<const std::uint8_t> bytes) noexcept { return !bytes.empty() && (bytes[0] & 1); }
// The pose `bytes` holds. `reference`: the pose its differences are from, as this end holds it
// (ignored by a whole pose). Nothing when the bytes are not a pose, or need a reference of
// another skeleton.
inline std::optional<QuantPose> decode(std::span<const std::uint8_t> bytes, const QuantPose *reference) noexcept {
    try {
        detail::BitReader r{bytes};
        const bool is_whole = r.get(1) != 0;
        if (r.get(3) != rotation_shift) return {};
        const auto valid = [](QuantBone &bone) {
            if (bone.largest > 3) throw std::runtime_error("Invalid pose rotation");
            const long most = 32767 >> rotation_shift;
            for (const auto component : bone.rotation)
                if (component > most || component < -most) throw std::runtime_error("Invalid pose rotation");
            for (const auto mm : bone.position)
                if (mm > position_limit || mm < -position_limit) throw std::runtime_error("Invalid pose position");
        };
        QuantPose pose;
        if (is_whole) {
            pose.skater = static_cast<std::uint16_t>(r.get(10));
            pose.board = static_cast<std::uint16_t>(r.get(7));
            if (pose.skater > max_skater_bones || pose.board > max_board_bones) return {};
            pose.bones.resize(std::size_t{1} + pose.skater + pose.board);
            for (auto &bone : pose.bones) {
                detail::whole_rotation(r, bone);
                for (auto &mm : bone.position) mm = static_cast<std::int32_t>(std::clamp<std::int64_t>(detail::small(r, 6), INT32_MIN, INT32_MAX));
                detail::scale(r, bone);
                valid(bone);
            }
            return pose;
        }
        if (!reference || reference->bones.size() != std::size_t{1} + reference->skater + reference->board) return {};
        pose = *reference;
        const auto fingers = static_cast<Fingers>(r.get(2));
        if (fingers > Fingers::whole) return {};
        const auto count = pose.bones.size();
        const auto list = [&](auto &&read) {
            const auto changed = r.get(10);
            std::size_t last{};
            for (std::uint64_t i = 0; i < changed; ++i) {
                const auto bone = last + r.gap() - 1;
                if (bone >= count) throw std::runtime_error("Invalid pose bone");
                last = bone + 1;
                read(pose.bones[bone]);
                valid(pose.bones[bone]);
            }
        };
        list([&](QuantBone &bone) {
            if (r.get(1)) return detail::whole_rotation(r, bone);
            for (auto &component : bone.rotation)
                component = static_cast<std::int16_t>(std::clamp<std::int64_t>(component + detail::small(r, 4), INT16_MIN, INT16_MAX));
        });
        list([&](QuantBone &bone) {
            for (auto &mm : bone.position) mm = static_cast<std::int32_t>(std::clamp<std::int64_t>(mm + detail::small(r, 6), INT32_MIN, INT32_MAX));
        });
        list([&](QuantBone &bone) { detail::scale(r, bone); });
        if (fingers == Fingers::whole)
            for (std::size_t bone = 0; bone < count; ++bone) {
                if (!finger_bone(pose, bone)) continue;
                detail::whole_rotation(r, pose.bones[bone]);
                for (auto &mm : pose.bones[bone].position) mm = static_cast<std::int32_t>(std::clamp<std::int64_t>(detail::small(r, 6), INT32_MIN, INT32_MAX));
                detail::scale(r, pose.bones[bone]);
                valid(pose.bones[bone]);
            }
        return pose;
    } catch (...) {
        return {};
    }
}
} // namespace dingosdk::multiplayer::pose_codec
