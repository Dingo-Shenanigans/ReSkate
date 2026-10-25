#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
namespace dingosdk::overlay::detail
{
inline std::span<const char> shader_chunk(std::span<const char> shader, const char *name)
{
    auto word = [&](std::size_t at) {
        std::uint32_t value{};
        std::memcpy(&value, shader.data() + at, 4);
        return value;
    };
    if (shader.size() < 32 || std::memcmp(shader.data(), "DXBC", 4))
        return {};
    const auto total = word(24), count = word(28);
    if (total > shader.size() || total < 32 || count > (total - 32) / 4)
        return {};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto at = word(32 + 4 * i);
        if (at < 32 + 4 * count || at > total || total - at < 8)
            return {};
        const auto size = word(at + 4);
        if (size > total - at - 8)
            return {};
        if (!std::memcmp(shader.data() + at, name, 4))
            return shader.subspan(at + 8, size);
    }
    return {};
}
inline std::uint32_t shader_output_component(std::span<const char> shader, std::uint32_t target)
{
    const auto signature = shader_chunk(shader, "OSG1");
    if (signature.size() < 8)
        return 0;
    const auto word = [&](std::size_t at) {
        std::uint32_t value{};
        std::memcpy(&value, signature.data() + at, 4);
        return value;
    };
    const auto count = word(0), start = word(4);
    if (start > signature.size() || count > (signature.size() - start) / 32)
        return 0;
    std::uint32_t component{};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto at = start + 32 * i;
        if (word(at + 8) != target || word(at + 12) != 64)
            continue;
        // The private target ABI consumes all four channels at that register.
        if (component || word(at) != 0 || word(at + 20) != target || (word(at + 24) & 255u) != 15u)
            return 0;
        component = word(at + 16);
    }
    return component;
}
// Reflection/statistics/checksum packaging may change when a shader is cached.
// Executable code, signatures, resource validation and feature requirements must agree.
inline bool same_shader(std::span<const char> a, std::span<const char> b)
{
    if (a.size() == b.size() && !a.empty() && !std::memcmp(a.data(), b.data(), a.size()))
        return true;
    for (const char *name : {"DXIL", "ISG1", "OSG1", "PSV0", "SFI0"})
    {
        const auto x = shader_chunk(a, name), y = shader_chunk(b, name);
        if (x.empty() || y.empty() || x.size() != y.size() || std::memcmp(x.data(), y.data(), x.size()))
            return false;
    }
    return true;
}
} // namespace dingosdk::overlay::detail
