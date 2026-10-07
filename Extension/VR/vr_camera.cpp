// Settings and the VR camera: moves the first-person camera to the eye whose
// turn it is, and records which pose produced the frame.
#include "vr_internal.h"
#include "Engine/Core/Log/logging.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::vr {
namespace detail {
Shared& shared() {
    static auto* value = new Shared; // Lives until process exit; never torn down from DllMain.
    return *value;
}
Settings current_settings() {
    auto& s = shared();
    std::lock_guard lock(s.settings_mutex);
    return s.settings;
}
double seconds_now() noexcept {
    static const auto frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}
}

bool valid(const Settings& v) noexcept {
    const auto in = [](float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; };
    return in(v.turn_smoothing, 0, Limits::turn_smoothing_max) &&
        in(v.world_scale, Limits::world_scale_min, Limits::world_scale_max) &&
        in(v.fov_correction, Limits::fov_correction_min, Limits::fov_correction_max) &&
        in(v.seat_forward, Limits::seat_forward_min, Limits::seat_max) && in(v.seat_up, Limits::seat_min, Limits::seat_max) &&
        in(v.seat_side, Limits::seat_min, Limits::seat_max) &&
        in(v.chat_view_size, Limits::chat_view_size_min, Limits::chat_view_size_max) &&
        in(v.chat_view_x, -Limits::chat_view_offset_max, Limits::chat_view_offset_max) &&
        in(v.chat_view_y, -Limits::chat_view_offset_max, Limits::chat_view_offset_max) &&
        in(v.seat_forward_foot, Limits::seat_forward_min, Limits::seat_max) &&
        in(v.seat_up_foot, Limits::seat_min, Limits::seat_max) && in(v.seat_side_foot, Limits::seat_min, Limits::seat_max) &&
        in(v.fov_scale, Limits::fov_scale_min, Limits::fov_scale_max) &&
        v.frame_lag >= 0 && v.frame_lag <= Limits::frame_lag_max &&
        v.hide_body >= 0 && v.hide_body <= Limits::hide_body_max && v.hide_body_foot >= 0 &&
        v.hide_body_foot <= Limits::hide_body_max &&
        v.view_mode >= 0 && v.view_mode <= Limits::view_mode_max && in(v.tilt_filter, 0.0f, Limits::tilt_filter_max) &&
        v.stereo_mode >= 0 && v.stereo_mode <= Limits::stereo_mode_max && in(v.depth_strength, 0.0f, Limits::depth_strength_max) &&
        v.edge_widening >= 0 && v.edge_widening <= Limits::edge_widening_max && v.gap_fill >= 0 && v.gap_fill <= 1 &&
        v.look_turn_degrees >= 0 &&
        v.look_turn_degrees <= Limits::look_turn_max && v.look_turn_style >= 0 && v.look_turn_style <= 1 &&
        in(v.crouch_follow, 0.0f, 1.0f) &&
        in(v.smooth_turn_speed, Limits::smooth_turn_speed_min, Limits::smooth_turn_speed_max) && in(v.comfort_vignette, 0.0f, Limits::comfort_vignette_max) &&
        in(v.third_distance, Limits::third_distance_min, Limits::third_distance_max) &&
        in(v.third_height, Limits::third_height_min, Limits::third_height_max) &&
        in(v.theater_distance, Limits::theater_distance_min, Limits::theater_distance_max) &&
        in(v.theater_width, Limits::theater_width_min, Limits::theater_width_max) &&
        v.recenter_key >= 0 && v.recenter_key <= 0xfe;
}

Settings settings() noexcept { return detail::current_settings(); }

bool set_settings(const Settings& value) noexcept {
    if (!valid(value)) return false;
    auto& s = detail::shared();
    {
        std::lock_guard lock(s.settings_mutex);
        s.settings = value;
    }
    s.settings_changed.store(true, std::memory_order_release);
    return true;
}

bool take_settings_changed() noexcept { return detail::shared().settings_changed.exchange(false); }

