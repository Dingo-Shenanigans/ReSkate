#include "anim_studio.h"
#include "native_asset_names.h"
#include "pop_grades.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Core/Log/logging.h"
#include "reskate_version.h"
#include "anim_research.h"
#include "anim_runtime_rules.h"
#include "anim_playback.h"
#include "Engine/Core/Profiling/profiler.h"
#include "anim_diagnostics.h"
#include "skater_mirror.h"
#include "miniz.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_lead_foot.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/ai_skaters.h"
#include "Extension/Skater/xray_skeleton.h"
#include "Extension/Profile/runtime_internal.h"
#include "native_library.h"
#include "marketplace.h"
#include "pack_file_transaction.h"
#include <Windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <limits>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <random>
#include <thread>
#include <set>
#include <sstream>
#include <vector>

namespace dingosdk::anim_studio {
namespace {
using logging::Channel;
using logging::Level;
struct DiagnosticHistory { std::mutex mutex; DiagnosticBuffer buffer; };
DiagnosticHistory& diagnostic_history() { static auto* history = new DiagnosticHistory; return *history; }
DiagnosticHistory& preview_history() { static auto* history = new DiagnosticHistory; return *history; }

// ---- guarded memory --------------------------------------------------------------------------------
constexpr std::uintptr_t highest_user = 0x00007fffffffffffULL;
bool peek(std::uintptr_t address, void* out, std::size_t size) noexcept {
    if (address < 0x10000 || address > highest_user - size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool poke(std::uintptr_t address, const void* data, std::size_t size) noexcept {
    if (address < 0x10000 || address > highest_user - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> std::optional<T> read(std::uintptr_t address) noexcept {
    T value{};
    if (!peek(address, &value, sizeof(T))) return std::nullopt;
    return value;
}
struct alignas(16) NativeBone {
    std::array<float, 4> scale, rotation, position;
};
static_assert(sizeof(NativeBone) == 0x30);

double now_seconds() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / frequency;
}

// ---- shared state --------------------------------------------------------------------------------
// A saved trick the game plays in place of its own: matched by the game's name for the trick.
struct CustomTrick {
    std::string trick; // the game's name ("ollie", "kickflip"), lower case
    std::string name;  // the saved animation's
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Animation> edits;
    bool goofy{};      // recorded goofy
    bool leads_right{}; // the foot it leads with, read from the clip itself (clip_leads_right), not its label
    std::optional<bool> fakie; // made rolling fakie (empty: from before that was kept)
    // The clips the game may play for its trick (its own and the library's pops of it), for NativeClock.
    std::shared_ptr<const NativeModels> native;
    int order{-1}; // its place in the playlist, when the playlist plays (In turns goes by it)
    // A trick as made for one height (low, mid, high): what COPY TO LOW, MID AND HIGH would make of it, kept in memory
    // only. "" the trick itself.
    std::string height;
    bool any_pop{};  // the trick itself on ANY pop: it plays when the pop is not known yet too
    // A version for a pop the trick is not set to (it is set to one pop): it plays there only when nothing of yours is
    // for that pop - as tricks played on the pops they had no version for until 2026-10-25, but as the copy would.
    bool fallback{};
    // The timeline's catch (find_catch): the board caught, the feet back on the deck. -1 none. Slowed over a longer air.
    float caught{-1};
};
// The game's own stance setting (Options, DefaultStanceIsGoofy), kept for the animation thread.
std::atomic<bool> game_goofy{};
// On the client thread, again whenever the profile changes: the player can change it in the game's options.
void read_game_stance() {
    static std::uint64_t read_at = UINT64_MAX;
    const auto changes = profile::Store::changes();
    if (changes == read_at) return;
    const auto& store = profile_runtime::local_runtime().store;
    if (!store) return;
    const bool goofy = store->bool_option("DefaultStanceIsGoofy").value_or(false);
    const bool was_goofy = game_goofy.exchange(goofy, std::memory_order_acq_rel);
    if (read_at == UINT64_MAX || was_goofy != goofy)
        logging::log(Level::info, Channel::skater, "Animation Studio: the game's stance is {}.", goofy ? "goofy" : "regular");
    read_at = changes;
}
// The skater's stance: the game's own setting, the one the game mirrors him by. It was the grind exit setting, with
// not set read as regular: a goofy rider who never set it had every trick recorded as switch and shown under SWITCH.
bool goofy_stance() noexcept { return game_goofy.load(std::memory_order_acquire); }
// The stance a trick was authored in: which foot led on the body its keys were made against. That is what
// decides whether it is played as it is or mirrored, and the studio's picture of it has to agree, or a
// trick is shown one way round and played the other. A recording says so in its label, taken from the
// game's stance setting as it happened. The game's own clips are labelled regular but stand goofy - his
// right foot at the nose, seen on screen and confirmed with the stance switch on 2026-10-21 - so a trick
// made on one of those was authored goofy, whatever its label says.
bool authored_goofy(const Animation& animation) noexcept {
    return animation.source.kind == "native_asset" || animation.stance == "goofy";
}
struct State;
// The way round the studio shows him: the game's own stance, or the one chosen in the studio. This is for
// the studio's picture only - what was recorded and what is played go by the game's stance setting
// (goofy_stance), so the two sides of a match are worked out the same way.
bool studio_goofy(int choice) noexcept { return choice == 0 ? goofy_stance() : choice == 2; }
// The game's own bit: whether his left foot is forward right now. It is the one thing about stance the
// game tells us directly; switch is worked out from it and the stance setting.
std::optional<bool> left_foot_leads(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    if (!core || read<std::uintptr_t>(core).value_or(0) != base + addr::no_bail::bail_core_vtable) return std::nullopt;
    const auto context = read<std::uintptr_t>(core + 0x3c0).value_or(0);
    const auto flags = context ? read<std::uint8_t>(context + addr::skater_lead_foot::stance_flags_offset) : std::nullopt;
    return flags ? std::optional<bool>{(*flags & addr::skater_lead_foot::left_foot_leads_mask) != 0} : std::nullopt;
}
// The physics context the stance byte lives in (0 when it cannot be found).
std::uintptr_t physics_context(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    if (!core || read<std::uintptr_t>(core).value_or(0) != base + addr::no_bail::bail_core_vtable) return 0;
    return read<std::uintptr_t>(core + 0x3c0).value_or(0);
}
// The game's board-backwards flag (context+0x13c5 bit 0x20, found 2026-10-22): set exactly while the deck is
// turned round under the skater, after a shuvit or a push out of fakie into switch.
bool board_backwards(std::uintptr_t base, std::uintptr_t component) noexcept;
// The whole byte the stance bit lives in: the other bits are not understood yet (the stance readout logs them).
std::optional<std::uint8_t> stance_flags(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    if (!core || read<std::uintptr_t>(core).value_or(0) != base + addr::no_bail::bail_core_vtable) return std::nullopt;
    const auto context = read<std::uintptr_t>(core + 0x3c0).value_or(0);
    return context ? read<std::uint8_t>(context + addr::skater_lead_foot::stance_flags_offset) : std::nullopt;
}
// The skater's body-relative forward speed (context+0x17fc, found 2026-10-22): below zero rolling fakie.
std::optional<float> forward_speed(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto context = physics_context(base, component);
    const auto speed = context ? read<float>(context + 0x17fc) : std::nullopt;
    return speed && std::isfinite(*speed) ? speed : std::nullopt;
}
bool rolling_fakie(float riding_forward) noexcept { return riding_forward < -0.3f; }
bool board_backwards(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto context = physics_context(base, component);
    const auto flags = context ? read<std::uint8_t>(context + 0x13c5) : std::nullopt;
    return flags && (*flags & 0x20) != 0;
}
std::optional<bool> switch_stance(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto left = left_foot_leads(base, component);
    return left ? std::optional<bool>{riding_switch(*left, goofy_stance())} : std::nullopt;
}
using CustomTricks = std::vector<CustomTrick>;
// How a saved trick plays in game: the game leads. Its own pose says which clip it plays and how far into it
// it is (NativeClock, PieceTracker for pieces), and the keys ride on the game's pose at that time - its IK,
// its height, its board. Forcing the studio's whole pose over it (these were 1 until 2026-10-22) put the
// skater at the clip's own height, took his feet off the game's deck and fought the game's next animation.
// A deck turned round under him is taken from the game's own board-backwards flag. Joint limits is a
// setting (JOINTS): it holds in the studio and in game alike, so what is posed is what plays.
constexpr float match_bent_joints = 0; // a joint with a key: its key on the game's pose
constexpr float match_whole_body = 0;  // every other joint, and the board: the game's own
constexpr bool fit_keys_to_air = true; // the trick's own clock, when the game's clip for it is not found
// In the air the keys ride on the game's own pose and clock, as the 1.0.8 studio (2026-10-23) played them, which played
// tricks right: no catch latch (the trick slowed through its catch to fill a longer air), no keys held as made off the
// game's clip, no feet put on the deck or lifted onto it. Those came after it (2026-10-23 night to 2026-10-25) and the feet
// snapped and stuck to the board after the catch, the catch struggled: off until they are right. Rolling on the ground a
// foot is still never drawn into the deck or the ground.
bool air_corrections = false; // (not constexpr: the code it gates is kept compiled)
// Down a drop - the air longer than the trick's own by more than a flat pop's give - the trick still takes its own time
// from its catch to its landing on the touchdown (AirStretch, the catch latch), with the keys as made off the
// game's clip (off_clip), whatever air_corrections says: on the game's clock it played its landing in the air and stood
// still to the touchdown, or handed back to the game's pose at its catch. The pose as made goes back to the game's from
// this long before the touchdown, over this long (seconds), so he comes down in the game's landing with the keys on it.
constexpr float drop_pose_back_by = 0.05f, drop_pose_back_over = 0.4f;
// Why a trick lets go at its catch on a higher pop than it was made on (and over at least 0.3 s, whatever the crossfade).
constexpr const char* higher_pop_hand_back = "its catch, on a higher pop than it was made on: the game's own pose from here";
struct Snapshot {
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Clip> idle; // shown while no trick is open: its last frame, standing on the board
    std::shared_ptr<const Animation> edits; // tracks only; its clip is empty
    float time{};
    // A trick looked at (peek_game_trick): shown instead of the open one while it is set, played from peek_since.
    std::shared_ptr<const Clip> peek_clip;
    std::shared_ptr<const Animation> peek_edits;
    double peek_since{-1};
    bool open{}, play_in_game{}, room{};
    float speed{1};
    bool goofy{}; // the way round the studio is working
    bool grab{};       // a hand near the board closes on it
    bool front_foot{};  // tricks are matched by which foot leads, and mirrored when the other one does
    bool limits{true};  // keyed joints are kept inside the range the game's own animation uses
    float crossfade{0.3f}; // the Crossfade setting: seconds to hand back to the game's next move
    std::shared_ptr<const CustomTricks> tricks;
};
struct History {
    std::vector<Track> tracks;
    float start{}, end{-1};
    std::optional<float> catch_at; // CATCH, as it was
    std::optional<float> crossfade, catch_hold; // THIS TRICK, as they were
};
// The playlists (presets/playlists.json, read_playlists): any number, one on at a time.
struct NamedPlaylist {
    std::string name;
    PlaylistMode mode{PlaylistMode::fixed};
    std::vector<PlaylistEntry> entries;
};
struct Playlists {
    std::string on; // the one on; "" none
    std::vector<NamedPlaylist> lists;
    bool packs_moved{}; // the packs that were switched on are in a playlist (once: packs play only from playlists since 2026-10-25)
    const NamedPlaylist* named(const std::string& name) const {
        const auto found = std::find_if(lists.begin(), lists.end(), [&](const NamedPlaylist& l) { return l.name == name; });
        return found == lists.end() ? nullptr : &*found;
    }
    NamedPlaylist* named(const std::string& name) { return const_cast<NamedPlaylist*>(std::as_const(*this).named(name)); }
};
struct State {
    std::mutex mutex;
    // Document.
    std::shared_ptr<const Clip> clip;
    // The casual stance shown until a trick is picked: the end of the game's mid ollie, standing on the board.
    std::shared_ptr<const Clip> idle_clip;
    bool idle_read{};
    Animation edits;
    std::string saved_as; // the library name it was loaded from or saved to
    bool dirty{};
    bool previewing{}; // open to watch only (preview_saved)
    // What was open when a preview started (the trick, its changes, its undo and redo), put back when the preview ends.
    struct Kept {
        std::shared_ptr<const Clip> clip;
        Animation edits;
        std::deque<History> undo, redo;
        std::string saved_as;
        float time{};
        bool dirty{};
    };
    std::optional<Kept> kept;
    std::shared_ptr<const Clip> peek_clip;          // a trick looked at from TRICKS (peek_game_trick)
    std::shared_ptr<const Animation> peek_edits;
    double peek_since{-1};
    std::deque<History> undo, redo;
    std::vector<std::pair<std::uint16_t, Offset>> clipboard;
    // Playback.
    float time{};
    bool playing{}, play_in_game{true}, room{true};
    bool solo{}; // a turn moves that joint alone: the joints below it are turned back by as much
    std::uint64_t clip_serial{}; // how many clips have been opened: the overlay starts clean on each
    // CATCH: the studio's own catch for the open trick (guess_catch), and what it was worked out from.
    float catch_guess{-1};
    std::uint64_t catch_guess_from{};
    std::chrono::steady_clock::time_point catch_guessed_at{};
    // DROP PREVIEW (THIS TRICK): metres (0 off), and while it plays the seconds off the drop and the time last shown.
    float drop_preview{};
    float drop_real{-1}, drop_shown{-1};
    // The studio's stance (0 as the game has him, 1 regular, 2 goofy) and the clip turned the other way
    // round, kept so it is mirrored once rather than every frame.
    int stance_choice{};
    bool showing_goofy{};
    bool grab_board{};
    bool front_foot{true};
    bool limits{true}; // Joint limits (JOINTS): keyed joints stay inside the range the game's own animation uses
    float crossfade{0.3f}; // Crossfade (SETTINGS > In game), saved
    // The ground he stands or rides on, from the live pose: no camera is allowed under it.
    float floor{};
    bool floor_valid{};
    // The camera travels with the skater for a set shot; the free camera stays put and he moves through it.
    bool camera_follows{true};
    Vec3 held_centre{};
    bool held_centre_valid{};
    std::shared_ptr<const CustomTricks> tricks;
    bool tricks_read{};
    // Mode.
    bool open{}, open_requested{}, open_request_pending{}, confirm_close{};
    std::string status, failure;
    // Published by the animation thread.
    std::vector<JointView> joints;
    Joint joints_placement{}; // the placement the joints were measured from (the view's frame)
    // The board without its keys, now, from its start (start_frame): what the board's fields add to.
    Joint board_base;
    bool board_base_valid{};
    Vec3 centre{};
    Quat facing{0, 0, 0, 1};
    bool centre_valid{};
    double pose_seen_at{};
    // Camera.
    Orbit orbit;
    Vec3 focus_offset{}; // eased towards the focused joint
    Vec3 eased_centre{};
    bool eased_valid{};
    // The skater's facing when the studio opened: the views (front, side...) are from it, and the
    // camera does not swing when the skater turns on the spot meanwhile.
    float base_yaw{};
    bool base_yaw_valid{};
    std::optional<std::uint16_t> focus;
    float view_shift{}, view_shift_x{};
    std::array<float, 16> camera{};
    float fov{};
    bool camera_valid{};
    std::uint64_t revision{1};
    std::vector<std::string> library;
    std::vector<Preset> presets;
    std::vector<PresetPack> packs;                 // every pack in ReSkate.Tricks/presets, as saved
    // Each pack's picture (presets/<pack>.png): its file time, absent when it has none. Read once when the packs are
    // read or a picture changes, not on every frame's view().
    std::map<std::string, std::int64_t> pack_pictures;
    bool pack_pictures_read{};
    std::map<std::string, std::int64_t> pack_times; // each pack's file: when it was last written (read with the packs)
    std::set<std::string> ungrouped;                // MY TRICKS: shown on their own, not with the others of their trick
    std::vector<PresetPack> packs_on;              // the ones switched on
    std::map<std::string, PresetPack> pack_drafts; // packs changed in PACKS and not saved yet
    std::string open_pack;                         // the pack open in PACKS
    std::string your_name;                         // MADE BY, remembered for new recordings and packs
    std::vector<std::string> favorite_packs;       // starred in PACKS
    bool hover_preview{true};                      // TRICKS: a hovered trick plays in a little window
    std::map<std::string, std::string> keys;       // the editor's rebound keys (View::keys)
    bool playlist_on{};                            // the playlist that plays (Playlists::on), as last read
    PlaylistMode playlist_mode{PlaylistMode::fixed};
    std::vector<PlaylistEntry> playlist;
    Playlists playlists;                           // every playlist (presets/playlists.json)
    std::string playlist_open;                     // the one PACKS > PLAYLIST shows
    std::vector<PackConflict> conflicts;           // tricks more than one pack that is on has (pack_conflicts)
    std::string export_folder, import_folder;      // where packs go out to and come in from, "" the defaults
    std::string test_trick;                        // Test in game: played in game until the studio opens again
    std::string test_returned;                     // back from testing it: the studio says something about it
    bool library_read{};
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const Animation> published_edits;
};
void load_settings(State& s);
State& state() {
    static auto* s = [] {
        auto* fresh = new State;
        load_settings(*fresh);
        return fresh;
    }();
    return *s;
}
// One transform after another (parent, then child in the parent's frame); uniform scale.
Joint compose(const Joint& parent, const Joint& child) noexcept {
    const float k = (parent.scale[0] + parent.scale[1] + parent.scale[2]) / 3.0f;
    const auto moved = rotate(parent.rotation, {child.position[0] * k, child.position[1] * k, child.position[2] * k});
    Joint out;
    out.position = {parent.position[0] + moved[0], parent.position[1] + moved[1], parent.position[2] + moved[2]};
    out.rotation = normalised(multiply(parent.rotation, child.rotation));
    const float c = (child.scale[0] + child.scale[1] + child.scale[2]) / 3.0f;
    out.scale = {k * c, k * c, k * c};
    return out;
}
// `world` seen from `parent`'s frame.
Joint relative(const Joint& parent, const Joint& world) noexcept {
    const float k = std::max((parent.scale[0] + parent.scale[1] + parent.scale[2]) / 3.0f, 1e-4f);
    const auto back = conjugate(parent.rotation);
    const auto d = rotate(back, {world.position[0] - parent.position[0], world.position[1] - parent.position[1], world.position[2] - parent.position[2]});
    Joint out;
    out.position = {d[0] / k, d[1] / k, d[2] / k};
    out.rotation = normalised(multiply(back, world.rotation));
    const float c = (world.scale[0] + world.scale[1] + world.scale[2]) / 3.0f;
    out.scale = {c / k, c / k, c / k};
    return out;
}
// The board's rig (board_break.cpp): 16 bones, 0 its reference, 1 its place in the world, 2 the deck's
// mount, which carries the deck, the trucks and the wheels. Moving bone 2 moves the whole board.
constexpr std::uint32_t board_bones = 16;
constexpr std::size_t board_mount = 2;
// The skater's Ground joint: on the floor he stands or rides on.
constexpr std::uint16_t ground_joint = 375;
// The joints the game hangs a hand's props on (RightHand_Attach, LeftHand_Attach).
constexpr std::uint16_t right_hand_attach = 50, left_hand_attach = 283;
// The studio's room: the character room of the game menu's Skaters page, under every map. The spot of
// the menu's skater, how far from it a rig counts as in the room, and the room's floor (measured
// 2026-10-10 on Stadium 1 and San Vansterdam, PR #230).
constexpr std::array<float, 3> room_spot{1.0f, -69.04f, -5.25f};
constexpr std::array<float, 3> room_reach{2.0f, 2.0f, 2.0f};
constexpr float room_floor = -69.99f;
// A turn about the up axis only: the way `joint` faces on the ground.
Quat ground_turn(const Joint& joint) noexcept {
    const auto along = rotate(joint.rotation, {1, 0, 0});
    const float angle = std::atan2(-along[2], along[0]);
    return {0, std::sin(angle * 0.5f), 0, std::cos(angle * 0.5f)};
}
Joint joint_of(const NativeBone& b) noexcept {
    Joint j;
    j.scale = {b.scale[0], b.scale[1], b.scale[2]};
    j.rotation = b.rotation;
    j.position = {b.position[0], b.position[1], b.position[2]};
    return j;
}
// The frame bone 2 hangs in, in the world.
Joint board_parent(const std::array<NativeBone, board_bones>& b) noexcept { return compose(joint_of(b[0]), joint_of(b[1])); }
// The trucks upright on the deck, as they are at rest (board_break_model.h), with their baseplates on the deck as
// Board Break keeps them. In the studio the game goes on steering its trucks from its own board, lying wherever it
// is, so the trucks of the board drawn in the studio turned about by themselves. Each truck keeps where the game
// has it (a wide deck's sit further out); only its lean and steer are taken off. Bones 4 and 8 the trucks, 5 and
// 9 their baseplates.
void hold_trucks_at_rest(std::array<NativeBone, board_bones>& b) noexcept {
    // Preview rest transforms only; Board Break itself now belongs to a plugin.
    struct TruckRest { Quat facing; Vec3 at, effector; };
    static constexpr TruckRest back{{-0.3641458f, -0.3641458f, -0.6061335f, 0.6061335f},
                                    {0.0005361f, -0.0183003f, -0.2344986f}, {0.0395021f, 0, 0}};
    static constexpr TruckRest front{{-0.6061674f, 0.6061674f, 0.3640894f, 0.3640894f},
                                     {0.0005361f, -0.0182917f, 0.2346680f}, {0.0395677f, 0, 0}};
    struct Truck { std::size_t truck, plate; const TruckRest* rest; };
    for (const Truck t : {Truck{4, 5, &back}, Truck{8, 9, &front}}) {
        const Quat facing = normalised(t.rest->facing);
        const Vec3 at{b[t.truck].position[0], b[t.truck].position[1], b[t.truck].position[2]};
        const Vec3 rest_at = t.rest->at;
        const auto effector = rotate(facing, t.rest->effector);
        const auto plate = rotate(conjugate(facing), Vec3{rest_at[0] + effector[0] - at[0], rest_at[1] + effector[1] - at[1],
                                                          rest_at[2] + effector[2] - at[2]});
        if (!finite(Joint{{1, 1, 1}, facing, plate})) continue;
        for (std::size_t k = 0; k < 4; ++k) b[t.truck].rotation[k] = facing[k];
        b[t.plate].rotation = {0, 0, 0, 1};
        for (std::size_t k = 0; k < 3; ++k) b[t.plate].position[k] = plate[k];
    }
}

// The animation thread's own, never touched elsewhere.
// Where a board touches the ground, in its mount's frame (board_shape): the tips of its nose and tail and the bottoms of its
// four wheels, and its up.
struct BoardShape {
    std::array<Vec3, 6> points{};
    Vec3 up{0, 1, 0}; // the board's up, in its mount's frame
};
struct Live {
    std::mutex animation_mutex;
    std::atomic<std::uintptr_t> base{}, component{}, board_holder{};
    std::atomic<std::uint64_t> generation{};
    std::atomic<std::uint64_t> animation_updates{};
    std::uint64_t seen_generation{~std::uint64_t{0}};
    std::vector<NativeBone> bones;
    std::vector<Joint> pose, sampled;
    // The board at takeoff, for the trick played in game.
    std::array<NativeBone, board_bones> board{};
    bool board_ok{};
    float predicted_air{}; // from it: how long this air lasts
    bool playback_goofy{};
    std::optional<bool> playback_switch;
    // In-game playback.
    PlaybackEpisode episode;
    MotionState last_motion{MotionState::unknown};
    std::atomic<MotionState> render_motion{MotionState::unknown};
    const CustomTrick* game_trick{}; // the saved trick playing now, from snapshot `game_tricks`
    // Where the game is in its own clip for that trick (NativeClock): the time the edits are laid on at.
    NativeClock native_clock;
    double clock_selected_at{-1}; // when the trick was chosen
    double clock_locked_at{-1};   // when the game's clip was found
    float clock_weight{};         // the layer, ramped in when found and out when the game moves on (eased when used)
    bool clock_fallback{};        // its clip was not found: the trick plays on its own clock instead
    // Off the game's clip in the air (a kicker's or a drop's longer air, where the game holds a pose of its own, or no clip
    // of the game's found): 0..1, eased in and out. The keys then go on the pose they were made on, the feet on the deck.
    float off_clip{};
    bool off_clip_said{};
    // On the game's clip for another pop than the trick was made on: 0..1, eased in and out. The trick then plays whole, as
    // made (its own body and board, its keys on them), on the game's height and air.
    float other_pop{};
    bool other_pop_said{};
    // Off a drop (catch_latch): the trick's own clock from its catch (AirStretch), the time it was at on the
    // touchdown, its last time, and the board's shape for where it touches.
    AirStretch stretch;
    float latch_touch_from{-1}, latch_last{-1};
    float to_ground_now{-1}; // its last look at the ground under the board (seconds; -1 none)
    float to_ground_smooth{-1}; // that look followed smoothly while the trick takes its own time (-1 none)
    float crossfade_now{-1}; // the playing trick's Crossfade (its own, or the main one)
    BoardShape latch_shape;
    bool latch_shape_ok{};
    // Once the game's pose leaves the clip (its own landing, a recording drifting off it) the trick goes on at
    // its own pace, its landing on the game's touchdown, until the game starts something new.
    double clock_lost_at{-1}, clock_touch_at{-1};
    float clock_touch_from{-1}, clock_last_time{-1};
    double drift_since{-1};      // measuring how far the game's pose is from the trick after the landing
    float drift_base{};
    int drift_count{};
    std::string clock_why;       // why it let go, said once
    std::uint64_t finished_episode{~std::uint64_t{0}}; // a trick let go is not taken up again in its air
    std::string finished_trick;
    // Board edit axes at takeoff, relative to actor placement. No recorded board pose is replayed.
    Quat board_frame_local{0, 0, 0, 1};
    bool board_at_takeoff_ok{};
    // The board's heading as it lies on the ground, relative to the placement, over the last second he rolled: the
    // board's keys turn about the board as it lay before the pop (rest_heading_before), as the studio's do (start_frame).
    std::deque<std::pair<double, Quat>> rest_headings;

    std::shared_ptr<const CustomTricks> game_tricks;
    std::map<std::string, std::size_t> variant_counts;
    std::map<std::string, std::size_t> attempt_variants;
    std::string stance_fallback_trick;
    // A trick's keys the other way round, kept so the mirror is made once rather than every frame.
    std::map<const Animation*, std::shared_ptr<const Animation>> mirrored_edits;
    bool playback_front_foot{true};
    bool played_mirrored{};
    double probe_riding_since{-1}; // the live probe takes riding once he has been riding a while
    // Edited pieces of the game's own animation, played when the game is seen playing them (PieceTracker).
    PieceTracker pieces;
    std::shared_ptr<const CustomTricks> pieces_for; // the saved tricks the tracker was set up from
    std::vector<const CustomTrick*> piece_tricks;   // the tracker's piece index -> the saved piece
    struct PiecePlay {
        const CustomTrick* trick{};
        bool mirrored{};
        float time{}, weight{};
        double at{-1};
        std::uint64_t episode{};
        Quat board_frame{0, 0, 0, 1};
        bool board_frame_ok{};
        bool board_backwards{};
    } piece_play;
    std::uint64_t piece_episodes{};
    // Saved loops of held moves (piece_tricks indices, and what they are): grind and slide loops, grab loops, and manual,
    // powerslide and brake loops. They are found by the pose he holds (held_pose_fits) where the game holds them, not by
    // moving through them (the tracker gets the others: tracked, its index -> piece_tricks).
    enum class HeldKind { none, grind, grab, ground };
    std::vector<std::pair<std::size_t, HeldKind>> held_loops;
    std::vector<std::size_t> tracked;
    struct HeldPlay {
        HeldKind kind{HeldKind::none};
        int candidate{-1};          // the loop that has been nearest, clearly, since candidate_since
        bool candidate_mirrored{};
        double candidate_since{-1};
        int playing{-1};            // the loop playing, looped on its own clock
        bool mirrored{};
        float time{};
        double off_since{-1};       // its pose has been off since
        double since{-1};
        bool told{};                // what was nearest has been logged this hold
    } held;
    std::deque<std::pair<double, std::array<Quat, 10>>> held_history; // the spine and the legs, the last half second
    // Giving way to the game. The native graph goes on underneath the authored layer the whole time, and
    // it changes its mind: a trick is refined, an obstacle turns it into something else. Then the layer
    // stands back smoothly (gave_way_at) rather than guess what the game will do.
    LimbWeights limbs;
    float riding_forward{};       // body-relative forward speed while last riding (below zero: fakie)
    bool playback_fakie{}, playback_backwards{}; // at this trick's takeoff
    std::vector<Joint> mirror_scratch;  // the pose being read while his mirrored one is written
    double gave_way_at{-1};             // the whole layer standing back after the trick changed under it
    std::string gave_way_from;
    std::vector<Joint> custom;
    // The studio's floor: the skater's Ground joint (375) when the studio opened, which lies on the
    // ground he stands or rides on (measured 2026-10-18: the board's root sits on it while riding, the
    // deck's mount 0.086 m above). Kept while the studio is open; taken again while he is in the air.
    float floor{};
    bool floor_valid{};
    std::shared_ptr<const Clip> floor_clip;
    bool floor_clip_peek{}; // floor_clip is a trick looked at from TRICKS
    bool preview_own_jump{};
    std::uint64_t failures{};
    bool failure_shown{}; // a failure is on screen: it is taken off after the next clean pass
    bool logged_layout{};
};
// The pose to draw, from the animation thread to the render publication (engine thread).
struct PreviewPacket {
    std::uintptr_t buffer{};
    std::uint32_t first{}, last{};
    std::vector<NativeBone> bones; // [first, last)
    double at{};
    std::uint64_t generation{}, sequence{}, epoch{};
    Joint mount;
    float weight{};
    std::uintptr_t holder{};
};
struct GameplayPacket {
    std::uint64_t generation{}, episode{}, sequence{}, epoch{};
    std::uintptr_t component{}, board_holder{};
    double at{};
    float time{}, weight{};
    bool board_frame_valid{}, board_owned{};
    LimbWeights limbs; // a limb the game has taken is given back to it
    bool mirrored{};   // the keys were mirrored for this stance, so the studio's pose is too
    bool board_backwards{}; // the game had the deck turned round under him at takeoff
    bool fakie{};           // he took off rolling fakie (for the playback verification's report)
    float match{};     // how far a keyed joint goes to the pose it has in the studio (match_bent_joints)
    float match_rest{}; // and how far every other joint and the board do (match_whole_body)
    float feet_match{}; // how far a foot on the deck goes where the studio has it on the board (off the game's clip)
    bool limits{};      // keyed joints are kept inside the game's range
    Quat board_frame_local{0, 0, 0, 1};
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Animation> edits;
};
std::atomic<std::uint64_t> gameplay_epoch{};
std::atomic<float> trick_blend{0.15f}; // one trick's keys into the next on the render thread: half the Crossfade setting
std::atomic<bool> overrides_allowed{true};
Live& live();
struct GameplayPackets {
    std::mutex mutex;
    std::shared_ptr<const GameplayPacket> latest;
    PresentationLatch<GameplayPacket> presentation;
};
GameplayPackets& gameplay_packets() { static auto* packets = new GameplayPackets; return *packets; }
void publish_gameplay_packet(std::shared_ptr<const GameplayPacket> packet) {
    auto& packets = gameplay_packets(); std::lock_guard lock(packets.mutex);
    if (!packet) {
        if (packets.latest || packets.presentation.pinned) gameplay_epoch.fetch_add(1, std::memory_order_acq_rel);
        packets.latest.reset(); packets.presentation.pinned.reset();
    } else if (packet->epoch == gameplay_epoch.load(std::memory_order_acquire)) packets.latest = std::move(packet);
}
std::shared_ptr<const GameplayPacket> gameplay_packet(bool presentation, std::uint64_t* frame = nullptr) {
    auto& packets = gameplay_packets(); std::lock_guard lock(packets.mutex);
    const auto epoch = gameplay_epoch.load(std::memory_order_acquire);
    // Always the newest: pinning one per presented frame drew an edit 1 to 3 updates older than the pose under
    // it whenever two updates fell in one frame (12% of frames at 54 fps, half of them at 36), and it then
    // caught up with a jump. Body and board read it the same way, so they stay together.
    (void)presentation;
    auto packet = packets.latest;
    if (frame) *frame = profiler::presents();
    if (!packet || packet->epoch != epoch || packet->generation != live().generation.load(std::memory_order_acquire) ||
        packet->component != live().component.load(std::memory_order_acquire) || !overrides_allowed.load(std::memory_order_acquire) ||
        now_seconds() - packet->at > 0.1) return {};
    return packet;
}
struct PreviewPackets {
    std::mutex mutex;
    std::uint64_t epoch{};
    std::shared_ptr<const PreviewPacket> latest;
    PresentationLatch<PreviewPacket> presentation;
};
PreviewPackets& preview_packets() { static auto* p = new PreviewPackets; return *p; }
void publish_preview_packet(std::shared_ptr<PreviewPacket> packet) {
    auto& p = preview_packets(); std::lock_guard lock(p.mutex);
    if (!packet) {
        if (p.latest || p.presentation.pinned) ++p.epoch;
        p.latest.reset(); p.presentation.pinned.reset();
    } else { packet->epoch = p.epoch; p.latest = std::move(packet); }
}
std::shared_ptr<const PreviewPacket> preview_packet(std::uint64_t& presentation) {
    auto& p = preview_packets(); std::lock_guard lock(p.mutex);
    auto packet = p.presentation.get(profiler::presents(), p.epoch, p.latest);
    presentation = p.presentation.presentation;
    if (!packet || packet->generation != live().generation.load(std::memory_order_acquire) ||
        !overrides_allowed.load(std::memory_order_acquire) || now_seconds() - packet->at > 0.1) return {};
    return packet;
}
void invalidate_outputs() noexcept {
    try {
        live().render_motion.store(MotionState::unknown, std::memory_order_release);
        publish_gameplay_packet({});
        publish_preview_packet({});
    } catch (...) {}
}
std::atomic<bool> open_flag{};
// The ground under the board in the air (ground_below), for the catch latch: where to look (the animation thread says, while a
// trick with a catch is in the air) and what was found there (the client thread looks, tick), with when.
struct GroundProbe {
    std::atomic<bool> wanted{};
    std::atomic<float> x{}, z{}, low{};       // the board's lowest point
    std::atomic<float> vx{}, vy{}, vz{};      // and how fast it flies
    std::atomic<float> to_ground{-1};         // seconds until it lands (time_to_landing), below 0 not known
    std::atomic<double> found_at{-1};
};
GroundProbe& ground_probe() { static GroundProbe probe; return probe; }
std::atomic<bool> research_capture_flag{};
// The local skater's pose and its board as they are drawn (after the body physics and the feet's placement on the
// board), taken at their render publication, for diagnostics and research.
struct RenderShot {
    std::mutex mutex;
    std::vector<NativeBone> skater;
    double at{-1};
    Joint board; // the board's mount, in the world
    double board_at{-1};
    std::uint64_t skater_generation{~std::uint64_t{0}}, board_generation{~std::uint64_t{0}};
    std::uint64_t skater_publications{}, board_publications{};
    std::vector<Joint> edited_skater;
    double edited_at{-1};
    std::uint64_t edited_generation{}, edited_sequence{}, edited_episode{}, edited_presentation{};
    std::optional<Joint> edited_board;
    double edited_board_at{-1};
    std::uint64_t edited_board_generation{}, edited_board_sequence{}, edited_board_presentation{};
    std::vector<Joint> board_rig;
};
RenderShot& render_shot() {
    static auto* value = new RenderShot;
    return *value;
}
// The studio shows in its room (open there), and until when the room's own skater
// and board are given back after it.
std::atomic<bool> room_flag{};
std::atomic<double> room_restore_until{};
// While the room opens: the menu's skater is looked for (not hidden), and when it was last seen there.
std::atomic<bool> room_watch{};
// The studio is open in the room (the walk got there): only then is the skater drawn in it.
std::atomic<bool> in_room_flag{};
std::atomic<double> room_seen_at{};
// The menu skater's facing (its placement's rotation): the studio's skater faces the same way, as the room's camera expects.
std::mutex room_facing_mutex;
Quat room_facing{0, 0, 0, 1};
bool room_facing_valid{};
// The loading screen: what it shows, and since when (for the fade).
std::atomic<Loading> loading_what{Loading::none};
std::atomic<double> loading_since{}, loading_until{};
Live& live() {
    static auto* l = new Live;
    return *l;
}

// Where a saved animation's files are. One saved now has a folder of its own named after it, holding its
// .json and .clip, so a trick is one folder to share. The ones saved before that are loose in the tricks
// folder and are read exactly where they lie: nothing is moved underneath anybody.
std::filesystem::path library_folder();
std::filesystem::path saved_file(const std::string& name, const wchar_t* extension) {
    const auto folder = library_folder();
    std::error_code error;
    const auto own = folder / name / (name + (extension[1] == 'j' ? ".json" : ".clip"));
    if (std::filesystem::is_regular_file(own, error)) return own;
    const auto loose = folder / (name + (extension[1] == 'j' ? ".json" : ".clip"));
    if (std::filesystem::is_regular_file(loose, error)) return loose;
    return own; // where a new one goes
}
std::filesystem::path saved_json(const std::string& name) { return saved_file(name, L".json"); }
std::filesystem::path saved_clip(const std::string& name) { return saved_file(name, L".clip"); }
std::filesystem::path library_folder() {
    std::array<wchar_t, 32768> buffer{};
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_ANIMATION_STUDIO_DIR", buffer.data(), static_cast<DWORD>(buffer.size()));
        length && length < buffer.size())
        return std::wstring(buffer.data(), length);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.Tricks";
}
// The studio's settings, kept beside the animations in ReSkate.Tricks so they survive a restart. It is not an
// animation: the library listings pass over it.
constexpr const wchar_t* settings_file = L"studio-settings.json";
void load_settings(State& s) {
    try {
        std::ifstream in(library_folder() / settings_file, std::ios::binary);
        if (!in) return;
        const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        if (!root.is_object()) return;
        s.front_foot = root.value("front_foot", s.front_foot);
        s.limits = root.value("joint_limits", s.limits);
        s.stance_choice = std::clamp(root.value("stance", s.stance_choice), 0, 2);
        s.grab_board = root.value("grab_board", s.grab_board);
        s.your_name = utf8_prefix(root.value("made_by", s.your_name.c_str()), 48);
        s.hover_preview = root.value("hover_preview", s.hover_preview);
        s.ungrouped.clear();
        if (root.contains("ungrouped") && root.at("ungrouped").is_array())
            for (const auto& name : root.at("ungrouped"))
                if (name.is_string() && s.ungrouped.size() < 2000) s.ungrouped.insert(name.string());
        s.favorite_packs.clear();
        if (root.contains("favorite_packs") && root.at("favorite_packs").is_array())
            for (const auto& name : root.at("favorite_packs"))
                if (name.is_string() && s.favorite_packs.size() < 500) s.favorite_packs.push_back(name.string());
        s.export_folder = root.value("export_folder", "");
        s.import_folder = root.value("import_folder", "");
        if (root.contains("crossfade") && root.at("crossfade").is_number())
            s.crossfade = std::clamp(static_cast<float>(root.at("crossfade").get<double>()), 0.1f, 0.8f);
        s.keys.clear();
        if (root.contains("editor_keys") && root.at("editor_keys").is_object())
            for (const auto& [action, key] : root.at("editor_keys").items())
                if (key.is_string() && action.size() < 32 && key.string().size() < 32 && s.keys.size() < 64) s.keys[action] = key.string();
    } catch (...) {}
}
void write_preset_file(const std::filesystem::path& path, const std::string& text); // below
// Caller holds the mutex.
void save_settings_locked(const State& s) {
    try {
        Json root = Json::object();
        root["front_foot"] = s.front_foot;
        root["joint_limits"] = s.limits;
        root["stance"] = s.stance_choice;
        root["grab_board"] = s.grab_board;
        root["crossfade"] = s.crossfade;
        if (!s.export_folder.empty()) root["export_folder"] = s.export_folder;
        if (!s.import_folder.empty()) root["import_folder"] = s.import_folder;
        if (!s.your_name.empty()) root["made_by"] = s.your_name;
        root["hover_preview"] = s.hover_preview;
        if (!s.ungrouped.empty()) root["ungrouped"] = Json(std::vector<std::string>(s.ungrouped.begin(), s.ungrouped.end()));
        if (!s.favorite_packs.empty()) root["favorite_packs"] = Json(s.favorite_packs);
        if (!s.keys.empty()) {
            Json keys = Json::object();
            for (const auto& [action, key] : s.keys) keys[action] = key;
            root["editor_keys"] = keys;
        }
        write_preset_file(library_folder() / settings_file, root.dump(2));
    } catch (...) {}
}
// The packs switched on: presets/current.txt, one name a line (a file from before holds one). A name whose
// pack is gone is passed over rather than stopping the others.
std::vector<std::string> pack_names_on() {
    std::vector<std::string> out;
    std::ifstream in(library_folder() / L"presets" / L"current.txt", std::ios::binary);
    for (std::string line; in && std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line == file_safe(line) && std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
    }
    return out;
}
std::filesystem::path pack_file(const std::string& name) { return library_folder() / L"presets" / (name + ".json"); }
// The playlist that plays: what the selection reads (read_playlist).
struct Playlist {
    bool on{};
    PlaylistMode mode{PlaylistMode::fixed};
    std::vector<PlaylistEntry> entries;
};
// The playlist from before there were many: presets/playlist.json, read once into presets/playlists.json.
std::filesystem::path playlist_file() { return library_folder() / L"presets" / L"playlist.json"; }
Playlist read_playlist_file() {
    Playlist out;
    try {
        std::ifstream in(playlist_file(), std::ios::binary);
        if (!in) return out;
        const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}));
        if (!root.is_object()) return out;
        out.on = root.value("on", false);
        out.mode = playlist_mode_named(root.value("mode", "fixed"));
        if (root.contains("entries") && root.at("entries").is_array())
            for (const auto& e : root.at("entries"))
                if (e.is_object() && out.entries.size() < 2000) {
                    PlaylistEntry entry{e.value("pack", ""), e.value("animation", "")};
                    if (!entry.pack.empty() || !entry.animation.empty()) out.entries.push_back(std::move(entry));
                }
    } catch (...) {}
    return out;
}
// ---- the playlists: presets/playlists.json ----
// Any number, one on at a time (or none): the one on decides what plays in game, its whole packs and single tricks in
// order; with none on the packs that are on do. Your own tricks play from a playlist: ACTIVE on one that is in nothing
// asks which playlist it goes in.
std::filesystem::path playlists_file() { return library_folder() / L"presets" / L"playlists.json"; }
Json playlist_entries_json(const std::vector<PlaylistEntry>& entries) {
    Json out = Json::array();
    for (const auto& e : entries) out.push_back({{"pack", e.pack}, {"animation", e.animation}});
    return out;
}
std::vector<PlaylistEntry> playlist_entries_of(const Json& root) {
    std::vector<PlaylistEntry> out;
    if (root.contains("entries") && root.at("entries").is_array())
        for (const auto& e : root.at("entries"))
            if (e.is_object() && out.size() < 2000) {
                PlaylistEntry entry{e.value("pack", ""), e.value("animation", "")};
                if (!entry.pack.empty() || !entry.animation.empty()) out.push_back(std::move(entry));
            }
    return out;
}
void write_playlists(const Playlists& playlists) {
    Json lists = Json::array();
    for (const auto& l : playlists.lists)
        lists.push_back(Json{{"name", l.name}, {"mode", playlist_mode_name(l.mode)}, {"entries", playlist_entries_json(l.entries)}});
    Json root{{"format", "animator-playlists"}, {"on", playlists.on}, {"playlists", std::move(lists)}};
    if (playlists.packs_moved) root["packs_moved"] = true;
    write_preset_file(playlists_file(), root.dump(2));
}
// Once, from before there were many: the old playlist is "Playlist 1" (on if it was), and when it was not on, the tricks
// ticked to play on their own (presets/playing.txt), which played beside the packs that were on, are "My tricks" - those
// packs first, then them - and that one is on: what played before plays on.
Playlists migrate_playlists() {
    Playlists out;
    const auto old = read_playlist_file();
    if (!old.entries.empty() || old.on) {
        out.lists.push_back({"Playlist 1", old.mode, old.entries});
        if (old.on) out.on = "Playlist 1";
    }
    std::error_code error;
    if (const auto ticked = library_folder() / L"presets" / L"playing.txt"; out.on.empty() && std::filesystem::exists(ticked, error)) {
        std::vector<std::string> loose;
        std::ifstream in(ticked, std::ios::binary);
        for (std::string line; in && std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line == file_safe(line) && std::filesystem::is_regular_file(saved_json(line), error)) loose.push_back(line);
        }
        if (!loose.empty()) {
            NamedPlaylist mine{"My tricks", PlaylistMode::fixed, {}};
            for (const auto& pack : pack_names_on()) mine.entries.push_back({pack, ""});
            for (const auto& name : loose) mine.entries.push_back({"", name});
            out.lists.push_back(std::move(mine));
            out.on = "My tricks";
        }
    }
    try {
        write_playlists(out);
        if (std::filesystem::exists(playlist_file(), error)) {
            auto kept = playlist_file();
            kept += L".before-playlists";
            std::filesystem::rename(playlist_file(), kept, error);
        }
        logging::log(Level::info, Channel::skater, "Animation Studio: {} playlists from the old playlist and the tricks ticked on their own; {} on.",
                     out.lists.size(), out.on.empty() ? std::string("none") : out.on);
    } catch (...) {}
    return out;
}
// Once: packs play only from a playlist now (the PACKS cards have no ON of their own). With none on, the packs that were
// switched on - which played then - go in "My packs", and that one is on: what played before plays on.
void move_packs_into_playlist(Playlists& lists) {
    if (lists.packs_moved) return;
    lists.packs_moved = true;
    if (lists.on.empty()) {
        const auto on = pack_names_on();
        if (!on.empty()) {
            std::string name = "My packs";
            for (unsigned n = 2; lists.named(name); ++n) name = "My packs " + std::to_string(n);
            NamedPlaylist mine{name, PlaylistMode::shuffle, {}};
            for (const auto& pack : on) mine.entries.push_back({pack, ""});
            lists.lists.push_back(std::move(mine));
            lists.on = name;
        }
    }
    try {
        write_playlists(lists);
        logging::log(Level::info, Channel::skater, "Animation Studio: packs play from playlists only now; {}.",
                     lists.on.empty() ? std::string("no pack was on") : lists.on + " is on");
    } catch (...) {}
}
Playlists read_playlists() {
    // The game's side and the studio both read it: the first to find none makes it, once.
    static std::mutex making;
    std::lock_guard lock(making);
    std::error_code error;
    if (!std::filesystem::exists(playlists_file(), error)) {
        auto made = migrate_playlists();
        move_packs_into_playlist(made);
        return made;
    }
    Playlists out;
    try {
        std::ifstream in(playlists_file(), std::ios::binary);
        const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}));
        if (!root.is_object()) return out;
        if (root.contains("playlists") && root.at("playlists").is_array())
            for (const auto& l : root.at("playlists")) {
                if (!l.is_object() || out.lists.size() >= 200) continue;
                NamedPlaylist list{utf8_prefix(l.value("name", ""), 48), playlist_mode_named(l.value("mode", "fixed")), playlist_entries_of(l)};
                if (list.name.empty() || out.named(list.name)) continue;
                out.lists.push_back(std::move(list));
            }
        out.on = root.value("on", "");
        if (!out.named(out.on)) out.on.clear();
        out.packs_moved = root.contains("packs_moved") && root.at("packs_moved").is_boolean() && root.at("packs_moved").get<bool>();
    } catch (...) {
        return out;
    }
    move_packs_into_playlist(out);
    return out;
}
Playlist playing_of(const Playlists& playlists) {
    if (const auto* on = playlists.named(playlists.on)) return {true, on->mode, on->entries};
    return {};
}
Playlist read_playlist() { return playing_of(read_playlists()); }
// A name no playlist has: "Playlist 2", "Playlist 3" ...
std::string free_playlist_name(const Playlists& playlists) {
    for (std::size_t n = playlists.lists.size() + 1;; ++n)
        if (const auto name = "Playlist " + std::to_string(n); !playlists.named(name)) return name;
}
// How the playlist's tricks take turns, for the game's side (the trick loader sets it with the set of tricks it is for).
std::atomic<int> playlist_turns_mode{0};
std::filesystem::path pack_thumbnail_file(const std::string& name) { return library_folder() / L"presets" / (name + ".png"); }
// A path as UTF-8 (path::string() is the ANSI code page, which throws on a name it cannot spell).
std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
// Where packs are exported to and imported from: as set in SETTINGS, or ReSkate.Tricks\exports and \import.
std::filesystem::path pack_export_folder(const State& s) {
    if (!s.export_folder.empty()) return std::filesystem::path(std::u8string(s.export_folder.begin(), s.export_folder.end()));
    return library_folder() / L"exports";
}
std::filesystem::path pack_import_folder(const State& s) {
    if (!s.import_folder.empty()) return std::filesystem::path(std::u8string(s.import_folder.begin(), s.import_folder.end()));
    return library_folder() / L"import";
}
// What plays a trick more than one pack that is on has (set_conflict_choice): presets/conflicts.json.
std::filesystem::path conflicts_file() { return library_folder() / L"presets" / L"conflicts.json"; }
// The studio's own lists in presets/ (playlist.json, locked.json, conflicts.json): a pack by one of these names would be
// written over it, or it over the pack.
bool reserved_pack_name(const std::string& name) {
    for (const char* own : {"playlist", "playlists", "locked", "conflicts"})
        if (_stricmp(name.c_str(), own) == 0) return true;
    return false;
}
ConflictChoices read_conflict_choices() {
    ConflictChoices out;
    try {
        std::ifstream in(conflicts_file(), std::ios::binary);
        if (!in) return out;
        const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        if (!root.is_object()) return out;
        for (const auto& [slot, choice] : root.items())
            if (choice.is_string() && choice.string().size() <= 64 && slot.size() <= 128) out[slot] = choice.string();
    } catch (...) {}
    return out;
}
PresetPack read_pack_file(const std::string& name) {
    std::ifstream in(pack_file(name), std::ios::binary);
    if (!in) throw std::runtime_error("The pack " + name + " is missing.");
    auto pack = read_pack_json(Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())));
    pack.name = name;
    return pack;
}
// Every pack there is (the playlist can take from any of them, on or not).
std::vector<PresetPack> all_packs() {
    std::vector<PresetPack> out;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(library_folder() / L"presets", error))
        if (entry.is_regular_file(error) && entry.path().extension() == L".json" && entry.path().stem() != L"playlist" &&
            entry.path().stem() != L"playlists" && entry.path().stem() != L"conflicts" && entry.path().stem() != L"locked")
            try { out.push_back(read_pack_file(entry.path().stem().string())); } catch (...) {}
    return out;
}
std::vector<PresetPack> packs_on() {
    std::vector<PresetPack> out;
    for (const auto& name : pack_names_on()) {
        try { out.push_back(read_pack_file(name)); }
        catch (const std::exception& e) { logging::log(Level::warning, Channel::skater, "Animation Studio: pack {} not used: {}", name, e.what()); }
    }
    return out;
}
void write_preset_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) throw std::runtime_error("Could not write " + path.filename().string());
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not replace preset selection");
}
// The tricks switched to PLAY on their own (MY TRICKS): presets/playing.txt, one name a line.
void write_playing_names(const std::set<std::string>& names) {
    std::string text;
    for (const auto& name : names) text += name + "\n";
    write_preset_file(library_folder() / L"presets" / L"playing.txt", text);
}
// With no file yet (a library from before PLAY), what played goes on playing: with no pack on every saved trick
// played, so all of `library` is switched on; with packs on the packs decided, so none. The file is written then, so a
// trick saved from now on starts switched off.
std::set<std::string> playing_names(const std::vector<std::string>& library, bool any_pack_on) {
    const auto file = library_folder() / L"presets" / L"playing.txt";
    std::set<std::string> out;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        if (!any_pack_on) out.insert(library.begin(), library.end());
        try { write_playing_names(out); } catch (...) {}
        return out;
    }
    std::ifstream in(file, std::ios::binary);
    for (std::string line; in && std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line == file_safe(line)) out.insert(line);
    }
    return out;
}
// ---- older animations, brought up to date ----------------------------------------------------------
// What changed under saved animations on 2026-10-22, and what is done once to each file made before:
// - its stance label could be wrong (one made with the right foot leading was labelled regular normal);
//   the label is set from the clip, so a pack picks the right variant per stance.
// - its board keys were made in the frame of the clip's own board, which on a clip recorded mirrored or
//   with the board backwards points the other way to the board the game draws; they are turned into the
//   board's nose-forward frame the studio and the game now both use.
// The old file is kept beside it (.json.before-upgrade) and the new one marked, so this happens once.
// "2026-10-22.2": the label check again, since files saved in between kept a label that did not match their
// clip (two of the same trick then counted as different stances and both played, taking turns). The board
// keys are only ever turned once (board_nose_forward).
inline constexpr const char* current_upgrade = "2026-10-22.2";
bool upgrade_animation(Animation& animation, const Clip& clip) {
    if (animation.upgraded == current_upgrade) return false;
    if (const auto lead = clip_leads_right(clip)) {
        const bool goofy = authored_goofy(animation);
        const bool label = leads_right(goofy, animation.switch_stance.value_or(false));
        if (label != *lead) animation.switch_stance = goofy != *lead;
    }
    Joint first_board;
    const bool turned_round = clip.sample_board(0, first_board) && rotate(first_board.rotation, Vec3{0, 0, 1})[0] < 0;
    if (!animation.board_nose_forward && turned_round)
        if (auto* board = animation.track(board_joint))
            for (auto& key : board->keys) {
                // Half a turn about the board's up: across and along change sign, up stays.
                key.rotation = {-key.rotation[0], key.rotation[1], -key.rotation[2], key.rotation[3]};
                key.offset = {-key.offset[0], key.offset[1], -key.offset[2]};
            }
    animation.board_nose_forward = true;
    animation.upgraded = current_upgrade;
    return true;
}
// Upgrades the saved file at `json_path` (its clip beside it) if it needs it. Returns whether it did.
bool upgrade_saved(const std::filesystem::path& json_path) {
    try {
        std::string text;
        {
            std::ifstream in(json_path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        auto animation = from_json(text);
        if (animation.upgraded == current_upgrade) return false;
        auto clip_path = json_path;
        clip_path.replace_extension(L".clip");
        std::ifstream clip_in(clip_path, std::ios::binary);
        if (!clip_in) return false;
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
        const auto clip = clip_from_bytes(bytes);
        animation.clip.times = {0, clip.duration()};
        animation.clip.takeoff = clip.takeoff;
        animation.clip.landing = clip.landing;
        const auto before_label = animation.switch_stance;
        if (!upgrade_animation(animation, clip)) return false;
        auto backup = json_path;
        backup += L".before-upgrade";
        std::error_code error;
        if (!std::filesystem::exists(backup, error)) std::filesystem::copy_file(json_path, backup, error);
        // Kept as saved when: "the one saved last plays" goes by it, and an upgrade is not a save.
        const auto saved_when = std::filesystem::last_write_time(json_path, error);
        write_preset_file(json_path, to_json(animation));
        if (!error) std::filesystem::last_write_time(json_path, saved_when, error);
        logging::log(Level::info, Channel::skater, "Animation Studio: upgraded {}{}.", json_path.stem().string(),
                     before_label != animation.switch_stance ? " (its stance label now matches the foot its clip leads with)" : "");
        return true;
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation Studio: {} could not be upgraded: {}", json_path.filename().string(), e.what());
        return false;
    }
}
// The tricks folder's own folders: never a trick's name, and never read as one (a trick saved as "presets" would be
// written into the presets folder; the MARKET's old market/market.json was listed as a trick called market).
bool reserved_name(const std::string& name) {
    for (const char* own : {"presets", "recorded", "library", "research", "market", "diagnostics"})
        if (_stricmp(name.c_str(), own) == 0) return true;
    return false;
}
// Every saved animation's metadata file: the ones loose in the tricks folder, and the ones in a folder of
// their own named after them. A folder without its own .json, and the studio's own folders, are passed over.
std::vector<std::filesystem::path> saved_jsons(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> out, loose;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == L".json" && entry.path().filename() != settings_file)
            loose.push_back(entry.path());
        else if (entry.is_directory(error) && !reserved_name(entry.path().filename().string())) {
            auto own = entry.path() / entry.path().filename();
            own += L".json";
            std::error_code inner;
            if (std::filesystem::is_regular_file(own, inner)) out.push_back(own);
        }
    }
    // A trick in both places is the same trick: the folder wins, as saved_file has it. Loaded twice it
    // would be two variants of itself and alternate with itself in game.
    for (const auto& path : loose)
        if (std::none_of(out.begin(), out.end(), [&](const auto& own) { return own.stem() == path.stem(); })) out.push_back(path);
    return out;
}
// When a saved animation's file was last written, for "the one saved last plays".
std::int64_t saved_time(const std::filesystem::path& path) {
    std::error_code error;
    const auto at = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<std::int64_t>(at.time_since_epoch().count());
}
// ---- tricks locked to their maker -----------------------------------------------------------------
// Every trick and pack made here carries this PC's key (maker_key). One carrying another PC's key plays, tests and
// previews, but is not opened to edit, copied, or exported again: people can share packs without their tricks being
// taken and passed on as somebody else's. The key is a one-way hash of Windows' machine id, so it says nothing about
// the PC. An imported trick made elsewhere is also remembered by its clip (presets/locked.json), so taking the key out of
// its file afterwards does not unlock it. (Files on a PC can always be changed by whoever owns it: this keeps honest
// people honest, it is not a safe.)
std::string sha256_hex(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) != 0)
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const auto b : digest) out += hex[b >> 4], out += hex[b & 15];
    return out;
}
const std::string& device_key() {
    static const std::string key = [] {
        std::string id;
        std::array<wchar_t, 128> buffer{};
        DWORD size = static_cast<DWORD>(sizeof(buffer));
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                         buffer.data(), &size) == ERROR_SUCCESS)
            for (const wchar_t c : std::wstring_view(buffer.data())) id += static_cast<char>(c & 0x7f);
        if (id.empty()) {
            // No machine id to read: the PC's name and its system drive's serial instead.
            std::array<wchar_t, 64> name{};
            DWORD length = static_cast<DWORD>(name.size());
            GetComputerNameW(name.data(), &length);
            DWORD serial = 0;
            GetVolumeInformationW(L"C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);
            for (DWORD i = 0; i < length; ++i) id += static_cast<char>(name[i] & 0x7f);
            id += "/" + std::to_string(serial);
        }
        return sha256_hex("reskate-animator-studio/maker/" + id).substr(0, 32);
    }();
    return key;
}
std::filesystem::path locked_clips_file() { return library_folder() / L"presets" / L"locked.json"; }
// The clips of tricks made on other PCs, by their hash, and whose they are.
std::map<std::string, std::string> read_locked_clips() {
    std::map<std::string, std::string> out;
    try {
        std::ifstream in(locked_clips_file(), std::ios::binary);
        if (!in) return out;
        const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}));
        if (root.is_object() && root.contains("clips") && root.at("clips").is_object())
            for (const auto& [hash, key] : root.at("clips").items())
                if (key.is_string() && hash.size() == 64) out[hash] = key.string();
    } catch (...) {}
    return out;
}
void remember_locked_clips(const std::map<std::string, std::string>& more, PackFileTransaction* transaction = nullptr) {
    if (more.empty()) return;
    auto all = read_locked_clips();
    for (const auto& [hash, key] : more) all[hash] = key;
    Json clips = Json::object();
    for (const auto& [hash, key] : all) clips[hash] = key;
    const auto text = Json{{"format", "animator-locked-clips"}, {"clips", std::move(clips)}}.dump(1);
    if (transaction) transaction->write(locked_clips_file(), text);
    else write_preset_file(locked_clips_file(), text);
}
// A saved clip's hash, kept while the file is the same.
std::string clip_hash(const std::filesystem::path& path) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::tuple<std::uintmax_t, std::filesystem::file_time_type, std::string>> known;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    const auto when = std::filesystem::last_write_time(path, error);
    std::lock_guard lock(mutex);
    if (const auto found = known.find(path); found != known.end() && std::get<0>(found->second) == size && std::get<1>(found->second) == when)
        return std::get<2>(found->second);
    std::ifstream in(path, std::ios::binary);
    const auto hash = sha256_hex(std::string((std::istreambuf_iterator<char>(in)), {}));
    known[path] = {size, when, hash};
    return hash;
}
// Makers' keys that are yours though made on another PC: the keys of the packs your Thunderstore team put out, once its
// token is checked to be the team's (market::verified_team). A creator on a new PC, after a reinstall or with a pack got
// back from the MARKET could not share his own pack again. Worked out with the library (claim_account_keys).
std::mutex account_keys_mutex;
std::set<std::string> account_keys;
bool locked_key(const std::string& maker_key) {
    if (maker_key.empty() || maker_key == device_key()) return false;
    std::lock_guard lock(account_keys_mutex);
    return !account_keys.contains(maker_key);
}
// The team a pack went out under: as it was published from here ("Team-Name"), or where it was got from.
std::string pack_team(const PresetPack& pack) {
    if (!pack.published_as.empty()) {
        const auto dash = pack.published_as.find('-');
        return dash == std::string::npos ? std::string{} : pack.published_as.substr(0, dash);
    }
    return market::installed_from(pack.name);
}
// The keys of the packs the checked team put out. True when they changed (the library is read again with them).
bool claim_account_keys(const std::vector<PresetPack>& packs) {
    std::set<std::string> keys;
    std::vector<std::string> names;
    const auto team = market::verified_team();
    if (!team.empty())
        for (const auto& pack : packs)
            if (!pack.maker_key.empty() && pack.maker_key != device_key() && _stricmp(pack_team(pack).c_str(), team.c_str()) == 0) {
                keys.insert(pack.maker_key);
                names.push_back(pack.name);
            }
    {
        std::lock_guard lock(account_keys_mutex);
        if (keys == account_keys) return false;
        account_keys = std::move(keys);
    }
    for (const auto& name : names)
        logging::log(Level::info, Channel::skater, "Animation Studio: {} is your team {}'s pack: yours to share on this PC too.", name, team);
    return true;
}
// Whether the saved animation `name` is locked to another PC's maker (read with the library).
void read_library_locked(State& s);
bool locked_locked(State& s, const std::string& name) {
    if (!s.library_read) read_library_locked(s);
    const auto found = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return _stricmp(p.name.c_str(), name.c_str()) == 0; });
    return found != s.presets.end() && found->locked;
}
// Another maker's trick: made on another PC (editable or not), or in a pack made on another PC (got from the MARKET or
// imported). Only your own tricks go in your packs and are shared, so nobody reshares tricks that are not theirs.
bool someone_elses_locked(State& s, const std::string& name) {
    if (!s.library_read) read_library_locked(s);
    const auto found = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return _stricmp(p.name.c_str(), name.c_str()) == 0; });
    if (found != s.presets.end() && (found->foreign || found->locked)) return true;
    return std::any_of(s.packs.begin(), s.packs.end(), [&](const PresetPack& pack) { return locked_key(pack.maker_key) && pack.has(name); });
}
void read_library_locked(State& s) {
    s.library.clear();
    s.presets.clear();
    s.packs.clear();
    s.pack_pictures_read = false;
    s.pack_times.clear();
    s.packs_on.clear();
    s.library_read = true;
    std::error_code error;
    const auto folder = library_folder();
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return;
    for (const auto& path : saved_jsons(folder)) s.library.push_back(path.stem().string());
    std::sort(s.library.begin(), s.library.end());
    s.library.erase(std::unique(s.library.begin(), s.library.end()), s.library.end());
    for (const auto& name : s.library) upgrade_saved(saved_json(name));
    try {
        s.packs_on = packs_on();
        const auto playing = playing_names(s.library, !s.packs_on.empty());
        const auto locked_clips = read_locked_clips();
        for (const auto& name : s.library) {
            try {
                std::ifstream in(saved_json(name), std::ios::binary);
                auto animation = from_json(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
                if (!animation.trick.empty()) {
                    s.presets.push_back({name, trick_key(animation.trick), animation.pop, false, false, animation.stance, animation.switch_stance, animation.entry});
                    s.presets.back().fakie = animation.fakie;
                    s.presets.back().leads_right = leads_right(authored_goofy(animation), animation.switch_stance.value_or(false));
                    s.presets.back().made_by = animation.author;
                    s.presets.back().label = animation.label;
                    s.presets.back().saved_at = saved_time(saved_json(name));
                    // By its clip as well, for one whose key was taken out; never one with this PC's key (made or saved
                    // here: a trick of yours from the same game clip as an installed one stays yours).
                    // By its clip only for a clip that is its maker's own work: a clip of the game's (native_asset) is the same file
                    // for everyone who made a trick from it, so a trick of yours from the same game clip as one in a pack you
                    // got was taken for that pack's (your own tricks, said to be someone else's when you shared your pack).
                    const auto by_clip = [&] {
                        if (locked_clips.empty() || animation.source.kind == "native_asset" ||
                            !(animation.maker_key.empty() || locked_key(animation.maker_key)))
                            return false;
                        const auto whose = locked_clips.find(clip_hash(saved_clip(name)));
                        return whose != locked_clips.end() && locked_key(whose->second);
                    };
                    s.presets.back().locked = (locked_key(animation.maker_key) && !animation.editable) || by_clip();
                    s.presets.back().foreign = locked_key(animation.maker_key) || s.presets.back().locked;
                    s.presets.back().playing = playing.contains(name);
                }
            } catch (...) {} // A broken animation stays listed for inspection, but is not a preset.
        }
        const auto packs = folder / L"presets";
        if (std::filesystem::is_directory(packs, error))
            for (const auto& entry : std::filesystem::directory_iterator(packs, error))
                if (entry.is_regular_file(error) && entry.path().extension() == L".json" && entry.path().stem() != L"playlist" &&
                    entry.path().stem() != L"playlists" && entry.path().stem() != L"conflicts" && entry.path().stem() != L"locked") {
                    try {
                        s.packs.push_back(read_pack_file(entry.path().stem().string()));
                        s.pack_times[s.packs.back().name] = saved_time(entry.path());
                    }
                    catch (const std::exception& e) {
                        logging::log(Level::warning, Channel::skater, "Animation Studio: pack {} not read: {}", entry.path().filename().string(), e.what());
                    }
                }
        std::sort(s.packs.begin(), s.packs.end(), [](const PresetPack& a, const PresetPack& b) { return a.name < b.name; });
        // Your team's packs made on another PC are yours (claim_account_keys): read again with that known, once.
        if (claim_account_keys(s.packs)) {
            read_library_locked(s);
            return;
        }
        const auto choices = read_conflict_choices();
        s.playlists = read_playlists();
        const auto playlist = playing_of(s.playlists);
        s.playlist_on = playlist.on;
        s.playlist_mode = playlist.mode;
        s.playlist = playlist.entries;
        if (!s.playlists.named(s.playlist_open))
            s.playlist_open = !s.playlists.on.empty() ? s.playlists.on : s.playlists.lists.empty() ? std::string{} : s.playlists.lists.front().name;
        if (playlist.on) {
            select_playlist(s.presets, s.packs, playlist.entries, playlist.mode);
            s.conflicts.clear();
        } else {
            // No playlist on: nothing plays (packs and tricks play from playlists only, as on the game's side).
            const std::set<std::string> none;
            select_presets(s.presets, std::span<const PresetPack>{}, nullptr, &none);
            s.conflicts.clear();
        }
    } catch (const std::exception& e) { s.status = e.what(); }
}

} // namespace
std::shared_ptr<Clip> read_game_clip(const GameTrick& trick); // a clip of the trick library, below
std::optional<std::pair<Animation, Clip>> onto_game_pop(const Animation& a, const Clip& clip, const std::string& pop); // below
namespace {
float find_catch(const Clip& clip, const Animation& edits); // below
// ---- the saved tricks the game plays ------------------------------------------------------------------
// Read on a thread of their own (trick_loader, below): first as soon as ReSkate starts, then whenever what plays changes,
// so neither the game nor the studio waits on them, and what played before goes on playing until the new set is ready.
// What was read before is kept (TrickCache) and only what changed on disk is read again, so a change of playlist, of its
// mode or of a pack takes moments, with a thousand tricks too. A clip that is the same as another - a pack's tricks edited
// from the same clip of the game's - is one clip in memory, and the poses NativeClock follows are worked out once for each
// clip and shared (each trick kept its own 2 MB clip and its own copies before: a thousand tricks took gigabytes, were
// read again on the game's own thread at every change, and what played was the set from before the change meanwhile).
namespace {
struct FileStamp {
    std::int64_t written{};
    std::uintmax_t size{};
    bool operator==(const FileStamp&) const = default;
};
FileStamp stamp_of(const std::filesystem::path& path) {
    std::error_code error;
    FileStamp out;
    if (const auto at = std::filesystem::last_write_time(path, error); !error) out.written = static_cast<std::int64_t>(at.time_since_epoch().count());
    if (const auto size = std::filesystem::file_size(path, error); !error) out.size = size;
    return out;
}
struct TrickCache {
    // Each saved trick's metadata, by its file: what is chosen from (select_playlist, select_presets).
    struct Meta {
        FileStamp json, clip;
        std::optional<Preset> preset; // none: not a trick that can play (no trick, no clip, not readable)
    };
    std::map<std::filesystem::path, Meta> meta;
    // The tricks read whole, by file, and the pop each was read for (its own, or its pack's).
    struct Loaded {
        FileStamp json, clip;
        std::string pop;
        CustomTrick trick;
        std::vector<CustomTrick> heights; // on ANY pop: its versions for the other heights (load_custom_trick)
    };
    std::map<std::filesystem::path, Loaded> loaded;
    std::shared_ptr<const std::vector<GameTrick>> catalog;      // the trick library they were read with
    std::map<std::string, std::shared_ptr<const Clip>> clips;   // the tricks' own clips, by what is in them
    std::map<std::string, std::vector<NativeModels::Model>> models; // a clip's models (native_models_of), by the same key
    std::map<std::string, std::vector<std::string>> references; // a trick's clips in the library, by its trick
    std::map<std::string, BoardTrack> reference_boards;           // each of those clips' board ("library|<file>")
    std::map<std::string, std::string> reference_pops;            // and its pop
    std::map<std::string, std::shared_ptr<const TimeMap>> time_maps; // an own clip on a library clip ("<own key>|<library key>")
    std::map<std::string, std::shared_ptr<const NativeModels>> natives; // a game clip's, for the versions moved onto it
};
TrickCache& trick_cache() {
    static auto* cache = new TrickCache; // the loader's own: only its thread uses it
    return *cache;
}
std::vector<std::uint8_t> file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("its file is missing");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// What is in a clip's file, as a key (`file` when it cannot be worked out: then it is shared with nothing).
std::string content_key(const std::vector<std::uint8_t>& bytes, bool native, const std::filesystem::path& file) {
    auto key = sha256_hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (key.empty()) key = "file|" + utf8_path(file);
    return key + (native ? "|native" : "|own");
}
// A trick's own clip from its file's bytes: the one in memory already when another trick's file has the same (`native`:
// its pop and landing placed where the game has them, as a trick edited from the game's clip has).
std::shared_ptr<const Clip> shared_clip(TrickCache& cache, const std::filesystem::path& file, bool native, std::string& key) {
    const auto bytes = file_bytes(file);
    key = content_key(bytes, native, file);
    auto& kept = cache.clips[key];
    if (!kept) {
        auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
        if (native && place_native_marks(*clip))
            logging::log(Level::info, Channel::skater, "Animation Studio: a trick's clip pops at {:.2f} s, where the game has it.", clip->takeoff);
        kept = std::move(clip);
    }
    return kept;
}
// A clip's models for NativeClock, worked out once for every trick that may play that clip.
const std::vector<NativeModels::Model>& models_of(TrickCache& cache, const std::string& key, const Clip& clip) {
    auto found = cache.models.find(key);
    if (found == cache.models.end()) found = cache.models.emplace(key, native_models_of(clip, 0, false)).first;
    return found->second;
}
// The game's clips a trick may play instead of its own: the library's low, mid and high pop of it, and - under the same
// trick name - its dark and light catches, underflips, held and late flips (followed too, or a trick caught dark played on
// its own clock, out of step). Their keys in `cache.models` ("library|<file>"), read once for every trick of the kind.
const std::vector<std::string>& library_references(TrickCache& cache, const std::string& trick) {
    if (const auto found = cache.references.find(trick); found != cache.references.end()) return found->second;
    std::vector<std::string> keys;
    const auto add = [&](const GameTrick& g) {
        const auto key = "library|" + g.file;
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
        cache.reference_pops[key] = g.pop;
        if (!cache.models.contains(key)) {
            try {
                const auto clip = read_game_clip(g); // read for its poses and its board, not kept
                cache.models.emplace(key, native_models_of(*clip, 0, false));
                cache.reference_boards.emplace(key, board_track(*clip));
            } catch (...) {
                return;
            }
        }
        keys.push_back(key);
    };
    try {
        if (const auto& library = cache.catalog) {
            std::vector<std::string> families; // "animation/dingo/c_proto_onb_trick_o_kickflip_"
            for (const auto& g : *library) {
                if (g.preview_only || g.file.empty() || trick_key(g.trick) != trick) continue;
                add(g);
                const auto cut = g.asset.rfind('_');
                if (g.asset.starts_with("animation/dingo/c_proto_onb_trick_") && cut != std::string::npos) families.push_back(g.asset.substr(0, cut + 1));
            }
            for (const auto& g : *library) {
                if (!g.preview_only || g.file.empty()) continue;
                for (const auto& family : families) {
                    if (!g.asset.starts_with(family)) continue;
                    const std::string_view style(g.asset.c_str() + family.size());
                    if (style.find("dark") != std::string_view::npos || style.find("light") != std::string_view::npos ||
                        style.find("uflip") != std::string_view::npos || style.find("held") != std::string_view::npos ||
                        style.find("lateflip") != std::string_view::npos)
                        add(g);
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation Studio: the library's pops of {} are not looked for: {}", trick, e.what());
    }
    return cache.references.emplace(trick, std::move(keys)).first->second;
}
// What the game may play for a trick: its own clip (clip 0) and the library's clips of it (its pops, its catches), each
// with its time as the trick's and whether it is another or a higher pop than `made_pop`, the pop the clip is of.
std::shared_ptr<const NativeModels> natives_for(TrickCache& cache, const std::string& key, const Clip& clip, const std::string& trick,
                                                const std::string& clip_pop, bool whole_body);
// One saved trick read whole, to play: its keys (on its pack's pop when its pack says), its clip, its stance and the
// clips the game may play for it. On ANY pop, `heights` gets its versions for the other heights.
std::optional<CustomTrick> load_custom_trick(TrickCache& cache, const std::filesystem::path& entry, const Preset& p,
                                             std::vector<CustomTrick>* heights = nullptr) {
    auto clip_path = entry;
    clip_path.replace_extension(L".clip");
    std::ifstream json_in(entry, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>());
    auto animation = std::make_shared<Animation>(from_json(text));
    if (animation->trick.empty()) return std::nullopt;
    // The pop it was made on, before its pack's rule: the game's clip it was edited from says (".._360flip_low").
    std::string made_pop = animation->pop;
    if (const auto& asset = animation->source.asset; animation->source.kind == "native_asset" && asset.starts_with("animation/dingo/c_proto_onb_trick_"))
        if (const auto cut = asset.rfind('_'); cut != std::string::npos)
            if (const auto end = asset.substr(cut + 1); end == "low" || end == "mid" || end == "high") made_pop = end;
    if (p.pack_pop) animation->pop = *p.pack_pop; // the pop its pack's rules (or a test) say it plays on
    std::string key;
    const auto clip = shared_clip(cache, clip_path, animation->source.kind == "native_asset", key);
    // A piece of the game's animation (a setup, a loop, the clip a trick is blended from) has no pop of its own: it is
    // found by its pose while it plays, not by a takeoff.
    const bool piece = animation->trick.starts_with("native:");
    if (clip->joints != skeleton_joints || (clip->takeoff < 0 && !piece)) return std::nullopt;
    const auto trick = trick_key(animation->trick); // a trick saved under an older spelling still plays
    const bool goofy = authored_goofy(*animation);
    // Its lead foot from the clip, its label only when the clip cannot tell: labels have been wrong (one made with the
    // right foot leading was labelled regular normal and played on the wrong leg).
    const bool label_right = leads_right(goofy, animation->switch_stance.value_or(false));
    const bool right = clip_leads_right(*clip).value_or(label_right);
    if (right != label_right)
        logging::log(Level::info, Channel::skater, "Animation Studio: {} is labelled as leading with the {} foot but leads with the {}; going by the clip.",
                     p.name, label_right ? "right" : "left", right ? "right" : "left");
    const auto place = [](const std::string& pop) { return pop == "low" || pop == "mid" || pop == "high"; };
    const std::string clip_pop = place(made_pop) ? made_pop : std::string{};
    const bool locked = locked_key(animation->maker_key) && !animation->editable;
    CustomTrick out{trick, p.name, clip, animation, goofy, right, animation->fakie, natives_for(cache, key, *clip, trick, clip_pop, locked)};
    out.caught = find_catch(*clip, *animation);
    out.any_pop = animation->pop.empty();
    if (!heights || piece || clip_pop.empty()) return out;
    // The trick as made for another height: what COPY TO LOW, MID AND HIGH makes of it - one of yours moved onto the game's
    // own clip of that height, its keys timed by the board; one locked to its maker (or a recording) on its own clip, as
    // made - made here, in memory, so it follows every change to the trick.
    const CustomTrick original = out;
    const auto version_for = [&](const std::string& height) -> std::optional<CustomTrick> {
        try {
            CustomTrick version = original;
            version.height = height;
            version.any_pop = false;
            std::optional<std::pair<Animation, Clip>> moved;
            if (!locked && height != clip_pop) moved = onto_game_pop(*animation, *clip, height);
            if (moved) {
                auto& [copy, game_clip] = *moved;
                copy.pop = height;
                // The game's clip of that height, once in memory for every trick moved onto it (the same way round).
                const auto clip_key = "height|" + copy.source.asset + (clip_leads_right(game_clip).value_or(false) ? "|right" : "|left");
                auto& kept = cache.clips[clip_key];
                if (!kept) kept = std::make_shared<Clip>(std::move(game_clip));
                version.clip = kept;
                version.edits = std::make_shared<Animation>(std::move(copy));
                auto& natives = cache.natives[clip_key + "|" + trick];
                if (!natives) natives = natives_for(cache, clip_key, *kept, trick, height, false);
                version.native = natives;
                version.caught = find_catch(*kept, *version.edits);
            } else {
                auto copy = std::make_shared<Animation>(*animation);
                copy->pop = height;
                version.edits = std::move(copy);
            }
            return version;
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: {} has no {} pop version: {}", p.name, height, e.what());
            return std::nullopt;
        }
    };
    if (animation->pop.empty()) {
        // On ANY pop (its own setting, or its pack's): a version for each other height, and the trick itself is the one for
        // its own height - exactly that pop, as its versions are for theirs (left on ANY, other tricks' versions outranked it
        // there, and cycling went round fewer tricks).
        std::size_t made = 0;
        for (const char* height : {"low", "mid", "high"})
            if (height != clip_pop)
                if (auto version = version_for(height)) {
                    heights->push_back(std::move(*version));
                    ++made;
                }
        if (made >= 2) {
            auto own = std::make_shared<Animation>(*animation);
            own->pop = clip_pop;
            out.edits = std::move(own);
        }
    } else if (place(animation->pop)) {
        // Set to one pop: on it, itself - or, when its clip is of another pop (set by hand, by its pack's rule, a copy made
        // before copies went onto the game's clip), its version for that pop, as TEST IN GAME plays it. On the other heights a
        // fallback version, played only when nothing of yours is for that pop: until 2026-10-25 a trick played on the pops it
        // had no version for, and testers' tricks then stopped playing on every pop the game graded otherwise.
        const std::string set_pop = animation->pop;
        if (set_pop != clip_pop)
            if (auto version = version_for(set_pop)) out = std::move(*version);
        for (const char* height : {"low", "mid", "high"})
            if (height != set_pop)
                if (auto version = version_for(height)) {
                    version->fallback = true;
                    heights->push_back(std::move(*version));
                }
    }
    return out;
}
// `clip_pop` is the pop the clip is of ("" not known). `whole_body`: on another pop's clip the trick plays whole, as made
// (other_pop, and higher_pop's own pace and hand-back) - only for a trick locked to its maker, which has no version moved
// onto that clip. One of yours played its keys on the game's clip there until 2026-10-25, and does again.
std::shared_ptr<const NativeModels> natives_for(TrickCache& cache, const std::string& key, const Clip& clip_ref, const std::string& trick,
                                                const std::string& clip_pop, bool whole_body) {
    const std::string& made_pop = clip_pop;
    const auto* clip = &clip_ref;
    auto native = std::make_shared<NativeModels>();
    native->edit_up = clip->takeoff;
    native->edit_down = clip->landing;
    for (auto model : models_of(cache, key, *clip)) {
        model.clip = 0;
        model.own = true;
        native->models.push_back(std::move(model));
    }
    native->maps.push_back(nullptr); // clip 0, its own: its own time
    native->other_pop.push_back(false);
    native->higher_pop.push_back(false);
    const auto pop_place = [](const std::string& pop) { return pop == "low" ? 0 : pop == "mid" ? 1 : pop == "high" ? 2 : -1; };
    std::optional<BoardTrack> made_track;
    std::size_t index = 1;
    for (const auto& reference : library_references(cache, trick)) {
        if (const auto found = cache.models.find(reference); found != cache.models.end() && !found->second.empty()) {
            for (auto model : found->second) {
                model.clip = index;
                model.own = false;
                native->models.push_back(std::move(model));
            }
            // Its time as the trick's, by where the board is (board_time_map): once for each clip of its, kept. Not for the
            // game's clip of the same pop: that is the trick's own timing, and the match only bent it.
            std::shared_ptr<const TimeMap> map;
            const auto pop = cache.reference_pops.find(reference);
            const bool same_pop = !made_pop.empty() && pop != cache.reference_pops.end() && pop->second == made_pop;
            if (const auto board = cache.reference_boards.find(reference); !same_pop && board != cache.reference_boards.end()) {
                auto& kept = cache.time_maps[key + "|" + reference];
                if (!kept) {
                    if (!made_track) made_track = board_track(*clip);
                    kept = std::make_shared<const TimeMap>(board_time_map(board->second, *made_track));
                }
                if (!kept->empty()) map = kept;
            }
            const bool other = whole_body && !made_pop.empty() && pop != cache.reference_pops.end() && !pop->second.empty() && pop->second != made_pop;
            const bool higher = other && pop_place(pop->second) > pop_place(made_pop) && pop_place(made_pop) >= 0;
            // A higher pop: the trick's own pace from the pop (1:1), not matched by the board - that held it still at its
            // catch through all the extra air, as if he floated (it is handed back to the game's pose from there instead).
            if (higher && found->second.front().up >= 0 && clip->takeoff >= 0) {
                auto pace = std::make_shared<TimeMap>();
                pace->from = {found->second.front().up};
                pace->to = {clip->takeoff};
                map = std::move(pace);
            }
            native->maps.push_back(std::move(map));
            native->other_pop.push_back(other);
            native->higher_pop.push_back(higher);
            ++index;
        }
    }
    return native;
}
} // namespace
// Every saved animation with a trick, ready to play in game, and how the playlist's tricks take turns. Read from the
// files, outside the lock, on the loader's thread.
struct LoadedTricks {
    std::shared_ptr<const CustomTricks> tricks;
    PlaylistMode mode{PlaylistMode::shuffle};
};
LoadedTricks read_custom_tricks(const std::string& test_trick) {
    LoadedTricks result;
    auto tricks = std::make_shared<CustomTricks>();
    result.tricks = tricks;
    std::error_code error;
    const auto folder = library_folder();
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return result;
    auto& cache = trick_cache();
    // The trick library rebuilt or refreshed: the game's clips are read again.
    std::shared_ptr<const std::vector<GameTrick>> catalog;
    try { catalog = game_tricks(); } catch (...) {}
    if (catalog != cache.catalog) {
        cache.catalog = catalog;
        cache.loaded.clear();
        cache.references.clear();
        // (These were kept across a rebuilt library: the boards, pops and maps of clips that were read again.)
        cache.reference_boards.clear();
        cache.reference_pops.clear();
        cache.time_maps.clear();
        cache.natives.clear();
        std::erase_if(cache.models, [](const auto& e) { return e.first.starts_with("library|"); });
    }
    // Select from small JSON metadata before expanding clips: an inactive pack variant must not take a whole animation's
    // memory or hold up a pack switch. Each file is brought up to date (upgrade_saved) and read only when it changed.
    std::vector<Preset> requested;
    std::map<std::string, std::filesystem::path> file_of;
    std::set<std::filesystem::path> seen;
    for (const auto& entry : saved_jsons(folder)) {
        seen.insert(entry);
        auto clip_path = entry;
        clip_path.replace_extension(L".clip");
        auto json_stamp = stamp_of(entry);
        const auto clip_stamp = stamp_of(clip_path);
        auto& meta = cache.meta[entry];
        if (!(meta.json == json_stamp && meta.clip == clip_stamp && meta.json.written)) {
            if (upgrade_saved(entry)) json_stamp = stamp_of(entry);
            meta = {json_stamp, clip_stamp, std::nullopt};
            try {
                std::ifstream in(entry, std::ios::binary);
                const auto animation = from_json(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
                if (!animation.trick.empty() && std::filesystem::is_regular_file(clip_path, error)) {
                    Preset p{entry.stem().string(), trick_key(animation.trick), animation.pop, false, false, animation.stance, animation.switch_stance, animation.entry};
                    p.fakie = animation.fakie;
                    p.leads_right = leads_right(authored_goofy(animation), animation.switch_stance.value_or(false));
                    p.saved_at = saved_time(entry);
                    meta.preset = std::move(p);
                }
            } catch (const std::exception& e) {
                logging::log(Level::warning, Channel::skater, "Animation preset metadata not loaded: {}: {}", entry.filename().string(), e.what());
            }
        }
        if (meta.preset) {
            requested.push_back(*meta.preset);
            file_of[meta.preset->name] = entry;
        }
    }
    std::erase_if(cache.meta, [&](const auto& e) { return !seen.contains(e.first); });
    try {
        // The playlist that is on says what plays; with none on, nothing does.
        const auto playlist = read_playlist();
        if (playlist.on) {
            select_playlist(requested, all_packs(), playlist.entries, playlist.mode);
            result.mode = playlist.mode;
        } else {
            // With no playlist on nothing of yours plays: packs and tricks play from playlists only.
            const std::set<std::string> none;
            select_presets(requested, std::span<const PresetPack>{}, nullptr, &none);
            result.mode = PlaylistMode::shuffle;
        }
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation pack not loaded: {}", e.what());
        return result;
    }
    // Test in game: that trick plays, on any pop, over whatever else there is of it in its own stance. Its other stance
    // versions (the same trick and pop made fakie, switch or switch fakie: make_stance_version) play in theirs, the newest
    // of each, so every stance is tried with its own version. (They were all stood down, and in switch the tested one
    // played mirrored instead of the switch version made for it.)
    if (!test_trick.empty())
        if (const auto t = std::find_if(requested.begin(), requested.end(), [&](const Preset& p) { return p.name == test_trick; }); t != requested.end()) {
            const auto stance_of = [](const Preset& p) { return std::pair<bool, bool>{preset_leads_right(p), p.fakie.value_or(false)}; };
            std::map<std::pair<bool, bool>, Preset*> others;
            for (auto& p : requested) {
                if (p.trick != t->trick) continue;
                p.active = false;
                if (p.name == t->name || p.pop != t->pop || stance_of(p) == stance_of(*t)) continue;
                auto& newest = others[stance_of(p)];
                if (!newest || p.saved_at > newest->saved_at) newest = &p;
            }
            t->active = true;
            t->pack_pop = std::string{};
            for (auto& [stance, p] : others) {
                p->active = true;
                p->pack_pop = std::string{};
            }
            logging::log(Level::info, Channel::skater, "Animation Studio: testing {} in game{}.", t->name,
                         others.empty() ? std::string{} : " (and its " + std::to_string(others.size()) + " other stance versions, each in its stance)");
        }
    // The ones that play, read whole: from memory when neither its files nor the pop it plays on changed.
    std::set<std::filesystem::path> used;
    std::size_t read_now = 0;
    for (const auto& p : requested) {
        if (!p.active) continue;
        const auto file = file_of.find(p.name);
        if (file == file_of.end()) continue;
        const auto& entry = file->second;
        const auto& meta = cache.meta[entry];
        const std::string pop = p.pack_pop ? "pack:" + *p.pack_pop : std::string("own");
        auto& loaded = cache.loaded[entry];
        used.insert(entry);
        if (!loaded.trick.clip || !(loaded.json == meta.json) || !(loaded.clip == meta.clip) || loaded.pop != pop) {
            loaded = {};
            try {
                std::vector<CustomTrick> heights;
                auto trick = load_custom_trick(cache, entry, p, &heights);
                if (!trick) continue;
                loaded = {meta.json, meta.clip, pop, std::move(*trick), std::move(heights)};
                ++read_now;
            } catch (const std::exception& e) {
                logging::log(Level::warning, Channel::skater, "Animation Studio: {} was not loaded: {}", entry.filename().string(), e.what());
                continue;
            }
        }
        auto trick = loaded.trick;
        trick.order = p.order;
        tricks->push_back(std::move(trick));
        for (auto version : loaded.heights) {
            version.order = p.order;
            tricks->push_back(std::move(version));
        }
    }
    // What plays no more lets go of its memory: its trick, its clip when no other trick has it, its models.
    std::erase_if(cache.loaded, [&](const auto& e) { return !used.contains(e.first); });
    std::erase_if(cache.clips, [](const auto& e) { return e.second.use_count() == 1; });
    std::erase_if(cache.models, [](const auto& e) {
        return !e.first.starts_with("library|") && (e.second.empty() || e.second.front().frames.use_count() == 1);
    });
    std::sort(tricks->begin(), tricks->end(), [](const CustomTrick& a, const CustomTrick& b) { return a.name < b.name; });
    std::map<std::string, std::size_t> kinds;
    for (const auto& t : *tricks) ++kinds[t.trick];
    std::string names;
    for (const auto& [trick, count] : kinds) names += (names.empty() ? "" : ", ") + trick + (count > 1 ? " x" + std::to_string(count) : std::string{});
    logging::log(Level::info, Channel::skater, "Animation Studio: {} custom tricks ready ({} read from disk, {} clips in memory){}{}.", tricks->size(),
                 read_now, cache.clips.size(), names.empty() ? "" : ": ", utf8_prefix(names, 2000));
    return result;
}

// Publishes what the animation thread works from. Caller holds the mutex.
// Which way round the studio shows a trick. One with a foot of its own - a recording, a stance version - is shown on
// it, as its STANCES tick says and as it plays: a switch one shows him riding switch, and it is saved as it is. A
// trick of the game's own library has no foot of its own (the game plays its clips either way round): it is turned
// into the stance the studio is set to - its clip, its keys and the undo history, the game's way - rather than only
// being drawn mirrored (drawn mirrored, the joint picked as the left leg was the clip's right one, so turning the left
// leg moved the right), and saved in that stance. Mirroring is exact and undoes itself, the file is not marked changed
// by it, and playback reads the lead foot from the clip. Returns whether it mirrored.
bool face_studio_stance(State& s) {
    if (!s.open || !s.clip || s.clip->empty() || s.clip->joints != skeleton_joints) return false;
    const auto lead = clip_leads_right(*s.clip);
    const bool own_foot = s.edits.source.kind != "native_asset";
    if (own_foot && !s.edits.keep_leads_right) return false; // a recording: as it was made
    const bool want_right = s.edits.keep_leads_right ? *s.edits.keep_leads_right : studio_goofy(s.stance_choice); // goofy: the right foot leads
    if (!lead || *lead == want_right) return false;
    try {
        auto clip = std::make_shared<const Clip>(mirror_clip(*s.clip));
        const auto mirror_tracks = [](std::vector<Track> tracks) {
            Animation only;
            only.tracks = std::move(tracks);
            return mirror_animation(only).tracks;
        };
        Animation edits = s.edits;
        const auto metadata = edits.clip;
        edits.clip = {};
        edits = mirror_animation(edits);
        edits.clip = metadata;
        // Its labels follow the clip, so a pack files it under the stance it now leads with.
        edits.switch_stance = authored_goofy(edits) != want_right;
        for (auto& step : s.undo) step.tracks = mirror_tracks(std::move(step.tracks));
        for (auto& step : s.redo) step.tracks = mirror_tracks(std::move(step.tracks));
        s.clip = std::move(clip);
        s.edits = std::move(edits);
        s.status = std::string("Shown in ") + (want_right ? "goofy" : "regular") +
                   " stance (SETTINGS > SKATER > Editing stance), mirrored the way the game mirrors. Left is left: edit as you see it.";
        logging::log(Level::info, Channel::skater, "Animation Studio: {} mirrored into the studio's {} stance for editing.",
                     s.edits.name.empty() ? std::string("the trick") : s.edits.name, want_right ? "goofy" : "regular");
        return true;
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation Studio: could not mirror the trick into the studio's stance: {}", e.what());
        return false;
    }
}
void publish_locked(State& s, bool edits_changed = true) {
    if (face_studio_stance(s)) edits_changed = true;
    overrides_allowed.store(s.open || s.play_in_game, std::memory_order_release);
    auto snap = std::make_shared<Snapshot>();
    snap->clip = s.clip;
    snap->idle = s.idle_clip;
    snap->tricks = s.tricks;
    if (edits_changed || !s.published_edits) s.published_edits = std::make_shared<const Animation>(s.edits);
    snap->edits = s.published_edits;
    snap->time = s.time;
    snap->peek_clip = s.peek_clip;
    snap->peek_edits = s.peek_edits;
    snap->peek_since = s.peek_since;
    snap->open = s.open;
    snap->play_in_game = s.play_in_game;
    // The stance the studio is set to. Standing him the other way round is done by turning him about on
    // the board, not by mirroring the clip: mirroring was tried twice, over all 395 joints and over the
    // body alone, and both tore the skater up on screen. Turning the placement cannot do that - it is one
    // joint's rotation - and it is also what the stance is: the same body at the other end of the board,
    // the left foot where the right one was.
    {
        const bool goofy = studio_goofy(s.stance_choice);
        s.showing_goofy = goofy;
        snap->goofy = goofy;
        snap->grab = s.grab_board;
        snap->front_foot = s.front_foot;
        snap->limits = s.limits;
        snap->crossfade = s.crossfade;
    }
    snap->room = s.room;
    snap->speed = s.edits.speed;
    s.snapshot = std::move(snap);
    ++s.revision;
}
std::shared_ptr<const Snapshot> snapshot() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.snapshot;
}
float duration_locked(const State& s) { return s.clip ? s.clip->duration() : 0.0f; }
// The brackets' keys stay at nothing - 0, 0, 0 - on every joint, so the edits always come in from the game's own
// pose at the start bracket and go back to it at the end: the curves start and end on 0 and never jump. A change
// made on a bracket's frame is taken back off its key. Returns how many keys it put back.
std::size_t pin_brackets_locked(State& s) {
    if (!s.clip || s.clip->empty()) return 0;
    const float start = s.edits.start, end = s.edits.end < 0 ? s.clip->duration() : s.edits.end;
    std::size_t pinned = 0;
    for (auto& t : s.edits.tracks)
        for (auto& k : t.keys) {
            if (std::abs(k.time - start) > 0.5f / 60 && std::abs(k.time - end) > 0.5f / 60) continue;
            if (k.rotation == Quat{0, 0, 0, 1} && k.offset == Vec3{}) continue;
            k.rotation = {0, 0, 0, 1};
            k.offset = {};
            ++pinned;
        }
    return pinned;
}
void changed_locked(State& s) {
    if (pin_brackets_locked(s))
        s.status = "The brackets' keys stay at 0, 0, 0, so your animation comes in from the game's pose and goes back to it: "
                   "pose between the brackets.";
    s.dirty = true;
    publish_locked(s);
}

// ---- the camera --------------------------------------------------------------------------------
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 unit(const Vec3& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 1e-6f ? Vec3{v[0] / l, v[1] / l, v[2] / l} : Vec3{0, 0, 1};
}
// The skater's facing about the up axis, from its placement joint: the angle of its forward on the
// ground. Measured 2026-10-18 by orbiting a recorded trick: a camera on the joint's -Z side shows the
// skater's left side, a quarter turn on his back; so the front is a quarter turn the other way.
float facing_yaw(const Quat& facing) {
    const auto forward = rotate(facing, {0, 0, -1});
    return std::atan2(forward[0], forward[2]) - 1.5707963f;
}
std::array<float, 16> orbit_matrix(const Vec3& centre, const Orbit& orbit, float base_yaw) {
    const float yaw = base_yaw + orbit.yaw;
    const float pitch = std::clamp(orbit.pitch, -1.45f, 1.45f);
    const Vec3 back{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 focus{centre[0] + orbit.pan[0], centre[1] + orbit.pan[1], centre[2] + orbit.pan[2]};
    const float distance = std::clamp(orbit.distance, 0.6f, 25.0f);
    const auto right = unit(cross({0, 1, 0}, back));
    const auto up = cross(back, right);
    std::array<float, 16> m{};
    for (std::size_t i = 0; i < 3; ++i) {
        m[i] = right[i];
        m[4 + i] = up[i];
        m[8 + i] = back[i];
        m[12 + i] = focus[i] + back[i] * distance;
    }
    m[15] = 1;
    return m;
}

// ---- the game's tricks ---------------------------------------------------------------------------
// Goes up when the trick lists change, so their caches are read again.
std::atomic<std::uint64_t> tricks_revision{1};
// Published on the client thread: animation callbacks must not lock AI/session state. Whether the game's pop grades are
// heard (the hook is on), and whether he is alone in the game (no session, no AI skaters: every grade is his).
std::atomic<bool> grades_heard{}, grades_solo{};

// A bracket is a key on every body joint at its time, with no turn or move of its own: the start and the end
// of the animation, keyed on all limbs at once.
void key_all_at(Animation& animation, float seconds) {
    for (const auto& joint : editable_joints()) {
        // The board as well: an empty offset turns and moves it by nothing, so the key holds it where the
        // trick already has it (board_edited) and the brackets anchor the board like any limb.
        const auto* track = animation.track(joint.index);
        if (track && track->locked) continue;
        bool has = false;
        if (track)
            for (const auto& key : track->keys) has |= std::abs(key.time - seconds) <= 1.0f / 120;
        if (!has) set_key(animation, joint.index, seconds, Offset{});
    }
}
// The brackets at the ends of the clip, with their keys on every joint: whenever a clip arrives.
void reset_brackets_locked(State& s) {
    if (!s.clip || s.clip->empty()) return;
    ++s.clip_serial; // a clip has arrived: the overlay clears what it was holding
    s.edits.speed = 1; // the editor always opens a trick at x1
    s.edits.start = 0;
    s.edits.end = -1;
    key_all_at(s.edits, 0);
    key_all_at(s.edits, s.clip->duration());
}
// The local skater's physics state is the one the header declares (defined further down): in the air on the
// board it is 200 or 201, on foot 500 to 599, grinding 400 to 499.
std::optional<bool> in_the_air(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto value = physics_state(base, component);
    if (!value) return std::nullopt;
    return std::find(addr::skater_state::board_air_states.begin(), addr::skater_state::board_air_states.end(), *value) !=
           addr::skater_state::board_air_states.end();
}
// The game's name for the trick the skater is doing ("ollie", "kickflip", "heelflip"): the animation
// holder (component +0xa0) -> +0x20 -> +0x250 -> +0x98, a C string. Found 2026-10-18 by walking the
// holder's objects mid-trick; right from 0.05 s after takeoff and through the whole air, the same on
// six tricks of three kinds. Empty when the chain or the text does not look right.
std::string current_trick(std::uintptr_t component) noexcept {
    const auto holder = read<std::uintptr_t>(component + 0xa0).value_or(0);
    const auto a = holder ? read<std::uintptr_t>(holder + 0x20).value_or(0) : 0;
    const auto b = a ? read<std::uintptr_t>(a + 0x250).value_or(0) : 0;
    const auto c = b ? read<std::uintptr_t>(b + 0x98).value_or(0) : 0;
    char text[49]{};
    if (!c || !peek(c, text, 48)) return {};
    std::string name;
    for (std::size_t i = 0; i < 48 && text[i]; ++i) {
        const char ch = text[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ' ' || ch == '-')) return {};
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return name.size() >= 2 ? trick_key(name) : std::string{}; // one name however the game spells it
}
void publish_joints(std::span<const Joint> pose, const Joint& placement, const std::optional<Joint>& board, const Quat& board_frame) {
    std::vector<JointView> views;
    views.reserve(editable_joints().size());
    for (const auto& e : editable_joints()) {
        JointView v;
        if (e.index == board_joint) {
            // The board: its keys turn and move it in the frame of its start (start_frame).
            if (!board) continue;
            v.joint = board_joint;
            v.position = board->position;
            v.rotation = board->rotation;
            v.parent_rotation = board_frame;
            views.push_back(v);
            continue;
        }
        v.joint = e.index;
        const auto w = world(pose, e.index);
        v.position = w.position;
        v.rotation = w.rotation;
        if (const auto p = parent(e.index); p >= 0) {
            const auto pw = world(pose, static_cast<std::uint16_t>(p));
            v.parent_rotation = pw.rotation;
            v.parent_scale = pw.scale[0];
        }
        views.push_back(v);
    }
    const auto hips = world(pose, hips_joint);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.joints = std::move(views);
    s.joints_placement = placement;
    // The orbit centres where the skater stands, at the height of the hips as drawn, so a trick's rise
    // stays in the picture (camera() eases towards it).
    s.centre = {placement.position[0], hips.position[1] + 0.05f, placement.position[2]};
    s.facing = placement.rotation;
    s.centre_valid = std::isfinite(hips.position[0]);
    s.pose_seen_at = now_seconds();
}
} // namespace

// The board's upward speed (m/s): its physics' root body (skater_state.h: core +0x430 -> +0x18 ->
// bodies at +0x20, body 0 the root, velocity at +0x70).
std::optional<std::array<float, 3>> board_velocity(std::uintptr_t base, std::uintptr_t component) noexcept; // below
std::optional<float> board_up_speed(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto v = board_velocity(base, component);
    if (!v) return std::nullopt;
    return (*v)[1];
}
// The board's body's speed (metres a second, the world's axes), or none.
std::optional<std::array<float, 3>> board_velocity(std::uintptr_t base, std::uintptr_t component) noexcept {
    namespace st = addr::skater_state;
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    const auto holder = core ? read<std::uintptr_t>(core + st::board_holder_offset).value_or(0) : 0;
    const auto physics = holder ? read<std::uintptr_t>(holder + st::board_physics_offset).value_or(0) : 0;
    if (!physics || read<std::uintptr_t>(physics).value_or(0) != base + st::board_physics_vtable) return std::nullopt;
    const auto bodies = read<std::uintptr_t>(physics + st::physics_bodies_offset).value_or(0);
    if (!bodies || read<std::uint32_t>(bodies).value_or(0) != st::board_body_count) return std::nullopt;
    if (read<std::uintptr_t>(bodies + st::body_owner_offset).value_or(0) != physics) return std::nullopt;
    const auto v = read<std::array<float, 3>>(bodies + st::body_velocity_offset);
    if (!v || !std::isfinite((*v)[0]) || !std::isfinite((*v)[1]) || !std::isfinite((*v)[2])) return std::nullopt;
    return v;
}
// The pop of an air, from how long it is going to last: low, mid or high. Only when the game's grade is not known.
std::string pop_of(float air) {
    if (!(air > 0)) return {};
    return air < 0.45f ? "low" : air < 0.65f ? "mid" : "high";
}
namespace {
struct Research {
    std::mutex mutex;
    bool active{};
    double started{};
    ResearchBuffer buffer;
    std::string last_file;
};
Research& research() { static Research value; return value; }
void research_tick(std::uintptr_t base, std::uintptr_t component, bool open) {
    auto& r = research();
    bool finished = false;
    {
        std::lock_guard lock(r.mutex);
        if (!r.active) return;
        ResearchSample sample;
        sample.seconds = now_seconds() - r.started;
        if (base && component) {
            sample.physics = physics_state(base, component);
            sample.trick = current_trick(component);
            sample.board_up_speed = board_up_speed(base, component);
            sample.switch_stance = switch_stance(base, component);
            sample.left_foot_leads = left_foot_leads(base, component);
        }
        sample.animation_updates = live().animation_updates.load(std::memory_order_acquire);
        {
            auto& shot = render_shot();
            std::lock_guard shot_lock(shot.mutex);
            sample.skater_publications = shot.skater_publications;
            sample.board_publications = shot.board_publications;
            const auto generation = live().generation.load(std::memory_order_acquire);
            const double now = now_seconds();
            if (shot.at >= 0 && shot.skater_generation == generation) sample.skater_age = std::max(0.0, now - shot.at);
            if (shot.board_at >= 0 && shot.board_generation == generation) sample.board_age = std::max(0.0, now - shot.board_at);
        }
        sample.studio_open = open;
        sample.goofy = goofy_stance();
        if (const auto snap = snapshot()) {
            sample.custom_enabled = snap->play_in_game;
        }
        finished = !r.buffer.append(std::move(sample));
    }
    if (finished) logging::write(Level::info, Channel::skater, research_trace("stop"));
}
} // namespace

// ---- where the game keeps the clip it is playing --------------------------------------------------
// The game says which trick it is doing (current_trick), never which of its own clips it is playing - and
// most of its animation is not tricks at all: setups before the pop, loops while riding, exits, the pieces
// the graph blends between. To edit one of those and have it show in game, the studio has to see it
// playing, and nobody has found yet where the game keeps that. This looks.
//
// From the skater's animation component, and from each step of the chain the trick's name hangs off, it
// follows pointers a few steps out. In everything it reads it looks for the names of the game's clips: as
// text, and as the FNV-1 hash the engine names things by, in each spelling it might keep. What it finds and
// the path to it are written out, taken riding, at the pop and in the air, so a single trick done with it
// armed says where to read the playing clip from. Reads only, through peek, so a wrong guess about what a
// pointer is costs nothing.
namespace live_probe {
// Second version. The first read 3000 objects out from the component and found the same 23 clip names at
// every moment - names of clips the game has loaded, not the one it is playing - and stopped at its limit
// each time. So this one goes further (more objects, more steps out, from the skater's entity too), looks
// for the names hashed more ways, and finds what points AT each place a clip name was seen. Above all it
// compares the moments: a place whose clip changes between riding, the pop and the air is what is
// playing, and the report puts those first.
std::atomic<bool> armed{};
std::atomic<int> taken{}; // bit 0 riding, 1 at the pop, 2 early in the air, 3 later in the air
constexpr int all_moments = 0xF;
std::mutex names_mutex;
struct Names {
    std::unordered_map<std::uint32_t, std::string> by32;
    std::unordered_map<std::uint64_t, std::string> by64;
};
std::shared_ptr<const Names> names;
std::uint32_t fnv1(std::string_view text) noexcept {
    std::uint32_t h = 0x811C9DC5u;
    for (const unsigned char c : text) { h *= 0x01000193u; h ^= c; }
    return h;
}
std::uint32_t fnv1a(std::string_view text) noexcept {
    std::uint32_t h = 0x811C9DC5u;
    for (const unsigned char c : text) { h ^= c; h *= 0x01000193u; }
    return h;
}
std::uint64_t fnv1_64(std::string_view text) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const unsigned char c : text) { h *= 0x100000001B3ull; h ^= c; }
    return h;
}
std::uint64_t fnv1a_64(std::string_view text) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const unsigned char c : text) { h ^= c; h *= 0x100000001B3ull; }
    return h;
}
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string hex(std::uintptr_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}
bool clip_text(const std::string& text) {
    const auto low = lower(text);
    return low.find("proto_") != std::string::npos || low.find("animation/dingo") != std::string::npos ||
           low.find("_onb_") != std::string::npos || low.find("_ofb_") != std::string::npos || low.find("nollie") != std::string::npos;
}
// What one moment saw: at each place (the path of pointers from a root), which clip name.
struct Moment {
    std::string when, trick;
    std::size_t objects{}, ms{};
    std::map<std::string, std::string> at;
};
std::vector<Moment> moments; // the animation thread only, between arming and the last moment
Moment scan(std::uintptr_t base, std::uintptr_t component, const std::string& when) {
    std::shared_ptr<const Names> known;
    {
        std::lock_guard lock(names_mutex);
        known = names;
    }
    Moment m;
    m.when = when;
    m.trick = current_trick(component);
    struct Node {
        std::uintptr_t at;
        std::uint32_t path; // index into paths
        int depth;
    };
    std::vector<std::string> paths;
    std::deque<Node> queue;
    const auto root = [&](std::uintptr_t at, const char* name) {
        if (!at) return;
        paths.emplace_back(name);
        queue.push_back({at, static_cast<std::uint32_t>(paths.size() - 1), 0});
    };
    const auto holder = read<std::uintptr_t>(component + 0xa0).value_or(0);
    const auto a = holder ? read<std::uintptr_t>(holder + 0x20).value_or(0) : 0;
    const auto b = a ? read<std::uintptr_t>(a + 0x250).value_or(0) : 0;
    root(b, "trick");
    root(a, "component+0xa0+0x20");
    root(holder, "component+0xa0");
    root(component, "component");
    root(base, "entity");
    constexpr int deepest = 6;
    constexpr std::size_t most_objects = 60000, span = 0x400;
    const double started = now_seconds();
    std::unordered_map<std::uintptr_t, std::uint32_t> seen; // object -> its path
    std::vector<std::uintptr_t> visited;
    std::vector<std::pair<std::uintptr_t, std::string>> named; // where a name was found, and the name
    std::vector<std::uint8_t> bytes(span);
    while (!queue.empty() && visited.size() < most_objects && now_seconds() - started < 0.6) {
        const auto node = queue.front();
        queue.pop_front();
        if (seen.count(node.at) || !peek(node.at, bytes.data(), span)) continue;
        seen.emplace(node.at, node.path);
        visited.push_back(node.at);
        const std::string here = paths[node.path];
        if (known)
            for (std::size_t o = 0; o + 4 <= span; o += 4) {
                std::uint32_t v;
                std::memcpy(&v, bytes.data() + o, 4);
                if (v == 0) continue;
                if (const auto hit = known->by32.find(v); hit != known->by32.end()) {
                    m.at[here + " +" + hex(o) + " (hash32)"] = hit->second;
                    named.emplace_back(node.at + o, hit->second);
                }
                if (o % 8 == 0 && o + 8 <= span) {
                    std::uint64_t w;
                    std::memcpy(&w, bytes.data() + o, 8);
                    if (const auto hit = known->by64.find(w); hit != known->by64.end()) {
                        m.at[here + " +" + hex(o) + " (hash64)"] = hit->second;
                        named.emplace_back(node.at + o, hit->second);
                    }
                }
            }
        for (std::size_t o = 0; o < span;) {
            std::size_t end = o;
            while (end < span && bytes[end] >= 0x20 && bytes[end] < 0x7f) ++end;
            if (end - o >= 8) {
                const std::string text(reinterpret_cast<const char*>(bytes.data() + o), end - o);
                if (clip_text(text)) {
                    m.at[here + " +" + hex(o) + " (text)"] = text;
                    named.emplace_back(node.at + o, text);
                }
            }
            o = end + 1;
        }
        if (node.depth < deepest)
            for (std::size_t o = 0; o + 8 <= span; o += 8) {
                std::uint64_t value;
                std::memcpy(&value, bytes.data() + o, 8);
                if (value >= 0x10000 && value <= highest_user && (value & 7) == 0 && !seen.count(static_cast<std::uintptr_t>(value))) {
                    paths.push_back(here + " +" + hex(o));
                    queue.push_back({static_cast<std::uintptr_t>(value), static_cast<std::uint32_t>(paths.size() - 1), node.depth + 1});
                }
            }
    }
    // Everything read that points at a place a name was seen, or a little before it (the start of the
    // thing the name belongs to). A graph node playing a clip holds a pointer to the clip, not its name.
    if (!named.empty()) {
        std::sort(named.begin(), named.end());
        for (const auto at : visited) {
            if (now_seconds() - started > 1.0) break;
            if (!peek(at, bytes.data(), span)) continue;
            for (std::size_t o = 0; o + 8 <= span; o += 8) {
                std::uint64_t value;
                std::memcpy(&value, bytes.data() + o, 8);
                if (value < 0x10000 || value > highest_user) continue;
                const auto hit = std::lower_bound(named.begin(), named.end(),
                                                  std::make_pair(static_cast<std::uintptr_t>(value), std::string{}));
                if (hit == named.end() || hit->first - value >= span) continue;
                m.at[paths[seen[at]] + " +" + hex(o) + " -> +" + hex(hit->first - value)] = hit->second;
            }
        }
    }
    m.objects = visited.size();
    m.ms = static_cast<std::size_t>((now_seconds() - started) * 1000);
    return m;
}
void write_report() {
    std::ostringstream out;
    out << "# Animation Studio live probe v2. Places whose clip CHANGED between the moments come first: those\n"
        << "# are what the game is playing. Then each moment in full.\n\n";
    std::map<std::string, std::vector<std::string>> by_place; // place -> what each moment had there
    for (std::size_t i = 0; i < moments.size(); ++i)
        for (const auto& [place, name] : moments[i].at) {
            auto& row = by_place[place];
            row.resize(moments.size());
            row[i] = name;
        }
    std::size_t changed = 0;
    out << "== changed between moments (";
    for (std::size_t i = 0; i < moments.size(); ++i) out << (i ? " | " : "") << moments[i].when;
    out << ")\n";
    for (auto& [place, row] : by_place) {
        row.resize(moments.size());
        bool differs = false;
        for (const auto& name : row) differs |= name != row[0];
        if (!differs) continue;
        if (++changed > 1500) break;
        out << place << '\n';
        for (std::size_t i = 0; i < row.size(); ++i) out << "    " << moments[i].when << ": " << (row[i].empty() ? "-" : row[i]) << '\n';
    }
    if (!changed) out << "(nothing changed)\n";
    for (const auto& m : moments) {
        out << "\n== " << m.when << "\n# trick the game reports: " << (m.trick.empty() ? "(none)" : m.trick) << "\n# objects read: "
            << m.objects << " in " << m.ms << " ms, places with a clip name: " << m.at.size()
            << (names ? "" : "\n# no clip names to look for: the native animation index is not installed") << '\n';
        std::size_t n = 0;
        for (const auto& [place, name] : m.at) {
            if (++n > 600) {
                out << "... and " << m.at.size() - 600 << " more\n";
                break;
            }
            out << place << "  " << name << '\n';
        }
    }
    try {
        const auto folder = library_folder() / L"research";
        std::filesystem::create_directories(folder);
        std::ofstream file(folder / "live-graph-probe.txt", std::ios::binary | std::ios::trunc);
        file << out.str();
    } catch (...) {}
}
void take(std::uintptr_t base, std::uintptr_t component, int which, const std::string& when) {
    if (taken.fetch_or(1 << which) & (1 << which)) return;
    moments.push_back(scan(base, component, when));
    write_report();
    logging::log(Level::info, Channel::skater, "Animation Studio: probe taken {} ({} objects, {} ms).", when, moments.back().objects,
                 moments.back().ms);
    if (taken.load() == all_moments) {
        armed.store(false);
        logging::log(Level::info, Channel::skater, "Animation Studio: probe done; see ReSkate.Tricks/research/live-graph-probe.txt.");
    }
}
} // namespace live_probe
std::string arm_live_probe() {
    // The names of every clip the game has, hashed every way the engine might keep them. Done here, on the
    // console's thread, so the animation thread only ever looks things up.
    // Not while a probe is being taken: the animation thread owns the moments until it is done.
    if (live_probe::armed.load()) return "A probe is already armed: ride for two seconds, then do one trick.";
    auto names = std::make_shared<live_probe::Names>();
    if (const auto assets = native_assets())
        for (const auto& asset : *assets) {
            const std::string& full = asset.asset; // the index keeps them lower case: animation/dingo/...
            const auto slash = full.rfind('/');
            const std::string short_name = slash == std::string::npos ? full : full.substr(slash + 1);
            std::string cased = full;
            if (cased.starts_with("animation/dingo/")) cased = "Animation/Dingo/" + cased.substr(16); // as the game spells it
            for (const auto& spelling : {full, short_name, cased}) {
                names->by32.emplace(live_probe::fnv1(spelling), full);
                names->by32.emplace(live_probe::fnv1a(spelling), full);
                names->by64.emplace(live_probe::fnv1_64(spelling), full);
                names->by64.emplace(live_probe::fnv1a_64(spelling), full);
            }
        }
    const auto spellings = names->by32.size() + names->by64.size();
    {
        std::lock_guard lock(live_probe::names_mutex);
        live_probe::names = std::move(names);
    }
    try { std::filesystem::remove(library_folder() / L"research" / "live-graph-probe.txt"); } catch (...) {}
    live_probe::moments.clear();
    live_probe::taken.store(0);
    live_probe::armed.store(true);
    return "Probe armed (" + std::to_string(spellings) +
           " clip-name hashes). Ride for two seconds, then do one trick and land it. It is taken riding, at the pop "
           "and twice in the air (the game may hitch for a moment each time) and written to "
           "ReSkate.Tricks/research/live-graph-probe.txt.";
}

std::string research_trace(const std::string& action) {
    auto& r = research();
    std::lock_guard lock(r.mutex);
    if (action == "start") {
        if (r.active) return "Animation trace is already running. Use animstudio trace stop to export it.";
        if (!r.buffer.samples.empty()) return "An unexported trace is retained. Use animstudio trace stop to retry export first.";
        r.buffer.samples.reserve(ResearchBuffer::capacity);
        r.started = now_seconds();
        r.active = true;
        research_capture_flag.store(true, std::memory_order_release);
        return "Animation trace started (60 seconds). Skate normally; animstudio trace stop exports early.";
    }
    if (action == "status")
        return std::format("Animation trace: {}, {} samples. Last export: {}", r.active ? "running" : "stopped",
                           r.buffer.samples.size(), r.last_file.empty() ? "none" : r.last_file);
    if (action != "stop") return "Usage: animstudio trace start|stop|status";
    if (!r.active && r.buffer.samples.empty()) return "No animation trace to export.";
    r.active = false;
    research_capture_flag.store(false, std::memory_order_release);
    try {
        const auto folder = library_folder() / L"research";
        std::filesystem::create_directories(folder);
        const auto path = folder / ("transitions-" + std::to_string(GetTickCount64()) + ".csv");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "# animator_trace_schema=3\n# game_sha256=" << supported_build::game_sha256 << '\n';
        out << "seconds,physics_state,phase_hint,trick_name,board_up_mps,studio_open,custom_playback_enabled,goofy_setting,recording_armed,switch_stance,left_foot_leads,animation_updates,skater_publications,board_publications,skater_age_s,board_age_s\n";
        for (const auto& s : r.buffer.samples) {
            const bool air = s.physics && (*s.physics == 200 || *s.physics == 201);
            const bool rail = s.physics && *s.physics >= 400 && *s.physics < 500;
            out << std::format("{:.6f},", s.seconds);
            if (s.physics) out << *s.physics;
            out << ',' << (!s.physics ? "unavailable" : air ? "air" : rail ? "rail_range" : "other") << ',' << s.trick << ',';
            if (s.board_up_speed) out << *s.board_up_speed;
            out << ',' << s.studio_open << ',' << s.custom_enabled << ',' << s.goofy << ',' << s.recording_armed << ',';
            if (s.switch_stance) out << *s.switch_stance;
            out << ',';
            if (s.left_foot_leads) out << *s.left_foot_leads;
            out << ',' << s.animation_updates << ',' << s.skater_publications << ',' << s.board_publications << ',' << s.skater_age << ',' << s.board_age << '\n';
        }
        out.close();
        if (!out) throw std::runtime_error("could not write trace");
        r.last_file = path.string();
        r.buffer.samples.clear();
        return "Animation trace exported: " + r.last_file;
    } catch (const std::exception& e) {
        return std::string("Animation trace export failed (samples kept; retry stop): ") + e.what();
    }
}
// The board's edit at `seconds`, in the frame of its start (`ground`, that frame's turn): moved along its X, Y
// and Z, and turned about them, whatever the board does in the trick.

// A hand that comes near the deck closes on it. The fingers are bent about the axis their own knuckles
// turn on, worked out from the hand itself rather than assumed, and the way round is settled by trying it:
// whichever way brings the fingertip nearer the board is the way a hand closes. Nothing here is keyed, so
// a grab is what the studio shows, never something saved into the animation.
//
// The deck, in its mount's frame: flat between the trucks, rising to the nose and the tail.
constexpr float deck_half_across = 0.105f, deck_half_thick = 0.020f, deck_half_along = 0.430f;
constexpr float deck_flat_to = 0.165f, deck_kick = 0.075f;
Vec3 nearest_on_deck(const Joint& board, const Vec3& point) {
    std::array<Vec3, 3> axes{};
    for (int i = 0; i < 3; ++i) {
        Vec3 e{};
        e[static_cast<std::size_t>(i)] = 1;
        axes[static_cast<std::size_t>(i)] = rotate(board.rotation, e);
    }
    const Vec3 from{point[0] - board.position[0], point[1] - board.position[1], point[2] - board.position[2]};
    std::array<float, 3> along{};
    for (int i = 0; i < 3; ++i) {
        const auto& a = axes[static_cast<std::size_t>(i)];
        along[static_cast<std::size_t>(i)] = from[0] * a[0] + from[1] * a[1] + from[2] * a[2];
    }
    const float out = std::clamp((std::abs(along[2]) - deck_flat_to) / std::max(deck_half_along - deck_flat_to, 1e-3f), 0.0f, 1.0f);
    const float kick = out * out * deck_kick;
    along[0] = std::clamp(along[0], -deck_half_across, deck_half_across);
    along[1] = std::clamp(along[1], -deck_half_thick, deck_half_thick + kick);
    along[2] = std::clamp(along[2], -deck_half_along, deck_half_along);
    Vec3 on = board.position;
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) on[c] += axes[static_cast<std::size_t>(i)][c] * along[static_cast<std::size_t>(i)];
    return on;
}
float apart(const Vec3& a, const Vec3& b) noexcept {
    return std::hypot(std::hypot(a[0] - b[0], a[1] - b[1]), a[2] - b[2]);
}
// Turns every joint of a finger about `axis` by `radians`, in the world, leaving the pose parent-local.
void bend_finger(std::span<Joint> pose, const std::array<std::uint16_t, 4>& finger, const Vec3& axis, float radians) {
    const auto turn = axis_angle(axis, radians);
    for (const auto joint : finger) {
        if (!joint || joint >= pose.size()) continue;
        const auto up = parent(joint);
        if (up < 0) continue;
        const auto parent_world = world(pose, static_cast<std::uint16_t>(up)).rotation;
        pose[joint].rotation = normalised(multiply(multiply(conjugate(parent_world), multiply(turn, parent_world)), pose[joint].rotation));
    }
}
void close_hand(std::span<Joint> pose, std::uint16_t hand, const std::array<std::array<std::uint16_t, 4>, 5>& fingers,
                const Joint& board) {
    if (hand >= pose.size()) return;
    const auto hand_at = world(pose, hand).position;
    const auto near_board = nearest_on_deck(board, hand_at);
    // Open at arm's length, closed when the hand is on it.
    constexpr float reach = 0.26f, held = 0.09f;
    const float amount = std::clamp((reach - apart(hand_at, near_board)) / (reach - held), 0.0f, 1.0f);
    if (amount <= 0.01f) return;
    for (const auto& finger : fingers) {
        std::uint16_t tip = 0;
        for (const auto joint : finger) if (joint && joint < pose.size()) tip = joint;
        if (!finger[0] || !tip || finger[0] >= pose.size()) continue;
        const auto root = world(pose, finger[0]).position;
        const auto tip_at = world(pose, tip).position;
        const Vec3 bone{tip_at[0] - root[0], tip_at[1] - root[1], tip_at[2] - root[2]};
        const Vec3 to_board{near_board[0] - tip_at[0], near_board[1] - tip_at[1], near_board[2] - tip_at[2]};
        // The knuckles turn about the line across the finger: along neither the bone nor the way it must go.
        Vec3 axis{bone[1] * to_board[2] - bone[2] * to_board[1], bone[2] * to_board[0] - bone[0] * to_board[2],
                  bone[0] * to_board[1] - bone[1] * to_board[0]};
        const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (length < 1e-5f) continue;
        for (auto& c : axis) c /= length;
        // Which way round closes it: the way that brings the tip nearer the board.
        const float step = 0.35f * amount;
        std::vector<Joint> tried(pose.begin(), pose.end());
        bend_finger(tried, finger, axis, step);
        const float closer = apart(world(tried, tip).position, near_board);
        bend_finger(pose, finger, axis, closer < apart(tip_at, near_board) ? step : -step);
    }
}

// The board under the feet, as when riding. The game's clips leave the board where the animators had
// it, and the game's foot placement puts the feet on it: before the pop and after the landing their
// feet sink into the board or float over it by up to 6 cm (measured 2026-10-18 in the kickflip and
// the ollie). Riding, the feet (joints 343 and 10) are 0.104 m above the board's mount. So on the
// ground the board goes that far under the lower foot; in the air it is the clip's own, with a short
// blend at the pop and the landing. `pose` is the clip's pose at `seconds`, `board` its board, both
// under the placement joint.
BoardShape board_shape(const Clip& clip);
Vec3 board_up(const Clip& clip);
constexpr std::uint16_t left_foot_joint = 343, right_foot_joint = 10;
float on_ground_share(const Clip& clip, float seconds) noexcept {
    const float up = clip.takeoff, down = clip.landing;
    if (up < 0 || down <= up) return 1;
    if (seconds <= up) return 1;
    if (seconds >= down) return 1;
    if (seconds < up + board_blend) return 1 - (seconds - up) / board_blend;
    if (seconds > down - board_blend) return (seconds - (down - board_blend)) / board_blend;
    return 0;
}
Joint board_on_feet(const Clip& clip, float seconds, std::span<const Joint> pose, Joint board) noexcept {
    // Only while the board lies flat: at the pop the back foot stands on the tail and the board is steep,
    // and pulling its middle under the feet would put it through the legs. Full up to 8 degrees of tilt,
    // none from 20.
    const float tilt_cos = rotate(board.rotation, board_up(clip))[1];
    const float flat = std::clamp((tilt_cos - 0.9397f) / (0.9903f - 0.9397f), 0.0f, 1.0f);
    const float share = on_ground_share(clip, seconds) * flat;
    if (share <= 0 || pose.size() <= left_foot_joint) return board;
    const auto placed = world(pose, placement_joint);
    const float left = relative(placed, world(pose, left_foot_joint)).position[1];
    const float right = relative(placed, world(pose, right_foot_joint)).position[1];
    const float want = std::min(left, right) - feet_above_board;
    if (std::isfinite(want)) board.position[1] += (want - board.position[1]) * share;
    return board;
}

// The board where the trick starts (time 0), under the placement joint: the 0, 0, 0 of the board's
// keys and fields. Kept per clip.
// The board in a clip is the animation's own board joint (Skateboard_Root), and the game turns that one
// round when it mirrors him; the board it draws comes from physics, nose along the skater's +X unless the
// board is backwards (measured 2026-10-22 in normal, fakie, switch and after shuvits). So a clip whose board
// starts out pointing the other way - one recorded mirrored, or with the board backwards - is turned round
// for its whole length, and every clip shows and keys its board nose-forward, the way the game draws it.
bool board_turned_round(const Clip& clip) noexcept {
    Joint first;
    return clip.sample_board(0, first) && rotate(first.rotation, Vec3{0, 0, 1})[0] < 0;
}
Joint nose_forward(const Clip& clip, Joint rel) noexcept {
    if (board_turned_round(clip)) rel.rotation = normalised(multiply(rel.rotation, Quat{0, 1, 0, 0}));
    return rel;
}
Joint start_board(const Clip& clip, bool fit_contact) {
    Joint board;
    if (Joint rel; clip.sample_board(0, rel)) {
        rel = nose_forward(clip, rel);
        board = fit_contact ? board_on_feet(clip, 0, clip.frame(0), rel) : rel;
    }
    return board;
}
// The board's start in the world, turned only about the up axis: its keys move it along this frame's X,
// Y and Z and turn it about them, so 0, 0, 0 is where it starts, on the ground, as it lies there.
Joint start_frame(const Clip& clip, const Joint& placement, float floor_y, bool fit_contact) {
    const auto start = compose(placement, start_board(clip, fit_contact));
    return {{1, 1, 1}, ground_turn(start), {start.position[0], floor_y, start.position[2]}};
}

// Where a board touches the ground, in its mount's frame: the tips of its nose and tail (the kicks rise
// a little) and the bottoms of its four wheels. A board at rest stands on its wheels with its mount
// 0.087 m up (riding measured 0.085). Its long axis and its up are found per clip, from the board at the
// trick's start: up is the axis nearest the placement joint's up, long the one nearest the line of
// the feet.
BoardShape board_shape(const Clip& clip) {
    BoardShape shape;
    const auto pose = clip.frame(0);
    Joint board;
    (void)clip.sample_board(0, board); // its turn only: the board's place does not matter here
    const auto placed = world(pose, placement_joint);
    const auto left = relative(placed, world(pose, left_foot_joint)).position, right = relative(placed, world(pose, right_foot_joint)).position;
    const Vec3 feet{left[0] - right[0], 0, left[2] - right[2]};
    int up = 1, along = 0;
    float best_up = -1, best_along = -1;
    std::array<Vec3, 3> axes{};
    for (int i = 0; i < 3; ++i) {
        Vec3 e{};
        e[static_cast<std::size_t>(i)] = 1;
        axes[static_cast<std::size_t>(i)] = rotate(board.rotation, e);
        if (std::abs(axes[static_cast<std::size_t>(i)][1]) > best_up) best_up = std::abs(axes[static_cast<std::size_t>(i)][1]), up = i;
    }
    for (int i = 0; i < 3; ++i) {
        if (i == up) continue;
        const auto& a = axes[static_cast<std::size_t>(i)];
        const float d = std::abs(a[0] * feet[0] + a[2] * feet[2]);
        if (d > best_along) best_along = d, along = i;
    }
    const int side = 3 - up - along;
    const float up_sign = axes[static_cast<std::size_t>(up)][1] >= 0 ? 1.0f : -1.0f;
    const auto point = [&](float l, float u, float w) {
        Vec3 v{};
        v[static_cast<std::size_t>(along)] = l;
        v[static_cast<std::size_t>(up)] = u * up_sign;
        v[static_cast<std::size_t>(side)] = w;
        return v;
    };
    shape.up = point(0, 1, 0);
    constexpr float tip = 0.40f, kick = 0.04f, wheelbase = 0.18f, track = 0.09f, wheels = 0.087f;
    shape.points = {point(tip, kick, 0), point(-tip, kick, 0), point(wheelbase, -wheels, track), point(wheelbase, -wheels, -track),
                    point(-wheelbase, -wheels, track), point(-wheelbase, -wheels, -track)};
    return shape;
}
Vec3 board_up(const Clip& clip) { return board_shape(clip).up; }
// The timeline's catch, with its keys on: the first moment, at least 45% into the air, that the board is back the way it
// lies under him at the landing (within 25 degrees) with a foot on it (ankle 20 cm or less over the deck, over its length),
// two frames running, after it had turned away or both feet had left it. Measured on recorded kickflips, 360 flips and
// ollies: caught 0.05 to 0.1 s before the landing. One whose board never leaves him is caught at 60% of its air; -1 for
// one with no air or no board, or one never caught before its landing.
namespace {
float guess_catch(const Clip& clip, const Animation& edits) {
    try {
        const float up_at = clip.takeoff, down = clip.landing;
        if (up_at < 0 || down <= up_at + 0.15f || clip.board.empty() || clip.joints != skeleton_joints) return -1;
        const auto shape = board_shape(clip);
        const Vec3 up = unit(shape.up);
        const Vec3 along = unit(Vec3{shape.points[0][0] - shape.points[1][0], shape.points[0][1] - shape.points[1][1], shape.points[0][2] - shape.points[1][2]});
        const Vec3 across = unit(cross(up, along));
        const auto dot = [](const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        const auto apart = [](const Quat& a, const Quat& b) {
            const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
            return 2 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 57.2957795f;
        };
        // The board as it is at the landing, under him: caught is the board back like that.
        Joint landed;
        if (!clip.sample_board(down, landed)) return -1;
        std::vector<Joint> pose(clip.joints);
        bool left = false; // the board turned away from it, or both feet off the deck, at least once
        int on = 0, off = 0;
        float first = -1;
        const float earliest = up_at + (down - up_at) * 0.45f;
        for (float t = up_at + 1.0f / 60; t < down - 1.0f / 60; t += 1.0f / 60) {
            clip.sample(t, pose);
            apply_edits(edits, t, pose);
            Joint board;
            if (!clip.sample_board(t, board)) return -1;
            const auto placed = world(pose, placement_joint);
            int feet_on = 0;
            for (const std::uint16_t foot : {left_foot_joint, right_foot_joint}) {
                const auto p = relative(board, relative(placed, world(pose, foot))).position;
                const float h = dot(p, up);
                if (std::abs(dot(p, along)) < 0.5f && std::abs(dot(p, across)) < 0.25f && h > -0.08f && h < 0.2f) ++feet_on;
            }
            const bool turned_back = apart(board.rotation, landed.rotation) < 25.0f;
            if (!turned_back || feet_on == 0) {
                on = 0;
                if (++off >= 2) left = true;
                continue;
            }
            off = 0;
            if (!left || t < earliest) continue;
            if (on++ == 0) first = t;
            if (on >= 2) return first < down - 0.03f ? first : -1;
        }
        return left ? -1 : up_at + (down - up_at) * 0.6f;
    } catch (...) {
        return -1;
    }
}
// The trick has air to hold a catch in, and `at` is in it: a frame or more after the pop, before the landing.
bool has_air(const Clip& clip) noexcept { return clip.takeoff >= 0 && clip.landing > clip.takeoff + 0.15f; }
bool catch_fits(const Clip& clip, float at) noexcept {
    return has_air(clip) && std::isfinite(at) && at >= clip.takeoff + 0.5f / key_fps && at < clip.landing - 0.03f;
}
// The studio's catch (AUTO CATCH), with the keys on. Keys holding both feet off the deck right to the landing (knees
// tucked up high) leave nothing caught before it: the game's own clip's catch then, so off a drop the tuck is what is held
// longer. (Such a trick had no catch at all: nothing slowed off a drop, and its flag said SET CATCH.)
float auto_catch(const Clip& clip, const Animation& edits) {
    const float keyed = guess_catch(clip, edits);
    return keyed >= 0 || edits.tracks.empty() ? keyed : guess_catch(clip, Animation{});
}
// The catch the latch holds: the one its maker set on the timeline (CATCH) when it is in the air, else the studio's.
float find_catch(const Clip& clip, const Animation& edits) {
    if (edits.catch_at && catch_fits(clip, *edits.catch_at)) return *edits.catch_at;
    return auto_catch(clip, edits);
}
} // namespace
// The lowest point of the board `board` (a mount in the world): what touches the ground first.
float lowest_point(const Joint& board, const BoardShape& shape) noexcept {
    float low = board.position[1];
    for (const auto& pt : shape.points) low = std::min(low, board.position[1] + rotate(board.rotation, pt)[1]);
    return low;
}
// A board keyed into the floor is held up on it: its lowest point (a tip as it tilts, or its wheels) no lower than
// `floor`. The keys ease its turn and its height each on their own, so between two keys on the ground a tilt swung a
// tip through the floor. Returns how far it was lifted (0 when it was not under it).
float keep_board_above(Joint& board, const BoardShape& shape, float floor) noexcept {
    const float low = lowest_point(board, shape);
    if (!std::isfinite(low) || !std::isfinite(floor) || !(low < floor)) return 0;
    board.position[1] += floor - low;
    return floor - low;
}

// Edited pieces of the game's own animation. The game never says which of its clips it is playing, so
// each saved piece is looked for in the skater's pose (PieceTracker): when the body is seen moving
// through one in step, its edits are laid over the game's pose at the time it has got to, exactly as a
// trick's are, and let go when it stops fitting. A named trick that is playing comes first.
// Held moves: which saved loop he holds (held_pose_fits), taken once it has been the clearly nearest for a moment, then
// played looped on its own clock until he leaves the move or holds another pose. Each kind is looked for only where the
// game holds it: grind and slide loops while grinding, grab loops in the air, manual, powerslide and brake loops on the
// ground - there only while he holds still (a push or a carve moves) and nearer the loop than the plain stance on the
// board. The distances are logged once a hold, so how near the game comes to the saved loops can be read off a log.
Live::HeldKind held_kind_of(const std::string& trick) {
    std::string a = trick.substr(trick.starts_with("native:") ? 7 : 0);
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](const char* word) { return a.find(word) != std::string::npos; };
    using K = Live::HeldKind;
    if (has("ofb")) return K::none; // on foot
    if (has("powerslide") || has("brake")) return has("loop") ? K::ground : K::none;
    if (has("manual")) return has("loop") || has("pose") ? K::ground : K::none;
    if (has("grab")) return has("inair") && has("loop") ? K::grab : K::none;
    if (has("loop") && (has("grind") || has("rail") || has("slide"))) return K::grind;
    return K::none;
}
struct HeldRule {
    float enter, leave; // pose distances (the mean of 1 - |dot|): 0.02 is about 23 degrees a joint
    const char* doing;
    const char* what;
};
HeldRule held_rule(Live::HeldKind kind) {
    switch (kind) {
    case Live::HeldKind::grind: return {0.02f, 0.035f, "grinding", "grind loop"};
    case Live::HeldKind::grab: return {0.02f, 0.035f, "in a grab", "grab loop"};
    default: return {0.015f, 0.03f, "holding a pose on the board", "manual, powerslide or brake loop"};
    }
}
constexpr float held_apart = 0.7f;  // the loop taken is well nearer than the next saved one
constexpr float held_still = 0.006f; // on the ground: how little the spine and legs may move over a quarter of a second
std::optional<PieceMatch> held_play(Live& l, const Snapshot& snap, double at, float dt, Live::HeldKind kind) {
    auto& g = l.held;
    if (g.kind != kind) {
        g = {};
        g.kind = kind;
    }
    if (g.since < 0) g.since = at;
    const auto rule = held_rule(kind);
    std::vector<std::size_t> pieces;
    std::vector<const Clip*> clips;
    for (const auto& [index, k] : l.held_loops)
        if (k == kind) {
            pieces.push_back(index);
            clips.push_back(l.piece_tricks[index]->clip.get());
        }
    if (clips.empty()) return std::nullopt;
    // On the ground the plain stance on the board is in the running: a loop only when he is nearer it than to that.
    const bool ground = kind == Live::HeldKind::ground;
    const std::size_t riding = clips.size();
    if (ground && snap.idle && !snap.idle->empty()) clips.push_back(snap.idle.get());
    const auto fits = held_pose_fits(clips, l.pose, kind == Live::HeldKind::grab);
    const HeldPose* loop = nullptr;
    float next = 1, riding_score = 1;
    bool has_next = false;
    for (const auto& f : fits) {
        if (f.piece == riding) riding_score = std::min(riding_score, f.score);
        else if (!loop) loop = &f;
        else if (!has_next) next = f.score, has_next = true;
    }
    if (!loop) return std::nullopt;
    const int nearest = static_cast<int>(pieces[loop->piece]);
    // How much the spine and legs moved over the last quarter of a second.
    float moved = 0;
    if (ground && !l.held_history.empty()) {
        const auto& now = l.held_history.back();
        const auto* then = &l.held_history.front();
        for (const auto& h : l.held_history)
            if (std::abs(h.first - (now.first - 0.25)) < std::abs(then->first - (now.first - 0.25))) then = &h;
        for (std::size_t j = 0; j < now.second.size(); ++j) {
            const auto& x = now.second[j];
            const auto& y = then->second[j];
            moved += 1 - std::abs(x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + x[3] * y[3]);
        }
        moved /= static_cast<float>(now.second.size());
    }
    if (!g.told && at - g.since > 0.4) {
        g.told = true;
        logging::log(Level::info, Channel::skater,
                     "Animation Studio: {}; the nearest saved {} is {}{} ({:.4f}; the next {:.4f}{}). One plays under {:.3f} and at most {:.0f}% of the next{}.",
                     rule.doing, rule.what, l.piece_tricks[static_cast<std::size_t>(nearest)]->name, loop->mirrored ? ", mirrored" : "", loop->score,
                     next, ground ? std::format("; the stance on the board {:.4f}; moved {:.4f}", riding_score, moved) : std::string{}, rule.enter,
                     held_apart * 100, ground ? std::format(", nearer than the stance, moving under {:.3f}", held_still) : std::string{});
    }
    if (g.playing >= 0) {
        const auto* playing = l.piece_tricks[static_cast<std::size_t>(g.playing)];
        float score = 1;
        for (const auto& f : fits)
            if (f.piece != riding && static_cast<int>(pieces[f.piece]) == g.playing) score = f.score;
        if (score > rule.leave || (ground && riding_score < score)) {
            if (g.off_since < 0) g.off_since = at;
        } else g.off_since = -1;
        if (g.off_since >= 0 && at - g.off_since > 0.3) {
            logging::log(Level::info, Channel::skater, "Animation Studio: {}; {} lets go: he holds another pose now ({:.4f}).", rule.doing, playing->name, score);
            g.playing = g.candidate = -1;
            return std::nullopt;
        }
        const float length = playing->clip->duration();
        g.time += dt;
        if (length > 0) g.time = std::fmod(g.time, length);
        return PieceMatch{static_cast<std::size_t>(g.playing), g.mirrored, g.time, score};
    }
    const bool clear = loop->score < rule.enter && loop->score < next * held_apart &&
                       (!ground || (loop->score < riding_score && moved < held_still));
    if (!clear) {
        g.candidate = -1;
        return std::nullopt;
    }
    if (g.candidate != nearest || g.candidate_mirrored != loop->mirrored) {
        g.candidate = nearest;
        g.candidate_mirrored = loop->mirrored;
        g.candidate_since = at;
        return std::nullopt;
    }
    if (at - g.candidate_since < 0.2) return std::nullopt; // held a moment, not passed through
    g.playing = nearest;
    g.mirrored = loop->mirrored;
    g.time = loop->time;
    g.off_since = -1;
    logging::log(Level::info, Channel::skater, "Animation Studio: {}; he holds {}{} ({:.4f}, the next {:.4f}): it plays, looped.", rule.doing,
                 l.piece_tricks[static_cast<std::size_t>(nearest)]->name, loop->mirrored ? ", mirrored" : "", loop->score, next);
    return PieceMatch{static_cast<std::size_t>(nearest), loop->mirrored, g.time, loop->score};
}
void play_pieces(Live& l, const Snapshot& snap, double at, std::uint64_t generation, std::uintptr_t component,
                 const Joint& live_placement, std::shared_ptr<const GameplayPacket>& scheduled, Joint& board_target,
                 float& board_weight, MotionState motion, std::optional<std::uint32_t> state) {
    if (l.pieces_for != snap.tricks) {
        l.pieces_for = snap.tricks;
        l.mirrored_edits.clear(); // an address can be reused by a new animation
        l.piece_tricks.clear();
        l.held_loops.clear();
        l.tracked.clear();
        l.held = {};
        std::vector<const Clip*> clips;
        if (snap.tricks)
            for (const auto& t : *snap.tricks)
                if (t.trick.starts_with("native:") && t.clip) {
                    // A held move's loop is found by the pose he holds (held_play); the rest by moving through them.
                    const auto kind = held_kind_of(t.trick);
                    const auto index = l.piece_tricks.size();
                    l.piece_tricks.push_back(&t);
                    if (kind != Live::HeldKind::none) l.held_loops.push_back({index, kind});
                    else {
                        l.tracked.push_back(index);
                        clips.push_back(t.clip.get());
                    }
                }
        l.pieces.set_pieces(clips);
        l.piece_play = {};
        if (!l.piece_tricks.empty())
            logging::log(Level::info, Channel::skater,
                         "Animation Studio: watching for {} edited piece{} of the game's animation: {} by how they move ({} move enough to be recognised), "
                         "{} held loop{} by the pose held.",
                         l.piece_tricks.size(), l.piece_tricks.size() == 1 ? "" : "s", clips.size(), l.pieces.looking_for() / 2, l.held_loops.size(),
                         l.held_loops.size() == 1 ? "" : "s");
    }
    auto& play = l.piece_play;
    if (l.piece_tricks.empty()) return;
    const float dt = play.at >= 0 ? static_cast<float>(std::clamp(at - play.at, 0.0, 0.1)) : 0.0f;
    if (play.at >= 0 && at - play.at > 0.2) play = {};
    play.at = at;
    // The spine and legs over the last half second, for telling a held pose from moving.
    if (l.pose.size() >= skeleton_joints) {
        static constexpr std::array<std::uint16_t, 10> telling{42, 44, 8, 9, 10, 11, 341, 342, 343, 344};
        std::array<Quat, 10> now{};
        for (std::size_t j = 0; j < telling.size(); ++j) now[j] = l.pose[telling[j]].rotation;
        l.held_history.emplace_back(at, now);
        while (l.held_history.size() > 2 && at - l.held_history.front().first > 0.5) l.held_history.pop_front();
    }
    // Where the game holds a move: grinding, in the air (a grab), on the board on the ground (a manual, a powerslide).
    using K = Live::HeldKind;
    K holding = K::none;
    if (motion == MotionState::rail) holding = K::grind;
    else if (motion == MotionState::air) holding = K::grab;
    else if (motion == MotionState::riding || (motion == MotionState::unknown && state && *state < 300)) holding = K::ground;
    const bool any_held = std::any_of(l.held_loops.begin(), l.held_loops.end(), [&](const auto& h) { return h.second == holding; });
    if (holding == K::none || !any_held) l.held = {};
    std::optional<PieceMatch> match;
    if (!l.game_trick && !scheduled) {
        match = l.pieces.update(at, l.pose);
        if (match) match->piece = match->piece < l.tracked.size() ? l.tracked[match->piece] : l.piece_tricks.size();
        // Nothing moving through a piece: the loop of a move he holds, if one of them is saved.
        if (!match && any_held) match = held_play(l, snap, at, dt, holding);
    } else l.pieces.reset();
    // Exact stance: a piece seen playing the other way round to the way it was made is left to the game.
    if (match && !snap.front_foot && match->mirrored) match.reset();
    if (match && match->piece < l.piece_tricks.size()) {
        const auto* trick = l.piece_tricks[match->piece];
        if (trick != play.trick || match->mirrored != play.mirrored || play.weight <= 0) {
            play.episode = (std::uint64_t{1} << 63) | ++l.piece_episodes;
            play.weight = 0;
            // The board's keys work in the frame the board has now, as a trick's do from its takeoff.
            play.board_frame_ok = false;
            if (l.board_ok) {
                const auto mount = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                if (finite(mount)) {
                    play.board_frame = normalised(multiply(conjugate(live_placement.rotation), ground_turn(mount)));
                    play.board_frame_ok = true;
                    play.board_backwards = board_backwards(l.base.load(std::memory_order_acquire), component);
                    if (play.board_backwards) play.board_frame = normalised(multiply(play.board_frame, Quat{0, 1, 0, 0}));
                }
            }
            logging::log(Level::info, Channel::skater, "Animation Studio: the game is playing {}{} ({:.2f} s in); playing your {}.",
                         trick->trick.substr(7), match->mirrored ? ", mirrored" : "", match->time, trick->name);
        }
        play.trick = trick;
        play.mirrored = match->mirrored;
        play.time = match->time;
        play.weight = std::min(1.0f, play.weight + (dt > 0 ? dt / 0.12f : 0.25f));
    } else if (play.trick) {
        // Let go over 150 ms, carrying on through the piece meanwhile.
        play.time += dt;
        play.weight -= dt > 0 ? dt / 0.15f : 0.25f;
        if (play.weight <= 0 || play.time > play.trick->clip->duration()) {
            play = {};
            play.at = at;
            return;
        }
    }
    if (!play.trick || play.weight <= 0 || scheduled) return;
    auto keys = play.trick->edits;
    if (play.mirrored) {
        auto found = l.mirrored_edits.find(play.trick->edits.get());
        if (found == l.mirrored_edits.end()) {
            try {
                found = l.mirrored_edits.emplace(play.trick->edits.get(), std::make_shared<const Animation>(mirror_animation(*play.trick->edits))).first;
            } catch (...) { found = l.mirrored_edits.end(); }
        }
        if (found == l.mirrored_edits.end()) return;
        keys = found->second;
    }
    const float weight = play.weight * animation_window_weight(*keys, play.time);
    if (weight <= 0) return;
    auto packet = std::make_shared<GameplayPacket>();
    packet->generation = generation; packet->episode = play.episode;
    packet->sequence = l.animation_updates.load(); packet->epoch = gameplay_epoch.load(std::memory_order_acquire);
    packet->component = component; packet->board_holder = l.board_holder.load(std::memory_order_acquire);
    packet->at = at; packet->time = play.time; packet->weight = weight;
    packet->clip = play.trick->clip; packet->edits = keys;
    packet->mirrored = play.mirrored; packet->match = match_bent_joints;
    packet->board_backwards = play.board_backwards;
    packet->match_rest = match_whole_body;
    packet->limits = snap.limits;
    packet->board_frame_local = play.board_frame; packet->board_frame_valid = play.board_frame_ok;
    packet->board_owned = true;
    if (play.board_frame_ok && l.board_ok) {
        Joint frame;
        frame.rotation = normalised(multiply(live_placement.rotation, play.board_frame));
        const auto native_board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
        board_target = board_edited(native_board, *keys, play.time, ground_turn(frame));
        board_weight = weight;
    }
    scheduled = std::move(packet);
}

// ---- stance readout --------------------------------------------------------------------------------
// Reverse-engineering aid. The game's animation graph mirrors the whole skater when physics sets
// Bool.Anim.Mirrored (SkaterCorePhysicsInputConfig.AnimMirrored); its clips are authored goofy, so regular
// is the mirrored one, and fakie is NOT a mirror (same clip, plus an upper-body additive layer). Which rule
// physics uses for Mirrored is not in the data, so this measures it: the stance byte as the game has it,
// which foot is physically at the board's nose, and which way the board travels - logged on every change
// and once a second, to ReSkate.Tricks/research/stance-readout.txt.
namespace stance_readout {
std::atomic<bool> on{};
double started{-1}, last_line{-1};
std::string last_key;
Vec3 last_mount{};
double last_mount_at{-1};
std::size_t lines{};
std::string fmt(float v) {
    char t[16];
    std::snprintf(t, sizeof(t), "%+.2f", v);
    return t;
}
void write(const std::string& line) {
    try {
        const auto folder = library_folder() / L"research";
        std::filesystem::create_directories(folder);
        std::ofstream file(folder / "stance-readout.txt", std::ios::binary | std::ios::app);
        file << line << '\n';
    } catch (...) {}
}
void tick(Live& l, double at, std::uintptr_t base, std::uintptr_t component, MotionState motion) {
    if (!on.load(std::memory_order_acquire)) return;
    if (started < 0) started = at;
    if (at - started > 300 || lines > 4000) {
        on.store(false);
        write("# stopped (5 minutes or 4000 lines)");
        logging::log(Level::info, Channel::skater, "Animation Studio: stance readout stopped.");
        return;
    }
    if (!l.board_ok || l.pose.size() < skeleton_joints) return;
    const auto mount = compose(board_parent(l.board), joint_of(l.board[board_mount]));
    if (!finite(mount)) return;
    // The feet and the board's travel, in the board's own frame: x across, y up, z along it.
    const auto in_board = [&](const Vec3& w) {
        return rotate(conjugate(mount.rotation), Vec3{w[0] - mount.position[0], w[1] - mount.position[1], w[2] - mount.position[2]});
    };
    const auto left = in_board(world(l.pose, 343).position), right = in_board(world(l.pose, 10).position);
    Vec3 travel{};
    if (last_mount_at >= 0 && at - last_mount_at > 1e-3) {
        const Vec3 moved{mount.position[0] - last_mount[0], mount.position[1] - last_mount[1], mount.position[2] - last_mount[2]};
        const auto local = rotate(conjugate(mount.rotation), moved);
        for (std::size_t i = 0; i < 3; ++i) travel[i] = static_cast<float>(local[i] / (at - last_mount_at));
    }
    last_mount = mount.position;
    last_mount_at = at;
    // The feet in the skater's own frame (the placement, joint 1): the game mirrors his body in it, so the
    // side each foot is on says whether it is playing his clips mirrored.
    const auto placed = world(l.pose, placement_joint);
    const auto in_placed = [&](const Vec3& w) {
        return rotate(conjugate(placed.rotation), Vec3{w[0] - placed.position[0], w[1] - placed.position[1], w[2] - placed.position[2]});
    };
    const auto pl = in_placed(world(l.pose, 343).position), pr = in_placed(world(l.pose, 10).position);
    const auto board_along = rotate(conjugate(placed.rotation), rotate(mount.rotation, Vec3{0, 0, 1}));
    const auto flags = stance_flags(base, component);
    const char* moving = motion == MotionState::riding ? "riding" : motion == MotionState::air ? "air" : motion == MotionState::rail ? "rail"
                       : motion == MotionState::offboard ? "offboard" : "unknown";
    // Which foot is further toward board +z, and which way along z the board is going.
    const char* toward_plus_z = left[2] > right[2] ? "left" : "right";
    const float speed = travel[2];
    const char* going = std::abs(speed) < 0.5f ? "still" : speed > 0 ? "+z" : "-z";
    char flag_text[8] = "--";
    if (flags) std::snprintf(flag_text, sizeof(flag_text), "%02x", *flags);
    const char* placed_side = pr[0] > pl[0] ? "R+x" : "L+x";
    // The same test on the pose as the renderer has it - where the recorder reads and where gameplay
    // edits are written. If the two disagree, the game mirrors between them.
    std::string render_side = "?  ";
    {
        auto& shot = render_shot();
        std::lock_guard shot_lock(shot.mutex);
        if (shot.skater.size() == skeleton_joints && at - shot.at < 0.1) {
            std::vector<Joint> drawn;
            drawn.reserve(shot.skater.size());
            for (const auto& bone : shot.skater) drawn.push_back(joint_of(bone));
            const auto at_place = world(drawn, placement_joint);
            const auto side = [&](std::uint16_t foot) {
                const auto w = world(drawn, foot).position;
                return rotate(conjugate(at_place.rotation), Vec3{w[0] - at_place.position[0], w[1] - at_place.position[1], w[2] - at_place.position[2]})[0];
            };
            render_side = side(10) > side(343) ? "R+x" : "L+x";
        }
    }
    // The game's own board-backwards bit and body-relative forward speed (found 2026-10-22, readout v2).
    std::string backwards = "?", forward = "?";
    if (const auto context = physics_context(base, component)) {
        if (const auto b = read<std::uint8_t>(context + 0x13c5)) backwards = (*b & 0x20) ? "yes" : "no ";
        if (const auto f = read<float>(context + 0x17fc)) forward = fmt(*f);
    }
    const std::string key = std::string(flag_text) + toward_plus_z + going + moving + placed_side + render_side + backwards +
                            (forward.size() && forward[0] == '-' ? "-" : "+");
    if (key == last_key && at - last_line < 1.0) return;
    last_key = key;
    last_line = at;
    ++lines;
    const auto trick = current_trick(component);
    write(std::format("{:7.2f}s  {:8s} flags {} (left_foot_leads {})  setting {}  foot toward +z: {:5s}  board going {:5s} ({} m/s)  "
                      "L {} {} {}  R {} {} {}  | placement: {} (L x {} R x {}) render: {}  backwards {}  forward {}  board +z along ({} {} {})  trick {}",
                      at - started, moving, flag_text, flags ? ((*flags & addr::skater_lead_foot::left_foot_leads_mask) ? "yes" : "no ") : "?  ",
                      goofy_stance() ? "goofy  " : "regular", toward_plus_z, going, fmt(speed), fmt(left[0]), fmt(left[1]), fmt(left[2]),
                      fmt(right[0]), fmt(right[1]), fmt(right[2]), placed_side, fmt(pl[0]), fmt(pr[0]), render_side, backwards, forward, fmt(board_along[0]),
                      fmt(board_along[1]), fmt(board_along[2]), trick.empty() ? "-" : trick));
    // The physics context around the stance byte, for finding the game's own mirror, fakie and
    // board-backwards flags by what changes with them. Appended as fixed-size records, one per line.
    if (const auto context = physics_context(base, component)) {
        std::array<std::uint8_t, 0x3000> bytes{};
        if (peek(context, bytes.data(), bytes.size())) {
            try {
                std::ofstream dump(library_folder() / L"research" / "stance-memory.bin", std::ios::binary | std::ios::app);
                const auto line = static_cast<std::uint32_t>(lines);
                dump.write(reinterpret_cast<const char*>(&line), 4);
                dump.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            } catch (...) {}
        }
    }
}
} // namespace stance_readout
std::string toggle_stance_readout() {
    const bool now = !stance_readout::on.load();
    if (now) {
        try { std::filesystem::remove(library_folder() / L"research" / "stance-readout.txt"); } catch (...) {}
        try { std::filesystem::remove(library_folder() / L"research" / "stance-memory.bin"); } catch (...) {}
        stance_readout::started = -1;
        stance_readout::last_key.clear();
        stance_readout::lines = 0;
        stance_readout::last_mount_at = -1;
        stance_readout::write("# Animation Studio stance readout v3. Ride normal, then fakie, then push while fakie (that puts you in switch) and ride switch.");
        stance_readout::write("# Feet are in the board's frame (x across, y up, z along). 'board going' is its travel along its own z.");
    } else stance_readout::write("# stopped by command");
    stance_readout::on.store(now);
    return now ? "Stance readout on: ride a few seconds in each stance (normal, fakie, switch, nollie), then type animstudio stance "
                 "again. Written to ReSkate.Tricks/research/stance-readout.txt."
               : "Stance readout off. See ReSkate.Tricks/research/stance-readout.txt.";
}

// ---- studio snapshot -------------------------------------------------------------------------------
// Written by itself 1.5 s after the studio shows a stance (on opening, and whenever the stance shown
// changes): where the studio put his body and board, and what the skater buffer holds later in the same
// frame, after the board is drawn. For finding why the studio's picture of him is off his board.
namespace studio_snapshot {
// Research only (RESKATE_STUDIO_RESEARCH=1): what the studio drew against what the game drew, per stance,
// written to ReSkate.Tricks/research. Off, nothing is written.
bool on() {
    static const bool enabled = [] {
        wchar_t flag[4]{};
        return GetEnvironmentVariableW(L"RESKATE_STUDIO_RESEARCH", flag, 4) == 1 && flag[0] == L'1';
    }();
    return enabled;
}
std::mutex mutex;
bool armed{};
double due{-1};
int last_key{-1};
std::vector<Joint> desired;
Joint desired_board, placement;
bool mirrored{}, goofy{}, clip_goofy{}, has_board{};
std::vector<Joint> late;
Joint late_board;
bool late_ok{};
void write_report() {
    static const std::array<std::pair<std::uint16_t, const char*>, 16> joints{{
        {7, "Hips"}, {42, "Spine"}, {10, "RightFoot"}, {343, "LeftFoot"}, {9, "RightLeg"}, {342, "LeftLeg"},
        {48, "RightForeArm"}, {277, "LeftForeArm"}, {376, "LeftAnkleEffectorAux"}, {377, "RightAnkleEffectorAux"},
        {390, "LeftAnkleEffDeck"}, {391, "RightAnkleEffDeck"}, {394, "HipsEffDeck"}, {375, "Ground"},
        {380, "Skateboard_Root"}, {1, "AITrajectory"}}};
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "# Animation Studio snapshot: studio stance " << (goofy ? "goofy" : "regular") << ", clip stands "
        << (clip_goofy ? "goofy" : "regular") << ", body mirrored by the studio: " << (mirrored ? "yes" : "no") << "\n";
    const auto in_frame = [&](const Joint& frame, const Joint& w) {
        Joint r;
        r.position = rotate(conjugate(frame.rotation), Vec3{w.position[0] - frame.position[0], w.position[1] - frame.position[1], w.position[2] - frame.position[2]});
        r.rotation = normalised(multiply(conjugate(frame.rotation), w.rotation));
        return r;
    };
    const auto v = [&](const Vec3& x) {
        std::ostringstream t;
        t << std::fixed << std::setprecision(3) << '(' << x[0] << ' ' << x[1] << ' ' << x[2] << ')';
        return t.str();
    };
    const auto table = [&](const char* title, const std::vector<Joint>& pose, const Joint& board, bool board_ok) {
        out << "== " << title << " (positions in that pose's AITrajectory frame)\n";
        if (pose.size() < skeleton_joints) {
            out << "   (none)\n";
            return;
        }
        const auto frame = world(pose, placement_joint);
        for (const auto& [j, name] : joints) {
            const auto w = world(pose, j);
            out << "   " << std::setw(3) << j << ' ' << std::left << std::setw(22) << name << std::right << v(in_frame(frame, w).position)
                << "  local rot (" << pose[j].rotation[0] << ' ' << pose[j].rotation[1] << ' ' << pose[j].rotation[2] << ' ' << pose[j].rotation[3] << ")\n";
        }
        if (board_ok) {
            const auto b = in_frame(frame, board);
            out << "   board mount " << v(b.position) << "  long axis (its z) " << v(rotate(b.rotation, Vec3{0, 0, 1})) << "  up (its y) "
                << v(rotate(b.rotation, Vec3{0, 1, 0})) << '\n';
            for (const auto& [j, name] : {std::pair<std::uint16_t, const char*>{343, "LeftFoot"}, {10, "RightFoot"}}) {
                const auto w = world(pose, j).position;
                const auto f = rotate(conjugate(board.rotation), Vec3{w[0] - board.position[0], w[1] - board.position[1], w[2] - board.position[2]});
                out << "   " << name << " in the board's frame (x across, y up, z along) " << v(f) << '\n';
            }
        }
    };
    table("the pose the studio built", desired, desired_board, has_board);
    table("the skater buffer later in the frame, after the board was drawn", late, late_board, late_ok);
    try {
        const auto folder = library_folder() / L"research";
        std::filesystem::create_directories(folder);
        std::ofstream file(folder / (std::string("studio-snapshot-") + (goofy ? "goofy" : "regular") + ".txt"), std::ios::binary | std::ios::trunc);
        file << out.str();
    } catch (...) {}
    logging::log(Level::info, Channel::skater, "Animation Studio: studio snapshot written ({}).", goofy ? "goofy" : "regular");
}
// Animation thread, each studio frame with no trick open.
void studio_frame(double at, bool goofy_now, bool clip_goofy_now, bool mirrored_now, const std::vector<Joint>& pose, const Joint& placed,
                  const std::optional<Joint>& board) {
    std::lock_guard lock(mutex);
    const int key = (goofy_now ? 1 : 0) | (mirrored_now ? 2 : 0);
    if (key != last_key) {
        last_key = key;
        due = at + 1.5;
        armed = false;
        late_ok = false;
    }
    if (due < 0 || at < due || armed) return;
    desired = pose;
    placement = placed;
    goofy = goofy_now;
    clip_goofy = clip_goofy_now;
    mirrored = mirrored_now;
    has_board = board.has_value();
    if (board) desired_board = *board;
    armed = true; // the board render takes the late read, then writes the report
}
void closed() {
    std::lock_guard lock(mutex);
    last_key = -1;
    due = -1;
    armed = false;
}
// Board render thread: after the board is put where the studio wants it.
void late_read(std::uintptr_t base, std::uintptr_t component, const Joint& board_drawn) {
    std::lock_guard lock(mutex);
    if (!armed) return;
    armed = false;
    due = -1;
    late.clear();
    if (component) {
        if (const auto holder = read<std::uintptr_t>(component + 0xa0); holder && *holder) {
            const auto layout = multiplayer::read_native_pose_layout(peek, base, *holder, 512);
            std::vector<NativeBone> bones(layout.count);
            if (layout.buffer && layout.count == skeleton_joints && peek(layout.buffer, bones.data(), bones.size() * sizeof(NativeBone)))
                for (const auto& b : bones) late.push_back(joint_of(b));
        }
    }
    late_board = board_drawn;
    late_ok = finite(board_drawn) && late.size() == skeleton_joints;
    write_report();
}
} // namespace studio_snapshot

// ---- the animation thread ------------------------------------------------------------------------
void on_animation(std::uintptr_t component) noexcept {
    auto& l = live();
    std::lock_guard animation_lock(l.animation_mutex);
    if (!component || component != l.component.load(std::memory_order_acquire)) return;
    const auto error = GetLastError();
    try {
        const auto generation = l.generation.load(std::memory_order_acquire);
        if (l.seen_generation != generation) {
            l.episode.reset();
            l.last_motion = MotionState::unknown;
            l.game_trick = nullptr;
            l.game_tricks.reset();
            l.attempt_variants.clear();
            l.floor_valid = false;
            l.floor_clip = nullptr;
            l.floor_clip_peek = false;
            l.logged_layout = false;
            l.seen_generation = generation;
        }
        const auto snap = snapshot();
        if (!snap || (!diagnostics_capture && !snap->open && !snap->play_in_game && !research_capture_flag.load(std::memory_order_acquire))) {
            invalidate_outputs();
            l.episode.reset();
            l.game_trick = nullptr;
            SetLastError(error);
            return;
        }
        if (snap->open || !snap->play_in_game) {
            l.episode.reset();
            l.game_trick = nullptr;
        }
        const auto base = l.base.load(std::memory_order_acquire);
        const auto holder = read<std::uintptr_t>(component + 0xa0);
        if (!holder || !*holder) throw std::runtime_error("the skater has no animation yet");
        const auto layout = multiplayer::read_native_pose_layout(peek, base, *holder, 512);
        if (!layout.buffer) throw std::runtime_error("the skater's pose is still loading");
        if (layout.count != skeleton_joints)
            throw std::runtime_error(std::format("the skater has {} joints, not the standard {}", layout.count, skeleton_joints));
        l.animation_updates.fetch_add(1, std::memory_order_relaxed);
        if (!l.logged_layout) {
            l.logged_layout = true;
            logging::log(Level::info, Channel::skater, "Animation Studio: skater pose found, {} joints at {:#x} (component {:#x}).",
                         layout.count, layout.buffer, component);
        }
        // The board's pose, read with the skater's.
        l.board_ok = false;
        if (const auto bh = l.board_holder.load(std::memory_order_acquire)) {
            const auto bl = multiplayer::read_native_pose_layout(peek, base, bh, 64);
            l.board_ok = bl.buffer && bl.count == board_bones && peek(bl.buffer, l.board.data(), sizeof(l.board)) &&
                         finite(compose(board_parent(l.board), joint_of(l.board[board_mount])));
        }
        l.bones.resize(layout.count);
        if (!peek(layout.buffer, l.bones.data(), l.bones.size() * sizeof(NativeBone))) throw std::runtime_error("the skater's pose is unreadable");
        l.pose.resize(layout.count);
        for (std::size_t i = 0; i < layout.count; ++i) {
            auto& j = l.pose[i];
            const auto& b = l.bones[i];
            j.scale = {b.scale[0], b.scale[1], b.scale[2]};
            j.rotation = b.rotation;
            j.position = {b.position[0], b.position[1], b.position[2]};
        }
        const double at = now_seconds();
        const auto current_state = physics_state(base, component);
        if (current_state) {
            // Each physics state the game uses, the first time this session (which is a manual, a powerslide...).
            static std::set<std::uint32_t> states_seen;
            if (states_seen.size() < 64 && states_seen.insert(*current_state).second)
                logging::log(Level::info, Channel::skater, "Animation Studio: physics state {} first seen this session.", *current_state);
        }
        const auto motion = motion_state(current_state);
        l.last_motion = motion;
        l.render_motion.store(motion, std::memory_order_release);
        const auto air = in_the_air(base, component);
        if (!snap->open) stance_readout::tick(l, at, base, component, motion);
        // Rolling fakie or not, kept from riding: in the air the game's own number swings about.
        if (motion == MotionState::riding)
            if (const auto speed = forward_speed(base, component)) l.riding_forward = *speed;
        DiagnosticSample diagnostic;
        bool diagnostic_pose = false;
        if (diagnostics_capture) {
            auto& history = snap->open ? preview_history() : diagnostic_history();
            { std::lock_guard lock(history.mutex); diagnostic_pose = history.buffer.pose_due(at); }
            diagnostic.at = at;
            diagnostic.generation = generation;
            diagnostic.sequence = l.animation_updates.load();
            diagnostic.failures = l.failures;
            diagnostic.physics = current_state;
            diagnostic.trick = current_trick(component);
            diagnostic.live_switch = switch_stance(base, component);
            diagnostic.goofy = goofy_stance();
            diagnostic.enabled = snap->play_in_game;
            if (diagnostic_pose) diagnostic.native = l.pose;
            if (l.board_ok) diagnostic.native_board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
        }
        const bool bailing = current_state && (*current_state == addr::no_bail::wipeout_physics_state ||
                                              *current_state == addr::no_bail::offboard_physics_state);
        if (!current_state && !snap->open) {
            l.episode.reset();
            l.game_trick = nullptr;
        }
        if (bailing && !snap->open) {
            l.episode.reset();
            l.game_trick = nullptr;
        }

        // Which joints to write back, and from what.
        std::size_t first = 0, last = 0; // Studio snapshots only. Gameplay publishes an edit schedule.
        std::shared_ptr<const GameplayPacket> scheduled;
        Joint board_target;
        bool mirrored_body = false; // he is being shown the other way round, so his board is too
        Quat board_frame{0, 0, 0, 1}; // the turn of the board's start (start_frame): its keys work in it
        float board_weight = 0;
        Joint live_placement = world(l.pose, placement_joint);
        // The studio's room: the studio draws the skater (and his board) on the room's floor, facing as
        // the menu's skater does, as high above the floor as he is above his own. Only what is drawn moves: the skater himself stays where he stopped.
        const bool in_room = snap->open && in_room_flag.load(std::memory_order_acquire);
        if (in_room) {
            Joint room = live_placement;
            {
                std::lock_guard lock(room_facing_mutex);
                if (room_facing_valid) room.rotation = room_facing;
            }
            const float above = live_placement.position[1] - world(l.pose, ground_joint).position[1];
            room.position = {room_spot[0], room_floor + above, room_spot[2]};
            const auto local = relative(l.pose[0], room);
            if (finite(local)) {
                l.pose[placement_joint] = local;
                live_placement = room;
            }
        }
        // The studio shows the open trick, or with none the casual stance (the idle clip's last frame,
        // no keys).
        // A trick looked at from TRICKS comes first: it plays once from its start and holds its end.
        const bool peeking = snap->peek_clip && !snap->peek_clip->empty() && snap->peek_since >= 0;
        const bool has_trick = peeking || (snap->clip && !snap->clip->empty());
        const Clip* shown = peeking ? snap->peek_clip.get() : has_trick ? snap->clip.get() : snap->idle && !snap->idle->empty() ? snap->idle.get() : nullptr;
        const float shown_time = peeking ? std::clamp(static_cast<float>(now_seconds() - snap->peek_since), 0.0f, shown->duration())
                                 : has_trick ? snap->time : shown ? shown->duration() : 0.0f;
        static const Animation no_edits;
        const Animation& shown_edits = peeking ? (snap->peek_edits ? *snap->peek_edits : no_edits) : has_trick ? *snap->edits : no_edits;
        // Render captures already include native contacts. Fitting their board
        // independently to the lowest foot corrupts that resolved relative pose.
        const bool fit_preview_contact = shown_edits.source.kind != "render_capture" &&
                                         shown_edits.source.position_codec != "native-piec-v1-extent";
        if (snap->open && shown) {
            // The studio: the recorded trick, played in place where the skater stands now. The placement
            // joint (1) carries the trick's rise and turn (it rides with the body), and the game places
            // the skater from it, so it is never written: the recording's rise and turn since its first
            // frame go onto the joints hanging from it instead; the rest of the body is the recording's.
            l.sampled.resize(layout.count);
            shown->sample(shown_time, l.sampled);
            const auto& start = shown->frame(0)[placement_joint];
            const auto& now_rec = l.sampled[placement_joint];
            const auto turn = normalised(multiply(conjugate(start.rotation), now_rec.rotation));
            // On the floor, on the board: the clip is moved up or down so its board's lowest point (its
            // wheels, or its tail at the pop) is on the studio's floor before the pop and after the landing,
            // and between them follows the arc of a jump as long as the clip's air (unless the clip rises by
            // itself, as a recording does), never under the floor.
            // A new trick starts from where the skater is now: his floor is taken again. A trick looked at from TRICKS
            // stands on the open one's floor, going in and coming out: he does not move. (It was compared with the open
            // clip, so while one was looked at this ran every frame: the floor was taken again from the pose being
            // played, mid-trick, and he bobbed and floated.)
            if (shown != l.floor_clip.get()) {
                const bool peek_switch = peeking || l.floor_clip_peek;
                l.floor_clip = peeking ? snap->peek_clip : has_trick ? snap->clip : snap->idle;
                l.floor_clip_peek = peeking;
                l.preview_own_jump = clip_has_vertical_trajectory(*shown);
                if (!peek_switch) l.floor_valid = false;
            }
            if (!l.floor_valid || (air && *air)) {
                l.floor = world(l.pose, ground_joint).position[1];
                l.floor_valid = std::isfinite(l.floor);
                if (l.floor_valid) {
                    auto& st = state();
                    std::lock_guard floor_lock(st.mutex);
                    st.floor = l.floor;
                    st.floor_valid = true;
                }
            }
            const auto& clip = *shown;
            const auto shape = board_shape(clip);
            // The height of the board's lowest point at `t` (its wheels, or a tip when it tilts).
            const auto board_y = [&](float t, float& ait_y) {
                std::vector<Joint>& tmp = l.custom;
                tmp.resize(layout.count);
                clip.sample(t, tmp);
                const auto ait = world(tmp, placement_joint);
                ait_y = ait.position[1];
                Joint rel;
                return clip.sample_board(t, rel) ? lowest_point(compose(ait, fit_preview_contact ? board_on_feet(clip, t, tmp, nose_forward(clip, rel))
                                                                                             : nose_forward(clip, rel)), shape)
                                                 : world(tmp, 10).position[1] - 0.18f;
            };
            constexpr float deck = 0.087f; // the deck's mount above the floor, the board on its wheels (board_shape)
            float ait_now{}, ait_up{}, ait_down{};
            const float by_now = board_y(shown_time, ait_now);
            float rise_y = 0;
            if (l.floor_valid && shown_edits.source.position_codec == "native-piec-v1-extent") {
                // Verified native vectors already carry AITrajectory and its inverse
                // Ground track. Place that source once on the studio floor; inferred
                // pop/landing markers must not flatten its preparation or invent an arc.
                const float source_floor = world(clip.frame(0), ground_joint).position[1];
                rise_y = l.floor - source_floor + ait_now - live_placement.position[1];
            } else if (l.floor_valid) {
                const float up = std::max(clip.takeoff, 0.0f), down = clip.landing > up ? clip.landing : up;
                const float t = shown_time;
                if (t <= up || t >= down || down <= up) rise_y = l.floor - by_now;
                else {
                    const float s_up = l.floor - board_y(up, ait_up), s_down = l.floor - board_y(down, ait_down);
                    const float u = (t - up) / (down - up);
                    // The clip's own rise in the air: a recording jumps by itself, the game's clips do not.
                    const float air_time = down - up;
                    // As high as the game's board rises on it: 0.40 m on a flat mid ollie of 0.49 s (measured 2026-10-25),
                    // where 9.81 showed 0.29 m. (The board falls at 20 m/s2, but it is not thrown up at once: the pop
                    // pushes it for its first tenth of a second.)
                    constexpr float rise = 13.3f;
                    const float arc = l.preview_own_jump ? 0.0f : rise * air_time * air_time / 8.0f * 4 * u * (1 - u);
                    rise_y = s_up + (s_down - s_up) * u + arc;
                    // Nothing of the board goes under the floor.
                    rise_y = std::max(rise_y, l.floor - by_now);
                }
                rise_y += ait_now - live_placement.position[1];
            } else rise_y = now_rec.position[1] - start.position[1];
            const auto rise = rotate(conjugate(live_placement.rotation), {0, rise_y, 0});
            for (std::size_t i = 2; i < std::min<std::size_t>(layout.count, shown->joints); ++i) {
                l.pose[i] = l.sampled[i];
                if (parent(static_cast<std::uint16_t>(i)) == placement_joint) {
                    const auto moved = rotate(turn, l.pose[i].position);
                    for (std::size_t a = 0; a < 3; ++a) l.pose[i].position[a] = rise[a] + moved[a];
                    l.pose[i].rotation = normalised(multiply(turn, l.pose[i].rotation));
                }
            }
            apply_edits(shown_edits, shown_time, l.pose);
            if (snap->limits) limit_keyed_joints(shown_edits, shown_time, l.pose);
            // Standing him the other way round: his body is reflected across its own middle, so the foot
            // that was at the nose is at the tail and the arm that led now trails. Only the skinned body
            // moves; the placement and everything hanging off it stay where they stood, so the board does
            // not move and nothing that is not him is dragged along. mirror_pose_frame works the locals
            // out again against the parents as they end up, which is what makes mirroring a part of a
            // skeleton safe - doing it bone by bone in each one's own frame is what tore him up before.
            // The game's own clips are labelled regular, but drawn as they are the skater stands goofy:
            // his right foot is at the nose (measured on screen 2026-10-21). That is the game's clips and
            // the casual stance he waits in, which is one of them. A recording is labelled from the game's
            // stance setting as it happens, so its label is true and is read as it is - reading every clip
            // backwards, as this did, turned a regular skater's own recordings round.
            // Which way round the clip stands, from the clip itself: goofy is the right foot leading.
            const bool clip_stands_goofy = clip_leads_right(*shown).value_or(!has_trick || authored_goofy(shown_edits));
            mirrored_body = !has_trick && snap->goofy != clip_stands_goofy;
            if (mirrored_body) {
                l.mirror_scratch.assign(l.pose.begin(), l.pose.end());
                mirror_pose_frame(l.mirror_scratch, l.pose, true);
            }
            // Nothing in his hands: the game hangs props (the phone of its idle, held still while the studio is
            // open) on the hands' attach joints, so those are made tiny and what hangs on them with them.
            for (const std::uint16_t attach : {right_hand_attach, left_hand_attach})
                if (attach < l.pose.size()) l.pose[attach].scale = {0.001f, 0.001f, 0.001f};
            first = in_room ? placement_joint : 2;
            last = layout.count;
            // The board as the recording had it, the same way the body hangs from the placement joint.
            if (Joint rel; shown->sample_board(shown_time, rel)) {
                rel = nose_forward(*shown, rel);
                if (fit_preview_contact) rel = board_on_feet(*shown, shown_time, l.sampled, rel);
                const auto moved = rotate(turn, rel.position);
                for (std::size_t a = 0; a < 3; ++a) rel.position[a] = rise[a] + moved[a];
                rel.rotation = normalised(multiply(turn, rel.rotation));
                // Reflected with him, and in the same place he is: after the clip's turn and rise, about the
                // placement, exactly as his body was reflected above. Reflecting it before the turn put the
                // mirror at a different angle to his whenever the clip had turned (the casual stance has),
                // and he stood across his board instead of on it.
                if (mirrored_body) rel = mirror_board_pose(rel);
                const auto unedited = compose(live_placement, rel);
                const auto origin = start_frame(*shown, live_placement, (l.floor_valid ? l.floor : live_placement.position[1]) + deck, fit_preview_contact);
                board_target = board_edited(unedited, shown_edits, shown_time, origin.rotation);
                // Never into the floor: the board is held up on it, and he is lifted with it - as a board tipped onto
                // its tail lifts its rider - so his feet stay on the deck.
                if (has_trick && l.floor_valid)
                    if (const float lift = keep_board_above(board_target, shape, l.floor); lift > 0) {
                        const auto up = rotate(conjugate(live_placement.rotation), {0, lift, 0});
                        for (std::size_t i = 2; i < std::min<std::size_t>(layout.count, l.pose.size()); ++i)
                            if (parent(static_cast<std::uint16_t>(i)) == placement_joint)
                                for (std::size_t a = 0; a < 3; ++a) l.pose[i].position[a] += up[a];
                    }
                board_frame = origin.rotation;
                board_weight = 1;
                auto& st = state();
                std::lock_guard base_lock(st.mutex);
                st.board_base = relative(origin, unedited);
                st.board_base_valid = has_trick && finite(st.board_base);
                if (snap->grab && finite(board_target)) {
                    close_hand(l.pose, right_hand_joint, right_fingers, board_target);
                    close_hand(l.pose, left_hand_joint, left_fingers, board_target);
                }
            }
        } else if (snap->open) {
            // No trick loaded yet: the live pose, with the edits of the start of the timeline.
            apply_edits(*snap->edits, 0, l.pose);
            if (snap->limits) limit_keyed_joints(*snap->edits, 0, l.pose);
            first = in_room ? placement_joint : 2;
            last = layout.count;
        } else if (snap->play_in_game && !bailing && snap->tricks && !snap->tricks->empty()) {
            const bool began = l.episode.observe(at, motion);
            // Rolling: the board's heading under him, kept a second (for the board's keys of the next trick).
            if (motion == MotionState::riding && l.board_ok && !began) {
                const auto mount = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                if (finite(mount)) {
                    l.rest_headings.emplace_back(at, normalised(multiply(conjugate(live_placement.rotation), ground_turn(mount))));
                    while (l.rest_headings.size() > 2 && at - l.rest_headings.front().first > 1.0) l.rest_headings.pop_front();
                }
            }
            // The probe, while armed: once riding, once on the first frame of the air (the pop's setup is
            // still blending out then), and once well into the air, where the trick's own pieces play.
            if (live_probe::armed.load(std::memory_order_acquire)) {
                if (motion != MotionState::riding) l.probe_riding_since = -1;
                else if (l.probe_riding_since < 0) l.probe_riding_since = at;
                // Riding first, two seconds in so the push and roll pieces have settled; then the pop and
                // the air of the next trick.
                if (motion == MotionState::riding && at - l.probe_riding_since > 2.0) live_probe::take(base, component, 0, "riding");
                else if ((live_probe::taken.load() & 1) && motion == MotionState::air) {
                    const double in_air = at - l.episode.takeoff;
                    if (in_air < 0.05) live_probe::take(base, component, 1, "at the pop");
                    else if (in_air > 0.15 && in_air < 0.3) live_probe::take(base, component, 2, "early in the air");
                    else if (in_air > 0.4) live_probe::take(base, component, 3, "later in the air");
                }
            }
            if (began) {
                l.playback_goofy = goofy_stance();
                l.playback_front_foot = snap->front_foot;
                l.playback_switch = switch_stance(base, component);
                l.playback_fakie = rolling_fakie(l.riding_forward);
                l.playback_backwards = board_backwards(base, component);
                l.game_trick = nullptr;
                // A new set of saved tricks (one was saved, deleted, reloaded) means the mirrors and board
                // frames worked out from the old set are for animations that no longer exist - and a new
                // one can be given the old one's address, so a stale entry would be found and believed.
                if (l.game_tricks != snap->tricks) {
                    l.mirrored_edits.clear();
                }
                l.game_tricks = snap->tricks;
                l.native_clock.reset(); l.clock_weight = 0; l.off_clip = 0; l.other_pop = 0;
                l.attempt_variants.clear(); l.stance_fallback_trick.clear(); l.predicted_air = 0;
                l.limbs = {}; l.gave_way_at = -1; l.gave_way_from.clear();
                l.board_at_takeoff_ok = false;
                if (l.board_ok) {
                    const auto mount = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                    if (finite(mount)) {
                        // The board as it lay on the ground before the pop, as the studio turns the keys about the board at
                        // the trick's start: by the takeoff the pop has turned it already - a 360 flip's by 55 degrees - and
                        // the keys tilted the deck that much the wrong way round (Heatz's Nollie 360 Flip, 2026-10-25). The
                        // board at the takeoff only when he was not rolling just before it (off a rail, out of the air).
                        const auto rest = rest_heading_before(l.rest_headings, at);
                        l.board_frame_local = rest ? *rest : normalised(multiply(conjugate(live_placement.rotation), ground_turn(mount)));
                        l.board_at_takeoff_ok = true;
                        // The game's board-backwards flag: the deck is turned round under him, and the game turns its
                        // animated board to match. The keys follow the animation's board, so they are turned too -
                        // a key on the nose stays on the end the trick treats as the nose.
                        if (board_backwards(base, component)) l.board_frame_local = normalised(multiply(l.board_frame_local, Quat{0, 1, 0, 0}));
                    }
                }
            }
            if (l.episode.phase == PlaybackPhase::idle) l.game_trick = nullptr;
            if (!l.game_trick && l.native_clock.active()) { l.native_clock.reset(); l.clock_weight = 0; }
            if (l.episode.phase == PlaybackPhase::air && l.predicted_air == 0 && at - l.episode.takeoff >= 0.03) {
                const double elapsed = at - l.episode.takeoff;
                const auto vy = board_up_speed(base, component);
                l.predicted_air = vy && *vy > 0.3f
                    ? static_cast<float>(std::clamp(2 * (*vy + 9.81 * elapsed) / 9.81 - elapsed, 0.15, 3.0)) : -1.0f;
            }
            // The game's trick, read for as long as he is in the air rather than only just after takeoff.
            // A name arriving late used to be ignored, so a trick that turned into another one kept the
            // first one's keys at full weight all the way down, which is what looked wrong.
            const bool airborne = l.episode.phase == PlaybackPhase::air || l.episode.phase == PlaybackPhase::landing;
            const auto name = airborne ? current_trick(component) : std::string{};
            // A change after we had settled on one is the game doing something else: the layer stands back
            // over 120 ms, and only then takes up the new trick, so nothing jumps between the two.
            if (!name.empty() && l.game_trick && l.episode.phase == PlaybackPhase::air && !l.episode.selection_open(at) &&
                l.game_trick->trick != name && l.gave_way_at < 0) {
                l.gave_way_at = at;
                l.gave_way_from = l.game_trick->trick;
                logging::log(Level::info, Channel::skater, "Animation Studio: the game turned {} into {}; standing back.",
                             l.gave_way_from, name);
            }
            if (l.gave_way_at >= 0 && at - l.gave_way_at >= 0.12) {
                l.game_trick = nullptr; // let it be chosen again, for whatever the game is doing now
                l.gave_way_at = -1;
                l.limbs = {};
            }
            // PopHeight can arrive after the trick name. A refined grade must
            // invalidate a mismatched explicit binding without rewinding time.
            // The game's grade of his pop: alone in the game any grade is his; with others about (online, AI skaters), his
            // skater's own (pop_grades). Until that is known, the air time says.
            const auto since_takeoff_ms = static_cast<std::uint64_t>(std::max(0.0, at - l.episode.takeoff) * 1000);
            const auto now_ms = GetTickCount64();
            const auto graded = grades_heard.load(std::memory_order_acquire)
                                    ? pop_grades::local_pop_height(now_ms > since_takeoff_ms ? now_ms - since_takeoff_ms : 0, now_ms,
                                                                   grades_solo.load(std::memory_order_acquire), l.episode.id)
                                    : 0;
            const auto graded_name = graded_pop_name(graded);
            const auto pop = !graded_name.empty() ? std::string(graded_name) : pop_of(l.predicted_air);
            // Not while standing back: the change that started it would otherwise be taken up in this same
            // frame and the old trick dropped at once, so the stand-back was never seen. It is taken up when
            // the stand-back is over (gave_way_at goes back to -1 and the old trick is let go).
            const bool let_go = l.finished_episode == l.episode.id && !name.empty() && name == l.finished_trick;
            // Once the trick plays on the game's clip, a refined grade of the same trick keeps it: choosing again dropped it in
            // one frame (nothing for the new pop) or faded it out and in again (a new clock).
            const bool settled = l.game_trick && l.game_trick->trick == name && l.clock_locked_at >= 0;
            if (!let_go && !settled && l.gave_way_at < 0 && l.game_tricks &&
                episode_selection_changed(l.game_trick ? l.game_trick->trick : std::string_view{}, name,
                                          l.game_trick ? l.game_trick->edits->pop : std::string_view{}, pop)) {
                const CustomTrick* found = nullptr;
                // Exact pop first; cycle eligible variants once per named trick in this attempt.
                std::vector<const CustomTrick*> variants;
                // By front foot: a trick is the same trick whichever way round the skater stands, and the
                // one that already leads with the right foot is preferred so nothing is mirrored that does
                // not have to be. Off, it is the old rule: the exact stance and switch it was recorded in.
                // Every trigger the game gives at takeoff goes into one rule (choose_stance_variants): the foot
                // that leads (the game's mirror), rolling fakie, the pop it graded. Nollie is its own trick name,
                // so it is already apart here; board backwards turns only the board's frame when it plays.
                const LiveStance live{leads_right(l.playback_goofy, l.playback_switch.value_or(false)), l.playback_fakie};
                std::vector<const CustomTrick*> candidates;
                std::vector<StanceCandidate> described;
                // Its takeoff: the ones made off the same kind of takeoff (flat ground, or out of a grind) first; when there
                // are none, any of them - a trick edited from the flat still plays off a ledge, out of a manual or a grind,
                // or off a ramp, where the takeoff is not known. The edits ride on the game's own pose either way.
                if (l.playback_switch.has_value())
                    for (const bool exact : {true, false}) {
                        for (const auto& t : *l.game_tricks) {
                            if (t.trick != name || (exact && t.edits->entry != entry_name(l.episode.entry))) continue;
                            candidates.push_back(&t);
                            // A fallback version only where nothing better is; a trick on ANY pop while the pop is not known.
                            int rank = playback_pop_rank(pop, t.edits->pop);
                            if (rank == 0 && t.fallback) rank = 3;
                            if (pop.empty() && t.any_pop) rank = 1;
                            described.push_back({t.leads_right, t.fakie, rank});
                        }
                        if (!candidates.empty()) break;
                    }
                for (const auto i : choose_stance_variants(described, live, l.playback_front_foot)) variants.push_back(candidates[i]);
                if (!variants.empty()) {
                    const bool in_turns = playlist_turns_mode.load(std::memory_order_acquire) == static_cast<int>(PlaylistMode::turns);
                    // The playlist's In turns: in the playlist's order, on every pass of the attempt (the pick is a place in it).
                    if (in_turns && variants.size() > 1)
                        std::stable_sort(variants.begin(), variants.end(), [](const CustomTrick* a, const CustomTrick* b) { return a->order < b->order; });
                    // By pop too: the first choice of an air is often made on the air time's guess, before the game's grade.
                    // Keyed by the trick alone, a guess with one trick fixed the pick, and the real group then always
                    // played its first (cycling played one version).
                    const std::string pick_key = name + "|" + pop;
                    auto choice = l.attempt_variants.find(pick_key);
                    if (choice == l.attempt_variants.end()) {
                        // Which plays this attempt: any of them at random, but not the one that played last.
                        // (previous is the one that played last, plus one: 0 when none has yet.)
                        auto& previous = l.variant_counts[name + "|" + entry_name(l.episode.entry) + "|" + pop];
                        static thread_local std::mt19937 random{static_cast<std::uint32_t>(GetTickCount64())};
                        const std::size_t pick = next_variant(previous, variants.size(), in_turns, random);
                        previous = pick + 1;
                        choice = l.attempt_variants.emplace(pick_key, pick).first;
                    }
                    found = variants[choice->second % variants.size()];
                } else if (!name.empty() && l.stance_fallback_trick != name &&
                           std::any_of(l.game_tricks->begin(), l.game_tricks->end(), [&](const CustomTrick& t) {
                               return t.trick == name;
                           })) {
                    l.stance_fallback_trick = name;
                    logging::log(Level::info, Channel::skater,
                                 "Animation Studio: {} uses native animation; no saved trick for {} pop leading with the {} foot{} ({}).", name, pop,
                                 live.leads_right ? "right" : "left", live.fakie ? ", fakie" : "",
                                 l.playback_front_foot ? "front foot" : "exact stance");
                }
                if (found != l.game_trick) {
                    if (found) l.episode.select(at);
                    l.game_trick = found;
                    // Its clock starts looking for the game's clip of it.
                    l.native_clock.reset();
                    if (found) l.native_clock.start(found->native);
                    l.clock_selected_at = at; l.clock_locked_at = -1; l.clock_weight = 0; l.off_clip = 0; l.off_clip_said = false;
                    l.other_pop = 0; l.other_pop_said = false;
                    l.stretch.reset(); l.latch_touch_from = l.latch_last = -1;
                    l.latch_shape_ok = false;
                    if (found && found->caught > 0) {
                        l.latch_shape = board_shape(*found->clip);
                        l.latch_shape_ok = true;
                    }
                    l.clock_lost_at = l.clock_touch_at = -1; l.clock_touch_from = l.clock_last_time = -1;
                    l.drift_since = -1; l.drift_base = 0; l.drift_count = 0; l.clock_why.clear();
                    l.clock_fallback = !found || !found->native || found->native->models.empty();
                    if (found) {
                        const bool lead_matches = found->leads_right == live.leads_right;
                        logging::log(Level::info, Channel::skater,
                                     "Animation Studio: {} (made leading with the {} foot{}) plays - {} foot leading{}{} - {} ({}).", found->name,
                                     found->leads_right ? "right" : "left", found->fakie.value_or(false) ? ", fakie" : "",
                                     live.leads_right ? "right" : "left", live.fakie ? ", fakie" : "",
                                     l.playback_backwards ? ", board backwards" : "", lead_matches ? "as made" : "mirrored",
                                     l.playback_front_foot ? "front foot" : "exact stance");
                    }
                    if (found)
                        logging::log(Level::info, Channel::skater, "Animation Studio: the game does a {} ({} pop by {}, {:.2f} s in the air), playing your {} ({}).", name,
                                     pop.empty() ? "unknown" : pop, graded ? "the game's grade" : "air time", l.predicted_air,
                                     found->height.empty() ? found->name
                                                           : found->name + " as made for the " + found->height + " pop" +
                                                                 (found->fallback ? " (nothing of yours is for that pop)" : found->any_pop || found->edits->pop.empty() ? " (it is on ANY pop)" : ""),
                                     "authored edits over fresh native pose");
                    if (found && !found->edits->pop.empty() && found->edits->pop != pop)
                        logging::log(Level::info, Channel::skater, "Animation Studio: {} is made for a {} pop; with none of yours for a {} pop it plays on this one too.",
                                     found->name, found->edits->pop, pop.empty() ? "unknown" : pop);
                }
            }
            if (const auto* trick = l.game_trick) {
                // The other foot leading means the same trick, the other way round: the keys are mirrored
                // so the front foot does what the front foot did. Made once and kept.
                const bool mirror_it = l.playback_front_foot && l.playback_switch.has_value() &&
                                       trick->leads_right != leads_right(l.playback_goofy, l.playback_switch.value_or(false));
                auto keys = trick->edits;
                if (mirror_it) {
                    auto found = l.mirrored_edits.find(trick->edits.get());
                    if (found == l.mirrored_edits.end()) {
                        try {
                            found = l.mirrored_edits.emplace(trick->edits.get(),
                                                             std::make_shared<const Animation>(mirror_animation(*trick->edits))).first;
                        } catch (...) { found = l.mirrored_edits.end(); }
                    }
                    if (found != l.mirrored_edits.end()) keys = found->second;
                }
                l.played_mirrored = mirror_it;
                const auto& played = *keys;
                // Its Crossfade: its own (THIS TRICK: OWN), which skips the main one for it, or the main one (SETTINGS).
                l.crossfade_now = std::clamp(played.crossfade.value_or(snap->crossfade), 0.1f, 0.8f);
                // Standing back after the trick changed: the whole layer, over the same 120 ms.
                const float give_way = l.gave_way_at >= 0 ? std::clamp(1 - static_cast<float>(at - l.gave_way_at) / 0.12f, 0.0f, 1.0f) : 1.0f;
                const auto& clip = *trick->clip;
                // The game leads. Its own pose says which of its clips it plays and how far into it it is
                // (NativeClock); the edits are laid on its pose at that time, eased in as it is found and out
                // as the game moves into anything else. The brackets only mask the edits, as in the studio.
                PlaybackSample sampled;
                NativeClock::Reading reading;
                // Off a drop, down stairs or over a kicker the game stays in the air longer than the timeline did (or
                // shorter, onto a ledge): from its catch the trick takes its own time (AirStretch), slowed
                // through the catch - board caught, knees up - and on to its landing on the touchdown, the ground looked for
                // under the board. Never held still. On flat ground, in step with the game, it is the game's clock.
                std::optional<double> touch_at;
                if (l.episode.contact.since) touch_at = *l.episode.contact.since;
                else if (l.episode.phase == PlaybackPhase::landing && l.episode.landing >= 0) touch_at = l.episode.landing;
                const auto catch_latch = [&](float time) -> float {
                    const float caught = trick->caught, land = clip.landing;
                    if (!(caught > 0) || !(land > caught) || time < 0 || !l.latch_shape_ok) return time;
                    // A trick that let go before it was slowed (on a higher pop, its hand-back at the catch) fades out
                    // on the game's clock. Slowed from here, it stood still while it faded.
                    if (!l.stretch.on() && !l.clock_why.empty()) return time;
                    if (touch_at) {
                        if (!l.stretch.on()) return time;
                        if (l.latch_touch_from < 0) l.latch_touch_from = l.latch_last >= 0 ? l.latch_last : l.stretch.time;
                        return landed_time(static_cast<float>(at - *touch_at), land, l.latch_touch_from);
                    }
                    // How long until the ground, from how high the board's lowest point is over it and how fast it falls.
                    std::optional<float> to_ground;
                    if (l.board_ok) {
                        const auto mount = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                        const float lowest = lowest_point(mount, l.latch_shape);
                        auto& probe = ground_probe();
                        const float landing_in = probe.to_ground.load(std::memory_order_acquire);
                        const double found = probe.found_at.load(std::memory_order_acquire);
                        const auto velocity = board_velocity(base, component);
                        if (finite(mount) && std::isfinite(lowest) && velocity) {
                            probe.x.store(mount.position[0], std::memory_order_release);
                            probe.z.store(mount.position[2], std::memory_order_release);
                            probe.low.store(lowest, std::memory_order_release);
                            probe.vx.store((*velocity)[0], std::memory_order_release);
                            probe.vy.store((*velocity)[1], std::memory_order_release);
                            probe.vz.store((*velocity)[2], std::memory_order_release);
                            probe.wanted.store(true, std::memory_order_release);
                        }
                        // Worked out on the client thread a moment ago (along the board's path): less the time since.
                        if (landing_in >= 0 && found >= 0 && now_seconds() - found < 0.15)
                            to_ground = std::max(0.0f, landing_in - static_cast<float>(now_seconds() - found));
                    }
                    const bool was_on = l.stretch.on();
                    const float hold = played.catch_hold.value_or(default_catch_hold);
                    l.to_ground_now = to_ground ? *to_ground : -1.0f;
                    time = l.stretch.update(time, to_ground ? *to_ground : -1.0f, 1.0f / 60, clip.takeoff, caught, land, hold);
                    if (l.stretch.on() && !was_on)
                        logging::log(Level::info, Channel::skater,
                                     "Animation Studio: {} takes its own time from {:.2f} s ({} catch at {:.2f} s, hold {:.0f}%): {:.2f} s to the ground, "
                                     "{:.2f} s in the timeline.", trick->name, l.stretch.time,
                                     trick->edits && trick->edits->catch_at && *trick->edits->catch_at == caught ? "its maker's" : "the studio's", caught,
                                     hold * 100, to_ground ? *to_ground : -1.0f, land - l.stretch.time);
                    l.latch_last = time;
                    return time;
                };
                if (!l.clock_fallback) {
                    reading = l.native_clock.update(l.pose, 1.0f / 60); // the game's own pose, before any edit
                    if (!reading.locked && at - l.clock_selected_at > 0.25) {
                        l.clock_fallback = true;
                        logging::log(Level::info, Channel::skater, "Animation Studio: the game's clip for {} was not found ({:.1f} degrees off); "
                                     "it plays on its own clock.", trick->name, reading.error);
                    }
                }
                if (!l.clock_fallback) {
                    if (reading.locked && l.clock_locked_at < 0) {
                        l.clock_locked_at = at;
                        logging::log(Level::info, Channel::skater, "Animation Studio: {} follows the game's {}clip {} at {:.2f} s ({:.1f} degrees).",
                                     trick->name, reading.mirrored ? "mirrored " : "", reading.clip == 0 ? std::string("(its own)") : std::to_string(reading.clip),
                                     reading.time, reading.error);
                    }
                    if (reading.switched)
                        logging::log(Level::info, Channel::skater, "Animation Studio: {} moved onto the game's clip {} at {:.2f} s ({:.1f} degrees): it fits the game better.",
                                     trick->name, reading.clip == 0 ? std::string("(its own)") : std::to_string(reading.clip), reading.time, reading.error);
                    // The game's touchdown, and where the trick was at it: the trick's landing goes on the game's.
                    std::optional<double> touch;
                    if (l.episode.contact.since) touch = *l.episode.contact.since;
                    else if (l.episode.phase == PlaybackPhase::landing && l.episode.landing >= 0) touch = l.episode.landing;
                    float time = reading.locked ? reading.time : -1.0f;
                    const float up = clip.takeoff, down = clip.landing;
                    // On the game's clip of a higher pop than the trick was made on: from its catch (its own; else four fifths
                    // into its air, where a flip is done) the trick hands back to the game's own pose, crossfaded, and the game
                    // holds its catch and lands as it does. Played to its landing it stood still in the air until the game's
                    // touchdown, then let go in one frame. Down a drop it takes its own time to the touchdown instead (AirStretch).
                    float hand_back = -1;
                    if (reading.locked && reading.clip > 0 && trick->native && !l.stretch.on()) {
                        const auto c = static_cast<std::size_t>(reading.clip);
                        if (c < trick->native->higher_pop.size() && trick->native->higher_pop[c] && up >= 0 && down > up)
                            // The later of its catch and 85% of its air: an ollie's catch comes just after the pop, and it was
                            // handed back 0.2 s into the air with its style hardly seen (test game, 2026-10-26).
                            hand_back = std::max(trick->caught > up && trick->caught < down ? trick->caught : up, up + 0.85f * (down - up));
                    }
                    if (reading.locked && touch && l.clock_touch_at < 0) { l.clock_touch_at = *touch; l.clock_touch_from = time; }
                    if (reading.locked && reading.lost) {
                        // Off the game's clip: the game's own landing animation, or a recording drifting off it near the
                        // landing. The trick goes on at its own pace - your landing too, on the game's touchdown - until
                        // the game starts something new (below), instead of handing back to the game's landing.
                        if (l.clock_lost_at < 0) {
                            l.clock_lost_at = at;
                            logging::log(Level::info, Channel::skater, "Animation Studio: {} goes on at its own pace from {:.2f} s{} ({:.1f} degrees off the game's clip).",
                                         trick->name, time, touch ? ", after the touchdown" : ", in the air", reading.error);
                        }
                        if (up >= 0 && down > up && hand_back < 0) { // (on a higher pop it keeps its own pace to its catch)
                            if (!touch) time = up + fitted_air_time(time - up, down - up, 0); // eases in to just before its landing
                            else if (l.clock_touch_from >= 0 && l.clock_touch_from < down)
                                time = std::max(time, landed_time(static_cast<float>(at - *touch), down, l.clock_touch_from));
                        }
                    }
                    time = catch_latch(time);
                    if (time >= 0 && l.clock_last_time >= 0) time = std::max(time, l.clock_last_time);
                    if (time >= 0) l.clock_last_time = time;
                    // Letting go when the game starts something new: the next pop (its crouch), a grind, the end of the
                    // trick (its end bracket, or its clip's end), or - after the landing, off the clip - its pose going a
                    // long way from the trick (a push, a turn into something else).
                    const float edit_end = played.end >= 0 ? played.end : clip.duration();
                    std::string why;
                    if (l.episode.phase == PlaybackPhase::rail_release || motion == MotionState::rail) why = "a grind";
                    else if (touch && current_state && *current_state == 103) why = "the next pop";
                    // Off a drop nothing follows the game's clip after the touchdown to see it move on: riding on, any state
                    // but plain riding (100) is the game's next move (a manual, a powerslide).
                    else if (touch && l.stretch.on() && current_state && *current_state != 100 && motion == MotionState::riding)
                        why = "the game's next move";
                    else if (time >= 0 && time >= edit_end - 0.5f / 60) why = "its end";
                    // Rolling after the touchdown looks like the end of every trick, so the clock holds its time there (a held
                    // pose): a trick with keys after its landing then held its last pose until the pose drifted away, seconds
                    // later. From the touchdown it has its own remaining time, and 0.4 s more.
                    else if (touch && l.clock_touch_from >= 0 && edit_end > l.clock_touch_from &&
                             at - *touch > (edit_end - l.clock_touch_from) + 0.4)
                        why = "its end (its own time after the touchdown)";
                    else if (hand_back >= 0 && time >= hand_back) why = higher_pop_hand_back;
                    // Followed through the landing and lost well after it: that is the game going into its next
                    // animation (a push), not its landing.
                    else if (reading.locked && reading.lost && touch && l.clock_lost_at - *touch > 0.3) why = "the game's next animation";
                    else if (reading.locked && reading.lost && touch) {
                        // Quick to notice (two updates), so the crossfade into a manual or the next move starts while the
                        // game is still blending into it.
                        if (l.drift_since < 0) { l.drift_since = at; l.drift_base = reading.error; l.drift_count = 0; }
                        else if (at - l.drift_since < 0.1) l.drift_base = std::min(l.drift_base, reading.error);
                        else {
                            l.drift_count = reading.error > l.drift_base + 6.0f ? l.drift_count + 1 : 0;
                            if (l.drift_count >= 2) why = "the game's next animation";
                        }
                    }
                    if (!why.empty() && l.clock_why.empty()) {
                        l.clock_why = why;
                        logging::log(Level::info, Channel::skater, "Animation Studio: {} lets go at {:.2f} s: {}.", trick->name, time, why);
                    }
                    if (!l.clock_why.empty()) why = l.clock_why; // once let go, it stays let go
                    // In over 0.1 s, out over 0.3 s, both eased, so handing over to the game's next move (a manual, the
                    // next trick) is a smooth crossfade rather than a snap. The trick's end is eased into before it comes.
                    const float target = reading.locked && why.empty() ? 1.0f : 0.0f;
                    constexpr float step = 1.0f / 60, fade_in = 0.1f;
                    // SETTINGS > Tricks: Crossfade, or the trick's own (THIS TRICK: OWN), which skips the main one for it.
                    const float fade_out = l.clock_why == higher_pop_hand_back ? std::max(l.crossfade_now, 0.3f) : l.crossfade_now;
                    l.clock_weight = target > l.clock_weight ? std::min(target, l.clock_weight + step / fade_in)
                                                             : std::max(target, l.clock_weight - step / fade_out);
                    const auto ease = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); };
                    const float before_end = time >= 0 ? ease((edit_end - time) / fade_out) : 1.0f;
                    sampled.time = time;
                    sampled.weight = ease(l.clock_weight) * before_end;
                    sampled.finished = reading.locked && !why.empty() && l.clock_weight <= 0;
                } else {
                    // Always at x1 in game: the speed is the studio's, for watching a trick (it was the saved one, so a trick
                    // saved while watched at x.5 played at half speed on its own clock).
                    sampled = l.episode.sample(at, clip.takeoff, clip.landing, clip.duration(), 1.0f, l.predicted_air, true, fit_keys_to_air);
                    if (sampled.time >= 0) sampled.time = std::min(clip.duration(), catch_latch(sampled.time));
                }
                // Off the game's clip in the air - the game holding a pose of its own through a kicker's or a drop's longer
                // air, or no clip of the game's found at all: the keys rode on a pose made for something else (a foot
                // through the deck, a pose not the timeline's). Then they go on the pose they were made on, and the feet on
                // the deck as the studio has them; by the touchdown it is the game's pose again, as on flat ground.
                // (From 2026-10-25 15:44 to 23:30 every trick also played its catch as made and held the feet on the deck from the
                // pop on, for feet through the deck that were the board keys' frame - fixed on its own. It held the feet to the
                // board after the catch like a magnet, on every pop: taken out again.)
                // Down a drop the pose as made (off_clip, and other_pop's whole body and board) goes back to the game's over
                // the end of the fall. Let go at the touchdown, the upright catch turned into the landing's crouch and the
                // board as made, 32 cm under the game's, came up onto it in one frame (a tre down the Tri-Towers gap, 2026-10-26).
                // The look at the ground jumps by up to 0.06 s from one update to the next (the board's path is looked along
                // again each time), and the board blended with it jerked 6 cm to and fro: it is followed smoothly, counting
                // down as the time goes, and once the pose goes back it only counts down (one look of 0.83 s among 0.36s
                // moved the board 4 cm back).
                if (!(l.stretch.on())) l.to_ground_smooth = -1;
                else if (l.to_ground_smooth < 0) l.to_ground_smooth = l.to_ground_now;
                else {
                    const float expected = std::max(0.0f, l.to_ground_smooth - 1.0f / 60);
                    const float next = l.to_ground_now >= 0 ? expected + (l.to_ground_now - expected) * 0.15f : expected;
                    l.to_ground_smooth = l.to_ground_smooth < drop_pose_back_by + drop_pose_back_over ? std::min(next, expected) : next;
                }
                const float as_made_till_ground = l.to_ground_smooth >= 0
                    ? std::clamp((l.to_ground_smooth - drop_pose_back_by) / drop_pose_back_over, 0.0f, 1.0f) : 1.0f;
                {
                    const bool touched_down = l.episode.contact.since.has_value() || (l.episode.phase == PlaybackPhase::landing && l.episode.landing >= 0);
                    const bool off = !touched_down && ((air_corrections && (l.clock_fallback || (reading.locked && reading.lost))) || (l.stretch.on() && l.clock_why.empty()));
                    constexpr float off_step = (1.0f / 60) / 0.15f;
                    l.off_clip = off ? std::min(1.0f, l.off_clip + off_step) : std::max(0.0f, l.off_clip - off_step);
                    l.off_clip = std::min(l.off_clip, as_made_till_ground);
                    if (off && !l.off_clip_said) {
                        l.off_clip_said = true;
                        logging::log(Level::info, Channel::skater, "Animation Studio: {} is off the game's clip in the air ({}): keys as made, feet on the deck as in the studio.",
                                     trick->name, l.clock_fallback ? "no clip of the game's found" : l.stretch.on() ? "it takes its own time off a drop" : "the game holds a pose of its own");
                    }
                }
                // A trick made on one pop, on the game's clip of another (its pack says any pop): the keys laid on that clip's
                // body - tucked otherwise on a higher pop - drew the legs through the board (renztre, low, on a high pop). It
                // plays whole as made instead: its own body and board with its keys, timed by the board (board_time_map) so
                // the flip keeps its pace and the extra air is held at the catch, as the game's own pops do; where he is in
                // the air is the game's.
                {
                    const auto* pops = trick->native ? &trick->native->other_pop : nullptr;
                    const bool other = pops && !l.clock_fallback && reading.locked && reading.clip > 0 &&
                                       static_cast<std::size_t>(reading.clip) < pops->size() && (*pops)[static_cast<std::size_t>(reading.clip)];
                    constexpr float other_step = (1.0f / 60) / 0.15f;
                    l.other_pop = other ? std::min(1.0f, l.other_pop + other_step) : std::max(0.0f, l.other_pop - other_step);
                    l.other_pop = std::min(l.other_pop, as_made_till_ground);
                    if (other && !l.other_pop_said) {
                        l.other_pop_said = true;
                        logging::log(Level::info, Channel::skater, "Animation Studio: {} is on the game's clip of another pop (clip {}): it plays whole, as made.",
                                     trick->name, reading.clip);
                    }
                }
                diagnostic.clock_error = reading.error;
                diagnostic.clock_clip = l.clock_fallback ? -2 : reading.clip;
                diagnostic.clock_state = l.clock_fallback ? "own clock" : !reading.locked ? "looking" : !l.clock_why.empty() ? "let go"
                                       : reading.lost ? "own pace" : "following";
                const float weight = sampled.weight * animation_window_weight(played, sampled.time) * give_way;
                diagnostic.preset = trick->name;
                diagnostic.source_kind = played.source.kind;
                diagnostic.recorded_goofy = trick->goofy; diagnostic.recorded_switch = played.switch_stance;
                diagnostic.retarget = false; diagnostic.mirror = l.played_mirrored;
                diagnostic.reverse_facing = false;
                diagnostic.source_timing = !l.clock_fallback;
                diagnostic.clip_time = sampled.time; diagnostic.weight = weight;
                if (sampled.time >= 0 && !sampled.finished) {
                    auto packet = std::make_shared<GameplayPacket>();
                    packet->generation = generation; packet->episode = l.episode.id;
                    packet->sequence = l.animation_updates.load(); packet->epoch = gameplay_epoch.load(std::memory_order_acquire);
                    packet->component = component; packet->board_holder = l.board_holder.load(std::memory_order_acquire);
                    packet->at = at; packet->time = sampled.time; packet->weight = weight;
                    packet->clip = trick->clip; packet->edits = keys; packet->limbs = l.limbs;
                    // The edits ride on the game's own pose (its IK, its height, its board), at its clip's time: nothing of
                    // the studio's pose is forced over it - but off the game's clip in the air (off_clip), the keyed joints go
                    // to the pose they were made on and the feet onto the deck as the studio has them.
                    const float off = l.off_clip * l.off_clip * (3 - 2 * l.off_clip);
                    const float whole = l.other_pop * l.other_pop * (3 - 2 * l.other_pop);
                    packet->mirrored = l.played_mirrored; packet->match = std::max(off, whole); packet->feet_match = off;
                    packet->board_backwards = l.playback_backwards;
                    packet->fakie = l.playback_fakie;
                    packet->match_rest = whole; // on another pop's clip: body and board as made (other_pop)
                    packet->limits = snap->limits;
                    // The board keys are authored in the board's own frame and played in it (nose-relative both
                    // times), so they need no turning for fakie. A deck turned round under him is the game's
                    // board-backwards flag, applied where the board is drawn.
                    const Quat takeoff_frame = l.board_frame_local;
                    packet->board_frame_local = takeoff_frame; packet->board_frame_valid = l.board_at_takeoff_ok;
                    packet->board_owned = l.episode.phase != PlaybackPhase::rail_release;
                    scheduled = std::move(packet);
                    // Diagnostic prediction only. The renderer evaluates these edits again over its
                    // fresh post-physics native pose; no body/board snapshot is queued for gameplay.
                    if (diagnostic_pose) {
                        apply_authored_edit_layer(played, sampled.time, l.pose, weight, &l.limbs);
                    }
                    if (scheduled->board_owned && scheduled->board_frame_valid && l.board_ok) {
                        Joint frame; frame.rotation = normalised(multiply(live_placement.rotation, takeoff_frame));
                        const auto native_board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                        board_target = board_edited(native_board, played, sampled.time, ground_turn(frame));
                        board_weight = weight;
                    }
                }
                if (sampled.finished) {
                    l.finished_episode = l.episode.id;
                    l.finished_trick = trick->trick;
                    l.game_trick = nullptr;
                    l.native_clock.reset();
                }
            }
            play_pieces(l, *snap, at, generation, component, live_placement, scheduled, board_target, board_weight, motion, current_state);
        }
        trick_blend.store(std::clamp((l.game_trick && l.crossfade_now > 0 ? l.crossfade_now : snap->crossfade) * 0.5f, 0.08f, 0.4f),
                          std::memory_order_relaxed);
        publish_gameplay_packet(std::move(scheduled));
        if (diagnostics_capture) {
            diagnostic.observed_phase = snap->open ? "studio_preview" : phase_name(l.episode.phase);
            if (snap->open) {
                diagnostic.clip_time = shown_time;
                diagnostic.preset = shown_edits.name;
                diagnostic.trick = shown_edits.trick;
                diagnostic.source_kind = shown_edits.source.kind;
                if (diagnostic_pose && shown) {
                    diagnostic.source.resize(shown->joints);
                    shown->sample(shown_time, diagnostic.source);
                    Joint source_board;
                    if (shown->sample_board(shown_time, source_board))
                        diagnostic.source_board = compose(world(diagnostic.source, placement_joint), source_board);
                }
            }
            diagnostic.entry = entry_name(l.episode.entry); diagnostic.episode = l.episode.id;
            diagnostic.takeoff_goofy = l.playback_goofy;
            diagnostic.takeoff_switch = l.playback_switch;
            diagnostic.board_weight = board_weight;
            if (board_weight > 0) diagnostic.desired_board = board_target;
            if (diagnostic_pose) diagnostic.desired = l.pose;
            { auto& shot = render_shot(); std::lock_guard lock(shot.mutex);
              diagnostic.skater_publications = shot.skater_publications;
              diagnostic.board_publications = shot.board_publications;
              diagnostic.render_edit_sequence = shot.edited_sequence;
              diagnostic.render_presentation = shot.edited_presentation;
              diagnostic.board_edit_sequence = shot.edited_board_sequence;
              diagnostic.board_presentation = shot.edited_board_presentation;
              if (diagnostic_pose && shot.board_generation == generation) {
                  diagnostic.board_rig = shot.board_rig;
                  diagnostic.board_rig_at = shot.board_at;
              }
              if (shot.skater_generation == generation && shot.at >= 0) diagnostic.skater_age = at - shot.at;
              if (shot.board_generation == generation && shot.board_at >= 0) diagnostic.board_age = at - shot.board_at;
              if (diagnostic_pose && shot.skater_generation == generation && shot.at >= 0) {
                  if (shot.edited_generation == generation && shot.edited_at >= 0) {
                      diagnostic.render_edited = shot.edited_skater; diagnostic.render_edited_at = shot.edited_at;
                  }
                  if (shot.edited_board_generation == generation && shot.edited_board_at >= 0) {
                      diagnostic.render_edited_board = shot.edited_board; diagnostic.render_edited_board_at = shot.edited_board_at;
                  }
                  diagnostic.render_at = shot.at;
                  diagnostic.render_native.reserve(shot.skater.size());
                  for (const auto& bone : shot.skater) diagnostic.render_native.push_back(joint_of(bone));
                  if (shot.board_generation == generation && std::abs(shot.at - shot.board_at) < 0.05) {
                      diagnostic.render_board = shot.board; diagnostic.render_board_at = shot.board_at;
                  }
              } }
            auto& history = snap->open ? preview_history() : diagnostic_history(); std::lock_guard lock(history.mutex);
            history.buffer.append(std::move(diagnostic));
        }
        auto preview = snap->open ? std::make_shared<PreviewPacket>() : nullptr;
        if (last > first) {
            for (std::size_t i = first; i < last; ++i)
                if (!finite(l.pose[i])) throw std::runtime_error("an edit made a broken pose; nothing was written");
            for (std::size_t i = first; i < last; ++i) {
                auto& b = l.bones[i];
                const auto& j = l.pose[i];
                std::memcpy(b.scale.data(), j.scale.data(), 12);
                b.rotation = j.rotation;
                std::memcpy(b.position.data(), j.position.data(), 12);
            }
            // Written when the pose is handed to the renderer (on_render): the skater's body physics
            // rebuild this buffer after the animation update, which a write now would not survive (seen
            // 2026-10-18: the buffer held the edit, the drawn skater did not).
            auto& p = *preview;
            p.buffer = layout.buffer;
            p.first = static_cast<std::uint32_t>(first);
            p.last = static_cast<std::uint32_t>(last);
            p.bones.assign(l.bones.begin() + static_cast<std::ptrdiff_t>(first), l.bones.begin() + static_cast<std::ptrdiff_t>(last));
            p.at = at;
            p.generation = generation;
        }
        if (preview) {
            auto& pb = *preview;
            pb.weight = snap->open && board_weight > 0 && finite(board_target) ? board_weight : 0;
            pb.mount = board_target;
            pb.at = at;
            pb.generation = generation;
            pb.holder = l.board_holder.load(std::memory_order_acquire);
            pb.sequence = l.animation_updates.load(std::memory_order_acquire);
        }
        // Publish both parts together. Whichever render callback arrives first pins this
        // immutable sample for the current presentation, including a missing body/board.
        publish_preview_packet(std::move(preview));
        if (!snap->open) l.floor_valid = false;
        if (snap->open) {
            std::optional<Joint> board;
            if (board_weight > 0 && finite(board_target)) board = board_target;
            else if (l.board_ok) board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
            publish_joints(l.pose, live_placement, board, board_frame);
            if (studio_snapshot::on() && !(snap->clip && !snap->clip->empty()))
                studio_snapshot::studio_frame(at, snap->goofy, true, mirrored_body, l.pose, live_placement, board);
        } else if (studio_snapshot::on()) {
            studio_snapshot::closed();
        }
        if (l.failure_shown) {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            s.failure.clear();
            l.failure_shown = false;
        }
    } catch (const std::exception& e) {
        invalidate_outputs();
        if (l.failures++ % 600 == 0) logging::log(Level::warning, Channel::skater, "Animation Studio: {}", e.what());
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.failure = e.what();
        l.failure_shown = true;
    } catch (...) {
        invalidate_outputs();
    }
    SetLastError(error);
}

// What other players get of the local skater: a saved trick that plays in game, as drawn here.
struct BodyPublicationState {
    std::mutex mutex; EditPublicationCache edits; EditTransition transition;
    std::vector<Offset> network_deltas;
    std::uint64_t network_generation{}, network_epoch{};
    double network_at{-1};
    // The feet as drawn on the last frame in the air, in the skater's placement frame; at a rail's lock, each foot on
    // the board as drawn then, and how much of the fade was left (slide_feet_on_the_games_board).
    std::array<Joint, 2> feet_in_the_air{};
    double feet_in_the_air_at{-1};
    std::array<Joint, 2> feet_on_board_at_lock{};
    float fade_left_at_lock{};
    bool on_rail{}, feet_at_lock_ok{};
};
BodyPublicationState& body_publication_state() { static auto* state = new BodyPublicationState; return *state; }
// While the studio is open, the lobby does not see it. The skater stands in the studio's room - the game's menu room,
// under the map, where every player's own menu skater is drawn, so the others found him standing in their character
// menu - and strikes the poses being edited. They are sent the skater and the board as last sent before it opened (he
// stands where he was), and nothing of the room; a few seconds more after the room, while the game gives its menu
// skater back.
struct Outgoing {
    std::mutex mutex;
    std::vector<multiplayer::Transform> skater, board; // as last sent with the studio closed
};
Outgoing& outgoing() { static auto* value = new Outgoing; return *value; }
bool hidden_from_lobby() noexcept {
    return is_open() || room_flag.load(std::memory_order_acquire) || now_seconds() < room_restore_until.load(std::memory_order_acquire);
}
void saved_tricks_to_send(std::uintptr_t component, std::vector<multiplayer::Transform>& skater) noexcept; // below
void on_send(std::uintptr_t component, std::vector<multiplayer::Transform>& skater) noexcept {
    try {
        if (component != live().component.load(std::memory_order_acquire)) return;
        auto& o = outgoing();
        if (hidden_from_lobby()) {
            std::lock_guard lock(o.mutex);
            if (o.skater.size() == skater.size()) skater = o.skater;
            else if (skater.size() > placement_joint) skater[placement_joint].scale = {0.001f, 0.001f, 0.001f}; // none sent yet: not shown
            return;
        }
        saved_tricks_to_send(component, skater);
        std::lock_guard lock(o.mutex);
        o.skater = skater;
    } catch (...) {}
}
void on_send_board(std::vector<multiplayer::Transform>& board) noexcept {
    try {
        auto& o = outgoing();
        std::lock_guard lock(o.mutex);
        if (!hidden_from_lobby()) {
            o.board = board;
            return;
        }
        if (o.board.size() == board.size()) board = o.board;
        else
            for (std::size_t i = 1; i < board.size(); ++i) board[i].scale = {0, 0, 0}; // none sent yet: not shown
    } catch (...) {}
}
// A saved trick that plays in game, as drawn here, for the pose other players get.
void saved_tricks_to_send(std::uintptr_t component, std::vector<multiplayer::Transform>& skater) noexcept {
    try {
        if (skater.size() < skeleton_joints) return;
        std::vector<Joint> native(skater.size());
        for (std::size_t i = 0; i < skater.size(); ++i) native[i] = {skater[i].scale, skater[i].rotation, skater[i].position};
        const auto holder = read<std::uintptr_t>(component + 0xa0).value_or(0);
        const auto layout = multiplayer::read_native_pose_layout(peek, live().base.load(), holder, 512);
        if (!layout.buffer || layout.count != native.size()) return;
        { auto& publication = body_publication_state(); std::lock_guard lock(publication.mutex);
          const auto gen = live().generation.load();
          publication.edits.restore(gen, live().animation_updates.load(), layout.buffer, native);
          const auto motion = live().render_motion.load();
          if (overrides_allowed.load() && motion != MotionState::unknown && motion != MotionState::offboard &&
              publication.network_generation == gen && publication.network_epoch == gameplay_epoch.load() && now_seconds() - publication.network_at < 0.1)
              apply_pose_deltas(publication.network_deltas, native); }
        for (const auto& joint : editable_joints()) {
            if (joint.index >= skeleton_joints) continue;
            skater[joint.index].rotation = native[joint.index].rotation;
            skater[joint.index].position = native[joint.index].position;
        }
    } catch (...) {}
}

// The room's own skater and board (the menu's): hidden while the studio
// shows in the room, given back after it. A skater is hidden by a tiny scale on its placement joint (a
// move, or scale 0, can move the game's camera or hide the room), a board by scale 0 on every bone.
// The game does not set the scale again, so it is given back here (PR #230 does the same).
void hide_in_room(std::uintptr_t animation_interface, bool skater) noexcept {
    const bool hide = room_flag.load(std::memory_order_acquire), watch = room_watch.load(std::memory_order_acquire);
    if (!hide && !watch && now_seconds() > room_restore_until.load(std::memory_order_acquire)) return;
    auto& l = live();
    const auto holder = animation_interface - 0xc0;
    const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), holder, 512);
    if (!layout.buffer || layout.count != (skater ? skeleton_joints : board_bones)) return;
    std::array<NativeBone, 3> b{};
    if (!peek(layout.buffer, b.data(), sizeof(b))) return;
    const auto at = compose(compose(joint_of(b[0]), joint_of(b[1])), skater ? Joint{} : joint_of(b[2])).position;
    for (std::size_t i = 0; i < 3; ++i)
        if (!(std::abs(at[i] - room_spot[i]) < room_reach[i])) return;
    if (skater) {
        room_seen_at.store(now_seconds(), std::memory_order_release);
        if (b[1].scale[0] >= 0.01f) {
            const auto placed = compose(joint_of(b[0]), joint_of(b[1]));
            std::lock_guard lock(room_facing_mutex);
            room_facing = placed.rotation;
            room_facing_valid = finite(placed);
        }
        const float scale = hide ? 0.001f : 1.0f;
        if ((b[1].scale[0] < 0.01f) == hide) return;
        const std::array<float, 3> s3{scale, scale, scale};
        (void)poke(layout.buffer + sizeof(NativeBone), s3.data(), sizeof(s3));
        return;
    }
    const float scale = hide ? 0.0f : 1.0f;
    if ((b[2].scale[0] == 0.0f) == hide) return;
    for (std::uint32_t i = 0; i < layout.count; ++i) {
        const std::array<float, 3> s3{scale, scale, scale};
        (void)poke(layout.buffer + i * sizeof(NativeBone), s3.data(), sizeof(s3));
    }
}

// ---- playback verification (test builds: RESKATE_PLAYBACK_VERIFY=1) --------------------------------
// Measures, every frame a saved trick is fully on, how far what the game DRAWS is from what the studio shows
// for that moment: the recording with its keys, mirrored the game's way when played mirrored, keyed joints in
// their limits. The drawn pose is read back from the skater's buffer after the board is drawn, so nothing the
// game does after our write can hide. One line per trick in ReSkate.Tricks/research/playback-verify.txt.
namespace playback_verify {
bool on() {
    static const bool enabled = [] {
        wchar_t flag[4]{};
        return GetEnvironmentVariableW(L"RESKATE_PLAYBACK_VERIFY", flag, 4) == 1 && flag[0] == L'1';
    }();
    return enabled;
}
struct Expected {
    std::uint64_t presentation{~std::uint64_t{0}}, episode{};
    double at{-1};
    float time{};
    std::vector<Joint> pose, native;
    std::string name;
    bool mirrored{}, fakie{}, backwards{};
};
struct Totals {
    std::uint64_t episode{};
    std::string name;
    bool mirrored{}, fakie{}, backwards{};
    std::size_t frames{};
    double body_sum{}, native_sum{};
    float body_max{}, hips_max{}, board_deg_max{}, board_cm_max{}, feet_cm_max{};
    std::uint16_t worst{};
    double last_at{-1};
    float t_first{-1}, t_last{};
};
std::mutex mutex;
Expected expected;
Totals totals;
float angle(const Quat& a, const Quat& b) noexcept {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 57.2957795f;
}
float distance_cm(const Vec3& a, const Vec3& b) noexcept {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z) * 100;
}
void flush_locked() {
    if (!totals.frames) return;
    const auto* worst = editable(totals.worst);
    const auto line = std::format(
        "{:<22} {:<9} {:<6} {:<10} frames {:4}  t {:.2f}-{:.2f}  body mean {:5.2f} max {:5.2f} deg ({})  hips {:4.1f} cm  board {:5.2f} deg {:4.1f} cm  "
        "feet on board {:4.1f} cm  | the game's own pose was {:5.2f} deg from the studio",
        totals.name.substr(0, 22), totals.mirrored ? "mirrored" : "as made", totals.fakie ? "fakie" : "normal",
        totals.backwards ? "board-back" : "board-fwd", totals.frames, totals.t_first, totals.t_last,
        totals.body_sum / static_cast<double>(totals.frames), totals.body_max, worst ? std::string(worst->label) : std::to_string(totals.worst),
        totals.hips_max, totals.board_deg_max, totals.board_cm_max, totals.feet_cm_max,
        totals.native_sum / static_cast<double>(totals.frames));
    try {
        const auto folder = library_folder() / L"research";
        std::filesystem::create_directories(folder);
        std::ofstream file(folder / "playback-verify.txt", std::ios::binary | std::ios::app);
        file << line << '\n';
    } catch (...) {}
    logging::log(Level::info, Channel::skater, "Animation Studio verify: {}", line);
    totals = {};
}
// Render thread, body: what the studio shows for this packet, kept for the board render to compare with.
void stash(const GameplayPacket& packet, std::uint64_t presentation, const std::vector<Joint>& native, double now) {
    if (!on() || !packet.clip || !packet.edits || packet.clip->joints != skeleton_joints || native.size() < skeleton_joints) return;
    std::lock_guard lock(mutex);
    if (totals.frames && (totals.episode != packet.episode || now - totals.last_at > 0.3)) flush_locked();
    if (packet.weight < 0.98f) return;
    const auto studio = studio_pose(*packet.clip, packet.time, packet.mirrored);
    auto pose = native;
    for (std::uint16_t j = 0; j < skeleton_joints; ++j)
        if (gameplay_body_joint(j) && editable(j)) pose[j] = studio[j];
    apply_edits(*packet.edits, packet.time, pose); // the board track (900) is past the skeleton and left out
    if (packet.limits) limit_keyed_joints(*packet.edits, packet.time, pose);
    expected = {presentation, packet.episode, now, packet.time, std::move(pose), native, packet.edits->name, packet.mirrored, packet.fakie,
                packet.board_backwards};
}
// Render thread, board: the drawn body read back now, the drawn board as written, against the studio's.
void compare(std::uint64_t presentation, double now, const std::vector<Joint>& drawn, const Joint& drawn_board, const Joint& studio_board) {
    if (!on()) return;
    std::lock_guard lock(mutex);
    if (expected.presentation != presentation || expected.pose.size() < skeleton_joints || drawn.size() < skeleton_joints || now - expected.at > 0.05) return;
    if (totals.frames && totals.episode != expected.episode) flush_locked();
    if (!totals.frames) {
        totals.episode = expected.episode;
        totals.name = expected.name;
        totals.mirrored = expected.mirrored;
        totals.fakie = expected.fakie;
        totals.backwards = expected.backwards;
        totals.t_first = expected.time;
    }
    float sum = 0, native_sum = 0;
    int count = 0;
    for (const auto& e : editable_joints()) {
        const auto j = e.index;
        if (j >= skeleton_joints || !gameplay_body_joint(j)) continue;
        const float error = angle(drawn[j].rotation, expected.pose[j].rotation);
        sum += error;
        native_sum += angle(expected.native[j].rotation, expected.pose[j].rotation);
        ++count;
        if (error > totals.body_max) {
            totals.body_max = error;
            totals.worst = j;
        }
    }
    if (count) {
        totals.body_sum += sum / static_cast<float>(count);
        totals.native_sum += native_sum / static_cast<float>(count);
    }
    totals.hips_max = std::max(totals.hips_max, distance_cm(drawn[hips_joint].position, expected.pose[hips_joint].position));
    totals.board_deg_max = std::max(totals.board_deg_max, angle(drawn_board.rotation, studio_board.rotation));
    totals.board_cm_max = std::max(totals.board_cm_max, distance_cm(drawn_board.position, studio_board.position));
    for (const std::uint16_t foot : {std::uint16_t{10}, std::uint16_t{343}}) {
        const auto on_drawn = relative(drawn_board, world(drawn, foot)).position;
        const auto on_studio = relative(studio_board, world(expected.pose, foot)).position;
        totals.feet_cm_max = std::max(totals.feet_cm_max, distance_cm(on_drawn, on_studio));
    }
    ++totals.frames;
    totals.t_last = expected.time;
    totals.last_at = now;
}
// Render thread, body, no trick on: a finished trick is written out.
void idle(double now) {
    if (!on()) return;
    std::lock_guard lock(mutex);
    if (totals.frames && now - totals.last_at > 0.3) flush_locked();
}
} // namespace playback_verify

void seat_feet_on_board(const GameplayPacket& packet, std::span<const Joint> native, std::vector<Joint>& edited) noexcept; // below
bool slide_feet_on_the_games_board(BodyPublicationState& body, MotionState motion, bool playing, std::span<const Joint> native,
                                   std::vector<Joint>& edited) noexcept; // below
void apply_gameplay_body(std::uintptr_t holder, const std::shared_ptr<const GameplayPacket>& packet, std::uint64_t presentation) {
    auto& l = live();
    auto& publication = body_publication_state(); std::lock_guard lock(publication.mutex);
    // An animation update landing while this ran left a newer pose under it: it is read and edited again
    // (once) rather than left unedited for a frame, which drew the game's plain pose in the middle of a trick.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto generation = l.generation.load(std::memory_order_acquire);
        const auto update = l.animation_updates.load(std::memory_order_acquire);
        const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(), holder, 512);
        if (!layout.buffer || layout.count != skeleton_joints) return;
        std::vector<NativeBone> current(layout.count);
        if (!peek(layout.buffer, current.data(), current.size() * sizeof(NativeBone))) return;
        std::vector<Joint> native; native.reserve(current.size());
        for (const auto& bone : current) native.push_back(joint_of(bone));
        publication.edits.restore(generation, update, layout.buffer, native);
        if (packet) playback_verify::stash(*packet, presentation, native, now_seconds());
        else playback_verify::idle(now_seconds());
        auto edited = native;
        if (packet) {
            std::vector<Joint> studio;
            if (packet->match > 0 && packet->clip && packet->clip->joints == skeleton_joints)
                studio = studio_pose(*packet->clip, packet->time, packet->mirrored);
            apply_authored_edit_layer(*packet->edits, packet->time, edited, packet->weight, &packet->limbs, studio, packet->match,
                                      packet->match_rest);
            if (packet->limits) limit_keyed_joints(*packet->edits, packet->time, edited);
            seat_feet_on_board(*packet, native, edited);
        }
        const auto desired_deltas = pose_edit_deltas(native, edited);
        edited = native;
        std::vector<Offset> applied_deltas;
        const auto motion = l.render_motion.load(std::memory_order_acquire);
        if (overrides_allowed.load() && motion != MotionState::unknown && motion != MotionState::offboard) {
            const auto deltas = publication.transition.evaluate(now_seconds(), generation, packet ? packet->episode : 0,
                packet ? reinterpret_cast<std::uintptr_t>(packet->edits.get()) : 0, desired_deltas,
                trick_blend.load(std::memory_order_relaxed));
            apply_pose_deltas(deltas, edited);
            applied_deltas.assign(deltas.begin(), deltas.end());
            // Other players get the feet on the board too.
            if (slide_feet_on_the_games_board(publication, motion, packet != nullptr, native, edited))
                applied_deltas = pose_edit_deltas(native, edited);
            // Fading out after a trick handed back (no packet, so seat_feet_on_board does not run), rolling on the ground:
            // no foot is drawn lower than the game has it, whatever the last keys leave in the fade.
            if (!packet && motion == MotionState::riding)
                for (const std::uint16_t foot : {left_foot_joint, right_foot_joint}) {
                    const auto drawn = world(edited, foot), own = world(native, foot);
                    if (finite(drawn) && finite(own) && drawn.position[1] < own.position[1] - 0.01f)
                        place_foot(edited, foot, Vec3{drawn.position[0], own.position[1], drawn.position[2]}, nullptr, 1.0f);
                }
        } else publication.transition.reset();
        if (generation != l.generation.load(std::memory_order_acquire) || update != l.animation_updates.load(std::memory_order_acquire) ||
            (packet && packet->epoch != gameplay_epoch.load(std::memory_order_acquire))) continue;
        for (const auto& joint : edited)
            if (!finite(joint)) throw std::runtime_error("Native edit layer produced an invalid joint");
        for (std::size_t i = 0; i < edited.size(); ++i) {
            if (edited[i] == joint_of(current[i])) continue;
            auto& bone = current[i]; bone.rotation = edited[i].rotation;
            std::memcpy(bone.position.data(), edited[i].position.data(), 12);
            if (!poke(layout.buffer + i * sizeof(NativeBone), &bone, sizeof(bone))) throw std::runtime_error("Cannot publish native edits");
        }
        publication.network_deltas = std::move(applied_deltas);
        publication.network_generation = generation; publication.network_at = now_seconds();
        publication.network_epoch = gameplay_epoch.load();
        if (diagnostics_capture) {
            auto& shot = render_shot(); std::lock_guard shot_lock(shot.mutex);
            shot.edited_skater = edited; shot.edited_at = now_seconds();
            shot.edited_generation = generation; shot.edited_sequence = packet ? packet->sequence : 0;
            shot.edited_episode = packet ? packet->episode : 0; shot.edited_presentation = presentation;
            // Strip our exact previous writes from the diagnostic/recording baseline too.
            shot.skater = current;
            for (std::size_t i = 0; i < native.size(); ++i) {
                shot.skater[i].rotation = native[i].rotation;
                std::memcpy(shot.skater[i].position.data(), native[i].position.data(), 12);
            }
        }
        publication.edits.commit(generation, update, layout.buffer, std::move(native), std::move(edited));
        return;
    }
}

// The local skater's pose handed to the renderer: the studio's pose goes in now, over whatever the
// body physics left there, each joint's own lanes only (each bone's 4th scale/position lane stays).
void on_render(std::uintptr_t animation_interface) noexcept {
    auto& l = live();
    const auto generation = l.generation.load(std::memory_order_acquire);
    const auto component = l.component.load(std::memory_order_acquire);
    if (!component) return;
    const auto error = GetLastError();
    try {
        const auto holder = read<std::uintptr_t>(component + 0xa0);
        if (!holder || !*holder || animation_interface != *holder + 0xc0) {
            hide_in_room(animation_interface, true);
            SetLastError(error);
            return;
        }
        if (diagnostics_capture || research_capture_flag.load(std::memory_order_acquire)) {
            const auto shot_layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), *holder, 512);
            if (shot_layout.buffer && shot_layout.count == skeleton_joints) {
                auto& shot = render_shot();
                std::lock_guard shot_lock(shot.mutex);
                shot.skater.resize(shot_layout.count);
                if (peek(shot_layout.buffer, shot.skater.data(), shot.skater.size() * sizeof(NativeBone))) {
                    shot.at = now_seconds();
                    shot.skater_generation = generation;
                    ++shot.skater_publications;
                }
            }
        }
        if (!is_open()) {
            std::uint64_t presentation{};
            const auto packet = gameplay_packet(true, &presentation);
            apply_gameplay_body(*holder, packet, presentation);
            SetLastError(error);
            return;
        }
        std::uint64_t presentation{};
        const auto packet = preview_packet(presentation);
        if (!packet || packet->bones.empty()) {
            SetLastError(error);
            return;
        }
        const auto& p = *packet;
        const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), *holder, 512);
        if (layout.buffer != p.buffer || layout.count != skeleton_joints || p.last > layout.count || p.first >= p.last) {
            SetLastError(error);
            return;
        }
        thread_local std::vector<NativeBone> current; // the render thread's, kept between frames
        current.resize(p.last - p.first);
        if (!peek(layout.buffer + p.first * sizeof(NativeBone), current.data(), current.size() * sizeof(NativeBone))) throw std::runtime_error("unreadable");
        for (std::size_t i = 0; i < current.size(); ++i) {
            std::memcpy(current[i].scale.data(), p.bones[i].scale.data(), 12);
            current[i].rotation = p.bones[i].rotation;
            std::memcpy(current[i].position.data(), p.bones[i].position.data(), 12);
        }
        if (!poke(layout.buffer + p.first * sizeof(NativeBone), current.data(), current.size() * sizeof(NativeBone)))
            throw std::runtime_error("Cannot publish studio pose");
        if (diagnostics_capture) {
            auto& shot = render_shot(); std::lock_guard lock(shot.mutex);
            shot.edited_skater.clear();
            for (const auto& bone : shot.skater) shot.edited_skater.push_back(joint_of(bone));
            if (shot.edited_skater.size() == layout.count)
                for (std::size_t i = 0; i < current.size(); ++i) shot.edited_skater[p.first + i] = joint_of(current[i]);
            shot.edited_at = now_seconds(); shot.edited_generation = generation;
            shot.edited_sequence = packet->sequence; shot.edited_presentation = presentation;
        }
    } catch (...) { invalidate_outputs(); }
    SetLastError(error);
}

struct BoardPublicationState {
    std::mutex mutex;
    EditPublicationCache edits;
    EditTransition transition;
    BoardShape shape{}; // the last trick's board, for the fade after it
    bool shape_ok{};
    // The board as drawn on the last frame in the air, in the skater's placement frame: where the feet stood on it at a
    // rail's lock (slide_feet_on_the_games_board).
    Joint drawn_in_the_air{};
    double drawn_in_the_air_at{-1};
};
BoardPublicationState& board_publication_state() { static auto* state = new BoardPublicationState; return *state; }
// Each frame in the air, the board as drawn, in the skater's placement frame (his pose holds the placement unedited).
void remember_board_in_the_air(BoardPublicationState& board, const Joint& drawn) noexcept {
    auto& l = live();
    const auto component = l.component.load(std::memory_order_acquire);
    const auto body_holder = component ? read<std::uintptr_t>(component + 0xa0).value_or(0) : 0;
    if (!body_holder) return;
    const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(), body_holder, 512);
    std::array<NativeBone, 2> placement_bones{};
    if (!layout.buffer || layout.count != skeleton_joints || !peek(layout.buffer, placement_bones.data(), sizeof(placement_bones))) return;
    board.drawn_in_the_air = relative(compose(joint_of(placement_bones[0]), joint_of(placement_bones[1])), drawn);
    board.drawn_in_the_air_at = now_seconds();
}

// ---- the feet on the deck ------------------------------------------------------------------------------
// The game's board this update (its mount), with our own write of the last frame taken back off, as the board's pass does.
bool games_board_now(std::uintptr_t board_holder, Joint& out) noexcept {
    try {
        auto& l = live();
        if (!board_holder) return false;
        const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(), board_holder, 64);
        if (!layout.buffer || layout.count != board_bones) return false;
        std::array<NativeBone, board_bones> bones{};
        if (!peek(layout.buffer, bones.data(), sizeof(bones))) return false;
        std::vector<Joint> native;
        for (const auto& bone : bones) native.push_back(joint_of(bone));
        {
            auto& publication = board_publication_state();
            std::lock_guard lock(publication.mutex);
            publication.edits.restore(l.generation.load(std::memory_order_acquire), l.animation_updates.load(std::memory_order_acquire), layout.buffer, native);
        }
        out = compose(compose(native[0], native[1]), native[board_mount]);
        return finite(out);
    } catch (...) {
        return false;
    }
}
// The board as its own pass will draw it this frame (apply_gameplay_board), worked out before the body is handed over:
// the game's board this update, with the trick's board keys on it, as far as the trick is in.
bool drawn_board_now(const GameplayPacket& packet, std::span<const Joint> skater, Joint& out) noexcept {
    try {
        auto& l = live();
        if (!packet.board_holder || !packet.board_owned || !packet.board_frame_valid || !packet.clip || !packet.edits || skater.size() < 2) return false;
        Joint native_mount;
        if (!games_board_now(packet.board_holder, native_mount)) return false;
        // On a rail (and off the board) its pass leaves the game's board as it is.
        const auto motion = l.render_motion.load(std::memory_order_acquire);
        if (!overrides_allowed.load() || motion == MotionState::rail || motion == MotionState::unknown || motion == MotionState::offboard) {
            out = native_mount;
            return finite(out);
        }
        const auto placement = compose(skater[0], skater[1]);
        Joint frame;
        frame.rotation = normalised(multiply(placement.rotation, packet.board_frame_local));
        auto target = board_edited(native_mount, *packet.edits, packet.time, ground_turn(frame));
        if (motion == MotionState::riding) {
            const auto shape = board_shape(*packet.clip);
            (void)keep_board_above(target, shape, lowest_point(native_mount, shape));
        }
        const float w = std::clamp(packet.weight, 0.0f, 1.0f);
        out = native_mount;
        out.rotation = slerp(native_mount.rotation, target.rotation, w);
        for (std::size_t i = 0; i < 3; ++i) out.position[i] += (target.position[i] - native_mount.position[i]) * w;
        return finite(out);
    } catch (...) {
        return false;
    }
}
// The body pass, after the fade (feet_stay_on_the_games_board): on a rail, while a trick cut short fades out, each foot
// stays on the game's board, sliding from where it stood on the board drawn on the last frame in the air (rails are
// reached from the air) to where the game has it. True when a foot was moved.
bool slide_feet_on_the_games_board(BodyPublicationState& body, MotionState motion, bool playing, std::span<const Joint> native,
                                   std::vector<Joint>& edited) noexcept {
    try {
        constexpr std::array<std::uint16_t, 2> feet{left_foot_joint, right_foot_joint};
        if (edited.size() < skeleton_joints || native.size() < skeleton_joints) return false;
        const double now = now_seconds();
        if (motion != MotionState::rail) {
            body.on_rail = false;
            if (motion != MotionState::air) return false;
            const auto placement = compose(edited[0], edited[1]);
            for (std::size_t i = 0; i < feet.size(); ++i) body.feet_in_the_air[i] = relative(placement, world(edited, feet[i]));
            body.feet_in_the_air_at = now;
            return false;
        }
        if (!body.on_rail) {
            body.on_rail = true;
            Joint board_in_the_air;
            double board_at = -1;
            {
                auto& board = board_publication_state();
                std::lock_guard lock(board.mutex);
                board_in_the_air = board.drawn_in_the_air; board_at = board.drawn_in_the_air_at;
            }
            // Both from the frame before the lock (a frame is 1/60 s), not from an earlier air.
            body.feet_at_lock_ok = now - body.feet_in_the_air_at < 0.1 && now - board_at < 0.1;
            for (std::size_t i = 0; i < feet.size(); ++i) {
                body.feet_on_board_at_lock[i] = relative(board_in_the_air, body.feet_in_the_air[i]);
                body.feet_at_lock_ok = body.feet_at_lock_ok && finite(body.feet_on_board_at_lock[i]);
            }
            body.fade_left_at_lock = body.transition.from_left;
        }
        // A fade begun in the air (the trick let go before the lock) is ended by the same share from the lock on.
        const float fade_left = body.fade_left_at_lock > 0 ? body.transition.from_left / body.fade_left_at_lock : 0.0f;
        if (!feet_stay_on_the_games_board(motion, playing) || !body.feet_at_lock_ok || !(fade_left > 0)) return false;
        Joint board;
        if (!games_board_now(live().board_holder.load(std::memory_order_acquire), board)) return false;
        bool moved = false;
        for (std::size_t i = 0; i < feet.size(); ++i) {
            const auto games = relative(board, world(native, feet[i]));
            if (!finite(games)) continue;
            const auto want = compose(board, foot_sliding_to_the_games(body.feet_on_board_at_lock[i], games, fade_left));
            place_foot(edited, feet[i], want.position, &want.rotation, 1.0f);
            moved = true;
        }
        return moved;
    } catch (...) {
        return false;
    }
}
// After the keys are on: a foot that is on the deck in the studio goes where the studio has it on the board, as far as the
// trick is off the game's clip (feet_match) - the game's board and the keyed legs had come apart there; and a foot is never
// drawn sunk into an upright deck, whatever the keys and the game did. The board is the one drawn this frame.
void seat_feet_on_board(const GameplayPacket& packet, std::span<const Joint> native, std::vector<Joint>& edited) noexcept {
    try {
        if (!(packet.weight > 0) || edited.size() < skeleton_joints || native.size() < skeleton_joints) return;
        Joint board;
        if (!drawn_board_now(packet, native, board)) return;
        // The board's own axes, from the clip (board_shape): up, along (tail to nose) and across.
        const auto shape = board_shape(*packet.clip);
        const Vec3 up = unit(shape.up);
        const Vec3 along = unit(Vec3{shape.points[0][0] - shape.points[1][0], shape.points[0][1] - shape.points[1][1], shape.points[0][2] - shape.points[1][2]});
        const Vec3 across = unit(cross(up, along));
        const auto on_deck = [&](const Vec3& p, float& height) {
            height = p[0] * up[0] + p[1] * up[1] + p[2] * up[2];
            const float a = p[0] * along[0] + p[1] * along[1] + p[2] * along[2];
            const float w = p[0] * across[0] + p[1] * across[1] + p[2] * across[2];
            return std::abs(a) < 0.45f && std::abs(w) < 0.18f;
        };
        constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 2> legs{{{left_foot_joint, 341}, {right_foot_joint, 8}}};
        const float w = std::min(packet.weight, 1.0f);
        // 1. Off the game's clip: the studio's foot on the studio's board - the recording's body and board, the keys on
        // both, under the live placement (which drops out of the relation) - put on the drawn board.
        if (packet.feet_match > 0 && packet.clip->joints == skeleton_joints && !packet.clip->board.empty()) {
            Joint rel;
            if (packet.clip->sample_board(std::clamp(packet.time, 0.0f, packet.clip->duration()), rel)) {
                rel = nose_forward(*packet.clip, rel);
                if (packet.mirrored) rel = mirror_board_pose(rel);
                if (packet.board_backwards) rel.rotation = normalised(multiply(rel.rotation, Quat{0, 1, 0, 0}));
                const auto placement = compose(native[0], native[1]);
                Joint frame;
                frame.rotation = normalised(multiply(placement.rotation, packet.board_frame_local));
                const auto studio_board = board_edited(compose(placement, rel), *packet.edits, packet.time, ground_turn(frame));
                const auto studio = studio_pose(*packet.clip, packet.time, packet.mirrored);
                std::vector<Joint> body(native.begin(), native.end());
                for (std::uint16_t j = 0; j < skeleton_joints; ++j)
                    if (gameplay_body_joint(j) && editable(j)) body[j] = studio[j];
                apply_edits(*packet.edits, packet.time, body);
                if (packet.limits) limit_keyed_joints(*packet.edits, packet.time, body);
                for (const auto& [foot, thigh] : legs) {
                    const auto on_board = relative(studio_board, world(body, foot));
                    float height = 0;
                    if (!finite(on_board) || !on_deck(on_board.position, height) || height < 0.02f) continue;
                    // On the deck up to 13 cm (the ankle stands 10.4 cm over the mount), let go by 20.
                    const float x = std::clamp((0.20f - height) / 0.07f, 0.0f, 1.0f);
                    const auto want = compose(board, on_board);
                    // Only a foot already near it is put on the deck: a foot the trick has far from the drawn board (an
                    // overgap's longer air, the board still falling or turning) was pulled the whole way there and stuck to
                    // it after the catch, as if magnetic (22sh, 2026-10-25). Full up to 12 cm away, none from 30.
                    const auto now_at = world(edited, foot).position;
                    const float gap = std::sqrt((want.position[0] - now_at[0]) * (want.position[0] - now_at[0]) +
                                                (want.position[1] - now_at[1]) * (want.position[1] - now_at[1]) +
                                                (want.position[2] - now_at[2]) * (want.position[2] - now_at[2]));
                    const float near_x = std::clamp((0.30f - gap) / 0.18f, 0.0f, 1.0f);
                    const float share = packet.feet_match * x * x * (3 - 2 * x) * near_x * near_x * (3 - 2 * near_x) * w *
                                        std::clamp(packet.limbs.of(thigh), 0.0f, 1.0f);
                    if (share <= 0) continue;
                    place_foot(edited, foot, want.position, &want.rotation, share);
                }
            }
        }
        // 2. Never sunk into the deck, nor into the ground while it rolls on it (lowest_ankle).
        const auto placement = compose(edited[0], edited[1]);
        const Vec3 board_up = rotate(board.rotation, up), body_up = rotate(placement.rotation, Vec3{0, 1, 0});
        if (board_up[0] * body_up[0] + board_up[1] * body_up[1] + board_up[2] * body_up[2] < 0.5f) return;
        const bool rolling = live().render_motion.load(std::memory_order_acquire) == MotionState::riding;
        if (!rolling && !air_corrections) return; // in the air the feet are the game's and the keys' (air_corrections)
        for (const auto& [foot, thigh] : legs) {
            const auto now = world(edited, foot);
            const auto on_board = relative(board, now);
            float height = 0;
            if (!finite(on_board)) continue;
            const float lowest = lowest_ankle(rolling, on_deck(on_board.position, height), height);
            float lift = height < lowest ? lowest - height : 0.0f;
            // The toes too (the joint after the foot): a foot pitched at a tilted deck had its ankle over it and its toes
            // through it. Over the deck they stay on top of it.
            const std::uint16_t toe_joint = foot == left_foot_joint ? 344 : 11; // Left toes, Right toes
            const auto toes = relative(board, world(edited, toe_joint));
            float toe_height = 0;
            if (finite(toes) && on_deck(toes.position, toe_height) && toe_height < 0.01f && toe_height >= -0.08f) lift = std::max(lift, 0.01f - toe_height);
            if (!(lift > 0)) continue;
            const Vec3 to{now.position[0] + board_up[0] * lift, now.position[1] + board_up[1] * lift, now.position[2] + board_up[2] * lift};
            place_foot(edited, foot, to, nullptr, w * std::clamp(packet.limbs.of(thigh), 0.0f, 1.0f));
        }
    } catch (...) {}
}
void apply_gameplay_board(std::uintptr_t holder, const std::shared_ptr<const GameplayPacket>& packet, std::uint64_t presentation) {
    auto& l = live();
    auto& publication = board_publication_state(); std::lock_guard lock(publication.mutex);
    // An animation update landing while this ran left a newer pose under it: it is read and edited again
    // (once) rather than left unedited for a frame, which drew the game's plain pose in the middle of a trick.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto generation = l.generation.load(std::memory_order_acquire), update = l.animation_updates.load(std::memory_order_acquire);
        const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(), holder, 64);
        if (!layout.buffer || layout.count != board_bones) return;
        std::array<NativeBone, board_bones> bones{};
        if (!peek(layout.buffer, bones.data(), sizeof(bones))) return;
        std::vector<Joint> native; for (const auto& bone : bones) native.push_back(joint_of(bone));
        publication.edits.restore(generation, update, layout.buffer, native);
        auto edited = native;
        const auto native_mount = compose(compose(native[0], native[1]), native[board_mount]);
        Joint want = native_mount;
        Joint studio_board;
        bool studio_board_ok = false;
        if (packet && packet->board_holder == holder && packet->board_owned && packet->board_frame_valid) {
            const auto body_holder = read<std::uintptr_t>(packet->component + 0xa0).value_or(0);
            const auto body_layout = multiplayer::read_native_pose_layout(peek, l.base.load(), body_holder, 512);
            std::array<NativeBone, 2> placement_bones{};
            if (body_layout.buffer && body_layout.count == skeleton_joints && peek(body_layout.buffer, placement_bones.data(), sizeof(placement_bones))) {
                const auto placement = compose(joint_of(placement_bones[0]), joint_of(placement_bones[1]));
                Joint frame; frame.rotation = normalised(multiply(placement.rotation, packet->board_frame_local));
                // Whole body: the board is the recording's too - nose-forward as the game draws it, mirrored with him
                // when he is - so his feet and the deck flip together as they do in the studio. Then the board keys.
                Joint base = native_mount;
                if (packet->match_rest > 0 && packet->clip && !packet->clip->board.empty()) {
                    Joint rel;
                    if (packet->clip->sample_board(std::clamp(packet->time, 0.0f, packet->clip->duration()), rel)) {
                        rel = nose_forward(*packet->clip, rel);
                        if (packet->mirrored) rel = mirror_board_pose(rel);
                        // With the deck turned round under him the game turns its trick board too: the recording's
                        // board is turned the same way, so the real deck keeps its nose where it is and nothing spins
                        // when the trick hands back to the game at the landing.
                        if (packet->board_backwards) rel.rotation = normalised(multiply(rel.rotation, Quat{0, 1, 0, 0}));
                        const auto recorded = compose(placement, rel);
                        if (finite(recorded)) {
                            const float k = std::min(packet->match_rest, 1.0f);
                            base.rotation = slerp(native_mount.rotation, recorded.rotation, k);
                            for (std::size_t i = 0; i < 3; ++i) base.position[i] += (recorded.position[i] - native_mount.position[i]) * k;
                        }
                    }
                }
                auto target = board_edited(base, *packet->edits, packet->time, ground_turn(frame));
                // Rolling on the ground the game's board stands on it, so the edited one goes no lower than it does.
                if (packet->clip) {
                    publication.shape = board_shape(*packet->clip);
                    publication.shape_ok = true;
                    if (l.render_motion.load(std::memory_order_acquire) == MotionState::riding)
                        (void)keep_board_above(target, publication.shape, lowest_point(native_mount, publication.shape));
                }
                studio_board = target;
                studio_board_ok = packet->weight >= 0.98f;
                if (target != native_mount) {
                    want.rotation = slerp(native_mount.rotation, target.rotation, packet->weight);
                    for (std::size_t i = 0; i < 3; ++i) want.position[i] += (target.position[i] - native_mount.position[i]) * packet->weight;
                }
            }
        }
        std::array<Offset, 1> desired{};
        if (want.rotation != native_mount.rotation) desired[0].rotation = normalised(multiply(want.rotation, conjugate(native_mount.rotation)));
        for (std::size_t k = 0; k < 3; ++k) desired[0].offset[k] = want.position[k] - native_mount.position[k];
        want = native_mount;
        const auto motion = l.render_motion.load(std::memory_order_acquire);
        if (overrides_allowed.load() && motion != MotionState::unknown && motion != MotionState::offboard && motion != MotionState::rail) {
            const auto delta = publication.transition.evaluate(now_seconds(), generation, packet ? packet->episode : 0,
                packet ? reinterpret_cast<std::uintptr_t>(packet->edits.get()) : 0, desired, trick_blend.load(std::memory_order_relaxed))[0];
            if (delta.rotation != Quat{0, 0, 0, 1}) want.rotation = normalised(multiply(delta.rotation, native_mount.rotation));
            for (std::size_t k = 0; k < 3; ++k) want.position[k] += delta.offset[k];
        } else publication.transition.reset();
        // The fade after a trick (or into the next one), rolling: no lower than the game's board either.
        if (motion == MotionState::riding && publication.shape_ok && want != native_mount)
            (void)keep_board_above(want, publication.shape, lowest_point(native_mount, publication.shape));
        if (motion == MotionState::air) remember_board_in_the_air(publication, want);
        if (want != native_mount) {
            edited[1] = relative(native[0], compose(want, relative(native[board_mount], Joint{})));
            edited[1].scale = native[1].scale;
        }
        if (generation != l.generation.load(std::memory_order_acquire) || update != l.animation_updates.load(std::memory_order_acquire) ||
            (packet && packet->epoch != gameplay_epoch.load(std::memory_order_acquire))) continue;
        if (!finite(edited[1])) throw std::runtime_error("Native board edit produced an invalid transform");
        if (edited[1] != joint_of(bones[1])) {
            auto& root = bones[1]; root.rotation = edited[1].rotation;
            std::memcpy(root.position.data(), edited[1].position.data(), 12);
            if (!poke(layout.buffer + sizeof(NativeBone), &root, sizeof(root))) throw std::runtime_error("Cannot publish board edits");
        }
        if (diagnostics_capture) {
            auto& shot = render_shot(); std::lock_guard shot_lock(shot.mutex);
            shot.edited_board = compose(compose(edited[0], edited[1]), edited[board_mount]);
            shot.edited_board_at = now_seconds(); shot.edited_board_generation = generation;
            shot.edited_board_sequence = packet ? packet->sequence : 0; shot.edited_board_presentation = presentation;
            shot.board = native_mount;
        }
        // Verification: the body as it will be drawn, read back now, and the board as written.
        if (studio_board_ok && playback_verify::on() && packet) {
            const auto body_holder = read<std::uintptr_t>(packet->component + 0xa0).value_or(0);
            const auto body_layout = multiplayer::read_native_pose_layout(peek, l.base.load(), body_holder, 512);
            std::vector<NativeBone> drawn_bones(skeleton_joints);
            if (body_layout.buffer && body_layout.count == skeleton_joints &&
                peek(body_layout.buffer, drawn_bones.data(), drawn_bones.size() * sizeof(NativeBone))) {
                std::vector<Joint> drawn;
                drawn.reserve(drawn_bones.size());
                for (const auto& bone : drawn_bones) drawn.push_back(joint_of(bone));
                playback_verify::compare(presentation, now_seconds(), drawn, compose(compose(edited[0], edited[1]), edited[board_mount]), studio_board);
            }
        }
        publication.edits.commit(generation, update, layout.buffer, std::move(native), std::move(edited));
        return;
    }
}

// The board's pose, just before it is handed to the renderer: its mount where the studio or the
// custom trick wants it.
void on_board_render(std::uintptr_t animation_interface, std::uintptr_t /*render_data*/) noexcept {
    auto& l = live();
    const auto generation = l.generation.load(std::memory_order_acquire);
    const auto error = GetLastError();
    try {
        // The board: its mount (bone 2) put where the studio or the custom trick wants it.
        const auto bh = l.board_holder.load(std::memory_order_acquire);
        if (bh && animation_interface != bh + 0xc0) hide_in_room(animation_interface, false);
        if (bh && animation_interface == bh + 0xc0 && (diagnostics_capture || research_capture_flag.load(std::memory_order_acquire))) {
            const auto shot_layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), bh, 64);
            std::array<NativeBone, board_bones> b{};
            if (shot_layout.buffer && shot_layout.count == board_bones && peek(shot_layout.buffer, b.data(), sizeof(b))) {
                const auto mount = compose(board_parent(b), joint_of(b[board_mount]));
                if (finite(mount)) {
                    auto& shot = render_shot();
                    std::lock_guard shot_lock(shot.mutex);
                    shot.board = mount;
                    shot.board_at = now_seconds();
                    shot.board_generation = generation;
                    shot.board_rig.clear();
                    for (const auto& bone : b) shot.board_rig.push_back(joint_of(bone));
                    ++shot.board_publications;
                }
            }
        }
        if (bh && animation_interface == bh + 0xc0) {
            if (!is_open()) {
                std::uint64_t presentation{};
                const auto packet = gameplay_packet(true, &presentation);
                apply_gameplay_board(bh, packet, presentation);
                SetLastError(error);
                return;
            }
            Joint target;
            float weight{};
            std::uint64_t presentation{};
            const auto packet = preview_packet(presentation);
            if (packet && packet->holder == bh) {
                target = packet->mount;
                weight = packet->weight;
            }
            if (weight > 0) {
                const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(std::memory_order_acquire), bh, 64);
                std::array<NativeBone, board_bones> b{};
                if (layout.buffer && layout.count == board_bones && peek(layout.buffer, b.data(), sizeof(b))) {
                    const auto parent_world = board_parent(b);
                    const auto now_mount = compose(parent_world, joint_of(b[board_mount]));
                    Joint want = target;
                    want.rotation = slerp(now_mount.rotation, target.rotation, weight);
                    for (std::size_t i = 0; i < 3; ++i) want.position[i] = now_mount.position[i] + (target.position[i] - now_mount.position[i]) * weight;
                    want.scale = now_mount.scale;
                    // The whole rig moves: its place in the world (bone 1) is set so the mount lands where wanted,
                    // so everything on the board goes with it (a pack deck's pieces hang on bone 1's other
                    // children, not on the mount: moving the mount alone left them lying on the ground).
                    const auto mount_local = joint_of(b[board_mount]);
                    const auto root_world = compose(want, relative(mount_local, Joint{}));
                    const auto root_local = relative(joint_of(b[0]), root_world);
                    if (finite(root_local)) {
                        auto& r = b[1];
                        r.rotation = root_local.rotation;
                        std::memcpy(r.position.data(), root_local.position.data(), 12);
                        if (!poke(layout.buffer + sizeof(NativeBone), &r, sizeof(NativeBone)))
                            throw std::runtime_error("Cannot publish studio board");
                        // The trucks at rest on the deck, not steered by the game's own board.
                        hold_trucks_at_rest(b);
                        for (const std::size_t bone : {std::size_t{4}, std::size_t{5}, std::size_t{8}, std::size_t{9}})
                            if (!poke(layout.buffer + bone * sizeof(NativeBone), &b[bone], sizeof(NativeBone)))
                                throw std::runtime_error("Cannot publish studio trucks");
                        if (diagnostics_capture) {
                            auto& shot = render_shot(); std::lock_guard lock(shot.mutex);
                            shot.edited_board = compose(board_parent(b), joint_of(b[board_mount]));
                            shot.edited_board_at = now_seconds(); shot.edited_board_generation = generation;
                            shot.edited_board_sequence = packet->sequence; shot.edited_board_presentation = presentation;
                        }
                        if (studio_snapshot::on())
                            studio_snapshot::late_read(l.base.load(std::memory_order_acquire), l.component.load(std::memory_order_acquire),
                                                       compose(board_parent(b), joint_of(b[board_mount])));
                    }
                }
            }
        }
    } catch (...) { invalidate_outputs(); }
    SetLastError(error);
}

// ---- the trick loader ----------------------------------------------------------------------------
// Reads the saved tricks (read_custom_tricks) on a thread of its own, below the game's: one read at a time, and once more
// when asked again while it read. What it read goes in when it is done; until then what played before plays on.
namespace {
struct TrickLoader {
    std::mutex mutex;
    bool running{}, again{};
};
TrickLoader& trick_loader() {
    static auto* loader = new TrickLoader; // process lifetime
    return *loader;
}
void run_trick_loader() {
    for (;;) {
        auto& s = state();
        std::string test;
        {
            std::lock_guard lock(s.mutex);
            test = s.test_trick;
        }
        LoadedTricks loaded;
        try {
            loaded = read_custom_tricks(test);
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: the saved tricks could not be read: {}", e.what());
        } catch (...) {}
        if (loaded.tricks) {
            std::lock_guard lock(s.mutex);
            playlist_turns_mode.store(static_cast<int>(loaded.mode), std::memory_order_release); // with the set it is for
            s.tricks = std::move(loaded.tricks);
            publish_locked(s);
        }
        auto& l = trick_loader();
        std::lock_guard lock(l.mutex);
        if (!l.again) {
            l.running = false;
            return;
        }
        l.again = false;
    }
}
} // namespace
void load_custom_tricks() {
    auto& l = trick_loader();
    std::lock_guard lock(l.mutex);
    if (l.running) {
        l.again = true;
        return;
    }
    l.running = true;
    l.again = false;
    try {
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            run_trick_loader();
        }).detach();
    } catch (...) {
        l.running = false;
    }
}

// ---- the engine thread ---------------------------------------------------------------------------
std::shared_ptr<Clip> read_game_clip(const GameTrick& trick); // a clip of the trick library, below
TickResult tick(std::uintptr_t base, std::uintptr_t component, bool open, float camera_fov) {
    read_game_stance();
    research_tick(base, component, open);
    auto& l = live();
    auto& s = state();
    TickResult result;
    bool needs_listener{};
    {
        std::lock_guard lock(s.mutex);
        if (!s.snapshot) publish_locked(s);
        open_flag.store(open, std::memory_order_release);
        const bool room_now = open && in_room_flag.load(std::memory_order_acquire);
        if (room_flag.exchange(room_now, std::memory_order_acq_rel) && !room_now)
            room_restore_until.store(now_seconds() + 3, std::memory_order_release);
        if (s.open != open) {
            if (open && !s.test_trick.empty()) {
                s.test_returned = std::exchange(s.test_trick, {});
                s.tricks_read = false;
            }
            s.open = open;
            s.base_yaw_valid = false;
            s.eased_valid = false;
            s.playing = false;
            publish_locked(s);
        }
        if (std::isfinite(camera_fov) && camera_fov > 1 && camera_fov < 175) s.fov = camera_fov;
        needs_listener = diagnostics_capture || research_capture_flag.load(std::memory_order_acquire) || s.open || room_watch.load() || (s.play_in_game && s.tricks && !s.tricks->empty());
        result.want_open = s.open_requested;
        result.changed = s.open_request_pending;
        s.open_request_pending = false;
    }
    // When the board lands, for the catch latch: the ground along its path (time_to_landing), on this (the client) thread.
    if (auto& probe = ground_probe(); probe.wanted.exchange(false, std::memory_order_acq_rel)) {
        const float x = probe.x.load(std::memory_order_acquire), z = probe.z.load(std::memory_order_acquire);
        const float t = time_to_landing(probe.low.load(std::memory_order_acquire), probe.vx.load(std::memory_order_acquire),
                                        probe.vy.load(std::memory_order_acquire), probe.vz.load(std::memory_order_acquire),
                                        [&](float dx, float dz, float top) { return local_ground_height(x + dx, z + dz, top, top - 60.0f); });
        probe.to_ground.store(t, std::memory_order_release);
        probe.found_at.store(now_seconds(), std::memory_order_release);
    }
    std::uintptr_t next_board_holder{};
    if (component && base) {
        // The skater's board (its own entity, with a 16-bone rig): its animation holder.
        std::uintptr_t holder{};
        try {
            const auto collection = read<std::uintptr_t>(component + 0x18).value_or(0);
            const auto entity = collection ? read<std::uintptr_t>(collection).value_or(0) : 0;
            if (entity) holder = multiplayer::read_native_board(peek, base, entity).holder;
        } catch (...) {}
        next_board_holder = holder;
    }
    {
        std::lock_guard animation_lock(l.animation_mutex);
        if (component != l.component.load(std::memory_order_acquire) || base != l.base.load(std::memory_order_acquire) ||
            next_board_holder != l.board_holder.load(std::memory_order_acquire))
            l.generation.fetch_add(1, std::memory_order_acq_rel);
        l.base.store(base, std::memory_order_release);
        l.component.store(component, std::memory_order_release);
        l.board_holder.store(next_board_holder, std::memory_order_release);
    }
    {
        // The saved tricks, read again when what plays changed: on the loader's thread (the first time as ReSkate starts),
        // never on the game's.
        bool read{};
        {
            std::lock_guard lock(s.mutex);
            read = !s.tricks_read;
            s.tricks_read = true;
        }
        if (read) load_custom_tricks();
    }
    // The casual stance, read once the studio opens.
    if (open) {
        bool read_idle{};
        {
            std::lock_guard lock(s.mutex);
            read_idle = !s.idle_read;
            s.idle_read = true;
        }
        if (read_idle) {
            try {
                const auto tricks = game_tricks();
                const auto& list = *tricks;
                // The game's own ollie.
                auto pick = std::find_if(list.begin(), list.end(), [](const GameTrick& t) { return t.trick == "ollie" && t.pop == "mid"; });
                if (pick != list.end() || !list.empty()) {
                    auto idle = read_game_clip(pick != list.end() ? *pick : list.front());
                    std::lock_guard lock(s.mutex);
                    s.idle_clip = std::move(idle);
                    publish_locked(s);
                }
            } catch (const std::exception& e) {
                logging::log(Level::warning, Channel::skater, "Animation Studio: no casual stance: {}", e.what());
            }
        }
    }
    static bool installed = false;
    if (needs_listener && !installed && base) {
        std::string detail;
        if (multiplayer::install_entity_hooks(base, detail)) {
            multiplayer::set_animation_studio_listener(&on_animation);
            multiplayer::set_animation_studio_render_listener(&on_render);
            multiplayer::set_animation_studio_board_listener(&on_board_render);
            multiplayer::set_animation_studio_pose_filter(&on_send);
            multiplayer::set_local_board_filter(&on_send_board);
            installed = true;
            logging::log(Level::info, Channel::skater, "Animation Studio: listening to the skater's animation.");
        } else {
            std::lock_guard lock(s.mutex);
            s.failure = "The animation hook is unavailable: " + detail;
        }
    }
    // The game's grade of each pop chooses the clip in play: listened to from the first time the studio listens.
    static bool grading_tried = false, grading = false;
    if (needs_listener && !grading_tried && base) {
        grading_tried = true;
        std::string why;
        grading = pop_grades::install(base, why);
        if (!grading) logging::log(Level::warning, Channel::skater, "Animation Studio: pops are chosen by their air time, not the game's grade: {}.", why);
    }
    grades_heard.store(grading, std::memory_order_release);
    grades_solo.store(!multiplayer_session_active() && ai_skaters::active_count() == 0, std::memory_order_release);
    return result;
}
void report(std::string message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.status = std::move(message);
    ++s.revision;
}
bool camera(std::array<float, 16>& matrix) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.open || !s.centre_valid) return false;
    Vec3 target{};
    if (s.focus) {
        for (const auto& j : s.joints)
            if (j.joint == *s.focus)
                for (std::size_t i = 0; i < 3; ++i) target[i] = j.position[i] - s.centre[i];
    }
    // Eases to the focused joint rather than jumping there.
    for (std::size_t i = 0; i < 3; ++i) s.focus_offset[i] += (target[i] - s.focus_offset[i]) * 0.15f;
    if (!s.eased_valid || std::abs(s.eased_centre[0] - s.centre[0]) + std::abs(s.eased_centre[2] - s.centre[2]) > 5) {
        s.eased_centre = s.centre;
        s.eased_valid = true;
    }
    for (std::size_t i = 0; i < 3; ++i) s.eased_centre[i] += (s.centre[i] - s.eased_centre[i]) * 0.12f;
    Vec3 centre{s.eased_centre[0] + s.focus_offset[0], s.eased_centre[1] + s.focus_offset[1], s.eased_centre[2] + s.focus_offset[2]};
    // The free camera is not carried along: it holds the spot it was left at and the skater moves through
    // it, so a trick can be watched going past rather than the world sliding under a camera stuck to him.
    if (!s.camera_follows) {
        // Taken again when he has gone a long way off - into the studio's room, or a level loaded - so the
        // camera is never left behind somewhere he no longer is.
        const float away = std::abs(centre[0] - s.held_centre[0]) + std::abs(centre[1] - s.held_centre[1]) + std::abs(centre[2] - s.held_centre[2]);
        if (!s.held_centre_valid || away > 5) {
            s.held_centre = centre;
            s.held_centre_valid = true;
        }
        centre = s.held_centre;
    } else s.held_centre_valid = false;
    if (!s.base_yaw_valid) {
        s.base_yaw = facing_yaw(s.facing);
        s.base_yaw_valid = true;
    }
    // No camera goes under the ground, whichever shot it is and however it has been orbited by hand: the
    // eye is lifted by raising how far above it looks down from, so it still holds what it is aimed at.
    Orbit orbit = s.orbit;
    if (s.floor_valid) {
        constexpr float above_floor = 0.30f; // enough that the near plane does not cut into the ground
        const float distance = std::clamp(orbit.distance, 0.6f, 25.0f);
        const float lowest = (s.floor + above_floor - centre[1]) / distance;
        if (lowest > std::sin(orbit.pitch)) orbit.pitch = std::asin(std::clamp(lowest, -1.0f, 1.0f));
    }
    auto m = orbit_matrix(centre, orbit, s.base_yaw);
    if (s.fov > 1 && s.fov < 175 && (s.view_shift != 0 || s.view_shift_x != 0)) {
        // Aim below and beside the centre so it shows that far above and right of the screen's middle.
        const float per_share = std::clamp(orbit.distance, 0.6f, 25.0f) * 2 * std::tan(s.fov * 3.14159265f / 360);
        const float up = s.view_shift * per_share, side = s.view_shift_x * per_share;
        const Vec3 aimed{centre[0] - m[4] * up - m[0] * side, centre[1] - m[5] * up - m[1] * side, centre[2] - m[6] * up - m[2] * side};
        m = orbit_matrix(aimed, orbit, s.base_yaw);
    }
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) matrix[i] = m[i];
    s.camera = matrix;
    s.camera_valid = true;
    return true;
}

// ---- the overlay -----------------------------------------------------------------------------------
bool is_open() noexcept { return open_flag.load(std::memory_order_acquire); }
std::atomic<float> fov_wanted{0};
void set_wanted_fov(float degrees) noexcept { fov_wanted.store(std::isfinite(degrees) && degrees > 10 && degrees < 150 ? degrees : 0.0f); }
float wanted_fov() noexcept { return is_open() ? fov_wanted.load() : 0.0f; }
bool room_wanted() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.room;
}
void watch_room(bool on) noexcept { room_watch.store(on, std::memory_order_release); }
void set_in_room(bool on) noexcept { in_room_flag.store(on, std::memory_order_release); }
bool room_skater_seen() noexcept { return now_seconds() - room_seen_at.load(std::memory_order_acquire) < 0.5; }
// The game's own load is still going: the loading screen stays until the game has been quiet for a second.
std::atomic<bool> loading_held{};
std::atomic<double> game_quiet_since{-1}; // -1: taken to be loading until a tick says otherwise
void game_loading_state(bool game_loading) noexcept {
    if (game_loading) game_quiet_since.store(-1, std::memory_order_release);
    else if (game_quiet_since.load() < 0) game_quiet_since.store(now_seconds(), std::memory_order_release);
}
void set_loading(Loading what, float seconds) noexcept {
    const double now = now_seconds();
    if (what != Loading::none) {
        loading_held.store(true);
        // Taken to be loading from here: the first tick that finds the game quiet starts the countdown, so the
        // screen is never lifted before the client tick has even seen the load begin.
        game_quiet_since.store(-1, std::memory_order_release);
        // A screen that is still showing stays up (no fade in again).
        if (loading_what.exchange(what, std::memory_order_acq_rel) == Loading::none || now > loading_until.load()) loading_since.store(now);
        loading_until.store(now + seconds);
    } else loading_until.store(std::min(loading_until.load(), now));
}
LoadingView loading() noexcept {
    constexpr double fade = 0.35;
    constexpr double longest = 60.0; // never held longer than this, whatever the game is doing
    const double now = now_seconds(), since = loading_since.load();
    // Held while the game loads: the screen stays up (its own time is pushed along) until the game has been
    // quiet for a moment, so the level, its title card and its streaming are all done behind it.
    if (loading_held.load() && loading_what.load() != Loading::none) {
        const double quiet = game_quiet_since.load();
        if ((quiet >= 0 && now - quiet > 1.5) || now - since > longest) {
            loading_held.store(false);
            loading_until.store(now);
        } else if (loading_until.load() < now + 0.2) {
            loading_until.store(now + 0.2);
        }
    }
    const double until = loading_until.load();
    LoadingView v;
    v.what = loading_what.load(std::memory_order_acquire);
    if (v.what == Loading::none || now > until + fade) return {};
    const double in = std::clamp((now - since) / fade, 0.0, 1.0), out = now < until ? 1.0 : 1.0 - (now - until) / fade;
    v.alpha = static_cast<float>(std::min(in, out));
    v.seconds = static_cast<float>(now - since);
    return v;
}
namespace {
// What the studio's catch is worked out from: the clip and the keys (and the brackets, outside which they do nothing).
std::uint64_t catch_inputs(const State& s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    };
    const void* clip = s.clip.get();
    mix(&clip, sizeof clip);
    mix(&s.clip_serial, sizeof s.clip_serial);
    mix(&s.edits.start, sizeof s.edits.start);
    mix(&s.edits.end, sizeof s.edits.end);
    for (const auto& track : s.edits.tracks) {
        mix(&track.joint, sizeof track.joint);
        mix(&track.hidden, sizeof track.hidden);
        for (const auto& key : track.keys) {
            mix(&key.time, sizeof key.time);
            mix(key.rotation.data(), sizeof key.rotation);
            mix(key.offset.data(), sizeof key.offset);
            mix(&key.ease, sizeof key.ease);
            mix(&key.smooth, sizeof key.smooth);
            mix(&key.blend_in, sizeof key.blend_in); // a key's blends change the pose the catch is found on too
            mix(&key.blend_out, sizeof key.blend_out);
        }
    }
    return h;
}
float catch_guess_locked(State& s) {
    if (!s.clip || s.clip->empty() || !has_air(*s.clip)) return -1;
    const auto from = catch_inputs(s);
    const auto now = std::chrono::steady_clock::now();
    if (from != s.catch_guess_from && (s.catch_guess_from == 0 || now - s.catch_guessed_at > std::chrono::milliseconds(200))) {
        s.catch_guess = auto_catch(*s.clip, s.edits);
        s.catch_guess_from = from;
        s.catch_guessed_at = now;
    }
    return s.catch_guess;
}
// The open trick off its DROP PREVIEW: off (on() false) without a drop, air or catch.
DropPlay drop_play_locked(State& s) {
    DropPlay p;
    if (!(s.drop_preview > 0.01f) || !s.clip || s.clip->empty() || !has_air(*s.clip)) return p;
    const float caught = s.edits.catch_at && catch_fits(*s.clip, *s.edits.catch_at) ? *s.edits.catch_at : catch_guess_locked(s);
    if (!(caught > 0)) return p;
    p.metres = s.drop_preview;
    p.takeoff = s.clip->takeoff;
    p.caught = caught;
    p.land = s.clip->landing;
    p.duration = s.clip->duration();
    p.hold = s.edits.catch_hold.value_or(default_catch_hold);
    return p;
}
} // namespace
View view() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    // The Thunderstore team checked (or unlinked): whose each pack is is worked out again.
    static unsigned account_seen = 0;
    if (const auto now = market::account_generation(); now != account_seen) {
        account_seen = now;
        s.library_read = false;
    }
    if (!s.library_read) read_library_locked(s);
    if (!s.pack_pictures_read) {
        s.pack_pictures_read = true;
        s.pack_pictures.clear();
        for (const auto& saved : s.packs) {
            std::error_code error;
            const auto picture = pack_thumbnail_file(saved.name);
            if (!std::filesystem::is_regular_file(picture, error)) continue;
            const auto at = std::filesystem::last_write_time(picture, error);
            if (!error) s.pack_pictures[saved.name] = static_cast<std::int64_t>(at.time_since_epoch().count());
        }
    }
    View v;
    v.open = s.open;
    v.board_base = s.board_base;
    v.board_base_valid = s.board_base_valid && s.open;
    v.pose_seen = s.pose_seen_at > 0 && now_seconds() - s.pose_seen_at < 1.0;
    v.status = s.status;
    v.failure = s.failure;
    v.camera = s.camera;
    v.fov = s.fov;
    v.base_yaw = s.base_yaw_valid ? s.base_yaw : 0.0f;
    v.camera_valid = s.camera_valid;
    v.name = s.edits.name;
    v.trick = s.edits.trick;
    v.author = s.edits.author;
    v.source_kind = s.edits.source.kind; v.entry = s.edits.entry;
    v.source_position_codec = s.edits.source.position_codec;
    if (s.clip) {
        v.source_pose_count = s.clip->times.size();
        for (std::size_t i = 1; i < s.clip->times.size(); ++i)
            v.source_max_sample_gap = std::max(v.source_max_sample_gap, s.clip->times[i] - s.clip->times[i-1]);
    }
    v.has_clip = s.clip && !s.clip->empty();
    v.dirty = s.dirty;
    v.previewing = s.previewing;
    v.kept_dirty = s.previewing && s.kept && s.kept->dirty;
    if (s.kept) v.kept_name = s.kept->edits.name.empty() ? s.kept->edits.trick : s.kept->edits.name;
    v.playing = s.playing;
    v.loop = s.edits.loop;
    v.play_in_game = s.play_in_game;
    v.room = s.room;
    v.solo = s.solo;
    v.stance_choice = s.stance_choice;
    v.own_lead_right = studio_goofy(s.stance_choice);
    // The open trick's stance, as it is saved: the foot it keeps (or its clip leads with) against the skater's own, and fakie.
    if (s.clip && !s.clip->empty() && !s.edits.trick.starts_with("native:"))
        if (const auto lead = s.edits.keep_leads_right ? s.edits.keep_leads_right : clip_leads_right(*s.clip))
            v.trick_stance = (*lead != v.own_lead_right ? 2 : 0) + (s.edits.fakie.value_or(false) ? 1 : 0);
    v.showing_goofy = s.showing_goofy;
    v.grab_board = s.grab_board;
    v.front_foot = s.front_foot;
    v.limits = s.limits;
    v.crossfade = s.crossfade;
    // The position fields measure a hand or a foot in the clip's own space, while the joints are shown in
    // the world the skater stands in. This is the turn between the two, so the overlay can point an arrow
    // along a field's axis and move that field by what the arrow is pulled.
    if (s.clip && !s.clip->empty()) {
        std::vector<Joint> native(skeleton_joints);
        s.clip->sample(std::round(s.time * 30.0f) / 30.0f, native); // frame_time, which is declared below
        const auto clip_place = world(native, placement_joint).rotation;
        v.field_axes = normalised(multiply(s.joints_placement.rotation, conjugate(clip_place)));
        v.field_axes_valid = finite(Joint{{1, 1, 1}, v.field_axes, {}});
    }
    v.floor = s.floor;
    v.floor_valid = s.floor_valid;
    v.duration = duration_locked(s);
    v.takeoff = s.clip ? s.clip->takeoff : -1;
    v.landing = s.clip ? s.clip->landing : -1;
    v.time = s.time;
    v.speed = s.edits.speed;
    v.joints = s.joints;
    for (const auto& t : s.edits.tracks) {
        TrackView tv{t.joint, {}, t.hidden, t.locked};
        for (const auto& k : t.keys) tv.keys.push_back({k.time, k.ease, k.smooth, k.rotation, k.offset, k.blend_out, k.blend_in});
        v.tracks.push_back(std::move(tv));
    }
    v.library = s.library;
    v.presets = s.presets;
    for (const auto& saved : s.packs) {
        const auto draft = s.pack_drafts.find(saved.name);
        const auto& pack = draft != s.pack_drafts.end() ? draft->second : saved;
        const bool on = std::any_of(s.packs_on.begin(), s.packs_on.end(), [&](const PresetPack& p) { return p.name == saved.name; });
        v.packs.push_back({saved.name, pack.made_by, on, pack.alternate, pack.members(), pack});
        v.packs.back().favorite = std::find(s.favorite_packs.begin(), s.favorite_packs.end(), saved.name) != s.favorite_packs.end();
        v.packs.back().installed = locked_key(pack.maker_key);
        if (const auto at = s.pack_times.find(saved.name); at != s.pack_times.end()) v.packs.back().modified = at->second;
        if (const auto picture = s.pack_pictures.find(saved.name); picture != s.pack_pictures.end()) {
            v.packs.back().thumbnail = utf8_path(pack_thumbnail_file(saved.name));
            v.packs.back().thumbnail_stamp = picture->second;
        }
    }
    v.open_pack = s.open_pack;
    v.ungrouped = s.ungrouped;
    v.conflicts = s.conflicts;
    v.playlist_on = s.playlist_on;
    v.playlist_playing = s.playlists.on;
    v.playlist_open = s.playlist_open;
    v.playlists.clear();
    for (const auto& l : s.playlists.lists) v.playlists.push_back({l.name, l.entries.size()});
    if (const auto* open = s.playlists.named(s.playlist_open)) {
        v.playlist_mode = open->mode;
        v.playlist = open->entries;
    } else {
        v.playlist_mode = PlaylistMode::fixed;
        v.playlist.clear();
    }
    v.hover_preview = s.hover_preview;
    v.keys = s.keys;
    v.export_folder = s.export_folder;
    v.import_folder = s.import_folder;
    v.export_path = utf8_path(pack_export_folder(s));
    v.import_path = utf8_path(pack_import_folder(s));
    v.library_path = utf8_path(library_folder());
    v.test_trick = s.test_trick;
    v.test_returned = s.test_returned;
    v.your_name = s.your_name;
    v.bracket_start = s.edits.start;
    v.bracket_end = s.edits.end;
    if (s.clip && !s.clip->empty() && has_air(*s.clip)) {
        v.catch_air = true;
        v.catch_set = s.edits.catch_at && catch_fits(*s.clip, *s.edits.catch_at);
        v.catch_at = v.catch_set ? *s.edits.catch_at : catch_guess_locked(s);
        v.keys_past_landing = keys_past_landing(s.edits, s.clip->takeoff, s.clip->landing, s.clip->duration());
    }
    v.trick_crossfade = s.edits.crossfade;
    v.catch_hold = s.edits.catch_hold.value_or(default_catch_hold);
    v.catch_hold_set = s.edits.catch_hold.has_value();
    v.drop_preview = s.drop_preview;
    if (const auto drop = drop_play_locked(s); drop.on()) v.drop_extra = drop_air(drop.land - drop.takeoff, drop.metres) - (drop.land - drop.takeoff);
    v.can_undo = !s.undo.empty();
    v.can_redo = !s.redo.empty();
    v.orbit = s.orbit;
    v.confirm_close = s.confirm_close;
    v.pop = s.edits.pop;
    v.revision = s.revision;
    v.clip_serial = s.clip_serial;
    return v;
}
// Keys sit on whole frames of the timeline (30 a second), never between two.
float frame_time(float seconds) { return std::round(seconds * 30.0f) / 30.0f; }
// The playhead's frame, for a key: on a frame, never past the clip's end (its length is seldom a whole number of frames).
float key_time_locked(const State& s) { return std::min(frame_time(s.time), duration_locked(s)); }
namespace { void write_pack_drafts_locked(State& s); } // with the packs, below
namespace { void end_preview_locked(State& s); } // below
void request_close() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    // A preview over a trick with changes not saved: back to that trick, so the warning is about it and SAVE saves it.
    if (s.previewing && s.kept && s.kept->dirty) end_preview_locked(s);
    if ((s.dirty && s.clip && !s.clip->empty()) || !s.pack_drafts.empty()) {
        s.confirm_close = true;
        ++s.revision;
        return;
    }
    s.open_requested = false;
    s.open_request_pending = true;
}
void answer_close(CloseAnswer answer) {
    std::string message;
    bool animation_changed{};
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        animation_changed = s.dirty && s.clip && !s.clip->empty();
    }
    if (answer == CloseAnswer::save && animation_changed && !save(message)) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.confirm_close = false;
        return;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.confirm_close = false;
    if (answer == CloseAnswer::cancel) return;
    if (answer == CloseAnswer::save && !s.pack_drafts.empty()) {
        try { write_pack_drafts_locked(s); }
        catch (const std::exception& e) { s.status = e.what(); publish_locked(s); return; }
    }
    if (answer == CloseAnswer::discard) s.pack_drafts.clear();
    s.open_requested = false;
    s.open_request_pending = true;
}
namespace { int add_copies_to_packs_locked(State& s, const std::string& original, const std::vector<std::string>& copies); } // below
void request_open(bool open) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.open_requested = open;
    s.open_request_pending = true;
}
void advance(float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.playing || !s.clip || s.clip->empty() || !(seconds > 0)) return;
    const float duration = s.clip->duration();
    // DROP PREVIEW: played as off a drop that high, its catch slowed as the game would (AirStretch).
    if (const auto drop = drop_play_locked(s); drop.on()) {
        if (s.drop_real < 0 || std::abs(s.time - s.drop_shown) > 1e-4f) s.drop_real = drop_real_time(drop, s.time); // moved by hand
        s.drop_real += seconds * s.edits.speed;
        float t = drop_clip_time(drop, s.drop_real);
        if (t >= duration - 1e-4f) {
            if (s.edits.loop) s.drop_real = t = 0;
            else {
                t = duration;
                s.playing = false;
            }
        }
        s.time = t;
        s.drop_shown = t;
        publish_locked(s, false);
        return;
    }
    s.time = advance_time(s.time, seconds, s.edits.speed, duration, s.edits.loop);
    if (!s.edits.loop && s.time >= duration) s.playing = false;
    publish_locked(s, false);
}
void set_time(float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!std::isfinite(seconds)) return;
    s.time = std::clamp(seconds, 0.0f, duration_locked(s));
    publish_locked(s, false);
}
void set_playing(bool playing) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.playing = playing && s.clip && !s.clip->empty();
    if (s.playing && s.time >= duration_locked(s) - 1e-3f) s.time = 0;
    publish_locked(s);
}
void set_loop(bool loop) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.loop = loop;
    changed_locked(s);
}
void set_speed(float speed) {
    if (!std::isfinite(speed)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.speed = std::clamp(speed, 0.1f, 4.0f);
    publish_locked(s, false);
}
std::optional<std::uint32_t> physics_state(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto core = component ? read<std::uintptr_t>(component + 0x70) : std::nullopt;
    if (!core || !*core) return std::nullopt;
    const auto type = read<std::uintptr_t>(*core);
    if (!type || *type != base + addr::no_bail::bail_core_vtable) return std::nullopt;
    const auto context = read<std::uintptr_t>(*core + 0x3c0);
    if (!context || !*context) return std::nullopt;
    return read<std::uint32_t>(*context + addr::skater_state::physics_state_offset);
}
std::atomic<std::uint64_t> room_switches{};
std::uint64_t room_changes() noexcept { return room_switches.load(std::memory_order_acquire); }
void set_room(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.room = on;
    s.eased_valid = false;
    room_switches.fetch_add(1, std::memory_order_acq_rel);
    s.status = on ? "To the studio room: the studio opens again there." : "Out of the studio room: the studio stays open where you are.";
    publish_locked(s);
}
void set_play_in_game(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.play_in_game = on;
    s.status = on ? "Your saved tricks play in game, as the studio shows them." : "Tricks play as the game has them.";
    publish_locked(s);
}
void set_front_foot(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.front_foot = on;
    save_settings_locked(s);
    s.status = on ? "Tricks play by front foot: any stance, mirrored when the other foot leads."
                  : "Tricks play only for the stance they were recorded in.";
    publish_locked(s);
}
void set_joint_limits(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.limits = on;
    save_settings_locked(s);
    s.status = on ? "Joint limits on: knees and elbows bend one way and every joint stays in the range the game uses, in the studio and in game."
                  : "Joint limits off: joints go wherever you push them, in the studio and in game.";
    publish_locked(s);
}
void set_crossfade(float seconds) {
    if (!std::isfinite(seconds)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.crossfade = std::clamp(seconds, 0.1f, 0.8f);
    save_settings_locked(s);
    s.status = std::format("Your tricks hand over to the game's next move over {:.2f} s.", s.crossfade);
    publish_locked(s, false);
}
void set_grab(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.grab_board = on;
    save_settings_locked(s);
    s.status = on ? "Hands close on the board when they come near it." : "Hands stay as they are.";
    publish_locked(s);
}
void set_stance(int choice) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.stance_choice = std::clamp(choice, 0, 2);
    save_settings_locked(s);
    const bool goofy = studio_goofy(s.stance_choice);
    s.status = s.stance_choice == 0 ? (goofy ? "Stance: as the game has him, goofy." : "Stance: as the game has him, regular.")
               : goofy ? "Stance: goofy." : "Stance: regular.";
    publish_locked(s);
}
void set_solo(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.solo = on;
    s.status = on ? "Solo: a turn moves that joint alone. The joints below keep the way they face."
                  : "A turn carries the joints below it, as before.";
    publish_locked(s);
}
void begin_change() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.undo.push_back({s.edits.tracks, s.edits.start, s.edits.end, s.edits.catch_at, s.edits.crossfade, s.edits.catch_hold});
    if (s.undo.size() > 200) s.undo.pop_front();
    s.redo.clear();
}
namespace {
// Solo: the editable joints hanging directly off `joint` - the ones whose nearest editable parent it is.
// The board is not part of the skeleton, so it neither holds children nor is one.
std::vector<std::uint16_t> solo_children(const State& s, std::uint16_t joint) {
    std::vector<std::uint16_t> children;
    if (!s.solo || joint == board_joint || !s.clip || s.clip->empty()) return children;
    for (const auto& e : editable_joints()) {
        if (e.index == board_joint || e.index == joint || e.index >= skeleton_joints) continue;
        if (const auto* t = s.edits.track(e.index); t && (t->locked || t->hidden)) continue;
        for (auto up = parent(e.index); up >= 0; up = parent(static_cast<std::uint16_t>(up))) {
            const auto above = static_cast<std::uint16_t>(up);
            if (above == joint) {
                children.push_back(e.index);
                break;
            }
            if (editable(above)) break; // a nearer editable parent owns this one
        }
    }
    return children;
}
// The way each of those joints faces in the world, with the edits as they stand.
std::vector<Quat> facing_of(const State& s, std::span<const std::uint16_t> children) {
    const float at = key_time_locked(s);
    std::vector<Joint> pose(skeleton_joints);
    s.clip->sample(at, pose);
    apply_edits(s.edits, at, pose);
    std::vector<Quat> facing;
    facing.reserve(children.size());
    for (const auto c : children) facing.push_back(world(pose, c).rotation);
    return facing;
}
// Turns each of them back to the way it faced. A joint's place in the world is the chain above it times
// its own key, so multiplying its key by conj(where it faces now) * (where it faced) puts it back.
void hold_facing(State& s, std::span<const std::uint16_t> children, std::span<const Quat> before) {
    const float at = key_time_locked(s);
    std::vector<Joint> pose(skeleton_joints);
    s.clip->sample(at, pose);
    apply_edits(s.edits, at, pose);
    for (std::size_t i = 0; i < children.size() && i < before.size(); ++i) {
        const auto child = children[i];
        const auto turn = normalised(multiply(conjugate(world(pose, child).rotation), before[i]));
        if (std::abs(turn[3]) > 0.999995f) continue; // it did not swing: it needs no key
        const auto* t = s.edits.track(child);
        Offset held = t ? evaluate(*t, at) : Offset{};
        held.rotation = normalised(multiply(held.rotation, turn));
        if (!editable(child)->moves) held.offset = {};
        set_key(s.edits, child, at, held);
    }
}
} // namespace

namespace {
// Keeps the key of `joint` at `at` inside the joint's range: the pose it makes (the clip's pose with the key
// on it) is limited, and the key is worked back out from that. So what is saved is already a natural pose,
// in the studio and in game alike. Caller holds the mutex.
void limit_key(State& s, std::uint16_t joint, float at) {
    if (!s.limits || !s.clip || s.clip->empty() || !has_joint_limit(joint)) return;
    const auto* t = s.edits.track(joint);
    if (!t || t->locked || t->keys.empty()) return;
    std::vector<Joint> native(s.clip->joints);
    s.clip->sample(at, native);
    if (joint >= native.size()) return;
    auto key = evaluate(*t, at);
    const auto posed = normalised(multiply(native[joint].rotation, key.rotation));
    const auto limited = limit_rotation(joint, posed);
    if (limited == posed) return;
    key.rotation = normalised(multiply(conjugate(native[joint].rotation), limited));
    set_key(s.edits, joint, at, key);
}
// The joint and the limb above it that a drag bends.
void limit_chain(State& s, std::uint16_t joint, std::size_t chain, float at) {
    int at_joint = joint;
    for (std::size_t i = 0; i <= chain && at_joint >= 0; ++i, at_joint = parent(static_cast<std::uint16_t>(at_joint)))
        limit_key(s, static_cast<std::uint16_t>(at_joint), at);
}
} // namespace
std::size_t limb_chain(std::uint16_t joint) noexcept {
    switch (joint) {
    case 10: case 343: case 49: case 278: return 3;  // the feet and the hands
    case 9: case 342: case 48: case 277: return 2;   // the knees and the elbows
    default: return 0;
    }
}
Offset offset_of(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto* t = s.edits.track(joint);
    return t ? evaluate(*t, s.time) : Offset{};
}
void set_offset(std::uint16_t joint, const Offset& value) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (!editable(joint)) return;
    if (const auto* t = s.edits.track(joint); t && t->locked) {
        s.status = "That track is locked.";
        return;
    }
    Offset clean = value;
    if (!editable(joint)->moves) clean.offset = {};
    for (auto& v : clean.offset) v = std::clamp(v, -2.0f, 2.0f);
    // Solo: the way the joints below face is read before the turn and put back after it.
    const auto children = solo_children(s, joint);
    std::vector<Quat> facing;
    if (!children.empty()) facing = facing_of(s, children);
    set_key(s.edits, joint, key_time_locked(s), clean);
    limit_key(s, joint, key_time_locked(s));
    if (!children.empty()) hold_facing(s, children, facing);
    changed_locked(s);
}
namespace {
} // namespace

// Both in the clip's own frame: the field is where the joint is now, from where it rests, in the skater's axes.
Vec3 limb_position(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.clip || s.clip->empty() || !editable(joint)) return {};
    const float at = key_time_locked(s);
    std::vector<Joint> native(skeleton_joints);
    s.clip->sample(at, native);
    std::vector<Joint> posed(native);
    apply_edits(s.edits, at, posed);
    const auto now = world(posed, joint).position, rest = world(native, joint).position;
    return {now[0] - rest[0], now[1] - rest[1], now[2] - rest[2]};
}

void set_limb_position(std::uint16_t joint, const Vec3& offset) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (!s.clip || s.clip->empty() || !editable(joint)) return;
    if (const auto* t = s.edits.track(joint); t && t->locked) {
        s.status = "That track is locked.";
        return;
    }
    const auto chain = limb_chain(joint);
    if (!chain) return;
    const float at = key_time_locked(s);
    std::vector<Joint> native(skeleton_joints);
    s.clip->sample(at, native);
    const auto rest = world(native, joint).position;
    const Vec3 target{rest[0] + offset[0], rest[1] + offset[1], rest[2] + offset[2]};
    try {
        anim_studio::drag_joint(s.edits, native, at, joint, target, chain);
    } catch (const std::exception& e) {
        s.status = e.what();
        return;
    }
    // The limb's end carries no offset of its own: the bend places it, so it cannot stretch the bone.
    Offset held{};
    if (const auto* t = s.edits.track(joint)) held.rotation = evaluate(*t, at).rotation;
    set_key(s.edits, joint, at, held);
    limit_chain(s, joint, chain, at);
    changed_locked(s);
}

void move_bracket(int which, float seconds) {
    if (!std::isfinite(seconds)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (!s.clip || s.clip->empty()) return;
    const float duration = s.clip->duration();
    const float t = std::clamp(seconds, 0.0f, duration);
    float start = s.edits.start, end = s.edits.end < 0 ? duration : s.edits.end;
    const float gap = std::min(duration, 1.0f / 30);
    if (which == 1) {
        start = std::clamp(t, 0.0f, std::max(0.0f, end - gap));
    } else if (which == 2) {
        end = std::clamp(std::max(t, start + gap), 0.0f, duration);
    } else return;
    // A bracket stops a frame short of a key you posed, on the side it came from: landing on it would put that pose
    // back to 0, 0, 0 (the brackets' keys stay at nothing, pin_brackets_locked).
    const float from = which == 1 ? s.edits.start : s.edits.end < 0 ? duration : s.edits.end;
    float to = std::clamp(on_a_frame(which == 1 ? start : end), 0.0f, duration);
    const auto posed = [&](float at) {
        for (const auto& track : s.edits.tracks)
            for (const auto& key : track.keys)
                if (std::abs(key.time - at) < 0.5f / key_fps && !(key.rotation == Quat{0, 0, 0, 1} && key.offset == Vec3{})) return true;
        return false;
    };
    const float step = to > from ? -1.0f / key_fps : 1.0f / key_fps;
    bool stopped = false;
    while (std::abs(to - from) >= 0.5f / key_fps && posed(to)) {
        to += step;
        stopped = true;
    }
    if (std::abs(to - from) < 0.5f / key_fps) to = from;
    if (!move_window_edge(s.edits, which, to, duration)) return;
    if (stopped) s.status = "The bracket stops next to your pose: on its frame it would put that pose back to the game's.";
    key_all_at(s.edits, s.edits.start);
    key_all_at(s.edits, s.edits.end < 0 ? duration : s.edits.end);
    changed_locked(s);
}

void set_catch(float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (!s.clip || s.clip->empty()) return;
    if (!std::isfinite(seconds) || seconds < 0) {
        if (!s.edits.catch_at) return;
        s.edits.catch_at.reset();
        s.status = "CATCH is the studio's own again: where the board is back under the feet.";
        changed_locked(s);
        return;
    }
    const auto& clip = *s.clip;
    if (!has_air(clip)) {
        s.status = "This trick has no air to hold a catch in.";
        ++s.revision;
        return;
    }
    // On a frame, a frame or more after the pop and before the landing (catch_fits).
    const float low = std::ceil((clip.takeoff + 1 / key_fps) * key_fps - 1e-3f) / key_fps;
    const float high = std::floor((clip.landing - 1 / key_fps) * key_fps + 1e-3f) / key_fps;
    if (high < low) return;
    const float at = std::clamp(on_a_frame(seconds), low, high);
    if (s.edits.catch_at && std::abs(*s.edits.catch_at - at) < 0.5f / key_fps) return;
    s.edits.catch_at = at;
    changed_locked(s);
}
// STANCE (THIS TRICK): the open trick becomes its normal, fakie, switch or switch fakie version, as make_stance_version
// makes one but in place: it keeps that foot (keep_leads_right; publish_locked mirrors the open trick onto it at once) and
// that fakie flag, and SAVE stores it so - its clip on that foot and its labels from it. In game it is then that stance's
// trick: Exact stance plays it only when riding so; Front foot plays it first when riding so.
void set_trick_stance(int version) {
    if (version < 0 || version > 3) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing || !s.clip || s.clip->empty()) return;
    if (s.edits.trick.starts_with("native:")) {
        s.status = "A piece of the game's animation plays whichever way round the game plays it.";
        ++s.revision;
        return;
    }
    const bool own_right = studio_goofy(s.stance_choice);
    const bool want_right = version >= 2 ? !own_right : own_right, want_fakie = version % 2 == 1;
    const auto lead = s.edits.keep_leads_right ? s.edits.keep_leads_right : clip_leads_right(*s.clip);
    if (s.edits.keep_leads_right && lead && *lead == want_right && s.edits.fakie.value_or(false) == want_fakie) return;
    s.edits.keep_leads_right = want_right;
    s.edits.fakie = want_fakie;
    s.edits.switch_stance = authored_goofy(s.edits) != want_right; // its label (set again from the clip if it is mirrored)
    static constexpr std::array<const char*, 4> said{"your own stance", "fakie", "switch", "switch fakie"};
    static constexpr std::array<const char*, 4> ride{"normal", "fakie", "switch", "switch fakie"};
    const auto i = static_cast<std::size_t>(version);
    changed_locked(s); // turned onto its foot here, which says so in the status: this says what it means
    s.status = std::format("{} is in {} now. Save it, and it plays when you ride {}.", s.edits.name.empty() ? std::string("The trick") : s.edits.name,
                           said[i], ride[i]);
    ++s.revision;
    logging::log(Level::info, Channel::skater, "Animation Studio: {} set to {} in the editor.", s.edits.name, ride[i]);
}
void set_trick_crossfade(float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing || !s.clip || s.clip->empty()) return;
    if (!std::isfinite(seconds) || seconds < 0) {
        if (!s.edits.crossfade) return;
        s.edits.crossfade.reset();
        s.status = "This trick hands over with the main Crossfade again (SETTINGS).";
    } else {
        const float own = std::round(std::clamp(seconds, 0.1f, 0.8f) * 100) / 100;
        if (s.edits.crossfade && std::abs(*s.edits.crossfade - own) < 0.005f) return;
        if (!s.edits.crossfade) s.status = "This trick hands over at its own Crossfade: the main one is skipped for it.";
        s.edits.crossfade = own;
    }
    changed_locked(s);
}
void set_catch_hold(float amount) {
    if (!std::isfinite(amount)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing || !s.clip || s.clip->empty()) return;
    const float hold = std::round(std::clamp(amount, 0.0f, 1.0f) * 100) / 100;
    if (s.edits.catch_hold && std::abs(*s.edits.catch_hold - hold) < 0.005f) return;
    s.edits.catch_hold = hold;
    changed_locked(s);
}
void set_drop_preview(float metres) {
    if (!std::isfinite(metres)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const float drop = std::round(std::clamp(metres, 0.0f, 4.0f) * 10) / 10;
    if (std::abs(drop - s.drop_preview) < 0.01f) return;
    s.drop_preview = drop;
    s.drop_real = -1;
    publish_locked(s, false);
}

void drag_limb(std::uint16_t joint, const Vec3& target_view) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (!s.clip || s.clip->empty() || !editable(joint)) return;
    if (const auto* t = s.edits.track(joint); t && t->locked) {
        s.status = "That track is locked.";
        return;
    }
    const float at = key_time_locked(s);
    std::vector<Joint> native(s.clip->joints);
    s.clip->sample(at, native);
    // The target is in the view's frame (the joints' frame); the solver works in the clip's. Both frames have
    // the placement joint in them: a point is moved from the view's placement to the clip's.
    std::vector<Joint> posed(native);
    apply_edits(s.edits, at, posed);
    const Joint clip_place = world(posed, placement_joint), view_place = s.joints_placement;
    const Vec3 offset{target_view[0] - view_place.position[0], target_view[1] - view_place.position[1], target_view[2] - view_place.position[2]};
    const Vec3 local = rotate(conjugate(view_place.rotation), offset);
    const Vec3 moved = rotate(clip_place.rotation, local);
    const Vec3 target{moved[0] + clip_place.position[0], moved[1] + clip_place.position[1], moved[2] + clip_place.position[2]};
    try {
        anim_studio::drag_joint(s.edits, native, at, joint, target, std::max<std::size_t>(limb_chain(joint), 2));
    } catch (const std::exception& e) {
        s.status = e.what();
        return;
    }
    limit_chain(s, joint, std::max<std::size_t>(limb_chain(joint), 2), at);
    changed_locked(s);
}
void delete_key(std::uint16_t joint, float time) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (const auto* track = s.edits.track(joint); track && track->locked) return;
    if (anim_studio::remove_key(s.edits, joint, time, 1e-3f)) changed_locked(s);
}
void move_key(std::uint16_t joint, float from, float to) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    auto* t = s.edits.track(joint);
    if (!t || t->locked) return;
    for (std::size_t i = 0; i < t->keys.size(); ++i)
        if (std::abs(t->keys[i].time - from) < 1e-3f) {
            anim_studio::move_key(*t, i, frame_time(to), duration_locked(s));
            changed_locked(s);
            return;
        }
}
void set_ease(std::uint16_t joint, float time, Ease ease) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (auto* t = s.edits.track(joint); t && !t->locked)
        for (auto& k : t->keys) {
            if (std::abs(k.time - time) >= 1e-3f) continue;
            const bool round = k.smooth && ease == Ease::smooth; // a Linear or Step key is never round
            if (k.ease == ease && k.smooth == round) continue;
            k.ease = ease;
            k.smooth = round;
            changed_locked(s);
        }
}
void set_blend(std::uint16_t joint, float time, bool in, float seconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    const float blend = std::isfinite(seconds) && seconds > 0 ? std::clamp(seconds, min_blend, max_blend) : 0.0f;
    if (auto* t = s.edits.track(joint); t && !t->locked)
        for (auto& k : t->keys) {
            auto& value = in ? k.blend_in : k.blend_out;
            if (std::abs(k.time - time) >= 1e-3f || value == blend) continue;
            value = blend;
            changed_locked(s);
        }
}
bool smooth_chosen_keys(std::span<const std::pair<std::uint16_t, float>> chosen, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return false; // a preview is to watch only
    const float end = s.edits.end < 0 ? (s.clip ? s.clip->duration() : 0.0f) : s.edits.end;
    std::map<std::uint16_t, std::vector<float>> by_joint;
    for (const auto& [joint, time] : chosen) by_joint[joint].push_back(time);
    std::size_t changed = 0;
    for (const auto& [joint, times] : by_joint)
        if (auto* t = s.edits.track(joint); t && !t->locked) changed += anim_studio::smooth_keys_at(*t, times, s.edits.start, end);
    message = !changed ? std::string("Those keys are smooth already. Ctrl+Z takes smoothing back.")
              : chosen.size() == 1 ? std::string("The key is smoothed: it eases into its neighbours and shows round.")
                                   : std::format("Smoothed {} keys: they ease into their neighbours and show round.", chosen.size());
    s.status = message;
    if (changed) changed_locked(s);
    else publish_locked(s);
    return changed > 0;
}
void reset_joint(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    std::erase_if(s.edits.tracks, [&](const Track& t) { return t.joint == joint && !t.locked; });
    changed_locked(s);
}
void set_track_flags(std::uint16_t joint, bool hidden, bool locked) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    auto* t = s.edits.track(joint);
    if (!t) {
        if (!editable(joint)) return;
        s.edits.tracks.push_back({joint, {}, hidden, locked});
    } else {
        t->hidden = hidden;
        t->locked = locked;
    }
    changed_locked(s);
}
void copy_pose() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.clipboard.clear();
    for (const auto& t : s.edits.tracks)
        if (!t.keys.empty()) s.clipboard.emplace_back(t.joint, evaluate(t, s.time));
    s.status = std::format("Copied the pose of {} joints.", s.clipboard.size());
}
bool settle_at_landing(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing || !s.clip || s.clip->empty()) return false;
    const auto joints = keys_past_landing(s.edits, s.clip->takeoff, s.clip->landing, s.clip->duration());
    if (joints.empty()) {
        message = "Nothing keyed in the air runs on past the landing.";
        return false;
    }
    const float at = std::round(s.clip->landing * key_fps) / key_fps;
    int done = 0;
    for (const auto joint : joints) {
        if (const auto* t = s.edits.track(joint); t && t->locked) continue;
        set_key(s.edits, joint, at, Offset{});
        ++done;
    }
    message = std::format("Keyed {} {} back to the game's own pose at LAND: what you keyed in the air lands with the game now.", done,
                          done == 1 ? "joint" : "joints");
    s.status = message;
    changed_locked(s);
    return true;
}
void paste_pose() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (s.clipboard.empty()) return;
    for (const auto& [joint, value] : s.clipboard) {
        const auto* t = s.edits.track(joint);
        if (!t || !t->locked) set_key(s.edits, joint, key_time_locked(s), value);
    }
    s.status = std::format("Pasted the pose of {} joints.", s.clipboard.size());
    changed_locked(s);
}
void undo() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (s.undo.empty()) return;
    s.redo.push_back({s.edits.tracks, s.edits.start, s.edits.end, s.edits.catch_at, s.edits.crossfade, s.edits.catch_hold});
    s.edits.start = s.undo.back().start;
    s.edits.end = s.undo.back().end;
    s.edits.catch_at = s.undo.back().catch_at;
    s.edits.crossfade = s.undo.back().crossfade;
    s.edits.catch_hold = s.undo.back().catch_hold;
    s.edits.tracks = std::move(s.undo.back().tracks);
    s.undo.pop_back();
    changed_locked(s);
}
void redo() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) return; // a preview is to watch only
    if (s.redo.empty()) return;
    s.undo.push_back({s.edits.tracks, s.edits.start, s.edits.end, s.edits.catch_at, s.edits.crossfade, s.edits.catch_hold});
    s.edits.start = s.redo.back().start;
    s.edits.end = s.redo.back().end;
    s.edits.catch_at = s.redo.back().catch_at;
    s.edits.crossfade = s.redo.back().crossfade;
    s.edits.catch_hold = s.redo.back().catch_hold;
    s.edits.tracks = std::move(s.redo.back().tracks);
    s.redo.pop_back();
    changed_locked(s);
}
// ---- packs (the PACKS tab) -------------------------------------------------------------------------
// One pack is used at a time; with none on, every saved animation plays. Every change - switching one on,
// what is in it, Fixed or Alternate, the cycle - is saved at once and plays at once.
namespace {
PresetPack& pack_draft_locked(State& s, const std::string& name) {
    if (const auto found = s.pack_drafts.find(name); found != s.pack_drafts.end()) return found->second;
    return s.pack_drafts.emplace(name, read_pack_file(name)).first->second;
}
// Caller holds the mutex: the library and the packs are read again, and the tricks that play with them.
void packs_changed_locked(State& s, const std::string& message) {
    read_library_locked(s);
    s.tricks_read = false;
    s.status = message;
    publish_locked(s);
}
void write_pack_names_on(const std::vector<std::string>& names) {
    std::string text;
    for (const auto& name : names) text += name + "\n";
    write_preset_file(library_folder() / L"presets" / L"current.txt", text);
}
// Caller holds the mutex. Every pack with changes is written, and the tricks read again.
void write_pack_drafts_locked(State& s) {
    for (const auto& [name, pack] : s.pack_drafts) write_preset_file(pack_file(name), pack_json(pack).dump(2));
    s.pack_drafts.clear();
    read_library_locked(s);
    s.tricks_read = false;
}
// Caller holds the mutex, the library read with the copies in it. Copies of `original` go into every pack it is
// in, each filed under its own trick and pop. Returns how many packs.
int add_copies_to_packs_locked(State& s, const std::string& original, const std::vector<std::string>& copies) {
    int joined = 0;
    const auto packs = s.packs;
    for (const auto& saved : packs) {
        if (!saved.has(original)) continue;
        auto& pack = pack_draft_locked(s, saved.name);
        for (const auto& copy : copies) {
            const auto found = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return p.name == copy; });
            if (found == s.presets.end() || pack.has(copy)) continue;
            pack.slots[preset_slot(found->trick, found->pop)].push_back(copy);
            if (saved.off.contains(original)) pack.off.insert(copy); // plays there as the one it was made from does
        }
        ++joined;
    }
    if (joined) write_pack_drafts_locked(s);
    try {
        auto playing = playing_names(s.library, !s.packs_on.empty());
        if (playing.contains(original)) {
            playing.insert(copies.begin(), copies.end());
            write_playing_names(playing);
        }
    } catch (...) {}
    return joined;
}
// What a change can leave behind in a pack: empty slots, the modes of slots that are gone, and unlinked names of tricks
// no longer in it.
void tidy_pack(PresetPack& pack) {
    std::erase_if(pack.slots, [](const auto& slot) { return slot.second.empty(); });
    std::erase_if(pack.slot_alternate, [&](const auto& mode) { return !pack.slots.contains(mode.first); });
    std::erase_if(pack.unlinked, [&](const std::string& member) { return !pack.has(member); });
}
// Caller holds the mutex: `change` is made to the pack and it is saved, and what plays read again.
template <class Change>
bool change_pack_locked(State& s, const std::string& name, std::string& message, Change&& change) {
    try {
        if (name.empty() || name != file_safe(name) || !std::filesystem::exists(pack_file(name)))
            throw std::runtime_error("There is no pack called " + name + ".");
        if (!s.library_read) read_library_locked(s);
        auto& pack = pack_draft_locked(s, name);
        change(pack);
        tidy_pack(pack);
        write_pack_drafts_locked(s);
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        s.pack_drafts.erase(name);
        message = e.what();
        s.status = message;
        publish_locked(s);
        return false;
    }
}
std::string shown_name(const State& s, const std::string& animation) {
    const auto found = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return p.name == animation; });
    return found != s.presets.end() && !found->label.empty() ? found->label : animation;
}
} // namespace
bool create_preset_pack(std::string& message, const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (!s.library_read) read_library_locked(s);
        PresetPack pack;
        pack.made_by = s.your_name;
        const bool unnamed = std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); });
        const auto wanted = unnamed ? std::string{} : file_safe(name);
        if (wanted.empty()) {
            int number = 1;
            do { pack.name = "Pack " + std::to_string(number++); } while (std::filesystem::exists(pack_file(pack.name)));
        } else {
            pack.name = wanted;
            if (reserved_pack_name(pack.name)) throw std::runtime_error(pack.name + " is a name the studio keeps for itself: choose another name.");
            if (std::filesystem::exists(pack_file(pack.name))) throw std::runtime_error("There is a pack called " + pack.name + " already: choose another name.");
        }
        write_preset_file(pack_file(pack.name), pack_json(pack).dump(2));
        s.open_pack = pack.name;
        message = "Made the pack " + pack.name + ". Put your tricks in it, then add it to a playlist to play it.";
        packs_changed_locked(s, message);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s); return false; }
}
void open_pack(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.open_pack = name;
    publish_locked(s, false);
}
bool set_pack_on(const std::string& name, bool on, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (name != file_safe(name) || !std::filesystem::exists(pack_file(name))) throw std::runtime_error("There is no pack called " + name + ".");
        auto names = pack_names_on();
        std::erase(names, name);
        if (on) names.push_back(name); // any number on at once; tricks more than one has are chosen between (conflicts)
        write_pack_names_on(names);
        message = on ? name + " is on: its tricks play." : name + " is off.";
        if (names.empty()) message += " No pack is on, so all your saved tricks play.";
        packs_changed_locked(s, message);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s); return false; }
}
bool set_pack_member(const std::string& pack_name, const std::string& animation, bool in, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.library_read) read_library_locked(s);
    const auto found = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return p.name == animation; });
    if (in && found == s.presets.end()) {
        message = animation + " has no trick yet: open it, give it its trick and save it first.";
        s.status = message;
        publish_locked(s, false);
        return false;
    }
    if (in && someone_elses_locked(s, animation)) {
        message = shown_name(s, animation) + " is another maker's: it plays for you, but only your own tricks go in your packs.";
        s.status = message;
        publish_locked(s, false);
        return false;
    }
    const auto slot = found != s.presets.end() ? preset_slot(found->trick, found->pop) : std::string{};
    message = in ? "Put " + shown_name(s, animation) + " in " + pack_name + ": tick ACTIVE there to have it play in game."
                 : "Took " + shown_name(s, animation) + " out of " + pack_name + ".";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) {
        for (auto& [filed, names] : pack.slots) std::erase(names, animation);
        pack.unlinked.erase(animation);
        if (in) {
            pack.slots[slot].push_back(animation);
            pack.off.insert(animation); // put in, not playing: PLAY switches it on
        } else pack.off.erase(animation);
    });
}
bool set_pack_mode(const std::string& pack_name, const std::string& slot, bool alternate, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    message = alternate ? "Alternate: one of the ones linked into the cycle plays each attempt, at random (never the same twice running)."
                        : "Fixed: the one chosen plays every time.";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) {
        if (!pack.slots.contains(slot)) throw std::runtime_error("That trick is not in " + pack_name + ".");
        pack.slot_alternate[slot] = alternate;
    });
}
bool set_pack_fixed(const std::string& pack_name, const std::string& animation, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    message = shown_name(s, animation) + " is the one that plays.";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) {
        const auto slot = pack.slot_of(animation);
        if (slot.empty()) throw std::runtime_error(animation + " is not in " + pack_name + ".");
        auto& names = pack.slots[slot];
        std::erase(names, animation);
        names.insert(names.begin(), animation);
        pack.slot_alternate[slot] = false;
    });
}
bool set_pack_linked(const std::string& pack_name, const std::string& animation, bool linked, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    message = shown_name(s, animation) + (linked ? " is in the cycle." : " is out of the cycle.");
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) {
        const auto slot = pack.slot_of(animation);
        if (slot.empty()) throw std::runtime_error(animation + " is not in " + pack_name + ".");
        if (linked) pack.unlinked.erase(animation);
        else pack.unlinked.insert(animation);
        pack.slot_alternate[slot] = true;
    });
}
bool set_label(const std::string& animation, const std::string& label_in, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        auto label = utf8_prefix(label_in, 48);
        while (!label.empty() && std::isspace(static_cast<unsigned char>(label.back()))) label.pop_back();
        while (!label.empty() && std::isspace(static_cast<unsigned char>(label.front()))) label.erase(label.begin());
        const auto safe = file_safe(animation);
        const auto path = saved_json(safe);
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error(animation + " is not in your tricks.");
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        // Only the label changes in the file. (Read into an animation and written back, it lost the clip's
        // length, pop and landing, which live in the .clip, and no longer loaded.)
        (void)from_json(text); // it has to be a saved animation
        auto root = Json::parse(text);
        if (!root.is_object()) throw std::runtime_error(animation + " is not a saved animation.");
        root["label"] = label;
        // A label is not a save: which of a kind plays goes by when it was saved.
        std::error_code error;
        const auto saved_when = std::filesystem::last_write_time(path, error);
        write_preset_file(path, root.dump(2));
        if (!error) std::filesystem::last_write_time(path, saved_when, error);
        if (s.saved_as == safe) s.edits.label = label;
        message = label.empty() ? animation + " is shown by its own name again." : animation + " is labelled " + label + ".";
        read_library_locked(s);
        s.status = message;
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s, false); return false; }
}
namespace {
// A saved animation's file, read whole; checked to be one.
std::string saved_text(const std::string& safe) {
    std::ifstream in(saved_json(safe), std::ios::binary);
    if (!in) throw std::runtime_error(safe + " is not in your tricks.");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    (void)from_json(text);
    return text;
}
} // namespace
void set_grouped(const std::string& animation, bool grouped) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto safe = file_safe(animation);
    if (grouped) s.ungrouped.erase(safe);
    else s.ungrouped.insert(safe);
    save_settings_locked(s);
    publish_locked(s, false);
}
bool set_saved_pop(const std::string& animation, const std::string& pop_in, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        const std::string pop = pop_in == "low" || pop_in == "mid" || pop_in == "high" ? pop_in : std::string{};
        const auto safe = file_safe(animation);
        const auto text = saved_text(safe);
        auto root = Json::parse(text);
        if (!root.is_object()) throw std::runtime_error(animation + " is not a saved animation.");
        const std::string trick = root.value("trick", "");
        if (trick.empty()) throw std::runtime_error(animation + " has no trick, so it has no pop: it plays as a piece of the game's animation.");
        // Only the pop changes in the file, and not when it was saved (which of a kind plays goes by that).
        root["pop"] = pop;
        const auto path = saved_json(safe);
        std::error_code error;
        const auto saved_when = std::filesystem::last_write_time(path, error);
        write_preset_file(path, root.dump(2));
        if (!error) std::filesystem::last_write_time(path, saved_when, error);
        if (s.saved_as == safe) s.edits.pop = pop;
        read_library_locked(s);
        // Filed under its new pop in every pack it is in (keeping Fixed or Alternate and its place in the cycle).
        const auto packs = s.packs;
        bool moved = false;
        for (const auto& saved : packs) {
            if (!saved.has(safe)) continue;
            auto& pack = pack_draft_locked(s, saved.name);
            // Where it was: its place in its slot, and the slot's own Fixed or Alternate.
            std::optional<bool> mode;
            std::size_t place = 0;
            for (auto& [slot, names] : pack.slots)
                if (const auto at = std::find(names.begin(), names.end(), safe); at != names.end()) {
                    place = static_cast<std::size_t>(at - names.begin());
                    if (const auto m = pack.slot_alternate.find(slot); m != pack.slot_alternate.end()) mode = m->second;
                    std::erase(names, safe);
                }
            const auto to = preset_slot(trick, pop);
            const bool new_slot = !pack.slots.contains(to) || pack.slots[to].empty();
            auto& list = pack.slots[to];
            list.insert(list.begin() + static_cast<std::ptrdiff_t>(std::min(place, list.size())), safe);
            // A slot of its own takes its mode with it; one others are in keeps theirs.
            if (new_slot && mode) pack.slot_alternate[to] = *mode;
            tidy_pack(pack);
            moved = true;
        }
        if (moved) write_pack_drafts_locked(s);
        s.tricks_read = false;
        message = pop.empty() ? animation + " plays on any pop (where you have no trick for that pop)." : animation + " plays on " + pop + " pops only.";
        s.status = message;
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s, false); return false; }
}
// The game's own clip of `pop` for a trick opened from the game's tricks, leading with the same foot as `clip`, and the
// animation's keys, brackets and catch moved onto its marks (retimed). Empty for a recording, a piece of the game's
// animation, or a pop the library does not have.
// A clip of the trick library, read once while its file stays the same (every trick moved onto it shares the read).
std::shared_ptr<const Clip> game_clip_once(const GameTrick& g) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<FileStamp, std::shared_ptr<const Clip>>> kept;
    const auto stamp = stamp_of(library_folder() / L"library" / std::filesystem::path(g.file));
    {
        std::lock_guard lock(mutex);
        if (const auto found = kept.find(g.file); found != kept.end() && found->second.first == stamp) return found->second.second;
    }
    std::shared_ptr<const Clip> clip = read_game_clip(g);
    std::lock_guard lock(mutex);
    if (kept.size() >= 96) kept.clear();
    kept[g.file] = {stamp, clip};
    return clip;
}
std::optional<std::pair<Animation, Clip>> onto_game_pop(const Animation& a, const Clip& clip, const std::string& pop) {
    if (a.source.kind != "native_asset") return std::nullopt;
    const auto& from = a.source.asset;
    const auto cut = from.rfind('_');
    if (cut == std::string::npos || !from.starts_with("animation/dingo/c_proto_onb_trick_")) return std::nullopt;
    const auto own = from.substr(cut + 1);
    if (own != "low" && own != "mid" && own != "high") return std::nullopt;
    const auto to = from.substr(0, cut + 1) + pop;
    if (to == from) return std::nullopt;
    try {
        const auto library = game_tricks();
        if (!library) return std::nullopt;
        const auto g = std::find_if(library->begin(), library->end(), [&](const GameTrick& t) { return t.asset == to && !t.preview_only && !t.file.empty(); });
        if (g == library->end()) return std::nullopt;
        const auto target = game_clip_once(*g);
        if (!target || target->empty()) return std::nullopt;
        Clip base = *target;
        const auto lead = clip_leads_right(clip), target_lead = clip_leads_right(base);
        if (lead && target_lead && *lead != *target_lead) base = mirror_clip(base);
        // Its keys onto the other clip by where the board is (board_time_map): the game flips at the same pace on every
        // pop and holds the catch longer on a higher one, so the even stretch put a copy's flip out of step with its board.
        const auto map = board_time_map(board_track(base), board_track(clip));
        Animation out = !map.empty() ? retimed_with(a, [&](float t) { return map.back(t); }, base.duration())
                                     : retimed(a, clip.takeoff, clip.landing, clip.duration(), base.takeoff, base.landing, base.duration());
        out.source.asset = to;
        out.clip = {};
        out.clip.times = {0, base.duration()};
        out.clip.takeoff = base.takeoff;
        out.clip.landing = base.landing;
        return std::pair{std::move(out), std::move(base)};
    } catch (...) {
        return std::nullopt;
    }
}
bool copy_saved_to_pops(const std::string& animation, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        const auto safe = file_safe(animation);
        const auto text = saved_text(safe);
        auto root = Json::parse(text);
        if (!root.is_object()) throw std::runtime_error(animation + " is not a saved animation.");
        if (root.value("trick", "").empty()) throw std::runtime_error(animation + " has no trick, so it has no pop to copy to.");
        // Locked to its maker: copied all the same, each copy on its own pop and still the maker's (its key and its lock go
        // with it: not opened, not shared). It keeps its own clip, so in game it plays as made on each pop (other_pop).
        const bool locked = locked_locked(s, safe);
        std::string clip;
        {
            std::ifstream in(saved_clip(safe), std::ios::binary);
            if (!in) throw std::runtime_error(animation + "'s clip is missing.");
            clip.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        // "Kickflip high" copies to "Kickflip low", "Kickflip mid", "Kickflip high 2".
        std::string base = safe;
        for (const char* word : {" low", " mid", " high", " any"})
            if (base.size() > std::strlen(word) && base.ends_with(word)) { base.resize(base.size() - std::strlen(word)); break; }
        const std::string label = root.value("label", "");
        const auto folder = library_folder();
        // A trick opened from the game's tricks is copied onto the game's own clip of each pop, its keys moved onto that
        // clip's pop and landing: a Kickflip mid made from the low one played the low one's motion in the studio while the
        // game played its mid. A recording keeps its own clip in every copy.
        const auto anim = from_json(text);
        Clip source_clip = clip_from_bytes(std::vector<std::uint8_t>(clip.begin(), clip.end()));
        if (anim.source.kind == "native_asset") place_native_marks(source_clip);
        std::vector<std::string> copies;
        int on_game_clips = 0;
        for (const char* pop : {"low", "mid", "high"}) {
            const std::string suffix = std::string(" ") + pop;
            const auto stem = utf8_prefix(base, 48 - suffix.size()) + suffix;
            std::string name = stem;
            unsigned number = 2;
            while (std::filesystem::exists(folder / name) || std::filesystem::exists(folder / (name + ".json")))
                name = variation_name(stem, number++);
            std::filesystem::create_directories(folder / name);
            if (auto moved = locked ? std::nullopt : onto_game_pop(anim, source_clip, pop)) {
                auto& [copy, game_clip] = *moved;
                copy.name = name;
                copy.pop = pop;
                if (!label.empty()) copy.label = utf8_prefix(label, 48 - suffix.size()) + suffix;
                const auto bytes = clip_bytes(game_clip);
                write_preset_file(folder / name / (name + ".clip"), std::string(bytes.begin(), bytes.end()));
                write_preset_file(folder / name / (name + ".json"), to_json(copy));
                ++on_game_clips;
            } else {
                auto copy = root;
                copy["name"] = name;
                copy["pop"] = pop;
                if (!label.empty()) copy["label"] = (utf8_prefix(label, 48 - suffix.size()) + suffix);
                write_preset_file(folder / name / (name + ".clip"), clip);
                write_preset_file(folder / name / (name + ".json"), copy.dump(2));
            }
            copies.push_back(name);
        }
        read_library_locked(s);
        const int packs = locked ? 0 : add_copies_to_packs_locked(s, safe, copies); // not into its maker's pack
        s.tricks_read = false;
        message = "Made " + copies[0] + ", " + copies[1] + " and " + copies[2] + (packs ? ", in " + std::to_string(packs) + (packs == 1 ? " pack" : " packs") + " with it" : std::string{}) +
                  (locked ? ". Each plays as made on its own pop, and stays its maker's: locked, not shared."
                   : on_game_clips ? ". Each is on the game's own clip of its pop and plays on that pop." : ". Each plays on its own pop.");
        s.status = message;
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s, false); return false; }
}
bool make_stance_version(const std::string& animation, int version, std::string& made, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (version < 0 || version > 3) throw std::runtime_error("There is no such stance.");
        const auto safe = file_safe(animation);
        auto anim = from_json(saved_text(safe));
        if (anim.trick.empty() || anim.trick.starts_with("native:"))
            throw std::runtime_error(animation + " is a piece of the game's animation: it plays whichever way round the game plays it.");
        if (locked_locked(s, safe)) throw std::runtime_error(animation + " is locked to its maker: it can be played, not copied.");
        std::string bytes;
        {
            std::ifstream in(saved_clip(safe), std::ios::binary);
            if (!in) throw std::runtime_error(animation + "'s clip is missing.");
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        Clip clip = clip_from_bytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        if (anim.source.kind == "native_asset") place_native_marks(clip);
        const bool own_right = studio_goofy(s.stance_choice);
        const bool want_right = version >= 2 ? !own_right : own_right, want_fakie = version % 2 == 1;
        const bool lead = clip_leads_right(clip).value_or(leads_right(authored_goofy(anim), anim.switch_stance.value_or(false)));
        if (lead != want_right) {
            clip = mirror_clip(clip);
            const auto timing = anim.clip;
            anim.clip = {};
            anim = mirror_animation(anim);
            anim.clip = timing;
        }
        anim.switch_stance = authored_goofy(anim) != want_right;
        anim.fakie = want_fakie;
        anim.keep_leads_right = want_right;
        // "Kickflip mid" makes "Kickflip mid switch"; a version of a version is named from the trick itself.
        static constexpr std::array<const char*, 4> suffix{" normal", " fakie", " switch", " switch fakie"};
        std::string base = safe;
        for (const char* word : {" switch fakie", " switch", " fakie", " normal"})
            if (base.size() > std::strlen(word) && base.ends_with(word)) { base.resize(base.size() - std::strlen(word)); break; }
        const std::string end = suffix[static_cast<std::size_t>(version)];
        const auto stem = utf8_prefix(base, 48 - end.size()) + end;
        const auto folder = library_folder();
        std::string name = stem;
        unsigned number = 2;
        while (std::filesystem::exists(folder / name) || std::filesystem::exists(folder / (name + ".json"))) name = variation_name(stem, number++);
        anim.name = name;
        if (!anim.label.empty()) {
            std::string label = anim.label;
            for (const char* word : {" switch fakie", " switch", " fakie", " normal"})
                if (label.size() > std::strlen(word) && label.ends_with(word)) { label.resize(label.size() - std::strlen(word)); break; }
            anim.label = utf8_prefix(label, 48 - end.size()) + end;
        }
        anim.clip.times = {0, clip.duration()};
        anim.clip.takeoff = clip.takeoff;
        anim.clip.landing = clip.landing;
        anim.upgraded = current_upgrade;
        anim.board_nose_forward = true;
        std::filesystem::create_directories(folder / name);
        const auto out = clip_bytes(clip);
        write_preset_file(folder / name / (name + ".clip"), std::string(out.begin(), out.end()));
        write_preset_file(folder / name / (name + ".json"), to_json(anim));
        read_library_locked(s);
        const int packs = add_copies_to_packs_locked(s, safe, {name});
        s.tricks_read = false;
        static constexpr std::array<const char*, 4> said{"in your own stance", "fakie", "switch", "switch fakie"};
        made = name;
        message = "Made " + name + ", the version " + said[static_cast<std::size_t>(version)] +
                  (packs ? ", in " + std::to_string(packs) + (packs == 1 ? " pack" : " packs") + " with it" : std::string{}) + ". Change it as you like.";
        s.status = message;
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s, false); return false; }
}
bool set_pack_trick_on(const std::string& pack_name, const std::string& animation, bool on, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    message = shown_name(s, animation) + (on ? " plays from " + pack_name + " again." : " is off in " + pack_name + ": it stays in the pack, but does not play.");
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) {
        if (!pack.has(animation)) throw std::runtime_error(animation + " is not in " + pack_name + ".");
        if (on) pack.off.erase(animation);
        else pack.off.insert(animation);
    });
}
bool set_trick_play(const std::string& animation, bool on, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (!s.library_read) read_library_locked(s);
        if (std::find(s.library.begin(), s.library.end(), animation) == s.library.end()) throw std::runtime_error(animation + " is not saved.");
        auto playing = playing_names(s.library, !s.packs_on.empty());
        if (on) playing.insert(animation);
        else playing.erase(animation);
        // One trick of your own plays on each pop and stance (the newest saved of those on, beside the packs that are on): the
        // others on for the same go off, so the one switched on is the one that plays. (An older one switched on was said to
        // play and never did, its PLAY lit.) The playlist, when it is on, says what plays instead.
        std::vector<std::string> stood_down;
        if (on && !s.playlist_on)
            if (const auto me = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return p.name == animation; }); me != s.presets.end()) {
                const auto slot = preset_slot(me->trick, me->pop) + stance_part(*me);
                for (const auto& p : s.presets)
                    if (p.name != animation && playing.contains(p.name) && preset_slot(p.trick, p.pop) + stance_part(p) == slot) {
                        playing.erase(p.name);
                        stood_down.push_back(shown_name(s, p.name));
                    }
            }
        write_playing_names(playing);
        if (!on) {
            // Off is off: it does not play from its packs either.
            std::vector<std::string> in_packs;
            for (const auto& pack : s.packs)
                if (pack.has(animation) && !pack.off.contains(animation)) in_packs.push_back(pack.name);
            for (const auto& pack : in_packs) {
                std::string ignored;
                change_pack_locked(s, pack, ignored, [&](PresetPack& p) { p.off.insert(animation); });
            }
        }
        read_library_locked(s);
        s.tricks_read = false;
        message = shown_name(s, animation) + (on ? " is active: it plays in game." : " is not active: it does not play in game now.");
        if (!stood_down.empty()) {
            std::string names;
            for (std::size_t i = 0; i < stood_down.size(); ++i) names += (i == 0 ? "" : i + 1 == stood_down.size() ? " and " : ", ") + stood_down[i];
            message = shown_name(s, animation) + " is active; " + names + (stood_down.size() == 1 ? " is" : " are") +
                      " not any more (one trick plays on each pop and stance).";
        }
        s.status = message;
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) {
        message = e.what();
        s.status = message;
        publish_locked(s, false);
        return false;
    }
}
bool set_pack_editable(const std::string& pack_name, bool editable, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (const auto found = std::find_if(s.packs.begin(), s.packs.end(), [&](const PresetPack& p) { return p.name == pack_name; });
        found != s.packs.end() && locked_key(found->maker_key)) {
        message = pack_name + " was made by someone else: whether it can be edited is theirs to choose.";
        s.status = message;
        publish_locked(s, false);
        return false;
    }
    message = editable ? pack_name + " is open: once shared, others can open its tricks to edit and copy them."
                       : pack_name + " is locked: once shared, its tricks play for others but cannot be opened, copied or shared again.";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) { pack.editable = editable; });
}
void set_pack_favorite(const std::string& pack_name, bool favorite) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase(s.favorite_packs, pack_name);
    if (favorite) s.favorite_packs.insert(s.favorite_packs.begin(), pack_name);
    save_settings_locked(s);
    publish_locked(s, false);
}
namespace {
void playlists_changed_locked(State& s, const Playlists& playlists, const std::string& message) {
    write_playlists(playlists);
    s.tricks_read = false; // the game's side takes it up
    read_library_locked(s);
    s.status = message;
    publish_locked(s, false);
}
void say_locked(State& s, std::string& message, std::string said) {
    message = std::move(said);
    s.status = message;
    publish_locked(s, false);
}
} // namespace
void set_playlist_on(bool on, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    const auto* open = lists.named(s.playlist_open);
    if (!open) return say_locked(s, message, "Make a playlist first: + NEW PLAYLIST.");
    const auto before = lists.on;
    lists.on = on ? open->name : std::string{};
    message = on ? (open->entries.empty() ? open->name + " is on - it is empty, so nothing of yours plays until you add to it."
                                          : open->name + " is on: it decides what plays in game.") +
                       (before.empty() || before == open->name ? std::string{} : " " + before + " is off now: one playlist plays at a time.")
                 : open->name + " is off: no playlist is on, so nothing of yours plays.";
    playlists_changed_locked(s, lists, message);
}
void set_playlist_mode(PlaylistMode mode, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    auto* open = lists.named(s.playlist_open);
    if (!open) return;
    open->mode = mode;
    message = mode == PlaylistMode::fixed ? "Fixed: where two in the playlist are the same trick, the one higher up plays every time."
              : mode == PlaylistMode::turns ? "In turns: the same trick from the playlist plays one after another, top to bottom."
                                            : "Shuffle: the same trick from the playlist plays at random, never twice running.";
    playlists_changed_locked(s, lists, message);
}
bool playlist_add(const PlaylistEntry& entry, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    if (!lists.named(s.playlist_open)) { // none yet: Playlist 1, open
        s.playlist_open = free_playlist_name(lists);
        lists.lists.push_back({s.playlist_open, PlaylistMode::fixed, {}});
    }
    auto* open = lists.named(s.playlist_open);
    if (std::find(open->entries.begin(), open->entries.end(), entry) != open->entries.end()) {
        say_locked(s, message, "That is in " + open->name + " already.");
        return false;
    }
    open->entries.push_back(entry);
    message = (entry.animation.empty() ? "Added the pack " + entry.pack : "Added " + shown_name(s, entry.animation)) + " to " + open->name + ".";
    playlists_changed_locked(s, lists, message);
    return true;
}
void playlist_remove(std::size_t index) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    auto* open = lists.named(s.playlist_open);
    if (!open || index >= open->entries.size()) return;
    open->entries.erase(open->entries.begin() + static_cast<std::ptrdiff_t>(index));
    playlists_changed_locked(s, lists, "Taken out of " + open->name + ".");
}
void playlist_move(std::size_t index, int by) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    auto* open = lists.named(s.playlist_open);
    if (!open) return;
    const auto to = static_cast<std::ptrdiff_t>(index) + by;
    if (index >= open->entries.size() || to < 0 || to >= static_cast<std::ptrdiff_t>(open->entries.size())) return;
    std::swap(open->entries[index], open->entries[static_cast<std::size_t>(to)]);
    playlists_changed_locked(s, lists, open->name + ": order changed.");
}
void open_playlist(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.playlists.named(name)) return;
    s.playlist_open = name;
    publish_locked(s, false);
}
bool new_playlist(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    const auto name = free_playlist_name(lists);
    lists.lists.push_back({name, PlaylistMode::fixed, {}});
    s.playlist_open = name;
    playlists_changed_locked(s, lists, "Made " + name + ": add whole packs or single tricks to it, then switch it on.");
    message = s.status;
    return true;
}
bool rename_playlist(const std::string& wanted, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    auto* open = lists.named(s.playlist_open);
    if (!open) return false;
    auto to = utf8_prefix(wanted, 48);
    while (!to.empty() && to.back() == ' ') to.pop_back();
    while (!to.empty() && to.front() == ' ') to.erase(to.begin());
    if (to.empty()) {
        say_locked(s, message, "A playlist needs a name.");
        return false;
    }
    if (to == open->name) return true;
    if (lists.named(to)) {
        say_locked(s, message, "There is a playlist called " + to + " already.");
        return false;
    }
    if (lists.on == open->name) lists.on = to;
    message = "Renamed " + open->name + " to " + to + ".";
    open->name = to;
    s.playlist_open = to;
    playlists_changed_locked(s, lists, message);
    return true;
}
bool delete_playlist(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    const auto* open = lists.named(s.playlist_open);
    if (!open) return false;
    const auto name = open->name;
    const bool was_on = lists.on == name;
    std::erase_if(lists.lists, [&](const NamedPlaylist& l) { return l.name == name; });
    if (was_on) lists.on.clear();
    s.playlist_open = lists.lists.empty() ? std::string{} : lists.lists.front().name;
    message = "Deleted " + name + (was_on ? ": no playlist is on, so nothing of yours plays." : ".");
    playlists_changed_locked(s, lists, message);
    return true;
}
std::string add_to_playlist(const std::string& playlist, const PlaylistEntry& entry, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto lists = s.playlists;
    std::string name = playlist;
    if (name.empty()) {
        name = free_playlist_name(lists);
        lists.lists.push_back({name, PlaylistMode::fixed, {}});
    }
    auto* list = lists.named(name);
    if (!list) {
        say_locked(s, message, "There is no playlist called " + name + ".");
        return {};
    }
    if (std::find(list->entries.begin(), list->entries.end(), entry) == list->entries.end()) list->entries.push_back(entry);
    if (lists.on.empty()) lists.on = name;
    const auto what = entry.animation.empty() ? "the pack " + entry.pack : shown_name(s, entry.animation);
    message = "Added " + what + " to " + name +
              (lists.on == name ? ": it plays in game now." : ". " + name + " is off, " + lists.on + " is on: switch " + name + " on to play it.");
    s.playlist_open = name;
    playlists_changed_locked(s, lists, message);
    return name;
}
ActiveAnswer set_trick_active(const std::string& animation, bool on, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.library_read) read_library_locked(s);
    const auto me = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return p.name == animation; });
    if (me == s.presets.end()) {
        say_locked(s, message, animation + " is not saved.");
        return ActiveAnswer::done;
    }
    const auto shown = shown_name(s, animation);
    const auto pack_named = [&](const std::string& name) -> const PresetPack* {
        const auto found = std::find_if(s.packs.begin(), s.packs.end(), [&](const PresetPack& p) { return p.name == name; });
        return found == s.packs.end() ? nullptr : &*found;
    };
    auto lists = s.playlists;
    auto* playing = lists.named(lists.on);
    // Where it is in the playlist that is on: as itself, and in a whole pack there.
    bool single = false;
    std::vector<std::string> whole;
    if (playing)
        for (const auto& e : playing->entries) {
            if (e.animation == animation) single = true;
            else if (e.animation.empty())
                if (const auto* pack = pack_named(e.pack); pack && pack->has(animation)) whole.push_back(pack->name);
        }
    const auto switch_in_pack = [&](const std::string& pack, bool to) {
        std::string ignored;
        change_pack_locked(s, pack, ignored, [&](PresetPack& p) {
            if (to) p.off.erase(animation);
            else p.off.insert(animation);
        });
    };
    if (!on) {
        if (playing) {
            std::vector<std::string> said;
            if (single) {
                std::erase_if(playing->entries, [&](const PlaylistEntry& e) { return e.animation == animation; });
                write_playlists(lists);
                said.push_back("out of " + playing->name);
            }
            for (const auto& pack : whole) {
                switch_in_pack(pack, false);
                said.push_back("off in " + pack);
            }
            std::string words;
            for (std::size_t i = 0; i < said.size(); ++i) words += (i == 0 ? "" : " and ") + said[i];
            message = said.empty() ? shown + " is not in " + playing->name + "." : "Took " + shown + " " + words + ".";
        } else {
            std::vector<std::string> off;
            const auto packs_on_now = s.packs_on; // switching one re-reads them
            for (const auto& pack : packs_on_now)
                if (pack.has(animation) && !pack.off.contains(animation)) {
                    switch_in_pack(pack.name, false);
                    off.push_back(pack.name);
                }
            std::string words;
            for (std::size_t i = 0; i < off.size(); ++i) words += (i == 0 ? "" : i + 1 == off.size() ? " and " : ", ") + off[i];
            message = off.empty() ? shown + " does not play now." : "Switched " + shown + " off in " + words + " (it stays in it).";
        }
        s.tricks_read = false;
        read_library_locked(s);
        s.status = message;
        publish_locked(s, false);
        return ActiveAnswer::done;
    }
    // On: back where it was switched off; in a playlist already, beaten to its pop; or into a playlist (the overlay asks).
    if (playing) {
        bool back = false;
        for (const auto& pack : whole)
            if (const auto* p = pack_named(pack); p && p->off.contains(animation)) {
                switch_in_pack(pack, true);
                back = true;
            }
        if (back) {
            s.tricks_read = false;
            read_library_locked(s);
            say_locked(s, message, shown + " is on in " + whole.front() + " again, in " + playing->name + ".");
            return ActiveAnswer::done;
        }
        if (single || !whole.empty()) {
            say_locked(s, message, shown + " is in " + playing->name + ", but another of its pop and stance plays first: move it up there.");
            return ActiveAnswer::show_playlist;
        }
        return ActiveAnswer::choose_playlist;
    }
    const auto packs_on_now = s.packs_on; // switching one re-reads them
    for (const auto& pack : packs_on_now)
        if (pack.has(animation)) {
            if (pack.off.contains(animation)) {
                switch_in_pack(pack.name, true);
                s.tricks_read = false;
                read_library_locked(s);
                say_locked(s, message, shown + " plays from " + pack.name + " again.");
                return ActiveAnswer::done;
            }
            say_locked(s, message, shown + " is on in " + pack.name + ", but another of its pop and stance plays first.");
            return ActiveAnswer::done;
        }
    return ActiveAnswer::choose_playlist;
}
void set_hover_preview(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.hover_preview = on;
    save_settings_locked(s);
    publish_locked(s, false);
}
void set_key_bindings(const std::map<std::string, std::string>& keys) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.keys = keys;
    save_settings_locked(s);
    publish_locked(s, false);
}
std::map<std::string, std::string> key_bindings() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.keys;
}
namespace {
void remove_saved_files(const std::string& safe); // below
}
// Into the Recycle Bin, each path whole (a trick's folder, a loose file), so a pack deleted by mistake can be got back
// from there. False when the shell did not take them all.
bool to_recycle_bin(const std::vector<std::filesystem::path>& paths) {
    std::wstring list;
    for (const auto& path : paths) {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) continue;
        list += path.wstring();
        list.push_back(L'\0');
    }
    if (list.empty()) return true;
    list.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = list.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
}
bool delete_pack(const std::string& name, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (name != file_safe(name)) throw std::runtime_error("There is no pack called " + name + ".");
        if (!s.library_read) read_library_locked(s);
        // A pack you got (someone else's: from the MARKET or imported) takes its tricks with it - as saved and as changed
        // here - but not one that is in another pack too (they were left behind, loose under OTHER MAKERS' TRICKS). Your own
        // pack's tricks stay yours, in MY TRICKS: a tester deleted his own pack and lost every trick in it (2026-10-26).
        std::set<std::string> members;
        const auto saved = std::find_if(s.packs.begin(), s.packs.end(), [&](const PresetPack& p) { return p.name == name; });
        const bool someone_elses = saved != s.packs.end() && locked_key(saved->maker_key);
        if (someone_elses)
            for (const auto& m : saved->members()) members.insert(m);
        if (const auto draft = s.pack_drafts.find(name); someone_elses && draft != s.pack_drafts.end())
            for (const auto& m : draft->second.members()) members.insert(m);
        std::vector<std::string> tricks, kept;
        for (const auto& member : members) {
            if (std::find(s.library.begin(), s.library.end(), member) == s.library.end()) continue;
            const bool elsewhere = std::any_of(s.packs.begin(), s.packs.end(), [&](const PresetPack& p) { return p.name != name && p.has(member); }) ||
                                   std::any_of(s.pack_drafts.begin(), s.pack_drafts.end(), [&](const auto& d) { return d.first != name && d.second.has(member); });
            (elsewhere ? kept : tricks).push_back(member);
        }
        std::vector<std::filesystem::path> paths;
        const auto folder = library_folder();
        for (const auto& trick : tricks) {
            const auto safe = file_safe(trick);
            if (safe.empty()) continue;
            paths.push_back(folder / safe);
            paths.push_back(folder / (safe + ".json"));
            paths.push_back(folder / (safe + ".clip"));
        }
        recycle_pack_files(paths, to_recycle_bin);
        std::filesystem::remove(pack_file(name));
        std::error_code gone;
        std::filesystem::remove(pack_thumbnail_file(name), gone);
        auto names = pack_names_on();
        if (std::erase(names, name)) write_pack_names_on(names);
        s.pack_drafts.erase(name);
        if (s.open_pack == name) s.open_pack.clear();
        // Out of every playlist: the pack, and its tricks that are gone.
        auto lists = read_playlists();
        bool listed = false;
        for (auto& list : lists.lists)
            listed |= std::erase_if(list.entries, [&](const PlaylistEntry& e) {
                return e.pack == name || (!e.animation.empty() && std::find(tricks.begin(), tricks.end(), e.animation) != tricks.end());
            }) > 0;
        if (listed) write_playlists(lists);
        const auto count = [](std::size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
        message = "Deleted the pack " + name +
                  (tricks.empty() ? std::string(".")
                                  : " and its " + count(tricks.size(), "trick", "tricks") +
                                        " (in the Recycle Bin, to get back from there).") +
                  (kept.empty() ? std::string{} : " " + count(kept.size(), "trick stays", "tricks stay") + ": in another pack too.");
        packs_changed_locked(s, message);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s); return false; }
}
bool set_pack_thumbnail(const std::string& pack, const std::string& png, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (pack.empty() || pack != file_safe(pack) || !std::filesystem::exists(pack_file(pack))) throw std::runtime_error("There is no pack called " + pack + ".");
        if (png.empty()) {
            std::error_code gone;
            std::filesystem::remove(pack_thumbnail_file(pack), gone);
            message = pack + " has no picture now.";
        } else {
            if (png.size() > 4 * 1024 * 1024 || png.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) throw std::runtime_error("That picture could not be used.");
            write_preset_file(pack_thumbnail_file(pack), png);
            message = pack + " has its picture.";
        }
        s.pack_pictures_read = false;
        s.status = message;
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s, false); return false; }
}
bool rename_pack(const std::string& from, const std::string& to_in, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        const auto to = file_safe(to_in);
        if (from.empty() || from != file_safe(from) || !std::filesystem::exists(pack_file(from))) throw std::runtime_error("There is no pack called " + from + ".");
        if (to.empty()) throw std::runtime_error("Give the pack a name: letters, numbers, spaces, - and _.");
        if (reserved_pack_name(to)) throw std::runtime_error(to + " is a name the studio keeps for itself: choose another name.");
        if (to == from) { message = "It is called that already."; s.status = message; publish_locked(s, false); return true; }
        if (std::filesystem::exists(pack_file(to)) && _stricmp(to.c_str(), from.c_str()) != 0) throw std::runtime_error("There is a pack called " + to + " already.");
        if (const auto draft = s.pack_drafts.find(from); draft != s.pack_drafts.end()) {
            write_preset_file(pack_file(from), pack_json(draft->second).dump(2));
            s.pack_drafts.erase(draft);
        }
        auto pack = read_pack_file(from);
        pack.name = to;
        write_preset_file(pack_file(to), pack_json(pack).dump(2));
        if (_stricmp(to.c_str(), from.c_str()) != 0) std::filesystem::remove(pack_file(from));
        std::error_code error;
        if (std::filesystem::exists(pack_thumbnail_file(from), error)) std::filesystem::rename(pack_thumbnail_file(from), pack_thumbnail_file(to), error);
        auto names = pack_names_on();
        for (auto& name : names) if (name == from) name = to;
        write_pack_names_on(names);
        auto choices = read_conflict_choices();
        bool changed = false;
        for (auto& [slot, choice] : choices) if (choice == from) { choice = to; changed = true; }
        if (changed) {
            Json root = Json::object();
            for (const auto& [slot, choice] : choices) root[slot] = choice;
            write_preset_file(conflicts_file(), root.dump(2));
        }
        // The playlists and the stars go by the pack's name too: renamed with it, or it drops out of them.
        auto lists = read_playlists();
        bool listed = false;
        for (auto& list : lists.lists)
            for (auto& entry : list.entries)
                if (entry.pack == from) {
                    entry.pack = to;
                    listed = true;
                }
        if (listed) write_playlists(lists);
        bool starred = false;
        for (auto& name : s.favorite_packs)
            if (name == from) {
                name = to;
                starred = true;
            }
        if (starred) save_settings_locked(s);
        if (s.open_pack == from) s.open_pack = to;
        message = "Renamed " + from + " to " + to + ".";
        packs_changed_locked(s, message);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s); return false; }
}
bool set_pack_pop_rule(const std::string& pack_name, const std::string& rule_in, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const std::string rule = rule_in == "any" || rule_in == "low" || rule_in == "mid" || rule_in == "high" ? rule_in : std::string{};
    message = rule.empty() ? pack_name + ": each trick plays on the pop it was saved for." : rule == "any" ? pack_name + ": every trick plays on any pop."
                                                                                     : pack_name + ": every trick plays on " + rule + " pops only.";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) { pack.pop_rule = rule; });
}
bool set_conflict_choice(const std::string& slot, const std::string& pack, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        auto choices = read_conflict_choices();
        if (pack.empty()) choices.erase(slot); // not decided: the studio asks again
        else choices[slot] = pack;          // "*" at random, or the pack whose plays it
        Json root = Json::object();
        for (const auto& [key, choice] : choices) root[key] = choice;
        write_preset_file(conflicts_file(), root.dump(2));
        message = pack.empty() || pack == "*" ? "Those play at random, a different one each time." : pack + "'s plays it.";
        packs_changed_locked(s, message);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; publish_locked(s); return false; }
}
void set_pack_folder(bool exporting, const std::string& path_in) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::string path = path_in;
    while (!path.empty() && (path.back() == ' ' || path.back() == '"' || path.back() == '\\' || path.back() == '/')) path.pop_back();
    while (!path.empty() && (path.front() == ' ' || path.front() == '"')) path.erase(path.begin());
    if (path.size() == 2 && path[1] == ':') path += '\\'; // "D:" alone is the current folder on D, not its root
    (exporting ? s.export_folder : s.import_folder) = path;
    save_settings_locked(s);
    s.status = path.empty() ? std::string(exporting ? "Packs are exported to ReSkate.Tricks\\exports." : "Packs are imported from ReSkate.Tricks\\import.")
                            : std::string(exporting ? "Packs are exported to " : "Packs are imported from ") + path + ".";
    publish_locked(s, false);
}
void open_pack_folder(bool exporting) {
    std::filesystem::path folder;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        folder = exporting ? pack_export_folder(s) : pack_import_folder(s);
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
bool test_in_game(const std::string& animation, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto name = file_safe(animation);
    if (name.empty() || !std::filesystem::exists(saved_json(name))) {
        message = "Save it first, then test it in game.";
        s.status = message;
        publish_locked(s, false);
        return false;
    }
    s.test_trick = name;
    s.play_in_game = true;
    s.tricks_read = false;
    s.open_requested = false; // the studio closes: go and skate it
    s.open_request_pending = true;
    message = "Testing " + name + " in game until you open the studio again (F8) or close the game: do the trick. ACTIVE puts it in a playlist, for good.";
    s.status = message;
    publish_locked(s, false);
    return true;
}
void clear_test_returned() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.test_returned.clear();
    publish_locked(s, false);
}
// ---- sharing packs --------------------------------------------------------------------------------
// A pack goes out as one zip laid out exactly like the tricks folder - presets/<pack>.json and each
// animation's own folder with its .json and .clip - so it can be unzipped straight into ReSkate.Tricks, or
// put in ReSkate.Tricks/import and taken in with Import. Animations from older builds are upgraded as they
// arrive (upgrade_saved), so a tester's pack works as it is.
namespace {
std::string read_whole(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("could not read " + path.filename().string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void write_whole(const std::filesystem::path& path, const std::string& data) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) throw std::runtime_error("could not write " + path.filename().string());
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::rename(temporary, path);
}
const char* share_readme =
    "ReSkate Animator Studio pack\r\n\r\n"
    "Either unzip this into the ReSkate.Tricks folder next to the game (it is laid out like that folder),\r\n"
    "or put the zip itself in ReSkate.Tricks\\import and choose Import packs in the studio (F8, PACKS).\r\n"
    "Animations made in older builds are brought up to date when they are first read.\r\n";
} // namespace

namespace {
// A pack's files as they are shared (export_pack, share_pack): `pack_name` "" for every saved animation.
struct Shared {
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<std::string> tricks; // a line each: "Kickflip (mid pop, switch)"
    std::string title, saved_first;
    PresetPack pack;
    std::size_t animations{};
};
Shared shared_files_locked(State& s, const std::string& pack_name) {
    if (!s.library_read) read_library_locked(s);
    Shared out;
    std::vector<std::string> names;
    bool editable = false; // whether the pack lets others edit its tricks
    auto& files = out.files;
    auto& title = out.title;
    auto& saved_first = out.saved_first;
    {
        if (!pack_name.empty()) {
            if (pack_name != file_safe(pack_name)) throw std::runtime_error("There is no pack called " + pack_name + ".");
            // What is shared is what you see: changes not saved yet are saved first.
            if (const auto draft = s.pack_drafts.find(pack_name); draft != s.pack_drafts.end()) {
                write_preset_file(pack_file(pack_name), pack_json(draft->second).dump(2));
                s.pack_drafts.erase(draft);
                s.tricks_read = false;
                saved_first = "Saved and exported ";
            }
            auto pack = read_pack_file(pack_name);
            // Someone else's pack (made on another PC: got from the MARKET or imported) stays theirs to share: not as a zip,
            // a Thunderstore package or a publish, whichever way it is asked for.
            if (locked_key(pack.maker_key))
                throw std::runtime_error("Woaahh, wait there: " + pack.name + " is someone else's pack. It plays for you, but it stays theirs to share.");
            out.pack = pack;
            title = pack.name;
            // The pack goes with its maker's key, and every trick in it on (what is switched off here is this PC's choice).
            auto shared = pack;
            editable = pack.editable;
            if (shared.maker_key.empty()) shared.maker_key = device_key();
            shared.off.clear();
            files.emplace_back("presets/" + title + ".json", pack_json(shared).dump(2));
            std::error_code picture_error;
            if (std::filesystem::is_regular_file(pack_thumbnail_file(title), picture_error))
                files.emplace_back("presets/" + title + ".png", read_whole(pack_thumbnail_file(title)));
            names = pack.members();
        } else {
            title = "All saved animations";
            names = s.library;
        }
        std::size_t animations = 0, skipped = 0;
        for (const auto& name : names) {
            const auto json = saved_json(name), clip = saved_clip(name);
            std::error_code error;
            if (!std::filesystem::is_regular_file(json, error) || !std::filesystem::is_regular_file(clip, error)) continue;
            // Another maker's trick is not shared again (editable or not, or one that came in a pack): a pack with one is not
            // exported, EXPORT ALL leaves it out.
            if (someone_elses_locked(s, name)) {
                if (!pack_name.empty())
                    throw std::runtime_error(name + " in this pack was made by someone else: it plays for you, but it is theirs to share. "
                                             "Take it out of the pack to export it.");
                ++skipped;
                continue;
            }
            // Made here: it goes with this PC's key, so it stays its maker's wherever it is installed.
            auto text = read_whole(json);
            try {
                auto root = Json::parse(text);
                if (root.is_object()) {
                    if (root.value("maker_key", "").empty()) root["maker_key"] = device_key();
                    // Locked or open to edit, as the pack is set (EXPORT ALL: locked).
                    if (editable) root["editable"] = true;
                    else root.erase("editable");
                    text = root.dump(2);
                }
            } catch (...) {}
            files.emplace_back(name + "/" + name + ".json", std::move(text));
            files.emplace_back(name + "/" + name + ".clip", read_whole(clip));
            ++animations;
            // What it is, for whoever looks at the pack: its trick, its pop, its stance.
            if (const auto p = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& x) { return x.name == name; }); p != s.presets.end()) {
                static constexpr std::array<const char*, 4> stances{"", ", fakie", ", switch", ", switch fakie"};
                const auto pop = pack_name.empty() ? p->pop : effective_pop(out.pack, p->pop);
                std::string line = shown_name(s, name) + " (" + (pop.empty() ? std::string("any pop") : pop + " pop") +
                                   stances[static_cast<std::size_t>(stance_version(*p, studio_goofy(s.stance_choice)))] + ")";
                out.tricks.push_back(std::move(line));
            }
        }
        out.animations = animations;
        if (!animations) throw std::runtime_error(skipped ? "Everything here was made by someone else: it is theirs to share." : "There is nothing saved in this pack to export.");
    }
    return out;
}
} // namespace
bool import_zips_locked(State& s, const std::vector<std::filesystem::path>& zips, bool move_done, std::string& message,
                        std::vector<std::string>* made_packs = nullptr, const std::string& replace = {}); // below
bool share_pack(const std::string& pack_name, SharedPack& out, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (pack_name.empty()) throw std::runtime_error("Choose a pack to share.");
        auto shared = shared_files_locked(s, pack_name);
        out.name = shared.title;
        out.made_by = shared.pack.made_by.empty() ? s.your_name : shared.pack.made_by;
        out.description = shared.pack.description;
        out.version = shared.pack.version;
        out.published = shared.pack.published;
        out.published_as = shared.pack.published_as;
        out.files = std::move(shared.files);
        out.tricks = std::move(shared.tricks);
        std::error_code error;
        if (std::filesystem::is_regular_file(pack_thumbnail_file(out.name), error)) out.picture = read_whole(pack_thumbnail_file(out.name));
        if (!shared.saved_first.empty()) read_library_locked(s);
        publish_locked(s, false);
        return true;
    } catch (const std::exception& e) {
        message = e.what();
        return false;
    }
}
void pack_published(const std::string& pack_name, const std::string& version, const std::string& published_as) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::string message = pack_name + " " + version + " is on Thunderstore.";
    (void)change_pack_locked(s, pack_name, message, [&](PresetPack& pack) {
        pack.version = version;
        pack.published = version;
        if (!published_as.empty()) pack.published_as = published_as;
    });
}
bool set_pack_cover_design(const std::string& pack_name, const std::string& design, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    message = "Cover saved.";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) { pack.cover = utf8_prefix(design, 4000); });
}
bool set_pack_description(const std::string& pack_name, const std::string& description, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    message = "Description saved.";
    return change_pack_locked(s, pack_name, message, [&](PresetPack& pack) { pack.description = utf8_prefix(description, 2000); });
}
bool import_pack_zip(const std::filesystem::path& zip, std::string& message, std::vector<std::string>* made, const std::string& replace) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return import_zips_locked(s, {zip}, false, message, made, replace);
}
std::filesystem::path studio_folder() { return library_folder(); }
std::filesystem::path pack_export_folder() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return pack_export_folder(s);
}
bool export_pack(const std::string& pack_name, std::string& message) {
    auto& s = state();
    std::unique_lock lock(s.mutex);
    try {
        auto shared = shared_files_locked(s, pack_name);
        auto& files = shared.files;
        const auto& title = shared.title;
        const auto& saved_first = shared.saved_first;
        const auto animations = shared.animations;
        files.emplace_back("README.txt", share_readme);
        const auto out_folder = pack_export_folder(s);
        const auto zip = out_folder / (file_safe(title) + ".zip");
        write_whole(zip, stored_zip(files));
        message = std::format("{}{} ({} animation{}) to {}.", saved_first.empty() ? "Exported " : saved_first, title,
                              animations, animations == 1 ? "" : "s", utf8_path(zip));
        if (!saved_first.empty()) read_library_locked(s);
        s.status = message;
        logging::log(Level::info, Channel::skater, "Animation Studio: {}", message);
        publish_locked(s);
        // There it is. Explorer can take a while to answer (shell extensions, a network folder): not with the studio held.
        lock.unlock();
        ShellExecuteW(nullptr, L"open", out_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    } catch (const std::exception& e) {
        message = std::string("Export failed: ") + e.what();
        s.status = message;
        publish_locked(s);
        return false;
    }
}

bool import_zips_locked(State& s, const std::vector<std::filesystem::path>& zips, bool move_done, std::string& message,
                        std::vector<std::string>* made_packs, const std::string& replace);
bool import_packs(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto folder = library_folder();
    const auto inbox = pack_import_folder(s);
    std::error_code error;
    std::filesystem::create_directories(inbox, error);
    std::vector<std::filesystem::path> zips;
    for (const auto& entry : std::filesystem::directory_iterator(inbox, error))
        if (entry.is_regular_file(error) && entry.path().extension() == L".zip") zips.push_back(entry.path());
    if (zips.empty()) {
        message = "Nothing to import: put a pack's .zip in " + utf8_path(inbox) + " first.";
        s.status = message;
        publish_locked(s);
        return false;
    }
    return import_zips_locked(s, zips, true, message);
}
// Takes in `zips` (moved into the inbox's imported folder after, with `move_done`): every animation, pack and picture in
// them checked first, names already taken given the next variation. A Thunderstore package's own files (manifest.json,
// reskate-animations.json, icon.png, README.md) are passed over.
namespace {
// A saved trick's files, both ways it can be saved (its own folder, and loose from before there were folders), and its
// folder once it is empty.
void remove_saved_files(const std::string& safe) {
    if (safe.empty()) return;
    const auto folder = library_folder();
    std::error_code error;
    for (const auto& path : {folder / safe / (safe + ".json"), folder / safe / (safe + ".clip"), folder / (safe + ".json"), folder / (safe + ".clip")})
        std::filesystem::remove(path, error);
    std::filesystem::remove(folder / safe, error);
}
// Names compared as Windows compares file names: "Kickflip" and "kickflip" are one folder.
struct NoCase {
    bool operator()(const std::string& a, const std::string& b) const { return _stricmp(a.c_str(), b.c_str()) < 0; }
};
// What the studio takes out of a pack's zip: its animations (a .json and a .clip each, by name), its packs
// (presets/<pack>.json) and their pictures (presets/<pack>.png). Only the names the studio itself makes, nothing over its
// size, and a Thunderstore package's own files passed over. Nothing is checked beyond that here: the caller checks it all
// before it writes any of it.
struct ZipContents {
    std::map<std::string, std::pair<std::string, std::string>, NoCase> found; // animation -> json, clip
    std::vector<std::pair<std::string, std::string>> pack_files;              // pack name -> json
    std::map<std::string, std::string> pack_pictures;                         // pack name -> png
};
ZipContents read_pack_zip(const std::filesystem::path& path) {
    std::error_code error;
    // Read whole: a pack is a few MB, so anything far bigger is not one.
    if (const auto size = std::filesystem::file_size(path, error); !error && size > 512ull * 1024 * 1024) throw std::runtime_error("it is too big to be a pack");
    const auto data = read_whole(path);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) throw std::runtime_error("not a zip file");
    struct Closer { mz_zip_archive* z; ~Closer() { mz_zip_reader_end(z); } } closer{&zip};
    ZipContents out;
    const auto count = mz_zip_reader_get_num_files(&zip);
    std::uint64_t unpacked = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory) continue;
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        const auto slash = name.rfind('/');
        const std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
        const std::string dir = slash == std::string::npos ? "" : name.substr(0, slash);
        const auto dot = file.rfind('.');
        if (dot == std::string::npos || stat.m_uncomp_size > 64ull * 1024 * 1024) continue;
        const std::string stem = file.substr(0, dot), ext = file.substr(dot);
        if (ext != ".json" && ext != ".clip" && ext != ".png") continue;
        if (dir.empty() && (stem == "manifest" || stem == "reskate-animations")) continue; // a Thunderstore package's own
        if (ext == ".png" && stat.m_uncomp_size > 4ull * 1024 * 1024) continue;
        if (stem != file_safe(stem)) continue; // only the names the studio itself makes
        if ((unpacked += stat.m_uncomp_size) > 1024ull * 1024 * 1024) throw std::runtime_error("it unpacks to more than 1 GB");
        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!bytes) throw std::runtime_error("could not unpack " + name);
        std::string content(static_cast<const char*>(bytes), size);
        mz_free(bytes);
        const auto last_dir = dir.substr(dir.rfind('/') == std::string::npos ? 0 : dir.rfind('/') + 1);
        if (ext == ".png") {
            if (last_dir == "presets" && content.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) out.pack_pictures[stem] = std::move(content);
            continue;
        }
        if (last_dir == "presets" && ext == ".json") out.pack_files.emplace_back(stem, std::move(content));
        else if (ext == ".json") out.found[stem].first = std::move(content);
        else out.found[stem].second = std::move(content);
    }
    return out;
}
} // namespace
bool import_zips_locked(State& s, const std::vector<std::filesystem::path>& zips, bool move_done, std::string& message,
                        std::vector<std::string>* made_packs, const std::string& replace) {
    const auto folder = library_folder();
    const auto inbox = pack_import_folder(s);
    if (!s.library_read) read_library_locked(s);
    std::error_code error;
    std::size_t animations = 0, packs = 0, renamed = 0;
    std::vector<std::string> problems;
    for (const auto& path : zips) {
        try {
            // Read it all first (under the studio's lock); nothing is written unless every file in it checks out.
            auto zipped = read_pack_zip(path);
            auto& found = zipped.found;
            auto& pack_files = zipped.pack_files;
            auto& pack_pictures = zipped.pack_pictures;
            // Check every animation, and every pack, before writing any: a zip that fails leaves nothing of itself behind.
            for (const auto& [pack_name, text] : pack_files) (void)read_pack_json(Json::parse(text));
            for (const auto& [name, pair] : found) {
                if (pair.first.empty() || pair.second.empty()) throw std::runtime_error(name + " is missing its .json or .clip");
                (void)from_json(pair.first);
                const std::vector<std::uint8_t> clip(pair.second.begin(), pair.second.end());
                if (clip_from_bytes(clip).joints != skeleton_joints) throw std::runtime_error(name + " is for another skeleton");
            }
            PackFileTransaction transaction(folder);
            std::set<std::string> removed_members;
            std::vector<std::string> new_packs;
            std::size_t imported = 0, renamed_now = 0;
            // Queue obsolete imported members for removal. Their names are reusable, but their files stay installed
            // until the complete replacement and its ownership metadata are ready to commit.
            std::optional<PresetPack> replaced;
            if (!replace.empty() && replace == file_safe(replace) && pack_files.size() == 1 && std::filesystem::exists(pack_file(replace), error)) {
                replaced = s.pack_drafts.contains(replace) ? s.pack_drafts.at(replace) : read_pack_file(replace);
                std::set<std::string> elsewhere;
                for (const auto& other : s.packs)
                    if (other.name != replace)
                        for (const auto& name : other.members()) elsewhere.insert(name);
                for (const auto& name : replaced->members()) {
                    if (elsewhere.contains(name)) continue;
                    try {
                        if (!locked_key(from_json(read_whole(saved_json(name))).maker_key)) continue; // made here: yours
                    } catch (...) { continue; }
                    const auto safe = file_safe(name);
                    for (const auto& file : {folder / safe / (safe + ".json"), folder / safe / (safe + ".clip"),
                                             folder / (safe + ".json"), folder / (safe + ".clip")}) transaction.remove(file);
                    removed_members.insert(name);
                }
            }
            // A name already taken by a different animation gets the next free variation, and the pack
            // that names it is told.
            // A name is taken by a saved animation, by one already given out in this zip, or by another of
            // the zip's own animations (its "Kickflip 2" is not free for a renamed "Kickflip").
            std::map<std::string, std::string, NoCase> rename;
            std::set<std::string, NoCase> claimed;
            for (const auto& [name, pair] : found) {
                std::string target = name;
                for (unsigned n = 2;; ++n) {
                    const bool zip_own = target != name && found.contains(target);
                    if (!claimed.contains(target) && !zip_own && !reserved_name(target)) {
                        const auto json = saved_json(target), clip = saved_clip(target);
                        if (transaction.removing(json) || !std::filesystem::exists(json, error)) break;
                        if (read_whole(json) == pair.first && std::filesystem::exists(clip, error) && read_whole(clip) == pair.second) break; // already here
                    }
                    target = variation_name(name, n);
                }
                claimed.insert(target);
                rename[name] = target;
            }
            // Whose the zip's tricks are: each its own maker's key, or its pack's for one that has none.
            std::string zip_key;
            for (const auto& [pack_name, text] : pack_files)
                try {
                    if (const auto key = read_pack_json(Json::parse(text)).maker_key; !key.empty()) { zip_key = key; break; }
                } catch (...) {}
            std::map<std::string, std::string> made_elsewhere;
            for (const auto& [name, pair] : found) {
                const auto& target = rename[name];
                if (target != name) ++renamed_now;
                auto json = pair.first;
                auto animation = from_json(json);
                const bool keyless = animation.maker_key.empty() && !zip_key.empty();
                if (keyless) animation.maker_key = zip_key;
                if (target != name || keyless) {
                    animation.name = target;
                    animation.clip.times = {0, clip_from_bytes(std::vector<std::uint8_t>(pair.second.begin(), pair.second.end())).duration()};
                    json = to_json(animation);
                }
                if (locked_key(animation.maker_key) && !animation.editable) made_elsewhere[sha256_hex(pair.second)] = animation.maker_key;
                transaction.write(folder / target / (target + ".json"), json);
                transaction.write(folder / target / (target + ".clip"), pair.second);
                ++imported;
            }
            remember_locked_clips(made_elsewhere, &transaction);
            for (auto& [pack_name, text] : pack_files) {
                auto pack = read_pack_json(Json::parse(text));
                pack.published.clear(); // its maker's, not this PC's
                pack.published_as.clear();
                // Renamed everywhere the pack names it: in its slots, and among the tricks left out of a cycle.
                const auto new_name = [&](const std::string& name) {
                    const auto r = rename.find(name);
                    return r != rename.end() ? r->second : name;
                };
                for (auto& [slot, list] : pack.slots)
                    for (auto& name : list) name = new_name(name);
                std::set<std::string> unlinked;
                for (const auto& name : pack.unlinked) unlinked.insert(new_name(name));
                pack.unlinked = std::move(unlinked);
                std::set<std::string> off;
                for (const auto& name : pack.off) off.insert(new_name(name));
                pack.off = std::move(off);
                std::string target = file_safe(pack_name);
                if (replaced) {
                    // In place of the old version, under its name: on or off as it was, what was switched off here
                    // still off, and the tricks you put in it yourself still in it.
                    target = replace;
                    for (const auto& name : replaced->off)
                        if (pack.has(name)) pack.off.insert(name);
                    for (const auto& [slot, names] : replaced->slots)
                        for (const auto& name : names) {
                            std::error_code missing;
                            if (!pack.has(name) && !removed_members.contains(name) && std::filesystem::exists(saved_json(name), missing)) pack.slots[slot].push_back(name);
                        }
                } else {
                    if (reserved_pack_name(target)) target = variation_name(target, 2);
                    for (unsigned n = 2; std::filesystem::exists(folder / L"presets" / (target + ".json"), error); ++n) {
                        if (read_whole(folder / L"presets" / (target + ".json")) == pack_json(pack).dump(2)) break;
                        target = variation_name(file_safe(pack_name), n);
                    }
                }
                pack.name = target;
                new_packs.push_back(target);
                transaction.write(folder / L"presets" / (target + ".json"), pack_json(pack).dump(2));
                if (const auto picture = pack_pictures.find(pack_name); picture != pack_pictures.end())
                    transaction.write(pack_thumbnail_file(target), picture->second);
            }
            transaction.commit();
            if (replaced) s.pack_drafts.erase(replace);
            if (made_packs) made_packs->insert(made_packs->end(), new_packs.begin(), new_packs.end());
            animations += imported;
            packs += new_packs.size();
            renamed += renamed_now;
            // Upgrade only committed files; a failed update never edits the installed version.
            for (const auto& [original, target] : rename) upgrade_saved(folder / target / (target + ".json"));
            // Out of the way, so the next Import does not take it in again.
            if (move_done) {
                std::filesystem::create_directories(inbox / L"imported", error);
                std::filesystem::rename(path, inbox / L"imported" / path.filename(), error);
                if (error) std::filesystem::remove(path, error);
            }
        } catch (const std::exception& e) {
            problems.push_back(path.filename().string() + ": " + e.what());
        }
    }
    s.tricks_read = false;
    read_library_locked(s);
    message = std::format("Imported {} animation{} and {} pack{}{}.", animations, animations == 1 ? "" : "s", packs, packs == 1 ? "" : "s",
                          renamed ? std::format(" ({} renamed: the name was taken)", renamed) : std::string{});
    if (packs) message += " Add a pack to a playlist in PACKS to play it.";
    if (!problems.empty()) message += " Not imported: " + problems.front() + (problems.size() > 1 ? std::format(" and {} more", problems.size() - 1) : "");
    s.status = message;
    logging::log(Level::info, Channel::skater, "Animation Studio: {}", message);
    publish_locked(s);
    return problems.empty();
}
bool unpack_preview(const std::filesystem::path& zip, const std::filesystem::path& into, std::vector<UnpackedTrick>& tricks, std::string& message) {
    tricks.clear();
    try {
        const auto zipped = read_pack_zip(zip);
        // Checked as the import checks them. The pack's own file says the order and the pops; one that does not read is
        // passed over (its tricks still play here).
        std::optional<PresetPack> pack;
        for (const auto& [pack_name, text] : zipped.pack_files)
            try {
                pack = read_pack_json(Json::parse(text));
                break;
            } catch (...) {}
        std::vector<std::pair<std::string, Animation>> checked;
        for (const auto& [name, pair] : zipped.found) {
            if (pair.first.empty() || pair.second.empty()) throw std::runtime_error(name + " is missing its .json or .clip");
            auto animation = from_json(pair.first);
            const std::vector<std::uint8_t> clip(pair.second.begin(), pair.second.end());
            if (clip_from_bytes(clip).joints != skeleton_joints) throw std::runtime_error(name + " is for another skeleton");
            checked.emplace_back(name, std::move(animation));
        }
        if (checked.empty()) throw std::runtime_error("there are no tricks in it");
        // In the pack's own order (the order its README lists them in), then any it does not name.
        if (pack) {
            const auto order = pack->members();
            const auto rank = [&](const std::string& name) {
                for (std::size_t i = 0; i < order.size(); ++i)
                    if (_stricmp(order[i].c_str(), name.c_str()) == 0) return i;
                return order.size();
            };
            std::stable_sort(checked.begin(), checked.end(), [&](const auto& a, const auto& b) { return rank(a.first) < rank(b.first); });
        }
        std::error_code error;
        std::filesystem::remove_all(into, error);
        std::filesystem::create_directories(into);
        for (const auto& [name, animation] : checked) {
            const auto& pair = zipped.found.at(name);
            UnpackedTrick out;
            out.name = name;
            out.label = animation.label.empty() ? name : animation.label;
            out.trick = animation.trick;
            out.pop = pack ? effective_pop(*pack, animation.pop) : animation.pop;
            out.switch_stance = animation.switch_stance.value_or(false);
            out.fakie = animation.fakie.value_or(false);
            out.json = into / (name + ".json");
            out.clip = into / (name + ".clip");
            write_whole(out.json, pair.first);
            write_whole(out.clip, pair.second);
            tricks.push_back(std::move(out));
        }
        return true;
    } catch (const std::exception& e) {
        tricks.clear();
        message = e.what();
        return false;
    }
}
bool save(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.previewing) { message = "This is a preview: open the trick itself to change it."; return false; }
    if (!s.clip || s.clip->empty()) {
        message = "Open a trick in TRICKS before saving.";
        return false;
    }
    // A piece of the game's own animation - a setup, a loop, an exit, a transition - has no trick to be named
    // after, and asking for one stopped it being saved at all. It is kept by the clip it came from
    // (source.asset) instead: saved, opened again, shared, and played in game once the studio can see that
    // clip playing (arm_live_probe is the search for where to see it).
    const bool native_piece = s.edits.source.kind == "native_asset" && !s.edits.source.asset.empty();
    if (s.edits.trick.empty() && s.clip->takeoff >= 0 && !native_piece) {
        message = "The game did not say which trick this was, so it could never play. Open the game's own trick in TRICKS instead.";
        s.status = message;
        return false;
    }
    if (s.edits.name.empty()) {
        // A new animation gets a name of its own, never one already in the library.
        // Nor one of the tricks folder's own folders: a trick saved as "presets" would be written into the
        // presets folder and then listed as a trick.
        const auto taken = [&](const std::string& n) {
            const auto safe = file_safe(n);
            if (reserved_name(safe)) return true;
            return std::find(s.library.begin(), s.library.end(), safe) != s.library.end();
        };
        std::string name = s.edits.trick.empty() ? "Trick" : s.edits.trick;
        int n = 1;
        while (taken(variation_name(file_safe(name), static_cast<unsigned>(n)))) ++n;
        s.edits.name = variation_name(file_safe(name), static_cast<unsigned>(n));
    }
    // A typed name that is one of the tricks folder's own folders is moved aside, for the same reason.
    auto name = file_safe(s.edits.name);
    if (reserved_name(name)) name = variation_name(name, 2);
    // Nor is another saved animation written over: a recording named after its trick, a game trick opened
    // under its own name, or a typed name, all of which can be a name already used. Only the animation this
    // one was opened from or last saved to is saved over.
    const auto in_library = [&](const std::string& n) {
        return std::any_of(s.library.begin(), s.library.end(), [&](const std::string& o) { return _stricmp(o.c_str(), n.c_str()) == 0; });
    };
    const bool own_file = !s.saved_as.empty() && _stricmp(name.c_str(), s.saved_as.c_str()) == 0;
    const auto wanted = name;
    if (!own_file && in_library(name)) {
        unsigned n = 2;
        while (in_library(variation_name(wanted, n))) ++n;
        name = variation_name(wanted, n);
    }
    const bool first_save = !in_library(name);
    const auto folder = library_folder();
    try {
        std::filesystem::create_directories(folder);
        Animation full = s.edits;
        full.speed = 1; // the studio's watching speed is not the trick's: it is saved at x1 and plays at x1
        full.name = name;
        // A stance version is saved on the foot it keeps (keep_leads_right), whichever way round it is open.
        std::shared_ptr<const Clip> written = s.clip;
        bool mirror_back = false;
        if (full.keep_leads_right)
            if (const auto lead = clip_leads_right(*s.clip)) mirror_back = *lead != *full.keep_leads_right;
        if (mirror_back) {
            written = std::make_shared<const Clip>(mirror_clip(*s.clip));
            const auto timing = full.clip;
            full.clip = {};
            full = mirror_animation(full);
            full.clip = timing;
            full.name = name;
        }
        // Its stance label is the foot its clip leads with, so packs and "the one saved last plays" treat two
        // recordings on the same foot as the same stance.
        if (const auto lead = clip_leads_right(*written)) {
            const bool goofy = authored_goofy(full);
            if (leads_right(goofy, full.switch_stance.value_or(false)) != *lead) full.switch_stance = goofy != *lead;
        }
        if (full.maker_key.empty()) full.maker_key = device_key(); // made here: it stays this PC's maker's
        full.upgraded = current_upgrade;
        full.board_nose_forward = true;
        full.clip.times = {0, written->duration()};
        full.clip.takeoff = written->takeoff;
        full.clip.landing = written->landing;
        const auto json = to_json(full);
        const auto bytes = clip_bytes(*written);
        // Both are written in full beside the old files first, and only then moved over them, the trick before
        // its description: a failed write (a full disk) leaves the saved animation as it was.
        std::filesystem::create_directories(folder / name);
        const auto json_path = folder / name / (name + ".json"), clip_path = folder / name / (name + ".clip");
        const auto staged = [](std::filesystem::path p) { p += L".saving"; return p; };
        for (const auto& [path, data, size] : {std::tuple{clip_path, static_cast<const void*>(bytes.data()), bytes.size()},
                                               std::tuple{json_path, static_cast<const void*>(json.data()), json.size()}}) {
            std::ofstream out(staged(path), std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            out.close();
            if (!out) throw std::runtime_error("could not write " + staged(path).string());
        }
        std::filesystem::rename(staged(clip_path), clip_path);
        std::filesystem::rename(staged(json_path), json_path);
        s.edits.name = name;
        s.saved_as = name;
        s.dirty = false;
        s.tricks_read = false; // the next tick reads the custom tricks again
        read_library_locked(s);
        message = name == wanted ? "Saved " + name + "." : "Saved as " + name + ": " + wanted + " is another saved animation.";
        // A first save says how it gets to play: ACTIVE puts it in a playlist (the card asks which).
        if (first_save) message += " Tick ACTIVE to put it in a playlist and play it in game.";
        s.status = message;
        logging::log(Level::info, Channel::skater, "Animation Studio: saved {} ({} keys) to {}.", name, s.edits.key_count(), folder.string());
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        std::error_code ignored;
        for (const char* extension : {".json.saving", ".clip.saving"}) std::filesystem::remove(folder / name / (name + extension), ignored);
        message = std::string("Saving failed: ") + e.what();
        s.status = message;
        return false;
    }
}
namespace {
std::vector<GameTrick> read_catalog() {
    std::vector<GameTrick> out;
    try {
        const auto indexed = native_assets();
        std::unordered_map<std::string_view, const NativeAsset*> named;
        for (const auto& asset : *indexed) named.emplace(asset.asset, &asset);
        std::ifstream in(library_folder() / L"library" / L"catalog.json", std::ios::binary);
        if (!in) return out;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto json = Json::parse(text);
        if (!json.is_array()) return out;
        for (std::size_t i = 0; i < json.size() && i < 10000; ++i) {
            const auto& e = json.at(i);
            if (!e.is_object()) continue;
            GameTrick t{e.value("category", ""), e.value("label", ""), e.value("trick", ""), e.value("pop", ""), e.value("file", ""), e.value("asset", ""), e.value("preview_only", false)};
            t.position_codec = e.value("position_codec", "");
            t.looping = e.value("native_looping", false);
            if (!t.preview_only) t.trick = trick_key(t.trick); // a library built with the older names matches the game
            if (t.preview_only && t.asset.starts_with("animation/dingo/")) {
                const auto found = named.find(t.asset);
                t.label = found != named.end() ? found->second->label : native_asset_name(t.asset).label;
                t.category = native_asset_folder(t.asset, "ClipControllerAsset", t.category);
            }
            const std::filesystem::path relative_file(t.file);
            if (t.trick.empty() || t.file.empty() || t.file.find("..") != std::string::npos ||
                relative_file.has_root_path() || relative_file.extension() != L".clip") continue;
            out.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation Studio: the trick library could not be read: {}", e.what());
    }
    return out;
}
struct GameCatalogCache {
    std::mutex mutex;
    std::shared_ptr<const std::vector<GameTrick>> entries;
    std::uint64_t revision{};
};
GameCatalogCache& game_catalog_cache() { static GameCatalogCache cache; return cache; }
} // namespace
std::shared_ptr<const std::vector<NativeAsset>> native_assets() {
    static std::mutex mutex;
    static std::uint64_t revision{};
    static std::shared_ptr<const std::vector<NativeAsset>> assets;
    std::lock_guard lock(mutex);
    const auto current_revision = tricks_revision.load(std::memory_order_acquire);
    if (assets && revision == current_revision) return assets;
    assets = [] {
        auto out = std::make_shared<std::vector<NativeAsset>>();
        try {
            const auto path = library_folder() / L"library" / L"native-catalog.json";
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) > 32 * 1024 * 1024) return out;
            std::ifstream in(path, std::ios::binary);
            const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), JsonLimits{32 * 1024 * 1024, 48, 2 * 1024 * 1024});
            if (!root.is_array() || root.size() > 100000) return out;
            for (const auto& e : root) {
                if (!e.is_object()) continue;
                NativeAsset asset{e.value("category", "Other"), e.value("asset", ""), e.value("kind", "unclassified")};
                asset.root_type = e.value("root_type", "");
                asset.decode_state = e.value("state", "indexed");
                asset.detail = e.value("detail", "");
                asset.partition = e.value("partition", "");
                asset.toc = e.value("toc", "");
                asset.bundle = e.value("bundle", "");
                if (asset.toc.size() > 512 || asset.bundle.size() > 512) continue;
                if (asset.partition.empty()) asset.partition = std::filesystem::path(e.value("document", "")).stem().string();
                if (asset.root_type.size() > 128 || asset.decode_state.size() > 32 || asset.detail.size() > 1024 || asset.partition.size() > 36) continue;
                const auto display = native_asset_name(asset.asset, asset.root_type, asset.category);
                asset.label = display.label; asset.context = display.context;
                asset.category = native_asset_folder(asset.asset, asset.root_type, asset.category);
                asset.search = display.search + native_search_key(asset.category);
                if ((asset.asset.starts_with("animation/dingo/") || asset.asset.starts_with("_af/")) && asset.asset.size() < 1024 && asset.category.size() < 256)
                    out->push_back(std::move(asset));
            }
            native_disambiguate_names(*out);
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: native asset index not loaded: {}", e.what());
        }
        return out;
    }();
    revision = current_revision;
    return assets;
}
NativeDocumentPage native_asset_fields(const std::string& asset, const std::string& path, std::size_t page) {
    NativeDocumentPage result;
    static std::mutex mutex;
    static std::string cached_asset, cached_error;
    static std::uint64_t cached_revision{};
    static std::shared_ptr<const Json> document;
    std::lock_guard lock(mutex);
    try {
        const auto revision = tricks_revision.load(std::memory_order_acquire);
        if (asset != cached_asset || cached_revision != revision) {
            cached_asset = asset; cached_revision = revision; document.reset(); cached_error.clear();
            try {
                std::string guid = asset;
                const auto assets = native_assets();
                // By name, or by partition when the inspector follows a reference.
                auto found = std::find_if(assets->begin(), assets->end(), [&](const NativeAsset& a) { return a.asset == asset; });
                if (found == assets->end())
                    found = std::find_if(assets->begin(), assets->end(), [&](const NativeAsset& a) { return !a.partition.empty() && a.partition == asset; });
                const NativeAsset* row = found != assets->end() ? &*found : nullptr;
                if (row) guid = row->partition;
                const bool resource = guid.starts_with("res:");
                const auto resource_id = resource ? guid.substr(4) : "";
                bool valid = guid.size() == 36;
                for (std::size_t i = 0; i < guid.size(); ++i) {
                    const char c = guid[i];
                    valid &= (i == 8 || i == 13 || i == 18 || i == 23) ? c == '-' :
                        ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
                }
                if (resource) valid = !resource_id.empty() && resource_id.size() <= 20 && resource_id.find_first_not_of("0123456789") == std::string::npos;
                if (!valid) throw std::runtime_error("No typed document for this indexed name.");
                const auto file = library_folder() / L"library" / (resource ? L"native-resources" : L"native-documents") / ((resource ? resource_id : guid) + ".json");
                std::error_code error;
                if (std::filesystem::is_regular_file(file, error)) {
                    if (std::filesystem::file_size(file, error) > 16 * 1024 * 1024) throw std::runtime_error("Typed document too large to show.");
                    std::ifstream in(file, std::ios::binary);
                    document = std::make_shared<const Json>(Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}), JsonLimits{16 * 1024 * 1024, 64, 2 * 1024 * 1024}));
                } else if (!resource && row && !row->toc.empty() && !row->bundle.empty()) {
                    // A library built in game keeps no copies of the documents: they are read from the game itself.
                    document = std::make_shared<const Json>(native_library::document_from_game(xray::game_folder(), row->toc, row->bundle, row->asset));
                } else
                    throw std::runtime_error("Typed document missing; rebuild the trick library in SETTINGS.");
            } catch (const std::exception& e) { cached_error = e.what(); }
        }
        if (!document) throw std::runtime_error(cached_error);
        result.title = document->value("asset", asset);
        const Json* node = document.get();
        std::size_t start = 0;
        while (start < path.size()) {
            const auto slash = path.find('/', start);
            const auto token = path.substr(start, slash == std::string::npos ? slash : slash - start);
            if (node->is_array()) {
                if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Invalid array path");
                node = &node->at(static_cast<std::size_t>(std::stoull(token)));
            } else node = &node->at(token);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        constexpr std::size_t page_size = 50;
        result.pages = std::max<std::size_t>(1, (node->size() + page_size - 1) / page_size);
        page = std::min(page, result.pages - 1);
        const auto add = [&](const std::string& label, const Json& value) {
            NativeField field;
            field.label = label; field.path = path.empty() ? label : path + '/' + label;
            field.expandable = value.is_object() || value.is_array();
            field.value = value.is_array() ? std::to_string(value.size()) + " items >" : value.is_object() ?
                ((value.contains("type") && value.at("type").is_string()) ? value.at("type").get<std::string>() : "Object") + " >" : value.dump();
            if (field.value.size() > 200) field.value.resize(200);
            if (value.is_object() && value.contains("kind") && value.at("kind").is_string() && value.at("kind").get<std::string>() == "external") field.reference = value.value("partition", "");
            if (value.is_object() && value.contains("resource_id") && value.at("resource_id").is_number_integer()) field.reference = "res:" + value.at("resource_id").dump();
            result.fields.push_back(std::move(field));
        };
        const auto first = page * page_size, last = first + page_size;
        if (node->is_array()) for (std::size_t i = first; i < std::min(last, node->size()); ++i) add(std::to_string(i), node->at(i));
        else if (node->is_object()) {
            std::size_t i = 0;
            for (const auto& [name, value] : node->items()) { if (i >= first && i < last) add(name, value); ++i; }
        }
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}
std::shared_ptr<const std::vector<GameTrick>> game_tricks() {
    auto& cache = game_catalog_cache();
    std::lock_guard lock(cache.mutex);
    const auto revision = tricks_revision.load(std::memory_order_acquire);
    if (!cache.entries || cache.revision != revision) {
        cache.entries = std::make_shared<const std::vector<GameTrick>>(sort_game_library(read_catalog()));
        cache.revision = revision;
        logging::log(Level::info, Channel::skater, "Animation Studio: {} game tricks in the library.", cache.entries->size());
    }
    return cache.entries;
}
void refresh_game_tricks(std::string& message) {
    tricks_revision.fetch_add(1, std::memory_order_acq_rel);
    message = "Library refreshed: " + std::to_string(game_tricks()->size()) + " clips.";
}
namespace {
// The trick library's build on this PC (native_library), one at a time, in the background.
struct LibraryBuilder {
    std::mutex mutex;
    LibraryBuild state;
    std::uint64_t revision{}; // tricks_revision the library on disk was last looked at for
    std::atomic<bool> stop{};
};
LibraryBuilder& library_builder() {
    static auto* builder = new LibraryBuilder;
    return *builder;
}
// What is on disk now: a library there, when it was built and for which game.
void look_at_library_locked(LibraryBuilder& b) {
    const auto folder = library_folder() / L"library";
    std::error_code error;
    b.state.exists = std::filesystem::is_regular_file(folder / L"catalog.json", error);
    b.state.built.clear();
    b.state.outdated = false;
    // Built again when it is from another version of the game, or from an older builder than this one (one with no
    // build-info.json at all is from before the in-game builder: it has no Kickflip MID, so a trick copied to MID
    // could not follow the game's clip and played on its own clock, out of step).
    if (b.state.exists) b.state.outdated = true;
    try {
        std::ifstream in(folder / L"build-info.json", std::ios::binary);
        if (in) {
            const auto info = Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}));
            b.state.built = info.value("built", "");
            const auto stamp = info.value("stamp", "");
            const int builder = info.value("builder", 0);
            b.state.outdated = (!stamp.empty() && stamp != supported_build::game_sha256) || builder < native_library::builder_version;
        }
    } catch (...) {}
    // Its files were being replaced one by one and that did not finish (replace_library_files): built again.
    if (b.state.exists && std::filesystem::exists(folder / L".replacing", error)) b.state.outdated = true;
}
// Where a step's `done` of `total` puts the whole build (decoding the clips takes most of it).
float build_fraction(std::string_view step, std::size_t done, std::size_t total) {
    const float part = total ? std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f) : 0.0f;
    if (step.starts_with("Reading")) return 0.15f * part;
    if (step.starts_with("Finding")) return 0.15f + 0.1f * part;
    if (step.starts_with("Decoding")) return 0.25f + 0.72f * part;
    return 0.97f + 0.03f * part;
}
void finish_library_build(bool failed, std::string message) {
    auto& b = library_builder();
    std::lock_guard lock(b.mutex);
    b.state.running = false;
    b.state.failed = failed;
    b.state.message = std::move(message);
    look_at_library_locked(b);
    b.revision = tricks_revision.load(std::memory_order_acquire);
}
// Renames a folder, trying again a few times: a file in it may be open for a moment (a clip being read).
void rename_folder(const std::filesystem::path& from, const std::filesystem::path& to) {
    for (int attempt = 0;; ++attempt) {
        std::error_code error;
        std::filesystem::rename(from, to, error);
        if (!error) return;
        if (attempt >= 20) throw std::runtime_error("could not move " + utf8_path(from) + ": " + error.message());
        Sleep(100);
    }
}
// The new library's files put over the old one's in place, and the old one's that the new one has not taken out: for
// when the library's folder itself cannot be moved. Marked (.replacing) until it is done, and build-info.json - what
// says the library is current - goes in last: one that fails halfway is built again on the next start.
void replace_library_files(const std::filesystem::path& staging, const std::filesystem::path& target) {
    const std::filesystem::path marker_name = L".replacing", info_name = L"build-info.json";
    {
        std::ofstream marker(target / marker_name, std::ios::binary | std::ios::trunc);
        if (!marker) throw std::runtime_error("could not write in " + utf8_path(target));
    }
    std::set<std::filesystem::path> fresh{marker_name, info_name};
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(staging, error), end; !error && it != end; it.increment(error)) {
        const auto relative = std::filesystem::relative(it->path(), staging, error);
        fresh.insert(relative);
        if (relative == info_name) continue; // last
        if (it->is_directory(error)) {
            std::filesystem::create_directories(target / relative, error);
            continue;
        }
        std::filesystem::create_directories((target / relative).parent_path(), error);
        rename_folder(it->path(), target / relative); // over the old file, trying again while it is open for a moment
    }
    // The new library not read to its end: nothing of the old one is taken out on a list that may be short.
    if (error) throw std::runtime_error("could not read the new library: " + error.message());
    std::vector<std::filesystem::path> stale;
    for (std::filesystem::recursive_directory_iterator it(target, error), end; !error && it != end; it.increment(error))
        if (!fresh.contains(std::filesystem::relative(it->path(), target, error))) stale.push_back(it->path());
    // Deepest first, so a folder is empty when its turn comes.
    std::sort(stale.begin(), stale.end(), [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
    for (const auto& path : stale) std::filesystem::remove(path, error);
    rename_folder(staging / info_name, target / info_name);
    std::filesystem::remove(target / marker_name, error);
    std::filesystem::remove_all(staging, error);
}
void run_library_build() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto folder = library_folder();
    const auto staging = folder / L"library.building", target = folder / L"library", old = folder / L"library.old";
    auto& b = library_builder();
    std::error_code error;
    try {
        if (folder.empty()) throw std::runtime_error("the studio's folder is unknown");
        std::filesystem::create_directories(folder, error);
        std::filesystem::remove_all(staging, error); // what a build stopped halfway left
        // The game closed between the two moves of a swap: the old library goes back in place, not out with the rest.
        if (!std::filesystem::exists(target, error) && std::filesystem::exists(old, error)) {
            try { rename_folder(old, target); } catch (...) {}
        }
        std::filesystem::remove_all(old, error);
        if (const auto space = std::filesystem::space(folder, error); !error && space.available < 2'500'000'000ull)
            throw std::runtime_error("the trick library needs about 2 GB free on the drive the game is on");
        const auto report = native_library::build(xray::game_folder(), staging, [&](std::string_view step, std::size_t done, std::size_t total) {
            std::lock_guard lock(b.mutex);
            b.state.step = std::string(step);
            b.state.done = done;
            b.state.total = total;
            b.state.fraction = std::max(b.state.fraction, build_fraction(step, done, total));
            return !b.stop.load(std::memory_order_acquire);
        }, std::string(supported_build::game_sha256));
        if (!report.tricks) throw std::runtime_error("no tricks were found in the game's files" + (report.first_error.empty() ? std::string{} : ": " + report.first_error));
        {
            std::lock_guard lock(b.mutex);
            b.state.step = "Putting it in place";
            b.state.fraction = 1;
        }
        const bool had = std::filesystem::exists(target, error);
        bool moved_old = false;
        if (had) {
            try {
                rename_folder(target, old);
                moved_old = true;
            } catch (const std::exception& e) {
                // The folder is in use (a program is standing in it, as a command prompt opened there does): its files are
                // replaced one by one instead.
                logging::log(Level::info, Channel::skater, "Animation Studio: the library's folder is in use ({}); replacing its files instead.", e.what());
            }
        }
        if (!had || moved_old) {
            try {
                rename_folder(staging, target);
            } catch (...) {
                if (moved_old) rename_folder(old, target);
                throw;
            }
        } else {
            replace_library_files(staging, target);
        }
        tricks_revision.fetch_add(1, std::memory_order_acq_rel);
        {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            s.tricks_read = false;
            s.idle_read = false;
        }
        logging::log(Level::info, Channel::skater, "Animation Studio: trick library built: {} clips, {} tricks, {} not taken, {} errors.", report.clips,
                     report.tricks, report.unsupported, report.errors);
        finish_library_build(false, "Trick library built: " + std::to_string(report.tricks) + " tricks and " +
                                        std::to_string(report.clips - report.tricks) + " other animations from your game.");
        std::filesystem::remove_all(old, error);
    } catch (const native_library::Stopped&) {
        std::filesystem::remove_all(staging, error);
        finish_library_build(false, "Stopped. Your trick library is as it was.");
    } catch (const std::exception& e) {
        std::filesystem::remove_all(staging, error);
        logging::log(Level::warning, Channel::skater, "Animation Studio: the trick library could not be built: {}", e.what());
        finish_library_build(true, std::string("The trick library could not be built: ") + e.what() + ".");
    }
}
} // namespace
LibraryBuild library_build() {
    auto& b = library_builder();
    std::lock_guard lock(b.mutex);
    if (const auto revision = tricks_revision.load(std::memory_order_acquire); !b.state.running && b.revision != revision) {
        look_at_library_locked(b);
        b.revision = revision;
    }
    return b.state;
}
void start_library_build(bool in_background) {
    auto& b = library_builder();
    {
        std::lock_guard lock(b.mutex);
        if (b.state.running) return;
        b.state.running = true;
        b.state.failed = false;
        b.state.step = "Starting";
        b.state.message.clear();
        b.state.done = b.state.total = 0;
        b.state.fraction = 0;
        b.stop.store(false, std::memory_order_release);
    }
    try {
        std::thread([in_background] {
            // Started by itself while he skates (an old library rebuilt on the first start): below the game, for the processor
            // and the disk alike, so the game's frames and its streaming come first. Asked for in the studio: at full speed.
            if (in_background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            run_library_build();
        }).detach();
    } catch (const std::exception& e) {
        finish_library_build(true, std::string("The trick library could not be built: ") + e.what() + ".");
    }
}
void stop_library_build() { library_builder().stop.store(true, std::memory_order_release); }
void build_library_when_needed(bool now) {
    static std::atomic<bool> looked{false};
    static const auto since = std::chrono::steady_clock::now();
    if (looked.load(std::memory_order_acquire)) return;
    // As soon as ReSkate runs (the first frame drawn), below the game's priority, so a library from before an update is
    // built again while the game loads and the player gets in, never when the studio is opened. It waited 20 s.
    if (!now && std::chrono::steady_clock::now() - since < std::chrono::seconds(1)) return;
    looked.store(true, std::memory_order_release);
    const auto build = library_build();
    if (build.running || (build.exists && !build.outdated)) return;
    if (build.exists) logging::log(Level::info, Channel::skater, "Animation Studio: the trick library is from an older studio or another version of the game: rebuilding it.");
    else logging::log(Level::info, Channel::skater, "Animation Studio: no trick library yet: building it from the game's files.");
    start_library_build(true);
}
// Where a trick opens: on its pop, where editing it begins - not on its start bracket, whose keys stay at 0 so
// a change there was put straight back. A piece with no pop opens a quarter of the way in.
float opening_time(const Clip& clip) noexcept {
    const float d = clip.duration();
    const float t = clip.takeoff >= 0 && clip.takeoff < d ? clip.takeoff : d * 0.25f;
    return std::round(t * 60) / 60;
}
std::shared_ptr<Clip> read_game_clip(const GameTrick& trick) {
    if (!safe_library_clip(trick.file)) throw std::runtime_error("invalid local library clip name");
    if (trick.position_codec != "native-piec-v1-extent")
        throw std::runtime_error("This native library has outdated position decoding. Rebuild or update it before previewing.");
    std::ifstream in(library_folder() / L"library" / std::filesystem::path(trick.file), std::ios::binary);
    if (!in) throw std::runtime_error("its file is missing");
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
    if (clip->joints != skeleton_joints) throw std::runtime_error("it is for another skeleton");
    place_native_marks(*clip);
    return clip;
}
namespace {
// The hover preview's tricks, read off the frame (peek_game_trick).
struct PeekRead {
    std::string key; // what it is and the stance it was turned into
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Animation> edits;
};
struct Peeks {
    std::mutex mutex;             // taken before the studio's own (state().mutex), never while that is held
    std::vector<PeekRead> kept;   // the last few read, the newest last
    std::set<std::string> reading;
    std::string to_play;          // the one to start once it is read ("" none)
};
Peeks& peeks() {
    static auto* value = new Peeks; // process lifetime
    return *value;
}
constexpr std::size_t peeks_kept = 8;
// Starts a read trick if it is still the one asked to play. The caller holds peeks().mutex.
void start_peek_locked(Peeks& p, const PeekRead& read) {
    if (p.to_play != read.key || !read.clip) return;
    p.to_play.clear();
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.peek_clip = read.clip;
    s.peek_edits = read.edits;
    s.peek_since = now_seconds();
    publish_locked(s, false);
}
// A clip and its edits turned the way the studio shows them when opened (face_studio_stance): one of the game's in the
// studio's stance (`goofy`: the right foot leads), a recording as made, a stance version on its own foot.
PeekRead turned(std::string key, std::shared_ptr<Clip> clip, Animation edits, bool goofy, bool game_clip) {
    if (!clip || clip->empty() || clip->joints != skeleton_joints) throw std::runtime_error("not a clip of the skater");
    const bool own_foot = !game_clip && edits.source.kind != "native_asset";
    const auto lead = clip_leads_right(*clip);
    const bool want_right = edits.keep_leads_right ? *edits.keep_leads_right : own_foot ? lead.value_or(goofy) : goofy;
    if (lead && *lead != want_right) {
        clip = std::make_shared<Clip>(mirror_clip(*clip));
        edits.clip = {};
        edits = mirror_animation(edits);
    }
    return {std::move(key), std::move(clip), std::make_shared<const Animation>(std::move(edits))};
}
bool studio_goofy_now() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return studio_goofy(s.stance_choice);
}
// A trick to look at, under `key`: one kept starts at once; else `read` reads it on its own thread.
bool ask_peek(const std::string& key, bool play, std::function<PeekRead()> read) {
    auto& p = peeks();
    std::lock_guard lock(p.mutex);
    if (play) p.to_play = key;
    for (auto it = p.kept.rbegin(); it != p.kept.rend(); ++it)
        if (it->key == key) {
            if (play) start_peek_locked(p, *it);
            return true;
        }
    if (!p.reading.insert(key).second) return true; // being read: it starts when it is
    try {
        std::thread([key, read = std::move(read)] {
            PeekRead done;
            try {
                done = read();
            } catch (const std::exception& e) {
                logging::log(Level::warning, Channel::skater, "Animation Studio: a preview could not be read: {}", e.what());
            } catch (...) {}
            auto& l = peeks();
            std::lock_guard lock(l.mutex);
            l.reading.erase(key);
            if (!done.clip) {
                if (l.to_play == key) l.to_play.clear();
                return;
            }
            l.kept.push_back(done);
            if (l.kept.size() > peeks_kept) l.kept.erase(l.kept.begin());
            start_peek_locked(l, done);
        }).detach();
    } catch (...) {
        p.reading.erase(key);
        if (p.to_play == key) p.to_play.clear();
        return false;
    }
    return true;
}
} // namespace
bool peek_game_trick(const GameTrick& trick, bool play) {
    if (!safe_library_clip(trick.file)) return false;
    const bool goofy = studio_goofy_now();
    const auto key = "library|" + trick.file + (goofy ? "|goofy" : "|regular");
    return ask_peek(key, play, [trick, key, goofy] { return turned(key, read_game_clip(trick), {}, goofy, true); });
}
bool peek_saved(const std::string& animation, bool play) {
    try {
        const auto safe = file_safe(animation);
        std::error_code error;
        const auto written = std::filesystem::last_write_time(saved_clip(safe), error);
        if (error) return false;
        const bool goofy = studio_goofy_now();
        // Saved again, it is another trick: when it was written is part of what it is.
        const auto key = "saved|" + safe + "|" + std::to_string(written.time_since_epoch().count()) + (goofy ? "|goofy" : "|regular");
        return ask_peek(key, play, [safe, key, goofy] {
            std::ifstream json_in(saved_json(safe), std::ios::binary), clip_in(saved_clip(safe), std::ios::binary);
            if (!json_in || !clip_in) throw std::runtime_error("its files are missing");
            auto edits = from_json(std::string((std::istreambuf_iterator<char>(json_in)), {}));
            auto clip = std::make_shared<Clip>(clip_from_bytes(std::vector<std::uint8_t>((std::istreambuf_iterator<char>(clip_in)), {})));
            if (edits.source.kind == "native_asset") place_native_marks(*clip);
            return turned(key, std::move(clip), std::move(edits), goofy, false);
        });
    } catch (...) { return false; }
}
bool peek_playing() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.peek_clip && s.peek_since >= 0;
}
void end_peek() {
    auto& p = peeks();
    std::lock_guard peek_lock(p.mutex);
    p.to_play.clear();
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.peek_clip) return;
    s.peek_clip.reset();
    s.peek_edits.reset();
    s.peek_since = -1;
    publish_locked(s, false);
}
float peek_duration() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.peek_clip ? s.peek_clip->duration() : 0.0f;
}
bool load_game_trick(const GameTrick& trick, std::string& message) {
    try {
        auto clip = read_game_clip(trick);
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.edits = {};
        s.previewing = false;
        s.kept.reset(); // opened to edit: what a preview kept goes (the studio asks first when it has changes)
        // A piece of the game's animation is known by its clip ("native:<asset>"), which is how it is found playing in game.
        s.edits.trick = trick.preview_only ? "native:" + trick.asset : trick.trick;
        s.edits.pop = trick.pop;
        s.edits.name = trick.label + " " + trick.pop;
        // The game's clips are labelled regular (authored_goofy knows they stand goofy).
        s.edits.stance = "regular";
        s.edits.switch_stance = false;
        s.edits.source = AnimationSource{"native_asset", trick.asset, "animation/dingo/animbase_default_skeleton", "skater-placement"};
        s.edits.source.position_codec = trick.position_codec;
        s.edits.entry = trick.entry;
        s.edits.loop = trick.looping;
        s.clip = std::move(clip);
        reset_brackets_locked(s);
        s.saved_as.clear();
        s.undo.clear();
        s.redo.clear();
        s.time = opening_time(*s.clip);
        s.playing = false;
        s.dirty = false; // only an edit makes it unsaved
        message = trick.preview_only ? "Opened " + trick.label + " to edit. It is a piece of the game's own animation rather than a whole trick, "
                                       "so it is saved by the clip it came from. In game it plays whenever the game plays that piece: the studio knows it by the skater's pose, about a quarter of a second after it starts." :
            "Opened the game's " + trick.label + " (" + trick.pop + " pop). In game, skate.'s own board physics and blending still play on top of your changes.";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = "Could not open " + trick.label + ": " + e.what();
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.status = message; // what was open stays open, and this says why the new one did not
        publish_locked(s);
        return false;
    }
}
bool load_saved(const std::string& name, std::string& message, bool preview);
bool load(const std::string& name, std::string& message) { return load_saved(name, message, false); }
bool preview_saved(const std::string& name, std::string& message) { return load_saved(name, message, true); }
namespace {
void end_preview_locked(State& s) {
    if (!s.previewing) return;
    s.previewing = false;
    ++s.clip_serial;
    s.playing = false;
    if (s.kept) {
        // Back to the trick that was open, as it was: its changes, its undo and redo.
        s.clip = std::move(s.kept->clip);
        s.edits = std::move(s.kept->edits);
        s.undo = std::move(s.kept->undo);
        s.redo = std::move(s.kept->redo);
        s.saved_as = std::move(s.kept->saved_as);
        s.time = s.kept->time;
        s.dirty = s.kept->dirty;
        s.kept.reset();
        s.status = "Preview closed: back to " + (s.edits.name.empty() ? std::string("your trick") : s.edits.name) + ".";
    } else {
        s.edits = {};
        s.clip.reset();
        s.saved_as.clear();
        s.undo.clear();
        s.redo.clear();
        s.dirty = false;
        s.status = "Preview closed.";
    }
    publish_locked(s);
}
} // namespace
void end_preview() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    end_preview_locked(s);
}
namespace {
// A trick opened from its two files: one of the library's (`library`, by its name) to edit or watch, or one of a pack's
// that is not installed (preview_unpacked), to watch only.
bool open_trick_files(const std::string& safe, const std::filesystem::path& json_path, const std::filesystem::path& clip_path, bool library,
                      std::string& message, bool preview) {
    try {
        std::ifstream json_in(json_path, std::ios::binary);
        if (!json_in) throw std::runtime_error(library ? "it is not in the library" : "its files are gone: preview it again");
        const std::string text((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>());
        auto animation = from_json(text);
        std::ifstream clip_in(clip_path, std::ios::binary);
        if (!clip_in) throw std::runtime_error("its recorded trick is missing");
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
        auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
        if (clip->joints != skeleton_joints) throw std::runtime_error("it was recorded on another skeleton");
        if (animation.source.kind == "native_asset") place_native_marks(*clip);
        // A key a frame past the end (rounded onto a frame past it by builds before 2026-10-23) is put on the end.
        for (auto& t : animation.tracks)
            for (auto& k : t.keys) {
                if (k.time > clip->duration() + 0.1f) throw std::runtime_error("a key lies past its clip");
                k.time = std::min(k.time, clip->duration());
            }
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.library_read) read_library_locked(s);
        if (library && !preview && locked_locked(s, safe))
            throw std::runtime_error("it is locked to its maker" + (animation.author.empty() ? std::string{} : " (" + animation.author + ")") +
                                     ": you can play it, test it and preview it, not open it to edit or copy it");
        animation.name = safe;
        if (preview && !s.previewing && s.clip && !s.clip->empty())
            s.kept = State::Kept{s.clip, s.edits, std::move(s.undo), std::move(s.redo), s.saved_as, s.time, s.dirty};
        else if (!preview) s.kept.reset();
        s.edits = std::move(animation);
        s.edits.speed = 1; // the editor always opens a trick at x1, whatever it was saved at
        s.clip = std::move(clip);
        ++s.clip_serial;
        s.previewing = preview;
        s.saved_as = library ? safe : std::string{}; // one not installed is no file of the library's
        s.undo.clear();
        s.redo.clear();
        s.time = opening_time(*s.clip);
        s.playing = false;
        s.dirty = false;
        const auto shown = s.edits.label.empty() ? safe : s.edits.label;
        message = !library ? "Previewing " + shown + ". It is not installed: GET puts it in your library." : preview ? "Previewing " + safe + "." : "Opened " + safe + ".";
        if (preview) s.playing = true;
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = "Could not open " + safe + ": " + e.what();
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.status = message;
        return false;
    }
}
} // namespace
bool load_saved(const std::string& name, std::string& message, bool preview) {
    const auto safe = file_safe(name);
    if (safe.empty()) {
        message = "Could not open " + name + ": it is not in the library";
        return false;
    }
    return open_trick_files(safe, saved_json(safe), saved_clip(safe), true, message, preview);
}
bool preview_unpacked(const UnpackedTrick& trick, std::string& message) {
    const auto safe = file_safe(trick.name);
    if (safe.empty() || safe != trick.name) {
        message = "Could not open " + trick.name + ".";
        return false;
    }
    return open_trick_files(safe, trick.json, trick.clip, false, message, true);
}
bool remove(const std::string& name, std::string& message, std::vector<std::string>* packs_gone) {
    const auto safe = file_safe(name);
    if (safe.empty()) {
        message = "There is no trick called " + name + ".";
        return false;
    }
    // Both ways it can be saved (a loose copy left behind would be listed again as the trick).
    remove_saved_files(safe);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.tricks_read = false;
    read_library_locked(s);
    // Out of every pack it was in. A pack made on another PC (got from the MARKET or imported) with no trick left is
    // deleted too, so it can be got again; your own packs stay, empty or not.
    std::vector<std::string> left, gone;
    try {
        const auto packs = s.packs;
        for (const auto& saved : packs) {
            if (!saved.has(safe)) continue;
            auto& pack = pack_draft_locked(s, saved.name);
            for (auto& [filed, names] : pack.slots) std::erase(names, safe);
            pack.off.erase(safe);
            pack.unlinked.erase(safe);
            tidy_pack(pack);
            const auto members = pack.members();
            const bool empty = std::none_of(members.begin(), members.end(), [&](const std::string& member) {
                return std::find(s.library.begin(), s.library.end(), member) != s.library.end();
            });
            (empty && locked_key(pack.maker_key) ? gone : left).push_back(saved.name);
        }
        for (const auto& pack : gone) s.pack_drafts.erase(pack);
        write_pack_drafts_locked(s);
        for (const auto& pack : gone) {
            std::error_code error;
            std::filesystem::remove(pack_file(pack), error);
            std::filesystem::remove(pack_thumbnail_file(pack), error);
            auto names = pack_names_on();
            if (std::erase(names, pack)) write_pack_names_on(names);
            if (s.open_pack == pack) s.open_pack.clear();
        }
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation Studio: {} was deleted, but its packs were not all updated: {}", safe, e.what());
    }
    const auto list = [](const std::vector<std::string>& names) {
        std::string out;
        for (std::size_t i = 0; i < names.size(); ++i) out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
        return out;
    };
    message = "Deleted " + safe + (left.empty() ? std::string{} : ", and took it out of " + list(left)) + "." +
              (gone.empty() ? std::string{} : " " + list(gone) + (gone.size() == 1 ? " had" : " had") + " no trick left, so " +
                                                   (gone.size() == 1 ? "it is gone too: get it again from the MARKET." : "they are gone too: get them again from the MARKET."));
    if (!left.empty() || !gone.empty()) packs_changed_locked(s, message);
    s.status = message;
    if (packs_gone) *packs_gone = gone;
    return true;
}
bool export_diagnostics(std::string& message) {
    try {
        DiagnosticBuffer copy, preview;
        { auto& history = diagnostic_history(); std::lock_guard lock(history.mutex); copy = history.buffer; }
        { auto& history = preview_history(); std::lock_guard lock(history.mutex); preview = history.buffer; }
        static std::atomic<unsigned> serial{};
        const auto folder = library_folder() / L"diagnostics" /
            ("dump-" + std::to_string(GetTickCount64()) + "-" + std::to_string(serial.fetch_add(1)));
        auto summary = write_diagnostic_history(copy, folder);
        summary["preview"] = write_diagnostic_history(preview, folder / L"preview");
        summary["game_sha256_contract"] = std::string(supported_build::game_sha256);
        summary["build"] = "studio-coherent-preview-v3";
        summary["network_upload"] = false;
        summary["failure"] = "";
        summary["active_pack"] = "";
        std::shared_ptr<const Clip> preview_clip;
        Animation preview_edits;
        { auto& s = state(); std::lock_guard lock(s.mutex);
          summary["failure"] = s.failure;
          std::string on;
          for (const auto& pack : s.packs_on) on += (on.empty() ? "" : ", ") + pack.name;
          summary["active_pack"] = on;
          summary["studio_open"] = s.open;
          summary["custom_playback_enabled"] = s.play_in_game;
          summary["gameplay_body_binding"] = "anatomical_local";
          summary["gameplay_key_timing"] = "source_seconds_from_takeoff";
          summary["gameplay_full_source_replacement"] = false;
          summary["preview_source"] = Json::object();
          summary["preview_source"]["name"] = s.edits.name;
          summary["preview_source"]["asset"] = s.edits.source.asset;
          summary["preview_source"]["kind"] = s.edits.source.kind;
          summary["preview_source"]["position_codec"] = s.edits.source.position_codec;
          preview_clip = s.clip; preview_edits = s.edits; }
        if (preview_clip && !preview_clip->empty()) {
            const auto bytes = clip_bytes(*preview_clip);
            std::ofstream source(folder / L"preview" / L"loaded-source.clip", std::ios::binary);
            source.exceptions(std::ios::badbit | std::ios::failbit);
            source.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            source.flush();
            write_preset_file(folder / L"preview" / L"loaded-edits.json", to_json(preview_edits));
        }
        std::ofstream skeleton(folder / "skeleton.csv");
        skeleton.exceptions(std::ios::failbit | std::ios::badbit);
        skeleton << "joint,parent,mirror,editable_label\n";
        for (std::uint16_t j = 0; j < skeleton_joints; ++j)
            skeleton << j << ',' << parent(j) << ',' << mirror_joint[j] << ',' << diagnostic_csv(editable(j) ? editable(j)->label : "") << '\n';
        skeleton.flush();
        Json presets = Json::array();
        std::vector<std::string> names;
        for (const auto& sample : copy.samples)
            if (!sample.preset.empty() && std::find(names.begin(), names.end(), sample.preset) == names.end()) names.push_back(sample.preset);
        const auto inputs = folder / L"presets";
        std::filesystem::create_directory(inputs);
        std::uintmax_t copied_bytes = 0;
        for (const auto& name : names) {
            if (name != file_safe(name)) continue;
            Json record = Json::object(); record["name"] = name;
            record["copied"] = false;
            for (const auto* extension : {".json", ".clip"}) {
                const auto source = std::string_view(extension) == ".json" ? saved_json(name) : saved_clip(name);
                std::error_code error;
                if (!std::filesystem::is_regular_file(source, error)) continue;
                const auto bytes = std::filesystem::file_size(source, error);
                if (error || bytes > 128 * 1024 * 1024 || copied_bytes + bytes > 128 * 1024 * 1024) continue;
                std::filesystem::copy_file(source, inputs / (name + extension));
                copied_bytes += bytes; record["copied"] = true;
            }
            presets.push_back(std::move(record));
        }
        summary["presets"] = std::move(presets);
        summary["preset_payload_bytes"] = copied_bytes;
        { const auto info = library_folder().parent_path() / L"build-info.json";
          std::error_code error;
          if (std::filesystem::is_regular_file(info, error) && std::filesystem::file_size(info, error) < 1024 * 1024)
              std::filesystem::copy_file(info, folder / L"client-build-info.json"); }
        write_preset_file(folder / L"summary.json", summary.dump(2));
        message = "Diagnostics saved: " + folder.string() + (copy.samples.empty() && preview.samples.empty() ? " (no poses captured yet)." : ".");
        auto& s = state(); std::lock_guard lock(s.mutex); s.status = message;
        return true;
    } catch (const std::exception& e) { message = std::string("Diagnostic export failed: ") + e.what(); return false; }
}
namespace {
// The last `limit` bytes of a file another process may still be writing (the game's log): read shared, never locked.
std::optional<std::string> read_tail(const std::filesystem::path& path, std::uint64_t limit) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) return std::nullopt;
    const auto total = static_cast<std::uint64_t>(size.QuadPart);
    const std::uint64_t start = total > limit ? total - limit : 0;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(start);
    if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) return std::nullopt;
    std::string out(static_cast<std::size_t>(total - start), '\0');
    DWORD read = 0;
    if (!out.empty() && !ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) return std::nullopt;
    out.resize(read);
    return out;
}
} // namespace
// REPORT A BUG (SETTINGS > STUDIO): everything the crew needs about a problem in one zip - the last 15 seconds
// (export_diagnostics), the end of the game's log, the studio's settings and which build this is - saved in
// ReSkate.Tricks\bug reports and shown in Explorer, to send with what happened. Nothing is sent from here.
bool report_bug(std::string& message) {
    try {
        std::string diagnostics;
        export_diagnostics(diagnostics);
        const auto library = library_folder();
        std::vector<std::pair<std::string, std::string>> files;
        std::uint64_t total = 0;
        constexpr std::uint64_t budget = 96ull * 1024 * 1024;
        const auto add = [&](const std::filesystem::path& path, const std::string& name, std::uint64_t limit) {
            if (total >= budget) return;
            auto text = read_tail(path, std::min(limit, budget - total));
            if (!text) return;
            total += text->size();
            files.emplace_back(name, std::move(*text));
        };
        // The newest diagnostics dump: the one export_diagnostics just made.
        std::error_code error;
        std::filesystem::path newest;
        std::filesystem::file_time_type newest_time{};
        for (const auto& entry : std::filesystem::directory_iterator(library / L"diagnostics", error)) {
            if (!entry.is_directory(error) || !entry.path().filename().string().starts_with("dump-")) continue;
            const auto when = entry.last_write_time(error);
            if (newest.empty() || when > newest_time) newest = entry.path(), newest_time = when;
        }
        if (!newest.empty())
            for (const auto& entry : std::filesystem::recursive_directory_iterator(newest, error))
                if (entry.is_regular_file(error))
                    add(entry.path(), "diagnostics/" + std::filesystem::relative(entry.path(), newest, error).generic_string(), 64ull * 1024 * 1024);
        // The game's log (its end, where the problem is) and a note of a silent exit, if there was one.
        if (const auto logs = logging::status().directory; !logs.empty()) {
            add(logs / L"ReSkate.log", "ReSkate.log", 8ull * 1024 * 1024);
            add(logs / L"crash_note.txt", "crash_note.txt", 256 * 1024);
        }
        add(library / settings_file, "studio-settings.json", 1024 * 1024);
        // Which build, and the studio as it is now.
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto stamp = std::format("{:04}-{:02}-{:02} {:02}-{:02}-{:02}", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        std::string about = std::format("ReSkate Animator Studio bug report, {}\r\nReSkate {} ({} {})\r\n", stamp, reskate_version,
                                        reskate_modified_build ? std::string(reskate_mod_id) : std::string("not a modified build"), reskate_mod_version);
        {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            std::string on;
            for (const auto& pack : s.packs_on) on += (on.empty() ? "" : ", ") + pack.name;
            about += std::format("Your stance (the game's setting): {}\r\nCustom tricks in game: {}\r\nTrick matching: {}\r\nCrossfade: {:.2f} s\r\n"
                                 "Packs on: {}\r\nOpen trick: {}\r\nLast message: {}\r\nLast failure: {}\r\n",
                                 goofy_stance() ? "goofy" : "regular", s.play_in_game ? "on" : "off", s.front_foot ? "front foot" : "exact stance",
                                 s.crossfade, on.empty() ? "none" : on, s.edits.name.empty() ? s.edits.trick : s.edits.name, s.status, s.failure);
        }
        about += "\r\nWhat happened (write it here, or in your message): the trick, your stance (normal / switch / fakie / nollie), "
                 "and what you saw.\r\n";
        files.emplace_back("report.txt", about);
        const auto zip = library / L"bug reports" / std::filesystem::path("ReSkate bug report " + stamp + ".zip");
        write_whole(zip, stored_zip(files));
        message = "Bug report saved: " + utf8_path(zip.filename()) + ". Send it to us with what happened and which trick.";
        logging::log(Level::info, Channel::skater, "Animation Studio: {}", message);
        {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            s.status = message;
            ++s.revision;
        }
        // There it is, chosen in Explorer.
        const std::wstring select = L"/select,\"" + zip.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    } catch (const std::exception& e) {
        message = std::string("The bug report was not saved: ") + e.what();
        return false;
    }
}
void rename(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.name = name;
    s.dirty = true;
    ++s.revision;
}
void set_your_name(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.your_name == utf8_prefix(name, 48)) return;
    s.your_name = utf8_prefix(name, 48);
    save_settings_locked(s);
    publish_locked(s, false);
}

void set_orbit(const Orbit& orbit) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.orbit = orbit;
    s.orbit.pitch = std::clamp(orbit.pitch, -1.45f, 1.45f);
    s.orbit.distance = std::clamp(orbit.distance, 0.6f, 25.0f);
}
void set_view_shift(float share, float right_share) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.view_shift = std::clamp(share, -0.4f, 0.4f);
    s.view_shift_x = std::clamp(right_share, -0.9f, 0.9f);
}
void focus_joint(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.focus == joint) s.focus.reset();
    else s.focus = joint;
}
void set_focus(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (joint == all_joints) s.focus.reset();
    else s.focus = joint;
}
void set_camera_follow(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.camera_follows = on;
    s.held_centre_valid = false; // taken again where the camera stands now
}
} // namespace dingosdk::anim_studio
