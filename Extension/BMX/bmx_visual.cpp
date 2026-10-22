#include "bmx_visual.h"
#include "bmx_visual_geometry.h"
#include "bmx_rider.h"
#include "bmx_world_transform.h"
#include "Engine/Game/UI/game_view.h"
#include "External/imgui/imgui.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>
#include <windows.h>

namespace dingosdk::bmx::visual {
namespace {
namespace bike_geometry=dingosdk::bmx::geometry;
using Vec = std::array<float, 3>;
constexpr float pi = 3.14159265358979323846f;
using detail::ReferenceTriangle;
const std::vector<ReferenceTriangle>& reference_geometry() {
    static const auto mesh=[] {
#if defined(DINGOSDK_BMX_REFERENCE_RESOURCE)
        HMODULE module{};
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&draw),&module)){
            const auto resource=FindResourceW(module,L"BMX_REFERENCE_GEOMETRY",MAKEINTRESOURCEW(10));
            const auto size=resource?SizeofResource(module,resource):0;
            const auto loaded=resource?LoadResource(module,resource):nullptr;
            const auto* bytes=loaded?static_cast<const std::byte*>(LockResource(loaded)):nullptr;
            if(bytes&&size>=44&&size<=44+100000*40)return detail::decode_geometry({bytes,size});
        }
#endif
        return std::vector<ReferenceTriangle>{};
    }();
    return mesh;
}
struct Feed { std::mutex mutex; Frame frame; std::chrono::steady_clock::time_point at; };
Feed& feed() { static auto* value = new Feed; return *value; }
std::atomic<std::uint64_t> draw_count{};
Vec add(Vec a, Vec b) { for (unsigned i = 0; i < 3; ++i) a[i] += b[i]; return a; }
Vec sub(Vec a, Vec b) { for (unsigned i = 0; i < 3; ++i) a[i] -= b[i]; return a; }
Vec scale(Vec a, float k) { for (auto& v : a) v *= k; return a; }
float dot(Vec a, Vec b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec cross(Vec a, Vec b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
Vec unit(Vec a) { return scale(a, 1.f / std::max(std::sqrt(dot(a,a)), .00001f)); }
void tube(std::vector<ReferenceTriangle>& out, Vec from, Vec to, float radius, unsigned part) {
    const auto axis = unit(sub(to,from)); const auto u = unit(cross(axis, std::abs(axis[1]) < .9f ? Vec{0,1,0} : Vec{1,0,0}));
    const auto v = cross(axis,u);
    for (int i=0;i<8;++i) {
        const auto offset = [&](int n) { const float a = n*pi*.25f; return scale(add(scale(u,std::cos(a)),scale(v,std::sin(a))),radius); };
        const auto a=offset(i), b=offset(i+1);
        out.push_back({{add(from,a),add(to,a),add(to,b)},part});
        out.push_back({{add(from,a),add(to,b),add(from,b)},part});
    }
}
void wheel(std::vector<ReferenceTriangle>& out, float z, unsigned part, bool tire) {
    const Vec hub{0,part==2?bike_geometry::rear_axle[1]:bike_geometry::front_axle[1],z};
    for(int i=0;i<24;++i) {
        const float a=i*pi/12.f, b=(i+1)*pi/12.f;
        if(tire) {
            for(int j=0;j<6;++j) {
                const auto point=[&](float angle,int side) { const float w=side*pi/3.f; return Vec{std::sin(w)*.026f,.34f+std::cos(angle)*(.314f+std::cos(w)*.026f),z+std::sin(angle)*(.314f+std::cos(w)*.026f)}; };
                out.push_back({{point(a,j),point(b,j),point(b,j+1)},part});
                out.push_back({{point(a,j),point(b,j+1),point(a,j+1)},part});
            }
        }
        tube(out,Vec{0,.34f+std::cos(a)*.275f,z+std::sin(a)*.275f},Vec{0,.34f+std::cos(b)*.275f,z+std::sin(b)*.275f},.009f,part);
        if(i%2==0) tube(out,hub,Vec{0,.34f+std::cos(a)*.275f,z+std::sin(a)*.275f},.0025f,part);
    }
}
const std::vector<ReferenceTriangle>& geometry() {
    static const auto result=[] {
        std::vector<ReferenceTriangle> mesh=reference_geometry();
        const bool reference=!mesh.empty();
        if(!reference){
        const Vec rear=bike_geometry::rear_axle, crank=world::crank_pivot, seat{0,.72f,-.22f}, head=bike_geometry::steering_pivot;
        for(const auto pair : {std::array<Vec,2>{rear,crank}, {rear,seat}, {crank,seat}, {seat,head}, {crank,head}}) tube(mesh,pair[0],pair[1],.022f,0);
        tube(mesh,head,bike_geometry::front_axle,.021f,1);
        tube(mesh,head,{0,.98f,.40f},.022f,1);
        tube(mesh,{-.32f,1.04f,.39f},{.32f,1.04f,.39f},.014f,5);
        }
        wheel(mesh,bike_geometry::rear_axle[2],2,!reference); wheel(mesh,bike_geometry::front_axle[2],3,!reference);
        tube(mesh,{0,.76f,.36f},{0,.86f,.39f},.018f,1);
        tube(mesh,{0,.55f,-.22f},{0,.76f,-.22f},.016f,0);
        tube(mesh,{0,.76f,-.32f},{0,.76f,-.09f},.055f,0);
        std::erase_if(mesh,[](const auto& triangle){return triangle.part==4;});
        const auto cy=world::crank_pivot[1],cz=world::crank_pivot[2],r=world::crank_radius;
        tube(mesh,{-.105f,cy,cz},{.105f,cy,cz},.023f,4);
        tube(mesh,{-.10f,cy,cz},{-.10f,cy,cz+r},.014f,4);
        tube(mesh,{.10f,cy,cz},{.10f,cy,cz-r},.014f,4);
        tube(mesh,{-.15f,cy,cz+r},{-.05f,cy,cz+r},.025f,4);
        tube(mesh,{.05f,cy,cz-r},{.15f,cy,cz-r},.025f,4);
        return mesh;
    }();
    return result;
}
Vec transform(Vec p,unsigned part,const Frame& frame) {
    chassis::Quaternion q;
    const auto* rotation=frame.chassis_quaternion_available&&chassis::unit_quaternion(frame.chassis_quaternion,q)?&q:nullptr;
    p=world::detail::local_point(p,part,frame,rotation);
    return add(p,frame.position);
}
}
bool uses_reference_mesh() noexcept { try{return !reference_geometry().empty();}catch(...){return false;} }
std::uint64_t draws() noexcept { return draw_count.load(std::memory_order_relaxed); }
bool pending() noexcept {
    auto& state=feed(); std::lock_guard lock(state.mutex);
    return state.frame.active && std::chrono::steady_clock::now()-state.at < std::chrono::milliseconds(200);
}
void begin_client_tick() noexcept {rider::begin_client_tick();}
Publication pending_placement() noexcept {return rider::pending_placement();}
bool complete_placement(std::uint64_t revision,std::uintptr_t entity,bool all_parts_placed) noexcept {
    return rider::complete_placement(revision,entity,all_parts_placed);
}
void publish(const Frame& frame) noexcept {
    auto& state=feed(); { std::lock_guard lock(state.mutex); state.frame=frame; state.at=std::chrono::steady_clock::now(); }
    rider::publish(frame);
}
void draw() noexcept {
    try {
        Frame frame; { auto& s=feed(); std::lock_guard lock(s.mutex); if(!s.frame.active||std::chrono::steady_clock::now()-s.at>=std::chrono::milliseconds(200))return; frame=s.frame; }
        const auto view=latest_game_view(); if(!view||view->vertical_fov<=1||view->vertical_fov>=175)return;
        const auto& m=view->world; const Vec right{m[0],m[1],m[2]},up{m[4],m[5],m[6]},back{m[8],m[9],m[10]},camera{m[12],m[13],m[14]};
        const auto size=ImGui::GetIO().DisplaySize; if(size.x<=0||size.y<=0)return;
        const float focal=size.y/(2*std::tan(view->vertical_fov*pi/360));
        struct Draw { ImVec2 p[3]; float depth; ImU32 color; }; std::vector<Draw> triangles; triangles.reserve(geometry().size());
        for(const auto& triangle:geometry()) {
            Draw item{}; Vec world[3]; bool valid=true;
            for(unsigned i=0;i<3;++i) {
                world[i]=transform(triangle.p[i],triangle.part,frame); const auto d=sub(world[i],camera); const float depth=-dot(d,back);
                if(!std::isfinite(depth)||depth<.08f){valid=false;break;}
                item.p[i]={size.x*.5f+dot(d,right)*focal/depth,size.y*.5f-dot(d,up)*focal/depth}; item.depth+=depth/3;
                if(!std::isfinite(item.p[i].x)||!std::isfinite(item.p[i].y)||std::abs(item.p[i].x)>size.x*8||std::abs(item.p[i].y)>size.y*8){valid=false;break;}
            }
            if(!valid)continue;
            const auto normal=unit(cross(sub(world[1],world[0]),sub(world[2],world[0])));
            const float shade=.38f+.62f*std::abs(dot(normal,unit(Vec{.4f,.8f,-.3f})));
            const bool tire=(triangle.part==2||triangle.part==3);
            const Vec color=tire?Vec{38,42,47}:triangle.part==4?Vec{170,180,188}:Vec{37,154,201};
            item.color=IM_COL32(static_cast<int>(color[0]*shade),static_cast<int>(color[1]*shade),static_cast<int>(color[2]*shade),255); triangles.push_back(item);
        }
        std::sort(triangles.begin(),triangles.end(),[](const auto& a,const auto& b){return a.depth>b.depth;});
        auto* draw=ImGui::GetBackgroundDrawList(); for(const auto& t:triangles)draw->AddTriangleFilled(t.p[0],t.p[1],t.p[2],t.color);
        if(!triangles.empty())draw_count.fetch_add(1,std::memory_order_relaxed);
        draw->AddText(ImVec2(22,size.y-48),IM_COL32(220,230,240,220),"BMX prototype | Left stick: steer | Right stick: hop / rear tricks");
    } catch(...) {}
}
}
