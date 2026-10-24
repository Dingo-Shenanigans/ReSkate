#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace dingosdk::plugins::ui_values {
using Values = std::map<std::string, float, std::less<>>;
inline constexpr std::size_t maximum_values = 32;
inline constexpr float maximum_magnitude = 100000;
inline bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 48) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
// Asset authors use this symbol as a float DataPath's SourceContext. Plugins cannot
// supply a symbol, a model handle or a path into one of the game's own models.
inline std::uint32_t symbol(std::string_view plugin, std::string_view name) {
    std::uint32_t hash = 5381;
    for (const unsigned char c : "ReSkate.Plugin." + std::string(plugin) + "." + std::string(name)) hash = hash * 33 ^ c;
    return hash;
}
inline bool distinct_symbols(std::string_view plugin, const Values& values) {
    std::set<std::uint32_t> symbols;
    for (const auto& [name, value] : values) {
        (void)value;
        const auto id = symbol(plugin, name);
        if (!id || !symbols.insert(id).second) return false;
    }
    return true;
}
std::string publish(std::uintptr_t base, std::string_view plugin, const Values& values);
void release(std::uintptr_t base, std::string_view plugin) noexcept;
}
