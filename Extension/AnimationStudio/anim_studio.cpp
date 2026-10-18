#include "anim_studio.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>

namespace dingosdk::anim_studio {
namespace {
using logging::Channel;
using logging::Level;

// ---- guarded memory --------------------------------------------------------------------------------
constexpr std::uintptr_t highest_user = 0x00007fffffffffffULL;
bool peek(std::uintptr_t address, void* out, std::size_t size) noexcept {
    if (address < 0x10000 || address > highest_user - size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool poke(std::uintptr_t address, const void* data, std::size_t size) noexcept {
    if (address < 0x10000 || address > highest_user - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> std::optional<T> read(std::uintptr_t address) noexcept {
    T value{};
    if (!peek(address, &value, sizeof(T))) return std::nullopt;
    return value;
}
struct alignas(16) NativeBone {
    std::array<float, 4> scale, rotation, position;
};
static_assert(sizeof(NativeBone) == 0x30);

double now_seconds() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / frequency;
}

// ---- shared state --------------------------------------------------------------------------------
// A saved trick the game plays in place of its own: matched by the game's name for the trick.
struct CustomTrick {
    std::string trick; // the game's name ("ollie", "kickflip"), lower case
    std::string name;  // the saved animation's
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Animation> edits;
};
using CustomTricks = std::vector<CustomTrick>;
struct Snapshot {
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Animation> edits; // tracks only; its clip is empty
    float time{};
    bool open{}, play_in_game{}, armed{};
    float speed{1};
    std::shared_ptr<const CustomTricks> tricks;
};
struct History {
    std::vector<Track> tracks;
};
struct State {
    std::mutex mutex;
    // Document.
    std::shared_ptr<const Clip> clip;
    Animation edits;
    std::string saved_as; // the library name it was loaded from or saved to
    bool dirty{};
    std::deque<History> undo, redo;
    std::vector<std::pair<std::uint16_t, Offset>> clipboard;
    // Playback.
    float time{};
    bool playing{}, play_in_game{true};
    std::shared_ptr<const CustomTricks> tricks;
    bool tricks_read{};
    // Mode.
    bool open{}, open_requested{}, open_request_pending{}, confirm_close{};
    Recording recording{};
    std::string status, failure;
    // Published by the animation thread.
    std::vector<JointView> joints;
    Vec3 centre{};
    Quat facing{0, 0, 0, 1};
    bool centre_valid{};
    double pose_seen_at{};
    // Camera.
    Orbit orbit;
    Vec3 focus_offset{}; // eased towards the focused joint
    Vec3 eased_centre{};
    bool eased_valid{};
    // The skater's facing when the studio opened: the views (front, side...) are from it, and the
    // camera does not swing when the skater turns on the spot meanwhile.
    float base_yaw{};
    bool base_yaw_valid{};
    std::optional<std::uint16_t> focus;
    float view_shift{}, view_shift_x{};
    std::array<float, 16> camera{};
    float fov{};
    bool camera_valid{};
    std::uint64_t revision{1};
    std::vector<std::string> library;
    bool library_read{};
    std::shared_ptr<const Snapshot> snapshot;
};
State& state() {
    static auto* s = new State;
    return *s;
}
// One transform after another (parent, then child in the parent's frame); uniform scale.
Joint compose(const Joint& parent, const Joint& child) noexcept {
    const float k = (parent.scale[0] + parent.scale[1] + parent.scale[2]) / 3.0f;
    const auto moved = rotate(parent.rotation, {child.position[0] * k, child.position[1] * k, child.position[2] * k});
    Joint out;
    out.position = {parent.position[0] + moved[0], parent.position[1] + moved[1], parent.position[2] + moved[2]};
    out.rotation = normalised(multiply(parent.rotation, child.rotation));
    const float c = (child.scale[0] + child.scale[1] + child.scale[2]) / 3.0f;
    out.scale = {k * c, k * c, k * c};
    return out;
}
// `world` seen from `parent`'s frame.
Joint relative(const Joint& parent, const Joint& world) noexcept {
    const float k = std::max((parent.scale[0] + parent.scale[1] + parent.scale[2]) / 3.0f, 1e-4f);
    const auto back = conjugate(parent.rotation);
    const auto d = rotate(back, {world.position[0] - parent.position[0], world.position[1] - parent.position[1], world.position[2] - parent.position[2]});
    Joint out;
    out.position = {d[0] / k, d[1] / k, d[2] / k};
    out.rotation = normalised(multiply(back, world.rotation));
    const float c = (world.scale[0] + world.scale[1] + world.scale[2]) / 3.0f;
    out.scale = {c / k, c / k, c / k};
    return out;
}
// The board's rig (board_break.cpp): 16 bones, 0 its reference, 1 its place in the world, 2 the deck's
// mount, which carries the deck, the trucks and the wheels. Moving bone 2 moves the whole board.
constexpr std::uint32_t board_bones = 16;
constexpr std::size_t board_mount = 2;
Joint joint_of(const NativeBone& b) noexcept {
    Joint j;
    j.scale = {b.scale[0], b.scale[1], b.scale[2]};
    j.rotation = b.rotation;
    j.position = {b.position[0], b.position[1], b.position[2]};
    return j;
}
// The frame bone 2 hangs in, in the world.
Joint board_parent(const std::array<NativeBone, board_bones>& b) noexcept { return compose(joint_of(b[0]), joint_of(b[1])); }

// The animation thread's own, never touched elsewhere.
struct Live {
    std::atomic<std::uintptr_t> base{}, component{}, board_holder{};
    std::vector<NativeBone> bones;
    std::vector<Joint> pose, sampled;
    // Recording.
    struct Frame {
        double at{};
        std::vector<Joint> joints;
        Joint board; // relative to the placement joint
        bool has_board{};
    };
    std::array<NativeBone, board_bones> board{};
    bool board_ok{};
    double board_vy{};      // the board's upward speed in the first frames of the air (m/s)
    float predicted_air{}; // from it: how long this air lasts
    std::deque<Frame> ring;
    std::vector<std::vector<Joint>> spare;
    bool airborne{};
    int ground_count{};
    double takeoff_at{}, landing_at{};
    // In-game playback.
    bool game_air{};
    double game_takeoff{}, game_landing{-1};
    int game_ground{};
    const CustomTrick* game_trick{}; // the saved trick playing now, from snapshot `game_tricks`
    std::shared_ptr<const CustomTricks> game_tricks;
    std::string recorded_trick; // the game's name of the trick being recorded
    std::vector<Joint> custom;
    std::uint64_t failures{};
    bool logged_layout{};
};
// The pose to draw, from the animation thread to the render publication (engine thread).
struct Pending {
    std::mutex mutex;
    std::uintptr_t buffer{};
    std::uint32_t first{}, last{};
    std::vector<NativeBone> bones; // [first, last)
    double at{};
};
// The board to draw: where its mount goes in the world, and how much of that (blending).
struct PendingBoard {
    std::mutex mutex;
    Joint mount;
    float weight{};
    double at{};
};
PendingBoard& pending_board() {
    static auto* p = new PendingBoard;
    return *p;
}
Pending& pending() {
    static auto* p = new Pending;
    return *p;
}
std::atomic<bool> open_flag{};
Live& live() {
    static auto* l = new Live;
    return *l;
}

std::filesystem::path library_folder() {
    std::array<wchar_t, 32768> buffer{};
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_ANIMATION_STUDIO_DIR", buffer.data(), static_cast<DWORD>(buffer.size()));
        length && length < buffer.size())
        return std::wstring(buffer.data(), length);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.Tricks";
}
void read_library_locked(State& s) {
    s.library.clear();
    s.library_read = true;
    std::error_code error;
    const auto folder = library_folder();
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.is_regular_file(error) && entry.path().extension() == L".json") s.library.push_back(entry.path().stem().string());
    std::sort(s.library.begin(), s.library.end());
}

// Every saved animation with a trick, ready to play in game. Read from the files, outside the lock.
std::shared_ptr<const CustomTricks> read_custom_tricks() {
    auto tricks = std::make_shared<CustomTricks>();
    std::error_code error;
    const auto folder = library_folder();
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return tricks;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != L".json") continue;
        try {
            std::ifstream json_in(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>());
            auto animation = std::make_shared<Animation>(from_json(text));
            if (animation->trick.empty()) continue;
            auto clip_path = entry.path();
            clip_path.replace_extension(L".clip");
            std::ifstream clip_in(clip_path, std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
            auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
            if (clip->joints != skeleton_joints || clip->takeoff < 0) continue;
            std::string trick;
            for (const char c : animation->trick) trick += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            tricks->push_back({trick, entry.path().stem().string(), std::move(clip), std::move(animation)});
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: {} was not loaded: {}", entry.path().filename().string(), e.what());
        }
    }
    std::string names;
    for (const auto& t : *tricks) names += (names.empty() ? "" : ", ") + t.trick;
    logging::log(Level::info, Channel::skater, "Animation Studio: {} custom tricks ready{}{}.", tricks->size(), names.empty() ? "" : ": ", names);
    return tricks;
}

// Publishes what the animation thread works from. Caller holds the mutex.
void publish_locked(State& s) {
    auto snap = std::make_shared<Snapshot>();
    snap->clip = s.clip;
    snap->tricks = s.tricks;
    auto edits = std::make_shared<Animation>();
    edits->tracks = s.edits.tracks;
    edits->speed = s.edits.speed;
    snap->edits = std::move(edits);
    snap->time = s.time;
    snap->open = s.open;
    snap->play_in_game = s.play_in_game;
    snap->armed = s.recording != Recording::off;
    snap->speed = s.edits.speed;
    s.snapshot = std::move(snap);
    ++s.revision;
}
std::shared_ptr<const Snapshot> snapshot() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.snapshot;
}
float duration_locked(const State& s) { return s.clip ? s.clip->duration() : 0.0f; }
void changed_locked(State& s) {
    s.dirty = true;
    publish_locked(s);
}

// ---- the camera --------------------------------------------------------------------------------
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 unit(const Vec3& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 1e-6f ? Vec3{v[0] / l, v[1] / l, v[2] / l} : Vec3{0, 0, 1};
}
// The skater's facing about the up axis, from its placement joint: the angle of its forward on the
// ground. Measured 2026-10-18 by orbiting a recorded trick: a camera on the joint's -Z side shows the
// skater's left side, a quarter turn on his back; so the front is a quarter turn the other way.
float facing_yaw(const Quat& facing) {
    const auto forward = rotate(facing, {0, 0, -1});
    return std::atan2(forward[0], forward[2]) - 1.5707963f;
}
std::array<float, 16> orbit_matrix(const Vec3& centre, const Orbit& orbit, float base_yaw) {
    const float yaw = base_yaw + orbit.yaw;
    const float pitch = std::clamp(orbit.pitch, -1.45f, 1.45f);
    const Vec3 back{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 focus{centre[0] + orbit.pan[0], centre[1] + orbit.pan[1], centre[2] + orbit.pan[2]};
    const float distance = std::clamp(orbit.distance, 0.6f, 25.0f);
    const auto right = unit(cross({0, 1, 0}, back));
    const auto up = cross(back, right);
    std::array<float, 16> m{};
    for (std::size_t i = 0; i < 3; ++i) {
        m[i] = right[i];
        m[4 + i] = up[i];
        m[8 + i] = back[i];
        m[12 + i] = focus[i] + back[i] * distance;
    }
    m[15] = 1;
    return m;
}

// ---- recording ---------------------------------------------------------------------------------
constexpr double before_takeoff = 0.7, after_landing = 0.7, ring_seconds = 2.5;
constexpr double shortest_air = 0.25, longest_air = 8.0;
void finish_recording(Live& l) {
    auto clip = std::make_shared<Clip>();
    clip->joints = skeleton_joints;
    const double start = l.takeoff_at - before_takeoff, end = l.landing_at + after_landing;
    double first = -1;
    bool all_board = true;
    for (const auto& frame : l.ring) {
        if (frame.at < start || frame.at > end) continue;
        if (first < 0) first = frame.at;
        clip->times.push_back(static_cast<float>(frame.at - first));
        clip->frames.insert(clip->frames.end(), frame.joints.begin(), frame.joints.end());
        clip->board.push_back(frame.board);
        all_board &= frame.has_board;
    }
    if (!all_board) clip->board.clear();
    if (clip->times.size() < 8 || first < 0) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.status = "The recording was too short. Try again.";
        s.recording = Recording::waiting;
        return;
    }
    clip->takeoff = static_cast<float>(std::max(0.0, l.takeoff_at - first));
    clip->landing = static_cast<float>(std::max(0.0, l.landing_at - first));
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.clip = std::move(clip);
    // Keys from an earlier clip stay, held inside the new one.
    for (auto& track : s.edits.tracks)
        for (auto& key : track.keys) key.time = std::min(key.time, s.clip->duration());
    s.time = s.clip->takeoff;
    s.playing = false;
    s.recording = Recording::off;
    if (!l.recorded_trick.empty()) {
        // Named after the trick the game says it was: saving it makes it play on that trick.
        s.edits.trick = l.recorded_trick;
        s.edits.name = l.recorded_trick;
    }
    s.open_requested = true;
    s.open_request_pending = true;
    s.dirty = true;
    s.status = std::format("Recorded {} ({:.2f} s in the air). Save it to play it in place of the game's {}.",
                           l.recorded_trick.empty() ? std::string("a trick") : l.recorded_trick, s.clip->landing - s.clip->takeoff,
                           l.recorded_trick.empty() ? std::string("trick") : l.recorded_trick);
    publish_locked(s);
    logging::log(Level::info, Channel::skater, "Animation Studio: {}", s.status);
}
void record(Live& l, double at, bool air, const std::string& trick) {
    // Keep a few seconds of poses; reuse the vectors that fall out.
    std::vector<Joint> joints;
    if (!l.spare.empty()) {
        joints = std::move(l.spare.back());
        l.spare.pop_back();
    }
    joints.assign(l.pose.begin(), l.pose.end());
    Live::Frame frame{at, std::move(joints), {}, false};
    if (l.board_ok) {
        frame.board = relative(world(l.pose, placement_joint), compose(board_parent(l.board), joint_of(l.board[board_mount])));
        frame.has_board = finite(frame.board);
    }
    l.ring.push_back(std::move(frame));
    const double keep_from = l.airborne || l.landing_at > 0 ? l.takeoff_at - before_takeoff - 0.1 : at - ring_seconds;
    while (!l.ring.empty() && l.ring.front().at < keep_from) {
        l.spare.push_back(std::move(l.ring.front().joints));
        l.ring.pop_front();
    }
    auto& s = state();
    if (!l.airborne && l.landing_at <= 0) {
        if (air) {
            l.airborne = true;
            l.takeoff_at = at;
            l.ground_count = 0;
            l.recorded_trick.clear();
            std::lock_guard lock(s.mutex);
            s.recording = Recording::in_air;
            s.status = "In the air...";
        }
        return;
    }
    if (l.airborne) {
        if (air) {
            if (!trick.empty() && at - l.takeoff_at < 0.3) l.recorded_trick = trick;
            l.ground_count = 0;
            if (at - l.takeoff_at > longest_air) {
                l.airborne = false;
                l.ring.clear();
                std::lock_guard lock(s.mutex);
                s.recording = Recording::waiting;
                s.status = "That was not a trick (too long in the air). Waiting for the next one.";
            }
            return;
        }
        if (++l.ground_count < 3) return; // a frame or two of contact mid-air is not a landing
        l.airborne = false;
        if (at - l.takeoff_at < shortest_air) {
            std::lock_guard lock(s.mutex);
            s.recording = Recording::waiting;
            s.status = "Too short a hop. Waiting for a trick.";
            return;
        }
        l.landing_at = at;
        std::lock_guard lock(s.mutex);
        s.recording = Recording::finishing;
        s.status = "Landed. Finishing the recording...";
        return;
    }
    if (at - l.landing_at >= after_landing) {
        finish_recording(l);
        l.landing_at = -1;
        l.ring.clear();
    }
}
void reset_recording(Live& l) {
    l.airborne = false;
    l.landing_at = -1;
    l.ground_count = 0;
    for (auto& f : l.ring) l.spare.push_back(std::move(f.joints));
    l.ring.clear();
}

// The local skater's physics state (no_bail.cpp's selector): component +0x70 the core, core +0x3c0 its
// context, context +0x1414 the state. In the air on the board: 200, 201.
std::optional<bool> in_the_air(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto core = read<std::uintptr_t>(component + 0x70);
    if (!core || !*core) return std::nullopt;
    const auto type = read<std::uintptr_t>(*core);
    if (!type || *type != base + addr::no_bail::bail_core_vtable) return std::nullopt;
    const auto context = read<std::uintptr_t>(*core + 0x3c0);
    if (!context || !*context) return std::nullopt;
    const auto value = read<std::uint32_t>(*context + addr::skater_state::physics_state_offset);
    if (!value) return std::nullopt;
    return std::find(addr::skater_state::board_air_states.begin(), addr::skater_state::board_air_states.end(), *value) !=
           addr::skater_state::board_air_states.end();
}

// The game's name for the trick the skater is doing ("ollie", "kickflip", "heelflip"): the animation
// holder (component +0xa0) -> +0x20 -> +0x250 -> +0x98, a C string. Found 2026-10-18 by walking the
// holder's objects mid-trick; right from 0.05 s after takeoff and through the whole air, the same on
// six tricks of three kinds. Empty when the chain or the text does not look right.
std::string current_trick(std::uintptr_t component) noexcept {
    const auto holder = read<std::uintptr_t>(component + 0xa0).value_or(0);
    const auto a = holder ? read<std::uintptr_t>(holder + 0x20).value_or(0) : 0;
    const auto b = a ? read<std::uintptr_t>(a + 0x250).value_or(0) : 0;
    const auto c = b ? read<std::uintptr_t>(b + 0x98).value_or(0) : 0;
    char text[49]{};
    if (!c || !peek(c, text, 48)) return {};
    std::string name;
    for (std::size_t i = 0; i < 48 && text[i]; ++i) {
        const char ch = text[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ' ' || ch == '-')) return {};
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return name.size() >= 2 ? name : std::string{};
}
// The body: Hips (7) and every joint below it. A saved trick replaces these only, never the placement
// joints the game moves the skater with, nor the board's bones in the skater's rig (376 on).
const std::array<bool, skeleton_joints>& body_joints() {
    static const auto mask = [] {
        std::array<bool, skeleton_joints> m{};
        for (std::uint16_t i = 0; i < skeleton_joints; ++i)
            for (int at = i; at >= 0; at = parent(static_cast<std::uint16_t>(at)))
                if (at == hips_joint) {
                    m[i] = true;
                    break;
                }
        return m;
    }();
    return mask;
}

void publish_joints(std::span<const Joint> pose, const Joint& placement, const std::optional<Joint>& board) {
    std::vector<JointView> views;
    views.reserve(editable_joints().size());
    for (const auto& e : editable_joints()) {
        JointView v;
        if (e.index == board_joint) {
            // The board: its own frame is the one its keys turn and move in.
            if (!board) continue;
            v.joint = board_joint;
            v.position = board->position;
            v.rotation = board->rotation;
            v.parent_rotation = board->rotation;
            views.push_back(v);
            continue;
        }
        v.joint = e.index;
        const auto w = world(pose, e.index);
        v.position = w.position;
        v.rotation = w.rotation;
        if (const auto p = parent(e.index); p >= 0) {
            const auto pw = world(pose, static_cast<std::uint16_t>(p));
            v.parent_rotation = pw.rotation;
            v.parent_scale = pw.scale[0];
        }
        views.push_back(v);
    }
    const auto hips = world(pose, hips_joint);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.joints = std::move(views);
    // The orbit centres where the skater stands, at the height of the hips as drawn, so a trick's rise
    // stays in the picture (camera() eases towards it).
    s.centre = {placement.position[0], hips.position[1] + 0.05f, placement.position[2]};
    s.facing = placement.rotation;
    s.centre_valid = std::isfinite(hips.position[0]);
    s.pose_seen_at = now_seconds();
}
} // namespace

