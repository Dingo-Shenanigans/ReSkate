#include "skate_trick_rule.h"
#include "Extension/Skater/skate_detector.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;
// ServerSkateThrowdown's Skate_SubmitAttempt handler as shipped: header {frame 0xc0,
// constants 0x710, code 0x1128 words}, code at +0xb20 (first word op26 -> 0x24), native
// call table of 224 {hash, pc} pairs at +0x4fe0. The setter test is its only ArrayIndexOf.
constexpr std::uint32_t graph_hash = 0xc4a37359, header_frame = 0xc0, header_constants = 0x710,
                        header_code_words = 0x1128, code_offset = 0xb20, first_word = 0x2426,
                        table_offset = 0x4fe0, table_count = 224, index_of_hash = 0x93dda190, index_of_pc = 0x60c;
// Its locals (page 2): the guard result, the event handle it checked, the active player,
// the setter index, WasSuccessful and the 0x1c-byte CompositeTrickRecord. (The two-landings
// rule, TwoToMakeItTrue, is a retail-stubbed dev setting and always off.)
constexpr Address guard_local = 0x81b, handle_local = 0x620, active_local = 0x638, index_local = 0x7bc,
                  landed_local = 0x811, trick_local = 0x378;

template <class T> T read(Address at) {
    T value{};
    if (!memory::peek(at, value)) throw std::runtime_error("S.K.A.T.E. trick rule: memory unavailable");
    return value;
}
bool poke(Address at, std::uint8_t value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t *>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The record's fields, as the game's copy check compares them (padding after IsLate left out).
struct Trick {
    std::uint32_t stance{}, direction{}, spins{}, held_flip{}; // held_flip: the float's bits
    std::uint8_t late{};
    std::uint32_t scorable{}, held_category{};
    bool operator==(const Trick &) const = default;
};
Trick read_trick(Address record) {
    const auto bytes = read<std::array<std::uint8_t, 0x1c>>(record);
    Trick trick;
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value{};
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    };
    trick.stance = u32(0);
    trick.direction = u32(4);
    trick.spins = u32(8);
    trick.held_flip = u32(0xc);
    trick.late = bytes[0x10];
    trick.scorable = u32(0x14);
    trick.held_category = u32(0x18);
    return trick;
}

struct Frame {
    Address locals{};
};
Frame frame_of(Address vm) {
    const auto resource = read<Address>(vm + 0x38);
    if (!resource || read<std::uint32_t>(resource + 0x10) != graph_hash) return {};
    const auto pages = read<Address>(vm + 0x30) + ((read<std::uint32_t>(resource + 0x20) + 15U) & ~15U);
    return {read<Address>(pages + 16)};
}

using IndexOf = std::uintptr_t (*)(const void *, const void *, const void *, std::int32_t *);
struct State {
    std::mutex mutex;
    std::uint32_t handle{};    // the event the history belongs to
    std::vector<Trick> tricks; // set so far in it
    std::optional<SkateRepeat> repeat;
    std::set<Address> patched, refused; // loaded copies of the handler
    std::atomic<IndexOf> original{};
};
State &state() {
    static auto *value = new State;
    return *value;
}

enum class Set { none, fresh, repeat };
// The setter's attempt in `frame`: a new trick joins the history, one already in it is
// reported (the caller turns it into a miss). Not a set: missed, or no scorable trick.
Set judge(const Frame &frame) {
    if (!frame.locals) return Set::none;
    const auto landed = read<std::uint8_t>(frame.locals + landed_local);
    const auto trick = read_trick(frame.locals + trick_local);
    const auto handle = read<std::uint32_t>(frame.locals + handle_local);
    static std::atomic<unsigned> logged{};
    if (logged.fetch_add(1) < 40)
        logging::log(logging::Level::info, logging::Channel::progression,
                     "Throwdowns: S.K.A.T.E. setter attempt in event {:#x}: landed {}, trick {:#x} (stance {}, "
                     "direction {}, spins {}, held flip {:#x}, late {}, category {}).",
                     handle, landed, trick.scorable, trick.stance, trick.direction,
                     static_cast<std::int32_t>(trick.spins), trick.held_flip, trick.late, trick.held_category);
    if (!landed || !trick.scorable) return Set::none;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (handle != s.handle) {
        s.handle = handle;
        s.tricks.clear();
    }
    if (std::find(s.tricks.begin(), s.tricks.end(), trick) != s.tricks.end()) {
        s.repeat = SkateRepeat{read<std::uint32_t>(frame.locals + active_local), std::chrono::steady_clock::now()};
        logging::log(logging::Level::info, logging::Channel::progression,
                     "Throwdowns: S.K.A.T.E. set refused: player {:#x} repeated a trick already set (trick {:#x}).",
                     s.repeat->player, trick.scorable);
        return Set::repeat;
    }
    s.tricks.push_back(trick);
    s.repeat.reset(); // a real set: the refused one before it is no longer the latest news
    logging::log(logging::Level::info, logging::Channel::progression,
                 "Throwdowns: S.K.A.T.E. trick {} of event {:#x} set (trick {:#x}, stance {}, spins {}).",
                 s.tricks.size(), handle, trick.scorable, trick.stance, static_cast<std::int32_t>(trick.spins));
    return Set::fresh;
}

