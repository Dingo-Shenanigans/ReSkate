#include "hom_rig.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace dingosdk::skate3_hom {
namespace {
using client_source::detail::first_person_read;
void say(const std::string &text) { logging::log(logging::Level::info, logging::Channel::assets, "HoM rig: {}", text); }

// A table of `count` parent indices (signed 16 or 32 bit): parent[0] is -1 or 0 and every
// other entry names an earlier joint.
bool looks_like_parents(const std::vector<std::int32_t> &p) {
    if (p.empty() || (p[0] != -1 && p[0] != 0 && p[0] != 0xFFFF)) return false;
    std::size_t earlier = 0;
    for (std::size_t i = 1; i < p.size(); ++i)
        if (p[i] >= 0 && static_cast<std::size_t>(p[i]) < i) ++earlier;
    return earlier + 4 >= p.size() - 1;
}

// `hom phys`: the skater's physics bodies (the ragdoll the game draws), raw, next to the animation
// skeleton's joints, to map one onto the other.
void phys_probe(std::uintptr_t base, std::uintptr_t client) {
    overlay::DebugModel skater;
    (void)client_source::detail::debug_skater(base, client, skater);
    const auto bodies = client_source::detail::debug_noclip_bodies(base, client, skater.skater_identity);
    say(std::format("phys: offboard {}, root ({:.3f},{:.3f},{:.3f})", bodies.offboard, bodies.root[0], bodies.root[1], bodies.root[2]));
    for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
        std::array<float, 76> f{};
        if (!first_person_read(bodies.parts[i], f.data(), sizeof(f))) continue;
        std::string line = std::format("body {:2} @{:#x}:", i - 8, bodies.parts[i]);
        for (std::size_t k = 0; k < f.size(); ++k) {
            if (!std::isfinite(f[k]) || std::abs(f[k]) > 1e7f || (std::abs(f[k]) < 1e-12f && f[k] != 0)) line += " *";
            else line += std::format(" {:.3f}", f[k]);
        }
        say(line);
    }
    const auto rig = latest_rig(std::chrono::milliseconds(1000));
    for (std::size_t i = 0; i < rig_bones && rig.valid; ++i)
        say(std::format("xray {:2}: a ({:.3f},{:.3f},{:.3f}) b ({:.3f},{:.3f},{:.3f})", i, rig.bones[i].a[0], rig.bones[i].a[1], rig.bones[i].a[2],
                        rig.bones[i].b[0], rig.bones[i].b[1], rig.bones[i].b[2]));
}

void probe(std::uintptr_t base, std::uintptr_t client) {
    const auto component = client_source::detail::first_person_component(base, client);
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder) return say("no animation holder");
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    if (!pose.buffer) return say("no pose buffer");
    say(std::format("{} joints, buffer {:#x}", pose.count, pose.buffer));

    // The pose, parent-local: scale.xyz, rotation.xyzw, position.xyz per joint.
    for (std::uint32_t first = 0; first < pose.count; first += 6) {
        std::string line;
        for (std::uint32_t i = first; i < std::min<std::uint32_t>(first + 6, pose.count); ++i) {
            std::array<float, 12> b{};
            if (!first_person_read(pose.buffer + i * 0x30ULL, b.data(), sizeof(b))) continue;
            line += std::format("[{}] p({:.3f},{:.3f},{:.3f}) s{:.2f}  ", i, b[8], b[9], b[10], b[0]);
        }
        say(line);
    }

    // Look for the skeleton's parent table among the resource's pointers.
    std::uintptr_t rig{}, definition{}, resource{};
    if (!first_person_read(holder + 0x78, &rig, 8) || !rig || !first_person_read(rig + 0x18, &definition, 8) || !definition ||
        !first_person_read(definition + 0x1a0, &resource, 8) || !resource)
        return say("no skeleton resource");
    say(std::format("resource {:#x}", resource));
    std::array<std::uint64_t, 32> head{};
    first_person_read(resource, head.data(), sizeof(head));
    std::string words;
    for (std::size_t i = 0; i < head.size(); ++i) words += std::format("{:#x} ", head[i]);
    say("resource head: " + words);
    for (std::size_t slot = 0; slot < head.size(); ++slot) {
        const auto pointer = static_cast<std::uintptr_t>(head[slot]);
        if (pointer < 0x10000 || pointer > 0x00007fffffffffffULL) continue;
        for (const std::size_t width : {2u, 4u}) {
            std::vector<unsigned char> raw(pose.count * width);
            if (!first_person_read(pointer, raw.data(), raw.size())) continue;
            std::vector<std::int32_t> values(pose.count);
            for (std::uint32_t i = 0; i < pose.count; ++i) {
                if (width == 2) {
                    std::int16_t v{};
                    std::memcpy(&v, raw.data() + i * 2, 2);
                    values[i] = v;
                } else {
                    std::memcpy(&values[i], raw.data() + i * 4, 4);
                }
            }
            if (!looks_like_parents(values)) continue;
            say(std::format("parent table candidate: resource+{:#x} -> {:#x}, {}-bit", slot * 8, pointer, width * 8));
            for (std::uint32_t first = 0; first < pose.count; first += 24) {
                std::string line;
                for (std::uint32_t i = first; i < std::min<std::uint32_t>(first + 24, pose.count); ++i) line += std::format("{} ", values[i]);
                say(std::format("parents[{}..]: {}", first, line));
            }
        }
    }
    say("done");
}

