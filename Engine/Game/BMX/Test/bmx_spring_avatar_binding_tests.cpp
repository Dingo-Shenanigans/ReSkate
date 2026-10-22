// Reuse the supported native-axis, proportion and real finger fixtures.
#define main retained_rider_pose_main
#include "Extension/BMX/Test/bmx_rider_pose_tests.cpp"
#undef main
#include "Extension/BMX/bmx_native_motion.h"
#include <iostream>

namespace {
namespace ab=dingosdk::bmx::avatar_binding;
namespace np=dingosdk::bmx::native_coupled::policy;
unsigned binding_checks{};
void expect_binding(bool value,const char* text){++binding_checks;check(value,text);}
np::Epoch epoch() {
    np::Epoch e;e.owner_world={0x1000,0x2000,0x3000,0x4000,0x5000,0x6000,0x7000,0x8000};
    e.bodies={np::BodyKey{0x6000,11,4},np::BodyKey{0x6000,12,7}};e.token=31;e.serial=90;e.time=100;e.seconds=static_cast<float>(np::native_seconds);return e;
}
std::array<float,16> basis(p::Vec root,p::Quat q) {
    std::array<float,16> out{};out[15]=1;
    for(unsigned axis=0;axis<3;++axis){p::Vec unit{};unit[axis]=1;const auto v=p::turn(q,unit);
        for(unsigned i=0;i<3;++i)out[axis*4+i]=v[i];out[12+axis]=root[axis];}
    return out;
}
struct BindingFixture {Frame frame;ab::Observation actual;ab::Capture capture;};
BindingFixture binding_fixture() {
    BindingFixture f;const auto e=epoch();const auto q=p::axis(1,.3f);const p::Vec root{627,199,1062};
    f.actual.available=true;f.actual.epoch=e;f.actual.bike_root=root;f.actual.bike_orientation=q;
    f.actual.spring_orientation=p::multiply(q,np::rider_from_chassis);
    f.actual.spring_com=p::add(root,p::turn(q,np::v2_roles[1].com_from_bike_root));
    auto& c=f.capture;c.requested=true;c.root_height_verified=true;c.mounted_geometry_verified=true;c.neutral=true;
    c.mount_progress=c.rider_blend=1;c.root_height=.25f;c.native.available=c.native.coherent_epoch=true;c.native.epoch=e;
    c.native.position=p::add(root,p::turn(q,{0,c.root_height,0}));c.native.transform=basis(c.native.position,q);
    auto& s=c.seed;s.ready=s.prepared=true;s.issue=dingosdk::bmx::mount_pose::Issue::none;s.request.position=root;
    s.request.world=e.owner_world[5];s.request.context=e.owner_world[7];s.request.serial=e.serial;s.request.simulation_time=e.time;
    s.root=root;s.orientation=q;s.up=p::turn(q,{0,1,0});s.com=p::add(root,p::turn(q,dingosdk::bmx::geometry::com_from_root));
    f.frame.active=true;f.frame.entity=e.owner_world[1];f.frame.native_body_token=e.token;f.frame.position=root;
    f.frame.chassis_quaternion_available=true;f.frame.chassis_quaternion=q;f.frame.spring_avatar.requested=true;
    f.frame.spring_avatar.actual=f.actual;f.frame.spring_avatar.capture=c;f.frame.spring_avatar.native_motion_lease_revision=3;
    return f;
}
void pure_mapping() {
    const auto f=binding_fixture();const auto binding=ab::capture(f.actual,f.capture);
    expect_binding(binding.ready,"fresh full-epoch completed neutral mount calibrates");
    auto target=ab::sample(binding,f.actual);
    expect_binding(target.ready&&ab::close(target.root,f.capture.native.position),"neutral reconstructs actual native root");
    expect_binding(ab::close_rotation(target.rotation,p::axis(1,.3f)),"neutral reconstructs native supplied basis without skeleton correction");
    auto changed=f.actual;changed.epoch.serial++;changed.epoch.time+=np::native_seconds;
    changed.spring_orientation=p::multiply(p::axis(1,.4f),p::multiply(p::axis(0,.2f),f.actual.spring_orientation));
    changed.spring_com=p::add(changed.spring_com,{.1f,.2f,-.15f});changed.spring_velocity={1,2,3};changed.spring_angular_velocity={.3f,-.2f,.5f};
    target=ab::sample(binding,changed);const auto offset=p::turn(changed.spring_orientation,binding.local_root_offset);
    expect_binding(target.ready&&ab::close(target.root,p::add(changed.spring_com,offset)),"root follows independent actual Spring yaw pitch and translation");
    expect_binding(ab::close(target.point_velocity,p::add(changed.spring_velocity,p::cross(changed.spring_angular_velocity,offset))),"point velocity includes omega cross rotated calibrated offset");
    expect_binding(changed.bike_root==f.actual.bike_root&&changed.bike_orientation==f.actual.bike_orientation,"mapping does not substitute a rigid bike-follow transform");
    auto sign=changed;for(auto& v:sign.spring_orientation)v=-v;
    const auto signed_target=ab::sample(binding,sign);
    expect_binding(signed_target.ready&&ab::close(signed_target.root,target.root)&&ab::close_rotation(signed_target.rotation,target.rotation),"quaternion sign does not change mapping");
    for(unsigned field=0;field<12;++field){auto bad=changed;
        if(field<8)bad.epoch.owner_world[field]++;else if(field==8)bad.epoch.bodies[0].generation++;
        else if(field==9)bad.epoch.bodies[1].generation++;else if(field==10)bad.epoch.bodies[1].index++;else bad.epoch.token++;
        expect_binding(!ab::sample(binding,bad).ready,"owner body generation index token or world replacement invalidates calibration");}
    auto stale=f.actual;stale.epoch.serial--;
    expect_binding(!ab::sample(binding,stale).ready,"earlier epoch cannot use calibration");
    stale=f.actual;stale.epoch.serial++;
    expect_binding(!ab::sample(binding,stale).ready,"serial advance without native time advance refuses");
    for(unsigned field=0;field<8;++field){auto bad=f.capture;
        if(field==0)bad.mount_progress=.99f;else if(field==1)bad.rider_blend=.99f;else if(field==2)bad.native.position[0]+=.65f;
        else if(field==3)bad.native.coherent_epoch=false;else if(field==4)bad.native.epoch.serial++;
        else if(field==5)bad.root_height_verified=false;else if(field==6)bad.mounted_geometry_verified=false;else bad.neutral=false;
        expect_binding(!ab::capture(f.actual,bad).ready,"entry standing offset incoherent epoch or unverified geometry cannot calibrate");}
    for(unsigned field=0;field<3;++field){auto bad=f.capture;
        if(field==0)for(unsigned i=0;i<3;++i)bad.native.transform[i]=-bad.native.transform[i];
        else if(field==1)bad.native.transform[0]*=1.1f;else bad.native.transform[4]+=.1f;
        expect_binding(!ab::capture(f.actual,bad).ready,"reflected scaled or sheared supplied basis refuses");}
    dingosdk::bmx::NativeMotionLeaseStatus lease;lease.available=lease.active=lease.details_available=lease.epoch_valid=true;
    lease.timing_available=lease.owner_available=lease.offboard=lease.motion_supported=lease.full_rotation=true;
    lease.lease_revision=3;lease.client=changed.epoch.owner_world[0];lease.entity=changed.epoch.owner_world[1];lease.core=changed.epoch.owner_world[2];
    lease.context=changed.epoch.owner_world[3];lease.rig=changed.epoch.owner_world[4];lease.serial=changed.epoch.serial;lease.simulation_time=changed.epoch.time;
    lease.seconds=changed.epoch.seconds;lease.target=target.root;lease.rotation=target.rotation;lease.velocity=target.point_velocity;
    auto frame=f.frame.spring_avatar;frame.actual=changed;
    expect_binding(ab::motion_lease_matches(frame,target,lease),"exact current SDK motion lease joins calibrated target");
    lease.lease_revision++;
    expect_binding(!ab::motion_lease_matches(frame,target,lease),"different lease revision cannot authorize a bound pose");
}
Fixture at_native_root(const BindingFixture&,p::Vec native_root) {
    auto native=hands_fixture();native.bones[1].position={native_root[0],native_root[1],native_root[2],17};
    native.bones[1].rotation=p::axis(1,.3f);return native;
}
void proximal_contacts() {
    auto f=binding_fixture();auto native=at_native_root(f,f.capture.native.position);p::SpringBinding binding;
    const auto before=native;
    for(unsigned field=0;field<8;++field){auto not_quiet=f.frame;auto refused=before;p::SpringBinding none;
        if(field==0)not_quiet.spring_avatar.capture.neutral=false;else if(field==1)not_quiet.pedal_effort=.1f;
        else if(field==2)not_quiet.steer=.1f;else if(field==3)not_quiet.barspin=.1f;
        else if(field==4)not_quiet.air_extension_blend=.1f;else if(field==5)not_quiet.movement_progress=.1f;
        else if(field==6)not_quiet.movement_bike_pitch=.1f;else not_quiet.movement_bike_roll=.1f;
        expect_binding(!p::solve(refused.bones,refused.rig,not_quiet,nullptr,false,&none).ready,"capture preserves caller quiet-input refusal and rejects nonneutral motion cues");
        expect_binding(std::memcmp(refused.bones.data(),before.bones.data(),before.bones.size()*sizeof(p::Bone))==0,"nonneutral capture refuses before mutating candidate bones");}
    const auto captured=p::solve(native.bones,native.rig,f.frame,nullptr,false,&binding);
    expect_binding(captured.ready&&captured.spring_calibrated&&binding.root.ready&&binding.proximal_ready,"completed strict native mounted candidate captures actual rig geometry");
    expect_binding(captured.hand_error<.003f&&captured.foot_error<.003f,"initial capture requires strict contacts even with no posture cue");
    binding.completed_pose_write=true;
    const auto neutral=p::compose(native.bones,native.rig);const auto hip=neutral[static_cast<unsigned>(native.rig.hips)].position;
    expect_binding(ab::close(p::add(f.actual.spring_com,p::turn(f.actual.spring_orientation,binding.local_hip)),hip),"captured proximal offset reconstructs solved mounted hips, not standing hips");
    expect_binding(p::length(p::sub(binding.local_hip,p::turn(p::inverse(f.actual.spring_orientation),p::sub(p::compose(before.bones,before.rig)[static_cast<unsigned>(before.rig.hips)].position,f.actual.spring_com))))>.05f,"entry-to-riding offset comes from mounted native geometry");
    auto advanced=f.frame;advanced.airborne=true;advanced.spring_avatar.actual.epoch.serial++;advanced.spring_avatar.actual.epoch.time+=np::native_seconds;
    advanced.spring_avatar.actual.spring_com=p::add(f.actual.spring_com,{.01f,.01f,-.01f});
    advanced.spring_avatar.actual.spring_orientation=p::multiply(p::axis(1,.04f),p::multiply(p::axis(0,.035f),f.actual.spring_orientation));
    const auto root=ab::sample(binding.root,advanced.spring_avatar.actual);
    auto next=at_native_root(f,root.root);const auto lengths=p::compose(next.bones,next.rig);
    const auto solved=p::solve(next.bones,next.rig,advanced,nullptr,false,&binding);
    expect_binding(solved.ready&&solved.spring_applied,"independent Spring proximal motion remains reachable with actual bike anchors");
    expect_binding(solved.hand_error<.003f&&solved.foot_error<.003f,"bound candidate preserves strict actual bike contacts");
    const auto actual=p::compose(next.bones,next.rig);
    const auto wanted=p::add(advanced.spring_avatar.actual.spring_com,p::turn(advanced.spring_avatar.actual.spring_orientation,binding.local_hip));
    expect_binding(p::length(p::sub(actual[static_cast<unsigned>(next.rig.hips)].position,wanted))<=.1801f,"proximal reach adjustment remains bounded and reported");
    expect_binding(std::abs(p::length(p::sub(actual[static_cast<unsigned>(next.rig.hips)].position,wanted))-solved.spring_fit_adjustment)<.0002f,"reported fit measures deviation from actual Spring proximal target");
    for(const auto chain:{next.rig.left_arm,next.rig.right_arm,next.rig.left_leg,next.rig.right_leg})for(unsigned i=1;i<3;++i)
        expect_binding(std::abs(p::length(p::sub(actual[chain[i]].position,actual[chain[i-1]].position))-p::length(p::sub(lengths[chain[i]].position,lengths[chain[i-1]].position)))<.0002f,"native limb length survives Spring retarget");
    for(bool left:{true,false}) {
        const auto& hand=actual[static_cast<unsigned>(left?next.rig.left_arm[2]:next.rig.right_arm[2])];
        expect_binding(p::length(p::sub(p::add(hand.position,p::turn(hand.rotation,p::palm_bar_offset(left))),p::grip(advanced,left)))<.003f,"closed palm stays on actual bike grip");
        const auto foot=actual[static_cast<unsigned>(left?next.rig.left_leg[2]:next.rig.right_leg[2])].position;
        expect_binding(p::length(p::sub(foot,p::pedal(advanced,left)))<.003f,"closed ankle stays on actual bike pedal proxy");
    }
    std::cout<<"bound hand_error_m="<<solved.hand_error<<" foot_error_m="<<solved.foot_error<<" proximal_fit_m="<<solved.spring_fit_adjustment<<'\n';
    auto old=f.frame;auto old_pose=at_native_root(f,f.capture.native.position);
    expect_binding(!p::solve(old_pose.bones,old_pose.rig,old,nullptr,false,&binding).ready,"completed binding rejects an earlier render epoch");
    auto same_serial=advanced;same_serial.spring_avatar.actual.epoch.time+=np::native_seconds;auto reused=at_native_root(f,root.root);
    expect_binding(!p::solve(reused.bones,reused.rig,same_serial,nullptr,false,&binding).ready,"completed binding rejects changed time under the same render serial");
    auto moved_bike=advanced;moved_bike.spring_avatar.actual.epoch.serial++;moved_bike.spring_avatar.actual.epoch.time+=np::native_seconds;
    moved_bike.position[0]+=.01f;moved_bike.spring_avatar.actual.bike_root=moved_bike.position;
    moved_bike.chassis_quaternion=p::multiply(p::axis(1,.02f),moved_bike.chassis_quaternion);
    moved_bike.spring_avatar.actual.bike_orientation=moved_bike.chassis_quaternion;
    auto bike_pose=at_native_root(f,root.root);auto bike_binding=binding;
    const auto bike_result=p::solve(bike_pose.bones,bike_pose.rig,moved_bike,nullptr,false,&bike_binding);
    expect_binding(bike_result.ready&&bike_result.spring_applied&&bike_result.hand_error<.003f&&bike_result.foot_error<.003f,"independently moved actual bike retains its own strict grip and pedal anchors");
    auto far=advanced;far.spring_avatar.actual.spring_com[0]+=3;
    const auto far_root=ab::sample(binding.root,far.spring_avatar.actual);auto refused=at_native_root(f,far_root.root);const auto unchanged=refused.bones;
    const auto rejected=p::solve(refused.bones,refused.rig,far,nullptr,false,&binding);
    expect_binding(!rejected.ready&&!rejected.spring_applied,"unreachable Spring-bike separation refuses rather than claiming bike-follow fallback");
    expect_binding(std::memcmp(unchanged.data(),refused.bones.data(),unchanged.size()*sizeof(p::Bone))==0,"failed bound candidates leave source pose untouched");
    auto legacy=f.frame;legacy.spring_avatar={};auto a=at_native_root(f,f.capture.native.position),b=a;p::SpringBinding unused;
    const auto ra=p::solve(a.bones,a.rig,legacy),rb=p::solve(b.bones,b.rig,legacy,nullptr,false,&unused);
    expect_binding(ra.ready==rb.ready&&std::memcmp(a.bones.data(),b.bones.data(),a.bones.size()*sizeof(p::Bone))==0,"default unrequested source path remains identical");
}
}
int main(){pure_mapping();proximal_contacts();expect_binding(retained_rider_pose_main()==0,"existing native pose and anatomy fixture suite remains passing");std::cout<<binding_checks<<" Spring avatar binding checks passed\n";return 0;}
