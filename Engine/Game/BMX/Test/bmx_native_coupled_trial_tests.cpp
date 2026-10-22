#include "Engine/Game/BMX/bmx_native_coupled_trial.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace t=dingosdk::bmx::native_coupled::trial_policy;
namespace n=dingosdk::bmx::native_coupled::policy;
namespace {
unsigned checks{};
void check(bool okay,const char* name){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
// Explicit host-only native API shape. No measured game/body evidence is
// produced or consumed by this pure policy executable.
struct Role {
    bool available{},active{},detached{};
    std::uintptr_t world{},native_state{};std::uint32_t body_index{},body_generation{};
    std::uint64_t token{},serial{};double simulation_time{};float step_seconds{},mass{};
    n::Vec position{},linear_velocity{},angular_velocity{},inertia{};n::Quat orientation{0,0,0,1};
};
struct Frame {
    bool available{},active{},parked{},detached{};n::Epoch epoch;
    std::array<Role,2> roles;std::array<n::Quat,2> model_orientation{};
    std::array<float,2> maximum_angular_speed{};std::array<bool,2> integrates_angular_accumulator{};
    std::array<std::uint32_t,8> categories{},collides_with{};
};
Frame initial(bool active=false){
    Frame f;f.available=true;f.active=active;f.parked=!active;
    auto& e=f.epoch;e.owner_world={1,2,3,4,5,6,7,8};e.bodies={n::BodyKey{6,11,4},n::BodyKey{6,12,7}};
    e.token=31;e.serial=90;e.time=1.5;e.seconds=static_cast<float>(n::native_seconds);
    f.model_orientation={n::Quat{0,0,0,1},n::rider_from_chassis};
    for(unsigned role=0;role<2;++role){auto& b=f.roles[role];b.available=true;b.active=active;b.world=e.bodies[role].world;
        b.native_state=e.owner_world[6];b.body_index=e.bodies[role].index;b.body_generation=e.bodies[role].generation;b.token=e.token;
        b.serial=e.serial;b.simulation_time=e.time;b.step_seconds=e.seconds;b.mass=n::v2_roles[role].mass;b.inertia=n::v2_roles[role].principal_moments;
        b.position=n::v2_roles[role].com_from_bike_root;b.orientation=n::native_orientation(f.model_orientation[role],n::v2_roles[role].principal_to_model);
        f.maximum_angular_speed[role]=15;f.integrates_angular_accumulator[role]=true;}
    if(active)f.collides_with.fill(0x101u);return f;
}
template<class Change> void reject_frame(Change change,const char* name){
    for(bool active:{false,true}){auto f=initial(active);change(f);check(!t::qualified(f,active),name);}
}
template<class Change> void reject_clock(Change change,const char* name){
    const auto e=initial().epoch;t::Clock clock;check(clock.begin(e,1000,true,true,true),"clock identity-rejection setup");
    auto next=e;change(next);check(!clock.accepts(next,1001)&&clock.deadline(next,1001)==0,name);
}
template<class Change> void different(Change change,const char* name){
    for(bool active:{false,true}){const auto a=initial(active);auto b=a;change(b);check(!t::same_actual(a,b)&&!t::same_actual(b,a),name);}
}
}
int main(){
    const auto parked=initial(),dynamic=initial(true);const auto e=parked.epoch;
    check(t::duration_ms==10000&&t::group_ms==100,"trial duration10s and per-group100ms are fixed policy constants");
    for(unsigned flags=0;flags<8;++flags){t::Clock clock;
        const bool accepted=clock.begin(e,1000,(flags&1)!=0,(flags&2)!=0,(flags&4)!=0);
        check(accepted==(flags==7),"exact private profile AND both ride/trial flags required");
        check(clock.used()==accepted,"failed opt-in cannot consume or arm trial");
        if(!accepted)check(clock.begin(e,1000,true,true,true),"subsequent valid begin after refused opt-in remains available");
    }
    t::Clock clock;
    check(!clock.used()&&!clock.accepts(e,1000)&&clock.deadline(e,1000)==0,"unstarted trial cannot submit");
    check(clock.begin(e,1000,true,true,true)&&clock.used(),"full opt-in arms once at actual activation time");
    check(clock.accepts(e,1000)&&clock.deadline(e,1000)==1100,"first command gets at most100ms");
    check(clock.deadline(e,10900)==11000&&clock.deadline(e,10950)==11000&&clock.deadline(e,10999)==11000,"late group deadline is clipped to original10s limit");
    check(clock.accepts(e,10999)&&!clock.accepts(e,11000)&&!clock.accepts(e,11001)&&clock.deadline(e,11000)==0,"exact10s endpoint is exclusive");
    check(!clock.begin(e,10999,true,true,true)&&!clock.begin(e,11001,true,true,true),"begin cannot extend or rearm a used trial before/after expiry");
    check(clock.deadline(e,10999)==11000,"refused rearm cannot move original deadline");
    clock.cancel();check(!clock.accepts(e,1001)&&clock.deadline(e,1001)==0&&clock.used(),"cancel closes submissions while retaining used latch");
    check(!clock.begin(e,1002,true,true,true),"cancel cannot rearm trial");
    t::Clock cancelled;cancelled.cancel();check(cancelled.begin(e,1000,true,true,true),"cancel before begin does not fabricate a consumed execution");
    auto next=e;++next.serial;next.time+=n::native_seconds;
    check(cancelled.accepts(next,1001)&&cancelled.deadline(next,1001)==1101,"same-owner future actual epoch uses fresh short group deadline");
    // Strict per-producer successor/all-four receipts belong to the existing
    // transaction policy. Clock alone is the overall identity/time window.
    next=e;next.serial+=10;next.time+=10*n::native_seconds;check(cancelled.accepts(next,1001),"clock does not pretend to replace separate exact-successor producer policy");
    for(unsigned owner=0;owner<8;++owner){reject_clock([&](auto& x){++x.owner_world[owner];},"every owner/world reference participates in trial identity");
        t::Clock invalid;auto zero=e;zero.owner_world[owner]=0;check(!invalid.begin(zero,1,true,true,true)&&!invalid.used(),"missing owner/world reference cannot arm trial");}
    for(unsigned role=0;role<2;++role){
        reject_clock([&](auto& x){++x.bodies[role].generation;},"both body generations bind trial");
        reject_clock([&](auto& x){x.bodies[role].index+=2;},"both body indices bind trial");
        reject_clock([&](auto& x){++x.bodies[role].world;},"both body worlds bind trial");
    }
    reject_clock([](auto& x){++x.token;},"allocation token replacement cannot inherit trial");
    reject_clock([](auto& x){--x.serial;},"serial before activation cannot inherit trial");
    reject_clock([](auto& x){x.time-=n::native_seconds;},"time before activation cannot inherit trial");
    reject_clock([](auto& x){x.seconds*=2;},"non-native timestep cannot inherit trial");
    reject_clock([](auto& x){x.time=std::numeric_limits<double>::quiet_NaN();},"NaN time cannot inherit trial");
    t::Clock overflow;const auto maximum=std::numeric_limits<std::uint64_t>::max();
    check(!overflow.begin(e,maximum-t::duration_ms+1,true,true,true)&&!overflow.used(),"10s deadline overflow refuses without consuming trial");
    check(overflow.begin(e,maximum-t::duration_ms,true,true,true),"maximum representable fixed deadline is admissible");
    check(overflow.deadline(e,maximum-t::group_ms)==maximum,"100ms deadline overflow boundary remains representable");
    check(overflow.deadline(e,maximum-t::group_ms+1)==0,"group deadline arithmetic overflow fails closed");
    check(t::qualified(parked,false)&&t::qualified(dynamic,true),"actual fixed parked and permitted dynamic role configurations qualify");
    check(!t::qualified(parked,true)&&!t::qualified(dynamic,false),"parked and dynamic states cannot be interchanged");
    reject_frame([](auto& f){f.available=false;},"unavailable actual frame refuses");
    reject_frame([](auto& f){f.active=!f.active;},"frame active bit must match requested phase");
    reject_frame([](auto& f){f.parked=!f.parked;},"frame parked bit must match requested phase");
    reject_frame([](auto& f){f.detached=true;},"detached pair refuses");
    for(unsigned owner=0;owner<8;++owner)reject_frame([&](auto& f){f.epoch.owner_world[owner]=0;},"qualified frame requires full nonzero epoch owner/world tuple");
    reject_frame([](auto& f){f.epoch.token=0;},"zero allocation token refuses");
    reject_frame([](auto& f){f.epoch.serial=0;},"zero actual serial refuses");
    reject_frame([](auto& f){f.epoch.seconds=.02f;},"actual native timestep required");
    reject_frame([](auto& f){f.epoch.bodies[1]=f.epoch.bodies[0];},"roles must identify distinct actual native bodies");
    for(unsigned role=0;role<2;++role){
        reject_frame([&](auto& f){f.roles[role].available=false;},"both native roles must be available");
        reject_frame([&](auto& f){f.roles[role].active=!f.active;},"both native active bits match pair phase");
        reject_frame([&](auto& f){f.roles[role].detached=true;},"detached native role refuses");
        reject_frame([&](auto& f){++f.roles[role].world;},"both role worlds match exact epoch");
        reject_frame([&](auto& f){++f.roles[role].native_state;},"both role native-state references match owner tuple");
        reject_frame([&](auto& f){++f.roles[role].body_index;},"both role indices match exact epoch");
        reject_frame([&](auto& f){++f.roles[role].body_generation;},"both role generations match exact epoch");
        reject_frame([&](auto& f){++f.roles[role].token;},"both role allocation tokens match exact epoch");
        reject_frame([&](auto& f){++f.roles[role].serial;},"both role serials match exact epoch");
        reject_frame([&](auto& f){f.roles[role].simulation_time+=n::native_seconds;},"both role times match exact epoch");
        reject_frame([&](auto& f){f.roles[role].step_seconds*=2;},"both role timesteps match exact epoch");
        reject_frame([&](auto& f){f.maximum_angular_speed[role]=14.999f;},"actual998 must be exactly15 for both roles");
        reject_frame([&](auto& f){f.integrates_angular_accumulator[role]=false;},"actualA68 angular accumulator enabled on both roles");
        reject_frame([&](auto& f){f.roles[role].mass+=.01f;},"actual role masses remain30/80kg");
        reject_frame([&](auto& f){f.roles[role].mass=std::numeric_limits<float>::quiet_NaN();},"nonfinite mass refuses");
        reject_frame([&](auto& f){f.roles[role].orientation={0,0,0,0};},"both native principal orientations must be unit quaternions");
        reject_frame([&](auto& f){f.roles[role].position[1]=std::numeric_limits<float>::infinity();},"nonfinite actual COM refuses");
        reject_frame([&](auto& f){f.roles[role].linear_velocity[0]=50.01f;},"actual component speed over50 refuses");
        reject_frame([&](auto& f){f.roles[role].angular_velocity[0]=40.01f;},"actual angular component speed over40 refuses");
        for(unsigned axis=0;axis<3;++axis){
            reject_frame([&](auto& f){f.roles[role].inertia[axis]=n::v2_roles[role].principal_moments[axis]-.01f;},"inertia cannot shrink below actual configured principal minimum");
            reject_frame([&](auto& f){f.roles[role].inertia[axis]=150.01f;},"inertia remains finite below150 on all axes");
            reject_frame([&](auto& f){f.roles[role].inertia[axis]=std::numeric_limits<float>::quiet_NaN();},"NaN inertia refuses");
            auto rest=parked;rest.roles[role].inertia[axis]+=.01f;check(!t::qualified(rest,false),"parked entry always retains configured role principal inertia");
        }
    }
    for(unsigned axis=0;axis<3;++axis){auto bike=dynamic;bike.roles[0].inertia[axis]+=.01f;check(!t::qualified(bike,true),"dynamic bike inertia cannot be rewritten by controller trial");
        auto rider=dynamic;rider.roles[1].inertia[axis]=150;check(t::qualified(rider,true),"dynamic rider may use separately verified principal inertia up to150");}
    for(unsigned shape=0;shape<8;++shape){
        reject_frame([&](auto& f){f.categories[shape]=1;},"all eight category masks must retain mutual-pair suppression0");
        auto rest=parked;rest.collides_with[shape]=0x101;check(!t::qualified(rest,false),"parked entry proxy collision mask must remain0");
        auto moving=dynamic;moving.collides_with[shape]=0;check(!t::qualified(moving,true),"dynamic proxy world collision mask must be0x101");
        moving=dynamic;moving.collides_with[shape]=0x103;check(!t::qualified(moving,true),"extra dynamic collision categories refuse");
    }
    check(t::same_actual(parked,parked)&&t::same_actual(dynamic,dynamic),"same actual observation compares equal in both qualified phases");
    different([](auto& f){++f.epoch.owner_world[7];},"same-actual comparator includes full epoch wrapper");
    different([](auto& f){++f.epoch.bodies[1].generation;},"same-actual comparator includes both body-key epochs");
    different([](auto& f){++f.epoch.serial;},"same-actual comparator includes serial");
    different([](auto& f){f.epoch.time+=n::native_seconds;},"same-actual comparator includes time");
    for(unsigned role=0;role<2;++role){
        different([&](auto& f){f.maximum_angular_speed[role]=14;},"reobserved actual998 change refuses");
        different([&](auto& f){f.integrates_angular_accumulator[role]=false;},"reobserved actualA68 change refuses");
        different([&](auto& f){f.roles[role].mass+=.001f;},"reobserved native mass change refuses");
        for(unsigned axis=0;axis<3;++axis){
            different([&](auto& f){f.roles[role].position[axis]+=.001f;},"reobserved actual COM change refuses");
            different([&](auto& f){f.roles[role].linear_velocity[axis]+=.001f;},"reobserved actual velocity change refuses");
            different([&](auto& f){f.roles[role].angular_velocity[axis]+=.001f;},"reobserved actual angular velocity change refuses");
            different([&](auto& f){f.roles[role].inertia[axis]+=.001f;},"reobserved actual principal inertia change refuses");
        }
        for(unsigned axis=0;axis<4;++axis){
            different([&](auto& f){f.roles[role].orientation[axis]+=.001f;},"reobserved actual principal orientation change refuses");
            different([&](auto& f){f.model_orientation[role][axis]+=.001f;},"reobserved actual model orientation change refuses");
        }
    }
    for(unsigned shape=0;shape<8;++shape){different([&](auto& f){f.categories[shape]=1;},"reobserved any category mask change refuses");
        different([&](auto& f){f.collides_with[shape]^=1;},"reobserved any collision mask change refuses");}
    // same_actual is deliberately a value-comparison helper, not readiness or
    // provenance proof. Production must independently qualify BOTH readings.
    auto invalid=parked;invalid.roles[1].body_generation++;
    check(!t::qualified(invalid,false)&&!(t::qualified(parked,false)&&t::qualified(invalid,false)&&t::same_actual(parked,invalid)),
        "metadata replacement cannot pass composed qualified-before/after proof");
    std::printf("coupled finite controller trial policy: %u checks PASS (explicit host fixtures)\n",checks);
}
