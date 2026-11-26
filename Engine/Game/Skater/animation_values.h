#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/animation_values.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>

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
// The name of the value a descriptor is for ("Animation/Dingo/Bool.Anim.DoingTrick"), when `node` is a descriptor of a
// boolean, a choice or a number: empty when it is anything else.
inline std::string descriptor_name(std::uintptr_t base, std::uintptr_t node) {
    std::uintptr_t type{}, secondary{};
    if (!pointer(node) || !memory::peek(node + layout::type, type)) return {};
    const bool boolean = type == base + layout::bool_type, integer = type == base + layout::enum_type;
    const bool real = type == base + layout::float_type;
    if (!boolean && !integer && !real) return {};
    if ((boolean || integer) && (!memory::peek(node + layout::secondary_type, secondary) ||
        secondary != base + (boolean ? layout::bool_secondary : layout::enum_secondary))) return {};
    char name[96]{};
    if (memory::peek_cstring(node + (real ? layout::float_name : layout::bool_name), name, sizeof(name)) < 0 ||
        !std::string_view(name).starts_with("Animation/Dingo/")) return {};
    return name;
}
// A descriptor's value as the rider holds it: nothing when `node` is not a descriptor, the rider's pages (`model`) do not
// hold the value now, or it does not read as its kind. The descriptor carries where the value is (its key: a page of the
// rider's, a place in it), filled in by the game when the value is first bound; one not bound yet has no value.
inline std::optional<Value> descriptor_value(std::uintptr_t base, std::uintptr_t model, std::uintptr_t node, std::string_view name) {
    std::uintptr_t type{}, page{};
    std::uint32_t key{}, occupied{};
    if (name.empty() || !memory::peek(node + layout::type, type)) return std::nullopt;
    const bool boolean = type == base + layout::bool_type, integer = type == base + layout::enum_type;
    const bool real = type == base + layout::float_type;
    if (!boolean && !integer && !real) return std::nullopt;
    if (!memory::peek(node + layout::key, key) || !(key >> 24) || (real && (key >> 24) != 4)) return std::nullopt;
    const auto index = (key >> 12) & 0x7ff;
    // (A rider has a handful of pages: a key naming one past sixteen is not a key.)
    if (index > layout::last_value || (!boolean && (index & 3)) || (key & 0xfff) >= 16 ||
        !memory::peek(model + layout::pages + (key & 0xfff) * sizeof(std::uintptr_t), page) || !pointer(page) ||
        !memory::peek(page + layout::occupied + (index >> 5) * sizeof(std::uint32_t), occupied) || !(occupied & (1u << (index & 31)))) return std::nullopt;
    Value value{}; value.boolean = boolean; value.real = real;
    value.address = page + index; value.descriptor = node; value.page = page; value.key = key;
    if (boolean) { std::uint8_t v{}; if (!memory::peek(value.address, v) || v > 1) return std::nullopt; value.number = v; }
    else if (integer) { std::int32_t v{}; if (!memory::peek(value.address, v) || v < 0 || v > 4095) return std::nullopt; value.number = v; }
    else { float v{}; if (!memory::peek(value.address, v) || !std::isfinite(v) || std::abs(v) > 1e6f) return std::nullopt; value.number = v; }
    return value;
}
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
            std::uintptr_t node{};
            if (!memory::peek(graph + slot, node)) break;
            node &= ~std::uintptr_t{4};
            const auto name = descriptor_name(base, node);
            auto value = descriptor_value(base, model, node, name);
            if (!value) continue;
            value->output = graph_offset == layout::output;
            result.emplace(name, *value);
        }
    }
    return result;
}
// Numeric output physics properties only. This never writes animation inputs, booleans,
// exported stance/transform flags, descriptor defaults or another rider's property page.
inline bool writable(std::string_view name, const Value& value) {
    return value.output && value.real && name.starts_with("Animation/Dingo/Float.Physics.");
}

