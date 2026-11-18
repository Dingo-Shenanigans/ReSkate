#include "plugin_api.h"
#include "plugin_picture.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>

// reskate.picture: a picture composed from a short list of drawing steps and written as a PNG into
// the plugin's data folder, a text measured the way a text step draws it, and a picture's size. The
// steps are checked here, so the game is handed only what it can draw (plugin_picture.h says what a
// step is); the drawing itself is the game's, off the plugin's thread.
namespace dingosdk::plugins {
namespace {
std::string extension_of(std::string_view path) {
    auto extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}
}

struct PictureApi {
    // What a request has used of its limits so far.
    struct Counts {
        int steps{};
        std::size_t points{};
    };

    // A number of the step at `table`: `fallback` when it is left out (an error when there is none).
    static float number(lua_State* L, int table, const char* name, const char* kind, std::optional<float> fallback = std::nullopt) {
        lua_getfield(L, table, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            if (!fallback) luaL_error(L, "a \"%s\" step needs %s", kind, name);
            return *fallback;
        }
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "a \"%s\" step's %s is a number", kind, name);
        const double value = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (!std::isfinite(value)) luaL_error(L, "a \"%s\" step's %s is not a finite number", kind, name);
        if (std::fabs(value) > picture::most_coordinate) luaL_error(L, "a \"%s\" step's %s is past a million pixels", kind, name);
        return static_cast<float>(value);
    }
    // The same, from 0 to 1 (an alpha, a focus).
    static float share(lua_State* L, int table, const char* name, const char* kind, std::optional<float> fallback = std::nullopt) {
        const float value = number(L, table, name, kind, fallback);
        if (!(value >= 0 && value <= 1)) luaL_error(L, "a \"%s\" step's %s is 0 to 1", kind, name);
        return value;
    }
    // `texel`: canvas pixels to one texel of a tiled picture, 1 when left out.
    static float texel(lua_State* L, int table, const char* kind) {
        const float value = number(L, table, "texel", kind, 1.0f);
        if (!(value > 0)) luaL_error(L, "a \"%s\" step's texel is more than 0", kind);
        return value;
    }
    // colour = {r, g, b, a}: each 0 to 1, a 1 when left out. Without one, `fallback` (an error when
    // there is none).
    static picture::Colour colour(lua_State* L, int table, const char* kind, std::optional<picture::Colour> fallback = std::nullopt) {
        lua_getfield(L, table, "colour");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            if (!fallback) luaL_error(L, "a \"%s\" step needs colour", kind);
            return *fallback;
        }
        static constexpr char shape[] = "a colour is {r, g, b, a}, each 0 to 1";
        if (!lua_istable(L, -1)) luaL_error(L, "%s", shape);
        float parts[4]{0, 0, 0, 1};
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(L, -1, i + 1);
            if (!(i == 3 && lua_isnil(L, -1))) {
                const double value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
                if (!(value >= 0 && value <= 1)) luaL_error(L, "%s", shape);
                parts[i] = static_cast<float>(value);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return {parts[0], parts[1], parts[2], parts[3]};
    }
    // A picture named by the string at `index`: a .png or .jpg of the plugin's own package, or "data:"
    // and one of its data folder (as the overlay's d:image names them). One that is not there is not
    // an error: its step draws nothing.
    static picture::File picture_file(lua_State* L, Plugin& plugin, int index) {
        index = lua_absindex(L, index);
        if (lua_type(L, index) != LUA_TSTRING) luaL_error(L, "a picture is named by its path");
        picture::File file;
        file.name = checked_text(L, index, 206, "a picture's path");
        const auto extension = extension_of(file.name);
        if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") luaL_error(L, "a picture is a .png or .jpg file");
        if (file.name.starts_with("data:")) {
            need(L, plugin, "files", "a data: picture");
            file.data = data_path(L, index, file.name.substr(5), false);
        } else file.file = package_file(L, plugin, file.name);
        return file;
    }
    // A font named by the string at `index`: a .ttf or .otf of the plugin's own package. Where each is
    // on disk is kept, so measuring a text does not look for its font's file again.
    static picture::File font_file(lua_State* L, Plugin& plugin, int index) {
        index = lua_absindex(L, index);
        if (lua_type(L, index) != LUA_TSTRING) luaL_error(L, "a font is named by its path");
        picture::File file;
        file.name = checked_text(L, index, 206, "a font's path");
        if (const auto found = plugin.picture_fonts_.find(file.name); found != plugin.picture_fonts_.end()) {
            file.file = found->second;
            return file;
        }
        const auto extension = extension_of(file.name);
        if (extension != ".ttf" && extension != ".otf") luaL_error(L, "a font is a .ttf or .otf file of the plugin's package");
        file.file = package_file(L, plugin, file.name);
        if (plugin.picture_fonts_.size() >= 64) plugin.picture_fonts_.clear();
        plugin.picture_fonts_.emplace(file.name, file.file);
        return file;
    }
    static float text_size(lua_State* L, double value) {
        if (!(value > 0 && value <= picture::most_layer_side)) luaL_error(L, "a text's size is more than 0 and at most %d pixels", picture::most_layer_side);
        return static_cast<float>(value);
    }

