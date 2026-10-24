#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The throwdown kit: throwdown modes made by plugins, shown with skate.'s native widgets. A plugin
// registers its modes (custom_modes.h) and publishes documents (the setup page, the waiting card,
// the in-game HUD, the match panel, its flags and what a running match asks of the game); the
// native UI renders whichever documents are current and reports what the player does as events
// for the plugin. Nothing here knows any particular mode: a mode is an id and the data its plugin
// sends. Documents arrive on the plugin thread and are read on the UI thread: every accessor
// returns a copy.
namespace dingosdk {
class Json;
}
namespace dingosdk::throwdowns::kit {
using Vec3 = std::array<float, 3>;

// ---- documents --------------------------------------------------------------------------------
struct SetupRow {
    std::string id, kind; // "slider" | "choice" | "button" | "info"
    std::string caption, label;
    float min{}, max{}, step{1}, value{};
    std::vector<std::string> options; // choice
    int selected{};                   // choice: 0-based
};
struct SetupSlider {
    bool present{};
    std::string caption;
    float min{}, max{}, step{1}, value{};
};
struct Setup {
    std::string mode; // the registered mode id ("<plugin>/<id>", custom_modes Mode::key())
    std::string title, art, description, back;
    SetupSlider players, timer;
    std::vector<SetupRow> rows;
    std::vector<std::string> hide_native{"rounds"};
};
struct Waiting {
    std::string mode, title, art, status;
    unsigned enrolled{}, capacity{};
    bool can_start{};
    bool countdown{};
    std::uint32_t remaining_ms{}, total_ms{};
};
struct Mark {
    std::string text;
    bool on{};
};
// A clock the native HUD ticks itself between publications.
struct Clock {
    bool present{};
    std::uint32_t remaining_ms{}, total_ms{};
    std::uint64_t sampled_ms{}; // GetTickCount64 when remaining_ms was true
    bool ticking{}, up{};       // up: counts up from remaining_ms (an elapsed time) to total_ms
};
struct BoardRow {
    std::string rank, name, value;
    std::uint32_t color{0xffffffffU};
    bool self{}, out{};
    std::vector<Mark> marks;
    Clock clock; // present: the value is this clock as mm:ss.mmm, ticked by the HUD
};
struct TimerText {
    std::string caption, text;
    Clock clock; // present: the text is this clock as m:ss.cc
};
struct Prompt {
    std::vector<std::string> inputs; // "UI_DpadLeft", "UI_ShoulderL", ...
    std::string caption, glyph;
    float hold{}; // 0..1: a hold button's progress
};
struct Marker {
    Vec3 at{};
    std::string text;
    bool distance{};
};
struct Hud {
    bool active{};
    std::string theme;
    struct Header {
        std::string title, art, status;
        std::array<float, 2> offset{};
        unsigned icon_size{}, frame_width{}; // 0: the theme's measured header
    } header;
    std::string layout; // "side" | "bottom"
    std::vector<std::string> counters;
    float score_width{480};
    std::string score_art; // the counters' widget family ("" = HUD_<theme>_Score)
    Clock clock;
    struct Board {
        bool present{};
        std::string title, variant;   // variant: "stamps" (a name and a stamp per mark) | ""
        std::string style;            // "outlined" | "plain"
        std::string name_art, out_art; // widget families of the name cell ("" = HUD_<theme>_Name)
        // The rank and value cells' widget families ("" = HUD_<theme>_Rank / HUD_<theme>_Value), the
        // value cell's least width (0: 224 px) and a text its values may carry before a clock
        // ("DNF  "), reserved in its width.
        std::string rank_art, value_art, reserve;
        float value_width{};
        std::string heading;           // "list": a label row over the counters and rows, all one list; "shared"
        std::vector<std::string> columns;
        std::vector<BoardRow> rows;
    } board;
    struct Results {
        bool present{};
        std::string title, notice, caption;
        // The heading's widget family ("" = ResultsHeading) and its title's width (0: fitted). The
        // rows take the board's cell families (board.name_art, rank_art, value_art, ...).
        std::string heading_art;
        float title_width{};
        std::string style; // "outlined" (as the board's: clean-outline cells, the red glow on tinted values) | "plain"
        std::vector<BoardRow> rows;
    } results;
    struct Intro {
        bool present{};
        std::string title, art_key, rules;
        std::uint64_t until_ms{};
    } intro;
    Clock countdown; // 3-2-1-GO
    struct Text {
        std::string text, style;
        std::uint64_t serial{};
        std::uint32_t ms{};
        std::string subtitle, art; // the banner's (its text is the title); a warning's art is its widget family
    } banner, callout, warning;
    struct Card {
        bool present{};
        std::string caption, title, image;
    } card;
    struct List {
        bool present{};
        std::string caption;
        std::vector<std::string> items;
    } list;
    struct Position {
        bool present{};
        unsigned place{};
        std::string medal;
        std::string art; // the widget family, "<art>_<Medal>_Widget" ("" = HUD_<theme>_Position)
    } position;
    std::vector<TimerText> timers;
    std::string timer_art; // the timers' widget family ("" = HUD_<theme>_Elapsed)
    struct Compass {
        bool present{};
        Vec3 at{};
        std::string art;
    } compass;
    std::vector<Marker> markers;
    struct Blackout {
        bool present{};
        std::string text;
    } blackout;
    struct Editor {
        bool present{};
        std::string title, variant; // variant: "builder" | "compact"
        std::vector<Prompt> prompts;
        std::string info_title;
        std::vector<std::pair<std::string, std::string>> info;
        std::string toast;
        float toast_alpha{};
        std::string modal;
        std::vector<Prompt> modal_actions;
    } editor;
};
struct PanelSetting {
    std::string icon, caption, value;
};
struct PanelPlayer {
    std::uint64_t id{};
    std::string name, value, rank, outcome, portrait;
    bool self{};
};
struct Panel {
    bool present{};
    std::string title, description, art, score_caption, quit;
    std::vector<PanelSetting> settings;
    std::vector<Prompt> prompts; // each prompt's glyph field carries its command
    std::vector<PanelPlayer> players;
};
struct Flag {
    std::string id, mode;
    std::uint64_t leader{};
    std::uint32_t game{};
    Vec3 at{};
    float yaw{};
    unsigned players{1}, capacity{6};
    bool open{true};
    std::vector<std::pair<std::string, std::string>> stats;
};
struct Match {
    bool markers{}, input_lock{}, offboard{}, teleports{}, in_activity{};
    std::vector<std::uint64_t> hidden;
};
struct Documents {
    std::string plugin;
    std::optional<Setup> setup;
    std::optional<Waiting> waiting;
    Hud hud;
    Panel panel;
    std::vector<Flag> flags;
    Match match;
    std::uint64_t revision{};
};

// ---- the store --------------------------------------------------------------------------------
// A plugin's documents (JSON with the field names above; Steam IDs as strings). Empty when taken,
// else why not.
std::string publish(std::string_view plugin, std::string_view json_text);
// The same, already parsed (the plugin API has the table as JSON).
std::string publish(std::string_view plugin, const Json& document);
// The plugin stopped: its documents and its pending events go.
void release(std::string_view plugin);
// Every plugin's current documents (the UI reads the one whose mode is open or playing).
std::vector<Documents> documents();
// The documents of the plugin that registered `mode` ("<plugin>/<id>", custom_modes Mode::key()), if it published any.
std::optional<Documents> documents_for(std::string_view mode);
// The plugin whose match is running now (its `match` asks something, or its hud is active).
std::optional<Documents> active();
// What every running match asks, merged (any plugin's `true` holds).
Match match() noexcept;
// Whether `plugin`'s own match lets its teleports through where the session turned noclip off: its
// `match.teleports`, while that match runs (its HUD is up, or `flag_mine`: the native flag is one of
// its modes, which the caller knows from flag::state()).
bool allows_teleports(std::string_view plugin, bool flag_mine) noexcept;
bool hidden(std::uint64_t player) noexcept;

// ---- events -----------------------------------------------------------------------------------
struct Event {
    std::string plugin, action, mode;
    std::vector<std::string> args;
};
// From the native UI (any thread): queued for the plugin that registered `mode` (or `plugin`).
void post(std::string_view mode, std::string action, std::vector<std::string> args = {});
void post_to(std::string_view plugin, std::string action, std::string mode, std::vector<std::string> args = {});
// What a plugin should hear since the last call (the plugin host delivers them as `throwdown_ui`).
std::vector<Event> take(std::string_view plugin);

// The mode whose setup page or flag the player is working with now (set by the native UI on
// `open`, cleared when the page closes and no flag is down).
void set_current(std::string_view mode);
std::string current();

// ---- the native flag ---------------------------------------------------------------------------
// One native flag registration at a time, for one plugin mode: the stock Spot Battle setup page
// (relabelled from the mode's `setup` document), native flag placement, then the waiting card,
// until the match starts or the owner leaves. Events go to the mode's plugin: setup_confirm,
// flag_placed {game, x, y, z, yaw}, setup_back, start, leave, flag_left.
namespace flag {
struct Options {
    unsigned players = 4, seconds = 120;
    bool setup_page = true;  // false: straight to placement (a re-placed start flag)
    float cursor_height = 0; // > 0: the placement cursor may rise this far (metres)
};
// False when another registration is busy.
bool begin(std::string_view mode, std::uint32_t game, const Options& options);
struct State {
    bool owned{}, waiting{}, placing{}, busy{};
    std::string mode;
    std::uint32_t game{};
};
State state() noexcept;
// The owner is done with it (refused, finished, cancelled): the registration goes.
void leave() noexcept;
// A guest joined another player's flag through the stock Join (throwdown_relay): the registration
// is adopted so the native waiting card and Leave stay in charge.
bool adopt(std::string_view mode, std::uint32_t game, std::uint32_t mmid, const Vec3& at, float yaw, unsigned seconds, unsigned capacity);
} // namespace flag
} // namespace dingosdk::throwdowns::kit