// The board's upward speed (m/s): its physics' root body (skater_state.h: core +0x430 -> +0x18 ->
// bodies at +0x20, body 0 the root, velocity at +0x70).
std::optional<float> board_up_speed(std::uintptr_t base, std::uintptr_t component) noexcept {
    namespace st = addr::skater_state;
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    const auto holder = core ? read<std::uintptr_t>(core + st::board_holder_offset).value_or(0) : 0;
    const auto physics = holder ? read<std::uintptr_t>(holder + st::board_physics_offset).value_or(0) : 0;
    if (!physics || read<std::uintptr_t>(physics).value_or(0) != base + st::board_physics_vtable) return std::nullopt;
    const auto bodies = read<std::uintptr_t>(physics + st::physics_bodies_offset).value_or(0);
    if (!bodies || read<std::uint32_t>(bodies).value_or(0) != st::board_body_count) return std::nullopt;
    if (read<std::uintptr_t>(bodies + st::body_owner_offset).value_or(0) != physics) return std::nullopt;
    const auto v = read<std::array<float, 3>>(bodies + st::body_velocity_offset);
    if (!v || !std::isfinite((*v)[1])) return std::nullopt;
    return (*v)[1];
}
// The pop of an air, from how long it is going to last: low, mid or high.
std::string pop_of(float air) {
    if (!(air > 0)) return {};
    return air < 0.45f ? "low" : air < 0.65f ? "mid" : "high";
}
// The board's edit at `seconds`: its own turn and move, after the recording's place.
Joint board_edited(const Joint& board, const Animation& edits, float seconds) {
    const auto* track = edits.track(board_joint);
    if (!track || track->hidden || track->keys.empty()) return board;
    const auto e = evaluate(*track, seconds);
    Joint out = board;
    const auto moved = rotate(board.rotation, e.offset);
    for (std::size_t i = 0; i < 3; ++i) out.position[i] += moved[i];
    out.rotation = normalised(multiply(board.rotation, e.rotation));
    return out;
}

