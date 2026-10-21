#include "modding_menu.h"
#include "skate_menu.h"
#include "skate_menu_internal.h"
#include "Engine/Vfs/builtin_packs.h"
#include "Engine/Vfs/initfs.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_conflicts.h"
#include "Extension/Assets/live_mods.h"
#include "Extension/BoardBreak/board_break.h"
#include "Extension/UI/skate_theme.h"
#include "deck_preview.h"
#include "Engine/Core/Log/logging.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

namespace dingosdk::overlay {
namespace {
// What the Mods folder holds, read again on a thread of its own every `period` while a page asks
// for it; the page draws the last that was read. These pages are drawn on the game's render
// thread, and reading the folder there (every mod's files, a whole folder's fingerprint for an
// excluded one) held each frame up for tens of milliseconds to seconds, every 1.5 s (2026-10-15).
template<class T> class FolderRead {
public:
    template<class Read> std::shared_ptr<const T> get(Read read, ULONGLONG period) {
        const auto now = GetTickCount64();
        if (now >= next_.load() && !reading_.exchange(true)) {
            next_ = now + period;
            try {
                std::thread([this, read] {
                    std::shared_ptr<const T> fresh;
                    try { fresh = std::make_shared<const T>(read()); } catch (...) {}
                    if (fresh) {
                        std::lock_guard lock(mutex_);
                        value_ = std::move(fresh);
                    }
                    reading_ = false;
                }).detach();
            } catch (...) { reading_ = false; }
        }
        std::lock_guard lock(mutex_);
        return value_;
    }
    // What the page itself has just changed, shown until the next read.
    void set(T value) {
        std::lock_guard lock(mutex_);
        value_ = std::make_shared<const T>(std::move(value));
    }
    void again() { next_ = 0; }
private:
    std::mutex mutex_;
    std::shared_ptr<const T> value_;
    std::atomic<bool> reading_{};
    std::atomic<ULONGLONG> next_{};
};
struct DeckScan {
    std::vector<std::string> installed, titles; // each installed deck's name for people
    std::string chosen;
};
}
namespace menu {
// Board Break's decks (Extension/BoardBreak/board_break.h). The game has one deck model and each
// deck is a mod's copy of it, so picking one applies the mods with that deck first and loads the
// level again. A button for each deck the feature was made with, greyed out while it is not
// installed, and for any other deck that is; the Mods folder is read again every 1.5 s while the
// card shows. A deck goes by the name its deck.ini gives it (label), else by its folder's.
void board_break_deck_card(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    static auto* scans = new FolderRead<DeckScan>; // process lifetime, as the threads that fill it
    const auto scan = scans->get([] {
        DeckScan read;
        read.installed = mods::installed_board_break_decks();
        read.chosen = mods::wanted_board_break_deck();
        for (const auto& deck : read.installed) read.titles.push_back(board_break::deck_label(deck));
        return read;
    }, 1500);
    if (!scan) return;
    const auto& installed = scan->installed;
    const auto& titles = scan->titles;
    const auto& chosen = scan->chosen;
    if (!model.board_break.available || installed.empty()) return;
    const auto there = [&](const std::string& deck) { return std::find(installed.begin(), installed.end(), deck) != installed.end(); };
    // The known decks in their own order, then the others that are installed: the numbers are the
    // ones `boardbreak deck` prints.
    std::vector<std::string> decks(mods::board_break_decks.begin(), mods::board_break_decks.end());
    for (const auto& deck : installed)
        if (std::find(decks.begin(), decks.end(), deck) == decks.end()) decks.push_back(deck);
    const auto label = [&](const std::string& deck) {
        std::string name;
        if (const auto at = std::find(installed.begin(), installed.end(), deck);
            at != installed.end() && static_cast<std::size_t>(at - installed.begin()) < titles.size())
            name = titles[static_cast<std::size_t>(at - installed.begin())];
        else {
            name = deck;
            bool start = true;
            for (auto& c : name) {
                if (c == '_') c = ' ';
                else if (start && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                start = c == ' ';
            }
        }
        const auto place = std::find(decks.begin(), decks.end(), deck) - decks.begin() + 1;
        return std::to_string(place) + " " + name;
    };
    const auto in_play = mods::board_break_in_play();
    begin_card(menu, "board-break-decks", "BOARD BREAK DECK", in_play.empty() ? "none active" : nullptr);
    const bool busy = live_mods::busy();
    // The carousel's panel, the preview's backdrop: darker than the card, as the menu's sidebar.
    constexpr ImU32 panel = IM_COL32(32, 32, 33, 255);
    if (menu::console_style()) {
        // Full screen: one row, left / right to a deck and A to put it in the game; the model of the
        // deck on the row turns under it.
        static int staged = -1;
        static std::string staged_from;
        std::vector<std::string> names;
        int playing = -1;
        for (std::size_t i = 0; i < decks.size(); ++i) {
            names.push_back(label(decks[i]) + (there(decks[i]) ? "" : " (not installed)"));
            if (decks[i] == in_play) playing = static_cast<int>(i);
        }
        if (staged < 0 || staged >= static_cast<int>(decks.size()) || staged_from != in_play) staged = playing, staged_from = in_play;
        bool load = false;
        const bool waiting = staged >= 0 && decks[static_cast<std::size_t>(staged)] != in_play;
        menu::option_row(menu, waiting ? "Deck   - A puts it in###deck" : "Deck###deck",
                         waiting ? "Press A to put this deck in the game (the level loads again)." : "Left / right to pick a deck, then A to put it in the game.",
                         staged, names, !busy && callbacks.queue_console_command, &load);
        if (load && staged >= 0) {
            const auto& deck = decks[static_cast<std::size_t>(staged)];
            if (!there(deck)) menu::feedback(menu, "That deck is not installed.");
            else if (deck != in_play) send_console(menu, callbacks, "boardbreak deck " + deck);
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float full = ImGui::GetContentRegionAvail().x;
        const float tall = std::clamp(full * 0.45f, px(240), px(420));
        deck_preview::draw(ImGui::GetWindowDrawList(), at, ImVec2(at.x + full, at.y + tall),
                           staged >= 0 ? decks[static_cast<std::size_t>(staged)] : in_play, panel);
        ImGui::Dummy(ImVec2(full, tall));
    } else {
        // In a window: a carousel. The deck's name between two arrows, the decks either side of it
        // under them, and its model turning below; the arrows, the names either side or left /
        // right over the panel go through the decks without loading anything, and the button
        // under the panel puts the deck shown in the game.
        static int browse = -1;
        static std::string browse_from;
        const int count = static_cast<int>(decks.size());
        int playing = 0;
        for (int i = 0; i < count; ++i)
            if (decks[static_cast<std::size_t>(i)] == in_play) playing = i;
        if (browse < 0 || browse >= count || browse_from != in_play) browse = playing, browse_from = in_play;
        const auto wrap = [&](int i) { return static_cast<std::size_t>(((i % count) + count) % count); };
        // The carousel's names without the numbers `boardbreak deck` knows them by.
        const auto name_of = [&](const std::string& deck) {
            auto text = label(deck);
            const auto space = text.find(' ');
            return space == std::string::npos ? text : text.substr(space + 1);
        };

        // The swipe: when the deck changes, every name slides out the way the carousel turned and
        // the new one slides in after it (the arrows fade out meanwhile, and back once it is over).
        struct Swipe {
            int from = -1, dir = 0;
            float t = 1.0f, arrows = 1.0f;
        };
        static Swipe swipe;
        constexpr float swipe_time = 0.26f, arrows_out = 0.08f, arrows_in = 0.18f;
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        if (swipe.t < 1.0f) swipe.t = std::min(1.0f, swipe.t + dt / swipe_time);
        const bool swiping = swipe.t < 1.0f && swipe.from >= 0 && swipe.from < count;
        swipe.arrows = swiping ? std::max(0.0f, swipe.arrows - dt / arrows_out) : std::min(1.0f, swipe.arrows + dt / arrows_in);
        const auto go = [&](int to, int dir) {
            to = static_cast<int>(wrap(to));
            if (to == browse) return;
            swipe.from = browse, swipe.dir = dir, swipe.t = 0.0f;
            browse = to;
        };
        const float eased = 1.0f - std::pow(1.0f - swipe.t, 3.0f); // ease out
        const auto faded = [](ImU32 colour, float alpha) {
            const auto a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f);
            return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
        };

        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float full = ImGui::GetContentRegionAvail().x;
        const float pad = px(14), gap = px(8);
        const float pill_h = ImGui::GetFrameHeight() + px(12), side_h = ImGui::GetFrameHeight();
        const float view_h = std::clamp(full * 0.62f, px(260), px(440));
        const float total_h = pad + pill_h + gap + side_h + gap + view_h + pad;
        const ImVec2 end(at.x + full, at.y + total_h);
        draw->AddRectFilled(at, end, panel);
        int step = 0;
        // The mark of the deck in the game: a blue asterisk after its name, wherever it shows.
        const auto asterisk = [&](ImDrawList* list, ImVec2 centre, ImU32 colour) {
            const float r = px(5.5f), thick = px(2.2f);
            for (int k = 0; k < 3; ++k) {
                const float angle = 3.14159265f * (0.5f + k / 3.0f);
                const ImVec2 d(std::cos(angle) * r, std::sin(angle) * r);
                list->AddLine(ImVec2(centre.x - d.x, centre.y - d.y), ImVec2(centre.x + d.x, centre.y + d.y), colour, thick);
            }
        };
        const float mark_w = px(16);
        const auto text_at = [&](ImFont* font, ImVec2 a, ImVec2 z, const std::string& text, ImU32 colour, float shift, bool active) {
            ImGui::PushFont(font);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const float wide = size.x + (active ? mark_w : 0.0f);
            const ImVec2 where(std::max(a.x + px(6), (a.x + z.x - wide) * 0.5f) + shift, (a.y + z.y - size.y) * 0.5f);
            draw->PushClipRect(a, z, true);
            draw->AddText(where, colour, text.c_str());
            if (active) {
                const float alpha = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
                asterisk(draw, ImVec2(where.x + size.x + mark_w * 0.62f, (a.y + z.y) * 0.5f), faded(skate_theme::blue, alpha));
            }
            draw->PopClipRect();
            ImGui::PopFont();
        };
        // The carousel itself: five boxes on a wheel (the deck shown in the big box in the middle,
        // its neighbours in the smaller boxes under the arrows, and one more hidden past each end).
        // On a swipe every box rolls to its neighbour's place, growing, shrinking and changing colour
        // on the way: the one coming in from past the end fades in, the one going out fades away.
        const float arrow_w = pill_h;
        const float pill_w = std::clamp(full - 2 * pad - 2 * (arrow_w + gap), px(120), px(320));
        const float y = at.y + pad;
        const ImVec2 pill_a(at.x + (full - pill_w) * 0.5f, y), pill_z(pill_a.x + pill_w, y + pill_h);
        const float side_w = std::min((full - 2 * pad - gap) * 0.5f, px(240));
        const float side_y = pill_z.y + gap;
        const ImVec2 left_a(at.x + pad, side_y), right_a(end.x - pad - side_w, side_y);
        struct Look {
            ImVec2 a, z;
            float big, alpha; // 1 for the middle box, 0 for a small one; how much it shows
        };
        const auto slot = [&](int k) {
            k = std::clamp(k, -2, 2);
            if (k == 0) return Look{pill_a, pill_z, 1.0f, 1.0f};
            const float shove = k == -2 ? -(side_w + gap) : k == 2 ? side_w + gap : 0.0f;
            const ImVec2 base = k < 0 ? left_a : right_a;
            return Look{ImVec2(base.x + shove, base.y), ImVec2(base.x + shove + side_w, base.y + side_h), 0.0f,
                        std::abs(k) == 2 ? 0.0f : 1.0f};
        };
        const auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
        const auto mix_colour = [&](ImU32 p, ImU32 q, float t) {
            ImU32 out = 0;
            for (const int shift : {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT, IM_COL32_A_SHIFT}) {
                const float value = mix(static_cast<float>((p >> shift) & 0xff), static_cast<float>((q >> shift) & 0xff), t);
                out |= static_cast<ImU32>(std::clamp(value, 0.0f, 255.0f) + 0.5f) << shift;
            }
            return out;
        };
        const bool listing = ImGui::IsPopupOpen("##deck-list");
        const bool hover_big = !swiping && (listing || ImGui::IsMouseHoveringRect(pill_a, pill_z));
        const bool hover_left = !swiping && count > 1 && ImGui::IsMouseHoveringRect(left_a, ImVec2(left_a.x + side_w, left_a.y + side_h));
        const bool hover_right = !swiping && count > 1 && ImGui::IsMouseHoveringRect(right_a, ImVec2(right_a.x + side_w, right_a.y + side_h));
        struct Box {
            Look look;
            int deck;
        };
        std::vector<Box> boxes;
        for (int k = -2; k <= 2; ++k) {
            if (count < 2 && k != 0) continue;
            if (count < 3 && std::abs(k) == 2) continue;
            Look look = slot(k);
            if (swiping) {
                const Look from = slot(k + swipe.dir);
                look = Look{ImVec2(mix(from.a.x, look.a.x, eased), mix(from.a.y, look.a.y, eased)),
                            ImVec2(mix(from.z.x, look.z.x, eased), mix(from.z.y, look.z.y, eased)), mix(from.big, look.big, eased),
                            mix(from.alpha, look.alpha, eased)};
            }
            if (look.alpha > 0.002f) boxes.push_back({look, static_cast<int>(wrap(browse + k))});
        }
        std::sort(boxes.begin(), boxes.end(), [](const Box& p, const Box& q) { return p.look.big < q.look.big; });
        draw->PushClipRect(at, end, true);
        for (const auto& box : boxes) {
            const auto& item = decks[static_cast<std::size_t>(box.deck)];
            const auto& l = box.look;
            const bool hovered = l.big > 0.99f ? hover_big : (l.a.x < pill_a.x ? hover_left : hover_right) && l.big < 0.01f;
            ImU32 bg = mix_colour(hovered ? IM_COL32(54, 54, 56, 255) : IM_COL32(44, 44, 45, 255),
                                  hovered ? IM_COL32(72, 72, 74, 255) : IM_COL32(63, 63, 64, 255), l.big);
            bg = faded(bg, l.alpha);
            draw->AddRectFilled(l.a, l.z, bg);
            if (item == in_play)
                draw->AddRectFilled(ImVec2(l.a.x, l.z.y - px(3)), l.z, faded(skate_theme::blue, l.alpha * l.big));
            const ImU32 text = there(item) ? mix_colour(IM_COL32(150, 152, 158, 255), skate_theme::white, l.big)
                                           : mix_colour(IM_COL32(95, 96, 100, 255), skate_theme::grey_text, l.big);
            const float inset = px(24) * l.big; // room for the list's chevron in the big box
            text_at(menu.bold, ImVec2(l.a.x + inset, l.a.y), ImVec2(l.z.x - inset, l.z.y), name_of(item), faded(text, l.alpha), 0.0f,
                    item == in_play);
        }
        draw->PopClipRect();

        // The arrows (faded out while the wheel turns), and what the boxes do when clicked: the big
        // one opens the list of every deck, the small ones turn the wheel to their deck.
        const auto arrow = [&](const char* id, float x, bool left) {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(arrow_w, pill_h));
            const ImU32 colour = faded(ImGui::IsItemHovered() && swipe.arrows > 0.99f ? skate_theme::white : IM_COL32(145, 145, 145, 255),
                                       swipe.arrows);
            const ImVec2 m(x + arrow_w * 0.5f, y + pill_h * 0.5f);
            const float r = pill_h * 0.26f;
            if (left) draw->AddTriangleFilled(ImVec2(m.x - r, m.y), ImVec2(m.x + r * 0.7f, m.y - r), ImVec2(m.x + r * 0.7f, m.y + r), colour);
            else draw->AddTriangleFilled(ImVec2(m.x + r, m.y), ImVec2(m.x - r * 0.7f, m.y + r), ImVec2(m.x - r * 0.7f, m.y - r), colour);
            return pressed && !swiping;
        };
        if (arrow("##deck-previous", pill_a.x - gap - arrow_w, true)) step = -1;
        if (arrow("##deck-next", pill_z.x + gap, false)) step = 1;
        const auto& deck = decks[static_cast<std::size_t>(browse)];
        ImGui::SetCursorScreenPos(pill_a);
        if (ImGui::InvisibleButton("##deck-name", ImVec2(pill_w, pill_h)) && !swiping) ImGui::OpenPopup("##deck-list");
        {
            // The list's mark: a small chevron at the big box's right end, pointing up while it is
            // open; it fades with the arrows.
            const float r = px(4.5f), cx = pill_z.x - px(16), cy = (pill_a.y + pill_z.y) * 0.5f;
            const ImU32 c = faded(IM_COL32(150, 152, 158, 255), swipe.arrows);
            if (listing) draw->AddTriangleFilled(ImVec2(cx - r, cy + r * 0.5f), ImVec2(cx, cy - r * 0.6f), ImVec2(cx + r, cy + r * 0.5f), c);
            else draw->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.5f), ImVec2(cx + r, cy - r * 0.5f), ImVec2(cx, cy + r * 0.6f), c);
        }
        if (count > 1) {
            ImGui::SetCursorScreenPos(left_a);
            if (ImGui::InvisibleButton("##deck-before", ImVec2(side_w, side_h)) && !swiping) step = -1;
            ImGui::SetCursorScreenPos(right_a);
            if (ImGui::InvisibleButton("##deck-after", ImVec2(side_w, side_h)) && !swiping) step = 1;
        }

