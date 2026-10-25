#pragma once
#include "anim_core.h"
#include "anim_library.h"
#include "anim_presets.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// FOURTH Animation Studio in the game: opens one of the game's own tricks, shows it
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
    Quat rotation{0, 0, 0, 1}; // the key's edit, for the timeline's channel rows and curves
    Vec3 offset{};
    float blend_out{}; // seconds back to the game's pose; 0: on to the next key (Key::blend_out)
    float blend_in{};  // seconds in from the game's pose; 0: in from the key before (Key::blend_in)
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
struct View {
    bool open{};       // the studio has the screen
    bool pose_seen{};  // a pose of the local skater was read lately
    std::string status, failure;
    // The camera the studio drives (rows right, up, back, position) and its vertical FOV in degrees.
    std::array<float, 16> camera{};
    float fov{};
    bool camera_valid{};
    float base_yaw{}; // the yaw the orbit's own is added to (the skater's facing when the studio opened)
    // The animation being edited.
    std::string name, trick, author;
    std::string source_kind, entry;
    std::string source_position_codec;
    std::size_t source_pose_count{};
    float source_max_sample_gap{};
    bool has_clip{}, dirty{}, playing{}, loop{true}, play_in_game{};
    bool previewing{}; // a saved trick open to watch only (preview_saved): nothing is keyed or saved
    // The trick that was open when the preview started, put back when it ends (end_preview): its changes not saved yet,
    // and its name. Opening another trick to edit drops it, so ask first.
    bool kept_dirty{};
    std::string kept_name;
    bool room{true}; // the studio opens in its room (studio_room.h)
    bool solo{};     // a turn moves one joint alone (set_solo)
    // Which way round the skater stands in the studio: 0 as the game has him, 1 regular, 2 goofy, and
    // which that works out to right now.
    int stance_choice{};
    bool own_lead_right{}; // the skater's own stance leads with the right foot (goofy): stance versions are told apart by it
    bool showing_goofy{};
    bool grab_board{};   // a hand near the deck closes its fingers on it
    bool front_foot{true}; // a trick is bound to the front foot, not to the left and right
    bool limits{true};     // Joint limits: keyed joints stay inside the range the game's own animation uses
    float crossfade{0.3f}; // seconds a trick takes to hand back to the game's next move in game (Crossfade)
    // The turn from the axes a hand's or a foot's POSITION fields are measured in, into the axes the
    // joints are shown in. The gizmo's arrows point along it, so pulling one moves the field it names.
    Quat field_axes{0, 0, 0, 1};
    bool field_axes_valid{};
    // The ground he stands or rides on. Nothing that is moved is allowed under it.
    float floor{};
    bool floor_valid{};
    float duration{}, takeoff{-1}, landing{-1}, time{}, speed{1};
    std::vector<JointView> joints; // the editable joints, now
    // The board without its keys, now, from where it starts: 0, 0, 0 is the board at the trick's start,
    // on the ground, as it lies there (Y up). The board's fields show it with its keys added.
    Joint board_base;
    bool board_base_valid{};
    std::vector<TrackView> tracks;
    std::vector<std::string> library; // saved animations, by name
    std::vector<Preset> presets;
    struct PackView {
        std::string name, made_by;
        bool on{}, alternate{};
        std::vector<std::string> members;
        PresetPack pack; // its slots, which are Fixed or Alternate, and what is left out of a cycle
        std::string thumbnail;        // its picture (presets/<pack>.png, thumbnail_side square), "" none
        std::int64_t thumbnail_stamp{}; // when that was last written: a new one is read again
        bool favorite{};                // starred: shown first in PACKS
        bool installed{};               // made on another PC (its maker's key is not this one's): its tricks play, not open
        std::int64_t modified{};        // when its file was last written (the file clock's count), 0 not known
    };
    // MY TRICKS shows a trick's saves together (one card a trick: its pops and stances); these are shown on their own.
    std::set<std::string> ungrouped;
    std::vector<PackConflict> conflicts; // tricks more than one pack that is on has, and what plays them
    // Where packs are exported to and imported from: as set (SETTINGS > Packs), or "" for ReSkate.Tricks\exports and \import.
    std::string export_folder, import_folder;
    std::string export_path, import_path; // and where that is, in full (UTF-8)
    std::string library_path;             // ReSkate.Tricks itself (UTF-8)
    std::string test_trick;    // Test in game: the trick played in game until the studio opens again
    std::string test_returned; // back from testing it: the studio says something about it (clear_test_returned)
    std::vector<PackView> packs;
    std::string open_pack; // the pack open in PACKS
    // The playlists (PACKS > PLAYLIST): any number, one on at a time (playlist_playing, "" none). The one on decides what
    // plays in game instead of the packs that are on. playlist_mode and playlist are the open one's (playlist_open, the one
    // the page shows); playlist_on: one is on.
    struct PlaylistView {
        std::string name;
        std::size_t size{};
    };
    std::vector<PlaylistView> playlists;
    std::string playlist_open, playlist_playing;
    bool playlist_on{};
    PlaylistMode playlist_mode{PlaylistMode::fixed};
    std::vector<PlaylistEntry> playlist;
    bool hover_preview{true}; // TRICKS: a trick hovered plays in a little window
    // The editor's keys rebound in SETTINGS > STUDIO > EDITOR KEYS: an action's name to its key's name (the overlay's
    // names). Only those not on their own key.
    std::map<std::string, std::string> keys;
    std::string your_name; // MADE BY, remembered for new tricks and packs
    float bracket_start{}, bracket_end{-1}; // the timeline brackets (Animation::start, end)
    // CATCH on the timeline: the moment the trick is caught, held in the air off a bigger drop or down stairs before it
    // plays on to its landing. catch_air: it has air to hold one in; catch_at: -1 when none was found; catch_set: its
    // maker put it there (else the studio found it).
    bool catch_air{}, catch_set{};
    float catch_at{-1};
    // Joints keyed in the air whose pose runs on past LAND to END ANIM. (keys_past_landing): the timeline offers to key them
    // back to the game's pose at the landing.
    std::vector<std::uint16_t> keys_past_landing;
    // THIS TRICK: its own Crossfade (empty: the main one), its CATCH HOLD (catch_hold_set: its maker set it), and the
    // studio's DROP PREVIEW (metres, 0 off) with the extra air that drop gives it (seconds; -1 off).
    std::optional<float> trick_crossfade;
    float catch_hold{default_catch_hold};
    bool catch_hold_set{};
    float drop_preview{}, drop_extra{-1};
    // The open trick's stance (THIS TRICK > STANCE): 0 your own, 1 fakie, 2 switch, 3 switch fakie; -1 none (a game piece).
    int trick_stance{-1};
    bool can_undo{}, can_redo{};
    Orbit orbit;
    bool confirm_close{}; // closing was asked with unsaved changes: the studio asks first
    std::string pop;      // "" any, "low", "mid", "high"
    std::uint64_t revision{};
    // Goes up every time a clip arrives: a trick opened, a saved animation loaded, a new animation started. The overlay starts clean on it.
    std::uint64_t clip_serial{};
};
View view();
// Arms a one-off search of the running game for where it keeps the clip it is playing. It is taken riding,
// at the pop and in the air of the next trick, and written to ReSkate.Tricks/research/live-graph-probe.txt.
// Read only. Returns what it said.
std::string arm_live_probe();
// Turns the stance readout on or off (ReSkate.Tricks/research/stance-readout.txt). Returns what it said.
std::string toggle_stance_readout();
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
void advance(float seconds);          // playback, by the render frame's time
void set_time(float seconds);
void set_playing(bool playing);
void set_loop(bool loop);
void set_speed(float speed);
// The skater's physics state (100 riding on the ground, 504 on foot), or nothing.
std::optional<std::uint32_t> physics_state(std::uintptr_t base, std::uintptr_t component) noexcept;
void set_play_in_game(bool on);
// Solo: a turn moves that joint alone. The editable joints hanging off it are turned back by as much,
// so the limb below keeps the way it faces instead of swinging with its parent. Bones are rigid, so
// those joints still travel with the one being turned: it is the way they face that is held.
void set_solo(bool on);
// Which way round the skater stands in the studio: 0 follows the game's own setting, 1 regular, 2 goofy.
// A clip authored the other way round is mirrored to show it, the keys staying anatomical (a left-hand key
// is still the left hand), which is how gameplay lays them over the native pose.
void set_stance(int choice);
// A hand brought near the board closes its fingers round the deck, by how near it is. The fingers are not
// keyed and not saved: this is the studio showing what the hand is doing, not an edit.
void set_grab(bool on);
// How many times the Studio room setting was switched: the client opens the studio in the room, or leaves it, at once.
std::uint64_t room_changes() noexcept;
// In game: how long a trick takes to fade out into the game's next move (a manual, the next trick), 0.1 to 0.8 s;
// one trick's keys blend into the next's over half of it. Saved with the studio's settings.
void set_crossfade(float seconds);
// Joint limits: on, every joint you key stays inside the range the game's own animation uses (knees and
// elbows bend one way, nothing twists round), while you pose and in game. Off, joints go where you push them.
void set_joint_limits(bool on);
// How a saved trick is matched to the skater doing it. On, a trick belongs to the front foot and the back
// foot: it plays for any stance, mirrored when the other foot leads. Off, it plays only for the stance and
// switch it was recorded in, which is what it did before.
void set_front_foot(bool on);
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
// CATCH: where the trick is caught (on a frame, between a frame after the pop and a frame before the landing); below 0
// gives it back to the studio. Call begin_change once when a drag starts.
void set_catch(float seconds);
// THIS TRICK: its own Crossfade in seconds (below 0: the main one again), its CATCH HOLD (0 to 1), and the studio's DROP
// PREVIEW (metres, 0 off; nothing saved). Call begin_change once when a drag of the first two starts.
// THIS TRICK > STANCE: the open trick as its normal (0), fakie (1), switch (2) or switch fakie (3) version, saved so.
void set_trick_stance(int version);
void set_trick_crossfade(float seconds);
void set_catch_hold(float amount);
void set_drop_preview(float metres);
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
// The key's blend in (`in`) or out. 0 takes it off; anything else is kept between min_blend and max_blend. Call
// begin_change first.
void set_blend(std::uint16_t joint, float time, bool in, float seconds);
// Smooths the given keys (joint, time): each eases into its neighbours and shows round. Call begin_change first.
bool smooth_chosen_keys(std::span<const std::pair<std::uint16_t, float>> chosen, std::string& message);
// Every joint at once (set_focus: the camera on the whole skater).
inline constexpr std::uint16_t all_joints = 0xffff;
void reset_joint(std::uint16_t joint); // takes all its keys off
// Keys the joints whose air pose runs past the landing (View::keys_past_landing) back to the game's own pose at LAND.
bool settle_at_landing(std::string& message);
void set_track_flags(std::uint16_t joint, bool hidden, bool locked);
void copy_pose();  // every joint's edit at the current time
void paste_pose(); // keys them all at the current time
void undo();
void redo();
// Library.
bool save(std::string& message);
// Packs. Any number can be on at once (a trick more than one has is chosen between: View::conflicts); with none
// on, every saved animation plays. Every change to a pack is saved at once and plays at once.
bool create_preset_pack(std::string& message, const std::string& name = {}); // empty, off, and opened
void open_pack(const std::string& name); // the pack open in PACKS; "" for the list of packs
bool set_pack_on(const std::string& name, bool on, std::string& message);
bool set_pack_member(const std::string& pack, const std::string& animation, bool in, std::string& message);
// Where the pack has more than one of a trick and pop (`slot`, preset_slot): Fixed plays its first one,
// Alternate one of the ones linked into its cycle each attempt, at random (never the same twice running).
bool set_pack_mode(const std::string& pack, const std::string& slot, bool alternate, std::string& message);
bool set_pack_fixed(const std::string& pack, const std::string& animation, std::string& message); // the one that plays
bool set_pack_linked(const std::string& pack, const std::string& animation, bool linked, std::string& message);
// Packs' pictures: a square this many pixels a side, saved as presets/<pack>.png beside the pack.
inline constexpr unsigned thumbnail_side = 512;
// A picture for a pack: PNG bytes of a thumbnail_side square (empty takes it off). It is exported with the pack.
bool set_pack_thumbnail(const std::string& pack, const std::string& png, std::string& message);
// How the pack's cover was made in the cover designer (its JSON), kept with the pack so the designer opens on it again.
bool set_pack_cover_design(const std::string& pack, const std::string& design, std::string& message);
// The pack's name changed (its file, its picture, and wherever it is on or chosen with it).
bool rename_pack(const std::string& from, const std::string& to, std::string& message);
// Pack rules: the pop every trick in the pack plays on: "" each its own, "any", or "low", "mid" or "high" only.
bool set_pack_pop_rule(const std::string& pack, const std::string& rule, std::string& message);
// A trick more than one pack that is on has (its slot): "*" play them at random, the name of the pack whose plays it, or
// "" not decided (they play at random, and the studio asks).
bool set_conflict_choice(const std::string& slot, const std::string& pack, std::string& message);
// Where packs are exported to and imported from ("" the defaults); open_folder shows it in Explorer.
void set_pack_folder(bool exporting, const std::string& path);
void open_pack_folder(bool exporting);
// Test in game: the studio closes and this saved trick plays in game, on any pop and over any pack, until the
// studio is opened again; then View::test_returned names it, until clear_test_returned.
bool test_in_game(const std::string& animation, std::string& message);
void clear_test_returned();
// The name a saved animation is shown by in MY TRICKS and the packs; empty takes it off. Its file keeps its
// name, so the packs it is in keep it.
bool set_label(const std::string& animation, const std::string& label, std::string& message);
// A saved trick's pop: "" any (it plays on every pop the game does that has no trick of its own for that pop),
// or only "low", "mid" or "high". The packs it is in file it under its new pop.
// MY TRICKS: a trick in its group (with the others of its trick) or on its own.
void set_grouped(const std::string& animation, bool grouped);
bool set_saved_pop(const std::string& animation, const std::string& pop, std::string& message);
// A low, a mid and a high copy of a saved trick, put in every pack it is in.
bool copy_saved_to_pops(const std::string& animation, std::string& message);
// A stance version of a saved trick, put in every pack it is in: 0 your own stance, 1 fakie, 2 switch, 3 switch fakie.
// Switch is the other foot leading (the trick and its keys mirrored), fakie rolling backwards (the same trick; in game
// the one made fakie plays when he rolls fakie). It keeps its foot when it is edited and saved. `made` is its name.
bool make_stance_version(const std::string& animation, int version, std::string& made, std::string& message);
bool delete_pack(const std::string& name, std::string& message); // the pack, not the tricks in it
// What its maker says of a pack (up to 2000 characters), shown wherever it is shared.
bool set_pack_description(const std::string& pack, const std::string& description, std::string& message);
// A pack as it is shared (EXPORT, the MARKET): its files - its pack file and picture, and its tricks with this PC's key -
// with what it is called, what is in it (a line a trick) and its picture's PNG ("" none). False, with why, when it cannot
// be: a trick in it made by someone else, or nothing saved in it.
struct SharedPack {
    std::string name, made_by, description, version, published;
    std::string published_as; // the Thunderstore package it went up as (Team-Name), "" none
    std::vector<std::pair<std::string, std::string>> files; // path in the zip, bytes
    std::vector<std::string> tricks;
    std::string picture;
};
bool share_pack(const std::string& pack, SharedPack& out, std::string& message);
// It went up on Thunderstore as this version of this package (Team-Name): its next update goes to the same package.
void pack_published(const std::string& pack, const std::string& version, const std::string& published_as);
// One zip of packs put in the library (as IMPORT does, for the zip named): the MARKET's installs. `made`, when given,
// gets the names the packs in it have in the library (a name already taken gets a number). `replace`: the pack an
// earlier version of this package became (an UPDATE or a REINSTALL): it is replaced in place, under its name, staying
// on or off as it was, and its tricks made elsewhere that no other pack has go first, so none is left as a copy.
bool import_pack_zip(const std::filesystem::path& zip, std::string& message, std::vector<std::string>* made = nullptr,
                     const std::string& replace = {});
