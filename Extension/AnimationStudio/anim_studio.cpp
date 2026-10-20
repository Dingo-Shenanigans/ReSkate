#include "anim_studio.h"
#include "trick_batch.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Core/Log/logging.h"
#include "anim_research.h"
#include "anim_runtime_rules.h"
#include "anim_playback.h"
#include "Engine/Core/Profiling/profiler.h"
#include "anim_diagnostics.h"
#include "skater_mirror.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/grind_exit.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/grind_exit_control.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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
};
// The skater's stance from the grind exit setting: goofy or regular (not set counts as regular).
bool goofy_stance() noexcept { return dingosdk::grind_exit_control::stance() == 2; }
std::optional<bool> switch_stance(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    if (!core || read<std::uintptr_t>(core).value_or(0) != base + addr::no_bail::bail_core_vtable) return std::nullopt;
    const auto context = read<std::uintptr_t>(core + 0x3c0).value_or(0);
    const auto flags = context ? read<std::uint8_t>(context + addr::grind_exit::stance_flags_offset) : std::nullopt;
    return flags ? std::optional<bool>{(*flags & addr::grind_exit::stance_switch_mask) != 0} : std::nullopt;
}
using CustomTricks = std::vector<CustomTrick>;
struct Snapshot {
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Clip> idle; // shown while no trick is open: its last frame, standing on the board
    std::shared_ptr<const Animation> edits; // tracks only; its clip is empty
    float time{};
    bool open{}, play_in_game{}, armed{}, room{};
    float speed{1};
    std::shared_ptr<const CustomTricks> tricks;
};
struct History {
    std::vector<Track> tracks;
    float start{}, end{-1};
};
struct State {
    std::mutex mutex;
    // Document.
    std::shared_ptr<const Clip> clip;
    // The casual stance shown until a trick is picked: the end of the game's mid ollie, standing on the board.
    std::shared_ptr<const Clip> idle_clip;
    bool idle_read{};
    // Record all tricks (trick_batch.h): the next landed trick is saved to the recorded tricks and the studio stays closed.
    bool batch{};
    bool manual_recording{};
    std::optional<BatchTake> batch_take;
    bool record_all_requested{};
    Animation edits;
    std::string saved_as; // the library name it was loaded from or saved to
    bool dirty{};
    std::deque<History> undo, redo;
    std::vector<std::pair<std::uint16_t, Offset>> clipboard;
    // Playback.
    float time{};
    bool playing{}, play_in_game{true}, room{true};
    bool solo{}; // a turn moves that joint alone: the joints below it are turned back by as much
    std::uint64_t clip_serial{}; // how many clips have been opened: the overlay starts clean on each
    std::shared_ptr<const CustomTricks> tricks;
    bool tricks_read{};
    // Mode.
    bool open{}, open_requested{}, open_request_pending{}, confirm_close{};
    Recording recording{};
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
    std::vector<std::string> packs;
    std::optional<PresetPack> pack;
    bool library_read{};
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const Animation> published_edits;
};
State& state() {
    static auto* s = new State;
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

// The animation thread's own, never touched elsewhere.
struct Live {
    std::mutex animation_mutex;
    std::atomic<std::uintptr_t> base{}, component{}, board_holder{};
    std::atomic<std::uint64_t> generation{};
    std::atomic<std::uint64_t> animation_updates{};
    std::uint64_t seen_generation{~std::uint64_t{0}};
    std::vector<NativeBone> bones;
    std::vector<Joint> pose, sampled;
    // Recording.
    struct Frame {
        double at{};
        std::vector<Joint> joints;
        Joint board; // relative to the placement joint
        bool has_board{};
    };
    std::array<NativeBone, board_bones> board{};
    bool board_ok{};
    float predicted_air{}; // from it: how long this air lasts
    std::deque<Frame> ring;
    std::vector<std::vector<Joint>> spare;
    bool airborne{};
    bool recording_seen_ground{};
    ContactWindow contact;
    double takeoff_at{}, landing_at{};
    double last_capture{-1};
    bool recorded_goofy{}, playback_goofy{};
    std::optional<bool> recorded_switch, playback_switch;
    // In-game playback.
    PlaybackEpisode episode;
    MotionState last_motion{MotionState::unknown};
    std::atomic<MotionState> render_motion{MotionState::unknown};
    std::string recorded_entry{"unknown"};
    const CustomTrick* game_trick{}; // the saved trick playing now, from snapshot `game_tricks`
    // Board edit axes at takeoff, relative to actor placement. No recorded board pose is replayed.
    Quat board_frame_local{0, 0, 0, 1};
    bool board_at_takeoff_ok{};

    std::shared_ptr<const CustomTricks> game_tricks;
    std::map<std::string, std::size_t> variant_counts;
    std::map<std::string, std::size_t> attempt_variants;
    std::string stance_fallback_trick;
    std::string recorded_trick; // the game's name of the trick being recorded
    std::vector<Joint> custom;
    // The studio's floor: the skater's Ground joint (375) when the studio opened, which lies on the
    // ground he stands or rides on (measured 2026-10-18: the board's root sits on it while riding, the
    // deck's mount 0.086 m above). Kept while the studio is open; taken again while he is in the air.
    float floor{};
    bool floor_valid{};
    std::shared_ptr<const Clip> floor_clip;
    bool preview_own_jump{};
    std::uint64_t failures{};
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
    bool mirror{}, board_frame_valid{}, board_owned{};
    Quat board_frame_local{0, 0, 0, 1};
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<const Animation> edits;
};
std::atomic<std::uint64_t> gameplay_epoch{};
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
    auto packet = presentation ? packets.presentation.get(profiler::presents(), epoch, packets.latest) : packets.latest;
    if (frame) *frame = packets.presentation.presentation;
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
// While a recording waits or runs: the local skater's pose and its board as they are drawn (after the
// body physics and the feet's placement on the board), taken at their render publication. A recording
// keeps these, so the board and the feet are where the game put them.
std::atomic<bool> capture_flag{};
std::atomic<bool> research_capture_flag{};
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
// A recording starts from a clean level: the game loads it again behind the loading screen, and the skater
// spawns clean (not half in the ground where the last respawn left him). The client tick takes the request,
// reloads, and says when the load is in; no trick is recorded meanwhile.
std::atomic<bool> clean_request{}, clean_hold{};
std::atomic<double> clean_until{};
std::atomic<double> loading_since{}, loading_until{};
Live& live() {
    static auto* l = new Live;
    return *l;
}

std::filesystem::path library_folder() {
    std::array<wchar_t, 32768> buffer{};
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_ANIMATION_STUDIO_DIR", buffer.data(), static_cast<DWORD>(buffer.size()));
        length && length < buffer.size())
        return std::wstring(buffer.data(), length);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.Tricks";
}
std::optional<PresetPack> current_pack() {
    const auto folder = library_folder() / L"presets";
    std::ifstream active(folder / L"current.txt", std::ios::binary);
    std::string name;
    if (!active || !std::getline(active, name) || name.empty()) return std::nullopt;
    if (name != file_safe(name)) throw std::runtime_error("Invalid active pack name");
    std::ifstream in(folder / (name + ".json"), std::ios::binary);
    if (!in) throw std::runtime_error("Active animation pack is missing: " + name);
    auto pack = read_pack_json(Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())));
    pack.name = name;
    return pack;
}
void write_preset_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) throw std::runtime_error("Could not write preset selection");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not replace preset selection");
}
void read_library_locked(State& s) {
    s.library.clear();
    s.presets.clear();
    s.packs.clear();
    s.pack.reset();
    s.library_read = true;
    std::error_code error;
    const auto folder = library_folder();
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.is_regular_file(error) && entry.path().extension() == L".json") s.library.push_back(entry.path().stem().string());
    std::sort(s.library.begin(), s.library.end());
    try {
        s.pack = current_pack();
        for (const auto& name : s.library) {
            try {
                std::ifstream in(folder / (name + ".json"), std::ios::binary);
                auto animation = from_json(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
                if (!animation.trick.empty()) s.presets.push_back({name, animation.trick, animation.pop, false, false, animation.stance, animation.switch_stance, animation.entry});
            } catch (...) {} // A broken animation stays listed for inspection, but is not a preset.
        }
        select_presets(s.presets, s.pack);
        const auto packs = folder / L"presets";
        if (std::filesystem::is_directory(packs, error))
            for (const auto& entry : std::filesystem::directory_iterator(packs, error))
                if (entry.is_regular_file(error) && entry.path().extension() == L".json") s.packs.push_back(entry.path().stem().string());
        std::sort(s.packs.begin(), s.packs.end());
    } catch (const std::exception& e) { s.status = e.what(); }
}

