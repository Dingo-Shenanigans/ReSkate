#include "skate_menu_internal.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "Extension/Plugins/plugin_ui.h"
#include "Extension/UI/skate_theme.h"

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
    if (pages) {
        const auto found = std::find_if(pages->begin(), pages->end(), [&](const auto& p) { return p.plugin == menu.plugin_page; });
        if (found != pages->end()) return &*found;
    }
    menu.plugin_page = plugin_manager_entry;
    return nullptr;
}

namespace {
// Which plugins are open in the manager, by id: in memory only. One waiting for an answer starts open.
std::map<std::string, bool>& opened() {
    static auto* value = new std::map<std::string, bool>;
    return *value;
}

ImU32 risk_colour(plugins::Risk risk) {
    return risk == plugins::Risk::red ? skate_theme::danger : risk == plugins::Risk::amber ? skate_theme::warning : IM_COL32(88, 90, 96, 255);
}

// One plugin: its name, its author and its switch on a line, then a chip for each kind of thing it can do
// (the riskiest first, red then amber then grey). A click anywhere but the switch opens the plugin to
// the plain list; one that is waiting for an answer starts open, with its buttons under the list.
void plugin_row(SkateMenu& menu, const plugins::Installed& plugin) {
    namespace st = skate_theme;
    const bool waiting = plugin.approval == plugins::Approval::asking;
    auto& open = opened();
    const auto found = open.find(plugin.id);
    const bool expanded = found != open.end() ? found->second : waiting;
    auto* draw = ImGui::GetWindowDrawList();
    auto* bold = menu.bold ? menu.bold : ImGui::GetFont();
    auto* body = menu.body ? menu.body : ImGui::GetFont();
    const float width = ImGui::GetContentRegionAvail().x;
    const float head = px(30), chips_height = px(24), gap = px(8);
    const char* answer = waiting ? "Waiting for your OK" : plugin.approval == plugins::Approval::on ? "ON" : "OFF";
    const float answer_width = waiting ? bold->CalcTextSizeA(px(14), FLT_MAX, 0, answer).x : px(64);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float right = origin.x + width;
    auto& io = ImGui::GetIO();

    // Two stops for the pad and keys: the row (opens and closes it), and the switch on top of it.
    ImGui::SetNextItemAllowOverlap();
    const bool toggled = ImGui::InvisibleButton("##expand", ImVec2(width, head + chips_height), ImGuiButtonFlags_EnableNav);
    const bool row_focused = ImGui::IsItemFocused() && io.NavVisible;
    bool hovered = ImGui::IsItemHovered(), over_switch = false, switch_focused = false;
    const ImVec2 switch_min(right - answer_width, origin.y + px(3)), switch_max(right, origin.y + head - px(3));
    if (!waiting) {
        ImGui::SetCursorScreenPos(switch_min);
        if (ImGui::InvisibleButton("##switch", ImVec2(answer_width, switch_max.y - switch_min.y), ImGuiButtonFlags_EnableNav))
            plugins::set_enabled(plugin.id, plugin.approval != plugins::Approval::on);
        over_switch = ImGui::IsItemHovered();
        switch_focused = ImGui::IsItemFocused() && io.NavVisible;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + head + chips_height));

