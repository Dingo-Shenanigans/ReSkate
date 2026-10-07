#include "native_party_internal.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Session/session.h"
#include <charconv>
#include <chrono>
#include <map>
#include <mutex>
#include <cstring>
#include <format>

// The game's own party buttons (Social menu, player cards, invites) end in a handful of request
// natives that need a native Blaze game group, which ReSkate never has. While the SDK owns the
// party, these hooks turn each request into a ReSkate party request (the session's "party"
// command, which whoever hosts answers) and complete the button's callback themselves.
// Party queries are answered from the published roster (analysis/party-re/native-core.md).
namespace dingosdk::multiplayer {
using namespace native_party_detail;
namespace {
using Delegate = Address;
struct Requests {
    Address (*invite)(Address, const std::uint64_t *, Delegate){};
    Address (*join_group)(Address, std::uint64_t, std::int32_t, Delegate){};
    Address (*join_player)(Address, const std::uint64_t *, Delegate){};
    Address (*decline)(Address, const Address *, Delegate){};
    Address (*leave)(Address, Delegate){};
    Address (*kick)(Address, const std::uint64_t *, Delegate){};
    Address (*promote)(Address, const std::uint64_t *, Delegate){};
    void (*privacy)(std::uint32_t, std::uint32_t){};
    void (*local_is_leader)(bool *){};
    bool (*in_group)(){};
    void (*group_status)(const std::uint64_t *, void *, bool *){};
    Address (*accept_invite)(Address, const Address *, const Address *, Delegate){};
    bool (*voice_unavailable)(){};
    std::atomic<bool> installed{};
    // Accepting an invite also clears it (DeleteInvite): that is no decline.
    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> accepted; // inviter -> when (ms)
    std::map<std::uint64_t, std::uint64_t> invited;  // player -> when the local player invited them (ms)
};
Requests &requests() { static auto *value = new Requests; return *value; }
std::atomic<bool> changes_allowed{};
constexpr const char *lobby_party = "Parties need a multiplayer session.";
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// The SDK answers only while it owns the party: a session roster is published and the game
// has no native group of its own.
std::shared_ptr<const Published> owned_party() {
    auto &s = state();
    if (!s.installed.load(std::memory_order_acquire) || !s.roster_active.load(std::memory_order_acquire)) return {};
    const auto native_client = read<Address>(s.base + party::party_client);
    if (native_client && read<Address>(native_client + 0x70)) return {};
    return s.published.load(std::memory_order_acquire);
}
const Record *local_record(const Published &p) {
    for (const auto &record : p.records)
        if (record.handle && record.player.local) return &record;
    return nullptr;
}
// A callback to answer: some requests pass an empty delegate (the invite toast's reject passes
// none), which the natives skip ([delegate] == 0) and so must we.
bool has_callback(Delegate callback) {
    Address delegate{};
    return callback && memory::peek(callback, delegate) && delegate;
}
// Answers the button's callback now: code 0 is success; anything else shows as a failure.
Address complete(Address out, Delegate callback, std::uint32_t code = 0, const std::string &text = {}) {
    const auto base = state().base;
    alignas(16) std::array<std::byte, 0x40> result{};
    reinterpret_cast<void (*)(void *, std::uint32_t, const char *)>(base + party::result_make)(result.data(), code, text.c_str());
    if (has_callback(callback)) reinterpret_cast<void (*)(Delegate, void *)>(base + party::delegate_invoke)(callback, result.data());
    reinterpret_cast<void (*)(void *)>(base + engine::native_text_release)(result.data());
    if (out) reinterpret_cast<void (*)(Address, Address)>(base + party::null_handle)(out, 0);
    return out;
}
// A code the Social callbacks treat as "no current group" (0xd5e8a544) with our own text.
constexpr std::uint32_t refused = 0xd5e8a544;
bool send(std::string_view verb, std::uint64_t player = 0) {
    std::string argument(verb);
    if (player) argument += " " + std::to_string(player);
    const bool queued = queue_command("party", argument, {});
    if (queued) logging::log(logging::Level::info, logging::Channel::ui, "Native party request: party {}.", argument);
    return queued;
}
bool session_player(const Published &p, std::uint64_t id) {
    const auto record = p.record(id);
    return record && !record->player.local;
}

Address invite_hook(Address out, const std::uint64_t *eaid, Delegate callback) {
    try {
        if (const auto p = owned_party(); p && eaid && session_player(*p, *eaid)) {
            if (!changes_allowed.load(std::memory_order_acquire)) return complete(out, callback, refused, lobby_party);
            {
                std::lock_guard lock(requests().mutex);
                requests().invited[*eaid] = now_ms();
            }
            if (!send("invite", *eaid)) return complete(out, callback, refused, "Could not send the invite.");
            return complete(out, callback);
        }
    } catch (...) {}
    return requests().invite(out, eaid, callback);
}
// Accepting an invite (reason 2) names the group the invite carried, which the SDK sets to the
// inviter's Steam ID; a direct join (1) names the party's leader the same way.
// The session player a group id names: an invite's GameId is the inviter's Steam ID; a party's
// group id (native_party_group_id) names that party, joined through its leader.
std::uint64_t group_player(const Published &p, std::uint64_t group) {
    if (session_player(p, group)) return group;
    for (const auto &record : p.records)
        if (record.handle && !record.player.local && record.group.id == group && record.group.leader_id &&
            session_player(p, record.group.leader_id))
            return record.group.leader_id;
    return 0;
}
bool accepted_recently(std::uint64_t inviter) {
    auto &r = requests();
    std::lock_guard lock(r.mutex);
    const auto it = r.accepted.find(inviter);
    return it != r.accepted.end() && now_ms() - it->second < 10000;
}
Address join_group_hook(Address out, std::uint64_t group, std::int32_t reason, Delegate callback) {
    try {
        const auto p = owned_party();
        if (const auto player = p ? group_player(*p, group) : 0) {
            if (!changes_allowed.load(std::memory_order_acquire)) return complete(out, callback, refused, lobby_party);
            // The toast's Accept already asked the server (accept_invite_hook); this is its follow-up.
            if (reason == 2 && accepted_recently(player)) return complete(out, callback);
            if (reason == 2) {
                std::lock_guard lock(requests().mutex);
                requests().accepted[player] = now_ms();
            }
            if (!send(reason == 2 ? "accept" : "join", player)) return complete(out, callback, refused, "Could not reach the server.");
            return complete(out, callback);
        }
    } catch (...) {}
    return requests().join_group(out, group, reason, callback);
}
std::uint64_t string_id(const Address *text) {
    Address chars{};
    if (!text || !memory::peek(reinterpret_cast<Address>(text), chars) || !chars) return 0;
    std::array<char, 32> buffer{};
    memory::peek_bytes(chars, buffer.data(), buffer.size() - 1);
    const std::string_view value(buffer.data());
    std::uint64_t id{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? id : 0;
}
// The invite toast's Accept: (Handle*, invite id, game id, Delegate(error, game id)). Both ids are
// the inviter's Steam ID (post_native_party_invite).
Address accept_invite_hook(Address out, const Address *invite, const Address *game, Delegate callback) {
    try {
        const auto p = owned_party();
        const auto inviter = string_id(invite);
        if (p && inviter && session_player(*p, inviter)) {
            const auto base = state().base;
            alignas(16) std::array<std::byte, 0x40> result{};
            const bool allowed = changes_allowed.load(std::memory_order_acquire);
            bool sent{};
            if (allowed) {
                {
                    std::lock_guard lock(requests().mutex);
                    requests().accepted[inviter] = now_ms();
                }
                sent = send("accept", inviter);
            }
            reinterpret_cast<void (*)(void *, std::uint32_t, const char *)>(base + party::result_make)(
                result.data(), sent ? 0 : refused, sent ? "" : allowed ? "Could not reach the server." : lobby_party);
            if (has_callback(callback))
                reinterpret_cast<void (*)(Delegate, void *, const Address *)>(base + party::delegate_invoke_text)(
                    callback, result.data(), game);
            reinterpret_cast<void (*)(void *)>(base + engine::native_text_release)(result.data());
            if (out) reinterpret_cast<void (*)(Address, Address)>(base + party::null_handle)(out, 0);
            return out;
        }
    } catch (...) {}
    return requests().accept_invite(out, invite, game, callback);
}
// Party voice options show while the local player is in a ReSkate party.
bool voice_unavailable_hook() {
    const bool native = requests().voice_unavailable();
    try {
        if (native)
            if (const auto p = owned_party()) {
                const auto local = local_record(*p);
                if (local && local->player.member && local->group.members > 1) return false;
            }
    } catch (...) {}
    return native;
}
Address join_player_hook(Address out, const std::uint64_t *eaid, Delegate callback) {
    try {
        if (const auto p = owned_party(); p && eaid && session_player(*p, *eaid)) {
            if (!changes_allowed.load(std::memory_order_acquire)) return complete(out, callback, refused, lobby_party);
            if (!send("join", *eaid)) return complete(out, callback, refused, "Could not reach the server.");
            return complete(out, callback);
        }
    } catch (...) {}
    return requests().join_player(out, eaid, callback);
}
Address decline_hook(Address out, const Address *invite, Delegate callback) {
    try {
        Address text{};
        if (const auto p = owned_party(); p && invite && memory::peek(reinterpret_cast<Address>(invite), text) && text) {
            std::array<char, 32> chars{};
            memory::peek_bytes(text, chars.data(), chars.size() - 1);
            std::uint64_t from{};
            const std::string_view id(chars.data());
            const auto parsed = std::from_chars(id.data(), id.data() + id.size(), from);
            if (parsed.ec == std::errc{} && parsed.ptr == id.data() + id.size() && session_player(*p, from)) {
                bool just_accepted{};
                {
                    auto &r = requests();
                    std::lock_guard lock(r.mutex);
                    const auto it = r.accepted.find(from);
                    just_accepted = it != r.accepted.end() && now_ms() - it->second < 10000;
                    if (it != r.accepted.end()) r.accepted.erase(it);
                }
                if (!just_accepted) send("decline", from);
                return complete(out, callback);
            }
        }
    } catch (...) {}
    return requests().decline(out, invite, callback);
}
Address leave_hook(Address out, Delegate callback) {
    try {
        if (const auto p = owned_party()) {
            const auto local = local_record(*p);
            if (!local || !local->player.member) return complete(out, callback, refused, "You're not in a party.");
            if (!changes_allowed.load(std::memory_order_acquire)) return complete(out, callback, refused, lobby_party);
            if (!send("leave")) return complete(out, callback, refused, "Could not reach the server.");
            return complete(out, callback);
        }
    } catch (...) {}
    return requests().leave(out, callback);
}
Address kick_hook(Address out, const std::uint64_t *eaid, Delegate callback) {
    try {
        if (const auto p = owned_party(); p && eaid && session_player(*p, *eaid)) {
            if (!changes_allowed.load(std::memory_order_acquire)) return complete(out, callback, refused, lobby_party);
            if (!send("kick", *eaid)) return complete(out, callback, refused, "Could not reach the server.");
            return complete(out, callback);
        }
    } catch (...) {}
    return requests().kick(out, eaid, callback);
}
Address promote_hook(Address out, const std::uint64_t *eaid, Delegate callback) {
    try {
        if (const auto p = owned_party(); p && eaid && session_player(*p, *eaid)) {
            if (!changes_allowed.load(std::memory_order_acquire)) return complete(out, callback, refused, lobby_party);
            if (!send("promote", *eaid)) return complete(out, callback, refused, "Could not reach the server.");
            return complete(out, callback);
        }
    } catch (...) {}
    return requests().promote(out, eaid, callback);
}
// Settings 4 (who may invite) and 5 (who may join directly): "Anyone" joining opens the party,
// anything else makes it invite-only. The game keeps its own copy of the setting too.
void privacy_hook(std::uint32_t setting, std::uint32_t value) {
    requests().privacy(setting, value);
    try {
        if (setting != 5) return;
        const auto p = owned_party();
        const auto local = p ? local_record(*p) : nullptr;
        if (local && local->player.member && local->player.leader && changes_allowed.load(std::memory_order_acquire))
            send((value & 1) ? "open" : "close");
    } catch (...) {}
}
void local_is_leader_hook(bool *out) {
    requests().local_is_leader(out);
    try {
        if (!out || *out) return;
        if (const auto p = owned_party()) {
            const auto local = local_record(*p);
            *out = local && local->player.member && local->player.leader;
        }
    } catch (...) {}
}
bool in_group_hook() {
    if (requests().in_group()) return true;
    try {
        if (const auto p = owned_party()) {
            const auto local = local_record(*p);
            return local && local->player.member;
        }
    } catch (...) {}
    return false;
}
// A player's GameGroupStatus (0x28 bytes: +8 group id, +0x10 member count, +0x11 limit,
// +0x12 is leader, +0x18 leader id): for a session player, their party as the roster says.
void group_status_hook(const std::uint64_t *eaid, void *out, bool *found) {
    requests().group_status(eaid, out, found);
    try {
        if (!eaid || !out || !found || *found) return;
        const auto p = owned_party();
        const auto record = p ? p->record(*eaid) : nullptr;
        if (!record || !record->group.id) return;
        const auto &g = record->group;
        std::array<std::byte, 0x28> status{};
        const auto put = [&](std::size_t at, auto value) { std::memcpy(status.data() + at, &value, sizeof(value)); };
        put(0x00, g.join); put(0x04, g.invite); put(0x08, g.id); put(0x10, g.members); put(0x11, g.limit);
        put(0x12, g.leader); put(0x18, g.leader_id); put(0x20, g.unlocked);
        std::memcpy(out, status.data(), status.size());
        *found = true;
    } catch (...) {}
}
} // namespace

void set_native_party_changes(bool allowed) noexcept { changes_allowed.store(allowed, std::memory_order_release); }
// The natives' own sequence (ui-actions.md 1.6). A fault in a native call returns false.
bool post_native_ui_event(std::uintptr_t base, std::uintptr_t type, const void *payload) noexcept {
    __try {
        alignas(16) std::array<std::byte, 16> context{};
        reinterpret_cast<void (*)(void *)>(base + party::current_context)(context.data());
        const auto dispatcher = reinterpret_cast<std::uintptr_t (*)(void *)>(base + party::event_dispatcher)(context.data());
        std::uint8_t live{};
        if (!dispatcher || !memory::peek(dispatcher + party::dispatcher_live, live) || !live) return false;
        struct Options {
            float delay;
            std::uint32_t count, flags;
        } options{0.f, 1, 0};
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t, const void *, void *, std::uintptr_t)>(base + party::event_post)(
            dispatcher, type, payload, &options, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace native_party_detail {
std::uint64_t last_native_invite(std::uint64_t id) {
    auto &r = requests();
    std::lock_guard lock(r.mutex);
    const auto it = r.invited.find(id);
    return it == r.invited.end() ? 0 : it->second;
}
// Raises a native UI event as the natives do (ui-actions.md 1.6); the payload is copied by the
// dispatcher's type-aware copy, so it may be freed after. Game thread.
bool post_event(std::uintptr_t type, const void *payload) {
    const auto base = state().base;
    return post_native_ui_event(base, base + type, payload);
}
void post_party_changes(const Published *previous, const Published &next) {
    if (!requests().installed.load(std::memory_order_acquire)) return;
    // The local party as the game shows it.
    const auto members = [](const Published *p) {
        std::map<std::uint64_t, bool> result; // id -> leads
        if (!p) return result;
        const Record *local{};
        for (const auto &record : p->records)
            if (record.handle && record.player.local) local = &record;
        if (!local || !local->group.id) return result;
        for (const auto &record : p->records)
            if (record.handle && record.player.member && record.group.id == local->group.id)
                result[record.player.id] = record.player.leader_of_party;
        return result;
    };
    const auto before = members(previous), after = members(&next);
    if (before == after) return;
    std::uint64_t local{};
    for (const auto &record : next.records)
        if (record.handle && record.player.local) local = record.player.id;
    struct Leave { std::uint64_t id; std::int32_t reason; std::int32_t padding; };
    // The local player joined a party (or it formed around them): one notification for them.
    if (before.empty() || !before.contains(local)) {
        if (after.contains(local)) post_event(party::group_join_event, &local);
    } else {
        for (const auto &[id, leads] : after)
            if (!before.contains(id)) post_event(party::group_join_event, &id);
    }
    if (!before.empty() && after.empty()) {
        const Leave left{local, 0, 0};
        post_event(party::group_leave_event, &left);
    } else {
        for (const auto &[id, leads] : before)
            if (!after.contains(id)) {
                const Leave left{id, 0, 0};
                post_event(party::group_leave_event, &left);
            }
    }
    for (const auto &[id, leads] : after)
        if (leads && before.contains(id) && !before.at(id)) post_event(party::leader_changed_event, &id);
}
} // namespace native_party_detail

bool post_native_party_invite(std::uint64_t from) noexcept {
    try {
        const auto p = owned_party();
        // The toast needs the inviter's player record, or it declines the invite at once.
        if (!p || !requests().installed.load(std::memory_order_acquire) || !session_player(*p, from)) return false;
        const auto id = std::to_string(from);
        const NativeText game(id), invite(id);
        struct Payload {
            Address game, invite;
            std::uint64_t inviter;
            std::int32_t type;
            bool matchmaking;
        } payload{game.value, invite.value, from, 1, false};
        static_assert(sizeof(Payload) == 0x20);
        return post_event(party::game_invite_event, &payload);
    } catch (...) { return false; }
}

namespace native_party_detail {
// Separate from install(): a build whose request natives moved keeps the other adapters.
void install_requests(Address base) {
    auto &r = requests();
    for (const auto &contract : party::party_request_contracts) {
        std::array<unsigned char, 32> bytes{};
        require(memory::read(base + contract.rva, bytes) && bytes == contract.bytes,
                "Native party request fingerprint differs; the game's party buttons stay native.");
    }
    struct Hook { Address rva; void *replacement; void **original; };
    const std::array hooks{
        Hook{party::request_invite, reinterpret_cast<void *>(&invite_hook), reinterpret_cast<void **>(&r.invite)},
        Hook{party::request_join_group, reinterpret_cast<void *>(&join_group_hook), reinterpret_cast<void **>(&r.join_group)},
        Hook{party::request_join_player, reinterpret_cast<void *>(&join_player_hook), reinterpret_cast<void **>(&r.join_player)},
        Hook{party::request_decline, reinterpret_cast<void *>(&decline_hook), reinterpret_cast<void **>(&r.decline)},
        Hook{party::request_leave, reinterpret_cast<void *>(&leave_hook), reinterpret_cast<void **>(&r.leave)},
        Hook{party::request_kick, reinterpret_cast<void *>(&kick_hook), reinterpret_cast<void **>(&r.kick)},
        Hook{party::request_promote, reinterpret_cast<void *>(&promote_hook), reinterpret_cast<void **>(&r.promote)},
        Hook{party::request_privacy, reinterpret_cast<void *>(&privacy_hook), reinterpret_cast<void **>(&r.privacy)},
        Hook{party::local_is_leader, reinterpret_cast<void *>(&local_is_leader_hook), reinterpret_cast<void **>(&r.local_is_leader)},
        Hook{party::in_group, reinterpret_cast<void *>(&in_group_hook), reinterpret_cast<void **>(&r.in_group)},
        Hook{party::player_group_status, reinterpret_cast<void *>(&group_status_hook), reinterpret_cast<void **>(&r.group_status)},
        Hook{party::request_accept_invite, reinterpret_cast<void *>(&accept_invite_hook), reinterpret_cast<void **>(&r.accept_invite)},
        Hook{party::voice_unavailable, reinterpret_cast<void *>(&voice_unavailable_hook), reinterpret_cast<void **>(&r.voice_unavailable)}};
    std::size_t prepared{};
    for (const auto &hook : hooks) {
        if (hook_prepare(reinterpret_cast<void *>(base + hook.rva), hook.replacement, hook.original) != HookOk) {
            while (prepared) hook_remove(reinterpret_cast<void *>(base + hooks[--prepared].rva));
            throw std::runtime_error("Cannot prepare the native party request hooks.");
        }
        ++prepared;
    }
    for (const auto &hook : hooks)
        require(hook_queue_enable(reinterpret_cast<void *>(base + hook.rva)) == HookOk, "Cannot enable the native party request hooks.");
    require(hook_apply_queued() == HookOk, "Cannot enable the native party request hooks.");
    r.installed.store(true, std::memory_order_release);
}
} // namespace native_party_detail
} // namespace dingosdk::multiplayer
