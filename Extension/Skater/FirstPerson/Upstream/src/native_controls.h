#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace jishsfirstpersonmod {
inline bool native_first_person_toggle(std::string_view command) {
    const auto space=[](char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; };
    while(!command.empty() && space(command.front())) command.remove_prefix(1);
    while(!command.empty() && space(command.back())) command.remove_suffix(1);
    const auto split=command.find_first_of(" \t\r\n");
    if(split==std::string_view::npos) return false;
    const auto name=command.substr(0,split);
    command.remove_prefix(split);
    while(!command.empty() && space(command.front())) command.remove_prefix(1);
    return (name=="firstperson" || name=="debug.first_person") && command=="toggle";
}
inline bool native_keyboard_binding(std::uint32_t binding) {
    if((binding&0xffffff00u)==0x80000000u) {
        const auto key=binding&255;
        return (key>='0' && key<='9') || (key>='A' && key<='Z') || key==0x20 ||
            (key>=0x70 && key<=0x7b) || key==0x11 || key==0x10 || key==0x12;
    }
    if((binding&0xc0000000u)!=0x40000000u) return false;
    unsigned count{},previous{};bool ended{};
    for(unsigned slot=0;slot<5;++slot) {
        const auto id=(binding>>(slot*6))&63;
        if(!id) { ended=true;continue; }
        if(ended || id>52 || id<=previous) return false;
        previous=id;++count;
    }
    return count>=2;
}
struct NativeControls {
    bool active{},board_only{},stabilize{},follow_flips{};
    bool camera_cycle_available{},camera_cycle{};
    float smoothing=60,head_pitch=86,head_roll=0,bob=25;
    std::array<std::uint32_t,17> bindings{};
    bool operator==(const NativeControls&) const = default;
};
inline bool native_binding_overlap(std::uint32_t combo,const NativeControls& native) {
    if(!combo || !native.active) return false;
    for(auto binding:native.bindings) {
        if(!binding) continue;
        // Sharing buttons does not reserve a different complete chord.
        if(binding==combo) return true;
    }
    return false;
}
inline bool native_key_overlap(unsigned key,const NativeControls& native) {
    if(!key || !native.active) return false;
    const auto from_id=[](unsigned id)->unsigned {
        if(id>=1 && id<=10) return '0'+id-1;
        if(id>=11 && id<=36) return 'A'+id-11;
        if(id==37) return 0x20;
        if(id>=38 && id<=49) return 0x70+id-38;
        return id==50 ? 0x11:id==51 ? 0x10:id==52 ? 0x12:0;
    };
    for(auto binding:native.bindings) {
        if((binding&0xffffff00u)==0x80000000u && (binding&255)==key) return true;
        if((binding&0xc0000000u)==0x40000000u)
            for(unsigned slot=0;slot<5;++slot) if(from_id((binding>>(slot*6))&63)==key) return true;
    }
    return false;
}
}
