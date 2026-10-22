#include "filmer_held.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/client_source_spawn.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace dingosdk::filmer::held {
namespace {
constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
constexpr std::size_t bone_stride = 0x30; // scale.xyzw | rotation.xyzw | position.xyzw
constexpr ULONGLONG fresh_ms = 300;       // a request or a hand older than this is gone
constexpr float ease_seconds = 0.35f;     // how long the arm takes to come up, or go back down

bool seh_read(std::uintptr_t address, void *out, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > highest - size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool seh_write(std::uintptr_t address, const void *in, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > highest - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void *>(address), in, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Your arm (the animation thread) and what the Filmer asks of it (the game thread).
struct Arm {
    std::mutex mutex;
    Request request;              // under the mutex
    ULONGLONG requested{};        // when, under the mutex
    std::atomic<std::uintptr_t> component{}, base{};
    std::atomic<bool> hooked{};
    bool hook_tried{};            // game thread
    std::string issue;            // game thread: why the arm cannot be bent
    // The animation thread's own.
    Request seen;
    ULONGLONG seen_at{};
    float weight{};
    ULONGLONG last_update{};
    std::atomic<std::uint32_t> bent{}, unreadable{};
    // What the game had in each joint written here, and what was written: the game does not set every
    // joint afresh each frame, so a joint still holding what was written is taken back to the game's
    // own value before it is worked out again (else the crouch and the hunch pile up frame on frame),
    // and every joint written is put back when the camera is put down. Under the mutex.
    std::array<hold::Bone, hold::pose_joints> original{}, written{};
    std::bitset<hold::pose_joints> touched{};
    // Which way your body faces (from the hips, before anything here changes them), and when.
    std::atomic<float> body_yaw{};
    std::atomic<ULONGLONG> body_yaw_at{};
    // Your board's animation holder, and its root's own size while it is hidden (render thread).
    std::atomic<std::uintptr_t> board_holder{};
    hold::Vec3 board_scale{1, 1, 1};
    bool board_hidden{};
    float push{};          // render thread: how much you are pushing, eased
};
Arm &arm() {
    static auto *value = new Arm;
    return *value;
}

// The skater's pose as it is handed to the renderer (the game has finished with it by then: bent any
// earlier, its own arm work puts the arm back), for your skater's.
// `animated`: called as the game finishes animating the skater: the request is taken and the arm
// eased; nothing is written, as the game works the arm again after this. Else called as the pose is
// handed to the renderer: written, so it is what you see (the others are sent it by on_outgoing).
bool same(const hold::Bone &a, const hold::Bone &b) noexcept {
    return a.scale == b.scale && a.rotation == b.rotation && a.position == b.position;
}
// A joint as read, with each part the game left as written taken back to the game's own (and the
// game's own kept for each part it set afresh). The game sets some parts of a joint each frame and not
// others (a joint's size never), so each is judged on its own.
void restore(hold::Bone &read, hold::Bone &original, const hold::Bone &written) noexcept {
    if (read.scale == written.scale) read.scale = original.scale; else original.scale = read.scale;
    if (read.rotation == written.rotation) read.rotation = original.rotation; else original.rotation = read.rotation;
    if (read.position == written.position) read.position = original.position; else original.position = read.position;
}
bool write_bone(std::uintptr_t buffer, std::uint16_t joint, const hold::Bone &bone) noexcept {
    std::array<float, 12> raw{bone.scale[0], bone.scale[1], bone.scale[2], 0.0f,
                              bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3],
                              bone.position[0], bone.position[1], bone.position[2], 1.0f};
    std::array<float, 12> was{};
    if (!seh_read(buffer + joint * bone_stride, was.data(), sizeof(was))) return false;
    raw[3] = was[3];
    raw[11] = was[11];
    return seh_write(buffer + joint * bone_stride, raw.data(), sizeof(raw));
}
// Every joint written here put back as the game had it.
void put_back(Arm &a, std::uintptr_t buffer) noexcept {
    std::lock_guard lock(a.mutex);
    if (a.touched.none()) return;
    for (std::size_t j = 0; j < a.touched.size(); ++j)
        if (a.touched[j]) (void)write_bone(buffer, static_cast<std::uint16_t>(j), a.original[j]);
    a.touched.reset();
}

// `animated`: called as the game finishes animating the skater: the request is taken and the arm
// eased; nothing is written. Else called as the pose is handed to the renderer: written, so it is what
// you see (the others are sent it by on_outgoing). Looking through the camera (hide_own), nothing is
// written: your own screen shows your arm as the game has it, out of the lens's way.
void bend(std::uintptr_t component, std::uintptr_t holder, bool animated) noexcept {
    auto &a = arm();
    const auto now = GetTickCount64();
    if (animated) {
        {
            std::unique_lock lock(a.mutex, std::try_to_lock);
            if (lock) {
                a.seen = a.request;
                a.seen_at = a.requested;
            }
        }
        const float dt = a.last_update ? std::clamp(static_cast<float>(now - a.last_update) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        a.last_update = now;
        const bool wanted = a.seen.hold && now - a.seen_at < fresh_ms;
        a.weight = std::clamp(a.weight + (wanted ? dt : -dt) / ease_seconds, 0.0f, 1.0f);
        return;
    }
    (void)component;
    try {
        const auto base = a.base.load(std::memory_order_acquire);
        const auto layout = multiplayer::read_native_pose_layout(seh_read, base, holder, 512);
        if (!layout.buffer || layout.count != hold::pose_joints) return;
        const bool hiding = a.seen.hide_own && a.seen.hold && now - a.seen_at < fresh_ms;
        if (a.weight <= 0.0f && !hiding) {
            put_back(a, layout.buffer);
            return;
        }
        // Only the joints from the root to the hand, the feet and the head are read: the rest stay as they are.
        std::array<hold::Bone, hold::pose_joints> pose{};
        const auto joints = hold::chain(a.seen.hand);
        std::bitset<hold::pose_joints> read{};
        const auto read_joint = [&](std::uint16_t joint) {
            if (read[joint]) return true;
            std::array<float, 12> raw{};
            if (!seh_read(layout.buffer + joint * bone_stride, raw.data(), sizeof(raw))) return false;
            pose[joint] = {{raw[0], raw[1], raw[2]}, {raw[4], raw[5], raw[6], raw[7]}, {raw[8], raw[9], raw[10]}};
            read[joint] = true;
            return true;
        };
        const auto fail = [&] { a.unreadable.fetch_add(1, std::memory_order_relaxed); };
        for (const auto joint : joints)
            if (!read_joint(joint)) return fail();
        for (const auto joint : hold::chain(a.seen.hand == hold::Hand::left ? hold::Hand::right : hold::Hand::left))
            if (!read_joint(joint)) return fail();
        for (const auto &leg : {std::span<const std::uint16_t>(hold::right_leg), std::span<const std::uint16_t>(hold::left_leg)})
            for (const auto joint : leg)
                if (!read_joint(joint)) return fail();
        for (const auto joint : hold::head_chain)
            if (!read_joint(joint)) return fail();
        {
            // A joint still holding what was written is taken back to the game's own value.
            std::lock_guard lock(a.mutex);
            for (std::size_t j = 0; j < read.size(); ++j) {
                if (!read[j]) continue;
                if (a.touched[j]) restore(pose[j], a.original[j], a.written[j]);
                else a.original[j] = pose[j];
            }
        }
        {
            const auto facing = hold::body_facing(pose);
            a.body_yaw.store(std::atan2(facing[0], facing[2]), std::memory_order_relaxed);
            a.body_yaw_at.store(now, std::memory_order_release);
        }
        const auto before = pose;
        hold::Goal goal;
        goal.hand = a.seen.hand;
        goal.aim = a.seen.aim;
        // Eased in and out: the arm comes up and goes back down instead of snapping.
        goal.weight = a.weight * a.weight * (3.0f - 2.0f * a.weight);
        goal.lens = a.seen.lens;
        goal.worn = a.seen.worn;
        goal.crouch = a.seen.crouch;
        goal.hunch = a.seen.hunch;
        goal.two_hands = a.seen.two_hands;
        goal.still = a.seen.still;
        goal.pivot = a.seen.pivot;
        goal.step = a.seen.step;
        goal.step_phase = a.seen.step_phase;
        {
            // Pushing on the board: the knees mostly straighten while the foot is down.
            const float pushing = a.seen.on_board ? hold::pushing(pose) : 0.0f;
            a.push += (pushing - a.push) * std::clamp(16.0f * 0.016f, 0.0f, 1.0f);
            goal.crouch *= 1.0f - 0.65f * a.push;
        }
        if (a.seen.relative) {
            std::array<hold::Placed, hold::right_chain.size()> placed{};
            if (!hold::compose(pose, joints, placed)) return;
            const auto &shoulder = placed[8].position, &f = a.seen.facing;
            goal.lens = {shoulder[0] + f[0] * a.seen.lens[0], shoulder[1] + a.seen.lens[1], shoulder[2] + f[2] * a.seen.lens[0]};
        }
        hold::Held held;
        if (hiding) {
            // You look through the camera: your arm as the game has it, the forearm (and the hand and
            // the camera on it) not drawn on your screen. The others are sent its own size (on_outgoing),
            // and it gets it back when you stop.
            pose[joints[9]].scale = {0.001f, 0.001f, 0.001f};
        } else {
            held = hold::hold(pose, goal);
            if (!held.valid) return;
        }
        {
            // What changes is written; a joint written before that no longer changes is put back.
            std::lock_guard lock(a.mutex);
            for (std::size_t j = 0; j < read.size(); ++j) {
                if (!read[j]) continue;
                const bool changed = !same(pose[j], before[j]);
                if (!changed && !a.touched[j]) continue;
                if (!write_bone(layout.buffer, static_cast<std::uint16_t>(j), pose[j])) return fail();
                a.written[j] = pose[j];
                a.touched[j] = changed;
            }
        }
        if (hiding) return;
        a.bent.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        a.unreadable.fetch_add(1, std::memory_order_relaxed);
    }
}

// The pose capture_local reads to send (client thread): the arm bent in it here, from the request
// itself, so the others see the camera held out whether or not the game drew or animated your skater
// in between (a camera filming someone else looks away from you).
struct Outgoing {
    float weight{}, push{};
    ULONGLONG last{};
    std::atomic<std::uint32_t> sent{}, bent{};
};
Outgoing &outgoing() {
    static auto *value = new Outgoing;
    return *value;
}
void on_outgoing(std::uintptr_t component, multiplayer::Pose &sent) noexcept {
    auto &a = arm();
    auto &skater = sent.skater;
    auto &o = outgoing();
    try {
        if (!component || component != a.component.load(std::memory_order_acquire)) return;
        o.sent.fetch_add(1, std::memory_order_relaxed);
        Request request;
        ULONGLONG requested{};
        {
            std::lock_guard lock(a.mutex);
            request = a.request;
            requested = a.requested;
        }
        const auto now = GetTickCount64();
        const float dt = o.last ? std::clamp(static_cast<float>(now - o.last) / 1000.0f, 0.0f, 0.2f) : 0.0f;
        o.last = now;
        const bool wanted = request.hold && now - requested < fresh_ms;
        o.weight = std::clamp(o.weight + (wanted ? dt : -dt) / ease_seconds, 0.0f, 1.0f);
        // The board out of sight for the others too: placed far under the ground.
        if (wanted && request.hide_board && !sent.board.empty()) sent.board.front().position[1] -= 100.0f;
        if (skater.size() < hold::pose_joints) return;
        thread_local std::vector<hold::Bone> pose;
        pose.resize(skater.size());
        for (std::size_t i = 0; i < skater.size(); ++i) pose[i] = {skater[i].scale, skater[i].rotation, skater[i].position};
        {
            // What the drawn pose still holds of the Filmer's is sent as the game had it.
            std::lock_guard lock(a.mutex);
            for (std::size_t j = 0; j < a.touched.size(); ++j)
                if (a.touched[j]) {
                    auto original = a.original[j];
                    restore(pose[j], original, a.written[j]);
                    skater[j] = {pose[j].position, pose[j].rotation, pose[j].scale};
                }
        }
        if (o.weight <= 0.0f) return;
        hold::Goal goal;
        goal.hand = request.hand;
        goal.aim = request.aim;
        goal.weight = o.weight * o.weight * (3.0f - 2.0f * o.weight);
        goal.lens = request.lens;
        goal.worn = request.worn;
        goal.crouch = request.crouch;
        goal.hunch = request.hunch;
        goal.two_hands = request.two_hands;
        goal.still = request.still;
        goal.pivot = request.pivot;
        goal.step = request.step;
        goal.step_phase = request.step_phase;
        o.push += ((request.on_board ? hold::pushing(pose) : 0.0f) - o.push) * std::clamp(dt * 8.0f, 0.0f, 1.0f);
        goal.crouch *= 1.0f - 0.65f * o.push;
        const auto joints = hold::chain(request.hand);

        if (request.relative) {
            std::array<hold::Placed, hold::right_chain.size()> placed{};
            if (!hold::compose(pose, joints, placed)) return;
            const auto &shoulder = placed[8].position, &f = request.facing;
            goal.lens = {shoulder[0] + f[0] * request.lens[0], shoulder[1] + request.lens[1], shoulder[2] + f[2] * request.lens[0]};
        }
        if (!hold::hold(pose, goal).valid) return;
        for (std::size_t j = 0; j < hold::pose_joints; ++j) {
            skater[j].rotation = pose[j].rotation;
            skater[j].position = pose[j].position;
        }
        o.bent.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
}

// Your board drawn or not: its root shrunk out of sight while you film on foot, and given its own size
// back after (the game never sets a joint's size again itself).
void board_render(std::uintptr_t holder) noexcept {
    auto &a = arm();
    const auto now = GetTickCount64();
    const bool hide = a.seen.hold && a.seen.hide_board && now - a.seen_at < fresh_ms;
    if (!hide && !a.board_hidden) return;
    try {
        const auto layout = multiplayer::read_native_pose_layout(seh_read, a.base.load(std::memory_order_acquire), holder, 63);
        if (!layout.buffer || !layout.count) return;
        hold::Vec3 scale{};
        if (!seh_read(layout.buffer, scale.data(), sizeof(scale))) return;
        if (hide) {
            if (!a.board_hidden || scale[0] > 0.01f) a.board_scale = scale;
            const hold::Vec3 tiny{0.001f, 0.001f, 0.001f};
            if (seh_write(layout.buffer, tiny.data(), sizeof(tiny))) a.board_hidden = true;
        } else {
            if (scale[0] < 0.01f) (void)seh_write(layout.buffer, a.board_scale.data(), sizeof(a.board_scale));
            a.board_hidden = false;
        }
    } catch (...) {}
}

void on_render(std::uintptr_t animation_interface) noexcept {
    if (const auto board = arm().board_holder.load(std::memory_order_acquire); board && animation_interface == board + 0xc0) {
        const auto error = GetLastError();
        board_render(board);
        SetLastError(error);
        return;
    }
    const auto component = arm().component.load(std::memory_order_acquire);
    if (!component) return;
    std::uintptr_t holder{};
    if (!seh_read(component + 0xa0, &holder, sizeof(holder)) || !holder || animation_interface != holder + 0xc0) return;
    const auto error = GetLastError();
    bend(component, holder, false);
    SetLastError(error);
}
void on_animation(std::uintptr_t component) noexcept {
    if (!component || component != arm().component.load(std::memory_order_acquire)) return;
    std::uintptr_t holder{};
    if (!seh_read(component + 0xa0, &holder, sizeof(holder)) || !holder) return;
    const auto error = GetLastError();
    bend(component, holder, true);
    SetLastError(error);
}

} // namespace

void set_local(const Request &request) noexcept {
    auto &a = arm();
    std::lock_guard lock(a.mutex);
    a.request = request;
    a.requested = GetTickCount64();
}

void tick(std::uintptr_t base, std::uintptr_t client, bool in_level) noexcept {
    auto &a = arm();
    try {
        if (!in_level) {
            a.component.store(0, std::memory_order_release);
            return;
        }
        a.base.store(base, std::memory_order_release);
        // The pose the others are sent always goes through here (cheap while the camera is not held).
        multiplayer::set_local_sent_pose_filter(&on_outgoing);
        a.component.store(local_skater_component(base, client), std::memory_order_release);
        // Your board's animation holder, for hiding it while you film on foot.
        try {
            const auto local = multiplayer::capture_local(base, client, false);
            std::uintptr_t board{};
            if (local.ready && local.entity) board = multiplayer::read_native_board(seh_read, base, local.entity).holder;
            a.board_holder.store(board, std::memory_order_release);
        } catch (...) {
            a.board_holder.store(0, std::memory_order_release);
        }
        Request request;
        {
            std::lock_guard lock(a.mutex);
            request = a.request;
        }
        // The animation hook, the first time the arm is wanted.
        if (request.hold && !a.hook_tried) {
            a.hook_tried = true;
            std::string detail;
            if (multiplayer::install_entity_hooks(base, detail)) {
                multiplayer::set_render_pose_listener(&on_render, 1);
                multiplayer::set_animation_evaluated_listener(&on_animation, 1);
                a.hooked.store(true, std::memory_order_release);
                logging::log(logging::Level::info, logging::Channel::skater, "Filmer: your arm holds the camera out while you film.");
            } else {
                a.issue = "Your arm cannot hold the camera out: " + detail;
                logging::log(logging::Level::warning, logging::Channel::skater, "Filmer: {}", a.issue);
            }
        }
    } catch (...) {}
}

std::optional<float> body_yaw() noexcept {
    auto &a = arm();
    const auto at = a.body_yaw_at.load(std::memory_order_acquire);
    if (!at || GetTickCount64() - at > 500) return std::nullopt;
    return a.body_yaw.load(std::memory_order_relaxed);
}

std::string status() {
    auto &a = arm();
    std::string arm_text = !a.issue.empty() ? a.issue
                           : a.hooked.load() ? std::format("arm bent {} times", a.bent.load(std::memory_order_relaxed))
                                             : "arm not needed yet";
    if (const auto unreadable = a.unreadable.load(std::memory_order_relaxed)) arm_text += std::format(" ({} unreadable poses)", unreadable);
    arm_text += std::format(", sent bent {} of {}", outgoing().bent.load(std::memory_order_relaxed), outgoing().sent.load(std::memory_order_relaxed));
    return "Held camera: " + arm_text;
}
} // namespace dingosdk::filmer::held
