#include "board_break.h"
#include "board_break_model.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Assets/live_mods.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace dingosdk::board_break {
namespace {
constexpr const char* enabled_preference = "BoardBreak";
// A slam of `heavy` force or more always breaks the board; a lighter one with `light`, rising to
// certain between 60% of `heavy` and `heavy` (break_chance). Half of all breaks stay joined by the
// grip tape. Now and then a board gives way under the rider: per second of riding, the nose goes.
constexpr float heavy = 10.0f, light = 0.25f, joined_share = 0.5f;
constexpr double gives_way = 0.0001;
constexpr std::size_t cause_count = 35;   // the collector's cause bytes (+0x28) and forces (+0x4c)
constexpr std::uint64_t recent_ms = 1500; // a cause belongs to a wipeout this soon before it
// The board is back in the skater's hand: close to the skater and not lying flat.
constexpr float hand_reach = 1.3f, hand_upright = 0.8f;
// The board rig's pose, in the order the game evaluates it: 1 the rig's place in the world, 2 the
// deck's mount, 3 the deck, 4 and 8 the trucks, and the spare bones that carry the four stretches
// (nose, front, back, tail; the tail's hangs on the front's).
constexpr int root_bone = 1, mount_bone = 2, deck_bone = 3, back_truck_bone = 4, front_truck_bone = 8;
// The trucks' effectors, each right after its truck's bone: a deck pack's truck model has the
// baseplates on them (board_break_model.h, baseplate_bone).
constexpr int back_plate_bone = 5, front_plate_bone = 9;
constexpr std::array<int, 4> carrier{12, 13, 15, 14};
constexpr std::uint32_t rig_bones = 16;

bool readable(std::uintptr_t p, void* out, std::size_t size) { return size ? memory::peek_bytes(p, out, size) : p >= 0x10000; }

template <class... Args> void say(logging::Level level, std::format_string<Args...> text, Args&&... args) {
    logging::log(level, logging::Channel::skater, "Board Break: {}", std::format(text, std::forward<Args>(args)...));
}

// Written by the physics step, read by the client tick.
struct Hit {
    std::atomic<std::uint64_t> at{};
    std::atomic<std::int32_t> reason{-1};
    std::atomic<float> force{};
};
Hit& last_cause() { static Hit value; return value; }
Hit& last_wipeout() { static Hit value; return value; }

struct Broken {
    std::uintptr_t board{};
    std::uint64_t since{};
    bool gave_way{}; // it went under the rider: it stays broken while it is ridden
    bool asked{};    // it was broken from the console, to be looked at: it stays until it is mended there
};
struct State {
    // Any thread.
    std::atomic<bool> ready{}, on{true}, wanted_mend{};
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> has_deck{}; // one of Board Break's decks is the one in the game
    std::atomic<int> wanted_snap{-1}; // asked for from another thread, done at the next tick
    std::mutex names;
    std::string deck, wanted_deck; // under `names`: the deck in the game, and one asked for (done at the next tick)
    // Client thread from here on.
    std::optional<Broken> broken;
    Break parts;            // what the broken board keeps, what flew off, how they hang together
    float angle{};          // how far a hanging end droops (radians), or how deep a fold sags (0 to 1)
    std::uint32_t flights{}; // counts the breaks, so each starts a flight of its own
    bool counted{}, hooked{};
    std::uint64_t wipeouts{}, back_since{}, last_error{}, rode_at{}, board_seen{};
    // The ground under the board: its own height while it rolls on it, and what of that was fresh
    // when the board broke (else not a number).
    float ride_height{};
    std::uint64_t ride_when{};
    float break_ground{std::numeric_limits<float>::quiet_NaN()};
    std::mt19937 random{std::random_device{}()};
};
State& state() {
    static auto* value = new State;
    return *value;
}

// What the render pose hook does to the boards, published by the client tick.
struct RenderEdit {
    std::atomic<std::uintptr_t> holder{};  // the local board's animation holder
    std::atomic<std::uint32_t> kept{};     // the piece it shows (Piece bits), 0 while it is whole
    std::atomic<int> hinge{};
    std::atomic<float> angle{};
    std::atomic<bool> park{};              // a breakable deck is in the game: whole boards hide their stretches
};
RenderEdit& render_edit() { static RenderEdit value; return value; }

// ---- the board's pose --------------------------------------------------------------------------
// One bone: scale, rotation and position, four floats each.
struct PoseBone {
    float scale[4], rotation[4], position[4];
};
bool load_bone(std::uintptr_t address, PoseBone& out) noexcept {
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Rotation and position of one bone, at its own size; the fourth lane of each is left as it is.
bool store_bone(std::uintptr_t address, Quat rotation, Vec position) noexcept {
    __try {
        auto* lane = reinterpret_cast<float*>(address);
        lane[0] = lane[1] = lane[2] = 1.0f;
        lane[4] = rotation.x; lane[5] = rotation.y; lane[6] = rotation.z; lane[7] = rotation.w;
        lane[8] = position.x; lane[9] = position.y; lane[10] = position.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Quat facing_of(const PoseBone& bone) noexcept { return unit({bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3]}); }
Vec place_of(const PoseBone& bone) noexcept { return {bone.position[0], bone.position[1], bone.position[2]}; }
// The pose buffer of a board, or 0 when it is not the rig this knows.
std::uintptr_t board_pose(std::uintptr_t base, std::uintptr_t holder) {
    const auto pose = multiplayer::read_native_pose_layout(readable, base, holder, 64);
    return pose.buffer && pose.count == rig_bones ? pose.buffer : 0;
}
std::uintptr_t bone_at(std::uintptr_t buffer, int index) noexcept { return buffer + static_cast<std::uint32_t>(index) * 0x30ULL; }
// The board's place in the world as its pose has it.
bool board_place(std::uintptr_t base, std::uintptr_t holder, Vec& at, Quat& facing) {
    const auto buffer = board_pose(base, holder);
    PoseBone root{};
    if (!buffer || !load_bone(bone_at(buffer, root_bone), root)) return false;
    at = place_of(root);
    facing = facing_of(root);
    return std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z);
}
// Moves a truck with a piece (truck_bone).
void carry_truck(std::uintptr_t buffer, const Rigid& motion, int index, Vec place) noexcept {
    PoseBone mount{}, truck{};
    if (!load_bone(bone_at(buffer, mount_bone), mount) || !load_bone(bone_at(buffer, index), truck)) return;
    const auto bone = truck_bone(motion, facing_of(mount), place_of(mount), facing_of(truck), place);
    (void)store_bone(bone_at(buffer, index), bone.facing, bone.at);
}

// Keeps the baseplates on the deck (baseplate_bone), from each truck's bone as the animation left
// it: a truck that a piece has carried off already, in a pose nobody has written since, is left.
void hold_baseplates(std::uintptr_t buffer) noexcept {
    const auto hold = [&](int truck_index, int plate_index, const TruckRest& rest) {
        PoseBone truck{};
        if (!load_bone(bone_at(buffer, truck_index), truck)) return;
        const Rigid now{facing_of(truck), place_of(truck)};
        if (!(length(now.at - rest.at) < 0.05f)) return;
        const auto bone = baseplate_bone(now, rest);
        (void)store_bone(bone_at(buffer, plate_index), bone.facing, bone.at);
    };
    hold(back_truck_bone, back_plate_bone, back_truck_rest);
    hold(front_truck_bone, front_plate_bone, front_truck_rest);
}

// ---- the piece that comes off --------------------------------------------------------------------
// The client tick starts a flight (a new id) when the board breaks; the render pose hook moves the
// piece through the world from there (a kick, gravity, the ground) and writes it relative to
// wherever the board itself is in that frame, so the two part company although both are the one board.
struct Flight {
    std::mutex mutex;
    // From the tick.
    std::uint32_t id{};   // changes with every break; 0 = no piece in flight
    std::uint32_t mask{}; // the piece's stretches of deck and its trucks
    float ground{};       // world height of the ground under the skater
    float rest{};         // how far above the ground the piece's origin lies once it rests
    Vec kick{}, spin{};   // start speed (m/s) and turning (axis x rad/s), in the deck's own axes
    float ridden{std::numeric_limits<float>::quiet_NaN()}; // the ground the board rolled on as it broke
    // The hook's.
    std::uint32_t running{};
    std::int64_t last{};
    Vec at{}, speed{}, turning{};
    Quat facing{};
    bool landed{};      // lying on what is taken for the ground right now
    bool settled{};     // has landed once: flat from then on, whichever side came up
    bool upside_down{};
    Vec from{};         // where the board was when it broke
    float limit{};      // the tick's first guess of the ground: never taken higher than this
    float tipped{};     // 0..1: how far a half on one truck has tipped onto its broken end
};
Flight& flight() { static Flight value; return value; }
// The stretches of deck that are in flight (0 for none) and, in `piece`, where they are from the
// board's own place; the piece's trucks are moved here.
std::uint32_t fly_piece(std::uintptr_t buffer, Rigid& piece) {
    auto& f = flight();
    std::unique_lock lock(f.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !f.id) return 0;
    PoseBone root{}, mount{};
    if (!load_bone(bone_at(buffer, root_bone), root) || !load_bone(bone_at(buffer, mount_bone), mount)) return 0;
    const Vec board_at = place_of(root);
    const Quat board_facing = facing_of(root), deck_turn = facing_of(mount);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    if (f.running != f.id) {
        f.running = f.id;
        f.last = now.QuadPart;
        f.at = f.from = board_at;
        f.facing = board_facing;
        f.speed = turn(board_facing, turn(deck_turn, f.kick));
        f.turning = turn(board_facing, turn(deck_turn, f.spin));
        f.landed = f.settled = f.upside_down = false;
        f.limit = f.ground;
        f.tipped = 0;
    }
    const auto dt = static_cast<float>(std::clamp((now.QuadPart - f.last) / frequency, 0.0, 0.05));
    f.last = now.QuadPart;
    // The ground is where the board itself lies: its lowest point right now, which physics brings
    // to rest on the ground. The skater's height at the slam (the hips, and the skater may be in the
    // air) is only an upper limit. It is the board's height NOW, not the lowest ever seen: one odd
    // pose (a teleport, a frame before the board is placed) must not sink the piece for good, and a
    // pose that is nowhere near the break is passed over altogether.
    const bool plausible = std::isfinite(board_at.x) && std::isfinite(board_at.y) && std::isfinite(board_at.z) && length(board_at - f.from) < 30.0f;
    float lowest = board_at.y + 1.0f;
    if (plausible) {
        for (const Vec corner : {Vec{0, 0, 0}, Vec{0, 0.093f, 0}, Vec{0, 0.141f, 0.41f}, Vec{0, 0.136f, -0.40f}, Vec{0.105f, 0.10f, 0},
                                 Vec{-0.105f, 0.10f, 0}, Vec{0.0876f, 0, front_axle}, Vec{-0.0876f, 0, front_axle}, Vec{0.0876f, 0, back_axle},
                                 Vec{-0.0876f, 0, back_axle}})
            lowest = (std::min)(lowest, (board_at + turn(board_facing, turn(deck_turn, corner))).y);
        // Only a board that is still near the piece says anything about the ground under the piece.
        if (std::hypot(board_at.x - f.at.x, board_at.z - f.at.z) < 4.0f) {
            // The ground the board rolled on as it broke is exact, where its lowest point depends on
            // how it has come to lie and on what: that height stays, unless the board has plainly
            // dropped below it since (off a ledge, down a stair).
            if (std::isfinite(f.ridden)) f.ground = lowest < f.ridden - 0.15f ? lowest : f.ridden;
            else f.ground = (std::min)(f.limit, lowest);
        }
    }
    // Where the piece's origin is once it lies on the ground: on its grip when it is upside down,
    // else on its underside or its wheels. A little above it: real ground is rough, and a deck is a
    // centimetre thick. A gap under a piece goes unseen, a piece cut off by the ground does not.
    const float lift = f.mask & (front_truck | back_truck) ? 0.004f : 0.012f;
    const auto lying = [&] {
        const bool flipped = f.settled ? f.upside_down : turn(f.facing, {0, 1, 0}).y < 0;
        return (flipped ? f.ground + 0.141f : f.ground + f.rest) + lift; // upside down it rests on the raised end of its grip side
    };
    if (f.landed && f.at.y > lying() + 0.01f) {
        f.landed = false; // the ground turned out to be lower: it drops the rest of the way
        f.speed = {};
        f.turning = {};
    }
    if (!f.landed) {
        f.speed.y -= 9.81f * dt;
        f.at = f.at + f.speed * dt;
        if (const auto rate = length(f.turning); rate > 1e-4f) f.facing = unit(about(f.turning * (1.0f / rate), rate * dt) * f.facing);
        if (f.speed.y <= 0 && f.at.y <= lying()) { // on its way down: it starts on the ground it will land on
            if (!f.settled) {
                // Down: lying flat, pointing the way it was, the same side up.
                const auto ahead = turn(f.facing, {0, 0, 1});
                f.upside_down = turn(f.facing, {0, 1, 0}).y < 0;
                f.facing = about({0, 1, 0}, std::atan2(ahead.x, ahead.z));
                if (f.upside_down) f.facing = unit(f.facing * about(turn(deck_turn, {0, 0, 1}), 3.14159265f)); // rolled over, about its length
                f.speed = {f.speed.x * 0.3f, 0, f.speed.z * 0.3f};
                f.settled = true;
            } else f.speed = {};
            f.at.y = lying();
            f.landed = true;
        }
    } else {
        f.at = f.at + f.speed * dt; // a short slide
        f.speed = f.speed * (std::max)(0.0f, 1.0f - 7.0f * dt);
        f.at.y = lying();           // and it stays on the ground if that comes up again
    }
    // A half board that came down on its wheels tips onto its broken end.
    const bool on_one_truck = (f.mask & (back_truck | front_truck)) && f.settled && !f.upside_down;
    f.tipped = on_one_truck && f.landed ? (std::min)(1.0f, f.tipped + dt / 0.18f) : on_one_truck ? f.tipped : 0.0f;
    const auto tip = on_root(deck_turn, f.mask & back_truck ? tipped_on_axle(back_axle, 1.0f, back_tip * f.tipped)
                                                            : tipped_on_axle(front_axle, -1.0f, front_tip * f.tipped));
    // Where that is from the board's own place.
    const auto back_to_board = conj(board_facing);
    const auto lies_facing = unit(back_to_board * f.facing);
    const auto lies_at = turn(back_to_board, f.at - board_at);
    piece = {unit(lies_facing * tip.facing), lies_at + turn(lies_facing, tip.at)};
    if (f.mask & back_truck) carry_truck(buffer, piece, back_truck_bone, back_truck_place);
    if (f.mask & front_truck) carry_truck(buffer, piece, front_truck_bone, front_truck_place);
    return f.mask & stretches;
}

// ---- what the hook writes ------------------------------------------------------------------------
// The local board, broken: the real deck goes away, the trucks come back (their baseplates with
// them), each stretch goes to where its piece lies and one that belongs to no piece goes away too.
void draw_broken(std::uintptr_t base, std::uintptr_t holder, std::uint32_t kept, Hinge hinge, float angle) {
    const auto buffer = board_pose(base, holder);
    PoseBone root{}, mount{}, deck{};
    if (!buffer || !load_bone(bone_at(buffer, root_bone), root) || !load_bone(bone_at(buffer, mount_bone), mount) ||
        !load_bone(bone_at(buffer, deck_bone), deck))
        return;
    hold_baseplates(buffer);
    // From where its animation put it this frame; a pose nobody has written since the last move stays as it is.
    if (const Vec at = place_of(deck); length(at) < 100.0f) (void)store_bone(bone_at(buffer, deck_bone), facing_of(deck), at + away);
    auto layout = lay_out(kept, hinge, angle, turn(facing_of(root), {0, 1, 0}).y, facing_of(mount));
    if (layout.back_truck) carry_truck(buffer, layout.back_motion, back_truck_bone, back_truck_place);
    if (layout.front_truck) carry_truck(buffer, layout.front_motion, front_truck_bone, front_truck_place);
    if (hinge == Hinge::none) {
        Rigid piece{};
        const auto flying = fly_piece(buffer, piece);
        for (std::size_t i = 0; i < layout.stretch.size(); ++i)
            if (flying & (1u << i)) layout.stretch[i] = {true, piece};
    }
    const auto bones = bones_of(layout.stretch);
    for (std::size_t i = 0; i < bones.size(); ++i) (void)store_bone(bone_at(buffer, carrier[i]), bones[i].facing, bones[i].at);
}
// A whole board, any player's: its four stretches go far away from it, and its baseplates sit on
// the deck.
void park_pieces(std::uintptr_t base, std::uintptr_t holder) {
    const auto buffer = board_pose(base, holder);
    if (!buffer) return;
    hold_baseplates(buffer);
    const auto bones = bones_of({});
    for (std::size_t i = 0; i < bones.size(); ++i) (void)store_bone(bone_at(buffer, carrier[i]), bones[i].facing, bones[i].at);
}

// ---- breaking and mending --------------------------------------------------------------------------
// The crack of the deck, straight to the speakers (not through the game's mixer): a wave file out
// of the `sounds` folder of one of Board Break's deck mods. Its subfolders light, medium and heavy
// hold the sounds for a break of that size, as many as one likes under any names; a size with none
// of its own takes the nearest size that has some, and wave files in the folder itself serve when
// no subfolder does.
void crack_sound(State& s, Size size) {
    static const std::array<std::vector<std::filesystem::path>, 4> sounds = [] {
        std::array<std::vector<std::filesystem::path>, 4> found;
        constexpr std::array<const wchar_t*, 4> names{L"light", L"medium", L"heavy", L""};
        for (const auto& mod : mods::board_break_folders()) {
            std::error_code failed;
            if (!std::filesystem::is_directory(mod / L"sounds", failed)) continue;
            for (std::size_t i = 0; i < names.size(); ++i) {
                for (std::filesystem::directory_iterator at(mod / L"sounds" / names[i], failed), end; !failed && at != end; at.increment(failed)) {
                    auto extension = at->path().extension().wstring();
                    for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
                    if (at->is_regular_file(failed) && extension == L".wav") found[i].push_back(at->path());
                }
                std::sort(found[i].begin(), found[i].end());
            }
            break;
        }
        return found;
    }();
    // Nearest first: a medium break without sounds of its own takes a heavy one before a light one.
    static constexpr std::array<std::array<std::size_t, 4>, 3> order{{{0, 1, 2, 3}, {1, 2, 0, 3}, {2, 1, 0, 3}}};
    for (const auto from : order[static_cast<std::size_t>(size)]) {
        const auto& files = sounds[from];
        if (files.empty()) continue;
        const auto& file = files[std::uniform_int_distribution<std::size_t>(0, files.size() - 1)(s.random)];
        (void)PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }
}
// `carried`: the skater is on foot with the board. The grip tape always holds then: a piece that
// came off would have to fall to the ground under the skater's feet, and where that is the board
// in a hand does not tell (a loose piece takes the ground from where the board rolled or lies).
bool snap(State& s, std::uintptr_t board, std::optional<Place> at, bool gave_way, bool carried, std::string_view why, bool asked = false) {
    if (!s.has_deck.load(std::memory_order_acquire) || s.broken || !board) return false;
    const auto place = at ? *at : static_cast<Place>(std::uniform_int_distribution<int>(0, 2)(s.random));
    const bool joined = carried || std::uniform_real_distribution<float>(0.0f, 1.0f)(s.random) < joined_share;
    const auto now = GetTickCount64();
    s.parts = broken_at(place, joined);
    s.angle = s.parts.hinge == Hinge::fold ? std::uniform_real_distribution<float>(0.35f, 1.0f)(s.random)
                                           : std::uniform_real_distribution<float>(0.30f, 0.65f)(s.random);
    s.break_ground = s.ride_when && now - s.ride_when < 400 ? s.ride_height : std::numeric_limits<float>::quiet_NaN();
    ++s.flights;
    s.broken = Broken{board, now, gave_way, asked};
    s.back_since = 0;
    crack_sound(s, break_size(place, joined, s.angle));
    say(logging::Level::debug, "the board broke at the {}{} ({}).", place_names[static_cast<std::size_t>(place)],
        joined ? ", held together by the grip tape" : "", why);
    return true;
}
void mend(State& s, std::string_view why) {
    if (!s.broken) return;
    say(logging::Level::debug, "the board is whole again after {:.1f} s ({}).", (GetTickCount64() - s.broken->since) / 1000.0, why);
    s.broken.reset();
    s.parts = {};
}
// Where the board is relative to the skater: how far, and how flat it lies (1 = flat on the ground
// either way up, 0 = on its edge, as when it is carried).
struct Hold {
    bool known{};
    float distance{}, upright{};
    bool in_hand() const noexcept { return known && distance < hand_reach && upright < hand_upright; }
};
Hold board_hold(std::uintptr_t base, std::uintptr_t client) {
    Hold out;
    const auto frame = multiplayer::capture_local(base, client, true);
    if (!frame.ready || frame.pose.board.empty()) return out;
    const auto& skater = frame.pose.root.position;
    const auto& board = frame.pose.board.front();
    out.distance = std::hypot(board.position[0] - skater[0], board.position[1] - skater[1], board.position[2] - skater[2]);
    const auto& q = board.rotation; // x, y, z, w: the board's up axis, turned into the world
    out.upright = std::abs(1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]));
    out.known = std::isfinite(out.distance) && std::isfinite(out.upright);
    return out;
}
bool riding(std::uint32_t physics) noexcept {
    // Ground/air and grind families; 300 is the wipeout, 5xx is on foot and getting on.
    return (physics >= 100 && physics < 300) || (physics >= 400 && physics < 500);
}
// The deck `wanted` means, of those installed: a name, a number in the list (from 1), or "next"
// for the one after the deck in the game. Empty when there is no such deck.
std::string deck_meant(std::string wanted, const std::string& in_game, const std::vector<std::string>& installed) {
    for (auto& c : wanted) c = c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (installed.empty()) return {};
    if (wanted.empty() || wanted == "next") {
        const auto now = std::find(installed.begin(), installed.end(), in_game);
        return now == installed.end() || now + 1 == installed.end() ? installed.front() : *(now + 1);
    }
    if (std::all_of(wanted.begin(), wanted.end(), [](char c) { return c >= '0' && c <= '9'; }) && wanted.size() <= 3) {
        // The decks the feature was made with keep their numbers whether they are installed or not;
        // the others follow, in the order they are listed.
        const auto number = static_cast<std::size_t>(std::stoi(wanted));
        const auto& known = mods::board_break_decks;
        if (number >= 1 && number <= known.size()) return std::string(known[number - 1]);
        std::size_t next = known.size();
        for (const auto& deck : installed)
            if (std::find(known.begin(), known.end(), deck) == known.end() && ++next == number) return deck;
        return {};
    }
    return wanted;
}
// What the hook is told this tick; `holder` 0 leaves the local board to be treated as any other.
void publish(State& s, std::uintptr_t base, std::uintptr_t client, std::uintptr_t holder) {
    auto& edit = render_edit();
    const bool broken = s.broken && holder;
    edit.park.store(s.has_deck.load(std::memory_order_acquire), std::memory_order_release);
    edit.angle.store(s.angle, std::memory_order_release);
    edit.hinge.store(static_cast<int>(broken ? s.parts.hinge : Hinge::none), std::memory_order_release);
    edit.kept.store(broken ? s.parts.kept : 0, std::memory_order_release);
    edit.holder.store(holder, std::memory_order_release);
    // The broken-off piece: one flight per break, gone when the board is whole again.
    auto& f = flight();
    std::lock_guard lock(f.mutex);
    if (!broken || !s.parts.loose) f.id = 0;
    else if (f.id != s.flights) {
        const auto frame = multiplayer::capture_local(base, client, true);
        const auto roll = [&](float low, float high) { return std::uniform_real_distribution<float>(low, high)(s.random); };
        const bool loose_end = !(s.parts.loose & (front_truck | back_truck)); // a nose or tail, no truck on it
        const float along = s.parts.loose & nose ? 1.0f : -1.0f;              // the way it leaves, along the board
        const float push = loose_end ? 1.0f : 0.55f;
        f.mask = s.parts.loose;
        f.ground = frame.ready ? frame.pose.root.position[1] : 0.0f;
        f.rest = loose_end ? -0.083f : 0.0f; // a bare piece lies on its underside, one with a truck stands on its wheels
        f.ridden = s.break_ground;
        f.kick = {roll(-0.9f, 0.9f) * push, (1.4f + roll(0.0f, 1.2f)) * push, along * (1.2f + roll(0.0f, 1.6f)) * push};
        f.spin = {along * (5.0f + roll(0.0f, 6.0f)) * push, roll(-3.0f, 3.0f) * push, roll(-2.0f, 2.0f) * push};
        f.id = s.flights;
    }
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return true;
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Board Break cannot follow the skater: its slams come through No Bail's hooks, which did not start.");
        return false;
    }
    s.base.store(base, std::memory_order_release);
    s.on.store(profile_runtime::local_preference(enabled_preference).value_or(false), std::memory_order_release);
    const auto in_game = mods::board_break_in_play();
    {
        std::lock_guard lock(s.names);
        s.deck = in_game;
    }
    s.has_deck.store(!in_game.empty(), std::memory_order_release);
    s.ready.store(true, std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater, "Board Break ready ({}, deck: {}).", s.on.load() ? "on" : "off",
        deck_label(in_game));
    return true;
}

