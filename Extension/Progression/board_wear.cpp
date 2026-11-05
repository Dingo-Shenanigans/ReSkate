#include "script_natives.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/script_natives.h"
#include <algorithm>
#include <array>
#include <atomic>
#include "local_challenge_trace.h"
#include "local_entitlement_trigger_runtime.h"

namespace dingosdk::profile_runtime {
namespace {
// Board wear. The retail ownable-context resolve (0xdbfa3b23)
// is a stub that always reports failure, so the authored UpdateBoardWear graph
// takes its "no saved wear" branch on every tick and writes 32 zero bytes over
// the six wear floats at skater-observer component +0x120 before its own
// accumulate section reads them back. Wear can therefore never exceed a single
// tick's increment. Dropping exactly that write lets the accumulator carry
// across ticks. The authored graph only runs while DingoAdvanceSettings
// BoardWearEnabled is set, so this is already gated by the `boardwear` control
// and is inert otherwise; every other field, component and graph is untouched.
constexpr std::array<std::uint32_t, 2> board_wear_graphs{0x73271441, 0x81d6d4c8};
// The six wear floats of the skater-observer component start here.
constexpr std::uint32_t board_wear_offset = 0x120;
std::atomic<unsigned> board_wear_holds{};
std::atomic<bool> board_wear_reset_pending{};

// True for the authored per-tick zeroing of the wear block that must be held.
bool hold_board_wear_reset(std::uintptr_t* arguments) noexcept {
    std::uintptr_t resource{}; std::uint32_t key{};
    if (!arguments || !executing_expression ||
        !challenge_trace_identity(executing_expression, resource, key) ||
        std::find(board_wear_graphs.begin(), board_wear_graphs.end(), key) == board_wear_graphs.end())
        return false;
    const auto argv = reinterpret_cast<std::uintptr_t>(arguments);
    std::uintptr_t base_slot{}, count_slot{}, offsets{}, source{};
    std::uint32_t base_offset{}, offset{}; char count{};
    std::array<std::uint8_t, 32> value{};
    // One field, at the wear block, whose incoming bytes are entirely zero.
    if (!read(argv + 3 * 8, base_slot) || !read(base_slot, base_offset) ||
        !read(argv + 4 * 8, count_slot) || !read(count_slot, count) || count != 1 ||
        !read(argv + 5 * 8, offsets) || !read(offsets, offset) ||
        offset - base_offset != board_wear_offset ||
        !read(argv + 7 * 8, source) || !read_bytes(source, value.data(), value.size())) return false;
    if (!std::all_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte == 0; })) return false;
    // A queued reset lets exactly one authored zeroing write through to the
    // game, which is how the block returns to zero without writing it here.
    return !board_wear_reset_pending.exchange(false, std::memory_order_relaxed);
}

// Logs the first held reset.
void note_board_wear_hold() {
    if (board_wear_holds.fetch_add(1, std::memory_order_relaxed) == 0)
        logging::log(logging::Level::info, logging::Channel::progression,
            "Board wear: holding the authored per-tick reset of the wear block.");
}

// SetFields (0xbdc6524d) writes several component fields at once; the VM calls
// it through its table row, so the row is rebound to this forwarder.
using SetFields = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t*);
SetFields set_fields_original{};
void set_fields_hook(std::uintptr_t a, std::uintptr_t b, std::uintptr_t* arguments) {
    if (hold_board_wear_reset(arguments)) {
        note_board_wear_hold();
        return;
    }
    set_fields_original(a, b, arguments);
}
}

void install_board_wear_hold(std::uintptr_t base) noexcept {
    static bool attempted{};
    if (attempted || !base) return;
    attempted = true;
    try {
        constexpr std::uintptr_t entry = addr::script_natives::set_fields_entry;
        std::uint32_t hash{};
        if (!read(base + entry, hash) || hash != 0xbdc6524d) return;
        set_fields_original = reinterpret_cast<SetFields>(base + addr::script_natives::set_fields);
        // The row's slot holds SetFields itself in Skate.exe's data (read from the file,
        // 2026-11-04); a slot bound to anything else is left alone.
        (void)patch_table_slot("SetFields native row", reinterpret_cast<void**>(base + entry + 8),
            reinterpret_cast<void*>(set_fields_original), reinterpret_cast<void*>(&set_fields_hook));
    } catch (...) {}
}

void reset_board_wear() noexcept {
    board_wear_reset_pending.store(true, std::memory_order_relaxed);
}
}
