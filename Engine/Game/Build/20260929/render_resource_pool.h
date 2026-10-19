// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// These are the complete instructions changed by the render-resource pool
// redirect. Keeping the before and after bytes together makes partial opcode
// patches impossible.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dingosdk::game::build::v20260929::render_resource_pool {
template<std::size_t N> consteval auto hex_bytes(const char (&hex)[N]) {
    static_assert(N % 2 == 1);
    std::array<std::uint8_t, (N - 1) / 2> result{};
    const auto nibble = [](char value) -> unsigned {
        if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
        throw "Invalid native render-resource contract hex";
    };
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>(
            (nibble(hex[index * 2]) << 4) | nibble(hex[index * 2 + 1]));
    return result;
}

inline constexpr auto constructor_entry = hex_bytes(
    "48895c2410488974241855574156488d6c24a04881ec60010000488b059f5034");
inline constexpr auto index_table_constructor_entry = hex_bytes(
    "4053555657415641574883ec78c5f9efc033ed488bf948896c2428488d4c2440");

// mov ecx, 0x2daa00 -> mov ecx, 0x11ab080 (the state owner's size, 0x3efd347).
inline constexpr auto other_renderer_size_before = hex_bytes("b900aa2d00");
inline constexpr auto other_renderer_size_after  = hex_bytes("b980b01a01");
inline constexpr auto vector_begin_before = hex_bytes("488d81b0000000");
inline constexpr auto vector_begin_after  = hex_bytes("488b8190000000");
inline constexpr auto vector_bytes_before = hex_bytes("480500001e00");
inline constexpr auto vector_bytes_after  = hex_bytes("48050000f000");
inline constexpr auto capacity_before = hex_bytes("81fa00200000");
inline constexpr auto capacity_after  = hex_bytes("81fa00000100");
inline constexpr auto append_metadata_before = hex_bytes("8bc941c7848f80032d00f0237449");
inline constexpr auto append_metadata_after  = hex_bytes("498b9728ae1a01c7048af0237449");

inline constexpr auto view_begin_before = hex_bytes("498d9680032d00");
inline constexpr auto view_begin_after  = hex_bytes("498b9628ae1a01");
inline constexpr auto view_end_before = hex_bytes("488d8a00800000");
inline constexpr auto view_end_after  = hex_bytes("498b8e30ae1a01");
inline constexpr auto view_store_end_before = hex_bytes("498d8680832d00");
inline constexpr auto view_store_end_after  = hex_bytes("498b8630ae1a01");

inline constexpr auto metadata_init_end_before = hex_bytes("488d8780832d00");
inline constexpr auto metadata_init_end_after  = hex_bytes("488b8730ae1a01");
inline constexpr auto metadata_init_begin_before = hex_bytes("488d9780032d00");
inline constexpr auto metadata_init_begin_after  = hex_bytes("488b9728ae1a01");
inline constexpr auto metadata_init_loop_end_before = hex_bytes("488d8a00800000");
inline constexpr auto metadata_init_loop_end_after  = hex_bytes("488b8f30ae1a01");
inline constexpr auto resource_state_begin_before = hex_bytes("498d4618");
inline constexpr auto resource_state_begin_after  = hex_bytes("498b0690");
inline constexpr auto resource_state_bytes_before = hex_bytes("48050000e700");
inline constexpr auto resource_state_bytes_after  = hex_bytes("480500003807");
inline constexpr auto resource_state_count_before = hex_bytes("bd00200000");
inline constexpr auto resource_state_count_after  = hex_bytes("bd00000100");
inline constexpr auto resource_state_end_add_before = hex_bytes("4881c30000e700");
inline constexpr auto resource_state_end_add_after  = hex_bytes("4881c300003807");
inline constexpr auto resource_state_end_store_before = hex_bytes("48050000e700");
inline constexpr auto resource_state_end_store_after  = hex_bytes("480500003807");
inline constexpr auto relation_masks_begin_before = hex_bytes("498d4718");
inline constexpr auto relation_masks_begin_after  = hex_bytes("498b0790");
inline constexpr auto relation_masks_bytes_before = hex_bytes("480500000600");
inline constexpr auto relation_masks_bytes_after  = hex_bytes("480500003000");
inline constexpr auto relation_masks_end_before = hex_bytes("498d8000000600");
inline constexpr auto relation_masks_end_after  = hex_bytes("498d8000003000");
inline constexpr auto resource_lookup_clamp_before = hex_bytes("b9ff1f0000");
inline constexpr auto resource_lookup_clamp_after  = hex_bytes("b9ffff0000");
inline constexpr auto resource_flags_fill_before = hex_bytes("4881c180800400");
inline constexpr auto resource_flags_fill_after  = hex_bytes("488b8930abf100");
inline constexpr auto resource_flags_read_priority = hex_bytes("430fb6840180832d00");
inline constexpr auto resource_flags_read_visibility = hex_bytes("0fb6940f80832d00");

struct Patch {
    std::uintptr_t rva;
    std::span<const std::uint8_t> expected;
    std::span<const std::uint8_t> replacement;
};