// ---- the animation thread ------------------------------------------------------------------------
void on_animation(std::uintptr_t component) noexcept {
    auto& l = live();
    if (!component || component != l.component.load(std::memory_order_acquire)) return;
    const auto error = GetLastError();
    try {
        const auto snap = snapshot();
        if (!snap || (!snap->open && !snap->armed && !snap->play_in_game)) {
            SetLastError(error);
            return;
        }
        const auto base = l.base.load(std::memory_order_acquire);
        const auto holder = read<std::uintptr_t>(component + 0xa0);
        if (!holder || !*holder) throw std::runtime_error("the skater has no animation yet");
        const auto layout = multiplayer::read_native_pose_layout(peek, base, *holder, 512);
        if (!layout.buffer) throw std::runtime_error("the skater's pose is still loading");
        if (layout.count != skeleton_joints)
            throw std::runtime_error(std::format("the skater has {} joints, not the standard {}", layout.count, skeleton_joints));
        if (!l.logged_layout) {
            l.logged_layout = true;
            logging::log(Level::info, Channel::skater, "Animation Studio: skater pose found, {} joints at {:#x} (component {:#x}).",
                         layout.count, layout.buffer, component);
        }
        // The board's pose, read with the skater's.
        l.board_ok = false;
        if (const auto bh = l.board_holder.load(std::memory_order_acquire)) {
            const auto bl = multiplayer::read_native_pose_layout(peek, base, bh, 64);
            l.board_ok = bl.buffer && bl.count == board_bones && peek(bl.buffer, l.board.data(), sizeof(l.board)) &&
                         finite(compose(board_parent(l.board), joint_of(l.board[board_mount])));
        }
        l.bones.resize(layout.count);
        if (!peek(layout.buffer, l.bones.data(), l.bones.size() * sizeof(NativeBone))) throw std::runtime_error("the skater's pose is unreadable");
        l.pose.resize(layout.count);
        for (std::size_t i = 0; i < layout.count; ++i) {
            auto& j = l.pose[i];
            const auto& b = l.bones[i];
            j.scale = {b.scale[0], b.scale[1], b.scale[2]};
            j.rotation = b.rotation;
            j.position = {b.position[0], b.position[1], b.position[2]};
        }
        const double at = now_seconds();
        const auto air = in_the_air(base, component);
        if (snap->armed && !snap->open && air) record(l, at, *air, *air ? current_trick(component) : std::string{});
        else if (!snap->armed) reset_recording(l);

        // Which joints to write back, and from what.
        std::size_t first = 0, last = 0; // [first, last) of l.pose written; 0, 0 = nothing
        Joint board_target;
        float board_weight = 0;
        const Joint live_placement = world(l.pose, placement_joint);
        if (snap->open && snap->clip && !snap->clip->empty()) {
            // The studio: the recorded trick, played in place where the skater stands now. The placement
            // joint (1) carries the trick's rise and turn (it rides with the body), and the game places
            // the skater from it, so it is never written: the recording's rise and turn since its first
            // frame go onto the joints hanging from it instead; the rest of the body is the recording's.
            l.sampled.resize(layout.count);
            snap->clip->sample(snap->time, l.sampled);
            const auto& start = snap->clip->frame(0)[placement_joint];
            const auto& now_rec = l.sampled[placement_joint];
            const auto turn = normalised(multiply(conjugate(start.rotation), now_rec.rotation));
            const auto rise = rotate(conjugate(l.pose[placement_joint].rotation), {0, now_rec.position[1] - start.position[1], 0});
            for (std::size_t i = 2; i < std::min<std::size_t>(layout.count, snap->clip->joints); ++i) {
                l.pose[i] = l.sampled[i];
                if (parent(static_cast<std::uint16_t>(i)) == placement_joint) {
                    const auto moved = rotate(turn, l.pose[i].position);
                    for (std::size_t a = 0; a < 3; ++a) l.pose[i].position[a] = rise[a] + moved[a];
                    l.pose[i].rotation = normalised(multiply(turn, l.pose[i].rotation));
                }
            }
            apply_edits(*snap->edits, snap->time, l.pose);
            first = 2;
            last = layout.count;
            // The board as the recording had it, the same way the body hangs from the placement joint.
            if (Joint rel; snap->clip->sample_board(snap->time, rel)) {
                const auto moved = rotate(turn, rel.position);
                for (std::size_t a = 0; a < 3; ++a) rel.position[a] = rise[a] + moved[a];
                rel.rotation = normalised(multiply(turn, rel.rotation));
                board_target = board_edited(compose(live_placement, rel), *snap->edits, snap->time);
                board_weight = 1;
            }
        } else if (snap->open) {
            // No trick recorded yet: the live pose, with the edits of the start of the timeline.
            apply_edits(*snap->edits, 0, l.pose);
            first = 2;
            last = layout.count;
        } else if (snap->play_in_game && !snap->armed && snap->tricks && !snap->tricks->empty() && air) {
            // In game: when the game does a trick there is a saved animation for, that animation plays on
            // the body in place of the game's own, from the takeoff, blended in and out.
            if (*air) {
                if (!l.game_air) {
                    l.game_air = true;
                    l.game_takeoff = at;
                    l.game_landing = -1;
                    l.game_trick = nullptr;
                    l.game_tricks = snap->tricks;
                    l.predicted_air = 0;
                }
                l.game_ground = 0;
                // How high the pop is: the rise in the first 0.05 s gives the upward speed, and from it the
                // time until the skater is back at the height he left.
                if (l.predicted_air == 0 && at - l.game_takeoff >= 0.03) {
                    // The board's own upward speed (its physics' root body), back to the height it left.
                    const double elapsed = at - l.game_takeoff;
                    const auto vy = board_up_speed(base, component);
                    l.predicted_air = vy && *vy > 0.3f
                                          ? static_cast<float>(std::clamp(2 * (*vy + 9.81 * elapsed) / 9.81 - elapsed, 0.15, 3.0)) : -1.0f;
                    l.game_trick = nullptr; // chosen again now that the pop is known
                }
                // The game names the trick within a few frames of the takeoff; a flip added late counts too.
                if (at - l.game_takeoff < 0.25 && l.game_tricks) {
                    const auto name = current_trick(component);
                    const auto pop = pop_of(l.predicted_air);
                    const CustomTrick* found = nullptr;
                    // The animation made for this pop, else the one for any pop.
                    for (const auto& t : *l.game_tricks)
                        if (t.trick == name && (t.edits->pop == pop || (t.edits->pop.empty() && !found))) found = &t;
                    if (found != l.game_trick) {
                        l.game_trick = found;
                        if (found)
                            logging::log(Level::info, Channel::skater, "Animation Studio: the game does a {} ({} pop, {:.2f} s in the air), playing your {}.", name,
                                         pop.empty() ? "unknown" : pop, l.predicted_air, found->name);
                    }
                }
            } else if (l.game_air && ++l.game_ground >= 3) {
                l.game_air = false;
                l.game_landing = at;
                if (l.game_trick)
                    logging::log(Level::info, Channel::skater, "Animation Studio: landed after {:.2f} s in the air (guessed {:.2f} s).", at - l.game_takeoff,
                                 l.predicted_air);
            }
            if (const auto* trick = l.game_trick) {
                const auto& clip = *trick->clip;
                const float speed = std::max(trick->edits->speed, 0.1f);
                float t = -1, weight = 1;
                if (l.game_air && clip.takeoff >= 0) {
                    // The recording's air stretched to this one's, so its keys meet this pop and landing.
                    float rate = speed;
                    if (l.predicted_air > 0 && clip.landing > clip.takeoff) rate *= (clip.landing - clip.takeoff) / l.predicted_air;
                    t = clip.takeoff + static_cast<float>(at - l.game_takeoff) * rate;
                    weight = std::clamp(static_cast<float>(at - l.game_takeoff) / 0.08f, 0.0f, 1.0f);
                    // In the air longer than the recording: the last frame before its landing holds.
                    if (clip.landing > clip.takeoff) t = std::min(t, clip.landing);
                } else if (l.game_landing > 0 && clip.landing >= 0) {
                    const float after = static_cast<float>(at - l.game_landing) * speed;
                    const float span = std::max(0.05f, std::min(0.35f, clip.duration() - clip.landing));
                    t = std::min(clip.landing + after, clip.duration());
                    weight = 1 - std::clamp(after / span, 0.0f, 1.0f);
                    if (weight <= 0) {
                        l.game_trick = nullptr;
                        l.game_landing = -1;
                        t = -1;
                    }
                }
                if (t >= 0 && weight > 0) {
                    l.custom.resize(layout.count);
                    std::copy(l.pose.begin(), l.pose.end(), l.custom.begin());
                    clip.sample(t, l.custom);
                    apply_edits(*trick->edits, t, l.custom);
                    if (Joint rel; clip.sample_board(t, rel)) {
                        board_target = board_edited(compose(live_placement, rel), *trick->edits, t);
                        board_weight = weight;
                    }
                    const auto& body = body_joints();
                    for (std::size_t i = 0; i < std::min<std::size_t>(layout.count, clip.joints); ++i) {
                        if (!body[i]) continue;
                        auto& j = l.pose[i];
                        const auto& c = l.custom[i];
                        j.rotation = slerp(j.rotation, c.rotation, weight);
                        for (std::size_t k = 0; k < 3; ++k) j.position[k] += (c.position[k] - j.position[k]) * weight;
                    }
                    first = 2;
                    last = layout.count;
                }
            }
        }
        if (last > first) {
            for (std::size_t i = first; i < last; ++i)
                if (!finite(l.pose[i])) throw std::runtime_error("an edit made a broken pose; nothing was written");
            for (std::size_t i = first; i < last; ++i) {
                auto& b = l.bones[i];
                const auto& j = l.pose[i];
                std::memcpy(b.scale.data(), j.scale.data(), 12);
                b.rotation = j.rotation;
                std::memcpy(b.position.data(), j.position.data(), 12);
            }
            // Written when the pose is handed to the renderer (on_render): the skater's body physics
            // rebuild this buffer after the animation update, which a write now would not survive (seen
            // 2026-10-18: the buffer held the edit, the drawn skater did not).
            auto& p = pending();
            std::lock_guard lock(p.mutex);
            p.buffer = layout.buffer;
            p.first = static_cast<std::uint32_t>(first);
            p.last = static_cast<std::uint32_t>(last);
            p.bones.assign(l.bones.begin() + static_cast<std::ptrdiff_t>(first), l.bones.begin() + static_cast<std::ptrdiff_t>(last));
            p.at = at;
        } else {
            auto& p = pending();
            std::lock_guard lock(p.mutex);
            p.bones.clear();
        }
        {
            auto& pb = pending_board();
            std::lock_guard lock(pb.mutex);
            pb.weight = board_weight > 0 && finite(board_target) ? board_weight : 0;
            pb.mount = board_target;
            pb.at = at;
        }
        if (snap->open) {
            std::optional<Joint> board;
            if (board_weight > 0 && finite(board_target)) board = board_target;
            else if (l.board_ok) board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
            publish_joints(l.pose, live_placement, board);
        }
    } catch (const std::exception& e) {
        if (l.failures++ % 600 == 0) logging::log(Level::warning, Channel::skater, "Animation Studio: {}", e.what());
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.failure = e.what();
    } catch (...) {
    }
    SetLastError(error);
}

