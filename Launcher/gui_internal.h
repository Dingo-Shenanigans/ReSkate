#pragma once

#include "gamepad_input.h"
#include "game_settings.h"
#include "launch.h"
#include "text_encoding.h"
#include "mod_profiles.h"
#include "start_maps.h"
#include "thunderstore.h"

#include "Engine/Vfs/mod_conflicts.h"
#include "Engine/Vfs/mod_list.h"
#include "Extension/UI/skate_theme.h"

#include <Windows.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Shared by the launcher window's files (gui*.cpp).
namespace dingosdk::launcher_gui::detail {

namespace fs = std::filesystem;
namespace update = launcher_update;

// ---------------------------------------------------------------- look

// The window's size at scale 1. Everything is laid out in these units and
// multiplied by g_scale, which gui.cpp lowers when the design size would not
// fit the screen's work area.
constexpr float design_width = 1440.0f;
constexpr float design_height = 840.0f;

inline ImU32 rgba(int r, int g, int b, float a = 1.0f) {
    return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}
namespace color {
inline const ImU32 background_top = rgba(9, 11, 15);
inline const ImU32 background_bottom = rgba(17, 20, 27);
inline const ImU32 text = rgba(236, 239, 244);
inline const ImU32 muted = rgba(128, 137, 151);
inline const ImU32 danger = rgba(255, 92, 92);
inline const ImU32 panel = rgba(26, 26, 26, 0.97f);
// Sampled from skate.'s own menus (HUB screen).
inline const ImU32 tile = skate_theme::tile;
inline const ImU32 tile_grey = skate_theme::tile_light;
inline const ImU32 blue = skate_theme::blue;
inline const ImU32 good = skate_theme::good;
inline const ImU32 warning = skate_theme::warning;
inline const ImU32 avatar = skate_theme::avatar;
inline const ImU32 ink = skate_theme::black;
inline const ImU32 outline = rgba(255, 255, 255, 0.12f);
}

// Blue button with black text, the colours of skate.'s selected tile.
using skate_theme::push_primary_button;
using skate_theme::pop_primary_button;

struct Fonts {
    ImFont* body{};     // Montserrat SemiBold
    ImFont* caption{};
    ImFont* bold{};     // Montserrat ExtraBold
    ImFont* heading{};
    ImFont* tile{};     // tile headers, like the HUB's "BOUNTIES"
    ImFont* action{};   // the PLAY tile
    ImFont* title{};    // brushed page title, like the HUB's "HUB"
};

inline float g_scale = 1.0f;
inline float S(float value) { return value * g_scale; }

// Optional photo behind the launcher; zero id means the drawn background.
struct Background {
    ImTextureID id{};
    float width{}, height{};
};
inline Background g_background;
// White tile icons (assets/launcher/icon_*.png), drawn faded like the HUB's.
inline Background g_icon_mods, g_icon_settings, g_icon_thunderstore;

using launcher_text::utf8;
using launcher_text::wide;

class Renderer;
// The window's renderer, for textures made after startup (package icons).
inline Renderer* g_renderer{};

void panel_title(const Fonts& fonts, const char* text);
// Brushed title, tilted like the HUB's; `position` is in screen space, and
// `size` zero means the title font's own. Newlines start a new line.
void page_title(ImDrawList* draw, const Fonts& fonts, ImVec2 position, const char* text, float size = 0);
// A centred modal panel; returns its size.
ImVec2 begin_panel(const char* id, ImVec2 size, ImVec2 panel);
// A page filling the whole window, for screens that outgrew a modal panel
// (Mods). Returns its size, so it lays out like a panel.
ImVec2 begin_page(const char* id, ImVec2 size);
// Minimise and close in the top-right corner. A page draws its own: it covers
// the main screen's.
void window_buttons(ImDrawList* draw, HWND window, ImVec2 size);
// An invisible button at an absolute screen position, for a tile drawn by hand.
bool tile_hit(const char* id, ImVec2 position, ImVec2 size, bool enabled, bool& hovered);
// A tile in a page's nav rail, blue while its page is open, like skate.'s
// selected tile. Leaves the cursor where the next tile goes.
bool nav_tile(const Fonts& fonts, float width, const char* label, bool selected, const std::string& count = {},
              bool accent = false, const std::string& tip = {});
// A pill with a count or a short word, like the badges on a mod manager's nav.
float badge_width(const Fonts& fonts, const std::string& text);
void badge(ImDrawList* draw, const Fonts& fonts, ImVec2 position, const std::string& text,
           ImU32 fill = color::blue, ImU32 ink = color::ink);
// How tall `text` is in `font`, wrapped at `wrap` (0: not wrapped); 0 for no text.
float text_height(ImFont* font, float wrap, const std::string& text);
// A page's banner, as wide as the page: its heading with a blue pill beside it (`pill` may be
// empty), a line on what the page is for, and at the right how things stand: a pill in
// `status_fill`, with `caption` under it (either may be empty). Leaves the cursor under it.
void page_banner(const Fonts& fonts, const char* heading, const char* pill, const std::string& about,
                 const std::string& status = {}, ImU32 status_fill = color::good, const std::string& caption = {});
// A card behind what a page draws on it: lighter edged while hovered, with a blue edge when `marked`.
void card(ImDrawList* draw, ImVec2 start, ImVec2 size, bool marked = false);
// A caption over its value: the detail lines of an open list row.
void field(const Fonts& fonts, const char* name, const std::string& value);
// One row of a mod list, `height` tall: a card, lighter edged while hovered, with a blue edge
// when it is ticked. Leave a gap under it (row_gap). Returns true when the row itself was clicked,
// which opens the mod's overview; the widgets drawn over it keep their clicks.
bool list_row(const char* id, float width, float height, bool ticked);
inline float row_gap() { return S(8); }
// An on/off switch, blue while on. Returns true when it was flipped.
bool toggle(const char* id, bool* on);
// A square button with three dots, for a row's menu.
bool more_button(const char* id, float size);

// Draws only the rows a scrolling child actually shows. Unlike ImGuiListClipper
// this copes with rows of different heights, which one open row needs.
template<class Height, class Row>
void virtual_rows(int count, Height height, Row row) {
    // Both axes: a row leaves the cursor wherever its last widget was.
    const ImVec2 start = ImGui::GetCursorPos();
    const float scroll = ImGui::GetScrollY(), view = ImGui::GetWindowHeight();
    float y = start.y;
    for (int index = 0; index < count; ++index) {
        const float tall = height(index);
        // One row past each edge too: a controller moving off the last row
        // shown needs the next one there to move to.
        if (y + 2 * tall >= scroll && y <= scroll + view + tall) {
            ImGui::SetCursorPos(ImVec2(start.x, y));
            row(index, tall);
        }
        y += tall;
    }
    // An item at the end, so the child scrolls over every row and no further.
    ImGui::SetCursorPos(ImVec2(start.x, y));
    ImGui::Dummy(ImVec2(1, 0));
}
void open_path(const fs::path& path);
// Opens an https:// page in the default browser; anything else is ignored.
void open_url(std::string_view url);

// ---------------------------------------------------------------- window

inline bool g_drag_allowed = true;
// Set while Settings waits for a key to bind; the window procedure stores the
// next key press here instead of letting ImGui see it.
inline std::atomic<bool> g_capturing_key{};
inline std::atomic<unsigned> g_captured_key{};
// Paths dropped on the window, picked up by the next frame.
inline std::mutex g_dropped_mutex;
inline std::vector<fs::path> g_dropped;
// Every connected controller merged into one: XInput pads (Xbox, and Steam
// Input's virtual pad on a Steam Deck), else a DualShock 4 / DualSense over HID.
PadState read_pad();
// The window's controller input, told about mouse moves by the window procedure.
inline PadFeed* g_pad_feed{};

// ---------------------------------------------------------------- settings

struct Settings {
    bool windowed{};
    int width{1920};
    int height{1080};
    bool loose_files{true};
    bool gpu_diagnostics{};
    bool discord_status{true};
    bool offline{};
    // The map the game starts in, by its level asset (start_maps.h); empty: San Vansterdam.
    std::string start_map;
    int menu_key{static_cast<int>(launcher::default_menu_key)};
    int console_key{static_cast<int>(launcher::default_console_key)};
    int log_level{2};
    std::string arguments;
    bool keep_open_after_launch{};
    // Off: never replace ReSkate.dll or the launcher (keeps a test build someone handed out).
    bool updates{true};
    // Off: a crash uploads nothing (RESKATE_CRASH_REPORTING=0 for the launcher and the game).
    bool crash_reports{true};
    // On: launches send how long their steps took (RESKATE_TRACING=1 for the launcher and the game).
    bool performance_timings{};
    // On: update to the newest pre-release on GitHub when there is one newer than the latest
    // release. Turned off, the next check goes back to the latest release.
    bool prereleases{};
    std::string steam_username;   // never the password
    bool steam_remember{true};
    bool steam_prefer_code{};
    // The versions of the privacy policy and the terms of service the player agreed to (0: none).
    std::uint64_t accepted_privacy{}, accepted_terms{};
    // The version of the home screen's announcement the player hid (0: none): one newer shows.
    std::uint64_t announcement_hidden{};
    // GET MODS: the Thunderstore categories shown only, and those hidden (thunderstore.h CategoryFilter).
    std::vector<std::string> mod_categories_only, mod_categories_hidden;
    // MY MODS: the same for the installed mods, by their Thunderstore categories ("No category" for a
    // mod Thunderstore does not list, or lists in none).
    std::vector<std::string> installed_categories_only, installed_categories_hidden;
    // What the mods gave the game's data at the last launch (launcher_mods::game_data_signature): a
    // launch with other mods empties the game's shader cache first.
    std::string shader_cache_mods;
};

inline constexpr std::array<const char*, 7> log_levels{"trace", "debug", "info", "warning", "error", "critical", "off"};

// ---------------------------------------------------------------- state

enum class Phase { checking, update_available, updating, game_missing, game_outdated, downloading,
                   merging, mods_broken, ready, launching, failed };

// A mod the pre-launch merge could not use, named so nobody has to guess which
// of their mods stopped working.
struct ModProblem {
    std::string name;      // folder under Mods/, so it can be switched off
    std::string title;
    std::string reason;
};

struct State {
    Phase phase{Phase::checking};
    std::string status{"Checking for updates"};
    std::string detail;
    float progress{-1};
    std::vector<std::string> qr;
    std::optional<update::Prompt> prompt;
    std::optional<update::Config> config;
    // Set when a merge before launch left mods out; PLAY waits on an answer.
    std::vector<ModProblem> mod_problems;
};

// Update checks, the Steam download and the game launch, one at a time on a worker thread.
class Launcher {
public:
    Launcher(const launcher_app::Session& session, std::vector<std::wstring> arguments);
    ~Launcher();

