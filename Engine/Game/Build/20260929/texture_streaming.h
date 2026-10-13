#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstdint>

// The game's TextureStreamingManager and its limit of streamed textures
// (Extension/World/texture_handle_limit.cpp).
//
// Every streamed texture holds a 16-bit handle: an index into the manager's table of slots
// (0x58 bytes each), 0xffff for none, and the code masks the index with 0x7fff. The table and
// what goes with it are sized for fb::MaxStreamingTextureCount, 0x5000 (20480), all inside the
// manager itself (one object of 0x24e780 bytes, FUN_143efd770 allocates it and keeps it at
// `manager`):
//   +0x480      a list of pointers, its 0x5000 entries at +0x498
//   +0x28498    the slots
//   +0x1e0498   how many slots are taken from the bottom (one kind of texture), +0x1e049c where
//               those taken from the top begin (the other kind; 0x5000 when none), and a hint
//               for each at +0x1e04a0 and +0x1e04a4
//   +0x1e0500   a float for each slot
//   +0x1f4500   two lists of pointers, 0x5000 entries each: headers at +0x1f4500 and +0x21c518,
//               entries behind each header
//   +0x244530   a sorted list of handles, its 0x5000 entries at +0x244548
// The lists are {begin, end, capacity end}; only the constructor names where their entries
// are. When the slots from the bottom and from the top meet, FUN_143e72660 writes a message
// ("Exceeded max streaming texture handle count") nobody reads and takes the next slot all the
// same: one past the table, which is the counter at +0x1e0498, whose low byte it then waits on
// as a lock for ever (a stall and then a crash in the stall's own report, 2026-10-11).
//
// The limit is raised by giving the manager a longer allocation and moving the slots, the
// floats and the lists' entries behind its end: every instruction that holds one of those
// offsets or sizes is in texture_streaming_sites.inc (a sweep of the build's code, checked two
// ways; nothing outside 0x3e5b350..0x3e77518 and the allocation holds one), and each is the
// same instruction with another number. The lists are pointed at their new entries once the
// constructor has run. The counters, the hints and everything else stay where they are.
//
// The renderer keeps floats of its own for every slot: an object for each view (0x500c0 bytes,
// made at 0x3f3d83a) with a block of 0x5000 floats for each of four layers from +0x40, a
// {pointer, count} for each block from +0x50040, and a pointer at +0x50080. It fills each
// block for the manager's count of slots (FUN_143e6c8e0 hands the view the manager's floats and
// that count), so the blocks have to grow with it: raising the manager alone ran the fill over
// the pointer behind the blocks (a crash at 0x3f31d2c on the first frame, 2026-10-11). Three
// functions go into the blocks: FUN_143f31aa0 (the fill) and FUN_143f319c0 (the merge into the
// manager's floats), both by layer * 0x5000 + 0x10 floats, written as lea [r + r*4], shl 12,
// add 0x10; and FUN_143f56560 (the jobs that write them), by the {pointer, count} of its
// layer, found as (layer + 0x5004) * 16. Nothing else in the code has that arithmetic. The
// blocks become raised_count floats each, the fields behind them move up, and the two
// lea/shl/add become an imul and the same add (renderer_rewrites).
namespace dingosdk::game::build::v20260929::texture_streaming {
inline constexpr std::uintptr_t manager = 0x76e92e0; // TextureStreamingManager*, null until made
inline constexpr Fingerprint constructor{0x3e5b350, {
    0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
    0x48,0x8d,0xac,0x24,0x20,0xff,0xff,0xff,0x48,0x81,0xec,0xe0,0x01,0x00,0x00,0x48}};
// __report_gsfailure: mov ecx, 2; int 0x29; ret. Every function's stack check jumps to it
// (FUN_14546ba30) when its cookie is not what it left (Extension/World/stack_check_report.cpp).
inline constexpr std::uintptr_t stack_check_failed = 0x546be9c;
inline constexpr std::array<unsigned char, 8> stack_check_failed_bytes{0xb9, 0x02, 0x00, 0x00, 0x00, 0xcd, 0x29, 0xc3};
inline constexpr std::uint32_t stock_count = 0x5000;
// The most the 15-bit index allows, a multiple of four (the floats are reset sixteen bytes at
// a time) and short of 0x7fff, which with the index's high bit set would read as no handle.
inline constexpr std::uint32_t raised_count = 0x7ffc;
inline constexpr std::uint32_t slot_size = 0x58;
inline constexpr std::uint32_t taken_from_bottom = 0x1e0498, taken_from_top = 0x1e049c;

inline constexpr std::uint32_t stock_size = 0x24e780;
inline constexpr std::uint32_t stock_slots = 0x28498, stock_floats = 0x1e0500;
// Behind the stock object, in this order.
inline constexpr std::uint32_t raised_slots = stock_size;
inline constexpr std::uint32_t raised_slot_bytes = raised_count * slot_size;
inline constexpr std::uint32_t raised_slots_end = raised_slots + raised_slot_bytes;
inline constexpr std::uint32_t raised_floats = raised_slots_end;
inline constexpr std::uint32_t raised_float_bytes = raised_count * 4;
struct List {
    std::uint32_t header, stock_entries, raised_entries, entry_size;
};
inline constexpr std::uint32_t lists_begin = raised_floats + raised_float_bytes;
inline constexpr List lists[]{
    {0x480, 0x498, lists_begin, 8},
    {0x1f4500, 0x1f4518, lists_begin + raised_count * 8, 8},
    {0x21c518, 0x21c530, lists_begin + raised_count * 16, 8},
    {0x244530, 0x244548, lists_begin + raised_count * 24, 2},
};
inline constexpr std::uint32_t raised_size = (lists_begin + raised_count * 26 + 0x7f) & ~0x7fu;
static_assert(raised_slots % 8 == 0 && raised_floats % 4 == 0 && lists_begin % 8 == 0 && raised_float_bytes % 16 == 0);

// The renderer's object: four blocks of floats from +0x40, then its fields.
inline constexpr std::uint32_t renderer_blocks = 0x40;
inline constexpr std::uint32_t renderer_stock_fields = renderer_blocks + 4 * stock_count * 4;   // 0x50040
inline constexpr std::uint32_t renderer_raised_fields = renderer_blocks + 4 * raised_count * 4; // 0x80000
static_assert(renderer_stock_fields == 0x50040 && renderer_raised_fields % 16 == 0);
// layer * count + 0x10, in floats: twelve bytes either way.
struct Rewrite {
    std::uint32_t rva;
    std::uint8_t stock[12], raised[12];
};
inline constexpr Rewrite renderer_rewrites[]{
    // lea r8, [r10 + r10*4]; shl r8, 12; add r8, 0x10  ->  imul r8, r10, raised_count; add r8, 0x10; nop
    {0x3f31a21, {0x4f, 0x8d, 0x04, 0x92, 0x49, 0xc1, 0xe0, 0x0c, 0x49, 0x83, 0xc0, 0x10},
     {0x4d, 0x69, 0xc2, raised_count & 0xff, raised_count >> 8, 0, 0, 0x49, 0x83, 0xc0, 0x10, 0x90}},
    // lea rdx, [rax + rax*4]; shl rdx, 12; add rdx, 0x10  ->  imul rdx, rax, raised_count; add rdx, 0x10; nop
    {0x3f31cfa, {0x48, 0x8d, 0x14, 0x80, 0x48, 0xc1, 0xe2, 0x0c, 0x48, 0x83, 0xc2, 0x10},
     {0x48, 0x69, 0xd0, raised_count & 0xff, raised_count >> 8, 0, 0, 0x48, 0x83, 0xc2, 0x10, 0x90}},
};

// Three functions of the manager keep stock_count entries on their own stack, checked or not:
// - FUN_143e585a0 and FUN_143e58770 (manager, list*): each goes through one of the two lists of
//   pointers, keeps the entries still to be done in a stack array and writes them back. Past
//   the array they write to null (0xdeadc0de).
// - FUN_143e58950 (begin, end, scratch): a radix sort of the entries of the list at +0x480
//   (eight bytes each, a float and a handle), called only by FUN_143e753c0 with a stack array as
//   its scratch room, unchecked.
inline constexpr Fingerprint filter_loading{0x3e585a0, {
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x6c,0x24,0x20,0x56,0x57,0x41,0x56,0xb8,0x40,
    0x80,0x02,0x00,0xe8,0xa8,0x34,0x61,0x01,0x48,0x2b,0xe0,0x48,0x8b,0x05,0xfe,0xbd}};
inline constexpr Fingerprint filter_waiting{0x3e58770, {
    0x40,0x57,0xb8,0x50,0x80,0x02,0x00,0xe8,0xe4,0x32,0x61,0x01,0x48,0x2b,0xe0,0x48,
    0x8b,0x05,0x3a,0xbc,0x36,0x03,0x48,0x33,0xc4,0x48,0x89,0x84,0x24,0x38,0x80,0x02}};
inline constexpr Fingerprint sort_wanted{0x3e58950, {
    0x4c,0x8b,0xdc,0x53,0x56,0x41,0x54,0x48,0x81,0xec,0x40,0x08,0x00,0x00,0x48,0x8b,
    0xc2,0x49,0x8b,0xf0,0x48,0x2b,0xc1,0x48,0x8b,0xda,0x48,0x83,0xe0,0xf8,0x4c,0x8b}};

// What the 32-bit value of a site is, and so what it becomes.
enum class Value : std::uint8_t {
    slot,        // an offset into the first slot: moved with the slots
    slot_bytes,  // the size of all the slots
    slot_end,    // the end of the slots (the destructor walks them backwards from it)
    count,       // the count of slots
    floats,      // the floats' offset
    float_bytes, // the size of all the floats
    alloc,       // the manager's size, where it is allocated and where it is freed
    r_alloc,     // the renderer's object: its size
    r_fill,      // the floats of its four blocks together
    r_block,     // where its second, third and fourth block begin
    r_field,     // a field behind its blocks
    r_spans,     // those fields' offset in sixteens (the {pointer, count} of a layer)
};
struct Site {
    std::uint32_t rva;
    std::uint8_t size, at;
    Value value;
    std::uint8_t bytes[15];
};
inline constexpr Site sites[]{
#include "texture_streaming_sites.inc"
};
constexpr std::uint32_t raised(Value value, std::uint32_t stock) {
    switch (value) {
    case Value::slot: return stock - stock_slots + raised_slots;
    case Value::slot_bytes: return raised_slot_bytes;
    case Value::slot_end: return raised_slots_end;
    case Value::count: return raised_count;
    case Value::floats: return stock - stock_floats + raised_floats;
    case Value::float_bytes: return raised_float_bytes;
    case Value::alloc: return raised_size;
    case Value::r_alloc: return stock - renderer_stock_fields + renderer_raised_fields;
    case Value::r_fill: return raised_count * 4;
    case Value::r_block: return renderer_blocks + (stock - renderer_blocks) / (stock_count * 4) * (raised_count * 4);
    case Value::r_field: return stock - renderer_stock_fields + renderer_raised_fields;
    case Value::r_spans: return renderer_raised_fields / 16;
    }
    return stock;
}
}
