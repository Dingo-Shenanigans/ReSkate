#include "plugin_manifest.h"
#include "plugin_effects.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Vfs/resident_assets.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/effects.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace dingosdk::plugins {
namespace {
// What each permission opens, where it is not plain from its name: "skater.control" the skater's
// push (with push_wait / push_reset), hold, flip speed (reskate.skater.flip_speed) and the competition's
// stock physics (reskate.physics);
// "camera" the free and pad-flown cameras and the Bone Cam (reskate.skater.bone_cam);
// "session.read" spectating, the session's world (reskate.session.world) and what other players wear
// (reskate.session.outfit); "session.skaters" also hiding other players, or some of their items
// (reskate.session.hide, hide_items); "events.tricks" the
// trick events, the trick list's feed and flick diagrams (reskate.tricks); "throwdowns" the
// throwdown kit (modes, documents, the native flag); "server.parks" a dedicated server's park mods
// (reskate.server.parks, park_spawn, park_remove); "variants" choosing which of its own mod's
// variants is in the game (reskate.package.choose); "viewmodel" a model of its own drawn in front of
// the camera (reskate.viewmodel); "strings" the text the game shows for its ids (reskate.strings).
// "outfit.stand_ins" what this PC shows in place of other players' items it does not have
// (reskate.outfit.stand_ins, Extension/Multiplayer/Remote/stand_ins.h); never a paid store item.
// "world.physics" native rigid bodies the plugin
// owns (reskate.world.physics: create, read, submit, receipt, acknowledge, release, prepare, limits) and,
// with "world.raycast", ray batches beside one (probe_batch). "world.physics.observe" adds
// probe_batch's passive diagnostics on top of those two: include_shape_categories (a read-time
// category mask), include_hit_owner (an equality-only token for a hit shape's owner slot) and
// query_preset "all" (a filter that tests every shape with a category, actors' shapes included).
// It grants no force, raw filter, exclusion or actor identity. probe_batch's
// require_named_actor_categories_absent shows no category and needs only "world.physics" and
// "world.raycast". "world.physics.reuse" needs "world.physics" too and lets create re-activate
// a body this same VM instance parked, with identical capsules, mass and inertia, instead of
// spending another of the process's native allocations; it grants no slot choice, mask, force or
// other owner's body. A manifest's "resident" list and "variants" are the merge's
// (Engine/Vfs/mod_list.h ResidentAssets, Variants), not the plugin host's.
// "objects.physics" lets the plugin claim its own local park placements (at most 32, offline only)
// and make them fall, roll and be knocked over (reskate.objects.physics, velocity, pose, motion;
// Extension/Objects/placement_physics.h); a claimed object's moved pose is never saved to the park.
// "marker.read" reads the local skater's marker saves and returns (reskate.marker.events).
// "ui.models" publishes bounded plugin-namespaced floats for authored native UI (not stock fields).
// "hud.score" shows the plugin's own line (trick names, points, multiplier, the line's total) in
// skate.'s score HUD (reskate.hud.score_line, Extension/UI/score_line_hud.h): display only, from a
// model of the SDK's own, never the game's score, challenges or progression. It stands apart from
// "hud" so plugins approved for "hud" are not given a new thing on screen without being asked.
// "screen.shader" runs the plugin's own pixel shader over the finished frame (reskate.screen.shader,
// Extension/UI/Overlay/screen_shader.h): its only output is the picture, and ReSkate times it on the GPU and turns
// it off when it takes too long. It stands apart from "overlay": it is code of the plugin's own on the GPU.
// "world.shader" runs the plugin's own pixel shader in the game's lit picture of the world, before its UI, with the
// world's depth, the camera and the plugin's data (reskate.world.shader, Extension/UI/Overlay/world_shader.h); the
// same watch on the GPU, and its only output is the picture.
// "world.sets" places the game's own set pieces (reskate.entities.piece; Extension/World/world_entities.h
// set_piece): a character light rig or a menu backdrop, for the plugin's own player only, at most 8.
// Only prefabs under lighting/prefabs/ and the presentation backgrounds: never a gameplay, store or
// progression prefab.
// "skater.copy" draws a copy of the local skater somewhere else (reskate.skater.copy;
// Extension/Multiplayer/Session/skater_copy.h): this game's alone, never in a session, nothing sent.
// "replay" grants nothing yet: no API checks it. It stays known so a manifest that lists it still
// loads (an unknown permission refuses the whole manifest).
constexpr std::array<std::string_view, 70> permissions{
    "console", "storage", "overlay", "menu", "ui.screen", "input", "input.write", "hud", "events.tricks", "events.skater",
    "skater.read", "skater.teleport", "skater.control", "skater.animation", "trainer", "camera",
    "world.raycast", "world.vehicles", "world.physics", "world.physics.observe", "world.physics.reuse", "world.objects", "world.entities", "world.environment", "world.ai", "travel", "session.read",
    "session.skaters", "session.nametags", "net.session", "net.http", "voice.control", "chat.read",
    "chat.send", "chat.local", "chat.commands", "server", "server.parks", "throwdowns", "replay", "music", "progression.read", "settings.read", "audio.ui", "audio", "outfit", "board", "mods.read",
    "events.physics", "game.speed", "files", "variants", "settings.write", "room", "capture", "effects", "events.physics.geometry", "viewmodel", "strings", "outfit.stand_ins",
    "objects.physics", "marker.read", "ui.models", "hud.score", "effects.game", "screen.shader", "world.shader", "world.sets", "skater.copy",
    "files.folders"};
// "files.folders" reads and writes files in folders beside the game that the manifest's "folders" names
// (reskate.files with "<folder>:<path>"): for files a player keeps, shares or brings from another tool,
// where a data folder under Mods would hide them. Never one of the game's or ReSkate's own folders.
}

