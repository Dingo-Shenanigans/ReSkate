#include "texture_handle_limit.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/texture_streaming.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>

namespace dingosdk::texture_handle_limit {
namespace {
namespace ts = addr::texture_streaming;
using Constructor = void* (*)(void* manager, void* arena);
Constructor original{};
std::uintptr_t image{};
std::atomic<bool> installed{}, raised{};
std::atomic<unsigned> lists_moved{};

std::uint32_t stock_value(const ts::Site& site) noexcept {
    std::uint32_t value{};
    std::memcpy(&value, site.bytes + site.at, 4);
    return value;
}
// Each list's entries go behind the manager's stock end, with what the constructor put in them.
// A list that is not where the constructor leaves it is left alone.
bool move_lists(std::uintptr_t manager) noexcept {
    __try {
        unsigned moved{};
        for (const auto& list : ts::lists) {
            auto* header = reinterpret_cast<std::uintptr_t*>(manager + list.header);
            const auto stock = manager + list.stock_entries;
            if (header[0] != stock || header[1] < stock || header[2] != stock + std::uintptr_t{ts::stock_count} * list.entry_size) continue;
            const auto used = header[1] - header[0];
            const auto entries = manager + list.raised_entries;
            std::memcpy(reinterpret_cast<void*>(entries), reinterpret_cast<const void*>(stock), used);
            header[0] = entries;
            header[1] = entries + used;
            header[2] = entries + std::uintptr_t{ts::raised_count} * list.entry_size;
            ++moved;
        }
        lists_moved.store(moved);
        return moved == std::size(ts::lists);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) void say_made(bool moved) noexcept {
    try {
        if (moved)
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Texture streaming: the game can hold {} streamed textures at once (its own limit is {}).", ts::raised_count, ts::stock_count);
        else
            logging::log(logging::Level::error, logging::Channel::runtime,
                "Texture streaming: the limit was raised but {} of the manager's {} lists were not where expected; they keep {} entries.",
                std::size(ts::lists) - lists_moved.load(), std::size(ts::lists), ts::stock_count);
    } catch (...) {}
}
void* made(void* manager, void* arena) {
    auto* result = original(manager, arena);
    if (raised.load() && result) say_made(move_lists(reinterpret_cast<std::uintptr_t>(result)));
    return result;
}

// Three of the manager's functions keep 20480 entries on their own stack, which more slots can
// outgrow (texture_streaming.h). Each is hooked and left exactly as it is up to that many.
using Filter = void (*)(void* manager, std::uintptr_t* list);
using Sort = void (*)(std::uint32_t* begin, std::uint32_t* end, std::uint32_t* scratch);
Filter original_filters[2]{};
Sort original_sort{};

// A longer list goes through the game's function a stretch at a time, each stretch presented as
// a list of its own ({begin, end, capacity end}), and what each keeps is closed up behind the
// last. The function only reads the list, empties entries in place and writes the kept ones back
// from `begin`, so a stretch is all it needs to see.
void filter_in_stretches(Filter filter, void* manager, std::uintptr_t* list) noexcept {
    constexpr std::uintptr_t stretch = std::uintptr_t{ts::stock_count} * 8;
    const auto begin = list[0], end = list[1];
    auto kept_end = begin;
    for (auto at = begin; at < end;) {
        const auto stop = std::min(end, at + stretch);
        std::uintptr_t part[3]{at, stop, stop};
        filter(manager, part);
        const auto kept = part[1] - part[0];
        if (kept && part[0] != kept_end) std::memmove(reinterpret_cast<void*>(kept_end), reinterpret_cast<const void*>(part[0]), kept);
        kept_end += kept;
        at = stop;
    }
    list[1] = kept_end;
}
template <std::size_t Index> void filtered(void* manager, std::uintptr_t* list) {
    const auto filter = original_filters[Index];
    if (!list || list[1] < list[0] || (list[1] - list[0]) / 8 <= ts::stock_count) return filter(manager, list);
    filter_in_stretches(filter, manager, list);
}
// The sort wants scratch room for as many entries as it sorts; its one caller gives it a
// stack array of 20480. More than that get room of their own for the call.
void sorted(std::uint32_t* begin, std::uint32_t* end, std::uint32_t* scratch) {
    const auto bytes = reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin);
    if (end < begin || bytes / 8 <= ts::stock_count) return original_sort(begin, end, scratch);
    auto* room = static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), 0, bytes));
    if (!room) return; // (unsorted this once: the order only says which textures are dealt with first)
    original_sort(begin, end, room);
    HeapFree(GetProcessHeap(), 0, room);
}
bool hook(std::uintptr_t base, const game::build::Fingerprint& print, void* replacement, void** kept) noexcept {
    std::array<unsigned char, 32> head{};
    const auto target = reinterpret_cast<void*>(base + print.rva);
    if (!memory::peek(base + print.rva, head) || head != print.bytes || hook_prepare(target, replacement, kept) != HookOk ||
        hook_enable(target) != HookOk) {
        *kept = nullptr;
        return false;
    }
    return true;
}