// The local skater's pose handed to the renderer: the studio's pose goes in now, over whatever the
// body physics left there, each joint's own lanes only (each bone's 4th scale/position lane stays).
void on_render(std::uintptr_t animation_interface) noexcept {
    auto& l = live();
    const auto component = l.component.load(std::memory_order_acquire);
    if (!component) return;
    const auto error = GetLastError();
    try {
        const auto holder = read<std::uintptr_t>(component + 0xa0);
        if (!holder || !*holder || animation_interface != *holder + 0xc0) {
            SetLastError(error);
            return;
        }
        auto& p = pending();
        std::lock_guard lock(p.mutex);
        if (p.bones.empty() || now_seconds() - p.at > 0.1) {
            SetLastError(error);
            return;
        }
        const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), *holder, 512);
        if (layout.buffer != p.buffer || layout.count != skeleton_joints || p.last > layout.count || p.first >= p.last) {
            SetLastError(error);
            return;
        }
        thread_local std::vector<NativeBone> current; // the render thread's, kept between frames
        current.resize(p.last - p.first);
        if (!peek(layout.buffer + p.first * sizeof(NativeBone), current.data(), current.size() * sizeof(NativeBone))) throw std::runtime_error("unreadable");
        for (std::size_t i = 0; i < current.size(); ++i) {
            std::memcpy(current[i].scale.data(), p.bones[i].scale.data(), 12);
            current[i].rotation = p.bones[i].rotation;
            std::memcpy(current[i].position.data(), p.bones[i].position.data(), 12);
        }
        (void)poke(layout.buffer + p.first * sizeof(NativeBone), current.data(), current.size() * sizeof(NativeBone));
    } catch (...) {
    }
    SetLastError(error);
}

