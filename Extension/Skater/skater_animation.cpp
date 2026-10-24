#include "skater_animation.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

// The pose paths of Animation Studio (PR 297, anim_studio.cpp: on_animation, on_render,
// on_board_render, on_send), for clips plugins play; see the header.
namespace dingosdk::skater_animation {
namespace {
using namespace anim_studio;
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

// ---- transforms ------------------------------------------------------------------------------------
// The smallest scale a posed joint is given: as small as hiding the skater makes its placement joint.
constexpr float smallest_scale = 0.001f;
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
Joint joint_of(const NativeBone& b) noexcept {
    Joint j;
    j.scale = {b.scale[0], b.scale[1], b.scale[2]};
    j.rotation = b.rotation;
    j.position = {b.position[0], b.position[1], b.position[2]};
    return j;
}
// The board's rig (board_pose.h): 16 bones, 0 its reference, 1 its place in the world, 2 the deck's
// mount, which carries the deck, the trucks and the wheels.
constexpr std::uint32_t board_bones = 16;
constexpr std::size_t board_mount = 2;
Joint board_parent(const std::array<NativeBone, board_bones>& b) noexcept { return compose(joint_of(b[0]), joint_of(b[1])); }

// The body: Hips (7) and every joint below it. A clip replaces these only, never the placement
// joints the game moves the skater with, nor the board's bones in the skater's rig.
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

// ---- a clip on the floor (Play::floor; PR 297's anim_studio.cpp) ------------------------------------------
// The board under the feet, as when riding. The game's clips leave the board where the animators had it,
// and the game's foot placement puts the feet on it: before the pop and after the landing their feet
// sink into the board or float over it by up to 6 cm. Riding, the feet (joints 343 and 10) are 0.104 m
// above the board's mount. So on the ground the board goes that far under the lower foot; in the air it
// is the clip's own, with a short blend at the pop and the landing.
constexpr float feet_above_board = 0.104f, board_blend = 0.06f;
constexpr std::uint16_t left_foot_joint = 343, right_foot_joint = 10;
float on_ground_share(const Clip& clip, float seconds) noexcept {
    const float up = clip.takeoff, down = clip.landing;
    if (up < 0 || down <= up || seconds <= up || seconds >= down) return 1;
    if (seconds < up + board_blend) return 1 - (seconds - up) / board_blend;
    if (seconds > down - board_blend) return (seconds - (down - board_blend)) / board_blend;
    return 0;
}
// Where a board touches the ground, in its mount's frame: the tips of its nose and tail (the kicks rise
// a little) and the bottoms of its four wheels; a board at rest stands on its wheels with its mount
// 0.087 m up. Its up is the axis nearest the placement joint's up, its length the one nearest the line
// of the feet, both from the board at the clip's start. Kept per clip.
struct BoardShape {
    std::array<Vec3, 6> points{};
    Vec3 up{0, 1, 0};
};
const BoardShape& board_shape(const Clip& clip) {
    thread_local const Clip* cached = nullptr;
    thread_local BoardShape shape;
    if (cached == &clip) return shape;
    cached = &clip;
    std::vector<Joint> pose(clip.joints);
    clip.sample(0, pose);
    Joint board;
    (void)clip.sample_board(0, board);
    const auto placed = world(pose, placement_joint);
    const auto left = relative(placed, world(pose, left_foot_joint)).position, right = relative(placed, world(pose, right_foot_joint)).position;
    const Vec3 feet{left[0] - right[0], 0, left[2] - right[2]};
    std::size_t up = 1, along = 0;
    float best_up = -1, best_along = -1;
    std::array<Vec3, 3> axes{};
    for (std::size_t i = 0; i < 3; ++i) {
        Vec3 e{};
        e[i] = 1;
        axes[i] = rotate(board.rotation, e);
        if (std::abs(axes[i][1]) > best_up) best_up = std::abs(axes[i][1]), up = i;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (i == up) continue;
        const float d = std::abs(axes[i][0] * feet[0] + axes[i][2] * feet[2]);
        if (d > best_along) best_along = d, along = i;
    }
    const std::size_t side = 3 - up - along;
    const float up_sign = axes[up][1] >= 0 ? 1.0f : -1.0f;
    const auto point = [&](float l, float u, float w) {
        Vec3 v{};
        v[along] = l;
        v[up] = u * up_sign;
        v[side] = w;
        return v;
    };
    shape.up = point(0, 1, 0);
    constexpr float tip = 0.40f, kick = 0.04f, wheelbase = 0.18f, track = 0.09f, wheels = 0.087f;
    shape.points = {point(tip, kick, 0), point(-tip, kick, 0), point(wheelbase, -wheels, track), point(wheelbase, -wheels, -track),
                    point(-wheelbase, -wheels, track), point(-wheelbase, -wheels, -track)};
    return shape;
}
// The lowest point of a board (its mount in the world): what touches the ground first.
float lowest_point(const Joint& board, const BoardShape& shape) noexcept {
    float low = board.position[1];
    for (const auto& pt : shape.points) low = std::min(low, board.position[1] + rotate(board.rotation, pt)[1]);
    return low;
}
// `board` (under the placement joint) under the feet of `pose` (the clip's at `seconds`), only while it
// lies flat: at the pop the back foot stands on the tail and the board is steep. Full up to 8 degrees of
// tilt, none from 20.
Joint board_on_feet(const Clip& clip, float seconds, std::span<const Joint> pose, Joint board) noexcept {
    const float tilt_cos = rotate(board.rotation, board_shape(clip).up)[1];
    const float flat = std::clamp((tilt_cos - 0.9397f) / (0.9903f - 0.9397f), 0.0f, 1.0f);
    const float share = on_ground_share(clip, seconds) * flat;
    if (share <= 0 || pose.size() <= left_foot_joint) return board;
    const auto placed = world(pose, placement_joint);
    const float left = relative(placed, world(pose, left_foot_joint)).position[1];
    const float right = relative(placed, world(pose, right_foot_joint)).position[1];
    const float want = std::min(left, right) - feet_above_board;
    if (std::isfinite(want)) board.position[1] += (want - board.position[1]) * share;
    return board;
}

// ---- the skater ------------------------------------------------------------------------------------
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
// The game's name for the trick the skater is doing ("ollie", "kickflip"): the animation holder
// (component +0xa0) -> +0x20 -> +0x250 -> +0x98, a C string (found 2026-10-18 for PR 297; right from
// 0.05 s after takeoff and through the whole air). Empty when the chain or the text looks wrong.
std::string trick_of(std::uintptr_t component) noexcept {
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
        name += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    return name.size() >= 2 ? name : std::string{};
}
// The board's upward speed (m/s): its physics' root body (skater_state.h).
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
std::string pop_of(float air) {
    if (!(air > 0)) return {};
    return air < 0.45f ? "low" : air < 0.65f ? "mid" : "high";
}

// ---- what the client thread asks -------------------------------------------------------------------
struct Plan {
    std::optional<Play> play;
    double started{};
    float from{};               // the clip's seconds at `started`
    std::optional<float> held;  // paused at these seconds of the clip
    std::uint64_t serial{};
    std::vector<Replacement> replacements;
    bool allowed{};
    float record_seconds{}; // > 0: record this long, from the next update
    std::uint64_t record_serial{};
    std::optional<arm_reach::Goal> reach;
    double reach_until{}, joints_until{}; // now_seconds() until which the reach holds, and the joints are read
    Shown reach_shown{};
    std::optional<carry::Goal> carry;       // carry(), until carry_until
    double carry_until{};
    bool carry_push{};
    Shown carry_shown{};
    std::vector<std::uint16_t> joints_read; // joint(): the joints asked for, in order
    std::vector<PoseOffset> offsets;        // pose(), until offsets_until
    float offsets_weight{1};
    double offsets_until{};
    Shown offsets_shown{};
};
// The game menu's character room (PR 297's studio room): the spot of the menu's skater, how far from
// it a rig counts as in the room, and the room's floor (measured 2026-10-10 on Stadium 1 and San
// Vansterdam, PR 230).
constexpr std::array<float, 3> room_spot{1.0f, -69.04f, -5.25f};
constexpr std::array<float, 3> room_reach{2.0f, 2.0f, 2.0f};
constexpr float room_floor = -69.99f;
constexpr std::uint16_t ground_joint = 375; // the skater's Ground joint: on the floor he stands or rides on
std::atomic<bool> room_watch{}, room_shown{};
std::atomic<double> room_seen_at{-1e9}, room_restore_until{};
// The menu skater's facing (its placement's rotation): the local skater faces the same way, as the room's camera expects.
std::mutex room_facing_mutex;
Quat room_facing{0, 0, 0, 1};
bool room_facing_valid{};
// The joints asked for as last evaluated (joint()), for the client thread.
std::mutex joints_mutex;
std::vector<std::pair<std::uint16_t, Joint>> joints_now;
// The client thread's: each joint asked for and when it was last.
std::vector<std::pair<std::uint16_t, double>> joints_asked;
std::mutex plan_mutex;
std::shared_ptr<const Plan> plan_now = std::make_shared<Plan>();
Plan plan_draft; // the client thread's, published as plan_now
std::shared_ptr<const Plan> plan() {
    std::lock_guard lock(plan_mutex);
    return plan_now;
}
void publish() {
    auto next = std::make_shared<const Plan>(plan_draft);
    std::lock_guard lock(plan_mutex);
    plan_now = std::move(next);
}
std::atomic<std::uint64_t> finished_serial{}; // the play the animation thread saw finish
std::mutex recorded_mutex;
std::shared_ptr<const Clip> recorded; // under recorded_mutex
std::atomic<bool> record_running{};

// ---- the animation thread's own ----------------------------------------------------------------------
struct Live {
    std::atomic<std::uintptr_t> base{}, component{}, board_holder{};
    std::vector<NativeBone> bones;
    std::vector<Joint> pose, custom, scratch;
    std::vector<Joint> sent; // the pose the others are sent, when it is not the one drawn (Shown)
    float carry_push{};      // how much a foot is down pushing, eased (carry's push_straightens)
    // The game's own scale of each joint a pose() scaled in the pose drawn. The game never sets a joint's
    // scale again, so the scale the last render wrote is still in the buffer at the next update: pose()
    // scaled it again on top (a forearm at 0.001 was at 1e-6 the next frame, and on to 0), and it stayed
    // when the pose ended (Filmer's own-view forearm, 2026-10-22: the arms gone after Filmer stopped,
    // and on foot the body physics, which read the buffer, lost the skater). Put back at every update.
    std::map<std::uint16_t, std::array<float, 3>> own_scale;
    std::uintptr_t own_scale_buffer{};
    // In the room: the placement written for the last render, and the game's own one before it.
    Joint room_written, game_placement;
    bool room_written_valid{}, game_placement_valid{};
    std::array<NativeBone, board_bones> board{};
    bool board_ok{};
    // A clip played on the floor (Play::floor): the floor's height, taken again for each new clip.
    float floor{};
    bool floor_valid{};
    const Clip* floor_clip{};
    // A game trick played as a replacement.
    bool game_air{};
    double game_takeoff{}, game_landing{-1};
    int game_ground{};
    float predicted_air{};
    const Replacement* game_trick{};
    std::shared_ptr<const Plan> game_plan; // keeps game_trick alive
    // Recording.
    std::atomic<std::uint64_t> record_serial{};
    double record_start{-1}, record_until{};
    Clip taking;
    bool taking_board{true}, was_air{};
    std::uint64_t failures{};
};
Live& live() {
    static auto* l = new Live;
    return *l;
}
// The pose to draw, from the animation thread to the render publication.
struct Pending {
    std::mutex mutex;
    std::uintptr_t buffer{};
    std::uint32_t first{}, last{};
    std::vector<NativeBone> bones; // [first, last)
    double at{};
};
Pending& pending() {
    static auto* p = new Pending;
    return *p;
}
// Where the board's mount goes in the world, and how much of that.
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
// What other players get of the local skater: the same joints as the renderer.
struct SentPose {
    std::mutex mutex;
    std::uintptr_t component{};
    std::uint32_t first{}, last{};
    std::vector<Joint> joints;
    double at{};
};
SentPose& sent_pose() {
    static auto* value = new SentPose;
    return *value;
}

void finish_recording(Live& l) {
    record_running.store(false, std::memory_order_release);
    l.record_start = -1;
    auto clip = std::make_shared<Clip>(std::move(l.taking));
    l.taking = {};
    if (clip->times.size() < 2) return;
    if (!l.taking_board) clip->board.clear();
    std::lock_guard lock(recorded_mutex);
    recorded = std::move(clip);
}
void record_frame(Live& l, double at, const std::optional<bool>& air) {
    if (l.record_start < 0) {
        l.record_start = at;
        l.taking = {};
        l.taking.joints = skeleton_joints;
        l.taking_board = true;
        l.was_air = air && *air;
    }
    const float t = static_cast<float>(at - l.record_start);
    l.taking.times.push_back(t);
    l.taking.frames.insert(l.taking.frames.end(), l.pose.begin(), l.pose.begin() + skeleton_joints);
    const auto placement = world(l.pose, placement_joint);
    if (l.board_ok) l.taking.board.push_back(relative(placement, compose(board_parent(l.board), joint_of(l.board[board_mount]))));
    else l.taking_board = false;
    const bool up = air && *air;
    if (up && !l.was_air && l.taking.takeoff < 0) l.taking.takeoff = t;
    if (!up && l.was_air && l.taking.takeoff >= 0 && l.taking.landing < 0) l.taking.landing = t;
    l.was_air = up;
    if (at >= l.record_until || l.taking.times.size() >= 900) finish_recording(l);
}

// The clip sampled at `seconds` (l.custom, its board `board`) moved onto the floor where the skater stands
// (Play::floor): the joints hanging from the placement joint get the clip's rise and turn since its first
// frame, as the game places the skater from that joint and it is never written; the rise puts the board's
// lowest point on the floor before the takeoff and after the landing, and between them follows the arc
// of a jump as long as the clip's air, unless the clip rises by itself (as a recording does), never under
// the floor. The floor is the Ground joint's height, taken again for each new clip and while in the air.
void place_on_floor(Live& l, const Clip& clip, float seconds, const Joint& placement, Joint* board, bool airborne) {
    if (&clip != l.floor_clip) {
        l.floor_clip = &clip;
        l.floor_valid = false;
    }
    if (!l.floor_valid || airborne) {
        l.floor = world(l.pose, ground_joint).position[1];
        l.floor_valid = std::isfinite(l.floor);
    }
    const auto& shape = board_shape(clip);
    // The height of the board's lowest point at `t` in the clip's own space, and its placement joint's.
    const auto board_y = [&](float t, float& placed_y) {
        l.scratch.resize(clip.joints);
        clip.sample(t, l.scratch);
        const auto placed = world(l.scratch, placement_joint);
        placed_y = placed.position[1];
        Joint rel;
        return clip.sample_board(t, rel) ? lowest_point(compose(placed, board_on_feet(clip, t, l.scratch, rel)), shape)
                                         : world(l.scratch, right_foot_joint).position[1] - 0.18f;
    };
    const std::span<const Joint> sampled(l.custom.data(), std::min<std::size_t>(l.custom.size(), clip.joints));
    if (board) *board = board_on_feet(clip, seconds, sampled, *board);
    const auto& start = clip.frame(0)[placement_joint];
    const auto& now_clip = l.custom[placement_joint];
    const auto turn = normalised(multiply(conjugate(start.rotation), now_clip.rotation));
    float placed_now{}, placed_up{}, placed_down{};
    const float low_now = board_y(seconds, placed_now);
    float rise_y = now_clip.position[1] - start.position[1];
    if (l.floor_valid) {
        const float up = std::max(clip.takeoff, 0.0f), down = clip.landing > up ? clip.landing : up;
        if (seconds <= up || seconds >= down || down <= up) rise_y = l.floor - low_now;
        else {
            const float s_up = l.floor - board_y(up, placed_up), s_down = l.floor - board_y(down, placed_down);
            const float u = (seconds - up) / (down - up);
            const bool own_jump = placed_now - (placed_up + (placed_down - placed_up) * u) > 0.12f;
            const float air = down - up;
            const float arc = own_jump ? 0.0f : 9.81f * air * air / 8.0f * 4 * u * (1 - u);
            rise_y = std::max(s_up + (s_down - s_up) * u + arc, l.floor - low_now);
        }
        rise_y += placed_now - placement.position[1];
    }
    const auto rise = rotate(conjugate(l.pose[placement_joint].rotation), {0, rise_y, 0});
    for (std::size_t i = 2; i < sampled.size(); ++i) {
        if (parent(static_cast<std::uint16_t>(i)) != placement_joint) continue;
        auto& j = l.custom[i];
        const auto moved = rotate(turn, j.position);
        for (std::size_t a = 0; a < 3; ++a) j.position[a] = rise[a] + moved[a];
        j.rotation = normalised(multiply(turn, j.rotation));
    }
    if (board) {
        const auto moved = rotate(turn, board->position);
        for (std::size_t a = 0; a < 3; ++a) board->position[a] = rise[a] + moved[a];
        board->rotation = normalised(multiply(turn, board->rotation));
    }
}

// What a clip's pose did, for one log line every 2 s while a clip plays (the size of a game trick's
// skater changed back and forth fast in the room): animation updates, renders that drew the clip's pose
// and renders that did not (it was stale or the buffer had moved), the scale and height written, and
// other skeletons the room hid.
struct Probe {
    std::mutex mutex;
    int updates{}, drawn{}, stale{}, moved{}, hidden{};
    float place_scale[2]{1e9f, -1e9f}, hips_scale[2]{1e9f, -1e9f}, hips_y[2]{1e9f, -1e9f}, root_scale[2]{1e9f, -1e9f};
    // From one update to the next: the most the hips moved, and the body's size (head to right foot).
    float size[2]{1e9f, -1e9f}, times[2]{1e9f, -1e9f};
    // The live skater's placement joint above his Ground joint, and the hips above the placement, as
    // the game posed him (a game clip holds its placement at the hips).
    float place_above[2]{1e9f, -1e9f}, hips_above[2]{1e9f, -1e9f};
    float hips_step{}, size_step{}, last_size{-1};
    Vec3 last_hips{1e9f, 1e9f, 1e9f};
    // Per joint, the most its local turn (degrees) and place (mm) changed from one update to the next:
    // in the game's pose as it came in, and in the pose written.
    std::vector<Joint> last_in, last_out;
    std::vector<float> in_turn, in_move, out_turn, out_move;
    static float angle(const Quat& a, const Quat& b) {
        const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return 2 * std::acos(std::min(d, 1.0f)) * 57.29578f;
    }
    static float apart(const Vec3& a, const Vec3& b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) * 1000;
    }
    static void step(std::span<const Joint> now, std::vector<Joint>& last, std::vector<float>& turn, std::vector<float>& move) {
        turn.resize(now.size());
        move.resize(now.size());
        if (last.size() == now.size())
            for (std::size_t i = 0; i < now.size(); ++i) {
                turn[i] = std::max(turn[i], angle(now[i].rotation, last[i].rotation));
                move[i] = std::max(move[i], apart(now[i].position, last[i].position));
            }
        last.assign(now.begin(), now.end());
    }
    // The three joints that changed most, as "joint:turn/move".
    static std::string worst(const std::vector<float>& turn, const std::vector<float>& move) {
        std::vector<std::size_t> order(turn.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return turn[a] + move[a] * 0.1f > turn[b] + move[b] * 0.1f; });
        std::string out;
        for (std::size_t k = 0; k < std::min<std::size_t>(3, order.size()); ++k)
            out += std::format("{}{}:{:.1f}deg/{:.0f}mm", k ? " " : "", order[k], turn[order[k]], move[order[k]]);
        return out;
    }
    void range(float (&r)[2], float v) {
        r[0] = std::min(r[0], v);
        r[1] = std::max(r[1], v);
    }
    void reset() {
        updates = drawn = stale = moved = hidden = 0;
        for (auto* r : {&place_scale, &hips_scale, &hips_y, &root_scale, &size, &times, &place_above, &hips_above}) (*r)[0] = 1e9f, (*r)[1] = -1e9f;
        hips_step = size_step = 0;
        std::fill(in_turn.begin(), in_turn.end(), 0.0f);
        std::fill(in_move.begin(), in_move.end(), 0.0f);
        std::fill(out_turn.begin(), out_turn.end(), 0.0f);
        std::fill(out_move.begin(), out_move.end(), 0.0f);
    }
};
Probe& probe() {
    static auto* p = new Probe;
    return *p;
}

