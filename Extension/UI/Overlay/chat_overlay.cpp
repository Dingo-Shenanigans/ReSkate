#include "overlay_internal.h"
#include "chat_emotes.h"
#include "chat_rich.h"
#include "nametag_gradient.h"
#include "role_badge.h"
#include <imgui_internal.h>
#include <optional>

// Multiplayer text chat in the bottom-right corner. Closed, the newest lines
// show for a few seconds and fade, taking no input. T opens it (overlay_input.cpp):
// the whole session log with a text box, Enter sends and closes, Esc closes.
// Lines come from the session through a small feed rather than the full model,
// so they can be polled on every presented frame.

namespace dingosdk::overlay {
namespace {
std::atomic<ChatFeed> chat_feed{};
}
void set_chat_feed(ChatFeed feed) noexcept { chat_feed.store(feed); }
} // namespace dingosdk::overlay

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto chat_hold = 10s, chat_fade = 1s;
constexpr std::size_t closed_lines = 6;

struct ChatState {
    MultiplayerChat feed;
    Clock::time_point next_poll{};
    std::uint64_t seen{};  // newest sequence already given an arrival time
    // When each line reached this overlay; the session keeps no clock for them.
    std::deque<std::pair<std::uint64_t, Clock::time_point>> arrivals;
    std::uint64_t feed_version{}; // counts polls of `feed`; the caches below are rebuilt after one
    std::array<char, multiplayer_chat_max_bytes + 1> input{};
    bool scroll_to_bottom{};
    std::vector<std::string> completions; // "/" commands the typed text can still become
    std::uint64_t completions_version{};  // the Suggestions::version `completions` was copied from
    // What the text in the box can become (suggestions()): worked out again only when that text,
    // the feed or the loaded emotes change, not several times every frame.
    struct Suggestions {
        bool valid{};
        std::string typed;
        std::uint64_t feed{};
        bool emotes_loaded{};
        std::uint64_t version{};                             // counts rebuilds
        std::vector<std::string> arguments;                  // argument_completions
        std::vector<const MultiplayerChatCommand*> commands; // matching_commands: into `feed`, until its next poll
        bool emote{};                                        // the last word is an unfinished :emote
        std::string before;                                  // everything ahead of it
        std::vector<std::string> emote_names;                // the emotes it can become
        std::vector<std::string> completions;                // what Tab cycles through
    } suggestions;
    bool refocus_input{};                  // an emote was clicked: back to the text box
    bool cursor_to_end{};                  // the box was filled for the player: type after it
    // Tab cycling: the text as typed before the first Tab, the options it offered, the one in the
    // box now and its text. Typing anything else ends the cycle.
    struct Cycle {
        std::string typed, written;
        std::vector<std::string> options;
        std::size_t index{};
        bool active{};
    } cycle;
};
ChatState& chat() {
    static ChatState value;
    return value;
}

Clock::time_point arrived(const ChatState& c, std::uint64_t sequence) {
    for (const auto& [line, at] : c.arrivals)
        if (line == sequence) return at;
    return {};
}

// The sender's role colour, as their nametag shows it; ReSkate's notices muted. Lines without
// a role (older sessions) fall back to a steady colour per sender.
ImU32 name_colour(const MultiplayerChatLine& line) {
    if (line.server) return line.color ? line.color : multiplayer::nametag_server;
    if (!line.sender) return theme::muted;
    if (line.color) return line.color;
    if (line.local) return theme::blue;
    constexpr std::array<ImU32, 6> palette{IM_COL32(255, 196, 0, 255), IM_COL32(99, 212, 113, 255),
        IM_COL32(255, 122, 122, 255), IM_COL32(190, 146, 255, 255), IM_COL32(94, 205, 250, 255),
        IM_COL32(255, 160, 90, 255)};
    auto mixed = line.sender * 0x9E3779B97F4A7C15ull;
    mixed ^= mixed >> 29;
    return palette[mixed % palette.size()];
}

// A message's own colour: the server's lines in lavender, everyone else's white.
ImU32 text_colour(const MultiplayerChatLine& line) {
    return line.text_color ? line.text_color : line.server ? multiplayer::nametag_server_text : theme::paper;
}

// "name:"; a role tag is drawn in its own box before it (role_badge.h).
std::string chat_label(const MultiplayerChatLine& line) { return line.name + ":"; }

