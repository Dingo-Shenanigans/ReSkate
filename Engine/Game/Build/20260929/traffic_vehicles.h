#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// The hierarchy pass for moving ECS entities. It can run on job-system threads.
namespace dingosdk::game::build::v20260929::traffic_vehicles {
inline constexpr std::uintptr_t hierarchy_update = 0x287d680;
inline constexpr std::array<unsigned char, 16> hierarchy_update_prefix{
    0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c};
inline constexpr std::uintptr_t query_result_vtable = 0x607af90;
inline constexpr std::uintptr_t local_transform_type = 0x74d4690;
inline constexpr std::uintptr_t world_transform_type = 0x74d63d0;
// The mission override hooks the component query (mission_progression::component_query) that this
// code calls. Its first five bytes are four pushes, which Detours replaces with a five-byte jump
// (0xe9), so once that hook is in only the prefix bytes after them still read as the game's
// (in game, take23: every take was refused on these bytes).
inline constexpr std::size_t component_query_hooked_bytes = 5;
inline constexpr unsigned char detours_jump = 0xe9;
}
