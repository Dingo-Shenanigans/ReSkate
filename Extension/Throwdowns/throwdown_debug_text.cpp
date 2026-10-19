#include "throwdown_debug_text.h"
#include "throwdown_relay.h"
#include "skate_trick_rule.h"
#include "Extension/Skater/skate_detector.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/native_throwdowns.h"
#include "Engine/Game/Build/20260929/throwdown_debug_text.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Engine/Core/Platform/memory.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;
// ClientSkateThrowdown.OnDebugPrint as shipped: header {frame 0x110, constants 0x3610,
// code 0x3ada words}, code at +0x3e00 (first word op26 -> 0x164), native call table of
// 953 {hash, pc} pairs at +0x13218. Each call instruction keeps its function pointer in
// words 1-2, filled at load with the native's fast thunk (one pointer per operand).
constexpr std::uint32_t graph_hash = 0xc33322b2, header_frame = 0x110, header_constants = 0x3610,
                        header_code_words = 0x3ada, code_offset = 0x3e00, first_word = 0x16426,
                        table_offset = 0x13218, table_count = 953;
// The virtual screen the graph lays its text out in (it asks the debug screen size).
constexpr std::uint32_t screen_width = 1920, screen_height = 1080;
// Where the graph puts each player's row (pc 0xa790-0xaa78): "name: " at x 100 and the
// letters at x 350, 35 apart from y 130. The name is drawn in row_up for the player whose
// turn it is and row_out once they are eliminated; the letters twice, the whole word dim
// and then the earned part ("S.K.") in letters_earned. The messages above use the graph's
// colour ports as shipped (P38-P44): a prompt, a trick, a success and a failure.
constexpr float row_name_x = 100, row_letters_x = 350, first_row_y = 130, row_height = 35;
constexpr std::uint32_t row_up = 0xffffff16, row_out = 0xff0000ff, letters_earned = 0xff0000ff;
constexpr std::uint32_t message_trick = 0xffe5ffff, message_success = 0xff0d7d41, message_failure = 0xff0406ff;
constexpr int letters_to_lose = 5;

// The HUD graph draws every frame, one call per text line and row: guarded
// same-process copies, not a system call per read (or per character).
template<class T> T read(Address at) {
    T value{};
    if (!memory::peek(at, value)) throw std::runtime_error("Throwdown debug text: memory unavailable");
    return value;
}

struct State {
    Address base{};
    int letters_drawing{};            // this frame's rows so far
    std::atomic<int> letters{-1};     // the last complete frame's total
    std::mutex mutex;
    std::vector<overlay::GameTextLine> drawing, shown;
    overlay::SkateHud hud; // `shown`, read
    std::vector<std::string> done; // the tricks set in this game, as the HUD named them
    std::chrono::steady_clock::time_point shown_at{};
    std::atomic<bool> reskate{true};
    std::set<Address> patched, refused; // loaded copies of the graph
    std::atomic<unsigned> logged{};
};
State& state() {
    static auto* value = new State;
    return *value;
}

// The lines the graph puts above the trick that was set.
constexpr std::array<std::string_view, 4> set_headers{"Trick set:", "Opponent set trick:", "Copy the trick:",
                                                      "Opponent copying trick:"};
// A set the no-repeat rule refused (skate_trick_rule.h) reaches the game as a missed set.
// While that failure shows, it says why.
void explain_repeat(overlay::SkateHud& hud) {
    const auto repeat = skate_last_repeat();
    if (!repeat || std::chrono::steady_clock::now() - repeat->at > std::chrono::seconds(3)) return;
    for (auto& message : hud.messages) {
        if (message.kind != overlay::SkateHudMessage::Kind::failure) continue;
        if (message.text == "Failed to set a trick!") {
            message.text = "Already done! That trick was set before.";
        } else if (message.text == "Opponent FAILED setting the trick!") {
            const auto name = throwdown_relay_player_name(repeat->player);
            message.text = (name.empty() ? std::string("Opponent") : name) + " set an already-done trick!";
        }
    }
}

