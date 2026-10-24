#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace dingosdk::multiplayer {
struct FlagBubbleDisplay { std::uint64_t context{}, display{}; };

// MapObjectComponent owns the ActivityDisplay referenced by its world bubble.
// Resolve it from the registered POI's generation-checked EntityHandle, never
// from proximity, the donor icon, or a global UI provider. This is a read-only
// lookup of the verified 20260929 ECS component store (no native query handles).
template<class Reader>
FlagBubbleDisplay flag_bubble_display(Reader&& reader,std::uintptr_t base,
                                     std::uint64_t entity,std::uint64_t context,
                                     std::uint32_t mmid) {
    try {
        auto get=[&]<class T>(std::uintptr_t at) {
            T result{};
            if(!at || !reader(at,&result,sizeof(result)))
                throw std::runtime_error("Flag bubble owner is unavailable.");
            return result;
        };
        const auto ptr=[&](std::uintptr_t at){return get.template operator()<std::uintptr_t>(at);};
        const auto u32=[&](std::uintptr_t at){return get.template operator()<std::uint32_t>(at);};
        if(!entity || !context || !mmid)return {};
        const auto world=static_cast<std::uint32_t>(entity>>32);
        const auto local=static_cast<std::uint32_t>(entity);
        // The native world ID uses two addressable bytes and a generation byte.
        if(world>>22)return {};
        const auto manager=ptr(base+0x74cd760);
        const auto world_page=ptr(ptr(manager+0x70)+(world>>14)*8);
        if(!world_page)return {};
        const auto world_context=world_page+((world>>8)&63)*0xa30;
        if(u32(world_context+8)!=world)return {};
        const auto slots=ptr(world_context+0x18);
        if(!slots)return {};
        const auto shift=u32(slots+0x2c),stride=u32(slots+0x24),mask=u32(slots+0x28);
        if(shift>16 || stride<24 || stride>4096 || mask>65535)return {};
        const auto begin=ptr(slots),end=ptr(slots+8);
        const auto index=(local>>8)>>shift;
        if(end<begin || (end-begin)%8 || (end-begin)/8>65536 || index>=(end-begin)/8)return {};
        const auto block=ptr(begin+index*8);
        if(!block)return {};
        const auto slot=(local>>8)&mask;
        const auto bits_offset=get.template operator()<std::int32_t>(slots+0x34);
        if(bits_offset<0 || bits_offset>1048576 || block<static_cast<std::uintptr_t>(bits_offset))return {};
        if(!(ptr(block+(slot>>6)*8-bits_offset)&(std::uint64_t{1}<<(slot&63))))return {};
        const auto record=block+slot*stride;
        if(u32(record)!=local)return {}; // Includes the entity generation.
        const auto archetype=ptr(record+16);
        if(!archetype)return {};
        const auto descriptors=ptr(archetype+0x10);
        const auto count=u32(archetype+0x18);
        if(!descriptors || count>128)return {};
        constexpr std::array<std::uint8_t,16> guid{
            0xf6,0xa1,0x98,0x17,0x4e,0x41,0xb5,0x43,0xaf,0xde,0x00,0x28,0x5b,0xd7,0x92,0x7d};
        std::uintptr_t descriptor{};
        for(unsigned i=0;i<count;++i) {
            const auto candidate=ptr(descriptors+i*8);
            if(!candidate)continue;
            const auto type=ptr(candidate+8),metadata=type?ptr(type):0;
            if(!metadata || get.template operator()<std::array<std::uint8_t,16>>(metadata+8)!=guid)continue;
            if(u32(metadata)!=0x68015bc7 || get.template operator()<std::uint16_t>(metadata+6)!=704)return {};
            // Verify both model fields instead of treating arbitrary component
            // bytes as a model handle if the reflected layout changes.
            const auto fields=ptr(metadata+0x38);
            const auto fields_count=get.template operator()<std::uint16_t>(metadata+0x2a);
            if(!fields || fields_count>32)return {};
            unsigned verified{};
            for(unsigned f=0;f<fields_count;++f) {
                const auto field=fields+f*24;
                const auto hash=u32(field);
                const auto offset=get.template operator()<std::uint16_t>(field+8);
                if((hash==0x9f0a28d2 && offset==576) || (hash==0x7ac3f897 && offset==584))++verified;
            }
            if(verified!=2)return {};
            descriptor=candidate;break;
        }
        if(!descriptor)return {};
        const auto table=ptr(world_context+0x9b0);
        const auto buckets=u32(world_context+0x9b8);
        if(!table || !buckets || buckets>65536)return {};
        auto node=ptr(table+(descriptor%buckets)*8);
        std::uintptr_t store{};
        for(unsigned i=0;node && i<128;++i) {
            if(ptr(node)==descriptor){store=ptr(node+8);break;}
            node=ptr(node+16);
        }
        if(!store || ptr(store)!=base+0x65d0e48 || ptr(store+0x350)!=descriptor)return {};
        const auto sentinel=store+0x10;
        auto leaf=ptr(sentinel);
        for(unsigned pages=0;leaf && leaf!=sentinel && pages<4096;++pages) {
            const auto keys=get.template operator()<std::uint16_t>(leaf+0x10);
            if(keys>64)return {};
            for(unsigned i=0;i<keys;++i) {
                const auto key=u32(leaf+0x18+i*4);
                if(key>local)return {};
                if(key!=local)continue;
                const auto holder=ptr(leaf+0x118+i*8),component=holder?ptr(holder):0;
                if(!component || ptr(component+576)!=context || u32(component+656)!=mmid)return {};
                return {context,ptr(component+584)};
            }
            leaf=ptr(leaf);
        }
    } catch(const std::runtime_error&) {
        // Flag removal and map transitions can retire an owner between frames.
    }
    return {};
}
} // namespace dingosdk::multiplayer