std::uintptr_t index_of(const void *array, const void *value, const void *type, std::int32_t *out) {
    const auto result = state().original.load(std::memory_order_acquire)(array, value, type, out);
    try {
        std::int32_t index{-1};
        if (out && memory::peek(reinterpret_cast<Address>(out), index)) {
            const auto frame = frame_of(profile_runtime::executing_expression);
            if(skate::active() && frame.locals) {
                auto record=read<std::array<std::uint8_t,28>>(frame.locals+trick_local);
                bool landed=read<std::uint8_t>(frame.locals+landed_local)!=0;
                if(skate::judge_native(read<std::uint32_t>(frame.locals+active_local),index==0,record,landed)) {
                    std::memcpy(reinterpret_cast<void*>(frame.locals+trick_local),record.data(),record.size());
                    poke(frame.locals+landed_local,landed?1:0);
                    logging::log(logging::Level::info,logging::Channel::progression,
                        "S.K.A.T.E. detector: native player {:#x}, {}, {}; target: {}",
                        read<std::uint32_t>(frame.locals+active_local),index==0?"setter":"follower",
                        landed?"accepted":"failed",skate::target_text());
                }
            } else if(index==0 && judge(frame)==Set::repeat)poke(frame.locals+landed_local,0);
        }
    } catch (...) {
    }
    return result;
}

Address image_end(Address base) {
    const auto dos = read<IMAGE_DOS_HEADER>(base);
    return base + read<IMAGE_NT_HEADERS64>(base + static_cast<Address>(dos.e_lfanew)).OptionalHeader.SizeOfImage;
}
// Points this loaded copy's setter test at index_of, if it is exactly the shipped graph.
void patch(Address base, Address resource) {
    if (read<std::uint32_t>(resource + 0x20) != header_frame || read<std::uint32_t>(resource + 0x24) != header_constants ||
        read<std::uint32_t>(resource + 0x2c) != header_code_words)
        throw std::runtime_error("header differs");
    const auto code = resource + code_offset;
    if (read<std::uint32_t>(code) != first_word) throw std::runtime_error("code differs");
    unsigned sites{};
    for (std::uint32_t i = 0; i < table_count; ++i) {
        const auto entry = read<std::array<std::uint32_t, 2>>(resource + table_offset + 8 * std::size_t{i});
        if (entry[0] == index_of_hash && entry[1] != index_of_pc) throw std::runtime_error("call table differs");
        sites += entry[0] == index_of_hash;
    }
    // One call: three inputs and the result.
    if (sites != 1 || (read<std::uint32_t>(code + index_of_pc) & 0xff) != 0x03)
        throw std::runtime_error("setter test differs");
    const auto slot = code + index_of_pc + 4;
    const auto current = read<Address>(slot);
    if (current < base || current >= image_end(base)) throw std::runtime_error("setter test is not bound");
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(slot), &info, sizeof(info)) ||
        !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        throw std::runtime_error("graph memory is not writable");
    auto &s = state();
    IndexOf expected{};
    if (!s.original.compare_exchange_strong(expected, reinterpret_cast<IndexOf>(current)) &&
        expected != reinterpret_cast<IndexOf>(current))
        throw std::runtime_error("setter test bound differently in another copy");
    *reinterpret_cast<void **>(slot) = reinterpret_cast<void *>(&index_of);
}
} // namespace

bool skate_attempt_prepare(Address base,Address vm) noexcept {
    if(!skate::active())return true;
    try {
        const auto resource=vm?read<Address>(vm+0x38):0;
        if(!resource || read<std::uint32_t>(resource+0x10)!=graph_hash)return true;
        auto& s=state();std::lock_guard lock(s.mutex);
        if(s.patched.contains(resource)) {
            if(read<Address>(resource+code_offset+index_of_pc+4)==reinterpret_cast<Address>(&index_of))return true;
            s.patched.erase(resource); // a world reload can reuse a resource address
        }
        patch(base,resource);s.patched.insert(resource);return true;
    }catch(...){skate::bridge_error();return false;}
}

void skate_attempt_ran(Address base, Address vm) noexcept {
    try {
        const auto resource = vm ? read<Address>(vm + 0x38) : 0;
        if (!resource || read<std::uint32_t>(resource + 0x10) != graph_hash) return;
        if(skate::active()) {
            const auto frame=frame_of(vm);
            if(frame.locals) {
                const auto player=read<std::uint32_t>(frame.locals+active_local);
                if(skate::native_pending(0)) {
                    logging::log(logging::Level::warning,logging::Channel::progression,
                        "S.K.A.T.E. detector: native attempt was not consumed (player {:#x}, guard {}); leaving to avoid a stuck turn.",
                        player,read<std::uint8_t>(frame.locals+guard_local));
                    skate::bridge_error();
                }
            }
            return;
        }
        auto &s = state();
        {
            std::lock_guard lock(s.mutex);
            if (s.patched.contains(resource) || s.refused.contains(resource)) return;
        }
        // This run went through the unpatched test: keep the set it made, if it made one
        // (the guard passed and the attempt was the setter's), so a repeat of it is caught.
        const auto frame = frame_of(vm);
        if (frame.locals && read<std::uint8_t>(frame.locals + guard_local) &&
            read<std::int32_t>(frame.locals + index_local) == 0)
            judge(frame);
        try {
            patch(base, resource);
            std::lock_guard lock(s.mutex);
            s.patched.insert(resource);
            logging::write(logging::Level::info, logging::Channel::progression,
                           "Throwdowns: S.K.A.T.E. no-repeat rule enabled for this throwdown.");
        } catch (const std::exception &e) {
            std::lock_guard lock(s.mutex);
            s.refused.insert(resource);
            logging::log(logging::Level::warning, logging::Channel::progression,
                         "Throwdowns: S.K.A.T.E. no-repeat rule left off: {}.", e.what());
        }
    } catch (...) {
    }
}

std::optional<SkateRepeat> skate_last_repeat() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.repeat;
}
} // namespace dingosdk::multiplayer
