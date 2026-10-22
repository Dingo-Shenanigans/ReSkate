#pragma once
#include "bmx_native_motion.h"
#include <array>
#include <cstdint>
#include <string>

namespace dingosdk::bmx::ramp_test {
struct Snapshot {
    bool requested{}, completed{}, prepared{}, failed{};
    std::string phase{"not_requested"}, issue, item, map, report_path;
    std::uint64_t object_id{};
    std::array<float, 3> fixture_origin{};
    float fixture_yaw{};
    unsigned baseline_samples{}, surface_samples{}, raised_samples{};
};
struct RideRoute {
    bool ready{};
    std::array<float, 3> start{}, forward{}, origin{};
    float yaw{}, center_right{}, foot_z{}, lip_z{}, lip_height{}, floor_y{};
    std::uint64_t object_id{};
    NativeFrame owner;
    std::uint64_t prepared_at_ms{};
    std::uint64_t world{}, generation{}, revision{};
};
// Explicit opt-in in the validated bmx-user-data-ramp-test development profile.
// A request is consumed once per process. This first experiment only creates
// one native BuildKit item and measures its collision; it never drives/teleports.
bool enabled() noexcept;
bool request() noexcept;
// Separate-process follow-up: verify the one existing fixture and a fresh
// approach/landing corridor, then queue one ordinary native teleport. No new
// object is placed and no riding controls or continuous pose writes are sent.
bool request_ride() noexcept;
// Verified client update thread, after the ordinary BMX tick; no motion lease
// is acquired. At most four existing native surface queries run per tick.
void tick(std::uintptr_t base, const NativeFrame& rider, bool gameplay_ready) noexcept;
Snapshot snapshot();
RideRoute route();
}
