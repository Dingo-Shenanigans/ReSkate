#include "Engine/Core/Platform/launcher_support.h"
#include "skate_menu_internal.h"
#include "skate_menu.h"
#include "modding_menu.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <initializer_list>

namespace dingosdk::overlay {
using namespace menu;
namespace menu {
using namespace theme;

// Set once per frame in draw_skate_menu, then read through px() while the menu
// and its pages draw.
namespace { float ui_scale = default_menu_scale; }
float px(float value) { return value * ui_scale; }

// Heavy uppercase header over a thin rule, like the titles on skate.'s HUB tiles.
void section(SkateMenu& menu, const char* text) {
    ImGui::Spacing();
    ImGui::PushFont(menu.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const auto at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + px(2)), skate_theme::tile_light);
    draw->AddRectFilled(at, ImVec2(at.x + px(34), at.y + px(3)), blue);
    ImGui::Dummy(ImVec2(0, px(8)));
}
float page_body_height(const SkateMenu& menu) {
    const bool feedback_shown = !menu.feedback.empty() && ImGui::GetTime() < menu.feedback_until;
    return std::max(80.0f, ImGui::GetContentRegionAvail().y - (feedback_shown ? px(48) : 0.0f));
}
void feedback(SkateMenu& menu, const char* text) {
    // An accepted request has nothing to say; only problems and results show.
    if (!text || !*text) return;
    menu.feedback = text;
    menu.feedback_until = ImGui::GetTime() + 6.0;
}
void send_console(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& command) {
    if (!callbacks.queue_console_command) {
        feedback(menu, "The game command dispatcher is unavailable.");
        return;
    }
    std::array<char, 512> result{};
    callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
    result.back() = 0;
    feedback(menu, result.data());
}
void debug_request(SkateMenu& menu, const CallbacksV3& callbacks, DebugRequest request) {
    std::array<char, 512> result{};
    if (!callbacks.queue_debug) return;
    const bool queued = callbacks.queue_debug(callbacks.user, request, result.data(), result.size());
    result.back() = '\0';
    feedback(menu, result[0] ? result.data() : (queued ? "" : "Change unavailable."));
}

bool toggle_row(SkateMenu& menu, const char* label, const char* hint, bool& value, bool available,
                const char* unavailable) {
    ImGui::PushID(label);
    ImGui::BeginDisabled(!available);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = px(36);
    const bool clicked = ImGui::Selectable("##toggle", false, ImGuiSelectableFlags_None, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    if (clicked) value = !value;
    // A dark tile per option with an ON/OFF block, like the game's option rows.
    auto* draw = ImGui::GetWindowDrawList();
    const unsigned seed = static_cast<unsigned>(ImGui::GetItemID());
    skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height - px(3)),
        hovered ? skate_theme::tile_light : skate_theme::tile_grey, seed, ui_scale);
    if (!available) skate_theme::scribble(draw, at, ImVec2(at.x + width * 0.6f, at.y + height - px(3)), seed, ui_scale * 0.5f);
    if (focused) draw->AddRect(at, ImVec2(at.x + width, at.y + height - px(3)), paper, 0, 0, px(2));
    const auto color = available ? paper : muted;
    const float block = px(58);
    draw->PushClipRect(at, ImVec2(at.x + width - block - px(12), at.y + height), true);
    draw->AddText(menu.bold, px(16), ImVec2(at.x + px(12), at.y + px(8)), color, label);
    draw->PopClipRect();
    const char* state = available ? (value ? "ON" : "OFF") : unavailable;
    const ImVec2 box(at.x + width - block - px(6), at.y + px(6));
    const bool on = available && value;
    draw->AddRectFilled(box, ImVec2(box.x + block, at.y + height - px(9)), on ? blue : IM_COL32(0, 0, 0, 90));
    const auto text = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, state);
    draw->AddText(menu.bold, px(14), ImVec2(box.x + (block - text.x) * 0.5f, box.y + (height - px(15) - text.y) * 0.5f),
        on ? skate_theme::black : muted, state);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hint);
    ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

