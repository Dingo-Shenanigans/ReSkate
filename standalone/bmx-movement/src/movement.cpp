#include <bmx/movement.hpp>
#include <bmx/detail/source/bmx_rider_motion.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace bmx {
namespace source = detail::source;
namespace math = source::tire::detail;
namespace control = source::chassis_control;
namespace {
constexpr double tau=2*std::numbers::pi;
bool simulation_valid(const SimulationSettings& settings) noexcept {
    if(!std::isfinite(settings.seconds)||settings.seconds<minimum_fixed_step||settings.seconds>maximum_fixed_step)return false;
    if(!std::isfinite(settings.hop_delay_seconds)||settings.hop_delay_seconds<0||settings.hop_delay_seconds>.5||
       settings.hop_impulse_steps<1||settings.hop_impulse_steps>16||!std::isfinite(settings.hop_impulse_multiplier)||
       settings.hop_impulse_multiplier<=0||settings.hop_impulse_multiplier>4)return false;
    auto body=control::proposed_body();body.mass=settings.mass;body.inertia_body=settings.inertia_body;body.inertia_orientation=settings.inertia_orientation;
    return source::tire::valid(body)&&source::tire::valid(settings.wheels[0])&&source::tire::valid(settings.wheels[1]);
}
bool rider_valid(const RiderSettings& r) noexcept {
    for(double value:{r.upper_arm,r.forearm,r.thigh,r.shin})if(!std::isfinite(value)||value<.05||value>1.5)return false;
    for(double value:{r.shoulder_half_width,r.hip_joint_half_width})if(!std::isfinite(value)||value<.01||value>.5)return false;
    for(auto value:{r.hips,r.chest,r.head_from_chest,r.shoulder_center_from_chest,r.hip_joint_center_from_hips})if(!math::finite(value,5))return false;
    for(auto axis:{r.shoulder_axis,r.hip_joint_axis})if(!math::finite(axis)||std::abs(math::dot(axis,axis)-1)>1e-5)return false;
    for(auto poles:{r.elbow_poles,r.knee_poles})for(auto pole:poles)if(!math::finite(pole,5)||math::dot(pole,pole)<1e-8)return false;
    return std::isfinite(r.motion_scale)&&r.motion_scale>=0&&r.motion_scale<=2&&
        std::isfinite(r.preload_crouch)&&r.preload_crouch>=0&&r.preload_crouch<=1&&
        std::isfinite(r.chest_preload_fraction)&&r.chest_preload_fraction>=0&&r.chest_preload_fraction<=1;
}
Vec3 wide(std::array<float,3> v) noexcept {return {v[0],v[1],v[2]};}
Vec3 root(const Body& body) noexcept {
    return math::sub(body.center_of_mass,rotate(body.orientation,wide(geometry::com_from_root)));
}
Vec3 world(const Output& frame,Vec3 local) noexcept {
    return math::add(frame.bike_root,rotate(frame.body.orientation,local));
}
Vec3 joint(Vec3 start,Vec3 end,double upper,double lower,Vec3 pole) noexcept {
    Vec3 direction;const auto delta=math::sub(end,start);
    const double d=std::sqrt(math::dot(delta,delta));
    if(!math::unit(delta,direction))return start;
    auto bend=math::sub(pole,math::mul(direction,math::dot(pole,direction)));
    if(!math::unit(bend,bend)) {
        bend=math::cross(direction,{1,0,0});
        if(!math::unit(bend,bend))bend={0,1,0};
    }
    const double reach=std::clamp(d,1e-6,upper+lower-1e-6);
    const double along=std::clamp((upper*upper-lower*lower+reach*reach)/(2*reach),-upper,upper);
    const double height=std::sqrt(std::max(0.0,upper*upper-along*along));
    return math::add(start,math::add(math::mul(direction,along),math::mul(bend,height)));
}
Vec3 reachable(Vec3 start,Vec3 wanted,double upper,double lower,bool& clamped) noexcept {
    const auto delta=math::sub(wanted,start);const double d=std::sqrt(math::dot(delta,delta));
    const double low=std::abs(upper-lower)+1e-6,high=upper+lower-1e-6;
    clamped=d<low||d>high;
    if(!clamped)return wanted;
    Vec3 direction;if(!math::unit(delta,direction))direction={0,-1,0};
    return math::add(start,math::mul(direction,std::clamp(d,low,high)));
}
RiderPose pose(const Output& frame,const RiderSettings& rider,const RiderFrame* measured=nullptr) noexcept {
    const auto proximal_world=[&](Vec3 local){
        if(!measured)return world(frame,local);
        const auto q=measured->neutral_orientation_from_bike;
        const Quat inverse{-q[0],-q[1],-q[2],q[3]};
        return math::add(measured->center_of_mass,rotate(measured->orientation,
            rotate(inverse,math::sub(local,measured->neutral_center_from_bike_root))));
    };
    const auto bend_world=[&](Vec3 direction){
        if(!measured)return rotate(frame.body.orientation,direction);
        const auto q=measured->neutral_orientation_from_bike;
        return rotate(measured->orientation,rotate({-q[0],-q[1],-q[2],q[3]},direction));
    };
    source::visual::Frame presentation;
    presentation.movement_id=frame.tricks.movement_id;
    presentation.movement_phase=static_cast<float>(frame.tricks.movement_phase);
    presentation.movement_progress=static_cast<float>(frame.tricks.movement_progress);
    const auto motion=source::rider::motion::sample(presentation);
    const auto hip_offset=math::mul(wide(motion.hip_offset),rider.motion_scale);
    const auto torso_offset=math::mul(wide(motion.torso_offset),rider.motion_scale);
    const double crouch=frame.actions.preload*rider.preload_crouch;
    const auto hips=math::add(math::add(rider.hips,{0,-crouch,0}),hip_offset);
    const auto chest=math::add(math::add(rider.chest,{0,-crouch*rider.chest_preload_fraction,0}),torso_offset);
    RiderPose result;result.hips=proximal_world(hips);result.chest=proximal_world(chest);
    result.head=proximal_world(math::add(chest,rider.head_from_chest));
    for(unsigned i=0;i<2;++i) {
        const double side=i==0?1.0:-1.0;
        result.shoulders[i]=proximal_world(math::add(math::add(chest,rider.shoulder_center_from_chest),math::mul(rider.shoulder_axis,side*rider.shoulder_half_width)));
        result.hips_joint[i]=proximal_world(math::add(math::add(hips,rider.hip_joint_center_from_hips),math::mul(rider.hip_joint_axis,side*rider.hip_joint_half_width)));
        result.hands[i]=grip_point(frame,i==0);
        result.feet[i]=pedal_point(frame,i==0);
        result.hand_release[i]=std::max(frame.tricks.bar_release,i==0?frame.tricks.release_left_hand:frame.tricks.release_right_hand);
        result.foot_release[i]=std::max(frame.tricks.tail_release,i==0?frame.tricks.release_left_foot:frame.tricks.release_right_foot);
        // Preserve actual authored contact anchors at rest. Source motion supplies
        // only offsets for the limb which the trick's release channel frees.
        const Vec3 neutral_hand{side*.36,1.25,.30};
        const Vec3 neutral_foot{side*.26,.99,-.23};
        const auto hand_offset=math::mul(math::sub(wide(motion.palms[i]),neutral_hand),rider.motion_scale*result.hand_release[i]);
        const auto foot_offset=math::mul(math::sub(wide(motion.ankles[i]),neutral_foot),rider.motion_scale*result.foot_release[i]);
        result.hands[i]=math::add(result.hands[i],rotate(frame.body.orientation,hand_offset));
        result.feet[i]=math::add(result.feet[i],rotate(frame.body.orientation,foot_offset));
        result.hands[i]=reachable(result.shoulders[i],result.hands[i],rider.upper_arm,rider.forearm,result.hand_target_clamped[i]);
        result.feet[i]=reachable(result.hips_joint[i],result.feet[i],rider.thigh,rider.shin,result.foot_target_clamped[i]);
        result.elbows[i]=joint(result.shoulders[i],result.hands[i],rider.upper_arm,rider.forearm,bend_world(rider.elbow_poles[i]));
        result.knees[i]=joint(result.hips_joint[i],result.feet[i],rider.thigh,rider.shin,bend_world(rider.knee_poles[i]));
    }
    return result;
}
void integrate(Body& body,const Output& output,const Tuning& tuning,double seconds) noexcept {
    body.linear_velocity=math::add(body.linear_velocity,math::add(math::mul(tuning.gravity,seconds),
        math::mul(math::add(math::mul(output.force,seconds),output.hop_impulse),1/body.mass)));
    // Euler's rigid-body equation in principal axes, including gyroscopic term.
    const auto q=body.orientation;
    const auto principal=body.inertia_orientation==Quat{0,0,0,1}?q:math::inertia_rotation(body);
    const Quat inverse{-principal[0],-principal[1],-principal[2],principal[3]};
    auto omega=rotate(inverse,body.angular_velocity);
    const auto local_torque=rotate(inverse,output.torque);
    const Vec3 momentum{omega[0]*body.inertia_body[0],omega[1]*body.inertia_body[1],omega[2]*body.inertia_body[2]};
    const auto gyro=math::cross(omega,momentum);
    for(unsigned i=0;i<3;++i)omega[i]+=(local_torque[i]-gyro[i])*seconds/body.inertia_body[i];
    body.angular_velocity=rotate(principal,omega);
    body.center_of_mass=math::add(body.center_of_mass,math::mul(body.linear_velocity,seconds));
    const auto w=body.angular_velocity;
    const Quat dq{w[0]*q[3]+w[1]*q[2]-w[2]*q[1],
        -w[0]*q[2]+w[1]*q[3]+w[2]*q[0],w[0]*q[1]-w[1]*q[0]+w[2]*q[3],
        -w[0]*q[0]-w[1]*q[1]-w[2]*q[2]};
    double n=0;for(unsigned i=0;i<4;++i){body.orientation[i]+=dq[i]*seconds*.5;n+=body.orientation[i]*body.orientation[i];}
    n=std::sqrt(n);for(auto& value:body.orientation)value/=n;
}
}

