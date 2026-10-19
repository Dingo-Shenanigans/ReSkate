#pragma once
#include "Extension/Skater/FirstPerson/Upstream/src/player_state_contract.h"
namespace jishsfirstpersonmod {
// Zero releases custom stick/glance control. Verified native animation
// handoffs may still blend through the separate movement sample.
inline int camera_movement_mode(bool verified,bool offboard,std::uintptr_t parent,
                                std::uintptr_t active,std::uintptr_t vtable,
                                std::uintptr_t base,std::uint32_t physics) {
    if(!verified) return 0;
    if(offboard) {
        for(const auto& state:player_state_contract::normal_foot_states)
            if(active==parent+state.offset && vtable==base+state.vtable) return 1;
        return 0;
    }
    return player_state_contract::riding(physics).value_or(false) ? 2:0;
}
}
