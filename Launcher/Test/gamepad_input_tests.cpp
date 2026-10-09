// Controller input in the launcher: focus, back, the trackpad pointer and its click,
// checked through real ImGui frames.
#include "Launcher/gamepad_input.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <string>

using dingosdk::launcher_gui::PadFeed;
using dingosdk::launcher_gui::PadState;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

constexpr std::uint16_t dpad_up = 0x0001, dpad_left = 0x0004;
constexpr std::uint16_t dpad_down = 0x0002, dpad_right = 0x0008, right_thumb = 0x0080, button_a = 0x1000, button_b = 0x2000;

struct Seen {
    std::string pressed;   // the button that reported a press this frame
    bool escape{};
    bool menu_open{};
    float list_scroll{};
};

// One launcher frame: the pad goes in before NewFrame, as gui.cpp does it.
// Two buttons stacked at the top left, and a menu popup opened on request.
Seen frame(PadFeed& feed, const PadState& pad, bool open_menu = false, bool panels = false, bool list = false) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    feed.update(io, pad, 1.0f);
    ImGui::NewFrame();
    feed.after_new_frame();
    Seen seen;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(800, 600));
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::SetCursorScreenPos(ImVec2(100, 100));
    if (ImGui::InvisibleButton("first", ImVec2(200, 50), ImGuiButtonFlags_EnableNav)) seen.pressed = "first";
    ImGui::SetCursorScreenPos(ImVec2(100, 200));
    if (ImGui::InvisibleButton("second", ImVec2(200, 50), ImGuiButtonFlags_EnableNav)) seen.pressed = "second";
    if (panels) {
        // A page's rail and content, side by side, as Settings and Mods lay them out.
        ImGui::SetCursorScreenPos(ImVec2(400, 200));
        ImGui::BeginChild("##rail", ImVec2(150, 200), ImGuiChildFlags_NavFlattened);
        if (ImGui::Button("tab", ImVec2(120, 40))) seen.pressed = "tab";
        ImGui::EndChild();
        ImGui::SetCursorScreenPos(ImVec2(580, 200));
        ImGui::BeginChild("##content", ImVec2(200, 200), ImGuiChildFlags_NavFlattened);
        if (ImGui::Button("option", ImVec2(120, 40))) seen.pressed = "option";
        ImGui::EndChild();
    }
    if (list) {
        // A long list in a panel, like GET MODS.
        ImGui::SetCursorScreenPos(ImVec2(400, 100));
        ImGui::BeginChild("##list", ImVec2(300, 200), ImGuiChildFlags_NavFlattened);
        for (int row = 0; row < 30; ++row) {
            ImGui::PushID(row);
            if (ImGui::Selectable("##row", false, 0, ImVec2(280, 40))) seen.pressed = "row " + std::to_string(row);
            ImGui::PopID();
        }
        seen.list_scroll = ImGui::GetScrollY();
        ImGui::EndChild();
    }
    if (open_menu) ImGui::OpenPopup("##menu");
    if (ImGui::BeginPopup("##menu")) {
        seen.menu_open = true;
        ImGui::MenuItem("Details");
        ImGui::EndPopup();
    }
    seen.escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::End();
    ImGui::Render();
    return seen;
}

PadState pad(std::uint16_t buttons = 0, std::int16_t right_x = 0, std::int16_t right_y = 0, std::int16_t left_y = 0) {
    PadState state;
    state.left_y = left_y;
    state.connected = true;
    state.buttons = buttons;
    state.right_x = right_x;
    state.right_y = right_y;
    return state;
}

// Presses and lets go, returning what the frames saw.
std::string press(PadFeed& feed, std::uint16_t button) {
    std::string pressed = frame(feed, pad(button)).pressed;
    const auto released = frame(feed, pad()).pressed;
    return pressed.empty() ? released : pressed;
}

// The install progress, drawn over the Mod manager while this is set.
bool g_installing = false;

