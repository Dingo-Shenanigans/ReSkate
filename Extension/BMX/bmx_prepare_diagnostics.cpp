#include "bmx_prepare_diagnostics.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>
#include <sstream>

namespace dingosdk::bmx {
namespace {
using Prepare=void(__fastcall*)(std::uintptr_t,std::uintptr_t);
using Lookup=std::uintptr_t(__fastcall*)(std::uintptr_t);
using Deliver=void(__fastcall*)(std::uintptr_t,std::uintptr_t);
constexpr std::uintptr_t prepare_rva=0x7fdf80,lookup_rva=0x526a00,deliver_rva=0x7e9490;
constexpr std::array<std::uint8_t,32> prepare_prefix{
    0x40,0x55,0x53,0x57,0x41,0x54,0x48,0x8d,0x6c,0x24,0xc1,0x48,0x81,0xec,0xd8,0x00,
    0x00,0x00,0x48,0x8b,0x05,0x27,0x64,0x9c,0x06,0x48,0x33,0xc4,0x48,0x89,0x45,0x2f};
constexpr std::array<std::uint8_t,32> lookup_prefix{
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x30,0x48,0x8d,0x99,0x90,0x04,0x00,0x00,
    0x48,0x8b,0xf9,0x48,0x8b,0xcb,0xe8,0x64,0x93,0x06,0x01,0x41,0xb9,0x11,0x00};
constexpr std::array<std::uint8_t,32> deliver_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,
    0xf1,0x48,0x8b,0xda,0x48,0x8d,0x4c,0x24,0x20,0xe8,0xf1,0xbd,0x5d,0x03,0x48};
struct State {
    std::atomic<Prepare> prepare{};std::atomic<Lookup> lookup{};std::atomic<Deliver> deliver{};
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> prepared{};
    std::atomic<const char*> status{"not_requested"};
    std::atomic<std::uint64_t> entered{},exited{},lookup_calls{},exact_lookup{},lookup_in_scope{},present{},missing{},
        flag_unreadable{},flag_zero{},flag_nonzero{},deliver_calls{},exact_delivery{},delivery_in_scope{},error_unreadable{};
    std::atomic<std::uint32_t> last_flag{},last_error_code{};
    std::atomic<bool> flag_available{},error_available{};
    std::mutex initialization;
    bool attempted{};
};
State& state(){static auto* value=new State;return *value;}
thread_local unsigned prepare_depth{};
void increment(std::atomic<std::uint64_t>& value) noexcept {value.fetch_add(1,std::memory_order_relaxed);}
struct ErrorRestore {DWORD value{GetLastError()};~ErrorRestore(){SetLastError(value);}};

// Native binding09ed0cc5 declares EventInfo (48-byte value) and Persistence
// (class reference storage). Adapter5932b20 loads args[0]/args[1] into RCX/RDX;
// 5932b30 jumps directly here. The full body uses no incoming R8/R9/stack args.
void __fastcall observe_prepare(std::uintptr_t event_info,std::uintptr_t persistence) {
    const auto incoming=GetLastError();auto& s=state();
    const auto original=s.prepare.load(std::memory_order_acquire);
    increment(s.entered);++prepare_depth;
    struct Scope {~Scope(){--prepare_depth;}} scope;
    SetLastError(incoming);original(event_info,persistence);
    const auto result_error=GetLastError();increment(s.exited);SetLastError(result_error);
}
//526a00 has one registry argument and returns its borrowed character_loadout
// row pointer after releasing the registry lock. No ownership is transferred.
std::uintptr_t __fastcall observe_lookup(std::uintptr_t registry) {
    const auto incoming=GetLastError();const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto& s=state();const auto original=s.lookup.load(std::memory_order_acquire);
    increment(s.lookup_calls);SetLastError(incoming);
    const auto result=original(registry);const auto result_error=GetLastError();
    if(caller==s.base.load(std::memory_order_acquire)+0x7fdfc1){
        increment(s.exact_lookup);if(prepare_depth)increment(s.lookup_in_scope);
        if(result){
            increment(s.present);std::uint8_t flag{};
            const bool available=result<=memory::highest_user_address-0x99&&memory::peek(result+0x98,flag);
            s.flag_available.store(available,std::memory_order_relaxed);
            if(available){s.last_flag.store(flag,std::memory_order_relaxed);increment(flag?s.flag_nonzero:s.flag_zero);}
            else increment(s.flag_unreadable);
        }else {increment(s.missing);s.flag_available.store(false,std::memory_order_relaxed);}
    }
    SetLastError(result_error);return result;
}
//7e9490's complete body reads Error from RCX and EventInfo from RDX, performs
// native retain/copy/delivery/release, and returns void. Only7fe34d's call is
// attributed to this preparation fallback; other callers merely pass through.
void __fastcall observe_delivery(std::uintptr_t error,std::uintptr_t event_info) {
    const auto incoming=GetLastError();const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto& s=state();const auto original=s.deliver.load(std::memory_order_acquire);increment(s.deliver_calls);
    if(caller==s.base.load(std::memory_order_acquire)+0x7fe352){
        increment(s.exact_delivery);if(prepare_depth)increment(s.delivery_in_scope);
        std::uint32_t code{};const bool available=error&&error<=memory::highest_user_address-0x24&&memory::peek(error+0x20,code);
        s.error_available.store(available,std::memory_order_relaxed);
        if(available)s.last_error_code.store(code,std::memory_order_relaxed);else increment(s.error_unreadable);
    }
    SetLastError(incoming);original(error,event_info);
    // No diagnostic calls after the original: its LastError returns unchanged.
}
bool prefix(std::uintptr_t address,const std::array<std::uint8_t,32>& expected) noexcept {
    std::array<std::uint8_t,32> actual{};return memory::peek_bytes(address,actual.data(),actual.size())&&actual==expected;
}
}

