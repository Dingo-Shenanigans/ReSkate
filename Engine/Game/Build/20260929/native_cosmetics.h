#pragma once
#include <array>
#include <cstdint>

// Remote skater and skateboard cosmetics.
namespace dingosdk::game::build::v20260929::native_cosmetics {
// Customization component that owns the recipe at +0x150.
inline constexpr std::uintptr_t cosmetic_component_vtable = 0x6084c88;
// Deep-copies a recipe value into a component (component, recipe).
inline constexpr std::uintptr_t recipe_copy = 0x5230b0;
inline constexpr std::array<unsigned char, 24> recipe_copy_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x08,0x48,0x8b,
    0xf9,0x48,0x8b,0x98,0x90,0x00,0x00,0x00};
// Publishes this AppearanceItem's per-instance named material map to its render meshes.
// (item, map_index), with map_index 0 for addMaterial("Top_mat").
inline constexpr std::uintptr_t publish_materials = 0x1331420;
inline constexpr std::array<unsigned char, 24> publish_materials_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,
    0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20};
// Typed per-instance shader setter reached by MaterialInstance:addEsVector.
// (material, ParamDbKey*, NativeValue*, priority) -> success. Inserts with the
// engine allocator and preserves the existing priority rules.
inline constexpr std::uintptr_t set_shader_parameter = 0x1312d00;
inline constexpr std::array<unsigned char, 24> set_shader_parameter_prefix{
    0x48,0x89,0x5c,0x24,0x20,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,
    0x55,0x48,0x8d,0x6c,0x24,0xc9,0x48,0x81};
// Initialized ColorRgb type descriptor; validate its type hash, size and index.
inline constexpr std::uintptr_t color_rgb_type = 0x7780c50;
// Finds a named entry in a loaded table by the FNV-1a hash of its name:
// (out, name, table, flag, context) -> out, the fifth passed on the stack.
// The skater loader's preset lookups reach it through a Lua binding that
// passes a nil table on when a preset's bundle was not found, and the function
// reads the table without checking it.
inline constexpr std::uintptr_t named_lookup = 0x16fc900;
inline constexpr std::array<unsigned char, 24> named_lookup_prefix{
    0x40,0x55,0x56,0x57,0x41,0x56,0x48,0x81,0xec,0xd8,0x02,0x00,0x00,0x48,0x8b,0x05,
    0xac,0x7a,0xac,0x05,0x48,0x33,0xc4,0x48};
// The skater scripts' item:addEsShaderPreset(material, preset): a Lua function (state) -> count
// of results. Its third argument is a reference to the loaded preset, which it uses without
// checking that the preset is there: an item whose preset has gone (or never loaded) by the
// time its script runs crashes the game in the material's construction.
inline constexpr std::uintptr_t add_shader_preset = 0x52cb70;
inline constexpr std::array<unsigned char, 16> add_shader_preset_prefix{
    0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x28,0x48,0x8b,0xd9};
// The two calls it makes to read that argument: (state, argument number) -> the reference, and
// (reference, 0) -> the preset it holds, or null.
inline constexpr std::uintptr_t script_preset_reference = 0x336f060;
inline constexpr std::array<unsigned char, 8> script_preset_reference_prefix{0x4c,0x8d,0x05,0x81,0xa2,0x0d,0x03,0xe9};
inline constexpr std::uintptr_t referenced_preset = 0x16fe9c0;
inline constexpr std::array<unsigned char, 17> referenced_preset_prefix{
    0x48,0x8b,0x09,0x48,0x85,0xc9,0x74,0x07,0x48,0x8b,0x01,0x48,0xff,0x60,0x18,0x33,0xc0};
}
