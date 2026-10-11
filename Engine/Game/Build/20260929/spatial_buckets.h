#pragma once

#include "entity_pages.h"

namespace dingosdk::game::build::v20260929::spatial_buckets {
using entity_pages::Kind;
using entity_pages::Patch;
using entity_pages::patch;

// Each native block holds 64 bounds. The flat root bucket previously overflowed
// at 2049 blocks, aliasing its first block pointer and count with array writes.
// Keep native block ownership and IDs; enlarge every bucket array together.
inline constexpr std::size_t blocks_per_bucket = 16'384;
inline constexpr std::size_t entries_per_block = 64;
inline constexpr std::size_t nodes_per_root = 9;
struct BucketLayout {
    std::uint32_t block_ids[blocks_per_bucket];
    std::uint64_t block_pointers[blocks_per_bucket];
    std::uint32_t block_count;
    std::uint8_t occupancy[blocks_per_bucket];
    std::uint16_t changed_ids[blocks_per_bucket];
    std::uint16_t changed_count;
};
struct alignas(16) RootLayout {
    std::uint8_t prefix[0x130];
    BucketLayout nodes[nodes_per_root];
    std::uint32_t node_count;
};
// (Left at eight times the game's while the entity pages are at four: room to spare.)
static_assert(blocks_per_bucket * entries_per_block >= entity_pages::capacity);
static_assert(blocks_per_bucket < 65'536);
static_assert(offsetof(BucketLayout, block_pointers) == 0x10000);
static_assert(offsetof(BucketLayout, block_count) == 0x30000);
static_assert(offsetof(BucketLayout, occupancy) == 0x30004);
static_assert(offsetof(BucketLayout, changed_ids) == 0x34004);
static_assert(offsetof(BucketLayout, changed_count) == 0x3c004);
static_assert(sizeof(BucketLayout) == 0x3c008);
static_assert(offsetof(RootLayout, node_count) == 0x21c178);
static_assert(sizeof(RootLayout) == 0x21c180);

// Full instruction fingerprints: allocation/clear, queries, insert, move, remove
// and scalar root traversal. The final two records are the two uint64 lanes of
// the AVX-512 traversal's shared root-stride literal. Transaction queues, stack
// frames and independent render-packet arrays retain their original sizes.
inline constexpr Patch patches[]{
    patch(0x01967434, Kind::spatial_bucket_layout, "8b9178390400", "8b9178c12100"),
    patch(0x01967480, Kind::spatial_bucket_layout, "4869c808780000", "4869c808c00300"),
    patch(0x01967492, Kind::spatial_bucket_layout, "8b8100600000", "8b8100000300"),
    patch(0x019674a4, Kind::spatial_bucket_layout, "410fb6ac0c04600000", "410fb6ac0c04000300"),
    patch(0x019674af, Kind::spatial_bucket_layout, "4e8bbce100200000", "4e8bbce100000100"),
    patch(0x0196e941, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x019704d4, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02b23bb0, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02b23bbd, Kind::spatial_bucket_layout, "8b943978390400", "8b943978c12100"),
    patch(0x02b23be0, Kind::spatial_bucket_layout, "4c69f808780000", "4c69f808c00300"),
    patch(0x02b23bec, Kind::spatial_bucket_layout, "458baf00600000", "458baf00000300"),
    patch(0x02b23c00, Kind::spatial_bucket_layout, "420fb6b43d04600000", "420fb6b43d04000300"),
    patch(0x02b23c0b, Kind::spatial_bucket_layout, "498bbcef00200000", "498bbcef00000100"),
    patch(0x02b636ad, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02b636ba, Kind::spatial_bucket_layout, "8b943978390400", "8b943978c12100"),
    patch(0x02b636e0, Kind::spatial_bucket_layout, "4c69e808780000", "4c69e808c00300"),
    patch(0x02b636f7, Kind::spatial_bucket_layout, "418b8500600000", "418b8500000300"),
    patch(0x02b63710, Kind::spatial_bucket_layout, "4f8bb4e500200000", "4f8bb4e500000100"),
    patch(0x02b63720, Kind::spatial_bucket_layout, "470fb6bc2c04600000", "470fb6bc2c04000300"),
    patch(0x02b64e6f, Kind::spatial_bucket_layout, "8b9a78390400", "8b9a78c12100"),
    patch(0x02b64ec5, Kind::spatial_bucket_layout, "4c69c008780000", "4c69c008c00300"),
    patch(0x02b64ed4, Kind::spatial_bucket_layout, "458bb000600000", "458bb000000300"),
    patch(0x02b64ef0, Kind::spatial_bucket_layout, "420fb68c0204600000", "420fb68c0204000300"),
    patch(0x02b64efc, Kind::spatial_bucket_layout, "420fb6840004600000", "420fb6840004000300"),
    patch(0x02b64f1e, Kind::spatial_bucket_layout, "420fb68c0204600000", "420fb68c0204000300"),
    patch(0x02b64f70, Kind::spatial_bucket_layout, "4869c808780000", "4869c808c00300"),
    patch(0x02b64f88, Kind::spatial_bucket_layout, "8b9100600000", "8b9100000300"),
    patch(0x02b64fa0, Kind::spatial_bucket_layout, "488bb4c100200000", "488bb4c100000100"),
    patch(0x02b64faa, Kind::spatial_bucket_layout, "0fb6840804600000", "0fb6840804000300"),
    patch(0x02b68d24, Kind::spatial_bucket_layout, "4869d080390400", "4869d080c12100"),
    patch(0x02bf511d, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02bf512a, Kind::spatial_bucket_layout, "8b943978390400", "8b943978c12100"),
    patch(0x02bf5150, Kind::spatial_bucket_layout, "4c69e808780000", "4c69e808c00300"),
    patch(0x02bf515c, Kind::spatial_bucket_layout, "418b8500600000", "418b8500000300"),
    patch(0x02bf5170, Kind::spatial_bucket_layout, "420fb6bc2d04600000", "420fb6bc2d04000300"),
    patch(0x02bf517b, Kind::spatial_bucket_layout, "498bb4ed00200000", "498bb4ed00000100"),
    patch(0x02c452e1, Kind::spatial_bucket_layout, "8b9178390400", "8b9178c12100"),
    patch(0x02c45320, Kind::spatial_bucket_layout, "4c69e808780000", "4c69e808c00300"),
    patch(0x02c4532d, Kind::spatial_bucket_layout, "418b8500600000", "418b8500000300"),
    patch(0x02c45340, Kind::spatial_bucket_layout, "430fb6b42e04600000", "430fb6b42e04000300"),
    patch(0x02c4534b, Kind::spatial_bucket_layout, "4b8bacf500200000", "4b8bacf500000100"),
    patch(0x02c4980c, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02cca351, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02cca87e, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02ccad91, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02cd2c66, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02cd6071, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02cde88d, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02cde89d, Kind::spatial_bucket_layout, "8b943178390400", "8b943178c12100"),
    patch(0x02cde8c0, Kind::spatial_bucket_layout, "4c69e808780000", "4c69e808c00300"),
    patch(0x02cde8cd, Kind::spatial_bucket_layout, "418b8500600000", "418b8500000300"),
    patch(0x02cde8e0, Kind::spatial_bucket_layout, "430fb6b42e04600000", "430fb6b42e04000300"),
    patch(0x02cde8eb, Kind::spatial_bucket_layout, "4b8bacf500200000", "4b8bacf500000100"),
    patch(0x02fc5afb, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x02ff3070, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x0489b56b, Kind::spatial_bucket_layout, "ba80390400", "ba80c12100"),
    patch(0x0489b58f, Kind::spatial_bucket_layout, "41b880390400", "41b880c12100"),
    patch(0x0489b5b0, Kind::spatial_bucket_layout, "c7807839040001000000", "c78078c1210001000000"),
    patch(0x0489ba76, Kind::spatial_bucket_layout, "4869de80390400", "4869de80c12100"),
    patch(0x0489bac4, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x0489bacf, Kind::spatial_bucket_layout, "c784017839040001000000", "c7840178c1210001000000"),
    patch(0x0489cb0d, Kind::spatial_bucket_layout, "418b8e30610000", "418b8e30010300"),
    patch(0x0489cb30, Kind::spatial_bucket_layout, "4280bc333461000040", "4280bc333401030040"),
    patch(0x0489cb52, Kind::spatial_bucket_layout, "49898e30210000", "49898e30010100"),
    patch(0x0489cb73, Kind::spatial_bucket_layout, "41898e30610000", "41898e30010300"),
    patch(0x0489cb7a, Kind::spatial_bucket_layout, "420fb6b43334610000", "420fb6b43334010300"),
    patch(0x0489cb98, Kind::spatial_bucket_layout, "41898e30610000", "41898e30010300"),
    patch(0x0489cba2, Kind::spatial_bucket_layout, "4989bcde30210000", "4989bcde30010100"),
    patch(0x0489cbc1, Kind::spatial_bucket_layout, "498bbcde30210000", "498bbcde30010100"),
    patch(0x0489cc1c, Kind::spatial_bucket_layout, "4288b43334610000", "4288b43334010300"),
    patch(0x0489cc24, Kind::spatial_bucket_layout, "410fb79634790000", "410fb79634c10300"),
    patch(0x0489cc35, Kind::spatial_bucket_layout, "6641399c4e34690000", "6641399c4e34410300"),
    patch(0x0489cc46, Kind::spatial_bucket_layout, "6641899c5634690000", "6641899c5634410300"),
    patch(0x0489cc52, Kind::spatial_bucket_layout, "6641899634790000", "6641899634c10300"),
    patch(0x0489d7cd, Kind::spatial_bucket_layout, "4869f908780000", "4869f908c00300"),
    patch(0x0489d7e0, Kind::spatial_bucket_layout, "0fb68c3e04600000", "0fb68c3e04000300"),
    patch(0x0489d7e8, Kind::spatial_bucket_layout, "4c8b8cf700200000", "4c8b8cf700000100"),
    patch(0x0489d822, Kind::spatial_bucket_layout, "0fb79704780000", "0fb79704c00300"),
    patch(0x0489d830, Kind::spatial_bucket_layout, "6639b44704680000", "6639b44704400300"),
    patch(0x0489d840, Kind::spatial_bucket_layout, "6689b45704680000", "6689b45704400300"),
    patch(0x0489d84b, Kind::spatial_bucket_layout, "66899704780000", "66899704c00300"),
    patch(0x0489dfb4, Kind::spatial_bucket_layout, "4869c180390400", "4869c180c12100"),
    patch(0x0489e971, Kind::spatial_bucket_layout, "4c69c908780000", "4c69c908c00300"),
    patch(0x0489e985, Kind::spatial_bucket_layout, "430fb68c0b04600000", "430fb68c0b04000300"),
    patch(0x0489e98e, Kind::spatial_bucket_layout, "4b8b9cd900200000", "4b8b9cd900000100"),
    patch(0x0489ea66, Kind::spatial_bucket_layout, "43fe8c0b04600000", "43fe8c0b04000300"),
    patch(0x0489ea6e, Kind::spatial_bucket_layout, "410fb79104780000", "410fb79104c00300"),
    patch(0x0489ea80, Kind::spatial_bucket_layout, "6645399c4904680000", "6645399c4904400300"),
    patch(0x0489ea91, Kind::spatial_bucket_layout, "6645899c5104680000", "6645899c5104400300"),
    patch(0x0489ea9d, Kind::spatial_bucket_layout, "6641899104780000", "6641899104c00300"),
    patch(0x04ead2a1, Kind::spatial_bucket_layout, "4869f880390400", "4869f880c12100"),
    patch(0x04eb2856, Kind::spatial_bucket_layout, "4869d080390400", "4869d080c12100"),
    patch(0x04eb6703, Kind::spatial_bucket_layout, "4869c880390400", "4869c880c12100"),
    patch(0x04eb9a90, Kind::spatial_bucket_layout, "448ba200600000", "448ba200000300"),
    patch(0x04eb9db9, Kind::spatial_bucket_layout, "0fb6bc0604600000", "0fb6bc0604000300"),
    patch(0x04eb9dc1, Kind::spatial_bucket_layout, "4c8bb4c600200000", "4c8bb4c600000100"),
    patch(0x04eecb4d, Kind::spatial_bucket_layout, "8ba978390400", "8ba978c12100"),
    patch(0x04eecb7f, Kind::spatial_bucket_layout, "4869d008780000", "4869d008c00300"),
    patch(0x065ee840, Kind::spatial_bucket_stride, "8039040000000000", "80c1210000000000"),
    patch(0x065ee848, Kind::spatial_bucket_stride, "8039040000000000", "80c1210000000000"),
};
}

