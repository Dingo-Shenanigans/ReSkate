#include "plugin_api.h"
#include "plugin_game.h"
#include "lualib.h"
#include <cmath>
#include <filesystem>

// reskate.viewmodel: a rigged, animated model drawn in front of the camera (Extension/UI/Overlay/viewmodel.h),
// as shooters draw the player's arms and gun; made with contrib/viewmodel_export.py. Positions here
// are in the view: x right, y up, z forward, metres from the eye.
namespace dingosdk::plugins {
namespace {
// A number field of the table at `index`, kept when absent, within [low, high].
void number_field(lua_State* L, int index, const char* name, float low, float high, float& out) {
    lua_getfield(L, index, name);
    if (!lua_isnil(L, -1)) {
        const double value = luaL_checknumber(L, -1);
        if (!(value >= low && value <= high)) luaL_error(L, "the viewmodel's %s is %g to %g", name, static_cast<double>(low), static_cast<double>(high));
        out = static_cast<float>(value);
    }
    lua_pop(L, 1);
}
// A vector field, each part within [low, high].
void vector_field(lua_State* L, int index, const char* name, float low, float high, Vec3& out) {
    lua_getfield(L, index, name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isvector(L, -1)) luaL_error(L, "the viewmodel's %s is a vector", name);
        const float* v = lua_tovector(L, -1);
        for (int i = 0; i < 3; ++i)
            if (!(v[i] >= low && v[i] <= high)) luaL_error(L, "each part of the viewmodel's %s is %g to %g", name, static_cast<double>(low), static_cast<double>(high));
        out = {v[0], v[1], v[2]};
    }
    lua_pop(L, 1);
}
std::array<float,12> affine(lua_State* L,int index,float translation=1000,float scale=16) {
    luaL_checktype(L,index,LUA_TTABLE);
    if (lua_objlen(L,index)!=12) luaL_error(L,"an affine transform is 12 numbers");
    std::array<float,12> matrix{};
    for (int i=0;i<12;++i) {
        lua_rawgeti(L,index,i+1);
        const double value=luaL_checknumber(L,-1);
        if (!(value>=-(i%4==3?translation:scale) && value<=(i%4==3?translation:scale))) luaL_error(L,"a transform exceeds its finite bounds");
        matrix[i]=static_cast<float>(value);lua_pop(L,1);
    }
    return matrix;
}
void independent(lua_State* L,const Vec3& a,const Vec3& b) {
    const auto x=a[1]*b[2]-a[2]*b[1],y=a[2]*b[0]-a[0]*b[2],z=a[0]*b[1]-a[1]*b[0];
    if (x*x+y*y+z*z<1e-6f) luaL_error(L,"forward and up need independent directions");
}
} // namespace