// The pose is parent-local {scale, Hamilton quaternion, translation} per joint; joint 1 carries
// the world placement, so composing from the root lands in world space (client_first_person.cpp).
// The 395-joint AnimBase skeleton, mapped from a live dump (hom rig, 2026-10-08): the spine runs
// 7 (pelvis) 42 43 44 45 (chest) 101 102 (neck) 103 (head) along local +X; legs hang off the
// pelvis (8-11 and 341-344: hip, knee, ankle, ball) and arms off the chest (46-49 and 275-278:
// clavicle, shoulder, elbow, wrist). Local -Z is the skater's right, so 8.. and 46.. are right.
struct Joint {
    std::array<float, 4> q{0, 0, 0, 1};
    std::array<float, 3> p{};
    float s = 1;
};
std::array<float, 3> rotate(const std::array<float, 4> &q, const std::array<float, 3> &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz), v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
bool child(const Joint &parent, std::uintptr_t buffer, std::uint16_t index, Joint &out) {
    std::array<float, 12> bone{};
    if (!first_person_read(buffer + index * 0x30ULL, bone.data(), sizeof(bone))) return false;
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(bone[i]) || std::abs(bone[i]) > 1000000.0f) return false;
    const auto &q = parent.q;
    const auto offset = rotate(q, {bone[8] * parent.s, bone[9] * parent.s, bone[10] * parent.s});
    for (std::size_t i = 0; i < 3; ++i) out.p[i] = parent.p[i] + offset[i];
    out.q = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5], q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
             q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7], q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    out.s = parent.s * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return std::isfinite(out.s) && out.s > 0.01f;
}
constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 26> chains{{
    {1, 0}, {7, 1}, {42, 7}, {43, 42}, {44, 43}, {45, 44}, {101, 45}, {102, 101}, {103, 102},
    {8, 7}, {9, 8}, {10, 9}, {11, 10}, {341, 7}, {342, 341}, {343, 342}, {344, 343},
    {46, 45}, {47, 46}, {48, 47}, {49, 48}, {275, 45}, {276, 275}, {277, 276}, {278, 277}, {0, 0}}};
