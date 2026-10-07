#include "gui_internal.h"

#include "mod_manager.h"

#include "Engine/Core/Log/logging.h"

#include <cstring>
#include <shobjidl.h>
#include <wrl/client.h>

#include <format>

using Microsoft::WRL::ComPtr;

// The Mods page: a nav rail on the left, and a list where every mod is a
// full-width row that opens where it sits. MY MODS is install order and
// enabled state; GET MODS is Thunderstore (gui_mods_browse.cpp).
namespace dingosdk::launcher_gui::detail {
namespace {

void save(ModsPanel& panel) {
    try {
        mods::save_mod_order(panel.root, panel.list.entries);
        panel.list.issue.clear();
        panel.message = "Saved. Changes apply the next time Skate starts.";
        panel.message_error = false;
    } catch (const std::exception& failure) {
        panel.message = failure.what();
        panel.message_error = true;
    }
}

// Picks up a finished install: rescan and select the new mod, or ask to replace.
void collect_install(ModsPanel& panel, const launcher_app::Session& session) {
    std::lock_guard lock(panel.mutex);
    if (!panel.finished) return;
    panel.finished = false;
    panel.progress = -1;
    if (!panel.finished_conflict.empty()) {
        panel.conflict_name = panel.finished_conflict;
        panel.conflict_source = panel.finished_source;
        panel.message.clear();
        return;
    }
    scan(panel, session);
    if (!panel.finished_error.empty()) {
        panel.message = panel.finished_error;
        panel.message_error = true;
        return;
    }
    for (std::size_t i = 0; i < panel.list.entries.size(); ++i)
        if (panel.list.entries[i].mod.name == panel.finished_name) panel.selected = static_cast<int>(i);
    panel.message_error = false;
    if (!panel.finished_note.empty()) {
        panel.message = panel.finished_note;   // Thunderstore installs say what they did and log it themselves
        panel.finished_note.clear();
        return;
    }
    panel.message = "Installed " + panel.finished_name + ". It loads the next time Skate starts.";
    logging::write(logging::Level::info, logging::Channel::launcher, "Mod installed: " + panel.finished_name);
}

// The Windows file (or folder) picker; empty when cancelled.
fs::path pick(HWND owner, bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        const COMDLG_FILTERSPEC filter[]{{L"Mod archive (*.zip)", L"*.zip"}};
        dialog->SetFileTypes(1, filter);
    }
    dialog->SetTitle(folder ? L"Choose a mod folder to install" : L"Choose a mod .zip to install");
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path{};
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

// Everything in the mod's folder, as Explorer would count it. The game's merged
// patch hard links a mod's archives rather than copying them, so this is also
// what removing the mod gives back.
std::uint64_t folder_size(const fs::path& directory) {
    std::uint64_t total{};
    std::error_code error;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        std::error_code ignored;
        if (it->is_regular_file(ignored) && !ignored) {
            const auto size = it->file_size(ignored);
            if (!ignored) total += size;
        }
    }
    return total;
}

std::string summary(const ModsPanel& panel, const mods::Mod& mod) {
    std::vector<std::string> parts;
    if (!mod.version.empty()) parts.push_back("v" + mod.version);
    if (const auto size = panel.sizes.find(mod.name); size != panel.sizes.end() && size->second)
        parts.push_back(size_text(size->second));
    if (!mod.author.empty()) parts.push_back("by " + mod.author);
    if (!mod.levels.empty()) parts.push_back(mod.levels.size() == 1 ? "1 map" : std::to_string(mod.levels.size()) + " maps");
    else if (mod.provides_layout) parts.push_back("game data");
    if (!mod.park_maps.empty()) parts.push_back(mod.park_maps.size() == 1 ? "1 park" : std::to_string(mod.park_maps.size()) + " parks");
    if (!mod.provides_layout && !mod.provides_levels && mod.park_maps.empty()) parts.push_back("nothing to load");
    std::string text;
    for (const auto& part : parts) text += (text.empty() ? "" : "  /  ") + part;
    return text;
}

// ---------------------------------------------------------------- nav rail

// The rail: the two pages, then what you can do to the Mods folder. Returns
// true when BACK was pressed.
// The way out to Thunderstore's website, in Thunderstore's colours (its green on its dark blue),
// taller than the plain buttons beside it.
bool thunderstore_button(const Fonts& fonts, const char* label, float height) {
    const ImVec2 position = ImGui::GetCursorScreenPos(), size(ImGui::GetContentRegionAvail().x, height);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(position.x + size.x, position.y + size.y);
    draw->AddRectFilled(position, end, hovered ? rgba(28, 44, 74) : rgba(19, 30, 52), S(4));
    draw->AddRect(position, end, rgba(35, 255, 176, hovered ? 1.f : 0.6f), S(4), 0, S(1.5f));
    // [ link  Thunderstore  mark ]: the link sign first, so it reads as a way out to a website,
    // the name, and Thunderstore's mark at the far end.
    const ImU32 green = rgba(35, 255, 176);
    const float pad = S(14), mark = g_icon_thunderstore.id ? height - S(20) : 0;
    {
        // A box with its top right corner open and an arrow leaving through it.
        const float box = S(13), stroke = S(1.8f);
        const ImVec2 at(position.x + pad, position.y + (height - box) * 0.5f);
        const ImVec2 outline[]{ImVec2(at.x + box * 0.45f, at.y + box * 0.15f), ImVec2(at.x, at.y + box * 0.15f),
                               ImVec2(at.x, at.y + box), ImVec2(at.x + box * 0.85f, at.y + box),
                               ImVec2(at.x + box * 0.85f, at.y + box * 0.55f)};
        draw->AddPolyline(outline, 5, green, 0, stroke);
        draw->AddLine(ImVec2(at.x + box * 0.4f, at.y + box * 0.6f), ImVec2(at.x + box, at.y), green, stroke);
        const ImVec2 head[]{ImVec2(at.x + box * 0.6f, at.y), ImVec2(at.x + box, at.y), ImVec2(at.x + box, at.y + box * 0.4f)};
        draw->AddPolyline(head, 3, green, 0, stroke);
    }
    const char* shown = std::strstr(label, "##"); // what follows only names the button
    if (!shown) shown = label + std::strlen(label);
    const auto text = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, label, shown);
    const float text_x = position.x + pad + S(13) + S(10);
    const ImVec4 clip(position.x, position.y, end.x - pad - mark - (mark ? S(8) : 0), end.y);
    draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, position.y + (height - text.y) * 0.5f), green, label, shown, 0,
        &clip);
    if (mark) {
        const ImVec2 at(end.x - pad - mark, position.y + (height - mark) * 0.5f);
        draw->AddImage(g_icon_thunderstore.id, at, ImVec2(at.x + mark, at.y + mark));
    }
    return pressed;
}

