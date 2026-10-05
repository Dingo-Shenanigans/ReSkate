#include "server_host.h"
#include "server_text.h"
#include <algorithm>
#include <array>
#include <cmath>

// Player votes (map, kick, time of day) and the chat commands that run them. Each vote is
// switched on and given its pass percentage in ReSkateServer.json ("votes"); also the map pool and rotation.
namespace dingosdk::server {
namespace {
constexpr std::array<std::string_view, 8> times{"default", "morning", "noon",       "afternoon",
                                                "evening", "night",   "weatherday", "weathernight"};
const char *vote_name(Host::VoteKind kind) {
    return kind == Host::VoteKind::map ? "Map" : kind == Host::VoteKind::kick ? "Kick" : "Time of day";
}
} // namespace

const VoteSetting &Host::vote_setting(VoteKind kind) const {
    return kind == VoteKind::map ? config_.votes.map : kind == VoteKind::kick ? config_.votes.kick : config_.votes.time;
}
std::uint8_t Host::enabled_votes() const {
    std::uint8_t bits{};
    if (config_.votes.map.enabled) bits |= server_vote_map;
    if (config_.votes.kick.enabled) bits |= server_vote_kick;
    // Time of day is a world layer choice: it only reaches players while layer sync is on.
    if (config_.votes.time.enabled && config_.world_layer_sync && !world_layers().empty()) bits |= server_vote_time;
    return bits;
}
void Host::reply(Guest &guest, std::string_view text) {
    // Chat lines are single lines: longer answers (admin commands) arrive a line at a time.
    unsigned lines{};
    for (std::size_t at = 0; at <= text.size() && lines < 12;) {
        const auto end = text.find('\n', at);
        const auto line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (!trim(line).empty()) {
            send_chat(line, &guest);
            ++lines;
        }
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
}
Host::Guest *Host::match_player(std::string_view text) {
    // A SteamID64, or the start of one connected player's name.
    text = trim(text);
    if (text.empty()) return nullptr;
    if (const auto id = number(split(text).first)) {
        auto *guest = find(*id);
        return guest && guest->handshaken ? guest : nullptr;
    }
    Guest *match{};
    for (auto &[id, guest] : guests_)
        if (guest->handshaken && lower(guest_name(*guest)).starts_with(lower(text))) {
            if (match) return nullptr;
            match = guest.get();
        }
    return match;
}

void Host::chat_command(Guest &guest, std::string_view line) {
    const auto [first, rest] = split(line);
    const auto verb = lower(first);
    if (verb.empty() || verb == "help" || verb == "?") {
        std::string text;
        const auto votes = enabled_votes();
        if (votes & server_vote_map) text += "/vote map <map>: start a vote to change the map\n";
        if (votes & server_vote_kick) text += "/vote kick <player>: start a vote to kick a player\n";
        if (votes & server_vote_time) text += "/vote tod <time>: vote for a time of day (morning, noon, night...)\n";
        if (votes) text += "/yes or /no: vote in the running vote\n";
        if (config_.map_rotation) text += "The map changes every " + std::to_string(config_.map_rotation) + " min.\n";
        if (config_.parties) text += "/party: your party (invite, accept, leave...; /party help); /p <message>: party chat\n";
        if (is_admin(guest.member.id)) text += "Admins: any server command as /<command>, e.g. /kick, /map, /tpall, /votes, /msg, /msg-party, /msg-admins\n";
        const std::string whisper = "/w <player> <message>: send a private message";
        return reply(guest, (text.empty() ? "This server has no player votes. Type /tp <player> to teleport.\n" : text) + whisper);
    }
    if (verb == "party") return party_command(guest, rest);
    if (verb == "p") {
        if (!config_.parties) return reply(guest, "Parties are off on this server.");
        return party_chat(guest, rest);
    }
    if (verb == "w" || verb == "whisper" || verb == "tell") {
        // A private message to one player, marked "[DM from ...]"; the sender sees an echo.
        const auto [who, text] = split(rest);
        if (text.empty()) return reply(guest, "/w <player> <message>, e.g. /w player hello");
        auto *other = match_player(who);
        if (!other) return reply(guest, "No single connected player matches \"" + std::string(who) + "\".");
        if (other == &guest) return reply(guest, "You cannot message yourself.");
        send_chat(dm_line(guest_name(guest), {}, text, multiplayer_chat_max_bytes), other);
        return reply(guest, "[DM to " + guest_name(*other) + "] " + clean_chat_text(text));
    }
    if (verb == "yes" || verb == "y") return cast_vote(guest, true);
    if (verb == "no" || verb == "n") return cast_vote(guest, false);
    if (verb == "vote") {
        const auto [what_text, argument] = split(rest);
        const auto what = lower(what_text);
        if (what == "yes" || what == "y") return cast_vote(guest, true);
        if (what == "no" || what == "n") return cast_vote(guest, false);
        if (what == "map") return start_vote(guest, VoteKind::map, argument);
        if (what == "kick") return start_vote(guest, VoteKind::kick, argument);
        if (what == "tod" || what == "time") return start_vote(guest, VoteKind::time, argument);
        if (vote_) return reply(guest, "Running: a vote to " + vote_->label + ". Type /yes or /no.");
        return reply(guest, enabled_votes() ? "Start one with /vote map, /vote kick or /vote tod (see /help)."
                                            : "This server has no player votes.");
    }
    // Admins run any server command from chat, as they do with "mp server". The caller
    // has already logged the line with any password hidden, so it is not logged again here.
    if (is_admin(guest.member.id)) {
        const auto id = guest.member.id;
        const auto answer = command(line, id);
        if (auto *still = find(id)) reply(*still, answer.empty() ? std::string("Done.") : answer);
        return;
    }
    reply(guest, "Unknown command /" + verb + ". Type /help for the list.");
}

void Host::start_vote(Guest &guest, VoteKind kind, std::string_view argument) {
    const auto &setting = vote_setting(kind);
    if (!(enabled_votes() & (kind == VoteKind::map ? server_vote_map : kind == VoteKind::kick ? server_vote_kick : server_vote_time)))
        return reply(guest, kind == VoteKind::time && setting.enabled
                                ? "Time of day votes need world layer sync on the server."
                                : std::string(vote_name(kind)) + " votes are off on this server.");
    if (vote_) return reply(guest, "A vote is already running: " + vote_->label + ". Type /yes or /no.");
    if (const auto wait = vote_cooldowns_.find(guest.member.id); wait != vote_cooldowns_.end() && now_ < wait->second)
        return reply(guest, "Wait " + std::to_string((wait->second - now_) / 1000000 + 1) + " s before starting another vote.");
    Vote vote;
    vote.kind = kind;
    vote.starter = guest.member.id;
    switch (kind) {
    case VoteKind::map: {
        if (argument.empty()) return reply(guest, "/vote map <map>, e.g. /vote map grom");
        // Players vote between the server's own maps (/maps); a raw level path is admins only.
        const auto *level = find_level(argument);
        if (!level || !valid_map_destination(map_destination(argument)))
            return reply(guest, "No single map is called \"" + std::string(argument) + "\".");
        if (!in_map_pool(config_, level->asset)) return reply(guest, level->name + " is not one of this server's maps.\n" + pool_text());
        if (map_hash(map_destination(argument)) == map_) return reply(guest, "The server is already on that map.");
        vote.value = std::string(argument);
        vote.label = "change the map to " + map_label(argument);
        break;
    }
    case VoteKind::kick: {
        auto *target = match_player(argument);
        if (!target) return reply(guest, "No single connected player matches \"" + std::string(argument) + "\".");
        if (target == &guest) return reply(guest, "You cannot vote to kick yourself.");
        if (is_admin(target->member.id)) return reply(guest, "Admins cannot be kicked by a vote.");
        vote.target = target->member.id;
        vote.label = "kick " + guest_name(*target);
        break;
    }
    case VoteKind::time: {
        const auto wanted = lower(argument);
        if (std::find(times.begin(), times.end(), wanted) == times.end())
            return reply(guest, "/vote tod <default|morning|noon|afternoon|evening|night|weatherday|weathernight>");
        vote.value = wanted;
        vote.label = "set the time of day to " + wanted;
        break;
    }
    }
    vote.yes.insert(guest.member.id); // the starter is for it
    vote.ends = now_ + static_cast<std::uint64_t>(config_.votes.seconds) * 1000000;
    vote_cooldowns_[guest.member.id] = now_ + static_cast<std::uint64_t>(config_.votes.cooldown) * 1000000;
    const auto label = vote.label;
    vote_ = std::move(vote);
    send_chat(guest_name(guest) + " started a vote to " + label + " (" + std::to_string(setting.percent) + "% needed, " +
              std::to_string(config_.votes.seconds) + " s). Type /yes or /no.");
    log_("[vote] " + guest_name(guest) + " started a vote to " + label + ".");
    check_vote(false);
}

void Host::cast_vote(Guest &guest, bool yes) {
    if (!vote_) return reply(guest, "No vote is running.");
    auto &vote = *vote_;
    if (vote.kind == VoteKind::kick && guest.member.id == vote.target) return reply(guest, "You cannot vote on your own kick.");
    const bool changed = !(yes ? vote.yes : vote.no).contains(guest.member.id);
    vote.yes.erase(guest.member.id);
    vote.no.erase(guest.member.id);
    (yes ? vote.yes : vote.no).insert(guest.member.id);
    if (!changed) return reply(guest, yes ? "You already voted yes." : "You already voted no.");
    check_vote(false);
}

void Host::cancel_vote(const std::string &why) {
    if (!vote_) return;
    const auto label = vote_->label;
    vote_.reset();
    send_chat("The vote to " + label + " was cancelled: " + why + ".");
    log_("[vote] The vote to " + label + " was cancelled: " + why + ".");
}

void Host::check_vote(bool expired) {
    if (!vote_) return;
    auto &vote = *vote_;
    // Everyone connected may vote, except the player a kick vote is about.
    std::set<std::uint64_t> voters;
    for (const auto &[id, guest] : guests_)
        if (guest->handshaken && id != vote.target) voters.insert(id);
    std::erase_if(vote.yes, [&](std::uint64_t id) { return !voters.contains(id); });
    std::erase_if(vote.no, [&](std::uint64_t id) { return !voters.contains(id); });
    if (vote.kind == VoteKind::kick && !find(vote.target)) return cancel_vote("the player left");
    const auto percent = std::clamp(vote_setting(vote.kind).percent, 1U, 100U);
    const auto eligible = static_cast<unsigned>(voters.size());
    // A kick needs a second player's yes: with only the starter and the target on, it fails.
    const auto needed = std::max(vote.kind == VoteKind::kick ? 2U : 1U, (eligible * percent + 99) / 100);
    const auto yes = static_cast<unsigned>(vote.yes.size()), no = static_cast<unsigned>(vote.no.size());
    const bool passed = yes >= needed;
    const bool lost = !passed && (expired || yes + (eligible - std::min(eligible, yes + no)) < needed);
    const auto tally = std::to_string(yes) + " yes, " + std::to_string(no) + " no, " + std::to_string(needed) + " needed";
    if (!passed && !lost) {
        if (yes != vote.shown_yes || no != vote.shown_no) {
            vote.shown_yes = yes;
            vote.shown_no = no;
            send_chat("Vote to " + vote.label + ": " + tally + ".");
        }
        return;
    }
    const auto done = std::move(vote);
    vote_.reset();
    if (lost) {
        send_chat("The vote to " + done.label + " failed (" + tally + ").");
        log_("[vote] The vote to " + done.label + " failed (" + tally + ").");
        return;
    }
    send_chat("The vote to " + done.label + " passed (" + tally + ").");
    log_("[vote] The vote to " + done.label + " passed (" + tally + ").");
    switch (done.kind) {
    case VoteKind::map: log_(command("map " + done.value)); break;
    case VoteKind::time: log_(command("tod " + done.value)); break;
    case VoteKind::kick:
        if (find(done.target)) {
            kicked_.insert(done.target);
            drop(done.target, "You were kicked from this server by a vote.");
        }
        break;
    }
}

std::string Host::pool_text() const {
    std::string text = config_.map_pool.empty() ? "Map pool: every map" : "Map pool:";
    if (!config_.map_pool.empty())
        for (const auto *level : pool_levels(config_)) text += "\n  " + level->name + (same_map(level->asset) ? "  (now)" : "");
    return text;
}
std::string Host::rotation_text() const {
    if (!config_.map_rotation) return "Map rotation is off.";
    const auto every = "The map changes every " + std::to_string(config_.map_rotation) + " min";
    const auto *next = next_pool_map(config_, config_.map);
    if (!next) return every + ", but the map pool has no other map.";
    if (!players()) return every + " while players are on. Next: " + next->name + ".";
    const auto due = map_since_ + std::uint64_t{config_.map_rotation} * 60000000;
    const auto left = due > now_ ? (due - now_ + 59999999) / 60000000 : 0;
    return every + ". Next: " + next->name + " in about " + std::to_string(std::max<std::uint64_t>(left, 1)) + " min.";
}
void Host::resend_maps() {
    for (auto &[id, guest] : guests_) guest->maps_sent = false;
}
void Host::tick_rotation() { // waits while nobody is on, and for a running map vote
    if (!config_.map_rotation || !players()) {
        map_since_ = now_;
        rotation_warned_ = false;
        return;
    }
    const auto due = map_since_ + std::uint64_t{config_.map_rotation} * 60000000;
    if (now_ + 60000000 < due) return;
    const auto *next = next_pool_map(config_, config_.map);
    if (!next) {
        map_since_ = now_;
        return;
    }
    if (!rotation_warned_ && config_.map_rotation > 1) {
        rotation_warned_ = true;
        send_chat("Next map in 1 minute: " + next->name + ".");
    }
    if (now_ < due || (vote_ && vote_->kind == VoteKind::map)) return;
    send_chat("Changing the map to " + next->name + ".");
    log_("[rotation] Changing the map to " + next->name + ".");
    change_map(next->name);
    save();
}
} // namespace dingosdk::server
