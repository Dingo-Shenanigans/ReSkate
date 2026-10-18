#include "first_person_settings_host.h"
#include "Extension/UI/Overlay/overlay.h"
#include <cmath>
#include "Engine/Game/Skater/first_person_fov.h"
#include <cstdio>

namespace dingosdk::first_person_host {
Snapshot read(const overlay::Model& model) noexcept {
    const auto& debug = model.debug;
    const auto& arm = debug.first_person_arm;
    Snapshot value;
    value.available = debug.available && debug.camera_available;
    value.enabled = debug.first_person;
    value.fov = debug.first_person_fov;
    value.game_fov = debug.camera_fov;
    value.spring = arm.enabled;
    value.board_only = arm.board_only;
    value.offset = arm.offset;
    value.rotation = arm.rotation;
    value.strength = {arm.up, arm.down, arm.left, arm.right};
    value.stabilize = arm.stabilize;
    value.follow_flips = arm.follow_flips;
    value.camera_cycle = arm.camera_cycle;
    value.smoothing = arm.smoothing;
    value.head_pitch = arm.head_pitch;
    value.head_roll = arm.head_roll;
    value.bob = arm.bob;
    return value;
}
namespace {
using Action = overlay::DebugAction;
struct Route { Field field; Action action; bool boolean; float low, high; };
constexpr Route routes[]{
    {Field::enabled, Action::set_first_person, true, 0, 1},
    {Field::fov, Action::set_first_person_fov, false, 0, first_person::maximum_fov},
    {Field::spring, Action::set_first_person_spring, true, 0, 1},
    {Field::board_only, Action::set_first_person_board_only, true, 0, 1},
    {Field::offset_x, Action::set_first_person_offset_x, false, -3, 3},
    {Field::offset_y, Action::set_first_person_offset_y, false, -3, 3},
    {Field::offset_z, Action::set_first_person_offset_z, false, -3, 3},
    {Field::pitch, Action::set_first_person_pitch, false, -180, 180},
    {Field::yaw, Action::set_first_person_yaw, false, -180, 180},
    {Field::roll, Action::set_first_person_roll, false, -180, 180},
    {Field::spring_up, Action::set_first_person_spring_up, false, 0, 100},
    {Field::spring_down, Action::set_first_person_spring_down, false, 0, 100},
    {Field::spring_left, Action::set_first_person_spring_left, false, 0, 100},
    {Field::spring_right, Action::set_first_person_spring_right, false, 0, 100},
    {Field::stabilize, Action::set_first_person_stabilize, true, 0, 1},
    {Field::follow_flips, Action::set_first_person_follow_flips, true, 0, 1},
    {Field::camera_cycle, Action::set_first_person_camera_cycle, true, 0, 1},
    {Field::smoothing, Action::set_first_person_smoothing, false, 0, 100},
    {Field::head_pitch, Action::set_first_person_head_pitch, false, 0, 100},
    {Field::head_roll, Action::set_first_person_head_roll, false, 0, 100},
    {Field::bob, Action::set_first_person_bob, false, 0, 100}
};
bool fail(char* result, std::size_t size, const char* text) {
    if (result && size) std::snprintf(result, size, "%s", text);
    return false;
}
}
bool queue(const overlay::CallbacksV3& callbacks, Field field, float value,
           char* result, std::size_t size) {
    if (result && size) result[0] = 0;
    if (!callbacks.queue_debug) return fail(result, size, "ReSkate camera queue unavailable.");
    for (const auto& route : routes) {
        if (route.field != field) continue;
        if ((field == Field::fov && !first_person::valid_fov_setting(value)) ||
            !std::isfinite(value) || value < route.low || value > route.high ||
            (route.boolean && value != 0 && value != 1))
            return fail(result, size, "Value is outside ReSkate's supported setting range.");
        const overlay::DebugRequest request{route.action, route.boolean && value != 0, route.boolean ? 0 : value};
        char fallback[512]{};
        return callbacks.queue_debug(callbacks.user, request, result && size ? result : fallback,
                                     result && size ? size : sizeof(fallback));
    }
    return fail(result, size, "Unknown First Person setting.");
}
}
