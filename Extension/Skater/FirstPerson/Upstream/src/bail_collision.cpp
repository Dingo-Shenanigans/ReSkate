#include "Extension/Skater/FirstPerson/Upstream/src/bail_collision.h"
#include "Extension/Skater/FirstPerson/Upstream/src/bail_collision_contract.h"
#include "Extension/Skater/FirstPerson/Upstream/src/mod.h"
#include "Engine/Core/Platform/memory.h"
#include <atomic>
#include <chrono>
#include <cstddef>
namespace jishsfirstpersonmod {
namespace {
#pragma warning(push)
#pragma warning(disable:4324) // Verified native query ABI includes explicit 16-byte padding.
struct alignas(16) SphereRequest {
    std::array<float,4> start{},end{};
    float radius{};std::uint32_t filter{0x40080303},mode{1};float tolerance{};
    std::uint32_t flags{};std::uint32_t padding{};
    std::uintptr_t allocator_vtable{},allocator{};
    std::array<std::uintptr_t,5> ignored{};std::uint64_t tail{};
};
struct alignas(16) QueryResult {
    std::uintptr_t world{},data{},allocator_vtable{},allocator{};
    std::uint32_t first{},count{};std::uint64_t padding{};
    std::array<std::uintptr_t,4> scope{};
};
#pragma warning(pop)
static_assert(offsetof(SphereRequest,radius)==0x20 && offsetof(SphereRequest,ignored)==0x48);
static_assert(sizeof(SphereRequest)==0x80 && offsetof(QueryResult,scope)==0x30 && sizeof(QueryResult)==0x50);
using Context=void*(*)(std::uintptr_t*);
using World=std::uintptr_t(*)(std::uintptr_t);
using Sphere=void*(*)(std::uintptr_t,QueryResult*,const SphereRequest*,const char*);
using Allocator=std::uintptr_t(*)();
using Release=void(*)(void*,std::uintptr_t);
Context context_of{};World world_of{};Sphere sphere{};Allocator allocator_of{};Release release{};
std::atomic<bool> ready{};std::atomic<DWORD> engine_thread{};
std::atomic<unsigned long long> last_report{};
std::atomic<bool> context_reported{};
bool current_world(std::uintptr_t* world) noexcept {
    __try {
        std::uintptr_t context{};context_of(&context);
        *world=context ? world_of(context):0;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Only POD locals here: SEH must not bypass C++ destructors.
bool execute(std::uintptr_t world,QueryResult* result,const SphereRequest* request) noexcept {
    __try { sphere(world,result,request,"Jish_BailCamera");return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool cleanup(QueryResult* result) noexcept {
    __try {
        if(result->scope[2]) {
            const auto allocator=allocator_of();std::uintptr_t table{},pop{};
            if(!dingosdk::memory::read(allocator,table) || !dingosdk::memory::read(table+8,pop) || !pop) return false;
            reinterpret_cast<void(*)(std::uintptr_t,void*)>(pop)(allocator,result->scope.data());
        }
        if(result->allocator) release(&result->allocator_vtable,result->data);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void fault() noexcept {
    if(ready.exchange(false)) log("Bail camera collision paused: native query/context/cleanup failed. Restart to retry.");
}
}
void initialize_bail_collision(std::uintptr_t base) noexcept {
    using namespace bail_collision_contract;
    const bool supported=verified([](std::uintptr_t address,void* bytes,std::size_t size) {
        return dingosdk::memory::read_bytes(address,bytes,size);
    },base);
    if(!supported) { log("Bail camera collision unavailable: game query fingerprints differ.");return; }
    context_of=reinterpret_cast<Context>(base+context_rva);world_of=reinterpret_cast<World>(base+world_rva);
    sphere=reinterpret_cast<Sphere>(base+sphere_rva);allocator_of=reinterpret_cast<Allocator>(base+allocator_rva);
    release=reinterpret_cast<Release>(base+release_rva);ready.store(true,std::memory_order_release);
    log("Bail camera collision query verified; radius=0.10m, clearance=0.01m. Awaiting engine-thread physics context.");
}
void observe_collision_engine_thread() noexcept {
    DWORD empty{};engine_thread.compare_exchange_strong(empty,GetCurrentThreadId());
}
bool bail_collision_available() noexcept { return ready.load(std::memory_order_acquire); }
std::uintptr_t bail_collision_world() noexcept {
    if(!bail_collision_available() || engine_thread.load()!=GetCurrentThreadId()) return 0;
    std::uintptr_t world{};
    if(!current_world(&world)) { fault();return 0; }
    if(world && !context_reported.exchange(true)) log("Bail camera collision: final camera write is on the observed engine thread with a live TLS physics world.");
    return world;
}
CollisionSweep native_bail_sweep(std::uintptr_t world,CollisionPoint from,CollisionPoint to,float radius) noexcept {
    if(!world || !bail_collision_available() || engine_thread.load()!=GetCurrentThreadId()) return {};
    for(unsigned i=0;i<3;++i) if(!std::isfinite(from[i]) || !std::isfinite(to[i]) || std::abs(from[i])>100000 || std::abs(to[i])>100000) return {};
    if(!std::isfinite(radius) || radius<=0 || radius>1) return {};
    SphereRequest request;std::copy(from.begin(),from.end(),request.start.begin());
    std::copy(to.begin(),to.end(),request.end.begin());request.radius=radius;
    QueryResult result;const auto start=std::chrono::steady_clock::now();
    const bool called=execute(world,&result,&request);
    CollisionSweep hit{called && result.world==world && result.count<=4096 && result.first<=UINT32_MAX-result.count,1};
    if(hit.valid && result.count) {
        std::uintptr_t fractions{};
        hit.valid=result.data && dingosdk::memory::read(result.data+8,fractions) && fractions;
        for(std::uint32_t n=0;hit.valid && n<result.count;++n) {
            float fraction{};
            hit.valid=dingosdk::memory::read(fractions+std::uintptr_t(result.first+n)*4,fraction) && std::isfinite(fraction) && fraction>=0 && fraction<=1;
            if(hit.valid) hit.fraction=std::min(hit.fraction,fraction);
        }
    }
    if(!cleanup(&result) || !called) { fault();return {}; }
    const auto microseconds=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    const auto now=GetTickCount64();auto previous=last_report.load();
    if((!hit.valid || microseconds>2000) && now-previous>10000 && last_report.compare_exchange_strong(previous,now))
        log("Bail camera collision query: valid="+std::to_string(hit.valid)+", hits="+std::to_string(result.count)+", time_us="+std::to_string(microseconds));
    return hit;
}
}
