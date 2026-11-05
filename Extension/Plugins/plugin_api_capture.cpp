#include "plugin_api.h"
#include "plugin_game.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// reskate.capture (the screen as the game draws it, saved as a GIF, APNG, PNG or WebM), the game
// menu's preview turned (reskate.room.turn) and read (reskate.menu.game, reskate.menu.grid,
// reskate.menu.favorites), what is up over the game (reskate.menu.state: the pause menu, ReSkate's
// menu, the replay editor), what the skater wears (reskate.outfit.worn, board, title) and
// reskate.files.reveal (the plugin's data folder shown in Explorer). The building blocks only: when
// to turn the preview and what a saved capture's card says are the calling plugin's.
namespace dingosdk::plugins {
namespace {
// A string option of a table at `index` (which may be nil), or `fallback`. Unlike text_field
// (plugin_api.h) it has a fallback and no length bound, and a non-string is Lua's own argument error.
std::string text_option(lua_State* L, int index, const char* name, const char* fallback) {
    if (lua_isnoneornil(L, index)) return fallback;
    lua_getfield(L, index, name);
    std::string value = fallback;
    if (!lua_isnil(L, -1)) value = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return value;
}
} // namespace

struct CaptureApi {
    static Plugin& capturing(lua_State* L, const char* what) { return permitted(L, "capture", what); }

