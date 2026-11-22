#include "plugin_api.h"
#include "lualib.h"

// reskate.replay (permission "replay"): the replay editor as a camera tool sees it. Its time (the moment the picture
// shows, on the replay's clock: the playhead, and while a video is exported the frame being filmed, slow motion and all),
// its timeline and trim, the camera's keyframes (read, and copied to be put back on a later visit), and where the replay
// has the skater. Read only, but for put_keys, which makes keys through the game's own add into sessions with none.
namespace dingosdk::plugins {
namespace {
void push_number(lua_State* L, const char* name, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}
void push_flag(lua_State* L, const char* name, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}

// replay.state(): nil outside the replay editor; else {time, playhead, from, to, keys_playing, exporting, frame, fps,
// ui_hidden, timeline = {start, length, at, trim_from, trim_to, shown_from, shown_to} or nil until it is read,
// chunks = {{clock_from, clock_to, from, to}, ...}}. Times are seconds on the replay's clock (keys are kept at them);
// the timeline's shares are of the whole replay (0 to 1), the bar showing shown_from to shown_to of it.
int replay_state(lua_State* L) {
    auto& plugin = permitted(L, "replay", "reskate.replay.state");
    const auto state = game_of(plugin).replay_state();
    if (!state) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 12);
    push_number(L, "time", state->time);
    push_number(L, "playhead", state->playhead);
    push_number(L, "from", state->from);
    push_number(L, "to", state->to);
    push_flag(L, "keys_playing", state->keys_playing);
    push_flag(L, "exporting", state->exporting);
    push_number(L, "frame", static_cast<double>(state->frame));
    push_number(L, "fps", state->fps);
    push_flag(L, "ui_hidden", state->ui_hidden);
    if (state->timeline) {
        lua_createtable(L, 0, 7);
        push_number(L, "start", state->start);
        push_number(L, "length", state->length);
        push_number(L, "at", state->at);
        push_number(L, "trim_from", state->trim_from);
        push_number(L, "trim_to", state->trim_to);
        push_number(L, "shown_from", state->shown_from);
        push_number(L, "shown_to", state->shown_to);
        lua_setfield(L, -2, "timeline");
    }
    lua_createtable(L, static_cast<int>(state->chunks.size()), 0);
    int n = 0;
    for (const auto& c : state->chunks) {
        lua_createtable(L, 0, 4);
        push_number(L, "clock_from", c.clock_from);
        push_number(L, "clock_to", c.clock_to);
        push_number(L, "from", c.from);
        push_number(L, "to", c.to);
        lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "chunks");
    return 1;
}
// replay.keys(): the camera's keyframes, every session's: {{session, time, camera ("orbit" or "free"; a number for
// another), fov, speed}, ...} by session and time; {} outside the editor.
int replay_keys(lua_State* L) {
    auto& plugin = permitted(L, "replay", "reskate.replay.keys");
    const auto keys = game_of(plugin).replay_keys();
    lua_createtable(L, static_cast<int>(keys.size()), 0);
    int n = 0;
    for (const auto& k : keys) {
        lua_createtable(L, 0, 5);
        push_number(L, "session", k.session);
        push_number(L, "time", k.time);
        if (k.camera == 0) lua_pushstring(L, "orbit");
        else if (k.camera == 1) lua_pushstring(L, "free");
        else lua_pushinteger(L, k.camera);
        lua_setfield(L, -2, "camera");
        push_number(L, "fov", k.fov);
        push_number(L, "speed", k.speed);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
// replay.copy_keys(): every camera keyframe as the game keeps it, copied and kept by ReSkate for this plugin (the game
// wipes them when the editor closes): the copy's number, or nil and why there is none. A plugin keeps at most 8.
int replay_copy_keys(lua_State* L) {
    auto& plugin = permitted(L, "replay", "reskate.replay.copy_keys");
    const auto copy = game_of(plugin).replay_copy_keys(plugin.id());
    if (!copy) return failed(L, "there are no camera keyframes to copy (or the replay editor is not open)");
    lua_pushnumber(L, static_cast<double>(copy));
    return 1;
}
// replay.put_keys(copy): that copy put back, as the editor had it: each key made by the game's own add, then given its kept
// value, only into sessions with no keys of their own and only those on footage the replay still has, within what the
// editor shows. How many went back, or nil and why none could.
int replay_put_keys(lua_State* L) {
    auto& plugin = permitted(L, "replay", "reskate.replay.put_keys");
    const double wanted = luaL_checknumber(L, 1);
    if (!(wanted >= 1 && wanted < 9007199254740992.0) || wanted != static_cast<double>(static_cast<std::uint64_t>(wanted)))
        luaL_argerror(L, 1, "a copy's number, as replay.copy_keys gave it");
    std::size_t put{};
    if (const auto why = game_of(plugin).replay_put_keys(plugin.id(), static_cast<std::uint64_t>(wanted), put); !why.empty()) return failed(L, why);
    lua_pushnumber(L, static_cast<double>(put));
    return 1;
}
// replay.skater(): where the replay has the skater now: {body, upper, board} (world positions), or nil.
int replay_skater(lua_State* L) {
    auto& plugin = permitted(L, "replay", "reskate.replay.skater");
    const auto skater = game_of(plugin).replay_skater();
    if (!skater) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    field_vec(L, "body", skater->body);
    field_vec(L, "upper", skater->upper);
    field_vec(L, "board", skater->board);
    return 1;
}
} // namespace

void install_replay_api(Plugin& /*plugin*/, lua_State* L) {
    add_functions(L, "replay", {{"state", replay_state}, {"keys", replay_keys}, {"copy_keys", replay_copy_keys}, {"put_keys", replay_put_keys},
                                {"skater", replay_skater}});
}
} // namespace dingosdk::plugins
