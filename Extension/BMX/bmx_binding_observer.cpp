#include "bmx_binding_observer.h"
#include "bmx_binding_snapshot.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>

namespace dingosdk::bmx::binding_observer {
namespace {
// Source-exact supported FBCE74D5... image. Complete call-free body92B:
// REA ev_c3f463691358362fc9e49fe4c5b91e670085e8176ec232874200de5e138e8ebb.
// RCX=scope, EDX=32-bit hash, RAX=descriptor pointer or0. No owner/activation ABI.
using Lookup=std::uintptr_t (__fastcall*)(std::uintptr_t,std::uint32_t);
constexpr std::uintptr_t lookup_rva=0x16d0840;
constexpr std::array<unsigned char,92> lookup_code{
    0x4c,0x63,0xca,0x4c,0x8b,0xc1,0x66,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,
    0x41,0x8b,0x48,0x10,0x33,0xd2,0x4d,0x8b,0x50,0x08,0x49,0x8b,0xc1,0x48,0xf7,0xf1,
    0x8b,0xc2,0x49,0x8b,0x04,0xc2,0x48,0x85,0xc0,0x74,0x13,0x0f,0x1f,0x44,0x00,0x00,
    0x44,0x3b,0x08,0x74,0x0d,0x48,0x8b,0x40,0x10,0x48,0x85,0xc0,0x75,0xf2,0x49,0x8b,
    0x04,0xca,0x41,0x8b,0x48,0x10,0x49,0x3b,0x04,0xca,0x75,0x0b,0x4d,0x8b,0x00,0x4d,
    0x85,0xc0,0x75,0xbc,0x33,0xc0,0xc3,0x48,0x8b,0x40,0x08,0xc3};
constexpr unsigned per_key=32;
constexpr std::uint64_t capture_ms=120000;
struct Row {detail::Snapshot value;std::uint64_t elapsed_ms{};DWORD thread_id{};};
struct Slot {std::atomic<unsigned> ready{};Row row;};
struct Shared {
    std::atomic<Lookup> original{};
    std::atomic<bool> capturing{};
    std::array<std::atomic<std::uint64_t>,2> reservations{},dropped{};
    std::atomic<std::uint64_t> published{};
    std::array<Slot,per_key*2> slots;
    std::uintptr_t base{},image_size{};
    std::uint64_t started{},last_poll{},last_count{},last_dropped{};
    std::filesystem::path root;
    std::mutex poll_mutex;
    Status status;
    bool attempted{};
    bool last_capturing{};
};
// Constructed before hook installation. Callback performs no allocation,
// locking, logging, path validation, native activation or descriptor invocation.
Shared shared;
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
bool opted_flag() noexcept {
    wchar_t flag[2]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_BINDING_OBSERVER",flag,2)==1&&flag[0]==L'1'&&
       user_data::development_requested();
}
bool opted_root(std::filesystem::path& root) {
    if(!opted_flag())return false;
    const auto requested=user_data::override_root();
    if(!requested||_wcsicmp(requested->filename().c_str(),L"bmx-user-data-chassis-probe")!=0)return false;
    root=*requested;return true;
}
void capture(std::uintptr_t scope,std::uint32_t hash,std::uintptr_t descriptor) noexcept {
    if(!shared.capturing.load(std::memory_order_relaxed))return;
    const auto key=detail::key_index(hash);if(key==detail::keys.size())return;
    const auto now=GetTickCount64();
    if(now<shared.started||now-shared.started>capture_ms){shared.capturing.store(false,std::memory_order_relaxed);return;}
    const auto sequence=shared.reservations[key].fetch_add(1,std::memory_order_relaxed);
    if(sequence>=per_key){shared.dropped[key].fetch_add(1,std::memory_order_relaxed);return;}
    auto& slot=shared.slots[key*per_key+static_cast<unsigned>(sequence)];
    Row row;row.elapsed_ms=now-shared.started;row.thread_id=GetCurrentThreadId();
    row.value=detail::inspect(scope,hash,descriptor,shared.base,shared.image_size,
        [](std::uintptr_t p,void* out,std::size_t n) noexcept {return memory::peek_bytes(p,out,n);});
    slot.row=row;slot.ready.store(1,std::memory_order_release);
    shared.published.fetch_add(1,std::memory_order_release);
}
std::uintptr_t __fastcall lookup_hook(std::uintptr_t scope,std::uint32_t hash) {
    const auto incoming=GetLastError();
    const auto original=shared.original.load(std::memory_order_acquire);
    SetLastError(incoming);
    // Nonnull permanent relay is published BEFORE target enable. Original runs
    // once with unchanged args; no native exception is caught or synthesized.
    return detail::invoke_observed(scope,hash,original,&capture,
        []() noexcept {return GetLastError();},[](DWORD value) noexcept {SetLastError(value);});
}
}
bool initialize(std::uintptr_t base) noexcept {
    ErrorGuard error;std::lock_guard lock(shared.poll_mutex);
    if(shared.attempted)return shared.status.initialized;
    shared.attempted=true;
    try {
        if(!opted_root(shared.root))return false;
        shared.status.requested=true;
        if(!supported_build::running_image_matches(base)){shared.status.issue="unsupported_image";return false;}
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        if(!memory::peek(base,dos)||dos.e_lfanew<0||dos.e_lfanew>0x10000||
           !memory::peek(base+static_cast<std::uintptr_t>(dos.e_lfanew),nt)||
           !detail::span(base,nt.OptionalHeader.SizeOfImage)||
           lookup_rva+lookup_code.size()>nt.OptionalHeader.SizeOfImage){shared.status.issue="image_bounds_rejected";return false;}
        std::array<unsigned char,lookup_code.size()> actual{};
        if(!memory::peek(base+lookup_rva,actual)||actual!=lookup_code){shared.status.issue="lookup_contract_rejected";return false;}
        const auto init=hook_initialize();
        if(init!=HookOk&&init!=HookAlreadyInitialized){shared.status.issue="hook_initialize_failed";return false;}
        void* relay{};auto* target=reinterpret_cast<void*>(base+lookup_rva);
        if(hook_prepare(target,reinterpret_cast<void*>(&lookup_hook),&relay)!=HookOk||!relay){shared.status.issue="hook_prepare_failed";return false;}
        shared.base=base;shared.image_size=nt.OptionalHeader.SizeOfImage;shared.started=GetTickCount64();
        shared.original.store(reinterpret_cast<Lookup>(relay),std::memory_order_release);
        shared.capturing.store(true,std::memory_order_release);
        if(hook_enable(target)!=HookOk){
            shared.capturing.store(false,std::memory_order_release);(void)hook_remove(target);
            shared.status.issue="hook_enable_failed";return false;
        }
        shared.status.initialized=true;shared.status.capturing=true;shared.status.issue="capturing_read_only";
        return true;
    }catch(...){shared.capturing.store(false,std::memory_order_release);shared.status.issue="initialize_failed";return false;}
}
Status poll(bool force_write) noexcept {
    ErrorGuard error;std::lock_guard lock(shared.poll_mutex);
    if(!shared.status.requested)return shared.status;
    try {
        if(!opted_flag()){
            shared.capturing.store(false,std::memory_order_release);shared.status.capturing=false;
            shared.status.issue="opt_in_or_profile_changed";return shared.status;
        }
        const auto now=GetTickCount64();
        // Keep filesystem/root validation and retrying failed writes off the
        // per-frame path. The exact env opt-in above is checked every tick.
        if(!force_write&&shared.last_poll&&now>=shared.last_poll&&now-shared.last_poll<1000)return shared.status;
        shared.last_poll=now;
        std::filesystem::path current;
        if(!opted_root(current)||current!=shared.root){
            shared.capturing.store(false,std::memory_order_release);shared.status.capturing=false;
            shared.status.issue="opt_in_or_profile_changed";return shared.status;
        }
        if(now<shared.started||now-shared.started>=capture_ms)shared.capturing.store(false,std::memory_order_release);
        shared.status.capturing=shared.capturing.load(std::memory_order_acquire);
        shared.status.samples=shared.published.load(std::memory_order_acquire);
        shared.status.dropped=shared.dropped[0].load(std::memory_order_relaxed)+shared.dropped[1].load(std::memory_order_relaxed);
        // Cheap tick path: do not construct JSON rows when no report is due.
        if(!force_write&&shared.status.written&&shared.status.samples==shared.last_count&&
           shared.status.dropped==shared.last_dropped&&shared.status.capturing==shared.last_capturing)return shared.status;
        Json rows=Json::array();std::uint64_t count{};
        for(const auto& slot:shared.slots){
            if(!slot.ready.load(std::memory_order_acquire))continue;
            const auto& r=slot.row;const auto& v=r.value;++count;
            rows.push_back(Json{{"elapsed_ms",r.elapsed_ms},{"thread_id",r.thread_id},
                {"hash",v.hash},{"scope",v.scope},{"descriptor",v.descriptor},
                {"descriptor_present",v.descriptor_present},{"scope_header_coherent",v.scope_header_coherent},
                {"scope_words",Json::array({v.scope_words[0],v.scope_words[1],v.scope_words[2]})},
                {"descriptor_words",Json::array({v.descriptor_words[0],v.descriptor_words[1]})},
                {"descriptor_coherent",v.descriptor_coherent},{"descriptor_hash",v.descriptor_hash},
                {"descriptor_flags",v.descriptor_flags},{"function",v.function},
                {"function_in_image",v.function_in_image},{"function_rva",v.function_rva}});
        }
        shared.status.samples=count;
        Json report{{"event","bmx_binding_observation"},{"process_id",GetCurrentProcessId()},
            {"read_only",true},{"activation_implemented",false},{"source_sha256","fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9"},
            {"image_base",shared.base},{"image_size",shared.image_size},{"lookup_rva",lookup_rva},
            {"initialized",shared.status.initialized},{"capturing",shared.status.capturing},
            {"issue",shared.status.issue},{"capacity_per_hash",per_key},{"capture_wall_limit_ms",capture_ms},
            {"samples",count},{"dropped",shared.status.dropped},{"rows",std::move(rows)}};
        std::ofstream file(shared.root/L"bmx-binding-observation.json",std::ios::binary|std::ios::trunc);
        const auto text=report.dump(2);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();
        if(!file){shared.status.issue="report_write_failed";return shared.status;}
        shared.status.written=true;shared.last_count=count;
        shared.last_dropped=shared.status.dropped;shared.last_capturing=shared.status.capturing;
    }catch(...){shared.status.issue="report_failed";}
    return shared.status;
}
}