namespace {
// The label column of field() and info(), and the width below which they stack.
float label_column() { return px(180); }
bool narrow() { return ImGui::GetContentRegionAvail().x < px(440); }
}
void begin_card(SkateMenu& menu, const char* id, const char* title, const char* subtitle) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, skate_theme::tile);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    // A card never scrolls itself; the wheel keeps scrolling the page around it.
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const auto at = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + px(4), at.y + ImGui::GetWindowHeight()), blue);
    if (title) {
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (subtitle) {
            ImGui::SameLine(0, px(10));
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted(subtitle);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, px(4)));
    }
}
void end_card() {
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, px(8)));
}
void field(SkateMenu&, const char* label, const char* tooltip) {
    const float start = ImGui::GetCursorPosX();
    const bool stacked = narrow();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    if (!stacked) ImGui::SameLine(start + label_column());
    ImGui::SetNextItemWidth(-FLT_MIN);
}
void info(SkateMenu&, const char* label, const std::string& value) {
    const float start = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (!narrow()) ImGui::SameLine(start + label_column());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
}
void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
void warn(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::warning);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
bool choice(SkateMenu& menu, const char* id, int& selected, std::initializer_list<const char*> options, bool enabled,
            float total) {
    ImGui::PushID(id);
    const auto count = static_cast<int>(options.size());
    const float gap = px(4);
    const float width = ((total > 0 ? total : ImGui::GetContentRegionAvail().x) - gap * (count - 1)) / count;
    const float height = px(32);
    auto* draw = ImGui::GetWindowDrawList();
    bool changed{};
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0, gap);
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::InvisibleButton("##choice", ImVec2(width, height)) && selected != i) {
            selected = i;
            changed = true;
        }
        ImGui::EndDisabled();
        const bool hovered = enabled && ImGui::IsItemHovered();
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            draw->AddRect(at, ImVec2(at.x + width, at.y + height), paper, 0, 0, px(2));
        ImGui::PopID();
        const bool on = selected == i;
        const ImU32 fill = on ? (enabled ? blue : IM_COL32(1, 131, 255, 120))
                         : hovered ? skate_theme::tile_light : skate_theme::tile_grey;
        skate_theme::rough_rect(draw, at, ImVec2(at.x + width, at.y + height), fill, static_cast<unsigned>(i + 29), ui_scale);
        const char* label = options.begin()[i];
        const auto size = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, label);
        draw->PushClipRect(at, ImVec2(at.x + width, at.y + height), true);
        draw->AddText(menu.bold, px(14), ImVec2(at.x + std::max(px(6), (width - size.x) * .5f), at.y + (height - size.y) * .5f),
            on ? skate_theme::black : enabled ? paper : muted, label);
        draw->PopClipRect();
    }
    ImGui::PopID();
    return changed;
}
void tag(SkateMenu& menu, const char* text, ImU32 colour) {
    const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, text);
    const ImVec2 pad(px(7), px(3));
    const auto at = ImGui::GetCursorScreenPos();
    // Centred on a widget row, so it lines up with text beside it that was
    // aligned to frame padding (table rows, buttons).
    const float line = ImGui::GetFrameHeight();
    const float top = at.y + std::max(0.0f, (line - size.y - pad.y * 2) * .5f);
    ImGui::Dummy(ImVec2(size.x + pad.x * 2, std::max(line, size.y + pad.y * 2)));
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + size.x + pad.x * 2, top + size.y + pad.y * 2), colour);
    draw->AddText(menu.bold, px(12), ImVec2(at.x + pad.x, top + pad.y), skate_theme::black, text);
}
bool primary_button(SkateMenu& menu, const char* label, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    skate_theme::push_primary_button();
    ImGui::PushFont(menu.bold);
    const bool clicked = ImGui::Button(label, ImVec2(-FLT_MIN, px(40)));
    ImGui::PopFont();
    skate_theme::pop_primary_button();
    ImGui::EndDisabled();
    return clicked;
}