bool rail(const Fonts& fonts, ModsPanel& panel, HWND window, float width,
          const std::vector<const thunderstore::Package*>& pending, bool installing) {
    const auto installed = panel.list.entries.size();
    const auto& store = panel.store;
    bool leave = false;

    ImGui::BeginDisabled(installing);
    if (nav_tile(fonts, width, "MY MODS", panel.tab == 0,
            pending.empty() ? std::to_string(installed) : std::to_string(pending.size()), !pending.empty(),
            pending.empty() ? std::string()
                            : pending.size() == 1 ? std::string("1 update on Thunderstore")
                                                  : std::format("{} updates on Thunderstore", pending.size())))
        panel.tab = 0;
    if (nav_tile(fonts, width, "GET MODS", panel.tab == 1,
            store.loaded ? std::to_string(store.packages.size()) : std::string("..."), false,
            store.loaded ? std::string() : std::string("Loading the Thunderstore listing")))
        panel.tab = 1;

    ImGui::Dummy(ImVec2(0, S(8)));
    if (!pending.empty()) {
        push_primary_button();
        if (ImGui::Button(std::format("UPDATE ALL ({})", pending.size()).c_str(), ImVec2(-1, S(34)))) {
            std::vector<thunderstore::Package> packages;
            for (const auto* package : pending) packages.push_back(*package);
            start_store_install(panel, std::move(packages));
        }
        pop_primary_button();
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    if (ImGui::Button("Install .zip", ImVec2(-1, S(32))))
        if (const auto path = pick(window, false); !path.empty()) start_install(panel, path, false);
    if (ImGui::Button("Install folder", ImVec2(-1, S(32))))
        if (const auto path = pick(window, true); !path.empty()) start_install(panel, path, false);
    if (ImGui::Button("Open Mods folder", ImVec2(-1, S(32)))) open_path(panel.root);
    ImGui::Dummy(ImVec2(0, S(4)));
    if (thunderstore_button(fonts, "Thunderstore##visit", S(48)))
        open_url(utf8(thunderstore::community_page(thunderstore::community())));
    ImGui::EndDisabled();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(38));
    push_primary_button();
    if (ImGui::Button("\xe2\x86\x90  BACK", ImVec2(-1, S(34)))) leave = true;
    pop_primary_button();
    return leave;
}

// ---------------------------------------------------------------- MY MODS

constexpr std::array<const char*, 7> filter_names{"All mods", "Enabled", "Disabled", "Maps", "Game data", "Updates",
                                                  "Problems"};
constexpr std::array<const char*, 3> order_names{"Load order", "Name", "Largest first"};

// Something the page was asked to do to one mod or to all the ticked ones.
// Carried out once the list is drawn: most of it reorders or shrinks the list.
struct Request {
    enum class Kind { none, enable, disable, top, bottom, up, down, update, uninstall };
    Kind kind{Kind::none};
    std::vector<std::string> mods;       // folder names
};

std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& letter : result) letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    return result;
}

bool update_waiting(const ModsPanel& panel, const thunderstore::Installed& installed, const std::string& name) {
    const auto* package = package_for(panel.store, name);
    return package && thunderstore::update_available(*package, installed);
}

// The rows the list shows, as places in the load order: what the search and the
// filter leave, in the order asked for.
std::vector<int> visible_mods(const ModsPanel& panel, const thunderstore::Installed& installed) {
    const auto& entries = panel.list.entries;
    const auto query = folded(panel.search.data());
    std::vector<int> view;
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const auto& entry = entries[static_cast<std::size_t>(index)];
        const auto& mod = entry.mod;
        bool keep = true;
        switch (panel.filter) {
        case 1: keep = entry.enabled; break;
        case 2: keep = !entry.enabled; break;
        case 3: keep = !mod.levels.empty(); break;
        case 4: keep = mod.levels.empty() && mod.provides_layout; break;
        case 5: keep = update_waiting(panel, installed, mod.name); break;
        case 6: keep = !mod.outdated.empty() || panel.list.excluded.contains(mod.name); break;
        default: break;
        }
        if (keep && !query.empty())
            keep = folded(mod.title).find(query) != std::string::npos ||
                   folded(mod.name).find(query) != std::string::npos ||
                   folded(mod.author).find(query) != std::string::npos;
        if (keep) view.push_back(index);
    }
    const auto size = [&](int index) {
        const auto found = panel.sizes.find(entries[static_cast<std::size_t>(index)].mod.name);
        return found == panel.sizes.end() ? std::uint64_t{} : found->second;
    };
    if (panel.order == 1)
        std::stable_sort(view.begin(), view.end(), [&](int a, int b) {
            return folded(entries[static_cast<std::size_t>(a)].mod.title) <
                   folded(entries[static_cast<std::size_t>(b)].mod.title);
        });
    else if (panel.order == 2)
        std::stable_sort(view.begin(), view.end(), [&](int a, int b) { return size(a) > size(b); });
    return view;
}

// Ticks or unticks the row at `position`; with `range`, every row from the last
// one ticked to it, the way Shift-click works in a file list.
void mark(ModsPanel& panel, const std::vector<int>& view, int position, bool on, bool range) {
    const auto name = [&](int at) -> const std::string& {
        return panel.list.entries[static_cast<std::size_t>(view[static_cast<std::size_t>(at)])].mod.name;
    };
    int from = position;
    if (range)
        for (int at = 0; at < static_cast<int>(view.size()); ++at)
            if (name(at) == panel.anchor) from = at;
    for (int at = std::min(from, position); at <= std::max(from, position); ++at) {
        if (on) panel.marked.insert(name(at));
        else panel.marked.erase(name(at));
    }
    panel.anchor = name(position);
}

