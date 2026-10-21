#include "skate_menu_internal.h"
#include "skate_style.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

// ADDONS > FIT CHECK: how the turnaround of the board or skater (V on the Board or Skaters screen) is made. Every change is a
// `turnaround set` console command, so the settings are kept the same way however they are set.
namespace dingosdk::overlay::menu {
namespace {
// The backgrounds offered besides transparent: a solid colour behind the board or skater.
struct Matte {
    const char* name;
    std::array<std::uint8_t, 3> colour;
};
constexpr std::array<Matte, 4> mattes{{{"Black", {0, 0, 0}}, {"Grey", {24, 25, 28}}, {"White", {255, 255, 255}}, {"Green screen", {0, 177, 64}}}};

// A choice the player just made, shown until the setting comes back from the turnaround: the page
// is drawn from the menu's model, which catches up a moment later, and stepping a full screen
// "< GIF >" twice before then would step from the old value twice.
struct Pending { int value = -1; double at = 0; };
Pending& pending(const char* id) {
    static std::array<std::pair<std::string, Pending>, 12> all{};
    for (auto& [name, p] : all)
        if (name == id) return p;
    for (auto& [name, p] : all)
        if (name.empty()) { name = id; return p; }
    return all.back().second;
}
int shown(const char* id, int current) {
    auto& p = pending(id);
    if (p.value < 0) return current;
    if (p.value == current || ImGui::GetTime() - p.at > 2.0) { p.value = -1; return current; }
    return p.value;
}
void picked(const char* id, int value) { pending(id) = {value, ImGui::GetTime()}; }

std::string hex(const std::array<std::uint8_t, 3>& c) {
    char text[8];
    std::snprintf(text, sizeof text, "#%02x%02x%02x", c[0], c[1], c[2]);
    return text;
}

} // namespace

void turnaround_addon(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& t = model.turnaround;
    const bool can = t.available && callbacks.queue_console_command && !t.busy;
    const auto set = [&](const std::string& key, const std::string& value) {
        send_console(menu, callbacks, "turnaround set " + key + " " + value);
    };

    begin_card(menu, "turnaround", "FIT CHECK", "Spin your board or skater and save it, to show off your mods or your setup");
    note("Press V on the Board screen, or on the Skaters screen with no item grid open. It spins once by itself (hands off) "
         "and is saved to the Fit Checks folder beside Skate.exe. V again stops it.");
    if (!t.status.empty()) info(menu, t.busy ? "Now" : "Last", t.status);
    const int pressed = button_row(menu, "turnaround-actions",
                                   {{"OPEN FIT CHECKS FOLDER", callbacks.queue_console_command != nullptr, false, "See the saved fit checks."},
                                    {"CANCEL", t.busy, false, "Stop the fit check being made."}});
    if (pressed == 0) send_console(menu, callbacks, "turnaround open");
    else if (pressed == 1) send_console(menu, callbacks, "turnaround cancel");
    end_card();

    begin_card(menu, "turnaround-options", "OPTIONS");
    field(menu, "File");
    int format = shown("format", t.format);
    if (choice(menu, "turnaround-format", format, {"GIF", "APNG", "WebM"}, can, 0.0f,
               {"Plays anywhere. Edges are a little rough.", "Best quality, smooth edges. Plays in Discord and browsers.",
                "Smallest file, smooth edges. Plays in Chrome, Firefox and Discord."}))
        picked("format", format), set("format", format == 1 ? "apng" : format == 2 ? "webm" : "gif");

    // Transparent, or one of the solid colours (a colour set with `fitcheck set matte` shows as none of them).
    field(menu, "Background");
    int matte = -1;
    for (std::size_t i = 0; i < mattes.size(); ++i)
        if (mattes[i].colour == t.matte) matte = static_cast<int>(i);
    int background = shown("background", t.transparent ? 0 : std::max(matte, 0) + 1);
    if (choice(menu, "turnaround-background", background, {"Transparent", "Black", "Grey", "White", "Green screen"}, can)) {
        picked("background", background);
        if (background == 0) set("background", "clear");
        else {
            set("matte", hex(mattes[static_cast<std::size_t>(std::clamp(background - 1, 0, static_cast<int>(mattes.size()) - 1))].colour));
            set("background", "matte");
        }
    }

    bool parts = shown("parts", t.parts ? 1 : 0) == 1;
    if (toggle_row(menu, "Info card", "The deck, grip, trucks and wheels (or the outfit) by name beside it.", parts, can))
        picked("parts", parts ? 1 : 0), set("parts", parts ? "on" : "off");

    field(menu, "Length", "How long one turn takes to play.");
    int seconds = shown("seconds", t.seconds <= 2 ? 0 : t.seconds <= 4 ? 1 : 2);
    if (choice(menu, "turnaround-seconds", seconds, {"2 s", "4 s", "6 s"}, can))
        picked("seconds", seconds), set("seconds", std::array{"2", "4", "6"}[static_cast<std::size_t>(seconds)]);

    field(menu, "Smoothness");
    int fps = shown("fps", t.fps <= 30 ? 0 : 1);
    if (choice(menu, "turnaround-fps", fps, {"Normal (30 fps)", "Smooth (60 fps)"}, can, 0.0f,
               {"Smaller files, quicker to make.", "Silky, and a slower spin to make it (a GIF plays at 50)."}))
        picked("fps", fps), set("fps", fps == 1 ? "60" : "30");

    field(menu, "Size");
    int size = shown("size", t.size <= 384 ? 0 : t.size <= 512 ? 1 : 2);
    if (choice(menu, "turnaround-size", size, {"Small", "Medium", "Large"}, can, 0.0f,
               {"384 pixels high.", "512 pixels high.", "768 pixels high: bigger files."}))
        picked("size", size), set("size", std::array{"384", "512", "768"}[static_cast<std::size_t>(size)]);
    end_card();
}
} // namespace dingosdk::overlay::menu
