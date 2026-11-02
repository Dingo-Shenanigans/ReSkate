#include "plugin_approval.h"
#include "plugin_api.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include "Engine/Core/Platform/launcher_support.h"
#include <fstream>

namespace dingosdk::plugins {
namespace {
// Each permission as a player reads it, in the words of the documented permissions table, with the
// category chip it is under and how much it matters. Red: other players see it, it changes your game
// settings or it reaches the internet (the table's "What other players see" column). Amber: it controls
// your own game or input, or touches your files. Grey: it only shows things to you or reads. Categories
// come out in the order they first appear here.
// Two calls that could be argued either way: world.environment is red because in a session only the host
// chooses parks, so as host it changes everyone's park; overlay is amber and not harmless because a full
// screen flash is how a plugin can startle or hurt a player (photosensitivity).
struct Capability {
    std::string_view permission, category;
    Risk risk;
    std::string_view phrase;
};
constexpr Capability capabilities[] = {
    {"settings.write", "SETTINGS", Risk::red, "Change your game settings, such as volume and graphics"},
    {"settings.read", "SETTINGS", Risk::grey, "Read your game settings"},
    {"net.http", "WEB", Risk::red, "Send requests over the internet"},
    {"net.session", "PLAYERS", Risk::red, "Send messages to the same plugin on other players' games"},
    {"throwdowns", "PLAYERS", Risk::red, "Host and manage throwdowns"},
    {"session.skaters", "PLAYERS", Risk::amber, "Hide, touch and read other players' skaters"},
    {"session.nametags", "PLAYERS", Risk::amber, "Change nametags on your game"},
    {"voice.control", "PLAYERS", Risk::amber, "Set how loud each player is to you"},
    {"outfit.stand_ins", "PLAYERS", Risk::grey, "Choose what you see in place of other players' items you don't have"},
    {"session.read", "PLAYERS", Risk::grey, "Read the session and its players"},
    {"chat.send", "CHAT", Risk::red, "Send chat lines as you"},
    {"chat.read", "CHAT", Risk::grey, "Read the session's chat"},
    {"chat.local", "CHAT", Risk::grey, "Show chat notices on your screen"},
    {"server", "SERVER", Risk::red, "Moderate, chat and run polls on a server you host"},
    {"server.parks", "SERVER", Risk::red, "Add or remove a server's park mods"},
    {"skater.teleport", "SKATER", Risk::red, "Teleport your skater"},
    {"skater.control", "SKATER", Risk::red, "Control your skater's movement, bails, pushes and physics"},
    {"skater.animation", "SKATER", Risk::red, "Play, edit and record your skater's animations"},
    {"trainer", "SKATER", Risk::red, "Send your skater back to a set point"},
    {"outfit", "SKATER", Risk::red, "Change what your skater wears"},
    {"board", "SKATER", Risk::amber, "Change how your board is drawn"},
    {"skater.read", "SKATER", Risk::grey, "Read your skater's position, speed, state and physics"},
    {"events.tricks", "SKATER", Risk::grey, "Follow your tricks, lines and trick score"},
    {"events.skater", "SKATER", Risk::grey, "Follow your skater's state, bails and launches"},
    {"events.physics", "SKATER", Risk::grey, "Follow your skater's physics every step"},
    {"events.physics.geometry", "SKATER", Risk::grey, "Follow your skater's physics every step"},
    {"world.objects", "WORLD", Risk::red, "List, place, move and remove park objects"},
    // Red: real bodies in your game's physics world, pushed with forces, which the game cannot free until it restarts.
    {"world.physics", "WORLD", Risk::red, "Create physical bodies in your world and push them around"},
    {"world.physics.observe", "WORLD", Risk::red, "See what its rays hit, other skaters' shapes included"},
    {"travel", "WORLD", Risk::red, "Travel to locations and levels"},
    {"world.environment", "WORLD", Risk::red, "Change the time of day, world controls and parks"},
    {"world.entities", "WORLD", Risk::amber, "Spawn objects that only you see"},
    {"world.ai", "WORLD", Risk::amber, "Spawn and clear AI skaters"},
    {"effects", "WORLD", Risk::amber, "Draw particle effects and decals in the world"},
    {"world.raycast", "WORLD", Risk::grey, "Cast rays and pick objects in the world"},
    {"camera", "CAMERA", Risk::amber, "Control the camera"},
    {"input", "INPUT", Risk::amber, "Bind your keys, pads and mouse, and read text input"},
    // Red: it can relabel anything the game shows, its store's buttons and prices included.
    {"strings", "SCREEN", Risk::red, "Change the text the game shows, in its menus and its store"},
    {"capture", "SCREEN", Risk::amber, "Record your screen"},
    {"viewmodel", "SCREEN", Risk::grey, "Draw a model of its own in front of your camera, such as something held"},
    {"hud", "SCREEN", Risk::amber, "Hide your HUD and change the trick ticker"},
    {"overlay", "SCREEN", Risk::amber, "Draw on your screen, with screen effects, toasts and x-ray view"},
    {"ui.screen", "SCREEN", Risk::grey, "Open its own screens and dialogs"},
    {"menu", "SCREEN", Risk::grey, "Edit the game's menu and add its own menu page"},
    {"music", "SOUND", Risk::amber, "Read and control your music playback"},
    {"audio", "SOUND", Risk::grey, "Play, stop and adjust sounds"},
    {"audio.ui", "SOUND", Risk::grey, "Play interface sounds"},
    {"files", "FILES", Risk::amber, "Read and write files in its own data folder"},
    {"room", "GAME", Risk::red, "Open and control the character room, where others see your skater"},
    {"game.speed", "GAME", Risk::amber, "Control slow motion and the speed of the game"},
    {"replay", "GAME", Risk::grey, "Use replays"},
    {"variants", "GAME", Risk::grey, "Choose which variant of its mod your game loads"},
    {"progression.read", "GAME", Risk::grey, "Read your progression"},
    {"mods.read", "GAME", Risk::grey, "Read your installed mods and their content"},
    {"storage", "BASIC", Risk::grey, "Keep its own saved data"},
    {"console", "BASIC", Risk::grey, "Add console commands"}};
}

std::optional<Manifest> read_manifest(const std::filesystem::path& directory) {
    try {
        std::string error;
        const auto text = read_package_file(directory / manifest_file, maximum_manifest_bytes, error);
        if (error.empty()) return parse_manifest(text);
    } catch (const std::exception&) {}
    return std::nullopt;
}

std::optional<Manifest> read_manifest(const PluginSources& sources) {
    const auto found = sources.find(plugin_source_key(manifest_file));
    if (found == sources.end()) return std::nullopt;
    try { return parse_manifest(found->second); } catch (const std::exception&) { return std::nullopt; }
}

PluginSnapshot plugin_snapshot(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    const auto maximum_source_bytes = Limits{}.source_bytes;
    const auto maximum_snapshot_bytes = Limits{}.memory;
    PluginSnapshot result;
    auto sources = std::make_shared<PluginSources>();
    std::string manifest_error;
    const auto manifest_text = read_package_file(directory / manifest_file, maximum_manifest_bytes, manifest_error);
    if (manifest_error.empty()) sources->emplace(plugin_source_key(manifest_file), manifest_text);
    else result.error = std::string(manifest_file) + " " + manifest_error;
    fs::path root = directory;
    if (manifest_error.empty()) {
        try {
            const auto manifest = parse_manifest(manifest_text);
            if (const auto slash = manifest.entry.find('/'); slash != std::string::npos) root = directory / manifest.entry.substr(0, slash);
        } catch (const std::exception&) {}
    }
    std::vector<fs::path> files{directory / manifest_file};
    std::error_code iteration_error;
    for (fs::recursive_directory_iterator it(root, iteration_error), end; !iteration_error && it != end; it.increment(iteration_error)) {
        std::error_code status_error;
        if (it->is_regular_file(status_error)) files.push_back(it->path());
        if (status_error && result.error.empty()) result.error = it->path().string() + " cannot be checked for a source snapshot";
    }
    if (iteration_error && result.error.empty()) result.error = root.string() + " could not be scanned for a source snapshot";
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    // Each file by its own SHA-256, then the sorted list of "path NUL digest newline" lines: no file
    // can lend bytes to the next one's name.
    std::string listing;
    std::size_t captured_bytes = 0;
    for (const auto& file : files) {
        const auto relative = file.lexically_relative(directory).generic_u8string();
        const std::string name(relative.begin(), relative.end());
        std::string digest;
        if (name == manifest_file && manifest_error.empty()) {
            digest = launcher::sha256_bytes(manifest_text);
        } else if (plugin_source_key(name).ends_with(".lua") || plugin_source_key(name).ends_with(".luau")) {
            std::string read_error;
            const auto source = read_package_file(file, maximum_source_bytes, read_error);
            if (read_error.empty()) {
                digest = launcher::sha256_bytes(source);
                if (source.size() > maximum_snapshot_bytes - std::min(captured_bytes, maximum_snapshot_bytes)) {
                    if (result.error.empty()) result.error = name + " makes the plugin source snapshot larger than " +
                                                              std::to_string(maximum_snapshot_bytes >> 20) + " MB";
                } else {
                    captured_bytes += source.size();
                    sources->emplace(plugin_source_key(name), source);
                }
            } else {
                if (result.error.empty()) result.error = name + " " + read_error;
                try { digest = launcher::sha256_file(file); } catch (const std::exception&) { digest = "unreadable"; }
            }
        } else {
            try { digest = launcher::sha256_file(file); } catch (const std::exception&) { digest = "unreadable"; }
        }
        listing += name;
        listing += '\0';
        listing += digest;
        listing += '\n';
    }
    if (manifest_error.empty()) {
        try {
            const auto manifest = parse_manifest(manifest_text);
            if (!sources->contains(plugin_source_key(manifest.entry)) && result.error.empty())
                result.error = manifest.entry + " is missing from the plugin source snapshot";
        } catch (const std::exception&) {}
    }
    result.fingerprint = std::to_string(files.size()) + '-' + launcher::sha256_bytes(listing);
    result.sources = std::move(sources);
    return result;
}

std::string plugin_fingerprint(const std::filesystem::path& directory) {
    return plugin_snapshot(directory).fingerprint;
}

std::string_view permission_category(std::string_view name) {
    for (const auto& capability : capabilities)
        if (capability.permission == name) return capability.category;
    return {};
}

std::vector<PermissionGroup> permission_groups(const Manifest& manifest) {
    std::vector<PermissionGroup> result;
    const auto add = [&](std::string_view category, Risk risk, std::string permission, std::string text) {
        auto group = std::find_if(result.begin(), result.end(), [&](const PermissionGroup& g) { return g.category == category; });
        if (group == result.end()) {
            result.push_back({std::string(category), risk, {}, {}, 0});
            group = result.end() - 1;
        }
        if (!permission.empty() && std::find(group->permissions.begin(), group->permissions.end(), permission) == group->permissions.end())
            group->permissions.push_back(std::move(permission));
        if (std::find(group->phrases.begin(), group->phrases.end(), text) != group->phrases.end()) return;
        // The phrases that make the category as risky as it is come first.
        if (risk > group->risk) {
            group->risk = risk;
            group->loud = 0;
        }
        if (risk == group->risk) group->phrases.insert(group->phrases.begin() + static_cast<std::ptrdiff_t>(group->loud++), std::move(text));
        else group->phrases.push_back(std::move(text));
    };
    for (const auto& capability : capabilities) {
        if (!manifest.permissions.contains(capability.permission)) continue;
        auto text = std::string(capability.phrase);
        if (capability.permission == "net.http" && !manifest.domains.empty()) {
            text += " (";
            for (const auto& domain : manifest.domains) text += (&domain == &manifest.domains.front() ? "" : ", ") + domain;
            text += ')';
        }
        add(capability.category, capability.risk, std::string(capability.permission), std::move(text));
    }
    // A permission this has no words for is shown by its name, and is not taken lightly.
    for (const auto& name : manifest.permissions)
        if (permission_category(name).empty()) add("OTHER", Risk::amber, name, name);
    if (result.empty()) add("BASIC", Risk::grey, {}, "No special permissions: only what every plugin has");
    // The ones that matter first, each risk in the order the table lists its categories.
    std::stable_sort(result.begin(), result.end(), [](const PermissionGroup& a, const PermissionGroup& b) { return a.risk > b.risk; });
    return result;
}

std::string approval_prompt(const std::vector<PermissionGroup>& groups) {
    const auto join = [](const std::vector<std::string>& actions) {
        std::string text;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (i) text += i + 1 == actions.size() ? " and " : ", ";
            text += actions[i];
        }
        return text;
    };
    std::string text = "This mod includes a plugin - a script that runs inside your game.\n\nWhat it can do\n";
    bool has_permissions = false;
    for (const auto& group : groups) {
        if (group.permissions.empty()) continue;
        has_permissions = true;
        const auto has = [&](std::string_view permission) {
            return std::find(group.permissions.begin(), group.permissions.end(), permission) != group.permissions.end();
        };
        std::vector<std::string> actions;
        if (group.category == "SETTINGS") {
            actions.push_back(has("settings.read") && has("settings.write") ? "Read and change game settings"
                               : has("settings.read") ? "Read game settings" : "Change game settings");
        } else if (group.category == "WEB") {
            actions.push_back("Send requests over the internet");
        } else if (group.category == "PLAYERS") {
            std::vector<std::string> session;
            if (has("session.read")) session.push_back("read your session and its players");
            if (has("session.skaters")) session.push_back("hide, touch, or read other players' skaters");
            if (has("session.nametags")) session.push_back("change the nametags you see");
            if (has("voice.control")) session.push_back("set how loud each player sounds to you");
            if (has("outfit.stand_ins")) session.push_back("choose what you see in place of other players' items you don't have");
            if (!session.empty()) actions.push_back(join(session));
            if (has("net.session")) actions.push_back("send messages to other players using the same plugin");
            if (has("throwdowns")) actions.push_back("host and manage throwdowns");
        } else if (group.category == "CHAT") {
            if (has("chat.read")) actions.push_back("read chat");
            if (has("chat.send")) actions.push_back("send chat lines as you");
            if (has("chat.local")) actions.push_back("show chat notices on your screen");
        } else if (group.category == "SERVER") {
            if (has("server")) actions.push_back("moderate servers you host");
            if (has("server.parks")) actions.push_back("add or remove park mods on servers you host");
        } else if (group.category == "SKATER") {
            if (has("skater.teleport")) actions.push_back("teleport your skater");
            if (has("skater.control")) actions.push_back("control your skater's movement, bails, pushes, and physics");
            if (has("trainer")) actions.push_back("send your skater back to a set point");
            if (has("skater.animation")) actions.push_back("play, edit, or record skater animations");
            if (has("outfit")) actions.push_back("change what your skater wears");
            if (has("board")) actions.push_back("change how your board is drawn");
            if (has("skater.read")) actions.push_back("read your skater's position, speed, state, and physics");
            if (has("events.tricks")) actions.push_back("follow your tricks, lines, and score");
            if (has("events.skater")) actions.push_back("follow your skater's state, bails, and launches");
            if (has("events.physics.geometry")) actions.push_back("follow detailed skater physics, including geometry, each step");
            else if (has("events.physics")) actions.push_back("follow your skater's physics each step");
        } else if (group.category == "WORLD") {
            if (has("world.objects")) actions.push_back("list and edit park objects");
            if (has("world.physics")) actions.push_back("create physical bodies in your world and push them around");
            if (has("world.physics.observe")) actions.push_back("see what its rays hit, other skaters' shapes included");
            if (has("travel")) actions.push_back("travel to locations and levels");
            if (has("world.environment")) actions.push_back("change parks, world controls, and time of day");
            if (has("world.entities")) actions.push_back("spawn objects visible only to you");
            if (has("world.ai")) actions.push_back("spawn or clear AI skaters");
            if (has("effects")) actions.push_back("draw particle effects and decals");
            if (has("world.raycast")) actions.push_back("probe the world and pick objects");
        } else if (group.category == "CAMERA") {
            actions.push_back("control the camera");
        } else if (group.category == "INPUT") {
            actions.push_back("bind keys, pads, and mouse, and read text input");
        } else if (group.category == "SCREEN") {
            if (has("strings")) actions.push_back("change the text the game shows, in its menus and its store");
            if (has("capture")) actions.push_back("record your screen");
            if (has("viewmodel")) actions.push_back("draw a model in front of your camera");
            if (has("hud")) actions.push_back("hide your HUD or change the trick ticker");
            if (has("overlay")) actions.push_back("draw on screen, including screen effects");
            if (has("ui.screen")) actions.push_back("open plugin screens and dialogs");
            if (has("menu")) actions.push_back("edit the game's menu and add menu pages");
        } else if (group.category == "SOUND") {
            if (has("music")) actions.push_back("read and control music playback");
            if (has("audio")) actions.push_back("play, stop, and adjust sounds");
            if (has("audio.ui")) actions.push_back("play interface sounds");
        } else if (group.category == "FILES") {
            actions.push_back("read and write files in its own data folder");
        } else if (group.category == "GAME") {
            if (has("game.speed")) actions.push_back("control game speed and slow motion");
            if (has("room")) actions.push_back("open and control the character room, where others see your skater");
            if (has("replay")) actions.push_back("use replays");
            if (has("variants")) actions.push_back("choose variants for its mod");
            if (has("progression.read")) actions.push_back("read progression");
            if (has("mods.read")) actions.push_back("read installed mods and their content");
        } else if (group.category == "BASIC") {
            if (has("storage")) actions.push_back("keep its own saved data");
            if (has("console")) actions.push_back("add console commands");
        } else {
            for (const auto& permission : group.permissions) actions.push_back("use the " + permission + " permission");
        }
        if (!actions.empty()) {
            auto summary = join(actions);
            if (summary.front() >= 'a' && summary.front() <= 'z') summary.front() = static_cast<char>(summary.front() - ('a' - 'A'));
            text += "- " + summary + "\n";
        }
    }
    if (!has_permissions) text += "- No special permissions\n";
    text += "\nOnly turn it on if you trust its source. Full permissions: Insert menu -> Plugins -> Plugin Manager.\n";
    text += "The rest of the mod works with the plugin off.";
    return text;
}