        // The list: every deck, the one in the game marked; picking one only goes to it.
        ImGui::SetNextWindowPos(ImVec2(pill_a.x, pill_z.y + px(2)));
        ImGui::SetNextWindowSizeConstraints(ImVec2(pill_w, 0), ImVec2(pill_w, px(340)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(40, 40, 42, 250));
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(63, 63, 64, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(72, 72, 74, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(82, 82, 84, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(6), px(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
        if (ImGui::BeginPopup("##deck-list")) {
            for (int i = 0; i < count; ++i) {
                const auto& item = decks[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, there(item) ? skate_theme::white : skate_theme::grey_text);
                const ImVec2 row = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##deck", i == browse, 0, ImVec2(0, ImGui::GetFrameHeight()))) {
                    // The way round that is shorter decides which way the names swipe.
                    const int ahead = static_cast<int>(wrap(i - browse));
                    go(i, ahead <= count / 2 ? 1 : -1);
                }
                auto* list = ImGui::GetWindowDrawList();
                const float row_h = ImGui::GetFrameHeight();
                ImGui::PushFont(menu.bold);
                const float text_y = row.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
                const auto item_name = name_of(item);
                list->AddText(ImVec2(row.x + px(12), text_y), there(item) ? skate_theme::white : skate_theme::grey_text, item_name.c_str());
                if (item == in_play)
                    asterisk(list, ImVec2(row.x + px(12) + ImGui::CalcTextSize(item_name.c_str()).x + mark_w * 0.62f, row.y + row_h * 0.5f),
                             skate_theme::blue);
                if (!there(item)) {
                    const float w = ImGui::CalcTextSize("NOT INSTALLED").x;
                    list->AddText(ImVec2(row.x + ImGui::GetContentRegionAvail().x - w - px(8), text_y), skate_theme::warning, "NOT INSTALLED");
                }
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);

        // The model.
        const ImVec2 view_a(at.x + pad, at.y + pad + pill_h + gap + side_h + gap), view_z(end.x - pad, view_a.y + view_h);
        deck_preview::draw(draw, view_a, view_z, deck, panel);
        const float inset = px(10);
        if (!there(deck)) {
            ImGui::PushFont(menu.bold);
            draw->AddText(ImVec2(view_a.x + inset, view_a.y + inset), skate_theme::warning, "NOT INSTALLED");
            ImGui::PopFont();
        }
        if (there(deck) && deck_preview::showing_stand_in())
            draw->AddText(ImVec2(view_a.x + inset, view_z.y - inset - ImGui::GetTextLineHeight()), IM_COL32(110, 112, 118, 255),
                          "No preview.obj in this deck's folder: a plain deck stands in.");
        if (!swiping && !listing && ImGui::IsMouseHoveringRect(at, end) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
        }
        if (step) go(browse + step, step);
        ImGui::SetCursorScreenPos(ImVec2(at.x, end.y + px(8)));
        ImGui::Dummy(ImVec2(full, 0));

        // Putting it in the game: the level loads again.
        const auto& shown = decks[static_cast<std::size_t>(browse)];
        if (shown == in_play) {
            primary_button(menu, "ACTIVE", false);
        } else if (!there(shown)) {
            primary_button(menu, "NOT INSTALLED", false);
        } else if (primary_button(menu, ("USE " + name_of(shown) + "  (the level loads again)").c_str(),
                                  !busy && callbacks.queue_console_command)) {
            send_console(menu, callbacks, "boardbreak deck " + shown);
        }
    }
    if (busy) note("Applying mods...");
    else if (!in_play.empty() && chosen != in_play && there(chosen))
        note((label(chosen) + " is chosen and shows once the level has loaded again.").c_str());
    note("The deck every board in the game has, and the one that breaks. Going through the decks loads nothing; "
         "putting one in the game loads the level again. Decks that are not installed are greyed out.");
    end_card();
}
}
namespace {
std::filesystem::path game_directory() {
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) throw std::runtime_error("Cannot locate the game directory.");
    return std::filesystem::path(std::wstring(executable.data(), length)).parent_path();
}
// What a mod brings, as the native Mods page words it.
std::string mod_contents(const mods::Mod& mod) {
    std::string text = mod.provides_layout && mod.provides_levels ? "Maps + game data"
                     : mod.provides_levels ? "Maps" : mod.provides_layout ? "Game data" : "";
    if (!mod.park_maps.empty()) text += text.empty() ? "Parks" : " + parks";
    return text.empty() ? "Nothing to load" : text;
}
// Every mod folder and what mods.json says about it, re-read every 1.5 s while
// the card is open, so a folder added or an edit made outside the game shows
// up. Enabling or disabling saves mods.json; "Apply now" merges the change in
// while the game runs (it takes effect at the next level load), including a
// mod folder added since launch.
void draw_installed_mods(SkateMenu& menu) {
    static auto* scans = new FolderRead<mods::ModList>; // process lifetime, as the threads that fill it
    static std::string action_error;
    const auto scanned = scans->get([] { return mods::scan_mods(mods::engine_data_root()); }, 1500);
    if (!scanned) {
        menu::note("Reading the Mods folder...");
        return;
    }
    const auto& list = *scanned;
    const auto& launch = mods::catalog();
    const auto applied = live_mods::applied_mods();
    // ReSkate's own packs (builtin_packs.h) live outside the Mods folder's list, always on. Without
    // this, the ones in effect read as mods whose folder was deleted. (Official mods are mods like
    // any other, in the list.)
    const auto kept = [&](const std::string& name) {
        const auto pack = mods::board_break_pack_of(name);
        return (pack.empty() ? name : pack).starts_with("reskate-builtin-");
    };
    // A Board Break deck pack is in effect when one of the decks inside it is (mod_catalog.h).
    const auto in_effect = [&](const std::string& name) {
        return std::ranges::any_of(applied, [&](const std::string& other) {
            return _stricmp(other.c_str(), name.c_str()) == 0 || _stricmp(mods::board_break_pack_of(other).c_str(), name.c_str()) == 0;
        });
    };
    const auto excluded = [&](const std::string& name) {
        return std::ranges::any_of(launch.excluded, [&](const mods::Mod& mod) { return _stricmp(mod.name.c_str(), name.c_str()) == 0; });
    };
    // Left out for a conflict: the mod it lost to (mod_conflicts.h), else empty.
    const auto lost_to = [&](const std::string& name) {
        const auto found = std::ranges::find_if(launch.excluded, [&](const mods::Mod& mod) { return _stricmp(mod.name.c_str(), name.c_str()) == 0; });
        return found == launch.excluded.end() ? std::string{} : mods::conflict_winner(*found);
    };
    const auto title_of = [&](const std::string& name) {
        for (const auto& entry : list.entries)
            if (_stricmp(entry.mod.name.c_str(), name.c_str()) == 0) return entry.mod.title.empty() ? name : entry.mod.title;
        return name;
    };
    // Board Break's other decks and other mods that bring a deck model: enabled, not loaded, and
    // nothing about them waits to be applied.
    const auto aside = mods::set_aside_for_board_break();
    const auto set_aside = [&](const mods::ModEntry& entry) {
        return entry.enabled && !in_effect(entry.mod.name) &&
               std::ranges::any_of(aside, [&](const std::string& other) { return _stricmp(other.c_str(), entry.mod.name.c_str()) == 0; });
    };
    std::size_t loaded{}, pending{};
    for (const auto& entry : list.entries) {
        const bool effect = in_effect(entry.mod.name);
        if (effect) ++loaded;
        if (entry.enabled != effect && !excluded(entry.mod.name) && !set_aside(entry)) ++pending;
    }
    // Mods still in effect whose folder has been deleted: applying unloads them.
    std::vector<std::string> removed;
    for (const auto& name : applied)
        if (mods::board_break_pack_of(name).empty() && !kept(name) &&
            std::ranges::none_of(list.entries, [&](const mods::ModEntry& entry) { return _stricmp(entry.mod.name.c_str(), name.c_str()) == 0; }))
            removed.push_back(name);
    pending += removed.size();
    loaded += removed.size();
    std::string count = std::to_string(loaded) + " loaded";
    const auto conflicted = static_cast<std::size_t>(std::ranges::count_if(launch.excluded, [](const mods::Mod& mod) {
        return !mods::conflict_winner(mod).empty(); }));
    if (launch.excluded.size() > conflicted) count += ", " + std::to_string(launch.excluded.size() - conflicted) + " not loaded";
    if (conflicted) count += ", " + std::to_string(conflicted) + " left out";
    menu::begin_card(menu, "installed-mods", "INSTALLED MODS", count.c_str());

    // Unapplied changes first, with the button that applies them.
    if (live_mods::busy()) {
        menu::note("Applying mods...");
    } else if (pending) {
        menu::warn((std::to_string(pending) + " change(s) not applied yet.").c_str());
        // One button, labelled by what the changes do to the level being played:
        // maps only just need applying; anything the level shows needs it
        // reloaded; taking away the map being played sends the player to San Van.
        const auto effect = live_mods::preview(list);
        if (ImGui::Button(live_mods::apply_label(effect), ImVec2(-FLT_MIN, 0))) {
            action_error.clear();
            menu::feedback(menu, live_mods::apply(effect == live_mods::ApplyEffect::reload_level).c_str());
        }
    }
    if (const auto last = live_mods::status(); !last.empty()) menu::note(("Last apply: " + last).c_str());

    auto* draw = ImGui::GetWindowDrawList();
    // A mod's tile; the button on its right enables or disables it.
    const auto tile = [&](int index, const std::string& title, const std::string& byline, const char* status,
                          ImU32 colour, const std::string& contents, const std::string& hover, const char* action) {
        bool clicked{};
        ImGui::PushID(index);
        const float button_width = action ? menu::px(96) : 0.f;
        const auto top = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x - (action ? button_width + menu::px(8) : 0.f);
        const float height = menu::px(54), pad = menu::px(14);
        skate_theme::rough_rect(draw, top, ImVec2(top.x + width, top.y + height), skate_theme::tile_grey,
                                static_cast<unsigned>(index + 151), menu::px(1));
        float right = top.x + width - pad;
        const auto contents_size = menu.body->CalcTextSizeA(menu::px(14), FLT_MAX, 0, contents.c_str());
        right -= contents_size.x;
        draw->AddText(menu.body, menu::px(14), ImVec2(right, top.y + (height - contents_size.y) * .5f),
                      skate_theme::grey_text, contents.c_str());
        right -= menu::px(14);
        const float status_width = menu.bold->CalcTextSizeA(menu::px(12), FLT_MAX, 0, status).x + menu::px(14);
        right -= status_width;
        ImGui::SetCursorScreenPos(ImVec2(right, top.y + (height - ImGui::GetFrameHeight()) * .5f));
        menu::tag(menu, status, colour);
        right -= menu::px(14);
        ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + menu::px(8)));
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(right, top.y + height), true);
        ImGui::BeginGroup();
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
        ImGui::TextUnformatted(byline.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(top);
        ImGui::Dummy(ImVec2(width, height));
        if (!hover.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
            ImGui::TextUnformatted(hover.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (action) {
            ImGui::SameLine(0, menu::px(8));
            ImGui::SetCursorScreenPos(ImVec2(top.x + width + menu::px(8), top.y + (height - ImGui::GetFrameHeight()) * .5f));
            clicked = ImGui::Button(action, ImVec2(button_width, 0));
            ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + height + ImGui::GetStyle().ItemSpacing.y));
            ImGui::Dummy(ImVec2(0, 0));
        }
        ImGui::PopID();
        return clicked;
    };
    const auto byline = [](const mods::Mod& mod) {
        std::string text = mod.author.empty() ? "Unknown author" : "by " + mod.author;
        if (!mod.version.empty()) text += "  -  v" + mod.version;
        return text;
    };
    std::optional<std::size_t> toggle;
    int index{};
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        const auto& mod = entry.mod;
        const bool effect = in_effect(mod.name);
        const char* status = "DISABLED";
        ImU32 colour = skate_theme::tile_light;
        std::string hover = mod.description;
        if (const auto winner = lost_to(mod.name); !winner.empty()) {
            status = "LEFT OUT";
            hover = "Left out: it changes the same parts of the game as " + title_of(winner) + ", which is higher in "
                    "the load order, so only that one loads.\n\nChoose which loads in the launcher's Mod Manager.";
        } else if (excluded(mod.name)) {
            status = "NOT LOADED";
            colour = skate_theme::danger;
            hover = "Not loaded: it could not be merged cleanly, so none of it is used.\n\n"
                    "Reinstall the whole mod folder, or rebuild it with a current ReSkate Studio.";
        } else if (set_aside(entry)) {
            status = "SET ASIDE";
            hover = "Set aside while a Board Break deck is the one in the game: the game has one deck model, so one "
                    "mod's deck loads at a time. Pick another deck under BOARD BREAK DECK, or disable Board Break's decks.";
        } else if (entry.enabled && effect) {
            status = "LOADED";
            colour = skate_theme::good;
        } else if (entry.enabled) {
            status = "APPLY TO LOAD";
            colour = skate_theme::danger;
        } else if (effect) {
            status = "APPLY TO UNLOAD";
            colour = skate_theme::danger;
        }
        if (tile(index++, mod.title.empty() ? mod.name : mod.title, byline(mod), status, colour, mod_contents(mod), hover,
                 entry.enabled ? "Disable" : "Enable"))
            toggle = i;
    }
    for (const auto& name : removed)
        tile(index++, name, "Folder deleted", "APPLY TO UNLOAD", skate_theme::danger, "",
             "This mod's folder is gone, but it stays loaded until you apply.", nullptr);
    if (toggle && list.issue.empty()) {
        try {
            auto entries = list.entries;
            entries[*toggle].enabled = !entries[*toggle].enabled;
            mods::save_mod_order(list.root, entries);
            auto shown = list;
            shown.entries = std::move(entries);
            scans->set(std::move(shown));
            scans->again();
            action_error.clear();
        } catch (const std::exception& error) { action_error = error.what(); }
    }
    if (!index) menu::note(list.present ? "No mods installed yet." : "No Mods folder yet. Create one next to Skate.exe.");
    if (!list.issue.empty()) menu::warn(("mods.json problem: " + list.issue).c_str());
    if (!action_error.empty()) menu::warn(("Could not save mods.json: " + action_error).c_str());
    menu::note("Order mods in the launcher's Mods panel. Maps, loading screens and cosmetics change at the next level load; "
               "other changes to game settings or the UI take effect after a restart.");
    menu::end_card();
}
void draw_custom_scripts(SkateMenu& menu) {
    menu::begin_card(menu, "custom-scripts", "CUSTOM SCRIPTS");
    menu::note("Drop .lua files or folders with init.lua into scripts/Custom/. Scripts and saved changes load on the next launch.");
    if (ImGui::Button("Refresh script list", ImVec2(-FLT_MIN, 0))) menu.custom_scripts_scanned = false;
    if (!menu.custom_scripts_scanned) {
        menu.custom_scripts_scanned = true;
        menu.custom_script_rows.clear();
        menu.custom_script_error.clear();
        try { menu.custom_script_rows = custom_scripts::discover(game_directory()); }
        catch (const std::exception& error) { menu.custom_script_error = error.what(); }
    }
    if (!menu.loose_files_saved) menu::warn("Turn on loose Lua / config files in the launcher to load custom scripts.");
    menu::note(custom_scripts::status("").c_str());
    if (!menu.custom_script_error.empty()) menu::warn(("Script settings error: " + menu.custom_script_error).c_str());
    if (menu.custom_script_rows.empty()) {
        menu::note("No custom scripts found. Names starting with _ or . are skipped.");
        menu::end_card();
        return;
    }
    std::optional<std::pair<std::size_t, std::size_t>> move;
    if (ImGui::BeginTable("custom_scripts", 4, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, menu::px(32));
        ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, menu::px(100));
        ImGui::TableSetupColumn("This session", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < menu.custom_script_rows.size(); ++index) {
            auto& script = menu.custom_script_rows[index];
            ImGui::PushID(script.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool enabled = script.enabled;
            if (ImGui::Checkbox("##enabled", &enabled)) {
                try {
                    custom_scripts::save_enabled(game_directory(), script.id, enabled);
                    script.enabled = enabled;
                    menu.custom_script_error.clear();
                    menu::feedback(menu, "Script selection saved for the next launch.");
                } catch (const std::exception& error) { menu.custom_script_error = error.what(); }
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", script.id.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(index == 0);
            if (ImGui::SmallButton("Up")) move = {{index, index - 1}};
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(index + 1 == menu.custom_script_rows.size());
            if (ImGui::SmallButton("Down")) move = {{index, index + 1}};
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", custom_scripts::status(script.id).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move) {
        try {
            auto ordered = menu.custom_script_rows;
            std::swap(ordered[move->first], ordered[move->second]);
            custom_scripts::save_order(game_directory(), ordered);
            menu.custom_script_rows = std::move(ordered);
            menu.custom_script_error.clear();
            menu::feedback(menu, "Script order saved for the next launch.");
        } catch (const std::exception& error) { menu.custom_script_error = error.what(); }
    }
    menu::end_card();
}
}
void draw_modding_menu(SkateMenu& menu, int tab, const Model&, const CallbacksV3&) {
    // Loose files are switched in the launcher; custom scripts only need to know
    // whether they are on.
    if (!menu.loose_files_settings_loaded) {
        menu.loose_files_settings_loaded = true;
        try { menu.loose_files_saved = initfs::loose_files_preference(game_directory()); }
        catch (const std::exception&) {}
    }
    if (tab == 1) draw_custom_scripts(menu);
    else draw_installed_mods(menu);
}
}
