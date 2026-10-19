#pragma once
#include <cstdint>
#include <array>
#include <optional>
namespace jishsfirstpersonmod::player_state_contract {
// From custom.2 no_bail::resolve / filter_contact_animation, supported game only.
inline constexpr std::uintptr_t core_vtable=0x6176d30;
inline constexpr std::uintptr_t component_offset=0x628, core_offset=0x70,
    physics_context_offset=0x3c0, selector_offset=0x440, physics_state_offset=0x1414;
// ReSkate-main.zip: no_bail.h, client_noclip.cpp and trainer_jump.cpp.
// 101/201 are observed but not individually named in that source.
enum class PhysicsState:std::uint32_t {
    riding_ground=100,crouch=103,pop=200,wipeout_ground=300,
    offboard=504,boneless_foot_plant=602
};
inline constexpr std::uint32_t wipeout=static_cast<std::uint32_t>(PhysicsState::wipeout_ground),
    offboard=static_cast<std::uint32_t>(PhysicsState::offboard);
inline const char* state_name(std::uint32_t state) {
    switch(static_cast<PhysicsState>(state)) {
    case PhysicsState::riding_ground:return "Riding on ground";
    case PhysicsState::crouch:return "Crouch";
    case PhysicsState::pop:return "Pop";
    case PhysicsState::wipeout_ground:return "Wipeout (before remap)";
    case PhysicsState::offboard:return "Off-board / remapped wipeout";
    case PhysicsState::boneless_foot_plant:return "Boneless foot plant";
    }
    if(state>=100 && state<200) return "Ground family";
    if(state>=200 && state<300) return "Air family";
    if(state>=400 && state<500) return "Grind family";
    return "Unmapped state";
}
inline constexpr std::uintptr_t choose_state_rva=0x4745a70,caller_rva=0x47da1cf;
inline constexpr std::array<unsigned char,32> caller_prefix{
    0x48,0x8b,0x8b,0x40,0x04,0x00,0x00,0x8b,0xd0,0x8b,0xf0,0xe8,0x91,0xb8,0xf6,0xff,
    0x8b,0xf8,0xb8,0xf8,0x01,0x00,0x00,0x81,0xff,0x2c,0x01,0x00,0x00,0x0f,0x44,0xf8};
// Verified stock-game publisher: core+3b8 -> +38 owns the animation output.
inline constexpr std::uintptr_t animation_owner=0x3b8,animation_output=0x38,
    wipeout_request_offset=0x87,animation_wipeout_offset=0x8d,recovery_offset=0x9e;
inline constexpr std::uintptr_t animation_request_site=0x47da8d7;
inline constexpr std::array<unsigned char,32> animation_request_prefix{
    0x8b,0x90,0xc4,0x13,0x00,0x00,0x48,0x8b,0x87,0xb8,0x03,0x00,0x00,0xc1,0xea,0x0f,
    0x80,0xe2,0x01,0x48,0x8b,0x48,0x38,0x88,0x91,0x87,0x00,0x00,0x00,0x48,0x8b,0x87};
inline constexpr std::uintptr_t animation_bail_site=0x47da965,recovery_site=0x47da984;
inline constexpr std::array<unsigned char,32> animation_bail_prefix{
    0x48,0x8b,0x87,0xc0,0x03,0x00,0x00,0xf7,0x80,0xd4,0x13,0x00,0x00,0x00,0x00,0x00,
    0x08,0x74,0x04,0xb0,0x01,0xeb,0x02,0x32,0xc0,0x88,0x83,0x8d,0x00,0x00,0x00,0x48};
inline constexpr std::array<unsigned char,18> recovery_prefix{
    0x48,0x8b,0x87,0xb8,0x03,0x00,0x00,0x48,0x8b,0x48,0x38,0x40,0x88,0xb1,0x9e,0x00,0x00,0x00};
// Custom.2 identifies ground/air and grind families. Live observations include
// 100/101/103, 200/201 and 504; user confirmed walking and bailing both show 504.
// Both should be third person for board-only. This is not a bail classifier.
inline constexpr bool board_verified=true,bail_verified=true;
// ReSkate-main offboard_flight.h/cpp: native parent owns these normal movement
// substates. Other animation/trajectory/ragdoll children do not prove recovery.
inline constexpr std::uintptr_t movement_parent_offset=0x3b0,offboard_vtable=0x65e8f40,
    active_substate_offset=0x48;
struct FootState { std::uintptr_t offset,vtable,update; };
inline constexpr std::array<FootState,3> normal_foot_states{{
    {0x60,0x65e8c20,0x48318a0}, // FollowGround
    {0x1c0,0x65e8c80,0x4832530}, // FollowGroundSlide
    {0x5b0,0x65e8da0,0x4820860} // Falling
}};
inline constexpr std::uintptr_t offboard_owner_site=0x47bf916,offboard_constructor_site=0x48108e9;
inline constexpr std::array<unsigned char,32> offboard_owner_prefix{
    0x4c,0x8b,0x8b,0x38,0x04,0x00,0x00,0x48,0x8d,0x4c,0x24,0x50,0x4c,0x8b,0x83,0x30,
    0x04,0x00,0x00,0x48,0x8b,0x93,0xc0,0x03,0x00,0x00,0x48,0x89,0x4c,0x24,0x20,0x48};
inline constexpr std::array<unsigned char,32> offboard_constructor_prefix{
    0x4c,0x89,0x41,0x10,0x4c,0x89,0x49,0x18,0x48,0x8b,0x42,0x08,0x48,0x89,0x41,0x20,
    0x48,0x8b,0x02,0x48,0x89,0x41,0x28,0x48,0x8b,0x42,0x10,0x48,0x89,0x41,0x30,0x48};
inline std::optional<bool> riding(std::uint32_t state) {
    if((state>=100 && state<300) || (state>=400 && state<500)) return true;
    if(state==offboard || state==wipeout) return false;
    return {}; // Unverified mounting/handplant/lifecycle states suspend policy.
}
}