bool granted_folder_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64 || name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ') return false;
    std::string lower;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-';
        if (!plain) return false;
        lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    if (lower.find("..") != std::string::npos) return false;
    // The game's and ReSkate's own, and what Windows keeps for devices.
    static constexpr std::array<std::string_view, 25> kept{"data", "mods", "moddata", "logs", "patch", "update", "__installer", "support", "engine",
        "core", "win32", "win64", "easyanticheat", "eaanticheat", "installer", "reskate", "plugins", "crashes", "cache", "profiles",
        "con", "prn", "aux", "nul", "steam_settings"};
    const auto base = std::string_view(lower).substr(0, lower.find('.'));
    if (std::find(kept.begin(), kept.end(), lower) != kept.end() || std::find(kept.begin(), kept.end(), base) != kept.end()) return false;
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9') return false;
    return true;
}

bool known_permission(std::string_view name) noexcept {
    for (const auto known : permissions) if (known == name) return true;
    return false;
}

std::vector<std::string_view> known_permissions() { return {permissions.begin(), permissions.end()}; }

std::string safe_relative_path(std::string_view path) {
    if (path.empty() || path.size() > 260 || path.front() == '/') return {};
    for (const unsigned char c : path)
        if (c < 0x20 || c > 0x7e || c == '\\' || c == ':') return {};
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") return {};
        // Windows drops trailing dots and spaces, so "a." would open "a".
        if (segment.back() == '.' || segment.back() == ' ') return {};
        start = end + 1;
    }
    return std::string(path);
}

bool names_a_device(std::string_view path) noexcept {
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        // (What comes before a name's first dot is what Windows looks at.)
        const auto base = path.substr(start, std::min(path.find('.', start), end) - start);
        const auto begins = [&](std::string_view word) {
            return std::equal(word.begin(), word.end(), base.begin(), [](char w, char c) { return (c >= 'A' && c <= 'Z' ? c + 32 : c) == w; });
        };
        if (base.size() == 3 && (begins("con") || begins("prn") || begins("aux") || begins("nul"))) return true;
        if (base.size() == 4 && (begins("com") || begins("lpt")) && base[3] >= '0' && base[3] <= '9') return true;
        start = end + 1;
    }
    return false;
}

const char* side_name(Side side) noexcept {
    return side == Side::server ? "server" : side == Side::both ? "both" : "client";
}