// One frame of the graph's text as messages and player rows.
overlay::SkateHud read_hud(const std::vector<overlay::GameTextLine>& lines) {
    overlay::SkateHud hud;
    std::vector<const overlay::GameTextLine*> messages;
    for (const auto& line : lines) {
        const bool row = !line.centered && line.y >= first_row_y && (line.x == row_name_x || line.x == row_letters_x);
        if (!row) {
            if (line.text.find_first_not_of(' ') != std::string::npos) messages.push_back(&line);
            continue;
        }
        const auto index = static_cast<std::size_t>((line.y - first_row_y) / row_height + 0.5f);
        if (index >= 64) continue;
        if (hud.players.size() <= index) hud.players.resize(index + 1);
        auto& player = hud.players[index];
        if (line.x == row_name_x) {
            player.name = line.text;
            if (player.name.ends_with(": ")) player.name.resize(player.name.size() - 2);
            player.up = line.color == row_up;
            player.out = player.out || line.color == row_out;
        } else if (line.color == letters_earned) {
            player.letters = static_cast<int>(std::count_if(line.text.begin(), line.text.end(),
                [](char c) { return c >= 'A' && c <= 'Z'; }));
        }
    }
    for (auto& player : hud.players) {
        player.letters = std::clamp(player.letters, 0, letters_to_lose);
        player.out = player.out || player.letters >= letters_to_lose;
    }
    std::stable_sort(messages.begin(), messages.end(), [](const auto* a, const auto* b) { return a->y < b->y; });
    for (const auto* line : messages) {
        using Kind = overlay::SkateHudMessage::Kind;
        const auto kind = line->color == message_trick ? Kind::trick
                        : line->color == message_success ? Kind::success
                        : line->color == message_failure ? Kind::failure : Kind::prompt;
        hud.messages.push_back({kind, line->text});
    }
    return hud;
}

std::string engine_string(Address string) {
    std::string text;
    const auto chars = string ? read<Address>(string) : 0;
    // Up to 200 characters, copied to the end of a page at a time (the bytes after
    // the terminator may belong to a page that is not mapped).
    std::array<char, 200> chunk{};
    while (chars && text.size() < chunk.size()) {
        const auto from = chars + text.size();
        const auto size = std::min<std::size_t>(chunk.size() - text.size(), 0x1000 - from % 0x1000);
        if (!memory::peek_bytes(from, chunk.data(), size)) break;
        const auto end = std::find(chunk.data(), chunk.data() + size, '\0');
        text.append(chunk.data(), end);
        if (end != chunk.data() + size) break;
    }
    return text;
}

// DebugDrawText2D's fast thunk takes one pointer per operand: int x, int y, float,
// string text, colour, float scale, float, u32, u32.
void draw_text(const std::int32_t* x, const std::int32_t* y, const float*, const Address* text, const std::uint32_t* colour,
               const float* scale, const float*, const std::uint32_t* a7, const std::uint32_t* a8) {
    try {
        auto& s = state();
        overlay::GameTextLine line;
        line.x = static_cast<float>(read<std::int32_t>(reinterpret_cast<Address>(x)));
        line.y = static_cast<float>(read<std::int32_t>(reinterpret_cast<Address>(y)));
        line.text = engine_string(reinterpret_cast<Address>(text));
        line.color = read<std::uint32_t>(reinterpret_cast<Address>(colour));
        line.centered = read<std::uint32_t>(reinterpret_cast<Address>(a7)) == 1;
        const auto raw_scale = read<float>(reinterpret_cast<Address>(scale));
        line.scale = raw_scale > 0.05f && raw_scale < 20.0f ? raw_scale : 1.0f;
        if (s.logged.fetch_add(1) < 24) {
            std::array<std::uint32_t, 4> wide{};
            memory::read(reinterpret_cast<Address>(colour), wide);
            std::uint32_t e7{}, e8{};
            memory::read(reinterpret_cast<Address>(a7), e7);
            memory::read(reinterpret_cast<Address>(a8), e8);
            logging::log(logging::Level::info, logging::Channel::progression,
                "Throwdowns: S.K.A.T.E. HUD text at ({}, {}) colour {:#010x} [{:#x} {:#x} {:#x}] scale {} extra {:#x} {:#x}: \"{}\".",
                line.x, line.y, line.color, wide[1], wide[2], wide[3], raw_scale, e7, e8, line.text);
        }
        std::lock_guard lock(s.mutex);
        if (s.drawing.size() < 128) s.drawing.push_back(std::move(line));
    } catch (...) {}
}
// The row label is GetPlayerName(FindPlayerById(uid)) for each {Uid, Score} entry of the
// letters list, which the loop points VM page 24 at: offline no player has a name, and
// relayed players have no native player. Answer from the session for the row's id.
constexpr std::uint32_t row_entry_page = 24, player_name_fast = addr::throwdown_debug_text::player_name_fast,
                         player_name_raw = addr::throwdown_debug_text::player_name_raw;
