#include "style_takes.h"
#include <lz4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dingosdk::style {
namespace {
constexpr std::uint32_t landing_ms = 450;
constexpr std::size_t maximum_frames = 900, maximum_joints = 512, maximum_board = 64;
constexpr std::size_t transform_floats = 10;

std::array<float, 3> rotate(const Quat &q, const std::array<float, 3> &v) noexcept {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz), v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
Transform blend(const Transform &a, const Transform &b, float amount) noexcept {
    Transform result;
    for (std::size_t i = 0; i < 3; ++i) {
        result.position[i] = a.position[i] + (b.position[i] - a.position[i]) * amount;
        result.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * amount;
    }
    result.rotation = mix(a.rotation, b.rotation, amount);
    return result;
}
Pose blend(const Pose &a, const Pose &b, float amount) {
    if (amount <= 0 || a.skater.size() != b.skater.size() || a.board.size() != b.board.size()) return a;
    if (amount >= 1) return b;
    Pose result;
    result.root = blend(a.root, b.root, amount);
    result.skater.reserve(a.skater.size());
    for (std::size_t i = 0; i < a.skater.size(); ++i) result.skater.push_back(blend(a.skater[i], b.skater[i], amount));
    result.board.reserve(a.board.size());
    for (std::size_t i = 0; i < a.board.size(); ++i) result.board.push_back(blend(a.board[i], b.board[i], amount));
    return result;
}
// The first frame at or past `time`.
std::size_t upper(const Clip &clip, float time) noexcept {
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
    std::uint32_t version, trick, learned, frames, joints, board, raw_bytes, rig;
};
float median(std::vector<float> values) {
    if (values.empty()) return 0;
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2));
    return values[values.size() / 2];
}
} // namespace