    // start({area = {left, top, right, bottom}, frame = "screen"|"menu", depth, height, skip, frames, seconds})
    static int start(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.start");
        Game::CaptureStart request;
        if (!lua_isnoneornil(L, 1)) {
            luaL_checktype(L, 1, LUA_TTABLE);
            lua_getfield(L, 1, "area");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                float edges[4]{};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    const double v = luaL_checknumber(L, -1);
                    if (!(v >= 0 && v <= 1)) luaL_error(L, "an area is {left, top, right, bottom}, each 0 to 1");
                    edges[i] = static_cast<float>(v);
                    lua_pop(L, 1);
                }
                if (!(edges[2] > edges[0] && edges[3] > edges[1])) luaL_error(L, "an area's right is past its left, and its bottom past its top");
                request.left = edges[0], request.top = edges[1], request.right = edges[2], request.bottom = edges[3];
            }
            lua_pop(L, 1);
            const auto frame = text_option(L, 1, "frame", "screen");
            if (frame != "screen" && frame != "menu") luaL_error(L, "frame is \"screen\" or \"menu\"");
            request.menu = frame == "menu";
            request.depth = flag_option(L, 1, "depth", true);
            request.background = flag_option(L, 1, "background", false);
            request.height = static_cast<std::uint32_t>(number_option(L, 1, "height", 0, 0, 4096));
            if (request.height && request.height < 64) luaL_error(L, "height is 0 (as on screen) or 64 to 4096");
            request.skip = static_cast<std::uint32_t>(number_option(L, 1, "skip", 0, 0, 600));
            request.most = static_cast<std::uint32_t>(number_option(L, 1, "frames", 2400, 1, 20000));
            request.seconds = number_option(L, 1, "seconds", 60, 1, 120);
        }
        if (!within(plugin.services_.now_ms(), plugin.control_second_, plugin.controls_, 4)) return result(L, "asked more than 4 times in a second");
        return result(L, game_of(plugin).capture_start(plugin.id(), request));
    }
    static int stop(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.stop");
        game_of(plugin).capture_stop(plugin.id());
        return 0;
    }
    static int discard(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.discard");
        game_of(plugin).capture_discard(plugin.id());
        return 0;
    }
    static int state(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.state");
        const auto now = game_of(plugin).capture_status(plugin.id());
        lua_createtable(L, 0, 8);
        field_text(L, "state", now.state);
        lua_pushinteger(L,static_cast<int>(now.dropped));lua_setfield(L,-2,"dropped");
        lua_pushinteger(L,static_cast<int>(now.pending));lua_setfield(L,-2,"pending");
        lua_pushinteger(L, static_cast<int>(now.taken));
        lua_setfield(L, -2, "taken");
        lua_pushinteger(L, static_cast<int>(now.kept));
        lua_setfield(L, -2, "kept");
        lua_pushboolean(L, now.depth);
        lua_setfield(L, -2, "depth");
        if (!now.message.empty()) field_text(L, "message", now.message);
        if (!now.file.empty()) field_text(L, "file", now.file);
        return 1;
    }
    static std::uint32_t frame_number(lua_State* L, int index) {
        const double n = luaL_checknumber(L, index);
        if (!(n >= 1 && n <= 20000) || n != std::floor(n)) luaL_argerror(L, index, "a frame is a whole number from 1");
        return static_cast<std::uint32_t>(n) - 1;
    }
    // compare(a, b): how unlike two frames taken look, 0 to 255; nil when either is not known.
    static int compare(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.compare");
        const float d = game_of(plugin).capture_compare(plugin.id(), frame_number(L, 1), frame_number(L, 2));
        if (d < 0) lua_pushnil(L);
        else lua_pushnumber(L, d);
        return 1;
    }
    static int kept(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.kept");
        const auto list = game_of(plugin).capture_kept(plugin.id());
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            lua_pushinteger(L, static_cast<int>(list[i]) + 1);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    // save(path, {frames, format, fps, size, background, cutout, solid, card = {title, lines}})
    static int save(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.save");
        need(L, plugin, "files", "reskate.capture.save");
        Game::CaptureSave request;
        request.path = data_path(L, 1, false);
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "frames");
        luaL_checktype(L, -1, LUA_TTABLE);
        const int count = lua_objlen(L, -1);
        if (count < 1 || count > 360) luaL_error(L, "a capture saves 1 to 360 frames");
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            request.frames.push_back(frame_number(L, lua_gettop(L)));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        request.format = text_option(L, 2, "format", "gif");
        if (request.format != "gif" && request.format != "apng" && request.format != "png" && request.format != "webm")
            luaL_error(L, "format is \"gif\", \"apng\", \"png\" or \"webm\"");
        if (request.format == "png" && count != 1) luaL_error(L, "a png is one frame");
        request.fps = static_cast<std::uint32_t>(number_option(L, 2, "fps", 30, 1, 60));
        request.size = static_cast<std::uint32_t>(number_option(L, 2, "size", 512, 64, 2048));
        lua_getfield(L, 2, "background");
        if (lua_isnumber(L, -1)) {
            const double c = lua_tonumber(L, -1);
            if (!(c >= 0 && c <= 0xFFFFFF) || c != std::floor(c)) luaL_error(L, "background is 0xRRGGBB, or \"transparent\"");
            request.background = static_cast<std::uint32_t>(c);
        } else if (!lua_isnil(L, -1) && std::string(luaL_checkstring(L, -1)) != "transparent")
            luaL_error(L, "background is 0xRRGGBB, or \"transparent\"");
        lua_pop(L, 1);
        request.cutout = text_option(L, 2, "cutout", "auto");
        if (request.cutout != "auto" && request.cutout != "depth" && request.cutout != "colour" && request.cutout != "none")
            luaL_error(L, "cutout is \"auto\", \"depth\", \"colour\" or \"none\"");
        request.solid = flag_option(L, 2, "solid", true);
        lua_getfield(L, 2, "card");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const int card = lua_gettop(L);
            request.card_title = text_option(L, card, "title", "");
            if (request.card_title.size() > 32) luaL_error(L, "a card's title is at most 32 bytes");
            lua_getfield(L, card, "lines");
            luaL_checktype(L, -1, LUA_TTABLE);
            const int lines = lua_objlen(L, -1);
            if (lines > 16) luaL_error(L, "a card has at most 16 lines");
            for (int i = 1; i <= lines; ++i) {
                lua_rawgeti(L, -1, i);
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                std::string label = luaL_checkstring(L, -2), value = luaL_checkstring(L, -1);
                if (label.size() > 64 || value.size() > 64) luaL_error(L, "a card's line is {label, value}, each at most 64 bytes");
                request.card.emplace_back(std::move(label), std::move(value));
                lua_pop(L, 3);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!within(plugin.services_.now_ms(), plugin.files_second_, plugin.file_calls_, 64)) return result(L, "more than 64 file calls in a second");
        return result(L, game_of(plugin).capture_save(plugin.id(), request));
    }

    // Read-only menu rig sample, with room permission and no ownership of the rig.
    static int room_pose(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "room", "reskate.room.pose");
        const std::string subject = luaL_optstring(L, 1, "board");
        if (subject != "board" && subject != "skater") luaL_argerror(L, 1, "board or skater");
        const auto pose = game_of(plugin).room_pose(subject == "skater");
        if (!pose) { lua_pushnil(L); return 1; }
        lua_createtable(L, 0, 4);
        lua_pushvector(L, pose->position[0], pose->position[1], pose->position[2]);
        lua_setfield(L, -2, "position");
        push_quaternion(L, pose->rotation);
        lua_setfield(L, -2, "rotation");
        lua_pushnumber(L, pose->age);
        lua_setfield(L, -2, "age");
        lua_pushnumber(L, static_cast<double>(pose->sequence));
        lua_setfield(L, -2, "sequence");
        return 1;
    }
    // room.turn(value, stick): the game menu's preview turned by a stand-in pad's stick, held 300 ms.
    static int room_turn(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "room", "reskate.room.turn");
        const double value = luaL_checknumber(L, 1);
        if (!(value >= -1 && value <= 1)) luaL_argerror(L, 1, "-1 to 1");
        const std::string stick = luaL_optstring(L, 2, "right_x");
        static constexpr const char* sticks[] = {"left_x", "left_y", "right_x", "right_y"};
        const auto at = std::find_if(std::begin(sticks), std::end(sticks), [&](const char* s) { return stick == s; });
        if (at == std::end(sticks)) luaL_argerror(L, 2, "\"left_x\", \"left_y\", \"right_x\" or \"right_y\"");
        return result(L, game_of(plugin).room_turn(plugin.id(), static_cast<float>(value), static_cast<int>(at - std::begin(sticks))));
    }
    static int menu_game(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.game");
        const auto menu = game_of(plugin).game_menu();
        lua_createtable(L, 0, 4);
        lua_pushboolean(L, menu.known);
        lua_setfield(L, -2, "known");
        lua_pushboolean(L, menu.open);
        lua_setfield(L, -2, "open");
        lua_pushboolean(L, menu.open && menu.skaters);
        lua_setfield(L, -2, "skaters");
        push_texts(L, menu.buttons, 64);
        lua_setfield(L, -2, "buttons");
        return 1;
    }
    // menu.state(): what is up over the game now, as menu_changed hands it over.
    static int menu_state(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.state");
        push_menu_state(L, game_of(plugin).menu_state());
        return 1;
    }
    static int menu_grid(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.grid");
        const auto grid = game_of(plugin).menu_grid();
        if (!grid) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 6);
        if (!grid->focused.empty()) field_text(L, "focused", grid->focused);
        if (!grid->title.empty()) field_text(L, "title", grid->title);
        if (!grid->category.empty()) field_text(L, "category", grid->category);
        push_texts(L, grid->items, 4000);
        lua_setfield(L, -2, "items");
        lua_pushinteger(L, static_cast<int>(grid->shown));
        lua_setfield(L, -2, "shown");
        lua_pushinteger(L, static_cast<int>(grid->total));
        lua_setfield(L, -2, "total");
        lua_pushboolean(L, grid->showing);
        lua_setfield(L, -2, "showing");
        lua_pushboolean(L, grid->held);
        lua_setfield(L, -2, "held");
        if (grid->prompts_unit > 0) {
            lua_createtable(L, 0, 4);
            const std::pair<const char*, float> fields[] = {
                {"left", grid->prompts_left}, {"top", grid->prompts_top}, {"width", grid->prompts_width}, {"unit", grid->prompts_unit}};
            for (const auto& [name, value] : fields) {
                lua_pushnumber(L, value);
                lua_setfield(L, -2, name);
            }
            lua_setfield(L, -2, "prompts");
        }
        return 1;
    }
    // menu.grid_show(keys): these items, in this order, in the grid that has the focus; nil gives it back.
    static int grid_show(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.grid_show");
        std::optional<std::vector<std::string>> keys;
        if (!lua_isnoneornil(L, 1)) keys = item_keys(L, 1);
        return result(L, game_of(plugin).grid_show(plugin.id(), std::move(keys)));
    }
    // menu.grid_mark(keys): the game's check on these items' tiles; nil or {} takes them off.
    static int grid_mark(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.grid_mark");
        std::vector<std::string> keys;
        if (!lua_isnoneornil(L, 1)) keys = item_keys(L, 1);
        game_of(plugin).grid_mark(plugin.id(), std::move(keys));
        return 0;
    }
    // menu.favorites(): the player's starred item keys, or nil and why the set is not read yet.
    // An empty table is a set that was read and holds nothing.
    static int menu_favorites(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.favorites");
        const auto set = game_of(plugin).menu_favorites();
        if (!set.ready) {
            lua_pushnil(L);
            if (set.why.empty()) lua_pushstring(L, "not loaded");
            else lua_pushlstring(L, set.why.data(), set.why.size());
            return 2;
        }
        lua_createtable(L, 0, static_cast<int>(set.keys.size()));
        for (const auto& key : set.keys) {
            lua_pushlstring(L, key.data(), key.size());
            lua_pushboolean(L, 1);
            lua_settable(L, -3);
        }
        return 1;
    }
    static int push_worn(lua_State* L, const std::vector<Game::WornItem>& list) {
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            lua_createtable(L, 0, 3);
            field_text(L, "slot", list[i].slot);
            field_text(L, "item", list[i].item);
            field_text(L, "title", list[i].title);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int outfit_worn(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.worn");
        return push_worn(L, game_of(plugin).outfit_worn());
    }
    static int outfit_board(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.board");
        return push_worn(L, game_of(plugin).board_parts());
    }
    static int outfit_title(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.title");
        const auto key = checked_text(L, 1, 255, "an item's key");
        const auto title = game_of(plugin).item_title(key);
        lua_pushlstring(L, title.data(), title.size());
        return 1;
    }
    // outfit.on_top(items): the plugin's items worn on top of the outfit, replaced; nil or {} takes them off.
    static int on_top(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.on_top");
        std::vector<std::string> items;
        if (!lua_isnoneornil(L, 1)) {
            luaL_checktype(L, 1, LUA_TTABLE);
            const int count = lua_objlen(L, 1);
            if (count > 32) luaL_argerror(L, 1, "at most 32 items");
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, 1, i);
                items.push_back(checked_text(L, lua_gettop(L), 200, "an item's name"));
                lua_pop(L, 1);
                if (items.back().empty()) luaL_argerror(L, 1, "an item's name is empty");
            }
        }
        if (!within(plugin.services_.now_ms(), plugin.control_second_, plugin.controls_, 4)) return result(L, "asked more than 4 times in a second");
        return result(L, game_of(plugin).outfit_on_top(plugin.id(), std::move(items)));
    }
    static int on_top_state(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.on_top_state");
        const auto now = game_of(plugin).outfit_on_top_state(plugin.id());
        lua_createtable(L, 0, 3);
        push_texts(L, now.items);
        lua_setfield(L, -2, "items");
        push_texts(L, now.worn);
        lua_setfield(L, -2, "worn");
        if (!now.note.empty()) field_text(L, "note", now.note);
        return 1;
    }
    static int files_reveal(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "files", "reskate.files.reveal");
        const auto folder = data_path(L, 1, true);
        if (!within(plugin.services_.now_ms(), plugin.reveal_second_, plugin.reveals_, 1)) return result(L, "once a second");
        return result(L, game_of(plugin).files_reveal(plugin.id(), folder));
    }
    static void install(lua_State* L) {
        add_functions(L, "capture", {{"start", start}, {"stop", stop}, {"discard", discard}, {"state", state}, {"compare", compare},
                                     {"kept", kept}, {"save", save}});
        add_functions(L, "room", {{"turn", room_turn}, {"pose", room_pose}});
        add_functions(L, "menu", {{"game", menu_game}, {"state", menu_state}, {"grid", menu_grid}, {"grid_show", grid_show},
                                  {"grid_mark", grid_mark}, {"favorites", menu_favorites}});
        add_functions(L, "outfit", {{"worn", outfit_worn}, {"board", outfit_board}, {"title", outfit_title}, {"on_top", on_top}, {"on_top_state", on_top_state}});
        add_functions(L, "files", {{"reveal", files_reveal}});
    }
};

void install_capture_api(Plugin&, lua_State* L) { CaptureApi::install(L); }

void push_menu_state(lua_State* L, const Game::MenuState& state) {
    lua_createtable(L, 0, 5);
    lua_pushboolean(L, state.known);
    lua_setfield(L, -2, "known");
    lua_pushboolean(L, state.pause);
    lua_setfield(L, -2, "pause");
    lua_pushboolean(L, state.reskate);
    lua_setfield(L, -2, "reskate");
    lua_pushboolean(L, state.replay_editor);
    lua_setfield(L, -2, "replay_editor");
    // Only the words plugins were told of (Enums.MenuPage) reach them.
    if (one_of(menu_pages, state.page)) field_text(L, "page", state.page);
}
} // namespace dingosdk::plugins
