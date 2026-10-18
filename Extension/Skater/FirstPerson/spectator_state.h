#pragma once
#include <cstdint>
namespace dingosdk::first_person_integration {
// Native party target/session identity; each new target begins in follow view.
struct SpectatorState {
    std::uint64_t id{},epoch{};std::uintptr_t context{};bool head{};
    bool observe(bool enabled,std::uint64_t next,std::uint64_t generation,std::uintptr_t world) {
        const bool changed=!enabled || !next || id!=next || epoch!=generation || context!=world;
        if(changed) {*this={};if(enabled && next){id=next;epoch=generation;context=world;}}
        return changed;
    }
    bool toggle(){if(!id)return false;head=!head;return true;}
};
}
