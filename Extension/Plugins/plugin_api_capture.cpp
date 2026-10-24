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
// menu's preview turned (reskate.room.turn) and read (reskate.menu.game, reskate.menu.grid), what the
// skater wears (reskate.outfit.worn, board, title) and reskate.files.reveal: from Fit Check (pull
// request 326), whose turn finding and card are now a plugin's (contrib/plugins/fit-check).
namespace dingosdk::plugins {
namespace {
int result(lua_State* L, const std::string& why) {
    lua_pushboolean(L, why.empty());
    if (why.empty()) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}
void push_text(lua_State* L, const std::string& text, const char* field) {
    lua_pushlstring(L, text.data(), text.size());
    lua_setfield(L, -2, field);
}
// An option of a table at `index`: a number within [least, most] (raises otherwise), or `fallback`.
double number_option(lua_State* L, int index, const char* name, double fallback, double least, double most) {
    if (lua_isnoneornil(L, index)) return fallback;
    lua_getfield(L, index, name);
    double value = fallback;
    if (!lua_isnil(L, -1)) {
        value = luaL_checknumber(L, -1);
        if (!(value >= least && value <= most)) luaL_error(L, "%s is %g to %g", name, least, most);
    }
    lua_pop(L, 1);
    return value;
}
bool flag_option(lua_State* L, int index, const char* name, bool fallback) {
    if (lua_isnoneornil(L, index)) return fallback;
    lua_getfield(L, index, name);
    const bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}
std::string text_option(lua_State* L, int index, const char* name, const char* fallback) {
    if (lua_isnoneornil(L, index)) return fallback;
    lua_getfield(L, index, name);
    std::string value = fallback;
    if (!lua_isnil(L, -1)) value = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return value;
}
// A per-second budget; false past it.
bool within(double now, double& second, int& count, int most) {
    if (now - second >= 1000) {
        second = now;
        count = 0;
    }
    return ++count <= most;
}
} // namespace

struct CaptureApi {
    static Game& game(Plugin& plugin) { return plugin.services_.game(); }
    static Plugin& capturing(lua_State* L, const char* what) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "capture", what);
        return plugin;
    }

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
            request.height = static_cast<std::uint32_t>(number_option(L, 1, "height", 0, 0, 4096));
            if (request.height && request.height < 64) luaL_error(L, "height is 0 (as on screen) or 64 to 4096");
            request.skip = static_cast<std::uint32_t>(number_option(L, 1, "skip", 0, 0, 600));
            request.most = static_cast<std::uint32_t>(number_option(L, 1, "frames", 2400, 1, 20000));
            request.seconds = number_option(L, 1, "seconds", 60, 1, 120);
        }
        if (!within(plugin.services_.now_ms(), plugin.control_second_, plugin.controls_, 4)) return result(L, "asked more than 4 times in a second");
        return result(L, game(plugin).capture_start(plugin.id(), request));
    }
    static int stop(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.stop");
        game(plugin).capture_stop(plugin.id());
        return 0;
    }
    static int discard(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.discard");
        game(plugin).capture_discard(plugin.id());
        return 0;
    }
    static int state(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.state");
        const auto now = game(plugin).capture_status(plugin.id());
        lua_createtable(L, 0, 6);
        push_text(L, now.state, "state");
        lua_pushinteger(L, static_cast<int>(now.taken));
        lua_setfield(L, -2, "taken");
        lua_pushinteger(L, static_cast<int>(now.kept));
        lua_setfield(L, -2, "kept");
        lua_pushboolean(L, now.depth);
        lua_setfield(L, -2, "depth");
        if (!now.message.empty()) push_text(L, now.message, "message");
        if (!now.file.empty()) push_text(L, now.file, "file");
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
        const float d = game(plugin).capture_compare(plugin.id(), frame_number(L, 1), frame_number(L, 2));
        if (d < 0) lua_pushnil(L);
        else lua_pushnumber(L, d);
        return 1;
    }
    static int kept(lua_State* L) {
        auto& plugin = capturing(L, "reskate.capture.kept");
        const auto list = game(plugin).capture_kept(plugin.id());
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
        return result(L, game(plugin).capture_save(plugin.id(), request));
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
        return result(L, game(plugin).room_turn(plugin.id(), static_cast<float>(value), static_cast<int>(at - std::begin(sticks))));
    }
    static int menu_game(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.game");
        const auto menu = game(plugin).game_menu();
        lua_createtable(L, 0, 3);
        lua_pushboolean(L, menu.open);
        lua_setfield(L, -2, "open");
        lua_pushboolean(L, menu.open && menu.skaters);
        lua_setfield(L, -2, "skaters");
        lua_createtable(L, static_cast<int>(menu.buttons.size()), 0);
        for (std::size_t i = 0; i < menu.buttons.size() && i < 64; ++i) {
            lua_pushlstring(L, menu.buttons[i].data(), menu.buttons[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "buttons");
        return 1;
    }
    static int menu_grid(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.grid");
        const auto grid = game(plugin).menu_grid();
        if (!grid) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 6);
        if (!grid->focused.empty()) push_text(L, grid->focused, "focused");
        if (!grid->title.empty()) push_text(L, grid->title, "title");
        if (!grid->category.empty()) push_text(L, grid->category, "category");
        lua_createtable(L, static_cast<int>(std::min<std::size_t>(grid->items.size(), 4000)), 0);
        for (std::size_t i = 0; i < grid->items.size() && i < 4000; ++i) {
            lua_pushlstring(L, grid->items[i].data(), grid->items[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
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
    static std::vector<std::string> keys_at(lua_State* L, int index, int most) {
        std::vector<std::string> keys;
        luaL_checktype(L, index, LUA_TTABLE);
        const int count = lua_objlen(L, index);
        if (count > most) luaL_argerror(L, index, most == 4000 ? "at most 4000 items" : "too many items");
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, index, i);
            keys.push_back(checked_text(L, lua_gettop(L), 255, "an item's key"));
            lua_pop(L, 1);
        }
        return keys;
    }
    // menu.grid_show(keys): these items, in this order, in the grid that has the focus; nil gives it back.
    static int grid_show(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.grid_show");
        std::optional<std::vector<std::string>> keys;
        if (!lua_isnoneornil(L, 1)) keys = keys_at(L, 1, 4000);
        return result(L, game(plugin).grid_show(plugin.id(), std::move(keys)));
    }
    // menu.grid_mark(keys): the game's check on these items' tiles; nil or {} takes them off.
    static int grid_mark(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.grid_mark");
        std::vector<std::string> keys;
        if (!lua_isnoneornil(L, 1)) keys = keys_at(L, 1, 4000);
        game(plugin).grid_mark(plugin.id(), std::move(keys));
        return 0;
    }
    static int push_worn(lua_State* L, const std::vector<Game::WornItem>& list) {
        lua_createtable(L, static_cast<int>(list.size()), 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            lua_createtable(L, 0, 3);
            push_text(L, list[i].slot, "slot");
            push_text(L, list[i].item, "item");
            push_text(L, list[i].title, "title");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int outfit_worn(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.worn");
        return push_worn(L, game(plugin).outfit_worn());
    }
    static int outfit_board(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.board");
        return push_worn(L, game(plugin).board_parts());
    }
    static int outfit_title(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.title");
        const auto key = checked_text(L, 1, 255, "an item's key");
        const auto title = game(plugin).item_title(key);
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
        return result(L, game(plugin).outfit_on_top(plugin.id(), std::move(items)));
    }
    static int on_top_state(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "outfit", "reskate.outfit.on_top_state");
        const auto now = game(plugin).outfit_on_top_state(plugin.id());
        const auto list = [&](const std::vector<std::string>& names, const char* field) {
            lua_createtable(L, static_cast<int>(names.size()), 0);
            for (std::size_t i = 0; i < names.size(); ++i) {
                lua_pushlstring(L, names[i].data(), names[i].size());
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
            lua_setfield(L, -2, field);
        };
        lua_createtable(L, 0, 3);
        list(now.items, "items");
        list(now.worn, "worn");
        if (!now.note.empty()) push_text(L, now.note, "note");
        return 1;
    }
    static int files_reveal(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "files", "reskate.files.reveal");
        const auto folder = data_path(L, 1, true);
        if (!within(plugin.services_.now_ms(), plugin.reveal_second_, plugin.reveals_, 1)) return result(L, "once a second");
        return result(L, game(plugin).files_reveal(plugin.id(), folder));
    }
    static void install(lua_State* L) {
        add_functions(L, "capture", {{"start", start}, {"stop", stop}, {"discard", discard}, {"state", state}, {"compare", compare},
                                     {"kept", kept}, {"save", save}});
        add_functions(L, "room", {{"turn", room_turn}});
        add_functions(L, "menu", {{"game", menu_game}, {"grid", menu_grid}, {"grid_show", grid_show}, {"grid_mark", grid_mark}});
        add_functions(L, "outfit", {{"worn", outfit_worn}, {"board", outfit_board}, {"title", outfit_title}, {"on_top", on_top}, {"on_top_state", on_top_state}});
        add_functions(L, "files", {{"reveal", files_reveal}});
    }
};

void install_capture_api(Plugin&, lua_State* L) { CaptureApi::install(L); }
} // namespace dingosdk::plugins