void note_cause(std::int32_t reason, float magnitude) noexcept {
    auto& hit = last_cause();
    const auto now = GetTickCount64();
    // Keep the strongest of a burst: one slam records several causes.
    if (now - hit.at.load(std::memory_order_relaxed) < 250 && magnitude <= hit.force.load(std::memory_order_relaxed)) return;
    hit.force.store(magnitude, std::memory_order_relaxed);
    hit.reason.store(reason, std::memory_order_relaxed);
    hit.at.store(now, std::memory_order_release);
}
void note_wipeout(std::uintptr_t causes) noexcept {
    std::array<std::uint8_t, cause_count> set{};
    std::array<float, cause_count> force{};
    auto& hit = last_wipeout();
    std::int32_t reason{-1};
    float strongest{};
    if (causes && memory::peek_bytes(causes + 0x28, set.data(), set.size()) && memory::peek_bytes(causes + 0x4c, force.data(), sizeof(force)))
        for (std::size_t i = 0; i < cause_count; ++i)
            if (set[i] && (reason < 0 || force[i] > strongest)) {
                reason = static_cast<std::int32_t>(i);
                strongest = force[i];
            }
    hit.force.store(strongest, std::memory_order_relaxed);
    hit.reason.store(reason, std::memory_order_relaxed);
    hit.at.store(GetTickCount64(), std::memory_order_release);
}

