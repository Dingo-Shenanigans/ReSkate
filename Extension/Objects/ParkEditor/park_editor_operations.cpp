#include "park_editor_runtime.h"
#include "park_editor_internal.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "park_document.h"
#include "park_mods.h"
#include "Engine/Vfs/mod_list.h"

namespace dingosdk {
using namespace profile_runtime;
using namespace profile_runtime::park_editor_detail;
namespace {
std::filesystem::path mods_root() {
    return editor_state().data_root / mods::mods_folder;
}
const editor::ParkMod *find_park_mod(std::string_view folder) {
    const auto &mods = editor_state().park_mods;
    const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto &mod) { return mod.folder == folder; });
    return it == mods.end() ? nullptr : &*it;
}
} // namespace
std::string edit_local_park(std::string_view operation, std::string_view map, std::uint64_t generation,
                            std::uint64_t revision, std::string_view argument,
                            const profile::PlacedObject &supplied, const std::vector<std::string> &texts) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &r = placements_runtime();
    auto &e = editor_state();
    try {
        if (!lobby_object_placement_allowed()) return "error: The host has disabled object placement for you.";
        if (!local_runtime().active || !placement_session_ready() || r.map != map ||
            generation != e.generation || revision != e.revision)
            return "error: Editor selection expired; wait for the current level.";
        if (!idle())
            return "error: Wait for the current edit or reload after an unacknowledged edit.";
        auto target = r.document.maps[r.map];
        const auto details = [&](std::size_t first) {
            if (texts.size() < first + 4) throw std::runtime_error("Park mod details are incomplete.");
            return editor::ParkModDetails{texts[first], texts[first + 1], texts[first + 2], texts[first + 3]};
        };
        const auto title_of = [&](std::string_view folder) {
            const auto *mod = find_park_mod(folder);
            return mod && !mod->details.title.empty() ? mod->details.title : std::string(folder);
        };
        // ---- park mods: files only, the layout in the world is unchanged
        if (operation == "mods-refresh") {
            refresh_park_mods();
            e.status = "Park mods refreshed.";
            ++e.revision;
            return e.status;
        }
        if (operation == "mod-save" || operation == "mod-create") {
            std::string folder(argument);
            if (operation == "mod-create") folder = editor::create_park_mod(mods_root(), details(0));
            else if (!find_park_mod(folder)) throw std::runtime_error("That park mod is not installed.");
            editor::save_mod_park(mods_root(), folder, {r.map, target});
            e.project = folder;
            refresh_park_mods();
            e.status = (operation == "mod-create" ? "Created park mod " : "Saved to ") + title_of(folder) +
                       " (" + std::to_string(target.size()) + " objects on " + r.map + ").";
            ++e.revision;
            return e.status;
        }
        if (operation == "mod-details") {
            if (!find_park_mod(argument)) throw std::runtime_error("That park mod is not installed.");
            editor::save_park_mod_details(mods_root(), argument, details(0));
            refresh_park_mods();
            e.status = "Saved the details of " + title_of(argument) + ".";
            ++e.revision;
            return e.status;
        }
        if (operation == "mod-convert") {
            const auto park = editor::load_park(e.directory, argument);
            const auto folder = editor::create_park_mod(
                mods_root(), {std::string(argument), "", "", "Converted from a park saved before park mods."});
            editor::save_mod_park(mods_root(), folder, park);
            refresh_park_mods();
            e.status = "Converted " + std::string(argument) + " into the park mod " + folder + " (" + park.map + ").";
            ++e.revision;
            return e.status;
        }
        // ---- operations that change the layout
        if (operation == "mod-open" || operation == "mod-load") {
            if (is_lobby_guest())
                throw std::runtime_error("In multiplayer only the host can load park mods.");
            if (!find_park_mod(argument)) throw std::runtime_error("That park mod is not installed.");
            target = editor::load_mod_park(mods_root(), argument, r.map).objects;
            // Editing a mod saves back into it; loading a preset leaves the project alone.
            if (operation == "mod-open") e.project = argument;
        } else if (operation == "new") {
            target.clear();
            e.project.clear();
        }
        else if (operation == "undo" || operation == "redo") {
            const bool undo = operation == "undo";
            const auto &history = undo ? e.undo : e.redo;
            if (history.empty())
                throw std::runtime_error("No history available.");
            if (!same_layout(target, undo ? history.back().after : history.back().before)) {
                e.undo.clear();
                e.redo.clear();
                throw std::runtime_error("Objects changed outside the editor. History was cleared.");
            }
            begin(undo ? history.back().before : history.back().after, undo ? -1 : 1);
            return e.status;
        } else if (operation == "place") {
            auto object = supplied;
            if (r.next_id == UINT64_MAX || target.size() >= 1024)
                throw std::runtime_error("Park object limit reached.");
            // The session's own limit on each player (the mutex is held: count here).
            if (const auto limit = lobby_object_limit(); limit && target.size() >= limit)
                throw std::runtime_error(object_limit_notice);
            object.id = r.next_id++;
            if (!e.assets || std::none_of(e.assets->begin(), e.assets->end(),
                                          [&](const auto &item) { return item.key == object.item; }))
                throw std::runtime_error("Object is not available in the installed Build Kit catalog.");
            target.push_back(std::move(object));
        } else if (operation == "move" || operation == "delete") {
            const auto found = std::find_if(target.begin(), target.end(),
                                            [&](const auto &x) { return x.id == supplied.id; });
            if (found == target.end())
                throw std::runtime_error("The selected object no longer exists.");
            if (operation == "delete")
                target.erase(found);
            else {
                found->position = supplied.position;
                found->rotation = supplied.rotation;
                found->scale = supplied.scale;
            }
        } else if (operation != "mod-open" && operation != "mod-load" && operation != "new")
            throw std::runtime_error("Unknown park editor operation.");
        begin(std::move(target), 0);
        if (operation == "mod-open" || operation == "mod-load")
            e.status = "Loading " + title_of(argument) + "...";
        return e.status;
    } catch (const std::exception &error) {
        e.status = std::string("error: ") + error.what();
        ++e.revision;
        return e.status;
    }
}
} // namespace dingosdk
