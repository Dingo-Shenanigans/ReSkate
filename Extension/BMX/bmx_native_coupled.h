#pragma once
#include "bmx_native_chassis.h"
#include "Engine/Game/BMX/bmx_native_coupled_policy.h"
#include "Engine/Game/BMX/bmx_native_coupled_calibration.h"
#include "Engine/Game/BMX/bmx_native_coupled_force_probe.h"
#include "Engine/Game/BMX/bmx_native_coupled_trial.h"

namespace dingosdk::bmx::native_coupled {
using Vec=policy::Vec;using Quat=policy::Quat;using Command=policy::Command;
struct Frame {
    bool available{},active{},parked{},detached{};
    std::array<native_chassis::Frame,2> roles; // actual native COM, velocities, principal-axis orientation/inertia
    std::array<Quat,2> model_orientation{}; // decoded model frame; no native pose correction
    std::array<float,2> maximum_angular_speed{}; // measured state998 property, not a postsolve bound claim
    std::array<float,2> allocated_maximum_angular_speed{}; // cached actual allocation readback before the setter
    std::array<bool,2> integrates_angular_accumulator{}; // actual stateA68 bits, never changed here
    std::array<std::uint32_t,8> categories{},collides_with{};
    policy::Epoch epoch;
    const char* issue="not_prepared";
};
struct Status {
    bool attempted{},prepared{},active{},parked{},detached{},failed{},blocked{};
    bool force_adoption_enabled{}; // hard false until mutual-pair/world collision proof exists
    unsigned live_bodies{},live_shapes{},allocation_attempts{};
    std::uint64_t token{},observations{},producer_matches{},submissions{},rejected{};
    policy::Receipt receipt;
    unsigned calibration_admission_rejections{},calibration_rejected_role=0xffffffffU;
    const char* calibration_admission_issue="not_attempted";
    const char* issue="not_prepared";
};
// Private exact development root + process opt-in; never selected implicitly.
bool enabled() noexcept;
// Allocates two genuine30/80kg engine bodies, five bike and three rider shapes.
// Initial milestone leaves both keyframed and all proxies disabled. activate
// remains closed until native mutual-pair suppression AND world collision are proved.
bool acquire(std::uintptr_t base,const NativeFrame&,const mount_pose::Seed&) noexcept;
Frame observe(std::uintptr_t base,const NativeFrame&) noexcept;
bool activate(std::uintptr_t base,const NativeFrame&) noexcept;
bool submit(const Frame&,const Command&) noexcept;
void cancel() noexcept; // SDK-only; cancels BOTH role commands
bool park(std::uintptr_t base,const NativeFrame&) noexcept;
bool detach(std::uintptr_t base,const NativeFrame&) noexcept; // future active mode: park Spring, retain dynamic bike
void forget() noexcept; // SDK-only; no stale-world cleanup
Status status() noexcept;
// Separate one-shot neutral diagnostic, never enables controller force adoption.
// Requires exact private root, RESKATE_BMX_COUPLED_CALIBRATION=pair|world, two
// fresh final-support seeds in the common current epoch, and client-thread polls.
// Caller must arrange the reviewed clear region and poll every native epoch.
bool begin_calibration(std::uintptr_t,const NativeFrame&,calibration::Kind,const std::array<mount_pose::Seed,2>&) noexcept;
calibration::Report poll_calibration(std::uintptr_t,const NativeFrame&,bool gates) noexcept;
calibration::Report calibration_report() noexcept; // client thread only, copied bounded48 samples
namespace force_probe {
// Separate exact-profile six-epoch diagnostic. No public arbitrary command is
// accepted, and normal collision/ride force adoption stays disabled.
bool selected() noexcept;
bool begin(std::uintptr_t,const NativeFrame&,const std::array<mount_pose::Seed,2>&) noexcept;
Report poll(std::uintptr_t,const NativeFrame&,bool gates) noexcept;
Report current_report() noexcept;
}
namespace detail {
// Existing synchronize hook dispatches here only with the compile feature ON.
void produce(void* incoming,std::uintptr_t wrapper) noexcept;
bool allocation_claimed() noexcept;
}
namespace controller_trial {
// Requires both developer flags/exact profile and both compiled features.
// Ordinary activate/submit remain closed; this trial has a10second backend TTL.
bool selected() noexcept;
bool activate(std::uintptr_t,const NativeFrame&) noexcept;
bool submit(const Frame&,const Command&) noexcept;
}
}
