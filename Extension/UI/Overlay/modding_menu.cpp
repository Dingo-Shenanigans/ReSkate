#include "modding_menu.h"
#include "skate_menu.h"
#include "skate_menu_internal.h"
#include "Engine/Vfs/builtin_packs.h"
#include "Engine/Vfs/initfs.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_conflicts.h"
#include "Extension/Assets/live_mods.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Core/Log/logging.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
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
}
namespace {
std::filesystem::path game_directory() {
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) throw std::runtime_error("Cannot locate the game directory.");
    return std::filesystem::path(std::wstring(executable.data(), length)).parent_path();
}
// What a mod brings, as the native Mods page words it.
std::string contents_text(const mods::Mod& mod) {
    std::string text;
    for (auto part : mods::mod_contents(mod)) {
        if (!text.empty()) part[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(part[0])));
        text += (text.empty() ? "" : " + ") + part;
    }
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
    std::size_t loaded{}, pending{};
    for (const auto& entry : list.entries) {
        const bool effect = in_effect(entry.mod.name);
        if (effect) ++loaded;
        // (A mod that is only a plugin is switched on and off live: nothing of it waits to be applied.)
        if (entry.enabled != effect && !excluded(entry.mod.name) && !set_aside(entry) && !mods::plugin_only(entry.mod)) ++pending;
    }
    // Mods still in effect whose folder has been deleted: applying unloads them.
    std::vector<std::string> removed;
    for (const auto& name : applied)
        if (mods::variant_owner(name).empty() && !kept(name) &&
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
        if (menu::primary_button(menu, live_mods::apply_label(effect))) {
            action_error.clear();
            menu::feedback(menu, live_mods::apply(effect == live_mods::ApplyEffect::reload_level).c_str());
        }
    }
    if (const auto last = live_mods::status(); !last.empty()) menu::note(("Last apply: " + last).c_str());

    // A row a mod: its name, who made it and what it brings, its state at the right end and a blue
    // edge while it is loaded. A opens why it is in that state and ENABLE or DISABLE under it.
    static std::string open_mod;
    const auto byline = [](const mods::Mod& mod) {
        std::string text = mod.author.empty() ? "Unknown author" : "by " + mod.author;
        if (!mod.version.empty()) text += "  /  v" + mod.version;
        return text;
    };
    std::optional<std::size_t> toggle;
    int index{};
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
        const auto& entry = list.entries[i];
        const auto& mod = entry.mod;
        const bool effect = in_effect(mod.name);
        const char* status = "DISABLED";
        bool loaded_now = false, trouble = false;
        std::string why; // what its state means, when it needs saying
        if (const auto winner = lost_to(mod.name); !winner.empty()) {
            status = "LEFT OUT";
            why = "Left out: it changes the same parts of the game as " + title_of(winner) + ", which is higher in "
                  "the load order, so only that one loads. Choose which loads in the launcher's Mod Manager.";
        } else if (excluded(mod.name)) {
            status = "NOT LOADED";
            trouble = true;
            why = "Not loaded: it could not be merged cleanly, so none of it is used. "
                  "Reinstall the whole mod folder, or rebuild it with a current ReSkate Studio.";
        } else if (set_aside(entry)) {
            status = "SET ASIDE";
            why = "Set aside: it changes what another mod's chosen variant replaces (a deck pack's deck, say), and "
                  "only one of them can be in the game. Pick another variant on that mod's own page, or disable that mod.";
        } else if (entry.enabled && (effect || mods::plugin_only(mod))) {
            // (A mod that is only a plugin runs as soon as it is on: nothing of it is applied.)
            status = "LOADED";
            loaded_now = true;
        } else if (mods::plugin_only(mod)) {
            // (and stops as soon as it is off)
        } else if (entry.enabled) {
            status = "APPLY TO LOAD";
            why = "Apply the changes above to load it.";
            trouble = true;
        } else if (effect) {
            status = "APPLY TO UNLOAD";
            why = "Apply the changes above to unload it.";
            trouble = true;
        }
        ImGui::PushID(index++);
        if (menu::expand_row(menu, mod.name.c_str(), mod.title.empty() ? mod.name : mod.title, byline(mod) + "  /  " + contents_text(mod),
                             status, open_mod, loaded_now)) {
            ImGui::Indent(menu::px(40));
            if (!mod.description.empty()) menu::note(mod.description.c_str());
            if (trouble) menu::warn(why.c_str());
            else if (!why.empty()) menu::note(why.c_str());
            ImGui::Unindent(menu::px(40));
            if (menu::row_actions(menu, "actions", {{entry.enabled ? "DISABLE" : "ENABLE", list.issue.empty(), !entry.enabled,
                                                     entry.enabled ? "Turn it off in mods.json." : "Turn it on in mods.json."}}) == 0)
                toggle = i;
        }
        ImGui::PopID();
    }
    for (const auto& name : removed) {
        ImGui::PushID(index++);
        if (menu::expand_row(menu, ("removed\n" + name).c_str(), name, "Folder deleted", "APPLY TO UNLOAD", open_mod)) {
            ImGui::Indent(menu::px(40));
            menu::warn("This mod's folder is gone, but it stays loaded until you apply.");
            ImGui::Unindent(menu::px(40));
        }
        ImGui::PopID();
    }
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
