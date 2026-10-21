#include <bmx/suspension.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>

namespace {
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
double length(bmx::Vec3 v){return std::hypot(v[0],v[1],v[2]);}
bmx::RawInput neutral(){bmx::RawInput r;r.available=true;r.device=1;return r;}
}
int main(){
    using namespace bmx;
    {
        SuspensionSettings stop;stop.unilateral_soft_limits=true;
        stop.linear_limits[0]={true,-.3,.3,{10000,250,50000}};
        Body a,b;a.mass=b.mass=1;a.inertia_body=b.inertia_body={1,1,1};
        for(double sign:{-1.,1.}){
            b.center_of_mass={sign*.299,0,0};b.linear_velocity={sign*(.1-1e-7),0,0};
            const auto before=solve_suspension(.01,a,b,stop);
            b.linear_velocity={sign*(.1+1e-7),0,0};const auto after=solve_suspension(.01,a,b,stop);
            check(before.valid&&after.valid,"both unilateral stop entry sides admit finite physical states");
            check(length(before.rider_force)<1e-10&&length(after.rider_force)<1e-4,
                "a newly crossing soft stop has continuous zero-force entry without a full-damper jump");
            check(sign*after.rider_force[0]<=0&&after.predicted_energy<=after.energy_before+1e-7,
                "soft stop entry is nonadhesive and retains the passive energy admission");
            b.center_of_mass={sign*.32,0,0};b.linear_velocity={};
            const auto outside=solve_suspension(.01,a,b,stop);
            check(outside.valid&&sign*outside.rider_force[0]<0&&outside.predicted_energy<=outside.energy_before+1e-7,
                "an already exceeded soft limit retains a passive restoring spring");
            b.linear_velocity={-sign*4,0,0};const auto escaping=solve_suspension(.01,a,b,stop);
            check(escaping.valid&&length(escaping.rider_force)<1e-10,
                "an escaping soft stop never attracts the body back toward the exceeded boundary");
            stop.linear_limits[0].drive.spring=0;b.linear_velocity={};const auto damper=solve_suspension(.01,a,b,stop);
            check(damper.valid&&length(damper.rider_force)<1e-10&&damper.predicted_energy==damper.energy_before,
                "a stationary exceeded damper-only stop cannot invent restoring energy");
            stop.linear_limits[0].drive.spring=10000;
        }
        check(!SuspensionSettings{}.unilateral_soft_limits&&streets_observed_v2().coupled.suspension.unilateral_soft_limits,
            "continuous unilateral stop policy is opt-in for generic hosts and selected by observedv2");
    }
    AttitudePIDSettings pid;pid.enabled=true;pid.air_pitch_control=.2;
    Body tilted;tilted.orientation={std::sin(.05),0,0,std::cos(.05)};
    AttitudePID balance;const auto first_balance=balance.step(.01,tilted,{0,1,0},false,pid);
    check(first_balance.valid&&first_balance.saturated&&std::abs(first_balance.local_torque[0]+2304)<1e-8,
        "native degree-error airPID clamps raw output before multipliers and emits physical restoring Nm");
    const auto steady_balance=balance.step(.01,tilted,{0,1,0},false,pid);
    const double error=-.1*180/std::acos(-1.);
    check(steady_balance.valid&&std::abs(steady_balance.local_torque[0]-error*.1*10*360*2*.2*2)<1e-8,
        "native PID derivative uses actual consecutive error history and air proportional mass factor10");
    balance.reset();const auto grounded_balance=balance.step(.01,tilted,{0,1,0},true,pid);
    check(grounded_balance.valid&&grounded_balance.local_torque[0]==0,"ground-neutral source pitchcontrol0 permits the bike/rider lean policy");
    AttitudePIDSettings manual_pid;manual_pid.enabled=true;manual_pid.ground_pitch_control=1;manual_pid.pitch_setpoint_degrees=-13.4740157480315;
    Body level;balance.reset();const auto manual_pid_first=balance.step(.01,level,{0,1,0},true,manual_pid);
    check(manual_pid_first.valid&&manual_pid_first.saturated&&manual_pid_first.local_torque[0]==-7680,
        "manual PID uses signed source-degree setpoint and clamps before physical torque multipliers");
    const auto manual_pid_steady=balance.step(.01,level,{0,1,0},true,manual_pid);
    check(std::abs(manual_pid_steady.local_torque[0]+13.4740157480315*.1*480*2)<1e-7,
        "manual setpoint proportional torque retains native units after derivative settles");
    manual_pid.pitch_setpoint_degrees=std::numeric_limits<double>::quiet_NaN();
    check(!valid_attitude_pid(manual_pid),"nonfinite manual pitch setpoint rejects configuration");
    {
        auto profile=streets_observed_v2();profile.coupled.actuation.enabled=false;
        profile.coupled.manual_pitch.enabled=false;profile.coupled.rider_stabilization.enabled=false;
        profile.coupled.chassis_stabilization.enabled=false;
        Body chassis,rider;chassis.mass=30;chassis.inertia_body={4,7,8};chassis.center_of_mass={0,3,0};
        rider.mass=80;rider.inertia_body={6,8,6};rider.center_of_mass={0,3.4,0};
        chassis.angular_velocity=rider.angular_velocity={0,1,0};
        std::array<ContactSample,2> clear;for(auto& query:clear){query.contact.status=ContactStatus::clear;query.separation=1;}
        CoupledMovement air_drag(profile);const auto damped=air_drag.step(neutral(),chassis,rider,clear);
        const double observed_drag=damped.rider_torque[1]-damped.suspension.rider_torque[1];
        check(damped.valid&&!damped.bike.grounded&&std::abs(observed_drag+8*2.75/(1+.01*2.75))<1e-8,
            "source airborne angular drag is included exactly once in physical rider wrench");
        check(observed_drag*rider.angular_velocity[1]<0,"air drag removes measured angular energy");
        profile.coupled.use_air_angular_drag=false;CoupledMovement legacy_drag(profile);
        const auto retained=legacy_drag.step(neutral(),chassis,rider,clear);
        check(retained.valid&&std::abs(retained.rider_torque[1]-retained.suspension.rider_torque[1]+8/(1+.01))<1e-8,
            "disabled airborne policy retains the configured role damping rate");
        profile.coupled.use_air_angular_drag=true;profile.coupled.air_angular_drag=std::numeric_limits<double>::quiet_NaN();
        CoupledMovement invalid_drag(profile);check(!invalid_drag.step(neutral(),chassis,rider,clear).valid,
            "nonfinite airborne drag rejects the coupled sample");
    }
    tilted.orientation={0,0,std::sin(.05),std::cos(.05)};balance.reset();const auto roll_balance=balance.step(.01,tilted,{0,1,0},false,pid);
    check(roll_balance.valid&&roll_balance.local_torque[2]<0&&length(roll_balance.torque)<=pid.maximum_torque,"source rollPID acts about bodylocalZ in restoring direction with finite torque budget");
    pid.enabled=false;check(balance.step(.01,tilted,{0,1,0},false,pid).local_torque==Vec3{},"generic host PID remains opt-in");
    pid.derivative=-1;check(!balance.step(.01,tilted,{0,1,0},false,pid).valid,"negative PID damping rejects the actuator configuration");
    {
        auto p=streets_observed_v2().movement;Body measured;measured.mass=30;
        measured.center_of_mass={0,.42,0};measured.inertia_body={4,7,8};
        std::array<ContactSample,2> plane;
        for(unsigned i=0;i<2;++i){auto& query=plane[i];query.contact.status=ContactStatus::clear;
            query.contact.point={0,0,i==0?.4:-.4};query.contact.normal={0,1,0};query.contact.forward={0,0,1};
            query.separation=.12;query.verified_plane=true;}
        Movement swept(p.tuning,p.simulation,p.rider);check(swept.step(neutral(),measured,plane).valid,"plane sweep arms from a valid consecutive observation");
        measured.linear_velocity={2,-30,0};auto frame=swept.step(neutral(),measured,plane);
        check(frame.valid&&frame.swept_plane_normal_mask==3&&frame.force[1]>0&&std::abs(frame.force[0])<1e-8&&std::abs(frame.force[2])<1e-8,
            "verified future plane crossing adds only bounded normal response beyond the ordinary query skin");
        check(!frame.grounded&&!frame.events.landed&&length(frame.hop_impulse)==0&&length(frame.force)<=p.tuning.force_limit+1e-8,
            "positive-gap sweep cannot fake support, score landing or launch a hop and retains the force cap");
        p.simulation.swept_plane_normals=false;Movement old_path(p.tuning,p.simulation,p.rider);
        measured.linear_velocity={};old_path.step(neutral(),measured,plane);measured.linear_velocity={2,-30,0};frame=old_path.step(neutral(),measured,plane);
        check(frame.valid&&frame.swept_plane_normal_mask==0&&length(frame.force)==0,"default-off sweep preserves the old clear-plane force path");
        p.simulation.swept_plane_normals=true;Movement unverified(p.tuning,p.simulation,p.rider);
        for(auto& query:plane)query.verified_plane=false;
        measured.linear_velocity={};unverified.step(neutral(),measured,plane);measured.linear_velocity={2,-30,0};frame=unverified.step(neutral(),measured,plane);
        check(frame.valid&&frame.swept_plane_normal_mask==0&&length(frame.force)==0,"unverified geometry cannot acquire an anticipated normal row");
    }
    {
        auto profile=streets_observed_v2();CoupledWorld manual_world(profile);auto input=neutral();
        for(unsigned i=0;i<200;++i)check(manual_world.step(input).valid,"manual neutral arming remains valid");
        input.pedal=1;for(unsigned i=0;i<100;++i)check(manual_world.step(input).valid,"moving manual approach remains valid");
        input.pedal=0;input.right_y=-.4;double front_lift{},rear_lift{};bool first_setpoint=false,direct_seen=false;
        for(unsigned i=0;i<120;++i){
            const auto out=manual_world.step(input);check(out.valid&&out.converged,"source manual actuator stays bounded and converged");
            front_lift=std::max(front_lift,out.input_contacts[0].separation);rear_lift=std::max(rear_lift,out.input_contacts[1].separation);
            if(out.manual_pitch_setpoint_degrees!=0&&!first_setpoint){
                check(std::abs(out.manual_pitch_setpoint_degrees+13.4740157480315*.04)<1e-8,
                    "manual angle starts with native grounded dt*4 interpolation");first_setpoint=true;
            }
            if(length(out.manual_rider_torque)>0){
                check(std::abs(length(out.manual_rider_torque)-450*56./127)<1e-8&&
                    std::abs(length(out.manual_chassis_torque)-450*.9*56./127)<1e-8,
                    "forward Manual Manny applies recovered distinct Spring and Chassis torques");direct_seen=true;
            }
        }
        check(first_setpoint&&direct_seen&&front_lift>.02&&rear_lift<.005,
            "mode-aware manual physically lifts the front wheel while actual rear support remains available");
        input.focused=false;const auto canceled=manual_world.step(input);
        check(canceled.valid&&canceled.manual_pitch_setpoint_degrees==0&&length(canceled.manual_rider_torque)==0&&length(canceled.manual_chassis_torque)==0,
            "focus loss cancels manual setpoint and direct actuation immediately");
        profile.coupled.manual_pitch.direct_torque=std::numeric_limits<double>::infinity();CoupledWorld bad_manual(profile);
        check(!bad_manual.step(neutral()).valid,"nonfinite source manual torque rejects the complete coupled step");
        profile=streets_observed_v2();CoupledWorld nose_world(profile);input=neutral();
        for(unsigned i=0;i<200;++i)check(nose_world.step(input).valid,"nose manual neutral arming stays valid");
        input.pedal=1;for(unsigned i=0;i<100;++i)check(nose_world.step(input).valid,"nose manual moving approach stays valid");
        input.pedal=0;input.right_y=.4;bool nose_torque=false;
        for(unsigned i=0;i<80;++i){const auto out=nose_world.step(input);check(out.valid,"source Nosey torque remains physically valid");
            if(length(out.manual_rider_torque)>0){nose_torque=true;
                check(out.manual_pitch_setpoint_degrees==0&&std::abs(length(out.manual_rider_torque)-450*1.5*56./127)<1e-8&&
                    std::abs(length(out.manual_chassis_torque)-450*1.38*56./127)<1e-8,
                    "live Manual Nosey uses exact forward torque multipliers and zero angle target");}}
        check(nose_torque,"direct Nosey mode actuates after source holdpoint activation");
    }
    namespace contact_math=detail::source::chassis_control::detail;
    const auto feasible=contact_math::contact_fraction_interval({100,0,0},{-80,0,0},30);
    check(feasible.valid&&std::abs(feasible.lower-.875)<1e-12&&feasible.upper==1,
        "opposing contact can cancel a strong external wrench into the unchanged net force budget");
    check(!contact_math::contact_fraction_interval({100,0,0},{80,0,0},30).valid,
        "aligned contact cannot hide a genuinely infeasible external wrench");
    std::array<JointActuationKey,2> keys{};keys[0].seconds=-.1;keys[1].seconds=.9;
    keys[0].command.target_position={0,.1,0};keys[1].command.target_position={0,.5,.2};
    keys[0].command.linear_spring={100,1100,100};keys[1].command.linear_spring={100,2100,100};
    keys[1].command.target_rotation={0,1,0,0};keys[1].command.rider_inertia={150,150,150};
    JointActuationTrack track;track.id=actuation_quick_hop;track.keys=keys;
    check(valid_joint_actuation(track),"finite actuator commands form a valid source timeline");
    const auto half=sample_joint_actuation(track,.4);
    check(half.available&&std::abs(half.command.target_position[1]-.3)<1e-12,"joint target interpolates in real engine seconds");
    check(std::abs(half.command.linear_spring[1]-1600)<1e-12&&std::abs(half.command.rider_inertia[1]-79)<1e-12,"drive and inertia observations interpolate together");
    check(std::abs(half.command.target_rotation[1]-std::sqrt(.5))<1e-12&&std::abs(half.command.target_rotation[3]-std::sqrt(.5))<1e-12,"joint target rotation follows shortest spherical arc");
    const auto before=sample_joint_actuation(track,-1),after=sample_joint_actuation(track,2);
    check(before.before_domain&&before.command.target_position==keys[0].command.target_position,"pre-domain actuator sampling holds actual first endpoint");
    check(after.after_domain&&after.command.target_position==keys[1].command.target_position,"post-domain actuator sampling holds actual final endpoint");
    keys[1].command.target_rotation={0,0,0,-1};
    check(std::abs(sample_joint_actuation(track,.4).command.target_rotation[3]-1)<1e-12,"antipodal observed quaternions do not invent a full joint turn");
    keys[1].seconds=keys[0].seconds;check(!sample_joint_actuation(track,0).available,"duplicate source times reject a command track");keys[1].seconds=.9;
    keys[1].command.target_rotation={0,0,0,0};check(!sample_joint_actuation(track,0).available,"missing source quaternion cannot become observed identity");keys[1].command.target_rotation={0,0,0,1};
    keys[1].command.linear_damper[0]=-1;check(!sample_joint_actuation(track,0).available,"negative measured drive damping rejects the command");keys[1].command.linear_damper[0]=0;
    check(!sample_joint_actuation(track,std::numeric_limits<double>::quiet_NaN()).available,"nonfinite playback time is rejected");
    std::set<unsigned> catalog_ids;unsigned hops=0;
    for(const auto& observed:streets_observed_joint_actuation_tracks()){
        check(valid_joint_actuation(observed),"all bundled observed actuator tracks satisfy strict numeric contract");
        if(observed.id<80)catalog_ids.insert(observed.id);if(observed.id==actuation_quick_hop)++hops;
        const auto middle=sample_joint_actuation(observed,(observed.keys.front().seconds+observed.keys.back().seconds)*.5);
        check(middle.available,"every observed phase has a finite interpolated actuator command");
    }
    check(catalog_ids.size()==58,"all58 verified native trick fires have actuator coverage");
    check(hops==1,"one first-hop fit track is bundled without fitting the held-out second hop");
    const auto profile=streets_observed_v2();check(profile.coupled.actuation.enabled,"v2 opts into measured actuator policy");
    for(unsigned support:{1u,2u,3u}){
        Movement negative(profile.movement.tuning,profile.movement.simulation,profile.movement.rider);
        Body body;body.mass=30;body.inertia_body={4,7,8};body.center_of_mass={0,.3,0};
        std::array<ContactSample,2> contacts;for(unsigned i=0;i<2;++i){auto& query=contacts[i];
            query.contact.status=support&(1u<<i)?ContactStatus::hit:ContactStatus::clear;
            query.contact.point={0,0,i==0?.4:-.4};query.contact.normal={0,1,0};query.contact.forward={0,0,1};
            query.verified_plane=true;query.separation=support&(1u<<i)?0:.2;}
        negative.step(neutral(),body,contacts);auto raw=neutral();raw.right_y=1;negative.step(raw,body,contacts);
        raw.right_y=-1;auto event=negative.step(raw,body,contacts);
        check(event.valid&&event.actions.hop_kind==HopKind::nollie&&event.hop_request_accepted==(support!=2),
            "source negative-hop eligibility rejects rear-only support without suppressing the input gesture metadata");
        double total=event.hop_impulse[1];for(unsigned i=0;i<30;++i){const auto frame=negative.step(neutral(),body,contacts);
            check(frame.valid,"source Nollie support policy remains finite through the bounded scheduled parts");total+=frame.hop_impulse[1];}
        const double expected=support==2?0:30*4.5*4./3*(support==3?.95:1);
        check(std::abs(total-expected)<1e-8,"two-wheel support applies the observed.95 Nollie factor while front-only retains selected4.5");
    }
    {
        CoupledWorld flick(profile);unsigned events=0,parts=0;double chassis_impulse=0,rider_impulse=0;
        Vec3 last_target{};bool enter_seen=false;
        for(unsigned tick=0;tick<590;++tick){auto input=neutral();
            if(tick>=280&&tick<320)input.right_y=-1;
            else if(tick>=320&&tick<330)input.right_y=1;
            const auto frame=flick.step(input);
            check(frame.valid&&frame.converged,"native-timeline RSflick remains physically valid and converged");
            events+=frame.bike.actions.hop_pressed;parts+=frame.bike.hop_impulse[1]>0;
            chassis_impulse+=frame.bike.hop_impulse[1];rider_impulse+=frame.rider_hop_impulse[1];
            if(tick>=280&&tick<320)check(frame.bike.actions.preload==-1&&!frame.bike.actions.hop_pressed,
                "full RS preload alone remains signed and cannot launch an early hop");
            if(tick>=280&&tick<320){enter_seen=enter_seen||(frame.joint_actuation_available&&frame.joint_actuation_id==actuation_preload);
                last_target=frame.applied_suspension.target_position;}
            if(tick==320)check(frame.bike.actions.hop_pressed&&std::abs(frame.bike.actions.hop_strength-4.5/6.4)<1e-12,
                "full RS reversal requests the distinct observed4.5-strength hop");
            if(tick==320)check(frame.bike.hop_request_accepted&&frame.joint_actuation_id==actuation_preload&&
                length({frame.applied_suspension.target_position[0]-last_target[0],frame.applied_suspension.target_position[1]-last_target[1],frame.applied_suspension.target_position[2]-last_target[2]})<1e-10,
                "accepted full RS reversal joins the measured release commands at the exact preceding target");
        }
        check(events==1&&parts==4&&std::abs(chassis_impulse-180)<1e-8&&std::abs(rider_impulse-360)<1e-8,
            "one RSflick yields exactly four recipient-weighted4.5m/s parts, not duplicateR3 strength");
        check(enter_seen,"full RS preload uses its own nonzero physical actuator track before the hop event");
    }
    for(unsigned held:{10u,40u,80u}){
        CoupledWorld variable(profile);unsigned accepted_hops=0;double total=0;
        for(unsigned tick=0;tick<150+held+250;++tick){auto input=neutral();
            if(tick>=150&&tick<150+held)input.right_y=-1;
            else if(tick>=150+held&&tick<160+held)input.right_y=1;
            const auto frame=variable.step(input);check(frame.valid&&frame.converged,"short, measured and extended full-preload holds remain numerically valid");
            accepted_hops+=frame.bike.hop_request_accepted;total+=frame.bike.hop_impulse[1];
        }
        check(accepted_hops==1&&std::abs(total-180)<1e-8,"retiming a preload hold cannot create extra or larger hop pulses");
    }
    for(bool moving:{false,true}){
        CoupledWorld canceled(profile);unsigned accepted_hops=0;double total=0;
        for(unsigned tick=0;tick<450;++tick){auto input=neutral();if(moving&&tick>=150&&tick<250)input.pedal=1;
            if(tick>=250&&tick<290)input.right_y=-1;
            const auto frame=canceled.step(input);check(frame.valid&&frame.converged,"full-preload neutral cancellation remains numerically valid");
            accepted_hops+=frame.bike.hop_request_accepted;total+=frame.bike.hop_impulse[1];
            if(tick>=250)check(frame.bike.grounded,"neutral cancellation of stationary or moving full preload does not create an artificial takeoff");
        }
        check(accepted_hops==0&&total==0,"full negative stick released to neutral cannot launch a hop");
    }
    {
        CoupledWorld pending(profile);for(unsigned tick=0;tick<200;++tick)pending.step(neutral());
        auto input=neutral();input.right_y=-1;pending.step(input);input.right_y=1;auto frame=pending.step(input);
        check(frame.bike.hop_request_accepted&&frame.joint_actuation_id==actuation_preload,"full-preload Ollie reserves the physical release track");
        input.right_y=.1;pending.step(input);input.right_y=-1;frame=pending.step(input);
        check(frame.bike.actions.hop_kind==HopKind::nollie&&!frame.bike.hop_request_accepted&&frame.joint_actuation_id==actuation_preload&&frame.joint_actuation_seconds>.01,
            "opposite gesture during the pending hop cannot restart or replace its admitted actuator track");
        input.quick_hop=true;frame=pending.step(input);
        check(frame.bike.actions.hop_kind==HopKind::quick_hop&&!frame.bike.hop_request_accepted&&frame.joint_actuation_id==actuation_preload&&frame.joint_actuation_seconds>.02,
            "R3 during a pending Ollie cannot restart the physical command or reserve a duplicate hop");
    }
    check(!CoupledSettings{}.actuation.enabled&&streets_observed_v1().simulation.hop_delay_seconds==.07,"generic host and observedv1 retain their original policies");
    CoupledWorld falling(profile);falling.reset({0,100,0});const auto initial=falling.rider_body().inertia_body;
    auto out=falling.step(neutral());Vec3 change{};for(unsigned i=0;i<3;++i)change[i]=out.rider.inertia_body[i]-initial[i];
    check(out.valid&&std::abs(length(change)-500*.01)<1e-9,"native air inertia changes by one vector MoveTowards budget");
    check(change[0]>0&&change[1]>0&&change[2]>0,"air inertia transition moves all principal components toward observed150 target");
    CoupledWorld world(profile);for(unsigned i=0;i<200;++i){out=world.step(neutral());check(out.valid,"actuated v2 neutral contact remains stable");}
    auto raw=neutral();raw.quick_hop=true;out=world.step(raw);
    check(out.valid&&out.joint_actuation_available&&out.joint_actuation_id==actuation_quick_hop,"accepted quickhop starts actuator commands at input event rather than first impulse");
    check(length(out.bike.hop_impulse)==0,"joint preload command cannot bypass the scheduled physical hop delay");
    const auto saved_chassis=world.body(),saved_rider=world.rider_body();
    CoupledTargets invalid;invalid.suspension_available=true;invalid.suspension=profile.coupled.suspension;invalid.suspension.linear[0].damper=-1;
    out=world.step(neutral(),invalid);
    check(!out.valid&&world.body().center_of_mass==saved_chassis.center_of_mass&&world.rider_body().center_of_mass==saved_rider.center_of_mass,"invalid auxiliary joint command cannot partially integrate either body");
    for(unsigned i=0;i<20;++i){out=world.step(neutral());check(out.valid&&length(out.bike.hop_impulse)==0&&!out.joint_actuation_available,"known rejected gap cancels pending hop and actuator playback together");}
    world.reset();for(unsigned i=0;i<5;++i)world.step(neutral());raw=neutral();raw.quick_hop=true;world.step(raw);raw.menu_open=true;out=world.step(raw);
    check(out.valid&&!out.joint_actuation_available&&length(out.bike.hop_impulse)==0,"menu transition cancels observed actuation without stopping passive body physics");
    // This physical path previously froze exactly when its final command key
    // handed back to a stale resting limit. Exercise playback AND continuation.
    CoupledWorld bri(profile);CoupledOutput previous;bool continuation=false,selected=false;
    for(unsigned tick=0;tick<650;++tick){
        const double time=tick*.01;raw=neutral();raw.quick_hop=time>=1.5&&time<1.6;
        if(time>=1.66&&time<2.14){raw.left_shoulder=true;raw.right_trigger=1;
            if(time>=1.74){raw.right_x=std::sqrt(.5);raw.right_y=-std::sqrt(.5);}}
        out=bri.step(raw);check(out.valid&&out.converged,"BriFlip physical playback and drive-state continuation do not exceed unchanged host budgets");
        selected=selected||out.bike.actions.movement==TrickId::bri_flip_left;
        if(previous.joint_actuation_available&&previous.joint_actuation_id==3&&!out.joint_actuation_available){
            continuation=true;check(std::abs(out.applied_suspension.linear_radial_limit.maximum-previous.applied_suspension.linear_radial_limit.maximum)<.7,
                "ending a recorded air command continues actual limit relaxation instead of snapping to latent baseline");}
        previous=out;
    }
    check(selected&&continuation,"BriFlip regression crosses the verified selection and source-playback end");
    // Actual controller capture selected McGriddle0.22s after R3 and held0.41s,
    // after only0.77s settling. Its rear wheel crossed+.120m to-.112m in one
    // tick. Include adjacent sampled timings while retaining100Hz control clocks.
    for(unsigned variant=0;variant<3;++variant){
        CoupledWorld mc(profile);bool fired=false,swept_seen=false;
        const unsigned select_tick=98+variant,release_tick=select_tick+40+variant;
        for(unsigned tick=1;tick<=450;++tick){raw=neutral();raw.device=100;raw.quick_hop=tick>=77&&tick<84;
            if(tick>=93&&tick<release_tick){raw.left_shoulder=true;raw.right_trigger=1;
                if(tick>=select_tick)raw.right_x=raw.right_y=-23170./32767.;}
            out=mc.step(raw);
            if(!out.valid||!out.converged)std::cerr<<"short McGriddle variant"<<variant<<" tick"<<tick<<" valid"<<out.valid<<" convergence"<<out.converged<<" residual"<<out.coupling_residual<<" spring"<<int(out.suspension.issue)<<" bike"<<int(out.bike.issue)<<'\n';
            check(out.valid&&out.converged,"actual short-settle McGriddle timing and adjacent samples remain valid");
            fired=fired||out.bike.actions.movement==TrickId::mc_griddle_left;
            swept_seen=swept_seen||out.bike.swept_plane_normal_mask!=0;
            for(unsigned wheel=0;wheel<2;++wheel)if(out.bike.swept_plane_normal_mask&(1u<<wheel))
                check(out.input_contacts[wheel].contact.status==ContactStatus::clear&&out.input_contacts[wheel].separation>profile.movement.tuning.contact_gap,
                    "anticipated McGriddle row retains its actual clear positive-gap query");
        }
        check(fired,"short-settle McGriddle regression accepts the intended trick");
        if(variant==1)check(swept_seen,"exact live-failure timing exercises the future-plane normal proof");
    }
    // These two actual short-settle handoffs previously remained in a large
    // radial-limit two-cycle even after96 unrelaxed coupling iterations.
    for(unsigned id:{0u,15u}){
        CoupledWorld attempt(profile);bool fired=false;
        for(unsigned tick=1;tick<=400;++tick){raw=neutral();raw.device=100;raw.quick_hop=tick>=77&&tick<84;
            if(tick>=93&&tick<140){
                if(id==0){raw.left_shoulder=true;raw.right_trigger=1;}else{raw.right_shoulder=true;raw.left_trigger=1;}
                if(tick>=99){if(id==0)raw.right_y=1;else raw.right_x=raw.right_y=-23170./32767.;}}
            out=attempt.step(raw);
            check(out.valid&&out.converged&&out.coupling_residual<=profile.coupled.coupling_tolerance,
                "actual ID0/15 short-settle command handoff satisfies the true unchanged coupling residual");
            check(out.suspension.predicted_energy<=out.suspension.energy_before+out.suspension.actuator_work+
                1e-7*std::max(1.,out.suspension.energy_before),"numerical convergence never bypasses suspension energy admission");
            fired=fired||static_cast<unsigned>(out.bike.actions.movement)==id;
        }
        check(fired,"radial-limit two-cycle regression actually selects the intended source trick");
    }
    CoupledWorld pump(profile);for(unsigned i=0;i<200;++i)pump.step(neutral());
    for(unsigned i=0;i<40;++i){raw=neutral();raw.left_y=-.47244096;out=pump.step(raw);check(out.valid,"negative pump request remains finite");}
    check(out.joint_actuation_available&&out.joint_actuation_id==actuation_pump_negative,"observed negative pump enter endpoint stays active while held");
    for(unsigned i=0;i<40;++i){raw=neutral();raw.left_y=.47244096;out=pump.step(raw);check(out.valid,"negative-to-positive pump transition remains finite");}
    check(out.joint_actuation_available&&out.joint_actuation_id==actuation_pump_positive,"positive-driven transition holds its actual endpoint rather than replaying negative entry");
    out=pump.step(neutral());check(out.valid&&out.joint_actuation_available&&out.joint_actuation_id==actuation_pump_positive,"positive-to-neutral uses its separately measured recovery phase");
    // Every remaining source selection must survive its complete physical
    // trajectory and return to the ordinary host policy without rejected ticks.
    for(const auto& entry:catalog::entries){const auto id=static_cast<unsigned>(entry.id);if(id==3)continue;
        CoupledWorld attempt(profile);bool fired=false;
        for(unsigned tick=0;tick<650;++tick){const double time=tick*.01;raw=neutral();raw.quick_hop=time>=1.5&&time<1.6;
            if(time>=1.66&&time<2.14){switch(id/10){
                case 0:raw.left_shoulder=true;raw.right_trigger=1;break;
                case 1:raw.right_shoulder=true;raw.left_trigger=1;break;
                case 2:raw.left_shoulder=raw.right_shoulder=true;break;
                case 3:raw.left_shoulder=true;break;case 4:raw.right_shoulder=true;break;
                case 5:raw.left_trigger=raw.right_trigger=1;break;case 6:raw.left_trigger=1;break;case 7:raw.right_trigger=1;break;}
                if(time>=1.74){const double angle=(id%10)*std::acos(-1.)/4;raw.right_x=std::sin(angle);raw.right_y=std::cos(angle);}}
            out=attempt.step(raw);
            if(!out.valid)std::cerr<<"actuation id"<<id<<" tick"<<tick<<" spring"<<int(out.suspension.issue)<<" bike"<<int(out.bike.issue)<<'\n';
            check(out.valid&&out.converged,"all58 accepted source-actuation paths remain valid and converged through hop, trick, landing and command handoff");
            fired=fired||out.bike.actions.movement==entry.id;
        }
        check(fired,"full physical actuator test actually accepts each catalog selection");
    }
    for(unsigned selection:{255u,34u,52u}){
        CoupledWorld ordinary(profile);auto input=neutral();for(unsigned i=0;i<200;++i)ordinary.step(input);
        unsigned caught=0,landed=0,failed=0;double touchdown_up=0;bool saw_air=false,checked_touchdown=false;
        for(unsigned tick=0;tick<300;++tick){input=neutral();input.quick_hop=tick==0;
            if(tick==24&&selection==34){input.left_shoulder=true;input.right_y=-1;}
            if(tick==24&&selection==52){input.left_trigger=input.right_trigger=1;input.right_x=1;}
            const auto before=ordinary.output();out=ordinary.step(input);check(out.valid,"ordinary sourcePID hop/trick/landing path stays physically valid");
            if(!out.bike.grounded)saw_air=true;
            if(saw_air&&!checked_touchdown&&out.bike.grounded){touchdown_up=rotate(before.bike.body.orientation,{0,1,0})[1];checked_touchdown=true;}
            caught+=out.bike.events.bar_caught||out.bike.events.tail_caught;
            landed+=out.bike.events.combo_landed;failed+=out.bike.events.failed||out.bike.events.canceled;
        }
        check(checked_touchdown&&touchdown_up>.98,"native external SpringPID returns ordinary hop/trick to level actual support");
        if(selection!=255){if(caught!=1||landed!=1||failed)std::cerr<<"ordinary selection"<<selection<<" caught"<<caught<<" landed"<<landed<<" failed"<<failed<<" finalfailure"<<int(out.bike.tricks.failure)<<" spin"<<out.bike.tricks.spin_radians<<" flip"<<out.bike.tricks.flip_radians<<" roll"<<out.bike.tricks.roll_radians<<'\n';
            check(caught==1&&landed==1&&failed==0,"ordinary barspin/whip uses rider rotation and catches and lands once without score relaxation");}
    }
    std::cout<<checks<<" joint actuator assertions passed\n";
}