ObservedProfile streets_observed_v1() noexcept {
    ObservedProfile profile;
    profile.simulation.seconds=.01;
    profile.simulation.hop_delay_seconds=.07;
    profile.simulation.hop_impulse_steps=4;
    // Empirical landing policy for this higher single-body hop. The default
    //7m/s gate rejects its ordinary upright7.6m/s first contact. This9.5m/s
    // threshold is independent scoring tolerance, not a recovered game value.
    profile.simulation.trick_settings.maximum_closing_speed=9.5;
    profile.tuning.gravity={0,-12.5,0};
    profile.tuning.steer_limit=18*std::numbers::pi/180;
    // Empirical single-body reduction, not the game's original brake equation.
    profile.tuning.brake_min_scale=.69;profile.tuning.brake_max_scale=1.9;profile.tuning.brake_speed_gain=.64;
    // Independent reference-host collision policy. At100Hz an ordinary7.6m/s
    // touchdown travels7.6cm per step; this query horizon and impact budget
    // prevent penetration without relaxing the5mm actual-support predicate.
    // This is not Unity's native contact offset or a recovered force limit.
    profile.tuning.contact_gap=.09;profile.tuning.force_limit=85000;
    // First clean hop's observed COM rise is 2.3476m. This is an independent
    // single-body fit; native nominal GetVel=3.2*2 is applied through joints
    // and additional forces, so it is not interchangeable with this parameter.
    profile.tuning.hop_delta_speed=7.9;
    for(auto& wheel:profile.simulation.wheels)wheel.radius=.26;
    // Neutral pre-input V2 landmarks: chassis-COM-local, X reflected once,
    // then shifted by source chassis COM(0,.3,0) into bike-root coordinates.
    // The independent combined mass/inertia are intentionally retained.
    auto& rider=profile.rider;
    rider.upper_arm=.277304;rider.forearm=.227670;rider.thigh=.417477;rider.shin=.405345;
    rider.hips={-.02118348157975856,.7810809026571783,-.2065955239621554};
    rider.chest={-.02261933067794657,1.0008245044188429,-.17803309595331496};
    rider.head_from_chest={-.028051737317272254,.3281391835065506,.14750017553063724};
    rider.shoulder_half_width=.18671621837747482;
    rider.shoulder_axis={.9863733156917094,-.0069295216867172734,.16437659146160863};
    rider.shoulder_center_from_chest={-.015857421775775465,.1959490655933478,.08554791076005878};
    rider.hip_joint_half_width=.097297;
    rider.hip_joint_axis={.9907801325905958,-.01461230682253242,.13468930675107693};
    rider.hip_joint_center_from_hips={.000014536908837733259,-.05328957495188935,-.005885254346929131};
    rider.elbow_poles={Vec3{.5068004938320224,-.8561983123782743,.10038779473727523},
        Vec3{-.27937650602921593,-.866989683972673,.41264713226178157}};
    rider.knee_poles={Vec3{.26244193939108645,-.6703556094797162,.6940803882027813},
        Vec3{-.06076796359051702,-.40324693830231234,.9130712794469404}};
    return profile;
}

