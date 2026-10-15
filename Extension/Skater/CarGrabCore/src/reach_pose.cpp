#include "car_grab/reach_pose.h"
#include <algorithm>
#include <cmath>
namespace car_grab::reach {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
float dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a,a)); }
Vec3 unit(Vec3 a) { const auto n=length(a); return n>1e-6f ? mul(a,1/n) : Vec3{1,0,0}; }
bool finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z)&&
    std::abs(a.x)<100000&&std::abs(a.y)<100000&&std::abs(a.z)<100000; }
Quaternion normalize(Quaternion q) {
    float norm=0; for(const auto f:q) norm+=f*f;
    if (norm<1e-12f||!std::isfinite(norm)) return {0,0,0,1};
    const auto inv=1/std::sqrt(norm); for(auto& f:q) f*=inv; return q;
}
Quaternion inverse(Quaternion q) { return {-q[0],-q[1],-q[2],q[3]}; }
Quaternion multiply(Quaternion a, Quaternion b) {
    return normalize({a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]});
}
Vec3 rotate(Quaternion q, Vec3 v) {
    const Vec3 axis{q[0],q[1],q[2]};
    const auto twice=mul(cross(axis,v),2);
    return add(v,add(mul(twice,q[3]),cross(axis,twice)));
}
Quaternion align(Vec3 from, Vec3 to) {
    from=unit(from); to=unit(to); const auto cosine=std::clamp(dot(from,to),-1.f,1.f);
    if(cosine<-.9999f) {
        const auto other=std::abs(from.x)<.7f ? Vec3{1,0,0}:Vec3{0,1,0};
        const auto axis=unit(cross(from,other)); return {axis.x,axis.y,axis.z,0};
    }
    const auto axis=cross(from,to); return normalize({axis.x,axis.y,axis.z,1+cosine});
}
Quaternion blend(Quaternion a, Quaternion b, float weight) {
    float similarity=0; for(unsigned i=0;i<4;++i) similarity+=a[i]*b[i];
    if(similarity<0) for(auto& f:b) f=-f;
    Quaternion out{}; for(unsigned i=0;i<4;++i) out[i]=a[i]+(b[i]-a[i])*weight;
    return normalize(out);
}
}
bool valid(const Joint& joint) noexcept {
    if(!finite(joint.scale)||!finite(joint.translation)) return false;
    if(joint.scale.x<.05f||joint.scale.x>20||joint.scale.y<.05f||joint.scale.y>20||joint.scale.z<.05f||joint.scale.z>20) return false;
    // Rotation followed by nonuniform ancestor scale requires a sheared matrix;
    // the evaluated skater rig normally has uniform unit scale. Fail closed.
    if(std::abs(joint.scale.x-joint.scale.y)>.01f*joint.scale.x||std::abs(joint.scale.x-joint.scale.z)>.01f*joint.scale.x) return false;
    float norm=0; for(const auto f:joint.rotation) { if(!std::isfinite(f)) return false; norm+=f*f; }
    return norm>.9f&&norm<1.1f;
}
Joint compose(const Joint& parent, const Joint& child) noexcept {
    const auto local=Vec3{child.translation.x*parent.scale.x,child.translation.y*parent.scale.y,child.translation.z*parent.scale.z};
    return {{parent.scale.x*child.scale.x,parent.scale.y*child.scale.y,parent.scale.z*child.scale.z},
        multiply(normalize(parent.rotation),normalize(child.rotation)),add(parent.translation,rotate(normalize(parent.rotation),local))};
}
Solution solve(const Arm& arm, Vec3 target, float weight) noexcept {
    Solution out;
    if(!finite(target)||!std::isfinite(weight)||!valid(arm.parent_world)||!valid(arm.shoulder)||!valid(arm.upper)||!valid(arm.lower)||!valid(arm.hand)) return out;
    weight=std::clamp(weight,0.f,1.f);
    const auto shoulder=compose(arm.parent_world,arm.shoulder);
    const auto upper=compose(shoulder,arm.upper), lower=compose(upper,arm.lower), hand=compose(lower,arm.hand);
    const auto first=sub(lower.translation,upper.translation), second=sub(hand.translation,lower.translation);
    const auto a=length(first), b=length(second);
    if(!std::isfinite(a)||!std::isfinite(b)||a<.1f||b<.1f||a>2.5f||b>2.5f) return out;
    out.available=true;
    if(weight==0) { out.upper=arm.upper.rotation; out.lower=arm.lower.rotation; out.hand_position=hand.translation; return out; }
    const auto toward=sub(target,upper.translation);
    const auto requested=length(toward);
    const auto direction=requested>1e-5f ? unit(toward):unit(sub(hand.translation,upper.translation));
    const auto minimum=std::abs(a-b)+.02f*std::min(a,b);
    const auto maximum=.98f*(a+b);
    const auto distance=std::clamp(requested,minimum,maximum);
    out.clamped=distance!=requested;
    const auto contact=add(upper.translation,mul(direction,distance));
    auto pole=sub(first,mul(direction,dot(first,direction)));
    if(length(pole)<1e-5f) {
        const auto axis=std::abs(direction.y)<.9f ? Vec3{0,-1,0}:Vec3{0,0,1};
        pole=sub(axis,mul(direction,dot(axis,direction)));
    }
    pole=unit(pole);
    const auto cosine=std::clamp((a*a+distance*distance-b*b)/(2*a*distance),-1.f,1.f);
    const auto sine=std::sqrt(std::max(0.f,1-cosine*cosine));
    const auto desired_first=add(mul(direction,a*cosine),mul(pole,a*sine));
    const auto desired_upper_world=multiply(align(first,desired_first),upper.rotation);
    const auto desired_upper=multiply(inverse(shoulder.rotation),desired_upper_world);
    auto solved_upper_local=arm.upper;
    solved_upper_local.rotation=desired_upper;
    const auto solved_upper=compose(shoulder,solved_upper_local);
    const auto solved_lower=compose(solved_upper,arm.lower), solved_hand=compose(solved_lower,arm.hand);
    const auto desired_lower_world=multiply(align(sub(solved_hand.translation,solved_lower.translation),sub(contact,solved_lower.translation)),solved_lower.rotation);
    const auto desired_lower=multiply(inverse(solved_upper.rotation),desired_lower_world);
    out.upper=blend(normalize(arm.upper.rotation),desired_upper,weight);
    out.lower=blend(normalize(arm.lower.rotation),desired_lower,weight);
    auto blended_upper=arm.upper, blended_lower=arm.lower;
    blended_upper.rotation=out.upper; blended_lower.rotation=out.lower;
    out.hand_position=compose(compose(compose(shoulder,blended_upper),blended_lower),arm.hand).translation;
    return out;
}
float blend_weight(float previous, bool active, double seconds) noexcept {
    if(!std::isfinite(previous)||!std::isfinite(seconds)||seconds<0) return 0;
    previous=std::clamp(previous,0.f,1.f);
    const float step=static_cast<float>(std::min(seconds,.15)/.15);
    return active ? std::min(1.f,previous+step):std::max(0.f,previous-step);
}
}