std::filesystem::path studio_folder();       // ReSkate.Tricks
std::filesystem::path pack_export_folder();  // where exports go
// A trick in a pack switched off (it stays in the pack, but does not play) or on again.
bool set_pack_trick_on(const std::string& pack, const std::string& animation, bool on, std::string& message);
// PLAY (MY TRICKS): on, the trick plays in game on its own, whichever packs are on; off, it does not play at all - from
// its packs neither (it is switched off in each). A trick saved or put in a pack starts switched off.
bool set_trick_play(const std::string& animation, bool on, std::string& message);
void set_pack_favorite(const std::string& pack, bool favorite); // starred packs come first in PACKS
// Whether others may open the pack's tricks to edit and copy them once it is shared (its maker's choice; locked unless
// set). It goes with the pack when it is exported.
bool set_pack_editable(const std::string& pack, bool editable, std::string& message);
// The open playlist (View::playlist_open): on (it decides what plays, and any other one goes off) or off (the packs that are
// on play), how its tricks take turns, and what is in it.
void set_playlist_on(bool on, std::string& message);
void set_playlist_mode(PlaylistMode mode, std::string& message);
bool playlist_add(const PlaylistEntry& entry, std::string& message); // makes Playlist 1 when there is none
void playlist_remove(std::size_t index);
void playlist_move(std::size_t index, int by); // up (-1) or down (1)
void open_playlist(const std::string& name);   // the one PACKS > PLAYLIST shows
bool new_playlist(std::string& message);        // "Playlist N", empty and off, and opened
bool rename_playlist(const std::string& to, std::string& message); // the open one
bool delete_playlist(std::string& message);     // the open one
// Puts a whole pack or one trick in a playlist ("" a new one) and opens it. The playlist goes on when none is (one that is
// on stays on). Returns the playlist's name ("" when it failed).
std::string add_to_playlist(const std::string& playlist, const PlaylistEntry& entry, std::string& message);
// ACTIVE on a saved trick. On: put back where it was switched off (its pack, in the playlist that is on or a pack that is
// on), or - when it is in nothing that plays - choose_playlist: the overlay asks which playlist it goes in. In a playlist
// already but beaten to its pop by one above it: show_playlist. Off: out of the playlist that is on, or off in its pack.
enum class ActiveAnswer { done, show_playlist, choose_playlist };
ActiveAnswer set_trick_active(const std::string& animation, bool on, std::string& message);
void set_hover_preview(bool on);
// The editor's rebound keys (View::keys), kept in studio-settings.json; empty puts every key back.
void set_key_bindings(const std::map<std::string, std::string>& keys);
std::map<std::string, std::string> key_bindings(); // the same, cheaply (the loading screen's tips name them)
// A pack (saved first if it has changes), or every saved animation for "", as
// ReSkate.Tricks/exports/<name>.zip, laid out like the tricks folder so it can be unzipped straight into it.
bool export_pack(const std::string& pack_name, std::string& message);
// Takes in every .zip in ReSkate.Tricks/import: checked first, names already taken are given the next
// variation, older animations upgraded, and the zip moved to import/imported.
bool import_packs(std::string& message);
bool load(const std::string& name, std::string& message);
// A saved trick open to watch only, a pack's or anyone's (one locked to its maker too): it plays, the camera moves, but
// nothing is keyed or saved. end_preview closes it.
bool preview_saved(const std::string& name, std::string& message);
void end_preview();
// The game's own tricks as studio clips, <folder>/library/catalog.json and its .clip files: built in game from the
// player's own copy (library_build below), or by contrib/animator-studio/make_trick_library.py.
struct NativeAsset {
    std::string category, asset, kind;
    std::string root_type, decode_state, detail, partition;
    std::string toc, bundle; // where in the game it was found (a library built in game)
    std::string label, context, search;
};
struct NativeField { std::string label, path, value, reference; bool expandable{}; };
struct NativeDocumentPage { std::string title, error; std::vector<NativeField> fields; std::size_t pages{}; };
NativeDocumentPage native_asset_fields(const std::string& asset, const std::string& path, std::size_t page);
std::shared_ptr<const std::vector<NativeAsset>> native_assets();
std::shared_ptr<const std::vector<GameTrick>> game_tricks(); // cached until explicit refresh
void refresh_game_tricks(std::string& message);
// The trick library, built on this PC from the player's own copy of the game (native_library.h) into
// ReSkate.Tricks/library: whether there is one, and a build running in the background and how far it has got.
// A build goes into a folder of its own and only takes the old library's place once it is whole.
struct LibraryBuild {
    bool exists{};   // there is a library
    bool outdated{}; // it was built from another version of the game, or by an older studio (native_library::builder_version)
    bool running{}, failed{};
    std::string built;   // when the library there was built ("" for one built before the studio could)
    std::string step;    // what the build is doing
    std::string message; // how the last build went
    std::size_t done{}, total{};
    float fraction{}; // the whole build, 0 to 1
};
LibraryBuild library_build();
void start_library_build(bool in_background = false); // nothing when one is running; in_background: below the game
void stop_library_build();  // the build stops soon after; the library stays as it was
// Asked every frame: with no trick library yet (a fresh install) or one built from another version of the game, the build
// starts by itself, once a session: `now` when the studio is open, else a little after the game has started. A build
// that failed waits for TRY AGAIN.
void build_library_when_needed(bool now);
bool export_diagnostics(std::string& message);
// REPORT A BUG: the last 15 seconds, the game's log, the studio's settings and the build in one zip in
// ReSkate.Tricks\bug reports, shown in Explorer. Nothing is sent.
bool report_bug(std::string& message);
bool load_game_trick(const GameTrick& trick, std::string& message);
// A trick done by the skater himself for a look (TRICKS' hover preview), over whatever is open, which stays as it is:
// it plays once from the start and holds its end, until end_peek. It is read, decoded and turned into the studio's
// stance on a thread of its own (a library clip is hundreds of KB: read on the frame, it stuttered the game), and the
// last few are kept, so one looked at again starts at once. `play`: it starts as soon as it is ready; without, it is
// only made ready (the mouse is settling on its card). False when it cannot be asked for.
bool peek_game_trick(const GameTrick& trick, bool play = true);
bool peek_saved(const std::string& animation, bool play = true);
bool peek_playing();   // the trick asked to play is playing
void end_peek();       // and one asked for that is not ready yet does not start
float peek_duration(); // the trick being looked at, in seconds (0 none)
// Deletes a saved trick and takes it out of every pack it is in. A pack got from someone else (the MARKET, an import)
// left with no trick goes too: `packs_gone` says which (for the MARKET to forget).
bool remove(const std::string& name, std::string& message, std::vector<std::string>* packs_gone = nullptr);
void rename(const std::string& name);
void set_your_name(const std::string& name); // MADE BY for new tricks and packs, without touching the one open
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
// The camera's field of view the studio wants (FILMER's fisheye), in degrees; 0 leaves the game's own. Any thread.
void set_wanted_fov(float degrees) noexcept;
float wanted_fov() noexcept;

// ---- the game side -------------------------------------------------------------------------------
// Engine thread, every client tick with the local skater's animation component (0 when there is
// none). Installs the animation listener the first time it is needed. Returns the open state the
// studio wants.
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
