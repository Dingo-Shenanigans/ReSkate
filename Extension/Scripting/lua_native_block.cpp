#include "lua_native_block.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/lua_natives.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string_view>

namespace dingosdk {
namespace {
namespace lua = addr::lua_natives;
using LoadLibraryFn = int (*)(void* state, const char* path, const char* symbol);
using PushFormat = const char* (*)(void* state, const char* format, ...);
using ParseChunk = void (*)(void* state, void* parser);
using LuaFunction = int (*)(void* state);

std::mutex install_mutex;
std::uintptr_t installed_base{};
PushFormat push_format{};
std::atomic<ParseChunk> original_parse{};

// A script that keeps trying must not fill the log.
void refused(std::atomic<unsigned>& count, std::string_view what, std::string_view detail = {}) noexcept {
    if (count.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::assets, "Lua script refused: {}{}{}. ReSkate does not let Lua do this.",
            what, detail.empty() ? "" : " ", detail);
    } catch (...) {}
}

// A path a script made up, for the log: one line, printable, not too long.
std::string shown(const char* text) noexcept {
    std::array<char, 200> copy{};
    if (!text || memory::peek_cstring(reinterpret_cast<std::uintptr_t>(text), copy.data(), copy.size()) < 0) return {};
    std::string result(copy.data());
    for (auto& c : result) if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) c = '?';
    return result;
}

// The DLL "did not load": package.loadlib answers nil and this message, require raises it.
int load_library(void* state, const char* path, const char*) {
    static std::atomic<unsigned> count{};
    refused(count, "loading a DLL", shown(path));
    push_format(state, "ReSkate does not let Lua load a DLL");
    return 1;
}

// Text only. A chunk that starts with the bytecode signature fails Lua's own check
// ("attempt to load a binary chunk (mode is 't')").
void parse_chunk(void* state, void* parser) {
    if (parser) *reinterpret_cast<const char**>(static_cast<unsigned char*>(parser) + lua::parser_mode_offset) = "t";
    original_parse.load(std::memory_order_acquire)(state, parser);
}

// The rest answer nothing at all, which a script reads as nil.
struct Refusal { const game::build::Fingerprint& contract; LuaFunction replacement; };
constexpr std::size_t refusal_count = 6;
constexpr std::array<std::string_view, refusal_count> refusal_names{
    "os.execute (starting a program)", "io.popen (starting a program)", "io.open (opening a file on disk)",
    "io.output (opening a file on disk)", "os.remove (deleting a file)", "os.rename (renaming a file)"};
template<std::size_t Index> int nothing(void*) {
    static std::atomic<unsigned> count{};
    refused(count, refusal_names[Index]);
    return 0;
}
const std::array<Refusal, refusal_count> refusals{{
    {lua::os_execute, &nothing<0>}, {lua::io_popen, &nothing<1>}, {lua::io_open, &nothing<2>},
    {lua::io_output, &nothing<3>}, {lua::os_remove, &nothing<4>}, {lua::os_rename, &nothing<5>}}};

bool matches(std::uintptr_t base, const game::build::Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::read_bytes(base + contract.rva, bytes.data(), bytes.size()) && bytes == contract.bytes;
}
} // namespace

bool start_lua_native_block(std::uintptr_t base, std::string& error) {
    std::lock_guard lock(install_mutex);
    error.clear();
    if (installed_base) {
        if (installed_base == base) return true;
        error = "Lua's native functions are already switched off on another image";
        return false;
    }
    if (!base || !matches(base, lua::load_library) || !matches(base, lua::push_format) || !matches(base, lua::parse_chunk)) {
        error = "Native Lua contract does not match the supported game"; return false;
    }
    for (const auto& refusal : refusals)
        if (!matches(base, refusal.contract)) { error = "Native Lua library contract does not match the supported game"; return false; }
    push_format = reinterpret_cast<PushFormat>(base + lua::push_format.rva);

    std::array<void*, refusal_count + 2> targets{};
    std::size_t prepared = 0;
    const auto undo = [&] { for (std::size_t i = 0; i < prepared; ++i) hook_remove(targets[i]); };
    const auto prepare = [&](std::uintptr_t rva, void* replacement, void** original) {
        auto* target = reinterpret_cast<void*>(base + rva);
        const auto status = hook_prepare(target, replacement, original);
        if (status != HookOk) { error = "Cannot prepare the Lua native block: " + std::to_string(status); return false; }
        targets[prepared++] = target;
        return true;
    };
    ParseChunk parse{};
    bool ready = prepare(lua::load_library.rva, reinterpret_cast<void*>(&load_library), nullptr) &&
        prepare(lua::parse_chunk.rva, reinterpret_cast<void*>(&parse_chunk), reinterpret_cast<void**>(&parse)) && parse;
    for (std::size_t i = 0; ready && i < refusals.size(); ++i)
        ready = prepare(refusals[i].contract.rva, reinterpret_cast<void*>(refusals[i].replacement), nullptr);
    if (!ready) {
        if (error.empty()) error = "Cannot prepare the Lua native block";
        undo(); return false;
    }
    original_parse.store(parse, std::memory_order_release);
    auto status = HookOk;
    for (std::size_t i = 0; status == HookOk && i < prepared; ++i) status = hook_queue_enable(targets[i]);
    if (status == HookOk) status = hook_apply_queued();
    if (status != HookOk) {
        undo();
        original_parse.store(nullptr, std::memory_order_release);
        error = "Cannot switch off Lua's native functions: " + std::to_string(status); return false;
    }
    installed_base = base;
    return true;
}
} // namespace dingosdk
