#include "Extension/BMX/bmx_world_transform.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace dingosdk::bmx;
constexpr float half_pi = 1.5707963267948966f;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool near(float a, float b, float tolerance = .00001f) { return std::abs(a - b) < tolerance; }
std::array<float, 3> apply_point(const world::Matrix& matrix, std::array<float, 3> point) {
    return {matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
        matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
        matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14]};
}
void point_is(std::array<float, 3> actual, std::array<float, 3> expected, const char* message) {
    for (unsigned i = 0; i < 3; ++i) check(near(actual[i], expected[i]), message);
}
using Vec = std::array<float,3>;
// Raw independent source anchors below were measured at the original .34m
// tire normalization. Scale dimensional mesh points and the source pivot only;
// arbitrary vectors, quaternion probes and every tolerance remain unchanged.
Vec source_point(Vec p) {
    const float scale=static_cast<float>(geometry::wheel_radius/.34);
    for(auto& value:p)value*=scale;
    return p;
}
using Quat = std::array<float,4>;
Quat product(Quat a,Quat b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Quat axis(unsigned index,float angle) { Quat q{0,0,0,std::cos(angle/2)};q[index]=std::sin(angle/2);return q; }
// Independent explicit rotation-matrix oracle, not chassis::rotate_unit().
Vec reference_rotation(Quat q,Vec v) {
    float n=0;for(float a:q)n+=a*a;const float scale=2/n;
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    return {(1-scale*(y*y+z*z))*v[0]+scale*(x*y-z*w)*v[1]+scale*(x*z+y*w)*v[2],
        scale*(x*y+z*w)*v[0]+(1-scale*(x*x+z*z))*v[1]+scale*(y*z-x*w)*v[2],
        scale*(x*z-y*w)*v[0]+scale*(y*z+x*w)*v[1]+(1-scale*(x*x+y*y))*v[2]};
}
Vec source_steer(Vec p,float angle) {
    const Vec pivot=source_point({0,.960253801f,.368546119f});
    const Quat q{0,.960119591f*std::sin(angle/2),-.279589648f*std::sin(angle/2),std::cos(angle/2)};
    for(unsigned i=0;i<3;++i)p[i]-=pivot[i];
    p=reference_rotation(q,p);
    for(unsigned i=0;i<3;++i)p[i]+=pivot[i];
    return p;
}
void quaternion_chassis() {
    Vec root{7,8,9};
    const Quat cyclic{.5f,.5f,.5f,.5f};
    check(chassis::root_from_com(std::array<float,4>{11,22,33,std::numeric_limits<float>::quiet_NaN()},
        cyclic,{.2f,.8f,-.15f},root),"COM W metadata is ignored even when nonfinite");
    point_is(root,{11.15f,21.8f,32.2f},"COM offset rotates from local root to measured world COM");
    Vec point{};
    check(chassis::rotate(cyclic,{1,0,0},point),"canonical Hamilton quaternion accepted");
    point_is(point,{0,1,0},"Hamilton XYZW active rotation maps X to Y for cyclic quaternion");
    visual::Frame frame;frame.position=root;frame.steer=.4f;frame.barspin=-.2f;frame.tailwhip=.7f;
    frame.front_wheel_phase=.8f;frame.rear_wheel_phase=-.4f;frame.crank_phase=1.1f;
    std::array<world::Matrix,dingosdk::bmx::world::part_count> local{};
    auto unplaced=frame;unplaced.position={};
    check(world::transforms(unplaced,local),"local articulation reference accepted");
    for(const auto q:{cyclic,axis(0,2*half_pi),product(axis(1,.71f),product(axis(0,half_pi-.000001f),axis(2,-1.3f))),
            product(axis(0,2*half_pi),product(axis(2,1.43f),axis(1,-.82f)))}) {
        frame.chassis_quaternion_available=true;frame.chassis_quaternion=q;
        frame.yaw=frame.pitch=frame.roll=std::numeric_limits<float>::quiet_NaN();
        std::array<world::Matrix,dingosdk::bmx::world::part_count> actual{};
        check(world::transforms(frame,actual),"full quaternion overrides unused Euler angles through flips and near vertical pitch");
        for(unsigned part=0;part<actual.size();++part)for(const Vec probe:{Vec{},Vec{.12f,.403f,-.308f},Vec{-.27f,1.08f,.39f}}) {
            auto expected=reference_rotation(q,apply_point(local[part],probe));
            for(unsigned a=0;a<3;++a)expected[a]+=root[a];
            point_is(apply_point(actual[part],probe),expected,"all six articulated parts share full Hamilton chassis rotation");
        }
        for(auto& value:frame.chassis_quaternion)value=-value;
        std::array<world::Matrix,dingosdk::bmx::world::part_count> antipodal{};
        check(world::transforms(frame,antipodal),"negative quaternion accepted");
        for(unsigned part=0;part<actual.size();++part)for(unsigned i=0;i<16;++i)
            check(near(actual[part][i],antipodal[part][i]),"q and -q produce identical native transforms");
    }
    frame={};frame.yaw=.7f;frame.pitch=-.4f;frame.roll=1.2f;frame.steer=.3f;
    std::array<world::Matrix,dingosdk::bmx::world::part_count> legacy{},ignored{};
    check(world::transforms(frame,legacy),"legacy rotation accepted");
    frame.chassis_quaternion={std::numeric_limits<float>::quiet_NaN(),2,3,4};
    check(world::transforms(frame,ignored)&&legacy==ignored,"unavailable quaternion leaves legacy matrices byte-for-byte unchanged");
    frame.chassis_quaternion_available=true;
    const auto original=ignored;
    for(const Quat bad:{Quat{},Quat{0,0,0,2},Quat{0,0,0,1.01f},
            Quat{std::numeric_limits<float>::quiet_NaN(),0,0,1},Quat{0,0,std::numeric_limits<float>::infinity(),1}}) {
        frame.chassis_quaternion=bad;
        check(!world::transforms(frame,ignored)&&ignored==original,"invalid available quaternion rejects without partial matrix mutation");
        const auto old_root=root;
        check(!chassis::root_from_com(Vec{1,2,3},bad,Vec{0,.8f,0},root)&&root==old_root,
            "invalid quaternion cannot modify COM conversion output");
    }
    Vec old_root=root;
    check(!chassis::root_from_com(Vec{1,std::numeric_limits<float>::quiet_NaN(),3},cyclic,Vec{0,1,0},root)&&root==old_root,
        "nonfinite COM XYZ rejects without output mutation");
    check(!chassis::root_from_com(Vec{1,2,3},cyclic,Vec{0,std::numeric_limits<float>::infinity(),0},root)&&root==old_root,
        "nonfinite local COM offset rejects without output mutation");
    frame={};frame.chassis_quaternion_available=true;frame.chassis_quaternion={0,0,0,1.00001f};
    check(world::transforms(frame,ignored),"native unit quaternion roundoff accepted");
    for(const auto& matrix:ignored)check(matrix[0]==1&&matrix[5]==1&&matrix[10]==1,"accepted roundoff is normalized without mesh scale");
}
} // namespace
int main() {
    visual::Frame frame;
    std::array<world::Matrix, dingosdk::bmx::world::part_count> matrices{};
    frame.position = {627.1f, 199.9f, 1062.8f};
    check(world::transforms(frame, matrices), "neutral frame accepted");
    for (const auto& matrix : matrices) {
        check(matrix[0] == 1 && matrix[5] == 1 && matrix[10] == 1 && matrix[15] == 1, "neutral basis identity");
        point_is(apply_point(matrix, {}), frame.position, "native world translation");
    }
    frame = {};
    frame.rear_wheel_phase = half_pi;
    check(world::transforms(frame, matrices), "wheel frame accepted");
    point_is(apply_point(matrices[2], source_point({0,.339931213f,-.601978029f})), source_point({0,.339931213f,-.601978029f}), "source rear hub fixed during rotation");
    point_is(apply_point(matrices[2], source_point({0,.639931213f,-.601978029f})), source_point({0,.339931213f,-.301978029f}), "rear wheel rotates around source axle");
    point_is(apply_point(matrices[3], source_point({0,.639931303f,.601978029f})), source_point({0,.639931303f,.601978029f}), "front wheel independent");
    frame = {};
    frame.steer = half_pi;
    frame.barspin = half_pi;
    check(world::transforms(frame, matrices), "steering frame accepted");
    point_is(apply_point(matrices[1], source_point({0,.960253801f,.368546119f})), source_point({0,.960253801f,.368546119f}), "source steerer pivot fixed");
    point_is(apply_point(matrices[1], source_point({0,.339931303f,.601978029f})), source_steer(source_point({0,.339931303f,.601978029f}),half_pi), "fork follows tilted source steering axis");
    point_is(apply_point(matrices[5], source_point({.334575045f,1.231948059f,.340623945f})), source_steer(source_point({.334575045f,1.231948059f,.340623945f}),2*half_pi), "barspin adds only to handlebars around tilted source axis");
    frame = {};
    frame.crank_phase = half_pi;
    check(world::transforms(frame, matrices), "crank frame accepted");
    point_is(apply_point(matrices[4], source_point({0,.400918115f,-.136770590f})), source_point({0,.400918115f,-.136770590f}), "source frame bottom bracket fixed");
    point_is(apply_point(matrices[4], source_point({.1f,.400918115f,.103790928f})), source_point({.1f,.160356597f,-.136770590f}), "crank arm quarter turn retains source radius");
    // Independent source spindle-to-BB offsets: +X starts low/forward.
    point_is(apply_point(matrices[6], source_point({.139242194f,.220121164f,.011273204f})),
        source_point({.139242194f,.261736754f,-.326377252f}), "left platform translates with source spindle");
    point_is(apply_point(matrices[7], source_point({-.139242213f,.599275463f,-.284814701f})),
        source_point({-.139242213f,.557659485f,.052835735f}), "right platform translates with source spindle");
    for(unsigned part:{6u,7u})check(std::abs(matrices[part][4])<1e-6f&&std::abs(matrices[part][5]-1)<1e-6f&&std::abs(matrices[part][6])<1e-6f,
        "separate platform normals remain upright through crank rotation");
    frame = {};
    frame.tailwhip = half_pi;
    check(world::transforms(frame, matrices), "tailwhip accepted");
    point_is(apply_point(matrices[0], source_point({0,.339931213f,-.601978029f})), source_steer(source_point({0,.339931213f,-.601978029f}),half_pi), "rear frame whips around source steerer");
    point_is(apply_point(matrices[1], source_point({0,.339931303f,.601978029f})), source_point({0,.339931303f,.601978029f}), "front assembly stays put during whip");
    frame = {};
    frame.yaw = half_pi;
    check(world::transforms(frame, matrices), "yaw accepted");
    point_is(apply_point(matrices[0], {0, 0, 1}), {1, 0, 0}, "native positive yaw turns forward toward positive X");
    frame.position = {900000, 999000, -999000};
    frame.roll = .12f; frame.pitch = .32f; frame.steer = .41f; frame.crank_phase = .73f;
    frame.front_wheel_phase = 5.2f; frame.rear_wheel_phase = 1.8f; frame.tailwhip = .36f; frame.barspin = 1.1f;
    check(world::transforms(frame, matrices), "far world coordinates accepted");
    for (const auto& matrix : matrices) {
        for (unsigned a = 0; a < 3; ++a) {
            float length{};
            for (unsigned r = 0; r < 3; ++r) length += matrix[a * 4 + r] * matrix[a * 4 + r];
            check(near(length, 1), "articulated basis stays unit at large coordinates");
            for (unsigned b = a + 1; b < 3; ++b) {
                float dot{};
                for (unsigned r = 0; r < 3; ++r) dot += matrix[a * 4 + r] * matrix[b * 4 + r];
                check(near(dot, 0), "articulated basis stays orthogonal");
            }
        }
    }
    frame.yaw = std::numeric_limits<float>::quiet_NaN();
    check(!world::transforms(frame, matrices), "NaN pose rejected");
    frame = {}; frame.position[1] = 1e9f;
    check(!world::transforms(frame, matrices), "unbounded world pose rejected");
    quaternion_chassis();
    std::cout << "BMX native world transform tests passed\n";
}
