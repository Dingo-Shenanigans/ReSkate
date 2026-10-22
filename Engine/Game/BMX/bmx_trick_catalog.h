#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace dingosdk::bmx::catalog {
// Authored selection identity: exact installed Streets BMX_TrickDataSets resources.
// Named IK-weight minima inform release masks; timings and motion targets remain
// bounded retargeting choices, not Unity replay or native collider animation.
enum class TrickId : std::uint8_t {
    grand_slam_left=0,
    twix_right=1,
    helicopter_right=2,
    bri_flip_left=3,
    willy_flip=4,
    mc_griddle_left=5,
    kick_whip_left=6,
    superman=7,
    grand_slam_right=10,
    christ_air=11,
    kick_whip_right=12,
    mc_griddle_right=13,
    bar_hug=14,
    bri_flip_right=15,
    helicopter_left=16,
    nothing=17,
    tuck_no=20,
    table=21,
    invert=22,
    turndown=23,
    suicide=24,
    euro=25,
    lookback=26,
    cannon_ball=27,
    tire_grab_left=31,
    seat_grab_left=32,
    bus_driver_left=33,
    barspin_left=34,
    toboggan_left=35,
    one_hander_left=36,
    xup_left=37,
    xup_right=41,
    one_hander_right=42,
    toboggan_right=43,
    barspin_right=44,
    bus_driver_right=45,
    seat_grab_right=46,
    tire_grab_right=47,
    tuck_up=50,
    no_foot_can_right=51,
    tailwhip_right=52,
    whopper_right=53,
    crankflip=54,
    whopper_left=55,
    tailwhip_left=56,
    no_foot_can_left=57,
    can_left=61,
    moto_left=62,
    nac_left=63,
    griz_air_left=65,
    one_footer_left=66,
    candy_bar_left=67,
    candy_bar_right=71,
    one_footer_right=72,
    griz_air_right=73,
    nac_right=75,
    moto_right=76,
    can_right=77,
    none=255
};
enum class Family : std::uint8_t { grand_slam, twix, helicopter, bri_flip, willy_flip, mc_griddle, kick_whip, superman, christ_air, bar_hug, nothing, tuck_no, table, invert, turndown, suicide, euro, lookback, cannon_ball, tire_grab, seat_grab, bus_driver, barspin, toboggan, one_hander, xup, tuck_up, no_foot_can, tailwhip, whopper, crankflip, can, moto, nac, griz_air, one_footer, candy_bar };
enum class Motion : std::uint8_t { pose, cycle };
enum Release : std::uint8_t { left_hand=1, right_hand=2, left_foot=4, right_foot=8 };
struct Entry {
    TrickId id; std::string_view name; Family family; int direction;
    std::uint8_t release_mask; Motion motion; double enter_seconds, recover_seconds;
    int bar_turns, tail_turns, crank_turns; std::uint32_t source_object;
    bool shared_clip_name; // BMX binding may reference a Scooter-named SyncAnimator.
    // Target/cycle amplitudes in the shared bike-local X pitch / Z roll basis.
    // Table/Euro/Invert signs follow named Joints rotation tracks; complex shared
    // Grand/Bri motion is an explicit bounded adaptation, not a two-axis replay.
    double bike_pitch{}, bike_roll{};
    double bar_pose_radians{}; // held-pose bar articulation, separate from a full barspin
};
inline constexpr std::array<Entry,58> entries{{
    {TrickId::grand_slam_left, "Grand Slam Left", Family::grand_slam, -1, 15, Motion::cycle, 0.45, .12, 0, 0, 0, 46222, true, 6.283185307179586, -.55},
    {TrickId::twix_right, "Twix Right", Family::twix, 1, 15, Motion::cycle, 0.55, .12, 1, 1, 0, 46109, false},
    {TrickId::helicopter_right, "Helicopter Right", Family::helicopter, 1, 15, Motion::cycle, 0.55, .12, 1, 1, 0, 46019, false},
    {TrickId::bri_flip_left, "Bri Flip Left", Family::bri_flip, -1, 12, Motion::cycle, 0.45, .12, 0, 0, 0, 46180, true, -.35, -6.283185307179586},
    {TrickId::willy_flip, "Willy Flip", Family::willy_flip, 0, 15, Motion::cycle, 0.45, .12, 0, 0, 0, 46124, false, 6.283185307179586, 0},
    {TrickId::mc_griddle_left, "Mc Griddle Left", Family::mc_griddle, -1, 12, Motion::cycle, 0.45, .12, 0, -1, 0, 46034, false},
    {TrickId::kick_whip_left, "Kick Whip Left", Family::kick_whip, -1, 15, Motion::cycle, 0.55, .12, 0, -1, 0, 46025, false},
    {TrickId::superman, "Superman", Family::superman, 0, 14, Motion::pose, 0.18, .12, 0, 0, 0, 46082, false},
    {TrickId::grand_slam_right, "Grand Slam Right", Family::grand_slam, 1, 15, Motion::cycle, 0.45, .12, 0, 0, 0, 46225, true, 6.283185307179586, .55},
    {TrickId::christ_air, "Christ Air", Family::christ_air, 0, 15, Motion::pose, 0.18, .12, 0, 0, 0, 46001, false},
    {TrickId::kick_whip_right, "Kick Whip Right", Family::kick_whip, 1, 15, Motion::cycle, 0.55, .12, 0, 1, 0, 46028, false},
    {TrickId::mc_griddle_right, "Mc Griddle Right", Family::mc_griddle, 1, 12, Motion::cycle, 0.45, .12, 0, 1, 0, 46037, false},
    {TrickId::bar_hug, "Bar Hug", Family::bar_hug, 0, 0, Motion::pose, 0.18, .12, 0, 0, 0, 45971, false},
    {TrickId::bri_flip_right, "Bri Flip Right", Family::bri_flip, 1, 12, Motion::cycle, 0.45, .12, 0, 0, 0, 46183, true, -.35, 6.283185307179586},
    {TrickId::helicopter_left, "Helicopter Left", Family::helicopter, -1, 15, Motion::cycle, 0.55, .12, -1, -1, 0, 46016, false},
    {TrickId::nothing, "Nothing", Family::nothing, 0, 15, Motion::pose, 0.18, .12, 0, 0, 0, 46058, false},
    {TrickId::tuck_no, "Tuck No", Family::tuck_no, 0, 3, Motion::pose, 0.18, .12, 0, 0, 0, 46100, false},
    {TrickId::table, "Table", Family::table, 0, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46085, false, 0, 1.38},
    {TrickId::invert, "Invert", Family::invert, 0, 4, Motion::pose, 0.18, .12, 0, 0, 0, 46022, false, -1.18, 1.45},
    {TrickId::turndown, "Turndown", Family::turndown, 0, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46106, false, 0, 0, -2.375},
    {TrickId::suicide, "Suicide", Family::suicide, 0, 3, Motion::pose, 0.18, .12, 0, 0, 0, 46079, false},
    {TrickId::euro, "Euro", Family::euro, 0, 4, Motion::pose, 0.18, .12, 0, 0, 0, 46007, false, -.12, 1.28},
    {TrickId::lookback, "Lookback", Family::lookback, 0, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46031, false, 0, 0, -3.141592653589793},
    {TrickId::cannon_ball, "Cannon Ball", Family::cannon_ball, 0, 15, Motion::pose, 0.18, .12, 0, 0, 0, 45998, false},
    {TrickId::tire_grab_left, "Tire Grab Left", Family::tire_grab, -1, 1, Motion::pose, 0.18, .12, 0, 0, 0, 46088, false},
    {TrickId::seat_grab_left, "Seat Grab Left", Family::seat_grab, -1, 1, Motion::pose, 0.18, .12, 0, 0, 0, 46073, false},
    {TrickId::bus_driver_left, "Bus Driver Left", Family::bus_driver, -1, 2, Motion::cycle, 0.45, .12, -1, 0, 0, 45980, false},
    {TrickId::barspin_left, "Barspin Left", Family::barspin, -1, 3, Motion::cycle, 0.45, .12, -1, 0, 0, 45974, false},
    {TrickId::toboggan_left, "Toboggan Left", Family::toboggan, -1, 1, Motion::pose, 0.18, .12, 0, 0, 0, 46094, false},
    {TrickId::one_hander_left, "One Hander Left", Family::one_hander, -1, 1, Motion::pose, 0.18, .12, 0, 0, 0, 46067, false},
    {TrickId::xup_left, "Xup Left", Family::xup, -1, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46127, false, 0, 0, -3.141592653589793},
    {TrickId::xup_right, "Xup Right", Family::xup, 1, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46130, false, 0, 0, 3.141592653589793},
    {TrickId::one_hander_right, "One Hander Right", Family::one_hander, 1, 2, Motion::pose, 0.18, .12, 0, 0, 0, 46070, false},
    {TrickId::toboggan_right, "Toboggan Right", Family::toboggan, 1, 2, Motion::pose, 0.18, .12, 0, 0, 0, 46097, false},
    {TrickId::barspin_right, "Barspin Right", Family::barspin, 1, 3, Motion::cycle, 0.45, .12, 1, 0, 0, 45977, false},
    {TrickId::bus_driver_right, "Bus Driver Right", Family::bus_driver, 1, 1, Motion::cycle, 0.45, .12, 1, 0, 0, 45983, false},
    {TrickId::seat_grab_right, "Seat Grab Right", Family::seat_grab, 1, 2, Motion::pose, 0.18, .12, 0, 0, 0, 46076, false},
    {TrickId::tire_grab_right, "Tire Grab Right", Family::tire_grab, 1, 2, Motion::pose, 0.18, .12, 0, 0, 0, 46091, false},
    {TrickId::tuck_up, "Tuck Up", Family::tuck_up, 0, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46103, false},
    {TrickId::no_foot_can_right, "No Foot Can Right", Family::no_foot_can, 1, 12, Motion::pose, 0.18, .12, 0, 0, 0, 46055, false},
    {TrickId::tailwhip_right, "Whip Right", Family::tailwhip, 1, 12, Motion::cycle, 0.55, .12, 0, 1, 0, 46115, false},
    {TrickId::whopper_right, "Whopper Right", Family::whopper, 1, 12, Motion::cycle, 0.55, .12, 1, 1, 0, 46121, false},
    {TrickId::crankflip, "Crankflip", Family::crankflip, 0, 12, Motion::cycle, 0.45, .12, 0, 0, 1, 46004, false},
    {TrickId::whopper_left, "Whopper Left", Family::whopper, -1, 12, Motion::cycle, 0.55, .12, -1, -1, 0, 46118, false},
    {TrickId::tailwhip_left, "Whip Left", Family::tailwhip, -1, 12, Motion::cycle, 0.55, .12, 0, -1, 0, 46112, false},
    {TrickId::no_foot_can_left, "No Foot Can Left", Family::no_foot_can, -1, 12, Motion::pose, 0.18, .12, 0, 0, 0, 46052, false},
    {TrickId::can_left, "Can Left", Family::can, -1, 4, Motion::pose, 0.18, .12, 0, 0, 0, 45986, false},
    {TrickId::moto_left, "Moto Left", Family::moto, -1, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46040, false},
    {TrickId::nac_left, "Nac Left", Family::nac, -1, 4, Motion::pose, 0.18, .12, 0, 0, 0, 46046, false},
    {TrickId::griz_air_left, "Griz Air Left", Family::griz_air, -1, 4, Motion::pose, 0.18, .12, 0, 0, 0, 46010, false},
    {TrickId::one_footer_left, "One Footer Left", Family::one_footer, -1, 4, Motion::pose, 0.18, .12, 0, 0, 0, 46061, false},
    {TrickId::candy_bar_left, "Candy Bar Left", Family::candy_bar, -1, 4, Motion::pose, 0.18, .12, 0, 0, 0, 45992, false},
    {TrickId::candy_bar_right, "Candy Bar Right", Family::candy_bar, 1, 8, Motion::pose, 0.18, .12, 0, 0, 0, 45995, false},
    {TrickId::one_footer_right, "One Footer Right", Family::one_footer, 1, 8, Motion::pose, 0.18, .12, 0, 0, 0, 46064, false},
    {TrickId::griz_air_right, "Griz Air Right", Family::griz_air, 1, 8, Motion::pose, 0.18, .12, 0, 0, 0, 46013, false},
    {TrickId::nac_right, "Nac Right", Family::nac, 1, 8, Motion::pose, 0.18, .12, 0, 0, 0, 46049, false},
    {TrickId::moto_right, "Moto Right", Family::moto, 1, 0, Motion::pose, 0.18, .12, 0, 0, 0, 46043, false},
    {TrickId::can_right, "Can Right", Family::can, 1, 8, Motion::pose, 0.18, .12, 0, 0, 0, 45989, false},
}};
inline constexpr const Entry* find(TrickId id) noexcept {
    for(const auto& entry:entries)if(entry.id==id)return &entry;
    return nullptr;
}
inline constexpr TrickId select(int set,int slot) noexcept {
    if(set<0||set>=8||slot<0||slot>=8)return TrickId::none;
    const auto id=static_cast<TrickId>(set*10+slot);
    return find(id)?id:TrickId::none;
}
inline constexpr bool legacy_rotation(TrickId id) noexcept {
    return id==TrickId::barspin_left||id==TrickId::barspin_right||
        id==TrickId::tailwhip_left||id==TrickId::tailwhip_right;
}
inline constexpr bool valid(TrickId id) noexcept { return id==TrickId::none||find(id); }
} // namespace dingosdk::bmx::catalog
