#include "plugin_api.h"
#include "lualib.h"

// reskate.videos (permission "videos"): the videos the game exports from its replay editor, by their files' names,
// moved for the plugin into a folder the player picks (Windows' folder picker; ReSkate keeps it for the plugin). A
// plugin never has a path: it cannot read, write or move any other file. (Extension/Rendering/export_videos.h)
namespace dingosdk::plugins {
namespace {
// videos.list(): {{name, bytes, written (seconds since 1970), ready (the game has let it go)}, ...}, newest first.
int videos_list(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.list");
    const auto videos = game_of(plugin).exported_videos();
    lua_createtable(L, static_cast<int>(videos.size()), 0);
    int n = 0;
    for (const auto& video : videos) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, video.name.data(), video.name.size());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, static_cast<double>(video.bytes));
        lua_setfield(L, -2, "bytes");
        lua_pushnumber(L, video.written);
        lua_setfield(L, -2, "written");
        lua_pushboolean(L, video.ready);
        lua_setfield(L, -2, "ready");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
// videos.folder(): the folder the player picked for this plugin's videos, as text to show; nil while there is none.
int videos_folder(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.folder");
    const auto folder = game_of(plugin).video_folder(plugin.id());
    if (folder.empty()) lua_pushnil(L);
    else lua_pushlstring(L, folder.data(), folder.size());
    return 1;
}
// videos.pick_folder([title]): Windows' folder picker opens over the game; the folder the player picks becomes this
// plugin's (videos.folder() says it once picked). true, or false and why it did not open.
int videos_pick_folder(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.pick_folder");
    const std::string title = lua_isnoneornil(L, 1) ? std::string() : checked_text(L, 1, 120, "a picker's title");
    const auto why = game_of(plugin).pick_video_folder(plugin.id(), title);
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
// videos.forget_folder(): videos stay where the game puts them again.
int videos_forget_folder(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.forget_folder");
    game_of(plugin).forget_video_folder(plugin.id());
    return 0;
}
// videos.open_folder(): File Explorer opens on the plugin's folder (the game's Videos folder while it has none).
int videos_open_folder(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.open_folder");
    const auto why = game_of(plugin).open_video_folder(plugin.id());
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
// videos.move(name): one of the game's videos (a name videos.list gave, ready) moved into the plugin's folder, on a
// thread of its own; videos.moved() says how it ended. true, or false and why it cannot start.
int videos_move(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.move");
    const auto name = checked_text(L, 1, 260, "a video's name");
    const auto why = game_of(plugin).move_video(plugin.id(), name);
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
// videos.moved(): how the plugin's moves have ended since it last asked: {{name, to (its name in the folder) or why
// (it stays where the game put it)}, ...}.
int videos_moved(lua_State* L) {
    auto& plugin = permitted(L, "videos", "reskate.videos.moved");
    const auto moves = game_of(plugin).video_moves(plugin.id());
    lua_createtable(L, static_cast<int>(moves.size()), 0);
    int n = 0;
    for (const auto& move : moves) {
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, move.name.data(), move.name.size());
        lua_setfield(L, -2, "name");
        if (move.why.empty()) {
            lua_pushlstring(L, move.to.data(), move.to.size());
            lua_setfield(L, -2, "to");
        } else {
            lua_pushlstring(L, move.why.data(), move.why.size());
            lua_setfield(L, -2, "why");
        }
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
} // namespace

void install_videos_api(Plugin& /*plugin*/, lua_State* L) {
    add_functions(L, "videos", {{"list", videos_list}, {"folder", videos_folder}, {"pick_folder", videos_pick_folder},
                                {"forget_folder", videos_forget_folder}, {"open_folder", videos_open_folder}, {"move", videos_move},
                                {"moved", videos_moved}});
}
} // namespace dingosdk::plugins