void set_ui_open(bool open) noexcept { detail::shared().ui_open.store(open, std::memory_order_release); }
namespace {
struct PresetStore {
    std::mutex mutex;
    std::vector<Preset> saved;
    bool changed = false;
};
PresetStore& preset_store() {
    static auto* value = new PresetStore;
    return *value;
}
// Built-in presets for people trying the mod. First person is the defaults; the other
// two start from them.
std::vector<Preset> builtin_presets() {
    Preset third{"Third person", {}, true};
    // Close to the game's own camera: behind and a little above, level, following smoothly.
    third.settings.view_mode = 1;
    third.settings.third_distance = 3.0f;
    third.settings.third_height = 0.6f;
    third.settings.turn_smoothing = 0.6f;
    third.settings.world_scale = 1.0f;
    third.settings.seat_forward = third.settings.seat_side = third.settings.seat_up = 0.0f;
    third.settings.seat_forward_foot = third.settings.seat_side_foot = third.settings.seat_up_foot = 0.0f;
    Preset tiny{"Tiny world", {}, true};
    // Third person from far and high with a giant's eye separation: the skater looks like a
    // small figure roaming a model world, and leaning moves you around it at that scale.
    tiny.settings.view_mode = 1;
    tiny.settings.third_distance = 8.0f;
    tiny.settings.third_height = 3.0f;
    tiny.settings.turn_smoothing = 1.2f;
    tiny.settings.world_scale = 4.0f;
    tiny.settings.seat_forward = tiny.settings.seat_side = tiny.settings.seat_up = 0.0f;
    tiny.settings.seat_forward_foot = tiny.settings.seat_side_foot = tiny.settings.seat_up_foot = 0.0f;
    return {{"First person", Settings{}, true}, std::move(third), std::move(tiny)};
}
}

std::vector<Preset> presets() {
    auto result = builtin_presets();
    auto& store = preset_store();
    std::lock_guard lock(store.mutex);
    result.insert(result.end(), store.saved.begin(), store.saved.end());
    return result;
}
bool apply_preset(const std::string& name) noexcept {
    try {
        for (const auto& preset : presets())
            if (preset.name == name) {
                const auto current = settings();
                auto next = preset.settings;
                next.enabled = current.enabled;
                next.recenter_key = current.recenter_key;
                return set_settings(next);
            }
    } catch (...) {}
    return false;
}
bool save_preset(const std::string& name) noexcept {
    try {
        if (name.empty() || name.size() > 40) return false;
        for (const auto& preset : presets())
            if (preset.builtin && preset.name == name) return false;
        auto& store = preset_store();
        std::lock_guard lock(store.mutex);
        const auto current = settings();
        for (auto& preset : store.saved)
            if (preset.name == name) {
                preset.settings = current;
                store.changed = true;
                return true;
            }
        store.saved.push_back({name, current, false});
        store.changed = true;
        return true;
    } catch (...) {
        return false;
    }
}
bool delete_preset(const std::string& name) noexcept {
    auto& store = preset_store();
    std::lock_guard lock(store.mutex);
    const auto before = store.saved.size();
    std::erase_if(store.saved, [&](const Preset& preset) { return preset.name == name; });
    store.changed = store.changed || store.saved.size() != before;
    return store.saved.size() != before;
}
std::vector<Preset> saved_presets() {
    auto& store = preset_store();
    std::lock_guard lock(store.mutex);
    return store.saved;
}
void set_saved_presets(std::vector<Preset> saved) noexcept {
    auto& store = preset_store();
    std::lock_guard lock(store.mutex);
    store.saved = std::move(saved);
}
bool take_presets_changed() noexcept {
    auto& store = preset_store();
    std::lock_guard lock(store.mutex);
    return std::exchange(store.changed, false);
}

void set_game_menu(bool open) noexcept { detail::shared().game_menu.store(open, std::memory_order_release); }
bool game_menu_open() noexcept { return detail::shared().game_menu.load(std::memory_order_acquire); }
std::atomic<int> right_stick_x{0};
std::array<std::atomic<int>, 4> pad_right_x{}, pad_trigger{};
std::atomic<int> trigger_level{0};
std::atomic<bool> body_showing{false};
bool wants_first_person(bool first_person_on) noexcept {
    try {
        if (first_person_on) return false;
        const auto options = settings();
        if (!options.enabled || !options.auto_first_person || !status().running) return false;
        // At most once a second: the game takes it only once a level is playable.
        static double next = 0;
        const double now = detail::seconds_now();
        if (now < next) return false;
        next = now + 1.0;
        return true;
    } catch (...) {
        return false;
    }
}
bool whole_body_view() noexcept { return body_showing.load(std::memory_order_relaxed); }
std::atomic<bool> last_on_board{true};
void note_on_board(bool on_board) noexcept { last_on_board.store(on_board, std::memory_order_relaxed); }
bool on_board_view() noexcept { return last_on_board.load(std::memory_order_relaxed); }
void observe_gamepad(std::uint32_t user, std::int16_t right_x, std::uint8_t trigger) noexcept {
    if (user >= pad_right_x.size()) return;
    pad_right_x[user].store(right_x, std::memory_order_relaxed);
    pad_trigger[user].store(trigger, std::memory_order_relaxed);
    int pressed = 0;
    for (const auto& t : pad_trigger) pressed = std::max(pressed, t.load(std::memory_order_relaxed));
    trigger_level.store(pressed, std::memory_order_relaxed);
    // The strongest of the XInput pads.
    int strongest = 0;
    for (const auto& x : pad_right_x)
        if (std::abs(x.load(std::memory_order_relaxed)) > std::abs(strongest)) strongest = x.load(std::memory_order_relaxed);
    right_stick_x.store(strongest, std::memory_order_relaxed);
}
void recenter() noexcept { detail::shared().recenter_generation.fetch_add(1, std::memory_order_acq_rel); }

