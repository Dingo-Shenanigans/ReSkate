#pragma once

#include <imgui.h>

#include <cstdint>

// Controller input for the launcher window. Kept free of Windows headers so
// the mapping can be tested anywhere; gui_gamepad.cpp reads the pads.
namespace dingosdk::launcher_gui {

// One frame of a controller in XInput's XINPUT_GAMEPAD layout: an Xbox pad,
// Steam Input's virtual pad (Steam Deck), or a DualShock 4 / DualSense read
// over HID. Several pads are merged into one.
struct PadState {
    bool connected{};
    std::uint16_t buttons{};   // XINPUT_GAMEPAD_* bits
    std::uint8_t left_trigger{}, right_trigger{};
    std::int16_t left_x{}, left_y{}, right_x{}, right_y{};
};

// Turns a pad into the launcher's ImGui input:
// - the D-pad moves the focus, A (Cross) presses the focused item;
// - the left stick scrolls, under the focus or under the pointer;
// - B (Circle) closes an open combo or menu and leaves a text field, and is
//   Escape otherwise, which the launcher's pages and modals already go back on;
// - the right stick moves a pointer and R3 clicks with it. Under Steam Input's
//   gamepad layouts the Steam Deck's right trackpad is the right stick and its
//   click is R3, so this is what makes the trackpad a mouse in the launcher.
class PadFeed {
public:
    // `trackpad`: the right stick is a Steam Deck's trackpad, not a thumbstick.
    explicit PadFeed(bool trackpad = false) : trackpad_(trackpad) {}
    // Call once a frame after the backends' NewFrame and before
    // ImGui::NewFrame(), which takes the events in.
    void update(ImGuiIO& io, const PadState& pad, float scale);
    // Call right after ImGui::NewFrame(): looks again for a D-pad left or right
    // that ImGui found nothing for.
    void after_new_frame();
    // A mouse move or a touch the window received (WM_MOUSEMOVE, client area
    // pixels). A move to a new place takes the pointer back from the pad.
    void mouse_moved(ImVec2 position);
    // The right stick owns the pointer. ImGui draws it and the system cursor
    // stays put: under Proton a moved system cursor comes back late as a
    // mouse move, which would look like the mouse taking over.
    bool pointing() const { return pointing_; }

private:
    bool trackpad_{};
    std::uint16_t buttons_{};
    ImGuiDir side_dir_{ImGuiDir_None};   // a D-pad left or right ImGui looked for last frame
    ImGuiID side_from_{};
    int side_tries_{};   // how many times it looked again
    ImGuiID last_nav_id_{}, last_nav_window_{};
    ImGuiKey back_key_{ImGuiKey_Escape};
    bool pointing_{};
    bool clicking_{};   // R3 holds the left button
    ImVec2 rest_{};     // where the right stick rests
    bool rest_known_{};
    float steady_{};    // seconds the right stick held still
    float last_x_{}, last_y_{};
    ImVec2 pointer_{};
    ImVec2 mouse_{-1, -1};   // where the last mouse move was
};

// Makes the item just submitted the one a controller starts on in its window.
// With a pad connected, the focus moves there at once when nothing has it yet
// or when the pad opened the page.
void default_focus();

// A list row a controller treats as one item: up and down go from row to row
// in one press, and the buttons on a row take the focus only from the row
// itself (D-pad right). begin_row() before the row's list_row, row_buttons()
// after it, end_row() after the row's last button.
void begin_row();
void row_buttons();
void end_row();

// Call once a frame, after the page's Begin, with whether a window over the
// page holds the focus, like the install progress. When it goes, the focus
// goes back to where it was, or to its list row: the button pressed may be
// gone by then, an INSTALL turned INSTALLED.
void hold_focus(bool covered);

// Call just before a list's EndChild(): up and down from a row stay in the
// list, so the end of the list holds the focus instead of passing it to
// whatever lies beyond, like the rail's BACK.
void keep_focus_in_list();

} // namespace dingosdk::launcher_gui
