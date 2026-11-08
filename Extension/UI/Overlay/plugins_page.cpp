#include "skate_menu_internal.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "Extension/Plugins/plugin_ui.h"
#include "Extension/UI/skate_theme.h"

#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

// The PLUGINS section: a page for each plugin that has one (reskate.menu.page, or panels only), drawn
// with the menu's own cards, tabs and rows. A plugin builds its page on the client thread ten times a
// second while it shows (plugin_ui.h), and what the player does goes back to it as actions.
namespace dingosdk::overlay::menu {
namespace {
// What the player last set on a widget, shown until the plugin's next build agrees, or a second
// passes: the plugin's answer comes a few frames later.
struct Edit {
    bool on{};
    float value{};
    int chosen{};
    double at{};
    std::string text;
    std::array<float, 3> colour{};
};
std::map<std::string, Edit>& edits() {
    static auto* value = new std::map<std::string, Edit>;
    return *value;
}
// A text field's characters while it is typed in (ImGui edits them in place), by widget; and the one
// being typed in, which keeps what the player types over what the plugin last sent.
std::map<std::string, std::array<char, 201>>& text_buffers() {
    static auto* value = new std::map<std::string, std::array<char, 201>>;
    return *value;
}
std::string& typing_in() {
    static auto* value = new std::string;
    return *value;
}
// Render thread: the pages as last published, held while drawn.
std::shared_ptr<const std::vector<plugin_ui::PageEntry>>& held_pages() {
    static auto* value = new std::shared_ptr<const std::vector<plugin_ui::PageEntry>>;
    return *value;
}

// One page or panel's widgets. A card opens with ui:card and closes at the next card or the end; a
// panel is a card of its own, titled with its title.
void draw_widgets(SkateMenu& menu, const plugin_ui::Panel& panel, double now) {
    bool in_card = false;
    if (!panel.page) {
        begin_card(menu, "plugin-panel", panel.title.c_str());
        in_card = true;
    }
    int item = 0;
    bool folded = false; // in a section the player folded away: its widgets are not drawn
    for (const auto& w : panel.widgets) {
        // A card opens here and closes at the next one: outside any widget's own ID, each its own.
        if (w.kind == plugin_ui::Widget::Kind::card) {
            if (in_card) end_card();
            const auto id = "plugin-card-" + std::to_string(item++);
            begin_card(menu, id.c_str(), w.text.c_str(), w.help.empty() ? nullptr : w.help.c_str());
            in_card = true;
            folded = false;
            continue;
        }
        // A section is a card that folds (fold_card keeps it open or not by its id); the plugin hears
        // when the player opens or folds it, and its `on` seeds it.
        if (w.kind == plugin_ui::Widget::Kind::section) {
            if (in_card) end_card();
            const auto id = "plugin-section\n" + panel.plugin + '\n' + panel.title + '\n' + w.id;
            ++item;
            const bool open = fold_card(menu, id.c_str(), w.text.c_str(), w.help.empty() ? nullptr : w.help.c_str(), w.on);
            in_card = true;
            folded = !open;
            auto& all = edits();
            auto edit = all.find(id);
            if (edit != all.end() && edit->second.on == w.on) {
                all.erase(edit); // the plugin has it now
                edit = all.end();
            }
            const bool sent = edit != all.end() && edit->second.on == open;
            if (open != w.on && !sent) {
                plugin_ui::Action action{panel.plugin, panel.title, w.id};
                action.on = open;
                plugin_ui::act(std::move(action));
                all[id] = Edit{open, 0, 0, now};
            }
            continue;
        }
        if (folded) continue;
        ImGui::PushID(item++);
        const auto key = panel.plugin + '\n' + panel.title + '\n' + w.id;
        auto& all = edits();
        const auto edit = all.find(key);
        const auto send = [&](plugin_ui::Action action, Edit remembered) {
            action.plugin = panel.plugin;
            action.panel = panel.title;
            action.widget = w.id;
            plugin_ui::act(std::move(action));
            remembered.at = now;
            all[key] = remembered;
        };
        const char* help = w.help.empty() ? "" : w.help.c_str();
        using Kind = plugin_ui::Widget::Kind;
        switch (w.kind) {
        case Kind::card: break; // above
        case Kind::tabs: {
            int selected = (edit != all.end() ? edit->second.chosen : w.chosen) - 1;
            if (edit != all.end() && edit->second.chosen == w.chosen) all.erase(edit);
            std::vector<const char*> names;
            for (const auto& name : w.choices) names.push_back(name.c_str());
            const int before = selected;
            category_tabs(menu, selected, names, "plugin-tabs");
            if (selected != before) {
                plugin_ui::Action action;
                action.chosen = selected + 1;
                send(action, {false, 0, selected + 1});
            }
            break;
        }
        case Kind::heading: section(menu, w.text.c_str()); break;
        case Kind::label: ImGui::TextWrapped("%s", w.text.c_str()); break;
        case Kind::note: note(w.text.c_str()); break;
        case Kind::warn: warn(w.text.c_str()); break;
        case Kind::info: info(menu, w.text.c_str(), w.help); break;
        case Kind::separator: ImGui::Separator(); break;
        case Kind::button:
            if (w.on ? primary_button(menu, w.text.c_str()) : button_row(menu, "##button", {{w.text.c_str(), true, false, w.help.empty() ? nullptr : help}}) == 0)
                send({}, {});
            break;
        case Kind::buttons: {
            const int pressed = button_grid(menu, "##buttons", w.choices, w.chosen - 1);
            if (pressed >= 0) {
                plugin_ui::Action action;
                action.chosen = pressed + 1;
                send(action, {false, 0, pressed + 1});
            }
            break;
        }
        case Kind::text: {
            // What the plugin holds, until the player types: then what they typed, while the field
            // has the keyboard and until the plugin's next build agrees.
            auto& buffer = text_buffers()[key];
            if (typing_in() != key) {
                const auto& shown = edit != all.end() ? edit->second.text : w.input;
                const auto length = std::min(shown.size(), buffer.size() - 1);
                std::copy_n(shown.data(), length, buffer.data());
                buffer[length] = '\0';
            }
            if (edit != all.end() && edit->second.text == w.input) all.erase(edit);
            field(menu, w.text.c_str());
            if (ImGui::InputTextWithHint("##text", w.help.c_str(), buffer.data(), buffer.size())) {
                plugin_ui::Action action;
                action.text = buffer.data();
                Edit remembered;
                remembered.text = action.text;
                send(action, remembered);
            }
            if (ImGui::IsItemActive()) typing_in() = key;
            else if (typing_in() == key) typing_in().clear();
            break;
        }
        case Kind::colour: {
            auto colour = edit != all.end() ? edit->second.colour : w.colour;
            if (edit != all.end() && edit->second.colour == w.colour) all.erase(edit);
            field(menu, w.text.c_str());
            if (ImGui::ColorEdit3("##colour", colour.data())) {
                plugin_ui::Action action;
                action.colour = colour;
                Edit remembered;
                remembered.colour = colour;
                send(action, remembered);
            }
            break;
        }
        case Kind::stepper: {
            float value = edit != all.end() ? edit->second.value : w.value;
            if (edit != all.end() && edit->second.value == w.value) all.erase(edit);
            const char* format = w.format.empty() ? "%.2f" : w.format.c_str();
            field(menu, w.text.c_str());
            // Left / right a step at a time; the box beside it takes a typed value.
            set_console_step(w.step);
            if (slider_float("##stepper", &value, w.minimum, w.maximum, format, ImGuiSliderFlags_AlwaysClamp)) {
                const float steps = std::round((std::clamp(value, w.minimum, w.maximum) - w.minimum) / w.step);
                value = std::clamp(w.minimum + steps * w.step, w.minimum, w.maximum);
                plugin_ui::Action action;
                action.value = value;
                send(action, {false, value});
            }
            break;
        }
        case Kind::list: {
            int chosen = edit != all.end() ? edit->second.chosen : w.chosen;
            if (edit != all.end() && edit->second.chosen == w.chosen) all.erase(edit);
            int picked = -1;
            // A row for each, as the music page's songs; A picks one.
            section(menu, w.text.c_str());
            for (int i = 0; i < static_cast<int>(w.choices.size()); ++i)
                if (list_row(menu, ("##row" + std::to_string(i)).c_str(), w.choices[static_cast<std::size_t>(i)], "", i + 1 == chosen)) picked = i;
            if (picked >= 0 && picked + 1 != chosen) {
                plugin_ui::Action action;
                action.chosen = picked + 1;
                send(action, {false, 0, picked + 1});
            }
            break;
        }
        case Kind::toggle: {
            bool on = edit != all.end() ? edit->second.on : w.on;
            if (edit != all.end() && edit->second.on == w.on) all.erase(edit);
            if (toggle_row(menu, w.text.c_str(), help, on)) {
                plugin_ui::Action action;
                action.on = on;
                send(action, {on});
            }
            break;
        }
        case Kind::slider: {
            float value = edit != all.end() ? edit->second.value : w.value;
            field(menu, w.text.c_str());
            if (slider_float("##slider", &value, w.minimum, w.maximum, w.format.empty() ? "%.2f" : w.format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                plugin_ui::Action action;
                action.value = value;
                send(action, {false, value});
            }
            break;
        }
        case Kind::dropdown: {
            int selected = (edit != all.end() ? edit->second.chosen : w.chosen) - 1;
            if (edit != all.end() && edit->second.chosen == w.chosen) all.erase(edit);
            if (option_row(menu, w.text.c_str(), help, selected, w.choices)) {
                plugin_ui::Action action;
                action.chosen = selected + 1;
                send(action, {false, 0, selected + 1});
            }
            break;
        }
        case Kind::bar:
            ImGui::ProgressBar((w.value - w.minimum) / (w.maximum - w.minimum), ImVec2(-FLT_MIN, px(22)), w.text.c_str());
            break;
        }
        ImGui::PopID();
    }
    if (in_card) end_card();
}

// A plugin's page body: its page first, then its panels as cards under it. False while it has
// neither yet (its first build has not come).
bool draw_plugin_panels(SkateMenu& menu, const std::string& plugin, double now) {
    const auto panels = plugin_ui::latest_panels();
    if (!panels) return false;
    bool any = false;
    for (const bool page : {true, false})
        for (const auto& panel : *panels) {
            if (panel.screen || panel.page != page || panel.plugin != plugin) continue;
            any = true;
            ImGui::PushID(panel.title.c_str());
            draw_widgets(menu, panel, now);
            ImGui::PopID();
        }
    return any;
}
}

std::shared_ptr<const std::vector<plugin_ui::PageEntry>> plugin_pages() {
    auto& held = held_pages();
    held = plugin_ui::latest_pages();
    return held;
}

const plugin_ui::PageEntry* shown_plugin_page(SkateMenu& menu) {
    const auto pages = held_pages() ? held_pages() : plugin_pages();
    if (pages) {
        const auto found = std::find_if(pages->begin(), pages->end(), [&](const auto& p) { return p.plugin == menu.plugin_page; });
        if (found != pages->end()) return &*found;
    }
    menu.plugin_page = plugin_manager_entry;
    return nullptr;
}

namespace {
// The list's open row (one at a time, by plugin id), and the plugins seen waiting for an answer: one
// opens by itself the first time it is seen waiting, so a prompt that was missed is not the only way
// to answer it. In memory only.
std::string& open_plugin() {
    static auto* value = new std::string;
    return *value;
}
std::set<std::string>& seen_waiting() {
    static auto* value = new std::set<std::string>;
    return *value;
}
// The plugin whose page was just left with B: its row takes the focus back as the list draws.
std::string& refocus_plugin() {
    static auto* value = new std::string;
    return *value;
}

ImU32 risk_colour(plugins::Risk risk) {
    return risk == plugins::Risk::red ? skate_theme::danger : risk == plugins::Risk::amber ? skate_theme::warning : IM_COL32(88, 90, 96, 255);
}

// One plugin as a row: its name; its author and the kinds of thing it can do, or, while it waits for an
// answer, what it asks for (amber, with an amber edge and ASKS); "Page >" when it has a page; ON or OFF
// at the right end. A opens its page when it has one, else its details; X turns it on or off (a click on
// ON / OFF too); Y opens its details. Its details are the plain list of what it can do, each kind in its
// risk's colour (red, amber, grey), and the buttons that switch or answer it. One waiting for an answer
// is only answered there, under the list of what it may do: X opens them instead of switching it.
void plugin_row(SkateMenu& menu, const plugins::Installed& plugin, bool has_page) {
    namespace st = skate_theme;
    const bool waiting = plugin.approval == plugins::Approval::asking;
    const bool on = plugin.approval == plugins::Approval::on;
    auto& open = open_plugin();
    if (waiting && seen_waiting().insert(plugin.id).second && open.empty()) open = plugin.id;
    const bool is_open = open == plugin.id;
    // The author is whoever wrote the manifest: cut short, so a long one cannot crowd out the rest.
    std::string by = plugin.author;
    if (by.size() > 40) {
        by.resize(40);
        while (!by.empty() && (static_cast<unsigned char>(by.back()) & 0xC0) == 0x80) by.pop_back(); // not mid-character
        if (!by.empty() && (static_cast<unsigned char>(by.back()) & 0xC0) == 0xC0) by.pop_back();
        by += "...";
    }
    std::string kinds;
    for (const auto& group : plugin.can) kinds += (kinds.empty() ? "" : ", ") + group.category;
    LibraryRow row;
    row.name = plugin.title;
    if (waiting) {
        row.detail = kinds.empty() ? std::string("Asks to run") : "Asks for: " + kinds;
        row.detail_colour = st::warning;
        row.edge = st::warning;
        row.tag = "ASKS";
        row.tag_fill = st::warning;
        row.tag_ink = st::black;
        row.hint = "A or X shows what it asks for, to answer it.";
    } else {
        row.detail = by;
        if (!kinds.empty()) row.detail += (row.detail.empty() ? "" : " \xc2\xb7 ") + kinds;
        row.tag = on ? "ON" : "OFF";
        if (on) row.tag_fill = st::blue, row.tag_ink = st::white;
        if (has_page) row.side = "Page \xe2\x80\xba";
        row.hint = has_page ? "A opens its page. X turns it on or off. Y shows what it may do." : "A shows what it may do. X turns it on or off.";
    }
    row.open = is_open;
    const auto pressed = library_row(menu, "##plugin", row);
    const ImGuiID row_id = ImGui::GetItemID();
    if (refocus_plugin() == plugin.id) {
        ImGui::SetFocusID(row_id, ImGui::GetCurrentWindow());
        ImGui::ScrollToItem();
        refocus_plugin().clear();
    }
    if (pressed == RowPress::a) {
        if (has_page && !waiting) {
            menu.plugin_page = plugin.id;
            open.clear();
        } else {
            open = is_open ? std::string() : plugin.id;
        }
    } else if (pressed == RowPress::x) {
        if (waiting) open = plugin.id;
        else plugins::set_enabled(plugin.id, !on);
    } else if (pressed == RowPress::y) {
        open = is_open ? std::string() : plugin.id;
    }
    if (!is_open || open != plugin.id) return;
    // B closes the details, and the focus comes back to the row from the buttons that are going away.
    hold_back();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::GetIO().WantTextInput) {
        open.clear();
        ImGui::SetFocusID(row_id, ImGui::GetCurrentWindow());
        return;
    }
    ImGui::Indent(px(40));
    for (const auto& group : plugin.can) {
        ImGui::PushStyleColor(ImGuiCol_Text, group.risk == plugins::Risk::grey ? st::grey_text : risk_colour(group.risk));
        ImGui::PushFont(menu.bold ? menu.bold : ImGui::GetFont());
        ImGui::TextUnformatted(group.category.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        for (const auto& phrase : group.phrases) note(("\xe2\x80\xa2 " + phrase).c_str());
    }
    ImGui::Unindent(px(40));
    if (waiting) {
        const int answer = row_actions(menu, "answer", {{"TURN ON", true, true, "Let it run."}, {"KEEP OFF", true, false, "It stays off."}});
        if (answer >= 0) plugins::set_enabled(plugin.id, answer == 0);
    } else {
        const RowButton turn{on ? "TURN OFF" : "TURN ON", true, !on, on ? "Stop it." : "Let it run."};
        const int action = has_page ? row_actions(menu, "switch", {turn, {"OPEN PAGE", true, false, "Its page, in place of this list."}})
                                    : row_actions(menu, "switch", {turn});
        if (action == 0) plugins::set_enabled(plugin.id, !on);
        if (action == 1) {
            menu.plugin_page = plugin.id;
            open.clear();
        }
    }
}

// PLUGINS' list: plugin developer mode, then every installed plugin, a row each, down one column.
void plugin_list(SkateMenu& menu, const std::vector<plugin_ui::PageEntry>& pages) {
    ImGui::BeginChild("plugin-list", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    page_prompts("Open page", "On / off", "Permissions");
    bool developer = plugins::developer_mode();
    if (toggle_row(menu, "Plugin dev mode", "Updates to plugins you already approved won't ask again.", developer))
        plugins::set_developer_mode(developer);
    const auto list = plugins::installed();
    section(menu, ("INSTALLED  \xc2\xb7  " + std::to_string(list.size())).c_str());
    if (list.empty())
        note("No plugin is installed. A plugin is a mod with a reskate-plugin.json: a script that runs inside the game and can add "
             "pages, modes and tools.");
    const auto asking = std::ranges::count_if(list, [](const plugins::Installed& p) { return p.approval == plugins::Approval::asking; });
    if (asking == 1) warn("A plugin is waiting for your OK. Open it to answer.");
    else if (asking > 1) warn((std::to_string(asking) + " plugins are waiting for your OK. Open one to answer.").c_str());
    for (const auto& plugin : list) {
        const bool has_page = std::ranges::any_of(pages, [&](const plugin_ui::PageEntry& p) { return p.plugin == plugin.id; });
        ImGui::PushID(plugin.id.c_str());
        plugin_row(menu, plugin, has_page);
        ImGui::PopID();
    }
    note("X turns a plugin on or off from its row. A on a plugin with a page opens it full size, with the plugin's own tabs; "
         "B comes back here. Y shows what it may do. While dev mode is on, updates to plugins you approved won't ask again.");
    if (!list.empty() && pages.empty())
        note("No plugin has a page of its own. A plugin adds one with reskate.menu.page (it needs the \"menu\" permission). "
             "plugin list in the console lists every plugin too.");
    ImGui::EndChild();
}
}

void plugins_page(SkateMenu& menu) {
    const auto pages = plugin_pages();
    const auto* shown = shown_plugin_page(menu);
    // The list, or the page of the plugin opened from it, full size: its own tabs are the page's tabs and
    // its name the page's title (skate_menu.cpp). B goes back to the list.
    if (!shown) {
        plugin_list(menu, *pages);
        return;
    }
    hold_back();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        refocus_plugin() = shown->plugin;
        menu.plugin_page = plugin_manager_entry;
        plugin_list(menu, *pages);
        return;
    }
    // Whose page this is and the way back, first on the page: B does it on a pad, but the pointer needs a
    // button, and the page reads as one plugin's rather than as PLUGINS itself.
    {
        const auto back = std::string("< PLUGINS    ") + shown->title;
        if (button_row(menu, "plugin-page-back", {{back.c_str(), true, false, "Back to the plugin list (B)."}}) == 0) {
            refocus_plugin() = shown->plugin;
            menu.plugin_page = plugin_manager_entry;
            plugin_list(menu, *pages);
            return;
        }
    }
    plugin_ui::note_page_shown(shown->plugin);
    const double now = ImGui::GetTime();
    std::erase_if(edits(), [&](const auto& item) { return now - item.second.at > 1.0; });
    ImGui::PushID(shown->plugin.c_str());
    ImGui::BeginChild("plugin-page", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    if (!draw_plugin_panels(menu, shown->plugin, now)) note("Loading...");
    ImGui::EndChild();
    ImGui::PopID();
}

PausePluginPage pause_menu_plugin_page(SkateMenu& menu, const std::string& plugin, ImVec2 a, ImVec2 b, float scale, bool input, bool take_focus) {
    PausePluginPage result;
    const auto pages = plugin_ui::latest_pages();
    if (!pages || std::ranges::none_of(*pages, [&](const plugin_ui::PageEntry& p) { return p.plugin == plugin; })) return result;
    result.shown = true;
    draw_in_kit(menu, scale, [&] {
        // The box keeps the keyboard focus while the pause page has handed it the pad: a click on the
        // page around it would leave ImGui's navigation with no window, and the D-pad doing nothing.
        // (Not over a popup of its own, which has the focus while it is open.)
        static bool focused_before = false;
        static ImGuiID heard = 0;
        const auto any_popup = [] { return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); };
        if (input && (take_focus || (!focused_before && !any_popup()))) ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowPos(a);
        ImGui::SetNextWindowSize(ImVec2(b.x - a.x, b.y - a.y));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        // Under the ReSkate menu or the console it is there to look at, as the rest of the pause page is.
        if (!input) flags |= ImGuiWindowFlags_NoInputs;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        if (ImGui::Begin("###pause-menu-plugin-page", nullptr, flags)) {
            if (input && take_focus && ImGui::GetCurrentContext()->NavWindow == ImGui::GetCurrentWindow()) {
                ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
                ImGui::SetNavCursorVisible(true);
            }
            // (The ReSkate menu over it may show another plugin's page: it has the say then.)
            if (input) plugin_ui::note_page_shown(plugin);
            const double now = ImGui::GetTime();
            std::erase_if(edits(), [&](const auto& item) { return now - item.second.at > 1.0; });
            ImGui::PushID(plugin.c_str());
            if (!draw_plugin_panels(menu, plugin, now)) note("Loading...");
            ImGui::PopID();
            focused_before = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        const ImGuiID now_focused = ImGui::GetFocusID();
        result.moved = input && now_focused && now_focused != heard && ImGui::GetCurrentContext()->NavCursorVisible;
        heard = now_focused;
        result.typing = ImGui::GetIO().WantTextInput;
        result.busy = result.typing || ImGui::IsAnyItemActive() || any_popup();
    });
    return result;
}
}
