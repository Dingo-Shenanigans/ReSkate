#include "server_parks.h"
#include "server_host.h"
#include "server_text.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"
#include "Extension/Multiplayer/Session/object_state.h"
#include <algorithm>
#include <fstream>
#include <optional>
#include <stdexcept>

// Park mods: the owner lists them in ReSkateServer.json (maps.park_mods), and the console, an
// admin or a custom vote spawns one while the map it was built on runs. Its objects belong to the
// server itself: they are sent as the layout of the server's own SteamID and epoch, which every
// player's game already takes from the server as it takes another player's (a game server's
// SteamID is never a player's). They count against nobody's object limit, nobody can move or
// delete them, and they stay when players leave; they go on a map change or a restart.
namespace dingosdk::server {
namespace fs = std::filesystem;
namespace {
constexpr std::string_view park_suffix = ".park.json";
std::string read_file(const fs::path &file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error) throw std::runtime_error("Cannot read " + path_utf8(file.filename()) + ".");
    if (size > max_park_file_bytes) throw std::runtime_error(path_utf8(file.filename()) + " is larger than 2 MB.");
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Cannot read " + path_utf8(file.filename()) + ".");
    return text;
}
bool read_floats(const Json &list, float *out, std::size_t count) {
    if (!list.is_array() || list.size() != count) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!list.at(i).is_number()) return false;
        out[i] = list.at(i).get<float>();
    }
    return true;
}
} // namespace

LoadedPark load_park_mod(const fs::path &mods, const ParkMod &park) {
    if (!valid_mod_folder(park.folder)) throw std::runtime_error("\"" + park.folder + "\" is not a mod folder name.");
    const auto folder = mods / fs::path(park.folder);
    std::error_code error;
    if (!fs::is_directory(folder, error))
        throw std::runtime_error("The mod folder \"" + park.folder + "\" is not in " + path_utf8(mods) + ".");
    const auto parks = folder / "parks";
    LoadedPark loaded;
    if (!park.key.empty() && !valid_park_key(park.key)) throw std::runtime_error("\"" + park.key + "\" is not a base map's key.");
    loaded.key = park.key;
    if (loaded.key.empty()) {
        // Without a key, the mod's only park.
        std::vector<std::string> keys;
        for (fs::directory_iterator it(parks, error), end; !error && it != end; it.increment(error)) {
            auto name = path_utf8(it->path().filename());
            if (!it->is_regular_file(error) || name.size() <= park_suffix.size() || !name.ends_with(park_suffix)) continue;
            name.resize(name.size() - park_suffix.size());
            if (valid_park_key(name)) keys.push_back(std::move(name));
        }
        std::sort(keys.begin(), keys.end());
        if (keys.empty()) throw std::runtime_error("The mod \"" + park.folder + "\" has no park (parks/<map>.park.json).");
        if (keys.size() > 1) {
            std::string list;
            for (const auto &key : keys) list += (list.empty() ? "" : ", ") + key;
            throw std::runtime_error("The mod \"" + park.folder + "\" has parks for " + list + ": set \"key\" to one of them.");
        }
        loaded.key = keys.front();
    }
    const auto file = parks / fs::path(loaded.key + std::string(park_suffix));
    if (!fs::is_regular_file(file, error))
        throw std::runtime_error("The mod \"" + park.folder + "\" has no park for " + loaded.key + " (parks/" + loaded.key + ".park.json).");
    const auto where = park.folder + "/parks/" + loaded.key + ".park.json";
    Json root;
    try {
        root = Json::parse(read_file(file));
    } catch (const std::exception &e) {
        throw std::runtime_error(where + " cannot be read: " + e.what());
    }
    if (!root.is_object() || !root.contains("format") || !root.at("format").is_string() || root.at("format").string() != "reskate-park")
        throw std::runtime_error(where + " is not a ReSkate park.");
    if (root.contains("schema_version") && (!root.at("schema_version").is_number() || root.at("schema_version").get<double>() != 1))
        throw std::runtime_error(where + " is from a newer ReSkate. Update the server.");
    if (!root.contains("maps") || !root.at("maps").is_object() || !root.at("maps").contains(loaded.key))
        throw std::runtime_error(where + " has no objects for " + loaded.key + ".");
    const auto &entries = root.at("maps").at(loaded.key);
    if (!entries.is_array()) throw std::runtime_error(where + " is not a ReSkate park.");
    if (entries.size() > multiplayer::max_owned_objects)
        throw std::runtime_error(where + " has " + std::to_string(entries.size()) + " objects; at most " +
                                 std::to_string(multiplayer::max_owned_objects) + " can be shown.");
    // Sizes other than 1 are an extension of the format: "object_scales": {map: {id: scale}}.
    const Json *scales{};
    if (root.contains("object_scales") && root.at("object_scales").is_object() && root.at("object_scales").contains(loaded.key) &&
        root.at("object_scales").at(loaded.key).is_object())
        scales = &root.at("object_scales").at(loaded.key);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries.at(index);
        NetworkObject object;
        object.id = loaded.objects.size() + 1;
        bool ok = entry.is_object() && entry.contains("item") && entry.at("item").is_string() && entry.contains("position") &&
                  entry.contains("rotation") && read_floats(entry.at("position"), object.position.data(), 3) &&
                  read_floats(entry.at("rotation"), object.rotation.data(), 4);
        if (ok) object.item = entry.at("item").string();
        if (ok && scales && entry.contains("id") && entry.at("id").is_number()) {
            const auto id = std::to_string(entry.at("id").get<std::uint64_t>());
            if (scales->contains(id)) {
                ok = scales->at(id).is_number();
                if (ok) object.scale = scales->at(id).get<float>();
            }
        }
        // What every player's game checks of an object it is sent: a Build Kit item, a place on
        // the map, a real rotation, a size it can show.
        if (!ok || !multiplayer::valid_network_object(object)) {
            auto item = ok ? object.item : std::string("(broken entry)");
            if (item.size() > 64) item.resize(64);
            loaded.skipped.push_back("#" + std::to_string(index + 1) + " " + item);
            continue;
        }
        loaded.objects.push_back(std::move(object));
    }
    if (loaded.objects.empty())
        throw std::runtime_error(where + (entries.empty() ? " has no objects." : " has no objects players' games can show (Build Kit items)."));
    return loaded;
}

