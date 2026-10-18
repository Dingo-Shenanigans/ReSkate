#include "anim_core.h"
#include "Engine/Core/Json/json.h"
#include "Extension/Multiplayer/Net/skater_bone_parents.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dingosdk::anim_studio {
namespace {
constexpr float pi = 3.14159265358979f;
float dot(const Quat& a, const Quat& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
Vec3 lerp(const Vec3& a, const Vec3& b, float t) noexcept {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
// Cubic Hermite through p1 -> p2 with Catmull-Rom slopes from the neighbours.
Vec3 hermite(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float u) noexcept {
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    Vec3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float m1 = (p2[i] - p0[i]) * 0.5f, m2 = (p3[i] - p1[i]) * 0.5f;
        out[i] = h00 * p1[i] + h10 * m1 + h01 * p2[i] + h11 * m2;
    }
    return out;
}
void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
float number(const Json& value, const char* what) {
    require(value.is_number(), what);
    const auto result = value.get<float>();
    require(std::isfinite(result), what);
    return result;
}
template <std::size_t N> std::array<float, N> numbers(const Json& value, const char* what) {
    require(value.is_array() && value.size() == N, what);
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = number(value.at(i), what);
    return out;
}
std::string_view ease_name(Ease ease) noexcept {
    return ease == Ease::linear ? "linear" : ease == Ease::step ? "step" : "smooth";
}

constexpr std::array<EditableJoint, 25> joints{{
    {board_joint, "Board", true},
    {7, "Hips", true},
    {42, "Lower back", false},
    {44, "Chest", false},
    {101, "Neck", false},
    {103, "Head", false},
    {275, "Left shoulder", false},
    {276, "Left arm", false},
    {277, "Left elbow", false},
    {278, "Left hand", false},
    {46, "Right shoulder", false},
    {47, "Right arm", false},
    {48, "Right elbow", false},
    {49, "Right hand", false},
    {341, "Left leg", false},
    {342, "Left knee", false},
    {343, "Left foot", false},
    {344, "Left toes", false},
    {8, "Right leg", false},
    {9, "Right knee", false},
    {10, "Right foot", false},
    {11, "Right toes", false},
    {43, "Middle back", false},
    {45, "Upper chest", false},
    {102, "Upper neck", false},
}};
} // namespace

Quat multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat conjugate(const Quat& q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }
Quat normalised(const Quat& q) noexcept {
    const float length = std::sqrt(dot(q, q));
    if (!(length > 1e-8f) || !std::isfinite(length)) return {0, 0, 0, 1};
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}
Quat slerp(Quat a, const Quat& b, float t) noexcept {
    float cosine = dot(a, b);
    if (cosine < 0) {
        a = {-a[0], -a[1], -a[2], -a[3]};
        cosine = -cosine;
    }
    if (cosine > 0.9995f) {
        Quat out{};
        for (std::size_t i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
        return normalised(out);
    }
    const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f)), sine = std::sin(angle);
    const float wa = std::sin((1 - t) * angle) / sine, wb = std::sin(t * angle) / sine;
    return normalised({a[0] * wa + b[0] * wb, a[1] * wa + b[1] * wb, a[2] * wa + b[2] * wb, a[3] * wa + b[3] * wb});
}
Vec3 rotate(const Quat& q, const Vec3& v) noexcept {
    const Vec3 u{q[0], q[1], q[2]};
    const float s = q[3];
    const Vec3 t{2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    return {v[0] + s * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + s * t[1] + (u[2] * t[0] - u[0] * t[2]),
            v[2] + s * t[2] + (u[0] * t[1] - u[1] * t[0])};
}
Quat axis_angle(const Vec3& axis, float radians) noexcept {
    const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(length > 1e-8f)) return {0, 0, 0, 1};
    const float s = std::sin(radians * 0.5f) / length;
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(radians * 0.5f)};
}
Quat from_euler(const Vec3& degrees) noexcept {
    const float k = pi / 180;
    return normalised(multiply(multiply(axis_angle({1, 0, 0}, degrees[0] * k), axis_angle({0, 1, 0}, degrees[1] * k)),
                               axis_angle({0, 0, 1}, degrees[2] * k)));
}
Vec3 to_euler(const Quat& raw) noexcept {
    const auto q = normalised(raw);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - z * w), m02 = 2 * (x * z + y * w);
    const float m12 = 2 * (y * z - x * w), m22 = 1 - 2 * (x * x + y * y);
    const float k = 180 / pi;
    const float b = std::asin(std::clamp(m02, -1.0f, 1.0f));
    if (std::abs(m02) > 0.9999f) {
        // Gimbal lock: all of the turn about X and Z goes on X.
        const float m10 = 2 * (x * y + z * w), m11 = 1 - 2 * (x * x + z * z);
        return {std::atan2(m10, m11) * k, b * k, 0};
    }
    return {std::atan2(-m12, m22) * k, b * k, std::atan2(-m01, m00) * k};
}
bool finite(const Joint& joint) noexcept {
    for (const float v : joint.scale) if (!std::isfinite(v) || std::abs(v) > 1000) return false;
    for (const float v : joint.rotation) if (!std::isfinite(v) || std::abs(v) > 1.01f) return false;
    for (const float v : joint.position) if (!std::isfinite(v) || std::abs(v) > 1e6f) return false;
    return true;
}

