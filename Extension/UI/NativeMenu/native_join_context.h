#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace dingosdk::multiplayer::menu_data {
// Captured ThrowdownDetailsMenuRule.FillContent: its Instance input is a
// reference to the native rule which owns MMId. Class fields use +38 (struct
// fields use +60); never infer the selected registration from a donor title.
template<class Read> std::optional<std::uint32_t> native_join_mmid(std::uintptr_t vm,Read read) {
    using P=std::uintptr_t;
    const auto pointer=[&](P at) {P v{};return read(at,&v,sizeof(v))?v:P{};};
    const auto word=[&](P at) {std::uint32_t v{};return read(at,&v,sizeof(v))?v:0U;};
    const auto resource=pointer(vm+0x38),instance=pointer(vm+0x30);
    constexpr std::array<std::uint32_t,10> expected{624,40656,76600,21866,312,0,292,8520953,54067355,17437442};
    std::array<std::uint32_t,10> layout{};
    if(!resource || !instance || pointer(instance)!=resource || word(resource+0x10)!=0xebb4ab69 ||
       !read(resource+0x20,layout.data(),sizeof(layout)) || layout!=expected)return {};
    const auto constants=pointer(instance+624),ports=pointer(instance+632);
    const auto type=constants?pointer(constants+27912):0,meta=type?pointer(type):0;
    std::uint16_t flags{},size{},count{};
    if(!meta || word(meta)!=1847693693U || !read(meta+4,&flags,2) || ((flags>>5)&31)!=3 ||
       !read(meta+6,&size,2) || size!=352 || !read(meta+0x2a,&count,2) || count!=4)return {};
    const auto fields=pointer(meta+0x38);std::optional<unsigned> offset;
    for(unsigned i=0;fields && i<count;++i)if(word(fields+i*24)==0x66ba7ea4) {
        std::uint16_t at{};
        if(read(fields+i*24+8,&at,2) && at==0x150)offset=at;
    }
    if(!offset || !ports)return {};
    auto object=pointer(ports+168);
    for(unsigned depth=0;object && depth<3;++depth) {
        if(object!=type && (pointer(object)==type || pointer(object+8)==type))return word(object+*offset);
        object=pointer(object);
    }
    return {};
}
} // namespace dingosdk::multiplayer::menu_data
