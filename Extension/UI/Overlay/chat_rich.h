#pragma once
#include <imgui.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Chat text with its :emote: images (chat_emotes.h), laid out in wrapped lines. Shared by the
// chat panel (chat_overlay.cpp) and the chat bubbles over each skater (nametag_overlay.cpp).
namespace dingosdk::overlay::chat_rich {
// Items refer to the message by offset, so a layout stays valid for as long as the text it
// was made from does not change.
struct RichItem {
    std::size_t begin{}, length{}; // within the message: the text, or the emote's name
    bool emote{};
    ImVec2 at, size;
    std::size_t line{};
};
struct RichText {
    std::vector<RichItem> items;
    float width{};       // the widest line, without trailing spaces
    float height{};
    std::size_t lines{}; // lines laid out (after any cut)
    bool truncated{};    // cut at `max_lines`: "..." ends the last line
    ImVec2 ellipsis{};   // where that "..." goes
};
// Whether `text` holds at least one known :emote:.
bool has_emotes(std::string_view text);
// Whether `text` is nothing but known :emotes: (and spaces); `count` says how many.
bool only_emotes(std::string_view text, std::size_t &count);
// `text` in lines no wider than `wrap`, the first starting `indent` in. Words longer than a
// line are broken where they must be. Emotes are `emote_scale` times the text height, centred
// on their line. With `max_lines`, anything past that many lines is cut.
RichText lay_out(ImFont *font, float size, std::string_view text, float wrap, float indent,
                 std::size_t max_lines = 0, float emote_scale = 1.3f);
// Draws a layout at `origin`, everything multiplied by `scale` (for animation).
void draw_rich(ImDrawList *draw, ImFont *font, float size, ImVec2 origin, std::string_view text, const RichText &rich,
               ImU32 colour, float alpha, float scale = 1.0f);
// The bad-word filter keeps a message's length, so the original's known :emote: spans can be
// put back into the masked text: a filtered word must not break an emote's name. Returns
// `masked` unchanged when the two do not line up.
std::string restore_emotes(std::string_view masked, std::string_view raw);
} // namespace dingosdk::overlay::chat_rich
