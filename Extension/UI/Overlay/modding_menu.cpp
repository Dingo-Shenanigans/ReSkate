#include "modding_menu.h"
#include "skate_menu.h"
#include "skate_menu_internal.h"
#include "Engine/Vfs/builtin_packs.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_conflicts.h"
#include "Extension/Assets/live_mods.h"
#include "Extension/Assets/mod_store.h"
#include "Launcher/thunderstore.h"
#include "overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Core/Log/logging.h"

#include <Windows.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
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
}
namespace {
// What a mod brings, as the native Mods page words it.
std::string contents_text(const mods::Mod& mod) {
    std::string text;
    for (auto part : mods::mod_contents(mod)) {
        if (!text.empty()) part[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(part[0])));
        text += (text.empty() ? "" : " + ") + part;
    }
    return text.empty() ? "Nothing to load" : text;
}
// What Thunderstore's listing says of the mod folders it has a package for: the newest version and its
// icon, by folder, worked out again only when a new copy of the listing comes. Render thread only.
struct Listed {
    std::string latest, icon;
};
struct NoCase {
    bool operator()(const std::string& a, const std::string& b) const { return _stricmp(a.c_str(), b.c_str()) < 0; }
};
const std::map<std::string, Listed, NoCase>& listed_by_folder() {
    static auto* by_folder = new std::map<std::string, Listed, NoCase>;
    static unsigned serial = ~0u;
    const auto listing = mod_store::listing();
    if (listing && listing->serial != serial) {
        serial = listing->serial;
        by_folder->clear();
        for (const auto& package : listing->packages)
            if (!package.versions.empty())
                (*by_folder)[thunderstore::folder_for(package.full_name)] = {package.latest().number, package.latest().icon};
    }
    return *by_folder;
}
// The newer version Thunderstore has of an installed mod, else empty.
std::string newer_version(const std::string& folder, const std::string& installed) {
    const auto& listed = listed_by_folder();
    const auto found = listed.find(folder);
    if (found == listed.end() || installed.empty() || thunderstore::compare_versions(found->second.latest, installed) <= 0) return {};
    return found->second.latest;
}
// A mod's square when it has no picture: a colour for what it mostly brings.
ImU32 kind_colour(const std::string& kind) {
    static constexpr std::array<ImU32, 8> colours{IM_COL32(122, 167, 214, 255), IM_COL32(201, 138, 75, 255), IM_COL32(154, 124, 255, 255),
                                                  IM_COL32(79, 179, 138, 255), IM_COL32(163, 131, 96, 255), IM_COL32(224, 111, 154, 255),
                                                  IM_COL32(91, 143, 217, 255), IM_COL32(190, 190, 96, 255)};
    return colours[std::hash<std::string>{}(kind) % colours.size()];
}

