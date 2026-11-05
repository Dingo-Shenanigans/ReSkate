#include "native_menu.h"
#include "native_menu_data.h"
#include "Engine/Core/Log/logging.h"
#include <array>
#include <atomic>
#include <cctype>
#include <string>
#include <utility>


// The pause menu's 3D backdrop (the room behind the HUB and Skater tabs), offered behind ReSkate's
// Mod Manager tab as a test (`ui modbackdrop`, off by default).
//
// How the game picks it, read from build 20260929's data (2026-11-04):
// - gameplay/core/dingogamecore places gameplay/myworld/prefab_myworldmenushared, which places the
//   room (presentation/assets/prefabs/backgrounds/prefab_3dmenusetup) and its camera.
// - dingogamecore listens to the UI data model MSGlobalDataModel
//   (ui/features/mystuff/cas/structs/msglobaldatamodel). A SelectEnumeration on its Environment
//   (MenuEnvironment: None 0, CAS 1, Skatepass 2, Store 3, Skatepedia 4) sends one event per value
//   into the menu prefab, and its MyWorldVisible feeds the prefab's gate. The room picks its look
//   (prefab_background_cas, _store, ...) from the same value.
// So writing those two fields is what the game's own tabs do to show it. ReSkate writes them only
// while its Mod Manager tab is on screen, and puts back what was there when it leaves, unless the
// game has written something else meanwhile.
namespace dingosdk::multiplayer {
using namespace menu_data;
namespace {
// TypeNameHash of MSGlobalDataModel, and its fields' NameHash (from their EBX).
constexpr std::uint32_t global_model = 0x17aa94a9;
struct Named { const char* name; std::uint32_t hash; };
constexpr std::array<Named, 7> watched{{{"IsFTUE", 0x929523fe}, {"IsStanceSelected", 0xd249a861},
    {"Environment", 0x6d0b347f}, {"ActiveCount", 0x6ee5f61e}, {"IsCASHUB", 0xd4817308},
    {"ActiveSlotPage", 0xc3d8194f}, {"MyWorldVisible", 0xf4dbe2a3}}};
constexpr unsigned environment = 2, world_visible = 6;
constexpr std::array<const char*, 5> environments{"none", "cas", "skatepass", "store", "skatepedia"};

std::atomic<int> wanted{-1}; // -1: off, else a MenuEnvironment value
struct Backdrop {
    Handle root{};
    Address type{};
    ULONGLONG next_scan{};
    bool applied{};
    int written{};            // the Environment ReSkate wrote
    std::int64_t before_environment{}, before_visible{};
    std::array<std::int64_t, watched.size()> seen{};
    bool seen_any{};
};
Backdrop& backdrop() { static Backdrop b; return b; }

void say(const std::string& text) { logging::write(logging::Level::info, logging::Channel::ui, "Menu backdrop: " + text); }

// A small scalar field as a number; false for anything else (a collection, a text).
bool number(const Context& c, Value v, std::int64_t& out) {
    const auto bytes = size(v.type);
    const auto at = c.address(v);
    switch (bytes) {
    case 1: out = read<std::uint8_t>(at); return true;
    case 2: out = read<std::int16_t>(at); return true;
    case 4: out = read<std::int32_t>(at); return true;
    default: return false;
    }
}
void write_number(const Context& c, Value v, std::int64_t value) {
    switch (size(v.type)) {
    case 1: c.set_if_changed(v, static_cast<std::uint8_t>(value)); break;
    case 2: c.set_if_changed(v, static_cast<std::int16_t>(value)); break;
    case 4: c.set_if_changed(v, static_cast<std::int32_t>(value)); break;
    default: throw std::runtime_error("the field is not a small number");
    }
}
Value find_root(const Context& c, Backdrop& b) {
    if (b.root && b.type && c.type_of(b.root) == b.type) return {b.root, b.type};
    b.root = 0; b.type = 0;
    const auto now = GetTickCount64();
    if (now < b.next_scan) return {};
    b.next_scan = now + 1000;
    const auto roots = c.roots({global_model});
    if (roots.empty()) return {};
    if (roots.size() > 1) say(std::to_string(roots.size()) + " MSGlobalDataModel roots; using the first.");
    b.root = roots.front().model.handle; b.type = roots.front().model.type;
    return roots.front().model;
}
} // namespace

std::string set_mod_backdrop(std::string_view choice) {
    std::string lower(choice);
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.empty() || lower == "status") {
        const int now = wanted.load();
        return now < 0 ? "Mod Manager backdrop: off." : std::string("Mod Manager backdrop: ") + environments[now] + ".";
    }
    if (lower == "off") {
        wanted.store(-1);
        return "Mod Manager backdrop: off. The Mod Manager tab shows the world again once you leave it.";
    }
    for (int i = 1; i < static_cast<int>(environments.size()); ++i)
        if (lower == environments[i]) {
            wanted.store(i);
            return std::string("Mod Manager backdrop: ") + environments[i] +
                   ". Open the Mod Manager tab in the pause menu; the game's menu room should show behind it.";
        }
    return "Use off, cas, store, skatepass or skatepedia.";
}

