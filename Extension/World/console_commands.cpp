#include "Engine/Game/World/world_names.h"
#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Multiplayer/Session/session.h"
#include <algorithm>
#include <array>
#include <optional>
namespace dingosdk::console {
namespace {
constexpr std::string_view base_level_asset = "Levels/Game/DingoLevel_Root/DingoLevel_Root";

const overlay::Level *base_level(const Model &model) {
    for (const auto &level : model.levels)
        if (equal(level.asset, base_level_asset))
            return &level;
    return nullptr;
}
// On a dedicated server, its admins choose from the maps that server has
// (its retail maps and the custom maps in its Mods folder), not this PC's.
using ServerMaps = std::optional<std::vector<std::string>>;
ServerMaps admin_server_maps() {
    auto session = multiplayer::model();
    if (!session.server_admin || session.server_maps.empty())
        return std::nullopt;
    return std::move(session.server_maps);
}
// A destination `levels`, `load` and Tab offer: never the base root, and on a
// dedicated server only its maps.
bool offered(const overlay::Level &level, const ServerMaps &maps) {
    if (equal(level.asset, base_level_asset))
        return false;
    return !maps || std::any_of(maps->begin(), maps->end(), [&](const auto &asset) { return equal(asset, level.asset); });
}
std::string destination_name(const overlay::Level &level) {
    if (!level.display_name.empty())
        return level.display_name;
    return world_level_name(level.asset);
}
const overlay::Level *find_destination(const Model &model, std::string_view name, const ServerMaps &maps) {
    if (name.empty())
        return nullptr;
    std::size_t number{};
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), number);
    if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size()) {
        std::size_t index = 0;
        for (const auto &level : model.levels)
            if (offered(level, maps) && ++index == number)
                return &level;
        return nullptr;
    }
    for (const auto &level : model.levels)
        if (offered(level, maps) && equal(level.asset, name))
            return &level;
    for (const bool exact : {true, false}) {
        const overlay::Level *result = nullptr;
        for (const auto &level : model.levels) {
            if (!offered(level, maps))
                continue;
            const auto label = destination_name(level);
            const auto alias = world_level_short_name(level.asset);
            if (exact ? (equal(label, name) || equal(alias, name))
                      : (ascii_case_insensitive_starts_with(label, name) ||
                         ascii_case_insensitive_starts_with(alias, name) ||
                         ascii_case_insensitive_starts_with(level.asset, name))) {
                if (result)
                    return nullptr;
                result = &level;
            }
        }
        if (result)
            return result;
    }
    return nullptr;
}
} // namespace
void register_world_commands(Commands &registry) {
    auto levels = action("levels", "List destinations and their one-based indices", Group::world);
    levels.execution = Execution::local;
    levels.run = [](const Model &m, const Values &, const Output &out) {
        const auto maps = admin_server_maps();
        if (maps)
            out("The server's maps:");
        std::size_t index = 0;
        for (const auto &level : m.levels)
            if (offered(level, maps))
                out(std::to_string(++index) + ": " + destination_name(level) + " - " + level.asset);
        if (!index)
            out(maps ? "You have none of the server's maps installed." : "No destinations have been discovered yet.");
    };
    registry.add(std::move(levels));
    auto destination = argument("destination|index");
    destination.complete = [](const Model &m, auto) {
        const auto maps = admin_server_maps();
        std::vector<std::string> result;
        for (const auto &level : m.levels) {
            if (!offered(level, maps))
                continue;
            const auto name = destination_name(level);
            result.push_back(find_destination(m, name, maps) == &level ? name : level.asset);
        }
        return result;
    };
    auto start = argument("start|-", Type::text, true);
    start.complete = [](const Model &m, auto args) {
        std::vector<std::string> result{"-"};
        if (!args.empty())
            if (const auto *level = find_destination(m, args[0], admin_server_maps()))
                result.insert(result.end(), level->start_points.begin(), level->start_points.end());
        return result;
    };
    auto load = action("load", "Load a destination with the base root supplied automatically", Group::world,
                       {destination, start});
    load.inspect = [](const Model &m) {
        const auto *root = base_level(m);
        const bool ready = root && root->native_registered && root->can_load;
        return State{
            ready && m.can_queue_load, {}, ready ? m.load_block_reason : "The base level is not ready.", {}, false};
    };
    load.run = [](const Model &m, const Values &args, const Output &out) {
        const auto *root = base_level(m);
        const auto maps = admin_server_maps();
        const auto *level = find_destination(m, std::get<std::string>(args[0]), maps);
        if (!root || !root->native_registered || !root->can_load) {
            out("error: The base level is not ready.");
            return;
        }
        if (!level) {
            out(maps ? "error: The server doesn't have that map (or you don't); use levels or Tab."
                     : "error: Destination is missing or ambiguous; use levels or Tab.");
            return;
        }
        // A dedicated server's admin moves the whole server; everyone loads it together.
        if (multiplayer::model().server_admin) {
            const bool sent = multiplayer::queue_command("server", "map " + root->asset + "|" + level->asset, "");
            out(sent ? "Asked the server to change map." : "error: The request queue is busy; try again.");
            return;
        }
        if (!level->can_load) {
            out("error: " +
                (level->load_block_reason.empty() ? "This destination is unavailable." : level->load_block_reason));
            return;
        }
        std::string spawn = args.size() > 1 ? std::get<std::string>(args[1]) : "";
        if (spawn == "-")
            spawn.clear();
        if (spawn.empty())
            if (const auto *automatic = level->automatic_start_point())
                spawn = *automatic;
        if ((!spawn.empty() &&
             std::find(level->start_points.begin(), level->start_points.end(), spawn) == level->start_points.end()) ||
            (spawn.empty() && !level->native_registered && !level->start_points.empty())) {
            out("error: Choose a start point from this destination; use Tab.");
            return;
        }
        request_level(root->asset, "", level->asset, spawn);
    };
    registry.add(std::move(load));
    for (std::size_t i = 0; i < world_layers().size(); ++i) {
        const auto &layer = world_layers()[i];
        auto mode = argument("default|on|off");
        mode.choices = {"default", "on", "off"};
        auto entry = variable("world " + std::string(layer.key), std::string(layer.detail), Group::world, mode);
        entry.inspect = [i](const Model &m) {
            return State{
                m.world.available,
                m.world.enabled[i] ? std::optional<std::string>(*m.world.enabled[i] ? "on" : "off") : std::nullopt,
                m.world.status[i].empty() ? "This layer is unavailable in the current level." : m.world.status[i],
                "Saved selection: " + m.world.choices[i] +
                    (m.world.ready && m.world.supported[i] ? "" : "; live state unavailable in this level"),
                m.world.choices[i] != "default"};
        };
        entry.run = [i](const Model &, const Values &args, const Output &out) {
            // While a dedicated server syncs layers, its admins change them for everyone.
            if (multiplayer::model().server_admin && local_profile_world_layers().controlled_by_host) {
                const bool sent = multiplayer::queue_command(
                    "server", "layer " + world_layers()[i].key + " " + std::get<std::string>(args[0]), "");
                out(sent ? "Sent to the server." : "error: The request queue is busy; try again.");
                return;
            }
            const bool saved = set_local_world_layer(world_layers()[i].key, std::get<std::string>(args[0]));
            out((saved ? "" : "error: ") + local_profile_world_layers().feedback);
        };
        entry.reset = [i](const Model &, const Output &out) {
            const bool saved = set_local_world_layer(world_layers()[i].key, "default");
            out((saved ? "" : "error: ") + local_profile_world_layers().feedback);
        };
        registry.add(std::move(entry));
    }
    // Time of day: each map has seven time layers, "<map>_tod_<n>_<name>"; one is forced on
    // and the rest off, or all seven go back to the level's own choice.
    static constexpr std::array<std::string_view, 8> times{"default", "morning",   "noon",       "afternoon",
                                                           "evening", "night",     "weatherday", "weathernight"};
    auto time = argument("default|morning|noon|afternoon|evening|night|weatherday|weathernight|next");
    time.choices.assign(times.begin(), times.end());
    time.choices.emplace_back("next"); // the one after the current: for a button that steps through them
    auto tod = variable("tod", "Time of day on the current map", Group::world, time);
    // The current map's time layers in slot order, or empty when it has none.
    const auto time_layers = [](const Model &m) {
        std::vector<std::size_t> layers;
        for (unsigned slot = 1; slot < times.size(); ++slot) {
            const auto prefix = std::string(world_map_key(m.world.map)) + "_tod_" + std::to_string(slot) + "_";
            const auto found = std::find_if(world_layers().begin(), world_layers().end(),
                                            [&](const auto &layer) { return layer.key.starts_with(prefix); });
            if (m.world.map == WorldMap::none || found == world_layers().end()) return std::vector<std::size_t>{};
            layers.push_back(static_cast<std::size_t>(found - world_layers().begin()));
        }
        return layers;
    };
    tod.inspect = [time_layers](const Model &m) {
        const auto layers = time_layers(m);
        std::size_t forced{};
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const auto &choice = m.world.choices[layers[i]];
            if (choice == "on") forced = forced ? times.size() : i + 1;
            else if (choice != "off") forced = times.size();
        }
        const auto value = layers.empty() || forced >= times.size() ? std::string("default") : std::string(times[forced]);
        return State{m.world.available && !layers.empty(), value,
                     m.world.map == WorldMap::none ? "Load a map first." : "This map has no time-of-day layers.",
                     "Saved for this map", value != "default"};
    };
    tod.run = [time_layers](const Model &m, const Values &args, const Output &out) {
        auto wanted = lower(std::get<std::string>(args[0]));
        if (wanted == "next") {
            // As inspect reads it: the one layer forced on, or the level's own.
            const auto now_layers = time_layers(m);
            std::size_t forced{};
            for (std::size_t i = 0; i < now_layers.size(); ++i) {
                const auto &choice = m.world.choices[now_layers[i]];
                if (choice == "on") forced = forced ? times.size() : i + 1;
                else if (choice != "off") forced = times.size();
            }
            wanted = std::string(times[forced >= times.size() ? 1 : (forced + 1) % times.size()]);
        }
        const auto found = std::find(times.begin(), times.end(), wanted);
        if (found == times.end()) { out("error: Choose default, morning, noon, afternoon, evening, night, weatherday or weathernight."); return; }
        const auto slot = static_cast<std::size_t>(found - times.begin());
        // While a dedicated server syncs layers, its admins set the time for everyone.
        if (multiplayer::model().server_admin && local_profile_world_layers().controlled_by_host) {
            const bool sent = multiplayer::queue_command("server", "tod " + wanted, "");
            out(sent ? "Sent to the server." : "error: The request queue is busy; try again.");
            return;
        }
        const auto layers = time_layers(m);
        if (layers.empty()) { out(m.world.map == WorldMap::none ? "error: Load a map first." : "error: This map has no time-of-day layers."); return; }
        // The others switch off before the new one comes on, so two never load together.
        bool saved = true;
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (i + 1 != slot) saved = saved && set_local_world_layer(world_layers()[layers[i]].key, slot ? "off" : "default");
        if (slot) saved = saved && set_local_world_layer(world_layers()[layers[slot - 1]].key, "on");
        out(saved ? "Time of day: " + wanted + "." : "error: " + local_profile_world_layers().feedback);
    };
    registry.add(std::move(tod));
    auto map = argument("map");
    for (const auto anchor : world_map_anchors())
        map.choices.emplace_back(world_map_key(world_layer_nodes()[anchor].map));
    auto defaults = action("world defaults", "Restore authored world layers for a map", Group::world, {map});
    defaults.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = restore_local_world_layers(std::get<std::string>(args[0]));
        out((saved ? "" : "error: ") + local_profile_world_layers().feedback);
    };
    registry.add(std::move(defaults));
    auto random_parks = action("park random", "Load a random layout in every park slot", Group::world);
    random_parks.inspect = [](const Model &m) {
        return State{m.parks.available && (!m.parks.controlled_by_host || m.multiplayer.server_admin), {},
                     "The host controls park layouts, or park controls are unavailable.", {}, false};
    };
    random_parks.run = [](const Model &, const Values &, const Output &out) {
        if (multiplayer::model().server_admin) {
            const bool sent = multiplayer::queue_command("server", "park random", "");
            out(sent ? "Sent to the server." : "error: The request queue is busy; try again.");
            return;
        }
        const bool saved = load_random_local_parks();
        out((saved ? "" : "error: ") + local_profile_parks().feedback);
    };
    registry.add(std::move(random_parks));
    auto random_on_launch = variable("park random-on-launch", "Randomize local parks on the next game launch",
                                     Group::world, argument("0|1", Type::boolean));
    random_on_launch.inspect = [](const Model &m) {
        return boolean_state(m.parks.available, m.parks.randomize_on_launch, "Park controls are unavailable.",
                             "Local preference; the host still controls multiplayer layouts");
    };
    random_on_launch.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_park_randomize_on_launch(std::get<bool>(args[0]));
        out((saved ? "" : "error: ") + local_profile_parks().feedback);
    };
    registry.add(std::move(random_on_launch));
    for (unsigned lot = 0; lot < park_lots.size(); ++lot) {
        auto layout = argument("layout|empty");
        layout.choices = {"empty"};
        for (unsigned family = 0; family < park_families.size(); ++family)
            for (unsigned i = 1; i <= park_lots[lot].counts[family]; ++i)
                layout.choices.push_back(park_id(family, i));
        auto entry = variable("park " + std::string(park_lots[lot].key),
                              "Choose the layout for " + std::string(park_lots[lot].label), Group::world, layout);
        entry.inspect = [lot](const Model &m) {
            return State{m.parks.available, m.parks.choices[lot].empty() ? "default" : m.parks.choices[lot],
                         "Park controls are unavailable.", "Saved layout", false};
        };
        entry.run = [lot](const Model &, const Values &args, const Output &out) {
            // A lobby's parks follow its host; a dedicated server's admins choose them for everyone.
            if (multiplayer::model().server_admin) {
                const bool sent = multiplayer::queue_command(
                    "server", "park " + std::string(park_lots[lot].key) + " " + std::get<std::string>(args[0]), "");
                out(sent ? "Sent to the server." : "error: The request queue is busy; try again.");
                return;
            }
            const bool saved = set_local_park(park_lots[lot].key, std::get<std::string>(args[0]));
            out((saved ? "" : "error: ") + local_profile_parks().feedback);
        };
        registry.add(std::move(entry));
    }
    struct Control {
        const char *key;
        const char *description;
        double maximum;
        double (*read)(const WorldControls &);
        bool population;
    };
    const Control controls[]{
        {"traffic", "Traffic density: -1 default, 0 off, 1 light, 2 normal, 3 busy", 3,
         [](const WorldControls &c) { return double(c.traffic); }, true},
        {"pedestrians", "Pedestrian density: -1 default, 0 off, 1 light, 2 normal, 3 busy", 3,
         [](const WorldControls &c) { return double(c.pedestrians); }, true},
        {"fog", "Fog: -1 default, 0 off, 1 on", 1, [](const WorldControls &c) { return double(c.fog); }, false},
        {"fog_distance", "Fog visibility multiplier; 0.1 to 5, or -1 for default", 5,
         [](const WorldControls &c) { return double(c.fog_distance); }, false},
        {"sky_brightness", "Sky brightness multiplier; 0 to 3, or -1 for default", 3,
         [](const WorldControls &c) { return double(c.sky_brightness); }, false},
        {"clouds", "Cloud opacity multiplier; 0 to 2, or -1 for default", 2,
         [](const WorldControls &c) { return double(c.clouds); }, false},
        {"wind_strength", "Wind strength; 0 to 30, or -1 for default", 30,
         [](const WorldControls &c) { return double(c.wind_strength); }, false},
        {"wind_direction", "Wind direction in degrees; 0 to 360, or -1 for default", 360,
         [](const WorldControls &c) { return double(c.wind_direction); }, false}};
    for (const auto &control : controls) {
        auto value = argument("value|-1", Type::number);
        value.minimum = -1;
        value.maximum = control.maximum;
        if (control.population)
            value.choices = {"-1", "0", "1", "2", "3"};
        if (equal(control.key, "fog"))
            value.choices = {"-1", "0", "1"};
        auto entry = variable(control.key, control.description, Group::world, value);
        entry.aliases = {"environment " + std::string(control.key)};
        entry.inspect = [control](const Model &m) {
            const auto v = control.read(m.world_controls.choices);
            return State{
                control.population ? m.world_controls.population_available : m.world_controls.environment_available,
                v == -1 ? "default" : value_text(v), "World controls are unavailable.", "Saved override", v != -1};
        };
        entry.run = [control](const Model &, const Values &args, const Output &out) {
            const bool saved = set_local_world_control(control.key, static_cast<float>(std::get<double>(args[0])));
            out(saved ? local_profile_world_controls().status
                      : "error: Invalid world value, unavailable control, or save failed.");
        };
        entry.reset = [control](const Model &, const Output &out) {
            const bool saved = set_local_world_control(control.key, -1);
            out(saved ? local_profile_world_controls().status : "error: World control could not be restored.");
        };
        registry.add(std::move(entry));
    }
    for (unsigned i=0;i<dingosdk::atmosphere_controls.size();++i) {
        const auto& c=dingosdk::atmosphere_controls[i];
        auto input=argument(c.type==AtmosphereType::texture ? "asset|default" : c.lanes>1 ? "x,y,z,w|default" : "value|default");
        input.complete=[i](const Model& m, auto) {
            std::vector<std::string> result{"default"};
            const auto& c=dingosdk::atmosphere_controls[i];
            if (c.type==AtmosphereType::texture) result.insert(result.end(),m.world_controls.textures[i].begin(),m.world_controls.textures[i].end());
            if (c.type==AtmosphereType::toggle) { result.push_back("0");result.push_back("1"); }
            for (unsigned j=0;j<c.option_count;++j) result.push_back(std::to_string(atmosphere_options[c.option_start+j].value));
            return result;
        };
        auto entry=variable(c.key,std::string(c.section)+" / "+c.label+"; default restores the authored value",Group::world,input);
        entry.inspect=[i](const Model& m) {
            const auto& c=dingosdk::atmosphere_controls[i];const auto& w=m.world_controls;
            const auto& reading=w.atmosphere[i];
            std::optional<std::string> current;
            if (reading.value) current=atmosphere_value_text(c,*reading.value);
            const auto saved=w.choices.atmosphere.find(c.key);
            std::string detail=reading.varies ? "Values vary across the level's environments. " : "";
            if (saved!=w.choices.atmosphere.end()) detail+="Custom: "+atmosphere_value_text(c,saved->second)+(reading.applied ? "" : " (pending)");
            else detail+="Authored values / active convenience commands";
            return State{w.environment_available,current,"Environment bindings are unavailable.",detail,saved!=w.choices.atmosphere.end()};
        };
        entry.run=[i](const Model&,const Values& values,const Output& out) {
            const bool ok=set_local_atmosphere_control(dingosdk::atmosphere_controls[i].key,std::get<std::string>(values[0]));
            out(ok ? local_profile_world_controls().status : "error: Invalid value, texture not loaded, or save failed.");
        };
        entry.reset=[i](const Model&,const Output& out) {
            out(set_local_atmosphere_control(dingosdk::atmosphere_controls[i].key,"default") ? local_profile_world_controls().status : "error: Could not restore this control.");
        };
        registry.add(std::move(entry));
    }
    for (unsigned kind=0;kind<3;++kind) {
        auto entry=action(std::string(atmosphere_groups[kind])+".reset","Restore this group's authored values",Group::world);
        entry.run=[kind](const Model&,const Values&,const Output& out) {
            out(reset_local_atmosphere_controls(kind) ? local_profile_world_controls().status : "error: Could not restore this group.");
        };
        registry.add(std::move(entry));
    }
    auto reset_value = argument("-1", Type::number);
    reset_value.choices = {"-1"};
    reset_value.optional = true;
    auto environment_reset = action("environment reset_environment", "Restore all authored atmosphere settings",
                                    Group::world, {reset_value});
    environment_reset.run = [](const Model &, const Values &, const Output &out) {
        out(set_local_world_control("reset_environment", -1) ? local_profile_world_controls().status
                                                             : "error: Could not restore environment.");
    };
    registry.add(std::move(environment_reset));
}
} // namespace dingosdk::console
