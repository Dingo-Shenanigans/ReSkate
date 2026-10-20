#pragma once
#include "anim_core.h"
#include "anim_library.h"
#include "anim_presets.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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
    bool smooth{}; // smoothed: the timeline draws it round
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
    std::string source_kind, source_asset, entry;
    std::string source_position_codec;
    std::size_t source_pose_count{};
    float source_max_sample_gap{};
    bool has_clip{}, dirty{}, playing{}, loop{true}, play_in_game{};
    bool room{true}; // the studio opens in its room (studio_room.h)
    bool solo{};     // a turn moves one joint alone (set_solo)
    // The turn from the axes a hand's or a foot's POSITION fields are measured in, into the axes the
    // joints are shown in. The gizmo's arrows point along it, so pulling one moves the field it names.
    Quat field_axes{0, 0, 0, 1};
    bool field_axes_valid{};
    float duration{}, takeoff{-1}, landing{-1}, time{}, speed{1};
    std::vector<JointView> joints; // the editable joints, now
    // The board without its keys, now, from where it starts: 0, 0, 0 is the board at the trick's start,
    // on the ground, as it lies there (Y up). The board's fields show it with its keys added.
    Joint board_base;
    bool board_base_valid{};
    std::vector<TrackView> tracks;
    std::vector<std::string> library; // saved animations, by name
    std::vector<Preset> presets;
    std::vector<std::string> packs;
    std::string active_pack;
    bool alternate_variants{};
    Recording recording{};
    float bracket_start{}, bracket_end{-1}; // the timeline brackets (Animation::start, end)
    bool can_undo{}, can_redo{};
    Orbit orbit;
    bool confirm_close{}; // closing was asked with unsaved changes: the studio asks first
    std::string pop;      // "" any, "low", "mid", "high"
    std::uint64_t revision{};
    // Goes up every time a clip arrives: a trick opened, a saved animation loaded, a take recorded, a new
    // animation started. The overlay starts clean on it.
    std::uint64_t clip_serial{};
};
View view();
// Opt-in 60-second read-only trace; exported under ReSkate.Tricks/research.
std::string research_trace(const std::string& action);
// Cheap, any thread: whether the studio has the screen.
bool is_open() noexcept;
// The studio's room (studio_room.h). Any thread.
bool room_wanted() noexcept;       // the Studio room row is on
void watch_room(bool on) noexcept; // look for the menu's skater in the room
bool room_skater_seen() noexcept;  // the menu's skater was drawn in the room just now
void set_in_room(bool on) noexcept; // the studio is open in the room: the skater is drawn there
// The loading screen over the walk to the room and back. `seconds`: how long it stays at least (it is
// set again while the walk goes on). Any thread.
enum class Loading : std::uint8_t { none, opening, leaving };
void set_loading(Loading what, float seconds) noexcept;
struct LoadingView {
    Loading what{};
    float alpha{};   // 0 when it does not show
    float seconds{}; // since it came up
};
LoadingView loading() noexcept;
// The client tick says whether the game is still loading (a level, a console load): the loading screen stays up
// until the game has been quiet for a second.
void game_loading_state(bool game_loading) noexcept;

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
// A recording starts from a clean level: take_clean_start() once per request, so the client tick loads the
// level again (behind the loading screen); clean_start_done() when that load is in. Nothing is recorded
// until then.
bool take_clean_start() noexcept;
void clean_start_done() noexcept;
// Record all tricks (trick_batch.h): the next landed trick is saved as recorded/<trick>__<pop>.clip.
struct BatchTake {
    bool ok{};
    std::string trick, pop, message;
    float air{};
};
void arm_batch_take();
void cancel_batch_take();
std::optional<BatchTake> take_batch_result();
void request_record_all();      // from the studio's TRICKS
bool take_record_all_request(); // the client tick takes it once
// The skater's physics state (100 riding on the ground, 504 on foot), or nothing.
std::optional<std::uint32_t> physics_state(std::uintptr_t base, std::uintptr_t component) noexcept;
void cancel_recording();
void set_play_in_game(bool on);
// Solo: a turn moves that joint alone. The editable joints hanging off it are turned back by as much,
// so the limb below keeps the way it faces instead of swinging with its parent. Bones are rigid, so
// those joints still travel with the one being turned: it is the way they face that is held.
void set_solo(bool on);
void set_room(bool on);               // the studio opens in its room, or where the skater is
// Keyframes. `begin_change` saves the state for undo once per gesture (a drag is one change).
void begin_change();
Offset offset_of(std::uint16_t joint); // the joint's edit at the current time
void set_offset(std::uint16_t joint, const Offset& value); // keys it at the current time
// Drags a limb: the joint goes to `target` (the skater's frame, as JointView::position is), the limb above it
// bends, and the bend is keyed at the current time. Call begin_change once when the drag starts.
void drag_limb(std::uint16_t joint, const Vec3& target);
// Which bracket: 1 start, 2 end. `seconds` is where it goes, on a whole frame. Only the keys sitting on
// that bracket travel with it; the keys between the brackets are left exactly where they are, so nothing
// is stretched. A key the bracket is pulled back past stops applying until it is taken out over it again.
void move_bracket(int which, float seconds);
// The joints that can be put somewhere rather than only turned, and how much of the limb above them bends
// to reach: 3 for a hand or a foot (the elbow or knee and the limb above it bend), 2 for an elbow or a
// knee (only the upper arm or the thigh turns, so placing a knee never swings the hips). 0 for the rest.
std::size_t limb_chain(std::uint16_t joint) noexcept;
// One of those joints' places, as the position fields show it: how far it is from its rest place. Setting it
// moves the joint there with the limb above it bent to reach (the bones keep their lengths); a point out of
// reach is brought to full stretch, never past it.
Vec3 limb_position(std::uint16_t joint);
void set_limb_position(std::uint16_t joint, const Vec3& offset);
void delete_key(std::uint16_t joint, float time);
void move_key(std::uint16_t joint, float from, float to);
void set_ease(std::uint16_t joint, float time, Ease ease);
// Smooths the keys of one joint, or of every joint when `joint` is `all_joints`. Call begin_change first.
inline constexpr std::uint16_t all_joints = 0xffff;
void smooth_keys(std::uint16_t joint);
void reset_joint(std::uint16_t joint); // takes all its keys off
void set_track_flags(std::uint16_t joint, bool hidden, bool locked);
void copy_pose();  // every joint's edit at the current time
void paste_pose(); // keys them all at the current time
void undo();
void redo();
// Library.
void new_animation();
bool save(std::string& message);
bool save_preset_copy(std::string& message);
bool activate_preset(const std::string& name, std::string& message);
bool create_preset_pack(std::string& message, const std::string& name = {});
bool use_preset_pack(const std::string& name, std::string& message);
bool set_pack_alternation(bool alternate, std::string& message);
bool include_preset(const std::string& name, bool included, std::string& message);
bool load(const std::string& name, std::string& message);
// The game's own tricks, made into studio clips by contrib/animator-studio/make_trick_library.py:
// <folder>/library/catalog.json and its .clip files.
struct NativeAsset {
    std::string category, asset, kind;
    std::string root_type, decode_state, detail, partition;
    std::string label, context, search;
};
struct NativeField { std::string label, path, value, reference; bool expandable{}; };
struct NativeDocumentPage { std::string title, error; std::vector<NativeField> fields; std::size_t pages{}; };
NativeDocumentPage native_asset_fields(const std::string& asset, const std::string& path, std::size_t page);
std::shared_ptr<const std::vector<NativeAsset>> native_assets();
std::shared_ptr<const std::vector<GameTrick>> game_tricks(); // cached until explicit refresh
void refresh_game_tricks(std::string& message);
bool export_diagnostics(std::string& message);
bool recorded_tricks();
bool load_game_trick(const GameTrick& trick, std::string& message);
bool remove(const std::string& name, std::string& message);
void rename(const std::string& name);
void set_trick(const std::string& trick);
// Camera.
void set_orbit(const Orbit& orbit);
void focus_joint(std::uint16_t joint); // the orbit centres on it, or comes off it when it is on it already
void set_focus(std::uint16_t joint);   // the orbit centres on it; all_joints for the whole skater
// Whether the camera travels with the skater. Off, it stays where it is in the world and he moves through
// it, which is what the free camera wants; on, it carries him along, which is what a set shot wants.
void set_camera_follow(bool on);
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
