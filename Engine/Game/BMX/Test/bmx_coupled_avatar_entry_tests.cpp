// SDK sequencing harness only. All native APIs below are explicit stubs; the
// real mounted geometry solve reuses supported native-axis/finger fixtures.
#define main retained_pose_main
#include "Extension/BMX/Test/bmx_rider_pose_tests.cpp"
#undef main
#include "Extension/BMX/bmx_rider.h"
#include "Extension/BMX/bmx_native_coupled.h"
#include "Extension/BMX/bmx_coupled_avatar_frame.h"
#include "Extension/BMX/bmx_world.h"

namespace entry_test {
namespace bm=dingosdk::bmx;namespace ab=bm::avatar_binding;namespace np=bm::native_coupled::policy;
bm::NativeFrame native;bm::native_coupled::Frame pair;bm::mount_pose::Seed seed;bm::NativeMotionLeaseStatus lease;
bm::visual::Publication publication;p::SpringBinding binding;p::EntryContacts contacts;unsigned checks{},submissions{},placements{},renders{},bound_renders{},successful_entry_renders{};
bool mutate_observe{},wrong_lease{},suppress_render{};
void expect(bool v,const char* text){++checks;check(v,text);}
void transform() {native.transform={1,0,0,0,0,1,0,0,0,0,1,0,native.position[0],native.position[1],native.position[2],1};}
void epoch_update() {
    auto& e=pair.epoch;native.serial=e.serial;native.simulation_time=e.time;native.seconds=e.seconds;
    seed.request.serial=e.serial;seed.request.simulation_time=e.time;
    for(auto& role:pair.roles){role.serial=e.serial;role.simulation_time=e.time;role.step_seconds=e.seconds;}
    transform();
}
void setup() {
    native={};pair={};seed={};lease={};publication={};binding={};contacts={};submissions=placements=renders=bound_renders=successful_entry_renders=0;mutate_observe=wrong_lease=suppress_render=false;
    native.available=native.timing_available=native.offboard=native.standable=native.motion_supported=true;
    auto& e=pair.epoch;e.owner_world={0x1000,0x2000,0x3000,0x4000,0x5000,0x6000,0x7000,0x8000};
    e.bodies={np::BodyKey{0x6000,11,4},np::BodyKey{0x6000,12,7}};e.token=31;e.serial=90;e.time=100;e.seconds=static_cast<float>(np::native_seconds);
    native.client=e.owner_world[0];native.entity=e.owner_world[1];native.core=e.owner_world[2];native.context=e.owner_world[3];native.rig=e.owner_world[4];
    native.position={627.3f,199.25f,1062};pair.available=pair.parked=true;pair.model_orientation={np::Quat{0,0,0,1},np::rider_from_chassis};
    seed.ready=seed.prepared=true;seed.issue=bm::mount_pose::Issue::none;seed.root={627,199,1062};seed.orientation={0,0,0,1};seed.up={0,1,0};
    seed.com=ab::add(seed.root,bm::geometry::com_from_root);seed.request.position=seed.root;seed.request.world=e.owner_world[5];seed.request.context=e.owner_world[7];
    for(unsigned i=0;i<2;++i){auto& role=pair.roles[i];role.available=true;role.world=e.bodies[i].world;role.body_index=e.bodies[i].index;
        role.body_generation=e.bodies[i].generation;role.token=e.token;role.mass=np::v2_roles[i].mass;role.inertia=np::v2_roles[i].principal_moments;
        role.position=ab::add(seed.root,np::v2_roles[i].com_from_bike_root);role.orientation=np::native_orientation(pair.model_orientation[i],np::v2_roles[i].principal_to_model);}
    epoch_update();
}
void advance() {
    if(lease.active)for(unsigned i=0;i<3;++i){const float v=lease.velocity[i]+std::clamp((lease.target[i]-native.position[i])*8.f,-2.f,2.f);native.position[i]+=v*pair.epoch.seconds;}
    ++pair.epoch.serial;pair.epoch.time+=np::native_seconds;epoch_update();
}
void render() {
    if(!publication.available||!publication.frame.active||suppress_render)return;
    auto f=hands_fixture();f.bones[1].position={native.position[0],native.position[1],native.position[2],17};
    const auto result=p::solve(f.bones,f.rig,publication.frame,&contacts,false,publication.frame.spring_avatar.requested?&binding:nullptr);
    ++renders;
    if(result.ready&&publication.frame.spring_avatar.requested){binding.completed_pose_write=true;if(result.spring_applied)++bound_renders;}
    if(result.ready&&!publication.frame.spring_avatar.requested)++successful_entry_renders;
}
}
namespace dingosdk::bmx {
NativeFrame read_native_frame(std::uintptr_t,std::uintptr_t) noexcept{return entry_test::native;}
bool submit_native_chassis_motion(const NativeFrame& n,const std::array<float,3>& velocity,const std::array<float,3>& target,const std::array<float,4>& rotation) noexcept {
    using namespace entry_test;auto& l=lease;l={};l.available=l.active=l.details_available=l.epoch_valid=l.timing_available=l.owner_available=l.offboard=l.motion_supported=l.full_rotation=true;
    l.client=n.client;l.entity=n.entity;l.core=n.core;l.context=n.context;l.rig=n.rig;l.serial=n.serial;l.simulation_time=n.simulation_time;l.seconds=n.seconds;
    l.lease_revision=3;l.target=target;l.velocity=velocity;l.rotation=rotation;if(wrong_lease)l.entity++;++submissions;return true;
}
NativeMotionLeaseStatus native_motion_lease_status(bool) noexcept{return entry_test::lease;}
void release_native_motion() noexcept{entry_test::lease.active=false;}
namespace native_coupled {
Frame observe(std::uintptr_t,const NativeFrame&) noexcept{auto out=entry_test::pair;if(entry_test::mutate_observe)out.roles[1].position[0]+=.01f;return out;}
}
namespace rider {
CoupledAvatarBindingStatus coupled_avatar_binding_status() noexcept{const auto& b=entry_test::binding;return {b.root,b.pose_epoch,b.proximal_ready,b.completed_pose_write,b.last_pose_applied,b.fit_adjustment,b.hand_error,b.foot_error};}
}
namespace visual {
void begin_client_tick() noexcept{++entry_test::publication.revision;}
void publish(const visual::Frame& frame) noexcept{auto& p=entry_test::publication;p.frame=frame;p.available=true;++p.revision;if(!frame.active||!frame.spring_avatar.requested)entry_test::binding={};}
Publication pending_placement() noexcept{return entry_test::publication;}
bool complete_placement(std::uint64_t revision,std::uintptr_t entity,bool complete) noexcept {
    if(!complete||revision!=entry_test::publication.revision||entity!=entry_test::native.entity)return false;entry_test::render();return true;
}
}
namespace world {
PlacementReceipt tick(std::uintptr_t,std::uintptr_t,const visual::Frame& f,std::uint64_t revision) noexcept{++entry_test::placements;return {revision,f.entity,8,8,true};}
}
namespace {
#include "Extension/BMX/bmx_coupled_avatar_entry.inc"
}
}
int main() {
    namespace bm=dingosdk::bmx;namespace ce=bm::coupled_avatar_entry;namespace et=entry_test;
    et::setup();ce::reset();bm::coupled_avatar_entry::Result result;bool saw_entry=false,saw_capture=false;
    for(unsigned i=0;i<360;++i){result=ce::tick(0x100000,et::native,et::pair,et::seed,.25f,true);et::expect(!result.failed,"parked neutral entry advances without a native body/controller allocation");
        if(result.published&&!result.calibrated)saw_entry=true;if(et::binding.root.ready)saw_capture=true;
        if(result.ready)break;et::advance();}
    et::expect(result.ready&&result.actual_spring_render_verified&&saw_entry&&saw_capture,"completed parked entry requires actual Spring render receipt after mounted capture");
    et::expect(et::bound_renders>=1&&et::submissions>1&&et::placements>1,"entry uses genuine motion/placement sequence and bound native geometry fixture");
    et::expect(et::successful_entry_renders>0,"partial mount uses the existing planted native contact transition");
    et::expect(!et::pair.active&&et::pair.parked,"neutral wrapper never activates physics or forces");
    const auto actual=bm::coupled_avatar_frame::observe(et::pair);const auto mapped=bm::avatar_binding::sample(et::binding.root,actual);
    et::expect(mapped.ready&&bm::avatar_binding::close(et::native.position,mapped.root,.003f),"current native root converges through bounded motion correction before readiness");
    et::advance();et::pair.active=true;et::pair.parked=false;et::pair.roles[1].position[0]+=.01f;
    bm::visual::Frame frame;frame.active=true;frame.entity=et::native.entity;frame.position=actual.bike_root;frame.chassis_quaternion=actual.bike_orientation;frame.chassis_quaternion_available=true;frame.airborne=true;
    const auto active=ce::bind_active(0x100000,et::native,et::pair,frame);
    et::expect(active.published&&!active.failed&&!active.ready&&et::binding.last_pose_applied,"active mapping publishes actual Spring pose without claiming a future render or entry readiness");
    et::expect(et::binding.hand_error<.003f&&et::binding.foot_error<.003f,"active Spring pose keeps strict actual bike contacts");
    et::advance();et::suppress_render=true;
    et::expect(ce::bind_active(0x100000,et::native,et::pair,frame).published,"active publication does not invent its future render receipt");
    et::advance();et::suppress_render=false;
    const auto missing=ce::bind_active(0x100000,et::native,et::pair,frame);
    et::expect(missing.failed&&!et::lease.active,"missing previous bound render refuses subsequent active motion");
    et::setup();ce::reset();et::seed.request.serial--;
    result=ce::tick(0x100000,et::native,et::pair,et::seed,.25f,true);
    et::expect(!result.ready&&!result.published&&!result.failed&&!et::submissions,"stale mount seed waits without any motion publication");
    et::setup();ce::reset();et::mutate_observe=true;
    result=ce::tick(0x100000,et::native,et::pair,et::seed,.25f,true);
    et::expect(!result.published&&!result.failed&&!et::submissions,"changed actual pair during enclosing revalidation waits without motion");
    et::setup();ce::reset();et::wrong_lease=true;
    result=ce::tick(0x100000,et::native,et::pair,et::seed,.25f,true);
    et::expect(result.failed&&!result.published&&!et::lease.active,"wrong owner in lease receipt withdraws entry");
    et::setup();ce::reset();(void)ce::tick(0x100000,et::native,et::pair,et::seed,.25f,true);et::advance();
    et::pair.epoch.bodies[1].generation++;et::pair.roles[1].body_generation++;
    result=ce::tick(0x100000,et::native,et::pair,et::seed,.25f,true);
    et::expect(result.failed&&!et::lease.active,"body generation replacement cancels entry and lease");
    et::setup();ce::reset();result=ce::tick(0x100000,et::native,et::pair,et::seed,.25f,false);
    et::expect(result.failed&&!et::submissions,"quiet-input proof is mandatory before entry motion");
    std::cout<<et::checks<<" coupled avatar entry sequencing checks passed (explicit native stubs)\n";return 0;
}