// Client thread, under the model write lock, every page tick of the pause menu.
void update_mod_backdrop(const Context& c, bool mod_manager_shown, bool in_menu) {
    auto& b = backdrop();
    const int want = wanted.load(std::memory_order_relaxed);
    if (want < 0 && !b.applied) { b.seen_any = false; return; }
    try {
        const auto root = find_root(c, b);
        if (!root.handle) return;
        std::array<Value, watched.size()> fields{};
        std::array<std::int64_t, watched.size()> now{};
        std::array<bool, watched.size()> known{};
        for (std::size_t i = 0; i < watched.size(); ++i) {
            try {
                fields[i] = c.field(root, watched[i].hash);
                known[i] = number(c, fields[i], now[i]);
            } catch (...) {}
        }
        if (!known[environment] || !known[world_visible]) {
            static bool told{};
            if (!std::exchange(told, true)) say("MSGlobalDataModel has no Environment or MyWorldVisible it can read; nothing done.");
            return;
        }
        // What the game writes as tabs change: logged while the test is on, so a run shows which
        // tab sets what.
        if (in_menu && (!b.seen_any || now != b.seen)) {
            std::string line = mod_manager_shown ? "on Mod Manager:" : "on another page:";
            for (std::size_t i = 0; i < watched.size(); ++i)
                if (known[i]) line += std::string(" ") + watched[i].name + "=" + std::to_string(now[i]);
            say(line);
            b.seen = now; b.seen_any = true;
        }
        const bool show = want > 0 && in_menu && mod_manager_shown;
        if (show) {
            if (!b.applied) {
                b.before_environment = now[environment];
                b.before_visible = now[world_visible];
                b.applied = true;
                say(std::string("showing the ") + environments[want] + " room behind Mod Manager (was Environment=" +
                    std::to_string(b.before_environment) + ", MyWorldVisible=" + std::to_string(b.before_visible) + ").");
            } else if (now[environment] != b.written) {
                say("the game set Environment=" + std::to_string(now[environment]) + " while Mod Manager was up; set back.");
            }
            write_number(c, fields[environment], want);
            write_number(c, fields[world_visible], 1);
            b.written = want;
        } else if (b.applied) {
            b.applied = false;
            // Left alone when the page now showing wrote its own.
            if (now[environment] == b.written) {
                write_number(c, fields[environment], b.before_environment);
                write_number(c, fields[world_visible], b.before_visible);
                say("put back Environment=" + std::to_string(b.before_environment) + ", MyWorldVisible=" +
                    std::to_string(b.before_visible) + ".");
            } else {
                say("left Environment=" + std::to_string(now[environment]) + " as the game set it.");
            }
        }
    } catch (const std::exception& e) {
        b.root = 0; b.applied = false;
        static std::string last;
        if (last != e.what()) { last = e.what(); say(std::string("unavailable (") + e.what() + ")."); }
    } catch (...) {
        b.root = 0; b.applied = false;
    }
}
} // namespace dingosdk::multiplayer
