#include "plugin_api.h"
#include "lua.h"
#include "lualib.h"
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <string_view>

namespace dingosdk::plugins {
namespace {
namespace lease = input_lease;
namespace capture = input_lease_capture;
constexpr char lease_type[] = "reskate.input.lease";
constexpr char epoch_type[] = "reskate.input.lease.epoch";
struct LeaseValue { std::uint64_t owner{}; lease::Handle handle{}; std::uint32_t user{}; };
// Diagnostic Lua fields never reconstruct a native observation. This copy is
// issued only after a successful read, inside this VM's opaque renewal epoch.
struct EpochValue { LeaseValue lease{}; lease::Snapshot snapshot{}; };

bool known(lease::Reason reason) noexcept { return std::string_view(lease::reason_name(reason)) != "invalid"; }
const char* refusal(lease::Reason reason) noexcept {
    return !known(reason) || reason == lease::Reason::none ? "unavailable" : lease::reason_name(reason);
}
int failed(lua_State* L, const char* reason) { lua_pushnil(L); lua_pushstring(L, reason); return 2; }
int failed(lua_State* L, lease::Reason reason) { return failed(L, refusal(reason)); }
int outcome(lua_State* L, bool accepted, lease::Reason reason) {
    if (accepted && reason == lease::Reason::none) { lua_pushboolean(L, true); return 1; }
    lua_pushboolean(L, false);
    lua_pushstring(L, accepted ? "invalid input lease backend result" : refusal(reason));
    return 2;
}
void text(lua_State* L, const char* key, std::uint64_t value) {
    const auto word = std::to_string(value);
    lua_pushlstring(L, word.data(), word.size()); lua_setfield(L, -2, key);
}
void number(lua_State* L, const char* key, double value) { lua_pushnumber(L, value); lua_setfield(L, -2, key); }
void boolean(lua_State* L, const char* key, bool value) { lua_pushboolean(L, value); lua_setfield(L, -2, key); }
void scope(lua_State* L, const char* mode = "observe") {
    lua_pushstring(L, "xinput"); lua_setfield(L, -2, "transport");
    lua_pushstring(L, "observed_stream"); lua_setfield(L, -2, "source_identity");
    lua_pushstring(L, "unobserved"); lua_setfield(L, -2, "physical_device_identity");
    lua_pushstring(L, mode); lua_setfield(L, -2, "mode");
    lua_pushstring(L, "unobserved"); lua_setfield(L, -2, "native_consumption");
}
void metatable(lua_State* L, const char* name) {
    luaL_newmetatable(L, name);
    lua_pushstring(L, "locked"); lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, true); lua_pop(L, 1);
}
template<class T> T* opaque(lua_State* L, const char* name) {
    auto* value = new (lua_newuserdata(L, sizeof(T))) T{};
    luaL_getmetatable(L, name); lua_setmetatable(L, -2); return value;
}
LeaseValue handle(lua_State* L, const Plugin& plugin, int index) {
    const auto value = *static_cast<const LeaseValue*>(luaL_checkudata(L, index, lease_type));
    if (value.owner != plugin.instance() || !value.handle.sequence || value.user > 3)
        luaL_error(L, "input lease belongs to another plugin instance");
    return value;
}
bool valid(const LeaseValue& value, const lease::Snapshot& row) noexcept {
    return row.handle == value.handle && row.user == value.user && row.user <= 3 &&
        row.source_epoch && row.context_generation && row.observation_serial && row.observed_ms;
}
bool valid(const LeaseValue& value, const lease::Receipt& row) noexcept {
    if (row.handle != value.handle || row.user != value.user || row.user > 3 || !row.source_epoch ||
        !row.context_generation || !row.requested_ms || !row.expires_ms ||
        row.requested_ms > std::numeric_limits<std::uint64_t>::max() - lease::lifetime_ms ||
        row.expires_ms < row.requested_ms || row.expires_ms - row.requested_ms < lease::lifetime_ms ||
        row.polls_in_flight > 128 || !known(row.reason) ||
        std::string_view(lease::status_name(row.status)) == "invalid") return false;
    const bool terminal = lease::terminal(row.status);
    if (!terminal && (row.source_released || row.orphaned || row.reason != lease::Reason::none)) return false;
    if (terminal && (row.reason == lease::Reason::none || row.source_released != (row.polls_in_flight == 0 && !row.capture_reserved))) return false;
    if ((!row.last_observation) != (!row.observed_ms) || (row.last_observation && row.observed_ms < row.requested_ms)) return false;
    if (row.status == lease::Status::warming && (row.last_observation || row.observed_ms || row.renewals || row.capture_reserved)) return false;
    return row.status != lease::Status::active || (row.last_observation && row.observed_ms < row.expires_ms);
}
// A complete receipt may still have exact callbacks pending. Unknown callback
// duty is never accepted as clean or reconstructed from the bounded known count.
bool valid(const LeaseValue& value, const capture::Receipt& row) noexcept {
    if (row.key.owner != value.owner || row.key.lease_sequence != value.handle.sequence ||
        row.key.user != value.user || row.key.user > 3 || !row.key.source_epoch || !row.key.context_generation ||
        !row.generation || !row.revision || !row.expires_ms || row.polls_in_flight > capture::maximum_callbacks ||
        row.neutral_releases > row.publications ||
        std::string_view(capture::phase_name(row.phase)) == "invalid" || row.phase == capture::Phase::idle ||
        std::string_view(capture::reason_name(row.reason)) == "invalid") return false;
    if (row.callbacks_unknown && (row.phase != capture::Phase::releasing || !row.debt || !row.quarantined)) return false;
    if (row.phase == capture::Phase::waiting_neutral &&
        (row.publications || row.neutral_releases || row.debt || row.quarantined || row.orphaned)) return false;
    if (row.phase == capture::Phase::capturing && (!row.debt || !row.publications || row.orphaned || row.quarantined)) return false;
    if (row.phase == capture::Phase::releasing && !row.debt) return false;
    return row.phase != capture::Phase::complete || !row.debt;
}
void push_pad(lua_State* L, const lease::Pad& pad) {
    lua_createtable(L, 0, 7);
    number(L, "buttons", pad.buttons); number(L, "left_trigger", pad.left_trigger); number(L, "right_trigger", pad.right_trigger);
    number(L, "left_x", pad.left_x); number(L, "left_y", pad.left_y); number(L, "right_x", pad.right_x); number(L, "right_y", pad.right_y);
    lua_setreadonly(L, -1, true);
}
void push_snapshot(lua_State* L, const LeaseValue& value, const lease::Snapshot& row) {
    lua_createtable(L, 0, 15);
    *opaque<EpochValue>(L, epoch_type) = {value, row}; lua_setfield(L, -2, "epoch");
    text(L, "sequence", row.handle.sequence); text(L, "source_epoch", row.source_epoch);
    text(L, "observation_serial", row.observation_serial); text(L, "observed_ms", row.observed_ms);
    text(L, "context_generation", row.context_generation);
    number(L, "user", row.user); number(L, "packet", row.packet);
    push_pad(L, row.pad); lua_setfield(L, -2, "pad");
    boolean(L, "neutral", lease::neutral(row.pad)); scope(L);
    lua_setreadonly(L, -1, true);
}
void push_receipt(lua_State* L, const lease::Receipt& row) {
    lua_createtable(L, 0, 18);
    text(L, "sequence", row.handle.sequence); number(L, "user", row.user);
    lua_pushstring(L, lease::status_name(row.status)); lua_setfield(L, -2, "status");
    lua_pushstring(L, lease::reason_name(row.reason)); lua_setfield(L, -2, "reason");
    text(L, "requested_ms", row.requested_ms); text(L, "expires_ms", row.expires_ms); text(L, "observed_ms", row.observed_ms);
    text(L, "source_epoch", row.source_epoch); text(L, "context_generation", row.context_generation);
    text(L, "last_observation", row.last_observation); text(L, "renewals", row.renewals);
    number(L, "polls_in_flight", row.polls_in_flight);
    boolean(L, "source_released", row.source_released); boolean(L, "orphaned", row.orphaned);
    boolean(L, "capture_reserved", row.capture_reserved); scope(L);
    lua_setreadonly(L, -1, true);
}
void push_capture(lua_State* L, const capture::Receipt& row) {
    lua_createtable(L, 0, 22);
    text(L, "sequence", row.key.lease_sequence); number(L, "user", row.key.user);
    text(L, "source_epoch", row.key.source_epoch); text(L, "context_generation", row.key.context_generation);
    text(L, "generation", row.generation); text(L, "revision", row.revision); text(L, "expires_ms", row.expires_ms);
    text(L, "publications", row.publications); text(L, "neutral_releases", row.neutral_releases);
    lua_pushstring(L, capture::phase_name(row.phase)); lua_setfield(L, -2, "phase");
    lua_pushstring(L, capture::reason_name(row.reason)); lua_setfield(L, -2, "reason");
    boolean(L, "debt", row.debt); boolean(L, "quarantined", row.quarantined);
    number(L, "polls_in_flight", row.polls_in_flight); boolean(L, "orphaned", row.orphaned);
    boolean(L, "callbacks_unknown", row.callbacks_unknown); scope(L, "capture");
    lua_setreadonly(L, -1, true);
}
}