// The board's pose, just before it is handed to the renderer: its mount where the studio or the
// custom trick wants it.
void on_board_render(std::uintptr_t animation_interface, std::uintptr_t /*render_data*/) noexcept {
    auto& l = live();
    const auto error = GetLastError();
    try {
        // The board: its mount (bone 2) put where the studio or the custom trick wants it.
        if (const auto bh = l.board_holder.load(std::memory_order_acquire); bh && animation_interface == bh + 0xc0) {
            Joint target;
            float weight{};
            {
                auto& pb = pending_board();
                std::lock_guard lock(pb.mutex);
                if (pb.weight > 0 && now_seconds() - pb.at < 0.1) {
                    target = pb.mount;
                    weight = pb.weight;
                }
            }
            if (weight > 0) {
                const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), bh, 64);
                std::array<NativeBone, board_bones> b{};
                if (layout.buffer && layout.count == board_bones && peek(layout.buffer, b.data(), sizeof(b))) {
                    const auto parent_world = board_parent(b);
                    const auto now_mount = compose(parent_world, joint_of(b[board_mount]));
                    Joint want = target;
                    want.rotation = slerp(now_mount.rotation, target.rotation, weight);
                    for (std::size_t i = 0; i < 3; ++i) want.position[i] = now_mount.position[i] + (target.position[i] - now_mount.position[i]) * weight;
                    want.scale = now_mount.scale;
                    // The whole rig moves: its place in the world (bone 1) is set so the mount lands where wanted,
                    // so everything on the board goes with it (a pack deck's pieces hang on bone 1's other
                    // children, not on the mount: moving the mount alone left them lying on the ground).
                    const auto mount_local = joint_of(b[board_mount]);
                    const auto root_world = compose(want, relative(mount_local, Joint{}));
                    const auto root_local = relative(joint_of(b[0]), root_world);
                    if (finite(root_local)) {
                        auto& r = b[1];
                        r.rotation = root_local.rotation;
                        std::memcpy(r.position.data(), root_local.position.data(), 12);
                        (void)poke(layout.buffer + sizeof(NativeBone), &r, sizeof(NativeBone));
                    }
                }
            }
        }
    } catch (...) {
    }
    SetLastError(error);
}

