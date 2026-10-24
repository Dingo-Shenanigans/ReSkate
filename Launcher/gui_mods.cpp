#include "gui_internal.h"

#include <imgui_internal.h>

#include "mod_manager.h"
#include "Engine/Vfs/builtin_packs.h"

#include "Engine/Core/Log/logging.h"

#include <cstring>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cctype>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <system_error>

using Microsoft::WRL::ComPtr;

// The Mods page: a nav rail on the left, and a list where every mod is a
// full-width row that opens where it sits. MY MODS is install order and
// enabled state; GET MODS is Thunderstore (gui_mods_browse.cpp).
namespace dingosdk::launcher_gui::detail {
namespace {

// The list changed: check it for conflicts again once it has been still for a moment, so a mod
// dragged about is checked once, where it lands.
void want_conflict_check(ModsPanel& panel, double delay = 0.5) {
    panel.conflicts.due = ImGui::GetTime() + delay;
    ++panel.conflicts.asked;
}

// Takes in a finished conflict check, and starts the one asked for once it is due and none runs.
void pump_conflict_check(ModsPanel& panel) {
    auto& check = panel.conflicts;
    {
        std::lock_guard lock(panel.mutex);
        if (check.pending.ready) {
            check.pending.ready = false;
            check.rows = std::move(check.pending.rows);
            check.order = std::move(check.pending.order);
            check.checked = check.pending.asked;
            check.ready = true;
            // A mod left out for a reason of its own (it could not be merged) is out whoever wins.
            std::set<std::string, std::less<>> out;
            for (const auto& [name, problems] : panel.list.excluded)
                if (problems.empty() || !problems.front().starts_with("conflicts with ")) out.insert(name);
            check.losses = mods::conflict_losses(check.order, check.rows, out);
            check.beats.clear();
            for (const auto& [lost, loss] : check.losses) check.beats[loss.winner].push_back(lost);
        }
    }
    if (!check.running && check.worker.joinable()) check.worker.join();
    // (Not while mods are being installed or updated: it reads the files they replace.)
    if (check.due < 0 || ImGui::GetTime() < check.due || check.running || panel.root.empty() || panel.installing) return;
    check.due = -1;
    // The worker gets its own copy of the mods: the list can change while it runs.
    auto snapshot = std::make_shared<std::vector<mods::Mod>>();
    for (const auto& entry : panel.list.entries)
        if (entry.enabled && entry.mod.provides_layout && entry.mod.outdated.empty()) snapshot->push_back(entry.mod);
    if (!check.checker) check.checker = std::make_unique<mods::ConflictChecker>(panel.root.parent_path());
    check.running = true;
    try {
        check.worker = std::thread([&panel, snapshot, asked = check.asked] {
            std::vector<const mods::Mod*> order;
            std::vector<std::string> names;
            for (const auto& mod : *snapshot) {
                order.push_back(&mod);
                names.push_back(mod.name);
            }
            // Half the cores, under normal priority: the launcher stays quick while it runs.
            auto rows = panel.conflicts.checker->check(order, std::max(1U, std::thread::hardware_concurrency() / 2), true);
            {
                std::lock_guard lock(panel.mutex);
                panel.conflicts.pending = {true, asked, std::move(rows), std::move(names)};
            }
            panel.conflicts.running = false;
        });
    } catch (const std::system_error&) {
        check.running = false;
    }
}

void save(ModsPanel& panel) {
    try {
        mods::save_mod_order(panel.root, panel.list.entries);
        want_conflict_check(panel);
        panel.list.issue.clear();
        panel.message = "Saved. Changes apply the next time Skate starts.";
        panel.message_error = false;
    } catch (const std::exception& failure) {
        panel.message = failure.what();
        panel.message_error = true;
    }
}

// Picks up a finished install: rescan and say what came in, or ask to replace. (Its details are not
// opened: the player may be browsing on, and finds the mod in the list.)
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
    panel.message_error = false;
    if (!panel.finished_note.empty()) {
        panel.message = panel.finished_note;   // Thunderstore installs say what they did and log it themselves
        panel.finished_note.clear();
        return;
    }
    panel.message = "Installed " + panel.finished_name + ". It loads the next time Skate starts.";
    logging::write(logging::Level::info, logging::Channel::launcher, "Mod installed: " + panel.finished_name);
}

// The picker for a mod to install: a .zip, or a folder.
fs::path pick(HWND owner, bool folder) {
    return folder ? pick_path(owner, true, L"Choose a mod folder to install", nullptr, nullptr)
                  : pick_path(owner, false, L"Choose a mod .zip to install", L"Mod archive (*.zip)", L"*.zip");
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
    // A controller starts on the open tab's tile.
    if (panel.tab == 0) default_focus();
    if (nav_tile(fonts, width, "GET MODS", panel.tab == 1,
            store.loaded ? std::to_string(store.packages.size()) : std::string("..."), false,
            store.loaded ? std::string() : std::string("Loading the Thunderstore listing")))
        panel.tab = 1;
    if (panel.tab == 1) default_focus();
    if (nav_tile(fonts, width, "PROFILES", panel.tab == 2,
            panel.profiles_loaded ? std::to_string(panel.profiles.list.size()) : std::string(), false, std::string()))
        panel.tab = 2;
    if (panel.tab == 2) default_focus();

    // UPDATE ALL is not here: it sits over the MY MODS list, next to the rows it updates.
    ImGui::Dummy(ImVec2(0, S(8)));
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

constexpr std::array<const char*, 8> filter_names{"All mods", "Enabled", "Disabled", "Maps", "Game data", "Updates",
                                                  "Problems", "Conflicts"};
constexpr std::array<const char*, 3> order_names{"Load order", "Name", "Largest first"};
// What a row dragged by its handle carries: its place in the load order.
constexpr const char* dragged_row = "RESKATE_MOD_ROW";

// Something the page was asked to do to one mod or to all the ticked ones.
// Carried out once the list is drawn: most of it reorders or shrinks the list.
struct Request {
    enum class Kind { none, enable, disable, top, bottom, up, down, move, place, update, uninstall };
    Kind kind{Kind::none};
    std::vector<std::string> mods;       // folder names
    std::string target;                  // move: the mod they go next to
    bool above{};                        // move: above it, not below
    int position{};                      // place: where in the load order, counted from 1
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

// Left out at the last launch for a reason of its own (it could not be merged), not a conflict.
bool broken(const ModsPanel& panel, const std::string& name) {
    const auto found = panel.list.excluded.find(name);
    return found != panel.list.excluded.end() && (found->second.empty() || !found->second.front().starts_with("conflicts with "));
}

// The mod `name` loses a conflict to, so that only that one loads: as the latest check of the list
// found it, or before one is in, as the last launch did (mod_catalog.cpp). Empty when none.
std::string conflict_winner(const ModsPanel& panel, const std::string& name) {
    if (panel.conflicts.ready) {
        const auto found = panel.conflicts.losses.find(name);
        return found == panel.conflicts.losses.end() ? std::string{} : found->second.winner;
    }
    constexpr std::string_view prefix = "conflicts with ";
    const auto found = panel.list.excluded.find(name);
    if (found == panel.list.excluded.end() || found->second.empty() || !found->second.front().starts_with(prefix)) return {};
    const auto& problem = found->second.front();
    return problem.substr(prefix.size(), problem.find(':') - prefix.size());
}
// What that conflict is over, for a player: "both change menus and skater setup (52 files)".
// Empty before a check is in.
std::string conflict_reason(const ModsPanel& panel, const std::string& name) {
    const auto found = panel.conflicts.losses.find(name);
    if (!panel.conflicts.ready || found == panel.conflicts.losses.end()) return {};
    std::vector<std::string> areas;
    std::size_t files{};
    for (const auto* row : found->second.rows) {
        files += row->assets;
        for (const auto& area : row->areas)
            if (std::ranges::find(areas, area) == areas.end()) areas.push_back(area);
    }
    std::string text;
    for (std::size_t at = 0; at < areas.size(); ++at)
        text += (at == 0 ? "" : at + 1 == areas.size() ? " and " : ", ") + areas[at];
    return "both change " + (text.empty() ? std::string("the same game files") : text) +
           std::format(" ({} {})", files, files == 1 ? "file" : "files");
}
// The mods `name` wins over in the latest check: they are left out for it.
const std::vector<std::string>* conflict_beats(const ModsPanel& panel, const std::string& name) {
    const auto found = panel.conflicts.beats.find(name);
    return panel.conflicts.ready && found != panel.conflicts.beats.end() ? &found->second : nullptr;
}
// A mod's title, by its folder name.
std::string title_of(const ModsPanel& panel, const std::string& name) {
    for (const auto& entry : panel.list.entries)
        if (entry.mod.name == name) return entry.mod.title.empty() ? name : entry.mod.title;
    return name;
}
// The conflicts the player has settled in the Resolve conflicts popup: of each pair, the mod they
// chose to load, kept in Mods/.reskate-conflict-choices.txt as "loads<TAB>left out" lines. A
// conflict that comes out as chosen is not asked about again; one that comes out the other way (a
// mod moved past the other) or is new is.
constexpr wchar_t conflict_choices_file[] = L".reskate-conflict-choices.txt";

void read_conflict_choices(ModsPanel& panel) {
    panel.conflict_choices.clear();
    std::ifstream in(panel.root / conflict_choices_file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab != std::string::npos && tab > 0 && tab + 1 < line.size())
            panel.conflict_choices.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
}

void write_conflict_choices(const ModsPanel& panel) {
    std::ofstream out(panel.root / conflict_choices_file, std::ios::binary | std::ios::trunc);
    for (const auto& [loads, left_out] : panel.conflict_choices) out << loads << '\t' << left_out << '\n';
}

// One conflict as the latest check has it: the mod that loads, the one left out for it, and what
// they both change.
struct ConflictPair {
    std::string loads, left_out;
    const mods::MergeReport::Conflict* row{};
};
// Every conflict that leaves a mod out, the highest left-out mod first.
std::vector<ConflictPair> conflict_pairs(const ModsPanel& panel) {
    std::vector<ConflictPair> pairs;
    if (!panel.conflicts.ready) return pairs;
    for (const auto& name : panel.conflicts.order)
        if (const auto loss = panel.conflicts.losses.find(name); loss != panel.conflicts.losses.end())
            for (const auto* row : loss->second.rows) pairs.push_back({row->kept, name, row});
    return pairs;
}
bool settled(const ModsPanel& panel, const ConflictPair& pair) {
    return panel.conflict_choices.contains({pair.loads, pair.left_out});
}
// Whether a mod left out for a conflict is so because the player chose it (every conflict it loses
// is settled). Before a check is in, none is.
bool left_out_by_choice(const ModsPanel& panel, const std::string& name) {
    bool any{};
    for (const auto& pair : conflict_pairs(panel)) {
        if (pair.left_out != name) continue;
        if (!settled(panel, pair)) return false;
        any = true;
    }
    return any;
}
// What two mods both change, for a player: "Both change scripts (6 files)".
std::string pair_reason(const ConflictPair& pair) {
    std::string areas;
    for (std::size_t at = 0; at < pair.row->areas.size(); ++at)
        areas += (at == 0 ? "" : at + 1 == pair.row->areas.size() ? " and " : ", ") + pair.row->areas[at];
    return "Both change " + (areas.empty() ? std::string("the same game files") : areas) +
           std::format(" ({} {})", pair.row->assets, pair.row->assets == 1 ? "file" : "files");
}

// What a mod brings, for the list's Type column: what says most about it, and on hover all of it.
struct Kind {
    std::string label, tip;
};
Kind kind_of(const mods::Mod& mod) {
    const auto parts = mods::mod_contents(mod);
    if (parts.empty()) return {"Nothing", "Holds nothing ReSkate loads"};
    std::string all;
    for (std::size_t at = 0; at < parts.size(); ++at) {
        auto part = parts[at];
        if (at) part[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(part[0])));
        all += (at == 0 ? "" : at + 1 == parts.size() ? " and " : ", ") + part;
    }
    return {parts.size() == 1 ? parts.front() : parts.front() + " +", "Holds: " + all};
}

// The rows the list shows, as places in the load order: what the search and the
// filter leave, in the order asked for.
// An installed mod's categories, as Thunderstore lists its package ("No category" when it lists none,
// or does not list the mod at all).
constexpr const char* no_category = "No category";
std::vector<std::string> categories_of(const ModsPanel& panel, const mods::Mod& mod) {
    const auto found = panel.categories_by_folder.find(mod.name);
    if (found == panel.categories_by_folder.end() || found->second.empty()) return {no_category};
    return found->second;
}

std::vector<int> visible_mods(const ModsPanel& panel, const thunderstore::Installed& installed, const thunderstore::CategoryFilter& categories) {
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
        case 6: keep = !mod.outdated.empty() || broken(panel, mod.name); break;
        case 7: keep = mod.outdated.empty() && (!conflict_winner(panel, mod.name).empty() || conflict_beats(panel, mod.name)); break;
        default: break;
        }
        if (keep && !categories.empty()) keep = thunderstore::shown_by(categories_of(panel, mod), categories);
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

// The mods a drag of the row at `index` moves: every ticked one when it is ticked among others,
// in their load order, else that one.
std::vector<std::string> dragged_mods(const ModsPanel& panel, int index) {
    const auto& entries = panel.list.entries;
    if (index < 0 || index >= static_cast<int>(entries.size())) return {};
    const auto& name = entries[static_cast<std::size_t>(index)].mod.name;
    if (!panel.marked.contains(name) || panel.marked.size() < 2) return {name};
    std::vector<std::string> names;
    for (const auto& entry : entries)
        if (panel.marked.contains(entry.mod.name)) names.push_back(entry.mod.name);
    return names;
}

void carry_out(ModsPanel& panel, const thunderstore::Installed& installed, const Request& request) {
    auto& entries = panel.list.entries;
    const auto asked = [&](const mods::ModEntry& entry) {
        return std::find(request.mods.begin(), request.mods.end(), entry.mod.name) != request.mods.end();
    };
    const auto count = request.mods.size();
    const auto saved = [&](const std::string& one, const std::string& many) {
        save(panel);
        if (!panel.message_error)
            panel.message = (count == 1 ? one : std::format("{} {}", count, many)) +
                            " Changes apply the next time Skate starts.";
    };
    // The open overview follows its mod through a reorder.
    const std::string open = panel.selected >= 0 && panel.selected < static_cast<int>(entries.size())
        ? entries[static_cast<std::size_t>(panel.selected)].mod.name : std::string();
    // Takes the asked mods out of the list and puts them back, in their order, at `at` (a place in
    // what is left: 0 is the top).
    const auto put = [&](auto&& at) {
        std::vector<mods::ModEntry> moving, rest;
        for (auto& entry : entries) (asked(entry) ? moving : rest).push_back(std::move(entry));
        const auto place = std::min<std::size_t>(at(rest), rest.size());
        rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(place), std::make_move_iterator(moving.begin()),
                    std::make_move_iterator(moving.end()));
        entries = std::move(rest);
    };
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
    case Request::Kind::move: {
        // (Dropped on one of the mods being moved: nothing to do.)
        if (std::ranges::find(request.mods, request.target) != request.mods.end()) break;
        const auto target = title_of(panel, request.target);
        put([&](const std::vector<mods::ModEntry>& rest) {
            const auto found = std::ranges::find_if(rest, [&](const mods::ModEntry& entry) { return entry.mod.name == request.target; });
            return static_cast<std::size_t>(found - rest.begin()) + (found != rest.end() && !request.above ? 1 : 0);
        });
        saved(std::format("Moved {} {} {}.", title_of(panel, request.mods.front()), request.above ? "above" : "below", target),
              std::format("mods moved {} {}.", request.above ? "above" : "below", target));
        break;
    }
    case Request::Kind::place:
        put([&](const std::vector<mods::ModEntry>&) { return static_cast<std::size_t>(std::max(request.position, 1) - 1); });
        saved(std::format("Moved to position {}.", std::max(request.position, 1)), "mods moved.");
        break;
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

// Over the list while Thunderstore has updates, as the conflicts bar is: how many and what they
// download, each mod's old -> new version, a switch to show only those mods, and UPDATE ALL. Mods
// with an update are listed first, so it sits right above the rows it updates.
void updates_bar(const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed,
                 const std::vector<const thunderstore::Package*>& pending) {
    if (pending.empty()) return;
    std::uint64_t download{};
    for (const auto* package : pending) download += package->latest().file_size;
    // (The first few by name: the list itself shows every one.)
    constexpr std::size_t named = 6;
    std::string names;
    for (std::size_t i = 0; i < pending.size() && i < named; ++i) {
        const auto* package = pending[i];
        const auto have = installed.find(thunderstore::folder_for(package->full_name));
        names += (names.empty() ? "" : ",  ") + package->title() +
                 (have != installed.end() && !have->second.empty() ? " " + have->second + " \xe2\x86\x92 " : " \xe2\x86\x92 ") +
                 package->latest().number;
    }
    if (pending.size() > named) names += std::format(",  and {} more", pending.size() - named);
    const auto blue = ImGui::ColorConvertU32ToFloat4(color::blue);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(blue.x, blue.y, blue.z, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(blue.x, blue.y, blue.z, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
    ImGui::BeginChild("##updates", ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    const float button = S(160), show = S(130), gap = S(8);
    const float top = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - button - gap - show - S(16));
    ImGui::PushFont(fonts.bold);
    auto heading = std::format("{} {} WAITING", pending.size(), pending.size() == 1 ? "UPDATE" : "UPDATES");
    if (download) heading += "  /  " + size_text(download);
    ImGui::TextColored(blue, "%s", heading.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", names.c_str());
    ImGui::PopTextWrapPos();
    const float bottom = ImGui::GetCursorPosY();
    const float button_y = top + (bottom - top - S(34)) * 0.5f;
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - button - gap - show, button_y));
    const bool only = panel.filter == 5;
    if (ImGui::Button(only ? "Show all" : "Show only these", ImVec2(show, S(34)))) panel.filter = only ? 0 : 5;
    ImGui::SameLine(0, gap);
    push_primary_button();
    const bool update_all = ImGui::Button(std::format("UPDATE ALL ({})", pending.size()).c_str(), ImVec2(button, S(34)));
    pop_primary_button();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", download ? ("Downloads " + size_text(download) + " from Thunderstore").c_str()
                                         : "Downloads the new versions from Thunderstore");
    if (update_all) {
        std::vector<thunderstore::Package> packages;
        for (const auto* package : pending) packages.push_back(*package);
        start_store_install(panel, std::move(packages));
    }
    ImGui::SetCursorPosY(std::max(bottom, button_y + S(34)));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// Where the table's columns are: from a row's left edge on the left, and the widths of those
// at its right, which are laid out from the right edge in.
struct Columns {
    float tick = S(12), grip = S(44), place = S(64), on = S(108), icon = S(162), name = S(202);
    float more = S(28), status = S(156), type = S(92), update = S(84);
};
// Where the right-hand columns of a row `width` wide start, from its left edge.
struct RightColumns {
    float more, status, type;
};
RightColumns right_columns(float width) {
    const Columns c;
    RightColumns r{};
    r.more = width - S(10) - c.more;
    r.status = r.more - S(12) - c.status;
    r.type = r.status - S(12) - c.type;
    return r;
}

// The labels over the table's columns.
void table_header(const Fonts& fonts) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const Columns c;
    const auto r = right_columns(width);
    const auto label = [&](float x, const char* text) {
        draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(start.x + x, start.y), color::muted, text);
    };
    label(c.place, "#");
    label(c.on, "ON");
    label(c.icon, "MOD");
    label(r.type, "TYPE");
    label(r.status, "STATUS");
    ImGui::Dummy(ImVec2(width, fonts.caption->FontSize + S(6)));
}

// One installed mod, a row of the table. The tick on the left picks it for the bar above the
// list, the handle beside it drags it to another place in the load order, its number moves it to
// a place typed in, the switch is whether it loads, and a click anywhere else opens everything
// the mod says about itself.
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
    begin_row();
    const bool pressed = list_row("##row", width, tall, ticked);
    bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    row_buttons();
    if (pressed) {
        // Ctrl and Shift pick, as they do in a file list; a plain click opens the mod.
        if (ImGui::GetIO().KeyShift) mark(panel, view, position, true, true);
        else if (ImGui::GetIO().KeyCtrl) mark(panel, view, position, !ticked, false);
        else {
            panel.selected = index;
            panel.overview = false;
        }
    }
    const auto* package = package_for(panel.store, mod.name);
    const bool update = package && thunderstore::update_available(*package, installed);
    const Columns c;
    const auto r = right_columns(width);
    const float middle = start.y + tall * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(start.x + c.tick, middle - ImGui::GetFrameHeight() * 0.5f));
    bool tick = ticked;
    if (ImGui::Checkbox("##pick", &tick)) mark(panel, view, position, tick, ImGui::GetIO().KeyShift);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tick mods to change several at once.\nShift-click ticks every mod in between.");

    // The handle: six dots, brighter while it can be grabbed.
    {
        const ImVec2 at(start.x + c.grip, start.y);
        ImGui::SetCursorScreenPos(at);
        ImGui::InvisibleButton("##grip", ImVec2(S(16), tall));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && reorder) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (hovered && !ImGui::GetDragDropPayload())
            ImGui::SetTooltip("%s", reorder ? "Drag to move it in the load order. Ticked mods move together."
                                            : "To drag mods into a new order, show the list in load order.");
        // Dragged, the row itself follows the pointer (installed_page draws it there): no tooltip.
        if (reorder && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload(dragged_row, &index, sizeof index);
            if (panel.drag_name != mod.name) {
                panel.drag_name = mod.name;
                panel.drag_grab = ImGui::GetMousePos().y - start.y;
            }
            ImGui::EndDragDropSource();
        }
        const ImU32 dots = !reorder ? rgba(255, 255, 255, 0.12f) : hovered ? color::text : color::muted;
        for (int row = -1; row <= 1; ++row)
            for (int column = 0; column < 2; ++column)
                draw->AddCircleFilled(ImVec2(at.x + S(5) + column * S(6), middle + row * S(6)), S(1.6f), dots);
    }

    // Its place in the load order, whatever order the list is shown in: click to type another.
    {
        ImGui::SetCursorScreenPos(ImVec2(start.x + c.place, start.y));
        if (ImGui::InvisibleButton("##place", ImVec2(S(40), tall)) && reorder) {
            panel.place = index + 1;
            ImGui::OpenPopup("##place_popup");
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetTooltip("%s", reorder ? "Its place in the load order. Click to move it to another."
                                            : "Its place in the load order.");
        const auto number = std::to_string(index + 1);
        draw->AddText(fonts.body, fonts.body->FontSize,
            ImVec2(start.x + c.place, middle - fonts.body->FontSize * 0.5f), hovered && reorder ? color::text : color::muted,
            number.c_str());
        if (ImGui::BeginPopup("##place_popup")) {
            ImGui::TextUnformatted("Move to position");
            ImGui::SetNextItemWidth(S(110));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool entered = ImGui::InputInt("##position", &panel.place, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Move") || entered) {
                request = {Request::Kind::place, {mod.name}, {}, false,
                           std::clamp(panel.place, 1, static_cast<int>(panel.list.entries.size()))};
                ImGui::CloseCurrentPopup();
            }
            ImGui::TextDisabled("1 is the top: it wins over every mod below it.");
            ImGui::EndPopup();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x + c.on, middle - S(11)));
    bool enabled = entry.enabled;
    if (toggle("##enabled", &enabled))
        request = {enabled ? Request::Kind::enable : Request::Kind::disable, {mod.name}};
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(entry.enabled ? "Enabled: loads when Skate starts" : "Disabled: stays installed, does not load");

    // The same icon the store shows, so a mod looks like itself on both pages.
    mod_icon(panel, package, ImVec2(start.x + c.icon, middle - S(15)), S(30));

    // Right to left: the menu, how it stands, and what it is.
    ImGui::SetCursorScreenPos(ImVec2(start.x + r.more, middle - c.more * 0.5f));
    if (more_button("##more", c.more)) menu = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("More");

    const auto kind = kind_of(mod);
    draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(start.x + r.type, middle - fonts.body->FontSize * 0.5f),
        color::muted, kind.label.c_str());
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(start.x + r.type, start.y), ImVec2(start.x + r.status, start.y + tall)))
        ImGui::SetTooltip("%s", kind.tip.c_str());

    // How it stands: a pill, and on hover why.
    {
        const ImVec2 at(start.x + r.status, middle - (fonts.caption->FontSize + S(8)) * 0.5f);
        std::string label, tip;
        ImU32 fill{};
        const auto winner = conflict_winner(panel, mod.name);
        if (!mod.outdated.empty()) {
            label = "OUTDATED", fill = color::warning;
            tip = "Made for another version of Skate (" + mod.outdated + "): it does not load.";
        } else if (!entry.enabled) {
            label = "OFF";
            tip = "Switched off: stays installed, does not load.";
        } else if (!winner.empty()) {
            const bool chosen = left_out_by_choice(panel, mod.name);
            label = chosen ? "LEFT OUT" : "CONFLICT", fill = chosen ? color::tile_grey : color::warning;
            const auto reason = conflict_reason(panel, mod.name);
            tip = (chosen ? "Left out: you chose " + title_of(panel, winner) + " to load instead."
                          : "Left out: it loses to " + title_of(panel, winner) + ", which is higher in the load order.") +
                  (reason.empty() ? std::string() : " They " + reason + ".") +
                  (chosen ? "" : " Resolve conflicts to choose which loads.");
        } else if (broken(panel, mod.name)) {
            label = "NOT LOADED", fill = color::danger;
            tip = "The game could not merge it cleanly, so none of it is used. Open it to see why.";
        }
        if (!label.empty() && fill) badge(draw, fonts, at, label, fill, fill == color::tile_grey ? color::text : color::ink);
        else if (!label.empty())
            draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(at.x, at.y + S(4)), color::muted, label.c_str());
        if (!tip.empty() && ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(ImVec2(start.x + r.status, start.y), ImVec2(start.x + r.more, start.y + tall)))
            ImGui::SetTooltip("%s", tip.c_str());
    }

    // The name, who made it, and its version: with an update waiting, old -> new in blue and the
    // UPDATE button right after it, so the button sits with the change it makes. A name too long
    // for the row is cut short before the button rather than pushing it into the Type column.
    {
        const float x = start.x + c.name, limit = start.x + r.type - S(12);
        const auto to = update ? (mod.version.empty() ? std::string() : "v" + mod.version + " ") + "\xe2\x86\x92 v" +
                                     package->latest().number
                               : std::string();
        const auto by = mod.author.empty() ? std::string()
                                           : "by " + mod.author + (mod.version.empty() || update ? std::string() : "  v" + mod.version);
        const auto wide = [&](ImFont* font, const std::string& text) {
            return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, text.c_str()).x;
        };
        float name_end = limit;
        if (update) {
            const float text = wide(fonts.bold, mod.title) + S(10) + (by.empty() ? 0.0f : wide(fonts.body, by) + S(10)) +
                               wide(fonts.body, to);
            const float button_x = std::min(x + text + S(12), limit - c.update);
            ImGui::SetCursorScreenPos(ImVec2(button_x, middle - S(13)));
            push_primary_button();
            if (ImGui::Button("UPDATE", ImVec2(c.update, S(26)))) request = {Request::Kind::update, {mod.name}};
            pop_primary_button();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thunderstore has v%s", package->latest().number.c_str());
            name_end = button_x - S(12);
        }
        const ImVec4 clip(x, start.y, name_end, start.y + tall);
        const float y = middle - fonts.bold->FontSize * 0.5f, body_y = middle - fonts.body->FontSize * 0.5f;
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(x, y), entry.enabled ? color::text : color::muted,
                      mod.title.c_str(), nullptr, 0, &clip);
        float after = x + fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, mod.title.c_str()).x + S(10);
        if (!by.empty()) {
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(after, body_y), color::muted, by.c_str(), nullptr, 0, &clip);
            after += wide(fonts.body, by) + S(10);
        }
        if (update)
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(after, body_y), color::blue, to.c_str(), nullptr, 0, &clip);
    }
    end_row();

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
        if (!group) {
            if (ImGui::MenuItem("Move up", nullptr, false, index > 0)) ask(Request::Kind::up);
            if (ImGui::MenuItem("Move down", nullptr, false, index + 1 < static_cast<int>(panel.list.entries.size())))
                ask(Request::Kind::down);
        }
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