    if (hovered) draw->AddRectFilled(origin, ImVec2(right, origin.y + head + chips_height), IM_COL32(255, 255, 255, 12), px(4));
    if (row_focused) draw->AddRect(origin, ImVec2(right, origin.y + head + chips_height), st::white, px(4), 0, px(2));
    // The name takes what the author and the switch leave. The author (whoever wrote the manifest) gets at
    // most a third of the row, cut with "...", and the name always keeps room: it is what the player trusts.
    const float title_room_min = std::min(px(150), width * 0.4f);
    const float by_size = px(13);
    std::string by = plugin.author.empty() ? std::string{} : "by " + plugin.author;
    const float by_max = std::max(0.0f, std::min(width * 0.35f, width - answer_width - gap * 3 - title_room_min));
    const auto by_fits = [&] { return body->CalcTextSizeA(by_size, FLT_MAX, 0, by.c_str()).x <= by_max; };
    if (!by.empty() && !by_fits()) {
        while (by.size() > 3 && !by_fits()) {
            by.pop_back();
            while (!by.empty() && (static_cast<unsigned char>(by.back()) & 0xC0) == 0x80) by.pop_back(); // not mid-character
            if (!by.empty() && (static_cast<unsigned char>(by.back()) & 0xC0) == 0xC0) by.pop_back();
        }
        by += "...";
        if (!by_fits()) by.clear();
    }
    const float by_width = by.empty() ? 0.0f : body->CalcTextSizeA(by_size, FLT_MAX, 0, by.c_str()).x;
    const float by_x = right - answer_width - gap * 2 - by_width;
    if (!by.empty()) draw->AddText(body, by_size, ImVec2(by_x, origin.y + (head - by_size) * 0.5f), st::grey_text, by.c_str());
    draw->PushClipRect(origin, ImVec2(by.empty() ? right - answer_width - gap : by_x - gap, origin.y + head), true);
    draw->AddText(bold, px(17), ImVec2(origin.x + px(6), origin.y + (head - px(17)) * 0.5f), st::white, plugin.title.c_str());
    draw->PopClipRect();
    if (waiting) {
        draw->AddText(bold, px(14), ImVec2(right - answer_width, origin.y + (head - px(14)) * 0.5f), st::warning, answer);
    } else {
        const bool on = plugin.approval == plugins::Approval::on;
        draw->AddRectFilled(switch_min, switch_max, on ? (over_switch ? st::blue_hover : st::blue) : (over_switch ? IM_COL32(72, 73, 78, 255) : st::tile_light), px(4));
        if (switch_focused) draw->AddRect(switch_min, switch_max, st::white, px(4), 0, px(2));
        const auto size = bold->CalcTextSizeA(px(14), FLT_MAX, 0, answer);
        draw->AddText(bold, px(14), ImVec2(switch_min.x + (answer_width - size.x) * 0.5f, switch_min.y + (switch_max.y - switch_min.y - size.y) * 0.5f),
                      on ? st::black : st::white, answer);
    }
    // The chips, as many as fit before "+N" and the open marker.
    const float chip_size = px(11), chip_height = px(18), chip_y = origin.y + head + (chips_height - chip_height) * 0.5f;
    float x = origin.x + px(6);
    std::size_t shown = 0;
    for (const auto& group : plugin.can) {
        const float chip_width = bold->CalcTextSizeA(chip_size, FLT_MAX, 0, group.category.c_str()).x + px(14);
        if (shown > 0 && x + chip_width > right - px(60)) break;
        draw->AddRectFilled(ImVec2(x, chip_y), ImVec2(x + chip_width, chip_y + chip_height), risk_colour(group.risk), px(9));
        draw->AddText(bold, chip_size, ImVec2(x + px(7), chip_y + (chip_height - chip_size) * 0.5f),
                      group.risk == plugins::Risk::grey ? st::white : st::black, group.category.c_str());
        x += chip_width + px(6);
        ++shown;
    }
    if (shown < plugin.can.size()) {
        const auto more = "+" + std::to_string(plugin.can.size() - shown);
        draw->AddText(bold, chip_size + px(1), ImVec2(x, chip_y + (chip_height - chip_size) * 0.5f), st::grey_text, more.c_str());
    }
    const ImVec2 mark(right - px(14), origin.y + head + chips_height * 0.5f);
    const float arm = px(5);
    if (expanded) draw->AddTriangleFilled(ImVec2(mark.x - arm, mark.y - arm * 0.5f), ImVec2(mark.x + arm, mark.y - arm * 0.5f), ImVec2(mark.x, mark.y + arm * 0.7f), st::grey_text);
    else draw->AddTriangleFilled(ImVec2(mark.x - arm * 0.5f, mark.y - arm), ImVec2(mark.x - arm * 0.5f, mark.y + arm), ImVec2(mark.x + arm * 0.7f, mark.y), st::grey_text);

