#include "plugin_api.h"
#include "plugin_wire.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

// The parts of the reskate API that reach players and other games: drawing on the overlay
// (overlay), toasts, menu panels (menu), menu sounds (audio.ui), the mod list (mods.read), the
// session (session.read) and messages to the same plugin elsewhere (net.session). As in
// plugin_api.cpp, every value is checked before it leaves the VM.
namespace dingosdk::plugins {
namespace {
float finite(lua_State* L, int index, const char* what) {
    const double value = luaL_checknumber(L, index);
    if (!std::isfinite(value) || std::fabs(value) > 1e6) luaL_error(L, "%s must be a finite number", what);
    return static_cast<float>(value);
}
plugin_ui::Vec3 point(lua_State* L, int index, const char* what) {
    const float* v = luaL_checkvector(L, index);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e6f) luaL_error(L, "%s must be a finite position", what);
    return {v[0], v[1], v[2]};
}

// "#RRGGBB", "#RRGGBBAA" or 0xRRGGBBAA, as ImGui's 0xAABBGGRR.
std::uint32_t colour_of(lua_State* L, int index) {
    std::uint32_t rgba = 0;
    if (lua_type(L, index) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        const std::string_view hex(text, length);
        if ((length != 7 && length != 9) || hex[0] != '#') luaL_error(L, "a colour is \"#RRGGBB\" or \"#RRGGBBAA\"");
        const auto [end, error] = std::from_chars(hex.data() + 1, hex.data() + length, rgba, 16);
        if (error != std::errc{} || end != hex.data() + length) luaL_error(L, "a colour is \"#RRGGBB\" or \"#RRGGBBAA\"");
        if (length == 7) rgba = rgba << 8 | 0xff;
    } else {
        const double value = luaL_checknumber(L, index);
        if (!(value >= 0 && value <= 4294967295.0) || value != std::floor(value)) luaL_error(L, "a colour number is 0xRRGGBBAA");
        rgba = static_cast<std::uint32_t>(value);
    }
    const std::uint32_t r = rgba >> 24, g = (rgba >> 16) & 0xff, b = (rgba >> 8) & 0xff, a = rgba & 0xff;
    return r | g << 8 | b << 16 | a << 24;
}
// A style table: colour, size, thickness, fill, rounding, align ("left", "centre", "right"), shadow.
plugin_ui::Style style_of(lua_State* L, int index) {
    plugin_ui::Style style;
    if (lua_isnoneornil(L, index)) return style;
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);
    const auto field = [&](const char* name) { return lua_getfield(L, index, name) != LUA_TNIL; };
    if (field("colour") || (lua_pop(L, 1), field("color"))) style.colour = colour_of(L, -1);
    lua_pop(L, 1);
    if (field("size")) style.size = std::clamp(finite(L, -1, "size"), 4.0f, 400.0f);
    lua_pop(L, 1);
    if (field("thickness")) style.thickness = std::clamp(finite(L, -1, "thickness"), 0.5f, 40.0f);
    lua_pop(L, 1);
    if (field("rounding")) style.rounding = std::clamp(finite(L, -1, "rounding"), 0.0f, 200.0f);
    lua_pop(L, 1);
    if (field("fill")) style.fill = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (field("shadow")) style.shadow = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (field("font")) {
        const std::string font = luaL_checkstring(L, -1);
        if (font == "bold") style.font = 0;
        else if (font == "body") style.font = 1;
        else if (font == "heading") style.font = 2;
        else if (font == "title") style.font = 3;
        else if (font == "title_large") style.font = 4;
        else luaL_error(L, "font is \"bold\", \"body\", \"heading\", \"title\" or \"title_large\"");
    }
    lua_pop(L, 1);
    // A picture's source rectangle, {u0, v0, u1, v1} from 0 to 1: one glyph of a font atlas, a tile.
    if (field("uv")) {
        luaL_checktype(L, -1, LUA_TTABLE);
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(L, -1, i + 1);
            const float v = finite(L, -1, "a uv corner");
            if (v < 0 || v > 1) luaL_error(L, "uv corners are 0 to 1");
            style.uv[static_cast<std::size_t>(i)] = v;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (field("wrap")) style.wrap = std::clamp(finite(L, -1, "wrap"), 0.0f, 10000.0f);
    lua_pop(L, 1);
    if (field("align")) {
        const std::string align = luaL_checkstring(L, -1);
        if (align == "left") style.align = 0;
        else if (align == "centre" || align == "center") style.align = 1;
        else if (align == "right") style.align = 2;
        else luaL_error(L, "align is \"left\", \"centre\" or \"right\"");
    }
    lua_pop(L, 1);
    return style;
}
const plugin_ui::Action* pending(Plugin& plugin, const std::vector<plugin_ui::Action>& actions, const std::string& panel,
                                 const std::string& widget) {
    (void)plugin;
    const auto found = std::find_if(actions.rbegin(), actions.rend(),
        [&](const auto& action) { return action.panel == panel && action.widget == widget; });
    return found == actions.rend() ? nullptr : &*found;
}
}

