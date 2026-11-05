#include "exchange.h"
#include <algorithm>
#include <stdexcept>
#include <variant>

namespace dingosdk::multiplayer::synchros {

Exchange::Exchange(Settings& settings, Cache& cache, Closet& closet, Channel& channel, Game game, std::uint64_t me)
    : settings_(settings), cache_(cache), closet_(closet), channel_(channel), game_(std::move(game)), me_(me) {}

void Exchange::note(const std::string& line) const {
    if (game_.note) game_.note("Skate Synchros: " + line);
}

void Exchange::send(std::uint64_t peer, const Message& message) {
    try {
        const auto bytes = encode(message);
        if (!channel_.send(peer, bytes)) note("a message to " + std::to_string(peer) + " could not be sent");
    } catch (const std::exception& failure) {
        note(std::string("a message could not be written: ") + failure.what());
    }
}

void Exchange::refuse(std::uint64_t peer, const std::string& why) {
    send(peer, Message{Refuse{why.substr(0, max_reason)}});
    auto& p = peers_[peer];
    p.refused = why;
    p.outfit.reset();
    p.welcomed = p.sharing = false;
    p.arriving.clear();
    p.expected.clear();
    p.to_fetch.clear();
    note("stopped with " + std::to_string(peer) + ": " + why);
    channel_.close(peer);
}

void Exchange::begin(std::uint64_t session) {
    session_ = session;
    grants_ = {session, {}};
    peers_.clear();
}

// A Hello with the proofs of every code and group held now. Sent when the side connection comes
// up, and again whenever the codes held change, so a code entered mid-session counts at once.
void Exchange::hello(std::uint64_t peer, Peer& p, bool again) {
    if (!p.linked || !p.refused.empty() || !may_ask(settings_, peer)) return;
    const auto proofs = proofs_for(settings_, session_, peer, me_);
    const Message message{Hello{session_, proofs.personal, proofs.group}};
    std::vector<std::uint8_t> bytes;
    try {
        bytes = encode(message);
    } catch (const std::exception&) {
        return;
    }
    if (p.hello_sent && (!again || bytes == p.asked_with)) return;
    send(peer, message);
    note("asked " + std::to_string(peer) + " for their outfit (" + std::to_string(proofs.personal.size()) + " code(s), " +
         std::to_string(proofs.group.size()) + " group(s))");
    p.hello_sent = true;
    p.asked_with = std::move(bytes);
}

// Whether a viewer may see this player's outfit, from the proofs it last sent: told to them when
// it changes, and an outfit made ready for them when it becomes yes.
void Exchange::judge(std::uint64_t peer, Peer& p) {
    const auto paired_before = settings_.paired.size();
    accept_proofs(settings_, grants_, p.proofs, session_, me_, peer);
    if (settings_.paired.size() != paired_before) settings_changed_ = true;
    const bool now = may_send(settings_, grants_, peer);
    const bool changed = now != p.sharing || !p.hello_seen;
    p.sharing = now;
    p.hello_seen = true;
    if (!changed) return;
    send(peer, Message{Welcome{p.sharing}});
    if (!p.sharing) note("not sharing this player's outfit with " + std::to_string(peer) + " (no matching code or group)");
    if (p.sharing) {
        std::set<std::string> worn;
        if (game_.wearing) worn = game_.wearing();
        p.outfit = std::make_unique<Outfit>(closet_, std::move(worn), game_.game_item);
        note("sharing this player's outfit with " + std::to_string(peer));
    } else {
        p.outfit.reset();
    }
}

void Exchange::connected(std::uint64_t peer) {
    auto& p = peers_[peer];
    p.linked = true;
    hello(peer, p, false);
}

void Exchange::settings_changed() {
    for (auto& [peer, p] : peers_) {
        if (!p.linked || !p.refused.empty()) continue;
        if (p.hello_seen) judge(peer, p);   // sharing switched, a group left, a player removed
        hello(peer, p, true);               // a code entered, a group joined, showing switched on
    }
}

void Exchange::closed(std::uint64_t peer) {
    // What arrived whole stays in the cache; what was half-way is asked for again next time.
    peers_.erase(peer);
}

void Exchange::blocked(std::uint64_t peer) {
    block(settings_, grants_, peer);
    if (peers_.contains(peer)) refuse(peer, "blocked");
    peers_.erase(peer);
    cache_.forget(peer);
    settings_changed_ = true;
}

void Exchange::ask(std::uint64_t peer, Peer& p) {
    if (!p.welcomed || !game_.worn_by) return;
    Want want;
    for (const auto& key : game_.worn_by(peer)) {
        if (want.items.size() >= max_items) break;
        if (game_.can_show && game_.can_show(key)) continue;   // this PC has it already
        want.items.push_back(key);
    }
    std::set<std::string> now(want.items.begin(), want.items.end());
    if (now == p.asked) return;
    p.asked = std::move(now);
    p.ready_told = false;
    if (want.items.empty()) return;
    send(peer, Message{want});
}

void Exchange::fetch_more(std::uint64_t peer, Peer& p) {
    // One batch in flight at a time: the next is asked for when this one has arrived.
    if (!p.arriving.empty() || p.to_fetch.empty()) return;
    Fetch fetch;
    while (!p.to_fetch.empty() && fetch.files.size() < fetch_batch) {
        const auto id = p.to_fetch.back();
        p.to_fetch.pop_back();
        if (cache_.has(id)) continue;
        const auto expected = p.expected.find(id);
        if (expected == p.expected.end()) continue;
        fetch.files.push_back(id);
        p.arriving.emplace(id, Arriving(expected->second));
    }
    if (!fetch.files.empty()) send(peer, Message{fetch});
    else check_ready(peer, p);
}

void Exchange::check_ready(std::uint64_t peer, Peer& p) {
    if (p.ready_told || !p.arriving.empty() || !p.to_fetch.empty() || p.listing.empty()) return;
    if (!cache_.missing(peer).empty()) return;
    p.ready_told = true;
    note(std::to_string(peer) + "'s outfit is here (" + std::to_string(p.listing.size()) + " items)");
    if (game_.outfit_ready) game_.outfit_ready(peer);
}

void Exchange::received(std::uint64_t peer, std::span<const std::uint8_t> bytes) {
    auto& p = peers_[peer];
    if (!p.refused.empty()) return;
    const auto message = decode(bytes);
    if (!message) {
        refuse(peer, "a message could not be read");
        return;
    }
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, Hello>) {
            // Wearer: a viewer offering its proofs (again, when its codes changed).
            if (m.session != session_) { refuse(peer, "a hello for another session"); return; }
            p.proofs = Proofs{m.personal, m.group};
            const bool first = !p.hello_seen;
            const bool was = p.sharing;
            judge(peer, p);
            // A viewer asking again is told again even when the answer is the same.
            if (!first && was == p.sharing) send(peer, Message{Welcome{p.sharing}});
        } else if constexpr (std::is_same_v<T, Welcome>) {
            // Viewer: whether they let this player in (again, when either side's settings changed).
            if (!p.hello_sent) { refuse(peer, "an unexpected welcome"); return; }
            const bool was = p.welcomed;
            p.welcomed = m.allowed;
            if (p.welcomed && !was) {
                p.asked.clear();   // ask for everything they wear
                note(std::to_string(peer) + " shares their outfit with this player");
            }
            if (!p.welcomed) note(std::to_string(peer) + " does not share their outfit with this player");
            if (p.welcomed) ask(peer, p);
        } else if constexpr (std::is_same_v<T, Want>) {
            if (!p.outfit || !p.sharing) { refuse(peer, "asked without being let in"); return; }
            for (const auto& contents : p.outfit->answer(m)) send(peer, Message{contents});
        } else if constexpr (std::is_same_v<T, Contents>) {
            if (!p.welcomed) { refuse(peer, "contents without a welcome"); return; }
            // Only what was asked for, and only items that came whole.
            for (const auto& item : m.items) {
                if (!p.asked.contains(item.item) || item.withheld != Withheld::none) continue;
                std::erase_if(p.listing, [&](const ItemContents& old) { return old.item == item.item; });
                p.listing.push_back(item);
                for (const auto& file : item.files) {
                    p.expected.insert_or_assign(file.id, file);
                    if (!cache_.has(file.id) && !p.arriving.contains(file.id) &&
                        std::find(p.to_fetch.begin(), p.to_fetch.end(), file.id) == p.to_fetch.end())
                        p.to_fetch.push_back(file.id);
                }
            }
            // Only items they still have on are kept.
            std::erase_if(p.listing, [&](const ItemContents& item) { return !p.asked.contains(item.item); });
            cache_.remember(peer, p.listing);
            fetch_more(peer, p);
            check_ready(peer, p);
        } else if constexpr (std::is_same_v<T, Fetch>) {
            if (!p.outfit || !p.outfit->fetch(m)) { refuse(peer, "asked for a file it was not offered"); return; }
        } else if constexpr (std::is_same_v<T, Piece>) {
            const auto found = p.arriving.find(m.id);
            if (found == p.arriving.end()) { refuse(peer, "sent a file that was not asked for"); return; }
            switch (found->second.take(m)) {
            case Arriving::Took::more: break;
            case Arriving::Took::refused: refuse(peer, "sent a file that does not match"); return;
            case Arriving::Took::whole: {
                const auto file = found->second.release();
                p.bytes_received += file.size();
                if (!cache_.put(m.id, file)) note("a received file could not be kept");
                p.arriving.erase(found);
                fetch_more(peer, p);
                check_ready(peer, p);
                break;
            }
            }
        } else if constexpr (std::is_same_v<T, Refuse>) {
            p.refused = m.reason.empty() ? "refused" : m.reason;
            p.outfit.reset();
            p.welcomed = p.sharing = false;
            note(std::to_string(peer) + " stopped: " + p.refused);
        }
    }, *message);
}

