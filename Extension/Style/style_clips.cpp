#include "style_clips.h"
#include "style_recording.h"
#include <lz4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace dingosdk::style {
namespace {
// A clip is a few seconds of render frames, which can come 300 times a second.
constexpr std::size_t maximum_frames = 2000, maximum_board = 64;
constexpr std::size_t transform_floats = 10;
constexpr std::size_t frame_floats = 2;
// The deck counts as off the ground this far above it. The feet are apart past this margin, and back on below that one.
constexpr float ground_margin_m = 0.04f, feet_apart_margin_m = 0.08f, feet_back_margin_m = 0.05f;
// The flick comes this long before the board leaves the ground. A recording is this long at least, an air time this long.
constexpr std::uint32_t flick_lead_ms = 80, min_recording_ms = 1000, min_air_ms = 100;
std::size_t first_frame_at(const Clip &clip, float time) noexcept {
    return static_cast<std::size_t>(
        std::ranges::lower_bound(clip.frames, time, {}, &ClipFrame::time) - clip.frames.begin());
}
void put(std::vector<float> &out, const Transform &t) {
    out.insert(out.end(), t.position.begin(), t.position.end());
    out.insert(out.end(), t.rotation.begin(), t.rotation.end());
    out.insert(out.end(), t.scale.begin(), t.scale.end());
}
Transform take(const float *&in) {
    Transform t;
    std::copy_n(in, 3, t.position.begin());
    std::copy_n(in + 3, 4, t.rotation.begin());
    std::copy_n(in + 7, 3, t.scale.begin());
    in += transform_floats;
    for (const auto value : t.position)
        if (!std::isfinite(value) || std::abs(value) > 1e6f) throw std::runtime_error("The clip holds a position that is not a place.");
    for (const auto value : t.scale)
        if (!std::isfinite(value) || value < 1e-4f || value > 100) throw std::runtime_error("The clip holds a scale that is not one.");
    if (!finite(t.rotation)) throw std::runtime_error("The clip holds a rotation that is not one.");
    t.rotation = normalized(t.rotation);
    return t;
}
struct Header {
    char magic[4];
    std::uint32_t version, trick, frames, joints, board, raw_bytes, rig;
    std::uint64_t key, recorded_at;
    char recipe[96], reskate[32];
};
static_assert(sizeof(Header) == clip_header_bytes);
template <std::size_t size> void put_text(char (&out)[size], const std::string &text) {
    std::memset(out, 0, size);
    std::memcpy(out, text.data(), std::min(text.size(), size - 1));
}
template <std::size_t size> std::string take_text(const char (&in)[size]) { return {in, strnlen(in, size)}; }
std::optional<Header> header_of(std::span<const std::uint8_t> bytes) {
    Header header{};
    if (bytes.size() < sizeof(header)) return std::nullopt;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (std::memcmp(header.magic, "RSCL", 4) || header.version != clip_version) return std::nullopt;
    return header;
}
Capture capture_of(const Header &header) { return {header.key, take_text(header.recipe), take_text(header.reskate), header.recorded_at}; }
float median(std::vector<float> values) {
    if (values.empty()) return 0;
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2));
    return values[values.size() / 2];
}
} // namespace