// Every saved animation with a trick, ready to play in game. Read from the files, outside the lock.
std::shared_ptr<const CustomTricks> read_custom_tricks() {
    auto tricks = std::make_shared<CustomTricks>();
    std::error_code error;
    const auto folder = library_folder();
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return tricks;
    // Select from small JSON metadata before expanding clips.
    // An inactive pack variant must not consume full animation memory or block a pack switch.
    std::vector<Preset> requested;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != L".json") continue;
        try {
            std::ifstream in(entry.path(), std::ios::binary);
            const auto animation = from_json(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            auto clip_path = entry.path();
            clip_path.replace_extension(L".clip");
            if (!animation.trick.empty() && std::filesystem::is_regular_file(clip_path, error))
                requested.push_back({entry.path().stem().string(), animation.trick, animation.pop, false, false, animation.stance, animation.switch_stance, animation.entry});
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation preset metadata not loaded: {}: {}", entry.path().filename().string(), e.what());
        }
    }
    try { select_presets(requested, current_pack()); }
    catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Animation pack not loaded: {}", e.what());
        return tricks;
    }
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != L".json") continue;
        if (std::none_of(requested.begin(), requested.end(), [&](const Preset& p) {
            return p.active && p.name == entry.path().stem().string();
        })) continue;
        try {
            std::ifstream json_in(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>());
            auto animation = std::make_shared<Animation>(from_json(text));
            if (animation->trick.empty()) continue;
            auto clip_path = entry.path();
            clip_path.replace_extension(L".clip");
            std::ifstream clip_in(clip_path, std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
            auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
            if (clip->joints != skeleton_joints || clip->takeoff < 0) continue;
            std::string trick;
            for (const char c : animation->trick) trick += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool goofy = animation->stance == "goofy";
            tricks->push_back({trick, entry.path().stem().string(), clip, std::move(animation), goofy});
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: {} was not loaded: {}", entry.path().filename().string(), e.what());
        }
    }
    std::sort(tricks->begin(), tricks->end(), [](const CustomTrick& a, const CustomTrick& b) { return a.name < b.name; });
    std::string names;
    for (const auto& t : *tricks) names += (names.empty() ? "" : ", ") + t.trick;
    logging::log(Level::info, Channel::skater, "Animation Studio: {} custom tricks ready{}{}.", tricks->size(), names.empty() ? "" : ": ", names);
    return tricks;
}

// Publishes what the animation thread works from. Caller holds the mutex.
void publish_locked(State& s, bool edits_changed = true) {
    overrides_allowed.store(s.open || (s.play_in_game && s.recording == Recording::off), std::memory_order_release);
    auto snap = std::make_shared<Snapshot>();
    snap->clip = s.clip;
    snap->idle = s.idle_clip;
    snap->tricks = s.tricks;
    if (edits_changed || !s.published_edits) s.published_edits = std::make_shared<const Animation>(s.edits);
    snap->edits = s.published_edits;
    snap->time = s.time;
    snap->open = s.open;
    snap->play_in_game = s.play_in_game && !s.manual_recording;
    snap->room = s.room;
    snap->armed = s.recording != Recording::off;
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
void changed_locked(State& s) {
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

// ---- recorded tricks -----------------------------------------------------------------------------
// Record all tricks keeps each take as <folder>/recorded/<trick>__<pop>.clip.
std::atomic<std::uint64_t> tricks_revision{1};
std::string recorded_file_name(const std::string& trick, const std::string& pop) {
    std::string safe;
    for (const char c : trick) safe += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    return safe + "__" + (pop.empty() ? std::string("mid") : pop) + ".clip";
}

// ---- recording ---------------------------------------------------------------------------------
constexpr double before_takeoff = 0.7, after_landing = 0.7, ring_seconds = 2.5;
constexpr double shortest_air = 0.25, longest_air = 8.0;

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
// The brackets at the ends of the clip, with their keys on every joint: whenever a clip arrives, by a
// recording or from the game's own library.
void reset_brackets_locked(State& s) {
    if (!s.clip || s.clip->empty()) return;
    ++s.clip_serial; // a clip has arrived: the overlay clears what it was holding
    s.edits.start = 0;
    s.edits.end = -1;
    key_all_at(s.edits, 0);
    key_all_at(s.edits, s.clip->duration());
}
void finish_recording(Live& l) {
    auto clip = std::make_shared<Clip>();
    clip->joints = skeleton_joints;
    const double start = l.takeoff_at - before_takeoff, end = l.landing_at + after_landing;
    double first = -1;
    bool all_board = true;
    for (const auto& frame : l.ring) {
        if (frame.at < start || frame.at > end) continue;
        if (first < 0) first = frame.at;
        clip->times.push_back(static_cast<float>(frame.at - first));
        clip->frames.insert(clip->frames.end(), frame.joints.begin(), frame.joints.end());
        clip->board.push_back(frame.board);
        all_board &= frame.has_board;
    }
    if (!all_board) clip->board.clear();
    if (clip->times.size() < 8 || first < 0) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.recording == Recording::off) return;
        s.status = "The recording was too short. Try again.";
        if (s.batch) {
            s.batch_take = BatchTake{false, {}, {}, "the recording was too short", 0};
            s.recording = Recording::off;
            s.batch = false;
        } else s.recording = Recording::waiting;
        return;
    }
    clip->takeoff = static_cast<float>(std::max(0.0, l.takeoff_at - first));
    clip->landing = static_cast<float>(std::max(0.0, l.landing_at - first));
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.recording == Recording::off) return; // canceled while the clip was assembled
    if (s.batch || s.manual_recording) {
        // Save recognized takes without opening the studio or driving the skater.
        s.batch = false;
        s.recording = s.manual_recording ? Recording::waiting : Recording::off;
        const float air = clip->landing - clip->takeoff;
        // Its pop, as pop_of has it: low under 0.45 s in the air, high from 0.65 s.
        BatchTake take{false, l.recorded_trick, air < 0.45f ? "low" : air < 0.65f ? "mid" : "high", {}, air};
        if (take.trick.empty()) take.message = "the game did not name the trick";
        else if (clip->board.empty()) take.message = "the board was not recorded";
        else {
            try {
                const auto folder = library_folder() / L"recorded";
                std::filesystem::create_directories(folder);
                const auto bytes = clip_bytes(*clip);
                const auto path = folder / std::filesystem::path(recorded_file_name(take.trick, take.pop));
                Animation metadata;
                metadata.name = take.trick;
                metadata.trick = take.trick;
                metadata.pop = take.pop;
                metadata.stance = l.recorded_goofy ? "goofy" : "regular";
                metadata.switch_stance = l.recorded_switch;
                metadata.source = {"render_capture", "", "animation/dingo/animbase_default_skeleton", "skater-placement"};
                metadata.entry = l.recorded_entry;
                metadata.clip.times = {0, clip->duration()};
                metadata.clip.takeoff = clip->takeoff;
                metadata.clip.landing = clip->landing;
                write_preset_file(path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                auto sidecar = path; sidecar.replace_extension(L".json");
                write_preset_file(sidecar, to_json(metadata));
                take.ok = true;
                tricks_revision.fetch_add(1, std::memory_order_acq_rel);
            } catch (const std::exception& e) {
                take.message = e.what();
            }
        }
        logging::log(Level::info, Channel::skater, "Animation Studio: recorded {} ({} pop, {:.2f} s in the air){}{}", take.trick.empty() ? "a trick" : take.trick,
                     take.pop, air, take.ok ? "" : ": not kept, ", take.message);
        if (s.manual_recording) {
            s.status = take.ok ? std::format("Saved {} ({} pop). Skate the next trick; F8 stops recording.", take.trick, take.pop)
                               : "Not kept: " + take.message + ". Waiting for the next trick.";
        } else s.batch_take = std::move(take);
        publish_locked(s);
        return;
    }
    s.clip = std::move(clip);
    // A recording is a new document. Old trick/pop/speed/history must not leak into it.
    s.edits = Animation{};
    s.saved_as.clear();
    s.undo.clear();
    s.redo.clear();
    s.time = 0; // at its start: on the board, on the ground
    s.playing = false;
    s.recording = Recording::off;
    s.edits.stance = l.recorded_goofy ? "goofy" : "regular";
    s.edits.switch_stance = l.recorded_switch;
    s.edits.source = {"render_capture", "", "animation/dingo/animbase_default_skeleton", "skater-placement"};
    s.edits.entry = l.recorded_entry;
    // A new trick starts clean: the old keys and brackets go, the recording is the only thing there is.
    s.edits.tracks.clear();
    reset_brackets_locked(s);
    if (!l.recorded_trick.empty()) {
        // Named after the trick the game says it was: saving it makes it play on that trick.
        s.edits.trick = l.recorded_trick;
        s.edits.name = l.recorded_trick;
    }
    s.open_requested = true;
    s.open_request_pending = true;
    s.dirty = true;
    s.status = std::format("Recorded {} ({:.2f} s in the air). Edit and save its keys to modify the game's {}.",
                           l.recorded_trick.empty() ? std::string("a trick") : l.recorded_trick, s.clip->landing - s.clip->takeoff,
                           l.recorded_trick.empty() ? std::string("trick") : l.recorded_trick);
    publish_locked(s);
    logging::log(Level::info, Channel::skater, "Animation Studio: {}", s.status);
}
void record(Live& l, double at, bool air, const std::string& trick, MotionState previous) {
    // Keep a few seconds of poses; reuse the vectors that fall out.
    std::vector<Joint> joints;
    if (!l.spare.empty()) {
        joints = std::move(l.spare.back());
        l.spare.pop_back();
    }
    joints.assign(l.pose.begin(), l.pose.end());
    double captured_at = -1;
    {
        // As drawn, when the render publication has given it lately: what the game shows, feet on the board.
        auto& shot = render_shot();
        std::lock_guard shot_lock(shot.mutex);
        const auto generation = l.generation.load(std::memory_order_acquire);
        if (shot.skater_generation == generation && fresh_sample(at, shot.at, l.last_capture) && shot.skater.size() == joints.size()) {
            captured_at = shot.at;
            for (std::size_t i = 0; i < joints.size(); ++i) joints[i] = joint_of(shot.skater[i]);
        }
    }
    Live::Frame frame{captured_at, std::move(joints), {}, false};
    if (captured_at >= 0 && frame.joints.size() == skeleton_joints) {
        // The native animation rig already carries the physical mount. Capturing
        // joint 380 keeps board and body in exactly the same publication instead
        // of accepting another board callback up to 50 ms away during a flip.
        frame.board = frame.joints[skateboard_root_joint];
        frame.has_board = finite(frame.board);
    }
    if (captured_at >= 0) {
        l.last_capture = captured_at;
        l.ring.push_back(std::move(frame));
    } else l.spare.push_back(std::move(frame.joints));
    const double keep_from = l.airborne || l.landing_at > 0 ? l.takeoff_at - before_takeoff - 0.1 : at - ring_seconds;
    while (!l.ring.empty() && l.ring.front().at < keep_from) {
        l.spare.push_back(std::move(l.ring.front().joints));
        l.ring.pop_front();
    }
    auto& s = state();
    if (!l.airborne && l.landing_at <= 0) {
        if (!air) l.recording_seen_ground = true;
        if (air && l.recording_seen_ground) {
            l.airborne = true;
            l.takeoff_at = at;
            l.recorded_goofy = goofy_stance();
            l.recorded_switch = switch_stance(l.base.load(std::memory_order_acquire), l.component.load(std::memory_order_acquire));
            l.recorded_entry = entry_name(previous == MotionState::rail ? EntryKind::rail : previous == MotionState::riding ? EntryKind::ground : EntryKind::unknown);
            l.contact.since.reset();
            l.recorded_trick.clear();
            std::lock_guard lock(s.mutex);
            s.recording = Recording::in_air;
            s.status = "In the air...";
        }
        return;
    }
    if (l.airborne) {
        if (air) {
            // The first trick the game names in this air is the one recorded: a flip or tweak after it does not
            // rename the recording.
            if (!trick.empty() && l.recorded_trick.empty() && at - l.takeoff_at < 0.3) l.recorded_trick = trick;
            l.contact.since.reset();
            if (at - l.takeoff_at > longest_air) {
                l.airborne = false;
                l.ring.clear();
                std::lock_guard lock(s.mutex);
                s.recording = Recording::waiting;
                s.status = "That was not a trick (too long in the air). Waiting for the next one.";
                if (s.batch) {
                    s.batch_take = BatchTake{false, {}, {}, "too long in the air", 0};
                    s.recording = Recording::off;
                    s.batch = false;
                }
            }
            return;
        }
        if (!l.contact.observe(at, true)) return;
        l.airborne = false;
        if (*l.contact.since - l.takeoff_at < shortest_air) {
            std::lock_guard lock(s.mutex);
            s.recording = Recording::waiting;
            s.status = "Too short a hop. Waiting for a trick.";
            if (s.batch) {
                s.batch_take = BatchTake{false, {}, {}, "too short a hop", 0};
                s.recording = Recording::off;
                s.batch = false;
            }
            return;
        }
        l.landing_at = *l.contact.since;
        std::lock_guard lock(s.mutex);
        s.recording = Recording::finishing;
        s.status = "Landed. Finishing the recording...";
        return;
    }
    if (at - l.landing_at >= after_landing) {
        {
            std::lock_guard lock(s.mutex);
            if (s.manual_recording) {
                const auto landed = physics_state(l.base.load(std::memory_order_acquire), l.component.load(std::memory_order_acquire));
                if (!landed || (*landed != 100 && *landed != 101)) {
                    l.landing_at = -1;
                    l.ring.clear();
                    s.recording = Recording::waiting;
                    s.status = "The landing did not hold. Waiting for the next trick.";
                    return;
                }
            }
        }
        finish_recording(l);
        l.landing_at = -1;
        l.ring.clear();
    }
}
void reset_recording(Live& l) {
    l.airborne = false;
    l.recording_seen_ground = false;
    l.landing_at = -1;
    l.contact.since.reset();
    l.last_capture = -1;
    for (auto& f : l.ring) l.spare.push_back(std::move(f.joints));
    l.ring.clear();
}

