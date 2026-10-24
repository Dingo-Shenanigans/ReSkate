#include "native_flag_session.h"
#include "native_flag_spawn.h"
#include "native_throwdowns.h"
#include "throwdown_kit.h"
#include "throwdown_lab.h"
#include "throwdown_relay.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <vector>

namespace dingosdk::multiplayer::native_flag {
namespace kit = throwdowns::kit;
namespace {
enum class Step { idle, open, setup, flag, placing, closing, waiting, cleanup };
struct Request {
    std::string mode;
    std::uint32_t game{};
    kit::flag::Options options;
    std::uint64_t world{};
};
// Events for the owner's plugin, posted once the placement lock is released.
struct Note {
    std::string mode, action;
    std::vector<std::string> args;
};
struct Placement {
    std::mutex mutex;
    Step step{};
    std::string mode;
    std::uint32_t game{};
    kit::flag::Options options;
    std::uint64_t world{}, until{}, changed{}, sent{};
    unsigned seconds{}, players{};
    std::uint32_t mmid{};
    std::vector<std::uint32_t> custom_mmids;
    bool created{}, setup_seen{}, flag_seen{}, exited{}, cancelling{}, destroy_queued{}, destroyed{};
    bool handed_off{}, match_seen{};
    bool show_setup{}, native_entry{}, confirmed{};
    bool owner_left{}; // kit::flag::leave: the plugin knows; no flag_left
    bool adopted{};    // a guest's registration (kit::flag::adopt)
    std::optional<std::array<float,3>> spawn;
    Facing facing=default_facing;
    std::optional<Request> pending; // opens once the previous registration's teardown finishes
    std::string status;
    std::vector<Note> notes;
};
Placement& placement() { static auto* p = new Placement; return *p; }

// Offline the native level generation; in a session the relay's world, which follows the transport.
std::uint64_t current_world() {
    const auto world = multiplayer_session_active() ? throwdown_relay_world() : 0;
    return world ? world : native_throwdown_world();
}
void note(Placement& p, std::string action, std::vector<std::string> args = {}) {
    if (!p.mode.empty()) p.notes.push_back({p.mode, std::move(action), std::move(args)});
}
void flush() noexcept {
    std::vector<Note> notes;
    {
        auto& p = placement();
        std::lock_guard lock(p.mutex);
        notes.swap(p.notes);
    }
    for (auto& n : notes) try { kit::post(n.mode, std::move(n.action), std::move(n.args)); } catch (...) {}
}
struct FlushOnExit { ~FlushOnExit() { flush(); } };
void reset(Placement& p) {
    p.step = Step::idle; p.mode.clear(); p.game = 0; p.options = {};
    p.spawn.reset(); p.facing = default_facing; p.mmid = 0; p.sent = 0;
    p.created = p.setup_seen = p.flag_seen = p.exited = p.cancelling = p.destroy_queued = p.destroyed = false;
    p.handed_off = p.match_seen = false;
    p.show_setup = p.native_entry = p.confirmed = p.owner_left = p.adopted = false;
}
// The registration ends; its plugin hears flag_left unless it ended it itself.
void retire(Placement& p) {
    if (p.step != Step::idle && !p.owner_left) note(p, "flag_left", {std::to_string(p.game)});
    reset(p);
}
void to_cleanup(Placement& p, std::uint64_t now) {
    // sent belongs to the previous navigation step. A newly cancelled flag still needs its
    // Exit, but repeated leave notifications must not queue that same Exit again.
    if (p.step != Step::cleanup) p.sent = 0;
    p.cancelling = true; p.step = Step::cleanup; p.until = now + 15000;
}
// Left before its setup was asked for (a plugin refusing its card at `open`): nothing of the game's
// is open to back out of, so the registration just ends. An exit the game never takes stays
// pending, and every later flag's setup waits behind it (2026-10-21, hosting with a player).
bool leave_unopened(Placement& p) {
    if (p.step != Step::open || p.created || p.setup_seen) return false;
    p.owner_left = true;
    retire(p);
    p.status.clear();
    return true;
}
void open_request(Placement& p, const Request& r, std::uint64_t now) {
    reset(p);
    p.step = Step::open; p.mode = r.mode; p.game = r.game; p.options = r.options; p.world = r.world;
    p.show_setup = p.native_entry = r.options.setup_page;
    p.seconds = r.options.seconds; p.players = r.options.players;
    p.changed = now; p.until = now + 15000;
    p.status = r.options.setup_page ? "Opening the Throwdown setup..." : "Opening flag placement...";
}
bool waiting_card(const Placement& p) {
    return p.step == Step::waiting && p.handed_off && p.spawn && p.mmid;
}
// Documents name their mode by the plugin's own id or the registered "<plugin>/<id>".
bool same_mode(std::string_view registered, std::string_view named) {
    if (named.empty() || registered == named) return true;
    return registered.size() > named.size() && registered.ends_with(named) && registered[registered.size() - named.size() - 1] == '/';
}
bool shows_waiting(const kit::Documents& d, std::string_view mode) {
    return d.waiting && same_mode(mode, d.waiting->mode);
}
// QueueStateNavigation's live graph publishes these two fields, with metadata
// [-1] for a command without arguments. Do not change the Back keyword stack.
bool navigate(const menu_data::Context& c, std::string_view name, std::uint32_t consumed = 0) {
    using namespace menu_data;
    const auto roots = c.roots({0xcc4776b5});
    require(roots.size() == 1, "Native navigation model is unavailable.");
    const auto current = c.field(roots.front().model, 0xf3ba9cf8);
    const auto metadata = c.field(roots.front().model, 0xf437b255);
    const auto pending = read<std::uint32_t>(c.address(current));
    if (pending && pending != consumed) {
        // The game's own navigation is still on its way: tried again at the next tick. Said every
        // few seconds, with what it is, for a setup that never opens.
        static std::uint64_t next_note{};
        if (const auto now = GetTickCount64(); now >= next_note) {
            next_note = now + 3000;
            logging::log(logging::Level::info, logging::Channel::ui, "Native flag: navigation {} waits; the game's navigation 0x{:08x} is pending.",
                         name, pending);
        }
        return false;
    }
    c.set(current, game::native_name_hash(name));
    c.array(metadata, std::vector<std::int32_t>{-1});
    logging::log(logging::Level::info, logging::Channel::ui, "Native flag: navigation {}.", name);
    return true;
}
} // namespace

bool placement_active() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.step != Step::idle && p.step != Step::waiting;
}
bool registration_owned() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    // The running match outlives its temporary native flag registration. Once that
    // registration is destroyed, stock challenges own their events.
    return p.step != Step::idle && (p.step != Step::waiting || p.mmid != 0);
}
bool registration_waiting() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return waiting_card(p);
}
bool registration_retiring() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.step == Step::cleanup;
}
bool owns_registration(std::uint32_t mmid) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return mmid && p.step != Step::idle && p.mmid == mmid;
}
bool hosting() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.step != Step::idle && !p.adopted;
}
bool setup_active() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.show_setup && (p.step == Step::open || p.step == Step::setup);
}
std::string mode() {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.mode;
}
std::uint32_t game() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.game;
}
float cursor_height() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.options.cursor_height > 0 && p.flag_seen && !p.cancelling && !p.spawn &&
        (p.step == Step::flag || p.step == Step::placing) ? p.options.cursor_height : 0.f;
}
Limits limits() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.step != Step::idle ? Limits{p.seconds, p.players} : Limits{};
}
std::string status() {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return p.status;
}
void set_setup_limits(unsigned seconds, unsigned players) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    if (p.step == Step::setup && seconds >= 1 && seconds <= 3600 && players >= 1 && players <= 10) { p.seconds = seconds; p.players = players; }
}
void retire_unconfirmed_setup() noexcept {
    {
        auto& p = placement(); std::lock_guard lock(p.mutex);
        // Called by a verified fresh picker callback, after returning to Throwdowns. A
        // cancelled setup with no registered flag cannot keep an unacknowledged Exit alive and
        // close or block the next page.
        if ((p.step != Step::open && p.step != Step::setup && p.step != Step::cleanup) ||
            !p.native_entry || p.flag_seen || p.mmid || p.handed_off) return;
        if (p.created) release_native_flag_placeholder();
        retire(p); p.pending.reset(); p.status.clear();
    }
    flush();
}
bool start() noexcept {
    std::string mode; std::uint32_t game{};
    {
        auto& p = placement(); std::lock_guard lock(p.mutex);
        if (!waiting_card(p)) return false;
        mode = p.mode; game = p.game;
    }
    try { kit::post(mode, "start", {std::to_string(game)}); } catch (...) {}
    logging::log(logging::Level::info, logging::Channel::ui, "Native flag: {} Start requested on the waiting card.", mode);
    return true;
}
bool leave() noexcept {
    {
        auto& p = placement(); std::lock_guard lock(p.mutex);
        if (p.step == Step::idle) return false;
        note(p, "leave", {std::to_string(p.game)});
        p.pending.reset(); // Quit/Cancel must not reopen a queued replacement
        if (!leave_unopened(p)) to_cleanup(p, GetTickCount64());
    }
    flush();
    return true;
}
bool quit_match() noexcept {
    try {
        const auto active = kit::active();
        if (!active) return false;
        auto mode = active->waiting ? active->waiting->mode : active->setup ? active->setup->mode : std::string{};
        if (mode.empty()) mode = native_flag::mode();
        if (mode.empty()) mode = kit::current();
        kit::post_to(active->plugin, "quit", std::move(mode));
        return true;
    } catch (...) { return false; }
}
void observe_graph(std::uint32_t graph) noexcept {
    {
        auto& p = placement(); std::lock_guard lock(p.mutex);
        if (p.step == Step::idle) return;
        if (graph == 0x0f131312 && p.step == Step::setup) { // Spot Battle setup OnBegin
            p.setup_seen = true; p.until = GetTickCount64() + 300000;
        }
        if (graph == 1867583346U && p.step == Step::setup && p.setup_seen) { // Same setup OnEnd
            // Back and Confirm both leave this native state. Stop adapting its UI immediately;
            // a successful Confirm enters the flag update below. Ignore a previous entry's
            // delayed OnEnd before this one's OnBegin.
            p.show_setup = false; p.changed = GetTickCount64(); p.until = p.changed + 1500;
        }
        if (graph == 1867583346U && p.step == Step::cleanup && !p.flag_seen && !p.mmid)
            p.exited = true; // the plugin closed the native setup without creating a flag
        // Spot Battle's own placement update runs after its validator/spawn initialization.
        // Its Confirm goes directly here; no S.K.A.T.E. area state, skip predicate, or
        // synthetic navigation is involved.
        if (graph == 0x9700be92 && p.created && p.setup_seen &&
            (p.step == Step::setup || (p.step == Step::flag && !p.flag_seen))) {
            p.show_setup = false;
            p.step = Step::flag; p.flag_seen = true; p.until = GetTickCount64() + 300000;
            if (p.native_entry && !p.confirmed) {
                p.confirmed = true;
                note(p, "setup_confirm", {std::to_string(p.players), std::to_string(p.seconds)});
            }
            logging::write(logging::Level::info, logging::Channel::ui, "Native flag: entered native Spot Battle flag placement.");
        }
    }
    flush();
}
void observe_queue(std::uint32_t mmid) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    if (!mmid || p.step == Step::idle) return;
    if (std::find(p.custom_mmids.begin(), p.custom_mmids.end(), mmid) == p.custom_mmids.end()) {
        if (p.custom_mmids.size() == 64) p.custom_mmids.erase(p.custom_mmids.begin());
        p.custom_mmids.push_back(mmid);
    }
}
bool known_queue(std::uint32_t mmid) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    return mmid && std::find(p.custom_mmids.begin(), p.custom_mmids.end(), mmid) != p.custom_mmids.end();
}
void observe_created() noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    if (p.step == Step::setup) p.created = true;
}
void observe_position(std::uint32_t mmid, const std::array<float,3>& spawn, const std::array<float,16>* transform) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    if ((p.step != Step::placing && p.step != Step::flag) || !p.created || !p.flag_seen || !mmid ||
        !std::all_of(spawn.begin(), spawn.end(), [](float n) { return std::isfinite(n) && std::abs(n) < 100000; })) return;
    if (transform) {
        const auto facing = transform_facing(*transform);
        if (!valid_facing(facing)) return;
        p.facing = facing;
    }
    p.spawn = spawn; p.mmid = mmid; p.step = Step::closing; p.until = GetTickCount64() + 15000;
    p.status = "Saving the starting flag...";
    logging::log(logging::Level::info, logging::Channel::ui,
        "Native flag: {} accepted spawn ({:.2f}, {:.2f}, {:.2f}), facing {:.2f} degrees, native queue {:#x}.",
        p.mode, spawn[0], spawn[1], spawn[2], facing_yaw(p.facing), mmid);
}
void observe_exit(bool cancelling) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    if (p.step == Step::idle) return;
    p.exited = true; p.cancelling = cancelling || !p.spawn;
    p.step = p.cancelling ? Step::cleanup : Step::waiting; p.changed = GetTickCount64(); p.until = p.changed + 15000;
    if (p.cancelling) p.status = "Flag placement cancelled.";
}
void observe_destroyed(std::uint32_t mmid) noexcept {
    auto& p = placement(); std::lock_guard lock(p.mutex);
    if (p.step == Step::waiting && p.handed_off && p.destroy_queued && p.mmid == mmid) {
        // The registration is a lobby flag, not the running match. Once its transform is
        // saved, the native teardown removes the pole, ground decal and waiting notification
        // without ending the match.
        p.mmid = 0; p.destroyed = true; p.changed = GetTickCount64(); return;
    }
    if (p.step == Step::cleanup && p.mmid == mmid) { p.destroyed = true; p.changed = GetTickCount64(); }
}
void abandon() noexcept {
    {
        auto& p = placement(); std::lock_guard lock(p.mutex);
        // The level's own teardown removes its queues. Never destroy an old MMID in a new
        // world, where the native coordinator can reuse it.
        if (p.step != Step::idle) {
            if (p.created) release_native_flag_placeholder();
            retire(p); p.status = "Flag placement ended because the map changed.";
        }
        p.pending.reset(); p.custom_mmids.clear();
    }
    flush();
}
void tick(const menu_data::Context& c) {
    const auto now = GetTickCount64();
    const auto world = current_world();
    auto& p = placement();
    std::string owner;
    {
        std::lock_guard lock(p.mutex);
        if (p.step == Step::idle) return;
        owner = p.mode;
    }
    // The owner's documents decide when its match started and ended (copies; read unlocked).
    std::optional<kit::Documents> docs;
    try { docs = kit::documents_for(owner); } catch (...) {}
    FlushOnExit flush_notes;
    std::string command; Step next{}; std::uint32_t destroy{};
    bool release{};
    {
        std::lock_guard lock(p.mutex);
        if (p.step == Step::idle || p.mode != owner) return;
        // The same native setup survives connecting to a session. Offline level IDs can change
        // to transport IDs before the flag is placed; keep ownership and bind the eventual spawn
        // to that session. Real level teardown calls abandon before unloading.
        if (p.native_entry && !p.handed_off && !p.spawn && p.step != Step::cleanup) {
            if (world) p.world = world;
            else if (now < p.until && p.step != Step::open) return; // setup can open before capture initializes
        } else if (world != p.world || !world) {
            if (p.created) release_native_flag_placeholder();
            logging::log(logging::Level::warning, logging::Channel::ui,
                "Native flag: session changed during step {}, world {} -> {}.", static_cast<unsigned>(p.step), p.world, world);
            retire(p); p.pending.reset(); p.status = "Flag placement ended because the session changed.";
            return;
        }
        if (p.step == Step::flag && p.flag_seen) { p.step = Step::placing; p.until = now + 300000; p.status = "Place the starting flag."; }
        if (p.step == Step::waiting) {
            // Keep the native registration alive: it owns the world flag, waiting notification,
            // and Start/Leave input prompts.
            if (!p.handed_off && p.spawn) {
                const auto& at = *p.spawn;
                note(p, "flag_placed", {std::to_string(p.game), std::format("{}", at[0]), std::format("{}", at[1]),
                                        std::format("{}", at[2]), std::format("{}", facing_yaw(p.facing))});
                p.handed_off = true; p.status.clear(); release = true;
            }
            // The registration lives while its plugin publishes the waiting card or a HUD: an editor
            // over the placed flag leaves the card out for a while and shows its own HUD instead.
            const bool waiting = docs && shows_waiting(*docs, p.mode);
            const bool playing = docs && docs->hud.active;
            const bool editing = playing && docs->hud.editor.present;
            if (waiting || playing) p.match_seen = true;
            // The waiting card gave way to the match HUD (not an editor's): the lobby flag goes, the
            // match stays.
            if (p.handed_off && p.match_seen && p.mmid && !p.destroy_queued && playing && !editing && !waiting) destroy = p.mmid;
            if ((p.match_seen && !waiting && !playing) || (p.handed_off && !p.match_seen && now >= p.until)) to_cleanup(p, now);
        }
        if (p.step == Step::setup && p.setup_seen && !p.show_setup && p.native_entry && now >= p.until) {
            // Back already performed the native navigation. Do not send an exit that could
            // close the next stock challenge's setup.
            release = p.created;
            note(p, "setup_back", {std::to_string(p.game)});
            retire(p); p.status.clear();
            if (release) release_native_flag_placeholder();
            return;
        }
        if ((p.step == Step::open || p.step == Step::setup) && !p.created && !p.setup_seen && now >= p.until) {
            logging::log(logging::Level::warning, logging::Channel::ui, "Native flag: the setup for {} did not open in 15 s (step {}).", p.mode,
                         static_cast<unsigned>(p.step));
            retire(p); p.status = "The setup did not open. Select the mode to try again.";
            return; // no native queue was created; do not close another menu
        }
        if (now >= p.until && p.step != Step::cleanup && p.step != Step::waiting) {
            p.spawn.reset(); p.status = "Flag placement timed out. Open the mode to try again.";
            to_cleanup(p, now);
        }
        if (p.step == Step::open) { command = "SpotBattle"; next = Step::setup; }
        // Native Confirm owns this transition for a setup page. OnEnd also fires on Back, so it
        // must never become an automatic placement.
        else if (p.step == Step::setup && p.created && p.setup_seen && !p.show_setup && !p.native_entry) { command = "ThrowdownPlacement"; next = Step::flag; }
        else if (p.step == Step::cleanup) {
            if (!p.exited && !p.sent) { command = "ThrowdownerExit"; next = Step::cleanup; }
            if (p.mmid && !p.destroy_queued) destroy = p.mmid;
            if ((p.exited && !p.mmid) || (p.destroyed && now >= p.changed + 500)) {
                release = p.created;
                const auto requested = std::move(p.pending);
                retire(p); p.pending.reset(); p.status.clear();
                if (requested && requested->world == world) open_request(p, *requested, now);
            } else if (now >= p.until) {
                p.status = "Could not close the flag placement. Back out of the native Throwdown before retrying.";
            }
        }
    }
    if (release) release_native_flag_placeholder();
    if (destroy && prepare_throwdown_injection() && queue_throwdown_destroy(destroy)) {
        std::lock_guard lock(p.mutex);
        if (p.mmid == destroy) p.destroy_queued = true;
    }
    if (!command.empty()) {
        // Publication can run expression callbacks. Arm the expected step before publishing,
        // without holding this mutex across native code.
        Step previous;
        { std::lock_guard lock(p.mutex); previous = p.step; p.step = next; }
        bool sent{};
        try { sent = navigate(c, command); }
        catch (...) { std::lock_guard lock(p.mutex); p.step = previous; throw; }
        std::lock_guard lock(p.mutex);
        if (!sent) p.step = previous;
        else { p.sent = now; if (next != Step::cleanup) p.until = now + 15000; }
    }
}
} // namespace dingosdk::multiplayer::native_flag