bool initialize_prepare_diagnostics(std::uintptr_t base) noexcept {
    ErrorRestore preserve;auto& s=state();
    try {
        std::lock_guard lock(s.initialization);
        if(s.attempted)return s.prepared.load(std::memory_order_acquire)&&s.base.load(std::memory_order_acquire)==base;
        s.attempted=true;std::array<wchar_t,2> flag{};
        if(!user_data::development_requested()||!user_data::override_root()||
            GetEnvironmentVariableW(L"RESKATE_BMX_FLOW_DIAGNOSTICS",flag.data(),static_cast<DWORD>(flag.size()))!=1||flag[0]!=L'1')return false;
        if(!supported_build::running_image_matches(base)){s.status.store("image_rejected");return false;}
        if(!prefix(base+prepare_rva,prepare_prefix)||!prefix(base+lookup_rva,lookup_prefix)||!prefix(base+deliver_rva,deliver_prefix)){
            s.status.store("prefix_rejected");return false;
        }
        std::uint32_t binding{};std::uintptr_t function{};
        std::array<std::uint8_t,5> lookup_call{},delivery_call{};
        if(!memory::peek(base+0x7f19ad0,binding)||binding!=0x09ed0cc5||
            !memory::peek(base+0x7f19b10,function)||function!=base+prepare_rva||
            !memory::peek(base+0x7fdfbc,lookup_call)||lookup_call!=std::array<std::uint8_t,5>{0xe8,0x3f,0x8a,0xd2,0xff}||
            !memory::peek(base+0x7fe34d,delivery_call)||delivery_call!=std::array<std::uint8_t,5>{0xe8,0x3e,0xb1,0xfe,0xff}){
            s.status.store("binding_rejected");return false;
        }
        const std::array targets{reinterpret_cast<void*>(base+prepare_rva),reinterpret_cast<void*>(base+lookup_rva),reinterpret_cast<void*>(base+deliver_rva)};
        const std::array replacements{reinterpret_cast<void*>(&observe_prepare),reinterpret_cast<void*>(&observe_lookup),reinterpret_cast<void*>(&observe_delivery)};
        std::array<void*,3> relays{};std::size_t count{};
        for(;count<targets.size();++count)if(hook_prepare(targets[count],replacements[count],&relays[count])!=HookOk||!relays[count])break;
        if(count!=targets.size()){
            while(count)(void)hook_remove(targets[--count]);s.status.store("prepare_failed");return false;
        }
        s.base.store(base,std::memory_order_release);
        s.prepare.store(reinterpret_cast<Prepare>(relays[0]),std::memory_order_release);
        s.lookup.store(reinterpret_cast<Lookup>(relays[1]),std::memory_order_release);
        s.deliver.store(reinterpret_cast<Deliver>(relays[2]),std::memory_order_release);
        for(auto* target:targets)if(hook_enable(target)!=HookOk){
            for(auto* rollback:targets)(void)hook_remove(rollback);s.status.store("enable_failed");return false;
        }
        s.prepared.store(true,std::memory_order_release);s.status.store("ready");return true;
    }catch(...){s.status.store("initialization_failed");return false;}
}

std::string prepare_diagnostics() {
    ErrorRestore preserve;const auto& s=state();std::ostringstream out;
    const auto value=[](const auto& counter){return counter.load(std::memory_order_relaxed);};
    out<<"Native character preparation observer: prepared="<<value(s.prepared)<<"; status="<<value(s.status)
        <<"; entered="<<value(s.entered)<<"; exited="<<value(s.exited)
        <<"; lookup_calls="<<value(s.lookup_calls)<<"; exact_lookup="<<value(s.exact_lookup)
        <<"; lookup_in_prepare_scope="<<value(s.lookup_in_scope)<<"; config_present="<<value(s.present)<<"; config_missing="<<value(s.missing)
        <<"; flag98_zero="<<value(s.flag_zero)<<"; flag98_nonzero="<<value(s.flag_nonzero)<<"; flag98_unreadable="<<value(s.flag_unreadable)
        <<"; last_raw_flag98=";
    if(value(s.flag_available))out<<value(s.last_flag);else out<<"unavailable";
    out<<"; delivery_calls="<<value(s.deliver_calls)<<"; exact_fallback_delivery="<<value(s.exact_delivery)
        <<"; delivery_in_prepare_scope="<<value(s.delivery_in_scope)<<"; fallback_error_unreadable="<<value(s.error_unreadable)
        <<"; last_raw_fallback_error_code=";
    if(value(s.error_available))out<<value(s.last_error_code);else out<<"unavailable";
    return out.str();
}
}