// Over the list: whether conflicts wait for the player, with the button that opens the popup to
// settle them; once all are settled, a quiet line to look at them again.
void conflicts_bar(const Fonts& fonts, ModsPanel& panel) {
    const auto& check = panel.conflicts;
    const bool checking = check.running || check.asked != check.checked;
    if (!check.ready) {
        if (checking) ImGui::TextDisabled("Checking your mods for conflicts...");
        return;
    }
    const auto pairs = conflict_pairs(panel);
    if (pairs.empty()) return;
    const auto open = static_cast<std::size_t>(std::count_if(pairs.begin(), pairs.end(),
        [&](const ConflictPair& pair) { return !settled(panel, pair); }));
    if (!open) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", std::format("{} {} resolved: the mods you chose load.", pairs.size(),
                                              pairs.size() == 1 ? "conflict" : "conflicts").c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Review")) panel.resolving = true;
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(rgba(255, 190, 60, 0.07f)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(rgba(255, 190, 60, 0.35f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
    ImGui::BeginChild("##conflicts", ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    const float button = S(190);
    const float top = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - button - S(16));
    ImGui::PushFont(fonts.bold);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning), "%s",
        std::format("{} {} TO RESOLVE", open, open == 1 ? "CONFLICT" : "CONFLICTS").c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Some of your mods change the same parts of the game in ways that can't be combined, so only one "
                        "of each pair can load. Choose which.");
    ImGui::PopTextWrapPos();
    const float bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - button, top + (bottom - top - S(34)) * 0.5f));
    push_primary_button();
    if (ImGui::Button("RESOLVE CONFLICTS", ImVec2(button, S(34)))) panel.resolving = true;
    pop_primary_button();
    ImGui::SetCursorPosY(std::max(bottom, top + S(34)));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// Mods that clash over the same part of the game, as the popup shows them: every mod linked to
