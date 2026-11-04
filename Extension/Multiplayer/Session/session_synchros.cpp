#include "session_internal.h"
#include "Extension/Multiplayer/Synchros/synchros_runtime.h"

namespace dingosdk::multiplayer::session_detail {
namespace {
std::set<std::string> worn_items(const Appearance& appearance) {
    std::set<std::string> keys;
    for (const auto* recipe : {&appearance.skater, &appearance.board})
        for (const auto& item : recipe->items)
            if (!item.asset.empty()) keys.insert(item.asset);
    return keys;
}
} // namespace

// Skate Synchros (Extension/Multiplayer/Synchros): who is in the session and what they wear, each
// update. It runs on its own side connections, never on the session's links.
void update_synchros(Session& s, std::uint64_t) {
    namespace synchros = dingosdk::multiplayer::synchros;
    synchros::SessionView view;
    view.playing = s.mode == Mode::host || s.mode == Mode::join;
    view.session = s.secret;
    view.me = s.transport.status().local_id;
    if (view.playing && view.me) {
        for (const auto& peer : active_peers(s)) {
            if (!peer.handshaken || !peer.member.id || peer.member.id == view.me) continue;
            // A dedicated server is no player.
            if (dedicated_host(s) && peer.member.id == s.host_id) continue;
            synchros::SessionView::Player player{peer.member.id, peer.cosmetic_revision, {}};
            if (const auto& appearance = peer.appearance.value()) player.worn = worn_items(*appearance);
            view.players.push_back(std::move(player));
        }
        if (s.sent_appearance) {
            view.wearing = worn_items(*s.sent_appearance);
            std::uint64_t hash = 1469598103934665603ULL;
            for (const auto& key : view.wearing)
                for (const unsigned char c : key) hash = (hash ^ c) * 1099511628211ULL;
            view.own_revision = hash;
        }
    }
    synchros::tick(s.transport, view);
}
} // namespace dingosdk::multiplayer::session_detail
