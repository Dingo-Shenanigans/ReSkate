#include "texture_handle_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/texture_handles.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>

namespace dingosdk::texture_handle_guard {
namespace {
namespace th = addr::texture_handles;
using Redirect = std::uintptr_t* (*)(std::uintptr_t, std::uint16_t);
using Resolve = std::uintptr_t (*)(const std::uint16_t*);
using Lookup = std::uintptr_t* (*)(std::uintptr_t, std::uint16_t);
Redirect original{};
Resolve original_resolve{};
Lookup lookup{};
std::uintptr_t image{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> misses{};
std::atomic<std::uint64_t> strays{};
using ValueCopy = void (*)(std::uintptr_t type, std::uint8_t* value, std::uintptr_t source, std::uint32_t count);
using ValueClear = void (*)(std::uintptr_t type, std::uint8_t* value, std::uint32_t count);
ValueCopy original_value_copy{};
ValueClear value_clear{};
std::atomic<std::uint64_t> foreign_values{};
// What both callers read when there is nothing: a holder with no texture in it, which sends them
// to the entry's own texture, null for a dead entry (texture_handles.h).
std::uintptr_t no_texture{};
constexpr std::size_t stub_page_bytes = 0x1000;

// Said for the first few, then each power of two: a stale handle is resolved on every frame it
// is drawn.
__declspec(noinline) void note(std::uint16_t id) noexcept {
    const auto count = misses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Textures: a texture handle ({:#x}) names a texture that is gone, where the game would have crashed; nothing is "
            "drawn for it ({} time(s) since the game started). A mod's material that keeps a texture another "
            "bundle unloaded does this.", id, count);
    } catch (...) {}
}

__declspec(noinline) void note_stray(std::uint16_t id) noexcept {
    const auto count = strays.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Textures: a texture handle ({:#x}) names something that is not a texture, where the game would have crashed; "
            "nothing is drawn for it ({} time(s) since the game started). A mod's material whose texture parameter is "
            "broken, or names a texture long gone, does this.", id, count);
    } catch (...) {}
}

// Each texture's vtable, and the two slots resolve calls on it, lie in the game's image; what an entry
// that never held a texture, or no longer does, keeps where its holder was is anything (texture_handles.h).
bool in_image(std::uintptr_t address) noexcept { return address - image < supported_build::game_image_size; }
bool is_texture(std::uintptr_t texture) noexcept {
    std::uintptr_t table{};
    if (!memory::peek(texture, table) || !in_image(table)) return false;
    std::array<std::uintptr_t, th::resolve_calls> calls{};
    if (!memory::peek(table + th::resolve_calls_offset, calls)) return false;
    for (const auto call : calls)
        if (!in_image(call)) return false;
    return true;
}
// A holder resolve may read through: one holding a texture, or none (the entry's own texture then).
bool holds_texture(std::uintptr_t holder) noexcept {
    std::uintptr_t texture{};
    return memory::peek(holder, texture) && (!texture || is_texture(texture));
}

std::uintptr_t* guarded(std::uintptr_t pool, std::uint16_t id) {
    if (auto* const holder = original(pool, id)) {
        if (holds_texture(reinterpret_cast<std::uintptr_t>(holder))) return holder;
        note_stray(id);
        return &no_texture;
    }
    note(id);
    return &no_texture;
}

// The holder resolve reads first is checked here; the one redirect gives in `guarded`; the entry's own
// texture, which resolve answers without calling it, on the way out.
std::uintptr_t guarded_resolve(const std::uint16_t* handle) {
    const auto id = *handle;
    std::uintptr_t textures{};
    if (memory::peek(image + th::textures, textures) && textures) {
        const auto holder = *lookup(textures + th::pool_offset, id);
        if (holder && !holds_texture(holder)) {
            note_stray(id);
            return 0;
        }
    }
    const auto texture = original_resolve(handle);
    if (!texture || is_texture(texture)) return texture;
    note_stray(id);
    return 0;
}

__declspec(noinline) void note_foreign(std::uint8_t kind) noexcept {
    const auto count = foreign_values.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Textures: a material's texture parameter holds a value of another kind ({:#x}), where the game would have "
            "crashed; it is left empty ({} time(s) since the game started). A mod's shader preset that is broken does this.",
            kind, count);
    } catch (...) {}
}

// A texture parameter value whose kind the type has no handler for gets the type's own empty value
// instead of a call through what lies past its handlers (texture_handles.h).
void guarded_value_copy(std::uintptr_t type, std::uint8_t* value, std::uintptr_t source, std::uint32_t count) {
    std::uint8_t kind{};
    if (memory::peek(source + th::value_kind_offset, kind) && kind < th::value_kinds) {
        original_value_copy(type, value, source, count);
        return;
    }
    note_foreign(kind);
    value_clear(type, value, count);
}

bool guard_value_copy(std::uintptr_t base) noexcept {
    std::array<unsigned char, 32> copy_head{}, clear_head{};
    if (!memory::peek(base + th::value_copy.rva, copy_head) || copy_head != th::value_copy.bytes ||
        !memory::peek(base + th::value_clear.rva, clear_head) || clear_head != th::value_clear.bytes)
        return false;
    value_clear = reinterpret_cast<ValueClear>(base + th::value_clear.rva);
    const auto target = reinterpret_cast<void*>(base + th::value_copy.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded_value_copy), reinterpret_cast<void**>(&original_value_copy)) != HookOk ||
        hook_enable(target) != HookOk) {
        original_value_copy = nullptr;
        return false;
    }
    return true;
}

