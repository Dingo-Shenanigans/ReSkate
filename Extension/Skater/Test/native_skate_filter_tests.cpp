#include "Extension/Skater/native_skate_filter.h"
#include "Extension/Skater/skate_detector.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include "Extension/Skater/native_trick_metadata.h"
#include <string>
namespace dingosdk::profile_runtime {thread_local std::uintptr_t executing_expression{};}
namespace {
using Address=std::uintptr_t;
bool original_membership{};
std::uintptr_t native_contains(const void*,const void*,const void*,std::uint8_t* out){*out=original_membership;return 77;}
std::uintptr_t native_or(const void* a,const void* b,std::uint8_t* out){*out=*static_cast<const std::uint8_t*>(a)||*static_cast<const std::uint8_t*>(b);return 88;}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
void native_filter_regressions() {
    using namespace dingosdk;
    // Minimal native-call contract fixture, not a fabricated game area object.
    // The corresponding instructions are independently verified against the actual RES.
    std::array<std::uint8_t,0x1200> resource{};std::array<std::uint8_t,128> instance{},vm{};std::array<std::uint8_t,512> locals{};
    const auto put=[](auto& bytes,std::size_t at,const auto& value){std::memcpy(bytes.data()+at,&value,sizeof(value));};
    const auto resource_at=reinterpret_cast<Address>(resource.data()),vm_at=reinterpret_cast<Address>(vm.data());
    put(vm,0x38,resource_at);put(vm,0x30,reinterpret_cast<Address>(instance.data()));put(instance,0,resource_at);put(instance,80,reinterpret_cast<Address>(locals.data()));
    put(resource,0x10,0x7841abd7u);put(resource,0x20,std::array<std::uint32_t,10>{64,472,466,781,6,0,0,655414,5177355,2048});put(resource,0x3b8,0xc27u);
    for(unsigned i=0;i<3;++i){put(resource,0xfec+i*8,0xb0b4285au);put(resource,0xff0+i*8,std::array<unsigned,3>{0x190,0x248,0x2cc}[i]);}
    put(resource,0xfec+24,0xeabfec6fu);put(resource,0xff0+24,0x8ccu);
    const auto code=0x3b8;
    put(resource,code+0x190,0x1bc03u);put(resource,code+0x190+12,std::array<std::uint32_t,8>{2,0xe0,5,0,0,0x40,2,0x1b9});
    put(resource,code+0x14c,0x18004u);put(resource,code+0x14c+12,std::array<std::uint32_t,10>{2,0x80,0,0x30,0,0x170,0,0x38,2,0xd8});
    put(resource,code+0x180,std::array<std::uint32_t,4>{0x1902e,2,0xd8,5});
    put(resource,code+0x1d8,std::array<std::uint32_t,4>{0x1e826,2,0x1ba,0x31c});
    put(resource,code+0x2cc,0x2f803u);put(resource,code+0x2cc+12,std::array<std::uint32_t,8>{2,0x78,2,0x174,0,0x78,2,0x1bc});
    put(resource,code+0x21c,0x24803u);put(resource,code+0x21c+12,std::array<std::uint32_t,8>{2,0x80,0,0x58,0,0x178,2,0x174});
    put(resource,code+0x298,0x2cc04u);put(resource,code+0x298+12,std::array<std::uint32_t,10>{3,0,0,0x68,0,0x17c,0,0x70,2,0x78});
    put(resource,code+0x8cc,0x8f002u);put(resource,code+0x8cc+12,std::array<std::uint32_t,6>{2,0x1ce,2,0x1d0,2,0x1d1});
    put(resource,code+0x834,0x86003u);put(resource,code+0x834+12,std::array<std::uint32_t,8>{3,0,0,0xe8,0,0x1b4,2,0x1cd});
    put(resource,code+0x87c,0x8a803u);put(resource,code+0x87c+12,std::array<std::uint32_t,8>{3,0,0,0xf0,0,0x1b8,2,0x1cf});
    put(resource,code+0x190+4,reinterpret_cast<Address>(&native_contains));
    put(resource,code+0x2cc+4,reinterpret_cast<Address>(&native_contains));put(resource,code+0x8cc+4,reinterpret_cast<Address>(&native_or));
    skate::begin({0xff,true});skate::turn(true);profile_runtime::executing_expression=vm_at;
    require(skate::prepare_native_filter(vm_at),"Native category/area binding failed");
    using Contains=std::uintptr_t (*)(const void*,const void*,const void*,std::uint8_t*);
    using Or=std::uintptr_t (*)(const void*,const void*,std::uint8_t*);
    Contains contains{};Or area{};std::memcpy(&contains,resource.data()+code+0x2cc+4,8);std::memcpy(&area,resource.data()+code+0x8cc+4,8);require(area==&native_or,"Native area function was replaced");
    Contains ignored{};std::memcpy(&ignored,resource.data()+code+0x190+4,8);
    std::array<std::uint8_t,16> string_meta{};put(string_meta,4,std::uint16_t(7<<5));put(string_meta,6,std::uint16_t(8));
    Address meta=reinterpret_cast<Address>(string_meta.data()),info=reinterpret_cast<Address>(&meta);
    const auto classify=[&](const char* name,bool was_ignored=false) {
        original_membership=was_ignored;std::uint8_t output=255;
        require(ignored(nullptr,&name,&info,&output)==77,"Native ignored call return changed");
        require(!skate::take_bridge_error(),"Native string binding failed");
        return bool(output);
    };
    for(const auto& entry:skate::native_tricks) {
        const std::string name(entry.name);
        require(skate::native_ignore_observation(entry.name).has_value(),"Scoring catalog identity unclassified");
        require(classify(name.c_str())==*skate::native_ignore_observation(entry.name),"Native ignored stage disagrees with policy");
    }
    require(classify("powerslide") && classify("tricklessair") && classify("pumping"),"Excluded movement seeded native attempt");
    require(classify("tailmanual") && classify("unknown-scorable"),"Manual or unknown movement seeded native attempt");
    for(unsigned i=0;i<20;++i)require(classify("slappyenter") && classify("slappyenter_stall"),"Slappy entry seeded native attempt");
    require(!classify("bs_tailslide") && !classify("fs_tailslide"),"Slappy exclusion suppressed an actual grind");
    require(!classify("ollie",true) && !classify("simplified_ollie",true),"Native ignored Ollie not restored");
    require(!classify("kickflip") && !classify("hospitalflipunderflip"),"Recognized core trick excluded");
    for(unsigned bit=0;bit<4;++bit) {
        skate::begin({static_cast<std::uint8_t>(0xffu&~(1u<<bit)),true});skate::turn(true);
        for(const auto& entry:skate::native_tricks) {
            const auto c=skate::category(entry.identity);
            const auto disabled=bit==0?skate::Category::flips:bit==1?skate::Category::grabs:bit==2?skate::Category::grinds:skate::Category::ground;
            if(c==disabled){const std::string name(entry.name);require(classify(name.c_str()),"Disabled category reached native first-valid/category checks");}
        }
    }
    skate::begin({0xff,true});skate::turn(true);
    skate::turn(false);require(!classify("powerslide") && !classify("slappyenter"),"Opponent native membership changed");
    skate::turn(true);original_membership=false;
    std::uint32_t category=19;std::uint8_t out{};
    require(contains(nullptr,&category,nullptr,&out)==77 && out==1,"Advanced native category not enabled");
    category=5;require(contains(nullptr,&category,nullptr,&out)==77 && !out,"Ignored manual seeded native result");
    for(const bool uses:{false,true})for(const bool inside:{false,true}) {
        locals[0x1cd]=uses;locals[0x1cf]=inside;const std::uint8_t a=!uses,b=uses&&inside;
        area(&a,&b,&out);locals[0x1d1]=out;
        require(bool(out)==(!uses || inside),"Native area predicate output was changed");

    }
    // Repeated excluded observations do not need a post-submission reset.
    for(unsigned i=0;i<20;++i)require(classify("powerslide"),"Repeated ignored movement became eligible");
    require(!classify("kickflip"),"A later core observation remained excluded");
    put(string_meta,4,std::uint16_t(15<<5));const char* invalid_name="kickflip";
    require(ignored(nullptr,&invalid_name,&info,&out)==77 && skate::take_bridge_error(),"Changed native ScorableName type silently accepted");
    put(string_meta,4,std::uint16_t(7<<5));
    // Area operands are deliberately not interpreted or guarded by this hook.
    // test7's category adapter must leave every native area path alone.
    put(resource,code+0x8cc+12,9u);require(skate::prepare_native_filter(vm_at),"Area operand influenced category adapter");
    std::memcpy(&area,resource.data()+code+0x8cc+4,8);require(area==&native_or,"Native area pointer changed");
    put(resource,code+0x190+12,9u);
    require(!skate::prepare_native_filter(vm_at) && skate::take_bridge_error(),"Changed ignored operands accepted");
    put(resource,code+0x190+12,2u);
    put(resource,code+0x2cc+12,9u);
    require(!skate::prepare_native_filter(vm_at) && skate::take_bridge_error(),"Changed category operands accepted");
    profile_runtime::executing_expression=0;
    skate::end();
}
