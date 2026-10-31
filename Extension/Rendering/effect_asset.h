#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/effects.h"

namespace dingosdk::effects {
// Only the level-wide Game compartment may supply effects. Searching streamed
// sublevels would let a manifest claim a resident root for an unrelated asset.
// Never cache a native pointer, including across a replacement of Game itself.
class AssetLookup {
public:
    template<class Find> std::uintptr_t resolve(std::uint64_t now, Find&& find) {
        if (!found_ && now < retry_) return 0;
        const auto asset = find(addr::effects::resident_compartment);
        found_ = asset != 0;
        if (!found_) retry_ = now + 2000;
        return asset;
    }
private:
    bool found_{};
    std::uint64_t retry_{};
};

inline bool asset_is_resident(std::uintptr_t asset) noexcept {
    std::uint16_t compartment{};
    return asset && asset < memory::highest_user_address - addr::effects::asset_compartment &&
        memory::peek(asset + addr::effects::asset_compartment, compartment) &&
        compartment == addr::effects::resident_compartment;
}

inline bool asset_type_matches(std::uintptr_t asset, bool decal) noexcept {
    // Named lookup can return any asset class. Check its reflected type before
    // passing it to an API that expects a blueprint or a decal template.
    std::uintptr_t type{}, record{};
    std::uint32_t hash{};
    return asset && asset < memory::highest_user_address - addr::effects::asset_type &&
        memory::peek(asset + addr::effects::asset_type, type) && memory::peek(type, record) &&
        memory::peek(record, hash) && hash ==
            (decal ? addr::effects::decal_type_hash : addr::effects::particle_type_hash);
}
}