// The Mod manager's layout: a rail of tabs beside a list whose rows carry
// buttons, the rail's tiles level with the gaps between rows.
std::string mods_page(PadFeed& feed, const PadState& state) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(900, 700);
    io.DeltaTime = 1.0f / 60.0f;
    feed.update(io, state, 1.0f);
    ImGui::NewFrame();
    feed.after_new_frame();
    std::string pressed;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 700));
    ImGui::Begin("##page", nullptr, ImGuiWindowFlags_NoDecoration);
    dingosdk::launcher_gui::hold_focus(g_installing);
    // The window's own buttons come first on the page, as in the launcher.
    ImGui::SetCursorPos(ImVec2(866, 0));
    if (ImGui::Button("_", ImVec2(30, 30))) pressed = "minimise";
    ImGui::SetCursorPos(ImVec2(20, 100));
    ImGui::BeginChild("##rail", ImVec2(200, 520), ImGuiChildFlags_NavFlattened);
    if (ImGui::Button("MY MODS", ImVec2(150, 40))) pressed = "MY MODS";
    dingosdk::launcher_gui::default_focus();
    if (ImGui::Button("GET MODS", ImVec2(150, 40))) pressed = "GET MODS";
    // BACK at the rail's foot, under the list's last row: the list leaves room
    // for a line of text below it.
    ImGui::SetCursorPos(ImVec2(0, 480));
    if (ImGui::Button("BACK", ImVec2(150, 40))) pressed = "BACK";
    ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(260, 100));
    ImGui::BeginChild("##content", ImVec2(600, 460), ImGuiChildFlags_NavFlattened);
    for (int row = 0; row < 12; ++row) {
        ImGui::PushID(row);
        dingosdk::launcher_gui::begin_row();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(560, 60)))
            pressed = "row " + std::to_string(row);
        const ImVec2 next = ImGui::GetCursorScreenPos();
        dingosdk::launcher_gui::row_buttons();
        ImGui::SetCursorScreenPos(ImVec2(start.x + 380, start.y + 14));
        if (ImGui::Button("INSTALL", ImVec2(100, 32))) pressed = "install " + std::to_string(row);
        dingosdk::launcher_gui::end_row();
        ImGui::SetCursorScreenPos(next);
        ImGui::PopID();
    }
    dingosdk::launcher_gui::keep_focus_in_list();
    ImGui::EndChild();
    ImGui::End();
    if (g_installing) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowFocus();
        ImGui::Begin("##installing", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPos(ImVec2(400, 400));
        if (ImGui::Button("CANCEL")) pressed = "cancel";
        ImGui::End();
    }
    ImGui::Render();
    return pressed;
}

// Taps a D-pad direction on the Mod manager, then A: what A pressed is where the focus went.
std::string mods_tap(PadFeed& feed, std::uint16_t button) {
    mods_page(feed, pad(button));
    for (int i = 0; i < 3; ++i) mods_page(feed, pad());
    std::string pressed = mods_page(feed, pad(button_a));
    pressed += mods_page(feed, pad());
    return pressed;
}

// MY MODS as the launcher draws it at the Steam Deck's scale: the rail with
// BACK at its foot, a tick above the list for every mod, and rows that carry
// a tick on the left and a switch and a menu on the right. Returns the name of
// the item holding the focus.
std::string my_mods(PadFeed& feed, const PadState& state) {
    constexpr float k = 0.85f;
    static std::map<ImGuiID, std::string> names;
    const auto name = [](const std::string& text) { names[ImGui::GetItemID()] = text; };
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1227, 716);
    io.DeltaTime = 1.0f / 60.0f;
    feed.update(io, state, k);
    ImGui::NewFrame();
    feed.after_new_frame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##page", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::SetCursorPos(ImVec2(28 * k, 156 * k));
    ImGui::BeginChild("##rail", ImVec2(236 * k, 716 - 180 * k), ImGuiChildFlags_NavFlattened);
    ImGui::Button("MY MODS", ImVec2(-1, 56 * k));
    name("MY MODS");
    dingosdk::launcher_gui::default_focus();
    ImGui::Button("GET MODS", ImVec2(-1, 56 * k));
    name("GET MODS");
    for (const char* label : {"Install .zip", "Install folder", "Open Mods folder", "Open Thunderstore"}) {
        ImGui::Button(label, ImVec2(-1, 32 * k));
        name(label);
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 38 * k);
    ImGui::Button("BACK", ImVec2(-1, 34 * k));
    name("BACK");
    ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(290 * k, 52 * k));
    ImGui::BeginChild("##content", ImVec2(1227 - 318 * k, 716 - 76 * k), ImGuiChildFlags_NavFlattened);
    static bool all = false;
    ImGui::Checkbox("##all", &all);
    name("tick all");
    ImGui::Spacing();
    const float body = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##mod_list", ImVec2(0, body), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    const float tall = 68 * k;
    for (int row = 0; row < 20; ++row) {
        ImGui::PushID(row);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        dingosdk::launcher_gui::begin_row();
        ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, tall));
        name("row " + std::to_string(row));
        const ImVec2 next = ImGui::GetCursorScreenPos();
        dingosdk::launcher_gui::row_buttons();
        ImGui::SetCursorScreenPos(ImVec2(start.x + 14 * k, start.y + (tall - ImGui::GetFrameHeight()) * 0.5f));
        static bool ticks[20];
        ImGui::Checkbox("##pick", &ticks[row]);
        name("tick " + std::to_string(row));
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - 96 * k, start.y + 23 * k));
        ImGui::Button("##switch", ImVec2(42 * k, 22 * k));
        name("switch " + std::to_string(row));
        dingosdk::launcher_gui::end_row();
        ImGui::SetCursorScreenPos(next);
        ImGui::PopID();
    }
    dingosdk::launcher_gui::keep_focus_in_list();
    ImGui::EndChild();
    ImGui::TextDisabled("Mods load top to bottom");
    ImGui::EndChild();
    ImGui::End();
    ImGui::Render();
    const auto found = names.find(GImGui->NavId);
    return found == names.end() ? std::string() : found->second;
}

