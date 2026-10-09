#include "server_host.h"
#include "server_text.h"
#include <algorithm>
#include <array>
#include <cmath>

// Player votes (map, kick, time of day, and the owner's own custom votes), polls,
// announcements, and the chat commands that run them. Each vote is switched on and given its
// pass percentage in ReSkateServer.json ("votes"); also the map pool and rotation. Games show
// the running vote or poll, and each announcement, as a card (the roster carries them).
namespace dingosdk::server {
namespace {
constexpr std::array<std::string_view, 8> times{"default", "morning", "noon",       "afternoon",
                                                "evening", "night",   "weatherday", "weathernight"};
const char *vote_name(Host::VoteKind kind) {
    switch (kind) {
    case Host::VoteKind::map: return "Map";
    case Host::VoteKind::kick: return "Kick";
    case Host::VoteKind::time: return "Time of day";
    case Host::VoteKind::custom: return "Custom";
    case Host::VoteKind::poll: return "Poll";
    }
    return "";
}
std::uint8_t vote_bit(Host::VoteKind kind) {
    switch (kind) {
    case Host::VoteKind::map: return server_vote_map;
    case Host::VoteKind::kick: return server_vote_kick;
    case Host::VoteKind::time: return server_vote_time;
    case Host::VoteKind::custom: return multiplayer::server_vote_custom;
    case Host::VoteKind::poll: return multiplayer::server_vote_poll;
    }
    return 0;
}
std::uint64_t microseconds(unsigned seconds) { return static_cast<std::uint64_t>(seconds) * 1000000; }
// `text` with each `key` replaced by `value`.
std::string fill(std::string text, std::string_view key, std::string_view value) {
    for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}
std::string joined(const std::vector<std::string> &items, std::string_view between) {
    std::string text;
    for (const auto &item : items) text += (text.empty() ? "" : std::string(between)) + item;
    return text;
}
// "Grom 3, San Vansterdam 1"
std::string answer_count(const std::vector<std::string> &answers, const std::vector<unsigned> &count) {
    std::string text;
    for (std::size_t i = 0; i < answers.size() && i < count.size(); ++i)
        text += (i ? ", " : "") + answers[i] + " " + std::to_string(count[i]);
    return text;
}
// "The vote to change the map to Grom", "The poll \"Next map?\""
std::string vote_title(Host::VoteKind kind, const std::string &label) {
    return kind == Host::VoteKind::poll ? "The poll \"" + label + "\"" : "The vote to " + label;
}
} // namespace

const VoteSetting &Host::vote_setting(VoteKind kind, std::size_t custom) const {
    static const VoteSetting none;
    switch (kind) {
    case VoteKind::map: return config_.votes.map;
    case VoteKind::kick: return config_.votes.kick;
    case VoteKind::time: return config_.votes.time;
    case VoteKind::custom: return custom < config_.votes.custom.size() ? config_.votes.custom[custom].setting : none;
    case VoteKind::poll: break;
    }
    return none;
}
std::uint8_t Host::enabled_votes() const {
    std::uint8_t bits{};
    if (config_.votes.map.enabled) bits |= server_vote_map;
    if (config_.votes.kick.enabled) bits |= server_vote_kick;
    // Time of day is a world layer choice: it only reaches players while layer sync is on.
    if (config_.votes.time.enabled && config_.world_layer_sync && !world_layers().empty()) bits |= server_vote_time;
    return bits;
}
multiplayer::ServerPolls Host::enabled_polls() const {
    using multiplayer::ServerPolls;
    return config_.votes.polls == "everyone" ? ServerPolls::everyone
         : config_.votes.polls == "admins"   ? ServerPolls::admins : ServerPolls::off;
}
std::vector<multiplayer::ServerCustomVote> Host::custom_votes() const {
    std::vector<multiplayer::ServerCustomVote> list;
    for (const auto &v : config_.votes.custom)
        if (v.setting.enabled) list.push_back({v.name, v.description, v.choices});
    return list;
}
void Host::reply(Guest &guest, std::string_view text, unsigned max_lines) {
    // Chat lines are single lines: longer answers (admin commands) arrive a line at a time.
    unsigned lines{};
    for (std::size_t at = 0; at <= text.size() && lines < max_lines;) {
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
    using multiplayer::ServerPolls;
    const auto [first, rest] = split(line);
    const auto verb = lower(first);
    const auto custom = custom_votes();
    const auto polls = enabled_polls();
    if (verb.empty() || verb == "help" || verb == "?") {
        std::string text;
        const auto votes = enabled_votes();
        if (votes & server_vote_map) text += "/vote map <map>: start a vote to change the map\n";
        if (votes & server_vote_kick) text += "/vote kick <player>: start a vote to kick a player\n";
        if (votes & server_vote_time) text += "/vote tod <time>: vote for a time of day (morning, noon, night...)\n";
        if (!custom.empty()) {
            std::vector<std::string> names;
            for (const auto &v : custom) names.push_back(v.name);
            text += "/vote <name>: this server's own votes (" + joined(names, ", ") + "); /vote list says what each does\n";
        }
        if (votes || !custom.empty()) text += "/yes or /no: vote in the running vote\n";
        if (polls == ServerPolls::everyone || (polls == ServerPolls::admins && is_admin(guest.member.id)))
            text += "/poll <question> | <answer> | <answer>...: ask everyone a question\n";
        if (polls != ServerPolls::off) text += "/1, /2...: answer the running poll\n";
        if (config_.map_rotation) text += "The map changes every " + std::to_string(config_.map_rotation) + " min.\n";
        if (config_.parties) text += "/party: your party (invite, accept, leave...; /party help); /p <message>: party chat\n";
        if (is_admin(guest.member.id))
            text += "Admins: any server command as /<command>, e.g. /kick, /map, /tpall, /votes, /announce, /msg, /msg-party, /msg-admins\n";
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
    // "/1".."/6": an answer to the running poll.
    const auto answer = [](std::string_view word) -> std::optional<std::size_t> {
        if (word.size() != 1 || word[0] < '1' || word[0] > '0' + static_cast<char>(multiplayer::max_vote_answers)) return {};
        return static_cast<std::size_t>(word[0] - '1');
    };
    if (const auto chosen = answer(verb)) return answer_poll(guest, *chosen);
    if (verb == "poll") {
        if (lower(trim(rest)) == "end") return end_poll(guest);
        return start_poll(guest, rest);
    }
    if (verb == "yes" || verb == "y") return cast_vote(guest, true);
    if (verb == "no" || verb == "n") return cast_vote(guest, false);
    if (verb == "vote") {
        const auto [what_text, argument] = split(rest);
        const auto what = lower(what_text);
        if (what == "yes" || what == "y") return cast_vote(guest, true);
        if (what == "no" || what == "n") return cast_vote(guest, false);
        if (const auto chosen = answer(what)) return answer_poll(guest, *chosen);
        if (what == "map") return start_vote(guest, VoteKind::map, argument);
        if (what == "kick") return start_vote(guest, VoteKind::kick, argument);
        if (what == "tod" || what == "time") return start_vote(guest, VoteKind::time, argument);
        if (what == "list") {
            if (custom.empty()) return reply(guest, "This server has no votes of its own.");
            std::string text;
            for (const auto &v : custom)
                text += "/vote " + v.name + (v.choices.empty() ? "" : " <" + joined(v.choices, "|") + ">") +
                        (v.description.empty() ? "" : ": " + v.description) + "\n";
            return reply(guest, text, static_cast<unsigned>(multiplayer::server_custom_vote_limit));
        }
        for (std::size_t i = 0; i < config_.votes.custom.size(); ++i)
            if (config_.votes.custom[i].name == what) return start_vote(guest, VoteKind::custom, argument, i);
        if (vote_) return reply(guest, running_vote_text());
        return reply(guest, enabled_votes() || !custom.empty() ? "Start one with /vote map, /vote kick, /vote tod or /vote list (see /help)."
                                                               : "This server has no player votes.");
    }
    // Admins run any server command from chat, as they do with "mp server". The caller
    // has already logged the line with any password hidden, so it is not logged again here.
    if (is_admin(guest.member.id)) {
        const auto id = guest.member.id;
        const auto answer_text = command(line, id);
        if (auto *still = find(id)) reply(*still, answer_text.empty() ? std::string("Done.") : answer_text);
        return;
    }
    reply(guest, "Unknown command /" + verb + ". Type /help for the list.");
}

std::string Host::running_vote_text() const {
    if (!vote_) return "No vote is running.";
    if (vote_->kind == VoteKind::poll)
        return "A poll is running: " + vote_->label + " Answer on the card at the right of your screen, or type /1 to /" +
               std::to_string(vote_->answers.size()) + ".";
    return "A vote is running: " + vote_->label + ". Type /yes or /no.";
}

void Host::start_vote(Guest &guest, VoteKind kind, std::string_view argument, std::size_t custom) {
    const auto &setting = vote_setting(kind, custom);
    const bool on = kind == VoteKind::custom ? setting.enabled : (enabled_votes() & vote_bit(kind)) != 0;
    if (!on)
        return reply(guest, kind == VoteKind::time && setting.enabled ? std::string("Time of day votes need world layer sync on the server.")
                            : kind == VoteKind::custom ? "That vote is off on this server."
                                                       : std::string(vote_name(kind)) + " votes are off on this server.");
    if (vote_) return reply(guest, running_vote_text());
    if (const auto wait = vote_cooldowns_.find(guest.member.id); wait != vote_cooldowns_.end() && now_ < wait->second)
        return reply(guest, "Wait " + std::to_string((wait->second - now_) / 1000000 + 1) + " s before starting another vote.");
    if (players() < setting.min_players)
        return reply(guest, "That vote needs " + std::to_string(setting.min_players) + " players on (" + std::to_string(players()) + " now).");
    Vote vote;
    vote.kind = kind;
    vote.custom = custom;
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
    case VoteKind::custom: {
        // The owner wrote the command; a player only picks one of its choices, never free text.
        const auto &v = config_.votes.custom[custom];
        auto choice = lower(trim(argument));
        if (v.choices.empty()) choice.clear();
        else if (std::find(v.choices.begin(), v.choices.end(), choice) == v.choices.end())
            return reply(guest, "/vote " + v.name + " <" + joined(v.choices, "|") + ">" + (v.description.empty() ? "" : ": " + v.description));
        vote.value = fill(fill(v.command, "{map}", config_.map), "{arg}", choice);
        vote.label = v.description.empty() ? v.name + (choice.empty() ? "" : " " + choice)
                                           : v.description + (choice.empty() ? "" : ": " + choice);
        // The card reads it as a question ("Reload the current map?"): it starts in lower case, as the others do.
        if (!vote.label.empty() && vote.label.front() >= 'A' && vote.label.front() <= 'Z') vote.label.front() = static_cast<char>(vote.label.front() + 32);
        break;
    }
    case VoteKind::poll: return start_poll(guest, argument);
    }
    if (config_.votes.starter_votes_yes) vote.yes.insert(guest.member.id);
    if (!++vote_ids_) ++vote_ids_;
    vote.id = vote_ids_;
    const auto seconds = setting.seconds ? setting.seconds : config_.votes.seconds;
    vote.ends = now_ + microseconds(seconds);
    vote_cooldowns_[guest.member.id] = now_ + microseconds(setting.cooldown ? setting.cooldown : config_.votes.cooldown);
    const auto label = vote.label;
    vote_ = std::move(vote);
    send_chat(guest_name(guest) + " started a vote to " + label + " (" + std::to_string(setting.percent) + "% needed, " +
              std::to_string(seconds) + " s). Vote on the card at the right of your screen, or type /yes or /no.");
    log_("[vote] " + guest_name(guest) + " started a vote to " + label + ".");
    check_vote(false);
}

void Host::start_poll(Guest &guest, std::string_view text) {
    using multiplayer::ServerPolls;
    const auto polls = enabled_polls();
    const bool admin = is_admin(guest.member.id);
    if (polls == ServerPolls::off) return reply(guest, "Polls are off on this server.");
    if (polls == ServerPolls::admins && !admin) return reply(guest, "Only admins can start a poll on this server.");
    if (vote_) return reply(guest, running_vote_text());
    if (const auto wait = vote_cooldowns_.find(guest.member.id); !admin && wait != vote_cooldowns_.end() && now_ < wait->second)
        return reply(guest, "Wait " + std::to_string((wait->second - now_) / 1000000 + 1) + " s before starting a poll.");
    // "<question> | <answer> | <answer>..."
    std::vector<std::string> parts;
    for (std::size_t at = 0; at <= text.size();) {
        const auto bar = text.find('|', at);
        auto part = clean_chat_text(trim(text.substr(at, bar == std::string_view::npos ? std::string_view::npos : bar - at)));
        cut_text(part, parts.empty() ? multiplayer::max_vote_label : multiplayer::max_vote_answer);
        if (!part.empty()) parts.push_back(std::move(part));
        if (bar == std::string_view::npos) break;
        at = bar + 1;
    }
    if (parts.size() < 3 || parts.size() > multiplayer::max_vote_answers + 1)
        return reply(guest, "/poll <question> | <answer> | <answer>... (2 to " + std::to_string(multiplayer::max_vote_answers) +
                                " answers), e.g. /poll Next map? | Grom | San Vansterdam");
    Vote poll;
    poll.kind = VoteKind::poll;
    poll.starter = guest.member.id;
    poll.label = parts.front();
    poll.answers.assign(parts.begin() + 1, parts.end());
    if (!++vote_ids_) ++vote_ids_;
    poll.id = vote_ids_;
    poll.ends = now_ + microseconds(config_.votes.poll_seconds);
    if (!admin) vote_cooldowns_[guest.member.id] = now_ + microseconds(config_.votes.cooldown);
    std::string choices;
    for (std::size_t i = 0; i < poll.answers.size(); ++i) choices += (i ? "  " : "") + std::string("/") + std::to_string(i + 1) + " " + poll.answers[i];
    vote_ = std::move(poll);
    send_chat(guest_name(guest) + " asks: " + vote_->label + " (" + std::to_string(config_.votes.poll_seconds) +
              " s). Answer on the card at the right of your screen, or type:");
    send_chat(choices);
    log_("[poll] " + guest_name(guest) + " started a poll: " + vote_->label + " " + joined(vote_->answers, " | "));
    check_vote(false);
}

void Host::cast_vote(Guest &guest, bool yes) {
    if (!vote_) return reply(guest, "No vote is running.");
    auto &vote = *vote_;
    if (vote.kind == VoteKind::poll) return reply(guest, running_vote_text());
    if (vote.kind == VoteKind::kick && guest.member.id == vote.target) return reply(guest, "You cannot vote on your own kick.");
    const bool changed = !(yes ? vote.yes : vote.no).contains(guest.member.id);
    vote.yes.erase(guest.member.id);
    vote.no.erase(guest.member.id);
    (yes ? vote.yes : vote.no).insert(guest.member.id);
    if (!changed) return reply(guest, yes ? "You already voted yes." : "You already voted no.");
    check_vote(false);
}

void Host::answer_poll(Guest &guest, std::size_t answer) {
    if (!vote_) return reply(guest, "No poll is running.");
    auto &poll = *vote_;
    if (poll.kind != VoteKind::poll) return reply(guest, running_vote_text());
    if (answer >= poll.answers.size()) return reply(guest, "Answer with /1 to /" + std::to_string(poll.answers.size()) + ".");
    const auto [chosen, added] = poll.chosen.try_emplace(guest.member.id, answer);
    if (!added && chosen->second == answer) return reply(guest, "You already answered " + poll.answers[answer] + ".");
    chosen->second = answer;
    check_vote(false);
}

void Host::end_poll(Guest &guest) {
    if (!vote_ || vote_->kind != VoteKind::poll) return reply(guest, "No poll is running.");
    if (vote_->starter != guest.member.id && !is_admin(guest.member.id))
        return reply(guest, "Only whoever started the poll, or an admin, can end it early.");
    check_vote(true);
}

std::set<std::uint64_t> Host::vote_voters(const Vote &vote) const {
    // Everyone connected may vote, except the player a kick vote is about.
    std::set<std::uint64_t> voters;
    for (const auto &[id, guest] : guests_)
        if (guest->handshaken && id != vote.target) voters.insert(id);
    return voters;
}
unsigned Host::votes_needed(const Vote &vote, unsigned voters) const {
    const auto percent = std::clamp(vote_setting(vote.kind, vote.custom).percent, 1U, 100U);
    // A kick needs a second player's yes: with only the starter and the target on, it fails.
    return std::max(vote.kind == VoteKind::kick ? 2U : 1U, (voters * percent + 99) / 100);
}
std::vector<unsigned> Host::poll_count(const Vote &poll) const {
    std::vector<unsigned> count(poll.answers.size());
    for (const auto &[voter, answer] : poll.chosen)
        if (answer < count.size()) ++count[answer];
    return count;
}

// What every game shows of the vote, with the next roster.
void Host::show_vote(const Vote &vote, std::uint8_t outcome) {
    const auto capped = [](unsigned value) { return static_cast<std::uint16_t>(std::min(value, 65535U)); };
    multiplayer::ServerVote shown;
    shown.id = vote.id;
    shown.kind = vote_bit(vote.kind);
    shown.outcome = outcome;
    if (vote.kind == VoteKind::poll) {
        shown.answers = vote.answers;
        for (const auto n : poll_count(vote)) shown.counts.push_back(capped(n));
    } else {
        shown.yes = capped(static_cast<unsigned>(vote.yes.size()));
        shown.no = capped(static_cast<unsigned>(vote.no.size()));
        shown.needed = capped(votes_needed(vote, static_cast<unsigned>(vote_voters(vote).size())));
    }
    shown.starter = vote.starter;
    shown.target = vote.target;
    shown.label = clean_chat_text(vote.label);
    cut_text(shown.label, multiplayer::max_vote_label);
    // A finished vote stays up a few seconds, to show how it ended; a poll's result a little longer.
    vote_shown_until_ = outcome == multiplayer::vote_running ? 0 : now_ + (vote.kind == VoteKind::poll ? 8000000 : 4000000);
    if (shown == vote_shown_) return;
    vote_shown_ = std::move(shown);
    roster_dirty_ = true;
}
void Host::cancel_vote(const std::string &why) {
    if (!vote_) return;
    const auto title = vote_title(vote_->kind, vote_->label);
    show_vote(*vote_, multiplayer::vote_cancelled);
    vote_.reset();
    send_chat(title + " was cancelled: " + why + ".");
    log_("[vote] " + title + " was cancelled: " + why + ".");
}

void Host::check_vote(bool expired) {
    if (!vote_) return;
    auto &vote = *vote_;
    const auto voters = vote_voters(vote);
    std::erase_if(vote.yes, [&](std::uint64_t id) { return !voters.contains(id); });
    std::erase_if(vote.no, [&](std::uint64_t id) { return !voters.contains(id); });
    std::erase_if(vote.chosen, [&](const auto &entry) { return !voters.contains(entry.first); });
    if (vote.kind == VoteKind::poll) {
        // Games show the count as it changes (the card); chat is not filled with it.
        if (!expired) return show_vote(vote, multiplayer::vote_running);
        const auto count = poll_count(vote);
        show_vote(vote, multiplayer::vote_passed); // "passed": it ran its course
        const auto done = std::move(vote);
        vote_.reset();
        const auto top = *std::max_element(count.begin(), count.end());
        std::vector<std::string> leaders;
        for (std::size_t i = 0; i < count.size(); ++i)
            if (top && count[i] == top) leaders.push_back(done.answers[i]);
        const auto outcome = !top ? std::string("nobody answered") : leaders.size() > 1 ? "a tie: " + joined(leaders, ", ")
                                                                                        : leaders.front() + " wins";
        const auto line = "Poll \"" + done.label + "\" ended: " + answer_count(done.answers, count) + " (" + outcome + ").";
        send_chat(line);
        log_("[poll] " + line);
        return;
    }
    if (vote.kind == VoteKind::kick && !find(vote.target)) return cancel_vote("the player left");
    const auto eligible = static_cast<unsigned>(voters.size());
    const auto needed = votes_needed(vote, eligible);
    const auto yes = static_cast<unsigned>(vote.yes.size()), no = static_cast<unsigned>(vote.no.size());
    const bool passed = yes >= needed;
    const bool lost = !passed && (expired || yes + (eligible - std::min(eligible, yes + no)) < needed);
    const auto tally = std::to_string(yes) + " yes, " + std::to_string(no) + " no, " + std::to_string(needed) + " needed";
    if (!passed && !lost) {
        // Games show the tally as it changes (the vote card); chat is not filled with it.
        show_vote(vote, multiplayer::vote_running);
        return;
    }
    show_vote(vote, passed ? multiplayer::vote_passed : multiplayer::vote_failed);
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
    case VoteKind::custom: log_(command(done.value)); break; // the owner's command, as the console
    case VoteKind::kick:
        if (find(done.target)) {
            kicked_.insert(done.target);
            drop(done.target, "You were kicked from this server by a vote.");
        }
        break;
    case VoteKind::poll: break;
    }
}

std::pair<std::string, bool> Host::votes_command(std::string_view argument) {
    // votes | votes <vote> on|off|<percent>|seconds <n>|cooldown <n>|min-players <n> | votes seconds|cooldown <n>
    // | votes polls off|admins|everyone | votes poll-seconds <n> | votes starter-yes on|off
    const auto [what_text, rest] = split(argument);
    const auto what = lower(what_text);
    const auto describe = [&](const std::string &label, const VoteSetting &v) {
        if (!v.enabled) return label + ": off";
        auto text = label + ": on, " + std::to_string(v.percent) + "% to pass";
        if (v.seconds) text += ", runs " + std::to_string(v.seconds) + " s";
        if (v.cooldown) text += ", " + std::to_string(v.cooldown) + " s cooldown";
        if (v.min_players > 1) text += ", needs " + std::to_string(v.min_players) + " players on";
        return text;
    };
    if (what.empty()) {
        auto text = describe("map votes", config_.votes.map) + "\n" + describe("kick votes", config_.votes.kick) + "\n" +
                    describe("time of day votes", config_.votes.time) + (config_.world_layer_sync ? "" : " (needs layer-sync on)");
        for (const auto &v : config_.votes.custom)
            text += "\n" + describe("/vote " + v.name + (v.choices.empty() ? "" : " <" + joined(v.choices, "|") + ">"), v.setting) +
                    " -> " + v.command;
        text += "\npolls: " + config_.votes.polls + ", run " + std::to_string(config_.votes.poll_seconds) + " s";
        text += "\nvotes last " + std::to_string(config_.votes.seconds) + " s; a player waits " + std::to_string(config_.votes.cooldown) +
                " s between votes; the starter " + (config_.votes.starter_votes_yes ? "votes yes" : "does not vote");
        if (vote_) text += "\nrunning: " + vote_title(vote_->kind, vote_->label);
        return {text, false};
    }
    const auto value = lower(trim(rest));
    if (what == "seconds" || what == "cooldown") {
        const auto n = number(value);
        const bool seconds = what == "seconds";
        if (!n || (seconds ? *n < 10 || *n > 300 : *n > 3600)) return {seconds ? "votes seconds <10-300>" : "votes cooldown <0-3600>", false};
        (seconds ? config_.votes.seconds : config_.votes.cooldown) = static_cast<unsigned>(*n);
        return {seconds ? "Votes now last " + std::to_string(*n) + " s." : "Players now wait " + std::to_string(*n) + " s between votes.", true};
    }
    if (what == "polls") {
        if (value != "off" && value != "admins" && value != "everyone") return {"votes polls off|admins|everyone (now " + config_.votes.polls + ")", false};
        config_.votes.polls = value;
        return {value == "off" ? "Polls are off." : value == "admins" ? "Admins can start polls." : "Everyone can start polls.", true};
    }
    if (what == "poll-seconds") {
        const auto n = number(value);
        if (!n || *n < 10 || *n > 600) return {"votes poll-seconds <10-600>", false};
        config_.votes.poll_seconds = static_cast<unsigned>(*n);
        return {"Polls now run " + std::to_string(*n) + " s.", true};
    }
    if (what == "starter-yes") {
        const auto toggle = on_off(value);
        if (!toggle) return {"votes starter-yes on|off", false};
        config_.votes.starter_votes_yes = *toggle;
        return {*toggle ? "Whoever starts a vote votes yes." : "Whoever starts a vote still has to vote.", true};
    }
    // One vote's settings.
    VoteKind kind = VoteKind::map;
    std::size_t custom{};
    std::string label;
    if (what == "map") label = "Map votes";
    else if (what == "kick") kind = VoteKind::kick, label = "Kick votes";
    else if (what == "tod" || what == "time") kind = VoteKind::time, label = "Time of day votes";
    else {
        kind = VoteKind::custom;
        while (custom < config_.votes.custom.size() && config_.votes.custom[custom].name != what) ++custom;
        if (custom == config_.votes.custom.size())
            return {"votes [<vote> on|off|<percent>|seconds <n>|cooldown <n>|min-players <n>] (map, kick, tod or a custom vote)\n"
                    "votes seconds|cooldown <n> | votes polls off|admins|everyone | votes poll-seconds <n> | votes starter-yes on|off",
                    false};
        label = "\"/vote " + what + "\" votes";
    }
    auto &setting = kind == VoteKind::map ? config_.votes.map : kind == VoteKind::kick ? config_.votes.kick
                  : kind == VoteKind::time ? config_.votes.time : config_.votes.custom[custom].setting;
    const auto [option_text, n_text] = split(value);
    const std::string option(option_text);
    if (option == "seconds" || option == "cooldown" || option == "min-players") {
        const auto n = number(n_text);
        if (option == "seconds") {
            if (!n || (*n && (*n < 10 || *n > 300))) return {"votes " + what + " seconds <10-300> (0: votes seconds)", false};
            setting.seconds = static_cast<unsigned>(*n);
            return {label + (*n ? " now last " + std::to_string(*n) + " s." : " now last as long as the others."), true};
        }
        if (option == "cooldown") {
            if (!n || *n > 3600) return {"votes " + what + " cooldown <1-3600> (0: votes cooldown)", false};
            setting.cooldown = static_cast<unsigned>(*n);
            return {label + (*n ? ": the starter now waits " + std::to_string(*n) + " s before another." : " now use the votes' cooldown."), true};
        }
        if (!n || *n < 1 || *n >= multiplayer::max_players)
            return {"votes " + what + " min-players <1-" + std::to_string(multiplayer::max_players - 1) + ">", false};
        setting.min_players = static_cast<unsigned>(*n);
        return {label + " now need " + std::to_string(*n) + " players on to start.", true};
    }
    if (const auto toggle = on_off(option)) {
        setting.enabled = *toggle;
        if (!*toggle && vote_ && vote_->kind == kind && vote_->custom == custom) cancel_vote("that vote was switched off");
        return {label + (*toggle ? " are on (" + std::to_string(setting.percent) + "% to pass)." : " are off."), true};
    }
    const auto percent = number(option.ends_with("%") ? std::string_view(option).substr(0, option.size() - 1) : std::string_view(option));
    if (!percent || *percent < 1 || *percent > 100)
        return {"votes " + what + " on|off|<1-100>|seconds <n>|cooldown <n>|min-players <n>", false};
    setting.percent = static_cast<unsigned>(*percent);
    return {label + " now need " + std::to_string(*percent) + "% to pass.", true};
}

void Host::announce(std::string_view text) {
    const auto line = clean_chat_text(text);
    if (line.empty()) return;
    send_chat(line);
    log_("[announcement] " + line);
    if (!config_.announcements.card) return;
    // Up long enough to read: longer text stays longer.
    const auto seconds = static_cast<std::uint16_t>(std::clamp<std::size_t>(5 + line.size() / 20, 6, 15));
    if (!++announcement_ids_) ++announcement_ids_;
    announcement_ = {announcement_ids_, seconds, line};
    announcement_until_ = now_ + microseconds(seconds);
    roster_dirty_ = true;
}
void Host::tick_announcements() { // the timer waits while nobody is on
    if (announcement_.id && now_ >= announcement_until_) {
        announcement_ = {};
        roster_dirty_ = true;
    }
    const auto &a = config_.announcements;
    if (!a.interval || a.messages.empty() || !players()) {
        announced_at_ = now_;
        return;
    }
    if (now_ - announced_at_ < std::uint64_t{a.interval} * 60000000) return;
    announced_at_ = now_;
    next_announcement_ %= a.messages.size();
    announce(a.messages[next_announcement_++]);
}
std::pair<std::string, bool> Host::announcements_command(std::string_view argument) {
    auto &a = config_.announcements;
    const auto [what_text, rest] = split(argument);
    const auto what = lower(what_text);
    const auto value = trim(rest);
    if (what.empty() || what == "list") {
        std::string text = a.messages.empty() ? "No announcements. Add one with: announcements add <text>"
                         : "Announcements" + (a.interval ? ", one every " + std::to_string(a.interval) + " min" : std::string(" (timer off)")) +
                               ", card " + (a.card ? "on" : "off") + ":";
        for (std::size_t i = 0; i < a.messages.size(); ++i) text += "\n" + std::to_string(i + 1) + ". " + a.messages[i];
        return {text, false};
    }
    if (what == "add") {
        const auto line = clean_chat_text(value);
        if (line.empty()) return {"announcements add <text>", false};
        if (a.messages.size() >= max_announcements) return {"There are already " + std::to_string(max_announcements) + " announcements.", false};
        a.messages.push_back(line);
        return {"Announcement " + std::to_string(a.messages.size()) + " added" +
                    (a.interval ? "." : ". The timer is off: announcements interval <minutes>."), true};
    }
    if (what == "remove") {
        const auto n = number(value);
        if (!n || !*n || *n > a.messages.size()) return {"announcements remove <1-" + std::to_string(a.messages.size()) + ">", false};
        a.messages.erase(a.messages.begin() + static_cast<std::ptrdiff_t>(*n - 1));
        return {"Announcement " + std::to_string(*n) + " removed.", true};
    }
    if (what == "clear") {
        a.messages.clear();
        return {"Announcements cleared.", true};
    }
    if (what == "interval") {
        const auto n = lower(value) == "off" ? std::optional<std::uint64_t>(0) : number(value);
        if (!n || *n > max_announcement_interval) return {"announcements interval <1-" + std::to_string(max_announcement_interval) + ">|off", false};
        a.interval = static_cast<unsigned>(*n);
        announced_at_ = now_;
        return {*n ? "An announcement every " + std::to_string(*n) + " min." : std::string("The announcement timer is off."), true};
    }
    if (what == "card") {
        const auto toggle = on_off(lower(value));
        if (!toggle) return {"announcements card on|off", false};
        a.card = *toggle;
        return {*toggle ? "Announcements also show as a card on each player's screen." : "Announcements show in chat only.", true};
    }
    return {"announcements [list | add <text> | remove <n> | clear | interval <minutes>|off | card on|off]", false};
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