// ---- every named value, not only the graph's own ----------------------------------------------------------
// The rider's animation graph lists some 160 of its values (read, above). The game holds about 1560 descriptors in
// memory (1560 on San Vansterdam, 841 of them bound; read from the running game, 2026-11-18): the manual the player
// asks for, the trick's phase, the grind the board is aimed at and the grab's type are among those the graph does not
// list. A descriptor is known by its type word, so they are found by a search of the game's memory for that word, and
// each is then read like a listed one.
//
// The words in [at, end) that are a descriptor's type, as addresses, up to `capacity` of them; `at` is left where the
// search stopped (at `end` when it is done, or when the memory went away under it). No objects: structured exceptions.
inline std::size_t descriptor_type_words(std::uintptr_t base, std::uintptr_t& at, std::uintptr_t end, std::uintptr_t* found,
                                         std::size_t capacity) noexcept {
    const std::uintptr_t low = base + layout::enum_type, span = layout::float_type - layout::enum_type;
    std::size_t count = 0;
    __try {
        for (; at + sizeof(std::uintptr_t) <= end && count < capacity; at += sizeof(std::uintptr_t)) {
            const auto word = *reinterpret_cast<const volatile std::uintptr_t*>(at);
            if (word - low > span) continue;
            if (word == base + layout::bool_type || word == base + layout::enum_type || word == base + layout::float_type)
                found[count++] = at;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        at = end;
    }
    return count;
}
// The descriptors found, by the name of their value. A value can have more than one descriptor in memory and only one
// of them bound (seen on the grind switches, 2026-10-11), and a level that was unloaded leaves its own readable under
// the same names: up to four are kept, and the first that reads is the value.
struct Found {
    std::unordered_map<std::string, std::array<std::uintptr_t, 4>> descriptors;
    void add(std::string name, std::uintptr_t node) {
        auto& kept = descriptors[std::move(name)];
        for (auto& slot : kept) {
            if (slot == node) return;
            if (!slot) { slot = node; return; }
        }
    }
    // The descriptors in one block of memory (a whole region: aligned to a word).
    void add_block(std::uintptr_t base, std::uintptr_t start, std::size_t size) {
        std::array<std::uintptr_t, 256> words{};
        for (auto at = start; at < start + size;) {
            const auto count = descriptor_type_words(base, at, start + size, words.data(), words.size());
            for (std::size_t i = 0; i < count; ++i) {
                const auto node = words[i] - layout::type;
                if (auto name = descriptor_name(base, node); !name.empty()) add(std::move(name), node);
            }
            if (!count) break;
        }
    }
    bool knows(std::string_view name) const { return descriptors.find(std::string(name)) != descriptors.end(); }
    std::optional<Value> read(std::uintptr_t base, std::uintptr_t core, std::string_view name) const {
        const auto at = descriptors.find(std::string(name));
        std::uintptr_t model{};
        if (at == descriptors.end() || !memory::peek(core + layout::model, model) || !pointer(model)) return std::nullopt;
        for (const auto node : at->second) {
            // Still that value's descriptor (its memory goes back to the game with its level and is used again).
            if (!node || descriptor_name(base, node) != name) continue;
            if (const auto value = descriptor_value(base, model, node, name)) return value;
        }
        return std::nullopt;
    }
};
// When to search: a level's descriptors are its own (their keys differ from level to level, and the old level's stay
// readable in freed memory), so what was found is forgotten when the level changes and looked for again three seconds
// into the new one, once it is in. While a name asked for is still not found it is looked for again fifteen seconds
// later, four times a level at most: a name the game does not have costs four searches, not one a second.
struct SearchRule {
    std::string level;
    unsigned searches{};
    std::uint64_t next_at{};
    // True when what was found is another level's and has to be forgotten.
    bool new_level(std::string_view now_level, std::uint64_t now_ms) {
        if (now_level == level) return false;
        level = now_level;
        searches = 0;
        next_at = now_ms + 3000;
        return true;
    }
    // True when a search starts now (the caller starts it).
    bool due(bool missing, bool searching, std::uint64_t now_ms) {
        if (!missing || searching || searches >= 4 || now_ms < next_at) return false;
        ++searches;
        next_at = now_ms + 15000;
        return true;
    }
};
}