std::span<const EditableJoint> editable_joints() noexcept { return joints; }
const EditableJoint* editable(std::uint16_t joint) noexcept {
    for (const auto& j : joints) if (j.index == joint) return &j;
    return nullptr;
}

void Clip::sample(float seconds, std::span<Joint> out) const noexcept {
    if (empty()) return;
    const auto count = std::min<std::size_t>(out.size(), joints);
    if (seconds <= times.front() || times.size() == 1) {
        std::copy_n(frames.begin(), count, out.begin());
        return;
    }
    if (seconds >= times.back()) {
        std::copy_n(frames.begin() + (times.size() - 1) * joints, count, out.begin());
        return;
    }
    const auto next = static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), seconds) - times.begin());
    const auto previous = next - 1;
    const float span = times[next] - times[previous];
    const float t = span > 1e-6f ? (seconds - times[previous]) / span : 0.0f;
    const auto* a = frames.data() + previous * joints;
    const auto* b = frames.data() + next * joints;
    for (std::size_t i = 0; i < count; ++i) {
        out[i].scale = lerp(a[i].scale, b[i].scale, t);
        out[i].rotation = slerp(a[i].rotation, b[i].rotation, t);
        out[i].position = lerp(a[i].position, b[i].position, t);
    }
}

bool Clip::sample_board(float seconds, Joint& out) const noexcept {
    if (empty() || board.size() != times.size()) return false;
    if (seconds <= times.front() || times.size() == 1) {
        out = board.front();
        return true;
    }
    if (seconds >= times.back()) {
        out = board.back();
        return true;
    }
    const auto next = static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), seconds) - times.begin());
    const auto previous = next - 1;
    const float span = times[next] - times[previous];
    const float t = span > 1e-6f ? (seconds - times[previous]) / span : 0.0f;
    out.scale = lerp(board[previous].scale, board[next].scale, t);
    out.rotation = slerp(board[previous].rotation, board[next].rotation, t);
    out.position = lerp(board[previous].position, board[next].position, t);
    return true;
}

Offset evaluate(const Track& track, float seconds) noexcept {
    const auto& keys = track.keys;
    if (keys.empty()) return {};
    if (seconds <= keys.front().time) return {keys.front().rotation, keys.front().offset};
    if (seconds >= keys.back().time) return {keys.back().rotation, keys.back().offset};
    std::size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].time <= seconds) ++i;
    const auto& a = keys[i];
    const auto& b = keys[i + 1];
    const float span = b.time - a.time;
    const float u = span > 1e-6f ? std::clamp((seconds - a.time) / span, 0.0f, 1.0f) : 1.0f;
    switch (a.ease) {
    case Ease::step:
        return {a.rotation, a.offset};
    case Ease::linear:
        return {slerp(a.rotation, b.rotation, u), lerp(a.offset, b.offset, u)};
    case Ease::smooth:
    default: {
        const float eased = u * u * (3 - 2 * u);
        const auto& before = i > 0 ? keys[i - 1].offset : a.offset;
        const auto& after = i + 2 < keys.size() ? keys[i + 2].offset : b.offset;
        return {slerp(a.rotation, b.rotation, eased), hermite(before, a.offset, b.offset, after, u)};
    }
    }
}

