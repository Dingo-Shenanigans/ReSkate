#include "server_host.h"
#include "server_text.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Game/World/park_randomization.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>

// Admin commands, from the server console or an admin's game, and the anti-cheat checks they switch.
namespace dingosdk::server {
namespace {
constexpr std::string_view help_text =
    "status | net [player] | players | say <text> | msg <player> <text> | msg-party <player> <text> | msg-admins <text> | kick <player> | ban <player or SteamID64> [name] | unban <SteamID64> | bans\n"
    "map <name, e.g. San Vansterdam> | maps | name <text> | password <text|off> | welcome <text|off> | listed on|off\n"
    "voice on|off | voice-range <50-1000> | distances <full> <half> <half-return> <low> | crowd <n>|off | rate <KB/s> | bone-scale <1-8>|off | bone-reach <0.5-20>|off\n"
    "placement everyone|admins|nobody | objects <number>|off | object-scaling on|off | effects on|off | clear-objects | noclip on|off | nobail on|off | boosts on|off | tuning on|off\n"
    "tpall [player] | tphere <player> | votes [<vote> on|off|<percent>|seconds|cooldown|min-players <n>] | vote-cancel\n"
    "votes polls off|admins|everyone | votes poll-seconds <n> | votes starter-yes on|off\n"
    "vote <map|kick|tod|<custom vote>> [argument] | poll <question> | <answer> | <answer>... | poll end\n"
    "poll-run <command with {answer}> | <question> | <answer> | <answer>...\n"
    "announce <text> | announcements [list|add <text>|remove <n>|clear|interval <minutes>|off|card on|off]\n"
    "map-pool [add|remove <map>|clear] | rotation [<minutes>|off]\n"
    "park <lot> <layout> | park random | layer-sync on|off | layer <key> default|on|off | tod <time|default>\n"
    "activity-log on|off | announce-throwdowns on|off | parties [on|off] | party-size <2-8> | afk-kick <minutes>|off | speed-check off|warn|kick\n"
    "score-check [off|warn|kick] | score-allow [<fingerprint>|remove <fingerprint>]\n"
    "reserved [slots <n> | add|remove <SteamID64>] | admin add|remove <SteamID64> | admins | update | quit";
} // namespace

std::string Host::command(std::string_view line, std::uint64_t admin) {
    const bool console = admin == 0;
    auto [action, argument] = split(line);
    const auto verb = lower(action);
    // The in-game menu's names for the same settings.
    const std::string name = verb == "voice-allow" ? "voice" : verb == "object-placement" ? "placement"
                           : verb == "object-limit" ? "objects"
                           : verb == "world-layer-sync" ? "layer-sync" : verb == "noclip-allow" ? "noclip"
                           : verb == "nobail-allow" ? "nobail" : verb == "boosts-allow" ? "boosts"
                           : verb == "tuning-enforce" ? "tuning" : verb;
    const auto target = [&](std::string_view text) -> Guest * {
        // A SteamID64 (optionally followed by the player's session epoch, as the
        // in-game menu sends it), or the start of a connected player's name.
        // Every name starts with "", so a bare `kick` would pick the only player.
        if (text.empty()) return nullptr;
        const auto [first, rest] = split(text);
        (void)rest;
        if (const auto id = number(first)) return find(*id);
        Guest *match{};
        for (auto &[id, guest] : guests_)
            if (guest->handshaken && lower(guest->member.name).starts_with(lower(text))) {
                if (match) return nullptr;
                match = guest.get();
            }
        return match;
    };
    const auto changed = [&](std::string text) {
        ++bans_revision_; // cheap: admins only get a fresh list when it moved
        save();
        roster_dirty_ = true;
        if (!console) log_(text); // the console logs its own replies
        return text;
    };
    if (name.empty() || name == "help") return std::string(help_text);
    if (name == "status")
        return config_.name + " | " + map_name() + " | " + std::to_string(players()) + "/" +
               std::to_string(config_.max_players) + " players | " + std::to_string(config_.tps) + " TPS | voice " +
               (voice_policy_.allowed ? "on" : "off") + " (" + std::to_string(static_cast<int>(config_.voice_range)) +
               " m) | password " + (password_ ? "on" : "off") + " | code " + invite();
    if (name == "net") return network_report(argument, console);
    if (name == "players") {
        std::string text = std::to_string(players()) + " players";
        for (const auto &[id, guest] : guests_)
            if (guest->handshaken)
                text += "\n  " + std::to_string(id) + "  " + guest_name(*guest) + (is_admin(id) ? "  (admin)" : "");
        return text;
    }
    if (name == "say") {
        if (!console) return "Use chat to talk to everyone.";
        if (argument.empty()) return "say <text>";
        send_chat(argument);
        return "[chat] Server: " + clean_chat_text(argument);
    }
    if (name == "msg" || name == "msg-party" || name == "msg-admins") {
        // Direct messages from the console or an admin, marked "[DM from ...]" so nobody takes them for chat.
        const auto *sender = console ? nullptr : find(admin);
        const std::string from = sender ? guest_name(*sender) : "Server";
        const bool to_admins = name == "msg-admins";
        const auto [who, text] = to_admins ? std::pair<std::string_view, std::string_view>{{}, trim(argument)} : split(argument);
        if (text.empty()) return to_admins ? "msg-admins <text>" : name + " <player> <text>";
        std::vector<Guest *> recipients;
        std::string scope, label;
        if (to_admins) {
            scope = label = "admins";
            for (auto &[id, guest] : guests_)
                if (guest->handshaken && is_admin(id)) recipients.push_back(guest.get());
        } else {
            auto *guest = match_player(who);
            if (!guest) return "No single connected player matches \"" + std::string(who) + "\".";
            label = guest_name(*guest);
            if (name == "msg") {
                recipients.push_back(guest);
            } else {
                const auto *details = parties_.party(parties_.party_of(guest->member.id));
                if (!details) return label + " is not in a party.";
                scope = "party";
                label += "'s party";
                for (const auto member : details->members)
                    if (auto *found = find(member); found && found->handshaken) recipients.push_back(found);
            }
        }
        if (recipients.empty()) return "No admins are online.";
        const auto message = dm_line(from, scope, text, multiplayer_chat_max_bytes);
        for (auto *guest : recipients) send_chat(message, guest);
        const auto done = "Sent to " + label + (recipients.size() > 1 || to_admins ? " (" + std::to_string(recipients.size()) + " players)" : "") + ".";
        if (!console) log_("[dm] " + from + " -> " + label + ": " + clean_chat_text(text));
        return done;
    }
    if (name == "kick") {
        auto *guest = target(argument);
        if (!guest || !guest->handshaken) return "No single connected player matches \"" + std::string(argument) + "\".";
        // Admins answer to the console, not to each other.
        if (!console && is_admin(guest->member.id)) return "Admins cannot kick other admins.";
        const auto label = guest_name(*guest);
        kicked_.insert(guest->member.id);
        drop(guest->member.id, "You were kicked from this server.");
        return changed(label + " was kicked until the server restarts.");
    }
    if (name == "ban") {
        auto [who, reason] = split(argument);
        auto *guest = target(who);
        std::uint64_t id = guest ? guest->member.id : number(who).value_or(0);
        if (!individual_steam_id(id)) return "Enter a connected player or a SteamID64 (17 digits starting 7656119).";
        if (id == admin) return "You cannot ban yourself.";
        if (!console && is_admin(id)) return "Admins cannot ban other admins.";
        if (is_banned(id)) return std::to_string(id) + " is already banned.";
        auto label = guest ? guest->member.name : clean_chat_text(reason);
        // Banned by SteamID after they left: the name they were last here under.
        if (const auto seen = seen_names_.find(id); label.empty() && seen != seen_names_.end()) label = seen->second;
        cut_text(label, 64);
        config_.bans.push_back({id, label, static_cast<std::int64_t>(std::time(nullptr))});
        if (guest) drop(id, "You were banned from this server.");
        return changed((label.empty() ? std::to_string(id) : label) + " was banned.");
    }
    if (name == "unban") {
        const auto id = number(argument).value_or(0);
        const auto found = std::find_if(config_.bans.begin(), config_.bans.end(), [&](const auto &b) { return b.id == id; });
        if (found == config_.bans.end()) return "That SteamID64 is not banned.";
        const auto label = found->name.empty() ? std::to_string(id) : found->name;
        config_.bans.erase(found);
        kicked_.erase(id);
        return changed(label + " was unbanned.");
    }
    if (name == "bans") {
        std::string text = std::to_string(config_.bans.size()) + " banned";
        for (const auto &ban : config_.bans) text += "\n  " + std::to_string(ban.id) + "  " + ban.name;
        return text;
    }
    if (name == "map") {
        // The map as it will be stored must still name a destination: one that does not would
        // leave the server unable to tell players where to go, and unable to start again.
        if (argument.empty() || !valid_map_destination(map_destination(argument)) ||
            !valid_map_destination(map_destination(map_setting(argument))))
            return "No single map is called \"" + std::string(argument) + "\". Type maps for the list.";
        // Only a map the server has: the game's own, or one from a mod in its Mods folder.
        if (!installed_map(argument))
            return "This server does not have that map. Put the map's mod folder in Mods next to the server, then restart it.";
        if (map_hash(map_destination(argument)) == map_) return "The server is already on that map.";
        change_map(argument);
        save();
        return changed("Changing map to " + map_name());
    }
    if (name == "maps") {
        std::string text = std::to_string(levels().size()) + " maps (custom maps come from Mods next to the server)";
        for (const auto &level : levels())
            text += "\n  " + level.name + (same_map(level.asset) ? "  (now)" : "") +
                    (!config_.map_pool.empty() && in_map_pool(config_, level.asset) ? "  (pool)" : "");
        return text;
    }
    if (name == "map-pool") { // map-pool [add|remove <map>|clear]
        const auto [what_text, map] = split(argument);
        const auto what = lower(what_text);
        if (what.empty()) return pool_text();
        if (what == "clear") {
            config_.map_pool.clear();
            resend_maps();
            return changed("The map pool is cleared: players vote between every map, and the rotation goes through them all.");
        }
        if (what != "add" && what != "remove") return "map-pool [add|remove <map>|clear]";
        const auto *level = find_level(map);
        if (!level || !valid_map_destination(map_destination(level->asset)))
            return "No single map is called \"" + std::string(map) + "\". Type maps for the list.";
        const auto pooled = [&](const std::string &entry) { return find_level(entry) == level; };
        const bool listed = std::any_of(config_.map_pool.begin(), config_.map_pool.end(), pooled);
        if (what == "add") {
            if (listed || config_.map_pool.empty()) return level->name + " is already in the map pool.";
            config_.map_pool.push_back(level->name);
        } else {
            if (config_.map_pool.empty()) // every map: keep all the others
                for (const auto *other : pool_levels(config_)) config_.map_pool.push_back(other->name);
            else if (!listed) return level->name + " is not in the map pool.";
            if (std::all_of(config_.map_pool.begin(), config_.map_pool.end(), pooled))
                return "The map pool needs at least one map. map-pool clear allows every map again.";
            std::erase_if(config_.map_pool, pooled);
        }
        resend_maps();
        return changed(level->name + (what == "add" ? " added to" : " removed from") + " the map pool.");
    }
    if (name == "rotation") { // rotation [<minutes>|off]
        if (argument.empty()) return rotation_text();
        const auto value = lower(argument);
        const auto minutes = value == "off" ? std::optional<std::uint64_t>(0) : number(value);
        if (!minutes || *minutes > max_map_rotation) return "rotation <1-1440 minutes>|off";
        config_.map_rotation = static_cast<unsigned>(*minutes);
        map_since_ = now_;
        rotation_warned_ = false;
        resend_maps();
        return changed(rotation_text());
    }
    if (name == "name") {
        if (!valid_server_name(argument)) return std::string("Server names are ") + server_name_rule + ".";
        config_.name = argument;
        if (text::contains_bad_words(config_.name))
            return changed("Server renamed to " + config_.name +
                           ". That name contains blocked words, so the server stays out of the server browser.");
        return changed("Server renamed to " + config_.name + ".");
    }
    if (name == "password") {
        if (argument.size() > 64) return "Passwords are at most 64 characters.";
        config_.password = argument == "off" ? std::string{} : std::string(argument);
        erase_key(password_);
        password_ = config_.password.empty() ? std::nullopt : password_key(config_.password, secret_);
        return changed(config_.password.empty() ? "Password removed. Anyone can join."
                                                : "Password set. Players already here stay; new ones need it.");
    }
    if (name == "welcome") {
        if (argument != "off" && !argument.empty() && !valid_chat_text(argument)) return "The welcome message is one chat line.";
        config_.welcome = argument == "off" ? std::string{} : std::string(argument);
        return changed(config_.welcome.empty() ? "Welcome message removed." : "Welcome message set.");
    }
    if (name == "chat-color" || name == "chat-colour") {
        // chat-color <#badge> [<#text>]: the server's own lines in chat.
        const auto [badge, text] = split(argument);
        if (badge.empty() || !parse_colour(badge) || (!text.empty() && !parse_colour(text)))
            return "chat-color <#RRGGBB badge> [<#RRGGBB text>] (now " + config_.chat_color + " " + config_.chat_text_color + ")";
        config_.chat_color = std::string(badge);
        if (!text.empty()) config_.chat_text_color = std::string(text);
        return changed("The server's chat lines are " + config_.chat_color + " with " + config_.chat_text_color + " text.");
    }
    if (name == "announce-throwdowns") {
        const auto value = on_off(argument);
        if (!value) return std::string("announce-throwdowns on|off (now ") + (config_.announce_throwdowns ? "on" : "off") + ")";
        config_.announce_throwdowns = *value;
        return changed(*value ? "Placed throwdowns are announced in chat." : "Placed throwdowns are no longer announced.");
    }
    if (name == "parties") {
        const auto value = on_off(argument);
        if (argument.empty()) return std::string(config_.parties ? "Parties are on.\n" : "Parties are off.\n") + party_status(0);
        if (!value) return "parties on|off";
        config_.parties = *value;
        if (!*value) {
            for (auto &[id, guest] : guests_) parties_.remove(id);
            parties_.take_withdrawn();
        }
        return changed(*value ? "Players can form parties." : "Parties are off; every party was ended.");
    }
    if (name == "speed-check") {
        const auto value = lower(argument);
        if (value != "off" && value != "warn" && value != "kick")
            return "speed-check off|warn|kick (now " + config_.speed_check + ")";
        config_.speed_check = value;
        if (value == "off")
            for (auto &[id, guest] : guests_) {
                guest->speed.restart();
                guest->speeding = false;
            }
        return changed(value == "off" ? "Game speed is no longer checked."
                       : value == "kick" ? "Players whose game runs fast are kicked."
                                         : "Players whose game runs fast are taken out of throwdowns and challenges.");
    }
    if (name == "score-check") {
        const auto value = lower(argument);
        if (value.empty()) {
            std::string text = "score-check " + config_.score_check + " (off|warn|kick)";
            for (const auto &[id, guest] : guests_) {
                if (!guest->handshaken) continue;
                text += "\n  " + guest_name(*guest) + ": ";
                if (!guest->scoring) text += "not reported";
                else if (!*guest->scoring) text += "the game's own scoring";
                else
                    text += scoring_text(*guest->scoring) + (guest->scoring_mods.empty() ? "" : " (" + guest->scoring_mods + ")") +
                            (guest->scoring_flagged ? ", out of throwdowns" : ", allowed");
            }
            return text;
        }
        if (value != "off" && value != "warn" && value != "kick") return "score-check off|warn|kick (now " + config_.score_check + ")";
        config_.score_check = value;
        // Kicking changes the guest list: collect first.
        std::vector<std::uint64_t> ids;
        for (const auto &[id, guest] : guests_) ids.push_back(id);
        for (const auto id : ids)
            if (auto *guest = find(id)) check_scoring(*guest);
        return changed(value == "off" ? "Mods that change scoring or physics are no longer checked."
                       : value == "kick" ? "Players whose mods change scoring or physics are kicked."
                                         : "Players whose mods change scoring or physics are taken out of throwdowns and challenges.");
    }
    if (name == "score-allow") {
        auto [what, rest] = split(argument);
        if (what.empty()) {
            std::string text = "score-allow <fingerprint> | score-allow remove <fingerprint>. Accepted besides the game's own:";
            if (config_.score_allow.empty()) text += " none";
            for (const auto fingerprint : config_.score_allow) text += "\n  " + scoring_text(fingerprint);
            return text;
        }
        const bool remove = lower(what) == "remove";
        const auto fingerprint = parse_scoring(remove ? rest : what);
        if (!fingerprint) return "A fingerprint is 16 hex digits, as score-check lists it.";
        const auto found = std::find(config_.score_allow.begin(), config_.score_allow.end(), *fingerprint);
        if (remove) {
            if (found == config_.score_allow.end()) return scoring_text(*fingerprint) + " was not accepted.";
            config_.score_allow.erase(found);
        } else if (found == config_.score_allow.end()) {
            config_.score_allow.push_back(*fingerprint);
        }
        std::vector<std::uint64_t> ids;
        for (const auto &[id, guest] : guests_) ids.push_back(id);
        for (const auto id : ids)
            if (auto *guest = find(id)) check_scoring(*guest);
        return changed(remove ? "Scoring " + scoring_text(*fingerprint) + " is no longer accepted."
                              : "Scoring " + scoring_text(*fingerprint) + " is accepted like the game's own.");
    }
    if (name == "rate") {
        // rate <KB/s>: what the server may send each player, from now on and to those on.
        const auto value = number(argument);
        if (!value || *value < 128 || *value > 16384)
            return "rate <128-16384> (KB/s for each player, now " + std::to_string(config_.send_rate) + ")";
        config_.send_rate = static_cast<unsigned>(*value);
        const bool applied = transport_.set_send_rate(static_cast<int>(config_.send_rate * 1024));
        return changed("Each player is sent at most " + std::to_string(config_.send_rate) + " KB/s" +
                       (applied ? "." : ": players who join from now on. Steam did not change the connections already open."));
    }
    if (name == "bone-scale") {
        // bone-scale <1-8>|off: how far a mod may resize part of a skater for the other players.
        float value{};
        const bool off = argument == "off" || argument == "0";
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
        if (!off && (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !(value >= 1.f && value <= 8.f)))
            return "bone-scale <1-8>|off: 1 shows every skater at the game's own proportions, off allows anything (now " +
                   (config_.bone_scale_limit >= 1.f ? std::to_string(config_.bone_scale_limit).substr(0, 4) : std::string("off")) + ")";
        config_.bone_scale_limit = off ? 0.f : value;
        // Whole states go again so that nobody keeps a reference with the old sizes in it.
        for (auto &[id, guest] : guests_)
            for (const auto &[other, unused] : guests_) guest->sender.forget(other, PacketKind::pose);
        return changed(off ? std::string("Mods may resize skaters' body parts freely.")
                           : value == 1.f ? std::string("Skaters show at the game's own proportions: resized body parts are not passed on.")
                                          : "Resized body parts show at up to " + std::to_string(value).substr(0, 4) + "x.");
    }
    if (name == "bone-reach") {
        const bool off = argument == "off";
        float value{};
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
        if (!off && (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !(value >= .5f && value <= 20.f)))
            return "bone-reach <0.5-20>|off: how many metres a bone of a skater may be from the one it hangs from (now " +
                   (config_.bone_reach_limit > 0.f ? std::to_string(config_.bone_reach_limit).substr(0, 4) : std::string("off")) + ")";
        config_.bone_reach_limit = off ? 0.f : value;
        // Whole states go again so that nobody keeps a reference with the old places in it.
        for (auto &[id, guest] : guests_)
            for (const auto &[other, unused] : guests_) guest->sender.forget(other, PacketKind::pose);
        return changed(off ? std::string("Skaters' bones may be moved any distance.")
                           : "Skaters' bones show at most " + std::to_string(value).substr(0, 4) + " m from where they hang.");
    }
    if (name == "crowd") {
        // crowd <poses a second>|off: the most one player is sent (crowd_limits).
        auto value = number(argument);
        if (argument == "off") value = 0;
        if (!value || *value > max_crowd_budget || !valid_crowd_budget(static_cast<unsigned>(*value)))
            return "crowd <" + std::to_string(min_crowd_budget) + "-" + std::to_string(max_crowd_budget) + ">|off (now " +
                   (config_.crowd_budget ? std::to_string(config_.crowd_budget) : std::string("off")) + ")";
        config_.crowd_budget = static_cast<unsigned>(*value);
        next_crowd_ = 0;
        return changed(*value ? "Each player is sent at most " + std::to_string(*value) + " poses a second: about " +
                                    std::to_string(*value / config_.tps) + " players near them at the full rate."
                              : std::string("No crowd limit: every player near is sent at the full rate."));
    }
    if (name == "afk-kick") {
        const auto value = lower(argument) == "off" ? std::optional<std::uint64_t>(0) : number(argument);
        if (!value || *value > 1440)
            return "afk-kick <minutes 1-1440>|off (now " + (config_.afk_kick ? std::to_string(config_.afk_kick) + " min" : std::string("off")) + ")";
        config_.afk_kick = static_cast<unsigned>(*value);
        // Nobody is removed for time away before the rule was set.
        for (auto &[id, guest] : guests_)
            if (guest->active_at) active(*guest);
        return changed(config_.afk_kick ? "Players away for " + std::to_string(config_.afk_kick) + " min are removed. Admins are not."
                                        : std::string("Players are no longer removed for being away."));
    }
    if (name == "party-size") {
        const auto value = number(argument);
        if (!value || *value < 2 || *value > 8) return "party-size <2-8> (now " + std::to_string(config_.party_size) + ")";
        config_.party_size = static_cast<unsigned>(*value);
        parties_.set_limit(config_.party_size);
        return changed("Parties hold up to " + std::to_string(config_.party_size) + " players. Larger ones stay until members leave.");
    }
    if (name == "activity-log") {
        const auto value = on_off(argument);
        if (!value) return std::string("activity-log on|off (now ") + (config_.activity_log ? "on" : "off") + ")";
        config_.activity_log = *value;
        if (!*value) activity_.clear();
        return changed(*value ? "Player activity (throwdowns, objects, loading) is logged."
                              : "Player activity is no longer logged.");
    }
    if (name == "listed") {
        const auto value = on_off(argument);
        if (!value) return "listed on|off";
        config_.listed = *value;
        return changed(*value ? "The server is listed in the server browser." : "The server is hidden; players need the code.");
    }
    if (name == "tps") {
        return "Dedicated servers run at " + std::to_string(dedicated_tps) + " TPS for now; it cannot be changed.";
    }
    if (name == "voice") {
        const auto value = on_off(argument);
        if (!value) return "voice on|off";
        config_.voice_chat = *value;
        if (voice_policy_.allowed != *value) {
            voice_policy_.allowed = *value;
            if (!++voice_policy_.revision) ++voice_policy_.revision;
        }
        return changed(*value ? "Voice chat allowed." : "Voice chat disabled for everyone.");
    }
    if (name == "voice-range") {
        float range{};
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), range);
        if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || !valid_voice_range(range))
            return "Choose a voice range from 50 to 1000 m.";
        config_.voice_range = range;
        return changed("Voice range set to " + std::to_string(static_cast<int>(range)) + " m.");
    }
    if (name == "distances") {
        MultiplayerDistances value;
        auto rest = argument;
        for (auto *field : {&value.full_rate_return, &value.half_rate_start, &value.half_rate_return, &value.low_rate_start}) {
            const auto [token, remaining] = split(rest);
            const auto result = std::from_chars(token.data(), token.data() + token.size(), *field);
            if (token.empty() || result.ec != std::errc{} || result.ptr != token.data() + token.size())
                return "distances <full> <half> <half-return> <low> (whole metres)";
            rest = remaining;
        }
        if (!rest.empty() || !value.valid())
            return "Use ordered distances: full < half <= half-return < low (at most 10000 m).";
        config_.distances = value;
        for (auto &[id, guest] : guests_) guest->pose_delivery = {};
        return changed("TPS distances updated.");
    }
    if (name == "placement") {
        // The protocol's "host only" is admins only here: the server has no skater of its own.
        const auto policy = parse_object_placement(argument == "admins" ? "host" : argument, config_.object_placement);
        if (!policy) return "placement everyone|admins|nobody";
        config_.object_placement = *policy;
        return changed(*policy == ObjectPlacement::everyone ? "Everyone can place objects."
                       : *policy == ObjectPlacement::host_only ? "Only admins can place objects. Everyone else's are frozen."
                                                               : "Object placement is off. Existing objects stay.");
    }
    if (name == "objects") {
        const auto limit = parse_object_limit(argument);
        if (!limit) return "objects <1-" + std::to_string(max_object_limit) + ">|off";
        config_.object_limit = *limit;
        for (auto &[id, guest] : guests_) guest->shared_from = 0; // look at every layout again
        return changed(*limit ? "Each player can place up to " + std::to_string(*limit) + " objects. Admins are not limited."
                              : std::string("Players can place as many objects as they like."));
    }
    if (name == "object-scaling") {
        const auto value = on_off(argument);
        if (!value) return std::string("object-scaling on|off (now ") + (config_.object_scaling ? "on" : "off") + ")";
        config_.object_scaling = *value;
        for (auto &[id, guest] : guests_) guest->shared_from = 0; // look at every layout again
        return changed(*value ? "Players can resize the objects they place."
                              : "Placed objects are their own size for everyone. Admins can still resize theirs.");
    }
    if (name == "effects") {
        const auto value = on_off(argument);
        if (!value) return std::string("effects on|off (now ") + (config_.sync_effects ? "on" : "off") + ")";
        config_.sync_effects = *value;
        return changed(*value ? "Players see each other's skater effects."
                              : "Players no longer see each other's skater effects. Ones already showing stay until that player's skater is shown again.");
    }
    if (name == "votes") {
        const auto [text, did] = votes_command(argument);
        return did ? changed(text) : text;
    }
    if (name == "announcements") {
        const auto [text, did] = announcements_command(argument);
        return did ? changed(text) : text;
    }
    if (name == "announce") {
        if (argument.empty()) return "announce <text>";
        announce(argument);
        return "Announced.";
    }
    if (name == "vote-cancel") {
        if (!vote_) return "No vote is running.";
        cancel_vote(console ? "the server cancelled it" : "an admin cancelled it");
        return "Vote cancelled.";
    }
    if (name == "vote") {
        // A vote started by the server: the same card and checks as /vote, without a cooldown or a vote of its own.
        Guest *by = console ? nullptr : find(admin);
        const auto [what_text, rest] = split(argument);
        const auto what = lower(what_text);
        const auto started = [](std::string why) { return why.empty() ? std::string("Vote started.") : why; };
        if (what.empty()) return vote_ ? running_vote_text() : std::string("vote <map|kick|tod|<custom vote>> [argument]");
        if (what == "map") return started(start_vote(by, VoteKind::map, rest));
        if (what == "kick") return started(start_vote(by, VoteKind::kick, rest));
        if (what == "tod" || what == "time") return started(start_vote(by, VoteKind::time, rest));
        for (std::size_t i = 0; i < config_.votes.custom.size(); ++i)
            if (config_.votes.custom[i].name == what) return started(start_vote(by, VoteKind::custom, rest, i));
        return "No vote is called \"" + what + "\": vote map, kick, tod or a custom vote's name (votes lists them).";
    }
    if (name == "poll") {
        Guest *by = console ? nullptr : find(admin);
        if (lower(trim(argument)) == "end") {
            const auto why = end_poll(by);
            return why.empty() ? "Poll ended." : why;
        }
        const auto why = start_poll(by, argument);
        return why.empty() ? "Poll started." : why;
    }
    if (name == "poll-run") {
        // "poll-run <command with {answer}> | <question> | <answer>...": the winner's command runs as
        // the console's, so only the console may set one up.
        if (!console) return "poll-run is for the server console only.";
        const auto bar = argument.find('|');
        const auto run = trim(argument.substr(0, bar == std::string_view::npos ? 0 : bar));
        if (run.empty())
            return "poll-run <command with {answer}> | <question> | <answer> | <answer>..., e.g. poll-run tod {answer} | Time of day? | morning | night";
        const auto why = start_poll(nullptr, argument.substr(bar + 1), std::string(run));
        return why.empty() ? "Poll started; the winning answer runs: " + std::string(run) : why;
    }
    if (name == "tpall" || name == "tphere") {
        // Where they go: the admin who asked, or (tpall from the console) the named player.
        Guest *to{};
        std::vector<Guest *> movers;
        if (name == "tpall") {
            to = argument.empty() ? (console ? nullptr : find(admin)) : target(argument);
            if (!to || !to->handshaken)
                return console && argument.empty() ? "tpall <player>: everyone goes to that player."
                                                   : "No single connected player matches \"" + std::string(argument) + "\".";
            for (auto &[id, guest] : guests_)
                if (guest->handshaken && guest->world_ready && guest.get() != to) movers.push_back(guest.get());
        } else {
            if (console) return "tphere is for admins in the game; the console can use tpall <player>.";
            to = find(admin);
            auto *who = target(argument);
            if (!who || !who->handshaken) return "No single connected player matches \"" + std::string(argument) + "\".";
            if (who == to) return "That is you.";
            movers.push_back(who);
        }
        if (!to || !to->latest_root) return "There is no position for " + (to ? guest_name(*to) : std::string("you")) + " yet.";
        if (movers.empty()) return "Nobody else is in the world.";
        const auto at = to->latest_root->position;
        unsigned sent{};
        for (std::size_t i = 0; i < movers.size(); ++i) {
            // A ring around them, so nobody lands inside anyone else.
            const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(movers.size());
            auto p = packet(PacketKind::teleport, now_);
            p.teleport = {at[0] + 2.5f * std::cos(angle), at[1] + 1.0f, at[2] + 2.5f * std::sin(angle)};
            if (send_packet(*movers[i], p, true, false)) ++sent;
        }
        const auto text = movers.size() == 1 && sent ? guest_name(*movers[0]) + " was teleported to " + guest_name(*to) + "."
                                                     : std::to_string(sent) + " player(s) teleported to " + guest_name(*to) + ".";
        if (!console) log_(text);
        return text;
    }
    if (name == "noclip" || name == "nobail" || name == "boosts") {
        auto &allowed = name == "noclip" ? config_.noclip : name == "nobail" ? config_.no_bail : config_.boosts;
        const auto value = argument == "toggle" ? std::optional<bool>(!allowed) : on_off(argument);
        if (!value) return name + " on|off (now " + (allowed ? "on" : "off") + ")";
        allowed = *value;
        const std::string tool = name == "noclip" ? "Noclip and teleporting" : name == "nobail" ? "No Bail" : "Boosts";
        return changed(tool + (*value ? (name == "boosts" ? " are" : " is") + std::string(" allowed for everyone.")
                                      : (name == "boosts" ? " are" : " is") + std::string(" off for players; admins keep it.")));
    }
    if (name == "tuning") {
        const auto value = argument == "toggle" ? std::optional<bool>(!config_.enforce_tuning) : on_off(argument);
        if (!value) return std::string("tuning on|off (now ") + (config_.enforce_tuning ? "on" : "off") + ")";
        config_.enforce_tuning = *value;
        return changed(*value ? "Players skate with the game's own physics tuning."
                              : "Players skate with their own physics tuning.");
    }
    if (name == "clear-objects") {
        std::size_t removed{};
        for (auto &[id, guest] : guests_) {
            if (!guest->handshaken) continue;
            for (const auto *state : {&guest->objects, &guest->shared})
                for (const auto &[object, value] : state->objects()) {
                    (void)value;
                    guest->cleared.insert(object);
                }
            removed += guest->shared.objects().size();
            if (guest->shared.revision()) guest->shared.replace({});
            guest->shared_from = guest->objects.revision();
        }
        ++object_clears_;
        return changed("Deleted " + std::to_string(removed) + " placed object" + (removed == 1 ? "." : "s."));
    }
    if (name == "park") {
        if (lower(argument) == "random") {
            config_.parks = random_park_choices();
            return changed("Random layouts selected for every park slot.");
        }
        const auto [lot_name, layout] = split(argument);
        const auto lot = std::find_if(park_lots.begin(), park_lots.end(), [&](const auto &l) { return l.key == lot_name; });
        if (lot == park_lots.end()) return "park construction|historic|financial <layout, e.g. skatepark_01, or empty>";
        const auto index = static_cast<unsigned>(lot - park_lots.begin());
        if (layout.empty() || !valid_park(index, layout)) return "That is not a layout for this lot.";
        config_.parks[index] = layout;
        return changed(std::string(lot->label) + " now shows " + park_label(layout) + ".");
    }
    if ((name == "layer-sync" || name == "layer" || name == "layers" || name == "tod") && world_layers().empty())
        return "World layers need world-layers.json next to the server (copy it from a player's "
               "%LOCALAPPDATA%\\ReSkate\\cache folder for the same game build).";
    if (name == "layer-sync") {
        const auto value = on_off(argument);
        if (!value) return "layer-sync on|off";
        config_.world_layer_sync = *value;
        apply_layers();
        return changed(*value ? "Everyone now follows the server's world layers." : "Players choose their own world layers.");
    }
    if (name == "layers") {
        // Several at once, as key=mode pairs: the in-game time of day sends seven.
        std::vector<std::pair<std::string, std::string>> changes;
        for (auto rest = argument; !rest.empty();) {
            const auto [pair, remaining] = split(rest);
            rest = remaining;
            const auto equals = pair.find('=');
            if (equals == std::string_view::npos) return "layers <key>=default|on|off ...";
            const auto key = pair.substr(0, equals), mode = pair.substr(equals + 1);
            if (std::none_of(world_layers().begin(), world_layers().end(), [&](const auto &l) { return l.key == key; }))
                return "No world layer is called \"" + std::string(key) + "\".";
            if (!valid_world_layer_mode(mode)) return "layers <key>=default|on|off ...";
            changes.emplace_back(key, mode);
        }
        if (changes.empty()) return "layers <key>=default|on|off ...";
        for (const auto &[key, mode] : changes) {
            if (mode == "default") config_.layers.erase(key);
            else config_.layers[key] = mode;
        }
        apply_layers();
        return changed(std::to_string(changes.size()) + " world layer" + (changes.size() == 1 ? "" : "s") + " changed" +
                       (config_.world_layer_sync ? "." : ". Turn on layer-sync to apply them to everyone."));
    }
    if (name == "tod") {
        // Every map's seven time layers ("<map>_tod_<n>_<name>"): one on and the rest off, or
        // all back to the level's own. Set for every map, so it holds across map changes.
        static constexpr std::array<std::string_view, 8> times{"default", "morning", "noon", "afternoon",
                                                               "evening", "night", "weatherday", "weathernight"};
        const auto wanted = lower(argument);
        const auto found = std::find(times.begin(), times.end(), wanted);
        if (found == times.end()) return "tod default|morning|noon|afternoon|evening|night|weatherday|weathernight";
        const auto slot = static_cast<char>('0' + (found - times.begin()));
        unsigned count{};
        for (const auto &layer : world_layers()) {
            const auto at = layer.key.find("_tod_");
            if (at == std::string::npos || at + 5 >= layer.key.size()) continue;
            if (slot == '0') config_.layers.erase(layer.key);
            else config_.layers[layer.key] = layer.key[at + 5] == slot ? "on" : "off";
            ++count;
        }
        if (!count) return "world-layers.json has no time-of-day layers.";
        apply_layers();
        return changed("Time of day set to " + std::string(*found) +
                       (config_.world_layer_sync ? " for everyone." : ". Turn on layer-sync to apply it to everyone."));
    }
    if (name == "layer") {
        const auto [key, mode] = split(argument);
        const auto found = std::find_if(world_layers().begin(), world_layers().end(), [&](const auto &l) { return l.key == key; });
        if (found == world_layers().end()) return "No world layer is called \"" + std::string(key) + "\".";
        if (!valid_world_layer_mode(mode)) return "layer <key> default|on|off";
        if (mode == "default") config_.layers.erase(std::string(key));
        else config_.layers[std::string(key)] = mode;
        apply_layers();
        return changed(found->label + " set to " + std::string(mode) +
                       (config_.world_layer_sync ? "." : ". Turn on layer-sync to apply it to everyone."));
    }
    if (name == "reserved") {
        // reserved | reserved add|remove <player or SteamID64>
        if (!console) return "Only the server console manages reserved slots.";
        const auto [sub, who] = split(argument);
        if (sub.empty()) {
            std::string text = std::to_string(extra_slots(config_)) + " extra slots beyond the " + std::to_string(config_.max_players) +
                               ": the admins and these players can join when the server is full";
            for (const auto id : config_.reserved) {
                const auto *guest = find(id);
                text += "\n  " + std::to_string(id) + (guest ? "  " + guest_name(*guest) : std::string{});
            }
            return text;
        }
        auto *guest = target(who);
        const auto id = guest ? guest->member.id : number(who).value_or(0);
        if (!individual_steam_id(id) || (sub != "add" && sub != "remove")) return "reserved | reserved add|remove <player or SteamID64>";
        const bool listed = std::find(config_.reserved.begin(), config_.reserved.end(), id) != config_.reserved.end();
        if (sub == "add") {
            if (!listed && config_.reserved.size() >= 1024) return "The reserved list is full.";
            if (!listed) config_.reserved.push_back(id);
            return changed(std::to_string(id) + " has a reserved slot: they can join when the server is full.");
        }
        std::erase(config_.reserved, id);
        return changed(std::to_string(id) + " no longer has a reserved slot.");
    }
    if (name == "admins" || name == "admin") {
        if (!console) return "Only the server console manages admins.";
        const auto [sub, who] = split(argument);
        if (name == "admins" || sub.empty()) {
            std::string text = std::to_string(config_.admins.size()) + " admins";
            for (const auto id : config_.admins) {
                const auto *guest = find(id);
                text += "\n  " + std::to_string(id) + (guest ? "  " + guest_name(*guest) : std::string{});
            }
            return text;
        }
        auto *guest = target(who);
        const auto id = guest ? guest->member.id : number(who).value_or(0);
        if (!individual_steam_id(id)) return "admin add|remove <player or SteamID64>";
        if (sub == "add") {
            if (!is_admin(id)) config_.admins.push_back(id);
            resend_maps();
            return changed(std::to_string(id) + " is an admin.");
        }
        if (sub == "remove") {
            std::erase(config_.admins, id);
            resend_maps();
            return changed(std::to_string(id) + " is no longer an admin.");
        }
        return "admin add|remove <player or SteamID64>";
    }
    return "Unknown command \"" + std::string(action) + "\". Type help.";
}
// A player's mods change how tricks score (their report; Engine/Vfs/mod_scoring.h): flagged
// players are taken out of linked throwdowns and coop challenges (the roster's scoring flag, and
// the server relays none of theirs), or kicked. The game keeps a scoring mod's points until it
// restarts, so the flag lasts for the session.
void Host::check_scoring(Guest &guest) {
    if (!guest.handshaken) return;
    const bool changed = guest.scoring && *guest.scoring &&
                         std::find(config_.score_allow.begin(), config_.score_allow.end(), *guest.scoring) == config_.score_allow.end();
    const auto name = guest_name(guest);
    if (config_.score_check == "off" || !changed) {
        if (!guest.scoring_flagged) return;
        guest.scoring_flagged = false;
        roster_dirty_ = true;
        log_("[anticheat] " + name + " may take part in throwdowns again (score-check " + config_.score_check + ").");
        return;
    }
    if (guest.scoring_flagged) return;
    const auto mods = guest.scoring_mods.empty() ? std::string("their mods") : guest.scoring_mods;
    log_("[anticheat] " + name + "'s mods change scoring or physics: " + mods + " (scoring " + scoring_text(*guest.scoring) + ").");
    if (config_.score_check == "kick")
        return drop(guest.member.id, "Your mods change scoring or physics (" + mods + "). Turn them off and restart Skate to play here.");
    // Every player's game says so in chat when the roster flags someone, the player themself included.
    guest.scoring_flagged = true;
    roster_dirty_ = true;
}
// A speedhack runs the player's game clock, and so their pose timestamps, faster than real
// time. Flagged players are taken out of linked throwdowns and coop challenges (the roster's
// speeding flag: every client drops them from those), or kicked; the flag clears after a minute
// of normal speed.
bool Host::check_speed(Guest &guest, std::uint64_t sent) {
    if (config_.speed_check == "off" || !guest.handshaken || !guest.world_ready || !sent) return true;
    if (!guest.speed.sample(sent, now_)) return true;
    const auto name = guest_name(guest);
    const auto speed = guest.speed.speed();
    if (guest.speed.flagged() && !guest.speeding) {
        char text[160];
        std::snprintf(text, sizeof text, "%s's game is running at %.2fx speed (a speed hack?).", name.c_str(), speed);
        log_(std::string("[anticheat] ") + text);
        if (config_.speed_check == "kick") {
            drop(guest.member.id, "Your game is running faster than normal. Turn off speed hacks to play here.");
            return false;
        }
        guest.speeding = true;
        guest.speed_normal_since = 0;
        roster_dirty_ = true;
        send_chat("The server measured your game running faster than normal: throwdowns and challenges are off for you until it's back to normal speed.", &guest);
        // Only the player is told in the game; for the admins it is in the log.
        return true;
    }
    if (!guest.speeding) return true;
    if (speed < SpeedCheck::limit) {
        if (!guest.speed_normal_since) guest.speed_normal_since = now_;
        if (now_ - guest.speed_normal_since >= 60000000) {
            guest.speeding = false;
            roster_dirty_ = true;
            log_("[anticheat] " + name + "'s game speed is back to normal.");
            send_chat("Your game speed is back to normal: throwdowns and challenges are on again.", &guest);
        }
    } else guest.speed_normal_since = 0;
    return true;
}
} // namespace dingosdk::server
