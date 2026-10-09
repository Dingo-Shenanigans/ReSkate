#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Remote/remote_collision.h"
#include "Extension/Multiplayer/Remote/native_vfx.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Throwdowns/throwdown_debug_text.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/UI/NativeMenu/native_menu_dump.h"
#include "Extension/Assets/live_mods.h"
#include <algorithm>
#include <array>

namespace dingosdk::console {
void register_multiplayer_commands(Commands &registry) {
    struct Command {
        const char *name;
        const char *description;
    };
    for (const auto &c : {Command{"host", "Host a Steam lobby for 2-32 players"},
                          Command{"join", "Join using the host's complete session code"},
                          Command{"browse", "Refresh the public Steam lobby browser"},
                          Command{"join-lobby", "Join a lobby selected in the server browser"},
                          Command{"echo", "Test a delayed remote skater locally"},
                          Command{"stop", "Disconnect and remove the remote skater"},
                          Command{"chat", "Send a text message to everyone in the session (T opens the chat box)"},
                          Command{"server", "Send a command to the dedicated server you are an admin of (try: help)"},
                          Command{"tp", "Teleport to a player (name start) or to x y z"},
                          Command{"party", "Your party in a session: invite|join|kick|promote <player>, "
                                           "accept|decline [player], leave, open, close, status"},
                          Command{"tpall", "Host or server admin: teleport everyone to you"},
                          Command{"tphere", "Host or server admin: teleport one player to you"},
                          Command{"nametags", "Show or hide player nametags (on, off, toggle)"},
                          Command{"pose-dump", "Research: record your own poses for a number of seconds (1-600) to logs/poses-*.bin"},
                          Command{"vote", "Answer the vote a dedicated server is running (yes, no), or its poll (1, 2...)"},
                          Command{"voice-chat", "Your own voice chat (on, off, toggle)"},
                          Command{"direct-connections", "Connect straight to servers that offer it, not through Steam's relays (on, off, toggle)"},
                          Command{"player-distance", "How far away another player still gets a skater, in metres (50-1000; 1000: every player). Past it: a nametag or dot"},
                          Command{"nametag-distance", "How far away a player's name still shows, in metres (10-500); past it they are a dot"},
                          Command{"nametag-dots", "Show far and off-screen players as dots (on, off, toggle)"},
                          Command{"nametags-friends", "Only your Steam friends have nametags (on, off, toggle)"},
                          Command{"chat-bubbles", "Show or hide chat bubbles above skaters (on, off, toggle)"},
                          Command{"chat-bubbles-own", "Also show your own chat messages above your skater (on, off, toggle)"},
                          Command{"chat-bubbles-distance", "How far away a player may be and still show a chat bubble, in metres (5-500)"},
                          Command{"chat-bubbles-duration", "How many seconds a chat bubble stays before it fades (1-30)"},
                          Command{"chat-bubbles-history", "How many recent messages stack above a skater (1-8)"},
                          Command{"score-check", "Host: keep players whose mods change scoring or physics out of throwdowns and "
                                                 "coop challenges (on, off, toggle; on by default)"},
                          Command{"retry", "Retry remote skater creation after an error"},
                          Command{"test", "Test a Steam socket-pair round trip"},
                          Command{"status", "Show multiplayer connection and animation status"}}) {
        std::vector<Argument> args;
        if (std::string_view(c.name) == "join")
            args.push_back(argument("join_code"));
        if (std::string_view(c.name) == "host") {
            args.push_back(argument("visibility", Type::text, true));
            args.push_back(argument("players", Type::text, true));
        }
        if (std::string_view(c.name) == "join-lobby")
            args.push_back(argument("lobby_id"));
        if (std::string_view(c.name) == "nametags" ||
            std::string_view(c.name) == "chat-bubbles" || std::string_view(c.name) == "chat-bubbles-own" ||
            std::string_view(c.name) == "nametag-dots" || std::string_view(c.name) == "nametags-friends" ||
            std::string_view(c.name) == "direct-connections" || std::string_view(c.name) == "vote" ||
            std::string_view(c.name) == "voice-chat" ||
            std::string_view(c.name) == "score-check")
            args.push_back(argument("choice"));
        if (std::string_view(c.name) == "chat-bubbles-distance" || std::string_view(c.name) == "nametag-distance")
            args.push_back(argument("metres"));
        if (std::string_view(c.name) == "chat-bubbles-duration" || std::string_view(c.name) == "pose-dump")
            args.push_back(argument("seconds"));
        if (std::string_view(c.name) == "player-distance")
            args.push_back(argument("metres"));
        if (std::string_view(c.name) == "chat-bubbles-history")
            args.push_back(argument("lines"));
        if (std::string_view(c.name) == "chat" || std::string_view(c.name) == "server" || std::string_view(c.name) == "party" ||
            std::string_view(c.name) == "tp" || std::string_view(c.name) == "tphere") {
            auto message = argument("message");
            message.rest = true;
            args.push_back(std::move(message));
        }
        auto entry = action(std::string("mp ") + c.name, c.description, Group::gameplay, std::move(args));
        entry.aliases = {std::string("multiplayer.") + c.name};
        entry.run = [name = std::string(c.name)](const Model &, const Values &values, const Output &out) {
            std::string arguments;
            for (const auto &value : values) {
                const auto &part = std::get<std::string>(value);
                if (part.empty())
                    continue;
                if (!arguments.empty())
                    arguments += ' ';
                arguments += part;
            }
            out(multiplayer::command(name, arguments));
        };
        registry.add(std::move(entry));
    }
    auto markers_choice = argument("on|off", Type::text, true);
    auto markers = action("mp map-markers",
                          "Named, selectable markers for your party members on the pause map, like the live game's "
                          "(default). Off, or players outside your party: the game's plain player dots",
                          Group::gameplay, {std::move(markers_choice)});
    markers.run = [](const Model &, const Values &values, const Output &out) {
        const auto &choice = std::get<std::string>(values[0]);
        if (choice == "on" || choice == "off") multiplayer::set_native_party_map_markers(choice == "on");
        else if (!choice.empty()) {
            out("error: Choose on or off.");
            return;
        }
        out(multiplayer::native_party_map_markers() ? "Map player markers: named and selectable."
                                                   : "Map player markers: the game's plain player dots.");
    };
    registry.add(std::move(markers));
    // Other players as solid capsules for your skater (remote_collision.cpp).
    auto collision_choice = argument("on|off", Type::text, true);
    collision_choice.complete = [](const Model &, auto) { return std::vector<std::string>{"on", "off"}; };
    auto collision = action("mp collision",
                            "Player collision: show whether other players are solid for your skater, or switch "
                            "ReSkate's collision capsules on or off. Both players need the game's Enable Party "
                            "Collision setting (shown while the playercollision feature is on)",
                            Group::gameplay, {std::move(collision_choice)});
    collision.run = [](const Model &, const Values &values, const Output &out) {
        const auto &choice = values.empty() ? std::string{} : std::get<std::string>(values[0]);
        if (!choice.empty()) {
            if (choice != "on" && choice != "off") {
                out("error: Choose on or off.");
                return;
            }
            multiplayer::set_remote_collision_enabled(choice == "on");
        }
        out(multiplayer::remote_collision_status());
    };
    registry.add(std::move(collision));
    // The native work far players' skaters skip (puppet_cost.cpp), each switchable.
    auto saving_name = argument("saving", Type::text, true);
    saving_name.complete = [](const Model &, auto) {
        return std::vector<std::string>{"driven-placement", "ecs-retry", "far-components"};
    };
    auto saving_choice = argument("on|off", Type::text, true);
    auto savings = action("mp puppet-savings",
                          "Far players' skaters: show the savings, or switch one (driven-placement, ecs-retry, "
                          "far-components) on or off",
                          Group::gameplay, {std::move(saving_name), std::move(saving_choice)});
    savings.run = [](const Model &, const Values &values, const Output &out) {
        using multiplayer::PuppetSaving;
        const auto &name = std::get<std::string>(values[0]);
        const auto &choice = values.size() > 1 ? std::get<std::string>(values[1]) : std::string{};
        if (!name.empty()) {
            static const std::array<std::pair<std::string_view, PuppetSaving>, 3> names{{
                {"driven-placement", PuppetSaving::driven_placement}, {"ecs-retry", PuppetSaving::ecs_retry},
                {"far-components", PuppetSaving::far_components}}};
            const auto found = std::find_if(names.begin(), names.end(), [&](const auto &n) { return n.first == name; });
            if (found == names.end()) {
                out("error: Unknown saving. Choose driven-placement, ecs-retry or far-components.");
                return;
            }
            if (choice != "on" && choice != "off") {
                out("error: Choose on or off.");
                return;
            }
            multiplayer::set_puppet_saving(found->second, choice == "on");
        }
        out(multiplayer::puppet_saving_status());
    };
    registry.add(std::move(savings));
    auto effects = action("mp effects",
                          "Skater effects shared with other players: what has been captured and played",
                          Group::gameplay, {});
    effects.run = [](const Model &, const Values &, const Output &out) { out(multiplayer::native_effects_status()); };
    registry.add(std::move(effects));
    auto hud_choice = argument("reskate|game", Type::text, true);
    hud_choice.complete = [](const Model &, auto) { return std::vector<std::string>{"reskate", "game"}; };
    auto hud = action("throwdown-hud",
                      "The S.K.A.T.E. throwdown HUD: ReSkate's (default) or the game's own debug text",
                      Group::gameplay, {std::move(hud_choice)});
    hud.run = [](const Model &, const Values &values, const Output &out) {
        const auto &choice = std::get<std::string>(values[0]);
        if (choice == "reskate" || choice == "game") multiplayer::set_skate_hud_reskate(choice == "reskate");
        else if (!choice.empty()) {
            out("error: Choose reskate or game.");
            return;
        }
        out(multiplayer::skate_hud_reskate() ? "S.K.A.T.E. HUD: ReSkate's." : "S.K.A.T.E. HUD: the game's debug text.");
    };
    registry.add(std::move(hud));
    auto offboard_choice = argument("on|off", Type::text, true);
    auto offboard = action("throwdown-offboard",
                           "S.K.A.T.E.: wait for your turn on foot, off the board (default on)",
                           Group::gameplay, {std::move(offboard_choice)});
    offboard.run = [](const Model &, const Values &values, const Output &out) {
        const auto &choice = std::get<std::string>(values[0]);
        if (choice == "on" || choice == "off") multiplayer::set_throwdown_offboard(choice == "on");
        else if (!choice.empty()) {
            out("error: Choose on or off.");
            return;
        }
        out(multiplayer::throwdown_offboard_enabled() ? "S.K.A.T.E.: waiting players stand off their boards."
                                                      : "S.K.A.T.E.: waiting players keep skating.");
    };
    registry.add(std::move(offboard));
    auto invite_choice = argument("on|off", Type::text, true);
    invite_choice.complete = [](const Model &, auto) { return std::vector<std::string>{"on", "off"}; };
    auto invite = action("challenge-invite",
                         "Coop challenges: starting a challenge invites party members near you, and you join theirs (default on)",
                         Group::gameplay, {std::move(invite_choice)});
    invite.run = [](const Model &, const Values &values, const Output &out) {
        const auto &choice = values.empty() ? std::string{} : std::get<std::string>(values[0]);
        if (choice == "on" || choice == "off") multiplayer::set_challenge_invites(choice == "on");
        else if (!choice.empty()) {
            out("error: Choose on or off.");
            return;
        }
        out(multiplayer::challenge_invites() ? "Coop challenges: on (nearby party members join each other's challenges)."
                                             : "Coop challenges: off (every challenge is your own).");
    };
    registry.add(std::move(invite));
    // Research aid for multiplayer throwdowns (analysis/throwdowns-native-mp.md).
    auto lab_args = argument("verb_and_arguments", Type::text, true);
    lab_args.rest = true;
    lab_args.complete = [](const Model &, auto) {
        return std::vector<std::string>{"status", "ai", "spawn", "join", "start", "destroy", "mmid", "trace", "endturn", "score", "mirror", "attempt", "relay"};
    };
    auto lab = action("throwdown", "Throwdown lab: add virtual participants, spawn/start/destroy queues (research aid)",
                      Group::console, {std::move(lab_args)});
    lab.run = [](const Model &model, const Values &values, const Output &out) {
        std::optional<std::array<float, 3>> skater;
        if (model.debug.skater_position_valid) skater = model.debug.skater_position;
        out(multiplayer::throwdown_lab_command(values.empty() ? std::string{} : std::get<std::string>(values[0]), skater));
    };
    registry.add(std::move(lab));
    // Research aid for multiplayer coop challenges (analysis/coop-challenges-mp.md).
    auto challenge_args = argument("verb_and_arguments", Type::text, true);
    challenge_args.rest = true;
    challenge_args.complete = [](const Model &, auto) {
        return std::vector<std::string>{"status", "virtual", "attempt", "clear"};
    };
    auto challenge = action("challenge", "Coop challenge lab: start a challenge with virtual participants, replay your attempt as one (research aid)",
                            Group::console, {std::move(challenge_args)});
    challenge.run = [](const Model &, const Values &values, const Output &out) {
        out(multiplayer::challenge_lab_command(values.empty() ? std::string{} : std::get<std::string>(values[0])));
    };
    registry.add(std::move(challenge));
    // Research aid: "all", "roots", "0x<schema>", or text every printed root must contain.
    auto what = argument("what", Type::text, true);
    what.complete = [](const Model &, auto) { return std::vector<std::string>{"all", "roots"}; };
    auto delay = argument("delay_seconds", Type::unsigned_integer, true);
    delay.maximum = 600;
    auto dump = action("ui dump", "Write the live native UI model tree to the game's logs folder (research aid)",
                       Group::console, {std::move(what), std::move(delay)});
    dump.run = [](const Model &, const Values &values, const Output &out) {
        const auto text = values.empty() ? std::string{} : std::get<std::string>(values[0]);
        const auto seconds = values.size() > 1 ? std::get<std::uint64_t>(values[1]) : 0;
        out(multiplayer::request_ui_dump(text, static_cast<unsigned>(seconds)));
    };
    registry.add(std::move(dump));
    auto then = argument("reload", Type::text, true);
    auto apply = action("mods apply", "Apply Mods/ and mods.json now; takes effect at the next level load, "
                        "or straight away with 'reload'", Group::console, {std::move(then)});
    apply.run = [](const Model &, const Values &values, const Output &out) {
        const auto reload = !values.empty() && _stricmp(std::get<std::string>(values[0]).c_str(), "reload") == 0;
        out(live_mods::apply(reload));
    };
    registry.add(std::move(apply));
}
} // namespace dingosdk::console
