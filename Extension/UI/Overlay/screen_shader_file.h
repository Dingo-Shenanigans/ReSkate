#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// A plugin's screen shader file (reskate.screen.shader; screen_shader.h draws it), checked before anything is made
// of it. No graphics here, so the plugin host and its tests read it too.
//
// A screen shader is one pixel shader, drawn over the whole finished frame (the game's HUD in it too) ahead of the
// overlay's own drawing. It may be:
//   - HLSL source (.hlsl), compiled by ReSkate (ps_5_0, entry `main`) with `prelude` in front of it;
//   - a compiled shader container (.cso, .dxbc or .dxil): DXBC (shader model 5) or DXIL (shader model 6, signed by
//     the compiler's validator, as dxc does by default), a pixel shader compiled against the same bindings.
// Its bindings, which ReSkate gives it and nothing else:
//   b0  the cbuffer below, as root constants: 2 float4s of ReSkate's, then the plugin's 12 (`values`);
//   t0  a copy of the frame, as it was before the shader (Texture2D, the back buffer's own format and encoding);
//   s0  linear filtering, clamped at the edges; s1 the nearest pixel, clamped.
// Its input: SV_Position (pixels) and TEXCOORD0 (0..1 across the frame, y down); its output one float4 at
// SV_Target0, which replaces the frame's pixel (alpha is not blended).
namespace dingosdk::screen_shader {
inline constexpr std::size_t value_vectors = 12;                // the plugin's float4s
inline constexpr std::size_t value_count = value_vectors * 4;   // its numbers, `values`
inline constexpr std::size_t most_source_bytes = 256u * 1024u;  // an .hlsl file
inline constexpr std::size_t most_compiled_bytes = 1024u * 1024u; // a compiled container
inline constexpr std::size_t most_parts = 64;
// A world shader's data (reskate.world.shader_data; world_shader.h): whole float4s, at most this many bytes.
inline constexpr std::size_t world_data_most_bytes = 64u * 1024u;

// In front of a plugin's HLSL source. A compiled shader declares the same itself (the bindings are what count;
// the names are the plugin's own).
inline constexpr const char *prelude = R"hlsl(
cbuffer ReSkateScreen : register(b0) {
    float4 reskate_screen;     // width, height (pixels), 1 / width, 1 / height
    float4 reskate_clock;      // seconds (counting from 0 again every 1000), seconds since the last frame, the
                               // output's transfer (0 SDR, 1 scRGB, 2 HDR10), its white in nits (80 for SDR)
    float4 reskate_values[12]; // the plugin's (reskate.screen.shader's values, four to a float4)
};
Texture2D<float4> reskate_frame : register(t0);
SamplerState reskate_linear : register(s0);
SamplerState reskate_point : register(s1);
#line 1
)hlsl";

enum class Kind : std::uint8_t { hlsl, dxbc, dxil };

namespace detail {
inline std::uint32_t read32(std::string_view bytes, std::size_t at) {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
inline bool ends_with(std::string_view name, std::string_view end) {
    if (name.size() < end.size()) return false;
    for (std::size_t i = 0; i < end.size(); ++i) {
        char c = name[name.size() - end.size() + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != end[i]) return false;
    }
    return true;
}
} // namespace detail

// Empty when `bytes` (the file `name`) is a screen shader ReSkate can take, with its kind; else why not.
inline std::string check_file(std::string_view name, std::string_view bytes, Kind &kind) {
    using detail::read32;
    if (detail::ends_with(name, ".hlsl")) {
        if (bytes.empty()) return "it is empty";
        if (bytes.size() > most_source_bytes) return "an .hlsl screen shader is at most 256 KB";
        if (bytes.find('\0') != std::string_view::npos) return "an .hlsl screen shader is text";
        kind = Kind::hlsl;
        return {};
    }
    if (!detail::ends_with(name, ".cso") && !detail::ends_with(name, ".dxbc") && !detail::ends_with(name, ".dxil"))
        return "a screen shader is an .hlsl, .cso, .dxbc or .dxil file";
    if (bytes.size() > most_compiled_bytes) return "a compiled screen shader is at most 1 MB";
    // The container: "DXBC", a 16-byte hash, version, its whole size, how many parts, then each part's offset;
    // each part a four-character code, its size, then its bytes.
    if (bytes.size() < 32 || bytes.substr(0, 4) != "DXBC") return "it is not a compiled shader (no DXBC container)";
    if (read32(bytes, 24) != bytes.size()) return "its container's size is not the file's";
    const std::uint32_t parts = read32(bytes, 28);
    if (parts == 0 || parts > most_parts || 32 + std::size_t{parts} * 4 > bytes.size()) return "its container's parts are not readable";
    bool dxil = false, dxbc = false;
    std::uint32_t version = 0;
    for (std::uint32_t i = 0; i < parts; ++i) {
        const std::size_t at = read32(bytes, 32 + std::size_t{i} * 4);
        if (at < 32 + std::size_t{parts} * 4 || at + 8 > bytes.size()) return "its container's parts are not readable";
        const auto code = bytes.substr(at, 4);
        const std::size_t size = read32(bytes, at + 4);
        if (size > bytes.size() - at - 8) return "its container's parts are not readable";
        // DXIL: a program header whose first word is the shader's kind (pixel 0) and model; DXBC's shader code
        // (SHEX or SHDR): a version token whose top half is the program's type (pixel 0).
        if (code == "DXIL" && size >= 4) dxil = true, version = read32(bytes, at + 8);
        else if ((code == "SHEX" || code == "SHDR") && size >= 4) dxbc = true, version = read32(bytes, at + 8);
    }
    if (dxil == dxbc) return "its container holds no shader code";
    if ((version >> 16) != 0) return "it is not a pixel shader";
    if (dxil && read32(bytes, 4) == 0 && read32(bytes, 8) == 0 && read32(bytes, 12) == 0 && read32(bytes, 16) == 0)
        return "its DXIL is not signed (compile it with dxc's validator on, its default)";
    kind = dxil ? Kind::dxil : Kind::dxbc;
    return {};
}
} // namespace dingosdk::screen_shader