bool retime(Clip &clip) {
    auto &frames = clip.frames;
    const auto first = [&](float from) {
        return static_cast<std::size_t>(std::ranges::find_if(frames, [&](const ClipFrame &f) { return f.time >= from; }) - frames.begin());
    };
    const std::size_t flick = first(0), caught = first(1), landed = first(2);
    if (flick >= frames.size() || caught >= frames.size() || landed >= frames.size() || !(flick < caught && caught < landed)) return false;
    // A slam after the catch has no landing. Its touchdown would be the first frame after the trick.
    if (frames[caught].time >= 2 || frames[landed].time > trick_end) return false;
    // The landing lasts landing_ms after the touchdown, or to the end of the clip.
    std::size_t over = landed;
    while (over + 1 < frames.size() && frames[over].at < frames[landed].at + landing_ms) ++over;
    if (over == landed) return false;
    const std::array<std::size_t, 4> marks{flick, caught, landed, over};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        auto &frame = frames[i];
        if (i < flick) frame.time = (static_cast<float>(frame.at) - static_cast<float>(frames[flick].at)) / 1000.0f - 0.0001f;
        else if (i >= over) frame.time = trick_end + (static_cast<float>(frame.at) - static_cast<float>(frames[over].at)) / 1000.0f;
        else {
            const std::size_t part = i >= landed ? 2 : i >= caught ? 1 : 0;
            const float span = std::max(1.0f, static_cast<float>(frames[marks[part + 1]].at - frames[marks[part]].at));
            frame.time = static_cast<float>(part) + (static_cast<float>(frame.at) - static_cast<float>(frames[marks[part]].at)) / span;
        }
    }
    // Two frames with the same millisecond must stay in order.
    for (std::size_t i = 1; i < frames.size(); ++i)
        if (frames[i].time <= frames[i - 1].time) frames[i].time = std::nextafter(frames[i - 1].time, 1e9f);
    return true;
}
bool unwarp(Clip &clip) {
    auto &frames = clip.frames;
    if (frames.size() < 16) return false;
    std::vector<float> step(frames.size()), speed;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const auto &a = frames[i - 1].pose.root.position, &b = frames[i].pose.root.position;
        step[i] = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
        if (!std::isfinite(step[i]) || step[i] > 2.0f) return false; // a jump back to the start is not movement
    }
    // Measure speed across 6 frames, because the time of one frame is too coarse.
    constexpr std::size_t window = 6;
    for (std::size_t i = window; i < frames.size(); ++i) {
        float travelled{};
        for (std::size_t k = i - window + 1; k <= i; ++k) travelled += step[k];
        if (const auto passed = frames[i].at - frames[i - window].at) speed.push_back(travelled / static_cast<float>(passed));
    }
    if (speed.size() < 8) return false;
    // The roll before the trick has real speed. Without enough roll, the fastest parts give the real speed.
    std::size_t lead{};
    while (lead + 1 < frames.size() && frames[lead + 1].time < 0) ++lead;
    float travelled{};
    for (std::size_t i = 1; i <= lead; ++i) travelled += step[i];
    std::ranges::sort(speed);
    const float real = lead >= 10 && frames[lead].at > frames.front().at + 100 ? travelled / static_cast<float>(frames[lead].at - frames.front().at)
                                                                              : speed[speed.size() * 85 / 100];
    if (!(real > 0.0005f)) return false; // almost no movement, so no measurement is possible
    double at{};
    for (std::size_t i = 1; i < frames.size(); ++i) {
        at += std::max(1.0, static_cast<double>(step[i] / real));
        frames[i].at = frames.front().at + static_cast<std::uint32_t>(at + 0.5);
    }
    return true;
}
Pose sample(const Clip &clip, float time) {
    if (clip.frames.empty()) return {};
    const auto next = upper(clip, time);
    if (next == 0) return clip.frames.front().pose;
    if (next >= clip.frames.size()) return clip.frames.back().pose;
    const auto &a = clip.frames[next - 1], &b = clip.frames[next];
    return blend(a.pose, b.pose, (time - a.time) / std::max(b.time - a.time, 1e-6f));
}
float first_time(const Clip &clip) noexcept { return clip.frames.empty() ? 0 : clip.frames.front().time; }
float last_time(const Clip &clip) noexcept { return clip.frames.empty() ? 0 : clip.frames.back().time; }
std::uint32_t duration(const Clip &clip) noexcept { return clip.frames.empty() ? 0 : clip.frames.back().at - clip.frames.front().at; }
float time_at(const Clip &clip, std::uint32_t milliseconds) noexcept {
    if (clip.frames.empty()) return 0;
    const auto at = clip.frames.front().at + milliseconds;
    const auto next = std::ranges::lower_bound(clip.frames, at, {}, &ClipFrame::at);
    if (next == clip.frames.begin()) return clip.frames.front().time;
    if (next == clip.frames.end()) return clip.frames.back().time;
    const auto &a = *(next - 1), &b = *next;
    return a.time + (b.time - a.time) * static_cast<float>(at - a.at) / static_cast<float>(std::max<std::uint32_t>(b.at - a.at, 1));
}
float step(const Clip &clip, float time, int frames) noexcept {
    if (clip.frames.empty()) return time;
    // The shown frame is the last frame at or before `time`.
    auto index = static_cast<std::ptrdiff_t>(upper(clip, time));
    if (index >= static_cast<std::ptrdiff_t>(clip.frames.size()) || clip.frames[static_cast<std::size_t>(index)].time > time) --index;
    index = std::clamp<std::ptrdiff_t>(index + frames, 0, static_cast<std::ptrdiff_t>(clip.frames.size()) - 1);
    return clip.frames[static_cast<std::size_t>(index)].time;
}

