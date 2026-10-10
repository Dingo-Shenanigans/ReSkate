#pragma once
#include "traffic_vehicle_surface.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::car_grab_native::surface_math {
using V = car_grab::Vec3;
using Q = std::array<float, 4>;
inline V add(V a, V b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline V sub(V a, V b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline V mul(V a, float b) noexcept { return {a.x*b, a.y*b, a.z*b}; }
inline float dot(V a, V b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline V cross(V a, V b) noexcept { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline bool finite(V a) noexcept {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
inline V unit(V a) noexcept {
    const auto n = dot(a, a);
    return finite(a) && std::isfinite(n) && n > 1e-10f ? mul(a, 1/std::sqrt(n)) : V{};
}
inline V rotate(V v, Q q) noexcept {
    const V u{q[0],q[1],q[2]};
    const auto t = mul(cross(u,v),2);
    return add(v,add(mul(t,q[3]),cross(u,t)));
}
struct Transform { V scale, position; Q rotation, inverse; };
inline bool transform(const std::array<float,12>& pose, Transform& out) noexcept {
    for (unsigned lane : {0U,1U,2U,4U,5U,6U,7U,8U,9U,10U})
        if (!std::isfinite(pose[lane])) return false;
    if (pose[0]<.0001f || pose[1]<.0001f || pose[2]<.0001f ||
        pose[0]>100 || pose[1]>100 || pose[2]>100 ||
        std::abs(pose[8])>100000 || std::abs(pose[9])>100000 || std::abs(pose[10])>100000)
        return false;
    const auto norm = pose[4]*pose[4]+pose[5]*pose[5]+pose[6]*pose[6]+pose[7]*pose[7];
    if (norm < .98f || norm > 1.02f) return false;
    const auto s = 1/std::sqrt(norm);
    out = {{pose[0],pose[1],pose[2]},{pose[8],pose[9],pose[10]},
        {pose[4]*s,pose[5]*s,pose[6]*s,pose[7]*s},
        {-pose[4]*s,-pose[5]*s,-pose[6]*s,pose[7]*s}};
    return true;
}
inline V local_point(V point, const Transform& t) noexcept {
    const auto p = rotate(sub(point,t.position),t.inverse);
    return {p.x/t.scale.x,p.y/t.scale.y,p.z/t.scale.z};
}
inline V world_point(V point, const Transform& t) noexcept {
    return add(t.position,rotate({point.x*t.scale.x,point.y*t.scale.y,point.z*t.scale.z},t.rotation));
}
inline V local_normal(V normal, const Transform& t) noexcept {
    const auto n = rotate(normal,t.inverse);
    return unit({n.x*t.scale.x,n.y*t.scale.y,n.z*t.scale.z});
}
inline V world_normal(V normal, const Transform& t) noexcept {
    return unit(rotate({normal.x/t.scale.x,normal.y/t.scale.y,normal.z/t.scale.z},t.rotation));
}
inline V local_tangent(V tangent, const Transform& t) noexcept {
    const auto v = rotate(tangent,t.inverse);
    return unit({v.x/t.scale.x,v.y/t.scale.y,v.z/t.scale.z});
}
inline V world_tangent(V tangent, const Transform& t) noexcept {
    return unit(rotate({tangent.x*t.scale.x,tangent.y*t.scale.y,tangent.z*t.scale.z},t.rotation));
}
// Four neighboring actual intersections define the local surface plane. Reject
// edges, sharp folds and degenerate data rather than manufacture a face normal.
inline bool footprint(V center, V left, V right, V down, V up, V rear,
                      V& normal, V& tangent) noexcept {
    if (!finite(center)||!finite(left)||!finite(right)||!finite(down)||!finite(up)) return false;
    rear = unit({rear.x,0,rear.z});
    if (dot(rear,rear)<.99f) return false;
    const auto dx = sub(right,left), dy = sub(up,down);
    normal = unit(cross(dx,dy));
    if (dot(normal,rear)<0) normal = mul(normal,-1);
    if (dot(normal,rear)<.5f || std::abs(normal.y)>.65f) return false;
    for (const auto p : {left,right,down,up}) {
        const auto d = sub(p,center);
        if (dot(d,d)>.0625f || std::abs(dot(d,normal))>.004f) return false;
    }
    tangent = unit(sub(dx,mul(normal,dot(dx,normal))));
    return dot(tangent,tangent)>.99f && std::abs(dot(normal,tangent))<.001f;
}
// Three measured points on each side of the same lip establish top and bottom
// planes. A compact collision cross-section is required for the verified rig's
// .066..085m finger bone-anchor lengths. The radius/depth is never fabricated.
inline bool wrap(V palm, V rear_normal, V top, V top_inside, V top_side,
                 V bottom, V bottom_inside, V bottom_side,
                 V& top_normal, V& bottom_normal) noexcept {
    for (const auto p : {palm,top,top_inside,top_side,bottom,bottom_inside,bottom_side})
        if (!finite(p)) return false;
    top_normal = unit(cross(sub(top_inside,top),sub(top_side,top)));
    if (top_normal.y<0) top_normal = mul(top_normal,-1);
    bottom_normal = unit(cross(sub(bottom_inside,bottom),sub(bottom_side,bottom)));
    if (bottom_normal.y>0) bottom_normal = mul(bottom_normal,-1);
    if (top_normal.y<.85f || bottom_normal.y>-.85f || dot(top_normal,bottom_normal)>-.8f)
        return false;
    const auto thickness = dot(sub(top,bottom),top_normal);
    if (thickness<.006f || thickness>.06f || top.y<palm.y || bottom.y>palm.y ||
        dot(sub(palm,top),top_normal)>.002f || dot(sub(palm,bottom),bottom_normal)>.002f ||
        std::abs(dot(top_normal,rear_normal))>.55f || std::abs(dot(bottom_normal,rear_normal))>.55f)
        return false;
    for (const auto p : {top,top_inside,top_side,bottom,bottom_inside,bottom_side}) {
        const auto d = sub(p,palm);
        if (dot(d,d)>.0064f || dot(d,rear_normal)>0.005f || dot(d,rear_normal)<-.045f)
            return false;
    }
    return true;
}
} // namespace dingosdk::car_grab_native::surface_math
