#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace dingosdk::guid_table_guard::detail {
enum class Probe { found, missing, unreadable };
// The native table uses 16-bit record indices. Its slots are a power of two,
// at least sixteen, sized for 1.5 times the source's record count.
inline std::uint32_t slot_count(std::uint32_t records) noexcept {
    if(records>0xffffu)return 0;
    const auto wanted=std::max(16u,records+records/2);
    std::uint32_t slots=16;
    while(slots<wanted)slots*=2;
    return slots;
}
template<class KeyOf> Probe probe_slots(std::span<const std::uint16_t> slots,
        const std::array<std::uint32_t,4>& key,KeyOf&& key_of) {
    if(slots.empty() || (slots.size()&(slots.size()-1)))return Probe::unreadable;
    const auto mask=static_cast<std::uint32_t>(slots.size()-1);
    const auto step=(key[0]&0x3e)|1u;
    auto at=key[0]&mask;
    // Odd steps visit every slot exactly once, including fully occupied tables.
    for(std::size_t tries=0;tries<slots.size();++tries,at=(at+step)&mask) {
        const auto index=slots[at];
        if(index==0xffffu)continue;
        const auto* other=key_of(index);
        if(other && std::equal(key.begin(),key.end(),other))return Probe::found;
    }
    return Probe::missing;
}
}