std::string recipe_text(const Recipe &recipe) {
    return std::format("recipe {} game {} flick {} still {} lead {} follow {}", recipe.version, recipe.game, recipe.flick_ms,
                       recipe.still_ms, recipe.lead_ms, recipe.follow_ms);
}
std::uint64_t recipe_key(const Recipe &recipe) {
    std::uint64_t key = 0xcbf29ce484222325;
    for (const unsigned char c : recipe_text(recipe)) key = (key ^ c) * 0x100000001b3;
    return key;
}
Pose blend(const Pose &a, const Pose &b, float amount) {
    if (amount <= 0 || a.skater.size() != b.skater.size() || a.board.size() != b.board.size()) return a;
    if (amount >= 1) return b;
    Pose result;
    result.root = multiplayer::interpolate(a.root, b.root, amount);
    result.skater.reserve(a.skater.size());
    for (std::size_t i = 0; i < a.skater.size(); ++i) result.skater.push_back(multiplayer::interpolate(a.skater[i], b.skater[i], amount));
    result.board.reserve(a.board.size());
    for (std::size_t i = 0; i < a.board.size(); ++i) result.board.push_back(multiplayer::interpolate(a.board[i], b.board[i], amount));
    return result;
}
bool retime(Clip &clip) {
    auto &frames = clip.frames;
    const auto first = [&](float from) {
        return static_cast<std::size_t>(std::ranges::find_if(frames, [&](const ClipFrame &f) { return f.time >= from; }) - frames.begin());
    };
    const std::size_t flick = first(0), caught = first(1), landed = first(2);
    if (flick >= frames.size() || caught >= frames.size() || landed >= frames.size() || !(flick < caught && caught < landed)) return false;
    // A slam after the catch has no landing. Its touchdown would be the first frame after the trick.
    if (frames[caught].time >= 2 || frames[landed].time > trick_end) return false;
    std::size_t over = landed;
    while (over + 1 < frames.size() && frames[over].at < frames[landed].at + TrickTracker::landing_ms) ++over;
    if (over == landed) return false;
    const std::array<std::size_t, 4> marks{flick, caught, landed, over};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        auto &frame = frames[i];
        if (i < flick) frame.time = (static_cast<float>(frame.at) - static_cast<float>(frames[flick].at)) / 1000.0f - 0.0001f;
        else if (i >= over) frame.time = trick_end + (static_cast<float>(frame.at) - static_cast<float>(frames[over].at)) / 1000.0f;
        else {
            const Part part = i >= landed ? Part::landing : i >= caught ? Part::fall : Part::pop;
            const auto index = static_cast<std::size_t>(part);
            const float span = std::max(1.0f, static_cast<float>(frames[marks[index + 1]].at - frames[marks[index]].at));
            frame.time = part_start(part) + (static_cast<float>(frame.at) - static_cast<float>(frames[marks[index]].at)) / span;
        }
    }
    // Two frames with the same millisecond must stay in order.
    for (std::size_t i = 1; i < frames.size(); ++i)
        if (frames[i].time <= frames[i - 1].time) frames[i].time = std::nextafter(frames[i - 1].time, 1e9f);
    return true;
}
Pose sample(const Clip &clip, float time) {
    if (clip.frames.empty()) return {};
    const auto next = first_frame_at(clip, time);
    if (next == 0) return clip.frames.front().pose;
    if (next >= clip.frames.size()) return clip.frames.back().pose;
    const auto &a = clip.frames[next - 1], &b = clip.frames[next];
    return blend(a.pose, b.pose, (time - a.time) / std::max(b.time - a.time, 1e-6f));
}
std::uint32_t duration(const Clip &clip) noexcept { return clip.frames.empty() ? 0 : clip.frames.back().at - clip.frames.front().at; }
float time_at(const Clip &clip, float milliseconds) noexcept {
    if (clip.frames.empty()) return 0;
    const float at = static_cast<float>(clip.frames.front().at) + (std::isfinite(milliseconds) ? milliseconds : 0.0f);
    const auto next = std::ranges::lower_bound(clip.frames, at, {}, [](const ClipFrame &f) { return static_cast<float>(f.at); });
    if (next == clip.frames.begin()) return clip.frames.front().time;
    if (next == clip.frames.end()) return clip.frames.back().time;
    const auto &a = *(next - 1), &b = *next;
    return a.time + (b.time - a.time) * (at - static_cast<float>(a.at)) / static_cast<float>(std::max<std::uint32_t>(b.at - a.at, 1));
}
float ms_at(const Clip &clip, float time) noexcept {
    if (clip.frames.empty()) return 0;
    const auto next = first_frame_at(clip, time);
    const float first = static_cast<float>(clip.frames.front().at);
    if (next == 0) return 0;
    if (next >= clip.frames.size()) return static_cast<float>(clip.frames.back().at) - first;
    const auto &a = clip.frames[next - 1], &b = clip.frames[next];
    const float amount = (time - a.time) / std::max(b.time - a.time, 1e-6f);
    return static_cast<float>(a.at) - first + static_cast<float>(b.at - a.at) * amount;
}