// "texture_limit_stock.txt" beside the game: the limit is left as the game has it (to compare).
bool stock_asked() noexcept {
    wchar_t path[MAX_PATH]{};
    const auto size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!size || size >= MAX_PATH) return false;
    std::wstring file(path, size);
    const auto slash = file.find_last_of(L"/\\");
    if (slash == std::wstring::npos) return false;
    file.resize(slash + 1);
    file += L"texture_limit_stock.txt";
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void say(logging::Level level, const char* text) noexcept {
    try {
        logging::log(level, logging::Channel::runtime, "Texture streaming: {}", text);
    } catch (...) {}
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return raised.load();
    if (!base) return false;
    image = base;
    if (stock_asked()) {
        say(logging::Level::warning, "texture_limit_stock.txt is there; the game keeps its own limit of streamed textures.");
        return false;
    }
    // Everything is checked before anything is changed: the constructor, and each instruction
    // byte for byte.
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + ts::constructor.rva, head) || head != ts::constructor.bytes) {
        say(logging::Level::warning, "this build's manager differs; the game keeps its own limit of streamed textures.");
        return false;
    }
    std::uintptr_t first = ~std::uintptr_t{}, last{};
    for (const auto& site : ts::sites) {
        std::array<unsigned char, 15> now{};
        if (site.size > now.size() || site.at + 4u > site.size || !memory::peek_bytes(base + site.rva, now.data(), site.size) ||
            std::memcmp(now.data(), site.bytes, site.size) != 0) {
            say(logging::Level::warning, "this build's streaming code differs; the game keeps its own limit of streamed textures.");
            return false;
        }
        first = std::min(first, base + site.rva);
        last = std::max(last, base + site.rva + site.size);
    }
    for (const auto& rewrite : ts::renderer_rewrites) {
        std::array<unsigned char, 12> now{};
        if (!memory::peek(base + rewrite.rva, now) || std::memcmp(now.data(), rewrite.stock, now.size()) != 0) {
            say(logging::Level::warning, "this build's renderer differs; the game keeps its own limit of streamed textures.");
            return false;
        }
        first = std::min(first, base + rewrite.rva);
        last = std::max(last, base + rewrite.rva + now.size());
    }
    std::uintptr_t manager{};
    if (!memory::peek(base + ts::manager, manager) || manager) {
        say(logging::Level::warning, "the game had made its manager already; it keeps its own limit of streamed textures.");
        return false;
    }
    const auto target = reinterpret_cast<void*>(base + ts::constructor.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&made), reinterpret_cast<void**>(&original)) != HookOk) {
        original = nullptr;
        say(logging::Level::warning, "the manager's constructor could not be hooked; the game keeps its own limit of streamed textures.");
        return false;
    }
    // One span of pages made writable for all of it, so no write can fail half way through.
    DWORD before{};
    const auto span = last - first;
    if (!VirtualProtect(reinterpret_cast<void*>(first), span, PAGE_EXECUTE_READWRITE, &before)) {
        say(logging::Level::warning, "the streaming code could not be made writable; the game keeps its own limit of streamed textures.");
        return false;
    }
    for (const auto& site : ts::sites) {
        const auto value = ts::raised(site.value, stock_value(site));
        std::memcpy(reinterpret_cast<void*>(base + site.rva + site.at), &value, 4);
    }
    for (const auto& rewrite : ts::renderer_rewrites)
        std::memcpy(reinterpret_cast<void*>(base + rewrite.rva), rewrite.raised, sizeof(rewrite.raised));
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(first), span, before, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(first), span);
    raised.store(true);
    const bool guarded = hook(base, ts::filter_loading, reinterpret_cast<void*>(&filtered<0>), reinterpret_cast<void**>(&original_filters[0])) &
                         hook(base, ts::filter_waiting, reinterpret_cast<void*>(&filtered<1>), reinterpret_cast<void**>(&original_filters[1])) &
                         hook(base, ts::sort_wanted, reinterpret_cast<void*>(&sorted), reinterpret_cast<void**>(&original_sort));
    if (!guarded)
        say(logging::Level::error, "the limit was raised but a function with room for 20480 on its stack could not be hooked; past 20480 the game may crash.");
    if (hook_enable(target) != HookOk)
        say(logging::Level::error, "the limit was raised but the constructor's hook did not start; the manager's lists keep 20480 entries.");
    return true;
}

