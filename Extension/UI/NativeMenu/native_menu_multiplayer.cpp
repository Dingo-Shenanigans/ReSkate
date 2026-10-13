#include "native_menu_internal.h"

// The pause menu's Multiplayer and Mod Options pages. The overlay draws everything in them
// (their tabs and what each tab holds: Extension/UI/Overlay/hub_page.cpp), because the game's
// menu widgets are a button, a text box and a line of text and cannot make a list with
// columns. What is left here is the page the game needs for each hub tab: one body, empty.
namespace dingosdk::multiplayer {
using namespace menu_data;
using namespace native_menu_detail;
namespace native_menu_detail {
void process_clipboard() noexcept {}
void process_actions(const Context&, const MultiplayerModel&) {
    // Nothing on the page has an action; anything queued is from a page since replaced.
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.pending.clear();
}
void render_section(const Context& context, const MultiplayerModel&, Section section) {
    auto& s = state();
    const auto index = static_cast<unsigned>(section);
    // One empty line in each column: a body with no rows at all is not one the game lays out,
    // and a line of text is nothing the game can put its focus on.
    std::vector<std::string> main, side;
    s.row_width = main_width;
    add_text(context, main, "blank", " ");
    s.row_width = side_width;
    add_text(context, side, "blank-side", " ");
    publish_rows(context, s.lists[index], index * 2, main);
    publish_rows(context, s.side_lists[index], index * 2 + 1, side);
}
} // namespace native_menu_detail
} // namespace dingosdk::multiplayer
