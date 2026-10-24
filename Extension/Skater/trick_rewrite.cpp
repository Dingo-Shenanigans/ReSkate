#include "trick_rewrite.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/trick_rewrite.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

namespace dingosdk::trick_rewrite {
namespace {
namespace tr = addr::trick_rewrite;

using RecordTrick = void (*)(std::uintptr_t recorder, std::uintptr_t packet);
using ResolveLocalizedKey = bool (*)(std::uintptr_t object, const char* key, char* buffer, std::uint32_t capacity);

struct Record {
    std::vector<std::uint32_t> from;
    std::uint32_t to{};
    std::int64_t until{}; // performance-counter ticks
};
struct State {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{}, label_ready{}, record_ready{};
    std::atomic<RecordTrick> record_original{};
    std::atomic<ResolveLocalizedKey> resolve_original{};
    // Read by the hooks without the lock: whether anything is asked for at all.
    std::atomic<bool> any_labels{}, any_records{};
    std::mutex mutex;
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> labels; // owner -> from -> to
    std::map<std::string, Record, std::less<>> records;                                       // owner -> pending
    std::atomic<std::uint64_t> labelled{}, recorded{}, no_room{};
};
State& state() {
    static auto* value = new State; // process lifetime: the hooks may call in until exit
    return *value;
}
struct PreserveError {
    DWORD value = GetLastError();
    ~PreserveError() { SetLastError(value); }
};
std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
std::int64_t qpc_ms(int ms) noexcept {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency * ms / 1000;
}
void refresh(State& s) {
    s.any_labels.store(std::any_of(s.labels.begin(), s.labels.end(), [](const auto& owner) { return !owner.second.empty(); }),
                       std::memory_order_release);
    s.any_records.store(!s.records.empty(), std::memory_order_release);
}

// Game thread(s). The game's method runs first and fills its caller's buffer; only then, and only for
// a label asked for on the ticker's route, is the text in that buffer replaced.
__declspec(noinline) bool resolve_localized_key(std::uintptr_t object, const char* key, char* buffer, std::uint32_t capacity) {
    auto& s = state();
    const bool success = s.resolve_original.load(std::memory_order_acquire)(object, key, buffer, capacity);
    if (!success || !buffer || !capacity || !s.any_labels.load(std::memory_order_acquire)) return success;
    PreserveError preserve;
    char text[most_label_text + 1]{};
    const auto length = memory::peek_cstring(reinterpret_cast<std::uintptr_t>(buffer), text, sizeof(text));
    if (length <= 0 || static_cast<std::uint32_t>(length) >= capacity) return success;
    std::string replacement;
    {
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return success;
        for (const auto& [owner, labels] : s.labels)
            if (const auto found = labels.find(std::string_view(text, static_cast<std::size_t>(length))); found != labels.end()) {
                replacement = found->second;
                break;
            }
    }
    if (replacement.empty()) return success;
    // Captured here, in the hook itself: frame 0 is this function, frame 1 its caller.
    std::array<void*, 2 + tr::ticker_label_route.size()> frames{};
    const auto count = RtlCaptureStackBackTrace(0, static_cast<ULONG>(frames.size()), frames.data(), nullptr);
    const auto base = s.base.load(std::memory_order_relaxed);
    bool route = count == frames.size();
    for (std::size_t i = 0; route && i < tr::ticker_label_route.size(); ++i)
        route = reinterpret_cast<std::uintptr_t>(frames[2 + i]) == base + tr::ticker_label_route[i];
    if (!route) return success;
    if (capacity <= replacement.size()) { // never cut a label short
        s.no_room.fetch_add(1, std::memory_order_relaxed);
        return success;
    }
    std::memcpy(buffer, replacement.c_str(), replacement.size() + 1);
    s.labelled.fetch_add(1, std::memory_order_relaxed);
    return success;
}

// Game thread. A trick a pending record asks for is recorded as its `to`, through a copy of the
// packet; anything else goes to the recorder untouched.
__declspec(noinline) void record_trick(std::uintptr_t recorder, std::uintptr_t packet) {
    auto& s = state();
    const auto original = s.record_original.load(std::memory_order_acquire);
    if (s.any_records.load(std::memory_order_acquire) && packet >= 0x10000 &&
        packet < memory::highest_user_address - tr::trick_packet_size) {
        PreserveError preserve;
        std::uint8_t valid{};
        std::uint32_t id{};
        if (memory::peek(packet + tr::trick_packet_valid, valid) && valid && memory::peek(packet + tr::trick_packet_id, id)) {
            std::optional<std::uint32_t> to;
            {
                std::lock_guard lock(s.mutex);
                const auto now = qpc_now();
                std::erase_if(s.records, [&](const auto& entry) { return now > entry.second.until; });
                for (auto it = s.records.begin(); it != s.records.end(); ++it)
                    if (std::find(it->second.from.begin(), it->second.from.end(), id) != it->second.from.end()) {
                        to = it->second.to;
                        s.records.erase(it); // once
                        break;
                    }
                refresh(s);
            }
            std::array<unsigned char, tr::trick_packet_size> copy{};
            if (to && memory::peek(packet, copy)) {
                std::memcpy(copy.data() + tr::trick_packet_id, &*to, sizeof(*to));
                s.recorded.fetch_add(1, std::memory_order_relaxed);
                // The recorder copies the packet before it returns, so the copy may live here.
                original(recorder, reinterpret_cast<std::uintptr_t>(copy.data()));
                return;
            }
        }
    }
    original(recorder, packet);
}

template<class Function, std::size_t N>
bool install(std::uintptr_t base, std::uintptr_t rva, const std::array<unsigned char, N>& prefix,
             Function replacement, std::atomic<Function>& original) noexcept {
    std::array<unsigned char, N> actual{};
    if (!memory::peek(base + rva, actual) || actual != prefix) return false;
    auto* const target = reinterpret_cast<void*>(base + rva);
    void* previous{};
    if (hook_prepare(target, reinterpret_cast<void*>(replacement), &previous) != HookOk) return false;
    if (!previous) {
        (void)hook_remove(target);
        return false;
    }
    original.store(reinterpret_cast<Function>(previous), std::memory_order_release);
    if (hook_enable(target) != HookOk) {
        (void)hook_remove(target);
        return false;
    }
    return true;
}
} // namespace

bool start(std::uintptr_t image_base) noexcept {
    auto& s = state();
    if (s.started.exchange(true, std::memory_order_acq_rel)) return labels_available() || records_available();
    s.base.store(image_base, std::memory_order_release);
    s.label_ready.store(install(image_base, tr::resolve_localized_key, tr::resolve_localized_key_prefix, &resolve_localized_key,
                                s.resolve_original), std::memory_order_release);
    s.record_ready.store(install(image_base, tr::record_trick, tr::record_trick_prefix, &record_trick, s.record_original),
                         std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater, "Trick labels and records for plugins: ticker labels {}, trick records {}.",
                 s.label_ready.load() ? "hooked" : "unavailable (this build's code differs)",
                 s.record_ready.load() ? "hooked" : "unavailable (this build's code differs)");
    return s.label_ready.load() || s.record_ready.load();
}
bool labels_available() noexcept { return state().label_ready.load(std::memory_order_acquire); }
bool records_available() noexcept { return state().record_ready.load(std::memory_order_acquire); }

std::string label(std::string_view owner, std::string_view from, std::string_view to) {
    if (from.empty() || from.size() > most_label_text || to.size() > most_label_text) return "a label is 1 to 63 characters";
    auto& s = state();
    if (!labels_available()) return "trick labels are unavailable for this game build";
    std::lock_guard lock(s.mutex);
    auto& mine = s.labels[std::string(owner)];
    if (to.empty()) mine.erase(std::string(from));
    else {
        if (!mine.contains(from) && mine.size() >= most_labels) return "a plugin has at most 16 labels";
        mine[std::string(from)] = std::string(to);
    }
    if (mine.empty()) s.labels.erase(std::string(owner));
    refresh(s);
    return {};
}
std::string record_as(std::string_view owner, std::vector<std::uint32_t> from, std::uint32_t to, int within_ms) {
    if (from.empty() || from.size() > 16) return "a record names 1 to 16 trick ids";
    for (const auto id : from)
        if (id > most_trick_id) return "a trick id is 0 to 336";
    if (to > most_trick_id) return "a trick id is 0 to 336";
    if (within_ms < 1 || within_ms > most_within_ms) return "a record waits 1 to 2000 ms";
    auto& s = state();
    if (!records_available()) return "trick records are unavailable for this game build";
    std::lock_guard lock(s.mutex);
    s.records[std::string(owner)] = Record{std::move(from), to, qpc_now() + qpc_ms(within_ms)};
    refresh(s);
    return {};
}
void release(std::string_view owner) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (const auto found = s.labels.find(owner); found != s.labels.end()) s.labels.erase(found);
    if (const auto found = s.records.find(owner); found != s.records.end()) s.records.erase(found);
    refresh(s);
}
Counts counts() noexcept {
    const auto& s = state();
    return {s.labelled.load(), s.recorded.load(), s.no_room.load()};
}
}
