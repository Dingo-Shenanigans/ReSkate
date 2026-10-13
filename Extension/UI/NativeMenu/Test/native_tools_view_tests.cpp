#include "Extension/UI/NativeMenu/native_tools_view.h"
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace dingosdk;
using namespace dingosdk::native_tools;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Capture {
    unsigned calls{};
    std::string command, root, destination, spawn;
    overlay::DebugRequest debug;
};
int main() {
    try {
        {
            // A BAM root with its seven time-of-day layers, as the level scan finds them.
            WorldLayerCatalog catalog{{{"Levels/Game/BAM_LevelRoot/Root", WorldMap::bam, -1, true}}, {0}, {}};
            for (const auto* time : time_of_day_keys) {
                const auto slot = static_cast<unsigned>(catalog.nodes.size());
                catalog.nodes.push_back({std::string("Levels/Game/BAM_LevelRoot/TOD_") + time, WorldMap::bam, 0, false});
                catalog.layers.push_back({std::string("bam_tod_") + time, time, "", WorldMap::bam, slot, slot, "Lighting"});
            }
            install_world_layer_catalog(std::move(catalog));
        }
        State state;
        overlay::Model model;
        Capture capture;
        overlay::CallbacksV3 cb;
        cb.user = &capture;
        cb.queue_load = [](void* user, const char* root, const char*, const char* destination, const char* spawn, char*, std::size_t) {
            auto& c = *static_cast<Capture*>(user); ++c.calls; c.root = root; c.destination = destination; c.spawn = spawn; return true;
        };
        cb.queue_debug = [](void* user, const overlay::DebugRequest& request, char*, std::size_t) {
            auto& c = *static_cast<Capture*>(user); ++c.calls; c.debug = request; return true;
        };
        cb.queue_console_command = [](void* user, const char* command, char*, std::size_t) {
            auto& c = *static_cast<Capture*>(user); ++c.calls; c.command = command; return true;
        };
        model.levels = {{std::string(root_asset), "Root"}, {"Studio/Example", "Studio", {"EntryA"}, {}, false}};
        model.can_queue_load = true;
        activate(state, model, cb, "select-level", "Studio/Example");
        check(capture.calls == 0 && state.destination == "Studio/Example", "Selecting a detached level must not load it");
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 1 && capture.root == root_asset && capture.destination == "Studio/Example" && capture.spawn == "EntryA", "Detached level uses its sole manifest spawn automatically");
        model.levels.back().native_registered = true;
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2 && capture.spawn.empty(), "Registered level uses the engine's default spawn without retaining a previous selection");
        check(state.feedback.empty(), "Accepted changes do not display queue diagnostics");
        model.multiplayer.active = true;
        check(!can_load(state, model, cb), "Guest cannot load a level even with an otherwise ready loader");
        const auto guest_page = render(state, model, cb, 0);
        check(std::none_of(guest_page.main.begin(), guest_page.main.end(), [](const auto& row) {
            return row.command == "select-level";
        }), "Guest level choices are visibly locked");
        check(std::none_of(guest_page.side.begin(), guest_page.side.end(), [](const auto& row) {
            return row.command == "load-level";
        }), "Guest load button has no activation delegate");
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2 && state.feedback == "Only the lobby host can change levels.",
            "A stale level action cannot load after joining a host");
        model.multiplayer.hosting = true;
        check(can_load(state, model, cb), "Host retains level controls");
        model.multiplayer = {};
        check(can_load(state, model, cb), "Level controls return when the host ends the lobby");
        model.multiplayer.lobby_joining = true;
        check(!can_load(state, model, cb), "Joining cannot race a manual level change");
        model.multiplayer = {};
        model.can_queue_load = false;
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2, "Stale load button cannot queue during transition");
        model.can_queue_load = true;
        model.levels.pop_back();
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2 && state.destination.empty(), "Removed level selection must not load");
        model.world.map = WorldMap::bam; model.parks.available = true;
        activate(state, model, cb, "park-lot", "financial");
        activate(state, model, cb, "park-family", "");
        activate(state, model, cb, "load-park", "");
        check(capture.command == "park financial flumppark_01", "Selected park targets its own lot");
        const auto calls = capture.calls;
        model.parks.controlled_by_host = true;
        activate(state, model, cb, "load-park", "");
        check(capture.calls == calls, "Host ownership is rechecked on activation");
        activate(state, model, cb, "noclip", "");
        check(capture.calls == calls, "Unavailable debug control cannot queue");
        model.debug.noclip = true;
        activate(state, model, cb, "noclip", "");
        check(capture.debug.action == overlay::DebugAction::set_noclip && !capture.debug.enabled, "Active noclip can always be disabled");
        model.graphics.available = true; model.graphics.ready[0] = true; model.graphics.enabled[0] = false;
        activate(state, model, cb, "graphics", "film_grain");
        check(capture.command == "graphics film_grain 1", "Toggle reads effective authored graphic state");
        model.progression.challenges_enabled = true; model.progression.challenges_hidden = true;
        activate(state, model, cb, "challenges", "");
        check(capture.command == "challenges 1", "Hidden challenges use show command");
        model.world.ready = true; model.world_controls.population_available = true;
        model.world_controls.choices.traffic = 3;
        activate(state, model, cb, "traffic", "");
        check(capture.command == "environment traffic -1", "Population cycles back to authored default");
        const auto locked_parks = render(state, model, cb, parks_section);
        const auto load = std::find_if(locked_parks.side.begin(), locked_parks.side.end(), [](const auto& row) { return row.id == "load-park"; });
        check(load != locked_parks.side.end() && load->command.empty(), "Host-owned park action is visibly unavailable");
        cb.queue_debug = [](void*, const overlay::DebugRequest&, char* result, std::size_t size) {
            std::snprintf(result, size, "Queued for the game update thread."); return true;
        };
        activate(state, model, cb, "noclip", "");
        check(state.feedback.empty(), "Successful callback diagnostics stay out of the menu");
        model.debug.status = "Native callback diagnostic";
        const auto player_page = render(state, model, cb, player_section);
        check(std::none_of(player_page.side.begin(), player_page.side.end(), [](const auto& row) {
            return row.title.find("Native callback") != std::string::npos;
        }), "Debug model status stays out of the player menu");
        const auto find = [](const std::vector<Row>& rows, std::string_view id) {
            return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        };
        // Hall of Meat sits with the player's other switches.
        const auto meat_row = [&] {
            const auto page = render(state, model, cb, player_section);
            return *find(page.main, "hall-of-meat");
        };
        check(meat_row().title == "Hall of Meat: Off" && meat_row().command.empty(), "Hall of Meat waits until it started");
        model.hall_of_meat = {true, false};
        activate(state, model, cb, "hall-of-meat", "");
        check(meat_row().command == "hall-of-meat" && capture.command == "hallofmeat 1", "Hall of Meat turns on");
        model.hall_of_meat = {true, true};
        activate(state, model, cb, "hall-of-meat", "");
        check(meat_row().title == "Hall of Meat: On" && capture.command == "hallofmeat 0", "and off again");
        const auto player_row = [&](std::string_view id) -> std::optional<Row> {
            const auto page = render(state, model, cb, player_section);
            const auto found = find(page.main, id);
            return found == page.main.end() ? std::nullopt : std::optional<Row>(*found);
        };
        model.style.enabled = false;
        check(player_row("style") && player_row("style")->title == "Style: Off", "Style has a switch, and it starts off");
        activate(state, model, cb, "style", "");
        check(player_row("style")->command == "style" && capture.command == "style 1", "Style turns on");
        model.style.enabled = true;
        model.style.available = false;
        check(player_row("style")->command.empty(), "Style is greyed out while it is unavailable");
        model.style.available = true;
        activate(state, model, cb, "style", "");
        check(player_row("style")->title == "Style: On" && capture.command == "style 0", "and off again");
        {
            const auto queue = std::exchange(cb.queue_console_command, nullptr);
            check(player_row("style")->command.empty(), "Style is greyed out without the console");
            cb.queue_console_command = queue;
        }
        model.style.enabled = false;
        // Its editor row shows only while style is on.
        check(!player_row("style-editor"), "Style starts without its editor");
        model.style.enabled = true;
        check(player_row("style-editor").has_value(), "The style editor shows while style is on");
        model.multiplayer.active = model.debug.available = true;
        check(player_row("style-editor") && player_row("style-editor")->command == "style-editor",
              "The style editor opens on a multiplayer server");
        model.style.server_too_full = true;
        check(player_row("style-editor") && player_row("style-editor")->title == "Style editor" &&
                  player_row("style-editor")->command.empty(),
              "but is greyed out on a server too full for its stand-in");
        model.style.server_too_full = model.multiplayer.active = model.debug.available = false;
        model.style.available = false;
        check(player_row("style-editor") && player_row("style-editor")->command.empty(), "and while style is unavailable");
        model.style.available = true;
        {
            model.debug.available = false;
            capture.command.clear();
            activate(state, model, cb, "style-editor", "");
            check(capture.command.empty(), "The style editor row does not open the editor without the debug camera");
            model.debug.available = true;
            activate(state, model, cb, "style-editor", "");
            check(capture.command == "style editor open", "The style editor row opens the editor");
            model.debug.available = false;
        }
        {
            // The STYLE tab, the HUB and this view open the editor on one rule.
            const auto openable = [&](auto change) {
                model.style.enabled = model.style.available = model.debug.available = true;
                model.style.server_too_full = false;
                change();
                return overlay::style_editor_openable(model);
            };
            check(openable([] {}) && openable([&] { model.multiplayer.active = true; }),
                  "The style editor opens with style on and available, with the debug tools, also on a multiplayer server");
            check(!openable([&] { model.debug.available = false; }), "but not without the debug tools");
            check(!openable([&] { model.style.server_too_full = true; }) && !openable([&] { model.style.available = false; }) &&
                      !openable([&] { model.style.enabled = false; }),
                  "nor on a server too full for its stand-in, while style is unavailable, or while it is off");
            check(!openable([&] { model.style.recording_tricks = true; }), "nor while the tricks are being recorded");
            model.style.recording_tricks = false;
            const auto reason = [&](auto change) {
                model.style.enabled = model.style.available = model.debug.available = true;
                model.style.server_too_full = false;
                model.style.unavailable_reason.clear();
                change();
                return overlay::style_editor_blocked_reason(model);
            };
            check(reason([] {}).empty(), "An editor that opens has no reason");
            check(reason([&] { model.style.enabled = false; }) == "Turn on Style to use the editor.", "Style off says so");
            check(reason([&] { model.style.available = false; model.style.unavailable_reason = "no bail is not ready"; }) ==
                      "No bail is not ready.",
                  "Style unavailable gives the layer's reason");
            check(reason([&] { model.style.available = false; }) == "Style is not available for this game build.",
                  "and a plain reason without one");
            check(reason([&] { model.style.server_too_full = true; }) ==
                      "The style editor is not available on a server this full.",
                  "A full server says so");
            check(reason([&] { model.style.recording_tricks = true; }) == "Your tricks are being recorded.",
                  "A running recording says so");
            model.style.recording_tricks = false;
            check(reason([&] { model.debug.available = false; }) == "The editor opens once your skater is in the game.",
                  "No debug tools: the skater is not in the game yet");
            model.style.enabled = model.style.available = true;
            model.multiplayer.active = model.debug.available = false;
        }
        {
            model.style.enabled = false;
            check(!player_row("style-preset"), "Style starts without its preset row");
            model.style.enabled = true;
            model.style.preset = "default";
            model.style.presets = {"default"};
            check(player_row("style-preset") && player_row("style-preset")->title == "Style preset: default" &&
                      player_row("style-preset")->command.empty(),
                  "The preset row names the preset in use, and is greyed out with no other preset");
            model.style.presets = {"alpha", "default", "street"};
            capture.command.clear();
            activate(state, model, cb, "style-preset", "");
            check(player_row("style-preset") && player_row("style-preset")->command == "style-preset" &&
                      capture.command == "style preset load street",
                  "The preset row loads the next preset");
            model.style.preset = "street";
            activate(state, model, cb, "style-preset", "");
            check(capture.command == "style preset load alpha", "and after the last, the first");
            model.style.available = false;
            check(player_row("style-preset") && player_row("style-preset")->command.empty(),
                  "The preset row is greyed out while style is unavailable");
            model.style.available = true;
            const auto queue = std::exchange(cb.queue_console_command, nullptr);
            check(player_row("style-preset") && player_row("style-preset")->command.empty(), "and without the console");
            cb.queue_console_command = queue;
            model.style.preset.clear(), model.style.presets.clear();
        }
        model.style.enabled = false;
        // Road Rash's switches sit there too; its blood and its healing wait for it to be on.
        const auto rash_row = [&](std::string_view id) {
            const auto page = render(state, model, cb, player_section);
            return *find(page.main, id);
        };
        check(rash_row("road-rash").title == "Road Rash: Off" && rash_row("road-rash").command.empty(), "Road Rash waits until it started");
        model.road_rash = {true, false, true};
        check(rash_row("road-rash-blood").command.empty() && rash_row("road-rash-heal").command.empty(), "Its blood and healing wait for it");
        activate(state, model, cb, "road-rash", "");
        check(rash_row("road-rash").command == "road-rash" && capture.command == "roadrash 1", "Road Rash turns on");
        model.road_rash = {true, true, true};
        check(rash_row("road-rash").title == "Road Rash: On" && rash_row("road-rash-blood").title == "Road Rash blood: On", "and shows it");
        activate(state, model, cb, "road-rash-blood", "");
        check(capture.command == "roadrashblood 0", "The blood switches off");
        activate(state, model, cb, "road-rash-heal", "");
        check(capture.command == "roadrash heal", "and the skater heals");
        // Board Break's switch sits there too, with the deck that is in the game; another deck waits for a deck pack.
        check(rash_row("board-break").title == "Board Break: Off" && rash_row("board-break").command.empty(), "Board Break waits until it started");
        model.board_break = {true, true, false, "None"};
        check(rash_row("board-break").title == "Board Break: On" && rash_row("board-break-deck").command.empty(),
              "Without a deck pack there is no deck to change");
        activate(state, model, cb, "board-break", "");
        check(capture.command == "boardbreak 0", "Board Break turns off");
        model.board_break = {true, true, true, "Wide"};
        check(rash_row("board-break-deck").title == "Board Break deck: Wide" && rash_row("board-break-deck").command == "board-break-deck",
              "The deck in the game shows");
        activate(state, model, cb, "board-break-deck", "");
        check(capture.command == "boardbreak deck next", "and steps to the next one");
        // Grind exit triggers: the switch, what LT / RT mean, and the stance toe/heel needs.
        const auto grind_row = [&](std::string_view id) {
            const auto page = render(state, model, cb, player_section);
            const auto row = find(page.main, id);
            return row == page.main.end() ? Row{} : *row;
        };
        check(grind_row("grind-exits").title == "Grind exit triggers: Off" && grind_row("grind-exits").command.empty(),
              "Grind exit triggers wait until they started");
        model.grind_exit = {true, false, true, 0};
        activate(state, model, cb, "grind-exits", "");
        check(grind_row("grind-exits").command == "grind-exits" && capture.command == "grindexit exits on", "Grind exit triggers turn on");
        check(grind_row("grind-exit-sides").title == "Input Mode: Toe / Heel" && grind_row("grind-exit-stance").title == "Your stance: Not set",
              "Toe / heel shows the stance it needs");
        activate(state, model, cb, "grind-exit-stance", "");
        check(capture.command == "grindexit stance goofy", "The stance is set");
        model.grind_exit = {true, true, true, 2};
        activate(state, model, cb, "grind-exit-stance", "");
        check(capture.command == "grindexit stance regular", "and changed");
        activate(state, model, cb, "grind-exit-sides", "");
        check(capture.command == "grindexit sides travel", "LT / RT switch to left / right of travel");
        model.grind_exit = {true, true, false, 2};
        check(grind_row("grind-exit-sides").title == "Input Mode: Left / Right" && grind_row("grind-exit-stance").id.empty(),
              "which needs no stance");
        activate(state, model, cb, "grind-exits", "");
        check(capture.command == "grindexit exits off", "and the triggers turn off");
        // Pop-out strength steps down from the full exit and wraps back to it.
        check(grind_row("grind-exit-strength").title == "Pop-out strength: 100%", "Pop-out strength shows the full exit");
        activate(state, model, cb, "grind-exit-strength", "");
        check(capture.command == "grindexit strength 75", "and steps down");
        model.grind_exit.strength = 40;
        activate(state, model, cb, "grind-exit-strength", "");
        check(capture.command == "grindexit strength 25", "a slider value steps to the next lower step");
        model.grind_exit.strength = 25;
        activate(state, model, cb, "grind-exit-strength", "");
        check(capture.command == "grindexit strength 100", "and the lowest wraps to the full exit");
        model.multiplayer.local_name = "steam_name";
        model.player_card = {true, "Card Name", {}};
        const auto card_page = render(state, model, cb, player_section);
        const auto field = find(card_page.side, "card-name");
        check(field != card_page.side.end() && field->input && field->title == "steam_name" && field->argument == "Card Name",
            "Card name field shows the Steam name as its hint and the saved name as its text");
        check(find(card_page.side, "card-name-reset")->command == "card-name-reset", "A custom card name can be reset");
        model.player_card.custom_name.clear();
        const auto steam_page = render(state, model, cb, player_section);
        check(find(steam_page.side, "card-name-reset")->command.empty(), "Steam name reset is idle without a custom name");
        {
            State parks;
            overlay::Model m;
            m.world.map = WorldMap::bam;
            m.parks.available = true;
            m.parks.choices.fill("flumppark_01");
            Capture log;
            auto controls = cb;
            controls.user = &log;
            const auto control = [&](const Page& page, std::string_view id) -> const Row& {
                const auto found = find(page.main, id);
                check(found != page.main.end() && found->button, "Random park controls are controller-focusable buttons");
                return *found;
            };
            auto page = render(parks, m, controls, parks_section);
            check(control(page, "load-random-parks").title == "Load Random Parks" &&
                control(page, "load-random-parks").command == "load-random-parks", "Controller exposes random loading for all lots");
            check(control(page, "park-random-on-launch").title == "Randomize on Launch: Off", "Launch toggle reads the shared preference");
            parks.parks.fill("megapark_01");
            activate(parks, m, controls, "load-random-parks", {});
            check(log.calls == 1 && log.command == "park random", "Random loading queues one shared command, not three separate rolls");
            check(parks.parks == ParkChoices{}, "Accepted random loading discards every staged layout");
            sync(parks, m);
            check(parks.parks == m.parks.choices, "A reroll of the same saved layouts still replaces staged selections");

            m.parks.controlled_by_host = true;
            page = render(parks, m, controls, parks_section);
            check(control(page, "load-random-parks").command.empty(), "Guests cannot activate random loading");
            activate(parks, m, controls, "load-random-parks", {});
            check(log.calls == 1, "A stale random button cannot load after joining a host");
            activate(parks, m, controls, "park-random-on-launch", {});
            check(log.calls == 2 && log.command == "park random-on-launch 1", "Guests can save their own next-launch preference");
            m.parks.randomize_on_launch = true;
            page = render(parks, m, controls, parks_section);
            check(control(page, "park-random-on-launch").title == "Randomize on Launch: On", "Controller reflects changes from the other menu");
            activate(parks, m, controls, "park-random-on-launch", {});
            check(log.calls == 3 && log.command == "park random-on-launch 0", "Launch toggle uses the latest shared state");

            // A role on a dedicated server with every command (the park one among them).
            m.multiplayer.dedicated = m.multiplayer.server_admin = m.multiplayer.server_all_commands = true;
            page = render(parks, m, controls, parks_section);
            check(!control(page, "load-random-parks").command.empty(), "Dedicated-server admins keep random park controls");
            activate(parks, m, controls, "load-random-parks", {});
            check(log.calls == 4 && log.command == "park random", "Server admin sends one shared command for server-side routing");
            m.world.map = WorldMap::grom;
            activate(parks, m, controls, "load-random-parks", {});
            check(log.calls == 4, "Random loading is unavailable on maps without these park lots");
            m.world.map = WorldMap::bam;
            m.parks.available = false;
            activate(parks, m, controls, "load-random-parks", {});
            activate(parks, m, controls, "park-random-on-launch", {});
            check(log.calls == 4, "Unavailable park controls cannot queue actions");
            m.parks.available = true;
            controls.queue_console_command = nullptr;
            page = render(parks, m, controls, parks_section);
            check(control(page, "load-random-parks").command.empty() && control(page, "park-random-on-launch").command.empty(),
                "Missing command callbacks disable both controls");
            controls.queue_console_command = [](void*, const char*, char*, std::size_t) { return false; };
            parks.parks.fill("megapark_01");
            activate(parks, m, controls, "load-random-parks", {});
            check(parks.parks[0] == "megapark_01" && !parks.feedback.empty(), "A rejected queue retains staged choices and reports failure");
        }
        {
            // Time of day forces one of a map's seven time layers, others off.
            overlay::Model world;
            world.world.available = world.world.ready = true;
            world.world.map = WorldMap::bam;
            world.world.supported.assign(world_layers().size(), true);
            State tod;
            Capture log;
            overlay::CallbacksV3 record;
            record.user = &log;
            record.queue_console_command = [](void* user, const char* command, char*, std::size_t) {
                auto& c = *static_cast<Capture*>(user); ++c.calls; c.command = command; return true;
            };
            check(time_of_day_available(world, record) && time_of_day(world) == 0, "Time of day starts at the level's default");
            activate(tod, world, record, "time-of-day", {});
            check(log.calls == 7 && log.command == "world bam_tod_1_morning on", "Cycling selects Morning last, after the others are off");
            for (unsigned slot = 0; slot < time_of_day_keys.size(); ++slot)
                world.world.choices[time_of_day_layer(WorldMap::bam, slot)] = slot == 4 ? "on" : "off";
            check(time_of_day(world) == 5, "A single forced layer reads back as that time");
            const auto page = render(tod, world, record, 0);
            check(std::any_of(page.side.begin(), page.side.end(), [](const auto& row) { return row.title == "Time of day: Night"; }),
                "The World page shows the current time of day");
            for (unsigned slot = 0; slot < 6; ++slot) activate(tod, world, record, "time-of-day", {});
            world.world.choices[time_of_day_layer(WorldMap::bam, 6)] = "on";
            check(time_of_day(world) == 0, "Two forced times read as no single choice");
            for (auto& choice : world.world.choices) choice = "default";
            world.world.choices[time_of_day_layer(WorldMap::bam, 6)] = "on";
            for (unsigned slot = 0; slot < 6; ++slot) world.world.choices[time_of_day_layer(WorldMap::bam, slot)] = "off";
            log.calls = 0;
            activate(tod, world, record, "time-of-day", {});
            check(log.calls == 7 && log.command.ends_with(" default"), "Cycling past the last time returns all seven to default");
            world.world.controlled_by_host = true;
            log.calls = 0;
            activate(tod, world, record, "time-of-day", {});
            check(log.calls == 0 && !time_of_day_available(world, record), "A host-forced lobby cannot change time of day");
        }
        {
            overlay::Model maps;
            maps.levels = {{std::string(root_asset), "Root"}, {"levels/Game/BAM_LevelRoot/BAM_LevelRoot"},
                {"levels/Game/DingoLevel_Splash/DingoLevel_Splash"}, {"Levels/Custom/Bedroom", "Bedroom"}};
            maps.levels.back().custom = true;
            State split;
            const auto shows = [](const Page& page, std::string_view asset) {
                return std::any_of(page.main.begin(), page.main.end(), [&](const auto& row) { return row.argument == asset; });
            };
            const auto world_page = render(split, maps, cb, world_section), custom_page = render(split, maps, cb, custom_section);
            check(shows(world_page, maps.levels[1].asset) && !shows(world_page, "Levels/Custom/Bedroom"), "World lists only official levels");
            check(shows(custom_page, "Levels/Custom/Bedroom") && !shows(custom_page, maps.levels[1].asset), "Custom Maps lists only mod levels");
            check(!shows(world_page, maps.levels[2].asset) && !shows(custom_page, maps.levels[2].asset), "Splash is never listed");
            activate(split, maps, cb, "select-level", maps.levels[2].asset);
            check(split.destination.empty(), "Splash cannot be selected");
        }
        std::puts("Native ReSkate control checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