    if (expanded) {
        ImGui::Indent(px(14));
        for (const auto& group : plugin.can) {
            ImGui::PushStyleColor(ImGuiCol_Text, group.risk == plugins::Risk::grey ? st::grey_text : risk_colour(group.risk));
            ImGui::PushFont(bold);
            ImGui::TextUnformatted(group.category.c_str());
            ImGui::PopFont();
            ImGui::PopStyleColor();
            for (const auto& phrase : group.phrases) note(("\xe2\x80\xa2 " + phrase).c_str());
        }
        ImGui::Unindent(px(14));
        if (waiting) {
            ImGui::Spacing();
            if (primary_button(menu, "Turn on")) plugins::set_enabled(plugin.id, true);
            if (ImGui::Button("Keep off", ImVec2(-FLT_MIN, px(34)))) plugins::set_enabled(plugin.id, false);
        }
    }
    if (toggled) open[plugin.id] = !expanded;
    ImGui::Dummy(ImVec2(0, px(6)));
}
}

// Every plugin that may load. One that has no answer yet says so, so a prompt that was missed is not
// the only way to answer.
void installed_plugins(SkateMenu& menu) {
    begin_card(menu, "plugins-installed", "INSTALLED PLUGINS");
    const auto list = plugins::installed();
    if (list.empty())
        note("No plugin is installed. A plugin is a mod with a reskate-plugin.json: a script that runs inside the game and can add "
             "pages, modes and tools.");
    for (const auto& plugin : list) {
        ImGui::PushID(plugin.id.c_str());
        plugin_row(menu, plugin);
        ImGui::PopID();
    }
    end_card();
}

// The first entry of PLUGINS: every installed plugin and whether it may run.
void plugin_manager_page(SkateMenu& menu, bool no_pages) {
    ImGui::BeginChild("plugin-manager", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    installed_plugins(menu);
    if (no_pages) {
        begin_card(menu, "plugins-none", "PLUGIN PAGES");
        note("No plugin has a page of its own. A plugin adds one with reskate.menu.page (it needs the \"menu\" permission). "
             "plugin list in the console lists every plugin too.");
        end_card();
    }
    begin_card(menu, "plugin-developer", "PLUGIN DEVELOPMENT");
    bool developer = plugins::developer_mode();
    if (toggle_row(menu, "Plugin developer mode", "", developer))
        plugins::set_developer_mode(developer);
    note("Updates to plugins you already approved won't ask again while this is on.");
    end_card();
    ImGui::EndChild();
}

void plugins_page(SkateMenu& menu) {
    const auto pages = plugin_pages();
    const auto* shown = shown_plugin_page(menu);
    // The PLUGIN MANAGER is the first entry, then each plugin with a page. A window lists them in its side
    // rail; full screen has no rail, so they are tabs here (tab 0 is the manager, tab n is pages[n - 1]).
    if (menu.fullscreen_drawn) {
        std::vector<const char*> names{"PLUGIN MANAGER"};
        int selected = 0;
        for (std::size_t i = 0; i < pages->size(); ++i) {
            names.push_back((*pages)[i].title.c_str());
            if (shown && (*pages)[i].plugin == shown->plugin) selected = static_cast<int>(i) + 1;
        }
        category_tabs(menu, selected, names, "plugin-pages");
        selected = std::clamp(selected, 0, static_cast<int>(pages->size()));
        shown = selected == 0 ? nullptr : &(*pages)[static_cast<std::size_t>(selected - 1)];
        menu.plugin_page = shown ? shown->plugin : plugin_manager_entry;
    }
    if (!shown) {
        plugin_manager_page(menu, pages->empty());
        return;
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