namespace {
std::string listed(const std::vector<std::string> &names) {
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i)
        text += (i ? (i + 1 == names.size() ? " and " : ", ") : "") + names[i];
    return text;
}
} // namespace

void Host::publish_parks() {
    std::vector<NetworkObject> layout;
    for (const auto &park : parks_spawned_) layout.insert(layout.end(), park.objects.begin(), park.objects.end());
    if (!park_objects_.revision() && layout.empty()) return;
    park_objects_.replace(layout);
}

void Host::remove_parks(std::string_view reason) {
    if (parks_spawned_.empty()) return;
    for (const auto &park : parks_spawned_) log_("[parks] " + park.name + " removed (" + std::string(reason) + ")");
    parks_spawned_.clear();
    publish_parks();
}

void Host::tick_parks() {
    std::vector<std::string> gone;
    for (auto &park : parks_spawned_) {
        if (!park.expires) continue;
        if (now_ >= park.expires) {
            gone.push_back(park.name);
        } else if (!park.warned && park.minutes > 1 && park.expires - now_ <= 60000000) {
            park.warned = true;
            send_chat("The " + park.name + " park goes in a minute.");
        }
    }
    if (gone.empty()) return;
    std::erase_if(parks_spawned_, [&](const SpawnedPark &park) { return std::find(gone.begin(), gone.end(), park.name) != gone.end(); });
    for (const auto &name : gone) {
        log_("[parks] " + name + " removed (expired)");
        send_chat("The " + name + " park is gone.");
    }
    publish_parks();
}