// ---- the engine thread ---------------------------------------------------------------------------
TickResult tick(std::uintptr_t base, std::uintptr_t component, bool open, float camera_fov) {
    auto& l = live();
    auto& s = state();
    TickResult result;
    bool needs_listener{};
    {
        std::lock_guard lock(s.mutex);
        if (!s.snapshot) publish_locked(s);
        open_flag.store(open, std::memory_order_release);
        if (s.open != open) {
            s.open = open;
            s.base_yaw_valid = false;
            s.eased_valid = false;
            s.playing = false;
            publish_locked(s);
        }
        if (std::isfinite(camera_fov) && camera_fov > 1 && camera_fov < 175) s.fov = camera_fov;
        needs_listener = s.open || s.recording != Recording::off || (s.play_in_game && s.tricks && !s.tricks->empty());
        result.want_open = s.open_requested;
        result.changed = s.open_request_pending;
        s.open_request_pending = false;
    }
    if (component && base) {
        // The skater's board (its own entity, with a 16-bone rig): its animation holder.
        std::uintptr_t holder{};
        try {
            const auto collection = read<std::uintptr_t>(component + 0x18).value_or(0);
            const auto entity = collection ? read<std::uintptr_t>(collection).value_or(0) : 0;
            if (entity) holder = multiplayer::read_native_board(peek, base, entity).holder;
        } catch (...) {}
        l.board_holder.store(holder, std::memory_order_release);
    }
    if (component != l.component.load(std::memory_order_acquire)) {
        l.base.store(base, std::memory_order_release);
        l.component.store(component, std::memory_order_release);
        l.logged_layout = false;
    }
    {
        bool read{};
        {
            std::lock_guard lock(s.mutex);
            read = !s.tricks_read;
            s.tricks_read = true;
        }
        if (read) {
            auto tricks = read_custom_tricks();
            std::lock_guard lock(s.mutex);
            s.tricks = std::move(tricks);
            publish_locked(s);
        }
    }
    static bool installed = false;
    if (needs_listener && !installed && base) {
        std::string detail;
        if (multiplayer::install_entity_hooks(base, detail)) {
            multiplayer::set_animation_studio_listener(&on_animation);
            multiplayer::set_animation_studio_render_listener(&on_render);
            multiplayer::set_animation_studio_board_listener(&on_board_render);
            installed = true;
            logging::log(Level::info, Channel::skater, "Animation Studio: listening to the skater's animation.");
        } else {
            std::lock_guard lock(s.mutex);
            s.failure = "The animation hook is unavailable: " + detail;
        }
    }
    return result;
}
void report(std::string message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.status = std::move(message);
    ++s.revision;
}
bool camera(std::array<float, 16>& matrix) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.open || !s.centre_valid) return false;
    Vec3 target{};
    if (s.focus) {
        for (const auto& j : s.joints)
            if (j.joint == *s.focus)
                for (std::size_t i = 0; i < 3; ++i) target[i] = j.position[i] - s.centre[i];
    }
    // Eases to the focused joint rather than jumping there.
    for (std::size_t i = 0; i < 3; ++i) s.focus_offset[i] += (target[i] - s.focus_offset[i]) * 0.15f;
    if (!s.eased_valid || std::abs(s.eased_centre[0] - s.centre[0]) + std::abs(s.eased_centre[2] - s.centre[2]) > 5) {
        s.eased_centre = s.centre;
        s.eased_valid = true;
    }
    for (std::size_t i = 0; i < 3; ++i) s.eased_centre[i] += (s.centre[i] - s.eased_centre[i]) * 0.12f;
    const Vec3 centre{s.eased_centre[0] + s.focus_offset[0], s.eased_centre[1] + s.focus_offset[1], s.eased_centre[2] + s.focus_offset[2]};
    if (!s.base_yaw_valid) {
        s.base_yaw = facing_yaw(s.facing);
        s.base_yaw_valid = true;
    }
    auto m = orbit_matrix(centre, s.orbit, s.base_yaw);
    if (s.fov > 1 && s.fov < 175 && (s.view_shift != 0 || s.view_shift_x != 0)) {
        // Aim below and beside the centre so it shows that far above and right of the screen's middle.
        const float per_share = std::clamp(s.orbit.distance, 0.6f, 25.0f) * 2 * std::tan(s.fov * 3.14159265f / 360);
        const float up = s.view_shift * per_share, side = s.view_shift_x * per_share;
        const Vec3 aimed{centre[0] - m[4] * up - m[0] * side, centre[1] - m[5] * up - m[1] * side, centre[2] - m[6] * up - m[2] * side};
        m = orbit_matrix(aimed, s.orbit, s.base_yaw);
    }
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) matrix[i] = m[i];
    s.camera = matrix;
    s.camera_valid = true;
    return true;
}

