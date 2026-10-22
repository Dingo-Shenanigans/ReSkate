#include "Engine/Game/BMX/bmx_geometry.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include "Extension/BMX/bmx_world_transform.h"
#include "Extension/BMX/bmx_collision_envelope.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
using namespace dingosdk::bmx;
using Vec=geometry::Vec;
void check(bool good,const char* text) {if(!good){std::cerr<<text<<'\n';std::exit(1);}}
double distance(Vec a,Vec b) {double sum=0;for(unsigned i=0;i<3;++i)sum+=double(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(sum);}
Vec point(const world::Matrix& m,Vec p) {return {
    m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],
    m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]};}
// Independent Rodrigues oracle from the measured manifest line, not the helper.
Vec steer(Vec p,double angle) {
    const double factor=geometry::wheel_radius/.34;
    const double py=.9602538008020781*factor,pz=.3685461188163388*factor;
    constexpr double ny=.9601195908265842,nz=-.279589648075517;
    const double x=p[0],y=p[1]-py,z=p[2]-pz,c=std::cos(angle),s=std::sin(angle),d=ny*y+nz*z;
    return {float(x*c+(ny*z-nz*y)*s),float(py+y*c+nz*x*s+ny*d*(1-c)),
        float(pz+z*c-ny*x*s+nz*d*(1-c))};
}
}
int main() {
    using namespace dingosdk::bmx;
    const double factor=geometry::wheel_radius/.34;
    const Vec source_front{0,float(.339931303*factor),float(.601978029*factor)};
    check(std::abs(geometry::wheelbase-1.203956057050485*factor)<1e-12,"source wheelbase preserves uniform scale");
    check(std::abs(geometry::source_uniform_scale*.8400284819674668-geometry::wheel_radius)<1e-12,"normalization uses source maximum tire radius");
    const Vec raw_rear{0,-.858041636f,-1.471101935f};Vec normalized{};
    for(unsigned i=0;i<3;++i)normalized[i]=float((raw_rear[i]-geometry::source_origin[i])*geometry::source_uniform_scale);
    check(distance(normalized,geometry::rear_axle)<1e-7,"canonical rear anchor follows the declared source transform");
    check(geometry::left_pedal.contact[1]<geometry::bottom_bracket[1]&&
        geometry::left_pedal.contact[2]>geometry::bottom_bracket[2],"source positive-X platform starts low and forward");
    check(geometry::right_pedal.contact[1]>geometry::bottom_bracket[1]&&
        geometry::right_pedal.contact[2]<geometry::bottom_bracket[2],"source negative-X platform starts high and rearward");
    double maximum_caster=0;
    for(int step=-40;step<=40;++step) {
        const float angle=step*.01f;const auto hub=geometry::wheel_hub(true,angle);
        check(distance(hub,steer(source_front,angle))<1e-7,"front hub follows exact fitted source steerer line");
        maximum_caster=std::max(maximum_caster,distance(hub,geometry::front_axle));
        visual::Frame frame;frame.steer=angle;std::array<world::Matrix,world::part_count> matrices;
        check(world::transforms(frame,matrices),"eight source native parts produce transforms");
        check(distance(point(matrices[3],source_front),hub)<2e-7,
            "measured-step wheel query hub and rendered hub share source caster geometry");
        const auto next_hub=geometry::wheel_hub(true,-angle);
        if(std::abs(angle)>.1f)check(distance(hub,next_hub)>.01*factor,
            "opposite next-step command must not replace the observed-step query deflection");
    }
    check(maximum_caster>.0201*factor&&maximum_caster<.0202*factor&&maximum_caster<.03,
        "actual maximum physical steering orbit stays inside retained 30mm backup recess");
    for(int step=0;step<=64;++step) {
        const float phase=float(step*6.283185307179586/64);
        visual::Frame frame;frame.crank_phase=phase;std::array<world::Matrix,world::part_count> matrices;
        check(world::transforms(frame,matrices),"all crank phases produce eight-part transforms");
        for(bool left:{false,true}) {
            const auto& source=geometry::pedal(left);const auto& matrix=matrices[left?6:7];
            check(distance(point(matrix,source.contact),geometry::pedal_point(left,phase))<2e-7,
                "rider contact and native platform agree at every phase");
            check(std::abs(matrix[4])<1e-6f&&std::abs(matrix[5]-1)<1e-6f&&std::abs(matrix[6])<1e-6f,
                "independent source platforms self-level without inverted shoe rotation");
            check(distance(point(matrix,source.spindle),geometry::crank_point(source.spindle,phase))<2e-7,
                "platform spindle stays attached to the rotating crank arm");
        }
    }
    check(world::part_count==8&&world::blueprints[6]=="reskate/bmx/left_pedal"&&
        world::blueprints[7]=="reskate/bmx/right_pedal","native eight-part order retains original six and appends source platforms");
    check(chassis_control::proposed_mass==106&&chassis_control::proposed_inertia==chassis_control::Vec{26,32,10}&&
        geometry::com_from_root==Vec{0,.9f,-.05f},"geometry adaptation leaves mass inertia and chosen COM unchanged");
    check(chassis_control::Tuning{}.wheelbase==geometry::wheelbase&&chassis_control::proposed_wheels()[0].radius==geometry::wheel_radius,
        "portable steering and tire geometry share the declared normalization target");
    for(unsigned i=0;i<2;++i) {
        const auto hub=geometry::wheel_hub(i==0);const auto& backup=collision_envelope::capsules[i];
        check(std::abs((backup.a[1]+backup.b[1])*.5f-hub[1])<1e-7&&backup.a[2]==hub[2]&&backup.b[2]==hub[2],
            "native backup capsules share the actual source wheel centers");
        const double extent=backup.radius+.5*distance(backup.a,backup.b);
        check(std::abs(geometry::wheel_radius-extent-.03)<1e-7,
            "native nonrolling backup retains exactly 30mm outer tire recess at either model size");
        check(maximum_caster+extent<geometry::wheel_radius,
            "full measured steering orbit plus backup extent remains inside the logical tire");
    }
    for(float pitch:{-.18f,-.09f,0.f,.09f,.18f}) {
        visual::Frame placed;placed.pitch=pitch;placed.position[1]=geometry::parked_root_clearance(pitch);
        std::array<world::Matrix,world::part_count> matrices;check(world::transforms(placed,matrices),"slope placement accepted");
        // Independent exact measured plane: normal=(0,cos(pitch),sin(pitch)).
        // Its floor goes through the bike-root horizontal origin.
        for(const auto hub:{geometry::front_axle,geometry::rear_axle}) {
            const auto actual=point(matrices[0],hub);
            const double normal_distance=actual[1]*std::cos(double(pitch))+actual[2]*std::sin(double(pitch));
            check(std::abs(normal_distance-geometry::wheel_radius)<2e-7,
                "setdown puts both source tires on the sampled mild plane without a false support gap");
        }
        if(std::abs(pitch)>.17f) {
            const double axle_y=(double(geometry::front_axle[1])+geometry::rear_axle[1])*.5;
            const double old_height=geometry::wheel_radius-axle_y*std::cos(double(pitch));
            check(old_height*std::cos(double(pitch))+axle_y-geometry::wheel_radius>.005*factor,
                "negative control exposes the source-scaled horizontal-floor formula gap");
        }
    }
    std::cout<<"BMX source geometry, eight-part pedal leveling and steering staging tests passed; max caster="<<maximum_caster<<" m.\n";
}
