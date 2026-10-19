#include "native_skate_filter.h"
#include "skate_detector.h"
#include "trick_logger.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <array>
#include <map>
#include <mutex>
#include <stdexcept>
namespace dingosdk::skate {
namespace {
using Address=std::uintptr_t;
using Contains=std::uintptr_t (*)(const void*,const void*,const void*,std::uint8_t*);
struct Binding {Contains original,ignored;};
std::mutex bindings_mutex;
std::map<Address,Binding> bindings;
template<class T> T read(Address at) {
    T v{};if(!memory::peek(at,v))throw std::runtime_error("Native skate category binding unavailable");return v;
}
bool write_pointer(Address at,Address value) noexcept {
    __try {*reinterpret_cast<Address*>(at)=value;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool write_bool(std::uint8_t* at,bool value) noexcept {
    __try {*at=value?1:0;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
std::uintptr_t ignored_contains(const void* array,const void* value,const void* type,std::uint8_t* out) {
    Contains original{};
    try {
        const auto resource=read<Address>(profile_runtime::executing_expression+0x38);
        {std::lock_guard lock(bindings_mutex);const auto it=bindings.find(resource);if(it!=bindings.end())original=it->second.ignored;}
        if(!original)throw std::runtime_error("Missing native ignored-trick function");
    } catch(...) {write_bool(out,true);bridge_error();return 0;}
    const auto result=original(array,value,type,out);
    try {
        // ScorableName is a native CString. ArrayContains receives addresses of
        // the value and TypeInfo registers, rather than their pointees.
        const auto info=read<Address>(reinterpret_cast<Address>(type));
        const auto meta=read<Address>(info);
        if(((read<std::uint16_t>(meta+4)>>5)&31)!=7 || read<std::uint16_t>(meta+6)!=8)
            throw std::runtime_error("Native ScorableName is not CString");
        auto pointer=read<Address>(reinterpret_cast<Address>(value));std::string name;
        if(pointer)for(unsigned i=0;i<256;++i) {
            const auto c=read<char>(pointer+i);if(!c)break;name.push_back(c);
        }
        if(name.size()==256)throw std::runtime_error("Native ScorableName is unbounded");
        if(const auto ignored=native_ignore_observation(name)) {
            const auto previous=read<std::uint8_t>(reinterpret_cast<Address>(out));
            if(previous>1 || !write_bool(out,*ignored))throw std::runtime_error("Invalid native ignored-trick result");
            trick_logger::native_observation(name,previous!=0,*ignored);
        }
    } catch(...) {write_bool(out,true);bridge_error();}
    return result;
}
std::uintptr_t contains(const void* array,const void* value,const void* type,std::uint8_t* out) {
    Contains original{};
    try {
        const auto resource=read<Address>(profile_runtime::executing_expression+0x38);
        {std::lock_guard lock(bindings_mutex);const auto it=bindings.find(resource);if(it!=bindings.end())original=it->second.original;}
        if(!original)throw std::runtime_error("Missing native skate category function");
    } catch(...) {write_bool(out,false);bridge_error();return 0;}
    const auto result=original(array,value,type,out);
    try {
        const auto category=read<std::uint32_t>(reinterpret_cast<Address>(value));
        if(const auto allowed=native_category_permission(category)) {
            const auto previous=read<std::uint8_t>(reinterpret_cast<Address>(out));
            if(previous>1 || !write_bool(out,*allowed))throw std::runtime_error("Invalid native category result");
            trick_logger::native_filter(category,previous!=0,*allowed);
        }
    } catch(...) {write_bool(out,false);bridge_error();}
    return result;
}
}
bool prepare_native_filter(std::uintptr_t vm) noexcept {
    if(!active())return true;
    try {
        const auto resource=read<Address>(vm+0x38);
        if(!resource || read<std::uint32_t>(resource+0x10)!=0x7841abd7)return true;
        const auto instance=read<Address>(vm+0x30);
        if(!instance || read<Address>(instance)!=resource)throw std::runtime_error("Native skate instance mismatch");
        // Actual supported ClientSkateThrowdown.OnCompositeTrickScoringEvent.
        constexpr std::array<std::uint32_t,10> header{64,472,466,781,6,0,0,655414,5177355,2048};
        if(read<std::array<std::uint32_t,10>>(resource+0x20)!=header || read<std::uint32_t>(resource+0x3b8)!=0xc27 ||
           read<std::uint16_t>(resource+0x3c)!=54)throw std::runtime_error("Native skate graph layout changed");
        std::array<unsigned,3> sites{};unsigned count{};
        for(unsigned i=0;i<54;++i)if(read<std::uint32_t>(resource+0xfec+i*8)==0xb0b4285a) {
            if(count>=sites.size())throw std::runtime_error("Native category call count changed");
            sites[count++]=read<std::uint32_t>(resource+0xff0+i*8);
        }
        if(count!=3 || sites!=std::array<unsigned,3>{0x190,0x248,0x2cc})throw std::runtime_error("Native category call sites changed");
        constexpr std::array<std::uint32_t,8> args{2,0x78,2,0x174,0,0x78,2,0x1bc};
        const auto code=resource+0x3b8;
        if(read<std::uint32_t>(code+0x190)!=0x1bc03 ||
           read<std::array<std::uint32_t,8>>(code+0x190+12)!=std::array<std::uint32_t,8>{2,0xe0,5,0,0,0x40,2,0x1b9} ||
           read<std::uint32_t>(code+0x14c)!=0x18004 ||
           read<std::array<std::uint32_t,10>>(code+0x14c+12)!=std::array<std::uint32_t,10>{2,0x80,0,0x30,0,0x170,0,0x38,2,0xd8} ||
           read<std::array<std::uint32_t,4>>(code+0x180)!=std::array<std::uint32_t,4>{0x1902e,2,0xd8,5} ||
           read<std::array<std::uint32_t,4>>(code+0x1d8)!=std::array<std::uint32_t,4>{0x1e826,2,0x1ba,0x31c})
            throw std::runtime_error("Native ignored-trick operands changed");
        if(read<std::uint32_t>(code+0x2cc)!=0x2f803 || read<std::array<std::uint32_t,8>>(code+0x2cc+12)!=args ||
           read<std::uint32_t>(code+0x21c)!=0x24803 ||
           read<std::array<std::uint32_t,8>>(code+0x21c+12)!=std::array<std::uint32_t,8>{2,0x80,0,0x58,0,0x178,2,0x174} ||
           read<std::uint32_t>(code+0x298)!=0x2cc04 ||
           read<std::array<std::uint32_t,10>>(code+0x298+12)!=std::array<std::uint32_t,10>{3,0,0,0x68,0,0x17c,0,0x70,2,0x78})
            throw std::runtime_error("Native allowed-category operands changed");
        std::lock_guard lock(bindings_mutex);
        const auto slot=code+0x2cc+4,ignore_slot=code+0x190+4;
        const auto current=read<Address>(slot),current_ignore=read<Address>(ignore_slot);
        if(current==reinterpret_cast<Address>(&contains) && current_ignore==reinterpret_cast<Address>(&ignored_contains)) {
            if(!bindings.contains(resource))throw std::runtime_error("Unbound native category adapter");
            return true;
        }
        if(!current || !current_ignore || current==reinterpret_cast<Address>(&contains) || current_ignore==reinterpret_cast<Address>(&ignored_contains))
            throw std::runtime_error("Native filter function unresolved or partially bound");
        bindings[resource]={reinterpret_cast<Contains>(current),reinterpret_cast<Contains>(current_ignore)};
        if(!write_pointer(ignore_slot,reinterpret_cast<Address>(&ignored_contains)) || !write_pointer(slot,reinterpret_cast<Address>(&contains)))
            throw std::runtime_error("Native filter slot unavailable");
        logging::write(logging::Level::info,logging::Channel::progression,
            "S.K.A.T.E.: native observation/category adapters installed at graph 7841abd7 pc 190/2cc; native area and submission unchanged.");
        return true;
    } catch(const std::exception& error) {
        logging::log(logging::Level::warning,logging::Channel::progression,"S.K.A.T.E. native category adapter refused: {}",error.what());
        bridge_error();return false;
    } catch(...) {bridge_error();return false;}
}
void native_filter_completed(std::uintptr_t vm,std::uint32_t pc) noexcept {
    if(pc || !active() || !trick_logger::full_capture())return;
    try {
        const auto resource=read<Address>(vm+0x38);
        if(!resource || read<std::uint32_t>(resource+0x10)!=0x7841abd7)return;
        {std::lock_guard lock(bindings_mutex);if(!bindings.contains(resource))return;}
        const auto instance=read<Address>(vm+0x30);
        if(read<Address>(instance)!=resource)return;
        const auto locals=read<Address>(instance+80); // frame 64; page 2
        const auto banned=read<std::uint8_t>(locals+0x1b6),ignored=read<std::uint8_t>(locals+0x1b7),allowed=read<std::uint8_t>(locals+0x1b8);
        if(banned>1 || ignored>1 || allowed>1)return;
        trick_logger::native_capture_trace(ignored,banned,allowed,read<std::uint32_t>(locals+0x134));
    } catch(...) {} // Diagnostic reads never change native eligibility.
}

}