void on_render_pose(std::uintptr_t animation_interface) noexcept {
    auto& edit = render_edit();
    // Without a breakable deck in the game the boards have no stretches: nothing to do.
    if (!edit.park.load(std::memory_order_acquire) || animation_interface <= 0xc0) return;
    const auto base = state().base.load(std::memory_order_acquire);
    const auto holder = edit.holder.load(std::memory_order_acquire);
    const auto kept = edit.kept.load(std::memory_order_acquire);
    const auto error = GetLastError();
    try {
        if (kept && holder && animation_interface == holder + 0xc0)
            draw_broken(base, holder, kept, static_cast<Hinge>(edit.hinge.load(std::memory_order_acquire)), edit.angle.load(std::memory_order_acquire));
        else park_pieces(base, animation_interface - 0xc0);
    } catch (...) { /* A board that is being built or torn down: the next frame. */ }
    SetLastError(error);
}

void on_client_tick(std::uintptr_t client) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    const auto base = s.base.load(std::memory_order_acquire);
    try {
        const auto now = GetTickCount64();
        // The deck in the game: the launch's, or the one the last live apply of the mods put there.
        const auto deck_in_game = mods::board_break_in_play();
        const bool in_game = !deck_in_game.empty();
        std::string wanted;
        bool changed{};
        {
            std::lock_guard lock(s.names);
            changed = s.deck != deck_in_game;
            s.deck = deck_in_game;
            wanted.swap(s.wanted_deck);
        }
        s.has_deck.store(in_game, std::memory_order_release);
        if (changed) say(logging::Level::info, "the deck in the game is now: {}{}.", deck_label(deck_in_game), in_game ? "" : " (boards cannot break)");
        if (!wanted.empty() && wanted != deck_in_game && mods::save_board_break_deck(wanted)) {
            mend(s, "another deck");
            say(logging::Level::info, "deck {} chosen. {}", deck_label(wanted), live_mods::apply(true));
        }
        if (in_game && !s.hooked) {
            // The boards' render pose hook is shared with multiplayer and first person, and only
            // installed when one of them asks.
            std::string detail;
            s.hooked = multiplayer::install_entity_hooks(base, detail);
            if (!s.hooked && now - s.last_error > 10000) {
                s.last_error = now;
                say(logging::Level::warning, "the boards' render pose hook is unavailable ({}).", detail);
            }
        }
        NoBailSkater skater;
        std::uintptr_t board{}, holder{};
        if (in_game && no_bail_skater(skater)) {
            const auto found = multiplayer::read_native_board(readable, base, skater.entity);
            board = found.entity;
            holder = found.holder;
        }
        if (!board) {
            // No local skater or no board (a respawn, a level change): the next board is whole. A
            // broken one that is only out of sight for a moment stays as it is.
            if (s.broken && in_game && now - s.board_seen < 500) return;
            mend(s, "the board is gone");
            s.counted = false;
            s.wanted_snap.store(-1, std::memory_order_release);
            s.wanted_mend.store(false, std::memory_order_release);
            publish(s, base, client, 0);
            return;
        }
        s.board_seen = now;
        if (s.broken && s.broken->board != board) mend(s, "the skater has another board");
        watch_physics_state(client, skater.entity);
        const auto watch = watched_physics_state();
        if (s.wanted_mend.exchange(false, std::memory_order_acq_rel)) mend(s, "asked for");
        const bool carried = watch.valid && watch.state >= 500 && watch.state < 600; // on foot and getting on
        if (const int place = s.wanted_snap.exchange(-1, std::memory_order_acq_rel); place >= 0)
            (void)snap(s, board, place < 3 ? std::optional(static_cast<Place>(place)) : std::nullopt, false, carried, "asked for", true);
        if (watch.valid) {
            if (watch.state == 100) {
                // Rolling on the ground, the right way up: the board's own height is the ground's.
                Vec at{};
                Quat facing{};
                if (board_place(base, holder, at, facing) && turn(facing, {0, 1, 0}).y > 0.9f) {
                    s.ride_height = at.y;
                    s.ride_when = now;
                }
            }
            const bool on = s.on.load(std::memory_order_acquire);
            if (!s.counted) {
                s.wipeouts = watch.wipeouts;
                s.counted = true;
            }
            if (watch.wipeouts != s.wipeouts) {
                s.wipeouts = watch.wipeouts;
                // The cause that made the selector choose Wipeout, or else the last one recorded.
                auto* hit = &last_wipeout();
                if (hit->reason.load() < 0 || now - hit->at.load() > recent_ms) hit = &last_cause();
                const bool known = now - hit->at.load() <= recent_ms && hit->reason.load() >= 0;
                const auto force = known ? hit->force.load() : 0.0f;
                if (on && !s.broken && std::uniform_real_distribution<float>(0.0f, 1.0f)(s.random) < break_chance(force, heavy, light))
                    (void)snap(s, board, std::nullopt, false, watch.previous >= 500 && watch.previous < 600, std::format("a slam of force {:.1f}", force));
            }
            // Now and then a board simply gives way under the rider: the nose goes.
            if (on && !s.broken && s.rode_at && riding(watch.state)) {
                const auto seconds = (std::min<std::uint64_t>)(now - s.rode_at, 250) / 1000.0;
                if (std::uniform_real_distribution<double>(0.0, 1.0)(s.random) < gives_way * seconds)
                    (void)snap(s, board, Place::nose, true, false, "it gave way while riding");
            }
            s.rode_at = now;
            if (s.broken) {
                // Whole again once the skater rides it, or has picked it up after the fall. A board that
                // gave way under the rider stays as it is while it is ridden; after a fall it is a
                // broken board like any other.
                const auto hold = board_hold(base, client);
                const bool on_it = riding(watch.state);
                if (s.broken->gave_way && watch.state == addr::no_bail::wipeout_physics_state) s.broken->gave_way = false;
                const bool back = s.broken->gave_way ? !on_it && hold.in_hand()
                                                     : on_it || (watch.state != addr::no_bail::wipeout_physics_state && hold.in_hand());
                // A respawn gives the skater a new physics selector, which the watch reports as state 0
                // until it has chosen once: the board is whole from that moment.
                if (watch.state == 0 && now - s.broken->since > 300) mend(s, "a respawn");
                else if (s.broken->asked) s.back_since = 0;
                else if (!back) s.back_since = 0;
                else if (!s.back_since) s.back_since = now;
                else if (now - s.back_since > 300 && now - s.broken->since > 1200) mend(s, on_it ? "riding again" : "back in hand");
            }
        }
        publish(s, base, client, holder);
    } catch (const std::exception& error) {
        // The skater or its board is being rebuilt: tried again at a later tick.
        const auto now = GetTickCount64();
        if (now - s.last_error > 10000 && logging::enabled(logging::Level::debug)) {
            s.last_error = now;
            say(logging::Level::debug, "waiting ({}).", error.what());
        }
    } catch (...) { /* A broken board is never worth the client tick. */ }
}