void carry_out(ModsPanel& panel, const thunderstore::Installed& installed, const Request& request) {
    auto& entries = panel.list.entries;
    const auto asked = [&](const mods::ModEntry& entry) {
        return std::find(request.mods.begin(), request.mods.end(), entry.mod.name) != request.mods.end();
    };
    const auto count = request.mods.size();
    const auto saved = [&](const char* one, const char* many) {
        save(panel);
        if (!panel.message_error)
            panel.message = (count == 1 ? std::string(one) : std::format("{} {}", count, many)) +
                            " Changes apply the next time Skate starts.";
    };
    // The open overview follows its mod through a reorder.
    const std::string open = panel.selected >= 0 && panel.selected < static_cast<int>(entries.size())
        ? entries[static_cast<std::size_t>(panel.selected)].mod.name : std::string();
    switch (request.kind) {
    case Request::Kind::enable:
    case Request::Kind::disable: {
        const bool on = request.kind == Request::Kind::enable;
        for (auto& entry : entries)
            if (asked(entry)) entry.enabled = on;
        saved(on ? "Mod enabled." : "Mod disabled.", on ? "mods enabled." : "mods disabled.");
        break;
    }
    case Request::Kind::top:
        std::stable_partition(entries.begin(), entries.end(), asked);
        saved("Moved to the top.", "mods moved to the top.");
        break;
    case Request::Kind::bottom:
        std::stable_partition(entries.begin(), entries.end(),
            [&](const mods::ModEntry& entry) { return !asked(entry); });
        saved("Moved to the bottom.", "mods moved to the bottom.");
        break;
    case Request::Kind::up:
    case Request::Kind::down: {
        const auto found = std::find_if(entries.begin(), entries.end(), asked);
        if (found == entries.end()) break;
        const bool up = request.kind == Request::Kind::up;
        if (up ? found == entries.begin() : found + 1 == entries.end()) break;
        std::iter_swap(found, up ? found - 1 : found + 1);
        save(panel);
        break;
    }
    case Request::Kind::update: {
        std::vector<thunderstore::Package> packages;
        for (const auto& name : request.mods)
            if (update_waiting(panel, installed, name)) packages.push_back(*package_for(panel.store, name));
        start_store_install(panel, std::move(packages));
        break;
    }
    case Request::Kind::uninstall:
        panel.confirm_remove = request.mods;
        break;
    case Request::Kind::none:
        break;
    }
    if (!open.empty())
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].mod.name == open) panel.selected = static_cast<int>(i);
}