std::array<float, 3> along(const std::array<float, 3> &from, const std::array<float, 3> &to, float metres) {
    std::array<float, 3> d{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-5f) return to;
    return {to[0] + d[0] / l * metres, to[1] + d[1] / l * metres, to[2] + d[2] / l * metres};
}
std::array<float, 3> lerp(const std::array<float, 3> &a, const std::array<float, 3> &b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// ---- Roll from the joints --------------------------------------------------------------------
// A bone mesh needs to know which way it faces around its own length. Joint-to-joint positions give
// the length direction only, so the roll used to come from the body's forward; that goes wrong once
// the body tumbles. Each bone instead follows its start joint's rotation R. While the skater is
// upright, K = R^T F is measured against the frame F built from the body forward (right then), and
// afterwards F = R K carries that same roll through any pose.
using Vec3 = std::array<float, 3>;
using Mat3 = std::array<Vec3, 3>; // three columns
Vec3 cross3(const Vec3 &a, const Vec3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot3(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 norm3(const Vec3 &a) {
    const float l = std::sqrt(dot3(a, a));
    return l > 1e-6f ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0, 0, 0};
}
Mat3 rotation_columns(const std::array<float, 4> &q) {
    return {rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, 1})};
}
// K = R^T F: entry (row i, column k) = dot(R column i, F column k).
Mat3 relative(const Mat3 &r, const Mat3 &f) {
    Mat3 k{};
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) k[col][row] = dot3(r[row], f[col]);
    return k;
}
Vec3 mat_vec(const Mat3 &r, const Vec3 &v) { // R v
    return {r[0][0] * v[0] + r[1][0] * v[1] + r[2][0] * v[2], r[0][1] * v[0] + r[1][1] * v[1] + r[2][1] * v[2],
            r[0][2] * v[0] + r[1][2] * v[1] + r[2][2] * v[2]};
}
void orthonormalise(Mat3 &m) {
    m[0] = norm3(m[0]);
    Vec3 y = m[1];
    const float d = dot3(y, m[0]);
    y = {y[0] - m[0][0] * d, y[1] - m[0][1] * d, y[2] - m[0][2] * d};
    m[1] = norm3(y);
    m[2] = cross3(m[0], m[1]);
}
constexpr std::array<std::uint16_t, rig_bones> reference_joint{103, 101, 44, 42, 7, 276, 47, 277, 48, 278, 49, 341, 8, 342, 9, 343, 10, 344, 11};
struct RollCalibration {
    std::array<Mat3, rig_bones> k{};
    std::array<bool, rig_bones> set{};
};
RollCalibration &roll_calibration() {
    static RollCalibration value;
    return value;
}

// The ragdoll's own physics bodies. Each sits on one joint of the animation skeleton (mapped from
// a standing skater, 2026-10-08: within 1 cm), and they are what the game draws: during a fall the
// animation pose trails them by about a frame (up to 0.2 m at speed), at rest they agree. So a
// joint is placed from its body when the bodies can be read.
constexpr std::array<std::uint16_t, 24> body_joint{0,   103, 101, 278, 277, 276, 275, 49, 48, 47, 46, 45,
                                                   44,  43,  42,  344, 343, 342, 341, 11, 10, 9,  8,  7};
bool read_body_positions(std::uintptr_t base, std::uintptr_t client, std::array<std::array<float, 3>, 24> &out) {
    overlay::DebugModel skater;
    (void)client_source::detail::debug_skater(base, client, skater);
    const auto bodies = client_source::detail::debug_noclip_bodies(base, client, skater.skater_identity);
    for (std::size_t k = 1; k < 24; ++k) {
        std::array<float, 3> position{};
        if (!first_person_read(bodies.parts[k + 8] + 20 * sizeof(float), position.data(), sizeof(position))) return false;
        for (const float v : position)
            if (!std::isfinite(v) || std::abs(v) > 1000000.0f) return false;
        out[k] = position;
    }
    return true;
}