struct InputLeaseApi {
    static Plugin& plugin(lua_State* L, int arguments) {
        auto& p = plugin_of(L);
        need(L, p, "input.lease", "reskate.input lease observation");
        if (p.services_.authority()) luaL_error(L, "input leases are client phase only");
        if (lua_gettop(L) != arguments) luaL_error(L, "input lease argument count differs");
        return p;
    }
    static Plugin& capture_plugin(lua_State* L, int arguments) {
        auto& p = plugin_of(L);
        need(L, p, "input.capture", "reskate.input selected-stream capture");
        need(L, p, "input.lease", "reskate.input selected-stream capture lease");
        if (p.services_.authority()) luaL_error(L, "input captures are client phase only");
        if (lua_gettop(L) != arguments) luaL_error(L, "input capture argument count differs");
        return p;
    }
    static void charge(lua_State* L, Plugin& p) {
        if (p.input_lease_calls_ >= 64) luaL_error(L, "more than 64 input lease calls in one tick");
        ++p.input_lease_calls_;
    }
    static int request(lua_State* L) {
        auto& p = plugin(L, 1);
        if (lua_type(L, 1) != LUA_TNUMBER) luaL_argerror(L, 1, "XInput user must be an integer from 0 to 3");
        const double user = lua_tonumber(L, 1);
        if (!std::isfinite(user) || user < 0 || user > 3 || std::floor(user) != user)
            luaL_argerror(L, 1, "XInput user must be an integer from 0 to 3");
        charge(L, p);
        if (p.input_lease_requests_ >= 1) return failed(L, "input lease requested more than once in one tick");
        ++p.input_lease_requests_;
        // Allocate before a backend request can retain ownership. If a backend
        // partially refuses, close still revokes this owner exactly once.
        auto* value = opaque<LeaseValue>(L, lease_type);
        p.input_lease_used_ = true;
        lease::Handle selected{}; auto reason = lease::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_request(p.instance_, static_cast<std::uint32_t>(user), selected, reason);
        if (!accepted || reason != lease::Reason::none || !selected.sequence) {
            lua_pop(L, 1);
            return accepted ? failed(L, "invalid input lease backend result") : failed(L, reason);
        }
        *value = {p.instance_, selected, static_cast<std::uint32_t>(user)};
        return 1;
    }
    static int read(lua_State* L) {
        auto& p = plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        lease::Snapshot row{}; auto reason = lease::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_read(p.instance_, value.handle, row, reason);
        if (!accepted) return failed(L, reason);
        if (reason != lease::Reason::none || !valid(value, row)) return failed(L, "invalid input lease backend sample");
        push_snapshot(L, value, row); return 1;
    }
    static int renew(lua_State* L) {
        auto& p = plugin(L, 2); const auto value = handle(L, p, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        if (lua_getmetatable(L, 2)) luaL_argerror(L, 2, "an issued plain input lease sample is required");
        lua_rawgetfield(L, 2, "epoch");
        const auto epoch = *static_cast<const EpochValue*>(luaL_checkudata(L, -1, epoch_type));
        lua_pop(L, 1);
        if (epoch.lease.owner != p.instance_ || epoch.lease.handle != value.handle || epoch.lease.user != value.user || !valid(value, epoch.snapshot))
            luaL_argerror(L, 2, "sample belongs to another input lease");
        charge(L, p); auto reason = lease::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_renew(p.instance_, value.handle, epoch.snapshot, reason);
        return outcome(L, accepted, reason);
    }
    static int release(lua_State* L) {
        auto& p = plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        auto reason = lease::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_release(p.instance_, value.handle, reason);
        return outcome(L, accepted, reason);
    }
    static int state(lua_State* L) {
        auto& p = plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        lease::Receipt row{}; auto reason = lease::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_state(p.instance_, value.handle, row, reason);
        if (!accepted) return failed(L, reason);
        if (reason != lease::Reason::none || !valid(value, row)) return failed(L, "invalid input lease backend receipt");
        push_receipt(L, row); return 1;
    }
    static int capture_request(lua_State* L) {
        auto& p = capture_plugin(L, 2); const auto value = handle(L, p, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        if (lua_getmetatable(L, 2)) luaL_argerror(L, 2, "an issued plain input lease sample is required");
        lua_rawgetfield(L, 2, "epoch");
        const auto epoch = *static_cast<const EpochValue*>(luaL_checkudata(L, -1, epoch_type));
        lua_pop(L, 1);
        if (epoch.lease.owner != p.instance_ || epoch.lease.handle != value.handle || epoch.lease.user != value.user || !valid(value, epoch.snapshot))
            luaL_argerror(L, 2, "sample belongs to another input lease");
        charge(L, p);
        auto reason = capture::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_capture_request(p.instance_, value.handle, epoch.snapshot, reason);
        if (accepted && reason == capture::Reason::none) { lua_pushboolean(L, true); return 1; }
        lua_pushboolean(L, false);
        const auto word = std::string_view(capture::reason_name(reason));
        lua_pushstring(L, accepted ? "invalid input capture backend result" :
            (word == "invalid" || reason == capture::Reason::none ? "unavailable" : word.data()));
        return 2;
    }
    static int capture_state(lua_State* L) {
        auto& p = capture_plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        capture::Receipt row{};
        if (!p.services_.game().input_lease_capture_state(p.instance_, value.handle, row)) return failed(L, "unavailable");
        if (!valid(value, row)) return failed(L, "invalid input capture backend receipt");
        push_capture(L, row); return 1;
    }
    static int capture_release(lua_State* L) {
        auto& p = capture_plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        const bool accepted = p.services_.game().input_lease_capture_release(p.instance_, value.handle);
        return outcome(L, accepted, accepted ? lease::Reason::none : lease::Reason::unavailable);
    }
    static int capture_acknowledge(lua_State* L) {
        auto& p = capture_plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        const bool accepted = p.services_.game().input_lease_capture_acknowledge(p.instance_, value.handle);
        return outcome(L, accepted, accepted ? lease::Reason::none : lease::Reason::unavailable);
    }
    static int acknowledge(lua_State* L) {
        auto& p = plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        auto reason = lease::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_acknowledge(p.instance_, value.handle, reason);
        return outcome(L, accepted, reason);
    }
};

void install_input_lease_api(Plugin&, lua_State* L) {
    metatable(L, lease_type); metatable(L, epoch_type);
    add_functions(L, "input", {{"lease", InputLeaseApi::request}, {"lease_read", InputLeaseApi::read},
        {"lease_renew", InputLeaseApi::renew}, {"lease_release", InputLeaseApi::release},
        {"lease_state", InputLeaseApi::state}, {"lease_acknowledge", InputLeaseApi::acknowledge},
        {"lease_capture", InputLeaseApi::capture_request}, {"lease_capture_state", InputLeaseApi::capture_state},
        {"lease_release_capture", InputLeaseApi::capture_release}, {"lease_acknowledge_capture", InputLeaseApi::capture_acknowledge}});
}
}