std::vector<std::uint8_t> encode_clip(const Clip &clip) {
    if (clip.frames.empty() || clip.frames.size() > maximum_frames) throw std::runtime_error("The clip has no frames or too many.");
    const auto joints = clip.frames.front().pose.skater.size(), board = clip.frames.front().pose.board.size();
    auto rig = clip.frames.front().rig.size();
    for (const auto &frame : clip.frames)
        if (frame.rig.size() != rig || rig > maximum_board) rig = 0;
    std::vector<float> raw;
    raw.reserve(clip.frames.size() * (frame_floats + (1 + joints + board + rig) * transform_floats));
    for (const auto &frame : clip.frames) {
        if (frame.pose.skater.size() != joints || frame.pose.board.size() != board)
            throw std::runtime_error("The clip's frames differ in shape.");
        raw.push_back(frame.time);
        raw.push_back(static_cast<float>(frame.at));
        put(raw, frame.pose.root);
        for (const auto &joint : frame.pose.skater) put(raw, joint);
        for (const auto &bone : frame.pose.board) put(raw, bone);
        for (std::size_t i = 0; i < rig; ++i) put(raw, frame.rig[i]);
    }
    const auto bytes = raw.size() * sizeof(float);
    Header header{{'R', 'S', 'C', 'L'}, clip_version, clip.trick, static_cast<std::uint32_t>(clip.frames.size()),
                  static_cast<std::uint32_t>(joints), static_cast<std::uint32_t>(board), static_cast<std::uint32_t>(bytes),
                  static_cast<std::uint32_t>(rig),
                  clip.capture.key, clip.capture.recorded_at, {}, {}};
    put_text(header.recipe, clip.capture.recipe);
    put_text(header.reskate, clip.capture.reskate);
    std::vector<std::uint8_t> out(sizeof(header) + static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(bytes))));
    std::memcpy(out.data(), &header, sizeof(header));
    const int packed =
        LZ4_compress_default(reinterpret_cast<const char *>(raw.data()), reinterpret_cast<char *>(out.data() + sizeof(header)),
                             static_cast<int>(bytes), static_cast<int>(out.size() - sizeof(header)));
    if (packed <= 0) throw std::runtime_error("The clip could not be packed.");
    out.resize(sizeof(header) + static_cast<std::size_t>(packed));
    return out;
}
std::optional<Capture> clip_capture(std::span<const std::uint8_t> head) {
    const auto header = header_of(head);
    return header ? std::optional(capture_of(*header)) : std::nullopt;
}
Clip decode_clip(std::span<const std::uint8_t> bytes) {
    if (bytes.size() <= sizeof(Header)) throw std::runtime_error("This is not a clip.");
    const auto read = header_of(bytes);
    if (!read) throw std::runtime_error("This is not a clip this version reads.");
    const auto &header = *read;
    const std::size_t per_frame = frame_floats + (1 + std::size_t{header.joints} + header.board + header.rig) * transform_floats;
    if (header.rig > maximum_board || !header.frames || header.frames > maximum_frames ||
        header.joints > max_rig_joints || header.board > maximum_board || !is_flip_trick(header.trick) ||
        header.raw_bytes != header.frames * per_frame * sizeof(float))
        throw std::runtime_error("This is not a clip this version reads.");
    std::vector<float> raw(header.raw_bytes / sizeof(float));
    if (LZ4_decompress_safe(reinterpret_cast<const char *>(bytes.data() + sizeof(header)), reinterpret_cast<char *>(raw.data()),
                            static_cast<int>(bytes.size() - sizeof(header)), static_cast<int>(header.raw_bytes)) !=
        static_cast<int>(header.raw_bytes))
        throw std::runtime_error("The clip is damaged.");
    Clip clip{static_cast<std::uint8_t>(header.trick), {}, capture_of(header)};
    clip.frames.reserve(header.frames);
    const float *in = raw.data();
    for (std::uint32_t i = 0; i < header.frames; ++i) {
        ClipFrame frame;
        frame.time = *in++;
        const float at = *in++;
        if (!std::isfinite(frame.time) || !std::isfinite(at) || at < 0 || at > 600000)
            throw std::runtime_error("The clip's timing is damaged.");
        frame.at = static_cast<std::uint32_t>(at);
        if (!clip.frames.empty() && (frame.time <= clip.frames.back().time || frame.at < clip.frames.back().at))
            throw std::runtime_error("The clip's frames are out of order.");
        frame.pose.root = take(in);
        frame.pose.skater.reserve(header.joints);
        for (std::uint32_t j = 0; j < header.joints; ++j) frame.pose.skater.push_back(take(in));
        frame.pose.board.reserve(header.board);
        for (std::uint32_t j = 0; j < header.board; ++j) frame.pose.board.push_back(take(in));
        frame.rig.reserve(header.rig);
        for (std::uint32_t j = 0; j < header.rig; ++j) frame.rig.push_back(take(in));
        clip.frames.push_back(std::move(frame));
    }
    return clip;
}