    State snapshot();
    bool busy() const { return busy_; }
    bool restart_requested() const { return restart_; }
    // Process id of the Skate this launcher started, once it exists.
    DWORD game() const { return game_; }
    // Injection finished and the game is running on its own.
    bool launched() const { return launched_; }
    // Resets the launch state after the retained game process exits.
    void game_exited(bool seen);
    Settings& settings() { return settings_; }
    const launcher_app::Session& session() const { return session_; }
    void save();
    // The update check again when an update choice changed since the last one: as either switch
    // is flipped, and each frame after (a check already running holds the next one until it ends).
    void check_again_if_update_choices_changed();
    // Whether the last update check found a pre-release newer than the latest release.
    bool prerelease_available() const { return prerelease_available_; }

    void check();
    // The merge the game would do at startup, run here so its failures can be
    // shown and answered. `ignore_mod_problems` plays with them left out.
    void play_anyway();
    // Back to READY without launching, so the mod manager can be opened.
    void dismiss_mod_problems();
    // Where the game reads Mods from: the game folder, or -dataPath.
    fs::path mods_data_root() const;
    void apply_updates();
    // `qr` signs in with a QR code; otherwise the saved Steam username is used.
    // The password only lives in memory until DepotDownloader asks for it; it
    // may be empty when DepotDownloader remembers the login.
    void download(bool validate, bool qr, std::string password);
    void play();
    void cancel();
    // Answers the pending Steam prompt; nothing cancels the download.
    void answer(std::optional<std::string> value);

