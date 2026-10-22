#pragma once
#include "bmx_native_coupled.h"
#include "bmx_chassis_ride.h"
#include "bmx_coupled_ride_frame.h"
namespace dingosdk::bmx::coupled_ride {
struct Result {
    chassis_ride::Step step;
    native_coupled::Frame actual_pair;
    coupled_ride_frame::RiderWitness actual_spring;
    bool prepared{},physics_active{},controller_committed{},evaluated{},queued{};
    bool avatar_binding_verified{},ragdoll_momentum_transfer_verified{};
    native_coupled::Command command;bool command_available{},native_before_cleanup_available{},actual_pair_observed_this_update{};
    native_coupled::Status native_before_cleanup; // exact rejection reason before generic park/stop
    const char* issue="not_started";
};
// Exact coupled private profile +distinct explicit process flag. Never selects
// the old one-body controller or changes the backend's adoption qualification.
bool selected() noexcept;
bool prepare(std::uintptr_t,const NativeFrame&,const mount_pose::Seed&,float actual_native_root_height) noexcept;
// Caller must first qualify its completed mounted avatar/render entry. Native
// collision/force qualification remains an additional independent hard gate.
bool activate_prepared(std::uintptr_t,const NativeFrame&) noexcept;
bool start(std::uintptr_t,const NativeFrame&,const mount_pose::Seed&,float actual_native_root_height) noexcept;
Result update(std::uintptr_t,const NativeFrame&,const RawInput&) noexcept;
void stop(std::uintptr_t,const NativeFrame&) noexcept;
Result status() noexcept; // client thread only; bounded SDK copy, not fresh native proof
}
