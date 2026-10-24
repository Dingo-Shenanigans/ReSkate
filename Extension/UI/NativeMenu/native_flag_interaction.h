#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace dingosdk::multiplayer::menu_data {
struct NativeFlagInteraction {std::uint32_t mmid{};std::uint64_t model{};};
// Exact native ThrowdownEventInteractable.OnInteractableFocusChanged graph.
// The event owns both MMID and UIInteractableHandle. Native field metadata
// supplies offsets; never infer ownership from the visible Spot Battle label.
template<class Read>
std::optional<NativeFlagInteraction> native_flag_interaction(std::uintptr_t vm,Read&& read) {
    const auto pointer=[&](std::uintptr_t at){std::uintptr_t x{};return at && read(at,&x,sizeof(x))?x:0;};
    const auto word=[&](std::uintptr_t at){std::uint32_t x{};return at && read(at,&x,sizeof(x))?x:0;};
    const auto half=[&](std::uintptr_t at)->std::uint16_t{std::uint16_t x{};return at && read(at,&x,sizeof(x))?x:0;};
    const auto resource=pointer(vm+0x38),instance=pointer(vm+0x30);
    constexpr std::array<std::uint32_t,10> expected{80,1392,3539,760,7,0,8,3604513,6160395,1};
    std::array<std::uint32_t,10> layout{};
    if(!resource || !instance || pointer(instance)!=resource || word(resource+0x10)!=0x627382a6 ||
       !read(resource+0x20,layout.data(),sizeof(layout)) || layout!=expected)return {};
    const auto constants=pointer(instance+80),ports=pointer(instance+88);
    const auto type=constants?pointer(constants+1056):0,meta=pointer(type);
    if(!meta || word(meta)!=3388528466U || ((half(meta+4)>>5)&31)!=3)return {};
    auto object=ports?pointer(ports+8):0;
    for(unsigned depth=0;object && depth<3;++depth) {
        if(pointer(object)==type || pointer(object+8)==type)break;
        object=pointer(object);
    }
    if(!object || (pointer(object)!=type && pointer(object+8)!=type))return {};
    const auto field=[&](std::uintptr_t metadata,std::uint32_t hash,unsigned width)->std::optional<unsigned> {
        const auto count=half(metadata+0x2a),size=half(metadata+6);
        const auto entries=pointer(metadata+0x38);
        if(count>32 || !entries || size>8192)return {};
        for(unsigned i=0;i<count;++i)if(word(entries+i*24)==hash) {
            const auto offset=half(entries+i*24+8);
            const auto ft=pointer(entries+i*24+16),fm=pointer(ft);
            if(fm && half(fm+6)==width && offset+width<=size)return offset;
        }
        return {};
    };
    const auto handle=field(meta,936281207U,8); // UIInteractableHandle
    // ClassInfoData's parent ClassInfo is a typed native reflection pointer.
    const auto parent=pointer(meta+0x30),base=pointer(parent);
    if(!handle || !base || word(base)!=2454258579U || ((half(base+4)>>5)&31)!=3)return {};
    const auto mmid=field(base,0x66ba7ea4,4);
    if(!mmid)return {};
    return NativeFlagInteraction{word(object+*mmid),pointer(object+*handle)};
}
} // namespace dingosdk::multiplayer::menu_data