bool available() noexcept { return state().ready.load(std::memory_order_acquire); }
bool enabled() noexcept { return available() && state().on.load(std::memory_order_acquire); }
void set_enabled(bool enabled) noexcept {
    state().on.store(enabled, std::memory_order_release);
    profile_runtime::set_local_preference(enabled_preference, enabled);
}
std::string deck() {
    auto& s = state();
    std::lock_guard lock(s.names);
    return s.deck;
}
std::string deck_label(std::string_view deck) {
    if (deck.empty()) return "None";
    std::string name(deck);
    bool start = true;
    for (auto& c : name) {
        if (c == '_') c = ' ';
        else if (start) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        start = c == ' ';
    }
    return name;
}
std::string choose_deck(std::string wanted) {
    auto& s = state();
    if (!available()) return "Board Break is unavailable for this game build.";
    const auto installed = mods::installed_board_break_decks();
    if (installed.empty()) return "None of Board Break's decks is installed.";
    const auto in_game = deck();
    const auto chosen = deck_meant(wanted, in_game, installed);
    if (chosen.empty()) return "There is no deck " + wanted + ".";
    if (std::find(installed.begin(), installed.end(), chosen) == installed.end())
        return "The " + deck_label(chosen) + " deck is not installed: its mod (Board_Break_" + chosen + ") is not in the Mods folder.";
    if (chosen == in_game) return "The " + deck_label(chosen) + " deck is the one in the game.";
    {
        std::lock_guard lock(s.names);
        s.wanted_deck = chosen;
    }
    return "The " + deck_label(chosen) + " deck: the mods are applied and the level loads again with it.";
}
void snap(int place) noexcept { state().wanted_snap.store(place >= 0 && place < 3 ? place : 3, std::memory_order_release); }
void mend() noexcept { state().wanted_mend.store(true, std::memory_order_release); }
} // namespace dingosdk::board_break