void row_player_name(const Address* player, Address out) {
    auto& s = state();
    try {
        const auto vm = profile_runtime::executing_expression;
        const auto resource = vm ? read<Address>(vm + 0x38) : 0;
        if (resource && read<std::uint32_t>(resource + 0x10) == graph_hash) {
            const auto frame = read<std::uint32_t>(resource + 0x20);
            const auto entry = read<Address>(read<Address>(vm + 0x30) + ((frame + 15U) & ~15U) + 8 * row_entry_page);
            if (entry) {
                const auto letters = read<std::int32_t>(entry + 4);
                if (letters > 0 && letters < 16) s.letters_drawing += letters;
            }
            const auto name = entry ? throwdown_relay_player_name(read<std::uint32_t>(entry)) : std::string{};
            if (!name.empty()) {
                reinterpret_cast<void (*)(Address, const char*, std::uint32_t)>(s.base + addr::native_throwdowns::string_assign_chars)(
                    out, name.data(), static_cast<std::uint32_t>(name.size()));
                return;
            }
        }
    } catch (...) {}
    reinterpret_cast<void (*)(const Address*, Address)>(s.base + player_name_raw)(player, out);
}
void debug_screen_width(std::uint32_t* out) { if (out) *out = screen_width; }
void debug_screen_height(std::uint32_t* out) { if (out) *out = screen_height; }

struct Native {
    std::uint32_t hash;
    Address fast; // the thunk the loader puts in the slot (RVA)
    void* replacement;
    unsigned sites;
};
const std::array natives{
    Native{0x056455c2, addr::throwdown_debug_text::draw_text_fast, reinterpret_cast<void*>(&draw_text), 31},           // DebugDrawText2D
    Native{0x33c6ca2a, player_name_fast, reinterpret_cast<void*>(&row_player_name), 1}, // GetPlayerName
    Native{0x72823522, addr::throwdown_debug_text::debug_screen_size_fast, reinterpret_cast<void*>(&debug_screen_width), 30},  // GetDebugScreenWidth
    Native{0x2e18a002, addr::throwdown_debug_text::debug_screen_size_fast, reinterpret_cast<void*>(&debug_screen_height), 31}}; // GetDebugScreenHeight

// Points this loaded copy's debug calls at the working ones, if it is exactly the shipped graph.
bool patch(Address base, Address resource) {
    if (read<std::uint32_t>(resource + 0x20) != header_frame || read<std::uint32_t>(resource + 0x24) != header_constants ||
        read<std::uint32_t>(resource + 0x2c) != header_code_words)
        throw std::runtime_error("header differs");
    const auto code = resource + code_offset;
    if (read<std::uint32_t>(code) != first_word) throw std::runtime_error("code differs");
    std::vector<std::pair<Address, void*>> slots;
    std::array<unsigned, natives.size()> found{};
    for (std::uint32_t i = 0; i < table_count; ++i) {
        const auto entry = read<std::array<std::uint32_t, 2>>(resource + table_offset + 8 * std::size_t{i});
        for (std::size_t n = 0; n < natives.size(); ++n) {
            if (entry[0] != natives[n].hash) continue;
            if (entry[1] >= header_code_words * 4) throw std::runtime_error("call table differs");
            const auto slot = code + entry[1] + 4;
            if ((read<std::uint32_t>(code + entry[1]) & 0xff) > 0x0f || read<Address>(slot) != base + natives[n].fast)
                throw std::runtime_error(std::format("call at pc {:#x} differs", entry[1]));
            slots.emplace_back(slot, natives[n].replacement);
            ++found[n];
        }
    }
    for (std::size_t n = 0; n < natives.size(); ++n)
        if (found[n] != natives[n].sites) throw std::runtime_error("call sites differ");
    for (const auto& [slot, replacement] : slots) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(slot), &info, sizeof(info)) ||
            !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            throw std::runtime_error("graph memory is not writable");
    }
    for (const auto& [slot, replacement] : slots) *reinterpret_cast<void**>(slot) = replacement;
    return true;
}
// The graph re-posts itself for the next frame: PostEventDelayedFrames at pc 0xad2c, operands
// S0 (self), payload, port offset, payload type and the frame count K1c8c (1). Every few frames
// is plenty for a HUD the overlay keeps for 300 ms, so that one call passes hud_frames instead.
// Kept apart from patch(): if it does not match, the HUD still works, redrawn every frame.
constexpr std::uint32_t repost_hash = 0x9967fc07, repost_pc = 0xad2c, repost_word = 0xad6004,
                        repost_frames_constant = 0x1c8c;
