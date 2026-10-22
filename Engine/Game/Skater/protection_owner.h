#pragma once
#include <cstdint>

namespace dingosdk::skater {
// Native addresses can be recycled after respawn/unload. A live deadline and matching
// hook object are insufficient: re-resolve the complete published ownership chain.
template<class Owner, class Resolve>
bool validated_protection_owner(const Owner& published, bool active, std::uintptr_t object,
    std::uintptr_t Owner::* member, Resolve&& resolve, Owner* output = nullptr) noexcept {
    if (!active || object != published.*member) return false;
    Owner current{};
    if (!resolve(published.client, published.entity, current) || current != published) return false;
    if (output) *output = current;
    return true;
}
} // namespace dingosdk::skater