struct UiApi {
    // ---- overlay: drawing ------------------------------------------------------------------
    static Plugin& drawing(lua_State* L) {
        auto& plugin = plugin_of(L);
        if (!plugin.drawing_) luaL_error(L, "drawing is only inside a draw handler: reskate.on(\"draw\", function(d) ... end)");
        return plugin;
    }
    static void add(lua_State* L, Plugin& plugin, plugin_ui::Shape shape) {
        plugin.shape_text_ += shape.text.size();
        if (plugin.shapes_.size() >= plugin.limits_.shapes || plugin.shape_text_ > plugin.limits_.shape_text_bytes) {
            if (const double now = plugin.services_.now_ms(); now - plugin.shapes_warned_at_ >= 10000) {
                plugin.shapes_warned_at_ = now;
                plugin.log(LogLevel::warn, plugin.shapes_.size() >= plugin.limits_.shapes
                                               ? "more than " + std::to_string(plugin.limits_.shapes) + " shapes in a frame: the rest are left out"
                                               : "more than " + std::to_string(plugin.limits_.shape_text_bytes >> 10) +
                                                     " KB of text in a frame's shapes: the rest are left out");
            }
            return;
        }
        (void)L;
        plugin.shapes_.push_back(std::move(shape));
    }
    static int text_shape(lua_State* L, plugin_ui::Shape::Kind kind) {
        auto& plugin = drawing(L);
        plugin_ui::Shape shape{kind, {point(L, 2, "the text's place")}, 0, checked_text(L, 3, 500, "a drawn text"), style_of(L, 4)};
        add(L, plugin, std::move(shape));
        return 0;
    }
    static int two_points(lua_State* L, plugin_ui::Shape::Kind kind) {
        auto& plugin = drawing(L);
        add(L, plugin, {kind, {point(L, 2, "a shape's first point"), point(L, 3, "a shape's second point")}, 0, {}, style_of(L, 4)});
        return 0;
    }
    static int circle_shape(lua_State* L, plugin_ui::Shape::Kind kind) {
        auto& plugin = drawing(L);
        const float radius = finite(L, 3, "the radius");
        if (radius < 0) luaL_argerror(L, 3, "a radius is 0 or more");
        add(L, plugin, {kind, {point(L, 2, "the circle's centre")}, radius, {}, style_of(L, 4)});
        return 0;
    }
    // A picture of the package's own (PNG or JPG), as the overlay loads it: its file's full path.
    static std::string image_file(lua_State* L, Plugin& plugin, int index) {
        const auto path = checked_text(L, index, 200, "a picture's path");
        if (const auto found = plugin.image_files_.find(path); found != plugin.image_files_.end()) return found->second;
        if (plugin.image_files_.size() >= plugin.limits_.images)
            luaL_error(L, "a plugin draws at most %d different pictures", static_cast<int>(plugin.limits_.images));
        auto extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") luaL_error(L, "a picture is a .png or .jpg file");
        const auto file = package_file(L, plugin, path);
        std::error_code code;
        if (!std::filesystem::is_regular_file(file, code)) luaL_error(L, "\"%s\" is not a file in the plugin's package", path.c_str());
        const auto utf8 = file.u8string();
        return plugin.image_files_.emplace(path, std::string(utf8.begin(), utf8.end())).first->second;
    }
    // d:image(path, top_left, bottom_right, style): style's colour tints it, rounding rounds it.
    static int image(lua_State* L) {
        auto& plugin = drawing(L);
        auto file = image_file(L, plugin, 2);
        add(L, plugin, {plugin_ui::Shape::Kind::image, {point(L, 3, "a picture's top left"), point(L, 4, "a picture's bottom right")}, 0,
                        std::move(file), style_of(L, 5)});
        return 0;
    }
    // d:world_image(path, position, height): standing at a world position, `height` metres tall.
    static int world_image(lua_State* L) {
        auto& plugin = drawing(L);
        auto file = image_file(L, plugin, 2);
        const float height = finite(L, 4, "the picture's height");
        if (!(height > 0 && height <= 1000)) luaL_argerror(L, 4, "a height is more than 0 and at most 1000 metres");
        add(L, plugin, {plugin_ui::Shape::Kind::world_image, {point(L, 3, "the picture's place")}, height, std::move(file), style_of(L, 5)});
        return 0;
    }
    // d:game_image(name, top_left, bottom_right, style): one of the game's own pictures; style.fit
    // "contain" (the default), "shape" or "panel", style.edge a panel's corner size in pixels.
    static int game_image(lua_State* L) {
        auto& plugin = drawing(L);
        auto name = checked_text(L, 2, 40, "a game picture's name");
        if (std::find(plugin_ui::game_images.begin(), plugin_ui::game_images.end(), name) == plugin_ui::game_images.end()) {
            std::string known;
            for (const auto one : plugin_ui::game_images) known += (known.empty() ? "" : ", ") + std::string(one);
            luaL_error(L, "the game's pictures are %s", known.c_str());
        }
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::game_image, {point(L, 3, "a picture's top left"), point(L, 4, "a picture's bottom right")}, 0,
                               std::move(name), style_of(L, 5)};
        if (lua_istable(L, 5)) {
            if (lua_getfield(L, 5, "fit") != LUA_TNIL) {
                const std::string fit = luaL_checkstring(L, -1);
                if (fit == "contain") shape.fit = 0;
                else if (fit == "shape") shape.fit = 1;
                else if (fit == "panel") shape.fit = 2;
                else luaL_error(L, "fit is \"contain\", \"shape\" or \"panel\"");
            }
            lua_pop(L, 1);
            if (lua_getfield(L, 5, "edge") != LUA_TNIL) shape.edge = std::clamp(finite(L, -1, "edge"), 0.0f, 400.0f);
            lua_pop(L, 1);
            if (lua_getfield(L, 5, "frame") != LUA_TNIL) {
                const double frame = luaL_checknumber(L, -1);
                if (shape.text != "loading_board") luaL_error(L, "only loading_board has frames");
                if (!(frame >= 0 && frame < plugin_ui::loading_board_frames) || frame != std::floor(frame))
                    luaL_error(L, "the loading board's frame is a whole number from 0 to %d", plugin_ui::loading_board_frames - 1);
                shape.frame = static_cast<int>(frame);
            }
            lua_pop(L, 1);
        }
        add(L, plugin, std::move(shape));
        return 0;
    }
    // d:gradient(top_left, bottom_right, {top_left, top_right, bottom_right, bottom_left}): a box
    // shading between the colours of its corners.
    static int gradient(lua_State* L) {
        auto& plugin = drawing(L);
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::gradient, {point(L, 2, "a box's top left"), point(L, 3, "a box's bottom right")}};
        luaL_checktype(L, 4, LUA_TTABLE);
        for (int i = 1; i <= 4; ++i) {
            lua_rawgeti(L, 4, i);
            if (lua_isnil(L, -1)) luaL_argerror(L, 4, "a gradient has four colours: top left, top right, bottom right, bottom left");
            shape.colours.push_back(colour_of(L, -1));
            lua_pop(L, 1);
        }
        add(L, plugin, std::move(shape));
        return 0;
    }
    // d:skeleton(paint, alpha): the local skater's x-ray (reskate.skater.xray has it ready), each body
    // named in `paint` in its colour, the rest left out; `alpha` (0 to 1) fades it all.
    static int skeleton(lua_State* L) {
        auto& plugin = drawing(L);
        luaL_checktype(L, 2, LUA_TTABLE);
        const double alpha = luaL_optnumber(L, 3, 1.0);
        if (!(alpha >= 0 && alpha <= 1)) luaL_argerror(L, 3, "alpha is 0 to 1");
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::skeleton};
        shape.colours.assign(plugin_ui::body_names.size(), 0);
        shape.style.colour = 0x00ffffffu | static_cast<std::uint32_t>(alpha * 255.0 + 0.5) << 24;
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "the skeleton's paint is by body name");
            const std::string name = lua_tostring(L, -2);
            const auto found = std::find(plugin_ui::body_names.begin() + 1, plugin_ui::body_names.end(), name);
            if (found == plugin_ui::body_names.end()) luaL_error(L, "\"%s\" is not a body: the bodies are body_hit's names", name.c_str());
            shape.colours[static_cast<std::size_t>(found - plugin_ui::body_names.begin())] = colour_of(L, -1);
            lua_pop(L, 1);
        }
        add(L, plugin, std::move(shape));
        return 0;
    }
    static int text(lua_State* L) { return text_shape(L, plugin_ui::Shape::Kind::text); }
    static int world_text(lua_State* L) { return text_shape(L, plugin_ui::Shape::Kind::world_text); }
    static int rect(lua_State* L) { return two_points(L, plugin_ui::Shape::Kind::rect); }
    static int line(lua_State* L) { return two_points(L, plugin_ui::Shape::Kind::line); }
    static int world_line(lua_State* L) { return two_points(L, plugin_ui::Shape::Kind::world_line); }
    static int circle(lua_State* L) { return circle_shape(L, plugin_ui::Shape::Kind::circle); }
    static int world_circle(lua_State* L) { return circle_shape(L, plugin_ui::Shape::Kind::world_circle); }
    static int poly(lua_State* L) {
        auto& plugin = drawing(L);
        luaL_checktype(L, 2, LUA_TTABLE);
        const int count = lua_objlen(L, 2);
        if (count < 3 || count > 64) luaL_argerror(L, 2, "a polygon has 3 to 64 points");
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::poly, {}, 0, {}, style_of(L, 3)};
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            shape.points.push_back(point(L, -1, "a polygon's point"));
            lua_pop(L, 1);
        }
        add(L, plugin, std::move(shape));
        return 0;
    }
    static int size(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "overlay", "the draw size");
        const auto display = plugin.services_.display();
        lua_pushvector(L, display[0], display[1], 0);
        return 1;
    }
    static int world_to_screen(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "overlay", "world_to_screen");
        const auto on = plugin.services_.world_to_screen(point(L, 2, "the world position"));
        // A point just in front of the camera and off to the side lands millions of pixels out,
        // where the drawing functions refuse it; nil here, as for a point behind the camera, so
        // a plugin that draws what it gets back keeps running.
        const auto drawable = [](float v) { return std::isfinite(v) && std::fabs(v) <= 1e6f; };
        if (on && drawable((*on)[0]) && drawable((*on)[1]) && drawable((*on)[2])) lua_pushvector(L, (*on)[0], (*on)[1], (*on)[2]);
        else lua_pushnil(L);
        return 1;
    }
    // d:rough_rect(top_left, bottom_right, style): skate.'s tile with hand-cut edges; style.seed picks
    // its edge (the same seed, the same edge), style.scale its roughness (1 at 1080 lines).
    static float scale_field(lua_State* L, int index) {
        if (!lua_istable(L, index)) return 1;
        float scale = 1;
        if (lua_getfield(L, index, "scale") != LUA_TNIL) scale = std::clamp(finite(L, -1, "scale"), 0.1f, 10.0f);
        lua_pop(L, 1);
        return scale;
    }
    static std::uint32_t seed_field(lua_State* L, int index) {
        if (!lua_istable(L, index)) return 1;
        std::uint32_t seed = 1;
        if (lua_getfield(L, index, "seed") != LUA_TNIL) {
            const double v = luaL_checknumber(L, -1);
            if (!(v >= 0 && v <= 4294967295.0)) luaL_error(L, "a seed is a whole number from 0");
            seed = static_cast<std::uint32_t>(v);
        }
        lua_pop(L, 1);
        return seed;
    }
    static int rough_rect(lua_State* L) {
        auto& plugin = drawing(L);
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::rough_rect, {point(L, 2, "a tile's top left"), point(L, 3, "a tile's bottom right")}, scale_field(L, 4),
                               {}, style_of(L, 4)};
        shape.seed = seed_field(L, 4);
        add(L, plugin, std::move(shape));
        return 0;
    }
    // d:scribble(top_left, bottom_right, style): the dry-brush scribble skate. draws over a tile.
    static int scribble(lua_State* L) {
        auto& plugin = drawing(L);
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::scribble, {point(L, 2, "a scribble's top left"), point(L, 3, "a scribble's bottom right")},
                               scale_field(L, 4), {}, style_of(L, 4)};
        shape.seed = seed_field(L, 4);
        add(L, plugin, std::move(shape));
        return 0;
    }
    // d:halftone(top_left, columns, rows, style): a screen-print of dots 7 pixels apart, rows offset.
    static int halftone(lua_State* L) {
        auto& plugin = drawing(L);
        const double columns = luaL_checknumber(L, 3), rows = luaL_checknumber(L, 4);
        if (!(columns >= 1 && columns <= 64 && rows >= 1 && rows <= 64)) luaL_error(L, "a halftone is 1 to 64 dots each way");
        plugin_ui::Shape shape{plugin_ui::Shape::Kind::halftone, {point(L, 2, "a halftone's top left")}, 0, {}, style_of(L, 5)};
        shape.columns = static_cast<int>(columns);
        shape.rows = static_cast<int>(rows);
        add(L, plugin, std::move(shape));
        return 0;
    }
    // d:clip(top_left, bottom_right): what is drawn after it shows only inside the box, until d:clip()
    // (or the end of the frame).
    static int clip(lua_State* L) {
        auto& plugin = drawing(L);
        if (lua_isnone(L, 2)) {
            add(L, plugin, {plugin_ui::Shape::Kind::unclip, {}});
            return 0;
        }
        add(L, plugin, {plugin_ui::Shape::Kind::clip, {point(L, 2, "a clip's top left"), point(L, 3, "a clip's bottom right")}});
        return 0;
    }
    // d:measure(text, style): how wide and tall the text is drawn, in the window's pixels.
    static int measure(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "overlay", "d:measure");
        const auto text = checked_text(L, 2, 500, "a measured text");
        const auto style = style_of(L, 3);
        const float k = plugin_ui::display()[1] / 1080.0f;
        const auto size = plugin_ui::measure(style.font, style.size * k, text, style.wrap * k);
        lua_pushvector(L, size[0], size[1], 0);
        return 1;
    }
    // overlay: a toast, ReSkate's notices in the top left. Three in 5 s; false past them.
    static int toast(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "overlay", "reskate.ui.toast");
        const auto message = checked_text(L, 1, 200, "a toast");
        const std::string level = luaL_optstring(L, 2, "info");
        if (level != "info" && level != "warn" && level != "error") luaL_argerror(L, 2, "the level is \"info\", \"warn\" or \"error\"");
        const double now = plugin.services_.now_ms();
        std::erase_if(plugin.toasts_, [&](double at) { return now - at >= 5000; });
        if (static_cast<int>(plugin.toasts_.size()) >= plugin.limits_.toasts) {
            lua_pushboolean(L, 0);
            return 1;
        }
        plugin.toasts_.push_back(now);
        plugin.services_.toast(plugin.id(), level == "error" ? LogLevel::error : level == "warn" ? LogLevel::warn : LogLevel::info, message);
        lua_pushboolean(L, 1);
        return 1;
    }

    // ---- menu: panels ----------------------------------------------------------------------
    // menu.page(title, build): the plugin's own page in the menu's PLUGINS section, `title` its name
    // there. Its panels show on it as cards, under what the page builds.
    static int page(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.page");
        const auto title = checked_text(L, 1, 24, "a page's title");
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (title.empty()) luaL_argerror(L, 1, "a page needs a title");
        if (plugin.page_) luaL_error(L, "a plugin has one page");
        for (const auto& entry : plugin.panels_)
            if (entry.title == title) luaL_error(L, "the panel \"%s\" has the page's title", title.c_str());
        lua_pushvalue(L, 2);
        plugin.page_ = Plugin::PanelEntry{title, lua_ref(L, -1)};
        lua_pop(L, 1);
        return 0;
    }
    static int panel(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "menu", "reskate.menu.panel");
        const auto title = checked_text(L, 1, 40, "a panel's title");
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (title.empty()) luaL_argerror(L, 1, "a panel needs a title");
        if (plugin.panels_.size() >= plugin.limits_.panels) luaL_error(L, "more than %d panels", static_cast<int>(plugin.limits_.panels));
        if (plugin.page_ && plugin.page_->title == title) luaL_error(L, "the page has the title \"%s\"", title.c_str());
        for (const auto& entry : plugin.panels_)
            if (entry.title == title) luaL_error(L, "the panel \"%s\" is there already", title.c_str());
        lua_pushvalue(L, 2);
        plugin.panels_.push_back({title, lua_ref(L, -1)});
        lua_pop(L, 1);
        return 0;
    }
    static Plugin& building(lua_State* L) {
        auto& plugin = plugin_of(L);
        if (!plugin.building_) luaL_error(L, "widgets are only made inside a page's or a panel's function");
        const auto most = plugin.building_->page ? plugin.limits_.page_widgets : plugin.limits_.widgets;
        if (plugin.building_->widgets.size() >= most)
            luaL_error(L, "a %s has at most %d widgets", plugin.building_->page ? "page" : "panel", static_cast<int>(most));
        return plugin;
    }
    static std::string widget_id(lua_State* L, Plugin& plugin, int index) {
        const auto id = checked_text(L, index, 32, "a widget's id");
        if (id.empty()) luaL_argerror(L, index, "a widget needs an id");
        for (const auto& widget : plugin.building_->widgets)
            if (widget.id == id) luaL_error(L, "two widgets in one panel have the id \"%s\"", id.c_str());
        return id;
    }
    static int plain(lua_State* L, plugin_ui::Widget::Kind kind) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{kind};
        if (kind != plugin_ui::Widget::Kind::separator) widget.text = checked_text(L, 2, 300, "a widget's text");
        plugin.building_->widgets.push_back(std::move(widget));
        return 0;
    }
    static int heading(lua_State* L) { return plain(L, plugin_ui::Widget::Kind::heading); }
    static int label(lua_State* L) { return plain(L, plugin_ui::Widget::Kind::label); }
    static int separator(lua_State* L) { return plain(L, plugin_ui::Widget::Kind::separator); }
    static int note(lua_State* L) { return plain(L, plugin_ui::Widget::Kind::note); }
    static int warn(lua_State* L) { return plain(L, plugin_ui::Widget::Kind::warn); }
    // ui:card(title, subtitle): a titled tile; the widgets after it go in it, until the next card.
    static int card(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::card};
        widget.text = checked_text(L, 2, 60, "a card's title");
        if (!lua_isnoneornil(L, 3)) widget.help = checked_text(L, 3, 120, "a card's subtitle");
        plugin.building_->widgets.push_back(std::move(widget));
        return 0;
    }
    // ui:info(label, value): a status line, the label muted and its value beside it.
    static int info(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::info};
        widget.text = checked_text(L, 2, 60, "an info line's label");
        widget.help = checked_text(L, 3, 200, "an info line's value");
        plugin.building_->widgets.push_back(std::move(widget));
        return 0;
    }
    // ui:tabs(id, names, chosen): the page's tabs; the chosen one's number, from 1. The page builds
    // that tab's widgets after it.
    static int tabs(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::tabs, widget_id(L, plugin, 2)};
        luaL_checktype(L, 3, LUA_TTABLE);
        const int count = lua_objlen(L, 3);
        if (count < 1 || count > 8) luaL_argerror(L, 3, "tabs are 1 to 8 names");
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 3, i);
            widget.choices.push_back(checked_text(L, -1, 24, "a tab's name"));
            lua_pop(L, 1);
        }
        widget.chosen = static_cast<int>(luaL_optinteger(L, 4, 1));
        if (const auto* action = pending(plugin, plugin.pending_, plugin.building_->title, widget.id)) widget.chosen = action->chosen;
        widget.chosen = std::clamp(widget.chosen, 1, count);
        const int chosen = widget.chosen;
        plugin.building_->widgets.push_back(std::move(widget));
        lua_pushinteger(L, chosen);
        return 1;
    }
    // A slider's number as shown: one %f (with a precision, "%.1f") or %d among plain text, "%%" a %.
    static std::string slider_format(lua_State* L, int index) {
        const auto format = checked_text(L, index, 24, "a slider's format");
        int conversions = 0;
        for (std::size_t i = 0; i < format.size(); ++i) {
            if (format[i] != '%') continue;
            if (i + 1 < format.size() && format[i + 1] == '%') {
                ++i;
                continue;
            }
            std::size_t j = i + 1;
            if (j < format.size() && format[j] == '.') {
                ++j;
                if (j < format.size() && format[j] >= '0' && format[j] <= '9') ++j;
            }
            if (j >= format.size() || (format[j] != 'f' && format[j] != 'd')) luaL_error(L, "a slider's format has one %%f or %%d");
            ++conversions;
            i = j;
        }
        if (conversions != 1) luaL_error(L, "a slider's format has one %%f or %%d");
        return format;
    }
    // ui:bar(text, value, minimum, maximum): a gauge, read only.
    static int bar(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::bar};
        widget.text = checked_text(L, 2, 60, "a bar's text");
        widget.minimum = finite(L, 4, "the minimum");
        widget.maximum = finite(L, 5, "the maximum");
        if (!(widget.maximum > widget.minimum)) luaL_argerror(L, 5, "the maximum must be above the minimum");
        widget.value = std::clamp(finite(L, 3, "the value"), widget.minimum, widget.maximum);
        plugin.building_->widgets.push_back(std::move(widget));
        return 0;
    }
    // Each returns its value: what the player set since the last build, else what it was given.
    static int button(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::button, widget_id(L, plugin, 2), checked_text(L, 3, 60, "a button's text")};
        widget.on = lua_toboolean(L, 4) != 0; // the card's main action
        const bool pressed = pending(plugin, plugin.pending_, plugin.building_->title, widget.id) != nullptr;
        plugin.building_->widgets.push_back(std::move(widget));
        lua_pushboolean(L, pressed);
        return 1;
    }
    static int toggle(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::toggle, widget_id(L, plugin, 2), checked_text(L, 3, 60, "a toggle's text")};
        widget.on = lua_toboolean(L, 4) != 0;
        if (!lua_isnoneornil(L, 5)) widget.help = checked_text(L, 5, 300, "a toggle's help");
        if (const auto* action = pending(plugin, plugin.pending_, plugin.building_->title, widget.id)) widget.on = action->on;
        const bool on = widget.on;
        plugin.building_->widgets.push_back(std::move(widget));
        lua_pushboolean(L, on);
        return 1;
    }
    static int slider(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::slider, widget_id(L, plugin, 2), checked_text(L, 3, 60, "a slider's text")};
        widget.minimum = finite(L, 5, "the minimum");
        widget.maximum = finite(L, 6, "the maximum");
        if (!(widget.maximum > widget.minimum)) luaL_argerror(L, 6, "the maximum must be above the minimum");
        widget.value = finite(L, 4, "the value");
        if (!lua_isnoneornil(L, 7)) {
            widget.format = slider_format(L, 7);
            // The integer form shows the value rounded: ImGui's float slider takes "%.0f".
            if (const auto at = widget.format.find("%d"); at != std::string::npos) widget.format.replace(at, 2, "%.0f");
        }
        if (const auto* action = pending(plugin, plugin.pending_, plugin.building_->title, widget.id)) widget.value = action->value;
        widget.value = std::clamp(widget.value, widget.minimum, widget.maximum);
        const float value = widget.value;
        plugin.building_->widgets.push_back(std::move(widget));
        lua_pushnumber(L, value);
        return 1;
    }
    static int dropdown(lua_State* L) {
        auto& plugin = building(L);
        plugin_ui::Widget widget{plugin_ui::Widget::Kind::dropdown, widget_id(L, plugin, 2), checked_text(L, 3, 60, "a dropdown's text")};
        luaL_checktype(L, 4, LUA_TTABLE);
        const int count = lua_objlen(L, 4);
        if (count < 1 || count > 32) luaL_argerror(L, 4, "a dropdown has 1 to 32 choices");
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 4, i);
            widget.choices.push_back(checked_text(L, -1, 60, "a choice"));
            lua_pop(L, 1);
        }
        widget.chosen = static_cast<int>(luaL_optinteger(L, 5, 1));
        if (!lua_isnoneornil(L, 6)) widget.help = checked_text(L, 6, 300, "a dropdown's help");
        if (const auto* action = pending(plugin, plugin.pending_, plugin.building_->title, widget.id)) widget.chosen = action->chosen;
        widget.chosen = std::clamp(widget.chosen, 1, count);
        const int chosen = widget.chosen;
        plugin.building_->widgets.push_back(std::move(widget));
        lua_pushinteger(L, chosen);
        return 1;
    }

    // ---- audio.ui ----------------------------------------------------------------------------
    static int play_ui(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "audio.ui", "reskate.audio.play_ui");
        const std::string name = luaL_checkstring(L, 1);
        const double now = plugin.services_.now_ms();
        if (now - plugin.sound_second_ >= 1000) {
            plugin.sound_second_ = now;
            plugin.sounds_ = 0;
        }
        if (++plugin.sounds_ > plugin.limits_.sounds_per_second) {
            lua_pushboolean(L, 0);
            return 1;
        }
        if (!plugin.services_.ui_sound(name))
            luaL_error(L, "there is no menu sound \"%s\": select, cancel, navigate, toggle, tab, countdown or go", name.c_str());
        lua_pushboolean(L, 1);
        return 1;
    }

    // ---- mods.read ---------------------------------------------------------------------------
    static int installed(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "mods.read", "reskate.mods.installed");
        std::string wanted = luaL_checkstring(L, 1);
        const auto lower = [](std::string text) {
            for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            return text;
        };
        wanted = lower(wanted);
        for (const auto& mod : plugin.services_.mods())
            if (lower(mod.folder) == wanted || lower(mod.title) == wanted) {
                lua_pushstring(L, mod.on ? "on" : "off");
                return 1;
            }
        lua_pushnil(L);
        return 1;
    }
    static int mods_list(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "mods.read", "reskate.mods.list");
        const auto mods = plugin.services_.mods();
        lua_createtable(L, static_cast<int>(mods.size()), 0);
        for (std::size_t i = 0; i < mods.size(); ++i) {
            lua_createtable(L, 0, 3);
            lua_pushlstring(L, mods[i].folder.data(), mods[i].folder.size());
            lua_setfield(L, -2, "folder");
            lua_pushlstring(L, mods[i].title.data(), mods[i].title.size());
            lua_setfield(L, -2, "title");
            lua_pushboolean(L, mods[i].on);
            lua_setfield(L, -2, "on");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    // ---- session.read --------------------------------------------------------------------------
    static const SessionState& session(lua_State* L, const char* what) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "session.read", what);
        return plugin.services_.session();
    }
    static int active(lua_State* L) {
        lua_pushboolean(L, session(L, "reskate.session.active").active);
        return 1;
    }
    static int dedicated(lua_State* L) {
        lua_pushboolean(L, session(L, "reskate.session.is_dedicated").dedicated);
        return 1;
    }
    static int players(lua_State* L) {
        const auto& s = session(L, "reskate.session.players");
        lua_createtable(L, static_cast<int>(s.players.size()), 0);
        for (std::size_t i = 0; i < s.players.size(); ++i) {
            push_player(L, s.players[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    static int find_player(lua_State* L, const char* what, bool SessionPlayer::*flag) {
        const auto& s = session(L, what);
        for (const auto& player : s.players)
            if (player.*flag) {
                push_player(L, player);
                return 1;
            }
        lua_pushnil(L);
        return 1;
    }
    static int me(lua_State* L) { return find_player(L, "reskate.session.me", &SessionPlayer::me); }
    static int host(lua_State* L) { return find_player(L, "reskate.session.host", &SessionPlayer::host); }

    // ---- net.session ---------------------------------------------------------------------------
    // send(to, message): to is "all", a player's id, or a player; the message a string or buffer.
    static int send(lua_State* L) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "net.session", "reskate.net.send");
        std::uint64_t to = 0;
        if (lua_istable(L, 1)) {
            lua_getfield(L, 1, "id");
            const std::string id = luaL_checkstring(L, -1);
            lua_pop(L, 1);
            if (std::from_chars(id.data(), id.data() + id.size(), to).ec != std::errc{} || !to) luaL_argerror(L, 1, "not a player");
        } else {
            const std::string text = luaL_checkstring(L, 1);
            if (text == "server") {
                if (plugin.services_.authority()) luaL_argerror(L, 1, "the server copy is the server: send to \"all\" or a player");
                to = wire::authority;
            } else if (text != "all" && (std::from_chars(text.data(), text.data() + text.size(), to).ec != std::errc{} || to <= wire::authority))
                luaL_argerror(L, 1, "send to \"all\", \"server\", a player's id, or a player");
        }
        std::string payload;
        if (lua_isbuffer(L, 2)) {
            std::size_t length = 0;
            const void* bytes = lua_tobuffer(L, 2, &length);
            payload.assign(static_cast<const char*>(bytes), length);
        } else {
            std::size_t length = 0;
            const char* text = luaL_checklstring(L, 2, &length);
            payload.assign(text, length);
        }
        if (payload.size() > plugin.limits_.message_bytes)
            luaL_error(L, "a message is at most %d bytes", static_cast<int>(plugin.limits_.message_bytes));
        const double now = plugin.services_.now_ms();
        if (now - plugin.message_second_ >= 1000) {
            plugin.message_second_ = now;
            plugin.message_bytes_ = 0;
        }
        plugin.message_bytes_ += payload.size() + wire::header;
        if (plugin.message_bytes_ > plugin.limits_.message_bytes_per_second)
            luaL_error(L, "more than %d KB of messages in a second", static_cast<int>(plugin.limits_.message_bytes_per_second >> 10));
        lua_pushboolean(L, plugin.services_.send(plugin.channel(), to, payload));
        return 1;
    }
    static void install(Plugin& plugin, lua_State* L);
};

void install_ui_api(Plugin& plugin, lua_State* L) { UiApi::install(plugin, L); }

void UiApi::install(Plugin& plugin, lua_State* L) {
    // The reskate table is on top of the stack.
    using F = UiApi;
    push_functions(L, {{"text", F::text}, {"rect", F::rect}, {"line", F::line}, {"circle", F::circle}, {"poly", F::poly},
                       {"world_text", F::world_text}, {"world_line", F::world_line}, {"world_circle", F::world_circle},
                       {"image", F::image}, {"world_image", F::world_image}, {"game_image", F::game_image}, {"rough_rect", F::rough_rect}, {"scribble", F::scribble}, {"halftone", F::halftone},
                       {"clip", F::clip}, {"measure", F::measure},
                       {"gradient", F::gradient}, {"skeleton", F::skeleton}, {"size", F::size}, {"world_to_screen", F::world_to_screen}});
    plugin.draw_table_ = lua_ref(L, -1);
    lua_pop(L, 1);

    push_functions(L, {{"heading", F::heading}, {"label", F::label}, {"separator", F::separator}, {"button", F::button},
                       {"toggle", F::toggle}, {"slider", F::slider}, {"dropdown", F::dropdown}, {"bar", F::bar}, {"card", F::card},
                       {"note", F::note}, {"warn", F::warn}, {"info", F::info}, {"tabs", F::tabs}});
    plugin.ui_table_ = lua_ref(L, -1);
    lua_pop(L, 1);

    add_functions(L, "ui", {{"toast", F::toast}});
    add_functions(L, "menu", {{"panel", F::panel}, {"page", F::page}});
    add_functions(L, "audio", {{"play_ui", F::play_ui}});
    add_functions(L, "mods", {{"installed", F::installed}, {"list", F::mods_list}});
    add_functions(L, "session", {{"active", F::active}, {"is_dedicated", F::dedicated}, {"players", F::players}, {"me", F::me}, {"host", F::host}});
    add_functions(L, "net", {{"send", F::send}});
}
}