constexpr Address post_frames_fast = addr::throwdown_debug_text::post_frames_fast;
constexpr std::int32_t hud_frames = 3;
void repost_hud(const void* self, const void* payload, const void* port, const void* type, const std::int32_t*) {
    reinterpret_cast<void (*)(const void*, const void*, const void*, const void*, const std::int32_t*)>(
        state().base + post_frames_fast)(self, payload, port, type, &hud_frames);
}
// Points this copy's re-post at repost_hud (patch() has checked the header and code).
void slow_repost(Address base, Address resource) {
    const auto code = resource + code_offset;
    unsigned sites{};
    for (std::uint32_t i = 0; i < table_count; ++i) {
        const auto entry = read<std::array<std::uint32_t, 2>>(resource + table_offset + 8 * std::size_t{i});
        if (entry[0] != repost_hash) continue;
        if (entry[1] != repost_pc) throw std::runtime_error("re-post calls differ");
        ++sites;
    }
    // Four operands and the frame count, self first.
    if (sites != 1 || read<std::uint32_t>(code + repost_pc) != repost_word ||
        read<std::array<std::uint32_t, 2>>(code + repost_pc + 12) != std::array<std::uint32_t, 2>{3, 0} ||
        read<std::array<std::uint32_t, 2>>(code + repost_pc + 12 + 8 * 4) !=
            std::array<std::uint32_t, 2>{0, repost_frames_constant})
        throw std::runtime_error("re-post differs");
    const auto slot = code + repost_pc + 4;
    if (const auto bound = read<Address>(slot); bound != base + post_frames_fast)
        throw std::runtime_error(std::format("re-post is bound to {:#x}, expected {:#x}", bound - base, post_frames_fast));
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(slot), &info, sizeof(info)) ||
        !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        throw std::runtime_error("graph memory is not writable");
    *reinterpret_cast<void**>(slot) = reinterpret_cast<void*>(&repost_hud);
}
} // namespace