bool rel32(std::uintptr_t next_instruction, std::uintptr_t target, std::int32_t& displacement) noexcept {
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next_instruction);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) return false;
    displacement = static_cast<std::int32_t>(delta);
    return true;
}

// The null the guard leads resolve to reaches the texture view cache, which asks the texture for its
// key before keeping the view it built (texture_handles.h). The stub skips the keeping for a null
// texture, so the cache hands back the empty view:
//   test r15, r15 / jz return / mov rbx, [cache] / add rbx, lock / jmp resume
bool skip_null_view_insert(std::uintptr_t base) noexcept {
    std::array<unsigned char, th::view_cache_insert.size()> actual{};
    if (!memory::peek(base + th::view_cache_insert_rva, actual) || actual != th::view_cache_insert) return false;
    auto* const stub = static_cast<std::uint8_t*>(memory::allocate_near_image(base, supported_build::game_image_size, stub_page_bytes));
    if (!stub) return false;
    const auto at = reinterpret_cast<std::uintptr_t>(stub);
    std::array<std::uint8_t, 28> code{0x4d, 0x85, 0xff, 0x0f, 0x84, 0, 0, 0, 0, 0x48, 0x8b, 0x1d, 0, 0, 0, 0,
        0x48, 0x81, 0xc3, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    std::memcpy(code.data() + 19, &th::view_cache_lock_offset, 4);
    std::int32_t leave{}, cache{}, resume{}, into{};
    if (!rel32(at + 9, base + th::view_cache_return_rva, leave) || !rel32(at + 16, base + th::view_cache, cache) ||
        !rel32(at + 28, base + th::view_cache_resume_rva, resume) || !rel32(base + th::view_cache_insert_rva + 5, at, into)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(code.data() + 5, &leave, 4);
    std::memcpy(code.data() + 12, &cache, 4);
    std::memcpy(code.data() + 24, &resume, 4);
    std::memset(stub, 0xcc, stub_page_bytes);
    std::memcpy(stub, code.data(), code.size());
    DWORD previous{};
    if (!VirtualProtect(stub, stub_page_bytes, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, stub_page_bytes)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    // jmp stub, then NOPs over the rest of the two moved instructions.
    const auto redirected = patch_code_jump("texture view cache insert", base + th::view_cache_insert_rva,
        th::view_cache_insert, at);
    if (redirected != HookOk) {
        // A refused patch wrote nothing; a failed write may have written some of it.
        if (redirected != HookBytesDiffer && redirected != HookOutOfReach)
            (void)patch_code_restore(base + th::view_cache_insert_rva, th::view_cache_insert);
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    return true;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    image = base;
    // Its own crash, on its own: kept whatever becomes of the handle guards below.
    if (!guard_value_copy(base))
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: this build's texture parameter copy differs or could not be hooked; it is left as it is.");
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + th::redirect.rva, head) || head != th::redirect.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: this build's texture code differs; it is left as it is.");
        return false;
    }
    // Without it the null only crashes a step later (Skate.exe+0x3e6ddc1), so the guard needs both.
    if (!skip_null_view_insert(base)) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: the game's texture view cache differs or could not be patched; it is left as it is.");
        return false;
    }
    const auto target = reinterpret_cast<void*>(base + th::redirect.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Texture handle guard: the game's function could not be hooked.");
        return false;
    }
    // Without the resolve check, a stray holder still crashes; the redirect guard is kept regardless.
    std::array<unsigned char, 32> resolve_head{}, lookup_head{};
    if (!memory::peek(base + th::resolve.rva, resolve_head) || resolve_head != th::resolve.bytes ||
        !memory::peek(base + th::lookup.rva, lookup_head) || lookup_head != th::lookup.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: this build's texture resolve differs; only missing textures are guarded.");
        return true;
    }
    lookup = reinterpret_cast<Lookup>(base + th::lookup.rva);
    const auto resolve = reinterpret_cast<void*>(base + th::resolve.rva);
    if (hook_prepare(resolve, reinterpret_cast<void*>(&guarded_resolve), reinterpret_cast<void**>(&original_resolve)) != HookOk ||
        hook_enable(resolve) != HookOk) {
        original_resolve = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Texture handle guard: the game's texture resolve could not be hooked; only missing textures are guarded.");
    }
    return true;
}

std::uint64_t stale() noexcept { return misses.load(std::memory_order_relaxed); }
} // namespace dingosdk::texture_handle_guard
