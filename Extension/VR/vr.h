#pragma once
// ReSkate VR: first person in an OpenXR headset.
//
// The game renders one view per frame. VR alternates the eye: each frame the
// first-person camera is moved to the next eye's pose from the headset, and
// the finished frame is copied into that eye's OpenXR swapchain at Present.
// The compositor shows the newest image of each eye, reprojected to the head's
// current pose. This is often called alternate eye rendering. Each eye updates
// at half the game's frame rate; the headset paces the game through xrWaitFrame.
//
// While the VR camera is not active or a menu is open (loading screens, third person),
// the game image is shown on a flat screen in the headset instead.
//
// Threads:
//   apply_camera / camera_fov    engine thread (first-person camera writes)
//   on_present / after_present   the game's Present thread (overlay hook)
//   everything else              any thread
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct IDXGISwapChain3;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace dingosdk::vr {
struct Settings {
    bool enabled = false;
    // While the headset runs, first person (which carries the VR camera, also for the third
    // person views) turns on by itself, also after a map change. Off: the game's own camera
    // shows on a flat screen until first person is turned on.
    bool auto_first_person = true;
    // Headset translation moves the camera; off keeps only the eye offset.
    bool positional = true;
    // Only the skater's heading turns the view; the horizon stays level.
    // Off also follows the animated head's pitch and roll (strong motion).
    bool level_horizon = true;
    // Seconds for the view to follow the skater's heading; 0 follows directly.
    float turn_smoothing = 0.61f;
    // Metres of game world per metre of real head movement and eye separation.
    float world_scale = 1.11f;
    // Rendered field of view relative to the headset's; above 1 leaves margin
    // for reprojection at the cost of resolution.
    float fov_scale = 0.9f;
    // Whether the native camera FOV is vertical (true) or horizontal.
    bool fov_vertical = true;
    // Multiplies the FOV tangent written to the game only (not the angles the
    // headset is told). 1 when the game renders the FOV it is given; the right
    // value is the one where the world stays still while the head turns (any
    // mismatch warps the view).
    float fov_correction = 1.23f;
    // Presents between a camera write and the image it produced. If depth
    // looks inside out or doubled, the eyes are paired with the wrong images:
    // try the next value.
    int frame_lag = 2;
    float theater_distance = 2.5f;
    float theater_width = 3.2f;
    // ReSkate's chat while the VR camera runs: on its own panel (chat_in_vr; otherwise drawn
    // into the game image), on the left controller (chat_on_hand; the view's spot while the
    // controller is not tracked). Typing (PC keyboard) keeps the VR view with a panel.
    bool chat_in_vr = true, chat_on_hand = true;
    // The view's chat panel (also the left hand's fallback): width and the centre's offset from
    // straight ahead, in metres at 0.85 m; it turns to face the eyes.
    float chat_view_size = 0.34f, chat_view_x = -0.30f, chat_view_y = -0.22f;
    // How the two eyes are made: 0 alternate eyes (the camera moves to each eye
    // in turn); 1 side-by-side input (the camera stays between the eyes and a
    // stereo shader, e.g. ReShade SuperDepth3D, draws left|right halves into
    // every frame; both eyes update with every camera frame); 2 depth stereo
    // (built in: both eyes from each frame and the game's depth buffer).
    int stereo_mode = 2;
    // Depth stereo: multiplies the eye shift (1 = real eye separation).
    float depth_strength = 0.6f;
    // Turn the game's vignette and chromatic aberration off while VR runs (they
    // add colour fringes that tear in stereo); your saved choices are kept.
    bool lens_effects_off = true;
    // The headset's controllers act as the gamepad (pad 0, merged with a real pad).
    bool controllers = true;
    // Depth stereo edges: pixels the near depth is widened by (0-3; more keeps soft
    // edges on their object, less keeps narrow gaps open), and how holes behind near
    // objects are filled: 1 background, 0 stretched near edge.
    int edge_widening = 1;
    int gap_fill = 1;
    // First person looks through the skater's eyes (math::eyes_heading and eyes_position:
    // the head through continuous filters; on foot, Walk where you look). Third person follows
    // the direction of travel, like the game's camera.
    float tilt_filter = 0.3f; // s: pitch/roll wobble with Level horizon off (flips pass at once)
    // 0 first person; 1 third person (the camera orbits behind the skater at
    // third_distance and third_height; the head is not hidden).
    int view_mode = 0;
    float third_distance = 2.5f, third_height = 0.5f;
    // First person on foot: the view turns only with the headset (and stick turns), not with
    // the body, so the stick walks where you look. The game moves the skater relative to the
    // camera; a view that followed the walk direction fed back into the stick (sideways input
    // circled). Off: the eyes on foot too.
    bool walk_where_you_look = true;
    // First person: during a wipeout the view moves to a fixed spot 3 m behind and 1.2 m
    // above where the skater fell, level, with the whole body shown, until they are up.
    bool bail_camera = true;
    // Comfort vignette: how far the edges darken while the view turns or tilts without your
    // head (body turns, snap and look turns, flips, the bail camera); 0 off, 1 strong.
    float comfort_vignette = 0.0f;
    // Right-stick turns (on foot with walk where you look, and orbiting the outside views):
    // 0 snap by look_turn_degrees per flick (0 off), 1 smooth at smooth_turn_speed degrees/s.
    int look_turn_style = 1;
    int look_turn_degrees = 30;
    float smooth_turn_speed = 120.0f;
    // Grabs (in the air with a trigger held), first person: the view moves outside like the
    // flip & bail view (grab_view), or stays and shows the whole body (grab_body).
    bool grab_view = false;
    bool grab_body = false;
    // How much crouching (before an ollie, pushing) lowers the view, 0 none, 1 all of the
    // skater's drop.
    float crouch_follow = 0.5f;
    // How much of the skater first person hides in VR, on the board (hide_body) and on
    // foot (hide_body_foot): 0 the head, 1 the neck and head (no collar or hood across
    // the view), 2 the upper torso, neck and head with the arms kept (the shoulders move
    // to the chest centre; in multiplayer other players see the arms scaled up), 3
    // everything above the waist, arms included, 4 all but the feet: the FeetOnly mod's
    // costume (vr_costume.cpp; without the mod it falls back to 3).
    int hide_body = 3;
    int hide_body_foot = 1;
    // Feet only: other players still see the saved outfit (the costume is only for this view).
    bool others_see_outfit = true;
    // Seat position relative to the skater's eyes, in metres (forward is positive), level
    // along the view's heading, on the board; the *_foot set applies on foot, and the view
    // blends between them in about half a second.
    // Slightly back by default: the eyes sit at the front of the hidden head.
    float seat_forward = -0.45f, seat_up = 0.17f;
    // Sideways seat offset in metres (right is positive), e.g. with the chest heading.
    float seat_side = -0.14f;
    float seat_forward_foot = -0.45f, seat_up_foot = 0.17f, seat_side_foot = -0.14f;
    // Virtual-key code that recenters the view; 0 disables it. Default F8.
    int recenter_key = 0x77;
};

struct Limits {
    static constexpr float turn_smoothing_max = 2.0f;
    static constexpr float world_scale_min = 0.25f, world_scale_max = 4.0f;
    static constexpr float fov_scale_min = 0.8f, fov_scale_max = 1.5f;
    static constexpr float fov_correction_min = 0.7f, fov_correction_max = 2.2f;
    static constexpr float seat_min = -0.5f, seat_max = 0.5f;
    static constexpr float seat_forward_min = -1.5f; // far back, for bodies that stay visible
    static constexpr int frame_lag_max = 4;
    static constexpr int hide_body_max = 4, hide_feet_only = 4;
    static constexpr int view_mode_max = 1, stereo_mode_max = 2, look_turn_max = 90;
    static constexpr float comfort_vignette_max = 1.0f;
    static constexpr float smooth_turn_speed_min = 30.0f, smooth_turn_speed_max = 360.0f;
    static constexpr float depth_strength_max = 3.0f;
    static constexpr float tilt_filter_max = 1.0f;
    static constexpr int edge_widening_max = 3;
    static constexpr float third_distance_min = 0.5f, third_distance_max = 8.0f;
    static constexpr float third_height_min = -1.0f, third_height_max = 3.0f;
    static constexpr float theater_distance_min = 0.5f, theater_distance_max = 20.0f;
    static constexpr float chat_view_size_min = 0.1f, chat_view_size_max = 1.0f, chat_view_offset_max = 0.6f;
    static constexpr float theater_width_min = 0.5f, theater_width_max = 30.0f;
};

bool valid(const Settings& settings) noexcept;
Settings settings() noexcept;
// Applies validated settings; returns false (and changes nothing) otherwise.
bool set_settings(const Settings& settings) noexcept;
// True once after any change, for the profile to save.
bool take_settings_changed() noexcept;
void recenter() noexcept;

// Named sets of VR settings. Built-in presets come first and cannot be deleted;
// saved ones live in the local profile. Applying keeps VR on/off and the
// recenter key.
struct Preset {
    std::string name;
    Settings settings;
    bool builtin = false;
};
std::vector<Preset> presets();
bool apply_preset(const std::string& name) noexcept;
// Saves the current settings under `name`, replacing a saved preset of that
// name; false for an empty name or a built-in one.
bool save_preset(const std::string& name) noexcept;
bool delete_preset(const std::string& name) noexcept;
// For the profile: the saved presets, and whether they changed since asked.
std::vector<Preset> saved_presets();
void set_saved_presets(std::vector<Preset> saved) noexcept;
bool take_presets_changed() noexcept;

// The overlay reports whether a menu, console or chat is open. They are drawn
// into the game image, so while one is open the headset shows the flat screen.
void set_ui_open(bool open) noexcept;
// The chat panel (Settings::chat_panel), Present thread. While wants_chat_panel(), the overlay
// draws the chat into its own texture (the back buffer's format, state COMMON, the top-left
// width x height pixels used; 0 x 0 when there is no chat to show) and hands it over each frame.
bool wants_chat_panel() noexcept;
void submit_chat_panel(ID3D12Resource* texture, std::uint32_t width, std::uint32_t height) noexcept;
// Same for the game's own main or pause menu (client thread).
void set_game_menu(bool open) noexcept;
bool game_menu_open() noexcept;

struct Status {
    bool loader = false, instance = false, session = false, running = false, focused = false;
    bool camera_active = false, theater_active = false;
    std::string runtime, system, state = "off", message;
    std::uint32_t image_width = 0, image_height = 0;
    std::uint32_t recommended_width = 0, recommended_height = 0;
    float display_hz = 0, present_hz = 0, camera_hz = 0;
    float render_fov_degrees = 0;
    std::uint64_t frames_submitted = 0, eye_images = 0, repeated_images = 0;
    // Thread ids of the last VR camera write and Present, for diagnosing how
    // images pair with camera frames.
    std::uint32_t camera_thread = 0, present_thread = 0;
};
Status status();

enum class CameraWriter { tick, render };
// The skater's body this frame: its origin (world position) and the heading of
// its chest in radians (NaN when unknown).
struct BodyPose {
    std::array<float, 3> origin{};
    float chest_heading = 0;
    bool on_board = false;
    std::uint32_t physics_state = 0; // the game's skater physics state (300 wipeout); 0 unknown
    std::array<float, 16> board{};   // the board's world matrix while on it; zero otherwise
};
// Moves a native camera matrix (rows right, up, backward, position; fourth
// lanes left as they are) to this frame's eye. Returns false, leaving the
// matrix unchanged, when VR is not driving the camera. `body` steadies the
// camera against quick body motion and gives the heading sources; its origin is
// ignored when it is not within 3 m of the head.
bool apply_camera(std::array<float, 16>& matrix, CameraWriter writer, const BodyPose* body = nullptr) noexcept;
// Depth stereo: hooks D3D12 device creation (call at startup, before the game
// creates its device) to find the scene depth buffer.
void start_depth_probe() noexcept;
// Present thread: picks the scene depth for this image size.
void depth_on_present(std::uint64_t width, std::uint32_t height) noexcept;
// Copies the scene depth (previous frame) at each of its clears while enabled.
void set_depth_copy(bool enabled) noexcept;
// A gamepad state the overlay's XInput hook merges into pad 0 (sticks -32768..32767,
// XINPUT_GAMEPAD_* buttons, triggers 0..255).
struct VirtualPad {
    bool active = false;
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0, right_trigger = 0;
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};
// The headset's controllers as a gamepad (vr_input.cpp); inactive unless VR runs
// focused with `controllers` on.
VirtualPad controller_pad() noexcept;
// The right stick X the game read for pad 0 (look turns on foot).
void observe_gamepad(std::uint32_t user, std::int16_t right_x, std::uint8_t trigger) noexcept;
// Pad 0 reports connected from the game's first poll (VR controllers on).
bool controllers_connected() noexcept;
// The right stick X of a DirectInput gamepad (DualShock 4 / DualSense), read directly
// (vr_gamepad.cpp); 0 when there is none.
std::int16_t directinput_right_x() noexcept;
// The strongest right stick X and trigger of XInput slots 0-3, polled by ReSkate (Steam
// Input's virtual pad appears after the game's only check of the empty slots).
struct PadPeek {
    std::int16_t right_x = 0;
    std::uint8_t trigger = 0; // the more pressed of the two
};
PadPeek xinput_peek() noexcept;
// First person shows the whole skater now (the flip & bail view, grabs).
bool whole_body_view() noexcept;
// Whether first person last saw the skater on the board (riding, air and grinds count),
// which picks hide_body or hide_body_foot.
void note_on_board(bool on_board) noexcept;
bool on_board_view() noexcept;

// Engine thread (debug tick): true when first person should be turned on for VR (at most
// once a second while it is off and the headset runs with auto_first_person).
bool wants_first_person(bool first_person_on) noexcept;
// The native FOV to write while VR drives the camera; `requested` otherwise.
float camera_fov(float requested) noexcept;

// Present hook. on_present runs before the game's Present with the presenting
// swapchain and its queue; after_present runs after it returns, on the same
// thread, only when on_present ran.
void on_present(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue) noexcept;
void after_present() noexcept;
}
