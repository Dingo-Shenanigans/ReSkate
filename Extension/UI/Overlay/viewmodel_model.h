#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A viewmodel (reskate.viewmodel): a rigged, animated model drawn in front of the camera, as a shooter
// draws the player's arms and gun. contrib/viewmodel_export.py makes the file with Blender. Everything
// in it is already in the view's space: x right, y up, z forward, metres from the eye. Each action's
// frames hold every bone's skin matrix (bone pose times inverse bind), so drawing one is sampling two
// frames and blending; no rig maths happens here.
//
// The .rsvm file, little-endian (a "short string" is a u8 length then that many bytes of UTF-8):
//   "RSVM", u32 version (1)
//   u32 bones, materials, parts, vertices, indices, clips
//   bones:     short string name, i16 parent (-1 none), f32[12] bind (the bone at rest, 3 rows of 4)
//   materials: short string name, albedo, normal, surface (pictures beside the file, "" none),
//              f32 roughness, f32 specular (used where there is no surface picture)
//   parts:     u32 material, u32 first index, u32 index count
//   vertices:  f32 position[3], normal[3], tangent[4] (w the bitangent's sign), uv[2]; u8 joints[4], weights[4]
//   indices:   u32 each, three a triangle
//   clips:     short string name, f32 frames a second, u32 frames, f32[frames][bones][12] skin matrices
namespace dingosdk::viewmodel {
// Three rows of a 4x4 whose last row is 0 0 0 1: out = m * (x, y, z, 1).
using Affine = std::array<float, 12>;
inline constexpr Affine identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
inline constexpr std::size_t max_bones = 128;

struct Vertex {
    float position[3], normal[3], tangent[4], uv[2];
    std::uint8_t joints[4], weights[4];
};
static_assert(sizeof(Vertex) == 56);

struct Bone {
    std::string name;
    int parent{-1};
    Affine bind{identity};
};
struct Material {
    std::string name, albedo, normal, surface;
    float roughness{0.5f}, specular{0.5f};
};
struct Part {
    std::uint32_t material{}, first{}, count{};
};
struct Clip {
    std::string name;
    float fps{60};
    std::uint32_t frames{};
    std::vector<float> skin; // frames * bones * 12
    double duration() const noexcept { return frames > 1 ? (frames - 1) / static_cast<double>(fps) : 0.0; }
};

struct Model {
    std::vector<Bone> bones;
    std::vector<Material> materials;
    std::vector<Part> parts;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Clip> clips;
    int bone(std::string_view name) const noexcept;
    int clip(std::string_view name) const noexcept;
};

// The model in `bytes`, or nothing with `why` saying what is wrong with the file. Every index is
// checked against what it indexes, so a model that parses can be drawn without further checks.
std::optional<Model> parse(std::string_view bytes, std::string& why);

Affine multiply(const Affine& a, const Affine& b) noexcept;
// a and b weighted: b's share is `t`. Enough for the short fades between clips.
Affine blend(const Affine& a, const Affine& b, float t) noexcept;
// The 3x4 of a placement: moved by `offset` (metres) after turning by yaw (about y), pitch (about x)
// and roll (about z), in degrees; + pitch tips the model's front up, + yaw turns it right.
Affine placement(const std::array<float, 3>& offset, float yaw, float pitch, float roll) noexcept;

// Every bone's skin matrix `seconds` into a clip; looped, or held on its last frame.
void sample(const Model& model, const Clip& clip, double seconds, bool loop, std::span<Affine> out);

// What plays: a clip started at a time, faded in over the one before it.
class Animator {
public:
    struct Play {
        int clip{-1};
        bool loop{true};
        float speed{1};
        double fade{0.1};  // seconds the clip before it is faded out over
    };
    // From `now` (seconds, any clock the caller keeps to).
    void play(const Model& model, const Play& play, double now);
    void stop() noexcept { current_ = {}; previous_ = {}; }
    struct State {
        int clip{-1};
        double time{};  // seconds into it
        bool done{};    // a clip that does not loop has reached its end
    };
    State state(const Model& model, double now) const noexcept;
    // Every bone's skin matrix at `now`; the rest pose with nothing playing.
    void pose(const Model& model, double now, std::span<Affine> out) const;

private:
    struct Running {
        Play play;
        double started{};
    };
    static double time_of(const Model& model, const Running& running, double now) noexcept;
    Running current_, previous_;
    double faded_at_{};
};
} // namespace dingosdk::viewmodel
