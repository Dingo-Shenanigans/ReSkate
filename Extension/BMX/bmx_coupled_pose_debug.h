#pragma once
#include "bmx_rider_pose.h"
#include "bmx_native_motion.h"
#include "Engine/Core/Json/json.h"
#include <atomic>
#include <bit>
#include <memory>

namespace dingosdk::bmx::rider::coupled_debug {
enum class Stage:unsigned {not_called,layout,publication,owner,pose_layout,published_buffer,bones,lease_before,
    solve,solved,publication_recheck,owner_recheck,pose_recheck,lease_recheck,ancestors,write,completed,count};
inline constexpr std::array<const char*,static_cast<unsigned>(Stage::count)> names{
    "not_called","layout","publication","owner","pose_layout","published_buffer","bones","lease_before",
    "solve","solved","publication_recheck","owner_recheck","pose_recheck","lease_recheck","ancestors","write","completed"};
struct Fixture {
    Stage stage{Stage::not_called};bool solved{},observed_selected{},clock_available{};
    std::uint64_t actual_serial{};double actual_time{};
    std::vector<pose::Bone> original;pose::Rig rig;visual::Frame frame;pose::EntryContacts entry;pose::SpringBinding binding;
    NativeMotionLeaseStatus lease;pose::Result result;
};
struct Store {
    std::atomic<bool> active{},claimed{},capture_failed{};std::atomic<unsigned> last{};
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(Stage::count)> exits{};
    std::atomic<std::uint64_t> callbacks{},matching_dispatches{},actual_serial{},published_serial{};
    std::atomic<bool> clock_available{};std::atomic<double> actual_time{},published_time{};
    std::atomic<std::shared_ptr<const Fixture>> fixture;
};
// Observation-only scope: recording a rejected callback neither retries nor
// grants a write. Only the first endpoint makes one additional bounded copy.
struct Trace {
    Store& store;bool active;Stage stage{Stage::not_called};std::shared_ptr<Fixture> captured;
    explicit Trace(Store& value,bool enabled):store(value),active(enabled){}
    void at(Stage next) noexcept {stage=next;}
    void capture(const std::vector<pose::Bone>& bones,const pose::Rig& rig,const visual::Frame& frame,
                 const pose::EntryContacts& entry,const pose::SpringBinding& binding,bool observed) noexcept {
        if(!active||!frame.spring_avatar.requested||!frame.spring_avatar.capture.requested||
           frame.mount_progress!=1||frame.rider_blend!=1||bones.size()!=395||rig.parents.size()!=386||rig.bind.size()!=386)return;
        bool expected=false;if(!store.claimed.compare_exchange_strong(expected,true))return;
        try {captured=std::make_shared<Fixture>();captured->original=bones;captured->rig=rig;captured->frame=frame;
            captured->entry=entry;captured->binding=binding;captured->observed_selected=observed;
        }catch(...){captured.reset();store.capture_failed.store(true);}
    }
    void clock(bool available,std::uint64_t serial,double time,const NativeMotionLeaseStatus& lease,const avatar_binding::Epoch& published) noexcept {
        if(!active)return;store.clock_available.store(available);store.actual_serial.store(serial);store.actual_time.store(time);
        store.published_serial.store(published.serial);store.published_time.store(published.time);
        if(captured){captured->clock_available=available;captured->actual_serial=serial;captured->actual_time=time;captured->lease=lease;}
    }
    void solved(const pose::Result& result) noexcept {if(captured){captured->solved=true;captured->result=result;}}
    ~Trace(){if(!active)return;store.last.store(static_cast<unsigned>(stage));store.exits[static_cast<unsigned>(stage)].fetch_add(1);
        if(captured){captured->stage=stage;store.fixture.store(std::move(captured));}}
};
template<std::size_t N> inline std::array<std::uint32_t,N> float_bits(const std::array<float,N>& value) noexcept {
    std::array<std::uint32_t,N> result{};for(std::size_t i=0;i<N;++i)result[i]=std::bit_cast<std::uint32_t>(value[i]);return result;
}
inline Json epoch_json(const avatar_binding::Epoch& e){auto keys=Json::array();for(const auto& k:e.bodies)keys.push_back(Json{{"world",k.world},{"index",k.index},{"generation",k.generation}});
    return Json{{"owner_world",e.owner_world},{"bodies",std::move(keys)},{"token",e.token},{"serial",e.serial},{"time",e.time},{"h",e.seconds}};}
inline Json observation_json(const avatar_binding::Observation& o){return Json{{"available",o.available},{"epoch",epoch_json(o.epoch)},
    {"spring_com",o.spring_com},{"spring_velocity",o.spring_velocity},{"spring_angular_velocity",o.spring_angular_velocity},
    {"spring_orientation",o.spring_orientation},{"bike_root",o.bike_root},{"bike_orientation",o.bike_orientation}};}
inline Json frame_json(const visual::Frame& f){const auto& a=f.spring_avatar;const auto& c=a.capture;const auto& s=c.seed;
    const Json seed{{"ready",s.ready},{"prepared",s.prepared},{"issue",static_cast<unsigned>(s.issue)},{"root",s.root},{"orientation",s.orientation},{"up",s.up},{"com",s.com},{"pitch",s.pitch},{"roll",s.roll},
        {"request_yaw",s.request.yaw},{"cross_plane_residual",s.cross_plane_residual},{"cross_plane_allowance",s.cross_plane_allowance},{"world",s.request.world},{"context",s.request.context},{"serial",s.request.serial},{"time",s.request.simulation_time},{"position",s.request.position}};
    const Json native{{"available",c.native.available},{"coherent_epoch",c.native.coherent_epoch},{"epoch",epoch_json(c.native.epoch)},{"position",c.native.position},{"transform",c.native.transform},{"transform_bits",float_bits(c.native.transform)}};
    const Json capture{{"requested",c.requested},{"neutral",c.neutral},{"root_height_verified",c.root_height_verified},{"mounted_geometry_verified",c.mounted_geometry_verified},
        {"root_height",c.root_height},{"mount_progress",c.mount_progress},{"rider_blend",c.rider_blend},{"native",native},{"seed",seed}};
    const Json avatar{{"requested",a.requested},{"actual",observation_json(a.actual)},{"native_motion_lease_revision",a.native_motion_lease_revision},{"capture",capture}};
    return Json{{"active",f.active},{"position",f.position},{"yaw",f.yaw},{"pitch",f.pitch},{"roll",f.roll},{"steer",f.steer},
        {"front_wheel_phase",f.front_wheel_phase},{"rear_wheel_phase",f.rear_wheel_phase},{"crank_phase",f.crank_phase},{"barspin",f.barspin},{"tailwhip",f.tailwhip},
        {"entity",f.entity},{"preload",f.preload},{"front_weight",f.front_weight},{"rear_weight",f.rear_weight},{"airborne",f.airborne},
        {"barspin_release",f.barspin_release},{"tailwhip_release",f.tailwhip_release},{"chassis_quaternion",f.chassis_quaternion},{"chassis_quaternion_available",f.chassis_quaternion_available},
        {"movement_id",static_cast<unsigned>(f.movement_id)},{"movement_phase",f.movement_phase},{"movement_progress",f.movement_progress},
        {"release_left_hand",f.release_left_hand},{"release_right_hand",f.release_right_hand},{"release_left_foot",f.release_left_foot},{"release_right_foot",f.release_right_foot},
        {"landing_compression",f.landing_compression},{"pedal_effort",f.pedal_effort},{"air_extension_blend",f.air_extension_blend},{"air_extension_available",f.air_extension_available},
        {"rider_uphill_blend",f.rider_uphill_blend},{"rider_gaze_counterpitch",f.rider_gaze_counterpitch},{"rider_posture_available",f.rider_posture_available},
        {"movement_bike_pitch",f.movement_bike_pitch},{"movement_bike_roll",f.movement_bike_roll},{"rider_mode",static_cast<unsigned>(f.rider_mode)},
        {"rider_blend",f.rider_blend},{"mount_token",f.mount_token},{"mount_progress",f.mount_progress},{"mount_from_left",f.mount_from_left},
        {"movement_timing_available",f.movement_timing_available},{"movement_holding",f.movement_holding},{"movement_recovering",f.movement_recovering},
        {"movement_caught",f.movement_caught},{"movement_recovery_progress",f.movement_recovery_progress},{"native_body_token",f.native_body_token},
        {"spring_avatar",avatar}};}
inline Json fixture_json(const Fixture& f){auto bones=Json::array(),left=Json::array(),right=Json::array();
    for(const auto& b:f.original){std::array<std::uint32_t,4> scale{},rotation{},position{};
        for(unsigned i=0;i<4;++i){scale[i]=std::bit_cast<std::uint32_t>(b.scale[i]);rotation[i]=std::bit_cast<std::uint32_t>(b.rotation[i]);position[i]=std::bit_cast<std::uint32_t>(b.position[i]);}
        bones.push_back(Json{{"scale",b.scale},{"rotation",b.rotation},{"position",b.position},{"scale_bits",scale},{"rotation_bits",rotation},{"position_bits",position}});}
    for(const auto& finger:f.rig.left_fingers)left.push_back(Json{{"metacarpal",finger.metacarpal},{"joints",finger.joints}});
    for(const auto& finger:f.rig.right_fingers)right.push_back(Json{{"metacarpal",finger.metacarpal},{"joints",finger.joints}});
    const auto& r=f.rig;const auto& l=f.lease;const auto& e=f.entry;const auto& b=f.binding;const auto& q=f.result;
    return Json{{"stage",names[static_cast<unsigned>(f.stage)]},{"solved",f.solved},{"observed_selected",f.observed_selected},
        {"clock_available",f.clock_available},{"actual_serial",f.actual_serial},{"actual_time",f.actual_time},{"frame",frame_json(f.frame)},{"original_bones",std::move(bones)},
        {"rig",Json{{"parents",r.parents},{"bind",r.bind},{"hips",r.hips},{"head",r.head},{"spine",r.spine},{"shoulders",r.shoulders},{"neck",r.neck},
            {"left_arm",r.left_arm},{"right_arm",r.right_arm},{"left_leg",r.left_leg},{"right_leg",r.right_leg},{"toe_base",r.toe_base},{"left_fingers",std::move(left)},{"right_fingers",std::move(right)}}},
        {"entry",Json{{"entity",e.entity},{"token",e.token},{"palms",e.palms},{"ankles",e.ankles},{"arm_poles",e.arm_poles},{"leg_poles",e.leg_poles},{"hands",e.hands},{"feet",e.feet}}},
        {"binding",Json{{"root_ready",b.root.ready},{"root_phase",static_cast<unsigned>(b.root.phase)},{"bind_epoch",epoch_json(b.root.bind_epoch)},
            {"local_root_offset",b.root.local_root_offset},{"local_root_basis",b.root.local_root_basis},{"pose_epoch",epoch_json(b.pose_epoch)},
            {"local_hip",b.local_hip},{"local_chest",b.local_chest},{"local_hip_basis",b.local_hip_basis},{"proximal_ready",b.proximal_ready},
            {"completed_pose_write",b.completed_pose_write},{"last_pose_applied",b.last_pose_applied},{"fit_adjustment",b.fit_adjustment},{"hand_error",b.hand_error},{"foot_error",b.foot_error}}},
        {"lease",Json{{"available",l.available},{"active",l.active},{"details_available",l.details_available},{"epoch_valid",l.epoch_valid},{"timing_available",l.timing_available},
            {"owner_available",l.owner_available},{"offboard",l.offboard},{"motion_supported",l.motion_supported},{"full_rotation",l.full_rotation},
            {"lease_revision",l.lease_revision},{"epoch_revision",l.epoch_revision},{"transform",l.transform},{"transform_bits",float_bits(l.transform)},{"position",l.position},{"yaw",l.yaw},{"standable",l.standable},{"client",l.client},{"entity",l.entity},{"core",l.core},{"context",l.context},{"rig",l.rig},
            {"serial",l.serial},{"time",l.simulation_time},{"h",l.seconds},{"target",l.target},{"velocity",l.velocity},{"rotation",l.rotation}}},
        {"result",Json{{"ready",q.ready},{"hand_error",q.hand_error},{"foot_error",q.foot_error},{"hand_contact",q.hand_contact},{"foot_contact",q.foot_contact},
            {"posture_attempts",q.posture_attempts},{"posture_scale",q.posture_scale},{"posture_fit_adjustment",q.posture_fit_adjustment},{"posture_gaze",q.posture_gaze},
            {"posture_requested",q.posture_requested},{"posture_fallback",q.posture_fallback},{"observed_requested",q.observed_requested},{"observed_applied",q.observed_applied},
            {"observed_strength",q.observed_strength},{"spring_requested",q.spring_requested},{"spring_calibrated",q.spring_calibrated},{"spring_applied",q.spring_applied},{"spring_fit_adjustment",q.spring_fit_adjustment}}}};}
}