// "park-mod list | add <name> [<name>...] [minutes] | remove <name> [<name>...] | clear". What it
// did is a "[parks]" line: the console and a vote log the reply, so it is logged here only for an
// admin in game. A vote's refusal also goes to chat, where the players who voted can read it.
std::string Host::park_mod_command(std::string_view argument, std::uint64_t admin) {
    const auto [action, rest] = split(argument);
    const auto verb = lower(action);
    const auto *by = admin ? find(admin) : nullptr;
    const std::string who = by_vote_ ? "a vote" : by ? guest_name(*by) : admin ? std::to_string(admin) : "console";
    const auto done = [&](const std::vector<std::string> &lines) {
        std::string text;
        for (const auto &line : lines) {
            if (admin) log_(line);
            text += (text.empty() ? "" : "\n") + line;
        }
        return text;
    };
    const auto spawned = [&](std::string_view name) {
        return std::find_if(parks_spawned_.begin(), parks_spawned_.end(), [&](const auto &park) { return park.name == name; });
    };
    const auto configured = [&](std::string_view name) -> const ParkMod * {
        for (const auto &park : config_.park_mods)
            if (park.name == name) return &park;
        return nullptr;
    };
    const auto objects = [&] {
        std::size_t count{};
        for (const auto &park : parks_spawned_) count += park.objects.size();
        return count;
    };
    std::vector<std::string> words;
    for (auto left = rest; !left.empty();) {
        const auto [word, more] = split(left);
        words.push_back(lower(word));
        left = more;
    }
    if (verb.empty() || verb == "list") {
        if (config_.park_mods.empty()) return "No park mods are set up (maps.park_mods in ReSkateServer.json).";
        std::string text = "Park mods (" + std::to_string(parks_spawned_.size()) + " of " + std::to_string(config_.park_limits.parks) +
                           " spawned, " + std::to_string(objects()) + " of " + std::to_string(config_.park_limits.objects) + " objects):";
        for (const auto &park : config_.park_mods) {
            text += "\n  " + park.name + "  \"" + park.folder + "\"" + (park.key.empty() ? "" : " (" + park.key + ")") + " on " + park.map;
            if (const auto found = spawned(park.name); found != parks_spawned_.end()) {
                text += "  spawned, " + std::to_string(found->objects.size()) + " objects, ";
                text += found->expires ? std::to_string((found->expires - std::min(found->expires, now_) + 59999999) / 60000000) + " min left"
                                       : std::string("until removed");
            }
        }
        return text;
    }
    if (verb == "add") {
        std::optional<unsigned> minutes;
        if (!words.empty() && number(words.back())) {
            const auto value = *number(words.back());
            words.pop_back();
            if (value < 1 || value > 1440) return "Minutes must be 1 to 1440.";
            minutes = static_cast<unsigned>(value);
        }
        if (words.empty()) return "park-mod add <name> [<name>...] [minutes]";
        const auto names = listed(words);
        // Everything is checked before anything is spawned: all of them, or none.
        const auto refuse = [&](const std::string &why) {
            if (by_vote_) send_chat("The " + names + " park" + (words.size() > 1 ? "s" : "") + " could not be spawned: " + why);
            return done({"[parks] " + names + " could not be spawned: " + why});
        };
        std::vector<const ParkMod *> chosen;
        for (const auto &name : words) {
            const auto *park = configured(name);
            if (!park) return refuse("there is no park mod called \"" + name + "\" (park-mod list).");
            if (std::find(chosen.begin(), chosen.end(), park) != chosen.end()) return refuse(name + " is named twice.");
            if (spawned(name) != parks_spawned_.end()) return refuse(name + " is already spawned.");
            if (!same_map(park->map)) return refuse(name + " belongs on " + park->map + ", and the server is on " + map_name() + ".");
            chosen.push_back(park);
        }
        if (parks_spawned_.size() + chosen.size() > config_.park_limits.parks)
            return refuse("at most " + std::to_string(config_.park_limits.parks) + " parks can be spawned at once (" +
                          std::to_string(parks_spawned_.size()) + " are).");
        std::vector<LoadedPark> loaded;
        auto total = objects();
        for (const auto *park : chosen) {
            try {
                loaded.push_back(load_park_mod(config_.mods, *park));
            } catch (const std::exception &e) {
                return refuse(e.what());
            }
            total += loaded.back().objects.size();
        }
        if (total > config_.park_limits.objects)
            return refuse("that makes " + std::to_string(total) + " park objects, and at most " + std::to_string(config_.park_limits.objects) +
                          " can be spawned at once.");
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            SpawnedPark park;
            park.name = chosen[i]->name;
            park.objects = std::move(loaded[i].objects);
            // IDs of the server's own: two parks may number their objects alike.
            for (auto &object : park.objects) object.id = ++park_object_ids_;
            if (minutes) {
                park.minutes = *minutes;
                park.expires = now_ + std::uint64_t{*minutes} * 60000000;
            }
            lines.push_back("[parks] " + park.name + " spawned (" + std::to_string(park.objects.size()) + " objects, " +
                            (minutes ? std::to_string(*minutes) + " min" : std::string("until removed")) + ") by " + who);
            if (const auto &skipped = loaded[i].skipped; !skipped.empty()) {
                std::string items;
                for (std::size_t j = 0; j < skipped.size() && j < 8; ++j) items += (j ? ", " : "") + skipped[j];
                lines.push_back("[parks] " + park.name + ": skipped " + std::to_string(skipped.size()) +
                                " object(s) players' games would not show: " + items + (skipped.size() > 8 ? ", ..." : ""));
            }
            parks_spawned_.push_back(std::move(park));
        }
        publish_parks();
        send_chat((words.size() > 1 ? "The " + names + " parks are" : "The " + names + " park is") +
                  (minutes ? " here for " + std::to_string(*minutes) + " min." : std::string(" here.")));
        return done(lines);
    }
    if (verb == "remove" || verb == "clear") {
        if (verb == "clear") {
            words.clear();
            for (const auto &park : parks_spawned_) words.push_back(park.name);
            if (words.empty()) return "No parks are spawned.";
        }
        if (words.empty()) return "park-mod remove <name> [<name>...]";
        for (std::size_t i = 1; i < words.size(); ++i)
            if (std::find(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(i), words[i]) != words.begin() + static_cast<std::ptrdiff_t>(i))
                return done({"[parks] " + words[i] + " could not be removed: it is named twice."});
        for (const auto &name : words)
            if (spawned(name) == parks_spawned_.end()) {
                const auto why = configured(name) ? name + " is not spawned." : "there is no park mod called \"" + name + "\" (park-mod list).";
                if (by_vote_) send_chat("The " + name + " park could not be removed: " + why);
                return done({"[parks] " + name + " could not be removed: " + why});
            }
        std::vector<std::string> lines;
        for (const auto &name : words) {
            parks_spawned_.erase(spawned(name));
            lines.push_back("[parks] " + name + " removed (by " + who + ")");
        }
        publish_parks();
        send_chat(words.size() > 1 ? "The " + listed(words) + " parks were removed." : "The " + words.front() + " park was removed.");
        return done(lines);
    }
    return "park-mod list | add <name> [<name>...] [minutes] | remove <name> [<name>...] | clear";
}
} // namespace dingosdk::server
