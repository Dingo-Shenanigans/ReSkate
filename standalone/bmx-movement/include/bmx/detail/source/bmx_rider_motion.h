#pragma once
#include "bmx_visual.h"
#include "bmx_geometry.h"
#include "bmx_rider_posture_presentation.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace bmx::detail::source::rider::motion {
using Vec=std::array<float,3>;
// Relative to the walker's ground/root projection, before yaw. Keeps the
// top-tube grip within a native-length arm through walking torso/hip motion.
inline constexpr Vec carry_bike_offset{-.34f,.47f,0};
inline constexpr Vec carry_palm_local=geometry::carry_tube_center;
// Native-proportion IK adaptations. Authored Sync/Animancer identities and
// named IKWeight contact cues are recovered; these target coordinates are local
// retargeting choices, NOT decoded Unity humanoid muscle rotations.
struct Targets {
    Vec hip_offset{}, torso_offset{};
    float hip_yaw{}, hip_roll{}, head_yaw{};
    std::array<Vec,2> palms{{{.36f,1.25f,.30f},{-.36f,1.25f,.30f}}};
    std::array<Vec,2> ankles{{{.26f,.99f,-.23f},{-.26f,.99f,-.23f}}};
};
inline bool valid(const visual::Frame& frame) {
    if(!catalog::valid(frame.movement_id))return false;
    for(float v:{frame.movement_phase,frame.movement_progress,frame.release_left_hand,
        frame.release_right_hand,frame.release_left_foot,frame.release_right_foot,
        frame.landing_compression,frame.pedal_effort,frame.air_extension_blend})if(!std::isfinite(v)||v<0||v>1)return false;
    if(frame.rider_posture_available&&
       (!std::isfinite(frame.rider_uphill_blend)||frame.rider_uphill_blend<0||frame.rider_uphill_blend>1||
        !std::isfinite(frame.rider_gaze_counterpitch)||frame.rider_gaze_counterpitch<0||
        frame.rider_gaze_counterpitch>rider_posture::maximum_counterpitch))return false;
    return true;
}
inline Targets sample(const visual::Frame& frame) {
    Targets out;
    const auto* entry=catalog::find(frame.movement_id);if(!entry)return out;
    using F=catalog::Family;
    const float d=entry->direction>0?-1.f:1.f; // bike +X is rider-left
    const float phase=std::clamp(frame.movement_phase,0.f,1.f);
    const auto hands=[&](float x,float y,float z){out.palms={Vec{x,y,z},Vec{-x,y,z}};};
    const auto feet=[&](float x,float y,float z){out.ankles={Vec{x,y,z},Vec{-x,y,z}};};
    switch(entry->family) {
    case F::superman:
        out.hip_offset={0,-.05f,-.18f};out.torso_offset={0,-.06f,-.12f};
        feet(.16f,.94f,-.92f);out.palms[1]={-.48f,1.24f,-.18f};break;
    case F::christ_air:
        out.hip_offset={0,.07f,-.02f};out.torso_offset={0,.04f,-.12f};
        hands(.68f,1.48f,-.02f);feet(.24f,.43f,-.30f);break;
    case F::nothing:
        out.hip_offset={0,.05f,-.02f};hands(.56f,1.40f,.05f);feet(.32f,.78f,-.32f);break;
    case F::tuck_no:
        out.hip_offset={0,-.09f,.03f};out.torso_offset={0,-.08f,-.05f};hands(.57f,1.50f,-.04f);break;
    case F::suicide:
        out.hip_offset={0,.02f,-.02f};out.torso_offset={0,.02f,-.11f};hands(.50f,1.35f,-.25f);break;
    case F::cannon_ball:
        out.hip_offset={0,-.12f,.04f};out.torso_offset={0,-.12f,-.02f};
        hands(.20f,1.02f,.12f);feet(.18f,1.00f,.13f);break;
    case F::bar_hug:
        out.hip_offset={0,-.15f,.09f};out.torso_offset={0,-.13f,.12f};break;
    case F::tire_grab:
        out.hip_offset={d*.025f,-.20f,.10f};out.torso_offset={d*.06f,-.19f,.10f};
        hands(.05f,.70f,.50f);out.head_yaw=d*.20f;break;
    case F::seat_grab:
        out.hip_offset={0,-.12f,-.04f};out.torso_offset={d*.06f,-.10f,-.15f};
        hands(.08f,.83f,-.25f);out.head_yaw=d*-.4f;break;
    case F::toboggan:
        out.hip_offset={d*.03f,-.15f,-.09f};out.torso_offset={d*.04f,-.13f,-.15f};
        hands(.08f,.85f,-.30f);out.hip_yaw=d*.18f;out.head_yaw=d*-.25f;break;
    case F::one_hander: hands(.65f,1.37f,.08f);out.torso_offset={-d*.03f,0,0};break;
    case F::griz_air:
        feet(.39f,1.02f,.14f);out.hip_offset={d*.035f,-.03f,0};out.hip_yaw=d*-.15f;break;
    case F::one_footer: feet(.47f,.67f,-.22f);break;
    case F::can:
        out.ankles={Vec{-.31f,.92f,.15f},Vec{.31f,.92f,.15f}};
        out.hip_yaw=d*-.16f;out.torso_offset={-d*.03f,0,0};break;
    case F::nac:
        out.ankles={Vec{-.27f,.76f,-.53f},Vec{.27f,.76f,-.53f}};
        out.hip_yaw=d*.20f;out.hip_offset={0,0,-.05f};break;
    case F::candy_bar:
        feet(.09f,1.18f,.43f);out.hip_offset={-d*.04f,-.06f,.06f};out.torso_offset={-d*.07f,-.03f,-.03f};break;
    case F::no_foot_can:
        out.ankles={Vec{d*.38f+.07f,.88f,.05f},Vec{d*.38f-.07f,.88f,.05f}};
        out.hip_offset={d*.045f,-.04f,.01f};out.hip_yaw=d*-.12f;break;
    case F::moto:
        out.hip_yaw=d*.25f;out.hip_roll=d*.14f;
        out.hip_offset={d*.06f,-.06f,-.04f};out.torso_offset={-d*.06f,-.02f,.02f};break;
    case F::table:
        out.hip_offset={-.065f,-.08f,0};out.torso_offset={-.10f,-.05f,.02f};out.hip_roll=-.18f;break;
    case F::invert:
        out.hip_offset={.13f,-.16f,.42f};out.torso_offset={.02f,-.14f,.20f};
        out.hip_roll=-.25f;out.ankles[0]={.34f,1.05f,.01f};break;
    case F::euro:
        out.hip_offset={-.04f,-.04f,-.03f};out.torso_offset={-.09f,-.02f,.04f};
        out.hip_roll=-.12f;out.ankles[0]={.32f,.94f,-.12f};break;
    case F::turndown:
        out.hip_yaw=.35f;out.hip_offset={.035f,-.15f,-.04f};out.torso_offset={-.08f,-.12f,0};out.head_yaw=-.35f;break;
    case F::lookback:
        out.hip_yaw=-.30f;out.hip_offset={0,-.20f,.025f};out.torso_offset={.08f,-.19f,-.04f};out.head_yaw=.75f;break;
    case F::xup:
        out.hip_yaw=d*-.25f;out.hip_offset={0,-.09f,0};out.torso_offset={d*.08f,-.09f,.01f};out.head_yaw=d*.2f;break;
    case F::tuck_up:
        out.hip_offset={0,-.18f,.08f};out.torso_offset={0,-.15f,.01f};break;
    case F::barspin:
        hands(.39f,1.25f,.24f);out.torso_offset={0,.015f,-.02f};break;
    case F::bus_driver:
        hands(.38f,1.25f,.23f);out.torso_offset={-d*.04f,-.025f,.01f};out.head_yaw=d*.15f;break;
    case F::tailwhip:
        feet(.30f,1.01f,-.27f);out.hip_offset={0,.015f,-.02f};break;
    case F::crankflip:
        feet(.23f,.93f,-.13f);out.hip_offset={0,-.025f,.01f};break;
    case F::whopper:
        feet(.34f,1.04f,-.23f);out.hip_offset={-d*.04f,-.06f,-.03f};out.torso_offset={d*.025f,-.05f,-.025f};break;
    case F::kick_whip:
        hands(.40f,1.25f,.27f);feet(.36f,.96f,-.23f);
        out.hip_yaw=d*.13f;break;
    case F::twix:
        hands(.43f,1.28f,.23f);feet(.32f,1.03f,-.24f);out.hip_yaw=d*-.16f;break;
    case F::helicopter:
        hands(.47f,1.37f,.15f);feet(.35f,1.02f,-.30f);out.hip_roll=d*.12f;break;
    case F::mc_griddle:
        feet(.33f,1.04f,-.18f);out.hip_offset={-d*.045f,-.035f,0};out.hip_yaw=d*.20f;break;
    case F::bri_flip:
        feet(.26f,1.08f,-.26f);out.hip_offset={0,-.08f,-.02f};out.torso_offset={0,-.08f,0};break;
    case F::grand_slam:
        hands(.43f,1.32f,.12f);feet(.31f,1.12f,-.23f);
        out.hip_roll=d*.20f;out.hip_yaw=d*.18f;out.hip_offset={0,-.10f,0};break;
    case F::willy_flip:
        hands(.43f,1.30f,.13f);feet(.27f,1.12f,-.25f);
        out.hip_offset={0,-.12f,-.04f};out.torso_offset={0,-.09f,-.04f};break;
    }
    // A phase of zero is an exact neutral posture, including during recovery.
    for(auto* v:{&out.hip_offset,&out.torso_offset})for(auto& x:*v)x*=phase;
    out.hip_yaw*=phase;out.hip_roll*=phase;out.head_yaw*=phase;
    return out;
}
} // namespace bmx::detail::source::rider::motion