    void restart();

private:
    launcher_app::Session session_;
    std::vector<std::wstring> arguments_;
    Settings settings_;
    bool relaunched_{};
    // The update choices the last check was made with: one changed in settings is checked
    // again straight away (save()).
    std::atomic<bool> checked_updates_{true}, checked_prereleases_{};
    std::atomic<bool> prerelease_available_{};
    bool binaries_{true};
    std::mutex mutex_;
    std::condition_variable answered_;
    State state_;
    std::string password_;
    bool qr_login_{true};
    std::optional<std::string> answer_;
    bool has_answer_{};
    std::thread worker_;
    std::atomic<bool> busy_{};
    std::atomic<bool> cancel_{};
    std::atomic<bool> restart_{};
    std::atomic<bool> ignore_mod_problems_{};
    std::atomic<DWORD> game_{};
    std::atomic<bool> launched_{};

    template<class Task> void start(Task task);
    static void wipe(std::string& value);
    std::optional<std::string> prompt(const update::Prompt& prompt);
    void set(Phase phase, std::string status, std::string detail = {}, float progress = -1);
    void set_progress(std::string detail, float progress);
    void fail(const std::string& message);
    std::optional<update::Config> config();
    update::Progress progress_for(std::string label);
    bool launcher_outdated(const update::Config& config) const;
    bool runtime_outdated(const update::Config& config) const;
    void run_check();
    void run_updates();
    void run_download(bool validate);
    // False when mods were left out and the launch should wait for an answer.
    bool run_mod_merge();
    void run_play();
};

// The CHANGELOGS panel's release, the newest: fetched on a worker the first time the panel
// opens and kept while the launcher runs. `note` and `text` belong to the UI thread.
struct Changelog {
    std::thread worker;
    std::atomic<bool> loading{};
    std::mutex mutex;
    bool arrived{};                                  // worker -> UI, under mutex
    update::ReleaseNote incoming;
    std::string incoming_error;