// ---- the animation thread ------------------------------------------------------------------------
void on_animation(std::uintptr_t component) noexcept {
    auto& l = live();
    if (!component || component != l.component.load(std::memory_order_acquire)) return;
    const auto error = GetLastError();
    try {
        const auto snap = plan();
        const bool wants_record = snap->record_seconds > 0 && snap->record_serial != l.record_serial.load(std::memory_order_acquire);
        const bool busy = record_running.load(std::memory_order_acquire) || wants_record;
        const double now = now_seconds();
        const bool reaching = snap->reach && now < snap->reach_until;
        const bool joints_wanted = now < snap->joints_until && !snap->joints_read.empty();
        const bool posing = !snap->offsets.empty() && now < snap->offsets_until;
        const bool carrying = snap->carry && now < snap->carry_until;
        const bool in_room = room_shown.load(std::memory_order_acquire);
        if (!busy && !reaching && !carrying && !joints_wanted && !posing && !in_room && l.own_scale.empty() &&
            (!snap->allowed || (!snap->play && snap->replacements.empty()))) {
            std::lock_guard lock(pending().mutex);
            pending().bones.clear();
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
        l.board_ok = false;
        if (const auto bh = l.board_holder.load(std::memory_order_acquire)) {
            const auto bl = multiplayer::read_native_pose_layout(peek, base, bh, 64);
            l.board_ok = bl.buffer && bl.count == board_bones && peek(bl.buffer, l.board.data(), sizeof(l.board)) &&
                         finite(compose(board_parent(l.board), joint_of(l.board[board_mount])));
        }
        l.bones.resize(layout.count);
        if (!peek(layout.buffer, l.bones.data(), l.bones.size() * sizeof(NativeBone))) throw std::runtime_error("the skater's pose is unreadable");
        // The room's placement that the last render wrote stays in this buffer when the animation update
        // leaves joint 1 alone, and the body physics reads the buffer next: it took the skater for 269 m
        // below where he is and bailed him every 3.5 s. The game's own placement goes back first.
        const auto read_placement = joint_of(l.bones[placement_joint]);
        if (l.room_written_valid && read_placement.scale == l.room_written.scale && read_placement.rotation == l.room_written.rotation &&
            read_placement.position == l.room_written.position) {
            if (l.game_placement_valid) {
                auto& b = l.bones[placement_joint];
                std::memcpy(b.scale.data(), l.game_placement.scale.data(), 12);
                b.rotation = l.game_placement.rotation;
                std::memcpy(b.position.data(), l.game_placement.position.data(), 12);
                (void)poke(layout.buffer + placement_joint * sizeof(NativeBone), &b, sizeof(NativeBone));
            }
        } else {
            l.game_placement = read_placement;
            l.game_placement_valid = true;
        }
        if (!room_shown.load(std::memory_order_acquire)) l.room_written_valid = false;
        // Scaled joints get the game's own scale back first, in the buffer too (the body physics read it
        // after this update; only the render gets the posed scale), and are posed again from it.
        if (l.own_scale_buffer != layout.buffer) {
            l.own_scale.clear(); // another skater's buffer: the game's scales
            l.own_scale_buffer = layout.buffer;
        }
        for (const auto& [i, scale] : l.own_scale) {
            auto& b = l.bones[i];
            if (std::memcmp(b.scale.data(), scale.data(), sizeof(scale)) == 0) continue;
            std::memcpy(b.scale.data(), scale.data(), sizeof(scale));
            (void)poke(layout.buffer + i * sizeof(NativeBone), b.scale.data(), sizeof(scale));
        }
        l.pose.resize(layout.count);
        for (std::size_t i = 0; i < layout.count; ++i) l.pose[i] = joint_of(l.bones[i]);
        if (snap->play) {
            auto& pr = probe();
            std::lock_guard probe_lock(pr.mutex);
            Probe::step(l.pose, pr.last_in, pr.in_turn, pr.in_move);
            const auto placed = world(l.pose, placement_joint);
            pr.range(pr.place_above, placed.position[1] - world(l.pose, ground_joint).position[1]);
            pr.range(pr.hips_above, world(l.pose, hips_joint).position[1] - placed.position[1]);
        }
        const double at = now_seconds();
        const auto air = in_the_air(base, component);
        // Recording: the pose as the game animated it, before anything here changes it.
        if (wants_record) {
            l.record_serial.store(snap->record_serial, std::memory_order_release);
            l.record_start = -1;
            l.record_until = at + std::clamp(snap->record_seconds, 0.2f, 10.0f);
            record_running.store(true, std::memory_order_release);
        }
        if (record_running.load(std::memory_order_acquire)) record_frame(l, at, air);
        // The character room: the skater drawn on its floor, facing as the menu's skater does, as high
        // above the floor as he is above his own ground. Everything after this hangs from it.
        if (in_room) {
            const auto live_placement = world(l.pose, placement_joint);
            Joint room = live_placement;
            {
                std::lock_guard lock(room_facing_mutex);
                if (room_facing_valid) room.rotation = room_facing;
            }
            const float above = live_placement.position[1] - world(l.pose, ground_joint).position[1];
            room.position = {room_spot[0], room_floor + above, room_spot[2]};
            if (const auto local = relative(l.pose[0], room); finite(local)) l.pose[placement_joint] = local;
        }

        const Clip* clip = nullptr;
        float t = -1, weight = 0;
        if (snap->allowed && snap->play && snap->play->clip && !snap->play->clip->empty()) {
            // A clip played in place from when it was asked for.
            const auto& p = *snap->play;
            clip = p.clip.get();
            const float elapsed = static_cast<float>(at - snap->started);
            const float blend = std::max(p.blend, 0.0f);
            const float run = snap->held ? *snap->held : snap->from + elapsed * std::max(p.speed, 0.05f);
            const float length = clip->duration();
            if (p.loop && length > 0) t = std::fmod(run, length);
            else t = std::min(run, length);
            weight = blend > 0 ? std::clamp(elapsed / blend, 0.0f, 1.0f) : 1.0f;
            if (!p.loop && !snap->held && run >= length) {
                const float after = (run - length) / std::max(p.speed, 0.05f);
                weight = std::min(weight, blend > 0 ? 1 - std::clamp(after / blend, 0.0f, 1.0f) : 0.0f);
                if (weight <= 0) finished_serial.store(snap->serial, std::memory_order_release);
            }
        } else if (snap->allowed && !snap->replacements.empty() && air) {
            // A game trick with a replacement: the clip plays on the body from the takeoff, blended in
            // and out, its air stretched to this one's (anim_studio.cpp's play in game).
            if (*air) {
                if (!l.game_air) {
                    l.game_air = true;
                    l.game_takeoff = at;
                    l.game_landing = -1;
                    l.game_trick = nullptr;
                    l.game_plan = snap;
                    l.predicted_air = 0;
                }
                l.game_ground = 0;
                if (l.predicted_air == 0 && at - l.game_takeoff >= 0.03) {
                    const double elapsed = at - l.game_takeoff;
                    const auto vy = board_up_speed(base, component);
                    l.predicted_air = vy && *vy > 0.3f
                                          ? static_cast<float>(std::clamp(2 * (*vy + 9.81 * elapsed) / 9.81 - elapsed, 0.15, 3.0)) : -1.0f;
                    l.game_trick = nullptr;
                }
                if (at - l.game_takeoff < 0.25 && l.game_plan) {
                    const auto name = trick_of(component);
                    const auto pop = pop_of(l.predicted_air);
                    const Replacement* found = nullptr;
                    for (const auto& r : l.game_plan->replacements)
                        if (r.trick == name && r.clip && (r.pop == pop || (r.pop.empty() && !found))) found = &r;
                    l.game_trick = found;
                }
            } else if (l.game_air && ++l.game_ground >= 3) {
                l.game_air = false;
                l.game_landing = at;
            }
            if (const auto* trick = l.game_trick) {
                const auto& c = *trick->clip;
                const float speed = std::max(trick->speed, 0.1f);
                if (l.game_air && c.takeoff >= 0) {
                    float rate = speed;
                    if (l.predicted_air > 0 && c.landing > c.takeoff) rate *= (c.landing - c.takeoff) / l.predicted_air;
                    t = c.takeoff + static_cast<float>(at - l.game_takeoff) * rate;
                    weight = std::clamp(static_cast<float>(at - l.game_takeoff) / 0.08f, 0.0f, 1.0f);
                    if (c.landing > c.takeoff) t = std::min(t, c.landing);
                } else if (l.game_landing > 0 && c.landing >= 0) {
                    const float after = static_cast<float>(at - l.game_landing) * speed;
                    const float span = std::max(0.05f, std::min(0.35f, c.duration() - c.landing));
                    t = std::min(c.landing + after, c.duration());
                    weight = 1 - std::clamp(after / span, 0.0f, 1.0f);
                    if (weight <= 0) {
                        l.game_trick = nullptr;
                        l.game_plan.reset();
                        l.game_landing = -1;
                        t = -1;
                    }
                }
                if (l.game_trick) clip = &c;
            }
        }

        std::size_t first = 0, last = 0; // [first, last) of l.pose written
        Joint board_target;
        float board_weight = 0;
        if (clip && t >= 0 && weight > 0) {
            const auto placement = world(l.pose, placement_joint);
            l.custom.resize(layout.count);
            std::copy(l.pose.begin(), l.pose.end(), l.custom.begin());
            clip->sample(t, std::span<Joint>(l.custom.data(), std::min<std::size_t>(l.custom.size(), clip->joints)));
            // A joint the clip only turns stands where the skater's own build has it.
            for (std::size_t i = 0; i < std::min<std::size_t>(l.custom.size(), clip->joints); ++i)
                if (clip->keeps_position(i)) l.custom[i].position = l.pose[i].position;
            Joint rel;
            const bool has_board = clip->sample_board(t, rel);
            if (snap->play && snap->play->floor && clip == snap->play->clip.get() && clip->joints > left_foot_joint)
                place_on_floor(l, *clip, t, placement, has_board ? &rel : nullptr, air && *air);
            if (has_board) {
                board_target = compose(placement, rel);
                board_weight = weight;
            }
            const auto& body = body_joints();
            for (std::size_t i = 0; i < std::min<std::size_t>(layout.count, clip->joints); ++i) {
                if (!body[i]) continue;
                auto& j = l.pose[i];
                const auto& c = l.custom[i];
                j.rotation = slerp(j.rotation, c.rotation, weight);
                if (clip->keeps_position(i)) continue; // the skater's own build
                for (std::size_t k = 0; k < 3; ++k) j.position[k] += (c.position[k] - j.position[k]) * weight;
            }
            first = 2;
            last = layout.count;
        }
        // [first, last) of a pose grown to take in [from, to).
        const auto widen = [](std::size_t& first, std::size_t& last, std::size_t from, std::size_t to) {
            if (last > first) {
                first = std::min(first, from);
                last = std::max(last, to);
            } else {
                first = from;
                last = to;
            }
        };
        // Joints turned and scaled, after the clip; then something carried; then an arm reaching. Each
        // goes into the pose drawn, the pose sent, or both (Shown), and its joints join what is written.
        std::vector<std::uint16_t> scaled_now; // joints scaled in the pose drawn this update
        const auto pose_joints = [&](std::vector<Joint>& pose, std::size_t& first, std::size_t& last) {
            const float w = std::clamp(snap->offsets_weight, 0.0f, 1.0f);
            for (const auto& o : snap->offsets) {
                if (o.joint < 2 || o.joint >= layout.count) continue;
                auto& j = pose[o.joint];
                j.rotation = normalised(multiply(j.rotation, slerp(Quat{0, 0, 0, 1}, o.rotation, w)));
                // Never quite 0: a joint at scale 0 has no inverse. This small, it is as good as gone.
                const float s = std::max(1.0f + (o.scale - 1.0f) * w, smallest_scale);
                if (s != 1.0f && &pose == &l.pose) {
                    l.own_scale.try_emplace(o.joint, j.scale);
                    scaled_now.push_back(o.joint);
                }
                for (auto& k : j.scale) k *= s;
                for (std::size_t a = 0; a < 3; ++a) j.position[a] += o.offset[a] * w;
                widen(first, last, o.joint, o.joint + 1u);
            }
        };
        if (carrying && snap->carry_push) {
            // Pushing on the board: the knees mostly straighten while the foot is down (eased, from the
            // pose as the animation left it).
            const float pushing = carry::pushing(std::span<const Joint>(l.pose.data(), l.pose.size()));
            l.carry_push += (pushing - l.carry_push) * 0.256f;
        } else {
            l.carry_push = 0.0f;
        }
        const auto carry_item = [&](std::vector<Joint>& pose, std::size_t& first, std::size_t& last) {
            auto goal = *snap->carry;
            goal.crouch *= 1.0f - 0.65f * l.carry_push;
            if (carry::carry(std::span<Joint>(pose.data(), pose.size()), goal).valid)
                widen(first, last, carry::written.front(), carry::written.back() + 1u);
        };
        const auto reach_arm = [&](std::vector<Joint>& pose, std::size_t& first, std::size_t& last) {
            const auto& goal = *snap->reach;
            if (arm_reach::reach(std::span<Joint>(pose.data(), pose.size()), goal).valid)
                widen(first, last, arm_reach::first_written(goal.hand), arm_reach::last_written(goal.hand) + 1u);
        };
        const auto for_everyone = [](bool on, Shown shown) { return on && shown == Shown::everyone; };
        if (for_everyone(posing, snap->offsets_shown)) pose_joints(l.pose, first, last);
        if (for_everyone(carrying, snap->carry_shown)) carry_item(l.pose, first, last);
        if (for_everyone(reaching, snap->reach_shown)) reach_arm(l.pose, first, last);
        // What only the others see goes on a copy that is sent; what only you see, on the pose drawn.
        const bool split = (posing && snap->offsets_shown != Shown::everyone) || (carrying && snap->carry_shown != Shown::everyone) ||
                           (reaching && snap->reach_shown != Shown::everyone);
        std::size_t sent_from = first, sent_to = last;
        if (split) {
            l.sent = l.pose;
            const auto apply = [&](Shown who, bool on, Shown shown, const auto& step) {
                if (!on || shown != who) return;
                if (who == Shown::others) step(l.sent, sent_from, sent_to);
                else step(l.pose, first, last);
            };
            for (const auto who : {Shown::others, Shown::me}) {
                apply(who, posing, snap->offsets_shown, pose_joints);
                apply(who, carrying, snap->carry_shown, carry_item);
                apply(who, reaching, snap->reach_shown, reach_arm);
            }
        }
        // A joint no longer scaled has had its own scale back since the top of this update: forgotten.
        std::erase_if(l.own_scale, [&](const auto& entry) { return std::ranges::find(scaled_now, entry.first) == scaled_now.end(); });
        if (joints_wanted) {
            const std::span<const Joint> pose(l.pose.data(), l.pose.size());
            thread_local std::vector<std::pair<std::uint16_t, Joint>> placed;
            placed.clear();
            for (const auto index : snap->joints_read)
                if (index < pose.size())
                    if (const auto w = anim_studio::world(pose, index); anim_studio::finite(w)) placed.emplace_back(index, w);
            std::lock_guard lock(joints_mutex);
            joints_now.swap(placed);
        }
        // In the room the placement is written too (it is the room's), and nothing else need change.
        if (in_room) {
            if (last > first) first = std::min<std::size_t>(first, placement_joint);
            else first = placement_joint, last = placement_joint + 1u;
        }
        if (last > first) {
            for (std::size_t i = first; i < last; ++i)
                if (!finite(l.pose[i])) throw std::runtime_error("a clip made a broken pose; nothing was written");
            for (std::size_t i = first; i < last; ++i) {
                auto& b = l.bones[i];
                const auto& j = l.pose[i];
                std::memcpy(b.scale.data(), j.scale.data(), 12);
                b.rotation = j.rotation;
                std::memcpy(b.position.data(), j.position.data(), 12);
            }
            if (in_room && first <= placement_joint && placement_joint < last) {
                l.room_written = joint_of(l.bones[placement_joint]);
                l.room_written_valid = true;
            }
            // Written when the pose goes to the renderer (on_render): the body physics rebuild this
            // buffer after the animation update.
            {
                auto& p = pending();
                std::lock_guard lock(p.mutex);
                p.buffer = layout.buffer;
                p.first = static_cast<std::uint32_t>(first);
                p.last = static_cast<std::uint32_t>(last);
                p.bones.assign(l.bones.begin() + static_cast<std::ptrdiff_t>(first), l.bones.begin() + static_cast<std::ptrdiff_t>(last));
                p.at = at;
            }
            if (clip) {
                auto& pr = probe();
                std::lock_guard probe_lock(pr.mutex);
                ++pr.updates;
                pr.range(pr.root_scale, l.pose[0].scale[0]);
                pr.range(pr.place_scale, world(l.pose, placement_joint).scale[0]);
                const auto hips = world(l.pose, hips_joint);
                pr.range(pr.hips_scale, hips.scale[0]);
                pr.range(pr.hips_y, hips.position[1]);
                const auto head = world(l.pose, 103).position, foot = world(l.pose, 10).position;
                const float size = std::sqrt((head[0] - foot[0]) * (head[0] - foot[0]) + (head[1] - foot[1]) * (head[1] - foot[1]) +
                                             (head[2] - foot[2]) * (head[2] - foot[2]));
                pr.range(pr.size, size);
                pr.range(pr.times, t);
                if (pr.last_hips[0] < 1e8f) {
                    float d = 0;
                    for (std::size_t a = 0; a < 3; ++a) d += (hips.position[a] - pr.last_hips[a]) * (hips.position[a] - pr.last_hips[a]);
                    pr.hips_step = std::max(pr.hips_step, std::sqrt(d));
                }
                if (pr.last_size >= 0) pr.size_step = std::max(pr.size_step, std::abs(size - pr.last_size));
                pr.last_hips = hips.position;
                pr.last_size = size;
                Probe::step(l.pose, pr.last_out, pr.out_turn, pr.out_move);
            }
        } else {
            auto& p = pending();
            std::lock_guard lock(p.mutex);
            p.bones.clear();
        }
        // What the others are sent: the pose drawn, or its own copy (Shown). Other players are never
        // sent the room: from joint 2 on only.
        const auto& sent = split ? l.sent : l.pose;
        if (!split) sent_from = first, sent_to = last;
        if (sent_to > sent_from) {
            for (std::size_t i = sent_from; i < sent_to; ++i)
                if (!finite(sent[i])) throw std::runtime_error("a pose for the others was broken; nothing was sent");
            const std::size_t sent_first = std::max<std::size_t>(sent_from, 2);
            auto& sp = sent_pose();
            std::lock_guard sent_lock(sp.mutex);
            sp.component = component;
            sp.first = static_cast<std::uint32_t>(sent_first);
            sp.last = static_cast<std::uint32_t>(std::max(sent_to, sent_first));
            sp.joints.assign(sent.begin() + static_cast<std::ptrdiff_t>(sent_first), sent.begin() + static_cast<std::ptrdiff_t>(std::max(sent_to, sent_first)));
            sp.at = at;
        }
        auto& pb = pending_board();
        std::lock_guard lock(pb.mutex);
        pb.weight = board_weight > 0 && finite(board_target) ? board_weight : 0;
        pb.mount = board_target;
        pb.at = at;
    } catch (const std::exception& e) {
        if (l.failures++ % 600 == 0) logging::log(Level::warning, Channel::skater, "Plugin animation: {}", e.what());
    } catch (...) {
    }
    SetLastError(error);
}

// What other players get of the local skater: the clip, as drawn here.
void on_send(std::uintptr_t component, std::vector<multiplayer::Transform>& skater) noexcept {
    try {
        auto& sp = sent_pose();
        std::lock_guard lock(sp.mutex);
        if (component != sp.component || now_seconds() - sp.at > 0.15 || sp.last > skater.size()) return;
        for (std::uint32_t i = sp.first; i < sp.last; ++i) {
            const auto& j = sp.joints[i - sp.first];
            skater[i].rotation = j.rotation;
            skater[i].position = j.position;
        }
    } catch (...) {
    }
}

// The room's own skater and board (the menu's): hidden while the local skater shows in the room, given
// back after it. A skater is hidden by a tiny scale on its placement joint (a move, or scale 0, can move
// the game's camera or hide the room), a board by scale 0 on every bone. The game does not set the
// scale again, so it is given back here (PR 297's hide_in_room).
void hide_in_room(std::uintptr_t animation_interface, bool skater) noexcept {
    const bool hide = room_shown.load(std::memory_order_acquire), watch = room_watch.load(std::memory_order_acquire);
    if (!hide && !watch && now_seconds() > room_restore_until.load(std::memory_order_acquire)) return;
    auto& l = live();
    const auto holder = animation_interface - 0xc0;
    const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), holder, 512);
    if (!layout.buffer || layout.count != (skater ? skeleton_joints : board_bones)) return;
    std::array<NativeBone, 3> b{};
    if (!peek(layout.buffer, b.data(), sizeof(b))) return;
    const auto at = compose(compose(joint_of(b[0]), joint_of(b[1])), skater ? Joint{} : joint_of(b[2])).position;
    for (std::size_t i = 0; i < 3; ++i)
        if (!(std::abs(at[i] - room_spot[i]) < room_reach[i])) return;
    if (skater) {
        room_seen_at.store(now_seconds(), std::memory_order_release);
        // Its facing is taken while the walk looks for it, not once our skater shows: the Skaters page
        // still turns its own skater when the mouse drags (the plugin orbiting its camera), and ours
        // turned with it and sprang back when the drag ended.
        if (b[1].scale[0] >= 0.01f && !hide) {
            const auto placed = compose(joint_of(b[0]), joint_of(b[1]));
            std::lock_guard lock(room_facing_mutex);
            room_facing = placed.rotation;
            room_facing_valid = finite(placed);
        }
        const float scale = hide ? 0.001f : 1.0f;
        if ((b[1].scale[0] < 0.01f) == hide) return;
        if (hide) {
            auto& pr = probe();
            std::lock_guard probe_lock(pr.mutex);
            ++pr.hidden;
        }
        const std::array<float, 3> s3{scale, scale, scale};
        (void)poke(layout.buffer + sizeof(NativeBone), s3.data(), sizeof(s3));
        return;
    }
    const float scale = hide ? 0.0f : 1.0f;
    if ((b[2].scale[0] == 0.0f) == hide) return;
    for (std::uint32_t i = 0; i < layout.count; ++i) {
        const std::array<float, 3> s3{scale, scale, scale};
        (void)poke(layout.buffer + i * sizeof(NativeBone), s3.data(), sizeof(s3));
    }
}