// another by a conflict, whichever way it came out, with what they change.
struct ConflictGroup {
    std::vector<std::string> mods;
    std::vector<std::string> areas;
    bool fresh{};   // a conflict in it the player has not settled
};
std::vector<ConflictGroup> conflict_groups(const ModsPanel& panel) {
    const auto& check = panel.conflicts;
    std::map<std::string, std::string, std::less<>> parent;
    const auto root = [&](std::string name) {
        while (parent.contains(name) && parent.at(name) != name) name = parent.at(name);
        return name;
    };
    const auto enabled = [&](const std::string& name) { return std::ranges::find(check.order, name) != check.order.end(); };
    for (const auto& row : check.rows) {
        if (!enabled(row.kept) || !enabled(row.lost)) continue;
        parent.try_emplace(row.kept, row.kept);
        parent.try_emplace(row.lost, row.lost);
        if (const auto a = root(row.kept), b = root(row.lost); a != b) parent[b] = a;
    }
    std::map<std::string, ConflictGroup> groups;
    for (const auto& name : check.order)
        if (parent.contains(name)) groups[root(name)].mods.push_back(name);
    for (const auto& row : check.rows) {
        if (!parent.contains(row.kept)) continue;
        auto& group = groups[root(row.kept)];
        for (const auto& area : row.areas)
            if (std::ranges::find(group.areas, area) == group.areas.end()) group.areas.push_back(area);
    }
    for (const auto& pair : conflict_pairs(panel))
        if (!settled(panel, pair) && parent.contains(pair.left_out)) groups[root(pair.left_out)].fresh = true;
    std::vector<ConflictGroup> result;
    for (auto& [first, group] : groups) result.push_back(std::move(group));
    return result;
}

