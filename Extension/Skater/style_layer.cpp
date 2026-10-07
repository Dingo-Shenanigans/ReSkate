#include "style_layer.h"
#include "style_skeleton.h"
#include "style_internal.h"
#include "style_file.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "no_bail.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dingosdk::style_layer {
namespace {
using namespace addr::style;
using style::Family;
using style::Quat;
using addr::style::bone_size, addr::style::bone_rotation, addr::style::bone_position;
// Measured 2026-10-04: Skatepedia's skater rolls approximately 10 m along Z on its stage.
constexpr auto &stage_centre = addr::style::skatepedia_stage, &stage_reach = addr::style::skatepedia_stage_reach;
constexpr float blend_seconds = 0.08f; // smooths sudden changes, such as the catch or a slam
constexpr float preview_play_seconds = 1.6f; // one pass of a previewed timeline
constexpr float preview_part_ms = preview_play_seconds * 1000.0f / style::trick_end;
constexpr style::Pace preview_pace{preview_part_ms, preview_part_ms, preview_part_ms};

// The joint rotations for each state family and each flip trick. A published snapshot never changes.
struct Snapshot {
    std::array<std::vector<style::JointDelta>, style::family_count> families;
    std::array<std::vector<style::Key>, style::flip_trick_names.size()> tricks; // sorted by time
    std::array<std::uint16_t, style::signature_joints.size()> signature{};
    Rig rig;
};
using Skeleton = std::vector<style::SkeletonJoint>;

struct Settings {
    std::mutex mutex;
    bool enabled{}, share{true}, dirty{true};
    style::Style style;
    std::shared_ptr<const Skeleton> skeleton;
    std::string issue; // why the layer is not applied
    bool skeleton_started{}, hooks_tried{}, hooks{};
    // The style file is read one time. With auto save, it is written 750 ms after each change.
    bool loaded{}, saved_enabled{}, saved_share{true};
    bool auto_save{true}, saved_auto_save{true};
    bool unsaved{}; // without auto save: the style differs from the file
    style::Style on_disk; // the style as last read or saved
    style::Pace editor_pace{style::even_pace}; // the pace of the clip on the editor's stand-in
    style::History history;
    std::uint64_t save_at{};
    std::string file_issue;
    bool keep_file{}; // the saved style did not load and has no copy
    std::string preset{"default"};
    std::vector<std::string> presets;
    std::uint64_t presets_at{}; // when the folder was last listed
};
Settings &settings() { static auto *value = new Settings; return *value; }

struct Live {
    std::atomic<std::uintptr_t> base{}, component{}, context{}, holder{}, trick_selection{};
    std::atomic<bool> active{}, share{true};
    std::atomic<bool> session_test{}; // the editor may run in a multiplayer session
    std::atomic<std::shared_ptr<const Snapshot>> snapshot;
    // The render pose handoff and the client tick share the pose bookkeeping below.
    std::mutex pose_mutex;
    style::PoseTracker tracker{skeleton_joints};
    std::uintptr_t buffer{};
    std::vector<style::JointDelta> merged;
    style::TrickTracker trick;
    style::Blender blender{skeleton_joints};
    std::uint64_t blended_at{};
    // Counters for the status line.
    std::atomic<std::uint64_t> render_calls{}, reused_calls{}, buffer_changes{}, failures{};
    std::atomic<std::uint32_t> state{}, shown_trick{};
    // The flip trick that the preview shows in every state. 0 is none.
    std::atomic<std::uint8_t> preview{};
    std::atomic<std::uint32_t> preview_time{};  // float bits
    std::atomic<std::uint64_t> preview_played{}; // the start time of playback, or 0 while held
    std::atomic<bool> restyle{true};
    // The frames of the player's own tricks as shown, for the restyle of replays.
    style::Takes takes;
    // The frames of the learned clips, for the restyle of Skatepedia's skater.
    style::Takes demos;
    std::atomic<std::uint64_t> stage_seen{};
    std::vector<style::JointDelta> restyled;
    std::atomic<std::uint64_t> replay_seen{}, replay_matches{}, replay_misses{};
    std::atomic<std::uint32_t> replay_trick{}, replay_time{}, replay_distance{};
    // The pose holder of the stand-in. The layer does not change it.
    std::atomic<std::uintptr_t> ignored{};
    // The recording of other rigs that request_learn() starts. pose_mutex guards the buffers.
    std::atomic<std::uint64_t> learn_until{};
    std::vector<std::uint8_t> learned, learned_done;
    // A cache of the kind of each other rig.
    struct Seen {
        std::uintptr_t holder{};
        std::uint64_t checked{};
        std::uint8_t kind{}; // 1: a skater. 2: a small rig such as a skateboard
    };
    std::atomic<std::uint64_t> clear_stage{}; // the time until which Skatepedia's skater and board stay hidden
    std::atomic<std::uint64_t> stage_watch{};  // the time until which other rigs are checked for Skatepedia's stage
    std::atomic<std::uintptr_t> ignored_board{};
    std::array<Seen, 64> seen;
    std::vector<style::JointDelta> timeline;
    // Other skaters on the standard skeleton that show the preview or a restyle.
    struct Other {
        std::uintptr_t holder{};
        std::unique_ptr<style::PoseTracker> tracker;
        std::uint64_t seen{};
        std::vector<style::JointDelta> shown;
    };
    std::array<Other, 8> others;
    std::atomic<std::uint64_t> other_calls{};
};
Live &live() { static auto *value = new Live; return *value; }

// GetTickCount64 moves in steps of 10 ms to 16 ms. That is too coarse to time one frame.
std::uint64_t steady_us() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// True when no remote skater exists apart from the stand-in. A skater in the stand-in's slot is a player while no stand-in shows.
bool solo() noexcept {
    if (multiplayer::other_remote_skaters(multiplayer::max_remote_players - 1)) return false;
    return live().ignored.load(std::memory_order_acquire) || !multiplayer::other_remote_skaters(multiplayer::max_remote_players);
}
// Writes a value into game memory. False if the page cannot be written.
template <class T> bool poke(std::uintptr_t address, const T &value) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Sets the three scale lanes of a bone. The fourth lane keeps its value.
bool write_scale(std::uintptr_t bone, float scale) noexcept { return poke(bone, std::array<float, 3>{scale, scale, scale}); }
std::optional<Family> family_of(std::uint32_t state) noexcept {
    if (state >= riding_states_begin && state < riding_states_end) return Family::riding;
    if (state >= grind_states_begin && state < grind_states_end) return Family::grind;
    if (state == offboard_state) return Family::offboard;
    return std::nullopt; // all other states keep the game's pose
}

// The signature joint rotations in the pose of `holder`, without the adjustment of `tracker`.
std::optional<style::Signature> signature(std::uintptr_t holder, const Snapshot &snapshot, const style::PoseTracker *tracker) {
    const auto pose = multiplayer::read_native_pose_layout(memory::peek_bytes, live().base.load(std::memory_order_acquire), holder, 512);
    if (!pose.buffer || pose.count != skeleton_joints) return std::nullopt;
    style::Signature result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto joint = snapshot.signature[i];
        if (!joint || !memory::peek(pose.buffer + joint * bone_size + bone_rotation, result[i]) || !style::finite(result[i]))
            return std::nullopt;
        if (const auto *base = tracker ? tracker->base(joint, result[i]) : nullptr) result[i] = *base;
    }
    return result;
}
// The timeline time of the preview, held or in a loop.
float preview_time(std::uint64_t now) noexcept {
    auto &l = live();
    if (const auto started = l.preview_played.load(std::memory_order_acquire))
        return std::fmod(static_cast<float>(now - started) / 1000.0f / preview_play_seconds, 1.0f) * style::trick_end;
    return std::bit_cast<float>(l.preview_time.load(std::memory_order_acquire));
}
// The rotations of a state family with a trick's keyframes at `time` on top. `timeline` is scratch space.
void merged(const Snapshot &snapshot, Family family, std::uint8_t trick, float time, const style::Pace &pace,
            std::vector<style::JointDelta> &timeline, std::vector<style::JointDelta> &out) {
    out = snapshot.families[static_cast<std::size_t>(family)];
    if (!trick || trick >= snapshot.tricks.size()) return;
    style::evaluate(snapshot.tricks[trick], time, timeline, pace);
    for (const auto &delta : timeline) {
        const auto same = std::ranges::find(out, delta.joint, &style::JointDelta::joint);
        if (same == out.end()) out.push_back(delta);
        else *same = delta;
    }
}
// Writes `deltas` into the pose of `holder`. Returns false if there is no work or no standard skeleton.
bool write_pose(style::PoseTracker &tracker, std::uintptr_t holder, const std::vector<style::JointDelta> &deltas,
                std::uintptr_t *buffer = nullptr) {
    auto &l = live();
    if (deltas.empty() && tracker.adjusted().empty()) return false;
    const auto pose = multiplayer::read_native_pose_layout(memory::peek_bytes, l.base.load(std::memory_order_acquire), holder, 512);
    if (!pose.buffer || pose.count != skeleton_joints) return false;
    if (buffer && pose.buffer != *buffer) {
        *buffer = pose.buffer;
        l.buffer_changes.fetch_add(1, std::memory_order_relaxed);
    }
    const auto rotation_at = [&](std::uint16_t joint) { return pose.buffer + joint * bone_size + bone_rotation; };
    const auto current = [&](std::uint16_t joint) {
        Quat value{};
        if (!memory::peek(rotation_at(joint), value)) throw std::runtime_error("pose unreadable");
        return value;
    };
    bool any_reused{};
    tracker.begin();
    for (const auto &delta : deltas) {
        const auto now = current(delta.joint);
        if (!style::finite(now)) continue; // the pose is not loaded yet
        bool reused{};
        const auto next = tracker.adjust(delta.joint, now, delta.rotation, &reused);
        any_reused |= reused;
        if (!poke(rotation_at(delta.joint), next)) throw std::runtime_error("pose unwritable");
    }
    tracker.end(current, [&](std::uint16_t joint, const Quat &base) { (void)poke(rotation_at(joint), base); });
    if (any_reused && buffer) l.reused_calls.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void apply(std::uintptr_t component) {
    auto &l = live();
    const auto snapshot = l.snapshot.load(std::memory_order_acquire);
    std::optional<Family> family;
    std::uint32_t state{};
    const bool known = memory::peek(l.context.load(std::memory_order_acquire) + context_physics_state, state);
    if (known) {
        l.state.store(state, std::memory_order_relaxed);
        family = family_of(state);
    }
    std::int32_t flip{-1};
    const auto selection = l.trick_selection.load(std::memory_order_acquire);
    const bool flip_known = selection && memory::peek(selection + trick_selection_flip_trick, flip);
    std::lock_guard lock(l.pose_mutex);
    const auto now = GetTickCount64();
    const auto micro = steady_us();
    const auto moment = l.trick.step(flip_known ? flip : -1, state == riding_ground_state || state == riding_landed_state,
                                     known && family.has_value(), family == Family::riding, micro / 1000);
    l.shown_trick.store(moment.trick, std::memory_order_relaxed);
    l.merged.clear();
    const auto preview = l.preview.load(std::memory_order_acquire);
    if (preview && snapshot) {
        style::evaluate(snapshot->tricks[preview], preview_time(now), l.merged, preview_pace);
    } else if (family && snapshot && l.active.load(std::memory_order_acquire)) {
        merged(*snapshot, *family, moment.trick, moment.time, l.trick.pace(moment.trick), l.timeline, l.merged);
    }
    const auto elapsed = l.blended_at ? static_cast<float>(std::min<std::uint64_t>(micro - l.blended_at, 100000)) / 1000000.0f : 0.0f;
    l.blended_at = micro;
    const auto &written = l.blender.step(l.merged, style::ease_amount(elapsed, blend_seconds));
    (void)write_pose(l.tracker, memory::peek_pointer(component, addr::style::component_pose_holder), written, &l.buffer);
    // The layer keeps the frame as shown, so that it can restyle a replay of this trick.
    if (moment.trick && snapshot && !preview)
        if (const auto shown = signature(memory::peek_pointer(component, addr::style::component_pose_holder), *snapshot, nullptr))
            l.takes.add({moment.trick, moment.time, *shown, written});
}
// Returns 1 for the skater skeleton, 2 for a small rig such as a skateboard, 0 for all others. The answer is cached for 2 s.
std::uint8_t other_kind(std::uintptr_t holder) noexcept {
    auto &l = live();
    const auto now = GetTickCount64();
    std::lock_guard lock(l.pose_mutex);
    Live::Seen *slot = &l.seen[0];
    for (auto &seen : l.seen) {
        if (seen.holder == holder) {
            slot = &seen;
            break;
        }
        if (seen.checked < slot->checked) slot = &seen;
    }
    if (slot->holder != holder || now > slot->checked + 2000) {
        std::uint8_t kind{};
        try {
            const auto pose = multiplayer::read_native_pose_layout(memory::peek_bytes, l.base.load(std::memory_order_acquire), holder, 512);
            if (pose.buffer) kind = pose.count == skeleton_joints ? 1 : pose.count >= 2 && pose.count <= 32 ? 2 : 0;
        } catch (...) {}
        // Skatepedia makes a new rig on each loop, so an unreadable rig is examined again soon.
        *slot = {holder, kind ? now : now < l.clear_stage.load(std::memory_order_relaxed) ? now - 2001 : now - 1900, kind};
    }
    return slot->kind;
}
// Hides a rig that is on Skatepedia's stage, so that only the stand-in shows there.
void clear_from_stage(std::uintptr_t holder, bool skater, bool clear) noexcept {
    auto &l = live();
    try {
        const auto pose = multiplayer::read_native_pose_layout(memory::peek_bytes, l.base.load(std::memory_order_acquire), holder, 512);
        if (!pose.buffer || pose.count < 2) return;
        // A skater has its world position in joint 1. Each joint of a board has its own.
        for (std::size_t joint = 1; joint < (skater ? 2u : std::min<std::size_t>(pose.count, 32)); ++joint) {
            const auto at = pose.buffer + joint * bone_size + bone_position;
            std::array<float, 3> position{};
            if (!memory::peek(at, position) || !std::isfinite(position[0] + position[1] + position[2])) continue;
            // An earlier frame moved it, and the game did not write the pose again.
            const bool moved = position[1] < -1000.0f;
            if (std::abs(position[0] - stage_centre[0]) > stage_reach[0] || std::abs(position[2] - stage_centre[2]) > stage_reach[2] ||
                std::abs(position[1] + (moved ? 2000.0f : 0.0f) - stage_centre[1]) > stage_reach[1]) {
                continue;
            }
            if (skater) l.stage_seen.store(GetTickCount64(), std::memory_order_relaxed);
            // The game does not restore the scale, so the layer restores it when the skater is no longer hidden.
            if (skater && !clear) {
                float scale{};
                if (memory::peek(pose.buffer + bone_size, scale) && scale < 0.01f) (void)write_scale(pose.buffer + bone_size, 1.0f);
            }
            // The layer restores the scale of a board in the same way.
            if (!skater && !clear && joint == 1) {
                Quat scale{};
                if (memory::peek(pose.buffer + bone_size, scale) && scale[0] == 0.0f && scale[1] == 0.0f)
                    for (std::size_t each = 0; each < std::min<std::size_t>(pose.count, 32); ++each) (void)write_scale(pose.buffer + each * bone_size, 1.0f);
            }
            if (moved || !clear) continue;
            if (skater) {
                // Scale 0.001, not a move: Skatepedia's camera follows a moved skater, and at scale 0 the stage disappears.
                (void)write_scale(pose.buffer + bone_size, 0.001f);
                continue;
            }
            position[1] -= 2000.0f;
            (void)poke(at, position);
            // The game does not draw a board from joint 1, so each joint also gets scale 0.
            for (std::size_t each = 0; each < std::min<std::size_t>(pose.count, 32); ++each) (void)write_scale(pose.buffer + each * bone_size, 0.0f);
        }
    } catch (...) {}
}
// Appends the pose of `holder` to the recording: time, holder, joint count, then the stored bones.
void learn(std::uintptr_t holder) noexcept {
    auto &l = live();
    try {
        const auto pose = multiplayer::read_native_pose_layout(memory::peek_bytes, l.base.load(std::memory_order_acquire), holder, 512);
        // Only skaters and boards. Other rigs would fill the recording.
        if (!pose.buffer || !pose.count || (pose.count != skeleton_joints && pose.count > 32)) return;
        std::lock_guard lock(l.pose_mutex);
        if (l.learned.size() > 96u * 1024 * 1024) return;
        // A recording of 8 s is about 36 MB. One reservation saves copies of the buffer as it grows.
        if (l.learned.empty()) l.learned.reserve(48u * 1024 * 1024);
        const std::uint64_t header[3]{steady_us() / 1000, holder, pose.count};
        const auto at = l.learned.size();
        l.learned.resize(at + sizeof(header) + pose.count * bone_size);
        std::memcpy(l.learned.data() + at, header, sizeof(header));
        if (!memory::peek_bytes(pose.buffer, l.learned.data() + at + sizeof(header), pose.count * bone_size)) l.learned.resize(at);
    } catch (...) {}
}
// Shows the preview or a restyle on another skater. Solo play only, because other players use the same skeleton.
void preview_other(std::uintptr_t holder) noexcept {
    auto &l = live();
    if (holder == l.ignored.load(std::memory_order_acquire) || holder == l.ignored_board.load(std::memory_order_acquire)) return;
    const bool recording = GetTickCount64() < l.learn_until.load(std::memory_order_relaxed);
    if (recording) learn(holder);
    const auto kind = other_kind(holder);
    // Skatepedia's skater shows that the stage exists. It stays visible while it is recorded.
    // Only the editor needs the stage, so other rigs are not read for it at other times.
    if (kind && (recording || GetTickCount64() < l.stage_watch.load(std::memory_order_relaxed))) clear_from_stage(holder, kind == 1, !recording && GetTickCount64() < l.clear_stage.load(std::memory_order_relaxed) && (solo() || session_test()));
    if (kind != 1) return;
    // During a recording, Skatepedia's skater shows the game's own animation.
    const bool learning = GetTickCount64() < l.learn_until.load(std::memory_order_relaxed) + 300;
    const auto preview = learning ? std::uint8_t{} : l.preview.load(std::memory_order_acquire);
    const auto snapshot = l.snapshot.load(std::memory_order_acquire);
    static const std::vector<style::JointDelta> none;
    const auto error = GetLastError();
    try {
        std::lock_guard lock(l.pose_mutex);
        const auto now = GetTickCount64();
        Live::Other *slot{}, *oldest = &l.others[0];
        for (auto &other : l.others) {
            if (other.holder == holder) slot = &other;
            if (other.seen < oldest->seen) oldest = &other;
        }
        const bool solo = snapshot && style_layer::solo();
        const bool matching = solo && !preview && !learning && l.active.load(std::memory_order_acquire) && l.restyle.load(std::memory_order_acquire) && (l.takes.size() || l.demos.size());
        if (!slot && !(solo && preview) && !matching) {
            SetLastError(error);
            return;
        }
        // Find the frame on screen among the known frames.
        const style::TakeFrame *frame{};
        if (matching) {
            std::optional<style::Signature> shown;
            try {
                shown = signature(holder, *snapshot, slot ? slot->tracker.get() : nullptr);
            } catch (...) {}
            if (shown) {
                float distance{};
                frame = l.demos.find(*shown, &distance);
                if (!frame) frame = l.takes.find(*shown, &distance);
                l.replay_distance.store(std::bit_cast<std::uint32_t>(distance), std::memory_order_relaxed);
                (frame ? l.replay_matches : l.replay_misses).fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (frame) {
            l.replay_trick.store(frame->trick, std::memory_order_relaxed);
            l.replay_time.store(std::bit_cast<std::uint32_t>(frame->time), std::memory_order_relaxed);
            l.replay_seen.store(now, std::memory_order_relaxed);
        }
        const bool wanted = (solo && preview) || frame;
        if (!slot) {
            if (!wanted) {
                SetLastError(error);
                return;
            }
            // A slot in use is not taken. A slot not seen for 1 s gets the game's rotations back first.
            if (oldest->holder && now < oldest->seen + 1000) {
                SetLastError(error);
                return;
            }
            if (oldest->holder && oldest->tracker) {
                try {
                    (void)write_pose(*oldest->tracker, oldest->holder, none);
                } catch (...) {}
            }
            slot = oldest;
            *slot = {holder, std::make_unique<style::PoseTracker>(skeleton_joints), now, {}};
        }
        slot->seen = now;
        if (solo && preview) {
            style::evaluate(snapshot->tricks[preview], preview_time(now), slot->shown, preview_pace);
        } else if (frame) {
            // Replace the rotations of the recorded frame with the rotations of the current style.
            merged(*snapshot, Family::riding, frame->trick, frame->time, l.trick.pace(frame->trick), l.timeline, l.restyled);
            style::restyle(frame->written, l.restyled, slot->shown);
        }
        if (write_pose(*slot->tracker, holder, wanted ? slot->shown : none))
            l.other_calls.fetch_add(1, std::memory_order_relaxed);
        if (!wanted && slot->tracker->adjusted().empty()) *slot = {};
    } catch (...) {
        l.failures.fetch_add(1, std::memory_order_relaxed);
    }
    SetLastError(error);
}
// The render pose handoff, on the engine thread after the client tick. Physics rewrites the body joints after animation, so the layer writes here.
void on_render(std::uintptr_t animation_interface) noexcept {
    auto &l = live();
    const auto holder = l.holder.load(std::memory_order_acquire);
    if (holder && animation_interface > addr::style::holder_animation_interface && animation_interface != holder + addr::style::holder_animation_interface) {
        preview_other(animation_interface - addr::style::holder_animation_interface);
        return;
    }
    if (!holder || animation_interface != holder + addr::style::holder_animation_interface) return;
    const auto error = GetLastError();
    l.render_calls.fetch_add(1, std::memory_order_relaxed);
    try {
        apply(l.component.load(std::memory_order_acquire));
    } catch (...) {
        l.failures.fetch_add(1, std::memory_order_relaxed);
    }
    SetLastError(error);
}
// Client thread. With sharing off, other players get the game's own rotations.
void filter_capture(std::uintptr_t component, std::vector<multiplayer::Transform> &skater) noexcept {
    auto &l = live();
    if (l.share.load(std::memory_order_acquire) || component != l.component.load(std::memory_order_acquire)) return;
    std::lock_guard lock(l.pose_mutex);
    for (const auto joint : l.tracker.adjusted())
        if (joint < skater.size())
            if (const auto *base = l.tracker.base(joint, skater[joint].rotation)) skater[joint].rotation = *base;
}

void read_skeleton() {
    try {
        std::thread([] {
            std::shared_ptr<const Skeleton> result;
            std::string error;
            try {
                std::vector<wchar_t> exe(32768);
                const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
                if (!length || length >= exe.size()) throw std::runtime_error("the game's folder is unknown");
                const auto started = GetTickCount64();
                result = std::make_shared<const Skeleton>(style::read_game_skeleton(std::filesystem::path(exe.data()).parent_path()));
                logging::log(logging::Level::info, logging::Channel::skater, "Style: read the skater skeleton ({} joints) in {} ms.",
                             result->size(), GetTickCount64() - started);
            } catch (const std::exception &failure) {
                error = std::format("could not read the skater skeleton: {}", failure.what());
                logging::log(logging::Level::warning, logging::Channel::skater, "Style is unavailable: {}.", error);
            }
            auto &s = settings();
            std::lock_guard lock(s.mutex);
            s.skeleton = std::move(result);
            s.issue = std::move(error);
            s.dirty = true;
        }).detach();
    } catch (...) {
        settings().issue = "could not start reading the skater skeleton";
    }
}
std::shared_ptr<const Snapshot> build(const Settings &s) {
    auto result = std::make_shared<Snapshot>();
    for (std::size_t i = 0; i < style::signature_joints.size(); ++i)
        if (const auto found = std::ranges::find(*s.skeleton, style::signature_joints[i], &style::SkeletonJoint::name); found != s.skeleton->end())
            result->signature[i] = static_cast<std::uint16_t>(found - s.skeleton->begin());
    const auto joint = [&](std::string_view name) {
        const auto found = std::ranges::find(*s.skeleton, name, &style::SkeletonJoint::name);
        return found != s.skeleton->end() ? static_cast<std::uint16_t>(found - s.skeleton->begin()) : std::uint16_t{};
    };
    auto parents = std::make_shared<std::vector<std::int32_t>>();
    for (const auto &bone : *s.skeleton) parents->push_back(bone.parent);
    result->rig = {joint("AITrajectory"), joint("Deck"), joint("LeftFoot"), joint("RightFoot"), std::move(parents)};
    for (std::uint8_t trick = 1; trick < result->tricks.size(); ++trick)
        for (const auto time : s.style.keys(trick)) result->tricks[trick].push_back({time, {}});
    for (const auto &[target, ms] : s.style.blend_outs)
        if (target.trick && target.id < result->tricks.size() && target.key < result->tricks[target.id].size())
            result->tricks[target.id][target.key].blend_out_ms = ms;
    for (const auto &[key, degrees] : s.style.rotations) {
        const auto found = std::ranges::find(*s.skeleton, key.second, &style::SkeletonJoint::name);
        if (found == s.skeleton->end()) continue;
        if (key.first.trick && key.first.key >= result->tricks[key.first.id].size()) continue;
        auto &list = key.first.trick ? result->tricks[key.first.id][key.first.key].joints : result->families[key.first.id];
        list.push_back(
            {static_cast<std::uint16_t>(found - s.skeleton->begin()), style::from_degrees(degrees[0], degrees[1], degrees[2])});
    }
    for (auto &keys : result->tricks) std::ranges::stable_sort(keys, {}, &style::Key::time);
    return result;
}
// Logs one time why the layer is not applied.
void refuse(Settings &s, std::string issue) {
    if (s.issue == issue) return;
    s.issue = std::move(issue);
    logging::log(logging::Level::warning, logging::Channel::skater, "Style is unavailable: {}.", s.issue);
}
} // namespace

namespace {
constexpr wchar_t style_folder[] = L"MyStyles";
constexpr std::string_view preset_extension = ".style.json";
std::filesystem::path presets_folder() { return mods::engine_data_root() / mods::mods_folder / style_folder / L"styles"; }
std::filesystem::path style_path(const std::string &preset) { return presets_folder() / (preset + std::string(preset_extension)); }
std::vector<std::string> list_presets(const std::string &in_use) {
    std::vector<std::string> names{in_use};
    std::error_code error;
    for (std::filesystem::directory_iterator at(presets_folder(), error), end; !error && at != end; at.increment(error)) {
        // A preset name is ASCII. Other names are skipped before conversion, which throws for some of them.
        const auto wide = at->path().filename().wstring();
        if (!std::ranges::all_of(wide, [](wchar_t c) { return c > 0 && c < 0x80; })) continue;
        std::string file;
        for (const auto c : wide) file.push_back(static_cast<char>(c));
        if (file.size() <= preset_extension.size() || !file.ends_with(preset_extension)) continue;
        auto name = file.substr(0, file.size() - preset_extension.size());
        if (style::preset_name(name) && name != in_use) names.push_back(std::move(name));
    }
    std::ranges::sort(names);
    return names;
}
// The preset in use is remembered beside the presets.
void remember_preset(const std::string &preset) {
    std::error_code error;
    std::filesystem::create_directories(presets_folder(), error);
    std::ofstream(presets_folder() / L"in-use.txt", std::ios::binary | std::ios::trunc) << preset;
}
void read_style(Settings &s);
// Client thread. Reads the switches and the preset in use one time.
void load(Settings &s) {
    s.loaded = true;
    s.saved_enabled = s.enabled = profile_runtime::local_preference("Style.Enabled").value_or(false);
    s.saved_share = s.share = profile_runtime::local_preference("Style.Share").value_or(true);
    s.saved_auto_save = s.auto_save = profile_runtime::local_preference("Style.AutoSave").value_or(true);
    live().share.store(s.share, std::memory_order_release);
    std::string remembered;
    std::ifstream(presets_folder() / L"in-use.txt", std::ios::binary) >> remembered;
    std::error_code error;
    if (style::preset_name(remembered) && std::filesystem::exists(style_path(remembered), error)) s.preset = remembered;
    read_style(s);
}
void read_style(Settings &s) {
    s.style = s.on_disk = {};
    s.dirty = true;
    s.unsaved = false;
    s.history.clear();
    s.keep_file = false;
    s.file_issue.clear();
    s.presets_at = 0;
    try {
        const auto path = style_path(s.preset);
        std::error_code error;
        if (!std::filesystem::exists(path, error)) return;
        if (std::filesystem::file_size(path, error) > style::maximum_style_bytes || error) throw std::runtime_error("the style file is too large");
        std::ifstream stream(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        s.style = s.on_disk = style::decode_style(text);
        s.dirty = true;
    } catch (const std::exception &failure) {
        s.file_issue = failure.what();
        std::error_code error;
        auto aside = style_path(s.preset);
        aside += L".unreadable";
        std::filesystem::copy_file(style_path(s.preset), aside, std::filesystem::copy_options::overwrite_existing, error);
        // Without a copy, the saved file is the only one: this session does not write over it.
        s.keep_file = static_cast<bool>(error);
        logging::log(logging::Level::warning, logging::Channel::skater,
                     "Style: could not load the preset {} ({}). A copy is beside it, with .unreadable added.", s.preset, s.file_issue);
    }
}
void save(Settings &s) {
    s.save_at = 0;
    if (s.keep_file) return;
    try {
        const auto path = style_path(s.preset);
        const auto manifest = path.parent_path().parent_path() / mods::manifest_file;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!std::filesystem::exists(manifest, error)) {
            auto root = Json::object();
            root["name"] = "My Styles";
            root["author"] = "";
            root["version_number"] = "1.0.0";
            root["description"] = "Styles made in the ReSkate style editor.";
            std::ofstream(manifest, std::ios::binary) << root.dump(2) << "\n";
        }
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << style::encode_style(s.style);
            stream.flush();
            if (!stream) throw std::runtime_error("could not write the style file");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("could not replace the style file");
        s.file_issue.clear();
        s.on_disk = s.style;
        s.unsaved = false;
    } catch (const std::exception &failure) {
        s.file_issue = failure.what();
        logging::log(logging::Level::warning, logging::Channel::skater, "Style: not saved: {}.", s.file_issue);
    }
}
// The change shows immediately. With auto save, the save follows 750 ms later.
void mark(Settings &s) {
    s.dirty = true;
    if (s.auto_save) s.save_at = GetTickCount64() + 750;
    else s.unsaved = s.style != s.on_disk;
}
// `before` is the style before the edit named `what`. An edit that changes nothing makes no undo step.
void changed(Settings &s, const style::Style &before, std::string what) {
    if (s.style == before) return;
    s.history.record(before, std::move(what));
    mark(s);
}
} // namespace
void request_enabled(bool enabled) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.enabled = enabled;
}
void request_share(bool share) {
    live().share.store(share, std::memory_order_release);
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.share = share;
}
bool request_joint(style::Target target, std::string_view joint, float x, float y, float z, std::string &error) {
    const auto known = std::ranges::find_if(style::editable_joints, [&](std::string_view name) { return style::same_text(name, joint); });
    if (known == style::editable_joints.end() ||
        target.id >= (target.trick ? style::flip_trick_names.size() : style::family_count) || (target.trick && !target.id) ||
        (!target.trick && target.key)) {
        error = "A style cannot rotate that joint.";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        error = "Angles must be numbers.";
        return false;
    }
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (target.trick && target.key >= s.style.keys(target.id).size()) {
        error = "That trick does not have that keyframe.";
        return false;
    }
    const auto before = s.style;
    const std::pair key{target, std::string(*known)};
    if (x == 0 && y == 0 && z == 0) s.style.rotations.erase(key);
    else s.style.rotations[key] = {x, y, z};
    changed(s, before, "change " + std::string(*known));
    return true;
}
void request_clear(std::optional<style::Target> target) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    const auto before = s.style;
    if (target) {
        std::erase_if(s.style.rotations, [&](const auto &entry) {
            return entry.first.first.trick == target->trick && entry.first.first.id == target->id;
        });
        if (target->trick) {
            s.style.times.erase(target->id);
            std::erase_if(s.style.blend_outs, [&](const auto &entry) { return entry.first.id == target->id; });
        }
    } else s.style = {};
    changed(s, before, target && target->trick ? "reset trick" : "reset");
}
namespace {
bool valid_trick(std::uint8_t trick) noexcept { return trick >= 1 && trick < style::flip_trick_names.size(); }
float on_timeline(float time) noexcept { return std::clamp(std::isfinite(time) ? time : 0.0f, 0.02f, style::trick_end - 0.02f); }
} // namespace
int request_key_add(std::uint8_t trick, float time, std::string &error) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    auto times = s.style.keys(trick);
    if (!valid_trick(trick) || times.size() >= style::max_keys) {
        error = "That trick cannot have more keyframes.";
        return -1;
    }
    const auto before_add = s.style;
    time = on_timeline(time);
    // The new keyframe starts as the pose shown at that time, at the shown clip's pace, so the addition changes nothing on screen.
    const auto added = static_cast<std::uint8_t>(times.size());
    std::vector<style::JointDelta> shown;
    style::evaluate(style::trick_keys(s.style, trick), time, shown, s.editor_pace);
    for (const auto &delta : shown) {
        const auto degrees = style::to_degrees(delta.rotation);
        if (std::abs(degrees[0]) > 0.01f || std::abs(degrees[1]) > 0.01f || std::abs(degrees[2]) > 0.01f)
            s.style.rotations[{style::Target{true, trick, added}, std::string(style::editable_joints[delta.joint])}] = degrees;
    }
    times.push_back(time);
    s.style.times[trick] = std::move(times);
    changed(s, before_add, "add keyframe");
    return static_cast<int>(s.style.times[trick].size()) - 1;
}
bool request_key_move(std::uint8_t trick, std::uint8_t key, float time, std::string &error) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    auto times = s.style.keys(trick);
    if (!valid_trick(trick) || key >= times.size()) {
        error = "That trick does not have that keyframe.";
        return false;
    }
    const auto before = s.style;
    times[key] = on_timeline(time);
    s.style.times[trick] = std::move(times);
    changed(s, before, "move keyframe");
    return true;
}
bool request_key_delete(std::uint8_t trick, std::uint8_t key, std::string &error) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    auto times = s.style.keys(trick);
    if (!valid_trick(trick) || key >= times.size()) {
        error = "That trick does not have that keyframe.";
        return false;
    }
    const auto before = s.style;
    times.erase(times.begin() + key);
    s.style.times[trick] = std::move(times);
    // Later keyframes and their rotations move down one number.
    style::Rotations kept;
    for (auto &[entry, degrees] : s.style.rotations) {
        auto target = entry.first;
        if (target.trick && target.id == trick) {
            if (target.key == key) continue;
            if (target.key > key) --target.key;
        }
        kept[{target, entry.second}] = degrees;
    }
    s.style.rotations = std::move(kept);
    style::BlendOuts blends;
    for (const auto &[entry, ms] : s.style.blend_outs) {
        auto target = entry;
        if (target.id == trick) {
            if (target.key == key) continue;
            if (target.key > key) --target.key;
        }
        blends[target] = ms;
    }
    s.style.blend_outs = std::move(blends);
    changed(s, before, "delete keyframe");
    return true;
}
bool request_key_blend_out(std::uint8_t trick, std::uint8_t key, float ms, std::string &error) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (!valid_trick(trick) || key >= s.style.keys(trick).size()) {
        error = "That trick does not have that keyframe.";
        return false;
    }
    const auto before = s.style;
    const style::Target target{true, trick, key};
    if (std::isfinite(ms) && ms > 0) s.style.blend_outs[target] = std::clamp(ms, style::min_blend_out_ms, style::max_blend_out_ms);
    else s.style.blend_outs.erase(target);
    changed(s, before, "blend out");
    return true;
}
void request_reload() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.save_at = 0;
    read_style(s);
}
namespace {
bool travel(bool back) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    auto style = back ? s.history.undo(s.style) : s.history.redo(s.style);
    if (!style) return false;
    s.style = std::move(*style);
    mark(s);
    return true;
}
} // namespace
bool request_undo() { return travel(true); }
bool request_redo() { return travel(false); }
void request_group(bool open) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.history.group(open);
}
void request_history_clear() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.history.clear();
}
void request_save() {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (s.loaded) save(s);
}
void request_auto_save(bool on) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.auto_save = on;
    // Edits that waited for a save are saved now.
    if (on && s.unsaved) s.save_at = GetTickCount64() + 750;
}
bool auto_saving() noexcept {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    return s.auto_save;
}
bool request_preset(std::string_view action, std::string_view name, std::string &error) {
    if (action == "folder") {
        // The shell can take a moment, so it runs off the game thread.
        try {
            std::thread([folder = presets_folder()] {
                std::error_code made;
                std::filesystem::create_directories(folder, made);
                const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (com) CoUninitialize();
            }).detach();
        } catch (...) {
            error = "The presets folder could not be opened.";
            return false;
        }
        return true;
    }
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) load(s);
    if (!style::preset_name(name)) {
        error = "A preset name has letters, digits, '-' and '_', at most 40 of them, and is not a Windows device name such as CON.";
        return false;
    }
    const std::string preset(name);
    std::error_code issue;
    const bool exists = preset == s.preset || std::filesystem::exists(style_path(preset), issue);
    if (action == "load" ? !exists : action == "delete" ? !exists : exists) {
        error = exists ? "A preset of that name exists." : "There is no preset of that name.";
        return false;
    }
    // Without auto save, a switch away from unsaved edits waits until they are saved or discarded. A copy takes them along.
    if (s.unsaved && (action == "load" || action == "new" || (action == "delete" && preset == s.preset))) {
        error = "The preset has unsaved changes. Save or discard them first.";
        return false;
    }
    // With auto save, edits not yet written go to the preset they were made in.
    if (s.save_at) save(s);
    if (action == "load") {
        s.preset = preset;
        read_style(s);
    } else if (action == "new" || action == "copy") {
        if (action == "new") s.style = {};
        s.preset = preset;
        s.keep_file = false;
        s.dirty = true;
        s.history.clear();
        save(s);
    } else if (action == "delete") {
        std::filesystem::remove(style_path(preset), issue);
        if (issue) {
            error = "The preset could not be deleted.";
            return false;
        }
        if (preset == s.preset) {
            // Another preset takes its place, or an empty "default".
            const auto left = list_presets("default");
            s.preset = left.size() > 1 && left.front() == "default" && !std::filesystem::exists(style_path("default"), issue) ? left[1] : left.front();
            read_style(s);
        }
    } else {
        error = "Use load, new, copy, delete or folder.";
        return false;
    }
    remember_preset(s.preset);
    s.presets_at = 0;
    return true;
}
style::StyleModel model() {
    style::StyleModel result;
    auto &s = settings();
    {
        std::lock_guard lock(s.mutex);
        result.enabled = s.enabled;
        result.share = s.share;
        result.saved = !s.save_at && !s.unsaved && s.file_issue.empty();
        result.auto_save = s.auto_save;
        result.unsaved = s.unsaved;
        result.save_issue = s.file_issue;
        if (const auto *name = s.history.undo_name()) result.undo_name = *name;
        if (const auto *name = s.history.redo_name()) result.redo_name = *name;
        for (const auto &[key, degrees] : s.style.rotations) {
            const auto joint = std::ranges::find(style::editable_joints, std::string_view(key.second));
            if (joint != style::editable_joints.end())
                result.rotations.push_back({key.first, static_cast<std::uint8_t>(joint - style::editable_joints.begin()), degrees});
        }
        result.times = s.style.times;
        result.blend_outs = s.style.blend_outs;
        // The folder is listed again every two seconds: a preset can be copied in while the game runs.
        if (const auto now = GetTickCount64(); now >= s.presets_at) {
            s.presets_at = now + 2000;
            try {
                s.presets = list_presets(s.preset);
            } catch (...) {
                s.presets = {s.preset};
            }
        }
        result.preset = s.preset;
        result.presets = s.presets;
    }
    result.preview = live().preview.load(std::memory_order_acquire);
    result.preview_playing = live().preview_played.load(std::memory_order_acquire) != 0;
    result.preview_time = preview_time(GetTickCount64());
    return result;
}
bool enabled() noexcept {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    return s.enabled;
}
void request_preview(std::uint8_t trick, float time, bool play) {
    auto &l = live();
    if (trick >= style::flip_trick_names.size()) trick = 0;
    l.preview_time.store(std::bit_cast<std::uint32_t>(std::clamp(std::isfinite(time) ? time : 0.0f, 0.0f, style::trick_end)),
                         std::memory_order_release);
    l.preview_played.store(play ? GetTickCount64() : 0, std::memory_order_release);
    l.preview.store(trick, std::memory_order_release);
}
void note_editor_pace(const style::Pace &pace) {
    auto &s = settings();
    std::lock_guard lock(s.mutex);
    s.editor_pace = pace;
}
void rotations_at(std::uint8_t trick, float time, const style::Pace &pace, std::vector<style::JointDelta> &out) {
    out.clear();
    const auto snapshot = live().snapshot.load(std::memory_order_acquire);
    if (!snapshot || trick >= snapshot->tricks.size()) return;
    std::vector<style::JointDelta> timeline;
    merged(*snapshot, Family::riding, trick, time, pace, timeline, out);
}
void ignore_holder(std::uintptr_t holder, std::uintptr_t board) noexcept {
    live().ignored.store(holder, std::memory_order_release);
    live().ignored_board.store(board, std::memory_order_release);
}
bool stage_present() noexcept { return GetTickCount64() < live().stage_seen.load(std::memory_order_relaxed) + 500; }
bool add_demo(std::uint8_t trick, float time, const std::vector<multiplayer::Transform> &skater) {
    auto &l = live();
    const auto snapshot = l.snapshot.load(std::memory_order_acquire);
    if (!snapshot || trick >= style::flip_trick_names.size()) return false;
    style::TakeFrame frame{trick, std::clamp(time, 0.0f, style::trick_end), {}, {}};
    for (std::size_t i = 0; i < frame.shown.size(); ++i) {
        const auto joint = snapshot->signature[i];
        if (!joint || joint >= skater.size()) return false;
        frame.shown[i] = skater[joint].rotation;
    }
    std::lock_guard lock(l.pose_mutex);
    l.demos.add(std::move(frame));
    return true;
}
void clear_demos() {
    auto &l = live();
    std::lock_guard lock(l.pose_mutex);
    l.demos = {};
}
void keep_stage_clear() noexcept { live().clear_stage.store(GetTickCount64() + 500, std::memory_order_relaxed); }
void watch_stage() noexcept { live().stage_watch.store(GetTickCount64() + 2000, std::memory_order_relaxed); }
Rig rig() {
    const auto snapshot = live().snapshot.load(std::memory_order_acquire);
    return snapshot ? snapshot->rig : Rig{};
}
std::vector<std::uint8_t> collect_learned() {
    auto &l = live();
    std::lock_guard lock(l.pose_mutex);
    return std::exchange(l.learned_done, {});
}
void request_learn() { live().learn_until.store(GetTickCount64() + 8000, std::memory_order_release); }
void request_session_test(bool allowed) { live().session_test.store(allowed, std::memory_order_release); }
bool session_test() noexcept { return live().session_test.load(std::memory_order_acquire); }
void request_restyle(bool on) { live().restyle.store(on, std::memory_order_release); }
bool restyling() noexcept { return live().restyle.load(std::memory_order_acquire); }
style::Playhead replay_playhead() noexcept {
    auto &l = live();
    if (GetTickCount64() - l.replay_seen.load(std::memory_order_relaxed) > 300) return {};
    return {static_cast<std::uint8_t>(l.replay_trick.load(std::memory_order_relaxed)),
            std::bit_cast<float>(l.replay_time.load(std::memory_order_relaxed))};
}
bool sharing() noexcept { return live().share.load(std::memory_order_acquire); }
std::string status() {
    auto &l = live();
    std::string head;
    std::size_t joints{};
    bool share{};
    {
        auto &s = settings();
        std::lock_guard lock(s.mutex);
        joints = s.style.rotations.size();
        share = s.share;
        head = !s.enabled ? "off" : !s.issue.empty() ? "unavailable: " + s.issue
               : !l.component.load(std::memory_order_acquire) ? "waiting for the local skater"
               : !l.active.load(std::memory_order_acquire) ? "suspended during the throwdown" : "applied";
    }
    return std::format("Style {} ({} joint rotation(s), {}); state {}, trick {}, calls {}, unchanged-pose calls {}, pose buffers {}, "
                       "failures {}, preview {} on {} other pose(s), replay frames matched {} missed {} nearest {:.5f}",
                       head, joints, share ? "shown to others" : "hidden from others", l.state.load(std::memory_order_relaxed),
                       style::flip_trick_names[l.shown_trick.load(std::memory_order_relaxed) % style::flip_trick_names.size()],
                       l.render_calls.load(std::memory_order_relaxed), l.reused_calls.load(std::memory_order_relaxed),
                       l.buffer_changes.load(std::memory_order_relaxed), l.failures.load(std::memory_order_relaxed),
                       style::flip_trick_names[l.preview.load(std::memory_order_relaxed) % style::flip_trick_names.size()],
                       l.other_calls.load(std::memory_order_relaxed),
                       l.replay_matches.load(std::memory_order_relaxed), l.replay_misses.load(std::memory_order_relaxed),
                       std::bit_cast<float>(l.replay_distance.load(std::memory_order_relaxed)));
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool menu_open) noexcept {
    auto &l = live();
    try {
        if (!menu_open) l.preview.store(0, std::memory_order_release);
        // The editor collects a finished recording.
        if (const auto until = l.learn_until.load(std::memory_order_relaxed); until && GetTickCount64() > until + 200) {
            l.learn_until.store(0, std::memory_order_relaxed);
            std::vector<std::uint8_t> recorded;
            {
                std::lock_guard lock(l.pose_mutex);
                recorded.swap(l.learned);
            }
            logging::log(logging::Level::info, logging::Channel::skater, "Style: recorded {} bytes of other rigs.", recorded.size());
            if (recorded.empty()) recorded.push_back(0); // an empty recording must also reach the editor
            std::lock_guard lock(l.pose_mutex);
            l.learned_done = std::move(recorded);
        }
        auto &s = settings();
        {
            std::lock_guard lock(s.mutex);
            if (!s.loaded) load(s);
            if (s.save_at && GetTickCount64() >= s.save_at) save(s);
            if (s.enabled != s.saved_enabled) profile_runtime::set_local_preference("Style.Enabled", s.saved_enabled = s.enabled);
            if (s.share != s.saved_share) profile_runtime::set_local_preference("Style.Share", s.saved_share = s.share);
            if (s.auto_save != s.saved_auto_save) profile_runtime::set_local_preference("Style.AutoSave", s.saved_auto_save = s.auto_save);
            // The listener continues until it has restored the game's rotations, then stops.
            if (!s.enabled) {
                l.active.store(false, std::memory_order_release);
                std::lock_guard pose(l.pose_mutex);
                if (l.tracker.adjusted().empty() && std::ranges::none_of(l.others, [](const Live::Other &other) { return other.holder != 0; })) {
                    l.holder.store(0, std::memory_order_release);
                    l.component.store(0, std::memory_order_release);
                }
                return;
            }
            if (!ready || !base) {
                l.holder.store(0, std::memory_order_release);
                l.component.store(0, std::memory_order_release);
                return;
            }
            // No Bail verified the physics state selector before its hook, so the bytes cannot be compared here.
            if (!no_bail_available()) {
                refuse(s, "this game build's physics state code is not the known one");
                return;
            }
            if (!s.skeleton_started) {
                s.skeleton_started = true;
                read_skeleton();
            }
            if (!s.skeleton) return;
            if (!s.hooks_tried) {
                s.hooks_tried = true;
                std::string detail;
                s.hooks = multiplayer::install_entity_hooks(base, detail);
                if (s.hooks) {
                    multiplayer::set_render_pose_style_listener(&on_render);
                    multiplayer::set_local_pose_filter(&filter_capture);
                } else {
                    refuse(s, "the animation hook is unavailable: " + detail);
                }
            }
            if (!s.hooks) return;
            if (s.dirty) {
                s.dirty = false;
                l.snapshot.store(build(s), std::memory_order_release);
            }
        }
        const auto frame = multiplayer::capture_local(base, client, false);
        std::uintptr_t component{}, core{}, context{};
        if (frame.ready && (component = memory::peek_pointer(frame.entity, addr::style::entity_component)) != 0 &&
            memory::peek_pointer(component) == base + addr::engine::skater_component_vtable &&
            (core = memory::peek_pointer(component, component_core)) != 0 && memory::peek_pointer(core) == base + addr::no_bail::bail_core_vtable)
            context = memory::peek_pointer(core, core_context);
        const auto holder = memory::peek_pointer(component, addr::style::component_pose_holder);
        if (!context || !holder) {
            l.component.store(0, std::memory_order_release);
            l.holder.store(0, std::memory_order_release);
            return;
        }
        l.holder.store(holder, std::memory_order_release);
        l.trick_selection.store(memory::peek_pointer(core, core_trick_selection), std::memory_order_release);
        l.base.store(base, std::memory_order_release);
        l.context.store(context, std::memory_order_release);
        l.active.store(!multiplayer::local_throwdown_active(), std::memory_order_release);
        if (l.component.exchange(component, std::memory_order_acq_rel) != component)
            logging::log(logging::Level::info, logging::Channel::skater, "Style: following the local skater (component {:#x}).",
                         component);
    } catch (...) {
        l.failures.fetch_add(1, std::memory_order_relaxed);
    }
}
} // namespace dingosdk::style_layer
