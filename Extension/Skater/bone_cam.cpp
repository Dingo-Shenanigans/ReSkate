#include "bone_cam.h"
#include "bone_sprites.h"
#include "skater_skeleton.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/UI/game_view.h"
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <limits>
#include <mutex>
#include <set>
#include <span>

namespace dingosdk::bonecam {
namespace {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
constexpr std::uint64_t duration_ms = 4200, fade_in_ms = 150, fade_out_ms = 600, settle_ms = 150;
// After-effects (a concussion's) last up to half a minute, a hit flashes briefly.
constexpr std::uint64_t mark_ms = 30000, hit_ms = 450;
constexpr std::uint16_t head_up = 0xffff; // the skull points along the head's own up axis

// The animation pose is parent-local {scale, Hamilton quaternion, translation} per joint, and the
// root chain carries the skater's world placement (client_first_person.cpp), so composing from
// the root lands in world space.
struct Joint {
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
    float scale = 1;
};
Vec3 rotate(const Quat &q, const Vec3 &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
            v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
Joint child(const Joint &parent, const std::array<float, 12> &bone) {
    const auto &q = parent.rotation;
    const auto offset = rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    Joint result;
    for (std::size_t i = 0; i < 3; ++i) result.position[i] = parent.position[i] + offset[i];
    result.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
                       q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
                       q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
                       q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    result.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return result;
}

// Each bone sprite between two joints.
struct Span {
    BoneSprite sprite;
    std::uint16_t a, b;
};
constexpr Span spans[]{
    {BoneSprite::torso, joint::hips, joint::head}, // the torso art runs up the neck to the skull
    {BoneSprite::femur_r, joint::right_up_leg, joint::right_leg}, {BoneSprite::femur_l, joint::left_up_leg, joint::left_leg},
    {BoneSprite::shin_r, joint::right_leg, joint::right_foot},    {BoneSprite::shin_l, joint::left_leg, joint::left_foot},
    {BoneSprite::foot_r, joint::right_foot, joint::right_toe},    {BoneSprite::foot_l, joint::left_foot, joint::left_toe},
    {BoneSprite::humerus_r, joint::right_arm, joint::right_fore_arm}, {BoneSprite::humerus_l, joint::left_arm, joint::left_fore_arm},
    {BoneSprite::forearm_r, joint::right_fore_arm, joint::right_hand}, {BoneSprite::forearm_l, joint::left_fore_arm, joint::left_hand},
    {BoneSprite::hand_r, joint::right_hand, joint::right_middle}, {BoneSprite::hand_l, joint::left_hand, joint::left_middle},
    {BoneSprite::skull, joint::head, head_up},
};
// A joint that stops dead hurts the bone beside it: cracked (or sprained, torn...) past `crack`
// m/s of speed lost into what it hit, broken past `snap`. Each bone has its own limits, roughly as
// bodies go: ankles, wrists and collarbones give first; the femur and pelvis take the most. A drop
// of about 2.5 m (7 m/s) onto one part is where it starts to hurt; a normal jump hurts nothing.
struct Watch {
    std::uint16_t joint;
    BoneSprite sprite;
    const char *cracked_bone, *cracked, *broken_bone, *broken;
    float crack, snap;
};
constexpr Watch watches[]{
    {joint::head, BoneSprite::skull, "SKULL", "CONCUSSION", "SKULL", "FRACTURED", 5.5f, 9.5f},
    {joint::right_hand, BoneSprite::forearm_r, "RIGHT WRIST", "SPRAINED", "RIGHT RADIUS", "SNAPPED", 6.5f, 9.5f},
    {joint::left_hand, BoneSprite::forearm_l, "LEFT WRIST", "SPRAINED", "LEFT RADIUS", "SNAPPED", 6.5f, 9.5f},
    {joint::right_fore_arm, BoneSprite::humerus_r, "RIGHT ELBOW", "DISLOCATED", "RIGHT HUMERUS", "FRACTURED", 7.5f, 11.0f},
    {joint::left_fore_arm, BoneSprite::humerus_l, "LEFT ELBOW", "DISLOCATED", "LEFT HUMERUS", "FRACTURED", 7.5f, 11.0f},
    {joint::right_arm, BoneSprite::torso, "RIGHT COLLARBONE", "CRACKED", "RIGHT COLLARBONE", "BROKEN", 7.0f, 10.0f},
    {joint::left_arm, BoneSprite::torso, "LEFT COLLARBONE", "CRACKED", "LEFT COLLARBONE", "BROKEN", 7.0f, 10.0f},
    {joint::spine2, BoneSprite::torso, "RIBS", "CRACKED", "RIBS", "BROKEN", 8.0f, 11.5f},
    {joint::hips, BoneSprite::torso, "PELVIS", "BRUISED", "PELVIS", "FRACTURED", 9.0f, 13.0f},
    {joint::right_leg, BoneSprite::femur_r, "RIGHT KNEE", "TORN", "RIGHT FEMUR", "FRACTURED", 8.5f, 12.5f},
    {joint::left_leg, BoneSprite::femur_l, "LEFT KNEE", "TORN", "LEFT FEMUR", "FRACTURED", 8.5f, 12.5f},
    {joint::right_foot, BoneSprite::shin_r, "RIGHT ANKLE", "SPRAINED", "RIGHT TIBIA", "SNAPPED", 7.0f, 10.0f},
    {joint::left_foot, BoneSprite::shin_l, "LEFT ANKLE", "SPRAINED", "LEFT TIBIA", "SNAPPED", 7.0f, 10.0f},
};
// A concussion's grade from the hardest hit the head took (m/s lost), and how long its after-
// effects last. Below the first, no concussion.
struct Concussion {
    float from;
    const char *what;
    std::uint64_t lasts_ms;
};
constexpr Concussion concussions[]{
    {5.5f, "MILD CONCUSSION", 4000}, {7.0f, "CONCUSSION", 7000}, {8.5f, "SEVERE CONCUSSION", 10000}, {10.0f, "KNOCKED OUT", 12000}};
constexpr std::size_t watch_count = std::size(watches);

enum class Setting { game, on, off };
struct State {
    std::atomic<std::uintptr_t> pose_base{},pose_client{};
    std::atomic<std::uint64_t> pose_context_at{};
    Setting setting = Setting::game;
    bool effects = true;
    bool ringing = true; // a concussion's ringing in the ears (it can be muted)
    std::uint64_t effects_until{};
    // A bail heard while still in the air: measured from now on, shown once the body lands.
    bool armed{};
    std::uint64_t armed_at{};
    float hips_vy{}; // the hips' vertical speed (m/s, up positive) at the last tick
    float fell{};    // the fastest the hips fell since the bail began (negative)
    float hit{};
    std::uint64_t hit_time{};
    Vec3 hit_at{};
    float head_hit{}; // the hardest hit the head took this bail (m/s lost): its concussion
    bool rang{};      // the concussion's ringing played this bail
    bool active{}, preview{}, wipeouts_known{}, have_position{}, have_velocity{}, pose_reported{}, posed{};
    std::uint64_t started{}, serial{}, wipeouts{}, last_tick{}, unready_since{};
    std::array<Vec3, watch_count> position{}, velocity{};
    // The speed each joint carried lately (decaying over a third of a second): into the ground,
    // and along it. A bail is often noticed only once the body has already hit, so the hit is
    // measured against what the joint carried before, not just the last tick.
    std::array<float, watch_count> carry_down{}, carry_side{};
    // The hips' recent speeds, to notice the body stopping dead (the ground, a pole, a wall).
    std::array<std::pair<std::uint64_t, float>, 24> hips_speeds{};
    std::size_t hips_next{};
    std::atomic<std::uint64_t> slams{};
    std::uint64_t slam_time{};
    // The head, chest, shoulders or hips striking an object at speed (bone_cam_struck), and when
    // any part last did.
    std::atomic<std::uint64_t> strikes{};
    std::uint64_t struck_time{};
    std::array<std::uint8_t, watch_count> level{};
    std::array<std::uint8_t, static_cast<std::size_t>(BoneSprite::count)> hurt{};
    std::vector<Joint> joints = std::vector<Joint>(skater_joint_count);
    std::vector<std::array<float, 12>> raw = std::vector<std::array<float, 12>>(skater_joint_count);
    std::atomic<bool> bail_pending{};
    std::mutex mutex;
    BoneCam shown;
};
State &state() {
    static auto *value = new State;
    return *value;
}
const Concussion *concussion_of(float head_hit) {
    const Concussion *found{};
    for (const auto &c : concussions)
        if (head_hit >= c.from) found = &c;
    return found;
}
// A concussion's ears ringing: one high tone, very quiet, fading over a few seconds.
void ring(float strength) {
    static std::vector<std::uint8_t> wav;
    constexpr std::uint32_t rate = 22050;
    const float seconds = 1.5f + 2.0f * strength, volume = 0.02f + 0.025f * strength;
    const auto count = static_cast<std::uint32_t>(seconds * rate), bytes = count * 2;
    PlaySoundW(nullptr, nullptr, 0); // the buffer below may be playing: stop it before it changes
    wav.assign(44 + bytes, 0);
    const auto put = [&](std::size_t at, std::uint32_t value, int size) {
        for (int i = 0; i < size; ++i) wav[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put(4, 36 + bytes, 4);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4);       // format block size
    put(20, 1, 2);        // PCM
    put(22, 1, 2);        // mono
    put(24, rate, 4);
    put(28, rate * 2, 4); // bytes per second
    put(32, 2, 2);
    put(34, 16, 2);
    std::memcpy(wav.data() + 36, "data", 4);
    put(40, bytes, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / rate;
        const float envelope = std::min(1.0f, t / 0.4f) * std::max(0.0f, 1.0f - t / seconds);
        const float tone = std::sin(6.2831853f * 3800.0f * t) * (0.85f + 0.15f * std::sin(6.2831853f * 3.0f * t));
        put(44 + i * 2, static_cast<std::uint16_t>(static_cast<std::int16_t>(tone * envelope * volume * 32767)), 2);
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
std::uint64_t now_ms() { return GetTickCount64(); }


// A bail begins: its injuries start from nothing, and from now on hits count (they need the body
// to have landed, so nothing hurts while it is still in the air).
void begin_bail(State &s, std::uint64_t now) {
    // Joint speeds tracked up to now carry over, so the hit that set off the bail still counts.
    if (now - s.last_tick > 100) {
        s.have_position = s.have_velocity = false;
        s.carry_down.fill(0);
        s.carry_side.fill(0);
        s.last_tick = now;
    }
    s.armed = true;
    s.armed_at = now;
    s.fell = std::min(0.0f, s.hips_vy);
    ++s.serial;
    s.head_hit = 0;
    s.rang = false;
    s.level.fill(0);
    s.hurt.fill(0);
    s.preview = false;
    s.pose_reported = false;
    s.posed = false;
}
// The X-ray comes up: when the body lands (or hits a wall), not while it is still flying.
void show(State &s, std::uint64_t now) {
    s.armed = false;
    s.active = true;
    s.started = now;
    s.effects_until = now + mark_ms;
    logging::log(logging::Level::info, logging::Channel::runtime, "Bone Cam: started.");
}
void start(State &s, std::uint64_t now) {
    begin_bail(s, now);
    show(s, now);
}
// Ends the X-ray and everything the slams left behind.
void stop(State &s) {
    s.active = false;
    s.armed = false;
    s.effects_until = 0;
    s.hit = 0;
    std::lock_guard lock(s.mutex);
    s.shown = {};
}
// The skater's pose this tick, composed into world space. Empty while it can be read, else why not.
std::string read_pose(State &s, std::uintptr_t base, std::uintptr_t client) {
    try {
        namespace detail = client_source::detail;
        const auto component = detail::first_person_component(base, client);
        std::uintptr_t holder{};
        if (!detail::first_person_read(component + 0xa0, &holder, 8) || !holder) return "no animation holder";
        const auto pose = multiplayer::read_native_pose_layout(detail::first_person_read, base, holder, 512);
        if (!pose.buffer) return "no pose buffer";
        if (pose.count != skater_joint_count) return "the skeleton has " + std::to_string(pose.count) + " joints";
        if (!detail::first_person_read(pose.buffer, s.raw.data(), skater_joint_count * sizeof(s.raw[0]))) return "pose unreadable";
        for (std::size_t i = 0; i < skater_joint_count; ++i) {
            const auto &bone = s.raw[i];
            for (const std::size_t lane : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
                if (!std::isfinite(bone[lane]) || std::abs(bone[lane]) > 1.0e6f) return "pose not ready";
            const auto parent = skater_joint_parent[i];
            s.joints[i] = child(parent < 0 ? Joint{} : s.joints[static_cast<std::size_t>(parent)], bone);
        }
        return {};
    } catch (const client_source::detail::SourceGuard &guard) {
        return guard.message ? guard.message : "skater unavailable";
    } catch (const std::exception &e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}
void injure(State &s, std::size_t w, std::uint8_t level) {
    if (level <= s.level[w]) return;
    s.level[w] = level;
    auto &hurt = s.hurt[static_cast<std::size_t>(watches[w].sprite)];
    hurt = std::max(hurt, level);
}
// Joints whose velocity changed sharply since the last tick hit something.
// Joints that hit the ground hard. Only the parts of the body touching the ground (the lowest at
// that moment) take damage, and only for the speed they were carrying into it and lost: a
// head-first slam cracks the skull and collarbones, a feet-first one the ankles and shins, and a
// ragdoll's flailing hurts nothing. The skull gives first; at most three bones break per bail.
// Outside a bail (`injuring` false) it only keeps the joints' speeds, and watches the hips for a
// dead stop: a body that loses 7 m/s within a third of a second hit something (bone_cam_slam).
void find_impacts(State &s, std::uint64_t now, bool injuring = true) {
    const float seconds = static_cast<float>(now - s.last_tick) / 1000.0f;
    s.last_tick = now;
    const bool usable = seconds > 0.004f && seconds < 0.1f;
    float lowest = std::numeric_limits<float>::max();
    for (const auto &w : watches) lowest = std::min(lowest, s.joints[w.joint].position[1]);
    auto broken = static_cast<std::size_t>(std::count(s.level.begin(), s.level.end(), std::uint8_t{2}));
    const float decay = usable ? std::exp(-seconds / 0.33f) : 0.0f;
    // Whether the body as a whole has stopped falling: the hips' vertical speed. In the air the
    // whole body drops together and nothing has hit anything, however the limbs swing.
    float hips_vertical = 0;
    for (std::size_t w = 0; w < watch_count; ++w)
        if (watches[w].joint == joint::hips && s.have_position && usable)
            hips_vertical = (s.joints[joint::hips].position[1] - s.position[w][1]) / seconds;
    const bool body_stopped = hips_vertical > -2.5f;
    // How fast the body as a whole was going (the hips' carried speeds): no part of it hits harder
    // than that, give or take. An arm swung down in a handplant, a foot on a stair, a jump on the
    // spot move a limb fast while the body barely moves, and none of them is a fall.
    float body_down = 0, body_side = 0;
    // The body's speed (the hips') last tick and now, to notice a part of it hitting an object.
    float body_before = 0, body_now = 0;
    for (std::size_t w = 0; w < watch_count; ++w)
        if (watches[w].joint == joint::hips) {
            body_down = s.carry_down[w];
            body_side = s.carry_side[w];
            const auto &was = s.velocity[w];
            body_before = std::sqrt(was[0] * was[0] + was[1] * was[1] + was[2] * was[2]);
            if (s.have_position && usable) {
                const auto &p = s.joints[joint::hips].position, &q = s.position[w];
                const float dx = (p[0] - q[0]) / seconds, dy = (p[1] - q[1]) / seconds, dz = (p[2] - q[2]) / seconds;
                body_now = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
    // A real collision slows the body too, as it wraps round what it hit.
    const bool body_slowed = body_before >= 6.0f && body_now < 0.85f * body_before;
    if (s.have_position && usable) {
        s.hips_vy = hips_vertical;
        if (s.armed) s.fell = std::min(s.fell, hips_vertical); // the fastest fall since the bail began
    }
    for (std::size_t w = 0; w < watch_count; ++w) {
        const auto &p = s.joints[watches[w].joint].position;
        if (s.have_position && usable) {
            Vec3 v{};
            for (int i = 0; i < 3; ++i) v[i] = (p[i] - s.position[w][i]) / seconds;
            const float down = std::max(0.0f, -v[1]), side = std::hypot(v[0], v[2]);
            // A teleport or a reset is not a hit.
            if (down > 60.0f || side > 60.0f) {
                s.position[w] = p;
                s.carry_down[w] = s.carry_side[w] = 0;
                continue;
            }
            if (watches[w].joint == joint::hips) {
                const float speed = std::hypot(side, v[1]);
                float fastest = 0;
                for (const auto &[at, was] : s.hips_speeds)
                    if (at && now - at <= 330) fastest = std::max(fastest, was);
                // Fast (8 m/s or more) a moment ago, all but stopped now, and not still falling:
                // the body hit something. Once in a while at most: on foot the measured speed
                // jitters, and one slam must not be counted thirty times.
                if (fastest >= 8.0f && speed < 2.5f && body_stopped && now - s.slam_time > 1500) {
                    s.slams.fetch_add(1);
                    s.slam_time = now;
                }
                s.hips_speeds[s.hips_next++ % s.hips_speeds.size()] = {now, speed};
            }
            // On the ground: among the body's lowest joints, the body no longer falling, and this
            // joint itself stopped (not just swinging past the others). Or against a wall or pole:
            // the whole body stopping dead, this joint with it.
            const bool stopped = std::abs(v[1]) < 2.5f;
            const bool on_ground = p[1] - lowest <= 0.3f && body_stopped && stopped;
            const bool against = s.slam_time && now - s.slam_time <= 150 && std::hypot(side, v[1]) < 3.0f;
            // Struck an object (a pole, a tree, a rail, a ramp's edge): moving with the body at speed
            // a moment ago, nearly stopped now while the body was still going, the body slowing as it
            // wraps round it. A swinging limb does not stop dead in the world while the body flies on.
            const auto &was = s.velocity[w];
            const float before = std::sqrt(was[0] * was[0] + was[1] * was[1] + was[2] * was[2]);
            const float after = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            const bool struck = s.have_velocity && body_slowed && before >= 0.7f * body_before && after <= 0.35f * body_before &&
                                p[1] - lowest > 0.3f;
            if (struck) {
                // The head, the chest, the shoulders or the hips: the body itself hit it (a limb alone
                // only counts once a bail is on).
                const auto j = watches[w].joint;
                const bool body = j == joint::head || j == joint::spine2 || j == joint::hips || j == joint::right_arm || j == joint::left_arm;
                if (body && now - s.struck_time > 1000) s.strikes.fetch_add(1);
                s.struck_time = now;
            }
            const bool touching = on_ground || against || struck;
            if (injuring && s.have_velocity && touching) {
                // The speed it carried into the ground and lost, plus some of the speed along it
                // lost (a scrape or a wall). Striking an object: all the speed it lost.
                const float into = std::min(std::max(0.0f, s.carry_down[w] - down), body_down + 2.0f);
                const float sideways = std::min(std::max(0.0f, s.carry_side[w] - side), body_side + 2.0f);
                const float impact = struck ? std::min(before - after, body_before + 2.0f) : into + 0.35f * sideways;
                // The preview shows the skeleton alone: nothing cracks or breaks in it.
                if (!s.preview) {
                    if (impact >= watches[w].snap && broken < 3) {
                        if (s.level[w] < 2) ++broken; // the cap holds within one tick too
                        injure(s, w, 2);
                    } else if (impact >= watches[w].crack) {
                        injure(s, w, 1);
                    }
                }
                if (!s.preview && watches[w].joint == joint::head) s.head_hit = std::max(s.head_hit, impact);
                // Spent: the same hit is not counted again next tick.
                if (impact >= 3.0f) {
                    s.carry_down[w] = down;
                    s.carry_side[w] = side;
                }
                if (!s.preview && impact >= 3.0f) {
                    // The hit's flash, brighter the harder the hit.
                    const float since = static_cast<float>(now - s.hit_time) / hit_ms;
                    const float left = s.hit * std::max(0.0f, 1.0f - since), strength = std::clamp(impact / 9.0f, 0.3f, 1.0f);
                    if (strength > left) {
                        s.hit = strength;
                        s.hit_time = now;
                        s.hit_at = p;
                    }
                }
            }
            s.carry_down[w] = std::max(s.carry_down[w] * decay, down);
            s.carry_side[w] = std::max(s.carry_side[w] * decay, side);
            s.velocity[w] = v;
        }
        s.position[w] = p;
    }
    if (usable) s.have_velocity = s.have_position;
    s.have_position = true;
}
// The joints the 3D X-ray builds its bones on. The spine between the hips and the head, and the
// collarbones (the arms' parents), are found once from the skeleton's parent table.
struct PoseJoints {
    std::vector<std::uint16_t> spine; // hips .. head
    std::array<std::uint16_t, 2> clavicle{};
};
const PoseJoints &pose_joints() {
    static const PoseJoints value = [] {
        PoseJoints p;
        for (int j = joint::head; j >= 0 && j != joint::hips; j = skater_joint_parent[static_cast<std::size_t>(j)])
            p.spine.insert(p.spine.begin(), static_cast<std::uint16_t>(j));
        p.spine.insert(p.spine.begin(), joint::hips);
        for (std::size_t side = 0; side < 2; ++side) {
            const auto arm = side ? joint::left_arm : joint::right_arm;
            const auto parent = skater_joint_parent[arm];
            p.clavicle[side] = static_cast<std::uint16_t>(parent >= 0 ? parent : arm);
        }
        return p;
    }();
    return value;
}
BoneCamPose pose_of(const State &s) {
    BoneCamPose pose;
    const auto &p = pose_joints();
    const auto at = [&](std::uint16_t j) { return s.joints[j].position; };
    for (const auto j : p.spine) pose.spine.push_back(at(j));
    pose.head = at(joint::head);
    pose.head_up = rotate(s.joints[joint::head].rotation, {1, 0, 0}); // the head joint's local +X is up
    for (std::size_t side = 0; side < 2; ++side) {
        const bool l = side == 1;
        pose.clavicle[side] = at(p.clavicle[side]);
        pose.shoulder[side] = at(l ? joint::left_arm : joint::right_arm);
        pose.elbow[side] = at(l ? joint::left_fore_arm : joint::right_fore_arm);
        pose.wrist[side] = at(l ? joint::left_hand : joint::right_hand);
        pose.fingers[side] = at(l ? joint::left_middle : joint::right_middle);
        pose.hip[side] = at(l ? joint::left_up_leg : joint::right_up_leg);
        pose.knee[side] = at(l ? joint::left_leg : joint::right_leg);
        pose.ankle[side] = at(l ? joint::left_foot : joint::right_foot);
        pose.toe[side] = at(l ? joint::left_toe : joint::right_toe);
    }
    pose.hurt.assign(s.hurt.begin(), s.hurt.end());
    pose.valid = true;
    return pose;
}

// The marks and hit as they look now: marks fade over their last 8 s.
void publish_effects(State &s, std::uint64_t now, BoneCam &cam) {
    if (!s.effects || s.preview) return;
    const float since = static_cast<float>(now - s.hit_time) / hit_ms;
    if (s.hit > 0 && since < 1) {
        cam.hit = s.hit * (1 - since) * (1 - since);
        cam.hit_at = s.hit_at;
    }
    // A concussion's after-effects, once the X-ray is over: kept faint (a soft dimming at the
    // edges, a slow sway of it), strongest at once and easing off over the concussion's time.
    if (const auto *c = concussion_of(s.head_hit); c && now >= s.started + duration_ms) {
        const auto into = now - (s.started + duration_ms);
        if (into < c->lasts_ms) {
            const float grade = std::clamp((s.head_hit - 5.5f) / 5.0f, 0.0f, 1.0f);
            cam.daze = (0.35f + 0.65f * grade) * (1.0f - static_cast<float>(into) / c->lasts_ms);
            if (!s.rang) {
                s.rang = true;
                if (s.ringing) ring(grade);
            }
        }
    }
    cam.effects = cam.hit > 0 || cam.daze > 0;
}
void publish(State &s, std::uint64_t now, bool bones = true) {
    BoneCam cam;
    publish_effects(s, now, cam);
    cam.active = s.active;
    if (!s.active) {
        std::lock_guard lock(s.mutex);
        s.shown = std::move(cam);
        return;
    }
    cam.serial = s.serial;
    cam.preview = s.preview;
    const auto elapsed = now - s.started, left = duration_ms > elapsed ? duration_ms - elapsed : 0;
    cam.fade = std::clamp(std::min(elapsed / static_cast<float>(fade_in_ms), left / static_cast<float>(fade_out_ms)), 0.0f, 1.0f);
    const auto &l = s.joints[joint::left_arm].position, &r = s.joints[joint::right_arm].position;
    cam.left = {l[0] - r[0], l[1] - r[1], l[2] - r[2]};
    if (bones) cam.pose = pose_of(s);
    for (const auto &span : bones ? std::span<const Span>(spans) : std::span<const Span>()) {
        BoneCamBone bone;
        bone.sprite = static_cast<std::uint8_t>(span.sprite);
        bone.a = s.joints[span.a].position;
        if (span.b == head_up) {
            // The head joint's local +X is up (client_first_person.cpp).
            const auto up = rotate(s.joints[span.a].rotation, {1, 0, 0});
            for (int i = 0; i < 3; ++i) bone.b[i] = bone.a[i] + up[i] * 0.2f;
        } else {
            bone.b = s.joints[span.b].position;
        }
        bone.hurt = s.hurt[static_cast<std::size_t>(span.sprite)];
        cam.bones.push_back(bone);
    }
    for (std::size_t w = 0; w < watch_count; ++w) {
        if (watches[w].joint == joint::head) continue; // the head is listed by its concussion below
        if (s.level[w])
            cam.injuries.push_back({s.level[w] == 2 ? watches[w].broken_bone : watches[w].cracked_bone,
                                    s.level[w] == 2 ? watches[w].broken : watches[w].cracked, s.level[w] == 2});
    }
    // The head: a fractured skull, and the concussion graded by how hard it hit.
    if (s.level[0] == 2) cam.injuries.push_back({"SKULL", "FRACTURED", true});
    if (const auto *c = concussion_of(s.head_hit)) cam.injuries.push_back({"HEAD", c->what, c->from >= 8.5f});
    std::stable_sort(cam.injuries.begin(), cam.injuries.end(), [](const auto &a, const auto &b) { return a.severe > b.severe; });
    std::lock_guard lock(s.mutex);
    s.shown = std::move(cam);
}
} // namespace

namespace {
void load_settings() noexcept; // the player's saved Bone Cam choices (below)
}
namespace {
// The owners (plugins' games) asking for the "game" setting's Bone Cam.
struct Askers {
    std::mutex mutex;
    std::set<std::string, std::less<>> owners;
    std::atomic<bool> any{};
};
Askers& askers() {
    static auto* value = new Askers;
    return *value;
}
bool game_asks() noexcept { return askers().any.load(std::memory_order_acquire); }
} // namespace
void bone_cam_game(std::string_view owner, bool on) {
    auto& a = askers();
    std::lock_guard lock(a.mutex);
    if (on) a.owners.emplace(owner);
    else if (const auto found = a.owners.find(owner); found != a.owners.end()) a.owners.erase(found);
    a.any.store(!a.owners.empty(), std::memory_order_release);
}
void bone_cam_release(std::string_view owner) { bone_cam_game(owner, false); }
void tick_bone_cam(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept {
    try {
        auto &s = state();
        static const bool loaded = (load_settings(), true); // the player's saved choices, once
        (void)loaded;
        const auto now = now_ms();
        s.pose_base=base;s.pose_client=client;s.pose_context_at=now;
        // Only a lasting loss of play ends it: a slam can briefly take the game out of play.
        if (!playing) {
            if (!s.unready_since) s.unready_since = now;
            if (!s.active || now - s.unready_since >= 2000) stop(s);
            return;
        }
        s.unready_since = 0;
        // Bails are reported by a plugin that watches the wipeout states (bone_cam_bail).
        const bool asked = game_asks();
        const bool wanted = s.setting == Setting::on || (s.setting == Setting::game && asked);
        if (s.bail_pending.exchange(false) && wanted && !s.active && !s.armed) begin_bail(s, now);
        // The X-ray comes up when the body lands: it was falling and has stopped. A bail with no
        // fall in it (the body already down) shows at once; one that never lands, after 6 s.
        if (s.armed) {
            const bool landed = s.fell < -3.0f && s.hips_vy > -2.5f;
            const bool never_fell = s.fell >= -3.0f && s.hips_vy > -2.5f && now - s.armed_at > 400;
            // Or the moment it hits something on the way down: a pole, a tree, a rail.
            const bool struck = s.struck_time >= s.armed_at && now - s.struck_time < 300;
            if (landed || never_fell || struck || now - s.armed_at > 6000) show(s, now);
        }
        // The X-ray ends after a few seconds; a concussion's after-effects last a while longer.
        if (s.active && now - s.started >= duration_ms) s.active = false;
        const bool showing = s.active || (s.effects && now < s.effects_until);
        if (!showing) {
            {
                std::lock_guard lock(s.mutex);
                s.shown = {};
            }
            // Between bails the joints are still followed, so the hit that sets off a bail counts.
            // A game that asks needs them with the Bone Cam off too: its on-foot slams (bone_cam_slam)
            // come from here.
            // While a bail is armed (in the air) its hits are measured already.
            if ((wanted || asked) && read_pose(s, base, client).empty()) find_impacts(s, now, s.armed);
            return;
        }
        if (const auto why = read_pose(s, base, client); !why.empty()) {
            if (!s.pose_reported) {
                s.pose_reported = true;
                logging::log(logging::Level::warning, logging::Channel::runtime, "Bone Cam: cannot read the skater's pose: {}", why);
            }
            // Mid-ragdoll the skater can be out of reach: the X-ray and injuries still show, with
            // the last pose read this bail if there was one.
            publish(s, now, s.posed);
            return;
        }
        s.posed = true;
        if (!s.pose_reported) {
            s.pose_reported = true;
            logging::log(logging::Level::info, logging::Channel::runtime, "Bone Cam: reading the skater's {} joints.", skater_joint_count);
        }
        find_impacts(s, now, s.active || s.armed);
        publish(s, now);
    } catch (...) {}
}

void bone_cam_bail() noexcept { state().bail_pending.store(true); }

std::uint64_t bone_cam_slams() noexcept { return state().slams.load(); }

std::uint64_t bone_cam_strikes() noexcept { return state().strikes.load(); }

std::string bone_cam_setting() {
    switch (state().setting) {
    case Setting::on: return "on";
    case Setting::off: return "off";
    default: return "game";
    }
}
bool bone_cam_ringing() noexcept { return state().ringing; }

namespace {
// The player's Bone Cam choices, kept between launches: %LOCALAPPDATA%\ReSkate\bone_cam.txt, one
// `name=value` per line. A missing or unreadable file leaves the defaults.
std::filesystem::path settings_file() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) return {};
    return std::filesystem::path(std::wstring(local.data(), length)) / L"ReSkate" / L"bone_cam.txt";
}
void save_settings(const State &s) {
    try {
        const auto path = settings_file();
        if (path.empty()) return;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::trunc);
        out << "show=" << (s.setting == Setting::on ? "on" : s.setting == Setting::off ? "off" : "game") << '\n'
            << "effects=" << (s.effects ? "on" : "off") << '\n'
            << "ringing=" << (s.ringing ? "on" : "off") << '\n';
    } catch (...) {}
}
void load_settings() noexcept {
    try {
        auto &s = state();
        const auto path = settings_file();
        if (path.empty()) return;
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto name = line.substr(0, eq), value = line.substr(eq + 1);
            if (name == "show") s.setting = value == "on" ? Setting::on : value == "off" ? Setting::off : Setting::game;
            else if (name == "effects") s.effects = value != "off";
            else if (name == "ringing") s.ringing = value != "off";
        }
    } catch (...) {}
}
} // namespace

std::string bone_cam_command(const std::vector<std::string> &arguments) {
    auto &s = state();
    const auto word = arguments.empty() ? std::string() : arguments[0];
    if (word.empty())
        return "Bone Cam: " + bone_cam_setting() + ", slam effects " + (s.effects ? "on" : "off") + ", ringing " +
               (s.ringing ? "on" : "off") + " (bonecam game|on|off|test, bonecam effects on|off, bonecam ring on|off).";
    if (word == "effects") {
        const auto value = arguments.size() > 1 ? arguments[1] : std::string();
        if (value != "on" && value != "off") return "error: usage: bonecam effects on|off";
        s.effects = value == "on";
        if (!s.effects) s.hit = 0;
        save_settings(s);
        return s.effects ? "Slam effects on: the hit flash and a concussion's after-effects." : "Slam effects off.";
    }
    if (word == "ring") {
        const auto value = arguments.size() > 1 ? arguments[1] : std::string();
        if (value != "on" && value != "off") return "error: usage: bonecam ring on|off";
        s.ringing = value == "on";
        if (!s.ringing) PlaySoundW(nullptr, nullptr, 0); // and quiet one already ringing
        save_settings(s);
        return s.ringing ? "Concussion ringing on." : "Concussion ringing muted.";
    }
    if (word == "game") s.setting = Setting::game;
    else if (word == "on") s.setting = Setting::on;
    else if (word == "off") { s.setting = Setting::off; stop(s); }
    else if (word == "test") {
        // A look at the Bone Cam without bailing: the skeleton alone, nothing broken.
        stop(s);
        start(s, now_ms());
        s.preview = true;
        return "Bone Cam preview for a few seconds.";    } else {
        return "error: usage: bonecam game|on|off|test";
    }
    save_settings(s);
    return word == "game" ? "Bone Cam shows while a game asks for it." : word == "on" ? "Bone Cam shows on every bail." : "Bone Cam off.";
}

BoneCam bone_cam() {
    auto &s = state();
    BoneCam cam;
    {
        std::lock_guard lock(s.mutex);
        if (!s.shown.active && !s.shown.effects) return {};
        cam = s.shown;
    }
    if (const auto view = latest_game_view()) {
        cam.camera = view->world;
        cam.vertical_fov = view->vertical_fov;
    }
    return cam;
}
std::optional<std::array<float,3>> bail_body_position() noexcept {
    try {
        auto& owner=state();const auto sampled=owner.pose_context_at.load();const auto now=now_ms();
        if(!sampled || now<sampled || now-sampled>250)return {};
        const auto base=owner.pose_base.load(),client=owner.pose_client.load();if(!base || !client)return {};
        // Do not mutate Bone Cam's impact history, pose publication or settings.
        thread_local State probe;
        if(!read_pose(probe,base,client).empty())return {};
        const auto at=probe.joints[joint::hips].position;
        if(std::any_of(at.begin(),at.end(),[](float value){return !std::isfinite(value) || std::abs(value)>100000.f;}))return {};
        return at;
    } catch(...) {return {};}
}
} // namespace dingosdk::bonecam