// One D-pad press, held for a few frames as a thumb does, then let go.
std::string my_mods_tap(PadFeed& feed, std::uint16_t button) {
    for (int i = 0; i < 6; ++i) my_mods(feed, pad(button));
    std::string at;
    for (int i = 0; i < 6; ++i) at = my_mods(feed, pad());
    return at;
}

void fresh() {
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
}

} // namespace

int main() {
    // ------------------------------------------------ focus
    {
        fresh();
        PadFeed feed;
        frame(feed, pad());
        frame(feed, pad());
        press(feed, dpad_down);
        check(ImGui::GetIO().NavVisible, "The D-pad shows the focus");
        // The first move lands on the first item; the next one goes down.
        press(feed, dpad_down);
        check(press(feed, button_a) == "second", "A presses the item the D-pad moved to");
        check(!feed.pointing() && !ImGui::GetIO().MouseDrawCursor, "No pointer is drawn while the focus is used");
    }
    {
        fresh();
        PadFeed feed;
        frame(feed, pad());
        frame(feed, pad(button_a));
        check(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown), "A (Cross) is the gamepad's activate button");
        PadState gone;   // unplugged with A held
        frame(feed, gone);
        frame(feed, gone);
        check(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown), "An unplugged pad lets go of A");
        check(!(ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad), "An unplugged pad is no gamepad");
    }

    // ------------------------------------------------ the Mod manager
    {
        fresh();
        PadFeed feed;
        mods_page(feed, pad());
        mods_page(feed, pad());
        check(GImGui->NavCursorVisible && mods_tap(feed, 0) == "MY MODS",
              "With a pad connected the page opens with the focus on its tab");
        check(mods_tap(feed, dpad_down) == "GET MODS", "Down goes to the next tab");
        const auto row = mods_tap(feed, dpad_right);
        check(row.rfind("row ", 0) == 0, "Right goes from a tab level with a gap between rows into the list");
        check(mods_tap(feed, dpad_left) == "GET MODS", "Left goes from the list back to the tabs");
        mods_tap(feed, dpad_right);
        const int first = std::stoi(mods_tap(feed, dpad_down).substr(4));
        check(mods_tap(feed, dpad_down) == "row " + std::to_string(first + 1), "Down goes to the next row in one press");
        check(mods_tap(feed, dpad_up) == "row " + std::to_string(first), "Up goes to the row above in one press");
        check(mods_tap(feed, dpad_right) == "install " + std::to_string(first), "Right goes to the row's own button");
        check(mods_tap(feed, dpad_up) == "row " + std::to_string(first - 1), "Up from a row's button goes to the row above");
        for (int i = 0; i < 14; ++i) mods_tap(feed, dpad_down);
        check(mods_tap(feed, dpad_down) == "row 11", "Down at the end of the list stays on its last row");
        check(mods_tap(feed, dpad_left) == "BACK", "Left from the end of the list reaches BACK, far below the tabs");
        mods_tap(feed, dpad_right);
        for (int i = 0; i < 14; ++i) mods_tap(feed, dpad_up);
        check(mods_tap(feed, dpad_up) == "row 0", "Up at the top of the list stays on its first row");
        const auto tab = mods_tap(feed, dpad_left);
        check(tab == "MY MODS" || tab == "GET MODS", "Left still leaves the list");
        mods_tap(feed, dpad_down);
        check(mods_tap(feed, dpad_down) == "BACK", "Down the tabs reaches BACK");

        // INSTALL on a row, then the install progress over the page until it is done.
        mods_tap(feed, dpad_up);
        mods_tap(feed, dpad_right);
        check(mods_tap(feed, dpad_right).rfind("install ", 0) == 0, "On a row's INSTALL");
        const int installed = std::stoi(mods_tap(feed, 0).substr(8));
        g_installing = true;
        for (int i = 0; i < 10; ++i) mods_page(feed, pad());
        g_installing = false;
        for (int i = 0; i < 3; ++i) mods_page(feed, pad());
        check(GImGui->NavCursorVisible, "The focus shows again once the install is done");
        check(mods_tap(feed, 0) == "row " + std::to_string(installed), "The focus goes back to the installed mod's row");
    }
    {
        fresh();
        ImGui::GetStyle().ScaleAllSizes(0.85f);
        PadFeed feed;
        for (int i = 0; i < 3; ++i) my_mods(feed, pad());
        std::string at = my_mods_tap(feed, dpad_right);
        for (int i = 0; i < 20 && at != "row 7"; ++i) at = my_mods_tap(feed, dpad_down);
        check(at == "row 7", "Right and down walk into the rows");
        check(my_mods_tap(feed, dpad_left) == "tick 7", "Left goes to the row's tick");
        at = my_mods_tap(feed, dpad_left);
        check(at != "tick all" && at.rfind("row", 0) != 0 && !at.empty(), "Left from a tick halfway down reaches the rail, not the tick above the list");
        my_mods_tap(feed, dpad_right);
        for (int i = 0; i < 3; ++i) my_mods_tap(feed, dpad_down);
        check(my_mods_tap(feed, dpad_left).rfind("tick ", 0) == 0, "Back on a row's tick");
        const std::string from = my_mods_tap(feed, dpad_up);
        check(from.rfind("row ", 0) == 0, "Up from a tick goes to the row above");
        const int row = std::stoi(from.substr(4));
        check(my_mods_tap(feed, dpad_up) == "row " + std::to_string(row - 1), "The next up goes to the next row, not to that row's tick");
        check(my_mods_tap(feed, dpad_right) == "switch " + std::to_string(row - 1), "Right goes to the row's switch");
        check(my_mods_tap(feed, dpad_down) == "row " + std::to_string(row), "Down from the switch goes to the row below");
        for (int i = 0; i < 25; ++i) at = my_mods_tap(feed, dpad_down);
        check(at == "row 19", "Down stops on the last row");
        my_mods_tap(feed, dpad_left);
        check(my_mods_tap(feed, dpad_left) == "BACK", "Left from the last row's tick reaches BACK");
    }
    {
        fresh();
        PadFeed feed;
        mods_page(feed, PadState{});
        mods_page(feed, PadState{});
        check(!GImGui->NavCursorVisible, "Without a pad nothing shows a focus");
    }

    // ------------------------------------------------ back
    {
        fresh();
        PadFeed feed;
        frame(feed, pad());
        frame(feed, pad());
        frame(feed, pad(button_b));
        check(ImGui::IsKeyPressed(ImGuiKey_Escape, false), "B is Escape with nothing open");
        frame(feed, pad());
        check(!ImGui::IsKeyDown(ImGuiKey_Escape), "Letting go of B lets go of Escape");
    }
    {
        fresh();
        PadFeed feed;
        frame(feed, pad());
        check(frame(feed, pad(), true).menu_open, "The menu opens");
        frame(feed, pad());
        const auto with_b = frame(feed, pad(button_b));
        const auto after = frame(feed, pad());
        check(!after.menu_open, "B closes an open menu");
        check(!with_b.escape && !after.escape, "Closing the menu is not also Escape, which would leave the page");
    }
    {
        fresh();
        PadFeed feed;
        frame(feed, pad());
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);   // the keyboard's Escape, held
        frame(feed, pad());
        frame(feed, pad());
        check(ImGui::IsKeyDown(ImGuiKey_Escape), "An idle pad does not let go of the keyboard's Escape");
    }

    {
        fresh();
        PadFeed feed;
        for (int i = 0; i < 2; ++i) frame(feed, pad(), false, true);
        const auto tap = [&](std::uint16_t button) { frame(feed, pad(button), false, true); frame(feed, pad(), false, true); };
        tap(dpad_down);   // onto "first"
        tap(dpad_down);   // "second", level with the panels
        tap(dpad_right);  // the rail
        tap(dpad_right);  // the content
        std::string pressed = frame(feed, pad(button_a), false, true).pressed;
        const auto released = frame(feed, pad(), false, true).pressed;
        check((pressed.empty() ? released : pressed) == "option",
              "The D-pad crosses from a page's rail into its content");
    }

    // ------------------------------------------------ scrolling
    {
        fresh();
        PadFeed feed;
        const auto tap = [&](std::uint16_t button) { frame(feed, pad(button), false, false, true); frame(feed, pad(), false, false, true); };
        for (int i = 0; i < 2; ++i) frame(feed, pad(), false, false, true);
        tap(dpad_right);   // from nothing onto the list's first row
        tap(dpad_right);
        float scroll{};
        for (int i = 0; i < 30; ++i) scroll = frame(feed, pad(0, 0, 0, -32767), false, false, true).list_scroll;
        scroll = frame(feed, pad(), false, false, true).list_scroll;
        check(scroll > 100, "The left stick scrolls the panel holding the focus");
        check(!feed.pointing() && GImGui->NavCursorVisible && GImGui->NavHighlightItemUnderNav,
              "Scrolling stays with the focus, not the mouse");
        for (int i = 0; i < 60; ++i) frame(feed, pad(0, 0, 0, 32767), false, false, true);
        check(frame(feed, pad(), false, false, true).list_scroll == 0, "Pushing up scrolls back to the top");
    }
    {
        fresh();
        PadFeed feed;
        ImGui::GetIO().AddMousePosEvent(500, 200);   // over the list
        for (int i = 0; i < 2; ++i) frame(feed, pad(), false, false, true);
        frame(feed, pad(0, 0, 0, 0), false, false, true);
        frame(feed, pad(right_thumb), false, false, true);   // the pointer shows, without a click
        frame(feed, pad(), false, false, true);
        float scroll{};
        for (int i = 0; i < 30; ++i) scroll = frame(feed, pad(0, 0, 0, -32767), false, false, true).list_scroll;
        scroll = frame(feed, pad(), false, false, true).list_scroll;
        check(scroll > 100, "The left stick scrolls the panel under the pointer");
        check(feed.pointing(), "Scrolling keeps the pointer");
    }
    {
        fresh();
        PadFeed feed;
        ImGui::GetIO().AddMousePosEvent(500, 200);   // the mouse resting over the list
        for (int i = 0; i < 2; ++i) frame(feed, pad(), false, false, true);
        float scroll{};
        for (int i = 0; i < 30; ++i) scroll = frame(feed, pad(0, 0, 0, -32767), false, false, true).list_scroll;
        scroll = frame(feed, pad(), false, false, true).list_scroll;
        check(scroll > 100, "The left stick scrolls the panel under the mouse");
    }

    // ------------------------------------------------ the Steam Deck's trackpad
    {
        fresh();
        PadFeed stick;
        PadFeed trackpad(true);
        ImGui::GetIO().AddMousePosEvent(400, 300);
        for (int i = 0; i < 20; ++i) frame(trackpad, pad());   // it settles first
        for (int i = 0; i < 10; ++i) frame(trackpad, pad(0, 3300, 0));   // a slow swipe
        frame(trackpad, pad());
        check(ImGui::GetIO().MousePos.x > 405, "A slow swipe on the trackpad moves the pointer");
        fresh();
        ImGui::GetIO().AddMousePosEvent(400, 300);
        for (int i = 0; i < 20; ++i) frame(stick, pad());
        for (int i = 0; i < 10; ++i) frame(stick, pad(0, 3300, 0));
        frame(stick, pad());
        check(!stick.pointing() && ImGui::GetIO().MousePos.x == 400, "The same small tilt of a thumbstick does not");

        fresh();
        PadFeed slowing(true);
        ImGui::GetIO().AddMousePosEvent(100, 300);
        for (int i = 0; i < 20; ++i) frame(slowing, pad());
        for (int i = 0; i < 5; ++i) frame(slowing, pad(0, 20000, 0));   // the swipe
        const float swiped = ImGui::GetIO().MousePos.x;
        float tilt = 20000;
        for (int i = 0; i < 12; ++i) {   // the thumb slows down before it lifts
            tilt *= 0.9f;
            frame(slowing, pad(0, static_cast<std::int16_t>(tilt), 0));
        }
        const float slowed = ImGui::GetIO().MousePos.x;
        frame(slowing, pad(0, -900, -1200));   // lifted: back to Steam's resting tilt
        frame(slowing, pad(0, -900, -1200));
        check(slowed > swiped + 60, "A swipe that slows down moves the pointer to its end");
        check(ImGui::GetIO().MousePos.x == slowed, "The pointer stops when the thumb lifts");

        fresh();
        PadFeed drifting(true);
        ImGui::GetIO().AddMousePosEvent(400, 300);
        for (int i = 0; i < 60; ++i) frame(drifting, pad(0, -2600, -2600));   // resting off the middle
        check(!drifting.pointing() && ImGui::GetIO().MousePos.x == 400 && ImGui::GetIO().MousePos.y == 300,
              "A trackpad resting off the middle does not move the pointer");
        for (int i = 0; i < 10; ++i) frame(drifting, pad(0, 9000, -2600));
        check(drifting.pointing() && ImGui::GetIO().MousePos.x > 420 && std::abs(ImGui::GetIO().MousePos.y - 300) < 1,
              "A swipe moves it from where the trackpad rests");
        for (int i = 0; i < 60; ++i) frame(drifting, pad(0, -1000, 1500));   // lifted: resting somewhere new
        const ImVec2 rested = ImGui::GetIO().MousePos;
        for (int i = 0; i < 60; ++i) frame(drifting, pad(0, -1000, 1500));
        check(ImGui::GetIO().MousePos.x == rested.x && ImGui::GetIO().MousePos.y == rested.y,
              "After a touch it rests again without the pointer creeping");
    }

    // ------------------------------------------------ trackpad pointer
    {
        fresh();
        PadFeed feed;
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(400, 300);
        feed.mouse_moved(ImVec2(400, 300));
        for (int i = 0; i < 20; ++i) frame(feed, pad());
        frame(feed, pad(0, 3000, 0));
        check(!feed.pointing() && io.MousePos.x == 400, "A thumb resting near the middle does not move the pointer");

        frame(feed, pad(0, 16000, 0));
        frame(feed, pad());
        check(feed.pointing() && io.MouseDrawCursor, "Tilting takes the pointer and draws it");
        check(io.MousePos.x > 400 && io.MousePos.y == 300, "Tilting right moves the pointer right, from the mouse's place");
        for (int i = 0; i < 120; ++i) frame(feed, pad(0, 32767, 32767));
        frame(feed, pad());
        check(io.MousePos.x == 799 && io.MousePos.y == 0, "The pointer stays inside the window");

        // The system cursor stays where the mouse left it, and is reported every frame.
        io.AddMousePosEvent(400, 300);
        frame(feed, pad());
        frame(feed, pad());
        check(io.MousePos.x == 799, "The system cursor's old place does not take the pointer back");
        feed.mouse_moved(ImVec2(400, 300));   // the same place, repeated by Windows
        check(feed.pointing(), "A mouse move repeated in the same place keeps the pointer");

        feed.mouse_moved(ImVec2(50, 60));     // the mouse, or a finger on the screen
        io.AddMousePosEvent(50, 60);
        frame(feed, pad());
        frame(feed, pad());
        check(!feed.pointing() && !io.MouseDrawCursor, "The mouse takes the pointer back");
        check(io.MousePos.x == 50, "The mouse's place wins once it moved");
    }
    {
        fresh();
        PadFeed feed;
        ImGui::GetIO().AddMousePosEvent(150, 225);   // over "second"
        frame(feed, pad());
        frame(feed, pad());
        std::string pressed = frame(feed, pad(right_thumb)).pressed;
        pressed += frame(feed, pad()).pressed;
        check(feed.pointing() && pressed.empty() && !ImGui::GetIO().MouseDown[0],
              "The first trackpad click (R3) only shows the pointer");
        pressed = frame(feed, pad(right_thumb)).pressed;
        check(ImGui::GetIO().MouseDown[0], "The next click holds the left button");
        pressed += frame(feed, pad()).pressed;
        check(pressed == "second", "The trackpad's click presses what the pointer is over");
        check(!ImGui::GetIO().MouseDown[0], "Letting go of R3 lets go of the left button");

        press(feed, dpad_down);
        check(!feed.pointing() && ImGui::GetIO().NavVisible, "The D-pad hands the screen back to the focus");
    }

    ImGui::DestroyContext();
    if (failures) {
        std::cerr << failures << " gamepad input check(s) failed\n";
        return 1;
    }
    std::cout << "Gamepad input checks passed.\n";
    return 0;
}