Track* Animation::track(std::uint16_t joint) noexcept {
    for (auto& t : tracks) if (t.joint == joint) return &t;
    return nullptr;
}
const Track* Animation::track(std::uint16_t joint) const noexcept {
    for (const auto& t : tracks) if (t.joint == joint) return &t;
    return nullptr;
}
std::size_t Animation::key_count() const noexcept {
    std::size_t n = 0;
    for (const auto& t : tracks) n += t.keys.size();
    return n;
}
void apply_edits(const Animation& animation, float seconds, std::span<Joint> pose) noexcept {
    for (const auto& track : animation.tracks) {
        if (track.hidden || track.keys.empty() || track.joint >= pose.size()) continue;
        const auto edit = evaluate(track, seconds);
        auto& joint = pose[track.joint];
        joint.rotation = normalised(multiply(joint.rotation, edit.rotation));
        for (std::size_t i = 0; i < 3; ++i) joint.position[i] += edit.offset[i];
    }
}
Key& set_key(Animation& animation, std::uint16_t joint, float seconds, const Offset& value, float merge) {
    auto* track = animation.track(joint);
    if (!track) {
        animation.tracks.push_back({joint, {}, false, false});
        track = &animation.tracks.back();
    }
    for (auto& key : track->keys)
        if (std::abs(key.time - seconds) <= merge) {
            key.rotation = normalised(value.rotation);
            key.offset = value.offset;
            return key;
        }
    Key key{seconds, normalised(value.rotation), value.offset, Ease::smooth};
    const auto at = std::upper_bound(track->keys.begin(), track->keys.end(), seconds,
                                     [](float t, const Key& k) { return t < k.time; });
    return *track->keys.insert(at, key);
}
bool remove_key(Animation& animation, std::uint16_t joint, float seconds, float merge) {
    auto* track = animation.track(joint);
    if (!track) return false;
    const auto before = track->keys.size();
    std::erase_if(track->keys, [&](const Key& k) { return std::abs(k.time - seconds) <= merge; });
    return track->keys.size() != before;
}
std::size_t move_key(Track& track, std::size_t index, float to, float duration) noexcept {
    if (index >= track.keys.size()) return index;
    auto key = track.keys[index];
    key.time = std::clamp(to, 0.0f, std::max(duration, 0.0f));
    track.keys.erase(track.keys.begin() + static_cast<std::ptrdiff_t>(index));
    const auto at = std::upper_bound(track.keys.begin(), track.keys.end(), key.time,
                                     [](float t, const Key& k) { return t < k.time; });
    const auto placed = static_cast<std::size_t>(at - track.keys.begin());
    track.keys.insert(at, key);
    return placed;
}

std::int16_t parent(std::uint16_t joint) noexcept {
    return joint < multiplayer::skater_bone_parents.size() ? multiplayer::skater_bone_parents[joint] : -1;
}
Joint world(std::span<const Joint> local, std::uint16_t joint) noexcept {
    std::array<std::uint16_t, 64> chain{};
    std::size_t length = 0;
    for (int at = joint; at >= 0 && at < static_cast<int>(local.size()) && length < chain.size(); at = parent(static_cast<std::uint16_t>(at)))
        chain[length++] = static_cast<std::uint16_t>(at);
    Joint result;
    float scale = 1;
    for (std::size_t i = length; i-- > 0;) {
        const auto& bone = local[chain[i]];
        const auto moved = rotate(result.rotation, {bone.position[0] * scale, bone.position[1] * scale, bone.position[2] * scale});
        for (std::size_t a = 0; a < 3; ++a) result.position[a] += moved[a];
        result.rotation = normalised(multiply(result.rotation, bone.rotation));
        scale *= (bone.scale[0] + bone.scale[1] + bone.scale[2]) / 3.0f;
    }
    result.scale = {scale, scale, scale};
    return result;
}

