#include "load_error_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/load_errors.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <string>

namespace dingosdk::load_error_guard {
namespace {
namespace le = addr::load_errors;
using Describe = void (*)(void* errors, void* text);
Describe original{};
std::uintptr_t image_begin{}, image_end{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> empties{}, faults{}, told{}, described_loads{};
thread_local unsigned depth{}; // a part can be a list of its own: only the outermost is logged
constexpr std::uint64_t most_parts = 4096, most_told = 40;

bool in_image(std::uintptr_t address) noexcept { return address >= image_begin && address < image_end; }

// One of the game's own objects: its vtable and the virtual that describes it are in the image.
bool a_part(std::uintptr_t part) noexcept {
    std::uintptr_t vtable{}, describe{};
    return memory::read(part, vtable) && in_image(vtable) && !(vtable & 7) && memory::read(vtable + 8, describe) && in_image(describe);
}
// Empties the parts that are not: how many. A list that cannot be read is left alone (the
// handler around the description is what is left for it).
unsigned empty_bad_parts(std::uintptr_t errors) noexcept {
    std::uintptr_t parts{};
    std::uint64_t count{};
    if (!memory::read(errors + le::parts_offset, parts) || !memory::read(errors + le::count_offset, count) || !parts || count > most_parts) return 0;
    unsigned emptied{};
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uintptr_t part{};
        if (!memory::read(parts + i * 8, part)) break;
        if (!part || a_part(part)) continue;
        __try {
            *reinterpret_cast<std::uintptr_t*>(parts + i * 8) = 0;
            ++emptied;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return emptied;
}
bool describe_guarded(void* errors, void* text) noexcept {
    __try {
        original(errors, text);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
// What the game wrote, for the log: the writer's buffer, on one line.
__declspec(noinline) void tell(std::uintptr_t text, unsigned emptied, bool whole) noexcept {
    if (told.fetch_add(1, std::memory_order_relaxed) >= most_told) return;
    try {
        std::array<char, 1024> buffer{};
        std::uintptr_t begin{};
        std::string said;
        if (memory::read(text, begin) && (begin &= ~std::uintptr_t{3})) {
            if (memory::peek_cstring(begin, buffer.data(), buffer.size()) < 0) buffer.back() = '\0';
            said = buffer.data();
        }
        for (auto& c : said)
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        std::string note;
        if (emptied) note += " " + std::to_string(emptied) + " part(s) of its error list were never set, where the game would have crashed; they read as no error.";
        if (!whole) note += " The description faulted and was cut short.";
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "The game described the error of a load that failed (content that is missing or broken, often a mod's): {}{}",
            said.empty() ? std::string("(nothing readable)") : said, note);
    } catch (...) {}
}
void guarded(void* errors, void* text) {
    const auto emptied = empty_bad_parts(reinterpret_cast<std::uintptr_t>(errors));
    if (emptied) empties.fetch_add(emptied, std::memory_order_relaxed);
    ++depth;
    const bool whole = describe_guarded(errors, text);
    --depth;
    if (!whole) faults.fetch_add(1, std::memory_order_relaxed);
    if (!depth) {
        described_loads.fetch_add(1, std::memory_order_relaxed);
        tell(reinterpret_cast<std::uintptr_t>(text), emptied, whole);
    }
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!memory::peek(base + le::describe.rva, head) || head != le::describe.bytes || !memory::peek(base, dos) || dos.e_lfanew <= 0 ||
        !memory::peek(base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) || !nt.OptionalHeader.SizeOfImage) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Load error guard: this build's load error code differs; it is left as it is.");
        return false;
    }
    image_begin = base;
    image_end = base + nt.OptionalHeader.SizeOfImage;
    const auto target = reinterpret_cast<void*>(base + le::describe.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Load error guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t emptied() noexcept { return empties.load(std::memory_order_relaxed); }
std::uint64_t described() noexcept { return described_loads.load(std::memory_order_relaxed); }
} // namespace dingosdk::load_error_guard