void skate_debug_text_ran(Address base, Address vm) noexcept {
    try {
        auto& s = state();
        const auto resource = vm ? read<Address>(vm + 0x38) : 0;
        if (!resource || read<std::uint32_t>(resource + 0x10) != graph_hash) return;
        {
            std::lock_guard lock(s.mutex);
            s.base = base;
            // The text drawn during the run that just finished is this frame's HUD.
            // A throwdown that starts after a pause is a new game: its tricks start fresh.
            const auto now = std::chrono::steady_clock::now();
            if (s.shown_at != std::chrono::steady_clock::time_point{} && now - s.shown_at > std::chrono::seconds(3))
                s.done.clear();
            s.shown.swap(s.drawing);
            s.drawing.clear();
            s.hud = read_hud(s.shown);
            if(const auto target=skate::target_text();!target.empty())
                for(std::size_t i=1;i<s.hud.messages.size();++i)
                    if(s.hud.messages[i].kind==overlay::SkateHudMessage::Kind::trick &&
                       std::ranges::find(set_headers,std::string_view(s.hud.messages[i-1].text))!=set_headers.end())s.hud.messages[i].text=target;
            // Every set trick is shown under one of the set headers at some point.
            for (std::size_t i = 1; i < s.hud.messages.size(); ++i) {
                const auto& trick = s.hud.messages[i];
                if (trick.kind == overlay::SkateHudMessage::Kind::trick &&
                    std::ranges::find(set_headers, std::string_view(s.hud.messages[i - 1].text)) != set_headers.end() &&
                    std::ranges::find(s.done, trick.text) == s.done.end() && s.done.size() < 64)
                    s.done.push_back(trick.text);
            }
            s.hud.done_tricks = s.done;
            explain_repeat(s.hud);
            s.shown_at = std::chrono::steady_clock::now();
            if (s.patched.contains(resource)) s.letters.store(s.letters_drawing, std::memory_order_release);
            s.letters_drawing = 0;
        }
        bool known{};
        {
            std::lock_guard lock(s.mutex);
            known = s.patched.contains(resource) || s.refused.contains(resource);
        }
        if (known) return;
        try {
            patch(base, resource);
            std::lock_guard lock(s.mutex);
            s.patched.insert(resource);
            logging::write(logging::Level::info, logging::Channel::progression,
                "Throwdowns: S.K.A.T.E. HUD (the game's debug text) enabled for this throwdown.");
            try {
                slow_repost(base, resource);
                logging::write(logging::Level::info, logging::Channel::progression,
                    "Throwdowns: S.K.A.T.E. HUD redrawn every 3 frames instead of every frame.");
            } catch (const std::exception& e) {
                logging::log(logging::Level::warning, logging::Channel::progression,
                    "Throwdowns: S.K.A.T.E. HUD still redrawn every frame: {}.", e.what());
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(s.mutex);
            s.refused.insert(resource);
            logging::log(logging::Level::warning, logging::Channel::progression,
                "Throwdowns: S.K.A.T.E. HUD left off: {}.", e.what());
        }
    } catch (...) {}
}

int skate_letters_total() noexcept {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (s.shown_at == std::chrono::steady_clock::time_point{} ||
            std::chrono::steady_clock::now() - s.shown_at > std::chrono::seconds(2))
            return -1;
    }
    return s.letters.load(std::memory_order_acquire);
}

overlay::GameText skate_debug_text() {
    auto& s = state();
    if (s.reskate.load(std::memory_order_relaxed)) return {};
    std::lock_guard lock(s.mutex);
    if (s.shown.empty() || std::chrono::steady_clock::now() - s.shown_at > std::chrono::milliseconds(300)) return {};
    return {static_cast<float>(screen_width), static_cast<float>(screen_height), s.shown};
}

overlay::SkateHud skate_hud() {
    auto& s = state();
    if (!s.reskate.load(std::memory_order_relaxed)) return {};
    std::lock_guard lock(s.mutex);
    if (s.shown.empty()) return {};
    const auto age = std::chrono::steady_clock::now() - s.shown_at;
    if (age <= std::chrono::milliseconds(300)) return s.hud;
    // The graph stops the moment the game ends, and offline there is no results screen: the
    // throwdown would just vanish. When its last frame shows one player left standing, keep
    // the final letters up for a few seconds with the winner.
    if (age > std::chrono::seconds(6)) return {};
    const auto standing = std::count_if(s.hud.players.begin(), s.hud.players.end(), [](const auto& p) { return !p.out; });
    const auto winner = std::find_if(s.hud.players.begin(), s.hud.players.end(), [](const auto& p) { return !p.out; });
    if (standing != 1 || s.hud.players.size() < 2) return {};
    auto result = s.hud;
    using Kind = overlay::SkateHudMessage::Kind;
    result.messages = {{Kind::success, "S.K.A.T.E.!"},
                      {Kind::trick, (winner->name.empty() ? std::string("The last player standing") : winner->name) + " wins"}};
    for (auto& player : result.players) player.up = false;
    return result;
}

void set_skate_hud_reskate(bool reskate) noexcept { state().reskate.store(reskate, std::memory_order_relaxed); }
bool skate_hud_reskate() noexcept { return state().reskate.load(std::memory_order_relaxed); }
} // namespace dingosdk::multiplayer