// What Tab offers for a command's argument: "/tp zee" -> the players starting "zee". Each is the
// whole text the box would hold. Empty when `typed` is not a command with such an argument.
std::vector<std::string> argument_completions(const MultiplayerChat& feed, std::string_view typed) {
    std::vector<std::string> result;
    if (typed.empty() || typed.front() != '/') return result;
    std::string lowered(typed);
    for (auto& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    static const std::vector<std::string> times{"default", "morning", "noon", "afternoon", "evening", "night",
                                                "weatherday", "weathernight"};
    for (const auto& command : feed.commands) {
        if (command.argument.empty() || !lowered.starts_with(command.name + " ")) continue;
        const auto& values = command.argument == "player" ? feed.players : command.argument == "map" ? feed.maps : times;
        const auto partial = lowered.substr(command.name.size() + 1);
        for (const auto& value : values) {
            std::string folded(value);
            for (auto& ch : folded) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (folded.starts_with(partial) && folded != partial) result.push_back(command.name + " " + value);
        }
        break;
    }
    return result;
}

// The "/" commands `typed` can still become (or is already giving arguments to).
std::vector<const MultiplayerChatCommand*> matching_commands(const MultiplayerChat& feed, std::string_view typed) {
    std::vector<const MultiplayerChatCommand*> result;
    if (typed.empty() || typed.front() != '/') return result;
    std::string lowered(typed);
    for (auto& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const auto& command : feed.commands)
        if (command.name.starts_with(lowered) || lowered.starts_with(command.name + " ")) result.push_back(&command);
    return result;
}
// Messages with their :emote: images: chat_rich.h, shared with the chat bubbles.
using namespace chat_rich;

// A chat line as drawn: its label, whether it has emotes and their layout, kept per line
// (sequences never repeat) for the font, size, width and indent it was laid out with, instead
// of laid out again every frame.
struct LineLayout {
    std::size_t length{}; // the line's text: the same line, to be sure
    std::string label;    // chat_label
    bool laid_out{};
    const ImFont* font{};
    float size{}, wrap{}, indent{};
    bool emotes_loaded{};
    bool emotes{};   // has_emotes; without any the text is drawn plainly
    std::string text; // what is drawn: the line's text with any emote the filter masked put back
    RichText rich;   // its layout, when it has
    ImVec2 extent{}; // its size: the layout's, or the plain text's wrapped at `wrap` (the closed view)
};
using LineLayouts = std::map<std::uint64_t, LineLayout>;
struct ChatLayouts {
    LineLayouts open, closed; // the open panel's lines and the closed view's fading ones
};
ChatLayouts& chat_layouts() {
    static ChatLayouts value;
    return value;
}
LineLayout& line_layout(LineLayouts& layouts, const MultiplayerChatLine& line) {
    auto& entry = layouts[line.sequence];
    if (entry.label.empty() || entry.length != line.text.size()) {
        entry = {};
        entry.length = line.text.size();
        entry.label = chat_label(line);
    }
    return entry;
}
const LineLayout& layout_for(LineLayout& entry, const MultiplayerChatLine& line, ImFont* font, float size, float wrap,
                             float indent) {
    const bool loaded = chat_emotes_loaded();
    if (entry.laid_out && entry.font == font && entry.size == size && entry.wrap == wrap && entry.indent == indent &&
        entry.emotes_loaded == loaded)
        return entry;
    entry.laid_out = true;
    entry.font = font;
    entry.size = size;
    entry.wrap = wrap;
    entry.indent = indent;
    entry.emotes_loaded = loaded;
    entry.text = restore_emotes(line.text, line.unmasked);
    entry.emotes = has_emotes(entry.text);
    entry.rich = entry.emotes ? lay_out(font, size, entry.text, wrap, indent) : RichText{};
    entry.extent = entry.emotes ? ImVec2(wrap, entry.rich.height) : font->CalcTextSizeA(size, FLT_MAX, wrap, entry.text.c_str());
    return entry;
}

// The emote being typed: the last word when it is ":" and at least two letters, not closed yet.
// `before` is everything ahead of it.
std::optional<std::string_view> typed_emote(std::string_view input, std::string_view& before) {
    const auto space = input.find_last_of(' ');
    const auto word = space == std::string_view::npos ? input : input.substr(space + 1);
    before = input.substr(0, input.size() - word.size());
    if (word.empty() || word.front() != ':' || word.find(':', 1) != std::string_view::npos || !chat_emotes_loaded())
        return std::nullopt;
    return word.substr(1);
}

// Tab: the first command the typed text can become, with a space for its argument.
// Also puts the cursor after text already in the box when the chat opens (a draft, or the "/" it
// was opened with) and after an emote the player clicked.
int complete_command(ImGuiInputTextCallbackData* data) {
    auto* c = static_cast<ChatState*>(data->UserData);
    if (!c) return 0;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (c->cursor_to_end) {
            c->cursor_to_end = false;
            data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        }
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackCompletion || c->completions.empty()) return 0;
    auto& cycle = c->cycle;
    const bool back = ImGui::GetIO().KeyShift;
    if (!cycle.active) {
        cycle.active = true;
        cycle.typed.assign(data->Buf, static_cast<std::size_t>(data->BufTextLen));
        cycle.options = c->completions;
        cycle.index = back ? cycle.options.size() - 1 : 0;
    } else {
        const auto count = cycle.options.size();
        cycle.index = back ? (cycle.index + count - 1) % count : (cycle.index + 1) % count;
    }
    cycle.written = cycle.options[cycle.index];
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, cycle.written.c_str());
    return 0;
}

// What the suggestion boxes show and Tab cycles through: while cycling, the options of the text
// as it was typed before the first Tab; otherwise those of the text in the box.
std::string suggesting_for(const ChatState& c) { return c.cycle.active ? c.cycle.typed : std::string(c.input.data()); }
bool highlighted(const ChatState& c, const std::string& option, std::size_t index) {
    return c.cycle.active ? option == c.cycle.written : index == 0;
}

