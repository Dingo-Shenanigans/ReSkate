#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::multiplayer {
// Native UI marker actions have no outputs. Denying the entry leaves both the saved marker and
// the skater untouched. A resumed/unrelated expression must always continue; never stop an
// arbitrary native interpreter cursor.
inline bool session_marker_action(std::uint32_t hash,const std::array<std::uint32_t,10>& layout,std::uint32_t pc) {
    if(pc)return false;
    struct Entry {std::uint32_t hash;std::array<std::uint32_t,10> layout;};
    constexpr std::array entries{
        Entry{0x12daba2a,{128,3792,2978,2958,13,0,50,1638558,7340060,17042434}}, // Set
        Entry{0xda5d517a,{288,11248,8935,8877,34,0,150,1638875,11534380,17042434}}, // Setting
        Entry{0x0d6708f4,{208,7528,5957,5920,23,0,100,1638717,9437220,17042434}}, // Undo
        Entry{0x177dbf1c,{208,7512,5956,5915,24,0,100,1638716,9437220,17042434}} // Return
    };
    for(const auto& entry:entries)if(entry.hash==hash && entry.layout==layout)return true;
    return false;
}
}
