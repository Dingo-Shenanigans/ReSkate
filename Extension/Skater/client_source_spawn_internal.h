#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Game/Skater/first_person_spring.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Shared by the debug camera, noclip, first-person and party spectate controls:
// guarded native readers, engine-thread state and the native camera path.
namespace dingosdk::client_source::detail {
namespace spawn = addr::client_source_spawn;
constexpr std::uintptr_t source_highest = memory::highest_user_address;
using SourceCameraMode = void (*)(std::uintptr_t, std::uint32_t);
using SourceCameraTransform = void (*)(std::uintptr_t, const std::array<float, 16>*);
using SourcePhysicsUpdate = void (*)(std::uintptr_t);
using SourceSkaterMotion = void (*)(std::uintptr_t, std::uintptr_t, const std::array<float,16>*, std::uint8_t);
struct SourceGuard { const char* message; };
inline void source_require(bool value, const char* message) { if (!value) throw SourceGuard{message}; }
inline bool source_range(std::uintptr_t value, std::size_t size) {
    return value >= 0x10000 && size && value <= source_highest - size;
}
inline bool source_object(std::uintptr_t value) { return source_range(value, 8) && value % 8 == 0; }
struct SourceLastError {
    DWORD value{GetLastError()};
    ~SourceLastError() { SetLastError(value); }
};
// Releases SourceState::busy after a successful test_and_set.
struct SourceBusyScope {
    std::atomic_flag& busy;
    ~SourceBusyScope() { busy.clear(std::memory_order_release); }
};
struct SourceWatch {
    std::uintptr_t address{};
    std::size_t size{};
    std::array<unsigned char, 16> bytes{};
};
class SourceReader {
public:
    // A guarded same-process copy. The camera, skater and physics chains are
    // read (and verified) every frame, often several times: a system call per
    // field added up to thousands per frame.
    bool raw(std::uintptr_t address, void* result, std::size_t size) const {
        if (!source_range(address, size) || size > 4096) return false;
        return memory::peek_bytes(address, result, size);
    }
    template<class T> T value(std::uintptr_t address, std::uintptr_t offset = 0) {
        static_assert(sizeof(T) <= 16);
        source_require(source_range(address, 1) && offset <= source_highest - address,
                       "Source metadata pointer rejected.");
        address += offset;
        T result{};
        source_require(raw(address, &result, sizeof(result)), "Source metadata read unavailable.");
        source_require(watches_.size() < 24000, "Source read bound exceeded.");
        // One allocation for a whole chain (a camera snapshot watches ~60 fields).
        if (watches_.empty()) watches_.reserve(128);
        SourceWatch watch{address, sizeof(result)};
        std::memcpy(watch.bytes.data(), &result, sizeof(result));
        watches_.push_back(watch);
        return result;
    }
    std::uintptr_t pointer(std::uintptr_t address, std::uintptr_t offset = 0) {
        return value<std::uintptr_t>(address, offset);
    }
    std::size_t count(std::uintptr_t begin, std::uintptr_t end, std::size_t stride) const {
        source_require(begin <= end && end <= source_highest && begin % 8 == 0 && end % 8 == 0 &&
            (!end || begin >= 0x10000) && (end - begin) % stride == 0 && (end - begin) / stride <= 64,
            "Source collection bound rejected.");
        return static_cast<std::size_t>((end - begin) / stride);
    }
    void verify() const {
        for (const auto& watch : watches_) {
            std::array<unsigned char, 16> after{};
            source_require(raw(watch.address, after.data(), watch.size) &&
                std::memcmp(after.data(), watch.bytes.data(), watch.size) == 0,
                "Source metadata changed during observation.");
        }
    }
private:
    std::vector<SourceWatch> watches_;
};
struct SourceCameraIdentity {
    std::uintptr_t context{}, manager{}, controller{}, selector{}, node{}, camera{}, view{}, view_manager{};
    std::array<std::uintptr_t, 2> helpers{};
    std::uint32_t priority{};
    bool operator==(const SourceCameraIdentity&) const = default;
};
struct InteractiveDebug {
    bool camera_owned{}, camera_ambiguous{}, ui_owned{};
    bool park_editor{}, editor_transition{}, editor_previous_camera{}, editor_previous_ui{}, editor_previous_noclip{};
    bool editor_previous_first_person{};
    // Shares Freecam's owned mode-1 camera; each tick publishes the head pose
    // instead of a flight step.
    bool first_person{}, first_person_waiting{};
    // "Third person on foot": first person handed the camera back while the
    // skater walks and takes it again once they are back on the board.
    bool first_person_paused{}, first_person_last_on_foot{};
    ULONGLONG first_person_foot_since{}, first_person_retry_after{};
    // FreeCamera vertical FOV (+0xac): chosen value (0 = unchanged) and the value
    // it held before first person took it, restored when first person ends.
    float first_person_fov{}, first_person_saved_fov{};
    // Freecam FOV (0 = the game's own) and the camera's own value, put back
    // when Freecam ends or the setting returns to the default.
    float free_camera_fov{}, free_camera_saved_fov{};
    first_person::Settings first_person_settings;
    SourceCameraIdentity camera_identity;
    std::uintptr_t ui_object{};
    std::uint8_t ui_original{}, ui_applied{};
    DWORD thread{};
    std::array<float, 16> flight_matrix{};
    float flight_speed = 15.0f;
    double flight_time{};
    bool flight_ready{}, noclip{}, no_bail{};
    std::uintptr_t noclip_entity{};
    struct VelocityRequest {
        std::uintptr_t client{}, entity{}, core{};
        ULONGLONG expires{};
        std::array<float, 3> velocity{};
        bool valid{};
        bool applied{};
    } noclip_velocity;
    VelocityRequest forward_velocity;
    VelocityRequest up_velocity;
    VelocityRequest offboard_up_velocity;
    std::uint64_t noclip_velocity_updates{}, noclip_motion_updates{};
    std::uint64_t forward_velocity_updates{};
    float forward_velocity_speed = 20.0f;
    std::uint64_t up_velocity_updates{};
    std::uint64_t offboard_up_velocity_updates{};
    float up_velocity_speed = 20.0f;
    float offboard_up_velocity_speed = 20.0f;
    float noclip_altitude{};
    bool noclip_altitude_valid{}, noclip_altitude_offboard{};
    // Player choices kept in the local profile (see load_saved_debug):
    // loaded once, re-applied after level-load resets, saved shortly after a
    // change so dragging a slider does not write every frame.
    bool saved_loaded{}, save_pending{};
    ULONGLONG save_due{};
    // A saved No Bail comes back once the player controls a skater again: it
    // owns native skater state, and a level load needs that released first.
    bool no_bail_restore{};
    ULONGLONG no_bail_restore_after{};
    std::string status;
};
// Engine-thread state for the interactive debug controls and the native camera
// functions they call.
struct SourceTrial {
    InteractiveDebug debug;
    std::uintptr_t base{};
    SourceCameraMode camera_mode{};
    SourceCameraTransform camera_transform{};
};
struct SourceState {
    std::mutex initialization_mutex;
    std::atomic<bool> initialized{};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    bool velocity_guard_attempted{}; // Protected by initialization_mutex.
    std::atomic<bool> velocity_guard_active{};
    std::atomic<bool> free_camera_active{};
    std::atomic<std::uintptr_t> offboard_boost_core{};
    std::atomic<SourcePhysicsUpdate> velocity_update_original{};
    std::atomic<SourceSkaterMotion> motion_original{};
    SourceTrial trial;
};
struct SourceCameraSnapshot {
    SourceCameraIdentity identity;
    std::uintptr_t active{};
    std::uint32_t mode{};
    std::uint8_t input_active{}, view_enabled{};
};
struct NoclipBodies {
    std::uintptr_t core{}, context{}, rig_wrapper{};
    float seconds{}, board_height{};
    bool offboard{};
    bool wipeout{}; // Spread-eagle/torpedo use the native wipeout physics path.
    std::array<float, 3> root{}; // Entity root: the camera target and the idle motion target.
    std::array<std::uintptr_t, 32> parts{}; // Board 0..8, native skeleton velocity parts 1..23.
};
constexpr std::uintptr_t camera_fov_offset = 0xac;
constexpr float first_person_fov_min = 40.0f, first_person_fov_max = 120.0f;
struct FirstPersonArm {
    std::atomic<DWORD> thread{};
    std::uintptr_t component{}, camera{};
    SourceCameraTransform transform{};
    std::array<float, 16> matrix{};
    float fov{};
    first_person::Settings settings;
    first_person::FrameSpring spring;
    // Head poses captured right after each animation update of `watched`, on
    // whichever thread ran it. Captures only read memory; the camera is always
    // written by the engine thread.
    struct Snapshot { std::array<float, 16> head{}; first_person::Vec3 origin{}; double time{}; };
    std::mutex snapshot_mutex;
    Snapshot latest;
    std::uint64_t captures{};
    std::atomic<std::uintptr_t> watched{}, watched_base{};
};

// client_source_spawn.cpp
SourceState& source_state();
bool source_writable(std::uintptr_t address, std::size_t size);
// source_spawn_camera.cpp
SourceCameraSnapshot source_camera_snapshot(const SourceTrial& trial, std::uintptr_t client);
std::array<float, 16> debug_view_matrix(const SourceTrial& trial, const SourceCameraSnapshot& camera);
// client_debug.cpp
double source_flight_now();
std::array<float, 16> debug_skater(std::uintptr_t base, std::uintptr_t client, overlay::DebugModel& model);
// client_noclip.cpp
void debug_stop_noclip(InteractiveDebug& debug) noexcept;
NoclipBodies debug_noclip_bodies(std::uintptr_t base, std::uintptr_t client, std::uintptr_t entity);
// client_first_person.cpp
FirstPersonArm& first_person_arm();
void first_person_disarm() noexcept;
bool first_person_read(std::uintptr_t address, void* out, std::size_t size) noexcept;
bool first_person_write_fov(std::uintptr_t camera, float fov) noexcept;
void first_person_restore_fov(InteractiveDebug& debug) noexcept;
void free_camera_restore_fov(InteractiveDebug& debug) noexcept;
std::uintptr_t first_person_component(std::uintptr_t base, std::uintptr_t client);
// Whether the local skater is walking (physics state Offboard); empty if unreadable.
std::optional<bool> first_person_on_foot(std::uintptr_t base, std::uintptr_t client) noexcept;
first_person::Vec3 first_person_head_matrix(std::uintptr_t base, std::uintptr_t component, std::array<float, 16>& matrix);
void first_person_on_render(std::uintptr_t animation_interface) noexcept;
void first_person_on_animation(std::uintptr_t component) noexcept;

template<class T> void debug_write(std::uintptr_t address, T value) {
    SIZE_T written{};
    source_require(source_writable(address, sizeof(T)) &&
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(T), &written) &&
        written == sizeof(T), "Debug setting write failed.");
    SourceReader reader;
    source_require(reader.value<T>(address) == value, "Debug setting write could not be verified.");
}
}
