#include "plugin_host.h"
#include <Windows.h>
#include <mmsystem.h>
#include "reskate_plugin.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/native_skater.h"
#include "Engine/Vfs/initfs.h"
#include "Extension/Multiplayer/Remote/board_debris.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Trainer/trainer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace dingosdk::plugins {
namespace {
using logging::Channel;
using logging::Level;
namespace mp = multiplayer;

struct Plugin {
    std::wstring name;
    HMODULE module{};
    ReSkatePluginTick tick{};
    bool dead{};
};
struct Slot {
    mp::BoardDebris piece;
    std::size_t owner{};
};
// Where the plugin API is allowed to act: only while the host is calling a plugin on the client tick.
struct Context {
    bool active{};
    std::uintptr_t base{}, client{}, skater{};
    std::size_t plugin{};
};
struct State {
    bool loaded{};
    std::filesystem::path root;
    std::vector<Plugin> plugins;
    Context context;
    std::array<Slot, RESKATE_MAX_PIECES> slots;
    // the board hidden through the API
    std::uintptr_t hidden_holder{}, hidden_identity{}, hidden_owner{};
    std::array<float, 3> last_position{};
    bool have_position{};
    std::uint64_t bail_until{};
};
// Client tick only.
State& state() {
    static auto* value = new State;
    return *value;
}

// Small guarded helpers; no C++ objects live in these frames, so __try is allowed.
bool safe_read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x1000 || address > 0x00007fffffffffffULL - size) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_enable_board(std::uintptr_t base, std::uintptr_t holder, std::uint8_t on) noexcept {
    using EnableBoard = void (*)(std::uintptr_t, std::uint8_t);
    __try { reinterpret_cast<EnableBoard>(base + addr::native_skater::enable_board_resources)(holder, on); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool holder_is_valid(std::uintptr_t base, std::uintptr_t holder) noexcept {
    std::uintptr_t vtable{};
    return holder && safe_read(holder, &vtable, sizeof(vtable)) && vtable == base + addr::engine::board_holder_vtable;
}
// Calls into plugin code. A fault is caught here and reported; the caller disables the plugin.
bool guarded_init(ReSkatePluginInit fn, const ReSkatePluginApi* api, std::int32_t* result) noexcept {
    __try { *result = fn(api); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool guarded_void(void (*fn)()) noexcept {
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool guarded_version(ReSkatePluginApiVersion fn, std::uint32_t* result) noexcept {
    __try { *result = fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

mp::Transform to_native(const ReSkateTransform& t) {
    mp::Transform out;
    for (std::size_t i = 0; i < 3; ++i) { out.position[i] = t.position[i]; out.scale[i] = t.scale[i]; }
    for (std::size_t i = 0; i < 4; ++i) out.rotation[i] = t.rotation[i];
    return out;
}
void from_native(const mp::Transform& t, ReSkateTransform& out) {
    for (std::size_t i = 0; i < 3; ++i) { out.position[i] = t.position[i]; out.scale[i] = t.scale[i]; }
    for (std::size_t i = 0; i < 4; ++i) out.rotation[i] = t.rotation[i];
}
bool finite(const ReSkateTransform& t) {
    for (const float v : t.position) if (!std::isfinite(v)) return false;
    for (const float v : t.rotation) if (!std::isfinite(v)) return false;
    for (const float v : t.scale) if (!std::isfinite(v)) return false;
    return true;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
std::wstring from_utf8(const char* text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}
void destroy_pieces_of(State& s, std::size_t owner, std::uintptr_t base) {
    for (auto& slot : s.slots)
        if (slot.piece && slot.owner == owner) { mp::debris_destroy(base, slot.piece); slot = {}; }
}
void restore_board(State& s, std::uintptr_t base, const char* why) {
    if (!s.hidden_holder) return;
    const auto holder = s.hidden_holder;
    s.hidden_holder = s.hidden_identity = 0;
    if (!holder_is_valid(base, holder)) return;
    const bool called = call_enable_board(base, holder, 1);
    logging::log(Level::info, Channel::skater, "Plugins: board restored ({}); call {}.", why, called ? "ok" : "faulted");
}
void disable(State& s, std::size_t index, std::uintptr_t base, const char* why) {
    auto& plugin = s.plugins[index];
    if (plugin.dead) return;
    plugin.dead = true;
    logging::log(Level::warning, Channel::skater, "Plugins: {} disabled ({}).", to_utf8(plugin.name), why);
    destroy_pieces_of(s, index, base);
    if (s.hidden_owner == index + 1) restore_board(s, base, "plugin disabled");
}

// ---- API implementations. They only act inside a plugin call on the client tick. ----
Context* active() {
    auto& c = state().context;
    return c.active ? &c : nullptr;
}
void api_log(std::int32_t level, const char* text) {
    if (!text) return;
    const auto l = level <= 0 ? Level::debug : level == 1 ? Level::info : level == 2 ? Level::warning : Level::error;
    logging::log(l, Channel::skater, "Plugin: {}", text);
}
std::size_t api_game_directory(char* buffer, std::size_t capacity) {
    const auto text = to_utf8(state().root.wstring());
    const std::size_t length = text.size();
    if (buffer && capacity) {
        const std::size_t n = std::min(length, capacity - 1);
        std::memcpy(buffer, text.data(), n);
        buffer[n] = 0;
    }
    return length;
}
std::int32_t api_skater(ReSkateSkater* out) {
    if (!out) return 0;
    *out = {};
    if (!active()) return 0;
    const auto t = trainer::telemetry();
    if (!t.skater) return 0;
    out->on_map = 1;
    for (std::size_t i = 0; i < 3; ++i) out->position[i] = t.position[i];
    out->physics_state = t.physics_state;
    return 1;
}
std::int32_t api_next_landing(std::uint64_t* cursor, ReSkateLanding* out) {
    if (!cursor || !out || !active()) return 0;
    const auto jump = trainer::telemetry().last;
    if (jump.serial == 0 || jump.serial == *cursor) return 0;
    *cursor = jump.serial;
    out->serial = jump.serial;
    out->fall_height = std::max(0.0f, jump.height + jump.drop);
    out->speed = jump.landing_speed;
    for (std::size_t i = 0; i < 3; ++i) out->position[i] = jump.landing[i];
    return 1;
}
std::int32_t api_force_bail() {
    const auto* c = active();
    return c && c->skater && force_bail(c->client, c->skater) ? 1 : 0;
}
std::int32_t api_board_pose(ReSkateTransform* root, ReSkateTransform* bones, std::size_t capacity, std::size_t* count) {
    const auto* c = active();
    if (!c || !c->skater || !root) return 0;
    const auto frame = mp::capture_local(c->base, c->client, true);
    if (!frame.ready || frame.pose.board.empty()) return 0;
    from_native(frame.pose.board.front(), *root);
    const std::size_t have = frame.pose.board.size() - 1;
    if (count) *count = have;
    if (bones)
        for (std::size_t i = 0; i < std::min(have, capacity); ++i) from_native(frame.pose.board[i + 1], bones[i]);
    return 1;
}
std::int32_t api_board_set_visible(std::int32_t visible) {
    auto& s = state();
    const auto* c = active();
    if (!c || !c->skater) return 0;
    if (visible) {
        if (s.hidden_holder && s.hidden_owner == c->plugin + 1) restore_board(s, c->base, "plugin request");
        return 1;
    }
    if (s.hidden_holder) return 1;
    try {
        const auto board = mp::read_native_board(safe_read, c->base, c->skater);
        if (!board.entity || !board.holder) return 0;
        const auto visual = mp::read_native_board_visual(safe_read, c->base, board.entity);
        if (!visual.initialized) return 0;
        if (!call_enable_board(c->base, visual.holder, 0)) return 0;
        s.hidden_holder = visual.holder;
        s.hidden_identity = c->skater;
        s.hidden_owner = c->plugin + 1;
        return 1;
    } catch (const std::exception&) { return 0; }
}
Slot* slot_for(std::uint32_t handle) {
    auto& s = state();
    const auto* c = active();
    if (!c || handle == 0 || handle > s.slots.size()) return nullptr;
    auto& slot = s.slots[handle - 1];
    return slot.piece && slot.owner == c->plugin ? &slot : nullptr;
}
std::uint32_t api_piece_create(const ReSkateTransform* at) {
    auto& s = state();
    const auto* c = active();
    if (!c || !at || !finite(*at)) return 0;
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        if (s.slots[i].piece) continue;
        std::string why;
        if (!mp::debris_create(c->base, c->client, to_native(*at), s.slots[i].piece, why)) {
            logging::log(Level::warning, Channel::skater, "Plugins: could not create a piece: {}", why);
            return 0;
        }
        s.slots[i].owner = c->plugin;
        return static_cast<std::uint32_t>(i + 1);
    }
    return 0;
}
std::int32_t api_piece_place(std::uint32_t handle, const ReSkateTransform* at) {
    auto* slot = slot_for(handle);
    if (!slot || !at || !finite(*at)) return 0;
    std::string why;
    if (!mp::debris_place(state().context.base, slot->piece, to_native(*at), why)) { *slot = {}; return 0; }
    return 1;
}
std::int32_t api_piece_set_bones(std::uint32_t handle, const ReSkateTransform* bones, std::size_t count) {
    auto* slot = slot_for(handle);
    if (!slot || !bones || count == 0 || count > 64) return 0;
    const auto holder = mp::debris_holder(slot->piece);
    if (!holder) return 0;
    std::vector<mp::Transform> pose;
    pose.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!finite(bones[i])) return 0;
        pose.push_back(to_native(bones[i]));
    }
    mp::debris_set_pose(holder, pose);
    return 1;
}
void api_piece_destroy(std::uint32_t handle) {
    auto* slot = slot_for(handle);
    if (!slot) return;
    mp::debris_destroy(state().context.base, slot->piece);
    *slot = {};
}
std::int32_t api_play_sound(const char* path_utf8) {
    if (!active() || !path_utf8 || !*path_utf8) return 0;
    std::filesystem::path path = from_utf8(path_utf8);
    if (path.is_relative()) path = state().root / path;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return 0;
    return PlaySoundW(path.c_str(), nullptr, SND_ASYNC | SND_FILENAME | SND_NODEFAULT) ? 1 : 0;
}
std::int32_t api_hud_bar(const char* id, const char* label, float fraction, std::uint32_t rgb) {
    if (!active() || !id || !*id || !std::isfinite(fraction)) return 0;
    overlay::set_plugin_hud_bar(id, label ? label : "", fraction, rgb);
    return 1;
}
void api_hud_clear(const char* id) {
    if (active() && id) overlay::clear_plugin_hud_bar(id);
}

const ReSkatePluginApi& api() {
    static const ReSkatePluginApi value = {
        sizeof(ReSkatePluginApi), RESKATE_PLUGIN_API_VERSION, &api_log, &api_game_directory, &api_skater,
        &api_next_landing, &api_force_bail, &api_board_pose, &api_board_set_visible, &api_piece_create,
        &api_piece_place, &api_piece_set_bones, &api_piece_destroy, &api_play_sound, &api_hud_bar,
        &api_hud_clear};
    return value;
}

void load_plugins(State& s, std::uintptr_t base) {
    s.loaded = true;
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return;
    std::error_code error;
    s.root = std::filesystem::canonical(std::filesystem::path(std::wstring(path.data(), length)), error).parent_path();
    if (error || !initfs::loose_files_session_enabled(s.root)) return;
    const auto folder = s.root / L"Plugins";
    if (!std::filesystem::is_directory(folder, error)) return;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error) && it->path().extension() == L".dll") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const auto name = to_utf8(file.filename().wstring());
        const HMODULE module = LoadLibraryW(file.c_str());
        if (!module) {
            logging::log(Level::warning, Channel::skater, "Plugins: {} could not be loaded (error {}).", name, GetLastError());
            continue;
        }
        const auto init = reinterpret_cast<ReSkatePluginInit>(GetProcAddress(module, "reskate_plugin_init"));
        if (!init) {
            logging::log(Level::warning, Channel::skater, "Plugins: {} has no reskate_plugin_init; skipped.", name);
            FreeLibrary(module);
            continue;
        }
        if (const auto version_fn = reinterpret_cast<ReSkatePluginApiVersion>(GetProcAddress(module, "reskate_plugin_api_version"))) {
            std::uint32_t wanted{};
            if (!guarded_version(version_fn, &wanted) || wanted == 0 || wanted > RESKATE_PLUGIN_API_VERSION) {
                logging::log(Level::warning, Channel::skater, "Plugins: {} needs plugin API {} but this game has {}; skipped.",
                    name, wanted, RESKATE_PLUGIN_API_VERSION);
                FreeLibrary(module);
                continue;
            }
        }
        s.plugins.push_back({file.filename().wstring(), module,
            reinterpret_cast<ReSkatePluginTick>(GetProcAddress(module, "reskate_plugin_tick")), false});
        const auto index = s.plugins.size() - 1;
        s.context = {true, base, 0, 0, index};
        std::int32_t ok{};
        const bool ran = guarded_init(init, &api(), &ok);
        s.context.active = false;
        if (!ran || !ok) {
            disable(s, index, base, ran ? "init failed" : "init faulted");
        } else {
            logging::log(Level::info, Channel::skater, "Plugins: {} loaded.", name);
        }
    }
}

// Brings the board back when the skater or board is gone, or the player respawned or was moved far.
void watch_hidden_board(State& s, std::uintptr_t base, std::uintptr_t skater) {
    if (!s.hidden_holder) return;
    const auto t = trainer::telemetry();
    const float step = s.have_position
        ? std::hypot(std::hypot(t.position[0] - s.last_position[0], t.position[1] - s.last_position[1]),
              t.position[2] - s.last_position[2])
        : 0.0f;
    bool same = false;
    try {
        const auto board = mp::read_native_board(safe_read, base, skater);
        same = skater == s.hidden_identity && board.holder == s.hidden_holder;
    } catch (...) {}
    if (!same) {
        logging::log(Level::info, Channel::skater, "Plugins: the skater or board changed; forgetting the hidden board.");
        s.hidden_holder = s.hidden_identity = 0;
    } else if (!t.skater || step > 12.0f) {
        restore_board(s, base, "respawn or teleport");
    } else {
        // the game switches the board's resources back on by itself at times: keep it off
        std::uint8_t flag{};
        if (safe_read(s.hidden_holder + 0x94, &flag, 1) && flag == 1) (void)call_enable_board(base, s.hidden_holder, 0);
    }
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, std::uintptr_t skater) noexcept {
    try {
        auto& s = state();
        if (!s.loaded) load_plugins(s, base);
        if (s.plugins.empty()) return;
        const auto t = trainer::telemetry();
        if (skater) {
            watch_impacts(client, skater);
            ImpactEvent ignored;
            while (take_impact(ignored)) {}
            watch_hidden_board(s, base, skater);
        } else {
            restore_board(s, base, "no skater");
        }
        s.last_position = t.position;
        s.have_position = t.skater;
        for (std::size_t i = 0; i < s.plugins.size(); ++i) {
            auto& plugin = s.plugins[i];
            if (plugin.dead || !plugin.tick) continue;
            s.context = {true, base, client, skater, i};
            const bool ran = guarded_void(plugin.tick);
            s.context.active = false;
            if (!ran) disable(s, i, base, "tick faulted");
        }
        s.context = {};
    } catch (const std::exception& error) {
        logging::log(Level::warning, Channel::skater, "Plugins: tick failed: {}", error.what());
    } catch (...) {}
}
} // namespace dingosdk::plugins