std::string to_json(const Animation& animation) {
    Json root = Json::object();
    root["format"] = "fourth-animation-studio";
    root["version"] = 1;
    root["name"] = animation.name;
    root["trick"] = animation.trick;
    root["speed"] = animation.speed;
    root["loop"] = animation.loop;
    root["pop"] = animation.pop;
    root["duration"] = animation.clip.duration();
    root["takeoff"] = animation.clip.takeoff;
    root["landing"] = animation.clip.landing;
    Json tracks = Json::array();
    for (const auto& track : animation.tracks) {
        if (track.keys.empty()) continue;
        Json t = Json::object();
        t["joint"] = track.joint;
        if (const auto* e = editable(track.joint)) t["label"] = std::string(e->label);
        t["hidden"] = track.hidden;
        t["locked"] = track.locked;
        Json keys = Json::array();
        for (const auto& key : track.keys) {
            Json k = Json::object();
            k["t"] = key.time;
            k["r"] = Json::array({key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3]});
            k["p"] = Json::array({key.offset[0], key.offset[1], key.offset[2]});
            k["ease"] = std::string(ease_name(key.ease));
            keys.push_back(std::move(k));
        }
        t["keys"] = std::move(keys);
        tracks.push_back(std::move(t));
    }
    root["tracks"] = std::move(tracks);
    return root.dump(2);
}
Animation from_json(std::string_view text) {
    const auto root = Json::parse(text);
    require(root.is_object() && root.value("format", "") == "fourth-animation-studio", "not an Animation Studio profile");
    require(root.contains("version") && root.at("version").is_number() && root.at("version").get<int>() == 1,
            "the profile is from a newer Animation Studio");
    Animation result;
    result.name = root.value("name", "");
    result.trick = root.value("trick", "");
    result.speed = root.contains("speed") ? std::clamp(number(root.at("speed"), "bad speed"), 0.1f, 4.0f) : 1.0f;
    result.loop = root.contains("loop") && root.at("loop").is_boolean() ? root.at("loop").get<bool>() : true;
    result.pop = root.value("pop", "");
    if (result.pop != "low" && result.pop != "mid" && result.pop != "high") result.pop.clear();
    const float duration = root.contains("duration") ? number(root.at("duration"), "bad duration") : 0.0f;
    require(duration >= 0 && duration < 120, "bad duration");
    std::size_t total = 0;
    if (root.contains("tracks")) {
        const auto& tracks = root.at("tracks");
        require(tracks.is_array() && tracks.size() <= joints.size(), "bad tracks");
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const auto& t = tracks.at(i);
            require(t.is_object() && t.contains("joint") && t.at("joint").is_number_integer(), "a track has no joint");
            const auto joint = t.at("joint").get<int>();
            require(joint >= 0 && joint < 65536 && editable(static_cast<std::uint16_t>(joint)), "a track names a joint the editor does not edit");
            require(!result.track(static_cast<std::uint16_t>(joint)), "two tracks for one joint");
            Track track{static_cast<std::uint16_t>(joint), {},
                        t.contains("hidden") && t.at("hidden").is_boolean() && t.at("hidden").get<bool>(),
                        t.contains("locked") && t.at("locked").is_boolean() && t.at("locked").get<bool>()};
            require(t.contains("keys") && t.at("keys").is_array(), "a track has no keys");
            const auto& keys = t.at("keys");
            total += keys.size();
            require(total <= 20000, "too many keys");
            for (std::size_t k = 0; k < keys.size(); ++k) {
                const auto& key = keys.at(k);
                require(key.is_object() && key.contains("t") && key.contains("r") && key.contains("p"), "a key is incomplete");
                Key value;
                value.time = number(key.at("t"), "bad key time");
                require(value.time >= 0 && value.time <= duration + 0.01f, "a key lies past the clip");
                value.rotation = normalised(numbers<4>(key.at("r"), "bad key rotation"));
                value.offset = numbers<3>(key.at("p"), "bad key offset");
                for (const float v : value.offset) require(std::abs(v) < 10, "a key moves a joint more than 10 m");
                const auto ease = key.value("ease", "smooth");
                value.ease = ease == "linear" ? Ease::linear : ease == "step" ? Ease::step : Ease::smooth;
                track.keys.push_back(value);
            }
            std::stable_sort(track.keys.begin(), track.keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
            result.tracks.push_back(std::move(track));
        }
    }
    return result;
}