// The local skater's pose handed to the renderer: the clip's pose goes in now, over whatever the
// body physics left there, each joint's own lanes only (each bone's 4th lane stays).
void on_render(std::uintptr_t animation_interface) noexcept {
    auto& l = live();
    const auto component = l.component.load(std::memory_order_acquire);
    if (!component) return;
    const auto error = GetLastError();
    try {
        const auto holder = read<std::uintptr_t>(component + 0xa0);
        if (!holder || !*holder || animation_interface != *holder + 0xc0) {
            hide_in_room(animation_interface, true);
            SetLastError(error);
            return;
        }
        auto& p = pending();
        std::lock_guard lock(p.mutex);
        const auto count = [](int Probe::*n) {
            auto& pr = probe();
            std::lock_guard probe_lock(pr.mutex);
            ++(pr.*n);
        };
        if (p.bones.empty() || now_seconds() - p.at > 0.1) {
            if (!p.bones.empty()) count(&Probe::stale);
            SetLastError(error);
            return;
        }
        const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), *holder, 512);
        if (layout.buffer != p.buffer || layout.count != skeleton_joints || p.last > layout.count || p.first >= p.last) {
            count(&Probe::moved);
            SetLastError(error);
            return;
        }
        count(&Probe::drawn);
        thread_local std::vector<NativeBone> current;
        current.resize(p.last - p.first);
        if (!peek(layout.buffer + p.first * sizeof(NativeBone), current.data(), current.size() * sizeof(NativeBone))) {
            SetLastError(error);
            return;
        }
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

