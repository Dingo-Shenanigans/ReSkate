#pragma once
#include "Extension/UI/NativeMenu/native_menu_lifetime.h"
#include <cstdint>
#include <span>
#include <vector>

namespace dingosdk::profile_runtime {
// Remove only our independently owned anchor. Preserve native records, handles,
// and order, including when another shelf was inserted ahead of ours.
template<class Ref>
std::vector<Ref> music_shelf_detached(std::span<const Ref> refs, std::uint64_t anchor) {
    std::vector<Ref> retained;
    retained.reserve(refs.size());
    for (const auto& ref : refs)
        if ((ref.handle & ~std::uint64_t{1}) != anchor) retained.push_back(ref);
    return retained;
}
}