// One installed mod. The tick on the left picks it for the bar above the list,
// the switch on the right is whether it loads, and a click anywhere else opens
// everything the mod says about itself.
void installed_row(const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed,
                   const std::vector<int>& view, int position, float tall, bool reorder, Request& request) {
    const int index = view[static_cast<std::size_t>(position)];
    auto& entry = panel.list.entries[static_cast<std::size_t>(index)];
    const auto& mod = entry.mod;
    ImGui::PushID(mod.name.c_str());
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool ticked = panel.marked.contains(mod.name);
    const bool pressed = list_row("##row", width, tall, ticked);
    bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (pressed) {
        // Ctrl and Shift pick, as they do in a file list; a plain click opens the mod.
        if (ImGui::GetIO().KeyShift) mark(panel, view, position, true, true);
        else if (ImGui::GetIO().KeyCtrl) mark(panel, view, position, !ticked, false);
        else {
            panel.selected = index;
            panel.overview = false;
        }
    }
    const float right = start.x + width;
    const bool left_out = panel.list.excluded.contains(mod.name);
    const auto* package = package_for(panel.store, mod.name);
    const bool update = package && thunderstore::update_available(*package, installed);

    ImGui::SetCursorScreenPos(ImVec2(start.x + S(14), start.y + (tall - ImGui::GetFrameHeight()) * 0.5f));
    bool tick = ticked;
    if (ImGui::Checkbox("##pick", &tick)) mark(panel, view, position, tick, ImGui::GetIO().KeyShift);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tick mods to change several at once.\nShift-click ticks every mod in between.");

    // Right to left: the menu, whether it loads, its place in the load order.
    const float more = S(30);
    const float more_x = right - S(12) - more;
    ImGui::SetCursorScreenPos(ImVec2(more_x, start.y + (tall - more) * 0.5f));
    if (more_button("##more", more)) menu = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("More");

    const float switch_x = more_x - S(12) - S(42);
    ImGui::SetCursorScreenPos(ImVec2(switch_x, start.y + (tall - S(22)) * 0.5f));
    bool enabled = entry.enabled;
    if (toggle("##enabled", &enabled))
        request = {enabled ? Request::Kind::enable : Request::Kind::disable, {mod.name}};
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(entry.enabled ? "Enabled: loads when Skate starts" : "Disabled: stays installed, does not load");

    const float arrow = ImGui::GetFrameHeight();
    const float arrows_x = switch_x - S(18) - arrow * 2 - S(6);
    const char* fixed = "Show all mods in load order to move them";
    ImGui::SetCursorScreenPos(ImVec2(arrows_x, start.y + (tall - arrow) * 0.5f));
    ImGui::BeginDisabled(!reorder || index == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) request = {Request::Kind::up, {mod.name}};
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", reorder ? "Load earlier: where two mods change the same thing, the higher one wins" : fixed);
    ImGui::SameLine(0, S(6));
    ImGui::BeginDisabled(!reorder || index + 1 == static_cast<int>(panel.list.entries.size()));
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) request = {Request::Kind::down, {mod.name}};
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reorder ? "Load later" : fixed);

    float next = arrows_x - S(14);
    if (update) {
        const float button = S(96);
        next -= button;
        ImGui::SetCursorScreenPos(ImVec2(next, start.y + (tall - S(30)) * 0.5f));
        push_primary_button();
        if (ImGui::Button("UPDATE", ImVec2(button, S(30)))) request = {Request::Kind::update, {mod.name}};
        pop_primary_button();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thunderstore has v%s", package->latest().number.c_str());
        next -= S(14);
    }
    const float pill_y = start.y + (tall - fonts.caption->FontSize - S(8)) * 0.5f;
    const auto pill = [&](const char* label, ImU32 fill) {
        next -= badge_width(fonts, label);
        badge(draw, fonts, ImVec2(next, pill_y), label, fill, color::ink);
        next -= S(12);
    };
    if (!mod.outdated.empty()) pill("OUTDATED", color::warning);
    else if (left_out) pill("NOT LOADED", color::danger);

    // The same icon the store shows, so a mod looks like itself on both pages.
    mod_icon(panel, package, ImVec2(start.x + S(48), start.y + (tall - S(48)) * 0.5f), S(48));
    const float text_x = start.x + S(108);
    const ImVec4 clip(text_x, start.y, next, start.y + tall);
    // The number is its place in the load order, whatever order the list is shown in.
    const auto title = std::to_string(index + 1) + ".  " + mod.title;
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(text_x, start.y + S(13)),
        entry.enabled ? color::text : color::muted, title.c_str(), nullptr, 0, &clip);
    if (const auto detail = summary(panel, mod); !detail.empty())
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, start.y + S(37)), color::muted, detail.c_str(),
            nullptr, 0, &clip);

    if (menu) ImGui::OpenPopup("##menu");
    if (ImGui::BeginPopup("##menu")) {
        // Opened on one of several ticked mods, the menu is about all of them.
        const bool group = ticked && panel.marked.size() > 1;
        const std::vector<std::string> names = group
            ? std::vector<std::string>(panel.marked.begin(), panel.marked.end()) : std::vector<std::string>{mod.name};
        const auto ask = [&](Request::Kind kind) { request = {kind, names}; };
        if (group) {
            ImGui::TextDisabled("%s", std::format("{} mods selected", names.size()).c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Enable")) ask(Request::Kind::enable);
            if (ImGui::MenuItem("Disable")) ask(Request::Kind::disable);
        } else {
            if (ImGui::MenuItem("Details")) {
                panel.selected = index;
                panel.overview = false;
            }
            if (ImGui::MenuItem(entry.enabled ? "Disable" : "Enable"))
                ask(entry.enabled ? Request::Kind::disable : Request::Kind::enable);
        }
        if (std::any_of(names.begin(), names.end(),
                [&](const std::string& name) { return update_waiting(panel, installed, name); }) &&
            ImGui::MenuItem("Update")) ask(Request::Kind::update);
        ImGui::Separator();
        if (ImGui::MenuItem("Move to top")) ask(Request::Kind::top);
        if (ImGui::MenuItem("Move to bottom")) ask(Request::Kind::bottom);
        if (!group) {
            ImGui::Separator();
            if (ImGui::MenuItem("Open folder")) open_path(mod.directory);
            if (package && !package->package_url.empty() && ImGui::MenuItem("Thunderstore page"))
                open_url(package->package_url);
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
        if (ImGui::MenuItem("Uninstall")) ask(Request::Kind::uninstall);
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void installed_page(Launcher& launcher, const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed,
                    float height, bool installing) {
    auto& entries = panel.list.entries;
    const float top = ImGui::GetCursorPosY();

    // ------------------------------------------------ search, filter, order, refresh
    const float combo = S(170), refresh = S(110);
    ImGui::SetNextItemWidth(std::max(S(140), ImGui::GetContentRegionAvail().x - (combo + S(10)) * 2 - refresh - S(10)));
    ImGui::InputTextWithHint("##installed_search", "Search your mods", panel.search.data(), panel.search.size());
    ImGui::SameLine();
    const auto choice = [&](const char* id, const auto& names, int& value) {
        const int last = static_cast<int>(names.size()) - 1;
        ImGui::SetNextItemWidth(combo);
        if (ImGui::BeginCombo(id, names[static_cast<std::size_t>(std::clamp(value, 0, last))])) {
            for (int i = 0; i <= last; ++i)
                if (ImGui::Selectable(names[static_cast<std::size_t>(i)], value == i)) value = i;
            ImGui::EndCombo();
        }
    };
    choice("##installed_filter", filter_names, panel.filter);
    ImGui::SameLine();
    choice("##installed_order", order_names, panel.order);
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(refresh, 0))) refresh_mods(launcher, panel);

    const auto view = visible_mods(panel, installed);
    // Arrows move a mod past its neighbour in the load order, which is only
    // what the list shows when nothing is filtered out or sorted another way.
    const bool reorder = panel.order == 0 && view.size() == entries.size();
    const auto shown = [&](int at) -> const std::string& {
        return entries[static_cast<std::size_t>(view[static_cast<std::size_t>(at)])].mod.name;
    };
    Request request;

    // ------------------------------------------------ tick all, and what to do with the ticked
    ImGui::Spacing();
    ImGui::BeginDisabled(installing);
    bool all = !view.empty();
    for (int at = 0; at < static_cast<int>(view.size()) && all; ++at) all = panel.marked.contains(shown(at));
    ImGui::BeginDisabled(view.empty());
    if (ImGui::Checkbox("##all", &all))
        for (int at = 0; at < static_cast<int>(view.size()); ++at) {
            if (all) panel.marked.insert(shown(at));
            else panel.marked.erase(shown(at));
        }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(view.size() == entries.size() ? "Tick every mod" : "Tick every mod shown");
    ImGui::SameLine(0, S(10));
    ImGui::AlignTextToFramePadding();
    const auto total = [&](auto&& counted) {
        std::uint64_t bytes{};
        for (const auto& [name, size] : panel.sizes)
            if (counted(name)) bytes += size;
        return bytes;
    };
    if (panel.marked.empty()) {
        const auto enabled = std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.enabled; });
        auto line = std::format("{} {}  /  {} enabled", entries.size(), entries.size() == 1 ? "mod" : "mods", enabled);
        if (const auto bytes = total([](const std::string&) { return true; })) line += "  /  " + size_text(bytes);
        if (view.size() != entries.size()) line += std::format("  /  {} shown", view.size());
        ImGui::TextDisabled("%s", line.c_str());
    } else {
        const std::vector<std::string> names(panel.marked.begin(), panel.marked.end());
        auto line = std::format("{} selected", names.size());
        if (const auto bytes = total([&](const std::string& name) { return panel.marked.contains(name); }))
            line += "  /  " + size_text(bytes);
        ImGui::TextUnformatted(line.c_str());
        const auto waiting = std::count_if(names.begin(), names.end(),
            [&](const std::string& name) { return update_waiting(panel, installed, name); });
        const auto update_label = std::format("UPDATE {}", waiting);
        struct Action { const char* label; float width; Request::Kind kind; };
        const std::array actions{Action{"Enable", S(84), Request::Kind::enable},
            Action{"Disable", S(84), Request::Kind::disable}, Action{"Move to top", S(112), Request::Kind::top},
            Action{"Move to bottom", S(132), Request::Kind::bottom},
            Action{update_label.c_str(), S(104), Request::Kind::update},
            Action{"Uninstall", S(96), Request::Kind::uninstall}};
        const float gap = S(8), clear = S(70);
        float across = clear;
        for (const auto& action : actions)
            if (action.kind != Request::Kind::update || waiting) across += action.width + gap;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - across));
        for (const auto& action : actions) {
            if (action.kind == Request::Kind::update && !waiting) continue;
            const bool primary = action.kind == Request::Kind::update, danger = action.kind == Request::Kind::uninstall;
            if (primary) push_primary_button();
            if (danger) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
            if (ImGui::Button(action.label, ImVec2(action.width, 0))) request = {action.kind, names};
            if (danger) ImGui::PopStyleColor();
            if (primary) pop_primary_button();
            ImGui::SameLine(0, gap);
        }
        if (ImGui::Button("Clear", ImVec2(clear, 0))) panel.marked.clear();
    }
    ImGui::EndDisabled();

    // ------------------------------------------------ what the player should know
    ImGui::PushTextWrapPos(0);
    if (launcher.game())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning),
            "Skate is running: restart it to apply changes.");
    if (!panel.list.issue.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "mods.json could not be read (%s), so the game loads no mods. Any change here rewrites it.",
            panel.list.issue.c_str());
    std::vector<std::string> dropped;
    for (const auto& entry : entries)
        if (entry.enabled && panel.list.excluded.contains(entry.mod.name) && entry.mod.outdated.empty())
            dropped.push_back(entry.mod.title);
    if (!dropped.empty()) {
        std::string names;
        for (const auto& title : dropped) names += (names.empty() ? "" : ", ") + title;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            dropped.size() == 1 ? "%s did not load: the game could not merge it cleanly, so none of its content is "
                                  "used. Open it to see what went wrong."
                                : "%s did not load: the game could not merge them cleanly, so none of their content "
                                  "is used. Open one to see what went wrong.", names.c_str());
    }
    if (!panel.list.missing.empty()) {
        std::string missing;
        for (const auto& name : panel.list.missing) missing += (missing.empty() ? "" : ", ") + name;
        ImGui::TextDisabled("mods.json also lists folders that are not installed: %s", missing.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // ------------------------------------------------ the list, and under it how its order works
    const float hint = ImGui::GetTextLineHeightWithSpacing();
    const float body = std::max(S(120), height - (ImGui::GetCursorPosY() - top) - hint);
    ImGui::BeginChild("##mod_list", ImVec2(0, body), ImGuiChildFlags_Borders);
    const auto note = [](const char* first, const char* second = nullptr) {
        ImGui::Spacing();
        ImGui::Indent(S(14));
        ImGui::TextDisabled("%s", first);
        if (second) ImGui::TextDisabled("%s", second);
        ImGui::Unindent(S(14));
    };
    if (entries.empty())
        note(panel.list.present ? "No mods installed yet." : "No Mods folder yet.",
             "Use GET MODS to browse Thunderstore, or drop a mod .zip on the window.");
    else if (view.empty()) note("No mods match.");
    const float row = S(68);
    ImGui::BeginDisabled(installing);
    virtual_rows(static_cast<int>(view.size()), [&](int) { return row; }, [&](int position, float tall) {
        installed_row(fonts, panel, installed, view, position, tall, reorder, request);
    });
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::TextDisabled("Mods load top to bottom: where two change the same thing, the higher one wins. "
                        "Changes apply the next time Skate starts.");

    // Ctrl+A and Delete, as in a file list, while nothing is being typed.
    if (!installing && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            for (int at = 0; at < static_cast<int>(view.size()); ++at) panel.marked.insert(shown(at));
        if (!panel.marked.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            request = {Request::Kind::uninstall, std::vector<std::string>(panel.marked.begin(), panel.marked.end())};
    }
    if (request.kind != Request::Kind::none) carry_out(panel, installed, request);
}

// Everything an installed mod says about itself, in a popup over the page.
void mod_overview(const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed, ImVec2 size,
                  bool installing) {
    if (panel.selected < 0 || panel.selected >= static_cast<int>(panel.list.entries.size())) {
        panel.selected = -1;
        return;
    }
    auto& entry = panel.list.entries[static_cast<std::size_t>(panel.selected)];
    const auto& mod = entry.mod;
    const auto* package = package_for(panel.store, mod.name);
    const bool update = package && thunderstore::update_available(*package, installed);

    if (!panel.overview) {
        ImGui::OpenPopup("##mod_overview");
        panel.overview = true;
    }
    const ImVec2 extent(std::min(S(680), size.x - S(80)), std::min(S(560), size.y - S(80)));
    ImGui::SetNextWindowPos(ImVec2((size.x - extent.x) * 0.5f, (size.y - extent.y) * 0.5f));
    ImGui::SetNextWindowSize(extent);
    if (!ImGui::BeginPopupModal("##mod_overview", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        panel.overview = false;        // dismissed with Escape
        panel.selected = -1;
        return;
    }
    const auto close = [&] {
        panel.overview = false;
        panel.selected = -1;
        ImGui::CloseCurrentPopup();
    };
    ImGui::PushFont(fonts.heading);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(mod.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextDisabled("%s", summary(panel, mod).c_str());
    ImGui::Spacing();
    if (update) {
        ImGui::BeginDisabled(installing);
        push_primary_button();
        if (ImGui::Button(("UPDATE TO v" + package->latest().number).c_str(), ImVec2(0, S(34)))) {
            start_store_install(panel, {*package});
            close();
        }
        pop_primary_button();
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button("Open folder", ImVec2(0, S(34)))) open_path(mod.directory);
    if (package && !package->package_url.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Thunderstore page", ImVec2(0, S(34)))) open_url(package->package_url);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(installing);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
    // The confirmation is its own modal, so this one has to go first.
    if (ImGui::Button("Uninstall", ImVec2(0, S(34)))) {
        const auto name = mod.name;
        close();
        panel.confirm_remove = {name};
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::Spacing();

    ImGui::BeginChild("##mod_overview_body",
        ImVec2(0, std::max(S(80), extent.y - ImGui::GetCursorPosY() - S(24) - ImGui::GetFrameHeight())));
    ImGui::PushTextWrapPos(0);
    if (!mod.outdated.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "Outdated: this mod does not load. It was made for another version of Skate (%s). Get an updated "
            "version, or rebuild it with the latest ReSkate Studio.", mod.outdated.c_str());
    if (const auto missing = panel.list.excluded.find(mod.name); missing != panel.list.excluded.end()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "Not loaded: the game could not merge this mod cleanly, so none of it is used. Reinstall the whole "
            "mod folder, or rebuild it with a current ReSkate Studio.");
        if (!missing->second.empty()) ImGui::TextDisabled("%s", missing->second.front().c_str());
    }
    field(fonts, "AUTHOR", mod.author);
    field(fonts, "VERSION", mod.version);
    field(fonts, "DESCRIPTION", mod.description);
    field(fonts, "FOLDER", "Mods\\" + mod.name);
    if (package)
        field(fonts, "THUNDERSTORE", package->full_name + (update ? "  (v" + package->latest().number + " available)"
                                                                  : std::string("  (up to date)")));
    if (!mod.tool.empty() || !mod.built.empty())
        field(fonts, "BUILT WITH", mod.tool + (mod.built.empty() ? "" : (mod.tool.empty() ? "" : ", ") + mod.built));
    std::string levels;
    for (const auto& level : mod.levels) {
        const auto slash = level.rfind('/');
        levels += (levels.empty() ? "" : "\n") + (slash == std::string::npos ? level : level.substr(slash + 1));
    }
    field(fonts, "MAPS", levels);
    std::string parks;
    for (const auto& map : mod.park_maps) parks += (parks.empty() ? "" : ", ") + map;
    field(fonts, "PARKS", parks);
    if (!mod.provides_layout && !mod.provides_levels && mod.park_maps.empty())
        field(fonts, "NOTE", "This folder has no layout.toc or reskate-levels.json, so the game has nothing to load from it.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    ImGui::SetCursorPosY(extent.y - S(24) - ImGui::GetFrameHeight());
    bool enabled = entry.enabled;
    if (ImGui::Checkbox("Loads when Skate starts", &enabled)) {
        entry.enabled = enabled;
        save(panel);
    }
    ImGui::SameLine(extent.x - S(28) - S(110));
    if (ImGui::Button("CLOSE", ImVec2(S(110), 0))) close();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- installing

// What a mod manager shows while it works: everything else dimmed, and one
// panel saying what is being downloaded.
void install_modal(const Fonts& fonts, ModsPanel& panel, ImVec2 size) {
    std::string activity;
    {
        std::lock_guard lock(panel.mutex);
        activity = panel.activity;
    }
    const float fraction = std::clamp(panel.progress.load(), 0.0f, 1.0f);
    const auto time = static_cast<float>(ImGui::GetTime());
    // Cancelling is not instant: the worker notices on its next chunk or file.
    // Say so, or the button looks dead and gets pressed again.
    const bool cancelling = panel.cancel.load();
    // Its own window over the page: the page's own draw list renders under
    // its children, so a panel drawn there would sit beneath the mod list.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##installing", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(0, 0), size, rgba(4, 6, 9, 0.72f));
    const ImVec2 extent(S(540), S(206));
    const ImVec2 origin((size.x - extent.x) * 0.5f, (size.y - extent.y) * 0.5f);
    const ImVec2 end(origin.x + extent.x, origin.y + extent.y);
    skate_theme::rough_rect(draw, origin, end, color::blue, 11, g_scale);
    draw->AddText(fonts.tile, fonts.tile->FontSize, ImVec2(origin.x + S(26), origin.y + S(18)), color::ink,
        cancelling ? "CANCELLING" : "INSTALLING");
    const auto line = cancelling ? std::string("Stopping as soon as the current file is done. "
                                               "The Mods folder is left as it was.")
                                 : activity;
    if (!line.empty())
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(origin.x + S(26), origin.y + S(70)),
            rgba(0, 0, 0, 0.78f), line.c_str(), nullptr, extent.x - S(52));
    skate_theme::striped_bar(draw, ImVec2(origin.x + S(26), origin.y + S(118)),
        ImVec2(end.x - S(26), origin.y + S(134)), fraction, time, g_scale);
    draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(origin.x + S(26), origin.y + S(142)), rgba(0, 0, 0, 0.78f),
        std::format("{:.0f}%", fraction * 100).c_str());
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(rgba(0, 0, 0, 0.38f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(rgba(0, 0, 0, 0.52f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(rgba(0, 0, 0, 0.66f)));
    ImGui::SetCursorScreenPos(ImVec2(end.x - S(26) - S(130), end.y - S(24) - S(34)));
    ImGui::BeginDisabled(cancelling);
    const bool pressed = ImGui::Button(cancelling ? "CANCELLING" : "CANCEL", ImVec2(S(130), S(34)));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    // Escape too: the button is the only way out of this panel otherwise.
    if (!cancelling && (pressed || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        panel.cancel = true;
        logging::write(logging::Level::info, logging::Channel::launcher,
            pressed ? "Mod install cancelled by the Cancel button."
                    : "Mod install cancelled with Escape.");
    }
    ImGui::End();
}

} // namespace

void refresh_mods(Launcher& launcher, ModsPanel& panel) {
    scan(panel, launcher.session());
    refresh_listing(panel, ImGui::GetTime(), true);
    panel.message.clear();
}

void scan(ModsPanel& panel, const launcher_app::Session& session) {
    panel.root = launcher_mods::mods_root(session.paths.directory);
    panel.list = mods::scan_mods(panel.root.parent_path());
    panel.sizes.clear();
    for (const auto& entry : panel.list.entries) panel.sizes[entry.mod.name] = folder_size(entry.mod.directory);
    panel.scanned = true;
    if (panel.selected >= static_cast<int>(panel.list.entries.size())) panel.selected = -1;
    std::erase_if(panel.marked, [&](const std::string& name) {
        return std::none_of(panel.list.entries.begin(), panel.list.entries.end(),
            [&](const mods::ModEntry& entry) { return entry.mod.name == name; });
    });
}

void start_install(ModsPanel& panel, const fs::path& source, bool replace) {
    if (panel.installing) return;
    if (panel.worker.joinable()) panel.worker.join();
    panel.installing = true;
    panel.cancel = false;
    panel.progress = 0;
    panel.message.clear();
    panel.message_error = false;
    {
        std::lock_guard lock(panel.mutex);
        panel.activity = "Installing " + utf8(source.filename().wstring());
    }
    const auto root = panel.root;
    panel.worker = std::thread([&panel, root, source, replace] {
        std::string name, error, conflict;
        try {
            name = launcher_mods::install(root, source, replace,
                [&panel](float fraction) { panel.progress = fraction; }, panel.cancel);
        } catch (const launcher_mods::AlreadyInstalled& existing) {
            conflict = existing.name;
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        std::lock_guard lock(panel.mutex);
        panel.finished = true;
        panel.finished_name = name;
        panel.finished_error = error;
        panel.finished_conflict = conflict;
        panel.finished_source = source;
        panel.activity.clear();
        panel.installing = false;
    });
}

// What PLAY shows instead of launching when the merge left mods out. The game
// would leave them out too, silently, three minutes into a loading screen.
void mods_broken_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel,
                        const std::vector<ModProblem>& problems) {
    const bool one = problems.size() == 1;
    const float rows = static_cast<float>(problems.size()) * S(52);
    const auto frame = begin_panel("##mods_broken_panel", size,
        ImVec2(S(620), std::min(size.y - S(80), S(300) + rows)));
    panel_title(fonts, one ? "A MOD COULD NOT BE MERGED" : "MODS COULD NOT BE MERGED");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(one
        ? "This mod cannot be combined with the game's files, so none of its content would load. Skate would "
          "start without it and never say why."
        : "These mods cannot be combined with the game's files, so none of their content would load. Skate would "
          "start without them and never say why.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeight() + ImGui::GetTextLineHeight() + S(56);
    ImGui::BeginChild("##broken_list", ImVec2(0, frame.y - ImGui::GetCursorPosY() - footer),
        ImGuiChildFlags_Borders);
    for (const auto& problem : problems) {
        ImGui::PushFont(fonts.bold);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::danger));
        ImGui::TextUnformatted(problem.title.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", problem.reason.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("Reinstall the whole mod folder, or rebuild it with a current ReSkate Studio.");
    ImGui::PopFont();

    ImGui::SetCursorPosY(frame.y - S(24) - ImGui::GetFrameHeight());
    // A rejected mods.json is not a mod, so there is nothing to switch off.
    const bool switchable = std::any_of(problems.begin(), problems.end(),
        [](const ModProblem& problem) { return !problem.name.empty(); });
    bool disable = false;
    if (switchable) {
        push_primary_button();
        disable = ImGui::Button(one ? "SWITCH IT OFF AND PLAY" : "SWITCH THEM OFF AND PLAY", ImVec2(S(260), 0));
        pop_primary_button();
        ImGui::SameLine(0, S(8));
    }
    if (disable) {
        // mods.json is the game's own switch, so the next launch skips them
        // without the merge finding out the hard way again.
        const auto root = launcher_mods::mods_root(launcher.session().paths.directory);
        auto list = mods::scan_mods(root.parent_path());
        for (auto& entry : list.entries)
            for (const auto& problem : problems)
                if (entry.mod.name == problem.name) entry.enabled = false;
        try {
            mods::save_mod_order(root, list.entries);
            logging::write(logging::Level::info, logging::Channel::launcher,
                "Mods switched off after a failed merge; launching without them");
        } catch (const std::exception& failure) {
            logging::write(logging::Level::warning, logging::Channel::launcher,
                std::string("Could not switch the mods off: ") + failure.what());
        }
        panel.scanned = false;
        launcher.play_anyway();
    }
    if (ImGui::Button("Open Mod Manager", ImVec2(S(160), 0))) {
        launcher.dismiss_mod_problems();
        ui.mods = true;
        panel.tab = 0;
        panel.scanned = false;
    }
    ImGui::SameLine(frame.x - S(28) - S(110));
    if (ImGui::Button("Play anyway", ImVec2(S(110), 0))) launcher.play_anyway();
    ImGui::End();
}

// What PLAY shows while mods have updates waiting. Playing on the version you
// have is a real choice -- a map you are mid-way through, an update you do not
// trust -- so it stays on offer, and taking it is remembered for the session.
void mods_outdated_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel) {
    if (!panel.scanned) scan(panel, launcher.session());
    const auto installed = installed_versions(panel.list, true);
    const auto pending = updates(panel.store, installed);
    // Nothing left to update (an update ran, or the listing changed): just go.
    if (pending.empty()) {
        ui.mods_update_prompt = false;
        launcher.play();
        return;
    }
    const bool one = pending.size() == 1;
    const auto frame = begin_panel("##mods_outdated_panel", size,
        ImVec2(S(600), std::min(size.y - S(80), S(250) + static_cast<float>(pending.size()) * S(44))));
    panel_title(fonts, one ? "A MOD HAS AN UPDATE" : "MODS HAVE UPDATES");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(one ? "Thunderstore has a newer version of this mod."
                            : "Thunderstore has newer versions of these mods.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeight() + S(44);
    ImGui::BeginChild("##outdated_list", ImVec2(0, frame.y - ImGui::GetCursorPosY() - footer),
        ImGuiChildFlags_Borders);
    for (const auto* package : pending) {
        const auto found = installed.find(thunderstore::folder_for(package->full_name));
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted(package->title().c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (found != installed.end() && !found->second.empty())
            ImGui::TextDisabled("v%s  ->  v%s", found->second.c_str(), package->latest().number.c_str());
        else
            ImGui::TextDisabled("-> v%s", package->latest().number.c_str());
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(frame.y - S(24) - ImGui::GetFrameHeight());
    push_primary_button();
    const bool update = ImGui::Button(one ? "UPDATE IT AND PLAY" : "UPDATE THEM AND PLAY", ImVec2(S(230), 0));
    pop_primary_button();
    if (update) {
        // The install panel lives on the mod manager page, so open it: an
        // update with no sign of it running is the same as a dead button.
        std::vector<thunderstore::Package> packages;
        for (const auto* package : pending) packages.push_back(*package);
        start_store_install(panel, std::move(packages));
        // Only promise the launch if the install really started.
        ui.play_after_install = panel.installing;
        ui.mods_update_prompt = false;
        ui.mods = true;
        panel.tab = 0;
    }
    ImGui::SameLine(0, S(8));
    if (ImGui::Button("Open Mod Manager", ImVec2(S(160), 0))) {
        ui.mods_update_prompt = false;
        ui.mods = true;
        panel.tab = 0;
        panel.scanned = false;
    }
    ImGui::SameLine(frame.x - S(28) - S(110));
    if (ImGui::Button("Play anyway", ImVec2(S(110), 0))) {
        ui.mods_update_prompt = false;
        ui.mods_updates_ignored = true;   // asked and answered; do not nag again
        launcher.play();
    }
    ImGui::End();
}

void mods_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, ModsPanel& panel, HWND window) {
    const auto& session = launcher.session();
    if (!panel.scanned) scan(panel, session);
    collect_install(panel, session);
    refresh_listing(panel, ImGui::GetTime());
    // A page, not a panel: the mod list and the Thunderstore browser both want
    // the whole window. It sits at (0, 0), so window and screen space agree.
    const auto frame = begin_page("##mods_panel", size);
    // Both lists draw mod icons, so both need finished ones uploaded.
    pump_icons(panel);
    auto* draw = ImGui::GetWindowDrawList();
    const bool installing = panel.installing;
    auto& entries = panel.list.entries;
    const auto installed = installed_versions(panel.list);
    const auto pending = updates(panel.store, installed);
    const auto close = [&] {
        ui.mods = false;
        panel.message.clear();
        panel.scanned = false;
        ui.play_after_install = false;   // leaving the page is a change of mind
    };

    // The page covers the main screen's buttons, so it draws its own.
    window_buttons(draw, window, frame);
    const float rail_x = S(28), rail_width = S(236);
    const float content_x = rail_x + rail_width + S(26);
    const float top = S(52), bottom = frame.y - S(24);
    const float title_size = S(44);
    page_title(draw, fonts, ImVec2(rail_x + S(2), top - S(4)), "MOD\nMANAGER", title_size);

    const float rail_top = top + title_size * 2 + S(16);
    ImGui::SetCursorPos(ImVec2(rail_x, rail_top));
    ImGui::BeginChild("##rail", ImVec2(rail_width, bottom - rail_top));
    const bool leave = rail(fonts, panel, window, rail_width, pending, installing);
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(content_x, top));
    ImGui::BeginChild("##content", ImVec2(frame.x - content_x - S(28), bottom - top));
    // With nothing to say, the list runs all the way down to BACK's bottom
    // edge; a message takes two lines off it until it is gone.
    const float status = panel.message.empty() ? S(4) : ImGui::GetTextLineHeight() * 2 + S(12);
    const float body = ImGui::GetWindowHeight() - status;
    if (panel.tab == 1) browse_page(launcher, fonts, panel, body, installing);
    else installed_page(launcher, fonts, panel, installed, body, installing);
    if (!panel.message.empty()) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - status + S(8));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(panel.message_error ? color::danger : color::good), "%s",
            panel.message.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    if (panel.tab == 1) package_overview(fonts, panel, frame, installing);
    else mod_overview(fonts, panel, installed, frame, installing);
    if (leave) close();
    if (!installing && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Escape lets go of the ticked mods first, and leaves the page after that.
        if (panel.tab == 0 && !panel.marked.empty()) panel.marked.clear();
        else close();
    }

    // ------------------------------------------------ confirmations
    // One title for both, so the popup is the same window whether it is one mod or several.
    const char* uninstall = panel.confirm_remove.size() > 1 ? "Uninstall mods###uninstall" : "Uninstall mod###uninstall";
    if (!panel.confirm_remove.empty() && !ImGui::IsPopupOpen(uninstall)) ImGui::OpenPopup(uninstall);
    if (!panel.conflict_name.empty() && !ImGui::IsPopupOpen("Replace mod")) ImGui::OpenPopup("Replace mod");
    ImGui::SetNextWindowSize(ImVec2(S(460), 0));
    if (ImGui::BeginPopupModal(uninstall, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        const auto names = panel.confirm_remove;
        const auto title = [&](const std::string& name) -> const std::string& {
            for (const auto& entry : entries)
                if (entry.mod.name == name) return entry.mod.title;
            return name;
        };
        std::uint64_t bytes{};
        for (const auto& name : names)
            if (const auto found = panel.sizes.find(name); found != panel.sizes.end()) bytes += found->second;
        ImGui::PushTextWrapPos(0);
        if (names.size() == 1) {
            ImGui::Text("Uninstall \"%s\"? Its folder goes to the Recycle Bin.", title(names.front()).c_str());
        } else {
            ImGui::Text("Uninstall these %d mods? Their folders go to the Recycle Bin.", static_cast<int>(names.size()));
            ImGui::Spacing();
            const std::size_t listed = std::min<std::size_t>(names.size(), 8);
            for (std::size_t i = 0; i < listed; ++i) ImGui::TextDisabled("%s", title(names[i]).c_str());
            if (names.size() > listed) ImGui::TextDisabled("and %d more", static_cast<int>(names.size() - listed));
        }
        if (bytes) ImGui::TextDisabled("That frees %s.", size_text(bytes).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0))) { panel.confirm_remove.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        push_primary_button();
        if (ImGui::Button("UNINSTALL", ImVec2(S(110), 0))) {
            // Stops at the first folder that will not go: what went is gone, the rest is untouched.
            std::vector<std::string> removed;
            std::string failure;
            for (const auto& name : names) {
                try {
                    launcher_mods::remove(panel.root, name);
                    removed.push_back(name);
                    logging::write(logging::Level::info, logging::Channel::launcher, "Mod removed: " + name);
                } catch (const std::exception& error) {
                    failure = error.what();
                    break;
                }
            }
            const auto gone = [&](const std::string& name) {
                return std::find(removed.begin(), removed.end(), name) != removed.end();
            };
            const auto first = removed.empty() ? std::string() : title(removed.front());
            std::erase_if(entries, [&](const auto& entry) { return gone(entry.mod.name); });
            if (!removed.empty()) save(panel);
            auto message = !failure.empty() ? failure
                : removed.size() == 1 ? "Uninstalled " + first + ". It is in the Recycle Bin if you want it back."
                : std::format("Uninstalled {} mods. They are in the Recycle Bin if you want them back.", removed.size());
            const bool error = !failure.empty() || panel.message_error;
            if (failure.empty() && panel.message_error) message = panel.message;   // mods.json could not be rewritten
            panel.selected = -1;
            scan(panel, session);
            panel.message = message;
            panel.message_error = error;
            panel.confirm_remove.clear();
            ImGui::CloseCurrentPopup();
        }
        pop_primary_button();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(S(440), 0));
    if (ImGui::BeginPopupModal("Replace mod", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushTextWrapPos(0);
        ImGui::Text("\"%s\" is already installed. Replace it? The installed copy goes to the Recycle Bin.",
            panel.conflict_name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0))) {
            panel.conflict_name.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        push_primary_button();
        if (ImGui::Button("REPLACE", ImVec2(S(110), 0))) {
            const auto source = panel.conflict_source;
            panel.conflict_name.clear();
            ImGui::CloseCurrentPopup();
            start_install(panel, source, true);
        }
        pop_primary_button();
        ImGui::EndPopup();
    }
    g_drag_allowed = !ImGui::IsAnyItemHovered() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::End();
    if (installing) install_modal(fonts, panel, frame);
    // collect_install at the top of this frame has already taken the result, so
    // an install that is no longer running is settled: go and play, as the
    // button that sent us here said it would.
    if (ui.play_after_install && !panel.installing) {
        ui.play_after_install = false;
        if (panel.message_error) {
            // It failed. Stay on the page, with the reason still on screen.
            logging::write(logging::Level::warning, logging::Channel::launcher,
                "Not launching after the mod update: " + panel.message);
        } else {
            close();
            launcher.play();
        }
    }
}

} // namespace dingosdk::launcher_gui::detail