// ---- kit::flag (throwdown_kit.h) -------------------------------------------------------------------
namespace dingosdk::throwdowns::kit::flag {
bool begin(std::string_view mode, std::uint32_t game, const Options& options) {
    using namespace multiplayer::native_flag;
    if (mode.empty() || !game || options.players < 1 || options.players > 10 || options.seconds < 1 || options.seconds > 3600 ||
        !std::isfinite(options.cursor_height) || options.cursor_height < 0 || options.cursor_height > 100000) return false;
    const auto world = current_world();
    if (!world) return false;
    auto& p = placement();
    bool accepted{};
    {
        std::lock_guard lock(p.mutex);
        const auto now = GetTickCount64();
        Request request{std::string(mode), game, options, world};
        if (!options.setup_page && p.step == Step::waiting && p.mode == mode && p.game == game && p.world == world &&
            p.handed_off && p.mmid && !p.destroy_queued) {
            // Re-placing this mode's start flag needs a new native picker after the old
            // registration is destroyed. Never reuse its MMID.
            p.pending = std::move(request); p.owner_left = true; to_cleanup(p, now);
            accepted = true;
        } else if (p.step == Step::cleanup) {
            p.pending = std::move(request); // opens once the old flag is down
            accepted = true;
        } else if (p.step == Step::idle ||
                   ((p.step == Step::open || p.step == Step::setup) && p.native_entry && !p.flag_seen && !p.mmid)) {
            // A fresh card can replace an abandoned setup even when the state machine omitted
            // OnEnd. Never replace an actual placed flag.
            if (p.step != Step::idle) {
                if (p.created) multiplayer::release_native_flag_placeholder();
                retire(p);
            }
            open_request(p, request, now);
            accepted = true;
        }
    }
    flush();
    if (accepted) multiplayer::prepare_throwdown_injection(); // prewarm cleanup before the player places the flag
    return accepted;
}
State state() noexcept {
    using namespace multiplayer::native_flag;
    auto& p = placement(); std::lock_guard lock(p.mutex);
    State s;
    s.busy = p.step != Step::idle;
    s.owned = s.busy && (p.step != Step::waiting || p.mmid != 0);
    s.waiting = waiting_card(p);
    s.placing = p.step == Step::flag || p.step == Step::placing || p.step == Step::closing;
    try { s.mode = p.mode; } catch (...) {}
    s.game = p.game;
    return s;
}
void leave() noexcept {
    using namespace multiplayer::native_flag;
    auto& p = placement(); std::lock_guard lock(p.mutex);
    p.pending.reset();
    if (p.step == Step::idle || leave_unopened(p)) return;
    p.owner_left = true;
    to_cleanup(p, GetTickCount64());
}
bool adopt(std::string_view mode, std::uint32_t game, std::uint32_t mmid, const Vec3& at, float yaw, unsigned seconds, unsigned capacity) {
    using namespace multiplayer::native_flag;
    try {
        const auto world = current_world();
        if (mode.empty() || !world || !game || !mmid || multiplayer::local_throwdown_event_active() || !std::isfinite(yaw) ||
            !std::all_of(at.begin(), at.end(), [](float n) { return std::isfinite(n) && std::abs(n) < 100000.f; })) return false;
        auto& p = placement(); std::lock_guard lock(p.mutex);
        if (p.step != Step::idle) return p.step == Step::waiting && p.world == world && p.mode == mode && p.game == game && p.mmid == mmid;
        reset(p);
        p.step = Step::waiting; p.mode = std::string(mode); p.world = world; p.game = game; p.mmid = mmid;
        p.spawn = at; p.facing = yaw_facing(yaw);
        p.seconds = std::clamp(seconds, 1U, 3600U); p.players = std::clamp(capacity, 1U, 10U);
        // The guest's plugin hears join, not flag_placed, and shows its waiting card from then.
        p.handed_off = p.exited = p.adopted = true; p.match_seen = false;
        p.changed = GetTickCount64(); p.until = p.changed + 15000; p.status.clear();
        if (std::find(p.custom_mmids.begin(), p.custom_mmids.end(), mmid) == p.custom_mmids.end()) p.custom_mmids.push_back(mmid);
        multiplayer::release_native_flag_placeholder(); // only this donor queue's activity-presence lease; no actual event exists
        return true;
    } catch (...) { return false; }
}
} // namespace dingosdk::throwdowns::kit::flag