Manifest parse_manifest(std::string_view text) {
    if (text.size() > maximum_manifest_bytes) throw std::runtime_error("reskate-plugin.json is larger than 64 KB");
    Json json;
    try {
        json = Json::parse(text, {maximum_manifest_bytes, 8, 4096});
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("reskate-plugin.json is not valid JSON: ") + error.what());
    }
    if (!json.is_object()) throw std::runtime_error("reskate-plugin.json must hold a JSON object");
    Manifest result;
    if (!json.contains("api") || !json.at("api").is_number_integer())
        throw std::runtime_error("reskate-plugin.json needs \"api\": the API version it was written for (1)");
    const auto api = json.at("api").get<std::int64_t>();
    if (api < 1) throw std::runtime_error("\"api\" must be 1 or more");
    if (api > api_version)
        throw std::runtime_error("written for plugin API " + std::to_string(api) + ", and this ReSkate has API " +
            std::to_string(api_version) + ": update ReSkate");
    result.api = static_cast<int>(api);
    if (!json.contains("entry") || !json.at("entry").is_string())
        throw std::runtime_error("reskate-plugin.json needs \"entry\": the Lua file to run, such as \"plugin/main.lua\"");
    result.entry = safe_relative_path(json.at("entry").string());
    if (result.entry.empty())
        throw std::runtime_error("\"entry\" must be a path inside the mod folder, with / between folders and no ..");
    if (!result.entry.ends_with(".lua") && !result.entry.ends_with(".luau"))
        throw std::runtime_error("\"entry\" must be a .lua or .luau file");
    if (json.contains("side")) {
        const auto& side = json.at("side");
        const auto name = side.is_string() ? side.string() : std::string{};
        if (name == "client") result.side = Side::client;
        else if (name == "server") result.side = Side::server;
        else if (name == "both") result.side = Side::both;
        else throw std::runtime_error("\"side\" must be \"client\", \"server\" or \"both\"");
    }
    if (json.contains("permissions")) {
        const auto& list = json.at("permissions");
        if (!list.is_array()) throw std::runtime_error("\"permissions\" must be a list of names");
        for (const auto& item : list) {
            if (!item.is_string()) throw std::runtime_error("\"permissions\" must be a list of names");
            const auto& name = item.string();
            if (!known_permission(name))
                throw std::runtime_error("unknown permission \"" + name + "\": check the spelling, or update ReSkate");
            result.permissions.insert(name);
        }
    }
    if (result.permissions.contains("events.physics.geometry") && !result.permissions.contains("events.physics"))
        throw std::runtime_error("events.physics.geometry also needs the events.physics permission");
    if (result.permissions.contains("world.physics.reuse") && !result.permissions.contains("world.physics"))
        throw std::runtime_error("world.physics.reuse also needs the world.physics permission");
    if (result.permissions.contains("effects.game") && !result.permissions.contains("effects"))
        throw std::runtime_error("effects.game also needs the effects permission");
    if (json.contains("effects")) {
        const auto& list = json.at("effects");
        if (!result.permissions.contains("effects") || !list.is_array() || list.size() > 128)
            throw std::runtime_error("effects needs the effects permission and at most 128 private asset names");
        const auto resident = json.contains("resident") ? mods::parse_resident_assets(json.at("resident")) :
            std::vector<mods::ResidentAssets>{};
        for (const auto& item : list) {
            if (!item.is_string() || (!private_effect_asset(item.string(), false) && !private_effect_asset(item.string(), true)))
                throw std::runtime_error("effects accepts private reskate particle/material paths only; stock and store assets are refused");
            const auto& name = item.string();
            const bool covered = std::any_of(resident.begin(), resident.end(), [&](const auto& bundle) {
                if (bundle.bundle != addr::effects::resident_bundle) return false;
                return std::any_of(bundle.roots.begin(), bundle.roots.end(), [&](const auto& root) {
                    return root.ends_with('/') ? name.starts_with(root) : name == root;
                });
            });
            if (!covered) throw std::runtime_error("every effect must be covered by resident roots in " +
                std::string(addr::effects::resident_bundle) + ": " + name);
            result.effects.insert(name);
        }
    }
    if (json.contains("domains")) {
        const auto& list = json.at("domains");
        if (!list.is_array() || list.size() > 16) throw std::runtime_error("\"domains\" must be a list of at most 16 host names");
        for (const auto& item : list) {
            if (!item.is_string()) throw std::runtime_error("\"domains\" must be a list of host names");
            std::string name = item.string();
            for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            const bool valid = !name.empty() && name.size() <= 253 && name.front() != '.' && name.back() != '.' &&
                               name.find("..") == std::string::npos && name.find('.') != std::string::npos &&
                               std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'; });
            if (!valid) throw std::runtime_error("\"" + item.string() + "\" is not a host name for \"domains\" (such as api.example.com: no scheme, port, path or *)");
            result.domains.push_back(name);
        }
        if (!result.domains.empty() && !result.permissions.contains("net.http"))
            throw std::runtime_error("\"domains\" is for web requests: add the \"net.http\" permission too");
    }
    if (json.contains("folders")) {
        const auto& list = json.at("folders");
        if (!list.is_array() || list.size() > 4) throw std::runtime_error("\"folders\" must be a list of at most 4 folder names");
        for (const auto& item : list) {
            if (!item.is_string() || !granted_folder_name(item.string()))
                throw std::runtime_error("\"folders\" names folders beside the game: 1 to 64 letters, digits, spaces, . _ and -, and none of the game's or ReSkate's own");
            if (std::find(result.folders.begin(), result.folders.end(), item.string()) == result.folders.end()) result.folders.push_back(item.string());
        }
        if (!result.folders.empty() && !result.permissions.contains("files.folders"))
            throw std::runtime_error("\"folders\" keeps files beside the game: add the \"files.folders\" permission too");
    }
    if (json.contains("content")) {
        const auto& list = json.at("content");
        if (!list.is_array() || list.size() > 8) throw std::runtime_error("\"content\" must be a list of at most 8 folder names");
        for (const auto& item : list) {
            if (!item.is_string()) throw std::runtime_error("\"content\" must be a list of folder names");
            std::string name = item.string();
            for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            const bool valid = !name.empty() && name.size() <= 32 &&
                               std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
            if (!valid) throw std::runtime_error("\"" + item.string() + "\" is not a folder name for \"content\" (1 to 32 letters, digits, _ and -)");
            if (std::find(result.content.begin(), result.content.end(), name) == result.content.end()) result.content.push_back(name);
        }
        if (!result.content.empty() && !result.permissions.contains("mods.read"))
            throw std::runtime_error("\"content\" reads other mods' folders: add the \"mods.read\" permission too");
    }
    // Fonts for its drawn texts. No permission of their own: a text is drawn under the permission
    // drawing needs already. (That each is a file is check_manifest_files's, which has the folder.)
    if (json.contains("fonts")) {
        const auto& list = json.at("fonts");
        if (!list.is_array() || list.size() > maximum_fonts) throw std::runtime_error("\"fonts\" must be a list of at most 8 font files");
        for (const auto& item : list) {
            if (!item.is_string()) throw std::runtime_error("\"fonts\" must be a list of font files, such as \"fonts/Display-Regular.ttf\"");
            const auto path = safe_relative_path(item.string());
            if (path.empty())
                throw std::runtime_error("\"" + item.string() + "\" in \"fonts\" must be a path inside the mod folder, with / between folders and no ..");
            std::string lower = path;
            for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            if (!lower.ends_with(".ttf") && !lower.ends_with(".otf")) throw std::runtime_error("\"" + path + "\" in \"fonts\" must be a .ttf or .otf file");
            if (std::find(result.fonts.begin(), result.fonts.end(), path) == result.fonts.end()) result.fonts.push_back(path);
        }
    }
    return result;
}

