#include "viewmodel_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace dingosdk::viewmodel {
namespace {
// Reads the file front to back; any read past its end marks it bad and reads zeros.
struct Reader {
    std::string_view bytes;
    std::size_t at{};
    bool bad{};
    bool take(void* out, std::size_t size) {
        if (bad || size > bytes.size() - at) {
            bad = true;
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, bytes.data() + at, size);
        at += size;
        return true;
    }
    template <class T> T value() {
        T out{};
        take(&out, sizeof out);
        return out;
    }
    std::string text() {
        const auto size = value<std::uint8_t>();
        std::string out(size, '\0');
        take(out.data(), size);
        return out;
    }
    // A count of things at least `each` bytes apiece, checked against what is left before anything is
    // made that big.
    bool fits(std::uint64_t count, std::size_t each) const { return !bad && count <= (bytes.size() - at) / each; }
};

bool finite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) return false;
    return true;
}
} // namespace

int Model::bone(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (bones[i].name == name) return static_cast<int>(i);
    return -1;
}
int Model::clip(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < clips.size(); ++i)
        if (clips[i].name == name) return static_cast<int>(i);
    return -1;
}

std::optional<Model> parse(std::string_view bytes, std::string& why) {
    Reader r{bytes};
    char magic[4]{};
    r.take(magic, 4);
    if (r.bad || std::memcmp(magic, "RSVM", 4) != 0) {
        why = "it is not a viewmodel (.rsvm) file";
        return std::nullopt;
    }
    if (const auto version = r.value<std::uint32_t>(); version != 1) {
        why = "it is a viewmodel of version " + std::to_string(version) + ", and this ReSkate reads version 1";
        return std::nullopt;
    }
    const auto bones = r.value<std::uint32_t>(), materials = r.value<std::uint32_t>(), parts = r.value<std::uint32_t>();
    const auto vertices = r.value<std::uint32_t>(), indices = r.value<std::uint32_t>(), clips = r.value<std::uint32_t>();
    const auto bad = [&](const std::string& what) {
        why = what;
        return std::nullopt;
    };
    if (r.bad) return bad("it ends in its header");
    if (bones == 0 || bones > max_bones) return bad("a viewmodel has 1 to " + std::to_string(max_bones) + " bones, and this has " + std::to_string(bones));
    if (materials == 0 || materials > 8) return bad("a viewmodel has 1 to 8 materials");
    if (parts == 0 || parts > 64) return bad("a viewmodel has 1 to 64 parts");
    if (vertices == 0 || vertices > 1'000'000) return bad("a viewmodel has 1 to 1000000 vertices");
    if (indices == 0 || indices % 3 != 0 || indices > 3'000'000) return bad("a viewmodel's indices are whole triangles, at most 1000000");
    if (clips > 64) return bad("a viewmodel has at most 64 clips");
    Model model;
    model.bones.resize(bones);
    for (auto& bone : model.bones) {
        bone.name = r.text();
        bone.parent = r.value<std::int16_t>();
        r.take(bone.bind.data(), sizeof bone.bind);
        if (bone.parent < -1 || bone.parent >= static_cast<int>(bones)) return bad("bone \"" + bone.name + "\" has a parent that is not one of its bones");
        if (!finite(bone.bind.data(), 12)) return bad("bone \"" + bone.name + "\" is not a number somewhere");
    }
    model.materials.resize(materials);
    for (auto& material : model.materials) {
        material.name = r.text();
        material.albedo = r.text();
        material.normal = r.text();
        material.surface = r.text();
        material.roughness = std::clamp(r.value<float>(), 0.0f, 1.0f);
        material.specular = std::clamp(r.value<float>(), 0.0f, 1.0f);
        if (!std::isfinite(material.roughness) || !std::isfinite(material.specular)) return bad("material \"" + material.name + "\" is not a number somewhere");
    }
    model.parts.resize(parts);
    for (auto& part : model.parts) {
        part.material = r.value<std::uint32_t>();
        part.first = r.value<std::uint32_t>();
        part.count = r.value<std::uint32_t>();
        if (part.material >= materials) return bad("a part's material is not one of its materials");
        if (part.first > indices || part.count > indices - part.first || part.count % 3 != 0)
            return bad("a part's triangles are not among its indices");
    }
    if (!r.fits(vertices, sizeof(Vertex))) return bad("it ends in its vertices");
    model.vertices.resize(vertices);
    r.take(model.vertices.data(), std::size_t{vertices} * sizeof(Vertex));
    for (const auto& v : model.vertices) {
        if (!finite(v.position, 12)) return bad("a vertex is not a number somewhere");
        for (int i = 0; i < 4; ++i)
            if (v.weights[i] && v.joints[i] >= bones) return bad("a vertex is weighted to a bone the model does not have");
    }
    if (!r.fits(indices, 4)) return bad("it ends in its indices");
    model.indices.resize(indices);
    r.take(model.indices.data(), std::size_t{indices} * 4);
    for (const auto index : model.indices)
        if (index >= vertices) return bad("a triangle uses a vertex the model does not have");
    model.clips.resize(clips);
    const std::size_t per_frame = std::size_t{bones} * 12;
    for (auto& clip : model.clips) {
        clip.name = r.text();
        clip.fps = r.value<float>();
        clip.frames = r.value<std::uint32_t>();
        if (!(clip.fps >= 1 && clip.fps <= 1000)) return bad("clip \"" + clip.name + "\" has a frame rate of 1 to 1000 a second");
        if (clip.frames == 0) return bad("clip \"" + clip.name + "\" has no frames");
        if (!r.fits(clip.frames, per_frame * 4)) return bad("it ends in clip \"" + clip.name + "\"");
        clip.skin.resize(clip.frames * per_frame);
        r.take(clip.skin.data(), clip.skin.size() * 4);
        if (!finite(clip.skin.data(), clip.skin.size())) return bad("clip \"" + clip.name + "\" is not a number somewhere");
    }
    if (r.bad) return bad("it ends early");
    if (r.at != bytes.size()) return bad("it has more after its last clip than a viewmodel holds");
    return model;
}