Use use() noexcept {
    Use result;
    result.limit = raised.load() ? ts::raised_count : ts::stock_count;
    std::uintptr_t manager{};
    std::uint32_t bottom{}, top{};
    if (!image || !memory::peek(image + ts::manager, manager) || !manager || !memory::peek(manager + ts::taken_from_bottom, bottom) ||
        !memory::peek(manager + ts::taken_from_top, top) || top > result.limit || bottom > result.limit)
        return result;
    result.known = true;
    result.taken = std::min(result.limit, bottom + (result.limit - top));
    return result;
}

void tick() noexcept {
    static ULONGLONG next{};
    static bool warned{}, full{};
    static std::uint32_t most{};
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    const auto state = use();
    if (!state.known) return;
    const auto percent = static_cast<unsigned>(std::uint64_t{state.taken} * 100 / state.limit);
    try {
        // (A new high, in steps: how close a session came is worth having in a log.)
        if (state.taken >= most + state.limit / 20) {
            most = state.taken;
            logging::log(logging::Level::info, logging::Channel::runtime, "Texture streaming: {} of {} streamed textures in use ({}%).",
                state.taken, state.limit, percent);
        }
        if (state.taken >= state.limit && !full) {
            full = true;
            logging::log(logging::Level::error, logging::Channel::runtime,
                "Texture streaming: all {} streamed textures are in use; the game hangs when it needs one more.", state.limit);
            overlay::notify(overlay::NoticeLevel::error, "Too many textures loaded",
                "The game is out of room for textures and may freeze. Uninstall some mods to play safely.");
        } else if (percent >= 90 && !warned) {
            warned = true;
            logging::log(logging::Level::warning, logging::Channel::runtime,
                "Texture streaming: {} of {} streamed textures in use ({}%); the game hangs at the limit.", state.taken, state.limit, percent);
            overlay::notify(overlay::NoticeLevel::warning, "Close to the texture limit",
                std::to_string(state.taken) + " of " + std::to_string(state.limit) +
                    " textures are loaded, and the game freezes at the limit. Do not install any more mods, or uninstall some.");
        } else if (percent < 80) {
            warned = full = false; // told again the next time it comes close
        }
    } catch (...) {}
}
} // namespace dingosdk::texture_handle_limit