    // One step, the table at `index`. `layer`: it is one of a layer's own, which are polygons and texts.
    static picture::Step step(lua_State* L, Plugin& plugin, int index, bool layer, Counts& counts) {
        using Kind = picture::Step::Kind;
        index = lua_absindex(L, index);
        if (!lua_istable(L, index)) luaL_error(L, "a step is a table");
        if (++counts.steps > picture::most_steps) luaL_error(L, "a picture has at most %d steps", picture::most_steps);
        luaL_checkstack(L, 8, "a picture's steps");
        lua_getfield(L, index, "kind");
        const std::string kind = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (layer && kind != "poly" && kind != "text") luaL_error(L, "a layer's steps are \"poly\" and \"text\" steps");
        const char* name = kind.c_str();
        const auto named = [&](const char* field, picture::File (*file)(lua_State*, Plugin&, int), bool needed) {
            lua_getfield(L, index, field);
            picture::File found;
            if (!lua_isnil(L, -1)) found = file(L, plugin, -1);
            else if (needed) luaL_error(L, "a \"%s\" step needs %s", name, field);
            lua_pop(L, 1);
            return found;
        };
        picture::Step out;
        if (kind == "fill") {
            out.kind = Kind::fill;
            out.colour = colour(L, index, name);
        } else if (kind == "tile") {
            out.kind = Kind::tile;
            out.picture = named("picture", picture_file, true);
            out.texel = texel(L, index, name);
        } else if (kind == "image") {
            out.kind = Kind::image;
            out.picture = named("picture", picture_file, true);
            out.alpha = share(L, index, "alpha", name, 1.0f);
            lua_getfield(L, index, "fit");
            if (!lua_isnil(L, -1)) {
                if (lua_type(L, -1) != LUA_TSTRING || std::string_view(lua_tostring(L, -1)) != "cover") luaL_error(L, "an image step's fit is \"cover\"");
                out.cover = true;
            }
            lua_pop(L, 1);
            if (out.cover) {
                out.focus = share(L, index, "focus", name, 0.5f);
            } else {
                out.x = number(L, index, "x", name);
                out.y = number(L, index, "y", name);
                out.w = number(L, index, "w", name);
                out.h = number(L, index, "h", name);
            }
        } else if (kind == "gradient") {
            out.kind = Kind::gradient;
            out.colour = colour(L, index, name);
            out.y0 = number(L, index, "y0", name);
            out.y1 = number(L, index, "y1", name);
            out.a0 = share(L, index, "a0", name);
            out.a1 = share(L, index, "a1", name);
        } else if (kind == "layer") {
            out.kind = Kind::layer;
            out.w = number(L, index, "w", name);
            out.h = number(L, index, "h", name);
            if (!(out.w >= 1 && out.w <= picture::most_layer_side && out.h >= 1 && out.h <= picture::most_layer_side) || out.w != std::floor(out.w) ||
                out.h != std::floor(out.h))
                luaL_error(L, "a layer is 1 to %d pixels wide and high", picture::most_layer_side);
            out.x = number(L, index, "cx", name);
            out.y = number(L, index, "cy", name);
            out.angle = number(L, index, "angle", name, 0.0f);
            lua_getfield(L, index, "steps");
            if (!lua_istable(L, -1)) luaL_error(L, "a layer step needs steps");
            const int steps = lua_gettop(L), count = lua_objlen(L, steps);
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, steps, i);
                out.steps.push_back(step(L, plugin, -1, true, counts));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else if (kind == "poly") {
            out.kind = Kind::poly;
            out.picture = named("picture", picture_file, false);
            // (Filled with a picture, its colour is only what it falls back to: nothing, left out.)
            out.colour = colour(L, index, name, out.picture.name.empty() ? std::nullopt : std::optional(picture::Colour{0, 0, 0, 0}));
            out.texel = texel(L, index, name);
            lua_getfield(L, index, "points");
            if (!lua_istable(L, -1)) luaL_error(L, "a poly step needs points");
            const int points = lua_gettop(L), count = lua_objlen(L, points);
            if (count < 3) luaL_error(L, "a polygon has at least 3 points");
            counts.points += static_cast<std::size_t>(count);
            if (counts.points > static_cast<std::size_t>(picture::most_points)) luaL_error(L, "a picture's polygons have at most %d points", picture::most_points);
            out.points.reserve(static_cast<std::size_t>(count));
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, points, i);
                if (!lua_istable(L, -1)) luaL_error(L, "a point is {x, y}");
                std::array<float, 2> point{};
                for (int axis = 0; axis < 2; ++axis) {
                    lua_rawgeti(L, -1, axis + 1);
                    const double value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : std::nan("");
                    if (!std::isfinite(value) || std::fabs(value) > picture::most_coordinate) luaL_error(L, "a point is {x, y}, finite and within a million pixels");
                    point[static_cast<std::size_t>(axis)] = static_cast<float>(value);
                    lua_pop(L, 1);
                }
                out.points.push_back(point);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else if (kind == "text") {
            out.kind = Kind::text;
            out.font = named("font", font_file, true);
            lua_getfield(L, index, "text");
            if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "a text step needs text");
            std::size_t length = 0;
            const char* text = lua_tolstring(L, -1, &length);
            if (length > picture::most_text_bytes) luaL_error(L, "a text is at most %d bytes", static_cast<int>(picture::most_text_bytes));
            out.text.assign(text, length);
            lua_pop(L, 1);
            out.size = text_size(L, number(L, index, "size", name));
            out.x = number(L, index, "x", name);
            out.y = number(L, index, "y", name);
            out.colour = colour(L, index, name);
            const auto align = text_field(L, index, "align", 8);
            if (!align.empty() && !one_of(aligns, align)) luaL_error(L, "a text step's align is \"left\", \"center\" or \"right\"");
            out.align = align == "right" ? picture::Align::right : align.starts_with("cent") ? picture::Align::center : picture::Align::left;
        } else {
            luaL_error(L, "a step's kind is \"fill\", \"tile\", \"image\", \"gradient\", \"layer\", \"poly\" or \"text\"");
        }
        return out;
    }

    // make(path, width, height, steps, fn): the steps drawn in order on an opaque canvas and written as
    // a PNG at `path` in the data folder (reskate.files' paths), whole or not at all. Drawing takes
    // tens of milliseconds, so it is done off the plugin's thread: true once it is started, and
    // fn(ok, why) comes in a later tick, when the file is there or cannot be. False and why when it
    // cannot be started. At most most_a_second a second and most_going at once.
    static int make(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "files", "reskate.picture.make");
        const double now = plugin.services_.now_ms();
        if (!within(now, plugin.files_second_, plugin.file_calls_, plugin.limits_.file_calls_per_second))
            luaL_error(L, "more than %d file calls in a second", plugin.limits_.file_calls_per_second);
        picture::Request request;
        request.path = data_path(L, 1, false);
        if (extension_of(request.path) != ".png") luaL_argerror(L, 1, "a picture is written as a .png file");
        const auto side = [&](int index) {
            const double value = luaL_checknumber(L, index);
            if (!(value >= 1 && value <= picture::most_side) || value != std::floor(value))
                luaL_error(L, "a picture is 1 to %d pixels wide and high", picture::most_side);
            return static_cast<int>(value);
        };
        request.width = side(2);
        request.height = side(3);
        luaL_checktype(L, 4, LUA_TTABLE);
        if (!lua_isnoneornil(L, 5)) luaL_checktype(L, 5, LUA_TFUNCTION);
        Counts counts;
        const int count = lua_objlen(L, 4);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 4, i);
            request.steps.push_back(step(L, plugin, -1, false, counts));
            lua_pop(L, 1);
        }
        if (plugin.pictures_.size() >= picture::most_going) return result(L, std::format("it draws {} pictures already", picture::most_going));
        if (!within(now, plugin.picture_second_, plugin.picture_starts_, picture::most_a_second))
            return result(L, std::format("more than {} pictures in a second", picture::most_a_second));
        auto job = std::make_shared<picture::Job>();
        if (const auto why = game_of(plugin).picture_make(plugin.id(), std::move(request), job); !why.empty()) return result(L, why);
        int callback = 0;
        if (!lua_isnoneornil(L, 5)) {
            lua_pushvalue(L, 5);
            callback = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        plugin.pictures_.push_back({callback, std::move(job)});
        lua_pushboolean(L, 1);
        return 1;
    }
    // The pictures that are done, in the order they were asked for: each one's fn(ok, why).
    static void deliver(Plugin& plugin) {
        while (plugin.state_ == State::running) {
            const auto done = std::find_if(plugin.pictures_.begin(), plugin.pictures_.end(),
                                           [](const Plugin::PictureGoing& going) { return going.job->done.load(std::memory_order_acquire); });
            if (done == plugin.pictures_.end()) return;
            const auto taken = std::move(*done);
            plugin.pictures_.erase(done);
            if (!taken.callback) continue;
            const auto& why = taken.job->why;
            lua_getref(plugin.L_, taken.callback);
            lua_unref(plugin.L_, taken.callback);
            lua_pushboolean(plugin.L_, why.empty());
            if (why.empty()) lua_pushnil(plugin.L_);
            else lua_pushlstring(plugin.L_, why.data(), why.size());
            plugin.call(2, "its picture callback", plugin.limits_.call_ms);
        }
    }
    // text_width(font, text, size): how many pixels wide the line is, the measure a text step aligns
    // by. No permission: it reads only the plugin's own package. nil and why for a font that cannot
    // be read.
    static int text_width(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto font = font_file(L, plugin, 1);
        const auto text = checked_text(L, 2, picture::most_text_bytes, "a text");
        const float em = text_size(L, luaL_checknumber(L, 3));
        const Plugin::Untimed untimed{plugin}; // reading the font, the first time (Plugin::Untimed)
        const auto width = game_of(plugin).picture_text_width(plugin.id(), font, text, em);
        if (!width) return failed(L, "the font cannot be read");
        lua_pushnumber(L, *width);
        return 1;
    }
    // size(picture): its width and height in pixels, or nil and why. A package's picture needs no
    // permission; a "data:" one is a file call.
    static int size(lua_State* L) {
        auto& plugin = plugin_of(L);
        const auto file = picture_file(L, plugin, 1);
        if (!file.data.empty() && !within(plugin.services_.now_ms(), plugin.files_second_, plugin.file_calls_, plugin.limits_.file_calls_per_second))
            luaL_error(L, "more than %d file calls in a second", plugin.limits_.file_calls_per_second);
        const Plugin::Untimed untimed{plugin}; // reading it
        const auto found = game_of(plugin).picture_size(plugin.id(), file);
        if (!found) return failed(L, "it is not a PNG or JPG of at most 4096 x 4096 pixels");
        lua_pushnumber(L, found->first);
        lua_pushnumber(L, found->second);
        return 2;
    }
};

void install_picture_api(Plugin&, lua_State* L) {
    using P = PictureApi;
    add_functions(L, "picture", {{"make", P::make}, {"text_width", P::text_width}, {"size", P::size}});
}
void deliver_pictures(Plugin& plugin) { PictureApi::deliver(plugin); }
}
