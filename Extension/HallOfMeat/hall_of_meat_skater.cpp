#include "hall_of_meat_skater.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/skater_body.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace dingosdk::hall_of_meat {
namespace {
namespace body = addr::skater_body;
namespace state = addr::skater_state;
using game::Vec3;
static_assert(body::body_bone_count == skater_body::count);

std::atomic<bool>& ready() {
    static std::atomic<bool> value{};
    return value;
}

// What each body touched in the step: the contact struct the rig's contact processing fills.
using Records = std::array<unsigned char, body::body_bone_count * body::bone_record_size>;
template <class T> T field(const Records& records, std::size_t index, std::uintptr_t offset) noexcept {
    T value{};
    std::memcpy(&value, records.data() + index * body::bone_record_size + offset, sizeof(T));
    return value;
}
Vec3 vec3(const std::array<float, 4>& value) noexcept {
    for (std::size_t i = 0; i < 3; ++i)
        if (!std::isfinite(value[i])) return {};
    return {value[0], value[1], value[2]};
}
float speed(float value) noexcept { return std::isfinite(value) && value > 0 && value < 1000 ? value : 0; }
bool read_contacts(const NoBailSkater& skater, skater_body::Contacts& result) noexcept {
    std::uintptr_t holder{}, contacts{};
    Records records{};
    std::array<std::uint8_t, skater_body::count> touching{};
    if (!memory::peek(skater.rig + body::contact_holder_offset, holder) || !memory::peek(holder + body::contact_struct_offset, contacts) ||
        !memory::peek(contacts + body::bone_records_offset, records) || !memory::peek(contacts + body::bone_touching_offset, touching))
        return false;
    for (std::size_t index = 0; index < skater_body::count; ++index) {
        auto& b = result.bodies[index];
        b.touching = touching[index] != 0;
        const float ordinary = speed(field<float>(records, index, body::bone_peak_offset));
        const float tracked = speed(field<float>(records, index, body::bone_tracked_peak_offset));
        b.impact = std::max(ordinary, tracked);
        // The slide of the contact that set the peak: one near a tracked point keeps its own.
        const std::uintptr_t side = tracked > ordinary ? body::bone_tracked_offset : 0;
        b.slide = vec3(field<std::array<float, 4>>(records, index, body::bone_slide_offset + side));
        const auto hit = [&](std::uintptr_t offset) { return field<std::uint8_t>(records, index, offset) != 0; };
        b.hit = {hit(body::bone_hit_board_offset), hit(body::bone_hit_vehicle_offset), hit(body::bone_hit_world_offset),
            hit(body::bone_hit_kind_5_offset), hit(body::bone_hit_kind_11_offset)};
    }
    return true;
}

// The offboard state's flags: the body in a ragdoll, and in the air.
struct Offboard {
    bool ragdoll{}, in_the_air{};
};
bool read_offboard(const NoBailSkater& skater, Offboard& flags) noexcept {
    std::uintptr_t trick_state{}, offboard{};
    std::uint8_t ragdoll{}, in_the_air{};
    if (!memory::peek(skater.core + state::trick_state_offset, trick_state) ||
        !memory::peek(trick_state + state::offboard_state_offset, offboard) ||
        !memory::peek(offboard + state::ragdoll_offset, ragdoll) || !memory::peek(offboard + state::in_the_air_offset, in_the_air))
        return false;
    flags = {ragdoll != 0, in_the_air != 0};
    return true;
}

bool finite(const Vec3& v) noexcept {
    return std::all_of(v.begin(), v.end(), [](float value) { return std::isfinite(value) && std::abs(value) < 1000; });
}
// Body `index` of the board's or the skeleton's physics, when the layout holds: its velocity, and
// its spin when asked for.
bool body_motion(std::uintptr_t physics, std::uintptr_t vtable, std::uint32_t count, std::size_t index, Vec3& velocity,
    Vec3* spin = nullptr) noexcept {
    std::uintptr_t type{}, bodies{}, owner{};
    std::uint32_t found{};
    const auto at = [&] { return bodies + index * state::physics_body_size; };
    if (!memory::peek(physics, type) || type != vtable || !memory::peek(physics + state::physics_bodies_offset, bodies) ||
        !memory::peek(bodies, found) || found != count || index >= count ||
        !memory::peek(at() + state::body_owner_offset, owner) || owner != physics ||
        !memory::peek(at() + state::body_velocity_offset, velocity) || (spin && !memory::peek(at() + state::body_spin_offset, *spin)))
        return false;
    return finite(velocity) && (!spin || finite(*spin));
}
// The board's root, and the pelvis with its spin.
bool read_motion(const NoBailSkater& skater, Vec3& board, Vec3& pelvis, Vec3& spin) noexcept {
    std::uintptr_t holder{}, board_physics{}, rig_physics{};
    return memory::peek(skater.core + state::board_holder_offset, holder) &&
        memory::peek(holder + state::board_physics_offset, board_physics) &&
        memory::peek(skater.rig + state::rig_physics_offset, rig_physics) &&
        body_motion(board_physics, skater.base + state::board_physics_vtable, state::board_body_count, state::board_root_body, board) &&
        body_motion(rig_physics, skater.base + state::rig_physics_vtable, state::rig_body_count,
            skater_body::index(skater_body::Bone::hips), pelvis, &spin);
}

// Where the local skater's physics step length is kept now (a float, seconds of game time), 0 while
// there is no local skater.
std::uintptr_t step_length_address() noexcept {
    NoBailSkater skater;
    std::uintptr_t input{};
    if (!no_bail_skater(skater) || !memory::peek(skater.core + body::core_step_input_offset, input) || !input) return 0;
    return input + body::step_input_length_offset;
}
}