// The popup that settles them: each clash as a card with its mods, a row each in an order that
// stays put while the popup is open, saying which loads and which is left out. "Load this one"
// moves a left-out mod above the others it clashes with; only the words beside the mods change.
// What the player leaves as it is counts as chosen (Done), so the list asks again only about a
// conflict that is new or has come out the other way since.
void resolve_popup(const Fonts& fonts, ModsPanel& panel, Request& request) {
    if (panel.resolving) {
        panel.resolve_order = panel.conflicts.order;
        ImGui::OpenPopup("##resolve_conflicts");
        panel.resolving = false;
    }
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 extent(std::min(S(860), viewport->Size.x - S(80)), std::min(S(680), viewport->Size.y - S(60)));
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + (viewport->Size.x - extent.x) * 0.5f,
                                   viewport->Pos.y + (viewport->Size.y - extent.y) * 0.5f));
    ImGui::SetNextWindowSize(extent);
    if (!ImGui::BeginPopupModal("##resolve_conflicts", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    // Where each mod stood when the popup opened: rows and cards keep that order.
    const auto place = [&](const std::string& name) {
        const auto found = std::ranges::find(panel.resolve_order, name);
        return static_cast<std::size_t>(found - panel.resolve_order.begin());
    };
    auto groups = conflict_groups(panel);
    for (auto& group : groups)
        std::ranges::sort(group.mods, [&](const std::string& a, const std::string& b) { return place(a) < place(b); });
    std::ranges::sort(groups, [&](const ConflictGroup& a, const ConflictGroup& b) { return place(a.mods.front()) < place(b.mods.front()); });
    const bool checking = panel.conflicts.running || panel.conflicts.asked != panel.conflicts.checked;

    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted("RESOLVE CONFLICTS");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Each card is mods that change the same part of the game in ways that can't be combined. "
                        "Pick the one you want with Load this one: it moves above the others, and they are left out. "
                        "Your choice is kept until the load order changes.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeight() + S(24);
    ImGui::BeginChild("##resolve_list", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_NavFlattened);
    if (groups.empty())
        ImGui::TextDisabled("%s", checking ? "Checking again..." : "No conflicts between your enabled mods.");
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = S(14), row = S(40), head = S(30);
    for (const auto& group : groups) {
        ImGui::PushID(group.mods.front().c_str());
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float tall = pad * 2 + head + row * static_cast<float>(group.mods.size());
        card(draw, start, ImVec2(width, tall));
        float x = start.x + pad;
        if (group.fresh) {
            badge(draw, fonts, ImVec2(x, start.y + pad), "NEW", color::warning, color::ink);
            x += badge_width(fonts, "NEW") + S(10);
        }
        std::string areas;
        for (std::size_t at = 0; at < group.areas.size(); ++at)
            areas += (at == 0 ? "" : at + 1 == group.areas.size() ? " and " : ", ") + group.areas[at];
        const auto heading = "They clash over " + (areas.empty() ? std::string("the same game files") : areas) +
                             (group.mods.size() == 2 ? ": only one can load." : ": only mods whose changes agree can load together.");
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(x, start.y + pad + S(1)), color::muted, heading.c_str());
        for (std::size_t at = 0; at < group.mods.size(); ++at) {
            const auto& name = group.mods[at];
            ImGui::PushID(name.c_str());
            const float y = start.y + pad + head + row * static_cast<float>(at);
            const float middle = y + row * 0.5f;
            const bool loads = !panel.conflicts.losses.contains(name);
            if (at > 0)
                draw->AddLine(ImVec2(start.x + pad, y), ImVec2(start.x + width - pad, y), color::outline);
            const char* standing = loads ? "LOADS" : "LEFT OUT";
            badge(draw, fonts, ImVec2(start.x + pad, middle - (fonts.caption->FontSize + S(8)) * 0.5f), standing,
                  loads ? color::good : color::tile_grey, loads ? color::ink : color::text);
            const float name_x = start.x + pad + badge_width(fonts, "LEFT OUT") + S(14);
            const float button = S(150);
            const ImVec4 clip(name_x, y, start.x + width - pad - button - S(12), y + row);
            draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(name_x, middle - fonts.bold->FontSize * 0.5f),
                          loads ? color::text : color::muted, title_of(panel, name).c_str(), nullptr, 0, &clip);
            if (!loads) {
                ImGui::SetCursorScreenPos(ImVec2(start.x + width - pad - button, middle - S(15)));
                if (ImGui::Button("Load this one", ImVec2(button, S(30)))) {
                    // Above the highest of the others it clashes with; settled as chosen over each.
                    std::string highest;
                    for (const auto& other : panel.conflicts.order) {
                        if (other == name) break;
                        const bool clash = std::ranges::any_of(panel.conflicts.rows, [&](const mods::MergeReport::Conflict& r) {
                            return (r.kept == other && r.lost == name) || (r.kept == name && r.lost == other); });
                        if (clash) {
                            highest = other;
                            break;
                        }
                    }
                    for (const auto& r : panel.conflicts.rows) {
                        const auto& other = r.kept == name ? r.lost : r.lost == name ? r.kept : std::string();
                        if (other.empty()) continue;
                        panel.conflict_choices.erase({other, name});
                        panel.conflict_choices.emplace(name, other);
                    }
                    write_conflict_choices(panel);
                    if (!highest.empty()) request = {Request::Kind::move, {name}, highest, true};
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", ("Move " + title_of(panel, name) + " above the mods here it clashes with, so it "
                                             "loads and they are left out").c_str());
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + tall + S(10)));
        ImGui::Dummy(ImVec2(width, 0));
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(16) - ImGui::GetFrameHeight());
    if (checking) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Checking again...");
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(24) - S(120));
    // Done keeps every conflict as it stands now: what the player saw and left is their choice.
    if (ImGui::Button("DONE", ImVec2(S(120), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // (Not a pair the player just turned round, which the list has not checked again yet.)
        for (const auto& pair : conflict_pairs(panel))
            if (!panel.conflict_choices.contains({pair.left_out, pair.loads})) panel.conflict_choices.emplace(pair.loads, pair.left_out);
        write_conflict_choices(panel);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void installed_page(Launcher& launcher, const Fonts& fonts, ModsPanel& panel, const thunderstore::Installed& installed,
                    const std::vector<const thunderstore::Package*>& pending, float height, bool installing) {
    auto& entries = panel.list.entries;
    const float top = ImGui::GetCursorPosY();
    {
        const auto enabled = std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.enabled; });
        // A mod left out for a conflict is no fault of its own: counted apart from the problems, and
        // only while the player has not settled it (Resolve conflicts).
        const auto conflicts = std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.enabled && entry.mod.outdated.empty() && !conflict_winner(panel, entry.mod.name).empty() &&
                   !left_out_by_choice(panel, entry.mod.name);
        });
        const auto problems = std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return !entry.mod.outdated.empty() || (entry.enabled && broken(panel, entry.mod.name));
        });
        const auto waiting = std::count_if(entries.begin(), entries.end(),
            [&](const auto& entry) { return update_waiting(panel, installed, entry.mod.name); });
        std::uint64_t bytes{};
        for (const auto& [name, size] : panel.sizes) bytes += size;
        auto caption = std::format("{} {}  /  {} on", entries.size(), entries.size() == 1 ? "mod" : "mods", enabled);
        if (bytes) caption += "  /  " + size_text(bytes);
        page_banner(fonts, "MY MODS", nullptr,
            "Your installed mods. They load top to bottom: where two change the same thing, the higher one wins. "
            "Drag a mod by its handle to move it. Changes apply the next time Skate starts.",
            problems ? std::format("{} {}", problems, problems == 1 ? "PROBLEM" : "PROBLEMS")
                : conflicts ? std::format("{} {}", conflicts, conflicts == 1 ? "CONFLICT" : "CONFLICTS")
                : waiting ? std::format("{} {} WAITING", waiting, waiting == 1 ? "UPDATE" : "UPDATES")
                : entries.empty() ? std::string("NONE YET") : std::string("ALL GOOD"),
            problems ? color::danger : conflicts ? color::warning : waiting ? color::blue
                : entries.empty() ? color::tile_grey : color::good, caption);
    }

    // ------------------------------------------------ search, filter, order, check, refresh
    const float combo = S(150), check = S(160), refresh = S(96), gap = S(10);
    ImGui::SetNextItemWidth(std::max(S(140), ImGui::GetContentRegionAvail().x - (combo + gap) * 3 - check - gap - refresh - gap));
    // Ctrl+F jumps to the search, as it does in a browser.
    if (!installing && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##installed_search", "Search your mods  (Ctrl+F)", panel.search.data(), panel.search.size());
    ImGui::SameLine(0, gap);
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
    ImGui::SameLine(0, gap);
    {
        std::vector<std::pair<std::string, int>> categories;
        for (const auto& entry : entries)
            for (const auto& category : categories_of(panel, entry.mod)) ++category_count(categories, category);
        auto& settings = launcher.settings();
        if (category_menu("##installed_category", settings.installed_categories_only, settings.installed_categories_hidden, std::move(categories), combo))
            launcher.save();
    }
    ImGui::SameLine(0, gap);
    choice("##installed_order", order_names, panel.order);
    ImGui::SameLine(0, gap);
    {
        const bool checking = panel.conflicts.running || panel.conflicts.asked != panel.conflicts.checked;
        ImGui::BeginDisabled(checking);
        if (ImGui::Button(checking ? "Checking..." : "Check conflicts", ImVec2(check, 0))) want_conflict_check(panel, 0);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Checks your enabled mods, in this order, for mods that change the same parts of the game. "
                              "The list checks itself after every change too.");
    }
    ImGui::SameLine(0, gap);
    if (ImGui::Button("Refresh", ImVec2(refresh, 0))) refresh_mods(launcher, panel);

    const auto view = visible_mods(panel, installed, {launcher.settings().installed_categories_only, launcher.settings().installed_categories_hidden});
    // Dragging, and typing a place, move a mod among the others in the load order: the order the
    // list must be shown in for where it lands to be what it looks like.
    const bool reorder = panel.order == 0 && !installing;
    const auto shown = [&](int at) -> const std::string& {
        return entries[static_cast<std::size_t>(view[static_cast<std::size_t>(at)])].mod.name;
    };
    Request request;

    // ------------------------------------------------ the conflicts, and the way to settle them
    ImGui::Spacing();
    conflicts_bar(fonts, panel);

    // ------------------------------------------------ the updates waiting on Thunderstore
    if (!pending.empty()) {
        ImGui::Spacing();
        ImGui::BeginDisabled(installing);
        updates_bar(fonts, panel, installed, pending);
        ImGui::EndDisabled();
    }

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
        ImGui::TextDisabled("%s", view.size() != entries.size()
            ? std::format("{} of {} shown", view.size(), entries.size()).c_str()
            : "Tick mods to change several at once. Shift-click ticks a run of them, Ctrl+A every one.");
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
        const float action_gap = S(8), clear = S(70);
        float across = clear;
        for (const auto& action : actions)
            if (action.kind != Request::Kind::update || waiting) across += action.width + action_gap;
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
            ImGui::SameLine(0, action_gap);
        }
        if (ImGui::Button("Clear", ImVec2(clear, 0))) panel.marked.clear();
    }
    ImGui::EndDisabled();

    // ------------------------------------------------ what else the player should know
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
        if (entry.enabled && entry.mod.outdated.empty() && broken(panel, entry.mod.name)) dropped.push_back(entry.mod.title);
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

    // ------------------------------------------------ the table
    table_header(fonts);
    const float body = std::max(S(120), height - (ImGui::GetCursorPosY() - top));
    ImGui::BeginChild("##mod_list", ImVec2(0, body), ImGuiChildFlags_NavFlattened);
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
    const float row = S(46) + row_gap();
    const ImVec2 origin = ImGui::GetCursorPos();
    const ImVec2 origin_screen = ImGui::GetCursorScreenPos();
    // A row dragged by its handle: the mods it moves, lifted out of the list and following the
    // pointer, and where among the rest they would land (slot).
    const auto* carried = ImGui::GetDragDropPayload();
    const bool dragging = reorder && carried && carried->IsDataType(dragged_row) && !panel.drag_name.empty();
    if (!dragging) panel.drag_name.clear();
    std::vector<std::string> moving;
    if (dragging) moving = dragged_mods(panel, *static_cast<const int*>(carried->Data));
    const auto lifted = [&](int index) {
        return std::ranges::find(moving, entries[static_cast<std::size_t>(index)].mod.name) != moving.end();
    };
    std::vector<int> rest, raised;
    for (const int index : view) (dragging && lifted(index) ? raised : rest).push_back(index);
    const float held = ImGui::GetMousePos().y - origin_screen.y - panel.drag_grab;   // the dragged row's top
    const auto slot = static_cast<std::size_t>(std::clamp(static_cast<int>(std::floor(held / row + 0.5f)), 0,
                                                          static_cast<int>(rest.size())));
    // Where each row goes: in view order, or with a drag on, the rest with a gap at the slot.
    std::vector<std::pair<int, float>> places;   // (index, height)
    if (!dragging || raised.empty()) {
        for (std::size_t at = 0; at < view.size(); ++at) places.emplace_back(view[at], static_cast<float>(at) * row);
    } else {
        for (std::size_t at = 0; at < rest.size(); ++at)
            places.emplace_back(rest[at], static_cast<float>(at < slot ? at : at + raised.size()) * row);
    }
    // Eased toward it: a row moved slides there. One never drawn before starts where it goes.
    const float ease = std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
    const auto eased = [&](const std::string& name, float target, bool follow) {
        auto [at, fresh] = panel.row_y.try_emplace(name, target);
        float& y = at->second;
        if (follow || fresh || std::abs(target - y) < 0.5f) y = target;
        else y += (target - y) * ease;
        return y;
    };
    std::map<int, int> position_of;   // place in the view, for ticking a run of rows
    for (std::size_t at = 0; at < view.size(); ++at) position_of[view[at]] = static_cast<int>(at);
    const float scroll = ImGui::GetScrollY(), seen = ImGui::GetWindowHeight();
    ImGui::BeginDisabled(installing);
    for (const auto& [index, target] : places) {
        const float y = eased(entries[static_cast<std::size_t>(index)].mod.name, target, false);
        // Only the rows in sight, and one past each edge (a controller moving off the last one needs the next).
        if (y + 2 * row < scroll || y > scroll + seen + row) continue;
        ImGui::SetCursorPos(ImVec2(origin.x, origin.y + y));
        installed_row(fonts, panel, installed, view, position_of[index], row - row_gap(), reorder, request);
    }
    // The dragged rows last, over the rest, where the pointer holds them, edged in blue.
    for (std::size_t at = 0; dragging && at < raised.size(); ++at) {
        const int index = raised[at];
        const float y = eased(entries[static_cast<std::size_t>(index)].mod.name, held + static_cast<float>(at) * row, true);
        ImGui::SetCursorPos(ImVec2(origin.x, origin.y + y));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(start.x + S(4), start.y + S(6)), ImVec2(start.x + width + S(4), start.y + row - row_gap() + S(6)),
                            rgba(0, 0, 0, 0.35f), S(6));
        installed_row(fonts, panel, installed, view, position_of[index], row - row_gap(), reorder, request);
        draw->AddRect(start, ImVec2(start.x + width, start.y + row - row_gap()), color::blue, S(6), 0, S(2));
    }
    // So the list scrolls over every row and no further.
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + static_cast<float>(view.size()) * row));
    ImGui::Dummy(ImVec2(1, 0));
    ImGui::EndDisabled();
    // Dropped anywhere on the list: the dragged mods go where the gap is, above the row after it
    // (or below the last).
    if (dragging && !rest.empty()) {
        const ImRect area(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                                                        ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
        if (ImGui::BeginDragDropTargetCustom(area, ImGui::GetID("##drop"))) {
            if (const auto* delivered = ImGui::AcceptDragDropPayload(dragged_row,
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                delivered && delivered->IsDelivery()) {
                const bool last = slot >= rest.size();
                request = {Request::Kind::move, moving, entries[static_cast<std::size_t>(last ? rest.back() : rest[slot])].mod.name, !last};
            }
            ImGui::EndDragDropTarget();
        }
    }
    // While a row is dragged near the list's top or bottom edge, the list scrolls that way.
    if (const auto* payload = ImGui::GetDragDropPayload(); payload && payload->IsDataType(dragged_row)) {
        const float mouse = ImGui::GetMousePos().y, edge = S(56);
        const float top_edge = ImGui::GetWindowPos().y, bottom_edge = top_edge + ImGui::GetWindowHeight();
        const float speed = S(900) * ImGui::GetIO().DeltaTime;
        if (mouse < top_edge + edge) ImGui::SetScrollY(ImGui::GetScrollY() - speed * (top_edge + edge - mouse) / edge);
        else if (mouse > bottom_edge - edge) ImGui::SetScrollY(ImGui::GetScrollY() + speed * (mouse - (bottom_edge - edge)) / edge);
    }
    keep_focus_in_list();
    ImGui::EndChild();

    // Ctrl+A and Delete, as in a file list, while nothing is being typed.
    if (!installing && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            for (int at = 0; at < static_cast<int>(view.size()); ++at) panel.marked.insert(shown(at));
        if (!panel.marked.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            request = {Request::Kind::uninstall, std::vector<std::string>(panel.marked.begin(), panel.marked.end())};
    }
    resolve_popup(fonts, panel, request);
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
    const ImVec2 extent(std::min(S(1080), size.x - S(80)), std::min(S(740), size.y - S(60)));
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
    // The icon (the package's, when the mod is on Thunderstore), with the name, a line of what
    // it is and its description beside it.
    const float icon = S(104), header_top = ImGui::GetCursorPosY();
    mod_icon(panel, package, ImGui::GetCursorScreenPos(), icon);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + icon + S(20));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(mod.title.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", summary(panel, mod).c_str());
    if (!mod.description.empty()) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextUnformatted(mod.description.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), header_top + icon) + S(14));
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

    ImGui::Dummy(ImVec2(0, S(6)));
    const auto columns = overview_columns(extent.y);
    ImGui::BeginChild("##mod_overview_body", ImVec2(columns.readme, columns.height), ImGuiChildFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
    if (!mod.outdated.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "Outdated: this mod does not load. It was made for another version of Skate (%s). Get an updated "
            "version, or rebuild it with the latest ReSkate Studio.", mod.outdated.c_str());
    if (const auto other = conflict_winner(panel, mod.name); !other.empty() && mod.outdated.empty()) {
        const auto winner = title_of(panel, other);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning),
            "Not loaded: this mod changes the same parts of the game as %s, which is higher in the load order, in a "
            "way the two cannot share, so only %s loads. To use this mod instead, move it above %s, or switch one "
            "of them off.", winner.c_str(), winner.c_str(), winner.c_str());
        if (const auto reason = conflict_reason(panel, mod.name); !reason.empty())
            ImGui::TextDisabled("%s", ("They " + reason + ".").c_str());
        else if (const auto found = panel.list.excluded.find(mod.name); found != panel.list.excluded.end() && !found->second.empty())
            ImGui::TextDisabled("%s", found->second.front().c_str());
    } else if (const auto* beats = conflict_beats(panel, mod.name); beats && entry.enabled) {
        std::string names;
        for (std::size_t at = 0; at < beats->size(); ++at)
            names += (at == 0 ? "" : at + 1 == beats->size() ? " and " : ", ") + title_of(panel, (*beats)[at]);
        ImGui::TextDisabled("%s", ("Loads, and leaves out " + names +
            ": they change the same parts of the game and are lower in the load order.").c_str());
    }
    if (const auto missing = panel.list.excluded.find(mod.name); missing != panel.list.excluded.end() && broken(panel, mod.name)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger),
            "Not loaded: the game could not merge this mod cleanly, so none of it is used. Reinstall the whole "
            "mod folder, or rebuild it with a current ReSkate Studio.");
        if (!missing->second.empty()) ImGui::TextDisabled("%s", missing->second.front().c_str());
    }
    if (!mod.provides_layout && !mod.provides_levels && mod.park_maps.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning),
            "This folder has no layout.toc or reskate-levels.json, so the game has nothing to load from it.");
    if (!readme_field(fonts, panel, package, &mod)) ImGui::TextDisabled("This mod has no README.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    begin_overview_details("##mod_overview_details", columns);
    overview_fact(fonts, "VERSION", mod.version.empty() ? std::string() : "v" + mod.version);
    overview_fact(fonts, "AUTHOR", mod.author);
    if (package)
        overview_fact(fonts, "THUNDERSTORE", package->full_name + (update ? "\nv" + package->latest().number + " available"
                                                                          : std::string("\nUp to date")));
    overview_fact(fonts, "FOLDER", "Mods\\" + mod.name);
    if (!mod.tool.empty() || !mod.built.empty())
        overview_fact(fonts, "BUILT WITH", mod.tool + (mod.built.empty() ? "" : (mod.tool.empty() ? "" : ", ") + mod.built));
    std::string levels;
    for (const auto& level : mod.levels) {
        const auto slash = level.rfind('/');
        levels += (levels.empty() ? "" : "\n") + (slash == std::string::npos ? level : level.substr(slash + 1));
    }
    overview_fact(fonts, "MAPS", levels);
    std::string parks;
    for (const auto& map : mod.park_maps) parks += (parks.empty() ? "" : ", ") + map;
    overview_fact(fonts, "PARKS", parks);
    end_overview_details();

    ImGui::SetCursorPosY(extent.y - S(24) - ImGui::GetFrameHeight());
    bool enabled = entry.enabled;
    if (ImGui::Checkbox("Loads when Skate starts", &enabled)) {
        entry.enabled = enabled;
        save(panel);
    }
    ImGui::SameLine(extent.x - S(28) - S(110));
    // Escape too, and so a controller's B: the modal has no other way out but CLOSE.
    if (ImGui::Button("CLOSE", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close();
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

fs::path pick_path(HWND owner, bool folder, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter_pattern) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder && filter_name && filter_pattern) {
        const COMDLG_FILTERSPEC filter[]{{filter_name, filter_pattern}};
        dialog->SetFileTypes(1, filter);
    }
    dialog->SetTitle(title);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path{};
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

void wait_for_conflict_check(const ModsPanel& panel) {
    for (int waited = 0; panel.conflicts.running && waited < 500; ++waited) Sleep(20);
}

void prepare_mods(ModsPanel& panel, const launcher_app::Session& session) {
    if (panel.root.empty()) {
        panel.root = launcher_mods::mods_root(session.paths.directory);
        panel.list = mods::scan_mods(panel.root.parent_path());
        want_conflict_check(panel, 0);
        read_conflict_choices(panel);
    }
    pump_conflict_check(panel);
}

void refresh_mods(Launcher& launcher, ModsPanel& panel) {
    scan(panel, launcher.session());
    refresh_listing(panel, ImGui::GetTime(), true);
    panel.message.clear();
}

void scan(ModsPanel& panel, const launcher_app::Session& session) {
    panel.root = launcher_mods::mods_root(session.paths.directory);
    panel.list = mods::scan_mods(panel.root.parent_path());
    want_conflict_check(panel, 0);
    read_conflict_choices(panel);
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
        wait_for_conflict_check(panel);   // (it may be reading the mod this replaces)
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
        ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
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
        ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
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
    // MY MODS opened from another tab: what GET MODS or a profile installed or took away is in the folder.
    if (panel.scanned && panel.tab == 0 && panel.shown_tab != -1 && panel.shown_tab != 0) scan(panel, session);
    panel.shown_tab = panel.tab;
    if (!panel.scanned) scan(panel, session);
    collect_install(panel, session);
    pump_conflict_check(panel);
    settle_profile(panel);
    refresh_listing(panel, ImGui::GetTime());
    // A page, not a panel: the mod list and the Thunderstore browser both want
    // the whole window. It sits at (0, 0), so window and screen space agree.
    const auto frame = begin_page("##mods_panel", size);
    // Both lists draw mod icons, so both need finished ones uploaded.
    pump_icons(panel);
    auto* draw = ImGui::GetWindowDrawList();
    const bool installing = panel.installing;
    hold_focus(installing);
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
    // Flattened, so a controller's D-pad crosses from the rail into the list and back.
    ImGui::BeginChild("##rail", ImVec2(rail_width, bottom - rail_top), ImGuiChildFlags_NavFlattened);
    const bool leave = rail(fonts, panel, window, rail_width, pending, installing);
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(content_x, top));
    ImGui::BeginChild("##content", ImVec2(frame.x - content_x - S(28), bottom - top), ImGuiChildFlags_NavFlattened);
    // With nothing to say, the list runs all the way down to BACK's bottom
    // edge; a message takes two lines off it until it is gone.
    const float status = panel.message.empty() ? S(4) : ImGui::GetTextLineHeight() * 2 + S(12);
    const float body = ImGui::GetWindowHeight() - status;
    if (panel.tab == 1) browse_page(launcher, fonts, panel, body, installing);
    else if (panel.tab == 2) profiles_page(fonts, panel, window, body, installing);
    else installed_page(launcher, fonts, panel, installed, pending, body, installing);
    if (!panel.message.empty()) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - status + S(8));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(panel.message_error ? color::danger : color::good), "%s",
            panel.message.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    // Read before the overviews: one closing on Escape must not also leave the page.
    const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (panel.tab == 1) package_overview(fonts, panel, frame, installing);
    else if (panel.tab == 0) mod_overview(fonts, panel, installed, frame, installing);
    if (leave) close();
    if (!installing && !popup_open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive() &&
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
        // Escape (and a controller's B) answers Cancel, never the destructive choice.
        if (ImGui::Button("Cancel", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            panel.confirm_remove.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        push_primary_button();
        if (ImGui::Button("UNINSTALL", ImVec2(S(110), 0))) {
            // Stops at the first folder that will not go: what went is gone, the rest is untouched.
            std::vector<std::string> removed;
            std::string failure;
            wait_for_conflict_check(panel);   // (it may be reading these mods' files; a second or two at most)
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
        ImGui::Text("\"%s\" is already installed. Replace it? The installed copy is deleted.",
            panel.conflict_name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
    profile_popups(panel);
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