Vec3 rotate(Quat q,Vec3 v) noexcept {return math::turn(q,v);}
RiderPose rider_pose(const Output& frame,const RiderSettings& settings,const RiderFrame* measured) noexcept {return pose(frame,settings,measured);}
Vec3 bike_point(const Output& frame,Vec3 point,bool tail_frame) noexcept {
    if(tail_frame) {
        const auto local=geometry::rotate_steering({static_cast<float>(point[0]),static_cast<float>(point[1]),static_cast<float>(point[2])},
            static_cast<float>(frame.tricks.tail_angle+frame.tricks.movement_tail_angle));
        point=wide(local);
    }
    const auto pivot=frame.bike_com_from_root;point=math::sub(point,pivot);
    const double p=frame.physical_trick_rotation?0:frame.tricks.movement_bike_pitch;
    const double r=frame.physical_trick_rotation?0:frame.tricks.movement_bike_roll;
    point={point[0],point[1]*std::cos(p)-point[2]*std::sin(p),point[1]*std::sin(p)+point[2]*std::cos(p)};
    point={point[0]*std::cos(r)-point[1]*std::sin(r),point[0]*std::sin(r)+point[1]*std::cos(r),point[2]};
    return world(frame,math::add(point,pivot));
}
Vec3 wheel_hub(const Output& frame,bool front) noexcept {
    return world(frame,wide(geometry::wheel_hub(front,front?static_cast<float>(frame.steering_angle):0)));
}
Vec3 grip_point(const Output& frame,bool left) noexcept {
    auto local=geometry::rotate_steering(geometry::grip(left).center,static_cast<float>(frame.steering_angle+frame.tricks.bar_angle+frame.tricks.movement_bar_angle));
    return bike_point(frame,wide(local),false);
}
Vec3 pedal_point(const Output& frame,bool left) noexcept {
    return bike_point(frame,wide(geometry::pedal_point(left,static_cast<float>(frame.crank_phase))),true);
}

