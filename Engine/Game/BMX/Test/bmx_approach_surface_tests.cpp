#include "Engine/Game/BMX/bmx_approach_surface.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace a=dingosdk::bmx::approach_surface;
namespace t=dingosdk::bmx::tire;
namespace c=dingosdk::bmx::chassis_control;
unsigned assertions{},scenarios{};
void check(bool ok,const char* why){++assertions;if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
bool near(double x,double y,double e=1e-6){return std::abs(x-y)<e;}
using V=a::Vec;
struct Fixture {
    t::Body body=c::proposed_body();
    a::Surface surface{true,true,{0,0,0},{0,1,0},{0,0,1},.03873955458402634,77,{11,22}};
    double slope{},height{};unsigned calls{},epochs{},fail_epoch{};
    unsigned exhausted{},clear_ray{},bad_body{},bad_entity{},unavailable{},bad_fraction{},off_ray{},throw_ray{};
    bool selected=true,fresh=true,hop=false;
    Fixture(){body.center_of_mass={0,.9,-.05};body.linear_velocity={0,-4.334842206636954-9.8*a::native_seconds,7};}
    a::RayHit cast(V from,V to) {
        ++calls;if(calls==exhausted){a::RayHit r;r.native_casts=2;r.elapsed_microseconds=5;r.budget_exhausted=true;return r;}
        if(calls==throw_ray)throw std::runtime_error("actual query unavailable");
        if(calls==unavailable)return {};
        if(calls==clear_ray)return {true,false};
        const double denominator=double(to[1])-from[1]-slope*(double(to[2])-from[2]);
        if(std::abs(denominator)<1e-12)return {true,false};
        const double fraction=(height+slope*from[2]-from[1])/denominator;
        if(fraction<0||fraction>1)return {true,false};
        V point{};for(unsigned i=0;i<3;++i)point[i]=float(from[i]+fraction*(double(to[i])-from[i]));
        a::RayHit r{true,true,point,float(fraction),surface.entity,surface.body};
        if(calls==bad_body)++r.body[1];
        if(calls==bad_entity)++r.entity;
        if(calls==bad_fraction)r.fraction=2;
        if(calls==off_ray)r.point[0]+=.01f;
        return r;
    }
    a::Result run(double seconds=a::native_seconds) {
        ++scenarios;return a::observe(selected,fresh,hop,seconds,body,surface,
            [&](V from,V to){auto r=cast(from,to);if(!r.budget_exhausted){r.native_casts=1;r.elapsed_microseconds=3;}return r;},[&]{++epochs;return !fail_epoch||epochs!=fail_epoch;});
    }
};
void success_and_prediction() {
    Fixture f;const auto before=f.body;const auto r=f.run();
    check(r.qualified&&r.considered&&r.issue==a::Issue::qualified_geometry,"fresh finite plane closes within step");
    check(r.native_casts==5&&r.query_microseconds==15,"retains actual native count and query time without confusing wrapper count");
    check(f.calls==5&&r.rays==5&&f.epochs==7&&r.epoch_before&&r.epoch_after,"five actual casts and before/each/after epochs");
    check(near(r.normal_velocity,f.body.linear_velocity[1],1e-12),"precontact velocity contains gravity exactly once; helper adds none");
    check(near(r.time_to_plane,f.surface.separation/-f.body.linear_velocity[1],1e-12),"TOI is actual gap divided by supplied closing speed");
    check(near(r.projected_center[2],7*r.time_to_plane),"tangential footprint travels to first plane crossing");
    check(near(r.target_normal_velocity,-f.surface.separation/a::native_seconds,1e-12),"target is geometric gap closure, not zero-velocity glue");
    check(f.body.center_of_mass==before.center_of_mass&&f.body.linear_velocity==before.linear_velocity&&f.body.angular_velocity==before.angular_velocity,"observer cannot modify caller prediction");
    c::ContactSample sample;sample.contact.status=t::ContactStatus::clear;sample.separation=f.surface.separation;sample.verified_plane=true;
    check(!c::eligible_support(f.body,sample,c::actual_support_tolerance,.1),"qualified geometry does not turn a clear plane into actual support");
    for(unsigned i=0;i<5;++i)check(r.observations[i].hit.body==f.surface.body&&near(r.observations[i].hit.point[1],0),"each accepted point is an actual same-body ray hit");
}
void zero_query_refusals() {
    const auto reject=[](Fixture f,a::Issue why){const auto r=f.run();check(!r.qualified&&r.issue==why&&f.calls==0,"invalid/noncrossing input casts no extra rays");};
    Fixture f;f.selected=false;reject(f,a::Issue::disabled);
    f={};f.fresh=false;reject(f,a::Issue::unqualified_step);
    f={};f.surface={};reject(f,a::Issue::no_current_plane);
    f={};f.surface.plane=false;reject(f,a::Issue::no_current_plane);
    f={};f.surface.body[0]=0;reject(f,a::Issue::no_body_identity);
    f={};f.surface.body[1]=0;reject(f,a::Issue::no_body_identity);
    f={};f.surface.separation=.03;reject(f,a::Issue::existing_skin);
    f={};f.surface.separation=-.02;reject(f,a::Issue::existing_skin);
    f={};f.surface.separation=.250001;reject(f,a::Issue::outside_query);
    f={};f.hop=true;reject(f,a::Issue::hop_reserved);
    f={};f.body.angular_velocity={10,0,0};reject(f,a::Issue::rotation_bound);
    f={};f.body.linear_velocity[1]=0;reject(f,a::Issue::not_closing);
    f={};f.body.linear_velocity[1]=1;reject(f,a::Issue::not_closing);
    f={};f.body.linear_velocity[1]=-.5;reject(f,a::Issue::not_crossing);
    f={};f.surface.normal={};reject(f,a::Issue::invalid);
    f={};f.surface.separation=std::numeric_limits<double>::quiet_NaN();reject(f,a::Issue::invalid);
    f={};f.body.linear_velocity[0]=51;reject(f,a::Issue::invalid);
    f={};const auto r=f.run(.01);check(r.issue==a::Issue::invalid&&f.calls==0,"different integration step is unavailable, not rescaled");
}
void adverse_current_queries() {
    // Each missing center/perimeter is a real finite-face/hole miss, never a filled plane.
    for(unsigned k=1;k<=5;++k){Fixture f;f.clear_ray=k;const auto r=f.run();check(!r.qualified&&r.issue==a::Issue::clear_footprint&&r.rays==k,"any finite-face missing footprint refuses geometry");}
    for(unsigned k=1;k<=5;++k){Fixture f;f.bad_body=k;const auto r=f.run();check(!r.qualified&&r.issue==a::Issue::body_changed,"coplanar different body is not same face witness");}
    for(unsigned k=1;k<=5;++k){Fixture f;f.bad_entity=k;check(f.run().issue==a::Issue::body_changed,"different entity on same opaque body refuses witness");}
    for(unsigned k=1;k<=5;++k){Fixture f;f.unavailable=k;check(f.run().issue==a::Issue::query_unavailable,"unavailable current query refuses witness");}
    Fixture f;f.exhausted=3;const auto budget=f.run();check(!budget.qualified&&budget.issue==a::Issue::query_unavailable&&budget.native_casts==4&&budget.query_microseconds==11&&budget.observations[2].hit.budget_exhausted,"exhausted native exclusion retains its actual count/time and cannot qualify");
    f={};f.bad_fraction=3;check(f.run().issue==a::Issue::malformed_hit,"fraction outside segment refuses witness");
    f={};f.off_ray=2;check(f.run().issue==a::Issue::malformed_hit,"returned off-segment point refuses witness");
    f={};f.throw_ray=2;check(f.run().issue==a::Issue::query_unavailable,"query exception is unavailable, never geometry");
    f={};f.height=.003;check(f.run().issue==a::Issue::plane_disagreement,"nearby parallel occluder beyond2.5mm refuses witness");
    f={};f.body.linear_velocity[2]=0;f.slope=std::tan(2*3.141592653589793/180);const auto angled=f.run();
    check(angled.issue==a::Issue::normal_disagreement&&!angled.qualified&&angled.rays==5,"fresh neighboring face with two-degree normal change refuses witness");
    for(unsigned k=1;k<=7;++k){f={};f.fail_epoch=k;const auto r=f.run();check(!r.qualified&&r.issue==a::Issue::epoch_changed,"owner/world/native-clock loss before, during or after refuses witness");check(r.rays==(k==1?0:k==7?5:k-2),"epoch loss stops remaining native casts immediately");}
}
void slopes_and_world_coordinates() {
    for(double slope:{-.6,-.2,0.,.2,.6})for(float offset:{0.f,274.f,10000.f}) {
        Fixture f;f.slope=slope;const auto n=std::sqrt(1+slope*slope);
        f.surface.point={offset,float(slope*offset),offset};f.height=0;
        f.surface.normal={0,float(1/n),float(-slope/n)};f.surface.forward={0,float(slope/n),float(1/n)};
        f.body.center_of_mass={offset,double(f.surface.point[1])+.9,offset};
        f.body.linear_velocity={0,-5/n,5*slope/n};const auto r=f.run();
        check(r.qualified&&r.rays==5,"fresh inclined-plane actual casts remain valid with world-coordinate float quantization");
        check(r.normal_dot>=a::minimum_normal_dot,"fitted actual patch agrees with current plane normal");
    }
    // Exactly250mm is in range only when high enough speed actually crosses it.
    Fixture f;f.surface.separation=.25;f.body.linear_velocity={0,-20,0};check(f.run().qualified,"query outer boundary is finite and crossing-gated");
    f={};f.surface.separation=.25;f.body.linear_velocity={0,-10,0};check(f.run().issue==a::Issue::not_crossing&&f.calls==0,"250mm presence alone never enables extra casts");
}
int main(){success_and_prediction();zero_query_refusals();adverse_current_queries();slopes_and_world_coordinates();
    std::cout<<"PASS "<<assertions<<" assertions across "<<scenarios<<" observer invocations; repeated coordinates/epoch loops are not independent cases\n";}
