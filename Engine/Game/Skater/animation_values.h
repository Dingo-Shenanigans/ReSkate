#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/animation_values.h"
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <optional>

namespace dingosdk::animation_values {
namespace layout = addr::animation_values;
struct Value {
    double number{};
    bool boolean{}, output{}, real{};
    std::uintptr_t address{}, descriptor{}, page{};
    std::uint32_t key{};
};
using Values = std::map<std::string, Value, std::less<>>;
inline bool pointer(std::uintptr_t value) { return value >= 0x10000 && value <= memory::highest_user_address && !(value & 7); }
inline Values read(std::uintptr_t base, std::uintptr_t core) {
    Values result;
    std::uintptr_t provider{}, model{};
    if (!memory::peek(core + layout::provider, provider) || !pointer(provider) ||
        !memory::peek(core + layout::model, model) || !pointer(model)) return result;
    for (const auto graph_offset : {layout::input, layout::output}) {
        std::uintptr_t graph{};
        if (!memory::peek(provider + graph_offset, graph)) continue;
        graph &= ~std::uintptr_t{4};
        if (!pointer(graph)) continue;
        for (std::uintptr_t slot = 0; slot < layout::graph_bytes; slot += sizeof(std::uintptr_t)) {
            std::uintptr_t node{}, type{}, secondary{}, page{};
            std::uint32_t key{}, occupied{};
            if (!memory::peek(graph + slot, node)) break;
            node &= ~std::uintptr_t{4};
            if (!pointer(node) || !memory::peek(node + layout::type, type)) continue;
            const bool boolean = type == base + layout::bool_type, integer = type == base + layout::enum_type;
            const bool real = type == base + layout::float_type;
            if (!boolean && !integer && !real) continue;
            if ((boolean || integer) && (!memory::peek(node + layout::secondary_type, secondary) ||
                secondary != base + (boolean ? layout::bool_secondary : layout::enum_secondary))) continue;
            if (!memory::peek(node + layout::key, key) || !(key >> 24) || (real && (key >> 24) != 4)) continue;
            const auto index = (key >> 12) & 0x7ff;
            if (index > layout::last_value || (!boolean && (index & 3)) ||
                !memory::peek(model + layout::pages + (key & 0xfff) * sizeof(std::uintptr_t), page) || !pointer(page) ||
                !memory::peek(page + layout::occupied + (index >> 5) * sizeof(std::uint32_t), occupied) || !(occupied & (1u << (index & 31)))) continue;
            char name[96]{};
            if (memory::peek_cstring(node + (real ? layout::float_name : layout::bool_name), name, sizeof(name)) < 0 ||
                !std::string_view(name).starts_with("Animation/Dingo/")) continue;
            Value value{}; value.boolean = boolean; value.real = real; value.output = graph_offset == layout::output;
            value.address = page + index; value.descriptor = node; value.page = page; value.key = key;
            if (boolean) { std::uint8_t v{}; if (!memory::peek(value.address, v) || v > 1) continue; value.number = v; }
            else if (integer) { std::int32_t v{}; if (!memory::peek(value.address, v) || v < 0 || v > 4095) continue; value.number = v; }
            else { float v{}; if (!memory::peek(value.address, v) || !std::isfinite(v) || std::abs(v) > 1e6f) continue; value.number = v; }
            result.emplace(name, value);
        }
    }
    return result;
}
// Numeric output physics properties only. This never writes animation inputs, booleans,
// exported stance/transform flags, descriptor defaults or another rider's property page.
inline bool writable(std::string_view name, const Value& value) {
    return value.output && value.real && name.starts_with("Animation/Dingo/Float.Physics.");
}
}