// The local skater's physics state is the one the header declares (defined further down): in the air on the
// board it is 200 or 201, on foot 500 to 599, grinding 400 to 499.
std::optional<bool> in_the_air(std::uintptr_t base, std::uintptr_t component) noexcept {
    const auto value = physics_state(base, component);
    if (!value) return std::nullopt;
    return std::find(addr::skater_state::board_air_states.begin(), addr::skater_state::board_air_states.end(), *value) !=
           addr::skater_state::board_air_states.end();
}
// Grinds are physics states 400 to 499 (no_bail.cpp's selector): on a rail the trick goes on with the rail.
MotionState motion_state(std::optional<std::uint32_t> state) noexcept {
    if (!state) return MotionState::unknown;
    if (*state == addr::no_bail::wipeout_physics_state || *state >= 500) return MotionState::offboard;
    if (*state >= 400 && *state < 500) return MotionState::rail;
    if (*state == 200 || *state == 201) return MotionState::air;
    if (*state == 100 || *state == 101) return MotionState::riding;
    return MotionState::unknown;
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
    return name.size() >= 2 ? name : std::string{};
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
std::optional<float> board_up_speed(std::uintptr_t base, std::uintptr_t component) noexcept {
    namespace st = addr::skater_state;
    const auto core = read<std::uintptr_t>(component + 0x70).value_or(0);
    const auto holder = core ? read<std::uintptr_t>(core + st::board_holder_offset).value_or(0) : 0;
    const auto physics = holder ? read<std::uintptr_t>(holder + st::board_physics_offset).value_or(0) : 0;
    if (!physics || read<std::uintptr_t>(physics).value_or(0) != base + st::board_physics_vtable) return std::nullopt;
    const auto bodies = read<std::uintptr_t>(physics + st::physics_bodies_offset).value_or(0);
    if (!bodies || read<std::uint32_t>(bodies).value_or(0) != st::board_body_count) return std::nullopt;
    if (read<std::uintptr_t>(bodies + st::body_owner_offset).value_or(0) != physics) return std::nullopt;
    const auto v = read<std::array<float, 3>>(bodies + st::body_velocity_offset);
    if (!v || !std::isfinite((*v)[1])) return std::nullopt;
    return (*v)[1];
}
// The pop of an air, from how long it is going to last: low, mid or high.
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
            sample.recording_armed = snap->armed;
        }
        finished = !r.buffer.append(std::move(sample));
    }
    if (finished) logging::write(Level::info, Channel::skater, research_trace("stop"));
}
} // namespace

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
        out << "# animator_trace_schema=2\n# game_sha256=" << supported_build::game_sha256 << '\n';
        out << "seconds,physics_state,phase_hint,trick_name,board_up_mps,studio_open,custom_playback_enabled,goofy_setting,recording_armed,switch_flag,animation_updates,skater_publications,board_publications,skater_age_s,board_age_s\n";
        for (const auto& s : r.buffer.samples) {
            const bool air = s.physics && (*s.physics == 200 || *s.physics == 201);
            const bool rail = s.physics && *s.physics >= 400 && *s.physics < 500;
            out << std::format("{:.6f},", s.seconds);
            if (s.physics) out << *s.physics;
            out << ',' << (!s.physics ? "unavailable" : air ? "air" : rail ? "rail_range" : "other") << ',' << s.trick << ',';
            if (s.board_up_speed) out << *s.board_up_speed;
            out << ',' << s.studio_open << ',' << s.custom_enabled << ',' << s.goofy << ',' << s.recording_armed << ',';
            if (s.switch_stance) out << *s.switch_stance;
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

// The board under the feet, as when riding. The game's clips leave the board where the animators had
// it, and the game's foot placement puts the feet on it: before the pop and after the landing their
// feet sink into the board or float over it by up to 6 cm (measured 2026-10-18 in the kickflip and
// the ollie). Riding, the feet (joints 343 and 10) are 0.104 m above the board's mount. So on the
// ground the board goes that far under the lower foot; in the air it is the clip's own, with a short
// blend at the pop and the landing. `pose` is the clip's pose at `seconds`, `board` its board, both
// under the placement joint.
struct BoardShape;
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
Joint start_board(const Clip& clip, bool fit_contact) {
    Joint board;
    if (Joint rel; clip.sample_board(0, rel)) board = fit_contact ? board_on_feet(clip, 0, clip.frame(0), rel) : rel;
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
struct BoardShape {
    std::array<Vec3, 6> points{};
    Vec3 up{0, 1, 0}; // the board's up, in its mount's frame
};
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
// The lowest point of the board `board` (a mount in the world): what touches the ground first.
float lowest_point(const Joint& board, const BoardShape& shape) noexcept {
    float low = board.position[1];
    for (const auto& pt : shape.points) low = std::min(low, board.position[1] + rotate(board.rotation, pt)[1]);
    return low;
}

// ---- the animation thread ------------------------------------------------------------------------
void on_animation(std::uintptr_t component) noexcept {
    auto& l = live();
    std::lock_guard animation_lock(l.animation_mutex);
    if (!component || component != l.component.load(std::memory_order_acquire)) return;
    const auto error = GetLastError();
    try {
        const auto generation = l.generation.load(std::memory_order_acquire);
        if (l.seen_generation != generation) {
            reset_recording(l);
            l.episode.reset();
            l.last_motion = MotionState::unknown;
            l.game_trick = nullptr;
            l.game_tricks.reset();
            l.attempt_variants.clear();
            l.floor_valid = false;
            l.floor_clip = nullptr;
            l.logged_layout = false;
            l.seen_generation = generation;
        }
        const auto snap = snapshot();
        if (!snap || (!diagnostics_capture && !snap->open && !snap->armed && !snap->play_in_game && !research_capture_flag.load(std::memory_order_acquire))) {
            invalidate_outputs();
            l.episode.reset();
            l.game_trick = nullptr;
            SetLastError(error);
            return;
        }
        if (snap->open || snap->armed || !snap->play_in_game) {
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
        const auto previous_motion = l.last_motion;
        const auto motion = motion_state(current_state);
        l.last_motion = motion;
        l.render_motion.store(motion, std::memory_order_release);
        const auto air = in_the_air(base, component);
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
            diagnostic.armed = snap->armed;
            if (diagnostic_pose) diagnostic.native = l.pose;
            if (l.board_ok) diagnostic.native_board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
        }
        const bool bailing = current_state && (*current_state == addr::no_bail::wipeout_physics_state ||
                                              *current_state == addr::no_bail::offboard_physics_state);
        if (!current_state && !snap->open) {
            l.episode.reset();
            l.game_trick = nullptr;
            if (snap->armed) reset_recording(l);
        }
        if (bailing && !snap->open) {
            l.episode.reset();
            l.game_trick = nullptr;
            if (snap->armed) {
                reset_recording(l);
                auto& s = state();
                std::lock_guard lock(s.mutex);
                s.recording = Recording::waiting;
                s.status = "Bail or off-board: waiting for a clean trick.";
            }
        }
        if (clean_hold.load(std::memory_order_acquire) && now_seconds() > clean_until.load(std::memory_order_relaxed)) clean_hold.store(false, std::memory_order_release);
        if (snap->armed && !snap->open && !bailing && air && !clean_hold.load(std::memory_order_acquire))
            record(l, at, *air, *air ? current_trick(component) : std::string{}, previous_motion);
        else if (!snap->armed) reset_recording(l);

        // Which joints to write back, and from what.
        std::size_t first = 0, last = 0; // Studio snapshots only. Gameplay publishes an edit schedule.
        std::shared_ptr<const GameplayPacket> scheduled;
        Joint board_target;
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
        const bool has_trick = snap->clip && !snap->clip->empty();
        const Clip* shown = has_trick ? snap->clip.get() : snap->idle && !snap->idle->empty() ? snap->idle.get() : nullptr;
        const float shown_time = has_trick ? snap->time : shown ? shown->duration() : 0.0f;
        static const Animation no_edits;
        const Animation& shown_edits = has_trick ? *snap->edits : no_edits;
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
            // A new trick starts from where the skater is now: his floor is taken again.
            if (shown != l.floor_clip.get()) {
                l.floor_clip = has_trick ? snap->clip : snap->idle;
                l.preview_own_jump = clip_has_vertical_trajectory(*shown);
                l.floor_valid = false;
            }
            if (!l.floor_valid || (air && *air)) {
                l.floor = world(l.pose, ground_joint).position[1];
                l.floor_valid = std::isfinite(l.floor);
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
                return clip.sample_board(t, rel) ? lowest_point(compose(ait, fit_preview_contact ? board_on_feet(clip, t, tmp, rel) : rel), shape)
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
                    const float arc = l.preview_own_jump ? 0.0f : 9.81f * air_time * air_time / 8.0f * 4 * u * (1 - u);
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
            // Nothing in his hands: the game hangs props (the phone of its idle, held still while the studio is
            // open) on the hands' attach joints, so those are made tiny and what hangs on them with them.
            for (const std::uint16_t attach : {right_hand_attach, left_hand_attach})
                if (attach < l.pose.size()) l.pose[attach].scale = {0.001f, 0.001f, 0.001f};
            first = in_room ? placement_joint : 2;
            last = layout.count;
            // The board as the recording had it, the same way the body hangs from the placement joint.
            if (Joint rel; shown->sample_board(shown_time, rel)) {
                if (fit_preview_contact) rel = board_on_feet(*shown, shown_time, l.sampled, rel);
                const auto moved = rotate(turn, rel.position);
                for (std::size_t a = 0; a < 3; ++a) rel.position[a] = rise[a] + moved[a];
                rel.rotation = normalised(multiply(turn, rel.rotation));
                const auto unedited = compose(live_placement, rel);
                const auto origin = start_frame(*shown, live_placement, (l.floor_valid ? l.floor : live_placement.position[1]) + deck, fit_preview_contact);
                board_target = board_edited(unedited, shown_edits, shown_time, origin.rotation);
                board_frame = origin.rotation;
                board_weight = 1;
                auto& st = state();
                std::lock_guard base_lock(st.mutex);
                st.board_base = relative(origin, unedited);
                st.board_base_valid = has_trick && finite(st.board_base);
            }
        } else if (snap->open) {
            // No trick loaded yet: the live pose, with the edits of the start of the timeline.
            apply_edits(*snap->edits, 0, l.pose);
            first = in_room ? placement_joint : 2;
            last = layout.count;
        } else if (snap->play_in_game && !snap->armed && !bailing && snap->tricks && !snap->tricks->empty()) {
            const bool began = l.episode.observe(at, motion);
            if (began) {
                l.playback_goofy = goofy_stance();
                l.playback_switch = switch_stance(base, component);
                l.game_trick = nullptr; l.game_tricks = snap->tricks;
                l.attempt_variants.clear(); l.stance_fallback_trick.clear(); l.predicted_air = 0;
                l.board_at_takeoff_ok = false;
                if (l.board_ok) {
                    const auto mount = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                    if (finite(mount)) {
                        l.board_frame_local = normalised(multiply(conjugate(live_placement.rotation), ground_turn(mount)));
                        l.board_at_takeoff_ok = true;
                    }
                }
            }
            if (l.episode.phase == PlaybackPhase::idle) l.game_trick = nullptr;
            if (l.episode.phase == PlaybackPhase::air && l.predicted_air == 0 && at - l.episode.takeoff >= 0.03) {
                const double elapsed = at - l.episode.takeoff;
                const auto vy = board_up_speed(base, component);
                l.predicted_air = vy && *vy > 0.3f
                    ? static_cast<float>(std::clamp(2 * (*vy + 9.81 * elapsed) / 9.81 - elapsed, 0.15, 3.0)) : -1.0f;
            }
            const auto name = l.episode.selection_open(at) ? current_trick(component) : std::string{};
            if (l.episode.entry != EntryKind::unknown && l.game_tricks && episode_selection_changed(l.game_trick ? l.game_trick->trick : std::string_view{}, name)) {
                const auto pop = pop_of(l.predicted_air);
                const CustomTrick* found = nullptr;
                // Exact pop first; cycle eligible variants once per named trick in this attempt.
                std::vector<const CustomTrick*> variants;
                const auto compatible = [&](const CustomTrick& t) {
                    return playback_stance_compatible(t.goofy, t.edits->switch_stance, l.playback_goofy, l.playback_switch);
                };
                int best_pop = 3;
                for (const auto& t : *l.game_tricks) {
                    if (t.trick != name || t.edits->entry != entry_name(l.episode.entry) || !l.playback_switch.has_value()) continue;
                    const int rank = playback_pop_rank(pop, t.edits->pop);
                    if (rank < best_pop) { variants.clear(); best_pop = rank; }
                    if (rank == best_pop) variants.push_back(&t);
                }
                // A recorded switch variant wins over converting another stance's edits.
                if (std::any_of(variants.begin(), variants.end(), [&](const CustomTrick* t) { return compatible(*t); }))
                    std::erase_if(variants, [&](const CustomTrick* t) { return !compatible(*t); });
                if (!variants.empty()) {
                    auto choice = l.attempt_variants.find(name);
                    if (choice == l.attempt_variants.end())
                        choice = l.attempt_variants.emplace(name, l.variant_counts[name + "|" + entry_name(l.episode.entry)]++).first;
                    found = variants[choice->second % variants.size()];
                } else if (!name.empty() && l.stance_fallback_trick != name &&
                           std::any_of(l.game_tricks->begin(), l.game_tricks->end(), [&](const CustomTrick& t) {
                               return t.trick == name && !compatible(t);
                           })) {
                    l.stance_fallback_trick = name;
                    logging::log(Level::info, Channel::skater,
                                 "Animation Studio: {} uses native animation; no compatible recording for {} / {} at takeoff.", name,
                                 l.playback_goofy ? "goofy" : "regular",
                                 l.playback_switch ? (*l.playback_switch ? "switch" : "normal") : "unknown switch state");
                }
                if (found && found != l.game_trick) {
                    l.episode.select(at);
                    l.game_trick = found;
                    if (found)
                        logging::log(Level::info, Channel::skater, "Animation Studio: the game does a {} ({} pop, {:.2f} s in the air), playing your {} ({}).", name,
                                     pop.empty() ? "unknown" : pop, l.predicted_air, found->name,
                                     "authored edits over fresh native pose");
                }
            }
            if (const auto* trick = l.game_trick) {
                const auto& played = *trick->edits;
                const auto& clip = *trick->clip;
                const bool mirror = (trick->goofy != played.switch_stance.value_or(false)) !=
                    (l.playback_goofy != l.playback_switch.value_or(false));
                const auto sampled = l.episode.sample(at, clip.takeoff, clip.landing, clip.duration(), std::max(played.speed, 0.1f), l.predicted_air);
                const float weight = sampled.weight * animation_window_weight(played, sampled.time);
                diagnostic.preset = trick->name;
                diagnostic.source_kind = played.source.kind;
                diagnostic.recorded_goofy = trick->goofy; diagnostic.recorded_switch = played.switch_stance;
                diagnostic.retarget = true; diagnostic.mirror = mirror;
                diagnostic.clip_time = sampled.time; diagnostic.weight = weight;
                if (sampled.time >= 0 && !sampled.finished) {
                    auto packet = std::make_shared<GameplayPacket>();
                    packet->generation = generation; packet->episode = l.episode.id;
                    packet->sequence = l.animation_updates.load(); packet->epoch = gameplay_epoch.load(std::memory_order_acquire);
                    packet->component = component; packet->board_holder = l.board_holder.load(std::memory_order_acquire);
                    packet->at = at; packet->time = sampled.time; packet->weight = weight; packet->mirror = mirror;
                    packet->clip = trick->clip; packet->edits = trick->edits;
                    packet->board_frame_local = l.board_frame_local; packet->board_frame_valid = l.board_at_takeoff_ok;
                    packet->board_owned = l.episode.phase != PlaybackPhase::rail_release;
                    scheduled = std::move(packet);
                    // Diagnostic prediction only. The renderer evaluates these edits again over its
                    // fresh post-physics native pose; no body/board snapshot is queued for gameplay.
                    if (diagnostic_pose) {
                        l.sampled.resize(layout.count); clip.sample(sampled.time, l.sampled);
                        apply_native_edit_layer(played, sampled.time, l.sampled, l.pose, mirror, weight);
                    }
                    if (scheduled->board_owned && scheduled->board_frame_valid && l.board_ok) {
                        Joint frame; frame.rotation = normalised(multiply(live_placement.rotation, l.board_frame_local));
                        const auto native_board = compose(board_parent(l.board), joint_of(l.board[board_mount]));
                        board_target = board_edited(native_board, played, sampled.time, ground_turn(frame), mirror);
                        board_weight = weight;
                    }
                }
                if (sampled.finished) l.game_trick = nullptr;
            }
        }
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
        }
    } catch (const std::exception& e) {
        invalidate_outputs();
        if (l.failures++ % 600 == 0) logging::log(Level::warning, Channel::skater, "Animation Studio: {}", e.what());
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.failure = e.what();
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
};
BodyPublicationState& body_publication_state() { static auto* state = new BodyPublicationState; return *state; }
void on_send(std::uintptr_t component, std::vector<multiplayer::Transform>& skater) noexcept {
    try {
        if (component != live().component.load(std::memory_order_acquire) || is_open() || skater.size() < skeleton_joints) return;
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

void apply_gameplay_body(std::uintptr_t holder, const std::shared_ptr<const GameplayPacket>& packet, std::uint64_t presentation) {
    auto& l = live();
    const auto generation = l.generation.load(std::memory_order_acquire);
    const auto update = l.animation_updates.load(std::memory_order_acquire);
    const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(), holder, 512);
    if (!layout.buffer || layout.count != skeleton_joints) return;
    auto& publication = body_publication_state(); std::lock_guard lock(publication.mutex);
    std::vector<NativeBone> current(layout.count);
    if (!peek(layout.buffer, current.data(), current.size() * sizeof(NativeBone))) return;
    std::vector<Joint> native; native.reserve(current.size());
    for (const auto& bone : current) native.push_back(joint_of(bone));
    publication.edits.restore(generation, update, layout.buffer, native);
    auto edited = native;
    if (packet) {
        std::vector<Joint> source(packet->clip->joints); packet->clip->sample(packet->time, source);
        apply_native_edit_layer(*packet->edits, packet->time, source, edited, packet->mirror, packet->weight);
    }
    const auto desired_deltas = pose_edit_deltas(native, edited);
    edited = native;
    std::vector<Offset> applied_deltas;
    const auto motion = l.render_motion.load(std::memory_order_acquire);
    if (overrides_allowed.load() && motion != MotionState::unknown && motion != MotionState::offboard) {
        const auto deltas = publication.transition.evaluate(now_seconds(), generation, packet ? packet->episode : 0,
            packet ? reinterpret_cast<std::uintptr_t>(packet->edits.get()) : 0, desired_deltas);
        apply_pose_deltas(deltas, edited);
        applied_deltas.assign(deltas.begin(), deltas.end());
    } else publication.transition.reset();
    if (generation != l.generation.load(std::memory_order_acquire) || update != l.animation_updates.load(std::memory_order_acquire) ||
        (packet && packet->epoch != gameplay_epoch.load(std::memory_order_acquire))) return;
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
        if (diagnostics_capture || capture_flag.load(std::memory_order_acquire) || research_capture_flag.load(std::memory_order_acquire)) {
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

struct BoardPublicationState { std::mutex mutex; EditPublicationCache edits; EditTransition transition; };
BoardPublicationState& board_publication_state() { static auto* state = new BoardPublicationState; return *state; }
void apply_gameplay_board(std::uintptr_t holder, const std::shared_ptr<const GameplayPacket>& packet, std::uint64_t presentation) {
    auto& l = live();
    const auto generation = l.generation.load(std::memory_order_acquire), update = l.animation_updates.load(std::memory_order_acquire);
    const auto layout = multiplayer::read_native_pose_layout(peek, l.base.load(), holder, 64);
    if (!layout.buffer || layout.count != board_bones) return;
    auto& publication = board_publication_state(); std::lock_guard lock(publication.mutex);
    std::array<NativeBone, board_bones> bones{};
    if (!peek(layout.buffer, bones.data(), sizeof(bones))) return;
    std::vector<Joint> native; for (const auto& bone : bones) native.push_back(joint_of(bone));
    publication.edits.restore(generation, update, layout.buffer, native);
    auto edited = native;
    const auto native_mount = compose(compose(native[0], native[1]), native[board_mount]);
    Joint want = native_mount;
    if (packet && packet->board_holder == holder && packet->board_owned && packet->board_frame_valid) {
        const auto body_holder = read<std::uintptr_t>(packet->component + 0xa0).value_or(0);
        const auto body_layout = multiplayer::read_native_pose_layout(peek, l.base.load(), body_holder, 512);
        std::array<NativeBone, 2> placement_bones{};
        if (body_layout.buffer && body_layout.count == skeleton_joints && peek(body_layout.buffer, placement_bones.data(), sizeof(placement_bones))) {
            const auto placement = compose(joint_of(placement_bones[0]), joint_of(placement_bones[1]));
            Joint frame; frame.rotation = normalised(multiply(placement.rotation, packet->board_frame_local));
            const auto target = board_edited(native_mount, *packet->edits, packet->time, ground_turn(frame), packet->mirror);
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
            packet ? reinterpret_cast<std::uintptr_t>(packet->edits.get()) : 0, desired)[0];
        if (delta.rotation != Quat{0, 0, 0, 1}) want.rotation = normalised(multiply(delta.rotation, native_mount.rotation));
        for (std::size_t k = 0; k < 3; ++k) want.position[k] += delta.offset[k];
    } else publication.transition.reset();
    if (want != native_mount) {
        edited[1] = relative(native[0], compose(want, relative(native[board_mount], Joint{})));
        edited[1].scale = native[1].scale;
    }
    if (generation != l.generation.load(std::memory_order_acquire) || update != l.animation_updates.load(std::memory_order_acquire) ||
        (packet && packet->epoch != gameplay_epoch.load(std::memory_order_acquire))) return;
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
    publication.edits.commit(generation, update, layout.buffer, std::move(native), std::move(edited));
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
        if (bh && animation_interface == bh + 0xc0 && (diagnostics_capture || capture_flag.load(std::memory_order_acquire) || research_capture_flag.load(std::memory_order_acquire))) {
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
                        if (diagnostics_capture) {
                            auto& shot = render_shot(); std::lock_guard lock(shot.mutex);
                            shot.edited_board = compose(board_parent(b), joint_of(b[board_mount]));
                            shot.edited_board_at = now_seconds(); shot.edited_board_generation = generation;
                            shot.edited_board_sequence = packet->sequence; shot.edited_board_presentation = presentation;
                        }
                    }
                }
            }
        }
    } catch (...) { invalidate_outputs(); }
    SetLastError(error);
}

// ---- the engine thread ---------------------------------------------------------------------------
std::shared_ptr<Clip> read_game_clip(const GameTrick& trick); // a clip of the trick library, below
void manual_recording_request(std::uintptr_t component, bool open) {
    static ULONGLONG checked{};
    const auto now = GetTickCount64();
    if (now - checked < 250) return;
    checked = now;
    try {
        const auto folder = library_folder();
        const auto request = folder / L"manual-recording.request";
        std::ifstream input(request);
        if (!input) return;
        std::string command, token;
        input >> command >> token;
        input.close();
        std::error_code ec;
        if (!std::filesystem::remove(request, ec) || token.empty()) return;
        std::string message;
        const bool busy = batch::active();
        auto& s = state();
        {
            std::lock_guard lock(s.mutex);
            if (command == "stop") {
                s.manual_recording = false;
                if (!s.batch) s.recording = Recording::off;
                message = "OK Manual recording stopped. Open F8 / TRICKS to see your takes.";
            } else if (command != "start") message = "ERROR Unknown recording request.";
            else if (!component || open) message = "ERROR Close Animator Studio and enter solo play on your board, then try again.";
            else if (multiplayer_session_active()) message = "ERROR Manual recording is available in solo play only.";
            else if (busy || s.batch || (!s.manual_recording && s.recording != Recording::off)) message = "ERROR Another recording is active. Finish or cancel it first.";
            else {
                s.manual_recording = true;
                s.recording = Recording::waiting;
                s.batch_take.reset();
                s.open_requested = false;
                message = "OK Manual recording ON. Do your own tricks; each recognized take is saved by pop. F8 stops recording.";
            }
            s.status = message;
            publish_locked(s);
        }
        std::ofstream response(folder / L"manual-recording.response", std::ios::trunc);
        response << token << '\n' << message << '\n';
        logging::log(Level::info, Channel::skater, "Animation Studio: {}", message);
    } catch (const std::exception& e) {
        logging::log(Level::warning, Channel::skater, "Manual recording request failed: {}", e.what());
    }
}
TickResult tick(std::uintptr_t base, std::uintptr_t component, bool open, float camera_fov) {
    research_tick(base, component, open);
    auto& l = live();
    auto& s = state();
    TickResult result;
    bool needs_listener{};
    {
        std::lock_guard lock(s.mutex);
        if (!s.snapshot) publish_locked(s);
        if (s.manual_recording && multiplayer_session_active()) {
            s.manual_recording = false;
            s.recording = Recording::off;
            s.status = "Manual recording stopped: solo play only.";
            publish_locked(s);
        }
        open_flag.store(open, std::memory_order_release);
        const bool room_now = open && in_room_flag.load(std::memory_order_acquire);
        if (room_flag.exchange(room_now, std::memory_order_acq_rel) && !room_now)
            room_restore_until.store(now_seconds() + 3, std::memory_order_release);
        if (s.open != open) {
            if (open && s.manual_recording) {
                s.manual_recording = false;
                s.recording = Recording::off;
                s.status = "Manual recording stopped. Your saved takes are in TRICKS.";
            }
            s.open = open;
            s.base_yaw_valid = false;
            s.eased_valid = false;
            s.playing = false;
            publish_locked(s);
        }
        if (std::isfinite(camera_fov) && camera_fov > 1 && camera_fov < 175) s.fov = camera_fov;
        capture_flag.store(s.recording != Recording::off, std::memory_order_release);
        needs_listener = diagnostics_capture || research_capture_flag.load(std::memory_order_acquire) || s.open || room_watch.load() || s.recording != Recording::off || (s.play_in_game && s.tricks && !s.tricks->empty());
        result.want_open = s.open_requested;
        result.changed = s.open_request_pending;
        s.open_request_pending = false;
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
        bool read{};
        {
            std::lock_guard lock(s.mutex);
            read = !s.tricks_read;
            s.tricks_read = true;
        }
        if (read) {
            auto tricks = read_custom_tricks();
            std::lock_guard lock(s.mutex);
            s.tricks = std::move(tricks);
            publish_locked(s);
        }
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
                const auto pick = std::find_if(list.begin(), list.end(), [](const GameTrick& t) { return t.trick == "ollie" && t.pop == "mid"; });
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
            multiplayer::set_local_pose_filter(&on_send);
            installed = true;
            logging::log(Level::info, Channel::skater, "Animation Studio: listening to the skater's animation.");
        } else {
            std::lock_guard lock(s.mutex);
            s.failure = "The animation hook is unavailable: " + detail;
        }
    }
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
    const Vec3 centre{s.eased_centre[0] + s.focus_offset[0], s.eased_centre[1] + s.focus_offset[1], s.eased_centre[2] + s.focus_offset[2]};
    if (!s.base_yaw_valid) {
        s.base_yaw = facing_yaw(s.facing);
        s.base_yaw_valid = true;
    }
    auto m = orbit_matrix(centre, s.orbit, s.base_yaw);
    if (s.fov > 1 && s.fov < 175 && (s.view_shift != 0 || s.view_shift_x != 0)) {
        // Aim below and beside the centre so it shows that far above and right of the screen's middle.
        const float per_share = std::clamp(s.orbit.distance, 0.6f, 25.0f) * 2 * std::tan(s.fov * 3.14159265f / 360);
        const float up = s.view_shift * per_share, side = s.view_shift_x * per_share;
        const Vec3 aimed{centre[0] - m[4] * up - m[0] * side, centre[1] - m[5] * up - m[1] * side, centre[2] - m[6] * up - m[2] * side};
        m = orbit_matrix(aimed, s.orbit, s.base_yaw);
    }
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) matrix[i] = m[i];
    s.camera = matrix;
    s.camera_valid = true;
    return true;
}