std::vector<std::uint8_t> encode_clip(const Clip &clip) {
    if (clip.frames.empty() || clip.frames.size() > maximum_frames) throw std::runtime_error("The clip has no frames or too many.");
    const auto joints = clip.frames.front().pose.skater.size(), board = clip.frames.front().pose.board.size();
    // The game's board is saved only when every frame has it.
    auto rig = clip.frames.front().rig.size();
    for (const auto &frame : clip.frames)
        if (frame.rig.size() != rig || rig > maximum_board) rig = 0;
    std::vector<float> raw;
    raw.reserve(clip.frames.size() * (21 + (1 + joints + board + rig) * transform_floats));
    for (const auto &frame : clip.frames) {
        if (frame.pose.skater.size() != joints || frame.pose.board.size() != board) throw std::runtime_error("The clip's frames differ in shape.");
        raw.push_back(frame.time);
        raw.push_back(static_cast<float>(frame.at));
        raw.push_back(frame.fov);
        raw.insert(raw.end(), frame.view.begin(), frame.view.end());
        raw.push_back(frame.rate);
        raw.push_back(frame.phase);
        put(raw, frame.pose.root);
        for (const auto &joint : frame.pose.skater) put(raw, joint);
        for (const auto &bone : frame.pose.board) put(raw, bone);
        for (std::size_t i = 0; i < rig; ++i) put(raw, frame.rig[i]);
    }
    const auto bytes = raw.size() * sizeof(float);
    Header header{{'R', 'S', 'T', 'K'}, 3, clip.trick, clip.learned, static_cast<std::uint32_t>(clip.frames.size()),
                  static_cast<std::uint32_t>(joints), static_cast<std::uint32_t>(board), static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(rig)};
    std::vector<std::uint8_t> out(sizeof(header) + static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(bytes))));
    std::memcpy(out.data(), &header, sizeof(header));
    const int packed = LZ4_compress_default(reinterpret_cast<const char *>(raw.data()), reinterpret_cast<char *>(out.data() + sizeof(header)),
                                            static_cast<int>(bytes), static_cast<int>(out.size() - sizeof(header)));
    if (packed <= 0) throw std::runtime_error("The clip could not be packed.");
    out.resize(sizeof(header) + static_cast<std::size_t>(packed));
    return out;
}
Clip decode_clip(std::span<const std::uint8_t> bytes) {
    Header header{};
    if (bytes.size() <= sizeof(header)) throw std::runtime_error("This is not a clip.");
    std::memcpy(&header, bytes.data(), sizeof(header));
    const std::size_t per_frame = 21 + (1 + std::size_t{header.joints} + header.board + header.rig) * transform_floats;
    if (std::memcmp(header.magic, "RSTK", 4) || header.version != 3 || header.rig > maximum_board || !header.frames || header.frames > maximum_frames ||
        header.joints > maximum_joints || header.board > maximum_board || header.trick < 1 || header.trick >= flip_trick_names.size() ||
        header.raw_bytes != header.frames * per_frame * sizeof(float))
        throw std::runtime_error("This is not a clip this version reads.");
    std::vector<float> raw(header.raw_bytes / sizeof(float));
    if (LZ4_decompress_safe(reinterpret_cast<const char *>(bytes.data() + sizeof(header)), reinterpret_cast<char *>(raw.data()),
                            static_cast<int>(bytes.size() - sizeof(header)), static_cast<int>(header.raw_bytes)) !=
        static_cast<int>(header.raw_bytes))
        throw std::runtime_error("The clip is damaged.");
    Clip clip{static_cast<std::uint8_t>(header.trick), header.learned != 0, {}};
    clip.frames.reserve(header.frames);
    const float *in = raw.data();
    for (std::uint32_t i = 0; i < header.frames; ++i) {
        ClipFrame frame;
        frame.time = *in++;
        const float at = *in++;
        if (!std::isfinite(frame.time) || !std::isfinite(at) || at < 0 || at > 600000) throw std::runtime_error("The clip's timing is damaged.");
        frame.at = static_cast<std::uint32_t>(at);
        frame.fov = *in++;
        for (auto &value : frame.view) value = *in++;
        for (const auto value : frame.view)
            if (!std::isfinite(value)) frame.fov = 0;
        if (!std::isfinite(frame.fov)) frame.fov = 0;
        frame.rate = *in++;
        frame.phase = *in++;
        if (!std::isfinite(frame.rate) || frame.rate <= 0) frame.rate = 1;
        if (!std::isfinite(frame.phase)) frame.phase = -1;
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
    result.rotation = {-transform.rotation[0], -transform.rotation[1], -transform.rotation[2], transform.rotation[3]};
    const auto back = rotate(result.rotation, transform.position);
    for (std::size_t i = 0; i < 3; ++i) result.position[i] = -back[i];
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

std::optional<Clip> clip_from_capture(std::uint8_t trick, const std::vector<RigFrame> &frames, const RigJoints &joints,
                                      const std::vector<Transform> &board, const Transform &deck_to_board, std::string &why) {
    if (frames.size() < 60) {
        why = "the recording is too short";
        return std::nullopt;
    }
    const auto needed = std::max({joints.trajectory, joints.deck, joints.left_foot, joints.right_foot});
    std::vector<Transform> decks;
    std::vector<float> height, apart;
    for (const auto &frame : frames) {
        if (frame.joints.size() <= needed) {
            why = "the recording is not of the skater skeleton";
            return std::nullopt;
        }
        const auto deck = world(frame.joints, joints.parents, joints.deck);
        const auto distance = [&](std::uint16_t foot) {
            const auto at = world(frame.joints, joints.parents, foot).position;
            return std::sqrt((at[0] - deck.position[0]) * (at[0] - deck.position[0]) + (at[1] - deck.position[1]) * (at[1] - deck.position[1]) +
                             (at[2] - deck.position[2]) * (at[2] - deck.position[2]));
        };
        decks.push_back(deck);
        height.push_back(deck.position[1]);
        apart.push_back(std::max(distance(joints.left_foot), distance(joints.right_foot)));
    }
    // The ground is the deck height at the lowest tenth of the frames.
    auto sorted = height;
    std::ranges::sort(sorted);
    const float ground = sorted[sorted.size() / 10];
    // Find the longest run with the deck off the ground, 12 frames or more from each end.
    std::size_t off{}, on{};
    for (std::size_t i = 0; i < frames.size();) {
        if (height[i] <= ground + 0.04f) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < frames.size() && height[end] > ground + 0.04f) ++end;
        if (i >= 12 && end + 12 <= frames.size() && end - i > on - off) off = i, on = end;
        i = end;
    }
    if (on - off < 8) {
        why = "no whole jump was seen in the recording";
        return std::nullopt;
    }
    // The catch: the feet return to the board. In a plain ollie the feet stay, so the catch is at one third of the air time.
    std::vector<float> grounded;
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (height[i] <= ground + 0.04f) grounded.push_back(apart[i]);
    const float resting = median(grounded);
    const auto peak = static_cast<std::size_t>(std::max_element(apart.begin() + static_cast<std::ptrdiff_t>(off),
                                                                apart.begin() + static_cast<std::ptrdiff_t>(on)) - apart.begin());
    std::size_t caught = off + (on - off) / 3;
    if (apart[peak] > resting + 0.08f)
        for (std::size_t i = peak; i < on; ++i)
            if (apart[i] < resting + 0.05f) {
                caught = i;
                break;
            }
    caught = std::clamp(caught, off + 1, on - 1);
    // The flick is 5 frames before the board leaves the ground.
    const std::size_t flick = off >= 5 ? off - 5 : 0;
    const std::size_t begin = flick >= 24 ? flick - 24 : 0, end = std::min(frames.size(), on + 45);
    Clip clip{trick, true, {}};
    clip.began = frames[begin].at;
    const Transform live_entity = board.empty() ? Transform{} : board.front();
    for (std::size_t i = begin; i < end; ++i) {
        ClipFrame frame;
        frame.at = frames[i].at - frames[begin].at;
        frame.time = i < flick ? -1.0f : i < caught ? 0.0f : i < on ? 1.0f : 2.0f;
        frame.pose.skater = frames[i].joints;
        frame.pose.root = world(frames[i].joints, joints.parents, joints.trajectory);
        frame.pose.root.scale = {1, 1, 1};
        if (!board.empty()) {
            // The borrowed board follows the deck joint. Its rig anchor keeps its offset on the board.
            const auto entity = compose(decks[i], deck_to_board);
            frame.pose.board = board;
            frame.pose.board.front() = entity;
            frame.pose.board.front().scale = live_entity.scale;
            if (board.size() > 2) {
                auto anchor = compose(entity, compose(inverse(live_entity), board[2]));
                anchor.scale = board[2].scale;
                frame.pose.board[2] = anchor;
            }
        }
        clip.frames.push_back(std::move(frame));
    }
    (void)unwarp(clip);
    if (!retime(clip)) {
        why = "the jump in the recording has no clear catch and landing";
        return std::nullopt;
    }
    return clip;
}
} // namespace dingosdk::style