// The MODS page, as a library: how many there are and how many are on, the changes that wait to be
// applied and the button that applies them; a filter (all, on, off, the ones with a problem) and a
// search; then a row a mod, its picture or a colour for its kind, its kind and version or its problem
// under the name, and its state as a tag. What the Mods folder holds is re-read every 1.5 s while the
// page shows, so a folder added or an edit made outside the game shows up. Turning a mod on or off
// saves mods.json; applying merges the change in while the game runs (it takes effect at the next level
// load), including a mod folder added since launch.
void draw_installed_mods(SkateMenu& menu) {
    namespace st = skate_theme;
    static auto* scans = new FolderRead<mods::ModList>; // process lifetime, as the threads that fill it
    static std::string action_error;
    menu::page_prompts("Details", "On / off", "Apply");
    const auto scanned = scans->get([] { return mods::scan_mods(mods::engine_data_root()); }, 1500);
    if (!scanned) {
        menu::note("Reading the Mods folder...");
        return;
    }
    const auto& list = *scanned;
    const auto& launch = mods::catalog();
    const auto applied = live_mods::applied_mods();
    // ReSkate's own packs (builtin_packs.h) live outside the Mods folder's list, always on. Without
    // this, the ones in effect read as mods whose folder was deleted.
    const auto kept = [&](const std::string& name) {
        const auto owner = mods::variant_owner(name);
        return (owner.empty() ? name : owner).starts_with("reskate-builtin-");
    };
    // A mod with variants is in effect when one of its variants is (mod_catalog.h).
    const auto in_effect = [&](const std::string& name) {
        return std::ranges::any_of(applied, [&](const std::string& other) {
            return _stricmp(other.c_str(), name.c_str()) == 0 || _stricmp(mods::variant_owner(other).c_str(), name.c_str()) == 0;
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
    // Set aside for another mod's variant (mod_catalog.h): enabled, not loaded, and nothing about them
    // waits to be applied.
    const auto aside = mods::set_aside_for_variants();
    const auto set_aside = [&](const mods::ModEntry& entry) {
        return entry.enabled && !in_effect(entry.mod.name) &&
               std::ranges::any_of(aside, [&](const std::string& other) { return _stricmp(other.c_str(), entry.mod.name.c_str()) == 0; });
    };

    // Each mod's state, worked out once for the counts, the filter and its row.
    enum class Kind { on, off, waits_on, waits_off, update, conflict, not_loaded, set_aside };
    struct Row {
        std::size_t entry{};
        Kind kind{};
        std::string why, newer, line; // what its state means; the newer version listed; the line under its name
    };
    std::vector<Row> rows;
    std::size_t pending{}, enabled{}, problems{};
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        const auto& mod = entry.mod;
        const bool effect = in_effect(mod.name);
        if (entry.enabled) ++enabled;
        Row row{i, entry.enabled ? Kind::on : Kind::off, {}, newer_version(mod.name, mod.version), {}};
        if (const auto winner = lost_to(mod.name); !winner.empty()) {
            row.kind = Kind::conflict;
            row.line = "Conflicts with " + title_of(winner);
            row.why = "Left out: it changes the same parts of the game as " + title_of(winner) + ", which is higher in "
                      "the load order, so only that one loads. Choose which loads in the launcher's Mod Manager.";
        } else if (excluded(mod.name)) {
            row.kind = Kind::not_loaded;
            row.line = "Not loaded: it could not be merged";
            row.why = "Not loaded: it could not be merged cleanly, so none of it is used. "
                      "Reinstall the whole mod folder, or rebuild it with a current ReSkate Studio.";
        } else if (set_aside(entry)) {
            row.kind = Kind::set_aside;
            row.line = "Set aside for another mod's variant";
            row.why = "Set aside: it changes what another mod's chosen variant replaces (a deck pack's deck, say), and "
                      "only one of them can be in the game. Pick another variant on that mod's own page, or disable that mod.";
        } else if (entry.enabled && (effect || mods::plugin_only(mod))) {
            // (A mod that is only a plugin runs as soon as it is on, and stops as soon as it is off:
            // nothing of it is applied.)
        } else if (mods::plugin_only(mod)) {
        } else if (entry.enabled) {
            row.kind = Kind::waits_on;
            row.line = "Loads when you apply";
            row.why = "Apply the changes above to load it.";
        } else if (effect) {
            row.kind = Kind::waits_off;
            row.line = "Unloads when you apply";
            row.why = "Apply the changes above to unload it.";
        }
        if (row.kind == Kind::waits_on || row.kind == Kind::waits_off) ++pending;
        if ((row.kind == Kind::on || row.kind == Kind::off) && !row.newer.empty()) {
            row.kind = Kind::update;
            row.line = "Update to v" + row.newer;
            row.why = "Thunderstore has v" + row.newer + ". Update it in the pause menu's Mod Manager, or in the launcher.";
        }
        if (row.line.empty()) {
            const auto contents = contents_text(mod);
            row.line = mod.version.empty() ? contents : contents + " \xc2\xb7 v" + mod.version;
        }
        if (row.kind == Kind::update || row.kind == Kind::conflict || row.kind == Kind::not_loaded || row.kind == Kind::set_aside) ++problems;
        rows.push_back(std::move(row));
    }
    // Mods still in effect whose folder has been deleted: applying unloads them.
    std::vector<std::string> removed;
    for (const auto& name : applied)
        if (mods::variant_owner(name).empty() && !kept(name) &&
            std::ranges::none_of(list.entries, [&](const mods::ModEntry& entry) { return _stricmp(entry.mod.name.c_str(), name.c_str()) == 0; }))
            removed.push_back(name);
    pending += removed.size();

    // How many, and the changes that wait, with the button that applies them: beside the count in a wide
    // page, under it in the narrow sheet. Labelled by what the changes do to the level being played: maps
    // only just need applying; anything the level shows needs it reloaded; taking away the map being
    // played sends the player to San Van.
    const bool can_apply = pending && !live_mods::busy();
    const auto apply = [&] {
        action_error.clear();
        menu::feedback(menu, live_mods::apply(live_mods::preview(list) == live_mods::ApplyEffect::reload_level).c_str());
    };
    {
        const float room = ImGui::GetContentRegionAvail().x, button = menu::px(240), start = ImGui::GetCursorPosX();
        ImGui::BeginGroup();
        ImGui::PushFont(menu.bold);
        ImGui::Text("%zu mods \xc2\xb7 %zu on", list.entries.size(), enabled);
        ImGui::PopFont();
        if (live_mods::busy()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st::warning), "Applying mods...");
        } else if (pending) {
            const auto waiting = std::to_string(pending) + (pending == 1 ? " change waits" : " changes wait") + " for APPLY";
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st::warning), "%s", waiting.c_str());
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st::grey_text), "Nothing waits to be applied.");
        }
        ImGui::EndGroup();
        if (can_apply) {
            const auto label = live_mods::apply_label(live_mods::preview(list));
            if (room >= menu::px(520)) {
                ImGui::SameLine(start + room - button);
                ImGui::BeginChild("apply", ImVec2(button, 0), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
                if (menu::primary_button(menu, label)) apply();
                ImGui::EndChild();
            } else if (menu::primary_button(menu, label)) {
                apply();
            }
        }
    }
    if (can_apply && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()) apply();
    if (const auto last = live_mods::status(); !last.empty()) menu::note(("Last apply: " + last).c_str());

    // The filter, then the search.
    static int filter = 0;
    static std::array<char, 96> search{};
    const auto problems_label = problems ? "PROBLEMS " + std::to_string(problems) : std::string("PROBLEMS");
    const int picked = menu::button_row(menu, "mod-filter", {{"ALL", true, filter == 0, "Every mod."}, {"ON", true, filter == 1, "The mods that are on."},
                                                             {"OFF", true, filter == 2, "The mods that are off."},
                                                             {problems_label.c_str(), true, filter == 3, "Updates, conflicts and mods that could not load."}});
    if (picked >= 0) filter = picked;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##mod-search", "Search mods...", search.data(), search.size());
    const auto lower = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const auto wanted = lower(search.data());

    // A row a mod. A opens why it is in its state and ENABLE or DISABLE under it; X switches it. (Y applies
    // anywhere on the page: above.)
    static std::string open_mod;
    std::optional<std::size_t> toggle;
    int index{}, shown{};
    for (const auto& row : rows) {
        const auto& entry = list.entries[row.entry];
        const auto& mod = entry.mod;
        const bool problem = row.kind == Kind::update || row.kind == Kind::conflict || row.kind == Kind::not_loaded || row.kind == Kind::set_aside;
        const bool listed = filter == 0 || (filter == 1 && entry.enabled) || (filter == 2 && !entry.enabled) || (filter == 3 && problem);
        const auto title = mod.title.empty() ? mod.name : mod.title;
        const bool found = wanted.empty() || lower(title + " " + mod.name + " " + mod.author).find(wanted) != std::string::npos;
        ++index;
        if (!listed || !found) continue;
        ++shown;
        menu::LibraryRow shown_row;
        shown_row.name = title;
        shown_row.detail = row.line;
        switch (row.kind) {
        case Kind::on:
        case Kind::waits_on:
            shown_row.tag = "ON";
            shown_row.tag_fill = st::blue;
            shown_row.tag_ink = st::white;
            break;
        case Kind::off:
        case Kind::waits_off: shown_row.tag = "OFF"; break;
        case Kind::update:
            shown_row.tag = "UPDATE";
            shown_row.tag_fill = shown_row.edge = shown_row.detail_colour = st::warning;
            shown_row.tag_ink = st::black;
            break;
        case Kind::set_aside:
            shown_row.tag = "SET ASIDE";
            shown_row.tag_fill = shown_row.edge = shown_row.detail_colour = st::warning;
            shown_row.tag_ink = st::black;
            break;
        case Kind::conflict:
        case Kind::not_loaded:
            shown_row.tag = row.kind == Kind::conflict ? "CONFLICT" : "ERROR";
            shown_row.tag_fill = shown_row.edge = st::danger;
            shown_row.detail_colour = IM_COL32(255, 138, 138, 255);
            shown_row.tag_ink = st::black;
            break;
        }
        if (row.kind == Kind::waits_on || row.kind == Kind::waits_off) shown_row.detail_colour = st::warning;
        // Its Thunderstore icon once that is loaded, else a colour for what it brings.
        if (const auto& listed_by = listed_by_folder(); !listed_by.empty())
            if (const auto it = listed_by.find(mod.name); it != listed_by.end() && !it->second.icon.empty())
                shown_row.image = detail::store_icon(it->second.icon);
        const auto kinds = mods::mod_contents(mod);
        shown_row.swatch = kind_colour(kinds.empty() ? std::string("mod") : kinds.front());
        shown_row.open = open_mod == mod.name;
        shown_row.hint = "A shows why it is in this state. X turns it on or off. Y applies the changes.";
        ImGui::PushID(index);
        const auto pressed = menu::library_row(menu, "##mod", shown_row);
        const ImGuiID row_id = ImGui::GetItemID();
        if (pressed == menu::RowPress::a) open_mod = shown_row.open ? std::string() : mod.name;
        if (pressed == menu::RowPress::x) toggle = row.entry;
        if (shown_row.open && open_mod == mod.name) {
            // B closes it, and the focus comes back to the row.
            menu::hold_back();
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::GetIO().WantTextInput) {
                open_mod.clear();
                ImGui::SetFocusID(row_id, ImGui::GetCurrentWindow());
            } else {
                ImGui::Indent(menu::px(40));
                std::string byline = mod.author.empty() ? "Unknown author" : "by " + mod.author;
                if (!mod.version.empty()) byline += "  /  v" + mod.version;
                menu::note((byline + "  /  " + contents_text(mod)).c_str());
                if (!mod.description.empty()) menu::note(mod.description.c_str());
                if (row.kind == Kind::conflict || row.kind == Kind::not_loaded || row.kind == Kind::waits_on || row.kind == Kind::waits_off)
                    menu::warn(row.why.c_str());
                else if (!row.why.empty()) menu::note(row.why.c_str());
                ImGui::Unindent(menu::px(40));
                if (menu::row_actions(menu, "actions", {{entry.enabled ? "DISABLE" : "ENABLE", list.issue.empty(), !entry.enabled,
                                                         entry.enabled ? "Turn it off in mods.json." : "Turn it on in mods.json."}}) == 0)
                    toggle = row.entry;
            }
        }
        ImGui::PopID();
    }
    for (const auto& name : removed) {
        ++index;
        if (filter == 1 || filter == 3 || (!wanted.empty() && lower(name).find(wanted) == std::string::npos)) continue;
        ++shown;
        ImGui::PushID(index);
        menu::LibraryRow gone;
        gone.name = name;
        gone.detail = "Folder deleted: unloads when you apply";
        gone.detail_colour = st::warning;
        gone.tag = "OFF";
        gone.swatch = IM_COL32(61, 62, 66, 255);
        gone.hint = "Its folder is gone, but it stays loaded until you apply. Y applies.";
        menu::library_row(menu, "##removed", gone);
        ImGui::PopID();
    }
    if (toggle && list.issue.empty()) {
        try {
            auto entries = list.entries;
            entries[*toggle].enabled = !entries[*toggle].enabled;
            mods::save_mod_order(list.root, entries);
            auto changed = list;
            changed.entries = std::move(entries);
            scans->set(std::move(changed));
            scans->again();
            action_error.clear();
        } catch (const std::exception& error) { action_error = error.what(); }
    }
    if (!index) menu::note(list.present ? "No mods installed yet." : "No Mods folder yet. Create one next to Skate.exe.");
    else if (!shown) menu::note("No mod matches.");
    if (!list.issue.empty()) menu::warn(("mods.json problem: " + list.issue).c_str());
    if (!action_error.empty()) menu::warn(("Could not save mods.json: " + action_error).c_str());
    menu::note("Order mods in the launcher's Mods panel. Maps, loading screens and cosmetics change at the next level load; "
               "other changes to game settings or the UI take effect after a restart.");
}
}
void draw_modding_menu(SkateMenu& menu, const Model&, const CallbacksV3&) {
    draw_installed_mods(menu);
}
ModAttention mod_attention() {
    ModAttention attention;
    const auto installed = mod_store::installed();
    if (!installed) return attention;
    for (const auto& mod : installed->mods) {
        if (mod.state == mod_store::State::left_out || mod.state == mod_store::State::not_loaded) ++attention.problems;
        const auto version = installed->versions.find(mod.folder);
        if (!mod.gone && version != installed->versions.end() && !newer_version(mod.folder, version->second).empty()) ++attention.updates;
    }
    return attention;
}
}
