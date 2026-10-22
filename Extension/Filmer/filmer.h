#pragma once
// The Filmer: skate. filmed the way a skater with a VX1000 films a line.
//
//   follow  an AI filmer rides with a skater (you, or someone in the session) and films them, with
//           a filmer's habits: late corrections, a loose frame, a hand that is never quite still,
//           but never losing the trick.
//   film    you are the filmer: the left stick rides your skater, the right stick points the camera
//           (the game no longer sees it, so no flicks while you film). The assist keeps a friend in
//           frame; the stick nudges the framing.
//
// Either way the overlay can make the picture look like it came off a VX1000 with a Century MK1
// fisheye (the overlay's screen look), and a hand slapped over the lens marks the end of a clip.
//
// The game thread owns the Filmer (filmer.cpp). Every change is a `filmer ...` console command, so the
// menu page only queues commands and reads the snapshot below.
#include "filmer_rig.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::filmer {
// What the overlay does to the picture while the Filmer has the camera. Everything can be switched off.
struct Look {
    bool enabled = true;     // the whole look
    bool fisheye = true;     // the MK1's bend (only with the fisheye lens)
    float bend = 1.0f;       // how much of it, 0..1
    bool bars = true;        // 4:3, black at the sides
    bool hood = true;        // the dark round edge of the fisheye's hood in the corners
    bool grade = true;       // the VX's colour: punchy, warm, a little crushed
    bool soft = true;        // standard definition: soft, with the camcorder's sharpening halo
    bool scanlines = true;   // interlaced fields
    bool grain = true;       // tape noise
    bool date = false;       // a date stamp in the corner
    bool viewfinder = false; // REC, the tape counter and the battery, as in the viewfinder
    bool anywhere = false;   // over any camera, not only the Filmer's (the replay editor's, say)
    // The tape and the camera's electronics.
    float age = 0.35f;        // tape age 0..1: how often and how badly the tape misbehaves
    bool dropouts = true;     // now and then a white streak where the tape lost its coating
    bool head_switching = true; // the torn, noisy lines at the very bottom
    bool tracking = true;     // a band of tearing rolls up the picture as each clip starts recording
    bool smear = true;        // reds trail to the right
    bool iris = true;         // the auto-iris lags the scene: blown out into the sun, murky into shade
};

struct Settings {
    Mode mode = Mode::off;
    // The game's camera height button (the right stick click) cycles: high, low, ReSkate's first person
    // (when that is in the cycle, Skater settings), the Filmer, and back to high.
    bool camera_cycle = true;
    // follow: D-pad left / right sends the filmer to that side of you (and keeps them there, round
    // whatever is coming up); the same again hands the choice back to them. Off: the D-pad is the game's.
    bool side_buttons = false;
    RigSettings rig;
    Look look;
    bool hide_hud = true;   // hide the game's interface while filming
    bool show_debug = false; // the menu's DEBUG tab (for tuning and testing)
    bool take_stick = true; // film mode: keep the right stick from the game
    bool hide_nametags = true; // film mode: no nametags over the others while you film
    // follow mode: the left stick's click is the filmer's (switch side, left / right), kept from the
    // game while following.
    bool stick_clicks = true;
    // follow: whom the AI films (0: you). film: whom the assist frames (0: whoever is closest ahead).
    std::uint64_t subject{}, target{};
    // The camera in your hand (filmer_held.h). You film: your arm holds it out where the lens is, and
    // the others see it there (they need this build).
    bool left_hand = false;   // which hand holds it
    std::string camera_item;  // that cosmetic, put in your hand while you film (filmer_outfit.h); empty: the default
    std::string camera_slot;  // the outfit slot that carries it ("hand", "eyewear"); empty: hand
    // You film: the camcorder is always worn in the hand and held in both, the skater crouched and
    // hunched over it (on foot: the upper body still, the board out of sight).
    float crouch_depth = 0.22f; // how far the hips go down (metres)
    float hunch_degrees = 60.0f; // how far the back curls over toward the camera
};

struct PlayerRow {
    std::uint64_t id{};
    std::string name;
    float distance{};
};

// One clip: from the camera rolling (or the last slap) to a slap.
struct Clip {
    int number{};
    double seconds{};      // how long it ran
    std::string ended_at;  // local time, for finding it in a recording
};

struct View {
    std::uint64_t revision{};
    Settings settings;
    bool active{};            // the Filmer has the camera this moment
    std::string status;       // what it is doing, or why not
    std::string last;         // what the last command answered
    std::string subject_name; // who is being filmed
    std::vector<PlayerRow> players;
    float auto_angle{};       // auto: degrees round the skater
    bool planted{};           // follow: the filmer was told to stop and film from where they stand
    bool lagging{};           // follow: too far behind for good footy, rolling to a stop
    bool resting{};           // follow: you are still, and the filmer has settled behind you
    std::string doing;        // follow: what the filmer is up to
    std::string cost;         // follow: what it costs a frame (DEBUG)
    std::string menu_note;    // DEBUG: the game's menu data, the bytes that last changed
    bool at_edge{};           // follow: the filmer stopped where you went over a drop or a gap
    bool stationary{};        // the camera is held still: mouse look and the long lens's zoom work
    float zoom{1.0f};         // the long lens's zoom
    bool cruising{};          // cruise control is holding your speed
    std::string held_status;  // the camera in hand: the arm and the camcorder
    std::string cruise_note;  // why cruise control is not, when it is switched on
    int clip{1};
    std::vector<Clip> clips; // the latest few, newest last
};

// Thread-safe snapshot (filmer_view.cpp, part of the overlay).
std::shared_ptr<const View> view() noexcept;
void publish(std::shared_ptr<const View>) noexcept;
// Seconds on one steady clock shared by the game thread and the overlay.
double now() noexcept;

// Game thread (filmer.cpp).
// Each client tick, after the session placed the other skaters for this frame. `ready` while the
// local skater can be controlled; `camera_phase` while the native camera may be changed;
// `game_state` the client's state, said in the status while the Filmer waits.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase, int game_state) noexcept;
// Each client tick after tick() while the client may create entities (`in_level`): the cameras in
// filmers' hands (filmer_held.h).
void tick_held(std::uintptr_t base, std::uintptr_t client, bool in_level) noexcept;
// Runs one `filmer` command and returns the line to print. Only from the console dispatcher.
// `hud_hidden`: whether the game's interface is hidden right now (the Filmer hides it while it films
// and only shows it again if it was the one that hid it).
std::string command(std::string_view verb, const std::vector<std::string> &arguments, bool hud_hidden);


// The EFFECTS presets: Clean (no look), VX1000 (the look as it comes) and Worn tape (an old,
// dubbed-to-death tape). `look_preset` says which one a look is (-1: none of them, Custom);
// `apply_look_preset` sets one (Over any camera stays as it was).
enum class LookPreset : int { custom = -1, clean = 0, vx = 1, worn = 2 };
LookPreset look_preset(const Look &look) noexcept;
void apply_look_preset(Look &look, LookPreset preset) noexcept;

// Names, for the command line and the menu.
const char *mode_name(Mode) noexcept;
const char *style_name(Style) noexcept;
const char *lens_name(Lens) noexcept;
} // namespace dingosdk::filmer
