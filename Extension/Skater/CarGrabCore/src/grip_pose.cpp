#include "car_grab/grip_pose.h"
#include <algorithm>
#include <cmath>
namespace car_grab::grip {
namespace {
constexpr float palm_clearance=.012f, finger_clearance=.008f;
struct SearchBudget {
    unsigned remaining=1024, repair_remaining=256; bool repairing{};
    bool usable() const { return remaining&&(!repairing||repair_remaining); }
    bool spend() { if(!usable())return false;--remaining;if(repairing)--repair_remaining;return true; }
};
Vec3 add(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a,float v) { return {a.x*v,a.y*v,a.z*v}; }
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float length(Vec3 v) { return std::sqrt(dot(v,v)); }
Vec3 unit(Vec3 v) { const auto n=length(v); return n>1e-7f?mul(v,1/n):Vec3{}; }
bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::abs(v.x)<100000&&std::abs(v.y)<100000&&std::abs(v.z)<100000; }
Quaternion normalize(Quaternion q) { float n=0; for(float v:q)n+=v*v; if(n<1e-12f||!std::isfinite(n))return {0,0,0,1}; for(auto& v:q)v/=std::sqrt(n); return q; }
Quaternion inverse(Quaternion q) { return {-q[0],-q[1],-q[2],q[3]}; }
Quaternion multiply(Quaternion a,Quaternion b) {
    return normalize({a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]});
}
Vec3 rotate(Quaternion q,Vec3 v) { Vec3 a{q[0],q[1],q[2]}; const auto c=mul(cross(a,v),2); return add(v,add(mul(c,q[3]),cross(a,c))); }
Quaternion blend(Quaternion a,Quaternion b,float w) { if(w<=0)return a; float d=0; for(unsigned i=0;i<4;++i)d+=a[i]*b[i]; if(d<0)for(auto& f:b)f=-f; Quaternion r; for(unsigned i=0;i<4;++i)r[i]=a[i]+(b[i]-a[i])*w; return normalize(r); }
Quaternion align(Vec3 a,Vec3 b) {
    a=unit(a); b=unit(b); const auto d=std::clamp(dot(a,b),-1.f,1.f);
    if(d<-.9999f) { const auto v=unit(cross(a,std::abs(a.x)<.7f?Vec3{1,0,0}:Vec3{0,1,0})); return {v.x,v.y,v.z,0}; }
    const auto v=cross(a,b); return normalize({v.x,v.y,v.z,1+d});
}
Quaternion basis(Vec3 x,Vec3 y,Vec3 z) {
    Quaternion q; const auto trace=x.x+y.y+z.z;
    if(trace>0) { const auto k=std::sqrt(trace+1)*2; q={(y.z-z.y)/k,(z.x-x.z)/k,(x.y-y.x)/k,k/4}; }
    else if(x.x>y.y&&x.x>z.z) { const auto k=std::sqrt(1+x.x-y.y-z.z)*2; q={k/4,(y.x+x.y)/k,(z.x+x.z)/k,(y.z-z.y)/k}; }
    else if(y.y>z.z) { const auto k=std::sqrt(1+y.y-x.x-z.z)*2; q={(y.x+x.y)/k,k/4,(z.y+y.z)/k,(z.x-x.z)/k}; }
    else { const auto k=std::sqrt(1+z.z-x.x-y.y)*2; q={(z.x+x.z)/k,(z.y+y.z)/k,k/4,(x.y-y.x)/k}; }
    return normalize(q);
}
// Intersection of a bone segment with the expanded rear/top/bottom hull slab.
// The bone capsule must stay outside at least one measured surface plane.
bool outside(Vec3 a,Vec3 b,const Surface& s) {
    float lo=0,hi=1;
    const std::array<Vec3,3> points{s.point,s.top_point,s.bottom_point};
    const std::array<Vec3,3> normals{s.normal,s.top_normal,s.bottom_normal};
    for(unsigned i=0;i<3;++i) {
        const auto from=dot(sub(a,points[i]),normals[i])-finger_clearance;
        const auto to=dot(sub(b,points[i]),normals[i])-finger_clearance;
        if(from>=-1e-5f&&to>=-1e-5f) return true;
        if(from<0&&to<0) continue;
        const auto at=from/(from-to);
        if(from>=0)lo=std::max(lo,at); else hi=std::min(hi,at);
        if(lo>=hi-1e-5f)return true;
    }
    return false;
}
bool valid_lip(const Surface& s,Vec3 up) {
    if(!s.wrap_valid||!finite(s.top_point)||!finite(s.bottom_point)||!finite(s.top_normal)||!finite(s.bottom_normal))return false;
    if(std::abs(length(s.top_normal)-1)>.05f||std::abs(length(s.bottom_normal)-1)>.05f||dot(s.top_normal,up)<.85f||dot(s.bottom_normal,up)>-.85f)return false;
    const auto height=dot(sub(s.top_point,s.bottom_point),up);
    const auto top=dot(sub(s.top_point,s.point),up), bottom=dot(sub(s.point,s.bottom_point),up);
    const auto depth_top=-dot(sub(s.top_point,s.point),s.normal),depth_bottom=-dot(sub(s.bottom_point,s.point),s.normal);
    return height>=.006f&&height<=.06f&&top>=-.002f&&bottom>=-.002f&&depth_top>=.001f&&depth_bottom>=.001f&&depth_top<=.04f&&depth_bottom<=.04f;
}
bool digit(const Finger& finger,const Joint& hand,Vec3 target,Vec3 pole,const Surface& s,Vec3 terminal_axis,std::array<Quaternion,3>& rotations,
           const float* preferred_pole,float& selected_pole,SearchBudget& budget,bool repair_choices) {
    if(!budget.spend())return false;
    const auto palm=reach::compose(hand,finger.palm);
    const auto first=reach::compose(palm,finger.joints[0]),second=reach::compose(first,finger.joints[1]),third=reach::compose(second,finger.joints[2]);
    const auto a=length(sub(second.translation,first.translation)),b=length(sub(third.translation,second.translation));
    const auto direction=unit(sub(target,first.translation)); const auto distance=length(sub(target,first.translation));
    if(distance<.001f||distance<std::abs(a-b)+.001f||distance>.995f*(a+b))return false;
    const auto cosine=std::clamp((a*a+distance*distance-b*b)/(2*a*distance),-1.f,1.f);
    Vec3 elbow{}; bool safe=false;
    // Prefer curling around the top/underside. A thumb starting beside the
    // rear face may need to bend outward first before passing under the lip.
    const auto attempt=[&](float parameter) {
        if(!budget.spend())return false;
        const auto candidate=parameter==0?pole:parameter==1?s.normal:
            parameter==2.f/3?add(pole,mul(s.normal,2)):add(mul(pole,1-parameter),mul(s.normal,parameter));
        const auto perpendicular=unit(sub(candidate,mul(direction,dot(candidate,direction))));
        if(length(perpendicular)<.5f)return false;
        elbow=add(first.translation,add(mul(direction,a*cosine),mul(perpendicular,a*std::sqrt(std::max(0.f,1-cosine*cosine)))));
        if(!outside(first.translation,elbow,s)||!outside(elbow,target,s))return false;
        selected_pole=parameter; return true;
    };
    if(preferred_pole) {
        safe=attempt(*preferred_pole);
        // Keep the same bend whenever it is still safe. At a boundary, nearby
        // continuous bend planes avoid the former 0/2:1/1 branch jump.
        for(unsigned step=1;repair_choices&&step<=8&&!safe&&budget.usable();++step) {
            const auto delta=step/64.f;
            if(*preferred_pole+delta<=1)safe=attempt(*preferred_pole+delta);
            if(!safe&&*preferred_pole-delta>=0)safe=attempt(*preferred_pole-delta);
        }
    }
    if(preferred_pole&&!repair_choices&&!safe)return false;
    if(!safe)for(const auto parameter:std::array<float,3>{0,2.f/3,1})if(attempt(parameter)) {safe=true;break;}
    if(!safe)return false;
    const auto world_first=multiply(align(sub(second.translation,first.translation),sub(elbow,first.translation)),first.rotation);
    rotations[0]=multiply(inverse(palm.rotation),world_first);
    auto j1=finger.joints[0]; j1.rotation=rotations[0]; const auto w1=reach::compose(palm,j1);
    const auto w2=reach::compose(w1,finger.joints[1]),w3=reach::compose(w2,finger.joints[2]);
    const auto world_second=multiply(align(sub(w3.translation,w2.translation),sub(target,w2.translation)),w2.rotation);
    rotations[1]=multiply(inverse(w1.rotation),world_second);
    auto j2=finger.joints[1]; j2.rotation=rotations[1]; const auto final_second=reach::compose(w1,j2);
    const auto final_third=reach::compose(final_second,finger.joints[2]);
    rotations[2]=multiply(inverse(final_second.rotation),multiply(align(rotate(final_third.rotation,{1,0,0}),terminal_axis),final_third.rotation));
    return true;
}
Solution checked_pose(Solution out,const Hand& h,const Joint& hand_world,const Surface& s,bool lip) {
    if(!out.available)return out;
    for(unsigned i=0;i<5;++i) {
        auto parent=reach::compose(hand_world,h.fingers[i].palm);
        Vec3 previous{};
        for(unsigned j=0;j<3;++j) {
            auto bone=h.fingers[i].joints[j]; bone.rotation=out.fingers[i][j];
            const auto joint=reach::compose(parent,bone);
            const bool safe=!j||(lip?outside(previous,joint.translation,s):
                dot(sub(previous,s.point),s.normal)>=finger_clearance-1e-5f&&dot(sub(joint.translation,s.point),s.normal)>=finger_clearance-1e-5f);
            if(!safe) {
                out.visual_safe=false; out.finger_pose_available=false; out.grip_valid=false; out.palm_contact=false;
                return out;
            }
            previous=joint.translation; parent=joint;
        }
    }
    return out;
}
}
static Solution solve_at_clearance(const reach::Arm& arm,const Hand& hand,const Surface& input,float weight,float prediction,float extra_clearance,SearchBudget& budget,
                                   const Choices* preferred=nullptr,bool repair_choices=false) noexcept {
    Solution out;
    if(!budget.spend())return out;
    if(!std::isfinite(weight)||!std::isfinite(prediction)||prediction<0||prediction>.1f||!finite(input.point)||!finite(input.normal)||!finite(input.tangent)||!finite(input.velocity)||length(input.velocity)>80)return out;
    if(std::abs(length(input.normal)-1)>.05f||std::abs(length(input.tangent)-1)>.05f||std::abs(dot(input.normal,input.tangent))>.1f)return out;
    if(!reach::valid(arm.parent_world)||!reach::valid(arm.shoulder)||!reach::valid(arm.upper)||!reach::valid(arm.lower)||!reach::valid(arm.hand))return out;
    for(const auto& finger:hand.fingers) {
        if(!reach::valid(finger.palm))return out;
        for(const auto& joint:finger.joints)if(!reach::valid(joint))return out;
        const auto a=length(finger.joints[1].translation),b=length(finger.joints[2].translation);
        if(a<.005f||b<.005f||a>.15f||b>.15f)return out;
    }
    weight=std::clamp(weight,0.f,1.f);
    auto s=input; s.normal=unit(s.normal); s.tangent=unit(sub(s.tangent,mul(s.normal,dot(s.normal,s.tangent))));
    const auto travel=mul(s.velocity,prediction); s.point=add(s.point,travel); s.top_point=add(s.top_point,travel); s.bottom_point=add(s.bottom_point,travel);
    auto up=unit(cross(s.normal,s.tangent)); if(up.y<0)up=mul(up,-1);
    if(up.y<.5f)return out;
    const auto z=mul(s.normal,hand.left?-1.f:1.f),y=unit(cross(z,up));
    const auto desired_world=basis(up,y,z);
    // Knuckle midpoint derives from live proportions. Skin clearance is an
    // explicit proxy: an unavailable skinned mesh endpoint is never claimed.
    Vec3 knuckles{};
    for(unsigned i=1;i<5;++i)knuckles=add(knuckles,reach::compose(hand.fingers[i].palm,hand.fingers[i].joints[0]).translation);
    const auto palm_local=mul(knuckles,.125f);
    const auto shoulder=reach::compose(arm.parent_world,arm.shoulder),upper=reach::compose(shoulder,arm.upper),lower=reach::compose(upper,arm.lower),native_hand=reach::compose(lower,arm.hand);
    const auto a=length(sub(lower.translation,upper.translation)),b=length(sub(native_hand.translation,lower.translation));
    out.shoulder=upper.translation; out.minimum_reach=std::abs(a-b)+.02f*std::min(a,b); out.maximum_reach=.98f*(a+b);
    const auto offset=rotate(desired_world,mul(palm_local,native_hand.scale.x));
    // The thumb base can protrude beyond the knuckle plane. Derive a larger
    // clearance from live roots so its capsule never starts inside the face.
    // This remains an explicit hand collision proxy, not a skin mesh socket.
    float clearance=palm_clearance;
    for(const auto& finger:hand.fingers) {
        const auto root=reach::compose(finger.palm,finger.joints[0]).translation;
        const auto facing=dot(rotate(desired_world,mul(sub(root,palm_local),native_hand.scale.x)),s.normal);
        clearance=std::max(clearance,finger_clearance-facing);
    }
    clearance+=extra_clearance;
    if(clearance>.05f)return {};
    out.palm_clearance=clearance;
    out.choices.extra_clearance=extra_clearance;
    Vec3 palm_target=s.point;
    if(valid_lip(s,up)) {
        // Put the live thumb root below the measured underside before closing.
        // The adjustment stays on the measured rear plane, within the 2 cm
        // contact tolerance and the provider's verified rear footprint.
        const auto thumb=reach::compose(hand.fingers[0].palm,hand.fingers[0].joints[0]).translation;
        const auto thumb_up=dot(rotate(desired_world,mul(sub(thumb,palm_local),native_hand.scale.x)),up);
        const auto bottom=dot(sub(s.bottom_point,s.point),up);
        const auto shift=std::min(0.f,bottom-finger_clearance-thumb_up);
        palm_target=add(palm_target,mul(up,std::max(-.0199f,shift)));
    }
    const auto wrist_target=sub(add(palm_target,mul(s.normal,clearance)),offset);
    const auto full_reach=reach::solve(arm,wrist_target,1); if(!full_reach.available)return out;
    // Blending the two joint rotations independently sweeps a curved endpoint
    // past the face. Solve a straight wrist path instead, without bone stretch.
    const auto path=add(native_hand.translation,mul(sub(wrist_target,native_hand.translation),weight));
    const auto reached=weight<=0?reach::solve(arm,wrist_target,0):weight>=.999f?full_reach:reach::solve(arm,path,1);
    if(!reached.available)return out;
    out.available=true; out.visual_safe=true; out.clamped=full_reach.clamped; out.upper=reached.upper; out.lower=reached.lower;
    auto solved_upper=arm.upper,solved_lower=arm.lower,solved_hand=arm.hand;
    solved_upper.rotation=out.upper; solved_lower.rotation=out.lower;
    const auto upper_world=reach::compose(shoulder,solved_upper),lower_world=reach::compose(upper_world,solved_lower);
    out.wrist=blend(arm.hand.rotation,multiply(inverse(lower_world.rotation),desired_world),weight); solved_hand.rotation=out.wrist;
    const auto hand_world=reach::compose(lower_world,solved_hand);
    const auto local_normal=Vec3{0,0,hand.left?1.f:-1.f};
    out.palm_position=add(reach::compose(hand_world,Joint{{1,1,1},{0,0,0,1},palm_local}).translation,mul(rotate(hand_world.rotation,local_normal),clearance));
    out.palm_error=length(sub(out.palm_position,s.point)); out.palm_contact=!out.clamped&&weight>=.999f&&out.palm_error<=.02f;
    for(unsigned i=0;i<5;++i)for(unsigned j=0;j<3;++j)out.fingers[i][j]=hand.fingers[i].joints[j].rotation;
    const auto lip=valid_lip(s,up);
    const auto finish=[&]() { return checked_pose(out,hand,hand_world,s,lip); };
    if(out.clamped||!lip)return finish();
    // Solve fingers against the fully reached hand, then blend local rotations
    // with the same 150 ms arm weight. Contact is still gated on the actual
    // blended palm. This also produces a smooth ordinary release.
    if(!full_reach.available||full_reach.clamped)return finish();
    auto full_upper=arm.upper,full_lower=arm.lower,full_hand=arm.hand;
    full_upper.rotation=full_reach.upper; full_lower.rotation=full_reach.lower;
    const auto full_lower_world=reach::compose(reach::compose(shoulder,full_upper),full_lower);
    full_hand.rotation=multiply(inverse(full_lower_world.rotation),desired_world);
    const auto full_hand_world=reach::compose(full_lower_world,full_hand);
    std::array<std::array<Quaternion,3>,5> closed{};
    for(unsigned i=0;i<5;++i) {
        const auto root=reach::compose(reach::compose(full_hand_world,hand.fingers[i].palm),hand.fingers[i].joints[0]);
        const auto plane_point=i==0?s.bottom_point:s.top_point;
        const auto plane_normal=i==0?s.bottom_normal:s.top_normal;
        auto target=add(plane_point,mul(s.tangent,dot(sub(root.translation,s.point),s.tangent)));
        target=sub(target,mul(plane_normal,dot(sub(target,plane_point),plane_normal)));
        target=add(target,mul(plane_normal,finger_clearance));
        const auto terminal=unit(sub(mul(s.normal,-1),mul(plane_normal,dot(mul(s.normal,-1),plane_normal))));
        bool found=false;
        // Shorter digits can pinch nearer the measured rear/top edge instead
        // of stretching to the deeper probe. Candidates stay on that same
        // verified plane and never cross behind the rear-face intersection.
        const auto depth=-dot(sub(target,s.point),s.normal);
        const auto attempt=[&](float fraction) {
            const auto candidate=sub(target,mul(terminal,std::max(0.f,depth)*(1-fraction)));
            if(!digit(hand.fingers[i],full_hand_world,candidate,plane_normal,s,terminal,closed[i],
                preferred?&preferred->pole[i]:nullptr,out.choices.pole[i],budget,repair_choices))return false;
            out.choices.depth[i]=fraction; return true;
        };
        if(preferred) {
            found=attempt(preferred->depth[i]);
            for(unsigned step=1;repair_choices&&step<=8&&!found&&budget.usable();++step) {
                const auto delta=step/64.f;
                if(preferred->depth[i]+delta<=1)found=attempt(preferred->depth[i]+delta);
                if(!found&&preferred->depth[i]-delta>=.25f)found=attempt(preferred->depth[i]-delta);
            }
        }
        if(!found&&(!preferred||repair_choices))for(const auto fraction:std::array<float,4>{1,.75f,.5f,.25f})if(attempt(fraction)) {found=true;break;}
        if(!found)return finish();
    }
    out.finger_pose_available=true; out.grip_valid=out.palm_contact;
    for(unsigned i=0;i<5;++i)for(unsigned j=0;j<3;++j)out.fingers[i][j]=blend(hand.fingers[i].joints[j].rotation,closed[i][j],weight);
    // Check the actual output on every path, including native digit fallbacks.
    return finish();
}
static Solution solve_default(const reach::Arm& arm,const Hand& hand,const Surface& surface,float weight,float prediction,SearchBudget& budget) noexcept {
    auto first=solve_at_clearance(arm,hand,surface,weight,prediction,0,budget);
    if(!first.available||first.clamped||!surface.wrap_valid||first.finger_pose_available)return first;
    // A wider measured edge may need a little additional hand clearance for
    // a short pinky to clear its top while the thumb clears its underside.
    // Choose the smallest safe proxy, bounded to 2 cm beyond the live roots;
    // each candidate still must pass every bone-plane and reach constraint.
    for(unsigned step=1;step<=10&&budget.usable();++step) {
        auto candidate=solve_at_clearance(arm,hand,surface,weight,prediction,.002f*step,budget);
        if(candidate.available&&candidate.finger_pose_available)return candidate;
    }
    return first;
}
Solution solve(const reach::Arm& arm,const Hand& hand,const Surface& surface,float weight,float prediction) noexcept {
    SearchBudget budget;return solve_default(arm,hand,surface,weight,prediction,budget);
}
Solution solve_continuous(const reach::Arm& arm,const Hand& hand,const Surface& surface,float weight,Continuity& memory,float prediction) noexcept {
    bool valid_memory=memory.have&&std::isfinite(memory.selected.extra_clearance)&&memory.selected.extra_clearance>=0&&memory.selected.extra_clearance<=.02f;
    for(unsigned i=0;i<5;++i)valid_memory=valid_memory&&std::isfinite(memory.selected.depth[i])&&memory.selected.depth[i]>=.25f&&memory.selected.depth[i]<=1&&
        std::isfinite(memory.selected.pole[i])&&memory.selected.pole[i]>=0&&memory.selected.pole[i]<=1;
    if(memory.have&&!valid_memory)memory={};
    const auto remember=[&](Solution result) {
        if(result.available&&result.visual_safe&&result.finger_pose_available) { memory.have=true;memory.selected=result.choices; }
        else if(!result.available)memory={};
        return result;
    };
    SearchBudget budget;
    if(!valid_memory)return remember(solve_default(arm,hand,surface,weight,prediction,budget));
    budget.repairing=true;
    auto first=solve_at_clearance(arm,hand,surface,weight,prediction,memory.selected.extra_clearance,budget,&memory.selected);
    if(!first.available) {
        budget.repairing=false;
        return remember(solve_default(arm,hand,surface,weight,prediction,budget));
    }
    if(first.clamped||!surface.wrap_valid)return remember(first);
    if(first.available&&first.visual_safe&&first.finger_pose_available)return remember(first);
    // Retain a safe clearance rather than repeatedly dropping to the lowest
    // discrete value. If it becomes unsafe, search the nearest 0.2 mm change.
    for(unsigned step=1;step<=8&&budget.usable();++step) {
        const auto delta=.0002f*step;
        for(const auto extra:std::array<float,2>{memory.selected.extra_clearance+delta,memory.selected.extra_clearance-delta}) {
            if(extra<0||extra>.02f)continue;
            auto candidate=solve_at_clearance(arm,hand,surface,weight,prediction,extra,budget,&memory.selected);
            if(candidate.available&&candidate.visual_safe&&candidate.finger_pose_available)return remember(candidate);
        }
    }
    if(budget.usable()) {
        auto candidate=solve_at_clearance(arm,hand,surface,weight,prediction,memory.selected.extra_clearance,budget,&memory.selected,true);
        if(candidate.available&&candidate.visual_safe&&candidate.finger_pose_available)return remember(candidate);
    }
    // Unreachable approach/plain-face fallback still passes the same actual
    // current-output checks. Numeric memory is retained for the same lease.
    budget.repairing=false;
    const auto fallback=solve_default(arm,hand,surface,weight,prediction,budget);
    return remember(fallback.available?fallback:first);
}
} // namespace car_grab::grip