    bool loaded{};
    std::string error;                               // why the last fetch failed
    update::ReleaseNote note;
    thunderstore::Readme text;
    // Whether pre-releases are wanted (the setting), and whether `note` was fetched with them.
    bool prereleases{}, fetched_prereleases{};

    ~Changelog() {
        if (worker.joinable()) worker.join();
    }
};

// The privacy policy and terms of service, from the ReSkate backend (GET /api/v1/legal/<kind>):
// each fetched on a worker the first time it is shown and kept while the launcher runs.
// Everything but the worker's hand-over belongs to the UI thread.
struct Legal {
    enum Kind { privacy, terms, count };
    struct Document {
        std::string title, updated, markdown;   // updated: YYYY-MM-DD
        std::uint64_t version{};
    };
    struct Slot {
        std::thread worker;
        std::atomic<bool> loading{};
        bool arrived{};                         // worker -> UI, under the mutex
        Document incoming;
        std::string incoming_error;

        bool loaded{};
        std::string error;                      // why the last fetch failed
        Document document;
        thunderstore::Readme text;
    };
    std::mutex mutex;
    std::array<Slot, count> slots;
    int open{-1};                               // the Kind shown, or -1 when the window is closed

    // The versions the backend has now (GET /api/v1/legal), looked up once a launch. One newer
    // than the player agreed to (Settings::accepted_*) puts up the agreement before anything else.
    std::thread versions_worker;
    bool versions_started{}, versions_arrived{};   // arrived: worker -> UI, under the mutex
    std::array<std::uint64_t, count> current{};
    bool agreement{};                           // the agreement window is up