struct ViewmodelApi {
    static Plugin& caller(lua_State* L, const char* what) { return permitted(L, "viewmodel", what); }
    // load(path): reads the package's .rsvm file (and the pictures it names, beside it) in the
    // background; info().status says when it is ready.
    static std::filesystem::path model_path(lua_State* L, Plugin& plugin, int index) {
        const auto path = checked_text(L, index, 200, "a viewmodel's path");
        auto extension = std::filesystem::path(path).extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".rsvm") luaL_error(L, "a viewmodel is a .rsvm file (contrib/viewmodel_export.py makes one)");
        const auto file = package_file(L, plugin, path);
        std::error_code code;
        if (!std::filesystem::is_regular_file(file, code)) luaL_error(L, "\"%s\" is not a file in the plugin's package", path.c_str());
        return file;
    }
    // load(path, {append = {path, ...}}): each static attachment uses pictures beside its own file.
    static int read_model(lua_State* L, bool preparing) {
        auto& plugin = caller(L, preparing ? "reskate.viewmodel.prepare" : "reskate.viewmodel.load");
        if (preparing && lua_isnoneornil(L, 1)) return result(L, game_of(plugin).viewmodel_prepare(plugin.id(), {}, {}));
        const auto file = model_path(L, plugin, 1);
        std::vector<std::filesystem::path> attachments;
        if (!lua_isnoneornil(L, 2)) {
            luaL_checktype(L, 2, LUA_TTABLE);
            lua_getfield(L, 2, "append");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                const int count = lua_objlen(L, -1);
                if (count > 8) luaL_error(L, "a viewmodel accepts up to 8 attachments");
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    const double key = lua_tonumber(L, -2);
                    if (lua_type(L, -2) != LUA_TNUMBER || !(key >= 1 && key <= count && key == std::floor(key)))
                        luaL_error(L, "append is a list of package paths");
                    lua_pop(L, 1);
                }
                for (int i = 1; i <= count; ++i) {
                    lua_rawgeti(L, -1, i);
                    attachments.push_back(model_path(L, plugin, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        return result(L, preparing ? game_of(plugin).viewmodel_prepare(plugin.id(), file, attachments) : game_of(plugin).viewmodel_load(plugin.id(), file, attachments));
    }
    static int load(lua_State* L) {return read_model(L,false);}
    static int prepare(lua_State* L) {return read_model(L,true);}
    static int activate_prepared(lua_State* L) {
        auto& plugin=caller(L,"reskate.viewmodel.activate_prepared");
        return result(L,game_of(plugin).viewmodel_activate_prepared(plugin.id()));
    }
    static int unload(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.unload");
        game_of(plugin).viewmodel_unload(plugin.id());
        return 0;
    }
    static int display(lua_State* L) {
        auto& plugin=caller(L,"reskate.viewmodel.display");
        if (lua_isnoneornil(L,1)) return result(L,game_of(plugin).viewmodel_display(plugin.id(),{},std::nullopt));
        const auto path=checked_text(L,1,200,"a display model path");
        auto ext=std::filesystem::path(path).extension().string();
        for (auto& c:ext) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext!=".rsvm") luaL_error(L,"a display model is a .rsvm file");
        const auto file=package_file(L,plugin,path);std::error_code error;
        if (!std::filesystem::is_regular_file(file,error)) luaL_error(L,"a display model must be a package file");
        luaL_checktype(L,2,LUA_TTABLE);Game::ViewmodelDisplay look;
        vector_field(L,2,"position",-1e7f,1e7f,look.position);
        vector_field(L,2,"forward",-1,1,look.forward);vector_field(L,2,"up",-1,1,look.up);independent(L,look.forward,look.up);
        lua_getfield(L,2,"material");look.material=checked_text(L,-1,63,"a display material name");lua_pop(L,1);
        lua_getfield(L,2,"live");look.live=lua_toboolean(L,-1)!=0;lua_pop(L,1);
        lua_getfield(L,2,"scene_clear");look.scene_clear=lua_toboolean(L,-1)!=0;lua_pop(L,1);
        lua_getfield(L,2,"depth_points");
        if (!lua_isnil(L,-1)) {
            luaL_checktype(L,-1,LUA_TTABLE);const int count=lua_objlen(L,-1);
            if (count>8) luaL_error(L,"at most eight world depth points");
            for (int i=1;i<=count;++i) {
                lua_rawgeti(L,-1,i);const float* v=lua_tovector(L,-1);
                if (!v || !std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || std::abs(v[0])>1e7f || std::abs(v[1])>1e7f || std::abs(v[2])>1e7f) luaL_error(L,"world depth points are bounded vectors");
                look.depth_points.push_back({v[0],v[1],v[2]});lua_pop(L,1);
            }
        }
        lua_pop(L,1);return result(L,game_of(plugin).viewmodel_display(plugin.id(),file,std::move(look)));
    }
    // info(): {status = "loading" | "ready" | "failed", why, clips = {{name, duration}}, bones = {names}},
    // or nil when this plugin has none loaded.
    static int info(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.info");
        const auto found = game_of(plugin).viewmodel_info(plugin.id());
        if (!found) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        field_text(L, "status", found->status);
        if (!found->why.empty()) field_text(L, "why", found->why);
        lua_createtable(L, static_cast<int>(found->clips.size()), 0);
        for (std::size_t i = 0; i < found->clips.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushlstring(L, found->clips[i].first.data(), found->clips[i].first.size());
            lua_setfield(L, -2, "name");
            lua_pushnumber(L, found->clips[i].second);
            lua_setfield(L, -2, "duration");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, "clips");
        push_texts(L, found->bones);
        lua_setfield(L, -2, "bones");
        lua_pushboolean(L,found->render_ready);lua_setfield(L,-2,"render_ready");
        field_text(L,"prepared_status",found->prepared_status.empty()?"none":found->prepared_status);
        if (!found->prepared_why.empty()) field_text(L,"prepared_why",found->prepared_why);
        lua_pushboolean(L,found->prepared_ready);lua_setfield(L,-2,"prepared_ready");
        field_text(L,"display_status",found->display_status.empty()?"none":found->display_status);
        if (!found->display_why.empty()) field_text(L,"display_why",found->display_why);
        return 1;
    }
    // show(look): drawn for the next 300 ms (so every tick), with {offset, yaw, pitch, roll, fov,
    // light, light_colour, sky, ground, ambient, brightness}, each optional.
    static int show(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.show");
        Game::ViewmodelLook look;
        if (!lua_isnoneornil(L, 1)) {
            luaL_checktype(L, 1, LUA_TTABLE);
            vector_field(L, 1, "offset", -5, 5, look.offset);
            number_field(L, 1, "yaw", -360, 360, look.yaw);
            number_field(L, 1, "pitch", -360, 360, look.pitch);
            number_field(L, 1, "roll", -360, 360, look.roll);
            number_field(L, 1, "fov", 20, 120, look.fov);
            vector_field(L, 1, "light", -1e3f, 1e3f, look.light);
            vector_field(L, 1, "light_colour", 0, 4, look.light_colour);
            vector_field(L, 1, "sky", 0, 4, look.sky);
            vector_field(L, 1, "ground", 0, 4, look.ground);
            number_field(L, 1, "ambient", 0, 4, look.ambient);
            number_field(L, 1, "brightness", 0, 4, look.brightness);
            vector_field(L,1,"fog",0,1e7f,look.fog);
            vector_field(L,1,"fog_colour",0,1,look.fog_colour);
            if (look.fog[2]!=0 && (look.fog[1]<=look.fog[0] || look.fog[2]>7))
                luaL_error(L,"fog needs increasing distances and a curve from1 to7");
            lua_getfield(L, 1, "world");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                Vec3 at{};
                vector_field(L, lua_gettop(L), "position", -1e7f, 1e7f, at);
                vector_field(L, lua_gettop(L), "forward", -1, 1, look.world_forward);
                vector_field(L, lua_gettop(L), "up", -1, 1, look.world_up);
                const auto& a = look.world_forward; const auto& b = look.world_up;
                const auto x = a[1]*b[2]-a[2]*b[1], y = a[2]*b[0]-a[0]*b[2], z = a[0]*b[1]-a[1]*b[0];
                if (x*x+y*y+z*z < 1e-6f) luaL_error(L, "the world's forward and up need independent directions");
                look.world_position = at;
                lua_getfield(L,-1,"camera");
                if (!lua_isnil(L,-1)) {
                    luaL_checktype(L,-1,LUA_TTABLE);Vec3 eye{};
                    vector_field(L,lua_gettop(L),"position",-1e7f,1e7f,eye);
                    vector_field(L,lua_gettop(L),"forward",-1,1,look.view_forward);
                    vector_field(L,lua_gettop(L),"up",-1,1,look.view_up);
                    number_field(L,lua_gettop(L),"fov",20,120,look.view_fov);
                    independent(L,look.view_forward,look.view_up);look.view_position=eye;
                }
                lua_pop(L,1);
            }
            lua_pop(L, 1);
            lua_getfield(L,1,"backdrop");
            if (!lua_isnil(L,-1)) {Vec3 colour{};vector_field(L,1,"backdrop",0,1,colour);look.backdrop=colour;}
            lua_pop(L,1);
            lua_getfield(L,1,"inset");
            if (!lua_isnil(L,-1)) {
                if (!look.world_position || !look.view_position) luaL_error(L,"an inset requires world placement and a camera");
                luaL_checktype(L,-1,LUA_TTABLE);const int index=lua_gettop(L);
                Game::ViewmodelLook::Inset inset;
                vector_field(L,index,"position",-1e7f,1e7f,inset.position);
                vector_field(L,index,"forward",-1,1,inset.forward);
                vector_field(L,index,"up",-1,1,inset.up);independent(L,inset.forward,inset.up);
                vector_field(L,index,"centre",0,1,inset.centre);
                vector_field(L,index,"radius",0,1,inset.radius);
                if (inset.radius[0]<.001f || inset.radius[1]<.001f) luaL_error(L,"an inset needs positive horizontal and vertical radii");
                vector_field(L,index,"projection_offset",-2,2,inset.projection_offset);
                number_field(L,index,"fov",20,120,inset.fov);
                number_field(L,index,"background_alpha",0,1,inset.background_alpha);
                for (auto entry : {std::pair{"hidden_bones",&inset.hidden_bones},std::pair{"hidden_materials",&inset.hidden_materials}}) {
                    lua_getfield(L,index,entry.first);
                    if (!lua_isnil(L,-1)) {
                        luaL_checktype(L,-1,LUA_TTABLE);const int count=lua_objlen(L,-1);
                        if (count>256) luaL_error(L,"at most 256 hidden inset names");
                        for (int i=1;i<=count;++i) {lua_rawgeti(L,-1,i);entry.second->push_back(checked_text(L,-1,63,"an inset name"));lua_pop(L,1);}
                    }
                    lua_pop(L,1);
                }
                look.inset=std::move(inset);
            }
            lua_pop(L,1);
            lua_getfield(L,1,"reflection");
            if (!lua_isnil(L,-1)) {
                if (!look.world_position || !look.view_position) luaL_error(L,"a reflection needs world placement and a camera");
                luaL_checktype(L,-1,LUA_TTABLE);const int index=lua_gettop(L);
                Game::ViewmodelLook::Reflection reflection;
                vector_field(L,index,"position",-1e7f,1e7f,reflection.position);
                vector_field(L,index,"normal",-1,1,reflection.normal);
                const auto& n=reflection.normal;
                if (n[0]*n[0]+n[1]*n[1]+n[2]*n[2]<1e-6f) luaL_error(L,"a reflection needs a nonzero plane normal");
                vector_field(L,index,"tint",0,1,reflection.tint);
                number_field(L,index,"time",0,1e7f,reflection.time);
                number_field(L,index,"distortion",0,.05f,reflection.distortion);
                number_field(L,index,"strength",0,1,reflection.strength);
                number_field(L,index,"texture_scale",.01f,100,reflection.texture_scale);
                lua_getfield(L,index,"parts");luaL_checktype(L,-1,LUA_TTABLE);const int count=lua_objlen(L,-1);
                if (count<1 || count>64) luaL_error(L,"a reflection has one to 64 model parts");
                for (int i=1;i<=count;++i) {lua_rawgeti(L,-1,i);reflection.parts.push_back(checked_text(L,-1,63,"a reflective part"));lua_pop(L,1);}
                lua_pop(L,1);
                lua_getfield(L,index,"portal_views");
                if (!lua_isnil(L,-1)) {
                    luaL_checktype(L,-1,LUA_TTABLE);
                    if (count!=2 || lua_objlen(L,-1)!=2) luaL_error(L,"paired portals require two views and two surfaces");
                    for (int i=1;i<=2;++i) {
                        lua_rawgeti(L,-1,i);luaL_checktype(L,-1,LUA_TTABLE);
                        const int view=lua_gettop(L);
                        Game::ViewmodelLook::Reflection::PortalView v;
                        vector_field(L,view,"eye",-1e7f,1e7f,v.eye);
                        vector_field(L,view,"forward",-1,1,v.forward);
                        vector_field(L,view,"up",-1,1,v.up);independent(L,v.forward,v.up);
                        vector_field(L,view,"exit",-1e7f,1e7f,v.exit);
                        vector_field(L,view,"normal",-1,1,v.normal);
                        const auto& axis=v.normal;
                        if (axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]<1e-6f) luaL_error(L,"a portal exit needs a normal");
                        reflection.portal_views.push_back(v);lua_pop(L,1);
                    }
                }
                lua_pop(L,1);look.reflection=std::move(reflection);
            }
            lua_pop(L,1);
            lua_getfield(L, 1, "pose_transform");
            if (!lua_isnil(L,-1)) look.pose_transform=affine(L,lua_gettop(L));
            lua_pop(L,1);
            lua_getfield(L,1,"part_transforms");
            if (!lua_isnil(L,-1)) {
                luaL_checktype(L,-1,LUA_TTABLE);lua_pushnil(L);
                while (lua_next(L,-2)) {
                    if (look.part_transforms.size()>=4096) luaL_error(L,"at most 4096 part transforms");
                    const auto name=checked_text(L,-2,63,"a part name");
                    look.part_transforms.emplace_back(name,affine(L,lua_gettop(L),1e7f,1e4f));
                    lua_pop(L,1);
                }
            }
            lua_pop(L,1);
            lua_getfield(L,1,"hidden_parts");
            if (!lua_isnil(L,-1)) {
                luaL_checktype(L,-1,LUA_TTABLE);const int count=lua_objlen(L,-1);
                if (count>4096) luaL_error(L,"at most 4096 hidden parts");
                for (int i=1;i<=count;++i) {lua_rawgeti(L,-1,i);look.hidden_parts.push_back(checked_text(L,-1,63,"a hidden part name"));lua_pop(L,1);}
            }
            lua_pop(L,1);
            lua_getfield(L,1,"fog_exempt_parts");
            if (!lua_isnil(L,-1)) {
                luaL_checktype(L,-1,LUA_TTABLE);const int count=lua_objlen(L,-1);
                if (count>4096) luaL_error(L,"at most4096 fog-exempt parts");
                for (int i=1;i<=count;++i) {lua_rawgeti(L,-1,i);look.fog_exempt_parts.push_back(checked_text(L,-1,63,"a fog-exempt part name"));lua_pop(L,1);}
            }
            lua_pop(L,1);
            lua_getfield(L,1,"material_colours");
            if (!lua_isnil(L,-1)) {
                luaL_checktype(L,-1,LUA_TTABLE);lua_pushnil(L);
                while (lua_next(L,-2)) {
                    if (look.material_colours.size()>=128) luaL_error(L,"at most 128 material colours");
                    const auto name=checked_text(L,-2,63,"a material name");
                    if (!lua_isvector(L,-1)) luaL_error(L,"a material colour is a vector");
                    const auto* colour=lua_tovector(L,-1);
                    for (int i=0;i<3;++i) if (!(colour[i]>=0 && colour[i]<=1)) luaL_error(L,"material colour components are finite and 0 to 1");
                    look.material_colours.emplace_back(name,std::array<float,3>{colour[0],colour[1],colour[2]});lua_pop(L,1);
                }
            }
            lua_pop(L,1);
            lua_getfield(L,1,"part_effects");
            if (!lua_isnil(L,-1)) {
                luaL_checktype(L,-1,LUA_TTABLE);lua_pushnil(L);
                while (lua_next(L,-2)) {
                    if (look.part_effects.size()>=4096) luaL_error(L,"at most 4096 part effects");
                    const auto name=checked_text(L,-2,63,"a part name");
                    const auto* value=lua_tovector(L,-1);
                    if (!value || !(value[0]>=0 && value[0]<=1) || !std::isfinite(value[1]) || !std::isfinite(value[2]) ||
                        std::abs(value[1])>1e6f || std::abs(value[2])>1e6f) luaL_error(L,"part effects require vector(opacity 0 to 1, UV scroll x/y)");
                    look.part_effects.emplace_back(name,std::array<float,3>{value[0],value[1],value[2]});lua_pop(L,1);
                }
            }
            lua_pop(L,1);
            lua_getfield(L, 1, "contact");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                if (!look.world_position) luaL_error(L, "a contact plane requires world placement");
                Vec3 at{};
                vector_field(L, lua_gettop(L), "position", -1e7f, 1e7f, at);
                vector_field(L, lua_gettop(L), "normal", -1, 1, look.contact_normal);
                number_field(L, lua_gettop(L), "margin", 0.001f, 0.2f, look.contact_margin);
                const auto& n = look.contact_normal;
                if (n[0]*n[0]+n[1]*n[1]+n[2]*n[2] < 1e-6f) luaL_error(L, "the contact normal needs a direction");
                look.contact_position = at;
            }
            lua_pop(L, 1);
            lua_getfield(L, 1, "scene_clear");
            look.scene_clear = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            if (look.scene_clear && !look.world_position) luaL_error(L, "verified scene visibility requires world placement");
            lua_getfield(L, 1, "scene_depth");
            look.scene_depth = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, 1, "depth_points");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                const int count = lua_objlen(L, -1);
                if (count > 8) luaL_error(L, "at most 8 depth points");
                for (int i = 1; i <= count; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (!lua_isvector(L, -1)) luaL_error(L, "a depth point is a vector");
                    const auto* v = lua_tovector(L, -1);
                    for (int k = 0; k < 3; ++k)
                        if (!std::isfinite(v[k]) || std::abs(v[k]) > (look.world_position ? 1e7f : 1000.0f)) luaL_error(L, "a depth point is within 1000 metres");
                    if (!look.world_position && v[2] <= 0.01f) luaL_error(L, "a depth point is in front of the eye");
                    look.depth_points.push_back({v[0], v[1], v[2]});
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, 1, "opacities");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (look.opacities.size() >= 128) luaL_error(L, "at most 128 material opacities");
                    if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "a material opacity is named");
                    const auto name = checked_text(L, -2, 63, "a material's name");
                    const auto value = luaL_checknumber(L, -1);
                    if (!(value >= 0 && value <= 1)) luaL_error(L, "a material opacity is 0 to 1");
                    look.opacities.emplace_back(name, static_cast<float>(value));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, 1, "transforms");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (look.transforms.size() >= 256) luaL_error(L, "at most 256 bone transforms");
                    if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "a bone transform is named");
                    const auto name = checked_text(L, -2, 63, "a bone's name");
                    luaL_checktype(L, -1, LUA_TTABLE);
                    if (lua_objlen(L, -1) != 12) luaL_error(L, "a bone transform is 12 numbers (three rows of four)");
                    std::array<float, 12> matrix{};
                    for (int i = 0; i < 12; ++i) {
                        lua_rawgeti(L, -1, i + 1);
                        const double value = luaL_checknumber(L, -1);
                        const double limit = i % 4 == 3 ? (look.world_position?1e7:1000) : 16;
                        if (!(value >= -limit && value <= limit)) luaL_error(L, "a bone transform exceeds its finite bounds");
                        matrix[i] = static_cast<float>(value);
                        lua_pop(L, 1);
                    }
                    look.transforms.emplace_back(name, matrix);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        return result(L, game_of(plugin).viewmodel_show(plugin.id(), look));
    }
    static int hide(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.hide");
        return result(L, game_of(plugin).viewmodel_show(plugin.id(), std::nullopt));
    }
    // play(clip, {loop = false, speed = 1, fade = 0.1}): from its start, faded in over what played.
    static int play(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.play");
        const auto clip = checked_text(L, 1, 120, "a clip's name");
        bool loop{}, restart{true};
        float speed{1}, fade{0.1f};
        if (!lua_isnoneornil(L, 2)) {
            luaL_checktype(L, 2, LUA_TTABLE);
            lua_getfield(L, 2, "loop");
            loop = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, 2, "restart");
            if (!lua_isnil(L, -1)) restart = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            number_field(L, 2, "speed", 0, 10, speed);
            number_field(L, 2, "fade", 0, 2, fade);
        }
        return result(L, game_of(plugin).viewmodel_play(plugin.id(), clip, loop, speed, fade, restart));
    }
    static int seek(lua_State* L) {
        auto& plugin=caller(L,"reskate.viewmodel.seek");
        const double seconds=luaL_checknumber(L,1);
        if (!std::isfinite(seconds) || seconds<0 || seconds>86400) luaL_error(L,"animation time is 0 to 86400 seconds");
        return result(L,game_of(plugin).viewmodel_seek(plugin.id(),seconds));
    }
    static int stop(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.stop");
        game_of(plugin).viewmodel_stop(plugin.id());
        return 0;
    }
    // state(): {clip, time, duration, done} of what plays, or nil.
    static int state(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.state");
        const auto now = game_of(plugin).viewmodel_state(plugin.id());
        if (!now) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, now->clip.data(), now->clip.size());
        lua_setfield(L, -2, "clip");
        lua_pushnumber(L, now->time);
        lua_setfield(L, -2, "time");
        lua_pushnumber(L, now->duration);
        lua_setfield(L, -2, "duration");
        lua_pushboolean(L, now->done);
        lua_setfield(L, -2, "done");
        return 1;
    }
    // bone(name): {position, x, y, z} of a bone now, in the view with show's placement, or nil.
    static int bone(lua_State* L) {
        auto& plugin = caller(L, "reskate.viewmodel.bone");
        const auto name = checked_text(L, 1, 120, "a bone's name");
        const auto found = game_of(plugin).viewmodel_bone(plugin.id(), name);
        if (!found) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        push_position(L, found->position);
        lua_setfield(L, -2, "position");
        push_position(L, found->x);
        lua_setfield(L, -2, "x");
        push_position(L, found->y);
        lua_setfield(L, -2, "y");
        push_position(L, found->z);
        lua_setfield(L, -2, "z");
        return 1;
    }
};

void install_viewmodel_api(Plugin&, lua_State* L) {
    add_functions(L, "viewmodel", {{"load", ViewmodelApi::load}, {"unload", ViewmodelApi::unload}, {"info", ViewmodelApi::info},
                                   {"show", ViewmodelApi::show}, {"hide", ViewmodelApi::hide}, {"play", ViewmodelApi::play}, {"seek", ViewmodelApi::seek}, {"display",ViewmodelApi::display},
                                   {"prepare",ViewmodelApi::prepare},{"activate_prepared",ViewmodelApi::activate_prepared},
                                   {"stop", ViewmodelApi::stop}, {"state", ViewmodelApi::state}, {"bone", ViewmodelApi::bone}});
}
} // namespace dingosdk::plugins