// The board's pose, just before it goes to the renderer: the whole rig moved so its mount lands where
// the clip has it (anim_studio.cpp's on_board_render).
void on_board_render(std::uintptr_t animation_interface, std::uintptr_t /*render_data*/) noexcept {
    auto& l = live();
    const auto error = GetLastError();
    try {
        const auto bh = l.board_holder.load(std::memory_order_acquire);
        if (!bh || animation_interface != bh + 0xc0) {
            hide_in_room(animation_interface, false);
            SetLastError(error);
            return;
        }
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
        // The trucks and wheels (every bone under the deck's mount) as they were when a clip took the
        // board: the game's board physics goes on moving them for its own deck, which the clip turns and
        // flips somewhere else, and they shook. Held, they ride the deck. The render thread's own.
        static std::array<NativeBone, board_bones> held{};
        static bool holding = false;
        if (weight <= 0) holding = false;
        if (weight > 0) {
            const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), bh, 64);
            std::array<NativeBone, board_bones> b{};
            if (layout.buffer && layout.count == board_bones && peek(layout.buffer, b.data(), sizeof(b))) {
                if (!holding) {
                    held = b;
                    holding = true;
                }
                // The deck's mount too: placed from a mount the physics had moved, the deck and the
                // trucks under it came apart.
                for (std::uint32_t i = board_mount; i < board_bones; ++i) b[i] = held[i];
                (void)poke(layout.buffer + board_mount * sizeof(NativeBone), &held[board_mount], (board_bones - board_mount) * sizeof(NativeBone));
                const auto now_mount = compose(board_parent(b), joint_of(b[board_mount]));
                Joint want = target;
                want.rotation = slerp(now_mount.rotation, target.rotation, weight);
                for (std::size_t i = 0; i < 3; ++i) want.position[i] = now_mount.position[i] + (target.position[i] - now_mount.position[i]) * weight;
                want.scale = now_mount.scale;
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
    } catch (...) {
    }
    SetLastError(error);
}