void category_tabs(SkateMenu& menu, int& selected, std::initializer_list<const char*> tabs, const char* id) {
    // A row of tiles like skate.'s tab bar: the selected tab is blue with black text.
    selected = std::clamp(selected, 0, static_cast<int>(tabs.size()) - 1);
    ImGui::PushID(id);
    const float gap = px(6);
    const float tab_width = (ImGui::GetContentRegionAvail().x - gap * (tabs.size() - 1)) / tabs.size();
    const float height = px(34);
    auto* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (i) ImGui::SameLine(0, gap);
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_width, height)) && selected != i) {
            selected = i;
            menu.feedback.clear();
        }
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            draw->AddRect(at, ImVec2(at.x + tab_width, at.y + height), paper, 0, 0, px(2));
        ImGui::PopID();
        const bool on = selected == i;
        skate_theme::rough_rect(draw, at, ImVec2(at.x + tab_width, at.y + height),
            on ? blue : hovered ? skate_theme::tile_light : skate_theme::tile_grey, static_cast<unsigned>(i + 11), ui_scale);
        const char* label = tabs.begin()[i];
        const auto size = menu.bold->CalcTextSizeA(px(15), FLT_MAX, 0, label);
        draw->PushClipRect(at, ImVec2(at.x + tab_width, at.y + height), true);
        draw->AddText(menu.bold, px(15), ImVec2(at.x + std::max(px(6), (tab_width - size.x) * .5f), at.y + (height - size.y) * .5f),
            on ? skate_theme::black : paper, label);
        draw->PopClipRect();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, px(6)));
}
}

namespace {
// The menu fonts are built into ReSkate.dll (External/fonts, shared with the
// launcher); Windows fonts are the fallback.
ImFont* embedded_font(const wchar_t* name, float size, const ImWchar* ranges) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&embedded_font), &module)) return nullptr;
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // the resource lives as long as the DLL
    config.OversampleH = size >= 32 ? 1 : 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(SizeofResource(module, resource)), size,
        &config, ranges);
}

enum Page { map, world, build, skater, training, multiplayer, progress, mods, settings, special, developer, page_count };
constexpr std::array<const char*, page_count> page_names{
    "MAP", "WORLD", "BUILD", "SKATER", "TRAINER", "MULTIPLAYER", "PROGRESS", "MODS", "SETTINGS", "SPECIAL", "DEVELOPER"};
constexpr std::array<const char*, page_count> page_subtitles{
    "Pick your spot.", "Set the vibe.", "Make the park yours.", "Ride it your way.", "Tune it. Drill it. Measure it.",
    "Bring your crew.", "Pick up where you want.", "Bring your own.", "Your controls, your screen.", "Not everyone gets this page.",
    "Under the hood."};
}

void load_skate_fonts(SkateMenu& menu) {
    auto* atlas = ImGui::GetIO().Fonts;
    atlas->TexGlyphPadding = 2;
    // Latin, Cyrillic and general punctuation cover player and lobby names.
    static const ImWchar ranges[]{0x0020, 0x024F, 0x0400, 0x052F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0x2190, 0x21FF, 0};
    std::array<char, MAX_PATH> windows{};
    const auto length = GetWindowsDirectoryA(windows.data(), static_cast<UINT>(windows.size()));
    const auto font = [&](const wchar_t* resource, const char* filename, float size) -> ImFont* {
        if (resource)
            if (auto* loaded = embedded_font(resource, size, ranges)) return loaded;
        if (length && length < windows.size()) {
            const auto path = std::string(windows.data()) + "\\Fonts\\" + filename;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                if (auto* loaded = atlas->AddFontFromFileTTF(path.c_str(), size)) return loaded;
        }
        ImFontConfig config;
        config.SizePixels = size;
        return atlas->AddFontDefault(&config);
    };
    // Every role has a Windows font and then ImGui's own font as fallbacks.
    atlas->AddFontDefault();
    menu.body = font(L"FONT_BODY", "arial.ttf", 17);
    menu.bold = font(L"FONT_HEADING", "arialbd.ttf", 17);
    menu.heading = font(L"FONT_HEADING", "arialbd.ttf", 22);
    menu.title = font(L"FONT_BRUSH", "arialbi.ttf", 44);
    menu.mono = font(nullptr, "consola.ttf", 15);
}

