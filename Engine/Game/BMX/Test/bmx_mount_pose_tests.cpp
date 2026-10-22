#include "Engine/Game/BMX/bmx_mount_pose.h"
#include "Extension/BMX/bmx_wheel_surface.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace dingosdk::bmx;
namespace m=mount_pose;
using Vec=m::Vec;
void check(bool value,const char* message) {if(!value){std::cerr<<message<<'\n';std::exit(1);}}
Vec add(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
Vec sub(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
Vec mul(Vec a,double s) {for(auto& v:a)v=static_cast<float>(v*s);return a;}
double dot(Vec a,Vec b) {double d=0;for(unsigned i=0;i<3;++i)d+=double(a[i])*b[i];return d;}
double length(Vec v) {return std::sqrt(dot(v,v));}
struct Plane {
    m::Request request{{0,0,0},0,123,456,71,1.25};
    Vec normal{0,1,0};
    Plane(float pitch,float yaw,Vec origin={}) {
        request.position=origin;request.yaw=yaw;
        normal={std::sin(yaw)*std::sin(pitch),std::cos(pitch),std::cos(yaw)*std::sin(pitch)};
    }
    m::Sample sample(Vec point) const {return {true,true,true,point,normal,
        request.world,request.context,request.serial,request.simulation_time};}
    std::array<m::Sample,2> initial() const {
        std::array<m::Sample,2> out;
        for(unsigned i=0;i<2;++i) {
            const auto z=i==0?geometry::front_axle[2]:geometry::rear_axle[2];
            auto p=add(request.position,{z*std::sin(request.yaw),0,z*std::cos(request.yaw)});
            p[1]-=static_cast<float>((normal[0]*(p[0]-request.position[0])+normal[2]*(p[2]-request.position[2]))/normal[1]);
            out[i]=sample(p);
        }
        return out;
    }
    std::array<m::Sample,2> final(const m::Seed& seed) const {
        std::array<m::Sample,2> out;
        for(unsigned i=0;i<2;++i) {
            const auto hub=add(seed.root,chassis::rotate_unit(seed.orientation,i==0?geometry::front_axle:geometry::rear_axle));
            const double distance=dot(sub(hub,request.position),normal);
            out[i]=sample(sub(hub,mul(normal,distance)));
        }
        return out;
    }
};
template<class Raycast>
bool adapter_samples(const m::Request& request,Raycast&& ray,Vec root,m::Quat rotation,std::array<m::Sample,2>& out) {
    for(unsigned i=0;i<2;++i) {
        const auto hub=add(root,chassis::rotate_unit(rotation,geometry::wheel_hub(i==0)));
        wheel_surface::Settings settings;settings.radius=static_cast<float>(geometry::wheel_radius);
        const auto surface=wheel_surface::search(hub,chassis::rotate_unit(rotation,{0,1,0}),
            chassis::rotate_unit(rotation,{0,0,1}),chassis::rotate_unit(rotation,{1,0,0}),ray,settings);
        if(!surface.available||!surface.hit||surface.feature!=wheel_surface::Feature::plane) {
            std::cerr<<"adapter surface "<<i<<" available="<<surface.available<<" hit="<<surface.hit<<" issue="<<int(surface.issue)<<'\n';return false;
        }
        out[i]={true,true,true,surface.point,surface.normal,request.world,request.context,request.serial,request.simulation_time};
    }
    return true;
}
bool adapter_samples(const Plane& plane,Vec root,m::Quat rotation,std::array<m::Sample,2>& out) {
    const auto ray=[&](Vec from,Vec to) -> wheel_surface::RaySample {
        const auto delta=sub(to,from);const auto denominator=dot(delta,plane.normal);
        if(std::abs(denominator)<1e-8)return {true,false,{},0};
        const auto fraction=dot(sub(plane.request.position,from),plane.normal)/denominator;
        if(fraction<0||fraction>1)return {true,false,{},0};
        return {true,true,add(from,mul(delta,fraction)),static_cast<float>(fraction)};
    };
    return adapter_samples(plane.request,ray,root,rotation,out);
}
void full_query_adapter() {
    for(const Vec origin:{Vec{},Vec{627.1f,199.9f,1062.8f}})
        for(float yaw:{0.f,.7f,-2.2f})for(float pitch:{0.f,-.09f,.09f,-.179f,.179f}) {
        Plane plane(pitch,yaw,origin);const auto height_samples=plane.initial();
        const auto measured_pitch=-std::atan2(height_samples[0].point[1]-height_samples[1].point[1],
            static_cast<float>(geometry::wheelbase));
        const float sy=std::sin(yaw*.5f),cy=std::cos(yaw*.5f),sp=std::sin(measured_pitch*.5f),cp=std::cos(measured_pitch*.5f);
        const m::Quat initial_rotation{cy*sp,sy*cp,-sy*sp,cy*cp};
        auto query_root=origin;query_root[1]+=geometry::parked_root_clearance(measured_pitch);
        std::array<m::Sample,2> initial{},final{};
        check(adapter_samples(plane,query_root,initial_rotation,initial),"actual wheel_surface initial adapter measures flat and mild planes");
        const auto candidate=m::prepare(plane.request,initial);
        if(!candidate.prepared)std::cerr<<"prepare adapter yaw="<<yaw<<" pitch="<<pitch<<" issue="<<int(candidate.issue)<<'\n';
        check(candidate.prepared,"actual wheel_surface output feeds mount preparation without synthetic normals");
        check(adapter_samples(plane,candidate.root,candidate.orientation,final),"actual final-pose wheel_surface witnesses are available");
        const auto ready=m::verify_final(candidate,final);
        if(!ready.ready) {
            std::cerr<<"final adapter origin="<<origin[0]<<','<<origin[1]<<','<<origin[2]<<" yaw="<<yaw<<" pitch="<<pitch<<" issue="<<int(ready.issue)<<'\n';
            for(unsigned i=0;i<2;++i) {
                const auto hub=add(candidate.root,chassis::rotate_unit(candidate.orientation,geometry::wheel_hub(i==0)));
                const auto difference=sub(hub,final[i].point);const auto d=dot(difference,final[i].normal);
                std::cerr<<"wheel "<<i<<" gap="<<d-geometry::wheel_radius<<" tangent="<<length(sub(difference,mul(final[i].normal,d)))<<" normaldot="<<dot(candidate.up,final[i].normal)<<'\n';
            }
        }
        check(ready.ready,"full two-pass native-query adapter geometry authorizes an actual witnessed seed");
    }
    // At the exact physical angle limit, a native float fit can fall a few
    // microradians outside. Preserve the guard and permit only that explicit
    // conservative classification; do not add an angle tolerance to fix it.
    for(float yaw:{0.f,.7f,-2.2f})for(float pitch:{-.18f,.18f}) {
        Plane plane(pitch,yaw,{627.1f,199.9f,1062.8f});
        const float sy=std::sin(yaw*.5f),cy=std::cos(yaw*.5f),sp=std::sin(pitch*.5f),cp=std::cos(pitch*.5f);
        const m::Quat q{cy*sp,sy*cp,-sy*sp,cy*cp};
        auto root=plane.request.position;root[1]+=geometry::parked_root_clearance(pitch);
        std::array<m::Sample,2> initial{},final{};check(adapter_samples(plane,root,q,initial),"boundary plane remains an actual query hit");
        const auto candidate=m::prepare(plane.request,initial);
        if(!candidate.prepared) {check(candidate.issue==m::Issue::too_steep,"boundary float-fit rejection has an explicit normal-angle cause");continue;}
        check(adapter_samples(plane,candidate.root,candidate.orientation,final),"boundary final plane has actual witnesses");
        const auto ready=m::verify_final(candidate,final);
        check(ready.ready||ready.issue==m::Issue::too_steep,"exact boundary either verifies or conservatively rejects the actual measured angle");
    }
}
// Independent closed-form equality of two plane/tire equations. It does not
// call the production pitch bisection, quaternion or height helpers. Keep the
// original Run102 planes and also check their historical .34m geometry golden.
std::array<double,3> two_plane_oracle(const m::Request& request,const std::array<m::Sample,2>& faces,
    double radius,const std::array<Vec,2>& hubs) {
    std::array<std::array<double,3>,2> n{};
    for(unsigned i=0;i<2;++i){double norm=0;for(unsigned a=0;a<3;++a)norm+=double(faces[i].normal[a])*faces[i].normal[a];
        for(unsigned a=0;a<3;++a)n[i][a]=faces[i].normal[a]/std::sqrt(norm);}
    std::array<double,3> mean{};double norm=0;
    for(unsigned a=0;a<3;++a){mean[a]=n[0][a]+n[1][a];norm+=mean[a]*mean[a];}
    for(auto& value:mean)value/=std::sqrt(norm);
    const double sy=std::sin(request.yaw),cy=std::cos(request.yaw);
    const double roll=-std::asin(cy*mean[0]-sy*mean[2]),sr=std::sin(roll),cr=std::cos(roll);
    std::array<double,2> A{},B{},C{};
    for(unsigned i=0;i<2;++i){const double y=hubs[i][1],z=hubs[i][2];
        const double along=n[i][0]*sy+n[i][2]*cy,side=n[i][0]*cy-n[i][2]*sy;
        double plane=radius;for(unsigned a=0;a<3;++a)plane+=n[i][a]*(double(faces[i].point[a])-request.position[a]);
        A[i]=(n[i][1]*cr*y+along*z)/n[i][1];
        B[i]=(along*cr*y-n[i][1]*z)/n[i][1];
        C[i]=(plane+side*sr*y)/n[i][1];}
    const double a=A[1]-A[0],b=B[1]-B[0],c=C[0]-C[1];
    const double pitch=std::asin(std::clamp(-c/std::hypot(a,b),-1.,1.))-std::atan2(a,b);
    return {pitch,roll,double(request.position[1])+C[0]-A[0]*std::cos(pitch)-B[0]*std::sin(pitch)};
}
void measured_run102() {
    const m::Request request{{307.9456481933594f,274.361328125f,-474.8175048828125f},-2.2860631942749023f,
        9019075584ULL,1184859472ULL,1,0.01666666567325592};
    std::array<m::Sample,2> actual{{
        {true,true,true,{307.488525390625f,274.3475341796875f,-475.2116394042969f},
            {-.009094401262700558f,.9998197555541992f,-.016664039343595505f},request.world,request.context,1,request.simulation_time},
        {true,true,true,{308.3978271484375f,274.3751525878906f,-474.4203186035156f},
            {-.01092158630490303f,.9996914863586426f,-.02230840176343918f},request.world,request.context,1,request.simulation_time}}};
    const auto candidate=m::prepare(request,actual);
    check(candidate.prepared&&candidate.cross_plane_residual[0]>.0061&&candidate.cross_plane_allowance>.0096,
        "Run102 measured valid local faces no longer require one common infinite plane");
    const auto historical=two_plane_oracle(request,actual,.34,
        std::array<Vec,2>{Vec{0,.33993130346462175f,.6019780285252407f},Vec{0,.3399312125684516f,-.6019780285252407f}});
    check(std::abs(historical[0]-.02289772179)<.000002&&std::abs(historical[1]-.0081471981)<.000002&&
        std::abs(historical[2]-274.3614067056)<.00004,"unchanged Run102 planes retain their historical .34m source-geometry solution");
    const auto expected=two_plane_oracle(request,actual,geometry::wheel_radius,{geometry::front_axle,geometry::rear_axle});
    check(std::abs(candidate.pitch-expected[0])<.000002&&std::abs(candidate.roll-expected[1])<.000002,
        "Run102 unchanged local faces match independent closed-form equality at the selected geometry");
    check(std::abs(candidate.root[1]-expected[2])<.00004,"selected geometry root matches independently solved elevation within unchanged native float precision");
    for(unsigned i=0;i<2;++i) {
        const auto hub=add(candidate.root,chassis::rotate_unit(candidate.orientation,geometry::wheel_hub(i==0)));
        const auto distance=dot(sub(hub,actual[i].point),actual[i].normal);
        check(std::abs(distance-geometry::wheel_radius)<.00004,"both Run102 source wheels match their own actual support plane");
        actual[i].point=sub(hub,mul(actual[i].normal,distance)); // exact analytic plane witness oracle
    }
    check(m::verify_final(candidate,actual).ready,"independent actual final-plane witnesses accept Run102's solved seed");
}
void gently_distinct_query_faces() {
    for(const Vec origin:{Vec{},Vec{307.945648f,274.361328f,-474.817505f}})
        for(float yaw:{0.f,.7f,-2.2860632f})for(float direction:{-1.f,1.f}) {
        const Vec forward{std::sin(yaw),0,std::cos(yaw)},left{std::cos(yaw),0,-std::sin(yaw)};
        const std::array<double,2> grade{direction*.019,direction*.025};const double across=.008;
        const auto ray=[&](Vec from,Vec to) -> wheel_surface::RaySample {
            double selected=2;Vec selected_point{};const auto delta=sub(to,from);
            for(unsigned face=0;face<2;++face) {
                const Vec normal{-float(grade[face]*forward[0]+across*left[0]),1,-float(grade[face]*forward[2]+across*left[2])};
                const auto denominator=dot(delta,normal);if(std::abs(denominator)<1e-8)continue;
                const auto t=dot(sub(origin,from),normal)/denominator;if(t<0||t>1)continue;
                const auto p=add(from,mul(delta,t));const auto along=dot(sub(p,origin),forward);
                if((face==0&&along<0)||(face==1&&along>=0))continue;
                if(t<selected){selected=t;selected_point=p;}
            }
            return selected<=1?wheel_surface::RaySample{true,true,selected_point,float(selected)}:
                wheel_surface::RaySample{true,false,{},0};
        };
        auto position=origin;const double half=geometry::wheelbase*.5;
        const double front_y=half*grade[0],rear_y=-half*grade[1];
        position[1]+=static_cast<float>((front_y+rear_y)*.5);
        const m::Request request{position,yaw,123,456,71,1.25};
        const auto initial_pitch=-std::atan2(front_y-rear_y,geometry::wheelbase);
        const float sy=std::sin(yaw*.5f),cy=std::cos(yaw*.5f),sp=std::sin(float(initial_pitch)*.5f),cp=std::cos(float(initial_pitch)*.5f);
        const m::Quat q{cy*sp,sy*cp,-sy*sp,cy*cp};
        auto query_root=position;query_root[1]+=geometry::parked_root_clearance(float(initial_pitch));
        std::array<m::Sample,2> initial{},final{};
        check(adapter_samples(request,ray,query_root,q,initial),"actual distinct gentle faces yield fresh local plane fits");
        const auto candidate=m::prepare(request,initial);
        if(!candidate.prepared)std::cerr<<"distinct faces prepare issue="<<int(candidate.issue)<<'\n';
        check(candidate.prepared,"distinct gentle local planes produce a bounded two-contact candidate");
        check(adapter_samples(request,ray,candidate.root,candidate.orientation,final),"solved hubs get independent actual wheel_surface witnesses");
        const auto ready=m::verify_final(candidate,final);
        check(ready.ready,"actual two-pass gentle-terrain geometry accepts both directions and yaws without common-plane fiction");
    }
    // A true parallel 3mm step has no measured normal-change allowance.
    Plane flat(0,0);auto step=flat.initial();step[0].point[1]=.003f;
    const auto rejected=m::prepare(flat.request,step);
    check(!rejected.prepared&&rejected.issue==m::Issue::discontinuous&&
        std::abs(rejected.cross_plane_allowance-.0025)<1e-9,"parallel three-millimeter discontinuity retains the original strict rejection");
}
}
int main() {
    full_query_adapter();
    measured_run102();gently_distinct_query_faces();
    double worst_gap=0;
    for(const Vec origin:{Vec{},Vec{627.1f,199.9f,1062.8f}})
        for(float yaw:{0.f,.7f,-2.2f})for(float pitch:{-.18f,-.09f,0.f,.09f,.18f}) {
        Plane plane(pitch,yaw,origin);const auto seed=m::prepare(plane.request,plane.initial());
        // At the exact angle endpoint, quantized world-space sample points can
        // require a slightly steeper equality solution than their normals.
        // Retain the physical bound: only this explicit rejection is valid.
        if(!seed.prepared&&origin!=Vec{}&&std::abs(pitch)==m::maximum_slope) {
            check(seed.issue==m::Issue::too_steep,"quantized exact-limit support conservatively rejects a seed beyond the slope bound");
            continue;
        }
        check(seed.prepared&&!seed.ready&&seed.issue==m::Issue::needs_final_witness,
            "fresh initial plane prepares a candidate without authorizing native activation");
        const auto ready=m::verify_final(seed,plane.final(seed));
        check(ready.ready&&m::matches(ready,123,456,71,1.25),"actual final wheel witnesses authorize the same scoped seed");
        Vec root;check(chassis::root_from_com(ready.com,ready.orientation,geometry::com_from_root,root)&&
            length(sub(root,ready.root))<.0002,"full rotated COM maps back to exact prepared bike root");
        for(const auto hub:{geometry::front_axle,geometry::rear_axle}) {
            const auto actual=add(ready.root,chassis::rotate_unit(ready.orientation,hub));
            const auto gap=dot(sub(actual,origin),plane.normal)-geometry::wheel_radius;
            worst_gap=std::max(worst_gap,std::abs(gap));
            check(gap>=-.0002&&gap<=.0002,"both tires stay on actual mild support planes without initial penetration");
        }
        if(std::abs(pitch)>.17f&&origin==Vec{}) {
            const Vec yaw_hub{geometry::front_axle[2]*std::sin(yaw),geometry::front_axle[1],geometry::front_axle[2]*std::cos(yaw)};
            const auto front=add(ready.root,yaw_hub);
            const Vec yaw_rear{geometry::rear_axle[2]*std::sin(yaw),geometry::rear_axle[1],geometry::rear_axle[2]*std::cos(yaw)};
            const auto rear=add(ready.root,yaw_rear);
            check(std::min(dot(front,plane.normal),dot(rear,plane.normal))-geometry::wheel_radius<-.11*(geometry::wheel_radius/.34),
                "negative yaw-only control retains the source-scaled penetration counterexample; final support limits are unchanged");
        }
    }
    Plane flat(0,0);const auto base=flat.initial();
    for(unsigned fault=0;fault<10;++fault) {
        auto support=base;
        if(fault==0)support[0].available=false;
        if(fault==1)support[0].hit=false;
        if(fault==2)support[0].verified_plane=false;
        if(fault==3)support[0].world++;
        if(fault==4)support[0].context++;
        if(fault==5)support[0].serial++;
        if(fault==6)support[0].simulation_time+=.016;
        if(fault==7)support[0].normal={0,0,0};
        if(fault==8)support[0].normal[0]=std::numeric_limits<float>::quiet_NaN();
        if(fault==9)support[0].point[1]+=.003f;
        check(!m::prepare(flat.request,support).prepared,"unavailable, unsupported, stale and discontinuous initial support fail closed");
    }
    for(float pitch:{-.181f,.181f,-1.5707963f,1.5707963f}) {
        Plane steep(pitch,0);check(!m::prepare(steep.request,steep.initial()).prepared,"steep/wall normals cannot authorize a mild-plane seed");
    }
    const auto candidate=m::prepare(flat.request,base);
    for(unsigned fault=0;fault<7;++fault) {
        auto final=flat.final(candidate);
        if(fault==0)final[0].available=false;
        if(fault==1)final[0].hit=false;
        if(fault==2)final[0].verified_plane=false;
        if(fault==3)final[0].world++;
        if(fault==4)final[0].point[1]+=.01f;
        if(fault==5)final[0].point[0]+=.05f;
        if(fault==6)final[0].normal={1,0,0};
        check(!m::verify_final(candidate,final).ready,"a final clear lip, occluder, bad normal or stale witness cannot activate native physics");
    }
    auto malformed=candidate;malformed.orientation={0,0,0,0};
    check(!m::verify_final(malformed,flat.final(candidate)).ready,"invalid candidate quaternion cannot bypass final checks");
    const auto ready=m::verify_final(candidate,flat.final(candidate));
    check(!m::matches(ready,124,456,71,1.25)&&!m::matches(ready,123,457,71,1.25)&&
        !m::matches(ready,123,456,72,1.25)&&!m::matches(ready,123,456,71,1.26),"activation lease pins exact world context and clock");
    std::cout<<"Full mount seed: slope/COM/final actual witness and scope tests passed; maxgap="<<worst_gap<<"m.\n";
}