// ---- the overlay -----------------------------------------------------------------------------------
bool is_open() noexcept { return open_flag.load(std::memory_order_acquire); }
View view() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.library_read) read_library_locked(s);
    View v;
    v.open = s.open;
    v.pose_seen = s.pose_seen_at > 0 && now_seconds() - s.pose_seen_at < 1.0;
    v.status = s.status;
    v.failure = s.failure;
    v.camera = s.camera;
    v.fov = s.fov;
    v.camera_valid = s.camera_valid;
    v.name = s.edits.name;
    v.trick = s.edits.trick;
    v.has_clip = s.clip && !s.clip->empty();
    v.dirty = s.dirty;
    v.playing = s.playing;
    v.loop = s.edits.loop;
    v.play_in_game = s.play_in_game;
    v.duration = duration_locked(s);
    v.takeoff = s.clip ? s.clip->takeoff : -1;
    v.landing = s.clip ? s.clip->landing : -1;
    v.time = s.time;
    v.speed = s.edits.speed;
    v.joints = s.joints;
    for (const auto& t : s.edits.tracks) {
        TrackView tv{t.joint, {}, t.hidden, t.locked};
        for (const auto& k : t.keys) tv.keys.push_back({k.time, k.ease});
        v.tracks.push_back(std::move(tv));
    }
    v.library = s.library;
    v.recording = s.recording;
    v.can_undo = !s.undo.empty();
    v.can_redo = !s.redo.empty();
    v.orbit = s.orbit;
    v.confirm_close = s.confirm_close;
    v.pop = s.edits.pop;
    v.revision = s.revision;
    return v;
}
// Keys sit on whole frames of the timeline (30 a second), never between two.
float frame_time(float seconds) { return std::round(seconds * 30.0f) / 30.0f; }
void request_close() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.dirty && s.clip && !s.clip->empty()) {
        s.confirm_close = true;
        ++s.revision;
        return;
    }
    s.open_requested = false;
    s.open_request_pending = true;
}
void answer_close(CloseAnswer answer) {
    std::string message;
    if (answer == CloseAnswer::save && !save(message)) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.confirm_close = false;
        return;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.confirm_close = false;
    if (answer == CloseAnswer::cancel) return;
    s.open_requested = false;
    s.open_request_pending = true;
}
void set_pop(const std::string& pop) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.pop = pop == "low" || pop == "mid" || pop == "high" ? pop : std::string{};
    s.dirty = true;
    ++s.revision;
}
bool copy_to_pops(std::string& message) {
    std::shared_ptr<const Clip> clip;
    Animation edits;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.clip || s.clip->empty() || s.edits.trick.empty()) {
            message = "Record a trick first.";
            return false;
        }
        clip = s.clip;
        edits = s.edits;
    }
    // The same keys for each pop: in game the air of each is stretched to its own, so pop and landing
    // fall where they should. Each copy can then be changed on its own.
    std::string base = file_safe(edits.trick);
    const auto folder = library_folder();
    try {
        std::filesystem::create_directories(folder);
        for (const char* pop : {"low", "mid", "high"}) {
            Animation copy = edits;
            copy.pop = pop;
            copy.name = base + " " + pop;
            copy.clip.times = {0, clip->duration()};
            copy.clip.takeoff = clip->takeoff;
            copy.clip.landing = clip->landing;
            const auto json = to_json(copy);
            const auto bytes = clip_bytes(*clip);
            std::ofstream(folder / (copy.name + ".json"), std::ios::binary).write(json.data(), static_cast<std::streamsize>(json.size()));
            std::ofstream(folder / (copy.name + ".clip"), std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    } catch (const std::exception& e) {
        message = std::string("Copying failed: ") + e.what();
        return false;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    read_library_locked(s);
    s.tricks_read = false;
    message = "Saved " + base + " low, mid and high. Open one to change it for that pop.";
    s.status = message;
    return true;
}
void request_open(bool open) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.open_requested = open;
    s.open_request_pending = true;
}
void advance(float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.playing || !s.clip || s.clip->empty() || !(seconds > 0)) return;
    const float duration = s.clip->duration();
    s.time += std::min(seconds, 0.1f) * s.edits.speed;
    if (s.time > duration) {
        if (s.edits.loop) s.time = std::fmod(s.time, std::max(duration, 0.01f));
        else {
            s.time = duration;
            s.playing = false;
        }
    }
    publish_locked(s);
}
void set_time(float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.time = std::clamp(seconds, 0.0f, duration_locked(s));
    publish_locked(s);
}
void set_playing(bool playing) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.playing = playing && s.clip && !s.clip->empty();
    if (s.playing && s.time >= duration_locked(s) - 1e-3f) s.time = 0;
    publish_locked(s);
}
void set_loop(bool loop) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.loop = loop;
    changed_locked(s);
}
void set_speed(float speed) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.speed = std::clamp(speed, 0.1f, 4.0f);
    changed_locked(s);
}
void record_next_trick() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.recording = Recording::waiting;
    s.status = "Recording: skate and do the trick. The studio opens again once you land.";
    s.open_requested = false;
    s.open_request_pending = true;
    publish_locked(s);
    logging::log(Level::info, Channel::skater, "Animation Studio: waiting to record the next trick.");
}
void cancel_recording() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.recording = Recording::off;
    s.status = "Recording cancelled.";
    publish_locked(s);
}
void set_play_in_game(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.play_in_game = on;
    s.status = on ? "Your saved tricks play in place of the game's." : "Tricks play as the game has them.";
    publish_locked(s);
}
void begin_change() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.undo.push_back({s.edits.tracks});
    if (s.undo.size() > 200) s.undo.pop_front();
    s.redo.clear();
}
Offset offset_of(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto* t = s.edits.track(joint);
    return t ? evaluate(*t, s.time) : Offset{};
}
void set_offset(std::uint16_t joint, const Offset& value) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!editable(joint)) return;
    if (const auto* t = s.edits.track(joint); t && t->locked) {
        s.status = "That track is locked.";
        return;
    }
    Offset clean = value;
    if (!editable(joint)->moves) clean.offset = {};
    for (auto& v : clean.offset) v = std::clamp(v, -2.0f, 2.0f);
    set_key(s.edits, joint, frame_time(s.time), clean);
    changed_locked(s);
}
void delete_key(std::uint16_t joint, float time) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (anim_studio::remove_key(s.edits, joint, time, 1e-3f)) changed_locked(s);
}
void move_key(std::uint16_t joint, float from, float to) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto* t = s.edits.track(joint);
    if (!t || t->locked) return;
    for (std::size_t i = 0; i < t->keys.size(); ++i)
        if (std::abs(t->keys[i].time - from) < 1e-3f) {
            anim_studio::move_key(*t, i, frame_time(to), duration_locked(s));
            changed_locked(s);
            return;
        }
}
void set_ease(std::uint16_t joint, float time, Ease ease) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (auto* t = s.edits.track(joint))
        for (auto& k : t->keys)
            if (std::abs(k.time - time) < 1e-3f) {
                k.ease = ease;
                changed_locked(s);
            }
}
void reset_joint(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.edits.tracks, [&](const Track& t) { return t.joint == joint && !t.locked; });
    changed_locked(s);
}
void set_track_flags(std::uint16_t joint, bool hidden, bool locked) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto* t = s.edits.track(joint);
    if (!t) {
        if (!editable(joint)) return;
        s.edits.tracks.push_back({joint, {}, hidden, locked});
    } else {
        t->hidden = hidden;
        t->locked = locked;
    }
    changed_locked(s);
}
void copy_pose() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.clipboard.clear();
    for (const auto& t : s.edits.tracks)
        if (!t.keys.empty()) s.clipboard.emplace_back(t.joint, evaluate(t, s.time));
    s.status = std::format("Copied the pose of {} joints.", s.clipboard.size());
}
void paste_pose() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.clipboard.empty()) return;
    for (const auto& [joint, value] : s.clipboard) {
        const auto* t = s.edits.track(joint);
        if (!t || !t->locked) set_key(s.edits, joint, frame_time(s.time), value);
    }
    s.status = std::format("Pasted the pose of {} joints.", s.clipboard.size());
    changed_locked(s);
}
void undo() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.undo.empty()) return;
    s.redo.push_back({s.edits.tracks});
    s.edits.tracks = std::move(s.undo.back().tracks);
    s.undo.pop_back();
    changed_locked(s);
}
void redo() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.redo.empty()) return;
    s.undo.push_back({s.edits.tracks});
    s.edits.tracks = std::move(s.redo.back().tracks);
    s.redo.pop_back();
    changed_locked(s);
}
void new_animation() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits = {};
    s.clip.reset();
    s.saved_as.clear();
    s.undo.clear();
    s.redo.clear();
    s.time = 0;
    s.playing = false;
    s.dirty = false;
    s.status = "New animation. Record a trick to start.";
    publish_locked(s);
}
bool save(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.clip || s.clip->empty()) {
        message = "Record a trick before saving.";
        return false;
    }
    if (s.edits.trick.empty()) {
        message = "Which trick is it? Type the game's trick in TRICK (ollie, kickflip...) or record it, then save.";
        s.status = message;
        return false;
    }
    if (s.edits.name.empty()) {
        // A new animation gets a name of its own, never one already in the library.
        const auto taken = [&](const std::string& n) { return std::find(s.library.begin(), s.library.end(), file_safe(n)) != s.library.end(); };
        std::string name = s.edits.trick.empty() ? "Trick" : s.edits.trick;
        int n = 1;
        while (taken(name + " " + std::to_string(n))) ++n;
        s.edits.name = name + " " + std::to_string(n);
    }
    const auto name = file_safe(s.edits.name);
    const auto folder = library_folder();
    try {
        std::filesystem::create_directories(folder);
        Animation full = s.edits;
        full.name = name;
        full.clip.times = {0, s.clip->duration()};
        full.clip.takeoff = s.clip->takeoff;
        full.clip.landing = s.clip->landing;
        const auto json = to_json(full);
        const auto bytes = clip_bytes(*s.clip);
        // Written beside the old files, then moved over them: a failed save never leaves half a profile.
        const auto json_path = folder / (name + ".json"), clip_path = folder / (name + ".clip");
        for (const auto& [path, data, size] : {std::tuple{json_path, static_cast<const void*>(json.data()), json.size()},
                                               std::tuple{clip_path, static_cast<const void*>(bytes.data()), bytes.size()}}) {
            auto temporary = path;
            temporary += L".saving";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
                if (!out) throw std::runtime_error("could not write " + temporary.string());
            }
            std::filesystem::rename(temporary, path);
        }
        s.edits.name = name;
        s.saved_as = name;
        s.dirty = false;
        s.tricks_read = false; // the next tick reads the custom tricks again
        read_library_locked(s);
        message = "Saved " + name + ".";
        s.status = message;
        logging::log(Level::info, Channel::skater, "Animation Studio: saved {} ({} keys) to {}.", name, s.edits.key_count(), folder.string());
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = std::string("Saving failed: ") + e.what();
        s.status = message;
        return false;
    }
}
const std::vector<GameTrick>& game_tricks() {
    static const auto list = [] {
        std::vector<GameTrick> out;
        try {
            std::ifstream in(library_folder() / L"library" / L"catalog.json", std::ios::binary);
            if (!in) return out;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto json = Json::parse(text);
            if (!json.is_array()) return out;
            for (std::size_t i = 0; i < json.size() && i < 2000; ++i) {
                const auto& e = json.at(i);
                if (!e.is_object()) continue;
                GameTrick t{e.value("category", ""), e.value("label", ""), e.value("trick", ""), e.value("pop", ""), e.value("file", "")};
                if (t.trick.empty() || t.file.empty() || t.file.find("..") != std::string::npos) continue;
                out.push_back(std::move(t));
            }
            // The catalog is written by category and trick, each trick's pops low, mid, high: kept as it is.
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: the trick library could not be read: {}", e.what());
        }
        logging::log(Level::info, Channel::skater, "Animation Studio: {} game tricks in the library.", out.size());
        return out;
    }();
    return list;
}
bool load_game_trick(const GameTrick& trick, std::string& message) {
    try {
        std::ifstream in(library_folder() / L"library" / std::filesystem::path(trick.file), std::ios::binary);
        if (!in) throw std::runtime_error("its file is missing");
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
        if (clip->joints != skeleton_joints) throw std::runtime_error("it is for another skeleton");
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.edits = {};
        s.edits.trick = trick.trick;
        s.edits.pop = trick.pop;
        s.edits.name = trick.label + " " + trick.pop;
        s.clip = std::move(clip);
        s.saved_as.clear();
        s.undo.clear();
        s.redo.clear();
        s.time = std::max(0.0f, s.clip->takeoff);
        s.playing = false;
        s.dirty = true;
        message = "Opened the game's " + trick.label + " (" + trick.pop + " pop). Change it and save: it then plays on your " + trick.label + "s.";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = "Could not open " + trick.label + ": " + e.what();
        return false;
    }
}
bool load(const std::string& name, std::string& message) {
    const auto folder = library_folder();
    try {
        const auto safe = file_safe(name);
        std::ifstream json_in(folder / (safe + ".json"), std::ios::binary);
        if (!json_in) throw std::runtime_error("it is not in the library");
        const std::string text((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>());
        auto animation = from_json(text);
        std::ifstream clip_in(folder / (safe + ".clip"), std::ios::binary);
        if (!clip_in) throw std::runtime_error("its recorded trick is missing");
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
        auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
        if (clip->joints != skeleton_joints) throw std::runtime_error("it was recorded on another skeleton");
        for (const auto& t : animation.tracks)
            for (const auto& k : t.keys)
                if (k.time > clip->duration() + 0.01f) throw std::runtime_error("a key lies past its recording");
        auto& s = state();
        std::lock_guard lock(s.mutex);
        animation.name = safe;
        s.edits = std::move(animation);
        s.clip = std::move(clip);
        s.saved_as = safe;
        s.undo.clear();
        s.redo.clear();
        s.time = std::max(0.0f, s.clip->takeoff);
        s.playing = false;
        s.dirty = false;
        message = "Opened " + safe + ".";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = "Could not open " + name + ": " + e.what();
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.status = message;
        return false;
    }
}
bool remove(const std::string& name, std::string& message) {
    const auto folder = library_folder();
    const auto safe = file_safe(name);
    std::error_code error;
    std::filesystem::remove(folder / (safe + ".json"), error);
    std::filesystem::remove(folder / (safe + ".clip"), error);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.tricks_read = false;
    read_library_locked(s);
    message = "Deleted " + safe + ".";
    s.status = message;
    return true;
}
void rename(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.name = name;
    s.dirty = true;
    ++s.revision;
}
void set_trick(const std::string& trick) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.trick = trick;
    if (s.edits.name.empty()) s.edits.name = trick;
    s.dirty = true;
    ++s.revision;
}
void set_orbit(const Orbit& orbit) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.orbit = orbit;
    s.orbit.pitch = std::clamp(orbit.pitch, -1.45f, 1.45f);
    s.orbit.distance = std::clamp(orbit.distance, 0.6f, 25.0f);
}
void set_view_shift(float share, float right_share) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.view_shift = std::clamp(share, -0.4f, 0.4f);
    s.view_shift_x = std::clamp(right_share, -0.9f, 0.9f);
}
void focus_joint(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.focus == joint) s.focus.reset();
    else s.focus = joint;
}
} // namespace dingosdk::anim_studio
