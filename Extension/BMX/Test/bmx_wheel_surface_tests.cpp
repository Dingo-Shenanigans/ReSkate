#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#include "Extension/BMX/bmx_wheel_surface.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
namespace w=dingosdk::bmx::wheel_surface;
using V=w::Vec;
void check(bool value,const char* why){if(!value){std::cerr<<why<<'\n';std::exit(1);}}
double distance(V a,V b){double value=0;for(unsigned i=0;i<3;++i)value+=double(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(value);}
struct Scene {
    double slope{.4},minimum_z{0},maximum_z{2.5},floor_y{0},yaw{};
    V translation{};
    bool floor{true},ramp{true};unsigned calls{};
    V direction(V p) const {return {static_cast<float>(std::cos(yaw)*p[0]+std::sin(yaw)*p[2]),p[1],static_cast<float>(-std::sin(yaw)*p[0]+std::cos(yaw)*p[2])};}
    V world(V p) const {return {static_cast<float>(std::cos(yaw)*p[0]+std::sin(yaw)*p[2]+translation[0]),p[1]+translation[1],static_cast<float>(-std::sin(yaw)*p[0]+std::cos(yaw)*p[2]+translation[2])};}
    V local(V p) const {for(unsigned i=0;i<3;++i)p[i]-=translation[i];return {static_cast<float>(std::cos(yaw)*p[0]-std::sin(yaw)*p[2]),p[1],static_cast<float>(std::sin(yaw)*p[0]+std::cos(yaw)*p[2])};}
    w::RaySample operator()(V from,V to) {
        ++calls;from=local(from);to=local(to);double best=2;V point{};
        const auto plane=[&](double m,double height,bool bounded) {
            const double denominator=double(to[1])-from[1]-m*(double(to[2])-from[2]);
            if(std::abs(denominator)<1e-12)return;
            const double fraction=(height+m*from[2]-from[1])/denominator;
            if(fraction<0||fraction>1||fraction>=best)return;
            V hit;for(unsigned i=0;i<3;++i)hit[i]=static_cast<float>(from[i]+fraction*(double(to[i])-from[i]));
            if(bounded&&(hit[2]<minimum_z-1e-7||hit[2]>maximum_z+1e-7||std::abs(hit[0])>1))return;
            best=fraction;point=hit;
        };
        if(floor)plane(0,floor_y,false);
        if(ramp)plane(slope,0,true);
        return {true,best<=1,world(point),best<=1?static_cast<float>(best):0};
    }
};
w::Result search(Scene& scene,V local_hub={0,.34f,-.06f},bool fakie=false) {
    return w::search(scene.world(local_hub),{0,1,0},scene.direction(fakie?V{0,0,-1}:V{0,0,1}),
        scene.direction(fakie?V{-1,0,0}:V{1,0,0}),[&](V a,V b){return scene(a,b);});
}
void floor_to_ramp_toe_and_fakie() {
    for(double yaw:{0.,1.2,-2.3})for(bool reverse:{false,true}) {
        Scene scene;scene.yaw=yaw;
        const auto hub=scene.world({0,.34f,-.06f});
        auto center=scene(scene.world({0,.49f,-.06f}),scene.world({0,-.36f,-.06f}));
        check(center.hit&&std::abs(center.point[1])<1e-7,"central hub-down ray still sees the flat floor before the ramp toe");
        const auto r=search(scene,{0,.34f,-.06f},reverse);
        const auto divisor=std::sqrt(1+scene.slope*scene.slope);
        const V expected=scene.world({0,static_cast<float>(1/divisor),static_cast<float>(-scene.slope/divisor)});
        check(r.available&&r.hit&&r.verified>=2&&r.rays<=18,"bounded adjacent patches find actual verified toe contact");
        check(distance(r.normal,expected)<2e-5&&scene.local(r.point)[2]>0,"selected normal and witness belong to the real uphill face ahead of the hub");
        const auto expected_gap=(double(hub[1])+.4*.06)/divisor-.34;
        check(std::abs(r.separation-expected_gap)<2e-6&&r.separation<0,"closest actual ramp gap wins over the central flat floor gap");
        check(std::abs(scene.local(r.point)[1]-.4*scene.local(r.point)[2])<2e-6,"result lever point is an actual native-style witness on the finite ramp plane");
    }
    Scene before;const auto flat=search(before,{0,.34f,-.3f});
    check(flat.available&&flat.hit&&distance(flat.normal,{0,1,0})<1e-6,"farther before the toe retains ordinary flat-floor support");
}
void steep_toe_coverage_is_geometry_bounded() {
    constexpr double pi=std::numbers::pi;
    for(double angle:{60.,70.,75.,80.})for(double yaw:{0.,1.2,-2.3})for(bool fakie:{false,true}) {
        Scene scene;scene.slope=std::tan(angle*pi/180);scene.yaw=yaw;
        // Exact first-touch position of a .34m disk still on the flat floor.
        // At75deg the lead is .261m, beyond the old fixed .24m candidates.
        const auto lead=.34*std::tan(angle*pi/360);
        const V hub{0,.34f,static_cast<float>(-lead+.0001)};
        const auto r=search(scene,hub,fakie);
        const auto divisor=std::sqrt(1+scene.slope*scene.slope);
        const V expected=scene.world({0,static_cast<float>(1/divisor),static_cast<float>(-scene.slope/divisor)});
        check(r.available&&r.hit&&r.rays<=18,"steep toe keeps bounded verified search in forward and fakie orientations");
        check(distance(r.normal,expected)<3e-5&&scene.local(r.point)[2]>0,
            "actual steep-plane normal is recovered from measured points rather than the hub radial direction");
        check(std::abs(scene.local(r.point)[1]-scene.slope*scene.local(r.point)[2])<3e-6&&r.separation<0,
            "steep toe has an actual finite-ramp witness and chooses its closest signed gap");
    }
    for(double angle:{80.01,85.}) {
        Scene wall;wall.slope=std::tan(angle*pi/180);
        const V hub{0,.34f,static_cast<float>(-.34*std::tan(angle*pi/360)+.0001)};
        const auto r=search(wall,hub);
        check(r.available&&r.hit&&distance(r.normal,{0,1,0})<1e-5,
            "faces beyond the declared80deg limit cannot be relabeled as wheel support; actual floor remains valid");
    }
}
void lips_and_missing_geometry_do_not_create_support() {
    Scene cut;cut.slope=1;cut.minimum_z=-3;cut.maximum_z=.05;cut.floor=false;
    const auto lip=search(cut,{0,static_cast<float>(.34*std::sqrt(2.)),0});
    check(lip.available&&(!lip.hit||lip.separation>.03f),"a disk beyond the finite endpoint radius has no physical support despite the extrapolated plane");
    cut.maximum_z=-.4;
    const auto unverified_depth=search(cut,{0,.2f,0});
    check(unverified_depth.available&&!unverified_depth.hit,"extrapolated deep plane with a distant real edge does not trigger false penetration failure");
    Scene clear;const auto air=search(clear,{0,3,0});
    check(air.available&&!air.hit&&air.rays==3&&clear.calls==3,"clear air uses only the three candidate center rays");
    Scene floor;floor.ramp=false;const auto ordinary=search(floor,{0,.34f,0});
    check(ordinary.available&&ordinary.hit&&ordinary.rays==18&&std::abs(ordinary.separation)<1e-6,"flat support stays exact within the fixed maximum query budget");
    check(ordinary.balance_normal_available&&ordinary.balance_normal==ordinary.normal,
        "ordinary plane balance uses the same actually fitted normal without another query");
    const auto deep=search(floor,{0,.15f,0});
    check(!deep.available&&!deep.hit&&deep.issue==w::Issue::penetration_limit,"actual witnessed penetration beyond ten centimeters fails closed");
}
void finite_lip_edges_have_actual_radial_witnesses() {
    constexpr double radius=.34;
    for(double yaw:{0.,1.2,-2.3})for(bool fakie:{false,true})for(bool floor:{false,true}) {
        Scene scene;scene.yaw=yaw;scene.floor=floor;
        const V hub{0,1.32f,2.6f};
        const auto r=search(scene,hub,fakie);
        const V endpoint=scene.world({0,1,2.5f});
        const auto expected_distance=std::sqrt(.1*.1+.32*.32);
        const V expected_normal=scene.world({0,static_cast<float>(.32/expected_distance),static_cast<float>(.1/expected_distance)});
        check(r.available&&r.hit&&r.feature==w::Feature::edge&&r.rays<=28,
            "finite kicker lip remains a real circular-wheel edge contact after its face-normal projection leaves the mesh");
        check(r.edge_uncertainty>0&&r.edge_uncertainty<=w::maximum_edge_uncertainty&&distance(r.point,endpoint)<.002,
            "edge lever comes from a fresh actual hit inside a declared two-millimeter boundary bracket");
        const auto& diagnostic=r.edge_diagnostic;
        check(diagnostic.candidate&&diagnostic.bounded&&diagnostic.bisections==9&&diagnostic.uncertainty_valid&&
              diagnostic.projection_beyond&&diagnostic.radial_valid&&diagnostic.witness_attempted&&diagnostic.witness_hit&&
              diagnostic.witness_near_boundary&&diagnostic.witness_on_plane&&diagnostic.witness_normal_valid&&diagnostic.verified,
            "diagnostics describe every existing successful finite-edge gate without authorizing an extra contact");
        check(diagnostic.witness_point==r.point&&diagnostic.uncertainty_m==r.edge_uncertainty&&
              diagnostic.boundary_error_m<=diagnostic.boundary_allowance_m&&diagnostic.plane_error_m<=.0025f,
            "reported witness coordinates and bounded errors are the actual accepted edge observations");
        check(diagnostic.original_witness_hit&&!diagnostic.retry_attempted,
            "an already valid original radial hit does not add a query or change its contact geometry");
        check(distance(r.normal,expected_normal)<.006&&std::abs(r.separation-(expected_distance-radius))<.002,
            "edge normal and gap match the analytic circle-to-finite-endpoint geometry in yaw and fakie");
        const auto divisor=std::sqrt(1+scene.slope*scene.slope);
        const V face=scene.direction({0,static_cast<float>(1/divisor),static_cast<float>(-scene.slope/divisor)});
        check(r.balance_normal_available&&distance(r.balance_normal,face)<3e-5&&distance(r.balance_normal,r.normal)>.4,
            "finite lip retains its independently fitted riding face; radial collision direction is not a balance plane");
        check(r.separation<0&&scene.local(r.point)[2]<=2.50001f&&std::abs(scene.local(r.point)[1]-.4*scene.local(r.point)[2])<3e-6,
            "witness lies on the measured finite surface; a lower floor does not bridge the missing face");
        const auto distant=search(scene,{0,1.34f,3.10f},fakie);
        check(distant.available&&!distant.hit,"truly clear finite lip beyond radius plus query reach stays clear");
    }
    Scene cut;cut.slope=1;cut.minimum_z=-3;cut.maximum_z=.05;cut.floor=false;
    const auto actual_depth=search(cut,{0,.22f,0});
    check(!actual_depth.available&&!actual_depth.hit&&actual_depth.issue==w::Issue::penetration_limit,
        "freshly witnessed finite endpoint penetration fails rather than hiding behind an unverified infinite plane");
    Scene floor_and_edge;floor_and_edge.slope=.1;floor_and_edge.maximum_z=2;
    const auto deep_after_floor=search(floor_and_edge,{0,.34f,2.1f});
    check(!deep_after_floor.available&&!deep_after_floor.hit&&deep_after_floor.issue==w::Issue::penetration_limit,
        "a valid flat-floor candidate cannot conceal a later actual deep edge penetration");
}
void edge_refinement_does_not_relabel_ambiguous_or_failed_queries() {
    for(unsigned failure:{0u,1u,2u}) {
        Scene scene;scene.slope=.05;scene.maximum_z=1;unsigned calls=0;
        const auto broken=[&](V from,V to) {
            auto sample=scene(from,to);++calls;
            if(calls==20) {
                if(failure==0)sample.available=false;
                if(failure==1)sample.point[0]+=.1f;
                if(failure==2)throw std::runtime_error("boundary query failure");
            }
            return sample;
        };
        const auto r=w::search({0,.34f,1.25f},{0,1,0},{0,0,1},{1,0,0},broken);
        check(calls==20&&!r.available&&!r.hit,"a failed boundary query discards an earlier valid floor candidate");
    }
    // A sloping infinite face can leave a short down-ray's vertical range.
    // Such a clear ray is NOT evidence that the face has a finite edge.
    Scene infinite;infinite.slope=std::tan(75*std::numbers::pi/180);infinite.minimum_z=-100;infinite.maximum_z=100;infinite.floor=false;
    const auto r=search(infinite,{0,.34f,0});
    check(r.available&&r.feature!=w::Feature::edge,"a plane outside the down-ray segment cannot manufacture an edge boundary");
}
void edge_diagnostics_distinguish_native_clear_and_rejected_witness() {
    for(bool misplaced:{false,true}) {
        Scene scene;scene.floor=false;unsigned calls=0;bool final_witness=false;
        const auto query=[&](V from,V to) {
            auto sample=scene(from,to);++calls;
            if(calls>=18&&std::abs(to[2]-from[2])>.01f) {
                final_witness=true;
                if(!misplaced) {sample.hit=false;sample.point={};sample.fraction=0;}
                else {
                    sample.fraction+=.01f;
                    for(unsigned i=0;i<3;++i)sample.point[i]=from[i]+(to[i]-from[i])*sample.fraction;
                }
            }
            return sample;
        };
        const auto r=w::search({0,1.32f,2.6f},{0,1,0},{0,0,1},{1,0,0},query);
        const auto& diagnostic=r.edge_diagnostic;
        check(final_witness&&r.available&&!r.hit&&r.rays==(misplaced?18u:20u)&&diagnostic.candidate&&diagnostic.bounded&&
              diagnostic.uncertainty_valid&&diagnostic.projection_beyond&&diagnostic.radial_valid&&
              diagnostic.witness_attempted&&!diagnostic.verified,
            "diagnostic recording retains the same bounded rays and rejects an unverified contact");
        if(!misplaced)check(!diagnostic.original_witness_hit&&diagnostic.retry_attempted&&diagnostic.retry_radial_attempted&&
                           !diagnostic.witness_hit&&!diagnostic.witness_near_boundary&&!diagnostic.witness_on_plane,
            "a genuinely clear native radial witness is distinguished from a hit rejected by geometric checks");
        else check(diagnostic.original_witness_hit&&!diagnostic.retry_attempted&&diagnostic.witness_hit&&!diagnostic.witness_near_boundary&&!diagnostic.witness_on_plane&&
                   diagnostic.boundary_error_m>diagnostic.boundary_allowance_m&&diagnostic.plane_error_m>.0025f,
            "a segment-valid hit away from the measured boundary reports the exact rejecting errors");
    }
}
void clear_boundary_query_requires_an_actual_interior_rewitness() {
    for(double yaw:{0.,1.2,-2.3})for(bool fakie:{false,true})for(V translation:{V{},V{300.7903f,274.3027f,-480.685f}}) {
        Scene scene;scene.floor=false;scene.yaw=yaw;scene.translation=translation;unsigned native_misses=0,query_calls=0;
        const auto query=[&](V from,V to) {
            auto sample=scene(from,to);
            const auto p=scene.local(sample.point);
            // Deliberately adversarial submillimeter directional disagreement,
            // not a claim that this is the retail engine's numerical tolerance.
            if(++query_calls==18&&sample.hit&&std::hypot(to[0]-from[0],to[2]-from[2])>.01f&&
               (scene.maximum_z-p[2])*std::sqrt(1+scene.slope*scene.slope)<.0008) {
                sample.hit=false;sample.point={};sample.fraction=0;++native_misses;
            }
            return sample;
        };
        const auto r=w::search(scene.world({0,1.32f,2.6f}),{0,1,0},
            scene.direction(fakie?V{0,0,-1}:V{0,0,1}),scene.direction(fakie?V{-1,0,0}:V{1,0,0}),query);
        const auto& d=r.edge_diagnostic;
        check(r.available&&r.hit&&r.feature==w::Feature::edge&&native_misses==1&&r.rays<=30&&
              !d.original_witness_hit&&d.retry_attempted&&d.retry_interior_hit&&d.retry_interior_on_plane&&
              d.retry_radial_attempted&&d.witness_hit&&d.verified&&d.retry_inset_m>=.0005f&&
              d.retry_uncertainty_m<=w::maximum_edge_uncertainty&&distance(d.retry_interior_point,d.inside_point)<1e-6,
            "large-world yaw/fakie boundary miss is repaired only by actual bounded interior and radial hits");
    }
    for(unsigned fault=0;fault<2;++fault) {
        Scene scene;scene.floor=false;unsigned calls=0;
        const auto query=[&](V a,V b) {
            auto sample=scene(a,b);++calls;
            if(calls==18) {if(fault==0)sample.available=false;else sample.point[0]+=.1f;}
            return sample;
        };
        const auto r=w::search({0,1.32f,2.6f},{0,1,0},{0,0,1},{1,0,0},query);
        check(!r.available&&!r.hit&&calls==18&&!r.edge_diagnostic.retry_attempted,
            "unavailable or malformed original radial query fails closed without a retry");
    }
    {
        Scene scene;scene.slope=.05;scene.maximum_z=1;unsigned calls=0;
        const auto query=[&](V a,V b) {
            auto sample=scene(a,b);++calls;
            if(calls==28){sample.hit=false;sample.point={};sample.fraction=0;}
            if(calls==29)sample.available=false;
            return sample;
        };
        const auto r=w::search({0,.34f,1.25f},{0,1,0},{0,0,1},{1,0,0},query);
        check(!r.available&&!r.hit&&r.verified>=1&&calls==29&&r.edge_diagnostic.retry_attempted,
            "unavailable fresh interior retry invalidates even an earlier accepted flat-plane candidate");
    }
}
void ambiguity_and_bad_queries() {
    unsigned calls=0;
    const auto corrugated=[&](V from,V to) {
        ++calls;const double height=.01*(1-std::cos(from[2]*2*std::numbers::pi/.06));
        const auto fraction=static_cast<float>((from[1]-height)/(from[1]-to[1]));
        return w::RaySample{true,true,{from[0],static_cast<float>(height),from[2]},fraction};
    };
    const auto rough=w::search({0,.34f,0},{0,1,0},{0,0,1},{1,0,0},corrugated);
    check(rough.available&&!rough.hit&&rough.rays==15&&calls==15,"nonplanar patches outside strict residual remain unsupported instead of inventing a normal");
    for(unsigned failure=0;failure<4;++failure) {
        Scene floor;floor.ramp=false;unsigned count=0;
        const auto broken=[&](V from,V to) {
            auto sample=floor(from,to);++count;
            if(count==10) {
                if(failure==0)sample.available=false;
                if(failure==1)sample.fraction=std::numeric_limits<float>::quiet_NaN();
                if(failure==2)sample.point[0]+=.1f;
                if(failure==3)throw std::runtime_error("query failure");
            }
            return sample;
        };
        const auto invalid=w::search({0,.34f,0},{0,1,0},{0,0,1},{1,0,0},broken);
        check(!invalid.available&&!invalid.hit&&count==10,"later unavailable/invalid query discards earlier candidate rather than using partial stale support");
    }
    Scene floor;floor.ramp=false;
    const auto invalid_axes=w::search({0,.34f,0},{0,2,0},{0,0,1},{1,0,0},[&](V a,V b){return floor(a,b);});
    check(!invalid_axes.available&&floor.calls==0,"invalid caller frame is rejected before any query");
}

struct ClosedWedge : Scene {
    w::RaySample operator()(V from,V to) {
        auto result=Scene::operator()(from,to);
        const auto a=local(from),b=local(to);
        const double dz=double(b[2])-a[2];
        if(std::abs(dz)<1e-12)return result;
        const auto t=(maximum_z-a[2])/dz;
        if(t<0||t>1||(result.hit&&t>=result.fraction))return result;
        V hit;for(unsigned i=0;i<3;++i)hit[i]=static_cast<float>(a[i]+t*(double(b[i])-a[i]));
        if(hit[1]<floor_y||hit[1]>slope*maximum_z||std::abs(hit[0])>1)return result;
        return {true,true,world(hit),static_cast<float>(t)};
    }
};
struct ContinuousCurve : Scene {
    double curvature{2};
    w::RaySample operator()(V from,V to) {
        const auto a=local(from),b=local(to);
        V delta;for(unsigned i=0;i<3;++i)delta[i]=b[i]-a[i];
        double best=2;V point{};
        const auto consider=[&](double t,bool curved) {
            if(t<0||t>1||t>=best)return;
            V p;for(unsigned i=0;i<3;++i)p[i]=static_cast<float>(a[i]+t*delta[i]);
            if((curved&&p[2]<maximum_z)||(!curved&&p[2]>maximum_z)||std::abs(p[0])>1)return;
            best=t;point=p;
        };
        const auto denominator=double(delta[1])-slope*delta[2];
        if(std::abs(denominator)>1e-12)consider((slope*a[2]-a[1])/denominator,false);
        const double u=double(a[2])-maximum_z;
        const double qa=curvature*delta[2]*delta[2];
        const double qb=denominator+2*curvature*u*delta[2];
        const double qc=double(a[1])-slope*a[2]+curvature*u*u;
        if(std::abs(qa)<1e-12) {if(std::abs(qb)>1e-12)consider(-qc/qb,true);}
        else if(const auto discriminant=qb*qb-4*qa*qc;discriminant>=0) {
            const auto square=std::sqrt(discriminant);
            consider((-qb-square)/(2*qa),true);consider((-qb+square)/(2*qa),true);
        }
        return {true,best<=1,world(point),best<=1?static_cast<float>(best):0};
    }
};
void plane_normal_refinement_closed_backface() {
    for(double yaw:{0.,1.2,-2.3})for(bool fakie:{false,true})for(bool translated:{false,true}) {
        ClosedWedge scene;scene.yaw=yaw;
        if(translated)scene.translation={300.8f,274.3f,-480.7f};
        const double pitch=5*std::numbers::pi/180;
        const auto up=scene.direction({0,static_cast<float>(std::cos(pitch)),static_cast<float>(std::sin(pitch))});
        const auto forward=scene.direction({0,static_cast<float>(-std::sin(pitch)),static_cast<float>(std::cos(pitch))});
        const auto left=scene.direction({1,0,0});
        const auto f=fakie?w::detail::mul(forward,-1):forward;
        const auto l=fakie?w::detail::mul(left,-1):left;
        const auto hub=scene.world({0,1.336f,2.492f});
        const auto result=w::search(hub,up,f,l,[&](V a,V b){return scene(a,b);});
        check(result.available&&result.hit&&result.feature==w::Feature::edge&&result.rays<=30,
              "normal-directed refinement sees the real finite top edge despite a vertical backface below it");
        const auto endpoint=scene.world({0,1,2.5f});
        check(distance(result.point,endpoint)<.002&&result.edge_uncertainty<=.002&&result.edge_diagnostic.verified,
              "closed-wedge output still requires the real radial witness inside the original two-millimeter edge bound");
        const auto clear=w::search(scene.world({0,1.5f,3.15f}),up,f,l,[&](V a,V b){return scene(a,b);});
        check(clear.available&&!clear.hit,"closed backface cannot bridge a genuinely clear lip beyond wheel/query reach");
    }
}
void refinement_preserves_unresolved_samples_and_query_failures() {
    for(float residual:{-.005f,.005f}) {
        Scene scene;scene.slope=.05;scene.maximum_z=1;unsigned calls=0;
        const auto query=[&](V from,V to) {
            auto sample=scene(from,to);++calls;
            if(calls==20) {
                const double n=std::sqrt(1+.05*.05),dy=double(to[1])-from[1],dz=double(to[2])-from[2];
                const auto fraction=(.05*from[2]-from[1]+residual*n)/(dy-.05*dz);
                sample={true,true,{},static_cast<float>(fraction)};
                for(unsigned i=0;i<3;++i)sample.point[i]=from[i]+sample.fraction*(to[i]-from[i]);
            }
            return sample;
        };
        const auto result=w::search({0,.34f,1.25f},{0,1,0},{0,0,1},{1,0,0},query);
        check(result.feature!=w::Feature::edge&&result.edge_diagnostic.refinement_rejected&&
              result.edge_diagnostic.refinement_classification==0&&
              result.edge_diagnostic.refinement_projection_valid&&!result.edge_diagnostic.bounded&&
              std::abs(result.edge_diagnostic.refinement_signed_plane_error_m-residual)<2e-6,
              "actual shallow below-face curvature/noise or above-face occlusion remains ambiguous under the unchanged residual rule");
    }
    for(double yaw:{0.,1.2})for(double curvature:{.5,1.,2.,4.}) {
        ContinuousCurve scene;scene.floor=false;scene.yaw=yaw;scene.curvature=curvature;
        const auto result=w::search(scene.world({0,1.32f,2.6f}),{0,1,0},scene.direction({0,0,1}),scene.direction({1,0,0}),
            [&](V a,V b){return scene(a,b);});
        check(result.available&&result.feature!=w::Feature::edge,
              "a real C1 continuous curved surface cannot be reclassified as a finite edge by normal refinement");
    }
}


void fresh_patch_recovers_tipped_finite_lip_without_a_cached_face() {
    struct Pose {double pitch,y,z;};
    // Measured Run81 departure poses in a canonical .4slope,1m-high kicker.
    const Pose poses[]={{26.40,1.31240845,2.62750073},{27.88,1.30477905,2.64167863},
        {29.36,1.29464722,2.65635616},{30.84,1.28189087,2.67148726},
        {32.32,1.26660156,2.68705190},{33.80,1.24871826,2.70307010}};
    for(double yaw:{0.,1.2,-2.3})for(bool fakie:{false,true})for(bool translated:{false,true})for(const auto pose:poses) {
        ClosedWedge scene;scene.yaw=yaw;
        if(translated)scene.translation={300.8f,274.3f,-480.7f};
        const auto angle=pose.pitch*std::numbers::pi/180;
        const auto up=scene.direction({0,static_cast<float>(std::cos(angle)),static_cast<float>(std::sin(angle))});
        auto forward=scene.direction({0,static_cast<float>(-std::sin(angle)),static_cast<float>(std::cos(angle))});
        auto left=scene.direction({1,0,0});
        if(fakie) {forward=w::detail::mul(forward,-1);left=w::detail::mul(left,-1);}
        const auto hub=scene.world({0,static_cast<float>(pose.y),static_cast<float>(pose.z)});
        const auto result=w::search(hub,up,forward,left,[&](V a,V b){return scene(a,b);});
        const auto endpoint=scene.world({0,1,2.5f});
        const auto analytic=std::hypot(pose.z-2.5,pose.y-1)-.34;
        check(result.available&&result.hit&&result.feature==w::Feature::edge&&result.rays<=42,
              "fresh nearby measured patch recovers the actual finite lip when a tipped original patch straddles its backface");
        check(result.edge_diagnostic.verified&&distance(result.point,endpoint)<.002&&
              std::abs(result.separation-analytic)<.002&&result.edge_uncertainty<=.002,
              "recovered tipped lip still uses a fresh actual radial hit and the unchanged two-millimeter feature bound");
    }
    Scene flat;flat.ramp=false;
    const auto rest=search(flat,{0,.34f,0});
    check(rest.hit&&rest.rays==18,"ordinary flat support never enters the adaptive patch pass");
    Scene air;
    const auto clear=search(air,{0,3,0});
    check(clear.available&&!clear.hit&&clear.rays==3,"clear air never spends adaptive patch queries");
}
void every_new_recenter_query_failure_invalidates_the_search() {
    ClosedWedge baseline;
    const auto angle=26.4*std::numbers::pi/180;
    const V up{0,static_cast<float>(std::cos(angle)),static_cast<float>(std::sin(angle))};
    const V forward{0,static_cast<float>(-std::sin(angle)),static_cast<float>(std::cos(angle))};
    const V left{1,0,0},hub{0,1.31240845f,2.62750073f};
    const auto accepted=w::search(hub,up,forward,left,[&](V a,V b){return baseline(a,b);});
    check(accepted.hit&&accepted.rays>7&&accepted.rays<=42,"fault fixture actually reaches the fresh recenter pass");
    // Run81's mixed original patch costs7queries. Fault EVERY query after it,
    // including both new centers, all fresh patch samples, normal/refinement
    // and final radial witnesses. No earlier partial contact can survive.
    for(unsigned fault=0;fault<3;++fault)for(unsigned fail=8;fail<=accepted.rays;++fail) {
        ClosedWedge scene;unsigned calls=0;
        const auto broken=[&](V a,V b) {
            auto sample=scene(a,b);++calls;
            if(calls==fail) {
                if(fault==0)sample.available=false;
                if(fault==1) {sample.hit=true;sample.fraction=std::numeric_limits<float>::quiet_NaN();}
                if(fault==2)throw std::runtime_error("fresh recenter query failure");
            }
            return sample;
        };
        const auto rejected=w::search(hub,up,forward,left,broken);
        check(calls==fail&&!rejected.available&&!rejected.hit&&rejected.rays<=42,
              "unavailable, malformed or throwing query anywhere in the fresh fallback invalidates the complete search");
    }
}

int main() {
    fresh_patch_recovers_tipped_finite_lip_without_a_cached_face();every_new_recenter_query_failure_invalidates_the_search();
    plane_normal_refinement_closed_backface();refinement_preserves_unresolved_samples_and_query_failures();
    floor_to_ramp_toe_and_fakie();steep_toe_coverage_is_geometry_bounded();lips_and_missing_geometry_do_not_create_support();
    finite_lip_edges_have_actual_radial_witnesses();edge_refinement_does_not_relabel_ambiguous_or_failed_queries();
    edge_diagnostics_distinguish_native_clear_and_rejected_witness();clear_boundary_query_requires_an_actual_interior_rewitness();ambiguity_and_bad_queries();
    std::cout<<"BMX bounded wheel surface search, toe geometry, lips and query validation tests passed\n";
}