// ---- the overlay -----------------------------------------------------------------------------------
bool is_open() noexcept { return open_flag.load(std::memory_order_acquire); }
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
        // A clean start holds it all the way: the walk out of the room, the level loading behind it and the
        // skater arriving are one screen, instead of ours ending and the game's own coming up after it.
        const bool clean = clean_hold.load(std::memory_order_acquire);
        if ((!clean && quiet >= 0 && now - quiet > 1.5) || now - since > longest) {
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
View view() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.library_read) read_library_locked(s);
    View v;
    v.open = s.open;
    v.board_base = s.board_base;
    v.board_base_valid = s.board_base_valid && s.open;
    v.pose_seen = s.pose_seen_at > 0 && now_seconds() - s.pose_seen_at < 1.0;
    v.status = s.status;
    v.failure = s.failure;
    v.camera = s.camera;
    v.fov = s.fov;
    v.camera_valid = s.camera_valid;
    v.name = s.edits.name;
    v.trick = s.edits.trick;
    v.source_kind = s.edits.source.kind; v.source_asset = s.edits.source.asset; v.entry = s.edits.entry;
    v.source_position_codec = s.edits.source.position_codec;
    if (s.clip) {
        v.source_pose_count = s.clip->times.size();
        for (std::size_t i = 1; i < s.clip->times.size(); ++i)
            v.source_max_sample_gap = std::max(v.source_max_sample_gap, s.clip->times[i] - s.clip->times[i-1]);
    }
    v.has_clip = s.clip && !s.clip->empty();
    v.dirty = s.dirty;
    v.playing = s.playing;
    v.loop = s.edits.loop;
    v.play_in_game = s.play_in_game;
    v.room = s.room;
    v.solo = s.solo;
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
    v.duration = duration_locked(s);
    v.takeoff = s.clip ? s.clip->takeoff : -1;
    v.landing = s.clip ? s.clip->landing : -1;
    v.time = s.time;
    v.speed = s.edits.speed;
    v.joints = s.joints;
    for (const auto& t : s.edits.tracks) {
        TrackView tv{t.joint, {}, t.hidden, t.locked};
        for (const auto& k : t.keys) tv.keys.push_back({k.time, k.ease, k.smooth});
        v.tracks.push_back(std::move(tv));
    }
    v.library = s.library;
    v.presets = s.presets;
    v.packs = s.packs;
    v.active_pack = s.pack ? s.pack->name : "";
    v.alternate_variants = s.pack && s.pack->alternate;
    v.recording = s.recording;
    v.bracket_start = s.edits.start;
    v.bracket_end = s.edits.end;
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
void request_close() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.dirty && s.clip && !s.clip->empty()) {
        s.confirm_close = true;
        ++s.revision;
        return;
    }
    s.open_requested = false;
    s.open_request_pending = true;
}
void answer_close(CloseAnswer answer) {
    std::string message;
    if (answer == CloseAnswer::save && !save(message)) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.confirm_close = false;
        return;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.confirm_close = false;
    if (answer == CloseAnswer::cancel) return;
    s.open_requested = false;
    s.open_request_pending = true;
}
void set_pop(const std::string& pop) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.pop = pop == "low" || pop == "mid" || pop == "high" ? pop : std::string{};
    s.dirty = true;
    ++s.revision;
}
bool copy_to_pops(std::string& message) {
    std::shared_ptr<const Clip> clip;
    Animation edits;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.clip || s.clip->empty() || s.edits.trick.empty()) {
            message = "Record a trick first.";
            return false;
        }
        clip = s.clip;
        edits = s.edits;
    }
    // The same keys for each pop: in game the air of each is stretched to its own, so pop and landing
    // fall where they should. Each copy can then be changed on its own.
    std::string base = file_safe(edits.trick);
    const auto folder = library_folder();
    try {
        std::filesystem::create_directories(folder);
        const auto bytes = clip_bytes(*clip);
        const std::string binary(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        for (const char* pop : {"low", "mid", "high"}) {
            Animation copy = edits;
            copy.pop = pop;
            const std::string suffix = std::string(" ") + pop;
            const auto stem = base.substr(0, 48 - suffix.size()) + suffix;
            copy.name = stem;
            unsigned number = 2;
            while (std::filesystem::exists(folder / (copy.name + ".json")) || std::filesystem::exists(folder / (copy.name + ".clip")))
                copy.name = variation_name(stem, number++);
            copy.clip.times = {0, clip->duration()};
            copy.clip.takeoff = clip->takeoff;
            copy.clip.landing = clip->landing;
            const auto json = to_json(copy);
            write_preset_file(folder / (copy.name + ".clip"), binary);
            write_preset_file(folder / (copy.name + ".json"), json);
        }
    } catch (const std::exception& e) {
        message = std::string("Copying failed: ") + e.what();
        return false;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    read_library_locked(s);
    s.tricks_read = false;
    message = "Saved new low, mid and high variations of " + base + "; existing animations were preserved.";
    s.status = message;
    return true;
}
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
    changed_locked(s);
}
bool take_clean_start() noexcept { return clean_request.exchange(false, std::memory_order_acq_rel); }
void clean_start_done() noexcept { clean_hold.store(false, std::memory_order_release); }
void arm_batch_take() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.batch = true;
    s.batch_take.reset();
    s.recording = Recording::waiting;
    publish_locked(s);
}
void cancel_batch_take() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.batch) return;
    s.batch = false;
    s.recording = Recording::off;
    publish_locked(s);
}
std::optional<BatchTake> take_batch_result() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return std::exchange(s.batch_take, std::nullopt);
}
void request_record_all() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.record_all_requested = true;
}
bool take_record_all_request() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return std::exchange(s.record_all_requested, false);
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
void record_next_trick() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.recording = Recording::waiting;
    s.status = "Starting clean: loading the level, then skate and do the trick.";
    s.open_requested = false;
    // The level loads again first, behind the loading screen, so the skater starts clean.
    clean_until.store(now_seconds() + 30.0, std::memory_order_relaxed);
    clean_hold.store(true, std::memory_order_release);
    clean_request.store(true, std::memory_order_release);
    set_loading(Loading::opening, 4.0f);
    s.open_request_pending = true;
    publish_locked(s);
    logging::log(Level::info, Channel::skater, "Animation Studio: waiting to record the next trick.");
}
void cancel_recording() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    clean_hold.store(false, std::memory_order_release);
    s.recording = Recording::off;
    s.status = "Recording cancelled.";
    publish_locked(s);
}
void set_room(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.room = on;
    s.eased_valid = false;
    s.status = on ? "From the next time it opens, the studio opens in its room." : "From the next time it opens, the studio opens where you are.";
    publish_locked(s);
}
void set_play_in_game(bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.play_in_game = on;
    s.status = on ? "Your saved keys modify native tricks. Full source motion is previewed in the studio." : "Tricks play as the game has them.";
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
    s.undo.push_back({s.edits.tracks, s.edits.start, s.edits.end});
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
    const float at = frame_time(s.time);
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
    const float at = frame_time(s.time);
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
    set_key(s.edits, joint, frame_time(s.time), clean);
    if (!children.empty()) hold_facing(s, children, facing);
    changed_locked(s);
}
namespace {
// The view's frame and the clip's, through the placement joint: a point or a direction is moved between them.
Vec3 to_clip(const Joint& view_place, const Joint& clip_place, const Vec3& p) {
    const Vec3 local = rotate(conjugate(view_place.rotation), {p[0] - view_place.position[0], p[1] - view_place.position[1], p[2] - view_place.position[2]});
    const Vec3 moved = rotate(clip_place.rotation, local);
    return {moved[0] + clip_place.position[0], moved[1] + clip_place.position[1], moved[2] + clip_place.position[2]};
}
Vec3 to_view(const Joint& view_place, const Joint& clip_place, const Vec3& p) {
    const Vec3 local = rotate(conjugate(clip_place.rotation), {p[0] - clip_place.position[0], p[1] - clip_place.position[1], p[2] - clip_place.position[2]});
    const Vec3 moved = rotate(view_place.rotation, local);
    return {moved[0] + view_place.position[0], moved[1] + view_place.position[1], moved[2] + view_place.position[2]};
}
} // namespace