    ~Legal() {
        for (auto& slot : slots)
            if (slot.worker.joinable()) slot.worker.join();
        if (versions_worker.joinable()) versions_worker.join();
    }
};

// Settings > GRAPHICS, AUDIO, CAMERA, CONTROLS, REPLAY: skate.'s own settings, read from its
// save each time Settings opens (game_settings.h).
struct GamePages {
    bool loaded{};
    launcher_game_settings::Saved saved;
    launcher_game_settings::Values values;          // as shown; stored once no control is held
    std::vector<std::pair<int, int>> resolutions;   // this PC's, for the Resolution setting
    std::string error;          // why they could not be read, or the last change not stored
};

// Panels the main screen can show; one at a time.
// The announcement the ReSkate backend has for the home screen (GET
// https://api.reskate.dev/api/v1/announcement), read as the launcher starts and every ten minutes,
// so it is written in the admin panel and reaches players without a release.
struct Announcement {
    struct Message {
        std::uint64_t version{};
        bool important{};
        std::string title, markdown, link, link_label;   // link: empty, or an https URL
    };
    std::thread worker;
    std::atomic<bool> loading{};
    std::mutex mutex;
    bool arrived{};                         // worker -> UI, under the mutex
    std::optional<Message> incoming;
    std::optional<Message> shown;           // none: there is nothing to show
    thunderstore::Readme text;
    double next_read{};                     // ImGui time of the next read
    ~Announcement() {
        if (worker.joinable()) worker.join();
    }
};

struct Ui {
    bool settings{};
    Announcement announcement;
    GamePages game;
    bool changelog{};
    Changelog notes;
    Legal legal;
    int settings_tab{};         // GAME, DISPLAY, KEYS, ADVANCED, then the game's own pages
    // The maps Settings offers to start in, read from the Mods folder when GAME is first drawn
    // after Settings opens.
    std::optional<std::vector<launcher_start_maps::StartMap>> start_maps;
    int binding{};              // 1 = menu key, 2 = console key, while waiting for a press
    std::string key_error;
    bool mods{};
    bool sign_in{};
    bool sign_in_validate{};
    bool focus{};
    std::array<char, 65> username{};
    std::array<char, 256> password{};
    std::array<char, 16> code{};
    // The MOD MANAGER tile's "2 of 3 enabled", re-read every few seconds.
    std::string mods_detail;
    // Its badge: updates waiting, or mods that did not load, which wins.
    std::string mods_mark;
    bool mods_mark_bad{};
    // Thunderstore updates waiting, so PLAY can say so and ask before it
    // launches without them. Answered once a session.
    std::size_t mods_pending{};
    bool mods_update_prompt{};
    bool mods_updates_ignored{};
    // "Update them and play" said play: launch once the install finishes.
    // Cleared by leaving the page or by an install that failed.
    bool play_after_install{};
    double mods_checked{-100};
    // Steam display name for the name plate, re-read every few seconds.
    std::string steam_name;
    double steam_checked{-100};
    // The account's Steam picture on the plate in place of the drawn logo; reloaded
    // when Steam caches a new one or another account signs in.
    ImTextureID steam_avatar{};
    fs::path steam_avatar_path;
    fs::file_time_type steam_avatar_time{};
    // One-time notice that Skate will start offline because Steam is not
    // running or not signed in. Decided on the first Steam check.
    bool steam_offline{};
    bool steam_offline_seen{};
};

// The Thunderstore listing, fetched in the background when the launcher
// starts and again on Refresh. `packages` belongs to the UI thread; the worker
// hands a finished fetch over through `incoming`.
struct Store {
    std::thread worker;
    std::atomic<bool> loading{};
    std::atomic<bool> stop{};
    std::mutex mutex;
    bool arrived{};                                  // worker -> UI, under mutex
    std::vector<thunderstore::Package> incoming;
    std::string incoming_error;

    bool loaded{};                                   // a listing arrived (it may be empty)
    std::string error;                               // why the last fetch failed
    std::vector<thunderstore::Package> packages;
    double fetched{-1e9};                            // ImGui time the last fetch started

    // The GET MODS page.
    std::array<char, 96> search{};
    int sort{};
    std::string selected;                            // full_name, while its overview is open
    bool overview{};                                 // the overview popup is showing
    std::vector<std::string> picked;                 // ticked, to install in one go

