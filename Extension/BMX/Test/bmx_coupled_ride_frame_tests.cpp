#include "Extension/BMX/bmx_coupled_ride_frame.h"
#include "Extension/BMX/bmx_native_coupled.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
namespace b=dingosdk::bmx;
namespace c=b::coupled_ride_frame;
namespace n=b::native_coupled::policy;
unsigned checks{};
void check(bool okay,const char* name){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
void near(double got,double wanted,double tolerance,const char* name){check(std::isfinite(got)&&std::abs(got-wanted)<=tolerance,name);}
b::native_coupled::Frame measured(){
    b::native_coupled::Frame f;f.available=f.active=true;f.epoch.owner_world={1,2,3,4,5,6,7,8};
    f.epoch.bodies={n::BodyKey{6,12,3},n::BodyKey{6,19,9}};f.epoch.token=4;f.epoch.serial=100;
    f.epoch.time=100*n::native_seconds;f.epoch.seconds=static_cast<float>(n::native_seconds);
    const c::Vec root{100,20,-30};const c::Quat q{0,.3826834324f,0,.9238795325f};
    for(unsigned role=0;role<2;++role){auto& x=f.roles[role];x.available=x.active=true;
        x.world=f.epoch.bodies[role].world;x.native_state=f.epoch.owner_world[6];x.body_index=f.epoch.bodies[role].index;
        x.body_generation=f.epoch.bodies[role].generation;x.token=f.epoch.token;x.serial=f.epoch.serial;
        x.simulation_time=f.epoch.time;x.step_seconds=f.epoch.seconds;x.mass=n::v2_roles[role].mass;x.inertia=n::v2_roles[role].principal_moments;
        f.model_orientation[role]=role?n::multiply(q,n::rider_from_chassis):q;
        x.orientation=n::native_orientation(f.model_orientation[role],n::v2_roles[role].principal_to_model);
        x.position=c::add(root,n::rotate(q,n::v2_roles[role].com_from_bike_root));
        x.linear_velocity={1,2,3};x.angular_velocity={0,2,0};
    }
    f.maximum_angular_speed={15,15};f.integrates_angular_accumulator={true,true};f.collides_with.fill(0x101);return f;
}
int main(){
    for(const auto& authored : ::bmx::detail::source::catalog::entries){
        const auto* native=b::catalog::find(static_cast<b::catalog::TrickId>(authored.id));
        check(native&&native->name==authored.name&&native->release_mask==authored.release_mask,"confirmed source trick presentation identity maps to installed catalog");
    }
    const auto pair=measured();const auto out=c::convert(pair,.95f);check(out.available,"both actual bodies convert");
    for(unsigned axis=0;axis<3;++axis)near(out.bike_root[axis],c::Vec{100,20,-30}[axis],.00003,"COM principal-to-model root conversion");
    check(out.ignored_bodies[0]==std::array<std::uint64_t,2>{6,(std::uint64_t{3}<<32)|12}&&
          out.ignored_bodies[1]==std::array<std::uint64_t,2>{6,(std::uint64_t{9}<<32)|19},"both real generation-qualified bodies excluded from queries");
    for(unsigned role=0;role<2;++role)for(unsigned axis=0;axis<3;++axis){
        near(out.bodies[role].center_of_mass[axis],pair.roles[role].position[axis],0,"actual COM never synthesized");
        near(out.bodies[role].linear_velocity[axis],pair.roles[role].linear_velocity[axis],0,"actual linear velocity");
        near(out.bodies[role].inertia_body[axis],pair.roles[role].inertia[axis],0,"actual principal inertia");
    }
    auto moved=pair;moved.roles[1].position[0]+=.25f;
    const auto spring_move=c::convert(moved,.95f);check(spring_move.available,"independent actual Spring movement admitted");
    check(spring_move.bike_root==out.bike_root,"Spring movement does not invent bike movement");
    near(spring_move.spring.center_of_mass[0]-out.spring.center_of_mass[0],.25,0,"actual Spring COM changes witness");
    check(spring_move.spring.epoch==pair.epoch,"Spring witness retains exact full pair epoch");
    check(!c::same_observation(pair,moved),"postquery Spring movement invalidates query batch");
    for(unsigned role=0;role<2;++role){
        auto wrong=pair;wrong.roles[role].native_state++;check(!c::convert(wrong,.95f).available,"native state must match epoch");
        wrong=pair;wrong.roles[role].body_generation++;check(!c::convert(wrong,.95f).available,"both generations must match epoch");
        wrong=pair;wrong.roles[role].serial++;check(!c::convert(wrong,.95f).available,"both role serials bound");
        wrong=pair;wrong.roles[role].orientation={0,0,0,1};check(!c::convert(wrong,.95f).available,"principal orientation cannot masquerade as model frame");
        wrong=pair;wrong.roles[role].linear_velocity[0]=std::numeric_limits<float>::quiet_NaN();check(!c::convert(wrong,.95f).available,"nonfinite actual body rejected");
        wrong=pair;wrong.roles[role].available=false;check(!c::same_observation(pair,wrong),"postquery unavailable role rejected");
        wrong=pair;wrong.roles[role].native_state++;check(!c::same_observation(pair,wrong),"postquery native state changed");
    }
    check(!c::convert(pair,std::numeric_limits<float>::infinity()).available,"invalid calibration rejected");
    auto sign=pair;for(auto& role:sign.roles)for(float& x:role.orientation)x=-x;
    check(c::convert(sign,.95f).available,"quaternion sign equivalent actual poses admitted");
    for(unsigned component=0;component<4;++component){auto normalized=pair;for(auto& q:normalized.model_orientation)q[component]*=1.00001f;
        const auto actual=c::convert(normalized,.95f);if(actual.available){double norm{};for(float x:actual.bike_rotation)norm+=double(x)*x;near(norm,1,1e-6,"geometry uses normalized measured modelQ");}}
    n::Receipt receipt;receipt.state=n::ReceiptState::queued;receipt.epoch=pair.epoch;receipt.token=pair.epoch.token;receipt.serial=pair.epoch.serial;receipt.hop_part=3;
    check(c::pending_status(pair.epoch,3,pair.epoch,receipt)==c::Pending::wait,"same epoch awaits actual next body without replay");
    auto next=pair.epoch;next.serial++;next.time+=next.seconds;
    check(c::pending_status(pair.epoch,3,next,receipt)==c::Pending::reject,"queued producer at next epoch cannot commit");
    receipt.state=n::ReceiptState::submitted;
    check(c::pending_status(pair.epoch,3,next,receipt)==c::Pending::next,"complete producer waits for next actual body epoch");
    for(auto state:{n::ReceiptState::idle,n::ReceiptState::terminal,n::ReceiptState::cancelled,n::ReceiptState::expired}){
        auto bad=receipt;bad.state=state;check(c::pending_status(pair.epoch,3,pair.epoch,bad)==c::Pending::reject,"terminal receipt rejects even before next body");}
    for(unsigned field=0;field<8;++field){auto bad=receipt;bad.epoch.owner_world[field]++;
        check(c::pending_status(pair.epoch,3,next,bad)==c::Pending::reject,"every receipt owner/world component bound");}
    auto badnext=next;badnext.serial++;badnext.time+=next.seconds;
    check(c::pending_status(pair.epoch,3,badnext,receipt)==c::Pending::reject,"skipped native epoch cannot commit");
    badnext=pair.epoch;badnext.time+=.001;check(c::pending_status(pair.epoch,3,badnext,receipt)==c::Pending::reject,"same serial different time rejects");
    badnext=next;badnext.bodies[1].generation++;check(c::pending_status(pair.epoch,3,badnext,receipt)==c::Pending::reject,"next actual Spring generation bound");
    check(c::pending_status(pair.epoch,4,next,receipt)==c::Pending::reject,"hop receipt identity bound");
    n::Command oversized;oversized.roles[0].force={40001,0,0};
    check(!n::prepare(oversized,n::native_seconds).valid,"native SDK40k force budget remains closed");
    std::printf("coupled measured frame/pending: %u checks PASS\n",checks);
}