// The commands, values and emotes `typed` can become, and what Tab cycles through for it. Built
// again only when `typed`, the feed (polled every 100 ms) or the loaded emotes change: the chat
// panel, its command list and the emote picker all ask every frame.
const ChatState::Suggestions& suggestions(ChatState& c, const std::string& typed) {
    auto& found = c.suggestions;
    const bool loaded = chat_emotes_loaded();
    if (found.valid && found.feed == c.feed_version && found.emotes_loaded == loaded && found.typed == typed) return found;
    found.valid = true;
    found.typed = typed;
    found.feed = c.feed_version;
    found.emotes_loaded = loaded;
    ++found.version;
    found.arguments = argument_completions(c.feed, typed);
    found.commands = matching_commands(c.feed, typed);
    std::string_view before;
    const auto emote = typed_emote(typed, before);
    found.emote = emote.has_value();
    found.before.assign(before);
    found.emote_names = emote ? chat_emote_names(*emote, 1024) : std::vector<std::string>{};
    // Tab: a command's players, maps or times; otherwise the commands the text can still become
    // (not one already typed in full); and the emotes after a ":".
    found.completions = found.arguments;
    if (found.completions.empty())
        for (const auto* command : found.commands)
            if (command->name.size() > typed.size()) found.completions.push_back(command->name);
    for (const auto& name : found.emote_names) found.completions.push_back(found.before + ":" + name + ":");
    return found;
}

// Whether `text` is `before` + ":" + `name` + ":", without building that string.
bool is_emote_text(std::string_view text, std::string_view before, std::string_view name) {
    return text.size() == before.size() + name.size() + 2 && text.starts_with(before) && text[before.size()] == ':' &&
           text.substr(before.size() + 1, name.size()) == name && text.back() == ':';
}