// "FAS3": the joints that never move in the clip (most of the face and the fingers) are stored once,
// the others every frame, and the board every frame when there is one. "FAS1" (no board) and "FAS2"
// (every joint every frame, then the board) still read.
std::vector<std::uint8_t> clip_bytes(const Clip& clip) {
    std::vector<std::uint8_t> out;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    };
    const auto put_joint = [&](const Joint& j) {
        put(j.scale.data(), 12);
        put(j.rotation.data(), 16);
        put(j.position.data(), 12);
    };
    const bool board = clip.board.size() == clip.times.size() && !clip.board.empty();
    const std::uint32_t frames = static_cast<std::uint32_t>(clip.times.size());
    put("FAS3", 4);
    put(&clip.joints, 4);
    put(&frames, 4);
    put(&clip.takeoff, 4);
    put(&clip.landing, 4);
    const std::uint8_t has_board = board ? 1 : 0;
    put(&has_board, 1);
    put(clip.times.data(), clip.times.size() * 4);
    std::vector<std::uint8_t> moves(clip.joints, 0);
    const auto same = [](const Joint& a, const Joint& b) {
        for (std::size_t i = 0; i < 3; ++i)
            if (a.scale[i] != b.scale[i] || a.position[i] != b.position[i]) return false;
        for (std::size_t i = 0; i < 4; ++i)
            if (a.rotation[i] != b.rotation[i]) return false;
        return true;
    };
    for (std::uint32_t j = 0; j < clip.joints && frames; ++j)
        for (std::uint32_t f = 1; f < frames && !moves[j]; ++f) moves[j] = !same(clip.frames[f * clip.joints + j], clip.frames[j]);
    put(moves.data(), moves.size());
    for (std::uint32_t j = 0; j < clip.joints && frames; ++j)
        if (!moves[j]) put_joint(clip.frames[j]);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t j = 0; j < clip.joints; ++j)
            if (moves[j]) put_joint(clip.frames[f * clip.joints + j]);
        if (board) put_joint(clip.board[f]);
    }
    return out;
}
Clip clip_from_bytes(std::span<const std::uint8_t> bytes) {
    std::size_t at = 0;
    const auto get = [&](void* data, std::size_t size) {
        require(bytes.size() - at >= size, "the clip file is cut short");
        std::memcpy(data, bytes.data() + at, size);
        at += size;
    };
    const auto get_joint = [&](Joint& j, const char* what) {
        get(j.scale.data(), 12);
        get(j.rotation.data(), 16);
        get(j.position.data(), 12);
        require(finite(j), what);
    };
    char magic[4]{};
    require(bytes.size() >= 20, "the clip file is cut short");
    get(magic, 4);
    const bool v3 = std::memcmp(magic, "FAS3", 4) == 0;
    bool board = std::memcmp(magic, "FAS2", 4) == 0;
    require(v3 || board || std::memcmp(magic, "FAS1", 4) == 0, "not an Animation Studio clip");
    Clip clip;
    std::uint32_t frames{};
    get(&clip.joints, 4);
    get(&frames, 4);
    get(&clip.takeoff, 4);
    get(&clip.landing, 4);
    require(clip.joints > 0 && clip.joints <= 512 && frames <= 20000, "the clip is too large");
    if (v3) {
        std::uint8_t has_board{};
        get(&has_board, 1);
        board = has_board != 0;
    }
    clip.times.resize(frames);
    get(clip.times.data(), frames * 4ull);
    for (std::size_t i = 0; i < clip.times.size(); ++i)
        require(std::isfinite(clip.times[i]) && clip.times[i] >= 0 && (i == 0 || clip.times[i] >= clip.times[i - 1]), "the clip's times are out of order");
    clip.frames.resize(static_cast<std::size_t>(frames) * clip.joints);
    if (board) clip.board.resize(frames);
    if (v3) {
        std::vector<std::uint8_t> moves(clip.joints);
        get(moves.data(), moves.size());
        std::size_t moving = 0;
        for (const auto m : moves) moving += m ? 1 : 0;
        require(bytes.size() - at == (clip.joints - moving) * 40ull + frames * (moving + (board ? 1 : 0)) * 40ull, "the clip file has the wrong size");
        Joint still;
        for (std::uint32_t j = 0; j < clip.joints; ++j)
            if (!moves[j]) {
                get_joint(still, "the clip holds a broken pose");
                for (std::uint32_t f = 0; f < frames; ++f) clip.frames[f * clip.joints + j] = still;
            }
        for (std::uint32_t f = 0; f < frames; ++f) {
            for (std::uint32_t j = 0; j < clip.joints; ++j)
                if (moves[j]) get_joint(clip.frames[f * clip.joints + j], "the clip holds a broken pose");
            if (board) get_joint(clip.board[f], "the clip holds a broken board");
        }
    } else {
        require(bytes.size() - at == static_cast<std::size_t>(frames) * (clip.joints + (board ? 1 : 0)) * 40ull, "the clip file has the wrong size");
        for (auto& j : clip.frames) get_joint(j, "the clip holds a broken pose");
        for (auto& j : clip.board) get_joint(j, "the clip holds a broken board");
    }
    require(std::isfinite(clip.takeoff) && std::isfinite(clip.landing), "bad takeoff or landing");
    return clip;
}
std::string file_safe(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_') out += c;
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Animation") : out;
}
} // namespace dingosdk::anim_studio
