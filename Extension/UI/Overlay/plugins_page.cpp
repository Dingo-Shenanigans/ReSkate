#include "skate_menu_internal.h"
#include "Extension/Plugins/plugin_ui.h"

#include <algorithm>
#include <map>
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
};
std::map<std::string, Edit>& edits() {
    static auto* value = new std::map<std::string, Edit>;
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
    for (const auto& w : panel.widgets) {
        // A card opens here and closes at the next one: outside any widget's own ID, each its own.
        if (w.kind == plugin_ui::Widget::Kind::card) {
            if (in_card) end_card();
            const auto id = "plugin-card-" + std::to_string(item++);
            begin_card(menu, id.c_str(), w.text.c_str(), w.help.empty() ? nullptr : w.help.c_str());
            in_card = true;
            continue;
        }
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
            if (w.on ? primary_button(menu, w.text.c_str()) : ImGui::Button(w.text.c_str(), ImVec2(-FLT_MIN, px(34)))) send({}, {});
            break;
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
}

std::shared_ptr<const std::vector<plugin_ui::PageEntry>> plugin_pages() {
    auto& held = held_pages();
    held = plugin_ui::latest_pages();
    return held;
}

const plugin_ui::PageEntry* shown_plugin_page(SkateMenu& menu) {
    const auto pages = held_pages() ? held_pages() : plugin_pages();
    if (!pages || pages->empty()) return nullptr;
    const auto found = std::find_if(pages->begin(), pages->end(), [&](const auto& p) { return p.plugin == menu.plugin_page; });
    if (found != pages->end()) return &*found;
    menu.plugin_page = pages->front().plugin;
    return &pages->front();
}

void plugins_page(SkateMenu& menu) {
    const auto pages = plugin_pages();
    const auto* shown = shown_plugin_page(menu);
    if (!shown) {
        begin_card(menu, "plugins-none", "PLUGINS");
        note("No plugin has a page here. A plugin adds one with reskate.menu.page (it needs the \"menu\" permission). "
             "plugin list in the console shows every plugin.");
        end_card();
        return;
    }
    // A window lists the plugins in its side rail; full screen has no rail, so they are tabs here.
    if (menu.fullscreen_drawn) {
        std::vector<const char*> names;
        int selected = 0;
        for (std::size_t i = 0; i < pages->size(); ++i) {
            names.push_back((*pages)[i].title.c_str());
            if ((*pages)[i].plugin == shown->plugin) selected = static_cast<int>(i);
        }
        category_tabs(menu, selected, names, "plugin-pages");
        shown = &(*pages)[static_cast<std::size_t>(std::clamp(selected, 0, static_cast<int>(pages->size()) - 1))];
        menu.plugin_page = shown->plugin;
    }
    plugin_ui::note_page_shown(shown->plugin);
    const double now = ImGui::GetTime();
    std::erase_if(edits(), [&](const auto& item) { return now - item.second.at > 1.0; });
    ImGui::PushID(shown->plugin.c_str());
    ImGui::BeginChild("plugin-page", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    const auto panels = plugin_ui::latest_panels();
    static const std::vector<plugin_ui::Panel> none;
    const auto& list = panels ? *panels : none;
    bool any = false;
    // The page first, then the plugin's panels as cards under it.
    for (const bool page : {true, false})
        for (const auto& panel : list) {
            if (panel.screen || panel.page != page || panel.plugin != shown->plugin) continue;
            any = true;
            ImGui::PushID(panel.title.c_str());
            draw_widgets(menu, panel, now);
            ImGui::PopID();
        }
    if (!any) note("Loading...");
    ImGui::EndChild();
    ImGui::PopID();
}
}