// Renderer owner fields the redirected instructions above address.
inline constexpr std::ptrdiff_t vector_begin_offset = 0x90;
inline constexpr std::ptrdiff_t vector_end_offset = 0x98;
inline constexpr std::ptrdiff_t vector_capacity_offset = 0xa0;
inline constexpr std::ptrdiff_t metadata_begin_offset = 0x11aae28;
inline constexpr std::ptrdiff_t metadata_end_offset = 0x11aae30;
inline constexpr std::ptrdiff_t resource_state_vector_offset = 0x2da920;
inline constexpr std::ptrdiff_t relation_mask_vector_offset = 0x114ab60;

inline constexpr std::uintptr_t constructor_rva = 0x03e7f300;
// The constructor is the base of two renderers. Only the one constructed here
// (vtable 0x64f2c10, 0x11ab080 bytes) has the resource-state and relation-mask
// vectors: every read of owner+0x2da920 and owner+0x114ab60 is in its code. The
// other (constructed at 0x030a0a46, vtable 0x6414168, kept at 0x75f2700) keeps
// fields of its own at +0x2da920. Both use the metadata: the patched view
// (0x030e6bff, called from 0x30a7e18 with the other renderer) and append
// (0x03e94fe6, called from 0x30d8ebb) read its pointers at +0x11aae28/+0x11aae30.
// The other renderer was allocated 0x2daa00 bytes, which ends before them, so it
// is allocated the state owner's 0x11ab080 instead (other_renderer_size, above).
inline constexpr std::uintptr_t state_owner_call_rva = 0x03f33a46;
inline constexpr std::uintptr_t index_table_constructor_rva = 0x03e7e400;
inline constexpr std::uintptr_t resource_flags_priority_rva = 0x03f56073;
inline constexpr std::uintptr_t resource_flags_visibility_rva = 0x03f5789a;
inline constexpr Patch patches[]{
    // The other renderer's allocation, large enough for the metadata pointers.
    {0x030c64cc, other_renderer_size_before, other_renderer_size_after},
    {0x03e7f33c, vector_begin_before, vector_begin_after},
    {0x03e7f351, vector_bytes_before, vector_bytes_after},
    {0x03e94e99, capacity_before, capacity_after},
    {0x03e94fe6, append_metadata_before, append_metadata_after},
    {0x030e6bff, view_begin_before, view_begin_after},
    {0x030e6c0d, view_end_before, view_end_after},
    {0x030e6c1b, view_store_end_before, view_store_end_after},
    {0x03f3f007, metadata_init_end_before, metadata_init_end_after},
    {0x03f3f058, metadata_init_begin_before, metadata_init_begin_after},
    {0x03f3f05f, metadata_init_loop_end_before, metadata_init_loop_end_after},
    {0x03f33b3f, resource_state_begin_before, resource_state_begin_after},
    {0x03f33b5c, resource_state_bytes_before, resource_state_bytes_after},
    // The native constructor sizes and constructs both synchronized arrays to
    // 8,192 entries after assigning their storage. Capacity alone is not
    // sufficient: high resource IDs would otherwise reach reserved but
    // unconstructed 0x738-byte state records.
    {0x03f33d9e, resource_state_count_before, resource_state_count_after},
    {0x03f33dce, resource_state_end_add_before, resource_state_end_add_after},
    {0x03f33df7, resource_state_end_store_before, resource_state_end_store_after},
    {0x03f33ba2, relation_masks_begin_before, relation_masks_begin_after},
    {0x03f33bad, relation_masks_bytes_before, relation_masks_bytes_after},
    {0x03f33ec1, relation_masks_end_before, relation_masks_end_after},
    // A render-worker lookup independently clamps the 16-bit resource ID
    // before indexing the state vector. Keep that ID paired with the raw ID
    // consumed by the following routine instead of folding 0x2000+ entries
    // onto stock slot 0x1fff.
    {0x03f60237, resource_lookup_clamp_before, resource_lookup_clamp_after},
    // The fill helper receives owner+0x290410. The replacement loads the
    // metadata end pointer at owner+0x11ab400, which is also the beginning of
    // the adjacent external per-resource byte flags.
    {0x03e898f3, resource_flags_fill_before, resource_flags_fill_after},
};

// Generated resource-flag stubs: the replaced priority/visibility reads, now
// loading the external flag array from the metadata end pointer.
inline constexpr std::array<std::uint8_t, 12> priority_stub_prefix{
    0x49, 0x8b, 0x80, 0x30, 0xae, 0x1a, 0x01, // mov rax,[r8+0x11ab400]
    0x42, 0x0f, 0xb6, 0x04, 0x08              // movzx eax,byte ptr [rax+r9]
};
inline constexpr std::array<std::uint8_t, 11> visibility_stub_prefix{
    0x48, 0x8b, 0x97, 0x30, 0xae, 0x1a, 0x01, // mov rdx,[rdi+0x11ab400]
    0x0f, 0xb6, 0x14, 0x0a                    // movzx edx,byte ptr [rdx+rcx]
};
} // namespace dingosdk::game::build::v20260929::render_resource_pool
