#pragma once
// Private CPU-request markers only. No native read/call, pose mutation or GPU identity claim.
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Core/Json/json.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <type_traits>

namespace dingosdk::bmx::grip_projection_trace {
inline constexpr wchar_t private_root[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-ramp-test";
inline constexpr std::size_t capacity=512;
inline constexpr std::uint64_t freshness_ms=100;
using Point=std::array<float,3>;
struct Source {
    std::uintptr_t base{};std::array<std::uintptr_t,5> owner{};
    std::uint64_t serial{},revision{},tick{};double native_time{};unsigned record{};
    std::array<Point,4> points{}; // requested gripL,gripR,rear axle,front axle; world coordinates
};
struct Camera {
    Source associated_source;std::array<float,16> world{};float fov{};
    std::uintptr_t address{};std::uint64_t tick{};bool available{};
};
struct ScreenPoint {float x{},y{},depth{};};
struct Draw {
    Source source;Camera camera;std::uint64_t sequence{},tick{};float width{},height{};
    std::array<ScreenPoint,4> points{};bool spatial_available{};
};
struct Packet {Draw record;std::array<char,256> text{};};
enum class CameraReason {unreported,source_uninitialized,source_busy,source_base,source_thread,
    skater_read,body_read,ineligible,feature_off,client_thread,request_busy,inactive,base_mismatch,
    owner_mismatch,source_stale,capacity_reached,snapshot_before,mode_not_gameplay,view_disabled,input_active,
    view_matrix,fov_read,snapshot_after,reader_verify,identity_changed,camera_address,camera_basis,
    publication_busy,publication_owner,request_stale,published,count};
inline constexpr std::array<const char*,static_cast<unsigned>(CameraReason::count)> camera_reason_names{
    "unreported","source_uninitialized","source_busy","source_base","source_thread","skater_read","body_read",
    "ineligible","feature_off","client_thread","request_busy","inactive","base_mismatch","owner_mismatch",
    "source_stale","capacity_reached","snapshot_before","mode_not_gameplay","view_disabled","input_active",
    "view_matrix","fov_read","snapshot_after","reader_verify","identity_changed","camera_address","camera_basis",
    "publication_busy","publication_owner","request_stale","published"};
inline constexpr std::size_t camera_diagnostic_capacity=32;
struct CameraDiagnostic {
    Source source;std::uint64_t tick{};CameraReason reason{};
    bool can_control{},camera_phase{},ambiguous{},park_editor{},body_chain_read{},body_owner_verified{};
    std::array<char,128> issue{};
    bool camera_values_observed{};std::array<float,16> observed_camera_world{};float observed_camera_fov{};
};
struct State {
    std::atomic<bool> initialized{},enabled{},ever_published{};std::atomic<std::uint64_t> contention{};
    std::mutex mutex;Source source;Camera camera;std::array<Draw,capacity> rows{};
    std::uintptr_t base{};std::uint64_t creation{};DWORD client_thread{};std::size_t count{};bool active{};
    std::array<CameraDiagnostic,camera_diagnostic_capacity> camera_diagnostics{};
    std::array<std::uint64_t,camera_reason_names.size()> camera_reason_counts{};
    std::uint64_t camera_diagnostic_entries{},camera_diagnostic_drops{};std::size_t camera_diagnostic_count{};
};
static_assert(std::is_trivially_copyable_v<Draw>);
static_assert(sizeof(State)<256*1024);
inline State state;
struct ErrorGuard {DWORD saved=GetLastError();~ErrorGuard(){SetLastError(saved);}};
inline bool flag(const wchar_t* name) noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(name,value,2)==1&&value[0]==L'1';}
inline bool accepting() noexcept {return state.enabled.load(std::memory_order_acquire)&&flag(L"RESKATE_BMX_GRIP_REQUEST_HUD");}
inline bool selected() noexcept {
    ErrorGuard error;if(!accepting())return false;
    try {return flag(L"RESKATE_BMX_GRIP_PROBE")&&flag(L"RESKATE_BMX_RAMP_RIDE")&&flag(L"RESKATE_BMX_NATIVE_CHASSIS")&&
        user_data::runtime_development_selected(L"bmx-user-data-ramp-test",private_root);}catch(...){return false;}
}
inline bool fresh(std::uint64_t now,std::uint64_t tick) noexcept {return tick&&now>=tick&&now-tick<=freshness_ms;}
inline bool same_owner(const Source& a,const Source& b) noexcept {return a.base==b.base&&a.owner==b.owner;}
inline bool complete(const Source& s) noexcept {
    if(!s.base||!s.serial||!s.revision||!s.tick||s.record>=128||!std::isfinite(s.native_time))return false;
    for(const auto value:s.owner)if(!value)return false;
    for(const auto& point:s.points)for(float value:point)if(!std::isfinite(value)||std::abs(value)>1e6f)return false;
    return true;
}
inline void initialize(std::uintptr_t base,std::uint64_t creation,bool image_verified,const std::filesystem::path& root) noexcept {
    ErrorGuard error;
    try {
        if(state.initialized.load(std::memory_order_acquire)||!image_verified||!base||!creation||
           !flag(L"RESKATE_BMX_GRIP_REQUEST_HUD")||_wcsicmp(root.c_str(),private_root)!=0||
           !user_data::runtime_development_selected(L"bmx-user-data-ramp-test",private_root)||
           !flag(L"RESKATE_BMX_GRIP_PROBE")||!flag(L"RESKATE_BMX_RAMP_RIDE")||!flag(L"RESKATE_BMX_NATIVE_CHASSIS"))return;
        state.base=base;state.creation=creation;state.client_thread=GetCurrentThreadId();
        state.initialized.store(true,std::memory_order_release);state.enabled.store(true,std::memory_order_release);
    }catch(...){}
}
inline void publish_source(const Source& source) noexcept {
    ErrorGuard error;if(!accepting()||!complete(source)||source.base!=state.base)return;
    std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock()){++state.contention;return;}
    if(state.active&&(!same_owner(source,state.source)||source.serial<state.source.serial||source.revision<state.source.revision)){
        state.active=false;state.camera.available=false;state.enabled.store(false,std::memory_order_release);return;}
    state.source=source;state.active=true;state.ever_published.store(true,std::memory_order_release);
}
inline void withdraw() noexcept {
    ErrorGuard error;if(!state.initialized.load(std::memory_order_acquire))return;
    if(state.ever_published.load(std::memory_order_acquire))state.enabled.store(false,std::memory_order_release);
    // Atomic revocation cannot be lost to a busy overlay; pre-ride inactive ticks do not consume the capture.
}
// Metadata only: retained CPU request ownership, not a newly read native owner.
inline std::optional<CameraDiagnostic> begin_camera_diagnostic(std::uintptr_t base,std::uintptr_t client,
    bool can_control,bool camera_phase) noexcept {
    ErrorGuard error;if(!selected()||GetCurrentThreadId()!=state.client_thread)return {};
    const auto now=GetTickCount64();std::unique_lock lock(state.mutex,std::try_to_lock);
    if(!lock.owns_lock())return {};
    if(!state.active||base!=state.base||state.source.base!=base||state.source.owner[0]!=client||
       !fresh(now,state.source.tick)||state.count>=capacity)return {};
    ++state.camera_diagnostic_entries;CameraDiagnostic result;result.source=state.source;result.tick=now;
    result.can_control=can_control;result.camera_phase=camera_phase;return result;
}
inline void finish_camera_diagnostic(CameraDiagnostic row,CameraReason reason,const char* issue=nullptr) noexcept {
    ErrorGuard error;if(!selected()||GetCurrentThreadId()!=state.client_thread)return;
    const auto now=GetTickCount64();std::unique_lock lock(state.mutex,std::try_to_lock);
    if(!lock.owns_lock())return;
    if(!state.active||!same_owner(row.source,state.source)||!fresh(now,row.tick)||
       !fresh(now,row.source.tick)||!fresh(now,state.source.tick))return;
    const auto index=static_cast<unsigned>(reason);if(index>=camera_reason_names.size())return;
    ++state.camera_reason_counts[index];row.reason=reason;
    if(issue)std::snprintf(row.issue.data(),row.issue.size(),"%.127s",issue);
    if(state.camera_diagnostic_count<camera_diagnostic_capacity)
        state.camera_diagnostics[state.camera_diagnostic_count++]=row;
    else ++state.camera_diagnostic_drops;
}
inline std::optional<Source> camera_request(std::uintptr_t base,const std::array<std::uintptr_t,5>& owner,bool eligible,
    CameraReason* outcome=nullptr) noexcept {
    ErrorGuard error;const auto reject=[&](CameraReason reason)->std::optional<Source>{if(outcome)*outcome=reason;return {};};
    if(!eligible)return reject(CameraReason::ineligible);
    if(!selected())return reject(CameraReason::feature_off);
    if(GetCurrentThreadId()!=state.client_thread)return reject(CameraReason::client_thread);
    std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock()){++state.contention;return reject(CameraReason::request_busy);}
    if(!state.active)return reject(CameraReason::inactive);
    if(base!=state.base||state.source.base!=base)return reject(CameraReason::base_mismatch);
    if(state.source.owner!=owner)return reject(CameraReason::owner_mismatch);
    if(!fresh(GetTickCount64(),state.source.tick))return reject(CameraReason::source_stale);
    if(state.count>=capacity)return reject(CameraReason::capacity_reached);
    if(outcome)*outcome=CameraReason::unreported;return state.source;
}
inline bool valid_camera(const std::array<float,16>& m,float fov) noexcept {
    if(!std::isfinite(fov)||fov<=1||fov>=175)return false;
    for(float v:m)if(!std::isfinite(v)||std::abs(v)>1e6f)return false;
    for(unsigned a=0;a<3;++a){float length{};for(unsigned i=0;i<3;++i)length+=m[a*4+i]*m[a*4+i];
        if(length<.99f||length>1.01f)return false;
        for(unsigned b=a+1;b<3;++b){float dot{};for(unsigned i=0;i<3;++i)dot+=m[a*4+i]*m[b*4+i];if(std::abs(dot)>.01f)return false;}}
    const float determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);
    return determinant>.99f&&determinant<1.01f;
}
inline void publish_camera(const Source& request,std::uintptr_t camera,const std::array<float,16>& world,float fov,
    CameraReason* outcome=nullptr) noexcept {
    ErrorGuard error;const auto reject=[&](CameraReason reason){if(outcome)*outcome=reason;};
    if(!selected()){reject(CameraReason::feature_off);return;}
    if(GetCurrentThreadId()!=state.client_thread){reject(CameraReason::client_thread);return;}
    if(camera<0x10000||camera>0x00007fffffffffffULL-0xb0){reject(CameraReason::camera_address);return;}
    auto camera_world=world; // LinearTransform SIMD W lanes are unused; keep raw diagnostic input intact.
    camera_world[3]=camera_world[7]=camera_world[11]=camera_world[15]=0.0f;
    if(!valid_camera(camera_world,fov)){reject(CameraReason::camera_basis);return;}
    const auto now=GetTickCount64();std::unique_lock lock(state.mutex,std::try_to_lock);
    if(!lock.owns_lock()){++state.contention;reject(CameraReason::publication_busy);return;}
    if(!state.active){reject(CameraReason::inactive);return;}
    if(!same_owner(request,state.source)){reject(CameraReason::publication_owner);return;}
    if(!fresh(now,request.tick)){reject(CameraReason::request_stale);return;}
    if(!fresh(now,state.source.tick)){reject(CameraReason::source_stale);return;}
    state.camera={request,camera_world,fov,camera,now,true};reject(CameraReason::published);
}
inline bool pending() noexcept {
    ErrorGuard error;if(!selected())return false;
    std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock())return false;
    return state.active&&state.count<capacity&&fresh(GetTickCount64(),state.source.tick);
}
inline bool project(const Point& point,const Camera& camera,float width,float height,ScreenPoint& output) noexcept {
    if(!valid_camera(camera.world,camera.fov)||!std::isfinite(width)||!std::isfinite(height)||width<1||height<1||width>16384||height>16384)return false;
    const auto& m=camera.world;const Point d{point[0]-m[12],point[1]-m[13],point[2]-m[14]};
    for(float value:d)if(!std::isfinite(value))return false;
    const float depth=-(d[0]*m[8]+d[1]*m[9]+d[2]*m[10]);if(!std::isfinite(depth)||depth<.08f)return false;
    const float focal=height/(2*std::tan(camera.fov*3.14159265358979323846f/360));
    ScreenPoint p{width*.5f+(d[0]*m[0]+d[1]*m[1]+d[2]*m[2])*focal/depth,
        height*.5f-(d[0]*m[4]+d[1]*m[5]+d[2]*m[6])*focal/depth,depth};
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>width*8||std::abs(p.y)>height*8)return false;
    output=p;return true;
}
inline std::optional<Packet> draw_packet(float width,float height) noexcept {
    ErrorGuard error;if(!selected())return {};const auto now=GetTickCount64();
    std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock()){++state.contention;return {};}
    if(!state.active||state.count>=capacity||!fresh(now,state.source.tick))return {};
    Packet packet;auto& row=packet.record;row.source=state.source;row.camera=state.camera;
    row.sequence=state.count+1;row.tick=now;row.width=width;row.height=height;
    row.spatial_available=row.camera.available&&same_owner(row.source,row.camera.associated_source)&&fresh(now,row.camera.tick);
    for(unsigned i=0;i<row.points.size();++i)row.spatial_available=row.spatial_available&&project(row.source.points[i],row.camera,width,height,row.points[i]);
    std::snprintf(packet.text.data(),packet.text.size(),"CPU REQUEST PID%lu P%llu R%u S%llu rev%llu | %s | src%llums cam%llums",
        static_cast<unsigned long>(GetCurrentProcessId()),static_cast<unsigned long long>(row.sequence),row.source.record,static_cast<unsigned long long>(row.source.serial),
        static_cast<unsigned long long>(row.source.revision),row.spatial_available?"VERIFY AXLE ANCHORS":"CAMERA UNAVAILABLE",
        static_cast<unsigned long long>(now-row.source.tick),static_cast<unsigned long long>(row.camera.available&&now>=row.camera.tick?now-row.camera.tick:0));
    state.rows[state.count++]=row;return packet;
}
inline Json export_snapshot() {
    ErrorGuard error;std::unique_lock lock(state.mutex,std::try_to_lock);
    if(!lock.owns_lock())return Json{{"available",false},{"issue","diagnostic_busy"}};
    Json rows=Json::array();
    for(std::size_t i=0;i<state.count;++i){const auto& r=state.rows[i];Json pixels=Json::array();for(const auto& p:r.points)pixels.push_back({p.x,p.y,p.depth});
        rows.push_back(Json{{"sequence",r.sequence},{"tick_ms",r.tick},{"grip_record",r.source.record},{"source_base",r.source.base},
            {"source_owner",r.source.owner},{"listener_serial",r.source.serial},{"listener_time",r.source.native_time},
            {"sdk_frame_placement_revision",r.source.revision},{"source_tick_ms",r.source.tick},{"cpu_requested_world_points",r.source.points},
            {"camera_tick_ms",r.camera.available?Json(r.camera.tick):Json(nullptr)},{"camera_address",r.camera.available?Json(r.camera.address):Json(nullptr)},
            {"camera_associated_grip_record",r.camera.available?Json(r.camera.associated_source.record):Json(nullptr)},
            {"camera_world",r.camera.available?Json(r.camera.world):Json(nullptr)},{"camera_vertical_fov",r.camera.available?Json(r.camera.fov):Json(nullptr)},
            {"viewport",{r.width,r.height}},{"spatial_available",r.spatial_available},{"projected_points",r.spatial_available?pixels:Json(nullptr)}});}
    Json reasons=Json::object(),observations=Json::array();
    for(unsigned i=0;i<camera_reason_names.size();++i)reasons[camera_reason_names[i]]=state.camera_reason_counts[i];
    for(std::size_t i=0;i<state.camera_diagnostic_count;++i){const auto& d=state.camera_diagnostics[i];
        Json input=nullptr;
        if(d.camera_values_observed){
            Json world=Json::array(),bits=Json::array();
            for(float value:d.observed_camera_world){
                world.push_back(std::isfinite(value)?Json(value):Json(nullptr));
                bits.push_back(std::bit_cast<std::uint32_t>(value));
            }
            input=Json{{"world",std::move(world)},{"world_float32_bits",std::move(bits)},
                {"vertical_fov",std::isfinite(d.observed_camera_fov)?Json(d.observed_camera_fov):Json(nullptr)},
                {"vertical_fov_float32_bits",std::bit_cast<std::uint32_t>(d.observed_camera_fov)}};
        }
        observations.push_back(Json{{"reason",camera_reason_names[static_cast<unsigned>(d.reason)]},{"issue",d.issue.data()},
            {"camera_values_observed",d.camera_values_observed},{"camera_validation_input",std::move(input)},
            {"tick_ms",d.tick},{"grip_record",d.source.record},{"listener_serial",d.source.serial},{"listener_time",d.source.native_time},
            {"sdk_frame_placement_revision",d.source.revision},{"source_tick_ms",d.source.tick},{"source_base",d.source.base},
            {"source_owner",d.source.owner},{"can_control",d.can_control},{"camera_phase",d.camera_phase},
            {"ambiguous",d.ambiguous},{"park_editor",d.park_editor},{"body_chain_read",d.body_chain_read},{"body_owner_verified",d.body_owner_verified}});}
    Json diagnostic{{"capacity",camera_diagnostic_capacity},{"qualified_debug_entries",state.camera_diagnostic_entries},
        {"stored",state.camera_diagnostic_count},{"capacity_drops",state.camera_diagnostic_drops},
        {"reason_counts",std::move(reasons)},{"observations",std::move(observations)},
        {"boundary","Default-off HUD metadata only; retained CPU request owner is not a new native-owner read. Zero entries does not identify a cause. Try-lock/freshness losses can omit receipts."}};
    return Json{{"schema",1},{"process_id",GetCurrentProcessId()},{"process_creation_filetime",std::to_string(state.creation)},
        {"capacity",capacity},{"count",state.count},{"contention_drops",state.contention.load()},{"camera_diagnostic",std::move(diagnostic)},
        {"point_order",{"requested_left_grip","requested_right_grip","requested_rear_axle","requested_front_axle"}},
        {"rendered_identity_proven",false},{"camera_gpu_join_proven",false},{"projection_qualified_by_pixels",false},
        {"boundary","CPU requested points and CPU camera only. Validate axle/reference anchors before interpreting pixels; native/GPU hand, bike and camera identities are unproved."},{"rows",std::move(rows)}};
}
}