bool hooked{};
std::string failure;
std::uint64_t serial{};
}

void watch_room(bool on) noexcept { room_watch.store(on, std::memory_order_release); }
bool room_skater_seen() noexcept { return now_seconds() - room_seen_at.load(std::memory_order_acquire) < 0.5; }
bool shown_in_room() noexcept { return room_shown.load(std::memory_order_acquire); }
void show_in_room(bool on) noexcept {
    if (room_shown.exchange(on, std::memory_order_acq_rel) && !on) room_restore_until.store(now_seconds() + 3, std::memory_order_release);
}
void play(std::optional<Play> clip) {
    plan_draft.play = std::move(clip);
    plan_draft.started = now_seconds();
    plan_draft.from = plan_draft.play ? plan_draft.play->at : 0;
    plan_draft.held = plan_draft.play && plan_draft.play->paused ? std::optional(plan_draft.from) : std::nullopt;
    plan_draft.serial = ++serial;
    publish();
}
std::optional<float> play_time() {
    if (!plan_draft.play || !plan_draft.play->clip) return std::nullopt;
    if (plan_draft.held) return plan_draft.held;
    const auto& p = *plan_draft.play;
    const float run = plan_draft.from + static_cast<float>(now_seconds() - plan_draft.started) * std::max(p.speed, 0.05f);
    const float length = p.clip->duration();
    if (p.loop && length > 0) return std::fmod(run, length);
    return std::min(run, length);
}
void seek(float seconds) {
    if (!plan_draft.play || !plan_draft.play->clip) return;
    const float t = std::clamp(seconds, 0.0f, plan_draft.play->clip->duration());
    if (plan_draft.held) plan_draft.held = t;
    else {
        // Playing on from there, already blended in.
        plan_draft.from = t;
        plan_draft.started = now_seconds() - std::max(plan_draft.play->blend, 0.0f);
        if (plan_draft.play->speed > 0) plan_draft.from -= std::max(plan_draft.play->blend, 0.0f) * plan_draft.play->speed;
    }
    publish();
}
void pause(bool paused) {
    if (!plan_draft.play || !plan_draft.play->clip || paused == plan_draft.held.has_value()) return;
    if (paused) plan_draft.held = play_time();
    else {
        const float t = *plan_draft.held;
        plan_draft.held.reset();
        seek(t);
        return;
    }
    publish();
}
bool playing() noexcept {
    return plan_draft.play.has_value() && finished_serial.load(std::memory_order_acquire) != plan_draft.serial;
}
void pose(std::vector<PoseOffset> offsets, float weight, Shown shown) {
    plan_draft.offsets = std::move(offsets);
    plan_draft.offsets_weight = weight;
    plan_draft.offsets_shown = shown;
    plan_draft.offsets_until = plan_draft.offsets.empty() ? 0 : now_seconds() + 0.3;
    publish();
}
void reach(std::optional<arm_reach::Goal> goal, Shown shown) {
    plan_draft.reach = std::move(goal);
    plan_draft.reach_shown = shown;
    plan_draft.reach_until = plan_draft.reach ? now_seconds() + 0.3 : 0;
    publish();
}
void carry(std::optional<carry::Goal> goal, bool push_straightens, Shown shown) {
    plan_draft.carry = std::move(goal);
    plan_draft.carry_push = push_straightens;
    plan_draft.carry_shown = shown;
    plan_draft.carry_until = plan_draft.carry ? now_seconds() + 0.3 : 0;
    publish();
}
std::optional<Joint> joint(std::uint16_t index) {
    if (index >= 395) return std::nullopt;
    const double now = now_seconds();
    std::erase_if(joints_asked, [&](const auto& asked) { return now - asked.second > 2.0; });
    const auto asked = std::find_if(joints_asked.begin(), joints_asked.end(), [&](const auto& a) { return a.first == index; });
    if (asked != joints_asked.end()) asked->second = now;
    else if (joints_asked.size() < 32) joints_asked.emplace_back(index, now);
    else return std::nullopt;
    std::vector<std::uint16_t> read;
    for (const auto& a : joints_asked) read.push_back(a.first);
    std::sort(read.begin(), read.end());
    if (read != plan_draft.joints_read || plan_draft.joints_until < now + 1.0) {
        plan_draft.joints_read = std::move(read);
        plan_draft.joints_until = now + 2.0;
        publish();
    }
    std::lock_guard lock(joints_mutex);
    for (const auto& [at, w] : joints_now)
        if (at == index) return w;
    return std::nullopt;
}
std::optional<arm_reach::Placed> hand(arm_reach::Hand side) {
    const auto at = joint(arm_reach::chain(side).back());
    if (!at) return std::nullopt;
    return arm_reach::Placed{at->position, at->rotation, at->scale[0]};
}
void set_replacements(std::vector<Replacement> list) {
    plan_draft.replacements = std::move(list);
    publish();
}
bool record(float seconds) {
    if (record_running.load(std::memory_order_acquire) || !(seconds > 0)) return false;
    plan_draft.record_seconds = std::min(seconds, 10.0f);
    plan_draft.record_serial = ++serial;
    publish();
    return true;
}
bool recording() noexcept {
    return record_running.load(std::memory_order_acquire) || (plan_draft.record_seconds > 0 && live().record_serial.load(std::memory_order_acquire) != plan_draft.record_serial);
}
std::shared_ptr<const Clip> take_recording() {
    std::lock_guard lock(recorded_mutex);
    return std::exchange(recorded, {});
}
std::string current_trick() noexcept {
    const auto component = live().component.load(std::memory_order_acquire);
    return component ? trick_of(component) : std::string{};
}

