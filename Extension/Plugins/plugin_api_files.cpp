#include "plugin_api.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "lua.h"
#include "lualib.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <format>

// More of reskate.files (a file's size and time, moving, copying, the Recycle Bin, a file's hash, a
// zip file unpacked), and two parts of the base set: reskate.hash, of bytes the plugin already holds,
// and reskate.zip, of a zip it holds as bytes or one that is a file (plugin_zip_file.h). Paths are the
// data folder's, or "<folder>:<path>" in a folder beside the game the manifest names and the player
// granted (data_path checks both).
namespace dingosdk::plugins {
namespace {
// What a zip may unpack to, whatever it says of itself: this many files, and this many bytes in all.
constexpr std::size_t zip_most_files = 4096, zip_most_bytes = 1024u << 20, zip_most_file_bytes = 64u << 20;
constexpr std::size_t hash_most_bytes = 64u << 20;
}

struct FilesApi {
    static Plugin& files_call(lua_State* L, const char* what) {
        auto& plugin = plugin_of(L);
        need(L, plugin, "files", what);
        if (!within(plugin.services_.now_ms(), plugin.files_second_, plugin.file_calls_, plugin.limits_.file_calls_per_second))
            luaL_error(L, "more than %d file calls in a second", plugin.limits_.file_calls_per_second);
        return plugin;
    }
    // info(path): {size, modified, folder} for a file or a folder there, or nil and why ("not found").
    // `modified` is seconds since 1970, as touch takes it.
    static int info(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.info");
        const auto path = data_path(L, 1, false);
        Game::FileInfo found;
        if (const auto why = game_of(plugin).files_info(plugin.id(), path, found); !why.empty()) return failed(L, why);
        lua_createtable(L, 0, 3);
        field_number(L, "size", static_cast<double>(found.size));
        field_number(L, "modified", found.modified);
        field_bool(L, "folder", found.folder);
        return 1;
    }
    // move(from, to): a file or a folder to another name or folder, in place of a file there (never of
    // a folder). The folders `to` names are made.
    static int move(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.move");
        const auto from = data_path(L, 1, false), to = data_path(L, 2, false);
        return result(L, game_of(plugin).files_move(plugin.id(), from, to));
    }
    // copy(from, to): a file, in place of a file there.
    static int copy(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.copy");
        const auto from = data_path(L, 1, false), to = data_path(L, 2, false);
        return result(L, game_of(plugin).files_copy(plugin.id(), from, to));
    }
    // touch(path, modified): sets a file's time (seconds since 1970), for a plugin that keeps the time a
    // file was really made when it rewrites it.
    static int touch(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.touch");
        const auto path = data_path(L, 1, false);
        const double modified = luaL_checknumber(L, 2);
        if (!std::isfinite(modified) || modified < 0 || modified > 32503680000.0) luaL_argerror(L, 2, "a time is seconds since 1970");
        return result(L, game_of(plugin).files_touch(plugin.id(), path, modified));
    }
    // recycle(paths): files and folders to the Recycle Bin, where the player can get them back: all of
    // them or none. Nothing is ever deleted for good in its place.
    static int recycle(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.recycle");
        std::vector<std::string> paths;
        if (lua_istable(L, 1)) {
            const int count = lua_objlen(L, 1);
            if (count < 1 || count > 256) luaL_argerror(L, 1, "1 to 256 paths");
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, 1, i);
                paths.push_back(data_path(L, -1, false));
                lua_pop(L, 1);
            }
        } else paths.push_back(data_path(L, 1, false));
        return result(L, game_of(plugin).files_recycle(plugin.id(), paths));
    }
    // hash(path): the SHA-256 of a file's bytes as 64 hex characters, without reading it into the plugin.
    static int hash(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.hash");
        const auto path = data_path(L, 1, false);
        std::string digest;
        if (const auto why = game_of(plugin).files_hash(plugin.id(), path, digest); !why.empty()) return failed(L, why);
        lua_pushlstring(L, digest.data(), digest.size());
        return 1;
    }
    // unzip(zip, folder, fn, options): a zip file of the data folder unpacked into a folder of it, each
    // file under its own name. A large zip takes seconds, so it is done off the plugin's thread: true
    // once it is started, and fn(ok, why, count) comes in a later tick, when every file is written or
    // none is. False and why when it cannot be started. options.only: a list of the names to unpack
    // (as the zip names them) instead of all; options.overwrite: a file that is there already is
    // replaced, where otherwise it is an error. At most zip_file::most_going at once.
    static int unzip(lua_State* L) {
        auto& plugin = files_call(L, "reskate.files.unzip");
        zip_file::Request request;
        request.zip = data_path(L, 1, false);
        request.folder = data_path(L, 2, true);
        if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TFUNCTION);
        if (!lua_isnoneornil(L, 4)) luaL_checktype(L, 4, LUA_TTABLE);
        request.overwrite = flag_option(L, 4, "overwrite", false);
        if (lua_istable(L, 4)) {
            lua_getfield(L, 4, "only");
            if (!lua_isnil(L, -1)) {
                if (!lua_istable(L, -1)) luaL_error(L, "only is a list of names");
                const int names = lua_gettop(L), count = lua_objlen(L, names);
                if (count < 1 || count > static_cast<int>(zip_file::most_files)) luaL_error(L, "only is 1 to %d names", static_cast<int>(zip_file::most_files));
                request.all = false;
                request.only.reserve(static_cast<std::size_t>(count));
                for (int i = 1; i <= count; ++i) {
                    lua_rawgeti(L, names, i);
                    if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "only is a list of names");
                    request.only.push_back(checked_text(L, -1, zip_file::most_name_bytes, "a name in a zip"));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        if (plugin.unzips_.size() >= zip_file::most_going) return result(L, std::format("it unpacks {} zips already", zip_file::most_going));
        if (zip_file::Job::alive() >= zip_file::most_going_in_all) return result(L, std::format("{} zips are being unpacked already", zip_file::most_going_in_all));
        auto job = std::make_shared<zip_file::Job>();
        if (const auto why = game_of(plugin).files_unzip(plugin.id(), std::move(request), job); !why.empty()) return result(L, why);
        int callback = 0;
        if (!lua_isnoneornil(L, 3)) {
            lua_pushvalue(L, 3);
            callback = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        plugin.unzips_.push_back({callback, std::move(job)});
        lua_pushboolean(L, 1);
        return 1;
    }
    // The zips that are done, in the order they were asked for: each one's fn(ok, why, count).
    static void deliver(Plugin& plugin) {
        while (plugin.state_ == State::running) {
            const auto done = std::find_if(plugin.unzips_.begin(), plugin.unzips_.end(),
                                           [](const Plugin::UnzipGoing& going) { return going.job->done.load(std::memory_order_acquire); });
            if (done == plugin.unzips_.end()) return;
            const auto taken = std::move(*done);
            plugin.unzips_.erase(done);
            if (!taken.callback) continue;
            const auto& why = taken.job->why;
            lua_getref(plugin.L_, taken.callback);
            lua_unref(plugin.L_, taken.callback);
            lua_pushboolean(plugin.L_, why.empty());
            if (why.empty()) lua_pushnil(plugin.L_);
            else lua_pushlstring(plugin.L_, why.data(), why.size());
            lua_pushnumber(plugin.L_, static_cast<double>(taken.job->count));
            plugin.call(3, "its unzip callback", plugin.limits_.call_ms);
        }
    }

    // ---- reskate.hash: of bytes the plugin holds (no permission) ---------------------------------------
    static std::string_view bytes_of(lua_State* L, int index, const char* what) {
        std::size_t length = 0;
        const char* text = luaL_checklstring(L, index, &length);
        if (length > hash_most_bytes) luaL_error(L, "%s takes at most 64 MB", what);
        return {text, length};
    }
    static int hash_sha256(lua_State* L) {
        (void)plugin_of(L);
        const auto digest = launcher::sha256_bytes(bytes_of(L, 1, "reskate.hash.sha256"));
        lua_pushlstring(L, digest.data(), digest.size());
        return 1;
    }
    static int hash_crc32(lua_State* L) {
        (void)plugin_of(L);
        const auto bytes = bytes_of(L, 1, "reskate.hash.crc32");
        lua_pushnumber(L, static_cast<double>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())));
        return 1;
    }

    // ---- reskate.zip: a zip the plugin holds as bytes (no permission) -----------------------------------
    // entries(bytes): its files as a list of {name, bytes}, in the zip's order, each checked against
    // its CRC; folders are left out. nil and why for bytes that are not a zip, a damaged file in it,
    // or one that would unpack to more than a zip may (4096 files, 64 MB a file, 1 GB in all).
    static int zip_entries(lua_State* L) {
        (void)plugin_of(L);
        std::size_t length = 0;
        const char* bytes = luaL_checklstring(L, 1, &length);
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, bytes, length, 0)) return failed(L, "it is not a zip");
        struct Close {
            mz_zip_archive* zip;
            ~Close() { mz_zip_reader_end(zip); }
        } close{&zip};
        const auto count = mz_zip_reader_get_num_files(&zip);
        if (count > zip_most_files) return failed(L, "it holds more than 4096 files");
        std::size_t total = 0;
        std::vector<std::pair<std::string, std::string>> files;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) return failed(L, "its list of files is damaged");
            if (stat.m_is_directory) continue;
            if (!stat.m_is_supported) return failed(L, std::string(stat.m_filename) + " in it is packed in a way that cannot be read");
            if (stat.m_uncomp_size > zip_most_file_bytes) return failed(L, std::string(stat.m_filename) + " in it is larger than 64 MB");
            total += static_cast<std::size_t>(stat.m_uncomp_size);
            if (total > zip_most_bytes) return failed(L, "it unpacks to more than 1 GB");
            std::string data(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
            // (miniz checks the CRC as it unpacks.)
            if (!mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0)) return failed(L, std::string(stat.m_filename) + " in it is damaged");
            files.emplace_back(stat.m_filename, std::move(data));
        }
        lua_createtable(L, static_cast<int>(files.size()), 0);
        for (std::size_t i = 0; i < files.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushlstring(L, files[i].first.data(), files[i].first.size());
            lua_setfield(L, -2, "name");
            lua_pushlstring(L, files[i].second.data(), files[i].second.size());
            lua_setfield(L, -2, "bytes");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
    // list(path): the files of a zip that is a file of the data folder (the files permission), as a
    // list of {name, size, packed, crc} in the zip's order, folders left out. It is read from the
    // zip's directory alone, so nothing is unpacked or checked, and a zip of hundreds of megabytes
    // lists at once. nil and why for a file that is not there, is not a zip, or holds more files than
    // a zip may.
    static int zip_list(lua_State* L) {
        auto& plugin = files_call(L, "reskate.zip.list");
        const auto path = data_path(L, 1, false);
        std::vector<zip_file::Entry> entries;
        {
            const Plugin::Untimed untimed{plugin}; // reading it
            if (const auto why = game_of(plugin).zip_list(plugin.id(), path, entries); !why.empty()) return failed(L, why);
        }
        if (entries.size() > zip_file::most_files) return failed(L, std::format("it holds more than {} files", zip_file::most_files));
        lua_createtable(L, static_cast<int>(entries.size()), 0);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            lua_createtable(L, 0, 4);
            field_text(L, "name", entries[i].name);
            field_number(L, "size", static_cast<double>(entries[i].size));
            field_number(L, "packed", static_cast<double>(entries[i].packed));
            field_number(L, "crc", static_cast<double>(entries[i].crc));
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }
};

void install_files_api(Plugin&, lua_State* L) {
    using F = FilesApi;
    add_functions(L, "files", {{"info", F::info}, {"move", F::move}, {"copy", F::copy}, {"touch", F::touch}, {"recycle", F::recycle}, {"hash", F::hash},
                               {"unzip", F::unzip}});
    add_functions(L, "hash", {{"sha256", F::hash_sha256}, {"crc32", F::hash_crc32}});
    add_functions(L, "zip", {{"entries", F::zip_entries}, {"list", F::zip_list}});
}
void deliver_unzips(Plugin& plugin) { FilesApi::deliver(plugin); }
}
