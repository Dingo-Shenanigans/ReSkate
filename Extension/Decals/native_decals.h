#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/decals.h"
#include "Engine/Game/Build/addresses.h"
#include <algorithm>
#include <cstring>
#include <optional>

namespace dingosdk::decals::native {
namespace build = addr::decals;
struct Handle { std::uint32_t id{}; bool borrowed{}; unsigned char padding[3]{}; };
static_assert(sizeof(Handle)==8);
struct Settings {
    std::array<float,4> culling{};
    std::uint8_t priority{}, flags{};
    std::uint16_t padding{};
    Handle resource;
};
static_assert(sizeof(Settings)==0x1c && offsetof(Settings,resource)==0x14);
struct alignas(16) Properties {
    std::array<float,4> params{};
    std::array<std::byte,56> shader{};
    std::uint64_t shader_key{}, blocks{}, material{};
    float alpha{}, override_culling{};
    std::uint32_t shader_id{}, mask{};
    std::uint16_t flags{};
    bool enabled{}, detached{};
    std::uint8_t row{}, column{}, default_expression{};
    std::array<std::byte,9> padding{};
};
static_assert(sizeof(Properties)==0x80 && offsetof(Properties,material)==0x58 &&
    offsetof(Properties,alpha)==0x60 && offsetof(Properties,mask)==0x6c && offsetof(Properties,flags)==0x70);
enum class ReceiverMask : std::uint32_t { all=0,static_only=1,dynamic_only=2,terrain_only=3 };
inline void assign_mask(Properties& props,ReceiverMask mask) {
    props.mask=static_cast<std::uint32_t>(mask); props.flags|=0x2000;
}

// 0x3dd1d70 is the raw-object assignment overload, not reference-to-reference
// copy: RDX is stored directly in [RCX] and its refcount at RDX+0x10 retained.
// Passing &asset instead retains stack memory and crashes the render worker.
using ReferenceAssign = void(*)(std::uint64_t*,std::uintptr_t);
inline void assign_material(Properties& props,std::uintptr_t asset,ReferenceAssign assign) {
    assign(&props.material,asset);
    props.flags|=0x40;
}
template<class F> F function(std::uintptr_t base, std::uintptr_t rva) {return reinterpret_cast<F>(base+rva);}
inline void destroy_properties(std::uintptr_t base,Properties& props) {
    auto* bytes=reinterpret_cast<std::byte*>(&props);
    for (const auto& item:build::property_cleanup)
        function<void(*)(void*)>(base,item.second)(bytes+item.first);
}
inline bool opacity_contracts_match(std::uintptr_t base) {
    return std::all_of(build::opacity_contracts.begin(),build::opacity_contracts.end(),[&](const auto& c) {
        std::array<unsigned char,32> bytes{};
        return memory::read_bytes(base+c.rva,bytes.data(),bytes.size()) && bytes==c.bytes;
    });
}
inline void set_opacity(std::uintptr_t base,std::uintptr_t manager,std::uint32_t id,float alpha) {
    if (!id) return;
    // The native comparison reads descriptor arrays even for a scalar update:
    // use the real initializer and destructor, never a zeroed partial struct.
    Properties props{};
    function<void(*)(void*)>(base,build::properties_init)(&props);
    struct Cleanup {
        std::uintptr_t base; Properties& props;
        ~Cleanup() {destroy_properties(base,props);}
    } cleanup{base,props};
    props.alpha=std::clamp(alpha,0.f,1.f); props.flags=4;
    function<void(*)(std::uintptr_t,std::uint32_t,const Properties*)>(base,build::update_properties)(manager,id,&props);
}
inline bool contracts_match(std::uintptr_t base) {
    return std::all_of(build::contracts.begin(),build::contracts.end(),[&](const auto& c) {
        std::array<unsigned char,32> bytes{};
        return memory::read_bytes(base+c.rva,bytes.data(),bytes.size()) && bytes==c.bytes;
    });
}
inline bool surface_contracts_match(std::uintptr_t base) {
    return std::all_of(build::surface_contracts.begin(),build::surface_contracts.end(),[&](const auto& c) {
        std::array<unsigned char,32> bytes{};
        return memory::read_bytes(base+c.rva,bytes.data(),bytes.size()) && bytes==c.bytes;
    });
}
inline void set_transform(std::uintptr_t base,std::uintptr_t manager,std::uint32_t id,const std::array<float,16>& transform) {
    if (!id) return;
    alignas(16) const auto matrix=transform;
    function<void(*)(std::uintptr_t,std::uint32_t,const float*)>(base,build::update_transform)(manager,id,matrix.data());
}
inline std::uintptr_t manager(std::uintptr_t base) {
    std::uintptr_t result{}, table{};
    if (!memory::read(base+build::manager,result) || !result || !memory::read(result,table) ||
        table!=base+build::manager_vtable) return 0; // Exclude the engine's null service.
    return result;
}
inline std::uintptr_t client_realm(std::uintptr_t manager) {
    // Allocation and release both need the client realm and its update queue.
    // The global manager can survive after teardown clears this realm slot.
    std::uintptr_t realm{}, queue{};
    if (!manager || !memory::peek(manager+build::manager_realm,realm) || !realm ||
        !memory::peek(realm+build::realm_update_queue,queue) || !queue) return 0;
    return realm;
}
inline Handle create(std::uintptr_t base, std::uintptr_t manager, std::uintptr_t material,
                     const std::array<float,16>& transform,std::optional<ReceiverMask> mask=std::nullopt) {
    // The concrete allocator (0x1993620) dereferences the client realm's
    // state at manager+0x70 and its pending-update queue at state+0x400.
    // The service can exist before that realm is attached.
    if (!client_realm(manager)) return {};
    alignas(16) Settings settings{};
    function<void(*)(void*)>(base,build::settings_init)(&settings);
    settings.culling={100,100,100,100}; settings.flags|=3;
    Properties props{};
    function<void(*)(void*)>(base,build::properties_init)(&props);
    struct Cleanup {
        std::uintptr_t base; Properties& p; Settings& s;
        ~Cleanup() {
            // Exact stack-descriptor cleanup from EnvironmentDecalVolume's
            // factory (0x199f740), in reverse construction order.
            destroy_properties(base,p);
            function<void(*)(void*)>(base,build::resource_release)(&s.resource);
        }
    } cleanup{base,props,settings};
    assign_material(props,material,function<ReferenceAssign>(base,build::reference_assign));
    if (mask) assign_mask(props,*mask);
    props.flags|=1; props.enabled=true;
    // A standalone resource has no parent. Native create copies both stack
    // descriptors and gives the returned handle one owned reference.
    Handle result{}, parent{};
    alignas(16) const auto matrix=transform;
    function<Handle*(*)(std::uintptr_t,Handle*,const Settings*,const Properties*,const Handle*,
                       const float*,bool)>(base,build::create)(manager,&result,&settings,&props,&parent,matrix.data(),false);
    return result;
}
inline void release(std::uintptr_t base, Handle& handle) {
    if (handle.id) function<void(*)(void*)>(base,build::release)(&handle);
    handle={};
}
}
