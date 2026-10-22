#pragma once
// Explicit private CPU entity-matrix observer. No renderer/GPU completion claim.
#include "Extension/Multiplayer/Remote/native_prop.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "bmx_visual.h"
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#ifdef _MSC_VER
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
#endif

namespace dingosdk::bmx::world::prop_transform_trace {
inline constexpr std::size_t capacity=64,parts=8;
inline constexpr std::uint64_t sample_interval_ms=100,window_limit_ms=20000;
inline constexpr wchar_t private_root[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-chassis-probe";
struct ErrorGuard{DWORD saved=GetLastError();~ErrorGuard(){SetLastError(saved);}};
struct FrameReadback {
    std::uintptr_t base{},client{},rider{};std::uint64_t revision{},mount_token{},wall_ms{},utc_filetime{};
    visual::Frame requested_frame{};
    std::array<multiplayer::ClientPropTransformReadback,parts> parts_readback{};
    bool finished{},scope_still_selected{},ready{};unsigned created{},placed{},live{};
};
struct Store {
    std::array<FrameReadback,capacity> rows{};std::size_t count{};
    std::uint64_t creation{},started{},last{};DWORD thread{};
    bool done{},exported{};const char* stop="not_started";
};
static_assert(std::is_trivially_copyable_v<FrameReadback>);
static_assert(sizeof(Store)<256*1024);
inline Store store;
inline bool flag(const wchar_t* name) noexcept {wchar_t v[2]{};return GetEnvironmentVariableW(name,v,2)==1&&v[0]==L'1';}
inline bool selected() noexcept {
    ErrorGuard error;
    // A process launched without the explicit flag never begins polling paths,
    // allocating observation rows, or reading additional native state.
    static const bool requested=flag(L"RESKATE_BMX_PROP_TRANSFORM_TRACE");
    return requested&&flag(L"RESKATE_BMX_PROP_TRANSFORM_TRACE")&&flag(L"RESKATE_BMX_NATIVE_CHASSIS")&&
        user_data::runtime_development_selected(L"bmx-user-data-chassis-probe",private_root);
}
inline FrameReadback* begin(std::uintptr_t base,std::uintptr_t client,const visual::Frame& frame,std::uint64_t revision) noexcept {
    ErrorGuard error;if(!selected()||store.done||store.exported||!frame.active||!frame.entity||!base||!client)return nullptr;
    const auto thread=GetCurrentThreadId();if(store.thread&&store.thread!=thread)return nullptr;
    const auto now=GetTickCount64();
    if(!store.thread){
        FILETIME c{},e{},k{},u{};if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u))return nullptr;
        store.creation=(std::uint64_t{c.dwHighDateTime}<<32)|c.dwLowDateTime;if(!store.creation)return nullptr;
        store.thread=thread;store.started=now;store.stop="recording";
    }
    if(now<store.started||now-store.started>=window_limit_ms){store.done=true;store.stop="bounded_wall_window_complete";return nullptr;}
    if(store.count>=capacity){store.done=true;store.stop="fixed_capacity_complete";return nullptr;}
    if(store.count&&(now<store.last||now-store.last<sample_interval_ms))return nullptr;
    auto& row=store.rows[store.count++];row={};row.base=base;row.client=client;row.rider=frame.entity;
    row.revision=revision;row.mount_token=frame.mount_token;row.requested_frame=frame;row.wall_ms=now;
    FILETIME utc{};GetSystemTimePreciseAsFileTime(&utc);row.utc_filetime=(std::uint64_t{utc.dwHighDateTime}<<32)|utc.dwLowDateTime;
    store.last=now;return &row;
}
inline void finish(FrameReadback* row,const multiplayer::ClientPropBatchResult& result) noexcept {
    ErrorGuard error;if(!row||!store.count||row!=&store.rows[store.count-1]||GetCurrentThreadId()!=store.thread)return;
    row->finished=true;row->scope_still_selected=selected();row->ready=result.ready;
    row->created=result.created;row->placed=result.placed;row->live=result.live;
}
inline void number(std::ostream& out,float n){if(std::isfinite(n))out<<n;else out<<"null";}
template<class T,std::size_t N>inline void array(std::ostream& out,const std::array<T,N>& a){
    out<<'[';for(std::size_t i=0;i<N;++i){if(i)out<<',';if constexpr(std::is_floating_point_v<T>)number(out,a[i]);else out<<a[i];}out<<']';
}
inline std::string serialize() {
    std::ostringstream out;out<<std::setprecision(std::numeric_limits<float>::max_digits10)<<std::boolalpha;
    out<<"{\"schema\":1,\"event\":\"bmx_private_entity_transform_readback\",\"process_id\":"<<GetCurrentProcessId()
       <<",\"creation_filetime\":\""<<store.creation<<'"'
       <<R"(,"private_root":"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-chassis-probe")"
       <<",\"capacity\":"<<capacity<<",\"count\":"<<store.count<<",\"sample_interval_ms\":"<<sample_interval_ms
       <<",\"window_limit_ms\":"<<window_limit_ms<<",\"stop\":\""<<store.stop<<"\",\"native_clock_available\":false"
       <<",\"native_serial\":null,\"native_time\":null,\"exact_contact_clock_join\":false,\"renderer_completion_proven\":false,\"gpu_completion_proven\":false,\"rows\":[";
    for(std::size_t i=0;i<store.count;++i){if(i)out<<',';const auto& r=store.rows[i];
        out<<"{\"base\":"<<r.base<<",\"client\":"<<r.client<<",\"rider_entity\":"<<r.rider<<",\"placement_revision\":";
        if(r.revision)out<<r.revision;else out<<"null";out<<",\"mount_token\":";if(r.mount_token)out<<r.mount_token;else out<<"null";
        out<<",\"wall_ms\":"<<r.wall_ms<<",\"utc_filetime\":\""<<r.utc_filetime<<"\",\"finished\":"<<r.finished
           <<",\"scope_still_selected\":"<<r.scope_still_selected<<",\"batch_ready\":"<<r.ready<<",\"batch_created\":"<<r.created
           <<",\"batch_placed\":"<<r.placed<<",\"batch_live\":"<<r.live<<",\"requested_bike_root\":";array(out,r.requested_frame.position);
        out<<",\"requested_chassis_quaternion\":";if(r.requested_frame.chassis_quaternion_available)array(out,r.requested_frame.chassis_quaternion);else out<<"null";
        out<<",\"steer\":";number(out,r.requested_frame.steer);out<<",\"crank_phase\":";number(out,r.requested_frame.crank_phase);
        out<<",\"front_wheel_phase\":";number(out,r.requested_frame.front_wheel_phase);out<<",\"rear_wheel_phase\":";number(out,r.requested_frame.rear_wheel_phase);
        out<<",\"parts\":[";
        for(std::size_t j=0;j<parts;++j){if(j)out<<',';const auto& p=r.parts_readback[j];
            const bool coherent=p.attempted&&p.bytes_read&&p.owner_fenced&&p.collection_fenced&&p.matrix_unchanged;
            out<<"{\"part_index\":"<<j<<",\"attempted\":"<<p.attempted<<",\"bytes_read\":"<<p.bytes_read<<",\"owner_fenced\":"<<p.owner_fenced
               <<",\"collection_fenced\":"<<p.collection_fenced<<",\"matrix_unchanged_during_read\":"<<p.matrix_unchanged
               <<",\"matrix_finite\":"<<p.matrix_finite<<",\"matrix_valid\":"<<p.matrix_valid<<",\"coherent_entity_matrix\":"<<coherent
               <<",\"issue\":\""<<p.issue<<"\",\"prop_identity\":";
            if(p.attempted)out<<'['<<p.owner.base<<','<<p.owner.entity<<','<<p.owner.parent<<','<<p.owner.context<<','<<p.owner.generation<<']';else out<<"null";
            out<<",\"collection\":";if(coherent)out<<p.collection;else out<<"null";
            out<<",\"collection_indices\":";if(coherent)out<<'['<<unsigned(p.count)<<','<<unsigned(p.first)<<','<<unsigned(p.extra)<<']';else out<<"null";
            out<<",\"requested_matrix\":";if(p.attempted)array(out,p.requested);else out<<"null";
            out<<",\"observed_entity_matrix\":";if(coherent&&p.matrix_finite)array(out,p.observed);else out<<"null";
            out<<",\"observed_raw_words\":";
            if(coherent){std::array<std::uint32_t,16> words{};std::memcpy(words.data(),p.observed.data(),sizeof(words));array(out,words);}else out<<"null";
            out<<",\"requested_words_equal\":";if(coherent)out<<(std::memcmp(p.requested.data(),p.observed.data(),sizeof(p.requested))==0);else out<<"null";
            out<<'}';
        }
        out<<"]}";
    }
    out<<"],\"boundary\":\"Immediate stored native entity matrices only. Not atomic native state, renderer/GPU consumption, measured shoe contact or an exact native-clock/contact join.\"}";
    return out.str();
}
// Client idle path only, after update_client_props returned and released its lock.
inline void poll_export(bool idle) noexcept {
    ErrorGuard error;if(!idle||!store.count||store.exported||GetCurrentThreadId()!=store.thread||!selected())return;
    store.done=true;if(std::strcmp(store.stop,"recording")==0)store.stop="bike_became_inactive_partial_window";
    try {
        const auto fresh=user_data::override_root();
        if(!fresh||fresh->native()!=private_root||!selected())return;
        const auto bytes=serialize();if(bytes.size()>1024*1024){store.stop="bounded_export_size_refused";return;}
        const auto name=*fresh/(L"bmx-prop-transform-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(store.creation)+L".json");
        const auto temporary=std::filesystem::path(name.native()+L".tmp");
        const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE){store.exported=true;return;}
        DWORD wrote{};const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&wrote,nullptr)&&wrote==bytes.size()&&FlushFileBuffers(file);
        CloseHandle(file);store.exported=true;if(ok)(void)MoveFileExW(temporary.c_str(),name.c_str(),MOVEFILE_WRITE_THROUGH);
    }catch(...){store.exported=true;}
}
} // namespace dingosdk::bmx::world::prop_transform_trace
