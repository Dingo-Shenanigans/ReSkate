#pragma once
#include "anim_core.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// FOURTH Animation Studio in the game: records a trick off the local skater's own animation, shows it
// on the skater while the studio is open, and puts the player's keyframes on the native animation.
//
// Threads: the overlay (render thread) reads view() and calls the commands; the client tick (engine
// thread) calls tick() and camera(); the local skater's animation update (often a worker thread)
// calls on_animation() right after the native graph wrote its pose (native_skater.cpp's listener),
// the same point first person uses to shrink the head. Only the joints of that one pose buffer are
// ever written, never more than its own joint count, and only after its layout was checked.
namespace dingosdk::anim_studio {
struct JointView {
    std::uint16_t joint{};
    Vec3 position{};             // world, with the edits
    Quat rotation{0, 0, 0, 1};    // world, with the edits
    Quat parent_rotation{0, 0, 0, 1};
    float parent_scale{1};
};
struct KeyView {
    float time{};
    Ease ease{};
};
struct TrackView {
    std::uint16_t joint{};
    std::vector<KeyView> keys;
    bool hidden{}, locked{};
};
struct Orbit {
    float yaw{0.6f}, pitch{0.18f}, distance{3.4f};
    Vec3 pan{}; // added to the skater's centre
};
enum class Recording : std::uint8_t { off, waiting, in_air, finishing };
struct View {
    bool open{};       // the studio has the screen
    bool pose_seen{};  // a pose of the local skater was read lately
    std::string status, failure;
    // The camera the studio drives (rows right, up, back, position) and its vertical FOV in degrees.
    std::array<float, 16> camera{};
    float fov{};
    bool camera_valid{};
    // The animation being edited.
    std::string name, trick;
    bool has_clip{}, dirty{}, playing{}, loop{true}, play_in_game{};
    float duration{}, takeoff{-1}, landing{-1}, time{}, speed{1};
    std::vector<JointView> joints; // the editable joints, now
    std::vector<TrackView> tracks;
    std::vector<std::string> library; // saved animations, by name
    Recording recording{};
    bool can_undo{}, can_redo{};
    Orbit orbit;
    bool confirm_close{}; // closing was asked with unsaved changes: the studio asks first
    std::string pop;      // "" any, "low", "mid", "high"
    std::uint64_t revision{};
};
View view();
// Cheap, any thread: whether the studio has the screen.
bool is_open() noexcept;

// ---- commands from the overlay -------------------------------------------------------------------
void request_open(bool open);         // asks the client tick to open or close the studio
void request_close();                 // closes, or asks first when there are unsaved changes
enum class CloseAnswer { save, discard, cancel };
void answer_close(CloseAnswer answer);
void set_pop(const std::string& pop); // which pop this animation is for ("" any)
bool copy_to_pops(std::string& message); // saves a low, a mid and a high copy of it
void advance(float seconds);          // playback, by the render frame's time
void set_time(float seconds);
void set_playing(bool playing);
void set_loop(bool loop);
void set_speed(float speed);
void record_next_trick();             // closes the studio until the next trick has been landed
void cancel_recording();
void set_play_in_game(bool on);
// Keyframes. `begin_change` saves the state for undo once per gesture (a drag is one change).
void begin_change();
Offset offset_of(std::uint16_t joint); // the joint's edit at the current time
void set_offset(std::uint16_t joint, const Offset& value); // keys it at the current time
void delete_key(std::uint16_t joint, float time);
void move_key(std::uint16_t joint, float from, float to);
void set_ease(std::uint16_t joint, float time, Ease ease);
void reset_joint(std::uint16_t joint); // takes all its keys off
void set_track_flags(std::uint16_t joint, bool hidden, bool locked);
void copy_pose();  // every joint's edit at the current time
void paste_pose(); // keys them all at the current time
void undo();
void redo();
// Library.
void new_animation();
bool save(std::string& message);
bool load(const std::string& name, std::string& message);
// The game's own tricks, made into studio clips by contrib/animator-studio/make_trick_library.py:
// <folder>/library/catalog.json and its .clip files.
struct GameTrick {
    std::string category, label, trick, pop, file;
};
const std::vector<GameTrick>& game_tricks(); // read once
bool load_game_trick(const GameTrick& trick, std::string& message);
bool remove(const std::string& name, std::string& message);
void rename(const std::string& name);
void set_trick(const std::string& trick);
// Camera.
void set_orbit(const Orbit& orbit);
void focus_joint(std::uint16_t joint); // the orbit centres on it
// How far above and right of the screen's middle the free area beside the menu has its middle, both
// as shares of the screen's height: the camera aims so the skater sits there.
void set_view_shift(float share, float right_share);

// ---- the game side -------------------------------------------------------------------------------
// Engine thread, every client tick with the local skater's animation component (0 when there is
// none). Installs the animation listener the first time it is needed. Returns the open state the
// studio wants (a finished recording opens it again).
struct TickResult {
    bool want_open{}, changed{};
};
TickResult tick(std::uintptr_t base, std::uintptr_t component, bool open, float camera_fov);
// Shown on the studio's status line (why it could not open, say).
void report(std::string message);
// Engine thread, while open: the studio camera's matrix (rows right, up, back, position).
bool camera(std::array<float, 16>& matrix);
// Animation thread: the local skater's pose was just evaluated.
void on_animation(std::uintptr_t component) noexcept;
// Engine thread: a pose was handed to the renderer (`animation_interface` = animation holder + 0xc0).
void on_render(std::uintptr_t animation_interface) noexcept;
void on_board_render(std::uintptr_t animation_interface, std::uintptr_t render_data) noexcept;
} // namespace dingosdk::anim_studio
