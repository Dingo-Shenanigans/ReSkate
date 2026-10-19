#include "Extension/Skater/FirstPerson/Upstream/src/bail_collision.h"
#include "Extension/Skater/FirstPerson/Upstream/src/bail_collision_contract.h"
#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
#include <iostream>
#include <fstream>
#include <limits>
namespace jishsfirstpersonmod {
void log(const std::string&) noexcept {}
std::filesystem::path module_directory() { return std::filesystem::current_path(); }
std::filesystem::path data_directory() { return module_directory(); }
}
using namespace jishsfirstpersonmod;
void check(bool value,const char* message) { if(!value) { std::cerr<<message<<'\n';std::exit(1); } }
bool close_enough(float a,float b) { return std::abs(a-b)<.0001f; }
int main() {
    BailCollision collision;
    unsigned calls{};
    auto floor=[&](CollisionPoint from,CollisionPoint to,float radius) {
        ++calls;check(close_enough(radius,.11f),"Incorrect camera clearance");
        if(to[1]<radius && to[1]<from[1]) return CollisionSweep{true,std::clamp((from[1]-radius)/(from[1]-to[1]),0.f,1.f)};
        return CollisionSweep{true,1};
    };
    auto free=[](auto,auto,float) { return CollisionSweep{true,1}; };
    auto point=collision.resolve({0,1,0},{0,2,0},1,free);
    check(close_enough(point[1],1) && !collision.blocked(),"Unobstructed camera changed");
    point=collision.resolve({1,-1,1},{0,2,0},1.016,floor);
    check(point[1]>=.11f && close_enough(point[0],1) && close_enough(point[2],1),"Floor penetration or blocked lateral movement");
    for(int i=0;i<15;++i) {
        point=collision.resolve({1+float(i)*.01f,-1,1},{},1.032+i*.016,floor);
        check(point[1]>=.11f,"Repeated contact entered floor");
    }
    check(calls<90,"Unbounded per-frame queries");
    const auto held=point;
    point=collision.resolve({3,2,1},{},1.272,[](auto,auto,float) { return CollisionSweep{}; });
    check(point==held,"Failed query moved camera");
    point=collision.resolve({3,2,1},{},1.288,free);
    check(point[1]>held[1] && point[1]<2,"Collision release was not smoothed");
    for(int i=0;i<50;++i) point=collision.resolve({3,2,1},{},1.304+i*.016,free);
    check(!collision.blocked() && close_enough(point[1],2),"Return did not finish");
    collision.reset();
    auto corner=[](CollisionPoint from,CollisionPoint to,float radius) {
        float fraction=1;
        for(unsigned i=0;i<2;++i) if(to[i]<radius && to[i]<from[i]) fraction=std::min(fraction,std::clamp((from[i]-radius)/(from[i]-to[i]),0.f,1.f));
        return CollisionSweep{true,fraction};
    };
    point=collision.resolve({-1,-1,2},{1,1,0},3,corner);
    check(point[0]>=.11f && point[1]>=.11f && close_enough(point[2],2),"Corner penetration");
    collision.reset();
    point=collision.resolve({0,1,0},{0,2,0},4,[](auto,auto,float) { return CollisionSweep{true,std::numeric_limits<float>::quiet_NaN()}; });
    check(close_enough(point[1],2),"Invalid hit moved camera");
    point=collision.resolve({20,1,0},{20,2,0},5,free);
    check(close_enough(point[0],20) && close_enough(point[1],1),"Teleport retained collision history");
    // Reproduce an immediate floor hit in both direct and axis forward casts.
    collision.reset();unsigned reverse_calls{};const CollisionPoint standing{0,1.8f,0};
    auto stuck_floor=[&](CollisionPoint from,CollisionPoint to,float radius) {
        if(from[1]<=.111f) return CollisionSweep{true,0};
        if(to[1]<radius && to[1]<from[1]) {
            ++reverse_calls;return CollisionSweep{true,(from[1]-radius)/(from[1]-to[1])};
        }
        return CollisionSweep{true,1};
    };
    collision.resolve(standing,{0,.10f,0},10,stuck_floor,true,&standing);
    point=collision.resolve(standing,{},10.049,stuck_floor,true,&standing);
    check(reverse_calls==0 && close_enough(point[1],.1f),"Reverse escape ran before 50ms");
    point=collision.resolve(standing,{},10.051,stuck_floor,true,&standing);
    check(reverse_calls==1 && point[1]>.111f,"Stuck camera did not escape on the clear side");
    point=collision.resolve(standing,{},10.067,stuck_floor,true,&standing);
    check(point[1]>.112f && point[1]<standing[1],"Escape did not resume smoothed return");
    collision.reset();reverse_calls=0;
    for(int i=0;i<20;++i) collision.resolve(standing,{0,.1f,0},11+i*.016,stuck_floor,false,&standing);
    check(reverse_calls==0,"Reverse escape ran before normal control returned");
    collision.reset();
    auto failed_reverse=[](CollisionPoint from,auto,float) {
        return from[1]>.111f ? CollisionSweep{}:CollisionSweep{true,0};
    };
    collision.resolve(standing,{0,.1f,0},12,failed_reverse,true,&standing);
    point=collision.resolve(standing,{},12.16,failed_reverse,true,&standing);
    check(close_enough(point[1],.1f),"Failed reverse query moved the camera");
    // Controlled return is faster, but still sweeps and smooths every move.
    BailCollision slow,fast;
    const auto blocked=[](auto,auto,float) { return CollisionSweep{true,0}; };
    slow.resolve(standing,{0,.1f,0},13,blocked);
    fast.resolve(standing,{0,.1f,0},13,blocked);
    const auto slow_point=slow.resolve(standing,{},13.016,free);
    const auto fast_point=fast.resolve(standing,{},13.016,free,true,&standing);
    check(fast_point[1]>slow_point[1] && fast_point[1]<standing[1],"Controlled return was not faster and smooth");
    const auto path=std::filesystem::current_path()/L"bail-collision-test-data"/L"settings.json";
    std::filesystem::create_directories(path.parent_path());
    write_config(path,Config{});
    check(read_config(path).bail_camera_collision,"New settings did not default collision on");
    SettingsStore store(path,Config{});Command command;command.kind=CommandKind::bail_collision;command.enabled=false;
    check(store.submit(command,100),"Collision toggle command failed");store.flush_due(100);
    if(store.snapshot().unsaved) std::cerr<<store.snapshot().feedback<<'\n';
    check(!store.snapshot().unsaved,"Collision settings save failed");
    check(!read_config(path).bail_camera_collision,"Collision setting did not persist");
    std::filesystem::remove(path);
    check(!bail_collision_contract::verified([](auto,void*,auto) { return false; },0),"Unsupported build accepted");
    std::cout<<"Collision resolver, native rejection, persistence and recovery passed\n";
}