Transform compose(const Transform &parent, const Transform &child) noexcept {
    Transform result;
    const auto offset = rotate(parent.rotation, {child.position[0] * parent.scale[0], child.position[1] * parent.scale[1],
                                                 child.position[2] * parent.scale[2]});
    for (std::size_t i = 0; i < 3; ++i) {
        result.position[i] = parent.position[i] + offset[i];
        result.scale[i] = parent.scale[i] * child.scale[i];
    }
    result.rotation = normalized(multiply(parent.rotation, child.rotation));
    return result;
}
Transform inverse(const Transform &transform) noexcept {
    Transform result;
    result.rotation = conjugate(transform.rotation);
    const auto back = rotate(result.rotation, transform.position);
    for (std::size_t i = 0; i < 3; ++i) result.position[i] = -back[i];
    return result;
}
Transform anchored(const Transform &entity, const Transform &old_entity, const Transform &anchor) noexcept {
    auto result = compose(entity, compose(inverse(old_entity), anchor));
    result.scale = anchor.scale;
    return result;
}
Transform world(const std::vector<Transform> &joints, std::span<const std::int32_t> parents, std::size_t joint) {
    // Parents come before their children, so the chain is short and ends at the root.
    std::array<std::size_t, 64> chain{};
    std::size_t length{};
    for (auto at = static_cast<std::int64_t>(joint); at >= 0 && length < chain.size() && static_cast<std::size_t>(at) < joints.size();
         at = static_cast<std::size_t>(at) < parents.size() ? parents[static_cast<std::size_t>(at)] : -1)
        chain[length++] = static_cast<std::size_t>(at);
    Transform result;
    while (length) result = compose(result, joints[chain[--length]]);
    return result;
}