namespace {
using namespace detail;

// Engine-thread state of the VR camera.
struct Camera {
    std::mutex mutex;
    std::uint32_t next_view = 0;
    math::HeadingRig heading_rig; // third person: the followed travel direction
    // First person: the eyes' filters, the fading difference from the heading the view had when
    // they took over (from the on-foot heading), and whether they drove the view last update.
    math::EyesHeading eyes;
    math::EyesPosition eyes_place;
    float eyes_blend = 0;
    bool eyes_on = false;
    math::TiltRig tilt_rig;
    double rig_time = 0;
    bool turn_held = false; // look turn: the right stick must return before the next
    // Bail camera: tilted (or upright again) since; the fixed eye and its heading.
    bool bail = false;
    double tilted_since = 0, upright_since = 0;
    // Grabs: in the air with a trigger held, last seen at grab_seen; the outside view (flip &
    // bail, grab) shares bail_eye and bail_heading.
    bool grab = false, outside = false;
    double grab_seen = 0;
    // Where the skater has been (body origins about 0.3 m apart): ground the third-person and
    // outside cameras stay above, and how far the camera is lifted now.
    std::deque<std::array<float, 3>> trail;
    float lift = 0;
    float boom = 1;       // the share of the distance behind the skater the camera keeps now
    float seat_level = 1; // how much of the seat offset applies (fades out on walls)
    float seat_board = 1; // 1 the board's seat offset, 0 the on-foot one; blends between
    std::array<float, 3> bail_eye{};
    float bail_heading = 0;
    // Comfort vignette: the last turn the game applied (everything but the headset) and the level.
    math::Quat last_turn{0, 0, 0, 1};
    bool turn_ready = false;
    float vignette = 0;
    float vignette_setting = 0;
    double vignette_preview_until = 0;
    float travel = std::numeric_limits<float>::quiet_NaN();
    std::array<float, 2> velocity{};
    float climb = 0;
    std::array<float, 3> origin{};
    double origin_time = 0;
    double render_seen = -100, last_write = -100, last_log = -100, last_skip_log = -100;
    float heading = std::numeric_limits<float>::quiet_NaN();
    math::Anchor anchor;
    std::uint64_t recenter_applied = 0;
};
Camera& camera() {
    static auto* value = new Camera;
    return *value;
}

bool locate(Shared& s, XrTime display_time, std::array<XrView, 2>& views, math::Pose& head) {
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = display_time;
    info.space = s.local_space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    std::uint32_t count = 0;
    if (XR_FAILED(s.xr.locate_views(s.session, &info, &state, 2, &count, views.data())) || count != 2) return false;
    if (!(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(s.xr.locate_space(s.view_space, s.local_space, display_time, &location)) ||
        !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    head = to_pose(location.pose);
    if (!(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        // Rotation-only tracking: keep the eyes' offsets around a fixed head.
        for (auto& view : views) {
            view.pose.position.x -= location.pose.position.x;
            view.pose.position.y -= location.pose.position.y;
            view.pose.position.z -= location.pose.position.z;
        }
        head.position = {};
    }
    return true;
}
}

bool apply_camera(std::array<float, 16>& matrix, CameraWriter writer, const BodyPose* body) noexcept {
    auto& s = shared();
    if (!s.tracking.load(std::memory_order_acquire)) return false;
    const auto options = current_settings();
    if (!options.enabled) return false;
    try {
        std::shared_lock xr_lock(s.xr_mutex);
        if (!s.tracking.load(std::memory_order_acquire) || !s.session) return false;
        const XrTime predicted = s.predicted_display_time.load(std::memory_order_acquire);
        const XrDuration period = s.display_period.load(std::memory_order_acquire);
        if (predicted <= 0) return false;
        // This write's image reaches the screen `frame_lag` frames after the
        // frame the headset is waiting for now.
        const XrTime display_time = predicted + period * static_cast<XrDuration>(options.frame_lag);
        std::array<XrView, 2> views{};
        math::Pose head;
        if (!locate(s, display_time, views, head)) return false;
        xr_lock.unlock();

        auto& c = camera();
        std::lock_guard lock(c.mutex);
        const double now = seconds_now();
        // The render handoff ends each frame. Without it (its hook unavailable),
        // the tick is the only writer and ends the frame itself.
        if (writer == CameraWriter::render) c.render_seen = now;
        const bool frame_end = writer == CameraWriter::render || now - c.render_seen > camera_timeout;

        const auto recenter = s.recenter_generation.load(std::memory_order_acquire);
        if (recenter != c.recenter_applied || now - c.last_write > 1.0) {
            // Recentre on request and whenever VR takes the camera back.
            c.anchor = math::anchor_from(head);
            if (now - c.last_log > 1.0) {
                c.last_log = now;
                logging::write(logging::Level::info, logging::Channel::graphics,
                    std::format("VR: recentred; head at ({:.2f}, {:.2f}, {:.2f}) m in the tracking space.",
                        head.position[0], head.position[1], head.position[2]));
            }
            c.recenter_applied = recenter;
            c.heading = std::numeric_limits<float>::quiet_NaN();
            c.travel = std::numeric_limits<float>::quiet_NaN();
            c.velocity = {};
            c.heading_rig = {};
            c.eyes = {};
            c.eyes_place = {};
            c.eyes_on = false;
            c.tilt_rig = {};
        }
        if (body && frame_end && c.origin_time > 0 &&
            first_person::length(first_person::subtract(body->origin, c.origin)) > 2.0f) {
            // A teleport (a session marker, a respawn): the origin jumps, faster than any skating.
            // The view starts again from the skater: first person through its eyes, third person
            // behind it, instead of keeping the old direction (which could face the skater).
            c.heading = options.view_mode == 0 ? std::numeric_limits<float>::quiet_NaN() : body->chest_heading; // NaN: the head's heading
            c.travel = std::numeric_limits<float>::quiet_NaN();
            c.velocity = {};
            c.heading_rig = {};
            c.eyes = {};
            c.eyes_place = {};
            c.eyes_on = false;
            c.tilt_rig = {};
            c.trail.clear();
            c.bail = c.outside = false;
            c.grab_seen = 0;
            c.origin_time = 0; // no velocity across the jump
        }
        if (body && frame_end) {
            // Direction of travel from the origin's velocity, averaged over a quarter second
            // and held while slower than 0.5 m/s (third person follows it), and the climb (the
            // third-person ground guard).
            const double dt = now - c.origin_time;
            if (dt > 0.001 && dt < 0.5) {
                const float seconds = static_cast<float>(dt), blend = 1 - std::exp(-seconds / 0.25f);
                c.velocity[0] += ((body->origin[0] - c.origin[0]) / seconds - c.velocity[0]) * blend;
                c.velocity[1] += ((body->origin[2] - c.origin[2]) / seconds - c.velocity[1]) * blend;
                if (std::hypot(c.velocity[0], c.velocity[1]) > 0.5f) c.travel = math::heading_of(c.velocity[0], c.velocity[1]);
                c.climb += ((body->origin[1] - c.origin[1]) / seconds - c.climb) * blend;
            }
            c.origin = body->origin;
            c.origin_time = now;
        }
        const float rig_elapsed = frame_end && c.rig_time > 0 ? static_cast<float>(now - c.rig_time) : 0.0f;
        if (frame_end) c.rig_time = now;
        const bool first = options.view_mode == 0;

        // The view's heading.
        //  - First person: through the skater's eyes (math::eyes_heading): the head's turn,
        //    with slow wobble smoothed and spins, coffins and carves passed.
        //  - First person on foot with Walk where you look: the heading holds and the headset
        //    and the right stick turn it. The game moves the skater relative to the camera, so a
        //    view that followed the walk would feed back into the stick.
        //  - Third person: the direction of travel, followed lazily (Turn smoothing), held when
        //    standing, like the game's own camera.
        const bool look = body && first && options.walk_where_you_look && !body->on_board && std::isfinite(c.heading);
        if (body && frame_end) {
            // The right stick: turns on foot, orbits the outside views. Any XInput pad, the VR
            // controllers, or a DirectInput pad.
            int x = right_stick_x.load(std::memory_order_relaxed);
            if (const auto vr_pad = controller_pad(); vr_pad.active && std::abs(vr_pad.right_x) > std::abs(x)) x = vr_pad.right_x;
            if (look || c.outside) {
                for (const int other : {static_cast<int>(directinput_right_x()), static_cast<int>(xinput_peek().right_x)})
                    if (std::abs(other) > std::abs(x)) x = other;
            }
            float turn = 0;
            if ((look || c.outside) && options.look_turn_style == 1) {
                // Smooth: the stick past its dead zone sets the speed (right turns clockwise).
                constexpr float dead = 8000;
                if (std::abs(x) > dead)
                    turn = -std::copysign((std::abs(x) - dead) / (32767 - dead), static_cast<float>(x)) * options.smooth_turn_speed *
                        math::pi / 180 * rig_elapsed;
                c.turn_held = false;
            } else {
                const int flick = x > 22000 ? 1 : x < -22000 ? -1 : 0;
                if (flick && !c.turn_held && (look || c.outside) && options.look_turn_degrees > 0)
                    turn = -flick * static_cast<float>(options.look_turn_degrees) * math::pi / 180;
                c.turn_held = flick != 0 || (c.turn_held && std::abs(x) > 8000);
            }
            if (turn != 0 && c.outside) c.bail_heading = math::wrap(c.bail_heading + turn);
            else if (turn != 0 && look) c.heading = math::wrap(c.heading + turn);
        }
        if (body && first) {
            const float eyes = math::eyes_heading(c.eyes, first_person::orientation(matrix), frame_end ? rig_elapsed : 0.0f);
            if (!look) {
                // Taking over from the on-foot heading (or at the start), the difference fades over 0.4 s.
                if (!c.eyes_on) c.eyes_blend = std::isfinite(c.heading) ? math::wrap(c.heading - eyes) : 0.0f;
                else if (frame_end) c.eyes_blend *= std::exp(-rig_elapsed / 0.4f);
                c.heading = math::wrap(eyes + c.eyes_blend);
            }
            c.eyes_on = !look;
            c.heading_rig.value = c.heading;
        } else if (body) {
            const float wanted = std::isfinite(c.travel) ? c.travel : std::isfinite(c.heading) ? c.heading : math::heading(matrix).value_or(0.0f);
            c.heading = math::follow_snap(c.heading_rig, wanted, rig_elapsed, options.turn_smoothing, 0.0f, 0.0f);
            c.eyes_on = false;
        } else if (const auto target = math::heading(matrix); target && (!std::isfinite(c.heading) || frame_end)) {
            // No body (the free camera's writer): the head camera's heading, followed lazily.
            c.heading = std::isfinite(c.heading) ? math::follow_snap(c.heading_rig, *target, rig_elapsed, options.turn_smoothing, 0.0f, 0.0f)
                                                 : (c.heading_rig.value = *target);
        }
        if (!std::isfinite(c.heading)) c.heading = 0;

        auto base = matrix;
        const bool body_near = body && std::abs(matrix[12] - body->origin[0]) < 3.0f &&
            std::abs(matrix[13] - body->origin[1]) < 3.0f && std::abs(matrix[14] - body->origin[2]) < 3.0f;
        if (body_near) {
            // The view's position: the body origin plus the head's offset from it through one
            // continuous filter (math::eyes_position): bob smoothed, ollies and flips passed,
            // crouches scaled by Crouch.
            const auto eyes_at = math::eyes_position(c.eyes_place, {matrix[12], matrix[13], matrix[14]}, body->origin,
                frame_end ? rig_elapsed : 0.0f, options.crouch_follow);
            for (std::size_t i = 0; i < 3; ++i) base[12 + i] = eyes_at[i];
        }
        {
            // The seat offset, level along the view's heading. On a wall it would push the camera
            // out past the head, so it fades out as the board nears vertical (up axis 0.55 to 0.25,
            // either side up), held through airs. Grinds tilt the board less, and an upside-down
            // board (darkslides) is as flat as an upright one: the seat stays and the board stays in view.
            if (body && frame_end) {
                const bool airborne_now = body->physics_state >= 200 && body->physics_state < 300;
                const float wanted = !body->on_board ? 1.0f : airborne_now ? c.seat_level
                    : std::clamp((std::abs(body->board[5]) - 0.25f) / 0.3f, 0.0f, 1.0f);
                c.seat_level += (wanted - c.seat_level) * (1 - std::exp(-std::clamp(rig_elapsed, 0.0f, 0.5f) / 0.15f));
                // Stepping on or off the board moves the seat between the two offsets (~0.5 s).
                const float board = body->on_board || (body->physics_state >= 100 && body->physics_state < 500) ? 1.0f : 0.0f;
                c.seat_board += (board - c.seat_board) * (1 - std::exp(-std::clamp(rig_elapsed, 0.0f, 0.5f) / 0.15f));
            }
            const float level = body ? c.seat_level : 1.0f;
            const auto mix = [&](float on_board, float on_foot) { return (on_foot + (on_board - on_foot) * c.seat_board) * level; };
            const auto seat = first_person::rotate(math::yaw(c.heading),
                {mix(options.seat_side, options.seat_side_foot), mix(options.seat_up, options.seat_up_foot),
                    -mix(options.seat_forward, options.seat_forward_foot)});
            for (std::size_t i = 0; i < 3; ++i) base[12 + i] += seat[i];
        }
        if (body && frame_end && !(body->physics_state >= 200 && body->physics_state < 300)) {
            // The trail: where the skater has touched ground (not the arc of an air), for the
            // cameras behind them.
            if (c.trail.empty() || std::hypot(body->origin[0] - c.trail.back()[0], body->origin[2] - c.trail.back()[2]) > 0.3f ||
                std::abs(body->origin[1] - c.trail.back()[1]) > 0.3f) {
                c.trail.push_back(body->origin);
                if (c.trail.size() > 300) c.trail.pop_front();
            }
        }
        // Cameras behind the skater (third person, the outside views): `anchor` plus `offset`,
        // placed below so they can pull in on slopes.
        bool behind_camera = false;
        std::array<float, 3> anchor{}, offset{};
        if (options.view_mode == 1) {
            // Third person: behind (+Z is backward) and above the skater, along the followed heading.
            behind_camera = true;
            anchor = {base[12], base[13], base[14]};
            offset = first_person::rotate(math::yaw(c.heading), {0, options.third_height, options.third_distance});
        }

        const auto view = c.next_view;
        auto eye = to_pose(views[view].pose);
        const bool side_by_side = options.stereo_mode != 0; // Side-by-side input or depth stereo.
        if (side_by_side) {
            // The game renders from between the eyes; the stereo shader makes both.
            const auto other = to_pose(views[view ^ 1u].pose);
            for (std::size_t i = 0; i < 3; ++i) eye.position[i] = (eye.position[i] + other.position[i]) * 0.5f;
        }
        const auto aspect = s.image_aspect.load(std::memory_order_acquire);
        std::array<math::Tangents, 2> tangents{};
        for (std::size_t i = 0; i < 2; ++i)
            tangents[i] = math::tangents_from_angles(views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown);
        const float tangent = math::render_tangent(tangents, aspect, options.fov_scale);
        const float fov = math::native_fov_degrees(tangent * options.fov_correction, aspect, options.fov_vertical);
        if (!std::isfinite(fov) || fov < 10.0f || fov > 170.0f) {
            if (now - c.last_skip_log > 1.0) {
                c.last_skip_log = now;
                logging::write(logging::Level::warning, logging::Channel::graphics,
                    std::format("VR: camera skipped: render FOV {:.1f} (tangent {:.3f}, aspect {:.3f}, scale {:.2f}, vertical {}); "
                        "eye 0 angles L{:.3f} R{:.3f} U{:.3f} D{:.3f}.", fov, tangent, aspect, options.fov_scale, options.fov_vertical,
                        views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown));
            }
            return false;
        }

        // Level horizon off: the head's pitch/roll, wobble filtered, flips passed.
        std::optional<math::Quat> tilt;
        if (body && !options.level_horizon)
            tilt = math::steady_tilt(c.tilt_rig, first_person::orientation(matrix), rig_elapsed, options.tilt_filter, 4.0f);
        // Bail camera. A wipeout shows as off the board (state 504, which the game also uses
        // for walking) with the head tilted past 65 degrees for 0.15 s; walking and running
        // stay under 50 (measured: 47 running, 49 getting on, 81-123 in a bail).
        // It ends once the head is upright for 0.4 s or the skater is back on the board.
        if (body && frame_end) {
            const bool on_board = body->on_board || (body->physics_state >= 100 && body->physics_state < 500);
            const float up = matrix[5]; // the head's up axis, vertical part
            if (!options.bail_camera || options.view_mode != 0 || on_board) {
                c.bail = false;
                c.tilted_since = 0;
            } else if (!c.bail) {
                c.tilted_since = up < 0.4226f ? (c.tilted_since > 0 ? c.tilted_since : now) : 0;
                if (c.tilted_since > 0 && now - c.tilted_since > 0.15) {
                    c.bail = true;
                    c.upright_since = 0;
                }
            } else {
                c.upright_since = up > 0.819f ? (c.upright_since > 0 ? c.upright_since : now) : 0;
                if (c.upright_since > 0 && now - c.upright_since > 0.4) c.bail = false;
            }
            // Grabs: in the air (state 200-299) with a trigger held; it ends 0.25 s after.
            const bool airborne = body->physics_state >= 200 && body->physics_state < 300;
            if (airborne && (options.grab_view || options.grab_body) && options.view_mode == 0) {
                int pressed = trigger_level.load(std::memory_order_relaxed);
                if (const auto vr_pad = controller_pad(); vr_pad.active)
                    pressed = std::max({pressed, static_cast<int>(vr_pad.left_trigger), static_cast<int>(vr_pad.right_trigger)});
                if (pressed < 100) pressed = std::max(pressed, static_cast<int>(xinput_peek().trigger));
                if (pressed >= 100) c.grab_seen = now;
            }
            c.grab = (options.grab_view || options.grab_body) && options.view_mode == 0 && c.grab_seen > 0 &&
                now - c.grab_seen < 0.25;
            // The outside view: the flip & bail view, or a grab with Grab view on. It starts 3 m
            // behind and 1.2 m above along the view heading and follows the skater.
            const bool outside = c.bail || (c.grab && options.grab_view);
            const auto behind = first_person::rotate(math::yaw(c.outside ? c.bail_heading : c.heading), {0, 1.2f, 3.0f});
            if (outside && !c.outside) {
                c.bail_heading = c.heading;
                for (std::size_t i = 0; i < 3; ++i) c.bail_eye[i] = body->origin[i] + behind[i];
            } else if (outside) {
                const float follow = 1 - std::exp(-std::clamp(rig_elapsed, 0.0f, 0.5f) / 0.3f);
                for (std::size_t i = 0; i < 3; ++i) c.bail_eye[i] += (body->origin[i] + behind[i] - c.bail_eye[i]) * follow;
            }
            c.outside = outside;
            body_showing.store(c.bail || c.grab, std::memory_order_relaxed);
        }
        if (c.outside) {
            behind_camera = true;
            offset = first_person::rotate(math::yaw(c.bail_heading), {0, 1.2f, 3.0f});
            for (std::size_t i = 0; i < 3; ++i) anchor[i] = c.bail_eye[i] - offset[i];
        }
        if (behind_camera) {
            // Keep the camera out of slopes behind the skater. The ground: 0.3 m below any body
            // origin the trail has near the camera (about 1.2 m under it; within 1.5 m, wider the
            // farther the camera is), and the slope ridden now continued back to the camera
            // (going down at a rise of s per metre, the ground d metres behind is s * d higher).
            // The camera first pulls in towards the skater (keeping them in view on steep drops),
            // then lifts for what is left. Both move at once towards the skater, slowly back.
            const float speed = std::hypot(c.velocity[0], c.velocity[1]);
            const bool airborne = body && body->physics_state >= 200 && body->physics_state < 300;
            const float slope = body && !airborne && c.climb < -0.5f ? std::min(-c.climb / std::max(speed, 0.5f), 3.0f) : 0.0f;
            const auto floor_at = [&](const std::array<float, 3>& spot) {
                float floor = -std::numeric_limits<float>::infinity();
                if (!body) return floor;
                const float distance = std::hypot(spot[0] - body->origin[0], spot[2] - body->origin[2]);
                const float reach = std::max(1.5f, 0.25f * distance);
                for (const auto& point : c.trail)
                    if (std::hypot(point[0] - spot[0], point[2] - spot[2]) < reach) floor = std::max(floor, point[1] + 0.3f);
                if (slope > 0) floor = std::max(floor, body->origin[1] + slope * distance + 0.3f);
                return floor;
            };
            const auto eye_at = [&](float share) {
                return std::array<float, 3>{anchor[0] + offset[0] * share, anchor[1] + offset[1], anchor[2] + offset[2] * share};
            };
            float share = 0.3f;
            for (const float candidate : {1.0f, 0.85f, 0.7f, 0.55f, 0.4f, 0.3f}) {
                const auto spot = eye_at(candidate);
                if (floor_at(spot) <= spot[1]) {
                    share = candidate;
                    break;
                }
            }
            const float step = std::clamp(rig_elapsed, 0.0f, 0.5f);
            if (frame_end) c.boom += (share - c.boom) * (1 - std::exp(-step / (share < c.boom ? 0.08f : 0.8f)));
            const auto spot = eye_at(c.boom);
            const float floor = floor_at(spot);
            // At most 1.5 m: in a pipe the wall ridden counts as ground behind the camera, and
            // floating far above the skater is worse than grazing the wall.
            const float wanted = std::isfinite(floor) ? std::clamp(floor - spot[1], 0.0f, 1.5f) : 0.0f;
            if (frame_end) c.lift += (wanted - c.lift) * (1 - std::exp(-step / (wanted > c.lift ? 0.08f : 0.8f)));
            base[12] = spot[0];
            base[13] = spot[1] + c.lift;
            base[14] = spot[2];
        } else {
            c.lift = 0;
            c.boom = 1;
        }
        // Comfort vignette: from how fast the view turns without the headset (body turns, snap
        // and look turns, flips, the bail camera): none below 0.6 rad/s, full at 2 rad/s.
        if (frame_end) {
            const auto turn_now = c.outside ? math::yaw(c.bail_heading)
                : tilt ? first_person::normalized(first_person::multiply(math::yaw(c.heading), *tilt))
                : options.level_horizon ? math::yaw(c.heading) : first_person::orientation(base);
            float rate = 0;
            if (c.turn_ready && rig_elapsed > 0) {
                float dot = 0;
                for (std::size_t i = 0; i < 4; ++i) dot += c.last_turn[i] * turn_now[i];
                rate = 2 * std::acos(std::clamp(std::abs(dot), 0.0f, 1.0f)) / rig_elapsed;
            }
            c.last_turn = turn_now;
            c.turn_ready = true;
            const float wanted = std::clamp((rate - 0.6f) / 1.4f, 0.0f, 1.0f);
            const float seconds = wanted > c.vignette ? 0.08f : 0.4f; // quick in, slow out
            c.vignette += (wanted - c.vignette) * (1 - std::exp(-std::clamp(rig_elapsed, 0.0f, 0.5f) / seconds));
            // Moving the slider previews the setting at full strength for 2 s.
            if (options.comfort_vignette != c.vignette_setting) {
                c.vignette_setting = options.comfort_vignette;
                c.vignette_preview_until = now + 2.0;
            }
            const float amount = (now < c.vignette_preview_until ? 1.0f : c.vignette) * options.comfort_vignette;
            s.vignette.store(amount, std::memory_order_release);
        }
        const auto result = c.outside ? math::compose(base, c.bail_heading, true, eye, head, c.anchor, options.positional, options.world_scale)
            : math::compose(base, c.heading, options.level_horizon, eye, head, c.anchor,
            options.positional, options.world_scale, 0, 0, 0, tilt ? &*tilt : nullptr);
        // Only the lanes we write; the fourth lane of each row is the game's and may hold anything.
        for (std::size_t i = 0; i < 16; ++i)
            if (i % 4 != 3 && !std::isfinite(result[i])) {
                if (now - c.last_skip_log > 1.0) {
                    c.last_skip_log = now;
                    logging::write(logging::Level::warning, logging::Channel::graphics,
                        std::format("VR: camera skipped: lane {} not finite (heading {}, eye orientation {} {} {} {}).", i,
                            c.heading, eye.orientation[0], eye.orientation[1], eye.orientation[2], eye.orientation[3]));
                }
                return false;
            }
        matrix = result;
        s.camera_fov_degrees.store(fov, std::memory_order_release);
        s.camera_time.store(now, std::memory_order_release);
        s.camera_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        c.last_write = now;
        if (frame_end) {
            {
                std::lock_guard records(s.records_mutex);
                EyeRecord record{s.next_sequence++, view, eye, math::layer_angles(tangent, aspect)};
                if (side_by_side) {
                    record.both = true;
                    record.left = to_pose(views[0].pose);
                    record.right = to_pose(views[1].pose);
                }
                s.records.push_back(record);
                while (s.records.size() > 16) s.records.pop_front();
            }
            c.next_view = view ^ 1u;
            s.camera_frames.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    } catch (...) {
        return false;
    }
}

float camera_fov(float requested) noexcept {
    auto& s = shared();
    if (!camera_active(seconds_now())) return requested;
    const float fov = s.camera_fov_degrees.load(std::memory_order_acquire);
    return fov > 0 ? fov : requested;
}
}