std::string check_manifest_files(const Manifest& manifest, const std::filesystem::path& directory) {
    std::error_code code;
    const auto root = std::filesystem::weakly_canonical(directory, code);
    for (const auto& font : manifest.fonts) {
        const auto file = directory / std::filesystem::path(font);
        const auto missing = "\"" + font + "\" in \"fonts\" is not a file in the mod folder";
        if (code || !std::filesystem::is_regular_file(file, code) || code) return missing;
        // (Once more with links and junctions followed, as a path a plugin names is checked.)
        const auto inside = std::filesystem::weakly_canonical(file, code).lexically_relative(root);
        if (code || inside.empty() || *inside.begin() == "..") return missing;
        const auto size = std::filesystem::file_size(file, code);
        if (code || size == 0) return "\"" + font + "\" in \"fonts\" cannot be read";
        if (size > maximum_font_bytes) return "\"" + font + "\" in \"fonts\" is larger than 8 MB";
    }
    return {};
}

namespace {
constexpr std::array<std::string_view, 22> runnable{".exe", ".dll", ".sys", ".scr", ".com", ".cpl", ".msi", ".msp",
    ".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh", ".hta", ".lnk", ".pif", ".reg"};
constexpr std::size_t most_files = 20000;

// path::string() throws on names outside the ANSI code page; a name here is only shown or compared.
std::string ascii_lowercase(const std::filesystem::path& path) {
    std::string text;
    for (const auto c : path.generic_wstring())
        text += c < 0x80 ? static_cast<char>(c >= L'A' && c <= L'Z' ? c + 32 : c) : '?';
    return text;
}
}

std::string check_package_files(const std::filesystem::path& directory) {
    std::error_code code;
    std::size_t files = 0;
    for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, code), end;
         !code && it != end; it.increment(code)) {
        std::error_code entry_code;
        if (!it->is_regular_file(entry_code)) continue;
        if (++files > most_files) return "it holds more than 20000 files";
        const auto shown = ascii_lowercase(std::filesystem::relative(it->path(), directory, entry_code));
        const auto extension = ascii_lowercase(it->path().extension());
        bool program = std::find(runnable.begin(), runnable.end(), extension) != runnable.end();
        if (!program) {
            std::ifstream in(it->path(), std::ios::binary);
            char magic[2]{};
            program = in.read(magic, 2) && magic[0] == 'M' && magic[1] == 'Z';
        }
        if (program) return "it holds " + shown + ", a program or a script Windows runs. A plugin never ships one";
    }
    return {};
}

}