void publish_pose(std::uintptr_t base, std::uintptr_t holder, std::uintptr_t client) {
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    if (!pose.buffer || pose.count != 395) return;
    std::array<Joint, 395> j{};
    std::array<bool, 395> have{};
    have[0] = true;
    if (!child(Joint{}, pose.buffer, 0, j[0])) return;
    for (std::size_t i = 0; i + 1 < chains.size(); ++i) {
        const auto [index, parent] = chains[i];
        if (!have[parent] || !child(j[parent], pose.buffer, index, j[index])) return;
        have[index] = true;
    }
    // Where the physics bodies are, they win; the head-top and limb tips follow from them.
    std::array<std::array<float, 3>, 395> placed{};
    for (std::size_t i = 0; i < 395; ++i) placed[i] = j[i].p;
    {
        std::array<std::array<float, 3>, 24> bodies{};
        bool ok = false;
        try {
            ok = client && read_body_positions(base, client, bodies);
        } catch (...) {
            ok = false;
        }
        if (ok) {
            // Sanity: every body must be near its joint (a different rig, a respawn): else keep the pose.
            for (std::size_t k = 1; k < 24 && ok; ++k) {
                const auto &a = bodies[k], &b = j[body_joint[k]].p;
                const float gap = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
                if (gap > 1.5f) ok = false;
            }
        }
        if (ok)
            for (std::size_t k = 1; k < 24; ++k) placed[body_joint[k]] = bodies[k];
        rig_from_bodies().store(ok);
    }
    const auto P = [&](std::uint16_t i) { return placed[i]; };
    RigView view;
    const auto head_top = [&] {
        const auto up = rotate(j[103].q, {0.17f * j[103].s, 0, 0});
        return std::array<float, 3>{P(103)[0] + up[0], P(103)[1] + up[1], P(103)[2] + up[2]};
    }();
    // Order: Skull, Neck, Rib Cage, Lower Spine, Hips, Bicep L/R, Forearm L/R, Hand L/R, Thigh L/R,
    // Calf L/R, Ankle L/R, Toes L/R (left first, as in bones()).
    view.bones = {{
        {P(103), head_top, 0.085f},
        {P(101), P(103), 0.035f},
        {P(44), P(101), 0.12f},
        {P(7), P(43), 0.05f},
        {P(341), P(8), 0.06f},
        {P(276), P(277), 0.035f}, {P(47), P(48), 0.035f},
        {P(277), P(278), 0.03f}, {P(48), P(49), 0.03f},
        {P(278), along(P(277), P(278), 0.09f), 0.03f}, {P(49), along(P(48), P(49), 0.09f), 0.03f},
        {P(341), P(342), 0.05f}, {P(8), P(9), 0.05f},
        {P(342), P(343), 0.04f}, {P(9), P(10), 0.04f},
        {P(343), lerp(P(343), P(344), 0.5f), 0.035f}, {P(10), lerp(P(10), P(11), 0.5f), 0.035f},
        {P(344), along(P(343), P(344), 0.07f), 0.025f}, {P(11), along(P(10), P(11), 0.07f), 0.025f},
    }};
    view.valid = true;
    {
        // Body forward from the skeleton's own shape: up the spine crossed with the hips' left-to-right,
        // turned to agree with the face (head local +Y looks out of the face).
        const auto sub3 = [](const std::array<float, 3> &a, const std::array<float, 3> &b) { return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
        const auto up = sub3(P(45), P(7));
        const auto right = sub3(P(8), P(341));
        std::array<float, 3> f{up[1] * right[2] - up[2] * right[1], up[2] * right[0] - up[0] * right[2], up[0] * right[1] - up[1] * right[0]};
        const auto face = rotate(j[103].q, {0, 1, 0});
        if (f[0] * face[0] + f[1] * face[1] + f[2] * face[2] < 0) f = {-f[0], -f[1], -f[2]};
        const float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        view.forward = l > 1e-4f ? std::array<float, 3>{f[0] / l, f[1] / l, f[2] / l} : face;
    }
    // Upright: the head at least half a metre above the hips (standing or walking off).
    const bool upright = P(103)[1] - P(7)[1] > 0.5f;
    rig_upright().store(upright);
    rig_lying().store(P(103)[1] - P(7)[1] < 0.3f);
    {
        auto &cal = roll_calibration();
        for (std::size_t i = 0; i < rig_bones; ++i) {
            auto &seg = view.bones[i];
            const Vec3 axis = norm3({seg.b[0] - seg.a[0], seg.b[1] - seg.a[1], seg.b[2] - seg.a[2]});
            const auto &joint = j[reference_joint[i]];
            const float qn = std::sqrt(joint.q[0] * joint.q[0] + joint.q[1] * joint.q[1] + joint.q[2] * joint.q[2] + joint.q[3] * joint.q[3]);
            if (dot3(axis, axis) < 0.5f || qn < 0.5f) continue;
            const std::array<float, 4> q{joint.q[0] / qn, joint.q[1] / qn, joint.q[2] / qn, joint.q[3] / qn};
            const Mat3 r = rotation_columns(q);
            if (upright) {
                // The frame this bone has right now, from the body forward.
                const float along = dot3(view.forward, axis);
                Vec3 front = {view.forward[0] - axis[0] * along, view.forward[1] - axis[1] * along, view.forward[2] - axis[2] * along};
                front = norm3(front);
                if (dot3(front, front) > 0.5f) {
                    const Mat3 f{axis, front, cross3(axis, front)};
                    Mat3 k = relative(r, f);
                    if (cal.set[i]) {
                        for (int c = 0; c < 3; ++c)
                            for (int e = 0; e < 3; ++e) k[c][e] = cal.k[i][c][e] * 0.92f + k[c][e] * 0.08f;
                    }
                    orthonormalise(k);
                    cal.k[i] = k;
                    cal.set[i] = true;
                }
            }
            if (cal.set[i]) {
                // Column 1 of R K is the bone's front; keep only its part across the bone.
                const Vec3 front = mat_vec(r, cal.k[i][1]);
                const float along = dot3(front, axis);
                const Vec3 across = norm3({front[0] - axis[0] * along, front[1] - axis[1] * along, front[2] - axis[2] * along});
                if (dot3(across, across) > 0.5f) {
                    seg.front = across;
                    seg.has_front = true;
                }
            }
        }
    }
    view.base = base;
    view.at = std::chrono::steady_clock::now();
    publish_rig(view);
}

// The game's UI draw flag (what ReSkate's own `hideui` toggles): cleared while a bail is on
// screen, put back afterwards unless someone else changed it meanwhile.
struct UiHide {
    bool applied{};
    std::uintptr_t object{};
};
bool write_byte(std::uintptr_t address, std::uint8_t value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t *>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void apply_ui_hide(std::uintptr_t base) noexcept {
    static UiHide hide;
    const bool want = hide_game_ui().load();
    namespace spawn = addr::client_source_spawn;
    std::uintptr_t object{}, vtable{}, type{};
    if (!first_person_read(base + spawn::ui_settings, &object, 8) || !object || !first_person_read(object, &vtable, 8) ||
        vtable != base + spawn::ui_settings_vtable || !first_person_read(object + 8, &type, 8) || type != base + spawn::ui_settings_type)
        return;
    std::uint8_t draw{};
    if (!first_person_read(object + 0x4e, &draw, 1) || draw > 1) return;
    if (want && !hide.applied && draw == 1) {
        if (write_byte(object + 0x4e, 0)) hide = {true, object};
    } else if (!want && hide.applied) {
        if (hide.object == object && draw == 0) (void)write_byte(object + 0x4e, 1);
        hide = {};
    }
}
} // namespace

void rig_tick(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        const auto component = client_source::detail::first_person_component(base, client);
        std::uintptr_t holder{};
        if (first_person_read(component + 0xa0, &holder, 8) && holder) {
            publish_pose(base, holder, client);
        }
    } catch (...) {
    }
    apply_ui_hide(base);
    if (phys_probe_requested().exchange(false)) {
        try {
            phys_probe(base, client);
            probe(base, client);
        } catch (const std::exception &error) {
            say(std::string("phys failed: ") + error.what());
        } catch (...) {
        }
    }
    if (!rig_probe_requested().exchange(false)) return;
    try {
        probe(base, client);
    } catch (const std::exception &error) {
        say(std::string("failed: ") + error.what());
    } catch (...) {
        say("failed");
    }
}
} // namespace dingosdk::skate3_hom