    ~Store() {
        stop = true;
        if (worker.joinable()) worker.join();
    }
};

// Package icons: fetched and decoded on a worker (cached on disk under
// %LOCALAPPDATA%\ReSkate\thunderstore\icons), uploaded on the UI thread.
struct Icons {
    struct Entry { ImTextureID id{}; bool queued{}; bool failed{}; int used{}; };
    std::map<std::string, Entry, std::less<>> entries;   // by icon URL; UI thread
    struct Decoded { std::string url; std::vector<unsigned char> pixels; unsigned width{}, height{}; };

    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    std::vector<Decoded> done;
    bool stop{};

    ~Icons() {
        { std::lock_guard lock(mutex); stop = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
};

// READMEs for the mod overviews: an installed mod's own README.md, or a package's from
// Thunderstore, fetched on a worker the first time its overview opens and kept while the
// launcher runs.
struct Readmes {
    struct Entry {
        bool loading{}, failed{};
        thunderstore::Readme readme;
    };
    std::map<std::string, Entry, std::less<>> entries;   // by "Owner-Name-1.2.3" or the mod's folder; UI thread
    struct Job { std::string key; std::wstring url; };
    struct Done { std::string key, markdown; bool failed{}; };

    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> queue;
    std::vector<Done> done;
    bool stop{};

    ~Readmes() {
        { std::lock_guard lock(mutex); stop = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
};

// The Mods panel's list plus one background install at a time.
struct ModsPanel {
    bool scanned{};
    fs::path root;
    mods::ModList list;
    // What each installed mod's folder holds on disk, by folder name; measured
    // when the folder is scanned.
    std::map<std::string, std::uint64_t, std::less<>> sizes;
    int selected{-1};                    // the mod whose overview is open
    bool overview{};                     // the overview popup is showing
    int tab{};                           // 0 MY MODS, 1 GET MODS
    // MY MODS: what the list is narrowed to and ordered by, and the mods ticked
    // to be changed together (folder names).
    std::array<char, 96> search{};
    int filter{};
    int order{};
    // Each Thunderstore package's categories by the folder it installs into, made again when a listing arrives.
    std::map<std::string, std::vector<std::string>, std::less<>> categories_by_folder;
    std::set<std::string, std::less<>> marked;
    std::string anchor;                  // the last mod ticked: where a Shift-click range starts
    int place{};                         // typed in a row's "move to position" popup
    int shown_tab{-1};                   // the tab drawn last, so MY MODS reads the folder again when it is opened
    // The conflicts the player settled (gui_mods.cpp): of each pair, (the mod that loads, the one left out).
    std::set<std::pair<std::string, std::string>> conflict_choices;
    bool resolving{};                    // the Resolve conflicts popup is to open
    std::vector<std::string> resolve_order;   // the load order as the popup opened: its rows keep it
    // MY MODS rows as drawn: each mod's height in the list, eased toward its place there, so a row
    // slides to where it goes rather than jumping (gui_mods.cpp).
    std::map<std::string, float, std::less<>> row_y;
    std::string drag_name;               // the row being dragged by its handle
    float drag_grab{};                   // how far below its top the pointer took hold of it
    // MY MODS: conflicts between the enabled mods as the list stands (mods::ConflictChecker),
    // checked off the UI thread a moment after each change. The worker hands its result over
    // in `pending` (under `mutex`); the UI thread takes it from there, so `rows` and what points
    // into them (`losses`) only change on the UI thread.
    struct ConflictCheck {
        std::unique_ptr<mods::ConflictChecker> checker;   // the worker's alone while it runs
        std::thread worker;
        std::atomic<bool> running{};
        double due{-1};                     // ImGui time the next check starts; -1 when none is asked
        std::uint64_t asked{}, checked{};   // which change the latest result is for
        bool ready{};                       // a result is in
        std::vector<mods::MergeReport::Conflict> rows;
        std::vector<std::string> order;     // the mods checked, highest priority first
        std::map<std::string, mods::ConflictLoss, std::less<>> losses;     // the mods that would be left out
        std::map<std::string, std::vector<std::string>, std::less<>> beats;   // and whom each winner leaves out
        struct Result {
            bool ready{};
            std::uint64_t asked{};
            std::vector<mods::MergeReport::Conflict> rows;
            std::vector<std::string> order;
        } pending;                          // under mutex
    } conflicts;
    std::string message;
    bool message_error{};
    std::vector<std::string> confirm_remove;   // folders awaiting "Uninstall" confirmation
    fs::path conflict_source;            // install waiting for "Replace" confirmation
    std::string conflict_name;

    std::thread worker;
    std::atomic<bool> installing{};
    std::atomic<bool> cancel{};
    std::atomic<float> progress{-1};
    std::mutex mutex;
    std::string activity;                // what the worker is doing, under mutex
    bool finished{};
    std::string finished_name, finished_error, finished_conflict, finished_note;
    fs::path finished_source;

    Store store;
    Icons icons;
    Readmes readmes;

    // PROFILES (gui_mods_profiles.cpp).
    launcher_profiles::Profiles profiles;
    bool profiles_loaded{};
    std::array<char, 64> profile_name{};    // typed for a new profile
    std::array<char, 96> profile_code{};    // typed to import
    std::array<char, 64> profile_renamed{}; // typed in the rename popup
    std::string profile_rename;             // the profile being renamed
    std::string profile_delete;             // the profile awaiting "Delete" confirmation
    // Loaded while its missing mods download: put in its order again once they are in.
    std::string profile_settle;
    // Talking to Thunderstore's profile service, off the UI thread.
    struct ProfileResult {
        bool ready{}, error{};
        std::string message, clipboard;
        std::optional<launcher_profiles::Profile> imported;
    };
    std::thread profile_worker;
    std::atomic<bool> profile_busy{};
    ProfileResult profile_result;           // worker -> UI, under mutex

    ~ModsPanel() {
        cancel = true;
        if (worker.joinable()) worker.join();
        if (profile_worker.joinable()) profile_worker.join();
        if (conflicts.worker.joinable()) conflicts.worker.join();
    }
};

void scan(ModsPanel& panel, const launcher_app::Session& session);
// Re-reads the Mods folder and the Thunderstore listing.
void refresh_mods(Launcher& launcher, ModsPanel& panel);
// Every frame, from the launcher's start: checks the enabled mods for conflicts in the background
// before the Mod Manager is first opened, so its list has the answer ready, and takes in checks as
// they finish. (Opening it reads the folder again; the check then reads only what changed.)
void prepare_mods(ModsPanel& panel, const launcher_app::Session& session);
// Waits (at most ten seconds) for a conflict check under way to finish: it may be reading the mod
// files an install or uninstall is about to replace. Off the UI thread where it can be.
void wait_for_conflict_check(const ModsPanel& panel);
void start_install(ModsPanel& panel, const fs::path& source, bool replace);
// The Windows file (or folder) picker; empty when cancelled. A file picker shows
// `filter_pattern` ("*.zip") under `filter_name`.
fs::path pick_path(HWND owner, bool folder, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter_pattern);

// ---------------------------------------------------------------- Profiles (gui_mods_profiles.cpp)

// The PROFILES page body, under the page header.
void profiles_page(const Fonts& fonts, ModsPanel& panel, HWND window, float height, bool installing);
// The rename and delete confirmations; call once a frame with the page's other popups.
void profile_popups(ModsPanel& panel);
// A profile loaded while its mods downloaded is put in order once they are in; call once
// a frame, after collect_install.
void settle_profile(ModsPanel& panel);

// ---------------------------------------------------------------- Thunderstore (gui_mods_browse.cpp)

// "1.27 GB", "263 MB", "12 KB": a mod's size, on both pages.
std::string size_text(std::uint64_t bytes);
// Starts a listing fetch when none ran yet, or when `force`.
void refresh_listing(ModsPanel& panel, double time, bool force = false);
// Takes over a fetch the worker finished; call once a frame.
void collect_listing(ModsPanel& panel);
// `enabled_only`: for what is asked before playing. A disabled mod does not load, so its
// update is offered on the Mods page and nowhere else.
thunderstore::Installed installed_versions(const mods::ModList& list, bool enabled_only = false);
// The package an installed mod folder came from, if the listing has it.
const thunderstore::Package* package_for(const Store& store, std::string_view folder);
std::vector<const thunderstore::Package*> updates(const Store& store, const thunderstore::Installed& installed);
// Downloads and installs (or updates) each package in turn on the panel's worker.
void start_store_install(ModsPanel& panel, std::vector<thunderstore::Package> packages);
// The package's icon texture, or empty while it loads; uploads finished icons.
ImTextureID package_icon(ModsPanel& panel, const thunderstore::Package& package);
// That icon drawn at `position`, or a placeholder square while it loads or
// when the mod never came from Thunderstore.
void mod_icon(ModsPanel& panel, const thunderstore::Package* package, ImVec2 position, float size);
void pump_icons(ModsPanel& panel);
// A category menu for a list of mods (GET MODS, MY MODS): each category shown only, hidden, or either,
// a click going round them, with how many mods are in each (category_count adds to that). One chosen
// before that no mod has now stays in the menu. True when the player changed what is chosen.
int& category_count(std::vector<std::pair<std::string, int>>& categories, const std::string& category);
bool category_menu(const char* id, std::vector<std::string>& only, std::vector<std::string>& hidden,
                   std::vector<std::pair<std::string, int>> categories, float width);
// The GET MODS page body, under the page header.
void browse_page(Launcher& launcher, const Fonts& fonts, ModsPanel& panel, float height, bool installing);
// Everything Thunderstore knows about one package, in a popup over the page.
void package_overview(const Fonts& fonts, ModsPanel& panel, ImVec2 size, bool installing);
// The README section of an overview: the installed mod's own README.md when it has one, else
// the package's from Thunderstore. Either may be null. False, with nothing drawn, when there is none.
bool readme_field(const Fonts& fonts, ModsPanel& panel, const thunderstore::Package* package, const mods::Mod* mod);
// A README's lines, wrapped to the window (also a release's notes: the same markdown).
void draw_readme(const Fonts& fonts, const thunderstore::Readme& readme);
// An overview's body: the README in a child of `readme` width (the caller's), then
// begin_overview_details ... end_overview_details around its facts, each an overview_fact.
struct OverviewColumns { float readme{}, details{}, height{}, gap{}; };
OverviewColumns overview_columns(float popup_height);
void begin_overview_details(const char* id, const OverviewColumns& columns);
void end_overview_details();
void overview_fact(const Fonts& fonts, const char* name, const std::string& value);
// Whether `package` is ticked for the next install, and ticking it.
bool picked(const Store& store, const std::string& full_name);
void pick(Store& store, const std::string& full_name, bool on);

void open_sign_in(Launcher& launcher, Ui& ui, bool validate);
void sign_in_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui);
void prompt_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, const update::Prompt& prompt, Ui& ui);
void qr_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, const std::vector<std::string>& rows);
void steam_offline_window(const Fonts& fonts, ImVec2 size, Ui& ui);
// What the newest release changed: its notes on GitHub.
void changelog_window(const Fonts& fonts, ImVec2 size, Ui& ui);
// The privacy policy or the terms of service (ui.legal.open), with a switch between them.
void legal_window(const Fonts& fonts, ImVec2 size, Ui& ui);
// Each frame: the backend's versions of both, looked up once; a newer one than the player
// agreed to raises ui.legal.agreement.
void check_legal_versions(Ui& ui, const Settings& settings);
// "I agree" to the current versions, or quit.
void legal_agreement_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, HWND window);
void settings_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, HWND window);
void mods_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel, HWND window);
// Shown instead of launching when the merge left mods out.
void mods_broken_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel,
                        const std::vector<ModProblem>& problems);
// Shown instead of launching while mods have Thunderstore updates waiting.
void mods_outdated_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel);

// The main screen: background, tiles, status and whichever panel is open.
void frame(Launcher& launcher, const Fonts& fonts, HWND window, Ui& ui, ModsPanel& mods_panel);

} // namespace dingosdk::launcher_gui::detail
