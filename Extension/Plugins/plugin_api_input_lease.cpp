#include "plugin_api.h"
#include "plugin_api_pad_fields.h"
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
namespace write = input_tracked_write;
constexpr char lease_type[] = "reskate.input.lease";
constexpr char epoch_type[] = "reskate.input.lease.epoch";
constexpr char write_type[] = "reskate.input.lease.write_version";
struct LeaseValue { std::uint64_t owner{}; lease::Handle handle{}; std::uint32_t user{}; };
// Diagnostic Lua fields never reconstruct a native observation. This copy is
// issued only after a successful read, inside this VM's opaque renewal epoch.
struct EpochValue { LeaseValue lease{}; lease::Snapshot snapshot{}; };
// Query identity only. There is deliberately no destructor or GC actuator.
struct WriteValue { LeaseValue lease{}; write::Version version{}; };

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
        row.polls_in_flight > 128 || (row.capture_reserved && row.write_reserved) || !known(row.reason) ||
        std::string_view(lease::status_name(row.status)) == "invalid") return false;
    const bool terminal = lease::terminal(row.status);
    if (!terminal && (row.source_released || row.orphaned || row.reason != lease::Reason::none)) return false;
    if (terminal && (row.reason == lease::Reason::none || row.source_released != (row.polls_in_flight == 0 && !row.capture_reserved && !row.write_reserved))) return false;
    if ((!row.last_observation) != (!row.observed_ms) || (row.last_observation && row.observed_ms < row.requested_ms)) return false;
    if (row.status == lease::Status::warming && (row.last_observation || row.observed_ms || row.renewals || row.capture_reserved || row.write_reserved)) return false;
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
// The version is opaque and VM-bound; receipt fields are diagnostics, never
// an authority reconstructed from Lua numbers, strings or a different lease.
WriteValue write_version(lua_State* L, const Plugin& plugin, const LeaseValue& value, int at) {
    const auto token = *static_cast<const WriteValue*>(luaL_checkudata(L, at, write_type));
    if (token.lease.owner != plugin.instance() || token.lease.handle != value.handle || token.lease.user != value.user ||
        !token.version.generation || !token.version.revision)
        luaL_argerror(L, at, "write version belongs to another input lease");
    return token;
}
bool valid(const WriteValue& value, const write::Receipt& row) noexcept {
    if (row.key.owner != value.lease.owner || row.key.lease_sequence != value.lease.handle.sequence ||
        row.key.user != value.lease.user || row.key.user > 3 || !row.key.source_epoch || !row.key.context_generation ||
        row.version != value.version || !row.version.generation || !row.version.revision ||
        row.desired_revision != row.version.revision || row.published_revision > row.desired_revision ||
        !row.write_expires_ms || !row.source_expires_ms || row.polls_in_flight > write::maximum_callbacks ||
        row.version_publications > row.publications || row.cleanup_publications > row.publications ||
        row.handback_publications > 1 || row.handback_publications > row.publications ||
        std::string_view(write::phase_name(row.phase)) == "invalid" ||
        std::string_view(write::reason_name(row.reason)) == "invalid" ||
        std::string_view(write::written_kind_name(row.last_written_kind)) == "invalid") return false;
    // The three disjoint complete-copy classes cannot exceed total. Subtract
    // only after each bound check so malformed u64 counters cannot wrap.
    if (row.cleanup_publications > row.publications - row.version_publications ||
        row.handback_publications > row.publications - row.version_publications - row.cleanup_publications) return false;
    if ((row.version_publications != 0) != (row.published_revision == row.desired_revision)) return false;
    if (row.published_revision && !row.publications) return false;
    if (row.publications && !row.handback_publications && !row.output_debt) return false;
    if ((row.last_written_kind == write::WrittenKind::none) != (row.publications == 0)) return false;
    if (row.last_written_kind == write::WrittenKind::desired && !row.published_revision) return false;
    if (row.last_written_kind == write::WrittenKind::cleanup && !row.cleanup_publications) return false;
    if ((row.last_written_kind == write::WrittenKind::handback) != (row.handback_publications == 1)) return false;
    // Counter exhaustion retains unknown cleanup duty, never active progress.
    // Identity/version/expiry values are not publication counters and stay exact.
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const bool saturated = row.version_publications == maximum || row.publications == maximum ||
        row.cleanup_publications == maximum || row.handback_publications == maximum;
    if (saturated && (row.phase != write::Phase::releasing || row.reason != write::Reason::exhausted ||
        !row.output_debt || !row.quarantined || !row.callbacks_unknown || row.handback_publications)) return false;
    if (row.callbacks_unknown && (row.phase != write::Phase::releasing || !row.output_debt || !row.quarantined)) return false;
    if (row.output_released != (row.phase == write::Phase::complete)) return false;
    if (row.phase == write::Phase::pending || row.phase == write::Phase::active) {
        if (row.reason != write::Reason::none || row.orphaned || row.quarantined || row.handback_publications) return false;
        if (row.phase == write::Phase::pending && row.version_publications) return false;
        if (row.phase == write::Phase::active && (!row.version_publications || !row.output_debt)) return false;
    }
    // A never-attempted cancellation may be releasing without output debt while
    // its exact admitted callback drains. Do not invent a publication witness.
    return row.phase != write::Phase::complete ||
        (!row.output_debt && !row.quarantined && !row.callbacks_unknown && !row.polls_in_flight &&
            (!row.publications || row.handback_publications == 1));
}
void push_write(lua_State* L, const write::Receipt& row) {
    lua_createtable(L, 0, 28);
    text(L, "sequence", row.key.lease_sequence); number(L, "user", row.key.user);
    text(L, "source_epoch", row.key.source_epoch); text(L, "context_generation", row.key.context_generation);
    text(L, "generation", row.version.generation); text(L, "revision", row.version.revision);
    text(L, "desired_revision", row.desired_revision); text(L, "published_revision", row.published_revision);
    text(L, "write_expires_ms", row.write_expires_ms); text(L, "source_expires_ms", row.source_expires_ms);
    text(L, "version_publications", row.version_publications); text(L, "publications", row.publications);
    text(L, "cleanup_publications", row.cleanup_publications); text(L, "handback_publications", row.handback_publications);
    lua_pushstring(L, write::phase_name(row.phase)); lua_setfield(L, -2, "phase");
    lua_pushstring(L, write::reason_name(row.reason)); lua_setfield(L, -2, "reason");
    lua_pushstring(L, write::written_kind_name(row.last_written_kind)); lua_setfield(L, -2, "last_written_kind");
    boolean(L, "output_debt", row.output_debt); boolean(L, "output_released", row.output_released);
    boolean(L, "quarantined", row.quarantined); boolean(L, "orphaned", row.orphaned);
    number(L, "polls_in_flight", row.polls_in_flight); boolean(L, "callbacks_unknown", row.callbacks_unknown); scope(L, "write");
    lua_setreadonly(L, -1, true);
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
    boolean(L, "capture_reserved", row.capture_reserved); boolean(L, "write_reserved", row.write_reserved); scope(L);
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
    static Plugin& write_plugin(lua_State* L, int arguments) {
        auto& p = plugin_of(L);
        need(L, p, "input.write", "reskate.input tracked selected-stream output");
        need(L, p, "input.lease", "reskate.input tracked output lease");
        if (p.services_.authority()) luaL_error(L, "input writes are client phase only");
        if (lua_gettop(L) != arguments) luaL_error(L, "input write argument count differs");
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
    static int write_request(lua_State* L) {
        auto& p = write_plugin(L, 3); const auto value = handle(L, p, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        if (lua_getmetatable(L, 2)) luaL_argerror(L, 2, "an issued plain input lease sample is required");
        lua_rawgetfield(L, 2, "epoch");
        const auto epoch = *static_cast<const EpochValue*>(luaL_checkudata(L, -1, epoch_type));
        lua_pop(L, 1);
        if (epoch.lease.owner != p.instance_ || epoch.lease.handle != value.handle || epoch.lease.user != value.user || !valid(value, epoch.snapshot))
            luaL_argerror(L, 2, "sample belongs to another input lease");
        const auto pad = pad_fields::normalized(L, 3);
        charge(L, p);
        // Allocation must precede admission. If delivery subsequently fails,
        // the already-known passive handle and owner-close still own cleanup.
        auto* token = opaque<WriteValue>(L, write_type);
        p.input_lease_used_ = true;
        write::Version version{}; auto reason = write::Reason::unavailable;
        const bool accepted = p.services_.game().input_lease_write_request(p.instance_, value.handle, epoch.snapshot, pad, version, reason);
        if (!accepted || reason != write::Reason::none || !version.generation || !version.revision) {
            lua_pop(L, 1);
            const auto word = std::string_view(write::reason_name(reason));
            return failed(L, accepted ? "invalid input write backend result" :
                (word == "invalid" || reason == write::Reason::none ? "unavailable" : word.data()));
        }
        *token = {value, version};
        return 1;
    }
    static int write_state(lua_State* L) {
        auto& p = write_plugin(L, 2); const auto value = handle(L, p, 1);
        const auto token = write_version(L, p, value, 2); charge(L, p);
        write::Receipt row{};
        if (!p.services_.game().input_lease_write_state(p.instance_, value.handle, token.version, row)) return failed(L, "unavailable");
        if (!valid(token, row)) return failed(L, "invalid input write backend receipt");
        push_write(L, row); return 1;
    }
    static int write_release(lua_State* L) {
        auto& p = write_plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        const bool accepted = p.services_.game().input_lease_write_release(p.instance_, value.handle);
        return outcome(L, accepted, accepted ? lease::Reason::none : lease::Reason::unavailable);
    }
    static int write_acknowledge(lua_State* L) {
        auto& p = write_plugin(L, 1); const auto value = handle(L, p, 1); charge(L, p);
        const bool accepted = p.services_.game().input_lease_write_acknowledge(p.instance_, value.handle);
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
    metatable(L, lease_type); metatable(L, epoch_type); metatable(L, write_type);
    add_functions(L, "input", {{"lease", InputLeaseApi::request}, {"lease_read", InputLeaseApi::read},
        {"lease_renew", InputLeaseApi::renew}, {"lease_release", InputLeaseApi::release},
        {"lease_state", InputLeaseApi::state}, {"lease_acknowledge", InputLeaseApi::acknowledge},
        {"lease_capture", InputLeaseApi::capture_request}, {"lease_capture_state", InputLeaseApi::capture_state},
        {"lease_release_capture", InputLeaseApi::capture_release}, {"lease_acknowledge_capture", InputLeaseApi::capture_acknowledge},
        {"lease_write", InputLeaseApi::write_request}, {"lease_write_state", InputLeaseApi::write_state},
        {"lease_release_write", InputLeaseApi::write_release}, {"lease_acknowledge_write", InputLeaseApi::write_acknowledge}});
}
}