Approvals::Approvals(std::filesystem::path file) : file_(std::move(file)) {
    try {
        std::ifstream input(file_, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto json = Json::parse(text, {1 << 20, 4, 65536});
        for (const auto& [id, record] : json.items())
            if (record.is_object() && record.contains("fingerprint") && record.at("fingerprint").is_string())
                records_[id] = {record.at("fingerprint").string(), record.contains("on") && record.at("on").is_boolean() && record.at("on").get<bool>()};
    } catch (const std::exception&) {} // no file yet, or one that cannot be read: every plugin asks
}

Approval Approvals::state(std::string_view id, std::string_view fingerprint) {
    std::unique_lock lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) return Approval::asking;
    if (found->second.fingerprint != fingerprint) {
        if (!developer_) return Approval::asking;
        const bool on = found->second.on;
        lock.unlock();
        set(id, fingerprint, on);
        return on ? Approval::on : Approval::off;
    }
    return found->second.on ? Approval::on : Approval::off;
}

void Approvals::set(std::string_view id, std::string_view fingerprint, bool on) {
    std::lock_guard lock(mutex_);
    records_[std::string(id)] = {std::string(fingerprint), on};
    auto json = Json::object();
    for (const auto& [name, record] : records_) {
        auto& item = json[name];
        item["fingerprint"] = record.fingerprint;
        item["on"] = record.on;
    }
    try {
        std::filesystem::create_directories(file_.parent_path());
        auto temporary = std::filesystem::path(file_) += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            const auto text = json.dump(2);
            if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("cannot write it");
        }
        std::filesystem::rename(temporary, file_);
    } catch (const std::exception&) { // kept for this run; the next answer tries to save again
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::path(file_) += ".tmp", ignored);
    }
}
}
