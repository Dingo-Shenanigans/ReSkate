#include "Extension/BMX/bmx_binding_snapshot.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace d=dingosdk::bmx::binding_observer::detail;
namespace {
constexpr std::uintptr_t scope_address=0x100000,descriptor_address=0x200000,image_address=0x140000000;
// Keep both outcomes reachable under optimized MSVC without disabling C4702.
volatile bool throw_requested=true;
struct Memory {
    std::array<std::uint64_t,3> scope{0,0x300000,257};
    std::array<std::uint64_t,2> descriptor{image_address+0x1895670,(std::uint64_t{7}<<32)|d::keys[1]};
    unsigned reads{},descriptor_reads{},scope_reads{};
    bool mutate_descriptor{},mutate_scope{},unreadable{};
    bool read(std::uintptr_t p,void* out,std::size_t n) noexcept {
        ++reads;if(unreadable)return false;
        if(p==scope_address&&n==sizeof(scope)){
            ++scope_reads;if(mutate_scope&&scope_reads==2)scope[0]=0x400000;
            std::memcpy(out,scope.data(),n);return true;
        }
        if(p==descriptor_address&&n==sizeof(descriptor)){
            ++descriptor_reads;if(mutate_descriptor&&descriptor_reads==2)descriptor[0]+=16;
            std::memcpy(out,descriptor.data(),n);return true;
        }
        return false;
    }
};
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
d::Snapshot inspect(Memory& m,std::uint32_t hash=d::keys[1],std::uintptr_t descriptor=descriptor_address){
    return d::inspect(scope_address,hash,descriptor,image_address,0x09144000,
        [&](std::uintptr_t p,void* out,std::size_t n) noexcept {return m.read(p,out,n);});
}
}
int main(){try {
    Memory m;auto v=inspect(m);
    require(v.scope_header_coherent&&v.descriptor_coherent&&v.function_in_image&&v.function_rva==0x1895670,"matching native descriptor");
    require(v.descriptor_flags==7,"unknown flags preserved without interpretation");
    m={};v=inspect(m,1);require(!m.reads&&!v.scope&&!v.descriptor_present,"nonallowlisted key does not read");
    m={};v=inspect(m,d::keys[1],0);require(v.scope_header_coherent&&!v.descriptor_present&&!v.descriptor_coherent&&m.descriptor_reads==0,"null native result");
    m={};m.mutate_descriptor=true;v=inspect(m);require(!v.descriptor_coherent&&!v.function_in_image,"descriptor race rejected");
    m={};m.mutate_scope=true;v=inspect(m);require(!v.scope_header_coherent,"scope header race visible");
    m={};m.descriptor[1]=d::keys[0];v=inspect(m);require(!v.descriptor_coherent&&!v.function_in_image,"different descriptor hash rejected");
    m={};m.descriptor[0]=0x700000;v=inspect(m);require(v.descriptor_coherent&&!v.function_in_image&&v.function==0x700000&&!v.function_rva,"external pointer never normalized");
    m={};m.unreadable=true;v=inspect(m);require(!v.scope_header_coherent&&!v.descriptor_coherent,"unreadable observations fail closed");
    m={};v=inspect(m,d::keys[1],d::highest-8);require(!v.descriptor_coherent&&m.descriptor_reads==0,"descriptor span overflow rejected");
    unsigned error=41,calls=0,captures=0;
    const auto result=d::invoke_observed(scope_address,d::keys[1],
        [&](std::uintptr_t scope,std::uint32_t hash){++calls;require(scope==scope_address&&hash==d::keys[1]&&error==41,"original args/incoming error unchanged");error=97;return descriptor_address;},
        [&](std::uintptr_t scope,std::uint32_t hash,std::uintptr_t descriptor) noexcept {++captures;if(scope!=scope_address||hash!=d::keys[1]||descriptor!=descriptor_address)std::terminate();error=999;},
        [&]() noexcept {return error;},[&](unsigned e) noexcept {error=e;});
    require(calls==1&&captures==1&&result==descriptor_address&&error==97,"original exactly once, return and outgoing error preserved");
    calls=0;captures=0;
    try {
        (void)d::invoke_observed(scope_address,d::keys[1],
            [&](std::uintptr_t,std::uint32_t)->std::uintptr_t {++calls;if(throw_requested)throw 19;return descriptor_address;},
            [&](std::uintptr_t,std::uint32_t,std::uintptr_t) noexcept {++captures;},
            [&]() noexcept {return error;},[&](unsigned e) noexcept {error=e;});
        require(false,"native exception must propagate");
    }catch(int value){require(value==19&&calls==1&&!captures,"exception is not converted to a native result");}
    std::cout<<"binding snapshot/forwarding tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
