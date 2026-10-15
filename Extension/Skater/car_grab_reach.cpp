#include "car_grab_reach.h"
#include "traffic_vehicle_provider.h"
#include "car_grab/reach_pose.h"
#include "car_grab/grip_pose.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include <Windows.h>
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_vfx.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dingosdk::car_grab_native {
namespace {
using car_grab::reach::Joint;
constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
constexpr double target_age = .1;
// Researched from the installed AntSkeletonAsset, not guessed from a skin rig.
constexpr std::array<std::uint16_t, 7> torso{0,1,7,42,43,44,45};
constexpr std::array<std::uint16_t, 4> right{46,47,48,49}, left{275,276,277,278};
struct NamedJoint { std::uint32_t index, hash; std::int32_t parent; };
constexpr std::array<NamedJoint, 53> named{{
    {0,0xba3a6344,-1},{1,0x465a9f84,0},{7,0x27311c9f,1},{42,0x7d572f96,7},
    {43,0xe63fe913,42},{44,0xe63fe910,43},{45,0xe63fe911,44},
    {46,0x693846a5,45},{47,0x7234b709,46},{48,0x13c48aab,47},{49,0xd8d7b6b6,48},
    {51,0xb3f6a953,49},{52,0xb3f6a950,51},{53,0xb3f6a951,52},
    {54,0x4f45d9cc,49},{55,0x283a08b6,54},{56,0x283a08b5,55},{57,0x283a08b4,56},
    {58,0x9dfb0ca0,49},{59,0xd7acdf0c,58},{60,0xd7acdf0f,59},{61,0xd7acdf0e,60},
    {62,0xb9c0e005,49},{63,0xfe776e07,62},{64,0xfe776e04,63},{65,0xfe776e05,64},
    {66,0xf2d8a605,49},{67,0xf66c21a9,66},{68,0xf66c21aa,67},{69,0xf66c21ab,68},
    {275,0x3ca15756,45},{276,0xa9603628,275},{277,0xc284d726,276},{278,0xc7e96519,277},
    {279,0x7b193081,278},{280,0x57acf777,279},{281,0x57acf774,280},{282,0x57acf775,281},
    {284,0x2ef5674b,278},{285,0xeba66cb7,284},{286,0xeba66cb4,285},{287,0xeba66cb5,286},
    {288,0xfbeaef50,278},{289,0x43b16f00,288},{290,0x43b16f03,289},{291,0x43b16f02,290},
    {292,0x21b9899a,278},{293,0x35c688dc,292},{294,0x35c688df,293},{295,0x35c688de,294},
    {296,0xb65b68b0,278},{297,0xb65b68b3,296},{298,0xb65b68b2,297}}};
struct DigitIndices { std::uint16_t palm; std::array<std::uint16_t,3> joints; };
constexpr std::array<DigitIndices,5> right_digits{{{0xffff,{51,52,53}},{62,{63,64,65}},{54,{55,56,57}},{66,{67,68,69}},{58,{59,60,61}}}};
constexpr std::array<DigitIndices,5> left_digits{{{0xffff,{296,297,298}},{288,{289,290,291}},{284,{285,286,287}},{292,{293,294,295}},{279,{280,281,282}}}};
struct State {
    std::mutex mutex;
    ReachTarget target;
    ReachFeedback feedback;
    car_grab::grip::Continuity continuity;
    bool have{}, chosen{}, use_left{};
    float weight{};
    double last_update{};
    std::atomic<std::uint64_t> serial{1};
    std::atomic<std::uintptr_t> watched_entity{};
    std::atomic<ReachStatus> status{ReachStatus::idle};
    std::atomic_flag applying=ATOMIC_FLAG_INIT;
};
State& state() { static auto* value = new State; return *value; }
bool readable(std::uintptr_t address, void* output, std::size_t size) noexcept {
    if(address<0x10000||size>0x10000||address>highest-size) return false;
    __try { std::memcpy(output,reinterpret_cast<const void*>(address),size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool write_rotation(std::uintptr_t address, const car_grab::reach::Quaternion& q) noexcept {
    if(address<0x10000||address>highest-sizeof(q)) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address),q.data(),sizeof(q)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool read(std::uintptr_t address, T& value) { return readable(address,&value,sizeof(value)); }
bool pointer(std::uintptr_t address, std::uintptr_t expected) {
    std::uintptr_t result{}; return read(address,result)&&result==expected;
}
double now() noexcept {
    static const double frequency=[] { LARGE_INTEGER n{}; return QueryPerformanceFrequency(&n)&&n.QuadPart>0 ? static_cast<double>(n.QuadPart):0.; }();
    LARGE_INTEGER value{};
    return frequency>0&&QueryPerformanceCounter(&value)&&value.QuadPart>=0 ? static_cast<double>(value.QuadPart)/frequency:0.;
}
bool same(const ReachTarget& a, const ReachTarget& b) {
    return a.base==b.base&&a.client==b.client&&a.context==b.context&&a.entity==b.entity&&a.world==b.world&&a.vehicle==b.vehicle;
}
bool finite_point(car_grab::Vec3 p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<100000&&std::abs(p.y)<100000&&std::abs(p.z)<100000; }
bool valid_target(const ReachTarget& t) {
    return t.base>=0x10000&&t.base<=highest-0x9144000&&t.client>=0x10000&&t.client<=highest-0x1000&&
        t.context>=0x10000&&t.context<=highest-0x1000&&t.entity>=0x10000&&t.entity<=highest-0x1000&&t.world&&t.vehicle&&
        std::isfinite(t.sampled_at)&&t.sampled_at>0&&t.surface_id&&finite_point(t.point)&&finite_point(t.normal)&&finite_point(t.tangent)&&finite_point(t.velocity)&&
        (!t.wrap_valid||(finite_point(t.top_point)&&finite_point(t.bottom_point)&&finite_point(t.top_normal)&&finite_point(t.bottom_normal)));
}
bool owns(const ReachTarget& t, std::uintptr_t component) {
    std::uintptr_t collection{}, player{}, core{}, physics{};
    std::uint32_t game_type{}, client_state{}, physics_state{};
    std::uint8_t pending{}, human{}, ai{};
    return pointer(t.client,t.base+addr::engine::client_vtable)&&pointer(t.client+8,t.context)&&
        read(t.client+0xc0,game_type)&&game_type<=1&&read(t.client+0xc4,client_state)&&(client_state==13||client_state==21)&&
        pointer(t.entity,t.base+addr::engine::skater_entity_vtable)&&pointer(t.entity+0x20,t.context)&&pointer(t.entity+0x628,component)&&
        read(t.entity+0xf8,player)&&player&&pointer(player,t.base+addr::engine::local_player_vtable)&&
        pointer(player+0x78,t.context)&&pointer(player+0xb8,t.entity)&&
        read(player+0x45,human)&&human==1&&read(player+0x44,ai)&&ai==0&&
        read(t.entity+0x7e0,pending)&&pending==0&&pointer(component,t.base+addr::engine::skater_component_vtable)&&
        read(component+0x18,collection)&&collection&&pointer(collection,t.entity)&&
        read(component+0x70,core)&&core&&pointer(core,t.base+addr::no_bail::bail_core_vtable)&&
        read(core+0x3c0,physics)&&physics&&read(physics+0x1414,physics_state)&&physics_state==100;
}
int joint_index(std::uintptr_t base, std::uintptr_t rig, std::uint32_t hash) noexcept {
    // The source-published VFX lookup only reads the rig's joint-ID map. It runs
    // here on the native animation evaluator, never on physics or a helper.
    __try { return reinterpret_cast<int(*)(std::uintptr_t,std::uint32_t,int)>(base+addr::native_vfx::joint_index.rva)(rig,hash,1); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool matching_skeleton(std::uintptr_t base, std::uintptr_t holder) {
    std::uintptr_t rig{}, definition{}, resource{}, skeleton{}, layout{}, parent_map{};
    if(!read(holder+0x78,rig)||!rig||!read(rig+0x18,definition)||!definition||!read(definition+0x1a0,resource)||!resource||
        !read(rig+0x138,skeleton)||!skeleton||!read(skeleton,layout)||!layout||!read(layout+0x60,parent_map)||!parent_map) return false;
    std::array<unsigned char,16> prefix{};
    if(!readable(base+addr::native_vfx::joint_index.rva,prefix.data(),prefix.size())||prefix!=addr::native_vfx::joint_index.prefix) return false;
    // Exact current-image getter 0x4897f90 reads skeleton[0]+0x60 as its int32
    // parent table. A remapped rig uses inverse/forward tables at +0x48/+0x30.
    // Accept only required joints that keep the researched canonical indices.
    std::uint8_t remapped{};
    if(!read(skeleton+0x288,remapped)||remapped>1) return false;
    std::uintptr_t inverse_map{}, forward_map{};
    if(remapped&&(!read(layout+0x48,inverse_map)||!inverse_map||!read(layout+0x30,forward_map)||!forward_map)) return false;
    for(std::size_t i=0;i<named.size();++i) {
        const auto& joint=named[i];
        if(joint_index(base,rig,joint.hash)!=static_cast<int>(joint.index)) return false;
        if(remapped) {
            std::uint32_t original{}, mapped{};
            if(!read(inverse_map+joint.index*4ULL,original)||original!=joint.index||!read(forward_map+joint.index*4ULL,mapped)||mapped!=joint.index) return false;
        }
        std::int32_t parent{};
        if(!read(parent_map+joint.index*4ULL,parent)||parent!=joint.parent) return false;
    }
    return true;
}
bool read_joint(std::uintptr_t buffer, unsigned index, Joint& result) {
    std::array<float,12> value{};
    if(!readable(buffer+index*0x30ULL,value.data(),sizeof(value))) return false;
    // Scale/translation W lanes can contain metadata and are deliberately ignored.
    result={{value[0],value[1],value[2]},{value[4],value[5],value[6],value[7]},{value[8],value[9],value[10]}};
    return car_grab::reach::valid(result);
}
bool arm(std::uintptr_t buffer, const Joint& parent, const std::array<std::uint16_t,4>& chain, car_grab::reach::Arm& out) {
    out.parent_world=parent;
    return read_joint(buffer,chain[0],out.shoulder)&&read_joint(buffer,chain[1],out.upper)&&
        read_joint(buffer,chain[2],out.lower)&&read_joint(buffer,chain[3],out.hand);
}
bool hand(std::uintptr_t buffer,bool use_left,car_grab::grip::Hand& out) {
    out.left=use_left;
    const auto& indices=use_left?left_digits:right_digits;
    for(unsigned i=0;i<5;++i) {
        if(indices[i].palm!=0xffff&&!read_joint(buffer,indices[i].palm,out.fingers[i].palm))return false;
        for(unsigned j=0;j<3;++j)if(!read_joint(buffer,indices[i].joints[j],out.fingers[i].joints[j]))return false;
    }
    return true;
}
float distance_squared(const car_grab::reach::Arm& candidate, car_grab::Vec3 point) {
    const auto shoulder=car_grab::reach::compose(candidate.parent_world,candidate.shoulder);
    const auto upper=car_grab::reach::compose(shoulder,candidate.upper);
    const auto dx=upper.translation.x-point.x,dy=upper.translation.y-point.y,dz=upper.translation.z-point.z;
    return dx*dx+dy*dy+dz*dz;
}
}
void clear_reach() noexcept {
    auto& s=state();
    // Revoke in-flight pose writes before waiting for the short state lock.
    s.watched_entity.store(0,std::memory_order_release);
    s.status.store(ReachStatus::idle,std::memory_order_release);
    s.serial.fetch_add(1,std::memory_order_acq_rel);
    std::lock_guard lock(s.mutex);
    s.have=false; s.weight=0; s.chosen=false; s.last_update=0; s.feedback={}; s.continuity={};
}
void publish_reach(const ReachTarget& next) noexcept {
    if(!valid_target(next)) { clear_reach(); return; }
    auto& s=state();
    std::lock_guard lock(s.mutex);
    const bool rearming=s.have&&!s.target.active&&next.active;
    if(!s.have||!same(s.target,next)||s.target.surface_id!=next.surface_id||rearming) {
        s.serial.fetch_add(1,std::memory_order_acq_rel);
        s.weight=0; s.chosen=false; s.last_update=0; s.feedback={}; s.continuity={};
        s.status.store(next.active?ReachStatus::blending:ReachStatus::idle,std::memory_order_release);
    }
    if(s.have&&s.target.active&&!next.active) {s.serial.fetch_add(1,std::memory_order_acq_rel);s.feedback={};}
    s.target=next; s.have=true;
    s.watched_entity.store(next.entity,std::memory_order_release);
}
ReachStatus reach_status() noexcept { return state().status.load(std::memory_order_acquire); }
bool read_reach_feedback(const ReachTarget& identity,ReachFeedback& out) noexcept {
    out={}; auto& s=state();
    std::lock_guard lock(s.mutex);
    if(!s.have||!s.target.active||!same(s.target,identity)||s.target.surface_id!=identity.surface_id||
        !grip_current(identity.world,identity.vehicle,identity.surface_id)||s.watched_entity.load(std::memory_order_acquire)!=identity.entity)return false;
    const auto age=now()-s.feedback.grip_reach.sampled_at;
    if(!s.feedback.grip_reach.valid||age<0||age>.15)return false;
    out=s.feedback; return true;
}
void reach_on_animation(std::uintptr_t component) noexcept {
    auto& s=state();
    const auto entity=s.watched_entity.load(std::memory_order_acquire);
    if(!entity||!component||!pointer(entity+0x628,component)) return;
    if(s.applying.test_and_set(std::memory_order_acquire)) return;
    struct Finish { State& s; DWORD error=GetLastError(); ~Finish() { s.applying.clear(std::memory_order_release); SetLastError(error); } } finish{s};
    std::uint64_t serial{};
    ReachStatus outcome=ReachStatus::unavailable;
    // Publish one final status, so the client cannot sample a temporary failure
    // while this callback is still validating a healthy pose.
    struct Report {
        State& s; std::uintptr_t entity; std::uint64_t& serial; ReachStatus& outcome;
        ~Report() {
            if(serial&&serial==s.serial.load(std::memory_order_acquire)&&s.watched_entity.load(std::memory_order_acquire)==entity) {
                s.status.store(outcome,std::memory_order_release);
                if(outcome==ReachStatus::unavailable) {
                    std::lock_guard lock(s.mutex);
                    if(serial==s.serial.load(std::memory_order_acquire)) {s.feedback={};s.continuity={};}
                }
            }
        }
    } report{s,entity,serial,outcome};
    const auto mark=[&](ReachStatus status) { outcome=status; };
    try {
        ReachTarget target;
        float weight{};
        car_grab::grip::Continuity continuity;
        double timestamp{}, age{};
        bool active{};
        {
            std::lock_guard lock(s.mutex);
            if(!s.have||s.target.entity!=entity) return;
            target=s.target; serial=s.serial.load(std::memory_order_acquire);
            timestamp=now(); age=timestamp-target.sampled_at;
            active=target.active&&age>=0&&age<=target_age;
            const auto seconds=s.last_update>0 ? timestamp-s.last_update:0.;
            if(seconds<0||seconds>.25) {s.weight=0;s.continuity={};}
            s.weight=car_grab::reach::blend_weight(s.weight,active,std::clamp(seconds,0.,.1));
            s.last_update=timestamp; weight=s.weight;
            continuity=s.continuity;
        }
        if(age<0||age>(target.active?target_age:.15))return;
        if(weight<=0&&!active) { mark(ReachStatus::idle); return; }
        if(!grip_current(target.world,target.vehicle,target.surface_id)||!owns(target,component)) return;
        std::uintptr_t holder{};
        if(!read(component+0xa0,holder)||!holder) return;
        const auto pose=multiplayer::read_native_pose_layout(readable,target.base,holder,512);
        if(!pose.buffer||pose.count!=395) return;
        // Names, indices and parents are rechecked each evaluated pose. Native
        // resources can be rebuilt in-place while retaining every pointer.
        if(serial!=s.serial.load(std::memory_order_acquire)||!matching_skeleton(target.base,holder)) return;
        Joint parent;
        for(const auto index:torso) { Joint child; if(!read_joint(pose.buffer,index,child)) return; parent=car_grab::reach::compose(parent,child); }
        car_grab::reach::Arm right_arm,left_arm;
        if(!arm(pose.buffer,parent,right,right_arm)||!arm(pose.buffer,parent,left,left_arm)) return;
        bool use_left{};
        {
            std::lock_guard lock(s.mutex);
            if(serial!=s.serial.load(std::memory_order_acquire)) return;
            if(!s.chosen) { s.use_left=distance_squared(left_arm,target.point)<distance_squared(right_arm,target.point); s.chosen=true; }
            use_left=s.use_left;
        }
        car_grab::grip::Hand digits;
        if(!hand(pose.buffer,use_left,digits))return;
        const car_grab::grip::Surface surface{target.point,target.normal,target.tangent,target.velocity,target.wrap_valid,
            target.top_point,target.bottom_point,target.top_normal,target.bottom_normal};
        const auto solution=car_grab::grip::solve_continuous(use_left?left_arm:right_arm,digits,surface,weight,continuity,static_cast<float>(std::min(age,.1)));
        overlay::DebugModel skater;
        (void)client_source::detail::debug_skater(target.base,target.client,skater);
        if(!skater.skater_position_valid||skater.skater_identity!=target.entity)return;
        ReachFeedback feedback;
        feedback.grip_reach={active,{solution.shoulder.x-skater.skater_position[0],solution.shoulder.y-skater.skater_position[1],solution.shoulder.z-skater.skater_position[2]},
            solution.minimum_reach,solution.maximum_reach,timestamp};
        feedback.unclamped=!solution.clamped; feedback.palm_contact=active&&solution.palm_contact; feedback.finger_grip=active&&solution.grip_valid;
        feedback.palm_error=solution.palm_error; feedback.surface_id=target.surface_id;
        if(!solution.available||!grip_current(target.world,target.vehicle,target.surface_id)||!owns(target,component)||!pointer(component+0xa0,holder)||
            serial!=s.serial.load(std::memory_order_acquire)||s.watched_entity.load(std::memory_order_acquire)!=entity) return;
        const auto latest=multiplayer::read_native_pose_layout(readable,target.base,holder,512);
        if(latest.buffer!=pose.buffer||latest.count!=pose.count||!grip_current(target.world,target.vehicle,target.surface_id)||
            serial!=s.serial.load(std::memory_order_acquire)||s.watched_entity.load(std::memory_order_acquire)!=entity) return;
        if(weight<=0||!solution.visual_safe) {
            std::lock_guard lock(s.mutex);
            if(serial!=s.serial.load(std::memory_order_acquire)||!same(s.target,target)||s.target.surface_id!=target.surface_id||
                !grip_current(target.world,target.vehicle,target.surface_id))return;
            feedback.palm_contact=false; feedback.finger_grip=false; s.feedback=feedback;s.continuity=continuity;
            mark(weight<=0?ReachStatus::blending:ReachStatus::approaching); return;
        }
        const auto& chain=use_left?left:right;
        if(!grip_current(target.world,target.vehicle,target.surface_id)||serial!=s.serial.load(std::memory_order_acquire)||s.watched_entity.load(std::memory_order_acquire)!=entity)return;
        bool wrote=write_rotation(pose.buffer+chain[1]*0x30ULL+0x10,solution.upper)&&
            write_rotation(pose.buffer+chain[2]*0x30ULL+0x10,solution.lower)&&write_rotation(pose.buffer+chain[3]*0x30ULL+0x10,solution.wrist);
        const auto& indices=use_left?left_digits:right_digits;
        if(solution.finger_pose_available)for(unsigned i=0;i<5&&wrote;++i)for(unsigned j=0;j<3&&wrote;++j)
            wrote=write_rotation(pose.buffer+indices[i].joints[j]*0x30ULL+0x10,solution.fingers[i][j]);
        if(wrote) {
            std::lock_guard lock(s.mutex);
            if(serial!=s.serial.load(std::memory_order_acquire)||!same(s.target,target)||s.target.surface_id!=target.surface_id||
                !grip_current(target.world,target.vehicle,target.surface_id))return;
            s.feedback=feedback;
            s.continuity=continuity;
            mark(weight<1?ReachStatus::blending:solution.grip_valid?ReachStatus::applied:solution.palm_contact?ReachStatus::contact:ReachStatus::approaching);
        }
        // Every next native evaluation writes the original pose before this
        // hook, so release restores naturally without saved stale bone writes.
    } catch(...) { mark(ReachStatus::unavailable); }
}
} // namespace dingosdk::car_grab_native