ImU32 with_alpha(ImU32 colour, float alpha) {
    const auto base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    return (colour & ~IM_COL32_A_MASK) |
        (static_cast<ImU32>(base * std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}
} // namespace

bool chat_pending() {
    auto& s = state();
    auto& c = chat();
    const auto now = Clock::now();
    if (now >= c.next_poll) {
        c.next_poll = now + 100ms;
        if (const auto feed = chat_feed.load()) {
            try { c.feed = feed(); } catch (...) { c.feed = {}; }
            ++c.feed_version;
            // Layouts of lines the feed no longer holds: all older than its first (a new
            // session's lines come after every earlier one).
            for (auto* layouts : {&chat_layouts().open, &chat_layouts().closed}) {
                if (c.feed.lines.empty()) layouts->clear();
                else layouts->erase(layouts->begin(), layouts->lower_bound(c.feed.lines.front().sequence));
            }
        }
        s.chat_available.store(c.feed.available);
        // Leaving the session closes an open chat box.
        if (!c.feed.available) s.chat_visible.store(false);
        for (const auto& line : c.feed.lines)
            if (line.sequence > c.seen) {
                c.arrivals.emplace_back(line.sequence, now);
                c.scroll_to_bottom = true;
            }
        c.seen = std::max(c.seen, c.feed.latest);
        while (c.arrivals.size() > multiplayer_chat_history) c.arrivals.pop_front();
    }
    if (s.chat_visible.load()) return true;
    if (!c.feed.available) return false;
    if (c.feed.vote.id || c.feed.announcement.id) return true; // the vote and announcement cards
    for (const auto& [line, at] : c.arrivals)
        if (now - at < chat_hold + chat_fade) return true;
    return false;
}

// The "/" commands matching what is typed, in a box just above the open chat panel.
// The emotes matching what is being typed (":ka"), with their images, above the chat panel.
// The emote picker: from a lone ":", a grid of every emote (narrowed by the letters typed after
// it) just above the open chat panel. Clicking one writes it in; Tab takes the first.
void emote_picker(ChatState& c, ImFont* heading, ImVec2 bottom_left, float width, float scale) {
    const auto& found = suggestions(c, suggesting_for(c));
    if (!found.emote) return;
    // Nothing below asks for the suggestions again, so these stay put while the grid draws.
    const auto& names = found.emote_names;
    const std::string_view before = found.before;
    if (names.empty()) return;
    const float pad = 8.0f * scale, cell = 36.0f * scale, image = 28.0f * scale, label = 22.0f * scale;
    const int columns = std::max(1, static_cast<int>((width - pad * 2.0f) / cell));
    const int rows = (static_cast<int>(names.size()) + columns - 1) / columns;
    const float grid = std::min(rows, 5) * cell;
    ImGui::SetNextWindowPos(bottom_left, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, pad * 2.0f + grid + label), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, with_alpha(theme::ink, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(theme::blue, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(theme::blue, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2((cell - image) * 0.5f, (cell - image) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("##reskate_emote_picker", nullptr, flags)) {
        std::string shown = names.front();
        ImGui::BeginChild("##emote_grid", ImVec2(0.0f, grid), ImGuiChildFlags_None);
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i % static_cast<std::size_t>(columns)) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            const bool current = c.cycle.active && is_emote_text(c.cycle.written, before, names[i]);
            if (current) {
                shown = names[i];
                ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(theme::blue, 0.45f));
            }
            ChatEmote emote;
            bool clicked{};
            if (chat_emote(names[i], emote)) {
                // Wide emotes keep their shape inside the square cell.
                const float w = std::min(image * emote.aspect, image), h = w / emote.aspect;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2((cell - w) * 0.5f, (cell - h) * 0.5f));
                clicked = ImGui::ImageButton("##emote", emote.texture, ImVec2(w, h), emote.uv0, emote.uv1);
                ImGui::PopStyleVar();
            } else {
                clicked = ImGui::Button("?", ImVec2(cell, cell));
            }
            if (current) {
                ImGui::PopStyleColor();
                ImGui::SetScrollHereY(0.5f);
            }
            if (ImGui::IsItemHovered()) shown = names[i];
            if (clicked) {
                const auto inserted = std::string(before) + ":" + names[i] + ":";
                c.input.fill(0);
                std::memcpy(c.input.data(), inserted.data(), std::min(inserted.size(), c.input.size() - 1));
                c.refocus_input = true;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PushFont(heading);
        ImGui::TextUnformatted((":" + shown + ":").c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::muted);
        ImGui::TextUnformatted(("   " + std::to_string(names.size()) + (names.size() == 1 ? " emote" : " emotes") +
                                "  |  click one, or Tab through them").c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(4);
}
// The first row to show so that row `current` of `count` is among the `limit` shown.
std::size_t first_shown(std::size_t current, std::size_t count, std::size_t limit) {
    if (count <= limit || current < limit) return 0;
    return std::min(current + 1 - limit, count - limit);
}
void draw_command_list(ChatState& c, ImFont* heading, ImFont* body, ImVec2 bottom_left, float width, float scale) {
    const auto& found = suggestions(c, suggesting_for(c));
    const auto& arguments = found.arguments;
    if (!arguments.empty()) {
        // The values Tab fills in: players, maps or times.
        constexpr std::size_t limit = 8;
        std::size_t current{};
        for (std::size_t i = 0; i < arguments.size(); ++i)
            if (highlighted(c, arguments[i], i)) current = i;
        const auto first = first_shown(current, arguments.size(), limit);
        const float size = 15.0f * scale, pad = 8.0f * scale, row = size + 4.0f * scale;
        const auto rows = std::min(arguments.size(), limit);
        const float height = pad * 2 + row * static_cast<float>(rows) + (arguments.size() > limit ? row : 0.0f);
        auto* draw = ImGui::GetForegroundDrawList();
        const ImVec2 min(bottom_left.x, bottom_left.y - height), max(bottom_left.x + width, bottom_left.y);
        draw->AddRectFilled(min, max, with_alpha(theme::ink, 0.94f), 4.0f * scale);
        float y = min.y + pad;
        for (std::size_t i = first; i < first + rows; ++i, y += row)
            draw->AddText(heading, size, ImVec2(min.x + pad, y), i == current ? theme::blue : theme::paper, arguments[i].c_str());
        if (arguments.size() > limit)
            draw->AddText(body, size * 0.9f, ImVec2(min.x + pad, y), theme::muted,
                          (std::to_string(current + 1) + " of " + std::to_string(arguments.size()) +
                           "; Tab for the next, Shift+Tab back").c_str());
        return;
    }
    const auto& matches = found.commands;
    if (matches.empty()) return;
    constexpr std::size_t shown_limit = 8;
    std::size_t current{};
    for (std::size_t i = 0; i < matches.size(); ++i)
        if (highlighted(c, matches[i]->name, i)) current = i;
    const auto first = first_shown(current, matches.size(), shown_limit);
    const float size = 15.0f * scale, pad = 8.0f * scale, gap = 4.0f * scale;
    const auto rows = std::min(matches.size(), shown_limit);
    const float row = size + gap;
    const float height = pad * 2 + row * static_cast<float>(rows) - gap + (matches.size() > shown_limit ? row : 0.0f);
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 min(bottom_left.x, bottom_left.y - height), max(bottom_left.x + width, bottom_left.y);
    draw->AddRectFilled(min, max, with_alpha(theme::ink, 0.94f), 4.0f * scale);
    float y = min.y + pad;
    for (std::size_t i = first; i < first + rows; ++i) {
        const auto& command = *matches[i];
        const auto usage_extent = heading->CalcTextSizeA(size, FLT_MAX, 0.0f, command.usage.c_str());
        draw->AddText(heading, size, ImVec2(min.x + pad, y), i == current ? theme::blue : theme::paper, command.usage.c_str());
        draw->PushClipRect(ImVec2(min.x + pad + usage_extent.x + 10.0f * scale, y), ImVec2(max.x - pad, y + row), true);
        draw->AddText(body, size * 0.92f, ImVec2(min.x + pad + usage_extent.x + 10.0f * scale, y + size * 0.05f),
                      theme::muted, command.description.c_str());
        draw->PopClipRect();
        y += row;
    }
    if (matches.size() > shown_limit)
        draw->AddText(body, size * 0.9f, ImVec2(min.x + pad, y), theme::muted,
                      (std::to_string(current + 1) + " of " + std::to_string(matches.size()) +
                       "; Tab for the next, Shift+Tab back").c_str());
}

// The vote a dedicated server is running, or has just finished: a card on the middle of the
// screen's right edge, in the look of the overlay's notices (a dark tile with a strip of colour
// down its left). It says what is voted on, the tally and the time left, and has Yes and No. The
// player answers with their binds (the keycaps on the buttons; F1 and F2 unless changed), or by
// clicking while the cursor is free.
// A poll on the same card: its question, then a row for each answer with its count and a bar of
// its share. The player answers with the number key on the row's keycap, by clicking a row while
// the cursor is free, or with /1, /2... in chat; their answer is the blue row.
void draw_poll(const MultiplayerVote& poll, ImFont* heading, ImFont* body, ImVec2 display, float scale, bool clickable) {
    const bool running = poll.outcome == 0;
    const ImU32 accent = running ? theme::blue : poll.outcome == 3 ? theme::muted : skate_theme::good;
    const char* title = running ? "POLL" : poll.outcome == 3 ? "POLL CANCELLED" : "POLL ENDED";
    const float width = 360.0f * scale, margin = 24.0f * scale, strip = 5.0f * scale;
    const float pad = 14.0f * scale, gap = 8.0f * scale, title_size = 13.0f * scale, size = 18.0f * scale, fine = 13.0f * scale;
    const float row = 30.0f * scale, between = 4.0f * scale, inner = width - strip - pad * 2.0f;
    const auto question_extent = heading->CalcTextSizeA(size, FLT_MAX, inner, poll.label.c_str());
    const bool answers = running && poll.may_vote;
    const auto rows = static_cast<float>(poll.answers.size());
    const float height = pad + title_size + gap + question_extent.y + gap + rows * row + std::max(0.0f, rows - 1.0f) * between +
                         (answers ? gap + fine : 0.0f) + pad;
    const ImVec2 min(display.x - margin - width, std::floor((display.y - height) * 0.5f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, with_alpha(theme::ink, 0.92f));
    draw->AddRectFilled(min, ImVec2(min.x + strip, max.y), accent);
    const float left = min.x + strip + pad, right = max.x - pad;
    float y = min.y + pad;
    draw->AddText(heading, title_size, ImVec2(left, y), accent, title);
    if (running) {
        const auto time = std::to_string(poll.seconds) + " s";
        const auto extent = heading->CalcTextSizeA(title_size, FLT_MAX, 0.0f, time.c_str());
        draw->AddText(heading, title_size, ImVec2(right - extent.x, y), theme::paper, time.c_str());
    }
    y += title_size + gap;
    draw->AddText(heading, size, ImVec2(left, y), theme::paper, poll.label.c_str(), nullptr, inner);
    y += question_extent.y + gap;
    unsigned total{}, most{};
    for (const auto count : poll.counts) total += count, most = std::max(most, count);
    for (std::size_t i = 0; i < poll.answers.size(); ++i) {
        const auto count = i < poll.counts.size() ? poll.counts[i] : 0U;
        const ImVec2 a(left, y), b(right, y + row);
        const bool chosen = poll.mine == i + 1;
        const bool hovered = answers && clickable && ImGui::IsMouseHoveringRect(a, b, false);
        draw->AddRectFilled(a, b, hovered ? skate_theme::tile_light : skate_theme::tile_grey);
        // Its share of the answers as a bar along the bottom; once ended, the winner's is green.
        const float share = total ? static_cast<float>(count) / static_cast<float>(total) : 0.0f;
        const ImU32 fill = chosen ? theme::blue : !running && count && count == most ? skate_theme::good : theme::muted;
        if (share > 0.0f) draw->AddRectFilled(ImVec2(a.x, b.y - 3.0f * scale), ImVec2(a.x + (b.x - a.x) * share, b.y), fill);
        if (chosen) draw->AddRectFilled(a, ImVec2(a.x + 3.0f * scale, b.y), theme::blue);
        const auto number = std::to_string(i + 1);
        const float text_y = y + (row - fine) * 0.5f - 1.0f * scale;
        // The key that gives this answer, as the game shows its own keys.
        {
            const float cap = 18.0f * scale;
            const ImVec2 at(a.x + 6.0f * scale, y + (row - cap) * 0.5f);
            const auto extent = heading->CalcTextSizeA(12.0f * scale, FLT_MAX, 0.0f, number.c_str());
            draw->AddRectFilled(at, ImVec2(at.x + cap, at.y + cap), chosen ? theme::blue : theme::paper, 3.0f * scale);
            draw->AddText(heading, 12.0f * scale, ImVec2(at.x + (cap - extent.x) * 0.5f, at.y + (cap - extent.y) * 0.5f),
                          chosen ? theme::paper : skate_theme::black, number.c_str());
        }
        const auto counted = std::to_string(count);
        const float count_width = heading->CalcTextSizeA(fine, FLT_MAX, 0.0f, counted.c_str()).x;
        draw->PushClipRect(ImVec2(a.x + 32.0f * scale, a.y), ImVec2(b.x - count_width - 18.0f * scale, b.y), true);
        draw->AddText(body, fine + 1.0f * scale, ImVec2(a.x + 32.0f * scale, text_y), theme::paper, poll.answers[i].c_str());
        draw->PopClipRect();
        draw->AddText(heading, fine, ImVec2(b.x - 10.0f * scale - count_width, text_y), theme::paper, counted.c_str());
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) queue_multiplayer_action("vote", number);
        y += row + between;
    }
    if (answers) {
        const auto hint = "Press 1 to " + std::to_string(poll.answers.size()) + " on the keyboard, or click an answer.";
        draw->AddText(body, fine, ImVec2(left, y - between + gap), theme::muted, hint.c_str());
    }
}

// A dedicated server's announcement: a card at the top middle of the screen in the same look,
// blue down its left, for as long as the server shows it.
void draw_announcement(const MultiplayerAnnouncement& announcement, ImFont* heading, ImFont* body, ImVec2 display, float scale) {
    if (!announcement.id || announcement.text.empty()) return;
    const float width = std::min(520.0f * scale, display.x - 48.0f * scale), strip = 5.0f * scale;
    const float pad = 14.0f * scale, gap = 6.0f * scale, title_size = 13.0f * scale, size = 18.0f * scale;
    const float inner = width - strip - pad * 2.0f;
    const auto text_extent = body->CalcTextSizeA(size, FLT_MAX, inner, announcement.text.c_str());
    const float height = pad + title_size + gap + text_extent.y + pad;
    const ImVec2 min(std::floor((display.x - width) * 0.5f), 24.0f * scale), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, with_alpha(theme::ink, 0.92f));
    draw->AddRectFilled(min, ImVec2(min.x + strip, max.y), theme::blue);
    const float left = min.x + strip + pad;
    draw->AddText(heading, title_size, ImVec2(left, min.y + pad), theme::blue, "ANNOUNCEMENT");
    draw->AddText(body, size, ImVec2(left, min.y + pad + title_size + gap), theme::paper, announcement.text.c_str(), nullptr, inner);
}

void draw_vote(const MultiplayerVote& vote, ImFont* heading, ImFont* body, ImVec2 display, float scale, bool clickable) {
    if (!vote.id) return;
    if (vote.poll) return draw_poll(vote, heading, body, display, scale, clickable);
    constexpr ImU32 green = skate_theme::good, red = skate_theme::danger;
    const bool running = vote.outcome == 0;
    const ImU32 accent = running ? theme::blue : vote.outcome == 1 ? green : vote.outcome == 2 ? red : theme::muted;
    const char* title = running ? "VOTE" : vote.outcome == 1 ? "VOTE PASSED" : vote.outcome == 2 ? "VOTE FAILED" : "VOTE CANCELLED";
    std::string question = vote.label;
    if (!question.empty() && question.front() >= 'a' && question.front() <= 'z') question.front() = static_cast<char>(question.front() - 32);
    if (running) question += '?';
    const float width = 360.0f * scale, margin = 24.0f * scale, strip = 5.0f * scale;
    const float pad = 14.0f * scale, gap = 8.0f * scale, title_size = 13.0f * scale, size = 18.0f * scale, fine = 13.0f * scale;
    const float button_height = 34.0f * scale, bar = 4.0f * scale, inner = width - strip - pad * 2.0f;
    const auto question_extent = heading->CalcTextSizeA(size, FLT_MAX, inner, question.c_str());
    const bool answers = running && vote.may_vote;
    const float height = pad + title_size + gap + question_extent.y + gap + bar + gap + fine +
                         (running ? gap + (answers ? button_height : fine) : 0.0f) + pad;
    const ImVec2 min(display.x - margin - width, std::floor((display.y - height) * 0.5f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, with_alpha(theme::ink, 0.92f));
    draw->AddRectFilled(min, ImVec2(min.x + strip, max.y), accent);
    const float left = min.x + strip + pad, right = max.x - pad;
    float y = min.y + pad;
    draw->AddText(heading, title_size, ImVec2(left, y), accent, title);
    if (running) {
        const auto time = std::to_string(vote.seconds) + " s";
        const auto extent = heading->CalcTextSizeA(title_size, FLT_MAX, 0.0f, time.c_str());
        draw->AddText(heading, title_size, ImVec2(right - extent.x, y), theme::paper, time.c_str());
    }
    y += title_size + gap;
    draw->AddText(heading, size, ImVec2(left, y), theme::paper, question.c_str(), nullptr, inner);
    y += question_extent.y + gap;
    // How far Yes is towards what it needs, then the tally under it.
    const float share = vote.needed ? std::clamp(static_cast<float>(vote.yes) / static_cast<float>(vote.needed), 0.0f, 1.0f) : 0.0f;
    draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + bar), skate_theme::tile_light);
    if (share > 0.0f) draw->AddRectFilled(ImVec2(left, y), ImVec2(left + inner * share, y + bar), green);
    y += bar + gap;
    float x = left;
    const auto word = [&](const std::string& text, ImU32 colour) {
        draw->AddText(body, fine, ImVec2(x, y), colour, text.c_str());
        x += body->CalcTextSizeA(fine, FLT_MAX, 0.0f, text.c_str()).x + 14.0f * scale;
    };
    word("Yes " + std::to_string(vote.yes), green);
    word("No " + std::to_string(vote.no), red);
    {
        const auto needed = std::to_string(vote.needed) + " needed";
        const auto extent = body->CalcTextSizeA(fine, FLT_MAX, 0.0f, needed.c_str());
        draw->AddText(body, fine, ImVec2(right - extent.x, y), theme::muted, needed.c_str());
    }
    y += fine;
    if (!running) return;
    y += gap;
    if (!answers) {
        draw->AddText(body, fine, ImVec2(left, y), theme::muted, "This vote is about you, so you have no vote in it.");
        return;
    }
    // Two tiles as the menu's: grey, the answer's colour once it is this player's, each with its
    // bind on a white keycap.
    const float button_width = (inner - gap) * 0.5f;
    const auto button = [&](float at, bool yes, std::uint32_t bind) {
        const ImVec2 a(at, y), b(at + button_width, y + button_height);
        const ImU32 colour = yes ? green : red;
        const bool chosen = vote.mine == (yes ? 1 : 2);
        const bool hovered = clickable && ImGui::IsMouseHoveringRect(a, b, false);
        draw->AddRectFilled(a, b, chosen ? colour : hovered ? skate_theme::tile_light : skate_theme::tile_grey);
        if (!chosen) draw->AddRectFilled(ImVec2(a.x, b.y - 3.0f * scale), b, colour);
        const std::string key = bind ? dingosdk::controller_combo_label(bind) : std::string{};
        const char* label = yes ? "YES" : "NO";
        const float label_size = 15.0f * scale, key_size = 12.0f * scale, cap_height = 20.0f * scale;
        const auto label_extent = heading->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
        const auto key_extent = key.empty() ? ImVec2{} : heading->CalcTextSizeA(key_size, FLT_MAX, 0.0f, key.c_str());
        const float cap_width = key.empty() ? 0.0f : key_extent.x + 12.0f * scale, between = key.empty() ? 0.0f : 8.0f * scale;
        float cursor = a.x + (button_width - cap_width - between - label_extent.x) * 0.5f;
        if (!key.empty()) {
            const ImVec2 cap(cursor, a.y + (button_height - cap_height) * 0.5f);
            draw->AddRectFilled(cap, ImVec2(cap.x + cap_width, cap.y + cap_height), theme::paper, 3.0f * scale);
            draw->AddText(heading, key_size, ImVec2(cap.x + 6.0f * scale, cap.y + (cap_height - key_extent.y) * 0.5f), skate_theme::black,
                          key.c_str());
            cursor += cap_width + between;
        }
        draw->AddText(heading, label_size, ImVec2(cursor, a.y + (button_height - label_extent.y) * 0.5f),
                      chosen ? skate_theme::black : theme::paper, label);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) queue_multiplayer_action("vote", yes ? "yes" : "no");
    };
    button(left, true, vote.yes_bind);
    button(left + button_width + gap, false, vote.no_bind);
}

void draw_chat() {
    auto& s = state();
    auto& c = chat();
    if (!c.feed.available) return;
    auto* heading = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(display.y / 1080.0f, 1.0f, 2.0f);
    const float width = 520.0f * scale, margin = 24.0f * scale;

    if (s.chat_visible.load()) {
        ImGui::SetNextWindowPos(ImVec2(display.x - margin, display.y - margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 320.0f * scale), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, with_alpha(theme::ink, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(theme::blue, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(12, 12, 12, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::paper);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##reskate_chat", nullptr, flags)) {
            ImGui::PushFont(body);
            ImGui::SetWindowFontScale(scale * 0.9f);
            ImGui::BeginChild("##reskate_chat_lines", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()));
            ImGui::PushTextWrapPos(0.0f);
            auto& layouts = chat_layouts().open;
            for (const auto& line : c.feed.lines) {
                auto& layout = line_layout(layouts, line);
                const ImVec2 line_start = ImGui::GetCursorScreenPos();
                const float line_width = ImGui::GetContentRegionAvail().x;
                if (!line.tag.empty()) {
                    const float size = ImGui::GetFontSize(), height = ImGui::GetTextLineHeight();
                    const float badge = role_badge_width(heading, size, line.tag);
                    draw_role_badge(ImGui::GetWindowDrawList(), heading, size, ImGui::GetCursorScreenPos(), height, line.tag,
                               name_colour(line), 1.0f);
                    ImGui::Dummy(ImVec2(badge, height));
                    ImGui::SameLine(0.0f, 5.0f * scale);
                }
                auto* draw = ImGui::GetWindowDrawList();
                const int name_vertices = draw->VtxBuffer.Size;
                ImGui::PushStyleColor(ImGuiCol_Text, name_colour(line));
                ImGui::TextUnformatted(layout.label.c_str());
                ImGui::PopStyleColor();
                shade_nametag_gradient(draw, name_vertices, ImGui::GetItemRectMin().x, ImGui::GetItemRectSize().x,
                                       name_colour(line), ImGui::GetTime());
                ImGui::SameLine(0.0f, 6.0f * scale);
                const auto& drawn = layout_for(layout, line, ImGui::GetFont(), ImGui::GetFontSize(), line_width,
                                               ImGui::GetCursorScreenPos().x - line_start.x);
                ImGui::PushStyleColor(ImGuiCol_Text, text_colour(line));
                if (drawn.emotes) {
                    draw_rich(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), line_start, drawn.text,
                              drawn.rich, ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
                    ImGui::SetCursorScreenPos(line_start);
                    ImGui::Dummy(ImVec2(line_width, drawn.rich.height));
                } else {
                    ImGui::TextUnformatted(drawn.text.c_str());
                }
                ImGui::PopStyleColor();
            }
            if (c.feed.lines.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::muted);
                ImGui::TextUnformatted("No messages yet. Everyone in the session sees what you send.");
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            if (c.scroll_to_bottom) {
                ImGui::SetScrollHereY(1.0f);
                c.scroll_to_bottom = false;
            }
            ImGui::EndChild();
            if (s.chat_focus_requested.exchange(false)) {
                c.scroll_to_bottom = true;
                // Opened with "/": an empty box starts the command; a draft left in it stays.
                if (s.chat_command_requested.exchange(false) && !c.input[0]) {
                    c.input.fill(0);
                    c.input[0] = '/';
                }
                c.cursor_to_end = true;
                // The overlay runs no frames while nothing of it is on screen, so after Esc closed
                // the chat ImGui can still hold the box as active, and would write its old text back
                // over ours: have it take the text from the buffer again.
                const auto input_id = ImGui::GetID("##reskate_chat_input");
                if (auto* active = ImGui::GetInputTextState(input_id); active && ImGui::GetActiveID() == input_id)
                    active->ReloadUserBufAndMoveToEnd();
                ImGui::SetKeyboardFocusHere();
            } else if (c.refocus_input) {
                c.refocus_input = false;
                c.cursor_to_end = true;
                ImGui::SetKeyboardFocusHere();
            }
            // Typing "/" lists the commands (drawn above the panel below); Tab cycles through them,
            // a command's players, maps or times, or the emotes after a ":".
            if (c.cycle.active && std::string_view(c.input.data()) != c.cycle.written) c.cycle = {};
            if (const auto& found = suggestions(c, suggesting_for(c)); found.version != c.completions_version) {
                c.completions = found.completions;
                c.completions_version = found.version;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##reskate_chat_input", "Say something, or / for commands  (Enter sends, Esc closes)",
                                         c.input.data(), c.input.size(),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                                             ImGuiInputTextFlags_CallbackAlways,
                                         complete_command, &c)) {
                const std::string text(c.input.data());
                if (text.find_first_not_of(' ') != std::string::npos) queue_multiplayer_action("chat", text);
                c.input.fill(0);
                s.chat_visible.store(false);
            }
            ImGui::PopFont();
        }
        const auto panel_top = ImGui::GetWindowPos().y;
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        const float above = panel_top - 6.0f * scale;
        draw_command_list(c, heading, body, ImVec2(display.x - margin - width, above), width, scale);
        emote_picker(c, heading, ImVec2(display.x - margin - width, above), width, scale);
        draw_announcement(c.feed.announcement, heading, body, display, scale);
        draw_vote(c.feed.vote, heading, body, display, scale, true); // the cursor is free while typing
        return;
    }

    // Closed: the newest lines still within their hold, stacked up from the
    // corner, each fading on its own. Drawn without a window, so no input.
    const auto now = Clock::now();
    std::vector<std::pair<const MultiplayerChatLine*, float>> shown;
    for (auto it = c.feed.lines.rbegin(); it != c.feed.lines.rend() && shown.size() < closed_lines; ++it) {
        const auto at = arrived(c, it->sequence);
        if (at == Clock::time_point{}) continue;
        const auto age = now - at;
        if (age >= chat_hold + chat_fade) break;
        const float alpha = age <= chat_hold ? 1.0f
            : 1.0f - std::chrono::duration<float>(age - chat_hold) / std::chrono::duration<float>(chat_fade);
        shown.emplace_back(&*it, alpha);
    }
    auto* draw = ImGui::GetForegroundDrawList();
    const float size = body->FontSize * scale * 0.9f, name_size = heading->FontSize * scale * 0.9f;
    const float pad_x = 10.0f * scale, pad_y = 5.0f * scale, gap = 4.0f * scale;
    const float text_width = width - pad_x * 2.0f;
    float bottom = display.y - margin;
    auto& layouts = chat_layouts().closed;
    for (const auto& [line, alpha] : shown) {
        auto& layout = line_layout(layouts, *line);
        const auto& label = layout.label;
        const auto name_extent = heading->CalcTextSizeA(name_size, FLT_MAX, 0.0f, label.c_str());
        const float badge = line->tag.empty() ? 0.0f : role_badge_width(heading, name_size, line->tag) + 5.0f * scale;
        const float indent = badge + name_extent.x + 6.0f * scale;
        // The message wraps under the name rather than beside it, so a long
        // line stays inside the panel.
        const bool beside = indent < text_width * 0.5f;
        const float wrap = beside ? text_width - indent : text_width;
        const auto& drawn = layout_for(layout, *line, body, size, wrap, 0.0f);
        const bool emotes = drawn.emotes;
        const auto text_extent = drawn.extent;
        const float height = beside ? std::max(name_extent.y, text_extent.y) : name_extent.y + text_extent.y;
        const ImVec2 top_left(display.x - margin - width, bottom - height - pad_y * 2.0f);
        draw->AddRectFilled(top_left, ImVec2(display.x - margin, bottom), with_alpha(theme::ink, 0.78f * alpha),
                            4.0f * scale);
        const ImVec2 at(top_left.x + pad_x, top_left.y + pad_y);
        draw_role_badge(draw, heading, name_size, at, name_extent.y, line->tag, name_colour(*line), alpha);
        const int name_vertices = draw->VtxBuffer.Size;
        draw->AddText(heading, name_size, ImVec2(at.x + badge, at.y), with_alpha(name_colour(*line), alpha), label.c_str());
        shade_nametag_gradient(draw, name_vertices, at.x + badge, name_extent.x, name_colour(*line), ImGui::GetTime());
        const ImVec2 text_at = beside ? ImVec2(at.x + indent, at.y) : ImVec2(at.x, at.y + name_extent.y);
        const auto text = with_alpha(text_colour(*line), alpha);
        if (emotes) draw_rich(draw, body, size, text_at, drawn.text, drawn.rich, text, alpha);
        else draw->AddText(body, size, text_at, text, drawn.text.c_str(), nullptr, wrap);
        bottom = top_left.y - gap;
    }
    // Clickable while the menu or the console has freed the cursor.
    draw_announcement(c.feed.announcement, heading, body, display, scale);
    draw_vote(c.feed.vote, heading, body, display, scale, s.visible.load() || s.console_visible.load());
}
} // namespace dingosdk::overlay::detail