void draw_skate_menu(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks,
                     bool& visible) {
    const bool on_controls = menu.page == settings && menu.settings_tab == 0;
    const bool on_voice = menu.page == multiplayer && menu.multiplayer_tab == multiplayer_voice_tab;
    if (!visible || !on_controls || menu.last_menu_frame + 1 != ImGui::GetFrameCount()) menu.recording_bind = 0;
    if (!visible || !on_voice || menu.last_menu_frame + 1 != ImGui::GetFrameCount()) menu.voice_bind_capture = 0;
    // Esc closes the menu, unless it is busy with something else: typing in a
    // field, an open popup, or recording a key or controller binding.
    if (visible && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput &&
        !menu.recording_bind && !menu.voice_bind_capture &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        visible = false;
    menu.last_menu_frame = ImGui::GetFrameCount();
    auto& io = ImGui::GetIO();
    ui_scale = std::clamp(model.menu_scale, min_menu_scale, max_menu_scale);
    const auto restore_font_scale = io.FontGlobalScale;
    // Widget text follows the global font scale; the pixel layout below follows px().
    io.FontGlobalScale = ui_scale;
    const ImVec2 maximum(std::max(px(400.0f), io.DisplaySize.x - 24), std::max(px(320.0f), io.DisplaySize.y - 24));
    ImGui::SetNextWindowSize(ImVec2(std::min(px(1000.0f), maximum.x), std::min(px(700.0f), maximum.y)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_FirstUseEver, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(px(680.0f), maximum.x), std::min(px(480.0f), maximum.y)), maximum);
    ImGui::PushFont(menu.body);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10), px(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0);
    const int colours = skate_theme::push_widget_colours();
    if (ImGui::Begin("ReSkate###skate-menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        const auto size = ImGui::GetWindowSize();
        const auto current = ImGui::GetWindowPos();
        const ImVec2 bounded(std::clamp(current.x, 0.0f, std::max(0.0f, io.DisplaySize.x - size.x)),
            std::clamp(current.y, 0.0f, std::max(0.0f, io.DisplaySize.y - size.y)));
        if (bounded.x != current.x || bounded.y != current.y) ImGui::SetWindowPos(bounded);
        const auto origin = ImGui::GetWindowPos();
        auto* draw = ImGui::GetWindowDrawList();
        const float sidebar = px(190.0f);

        // Sidebar: brushed wordmark, then one tile per page like the HUB's tab icons.
        draw->AddRectFilled(origin, ImVec2(origin.x + sidebar, origin.y + size.y), IM_COL32(10, 10, 11, 250));
        {
            const int start = draw->VtxBuffer.Size;
            const ImVec2 at(origin.x + px(16), origin.y + px(14));
            draw->AddText(menu.title, px(40), ImVec2(at.x + px(2), at.y + px(3)), IM_COL32(0, 0, 0, 160), "RESKATE");
            draw->AddText(menu.title, px(40), at, paper, "RESKATE");
            const auto extent = menu.title->CalcTextSizeA(px(40), FLT_MAX, 0, "RESKATE");
            skate_theme::rotate_since(draw, start, -4.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
        }
        draw->AddText(menu.body, px(12), ImVec2(origin.x + px(18), origin.y + px(66)), muted, "YOUR SESSION. YOUR RULES.");
        if (trainer_page_wanted()) menu.page = training;
        menu.page = std::clamp(menu.page, 0, static_cast<int>(page_count) - 1);
        // Developer (the network view) is hidden for now; offline there is no Multiplayer either.
        // Special is only there for a player the backend lists.
        const auto hidden = [&](int page) {
            return page == developer || (model.steam_offline && page == multiplayer) ||
                   (page == special && model.multiplayer.identity_tag.empty());
        };
        if (hidden(menu.page)) menu.page = map;
        ImGui::SetCursorPos(ImVec2(px(10), px(96)));
        ImGui::BeginChild("navigation", ImVec2(sidebar - px(20), size.y - px(140)), ImGuiChildFlags_None);
        const float tab_height = px(40);
        for (int i = 0; i < static_cast<int>(page_count); ++i) {
            if (hidden(i)) continue;
            const auto at = ImGui::GetCursorScreenPos();
            const bool selected = menu.page == i;
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##page", ImVec2(sidebar - px(20), tab_height)) && !selected) {
                menu.page = i;
                menu.recording_bind = 0;
                menu.voice_bind_capture = 0;
                menu.feedback.clear();
            }
            const bool hovered = ImGui::IsItemHovered();
            auto* nav = ImGui::GetWindowDrawList();
            const ImVec2 end(at.x + sidebar - px(20), at.y + tab_height - px(4));
            if (selected || hovered)
                skate_theme::rough_rect(nav, at, end, selected ? blue : skate_theme::tile_grey, static_cast<unsigned>(i + 3), ui_scale);
            if (ImGui::IsItemFocused() && io.NavVisible) nav->AddRect(at, end, paper, 0, 0, px(2));
            nav->AddText(menu.bold, px(16), ImVec2(at.x + px(12), at.y + (tab_height - px(4) - px(16)) * .5f),
                selected ? skate_theme::black : paper, page_names[i]);
            ImGui::PopID();
        }
        ImGui::EndChild();
        // Keycap-style hint, like the game's "Esc Back" prompt.
        {
            static const auto label = dingosdk::launcher::key_cap(dingosdk::launcher::overlay_keys().menu);
            keycap(draw, menu.bold, ui_scale, ImVec2(origin.x + px(16), origin.y + size.y - px(34)), label.c_str(), "Close");
        }

        // Page header: brushed title like the game's "HUB", with its subtitle.
        {
            const int start = draw->VtxBuffer.Size;
            const ImVec2 at(origin.x + sidebar + px(22), origin.y + px(12));
            draw->AddText(menu.title, px(44), ImVec2(at.x + px(2), at.y + px(3)), IM_COL32(0, 0, 0, 160), page_names[menu.page]);
            draw->AddText(menu.title, px(44), at, paper, page_names[menu.page]);
            const auto extent = menu.title->CalcTextSizeA(px(44), FLT_MAX, 0, page_names[menu.page]);
            skate_theme::rotate_since(draw, start, -3.0f, ImVec2(at.x + extent.x * .5f, at.y + extent.y * .5f));
            draw->AddText(menu.body, px(14), ImVec2(at.x + extent.x + px(18), at.y + px(22)), muted, page_subtitles[menu.page]);
        }
        ImGui::SetCursorPos(ImVec2(size.x - px(44), px(16)));
        if (ImGui::Button("X", ImVec2(px(28), px(28)))) visible = false;
        ImGui::SetCursorPos(ImVec2(sidebar + px(20), px(76)));
        ImGui::BeginChild("page", ImVec2(size.x - sidebar - px(40), size.y - px(96)), ImGuiChildFlags_None);
        switch (menu.page) {
        case map: map_page(menu, model, callbacks); break;
        case world: world_page(menu, model, callbacks); break;
        case build: build_page(menu, model, callbacks); break;
        case skater: skater_page(menu, model, callbacks); break;
        case training: trainer_page(menu, model, callbacks); break;
        case multiplayer: multiplayer_page(menu, model, callbacks); break;
        case progress: progression_page(menu, model, callbacks); break;
        case mods:
            // The SCRIPTS tab (custom Lua scripts, modding_menu.cpp) is not offered for now:
            // nothing uses them. The page is the installed mods.
            menu.mods_tab = 0;
            ImGui::PushID(menu.mods_tab);
            ImGui::BeginChild("mods-tab", ImVec2(0, page_body_height(menu)));
            draw_modding_menu(menu, menu.mods_tab);
            ImGui::EndChild();
            ImGui::PopID();
            break;
        case settings: settings_page(menu, model, callbacks); break;
        case special: special_page(menu, model, callbacks); break;
        case developer: developer_page(menu, model, callbacks); break;
        }
        if (!menu.feedback.empty() && ImGui::GetTime() < menu.feedback_until) {
            // The feedback line sits in a dark strip with a blue edge, like the game's toasts.
            ImGui::Spacing();
            const auto& result = (menu.page == skater || (menu.page == settings && menu.settings_tab == 1)) &&
                !model.debug.status.empty() && ImGui::GetTime() > menu.feedback_until - 5.75 ? model.debug.status : menu.feedback;
            const auto at = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float text_height = ImGui::CalcTextSize(result.c_str(), nullptr, false, width - px(24)).y;
            auto* page = ImGui::GetWindowDrawList();
            page->AddRectFilled(at, ImVec2(at.x + width, at.y + text_height + px(12)), skate_theme::tile);
            page->AddRectFilled(at, ImVec2(at.x + px(4), at.y + text_height + px(12)), blue);
            ImGui::SetCursorScreenPos(ImVec2(at.x + px(14), at.y + px(6)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - px(24));
            ImGui::TextUnformatted(result.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(9);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
}
}