Movement::Movement(Tuning tuning,SimulationSettings simulation,RiderSettings rider) noexcept :tuning_(tuning),simulation_(simulation),rider_(rider),controller_(tuning),mapper_(simulation.input_settings),tricks_(simulation.trick_settings),wheels_(simulation.wheels) {}
void Movement::cancel_hop() noexcept {
    hop_pending_=false;hop_delay_left_=hop_strength_=0;hop_steps_left_=0;hop_remaining_={};
}
void Movement::reject_sample() noexcept {
    // A rejected observation breaks consecutive contact/steering history.
    // Keep landed tally and wheel phases, but require fresh control neutral.
    controller_.reset();serial_=0;cancel_hop();mapper_.reset();tricks_.cancel();
}
void Movement::invalidate_sample() noexcept {reject_sample();}
void Movement::reset() noexcept {
    controller_.reset();mapper_.reset();tricks_.reset();wheels_=simulation_.wheels;
    serial_=hop_event_=0;front_phase_=rear_phase_=crank_phase_=0;
    body_identity_=0;device_known_=false;cancel_hop();
}
Output Movement::step(const RawInput& raw,const Body& body,const std::array<ContactSample,2>& contacts,std::uint64_t body_identity,ExternalWrench external,TrickRotationObservation rider_rotation) noexcept {
    Output out;out.body=body;out.bike_root=root(body);
    const double seconds=simulation_.seconds;
    if(!simulation_valid(simulation_)) {
        reject_sample();
        if(!std::isfinite(seconds)||seconds<minimum_fixed_step||seconds>maximum_fixed_step)out.issue=control::Issue::unsupported_step;
        return out;
    }
    if(!body_identity||!source::tire::valid(body)||!control::detail::tuning_valid(tuning_)||!rider_valid(rider_)) {
        reject_sample();return out;
    }
    const auto judged_rotation=rider_rotation.available?rider_rotation.orientation:body.orientation;
    const auto judged_velocity=rider_rotation.travel_available?rider_rotation.travel_velocity:body.linear_velocity;
    if(!math::finite(judged_velocity)){reject_sample();return out;}
    double judged_norm=0;for(double value:judged_rotation){if(!std::isfinite(value)){reject_sample();return out;}judged_norm+=value*value;}
    if(std::abs(judged_norm-1)>.001){reject_sample();return out;}
    if(body_identity_&&body_identity_!=body_identity)reset();
    body_identity_=body_identity;
    if(device_known_&&input_device_!=raw.device)cancel_hop();
    input_device_=raw.device;device_known_=true;
    unsigned support_mask=0;
    for(unsigned i=0;i<contacts.size();++i)if(control::eligible_support(body,contacts[i]))support_mask|=1u<<i;
    const bool supported=support_mask!=0;
    const auto actions=mapper_.update(raw,{supported},seconds);
    if(!actions.active)cancel_hop();
    const bool negative_hop=simulation_.observed_nollie_support_policy&&actions.hop_kind==HopKind::nollie;
    const bool new_hop=actions.hop_pressed&&!hop_pending_&&!hop_steps_left_&&!(negative_hop&&support_mask==2);
    if(new_hop) {
        hop_pending_=true;hop_strength_=actions.hop_strength*(negative_hop&&support_mask==3?.95:1);hop_delay_left_=simulation_.hop_delay_seconds;
    } else if(hop_pending_)hop_delay_left_=std::max(0.,hop_delay_left_-seconds);
    // Expiry is consumed here, including when support was lost during the wait.
    // A rejected request is never saved for a future landing.
    const bool hop_ready=hop_pending_&&hop_delay_left_<=1e-9;
    control::Controls command;command.enabled=actions.active;
    command.pedal=actions.pedal;command.brake=actions.brake;command.steer=actions.steer;
    command.manual=supported?actions.manual:std::max(0.0,-actions.air_pitch);
    command.nose_manual=supported?actions.nose_manual:std::max(0.0,actions.air_pitch);
    command.air_yaw=actions.steer;command.hop_strength=hop_ready&&supported?hop_strength_:0;
    if(simulation_.predict_hop_parts)command.hop_impulse_fraction=simulation_.hop_impulse_multiplier/simulation_.hop_impulse_steps;
    command.recovery_step_verified=serial_>0;
    command.pump_operator_available=actions.active&&raw.available&&raw.focused&&!raw.menu_open;
    command.ground_pump=actions.front_weight;
    command.pump_inhibited=hop_pending_||hop_steps_left_||raw.quick_hop||actions.preload>0;
    const auto& state=tricks_.state();
    command.rotation_reserved=std::hypot(raw.left_x,raw.left_y)>.2||std::hypot(raw.right_x,raw.right_y)>.2||
        raw.left_shoulder||raw.right_shoulder||raw.left_trigger>.05||raw.right_trigger>.05||
        state.pending_units||state.movement_holding||state.movement_recovering||state.landing_pending;
    if(command.hop_strength>0&&hop_event_<std::numeric_limits<std::uint64_t>::max())command.hop_event_id=++hop_event_;
    if(hop_ready){hop_pending_=false;hop_strength_=hop_delay_left_=0;}
    const auto predicted_hop=simulation_.predict_hop_parts&&hop_steps_left_?
        math::mul(hop_remaining_,1.0/hop_steps_left_):Vec3{};
    const auto controller_before=controller_;
    auto result=controller_.step(seconds,body,wheels_,contacts,command,{},
        {math::mul(external.force,seconds),math::mul(external.torque,seconds),predicted_hop});
    if(simulation_.swept_plane_normals&&result.valid&&result.pre_contact_prediction_available){
        std::array<control::SpeculativeNormal,2> speculative{};bool any=false;
        for(unsigned i=0;i<2;++i){const auto& sample=contacts[i];
            const double closing=math::dot(source::tire::point_velocity(result.pre_contact_prediction,sample.contact.point),sample.contact.normal);
            if(sample.contact.status==ContactStatus::clear&&sample.verified_plane&&sample.contact.surface_velocity==Vec3{}&&
                sample.separation>std::max(.03,tuning_.contact_gap)&&sample.separation<=.25&&closing<-sample.separation/seconds){
                speculative[i]={true,true,sample.contact.point,sample.contact.normal,sample.separation,result.pre_contact_prediction};any=true;
            }
        }
        if(any){
            // Restart the disposable contact solve from identical history.
            // Input mapping, hop event ownership and scoring still commit once.
            controller_=controller_before;
            result=controller_.step(seconds,body,wheels_,contacts,command,speculative,
                {math::mul(external.force,seconds),math::mul(external.torque,seconds),predicted_hop});
        }
    }
    out.issue=result.issue;out.valid=result.valid;out.actions=actions;
    if(!result.valid){reject_sample();out.tricks=tricks_.state();return out;}
    out.hop_request_accepted=new_hop;
    ++serial_;wheels_=result.wheels;
    if(result.hop_applied) {
        hop_remaining_=simulation_.predict_hop_parts?math::mul(result.hop_impulse,simulation_.hop_impulse_steps):result.hop_impulse;
        hop_steps_left_=simulation_.hop_impulse_steps;
    }
    const bool hop_part=hop_steps_left_>0;
    if(hop_part) {
        out.hop_impulse=math::mul(hop_remaining_,1.0/hop_steps_left_);
        out.hop_velocity_change=math::mul(out.hop_impulse,1/(body.mass*(simulation_.predict_hop_parts?simulation_.hop_impulse_multiplier:1)));
        hop_remaining_=math::sub(hop_remaining_,out.hop_impulse);--hop_steps_left_;
    }
    out.grounded=supported&&!hop_part;out.force=result.force;out.torque=result.torque;
    out.swept_plane_normal_mask=result.speculative_normal_mask;
    out.swept_plane_normal_impulse=result.speculative_normal_impulse;
    out.steering_angle=result.front_steer_angle;
    front_phase_=std::remainder(front_phase_+wheels_[0].angular_speed*seconds,tau);
    rear_phase_=std::remainder(rear_phase_+wheels_[1].angular_speed*seconds,tau);
    if(actions.pedal>0)crank_phase_=std::remainder(crank_phase_+wheels_[1].angular_speed*seconds/2.6,tau);
    out.front_wheel_phase=front_phase_;out.rear_wheel_phase=rear_phase_;out.crank_phase=crank_phase_;
    source::TrickInput input;input.enabled=actions.active;input.grounded=out.grounded;input.owner=body_identity;input.device=raw.device;
    input.barspin=actions.handlebar;input.tailwhip=actions.tailwhip;input.movement=actions.movement;
    const double neutral=mapper_.settings().deadzone+(mapper_.settings().maximum_magnitude-mapper_.settings().deadzone)*mapper_.settings().neutral_threshold;
    input.bar_neutral=std::abs(raw.left_x)<=neutral&&std::abs(raw.left_y)<=neutral;
    input.rear_neutral=std::hypot(raw.right_x,raw.right_y)<=neutral;
    const auto forward=rotate(judged_rotation,{0,0,1});const auto up=rotate(body.orientation,{0,1,0});
    input.body_yaw=std::atan2(forward[0],forward[2]);
    input.travel_yaw=std::hypot(judged_velocity[0],judged_velocity[2])>.05?
        std::atan2(judged_velocity[0],judged_velocity[2]):input.body_yaw;
    input.orientation_available=true;input.body_quaternion=judged_rotation;input.landing_quality_available=true;
    input.body_up_alignment=1;input.support_up_alignment=1;
    for(const auto& sample:contacts)if(control::eligible_support(body,sample)) {
        source::tire::Basis basis;
        if(!source::tire::contact_basis(sample.contact.normal,sample.contact.forward,basis))continue;
        input.body_up_alignment=std::min(input.body_up_alignment,math::dot(up,basis.normal));
        input.support_up_alignment=std::min(input.support_up_alignment,basis.normal[1]);
        input.closing_normal_speed=std::max(input.closing_normal_speed,std::max(0.0,-math::dot(
            math::sub(source::tire::point_velocity(body,sample.contact.point),sample.contact.surface_velocity),basis.normal)));
    }
    out.events=tricks_.update(seconds,input);out.tricks=tricks_.state();
    out.crank_phase+=out.tricks.movement_crank_angle;out.rider=pose(out,rider_);
    return out;
}