Affine multiply(const Affine& a, const Affine& b) noexcept {
    Affine out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c];
        out[r * 4 + 3] += a[r * 4 + 3];
    }
    return out;
}

Affine blend(const Affine& a, const Affine& b, float t) noexcept {
    Affine out;
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    return out;
}

Affine placement(const std::array<float, 3>& offset, float yaw, float pitch, float roll) noexcept {
    constexpr float degree = std::numbers::pi_v<float> / 180.0f;
    const float cy = std::cos(yaw * degree), sy = std::sin(yaw * degree);
    const float cp = std::cos(pitch * degree), sp = std::sin(pitch * degree);
    const float cr = std::cos(roll * degree), sr = std::sin(roll * degree);
    // Yaw about y (+ turns the front, +z, toward +x), pitch about x (+ tips the front up toward +y), roll
    // about z; applied roll, then pitch, then yaw.
    const Affine turn_yaw{cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0};
    const Affine turn_pitch{1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0};
    const Affine turn_roll{cr, -sr, 0, 0, sr, cr, 0, 0, 0, 0, 1, 0};
    auto out = multiply(turn_yaw, multiply(turn_pitch, turn_roll));
    out[3] = offset[0];
    out[7] = offset[1];
    out[11] = offset[2];
    return out;
}

void sample(const Model& model, const Clip& clip, double seconds, bool loop, std::span<Affine> out) {
    const std::size_t bones = std::min(out.size(), model.bones.size());
    const std::size_t per_frame = model.bones.size() * 12;
    double frame = std::max(0.0, seconds) * clip.fps;
    const double last = static_cast<double>(clip.frames - 1);
    if (clip.frames > 1 && loop) frame = std::fmod(frame, last); // the last frame is the first again
    frame = std::min(frame, last);
    const auto a = static_cast<std::size_t>(frame);
    const std::size_t b = std::min<std::size_t>(a + 1, clip.frames - 1);
    const float t = static_cast<float>(frame - static_cast<double>(a));
    for (std::size_t i = 0; i < bones; ++i) {
        Affine first, second;
        std::memcpy(first.data(), clip.skin.data() + a * per_frame + i * 12, sizeof first);
        std::memcpy(second.data(), clip.skin.data() + b * per_frame + i * 12, sizeof second);
        out[i] = blend(first, second, t);
    }
}

void Animator::play(const Model& model, const Play& play, double now) {
    if (play.clip < 0 || play.clip >= static_cast<int>(model.clips.size())) return;
    // The clip playing now fades out (from where it is, not from the start of its own fade).
    if (current_.play.clip >= 0) {
        previous_ = current_;
        faded_at_ = now;
    } else previous_ = {};
    current_ = {play, now};
    current_.play.speed = std::clamp(play.speed, 0.0f, 10.0f);
    current_.play.fade = std::clamp(play.fade, 0.0, 2.0);
}

double Animator::time_of(const Model& model, const Running& running, double now) noexcept {
    const double time = std::max(0.0, (now - running.started) * running.play.speed);
    const double length = model.clips[static_cast<std::size_t>(running.play.clip)].duration();
    if (running.play.loop) return length > 0 ? std::fmod(time, length) : 0.0;
    return std::min(time, length);
}

Animator::State Animator::state(const Model& model, double now) const noexcept {
    if (current_.play.clip < 0) return {};
    const double length = model.clips[static_cast<std::size_t>(current_.play.clip)].duration();
    const double time = time_of(model, current_, now);
    const bool done = !current_.play.loop && (now - current_.started) * current_.play.speed >= length;
    return {current_.play.clip, time, done};
}

void Animator::pose(const Model& model, double now, std::span<Affine> out) const {
    const std::size_t bones = std::min(out.size(), model.bones.size());
    if (current_.play.clip < 0) {
        std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(bones), identity);
        return;
    }
    const auto& clip = model.clips[static_cast<std::size_t>(current_.play.clip)];
    sample(model, clip, time_of(model, current_, now), false, out);
    const double fade = current_.play.fade;
    if (previous_.play.clip < 0 || fade <= 0 || now - faded_at_ >= fade) return;
    std::array<Affine, max_bones> before;
    const auto& old = model.clips[static_cast<std::size_t>(previous_.play.clip)];
    sample(model, old, time_of(model, previous_, now), false, std::span(before).first(bones));
    const float t = static_cast<float>(std::clamp((now - faded_at_) / fade, 0.0, 1.0));
    for (std::size_t i = 0; i < bones; ++i) out[i] = blend(before[i], out[i], t);
}
} // namespace dingosdk::viewmodel