bool start_skater(std::uintptr_t base) noexcept {
    if (ready().load(std::memory_order_acquire)) return true;
    for (const auto& contract : body::contracts) {
        std::array<unsigned char, 32> actual{};
        if (!memory::peek(base + contract.rva, actual) || actual != contract.bytes) {
            logging::log(logging::Level::warning, logging::Channel::skater,
                "Hall of Meat cannot read the skater's body: the native contract at 0x{:x} did not match.", contract.rva);
            return false;
        }
    }
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Hall of Meat cannot follow the skater: its physics steps come through No Bail's hooks, which did not start.");
        return false;
    }
    ready().store(true, std::memory_order_release);
    return true;
}

bool skater_available() noexcept { return ready().load(std::memory_order_acquire); }

Step read_step(const NoBailSkater& skater, float seconds, bool wipeout) noexcept {
    Step step;
    step.seconds = seconds;
    step.wipeout = wipeout;
    std::uint32_t physics_state{};
    if (memory::peek(skater.context + state::physics_state_offset, physics_state)) {
        // On the board by the physics state; off it (on foot, and through a whole bail) by the offboard state.
        const bool offboard = physics_state == addr::no_bail::offboard_physics_state;
        Offboard flags;
        const bool known = offboard && read_offboard(skater, flags);
        step.airborne = std::find(state::board_air_states.begin(), state::board_air_states.end(), physics_state) !=
            state::board_air_states.end() || (known && flags.in_the_air);
        if (!offboard || known) step.ragdoll = known && flags.ragdoll;
        Vec3 board{}, pelvis{}, spin{};
        if (read_motion(skater, board, pelvis, spin)) {
            step.velocity = offboard ? pelvis : board;
            step.spin = game::length(spin);
        }
    }
    (void)read_contacts(skater, step.body);
    return step;
}

float step_length() noexcept {
    const auto address = step_length_address();
    float seconds{};
    return address && memory::peek(address, seconds) ? seconds : 0.0f;
}

bool set_step_length(float seconds) noexcept {
    const auto address = step_length_address();
    float current{};
    if (!address || !memory::peek(address, current)) return false;
    if (current == seconds) return true;
    __try {
        *reinterpret_cast<volatile float*>(address) = seconds; // read by the physics thread each step
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}