FlatWorld::FlatWorld(Tuning tuning,SimulationSettings simulation,RiderSettings rider) noexcept :movement_(tuning,simulation,rider) {reset();}
void FlatWorld::reset(Vec3 position,double yaw) noexcept {
    movement_.reset();body_=control::proposed_body();
    body_.mass=movement_.simulation_settings().mass;body_.inertia_body=movement_.simulation_settings().inertia_body;
    body_.inertia_orientation=movement_.simulation_settings().inertia_orientation;
    body_.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
    body_.center_of_mass=math::add(position,rotate(body_.orientation,wide(geometry::com_from_root)));
    output_={};output_.valid=simulation_valid(movement_.simulation_settings())&&rider_valid(movement_.rider_settings())&&source::tire::valid(body_);output_.body=body_;output_.bike_root=position;output_.grounded=true;output_.rider=pose(output_,movement_.rider_settings());
}
Output FlatWorld::step(const RawInput& raw) noexcept {
    auto measured=output_;measured.body=body_;measured.bike_root=root(body_);
    std::array<ContactSample,2> contacts;
    for(unsigned i=0;i<2;++i) {
        const auto hub=wheel_hub(measured,i==0);
        auto& sample=contacts[i];sample.separation=hub[1]-movement_.simulation_settings().wheels[i].radius;
        sample.contact.status=sample.separation<=movement_.tuning().contact_gap?ContactStatus::hit:ContactStatus::clear;
        sample.contact.point={hub[0],0,hub[2]};sample.contact.normal={0,1,0};
        // Controller supplies its caster-axis steering rotation exactly once.
        // The contact adapter reports the unsteered body-forward reference.
        sample.contact.forward=rotate(body_.orientation,{0,0,1});
        // A near-vertical wheel still has a well-defined rolling projection.
        if(std::hypot(sample.contact.forward[0],sample.contact.forward[2])<1e-8)sample.contact.forward={0,0,1};
        sample.balance_normal={0,1,0};sample.balance_normal_available=true;sample.verified_plane=true;
    }
    auto frame=movement_.step(raw,body_,contacts);
    if(frame.valid) {
        auto candidate=body_;integrate(candidate,frame,movement_.tuning(),movement_.fixed_seconds());
        if(source::tire::valid(candidate))body_=candidate;
        else frame.valid=false;
    }
    frame.body=body_;frame.bike_root=root(body_);frame.rider=pose(frame,movement_.rider_settings());output_=frame;return output_;
}
const char* issue_name(control::Issue issue) noexcept {
    switch(issue) {
    case control::Issue::none:return "none";case control::Issue::unsupported_step:return "unsupported fixed step";
    case control::Issue::invalid_sample:return "invalid sample";case control::Issue::unavailable_contact:return "wheel query unavailable";
    case control::Issue::penetration_limit:return "wheel penetration limit";case control::Issue::output_limit:return "force or torque limit";
    }return "unknown";
}
const char* trick_failure_name(TrickFailure failure) noexcept {return source::trick_failure_name(failure).data();}
}