void Exchange::tick() {
    for (auto& [peer, p] : peers_) {
        if (!p.outfit || !p.refused.empty()) continue;
        // Pieces go while the side connection's queue has room: Steam paces them from there.
        for (int sent = 0; sent < 64; ++sent) {
            const auto queued = channel_.pending(peer);
            if (queued < 0 || queued > queue_limit) break;
            const auto piece = p.outfit->next();
            if (!piece) break;
            send(peer, Message{*piece});
        }
    }
}

void Exchange::outfit_changed(std::uint64_t peer) {
    const auto found = peers_.find(peer);
    if (found != peers_.end() && found->second.refused.empty()) ask(peer, found->second);
}

void Exchange::own_outfit_changed() {
    if (!game_.wearing) return;
    const auto worn = game_.wearing();
    for (auto& [peer, p] : peers_)
        if (p.outfit) p.outfit->wearing(worn);
}

Exchange::Status Exchange::status(std::uint64_t peer) const {
    Status s;
    const auto found = peers_.find(peer);
    if (found == peers_.end()) return s;
    const auto& p = found->second;
    s.welcomed = p.welcomed;
    s.sharing = p.sharing;
    s.items = p.listing.size();
    s.files_missing = p.to_fetch.size() + p.arriving.size();
    s.bytes_received = p.bytes_received;
    s.refused = p.refused;
    return s;
}

bool Exchange::take_settings_changed() noexcept {
    const bool changed = settings_changed_;
    settings_changed_ = false;
    return changed;
}

} // namespace dingosdk::multiplayer::synchros