namespace {
struct DeckTrack {
    std::vector<Transform> decks;
    std::vector<float> height, feet_from_deck;
};
// Frames [off, on) have the deck off the ground.
struct Jump {
    std::size_t off{}, on{};
};
struct Marks {
    std::size_t flick{}, caught{}, on{};
};

std::size_t first_at(const std::vector<RigFrame> &frames, std::uint32_t at) {
    return static_cast<std::size_t>(std::ranges::lower_bound(frames, at, {}, &RigFrame::at) - frames.begin());
}
std::optional<DeckTrack> track_deck(const std::vector<RigFrame> &frames, const RigJoints &joints, std::string &why) {
    const auto needed = std::max({joints.trajectory, joints.deck, joints.left_foot, joints.right_foot});
    DeckTrack track;
    for (const auto &frame : frames) {
        if (frame.joints.size() <= needed) {
            why = "the recording is not of the skater skeleton";
            return std::nullopt;
        }
        const auto deck = world(frame.joints, joints.parents, joints.deck);
        const auto from_deck = [&](std::uint16_t foot) {
            return style::distance(world(frame.joints, joints.parents, foot).position, deck.position);
        };
        track.decks.push_back(deck);
        track.height.push_back(deck.position[1]);
        track.feet_from_deck.push_back(std::max(from_deck(joints.left_foot), from_deck(joints.right_foot)));
    }
    return track;
}
float ground_height(std::vector<float> height) {
    std::ranges::sort(height);
    return height[height.size() / 10];
}
Jump longest_jump(const std::vector<RigFrame> &frames, const std::vector<float> &height, float ground) {
    // The jump needs this much recording before and after it.
    constexpr std::uint32_t margin_ms = 200;
    Jump jump;
    for (std::size_t i = 0; i < frames.size();) {
        if (height[i] <= ground + ground_margin_m) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < frames.size() && height[end] > ground + ground_margin_m) ++end;
        if (end < frames.size() && frames[i].at >= frames.front().at + margin_ms && frames[end].at + margin_ms <= frames.back().at &&
            end - i > jump.on - jump.off) {
            jump.off = i;
            jump.on = end;
        }
        i = end;
    }
    return jump;
}
// The catch: the feet return to the board. In a plain ollie the feet stay, so the catch is at one third of the air time.
std::size_t catch_frame(const DeckTrack &track, float ground, const Jump &jump) {
    const auto &apart = track.feet_from_deck;
    std::vector<float> grounded;
    for (std::size_t i = 0; i < apart.size(); ++i)
        if (track.height[i] <= ground + ground_margin_m) grounded.push_back(apart[i]);
    const float resting = median(grounded);
    const auto peak = static_cast<std::size_t>(std::max_element(apart.begin() + static_cast<std::ptrdiff_t>(jump.off),
                                                                apart.begin() + static_cast<std::ptrdiff_t>(jump.on)) - apart.begin());
    std::size_t caught = jump.off + (jump.on - jump.off) / 3;
    if (apart[peak] > resting + feet_apart_margin_m)
        for (std::size_t i = peak; i < jump.on; ++i)
            if (apart[i] < resting + feet_back_margin_m) {
                caught = i;
                break;
            }
    return std::clamp(caught, jump.off + 1, jump.on - 1);
}
Clip cut_clip(std::uint8_t trick, const std::vector<RigFrame> &frames, const RigJoints &joints, const std::vector<Transform> &board,
              const Transform &deck_to_board, const std::vector<Transform> &decks, const Marks &marks) {
    const std::size_t begin = first_at(frames, frames[marks.flick].at > lead_ms ? frames[marks.flick].at - lead_ms : 0);
    const std::size_t end = std::max(first_at(frames, frames[marks.on].at + follow_ms), marks.on + 1);
    Clip clip{trick, {}, {}};
    clip.began = frames[begin].at;
    const Transform live_entity = board.empty() ? Transform{} : board[board_entity];
    for (std::size_t i = begin; i < end; ++i) {
        ClipFrame frame;
        frame.at = frames[i].at - frames[begin].at;
        frame.time = i < marks.flick ? -1.0f : i < marks.caught ? 0.0f : i < marks.on ? 1.0f : 2.0f;
        frame.pose.skater = frames[i].joints;
        frame.pose.root = world(frames[i].joints, joints.parents, joints.trajectory);
        frame.pose.root.scale = {1, 1, 1};
        if (!board.empty()) {
            // The borrowed board follows the deck joint. Its rig anchor keeps its offset on the board.
            const auto entity = compose(decks[i], deck_to_board);
            frame.pose.board = board;
            frame.pose.board[board_entity] = entity;
            frame.pose.board[board_entity].scale = live_entity.scale;
            if (board.size() > board_anchor) frame.pose.board[board_anchor] = anchored(entity, live_entity, board[board_anchor]);
        }
        clip.frames.push_back(std::move(frame));
    }
    return clip;
}
} // namespace

std::optional<Clip> clip_from_capture(std::uint8_t trick, const std::vector<RigFrame> &frames, const RigJoints &joints,
                                      const std::vector<Transform> &board, const Transform &deck_to_board, std::string &why) {
    if (frames.size() < 2 || frames.back().at < frames.front().at + min_recording_ms) {
        why = "the recording is too short";
        return std::nullopt;
    }
    const auto track = track_deck(frames, joints, why);
    if (!track) return std::nullopt;
    const float ground = ground_height(track->height);
    const auto jump = longest_jump(frames, track->height, ground);
    if (jump.on - jump.off < 2 || frames[jump.on].at < frames[jump.off].at + min_air_ms) {
        why = "no whole jump was seen in the recording";
        return std::nullopt;
    }
    const auto lead = frames[jump.off].at > flick_lead_ms ? frames[jump.off].at - flick_lead_ms : 0;
    const Marks marks{std::min(first_at(frames, lead), jump.off - 1), catch_frame(*track, ground, jump), jump.on};
    auto clip = cut_clip(trick, frames, joints, board, deck_to_board, track->decks, marks);
    if (!retime(clip)) {
        why = "the jump in the recording has no clear catch and landing";
        return std::nullopt;
    }
    return clip;
}
} // namespace dingosdk::style