// Both in the clip's own frame: the field is where the joint is now, from where it rests, in the skater's axes.
Vec3 limb_position(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.clip || s.clip->empty() || !editable(joint)) return {};
    const float at = frame_time(s.time);
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
    if (!s.clip || s.clip->empty() || !editable(joint)) return;
    if (const auto* t = s.edits.track(joint); t && t->locked) {
        s.status = "That track is locked.";
        return;
    }
    const auto chain = limb_chain(joint);
    if (!chain) return;
    const float at = frame_time(s.time);
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
    changed_locked(s);
}

void move_bracket(int which, float seconds) {
    if (!std::isfinite(seconds)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
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
    if (!move_window_edge(s.edits, which, which == 1 ? start : end, duration)) return;
    key_all_at(s.edits, s.edits.start);
    key_all_at(s.edits, s.edits.end < 0 ? duration : s.edits.end);
    changed_locked(s);
}

void drag_limb(std::uint16_t joint, const Vec3& target_view) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.clip || s.clip->empty() || !editable(joint)) return;
    if (const auto* t = s.edits.track(joint); t && t->locked) {
        s.status = "That track is locked.";
        return;
    }
    const float at = frame_time(s.time);
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
    changed_locked(s);
}
void delete_key(std::uint16_t joint, float time) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (const auto* track = s.edits.track(joint); track && track->locked) return;
    if (anim_studio::remove_key(s.edits, joint, time, 1e-3f)) changed_locked(s);
}
void move_key(std::uint16_t joint, float from, float to) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
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
    if (auto* t = s.edits.track(joint); t && !t->locked)
        for (auto& k : t->keys)
            if (std::abs(k.time - time) < 1e-3f) {
                k.ease = ease;
                changed_locked(s);
            }
}
void smooth_keys(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const float end = s.edits.end < 0 ? (s.clip ? s.clip->duration() : 0.0f) : s.edits.end;
    std::size_t changed = 0, tracks = 0;
    for (auto& t : s.edits.tracks) {
        if (t.locked || (joint != all_joints && t.joint != joint)) continue;
        if (const auto n = anim_studio::smooth_track(t, s.edits.start, end)) {
            changed += n;
            ++tracks;
        }
    }
    s.status = changed ? std::format("Smoothed {} key(s) on {} track(s).", changed, tracks) : "Nothing to smooth here.";
    if (changed) changed_locked(s);
    else publish_locked(s);
}
void reset_joint(std::uint16_t joint) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.edits.tracks, [&](const Track& t) { return t.joint == joint && !t.locked; });
    changed_locked(s);
}
void set_track_flags(std::uint16_t joint, bool hidden, bool locked) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
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
void paste_pose() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.clipboard.empty()) return;
    for (const auto& [joint, value] : s.clipboard) {
        const auto* t = s.edits.track(joint);
        if (!t || !t->locked) set_key(s.edits, joint, frame_time(s.time), value);
    }
    s.status = std::format("Pasted the pose of {} joints.", s.clipboard.size());
    changed_locked(s);
}
void undo() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.undo.empty()) return;
    s.redo.push_back({s.edits.tracks, s.edits.start, s.edits.end});
    s.edits.start = s.undo.back().start;
    s.edits.end = s.undo.back().end;
    s.edits.tracks = std::move(s.undo.back().tracks);
    s.undo.pop_back();
    changed_locked(s);
}
void redo() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.redo.empty()) return;
    s.undo.push_back({s.edits.tracks, s.edits.start, s.edits.end});
    s.edits.start = s.redo.back().start;
    s.edits.end = s.redo.back().end;
    s.edits.tracks = std::move(s.redo.back().tracks);
    s.redo.pop_back();
    changed_locked(s);
}
void new_animation() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits = {};
    s.clip.reset();
    s.saved_as.clear();
    s.undo.clear();
    s.redo.clear();
    s.time = 0;
    s.playing = false;
    s.dirty = false;
    s.status = "New animation. Record a trick to start.";
    publish_locked(s);
}
bool save_preset_copy(std::string& message) {
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.clip || s.clip->empty()) { message = "Record or load an animation first."; return false; }
        if (!s.library_read) read_library_locked(s);
        const auto base = file_safe(s.edits.name.empty() ? s.edits.trick : s.edits.name);
        int number = 2;
        std::string name;
        do { name = variation_name(base, static_cast<unsigned>(number++)); }
        while (std::filesystem::exists(library_folder() / (name + ".json")) || std::filesystem::exists(library_folder() / (name + ".clip")));
        s.edits.name = name;
        s.dirty = true;
    }
    return save(message);
}
namespace {
template<class Change> bool change_pack(Change change, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (!s.pack) throw std::runtime_error("Create or choose a pack first in ANIMATIONS.");
        auto pack = *s.pack;
        change(pack, s);
        write_preset_file(library_folder() / L"presets" / (pack.name + ".json"), pack_json(pack).dump(2));
        s.tricks_read = false;
        read_library_locked(s);
        message = "Updated pack " + pack.name + ".";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; return false; }
}
const Preset& saved_preset(const State& s, const std::string& name) {
    const auto found = std::find_if(s.presets.begin(), s.presets.end(), [&](const Preset& p) { return p.name == name; });
    if (found == s.presets.end()) throw std::runtime_error("Save the animation before choosing it as a preset.");
    return *found;
}
} // namespace
bool create_preset_pack(std::string& message, const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (!s.library_read) read_library_locked(s);
        auto pack = s.pack.value_or(PresetPack{});
        if (!s.pack) for (const auto& p : s.presets) pack.slots[preset_slot(p.trick, p.pop)].push_back(p.name);
        if (name.empty()) {
            int number = 1;
            do { pack.name = "Pack " + std::to_string(number++); }
            while (std::filesystem::exists(library_folder() / L"presets" / (pack.name + ".json")));
        } else {
            pack.name = file_safe(name);
            if (std::filesystem::exists(library_folder() / L"presets" / (pack.name + ".json")))
                throw std::runtime_error("That pack name already exists. Choose another name.");
        }
        write_preset_file(library_folder() / L"presets" / (pack.name + ".json"), pack_json(pack).dump(2));
        write_preset_file(library_folder() / L"presets" / L"current.txt", pack.name);
        read_library_locked(s);
        s.tricks_read = false;
        message = "Created " + pack.name + ". Choose variations in PLAYBACK.";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; return false; }
}
bool use_preset_pack(const std::string& name, std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    try {
        if (!name.empty()) {
            if (name != file_safe(name)) throw std::runtime_error("Invalid pack name");
            std::ifstream in(library_folder() / L"presets" / (name + ".json"), std::ios::binary);
            if (!in) throw std::runtime_error("Pack not found");
            read_pack_json(Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())));
        }
        write_preset_file(library_folder() / L"presets" / L"current.txt", name);
        read_library_locked(s);
        s.tricks_read = false;
        message = name.empty() ? "Using all saved animations (one fixed variant per trick/pop)." : "Using pack " + name + ".";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) { message = e.what(); s.status = message; return false; }
}
bool set_pack_alternation(bool alternate, std::string& message) {
    return change_pack([=](PresetPack& pack, const State&) { pack.alternate = alternate; }, message);
}
bool activate_preset(const std::string& name, std::string& message) {
    return change_pack([&](PresetPack& pack, const State& s) {
        const auto& p = saved_preset(s, name);
        auto& names = pack.slots[preset_slot(p.trick, p.pop)];
        std::erase(names, name);
        names.insert(names.begin(), name);
    }, message);
}
bool include_preset(const std::string& name, bool included, std::string& message) {
    return change_pack([&](PresetPack& pack, const State& s) {
        const auto& p = saved_preset(s, name);
        auto& names = pack.slots[preset_slot(p.trick, p.pop)];
        std::erase(names, name);
        if (included) names.push_back(name);
    }, message);
}
bool save(std::string& message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.clip || s.clip->empty()) {
        message = "Record a trick before saving.";
        return false;
    }
    if (s.edits.trick.empty() && s.clip->takeoff >= 0) {
        message = "Which trick is it? Type the game's trick in TRICK (ollie, kickflip...) or record it, then save.";
        s.status = message;
        return false;
    }
    if (s.edits.name.empty()) {
        // A new animation gets a name of its own, never one already in the library.
        const auto taken = [&](const std::string& n) { return std::find(s.library.begin(), s.library.end(), file_safe(n)) != s.library.end(); };
        std::string name = s.edits.trick.empty() ? "Trick" : s.edits.trick;
        int n = 1;
        while (taken(variation_name(file_safe(name), static_cast<unsigned>(n)))) ++n;
        s.edits.name = variation_name(file_safe(name), static_cast<unsigned>(n));
    }
    const auto name = file_safe(s.edits.name);
    const auto folder = library_folder();
    try {
        std::filesystem::create_directories(folder);
        Animation full = s.edits;
        full.name = name;
        full.clip.times = {0, s.clip->duration()};
        full.clip.takeoff = s.clip->takeoff;
        full.clip.landing = s.clip->landing;
        const auto json = to_json(full);
        const auto bytes = clip_bytes(*s.clip);
        // Written beside the old files, then moved over them: a failed save never leaves half a profile.
        const auto json_path = folder / (name + ".json"), clip_path = folder / (name + ".clip");
        for (const auto& [path, data, size] : {std::tuple{json_path, static_cast<const void*>(json.data()), json.size()},
                                               std::tuple{clip_path, static_cast<const void*>(bytes.data()), bytes.size()}}) {
            auto temporary = path;
            temporary += L".saving";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
                if (!out) throw std::runtime_error("could not write " + temporary.string());
            }
            std::filesystem::rename(temporary, path);
        }
        s.edits.name = name;
        s.saved_as = name;
        s.dirty = false;
        s.tricks_read = false; // the next tick reads the custom tricks again
        read_library_locked(s);
        message = "Saved " + name + ".";
        s.status = message;
        logging::log(Level::info, Channel::skater, "Animation Studio: saved {} ({} keys) to {}.", name, s.edits.key_count(), folder.string());
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = std::string("Saving failed: ") + e.what();
        s.status = message;
        return false;
    }
}
namespace {
std::vector<GameTrick> read_catalog() {
    std::vector<GameTrick> out;
    try {
        std::ifstream in(library_folder() / L"library" / L"catalog.json", std::ios::binary);
        if (!in) return out;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto json = Json::parse(text);
        if (!json.is_array()) return out;
        for (std::size_t i = 0; i < json.size() && i < 2000; ++i) {
            const auto& e = json.at(i);
            if (!e.is_object()) continue;
            GameTrick t{e.value("category", ""), e.value("label", ""), e.value("trick", ""), e.value("pop", ""), e.value("file", ""), e.value("asset", ""), e.value("preview_only", false)};
            t.position_codec = e.value("position_codec", "");
            t.looping = e.value("native_looping", false);
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
std::vector<GameTrick> read_recorded() {
    std::vector<GameTrick> out;
    std::error_code error;
    const auto folder = library_folder() / L"recorded";
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error)) continue;
        const auto identity = recorded_clip_identity(entry.path().filename().string());
        if (!identity) continue;
        GameTrick take;
        take.trick = identity->first; take.pop = identity->second;
        take.file = entry.path().filename().string(); take.recorded = true;
        // Old batch clips had no stance sidecar. Preserve their legacy regular assumption.
        take.switch_stance.reset();
        auto sidecar = entry.path(); sidecar.replace_extension(L".json");
        if (std::filesystem::is_regular_file(sidecar, error)) {
            try {
                std::ifstream in(sidecar, std::ios::binary);
                const auto metadata = from_json(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
                take.looping = metadata.loop;
                if (metadata.trick.empty() || metadata.pop != take.pop) throw std::runtime_error("recorded take metadata does not match its slot");
                take.trick = metadata.trick; take.stance = metadata.stance; take.switch_stance = metadata.switch_stance;
                take.source = metadata.source; take.entry = metadata.entry;
            } catch (const std::exception& e) {
                logging::log(Level::warning, Channel::skater, "Recorded take not loaded: {}: {}", take.file, e.what());
                continue;
            }
        }
        take.label = take.trick;
        bool first = true;
        for (auto& c : take.label) { if (c == '_') c = ' '; if (first) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); first = c == ' '; }
        take.category = take.trick.starts_with("nollie") || take.trick.starts_with("n_") ? "Nollie tricks" : "Flip tricks";
        out.push_back(std::move(take));
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
    static const auto assets = [] {
        auto out = std::make_shared<std::vector<NativeAsset>>();
        try {
            const auto path = library_folder() / L"library" / L"native-catalog.json";
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) > 32 * 1024 * 1024) return out;
            std::ifstream in(path, std::ios::binary);
            const auto root = Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            if (!root.is_array() || root.size() > 100000) return out;
            for (const auto& e : root) {
                if (!e.is_object()) continue;
                NativeAsset asset{e.value("category", "Other"), e.value("asset", ""), e.value("kind", "unclassified")};
                if (asset.asset.starts_with("animation/dingo/") && asset.asset.size() < 1024 && asset.category.size() < 256)
                    out->push_back(std::move(asset));
            }
        } catch (const std::exception& e) {
            logging::log(Level::warning, Channel::skater, "Animation Studio: native asset index not loaded: {}", e.what());
        }
        return out;
    }();
    return assets;
}
std::shared_ptr<const std::vector<GameTrick>> game_tricks() {
    auto& cache = game_catalog_cache();
    std::lock_guard lock(cache.mutex);
    const auto revision = tricks_revision.load(std::memory_order_acquire);
    if (!cache.entries || cache.revision != revision) {
        cache.entries = std::make_shared<const std::vector<GameTrick>>(merge_game_library(read_catalog(), read_recorded()));
        cache.revision = revision;
        logging::log(Level::info, Channel::skater, "Animation Studio: {} game tricks in the library.", cache.entries->size());
    }
    return cache.entries;
}
void refresh_game_tricks(std::string& message) {
    tricks_revision.fetch_add(1, std::memory_order_acq_rel);
    message = "Library refreshed: " + std::to_string(game_tricks()->size()) + " clips (recorded takes preferred per trick/pop).";
}
bool recorded_tricks() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(library_folder() / L"recorded", error))
        if (entry.path().extension() == L".clip") return true;
    return false;
}
std::shared_ptr<Clip> read_game_clip(const GameTrick& trick) {
    if (!safe_library_clip(trick.file)) throw std::runtime_error("invalid local library clip name");
    if (!trick.recorded && trick.position_codec != "native-piec-v1-extent")
        throw std::runtime_error("This native library has outdated position decoding. Rebuild or update it before previewing.");
    std::ifstream in(library_folder() / (trick.recorded ? L"recorded" : L"library") / std::filesystem::path(trick.file), std::ios::binary);
    if (!in) throw std::runtime_error("its file is missing");
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
    if (clip->joints != skeleton_joints) throw std::runtime_error("it is for another skeleton");
    return clip;
}
bool load_game_trick(const GameTrick& trick, std::string& message) {
    try {
        auto clip = read_game_clip(trick);
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.edits = {};
        s.edits.trick = trick.preview_only ? "" : trick.trick;
        s.edits.pop = trick.pop;
        s.edits.name = trick.label + " " + trick.pop;
        // Native clips are canonical regular; batch recordings keep their takeoff metadata.
        s.edits.stance = trick.stance;
        s.edits.switch_stance = trick.switch_stance;
        s.edits.source = trick.recorded ? trick.source : AnimationSource{"native_asset", trick.asset, "animation/dingo/animbase_default_skeleton", "skater-placement"};
        if (!trick.recorded) s.edits.source.position_codec = trick.position_codec;
        s.edits.entry = trick.entry;
        s.edits.loop = trick.looping;
        s.clip = std::move(clip);
        reset_brackets_locked(s);
        s.saved_as.clear();
        s.undo.clear();
        s.redo.clear();
        s.time = 0; // at its start: on the board, on the ground
        s.playing = false;
        s.dirty = false; // only an edit makes it unsaved
        message = trick.preview_only ? "Opened " + trick.label + " for editing. Live grind phase replacement is not connected yet; saving preserves your work without activating it in game." :
            "Opened the game's " + trick.label + " (" + trick.pop + " pop). Native board physics and graph constraints are not included in this clip.";
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
bool load(const std::string& name, std::string& message) {
    const auto folder = library_folder();
    try {
        const auto safe = file_safe(name);
        std::ifstream json_in(folder / (safe + ".json"), std::ios::binary);
        if (!json_in) throw std::runtime_error("it is not in the library");
        const std::string text((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>());
        auto animation = from_json(text);
        std::ifstream clip_in(folder / (safe + ".clip"), std::ios::binary);
        if (!clip_in) throw std::runtime_error("its recorded trick is missing");
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
        auto clip = std::make_shared<Clip>(clip_from_bytes(bytes));
        if (clip->joints != skeleton_joints) throw std::runtime_error("it was recorded on another skeleton");
        for (const auto& t : animation.tracks)
            for (const auto& k : t.keys)
                if (k.time > clip->duration() + 0.01f) throw std::runtime_error("a key lies past its recording");
        auto& s = state();
        std::lock_guard lock(s.mutex);
        animation.name = safe;
        s.edits = std::move(animation);
        s.clip = std::move(clip);
        s.saved_as = safe;
        s.undo.clear();
        s.redo.clear();
        s.time = 0; // at its start: on the board, on the ground
        s.playing = false;
        s.dirty = false;
        message = "Opened " + safe + ".";
        s.status = message;
        publish_locked(s);
        return true;
    } catch (const std::exception& e) {
        message = "Could not open " + name + ": " + e.what();
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.status = message;
        return false;
    }
}
bool remove(const std::string& name, std::string& message) {
    const auto folder = library_folder();
    const auto safe = file_safe(name);
    std::error_code error;
    std::filesystem::remove(folder / (safe + ".json"), error);
    std::filesystem::remove(folder / (safe + ".clip"), error);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.tricks_read = false;
    read_library_locked(s);
    message = "Deleted " + safe + ".";
    s.status = message;
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
          summary["active_pack"] = s.pack ? s.pack->name : "";
          summary["studio_open"] = s.open;
          summary["custom_playback_enabled"] = s.play_in_game;
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
                const auto source = library_folder() / (name + extension);
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
void rename(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.name = name;
    s.dirty = true;
    ++s.revision;
}
void set_trick(const std::string& trick) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.edits.trick = trick;
    if (s.edits.name.empty()) s.edits.name = trick;
    s.dirty = true;
    ++s.revision;
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
} // namespace dingosdk::anim_studio
