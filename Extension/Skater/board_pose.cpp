#include "board_pose.h"
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace dingosdk::board_pose {
namespace {
using anim_studio::conjugate;
using anim_studio::multiply;
using anim_studio::normalised;
using anim_studio::rotate;
constexpr int place_bone = 1;
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
template <class T> std::optional<T> read_value(std::uintptr_t address) noexcept {
    T value{};
    if (!peek(address, &value, sizeof(T))) return std::nullopt;
    return value;
}
struct alignas(16) Lane {
    std::array<float, 4> scale, rotation, position;
};
static_assert(sizeof(Lane) == 0x30);
// Rotation and position of one bone; its scale, and each fourth lane, as they are.
bool store(std::uintptr_t address, const Quat& rotation, const Vec3& position) noexcept {
    if (address < 0x10000 || address > highest_user - sizeof(Lane)) return false;
    __try {
        auto* lane = reinterpret_cast<float*>(address);
        lane[4] = rotation[0]; lane[5] = rotation[1]; lane[6] = rotation[2]; lane[7] = rotation[3];
        lane[8] = position[0]; lane[9] = position[1]; lane[10] = position[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool finite(const Vec3& v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float length(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
Quat about(const Vec3& axis, float angle) {
    const float s = std::sin(angle * 0.5f);
    return normalised({axis[0] * s, axis[1] * s, axis[2] * s, std::cos(angle * 0.5f)});
}
// The pose a bone is written with, as the client tick asked it.
struct Wanted {
    bool hidden{};
    bool everyone{}; // hidden from the other players too
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
    Space space{};
};
// A piece in flight: its frame in the world, and each of its bones in that frame.
struct Piece {
    std::vector<int> bones;
    Vec3 velocity{}, spin{};
    float ground{};
    bool started{}, landed{};
    Vec3 at{};
    Quat facing{0, 0, 0, 1};
    std::map<int, std::pair<Quat, Vec3>> held; // each bone relative to the piece's frame
    LARGE_INTEGER last{};
};
struct Shared {
    std::mutex mutex;
    std::string owner;
    std::map<int, Wanted> wanted;
    std::set<int> parked;
    std::map<std::string, std::vector<int>, std::less<>> park_by;
    std::optional<Piece> piece;
    std::optional<std::array<Bone, bones>> drawn;
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
std::atomic<std::uintptr_t> base_now{}, holder_now{};
std::atomic<bool> any{};
// The bones hidden from everyone, as bits (bone b is bit b), for the sent pose filter without the lock.
std::atomic<std::uint32_t> hidden_from_all{};
// How every board is fitted (fit): the baseplates held on the deck, and the wheels' spread. Read by
// the render hook without the lock; the owner is under it.
std::atomic<bool> fitted{}, fit_plates{};
std::atomic<float> fit_axle{1.0f};
std::string fit_owner;
struct TruckRest {
    int truck, plate;
    Quat facing; // the truck's bone on the deck's, at rest
    Vec3 at;
    Vec3 effector; // its effector on the truck's bone, at rest (not turned)
};
constexpr std::array<TruckRest, 2> truck_rests{{
    {4, 5, {-0.3641458f, -0.3641458f, -0.6061335f, 0.6061335f}, {0.0005361f, -0.0183003f, -0.2344986f}, {0.0395021f, 0.0f, 0.0f}},
    {8, 9, {-0.6061674f, 0.6061674f, 0.3640894f, 0.3640894f}, {0.0005361f, -0.0182917f, 0.2346680f}, {0.0395677f, 0.0f, 0.0f}},
}};
// The wheels, each on its truck's bone, about 0.0876 m out along the axle.
constexpr std::array<int, 4> wheel_bones{6, 7, 10, 11};
constexpr float wheel_offset = 0.0876f;
bool store_float(std::uintptr_t address, float value) noexcept {
    if (address < 0x10000 || address > highest_user - sizeof(float)) return false;
    __try {
        *reinterpret_cast<float*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void refresh_any(Shared& s) {
    any.store(!s.wanted.empty() || !s.parked.empty() || s.piece.has_value(), std::memory_order_release);
    std::uint32_t bits = 0;
    for (const auto& [b, want] : s.wanted)
        if (want.hidden && want.everyone) bits |= 1u << b;
    hidden_from_all.store(bits, std::memory_order_release);
}
// The local skater's pose as it is sent: the bones hidden from everyone out of sight in it too, as they
// are drawn here. board[0] is the board entity's place in the world, then its rig's bones from 0.
void on_sent_pose(std::uintptr_t, multiplayer::Pose& pose) noexcept {
    const auto bits = hidden_from_all.load(std::memory_order_acquire);
    if (!bits) return;
    for (int b = first_writable; b < bones; ++b)
        if ((bits & (1u << b)) && static_cast<std::size_t>(b + 1) < pose.board.size()) {
            auto& at = pose.board[static_cast<std::size_t>(b + 1)].position;
            if (length(at) < 100.0f) at[1] -= 2000.0f;
        }
}
std::string take(Shared& s, std::string_view owner) {
    if (!s.owner.empty() && s.owner != owner) return s.owner + " holds the board";
    s.owner = owner;
    return {};
}
std::uintptr_t board_buffer(std::uintptr_t base, std::uintptr_t holder) {
    const auto pose = multiplayer::read_native_pose_layout(peek, base, holder, 64);
    return pose.buffer && pose.count == static_cast<std::uint32_t>(bones) ? pose.buffer : 0;
}
std::uintptr_t bone_at(std::uintptr_t buffer, int index) { return buffer + static_cast<std::uintptr_t>(index) * sizeof(Lane); }

// The piece's flight this frame: gravity, then the ground, where it settles flat the side it came down.
void fly(Piece& p, const Quat& place_rotation, const Vec3& place_at, const std::array<Lane, bones>& lanes) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    if (!p.started) {
        p.started = true;
        p.at = place_at;
        p.facing = place_rotation;
        p.last = now;
        for (const int b : p.bones)
            p.held[b] = {Quat{lanes[b].rotation}, Vec3{lanes[b].position[0], lanes[b].position[1], lanes[b].position[2]}};
    }
    const auto dt = static_cast<float>(std::clamp((now.QuadPart - p.last.QuadPart) / frequency, 0.0, 0.05));
    p.last = now;
    if (!p.landed) {
        p.velocity[1] -= 9.81f * dt;
        p.at = add(p.at, mul(p.velocity, dt));
        if (const float rate = length(p.spin); rate > 1e-4f) p.facing = normalised(multiply(about(mul(p.spin, 1.0f / rate), rate * dt), p.facing));
        if (p.velocity[1] <= 0 && p.at[1] <= p.ground) {
            // Down: flat, pointing the way it was, a short slide.
            const auto ahead = rotate(p.facing, {0, 0, 1});
            const bool upside = rotate(p.facing, {0, 1, 0})[1] < 0;
            p.facing = about({0, 1, 0}, std::atan2(ahead[0], ahead[2]));
            if (upside) p.facing = normalised(multiply(p.facing, about({0, 0, 1}, 3.14159265f)));
            p.velocity = {p.velocity[0] * 0.3f, 0, p.velocity[2] * 0.3f};
            p.spin = {};
            p.at[1] = p.ground;
            p.landed = true;
        }
    } else {
        p.at = add(p.at, mul(p.velocity, dt));
        p.velocity = mul(p.velocity, std::max(0.0f, 1.0f - 7.0f * dt));
        p.at[1] = p.ground;
    }
}
}

std::string set(std::string_view owner, int bone, const Bone& value, Space space) {
    if (bone < first_writable || bone >= bones) return "a board's bone is 2 to 15";
    if (!finite(value.position) || length(value.position) > 1000 ||
        !std::all_of(value.rotation.begin(), value.rotation.end(), [](float v) { return std::isfinite(v); }))
        return "a bone's place is finite";
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (auto why = take(s, owner); !why.empty()) return why;
    s.wanted[bone] = {false, false, normalised(value.rotation), value.position, space};
    refresh_any(s);
    return {};
}
std::string hide(std::string_view owner, int bone, bool everyone) {
    if (bone < first_writable || bone >= bones) return "a board's bone is 2 to 15";
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (auto why = take(s, owner); !why.empty()) return why;
    s.wanted[bone] = {true, everyone};
    refresh_any(s);
    return {};
}
void clear(std::string_view owner, int bone) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (const auto parked = s.park_by.find(owner); parked != s.park_by.end() && bone < 0) {
        s.park_by.erase(parked);
        s.parked.clear();
        for (const auto& [who, list] : s.park_by) s.parked.insert(list.begin(), list.end());
    }
    if (s.owner == owner) {
        if (bone < 0) {
            s.wanted.clear();
            s.piece.reset();
        } else s.wanted.erase(bone);
        if (s.wanted.empty() && !s.piece) s.owner.clear();
    }
    refresh_any(s);
}
void park(std::string_view owner, std::vector<int> list) {
    std::erase_if(list, [](int b) { return b < first_writable || b >= bones; });
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.park_by[std::string(owner)] = std::move(list);
    s.parked.clear();
    for (const auto& [who, bones_of] : s.park_by) s.parked.insert(bones_of.begin(), bones_of.end());
    refresh_any(s);
}
std::string fit(std::string_view owner, std::optional<Fit> value) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!value) {
        if (fit_owner == owner) {
            fit_owner.clear();
            fitted.store(false, std::memory_order_release);
        }
        return {};
    }
    if (!(value->axle >= 0.8f && value->axle <= 1.6f)) return "a board's axle spread is 0.8 to 1.6 (1: the game's own)";
    if (!fit_owner.empty() && fit_owner != owner) return fit_owner + " fits the boards";
    fit_owner = owner;
    fit_plates.store(value->plates, std::memory_order_release);
    fit_axle.store(value->axle, std::memory_order_release);
    fitted.store(value->plates || value->axle != 1.0f, std::memory_order_release);
    return {};
}
std::string toss(std::string_view owner, std::vector<int> list, Vec3 velocity, Vec3 spin, float ground) {
    std::erase_if(list, [](int b) { return b < first_writable || b >= bones; });
    if (list.empty()) return "a piece is bones 2 to 15";
    if (!finite(velocity) || !finite(spin) || length(velocity) > 50 || length(spin) > 50 || !std::isfinite(ground))
        return "a piece leaves at most 50 m/s, turning at most 50 rad/s";
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (auto why = take(s, owner); !why.empty()) return why;
    Piece piece;
    piece.bones = std::move(list);
    piece.velocity = velocity;
    piece.spin = spin;
    piece.ground = ground;
    s.piece = std::move(piece);
    refresh_any(s);
    return {};
}
std::optional<std::array<Bone, bones>> read() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.drawn;
}
void tick(std::uintptr_t base, std::uintptr_t component) noexcept {
    std::uintptr_t holder{};
    try {
        if (component && base) {
            const auto collection = read_value<std::uintptr_t>(component + 0x18).value_or(0);
            const auto entity = collection ? read_value<std::uintptr_t>(collection).value_or(0) : 0;
            if (entity) holder = multiplayer::read_native_board(peek, base, entity).holder;
        }
    } catch (...) {}
    base_now.store(base, std::memory_order_release);
    holder_now.store(holder, std::memory_order_release);
    static bool sending{};
    if (!sending) {
        multiplayer::set_local_sent_pose_filter(&on_sent_pose);
        sending = true;
    }
}

void on_render_pose(std::uintptr_t animation_interface) noexcept {
    if (animation_interface <= 0xc0) return;
    const auto error = GetLastError();
    try {
        const auto base = base_now.load(std::memory_order_acquire);
        const auto holder = animation_interface - 0xc0;
        const bool local = holder == holder_now.load(std::memory_order_acquire) && holder;
        const bool fitting = fitted.load(std::memory_order_acquire);
        if (!local && !fitting && !any.load(std::memory_order_acquire)) {
            SetLastError(error);
            return;
        }
        const auto buffer = board_buffer(base, holder);
        if (!buffer) {
            SetLastError(error);
            return;
        }
        std::array<Lane, bones> lanes{};
        if (!peek(buffer, lanes.data(), sizeof(lanes))) {
            SetLastError(error);
            return;
        }
        auto& s = shared();
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            SetLastError(error);
            return;
        }
        // Out of sight: far below the board, from where its animation put it.
        const auto away = [&](int b) {
            const Vec3 at{lanes[b].position[0], lanes[b].position[1], lanes[b].position[2]};
            if (length(at) < 100.0f) (void)store(bone_at(buffer, b), Quat{lanes[b].rotation}, add(at, {0, -2000, 0}));
        };
        for (const int b : s.parked) away(b);
        if (fitting && fit_plates.load(std::memory_order_acquire)) {
            // Each baseplate on the deck, from its truck's bone as the animation left it (not while
            // the truck is off its place: a plugin moved it).
            for (const auto& rest : truck_rests) {
                const Quat facing = normalised(Quat{lanes[rest.truck].rotation});
                const Vec3 at{lanes[rest.truck].position[0], lanes[rest.truck].position[1], lanes[rest.truck].position[2]};
                if (!(length(sub(at, rest.at)) < 0.05f)) continue;
                const auto back = conjugate(facing);
                (void)store(bone_at(buffer, rest.plate), normalised(multiply(back, rest.facing)),
                            rotate(back, sub(add(rest.at, rotate(rest.facing, rest.effector)), at)));
            }
        }
        if (fitting) {
            // The deck's own trucks: the wheels' place along the axle is set rather than scaled, so a
            // pose that comes by twice is not moved twice.
            if (const float axle = fit_axle.load(std::memory_order_acquire); axle != 1.0f)
                for (const int b : wheel_bones) {
                    int way = 0; // the axle, in the truck's axes: the largest part of where the wheel sits
                    for (int i = 1; i < 3; ++i)
                        if (std::abs(lanes[b].position[i]) > std::abs(lanes[b].position[way])) way = i;
                    const float out = std::abs(lanes[b].position[way]);
                    if (out > 0.05f && out < 0.16f)
                        (void)store_float(bone_at(buffer, b) + (8 + way) * sizeof(float), std::copysign(wheel_offset * axle, lanes[b].position[way]));
                }
        }
        if (!local) {
            SetLastError(error);
            return;
        }
        std::array<Bone, bones> drawn{};
        for (int b = 0; b < bones; ++b)
            drawn[b] = {{lanes[b].position[0], lanes[b].position[1], lanes[b].position[2]}, Quat{lanes[b].rotation},
                        {lanes[b].scale[0], lanes[b].scale[1], lanes[b].scale[2]}};
        s.drawn = drawn;
        const Quat place_rotation = normalised(Quat{lanes[place_bone].rotation});
        const Vec3 place_at{lanes[place_bone].position[0], lanes[place_bone].position[1], lanes[place_bone].position[2]};
        const auto back = conjugate(place_rotation);
        const auto from_world = [&](const Quat& rotation, const Vec3& at) {
            return std::pair{normalised(multiply(back, rotation)), rotate(back, sub(at, place_at))};
        };
        for (const auto& [b, want] : s.wanted) {
            if (want.hidden) away(b);
            else if (want.space == Space::local) (void)store(bone_at(buffer, b), want.rotation, want.position);
            else {
                const auto [rotation, at] = from_world(want.rotation, want.position);
                (void)store(bone_at(buffer, b), rotation, at);
            }
        }
        if (s.piece) {
            auto& p = *s.piece;
            // A piece leaves from where the plugin last put its bones (the pose before this frame's
            // writes can hold them parked out of sight), else from where the animation put them.
            auto start = lanes;
            if (!p.started)
                for (const int b : p.bones)
                    if (const auto want = s.wanted.find(b); want != s.wanted.end() && !want->second.hidden) {
                        auto [rotation, at] = want->second.space == Space::local ? std::pair{want->second.rotation, want->second.position}
                                                                                 : from_world(want->second.rotation, want->second.position);
                        start[b].rotation = rotation;
                        start[b].position = {at[0], at[1], at[2], start[b].position[3]};
                    }
            fly(p, place_rotation, place_at, start);
            for (const auto& [b, rel] : p.held) {
                // The bone in the world: the piece's frame (which was the board's place as it left)
                // times the bone's pose then; written from wherever the board's place is now.
                const auto world_rotation = normalised(multiply(p.facing, rel.first));
                const auto world_at = add(p.at, rotate(p.facing, rel.second));
                const auto [rotation, at] = from_world(world_rotation, world_at);
                (void)store(bone_at(buffer, b), rotation, at);
            }
        }
    } catch (...) {
    }
    SetLastError(error);
}
}