void tick(std::uintptr_t base, std::uintptr_t component, bool allowed) {
    auto& l = live();
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
    }
    if (allowed != plan_draft.allowed) {
        plan_draft.allowed = allowed;
        publish();
    }
    {
        static double next_probe = 0;
        const double t = now_seconds();
        if (plan_draft.play && t >= next_probe) {
            next_probe = t + 2;
            auto& pr = probe();
            std::lock_guard probe_lock(pr.mutex);
            if (pr.updates || pr.drawn || pr.stale || pr.moved)
                logging::log(Level::info, Channel::skater,
                             "Clip pose (2 s): {} updates, {} renders drew it, {} stale, {} on another buffer, {} room hides; root scale {:.4f}..{:.4f}, "
                             "placement {:.4f}..{:.4f}, hips {:.4f}..{:.4f} at height {:.3f}..{:.3f}; clip time {:.3f}..{:.3f}; head to foot {:.3f}..{:.3f} m; "
                             "most in one update: hips {:.3f} m, head to foot {:.3f} m; most changed coming in {}, written {}; live placement {:.3f}..{:.3f} m above the ground, hips {:.3f}..{:.3f} m above it{}",
                             pr.updates, pr.drawn, pr.stale, pr.moved, pr.hidden, pr.root_scale[0], pr.root_scale[1], pr.place_scale[0],
                             pr.place_scale[1], pr.hips_scale[0], pr.hips_scale[1], pr.hips_y[0], pr.hips_y[1], pr.times[0], pr.times[1],
                             pr.size[0], pr.size[1], pr.hips_step, pr.size_step, Probe::worst(pr.in_turn, pr.in_move),
                             Probe::worst(pr.out_turn, pr.out_move), pr.place_above[0], pr.place_above[1], pr.hips_above[0], pr.hips_above[1],
                             room_shown.load(std::memory_order_acquire) ? " (in the room)" : "");
            pr.reset();
        }
    }
    // A play that finished is over.
    if (plan_draft.play && finished_serial.load(std::memory_order_acquire) == plan_draft.serial) {
        plan_draft.play.reset();
        publish();
    }
    const double now = now_seconds();
    const bool wanted = plan_draft.play || !plan_draft.replacements.empty() || recording() ||
                        (plan_draft.reach && now < plan_draft.reach_until) || (plan_draft.carry && now < plan_draft.carry_until) ||
                        now < plan_draft.joints_until ||
                        (!plan_draft.offsets.empty() && now < plan_draft.offsets_until) || room_watch.load(std::memory_order_acquire) ||
                        room_shown.load(std::memory_order_acquire) || now < room_restore_until.load(std::memory_order_acquire);
    if (wanted && !hooked && base) {
        std::string detail;
        if (multiplayer::install_entity_hooks(base, detail)) {
            multiplayer::set_pose_override_listener(&on_animation);
            multiplayer::set_pose_override_render_listener(&on_render);
            multiplayer::set_pose_override_board_listener(&on_board_render);
            multiplayer::set_local_pose_filter(&on_send);
            hooked = true;
            logging::write(Level::info, Channel::skater, "Plugin animation: listening to the skater's animation.");
        } else failure = "the animation hook is unavailable: " + detail;
    }
}

std::string status() {
    if (!hooked) return failure.empty() ? "not hooked (no clip asked for yet)" : failure;
    return std::format("hooked; {}playing, {} replacement(s){}", playing() ? "" : "not ", plan_draft.replacements.size(),
                       recording() ? ", recording" : "");
}
}
