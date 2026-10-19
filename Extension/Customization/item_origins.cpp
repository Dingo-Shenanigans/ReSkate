#include "item_origins.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_item_sources.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace dingosdk::item_origins {
namespace {
namespace fs = std::filesystem;
fs::path utf8_path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }
std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}
struct Table {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const Origin>, std::less<>> by_key;
    fs::file_time_type written{};
    std::uintmax_t size{};
    bool read{};
    ULONGLONG next_look{};
};
Table& table() { static auto* value = new Table; return *value; } // process lifetime

void refresh(Table& t) {
    const auto now = GetTickCount64();
    if (t.read && now < t.next_look) return;
    t.next_look = now + 2000;
    const auto mods_root = mods::catalog().root;
    const auto file = mods_root / mods::generated_folder / mods::item_sources_file;
    std::error_code error;
    const auto written = fs::last_write_time(file, error);
    const auto size = error ? 0 : fs::file_size(file, error);
    if (error) {
        if (t.read && t.by_key.empty()) return;
        t.by_key.clear();
        t.read = true;
        return;
    }
    if (t.read && written == t.written && size == t.size) return;
    t.written = written;
    t.size = size;
    t.read = true;
    t.by_key.clear();
    // The first that lists an item is the one the game has (highest priority first).
    for (const auto& source : mods::read_item_sources(file)) {
        auto origin = std::make_shared<Origin>();
        origin->title = source.title.empty() ? source.mod : source.title;
        std::replace(origin->title.begin(), origin->title.end(), '_', ' ');
        origin->author = source.author;
        origin->version = source.version;
        origin->name = mods::item_source_name(source);
        origin->icon = mods_root / utf8_path(source.folder) / L"icon.png";
        // A file from before the version and folder were recorded: the mod as the launch read it.
        if (source.folder.empty() || source.version.empty()) {
            const auto& loaded = mods::catalog().mods;
            if (const auto mod = std::ranges::find(loaded, source.mod, &mods::Mod::name); mod != loaded.end()) {
                if (origin->version.empty()) origin->version = mod->version;
                if (source.folder.empty()) origin->icon = mod->directory / L"icon.png";
            }
        }
        origin->items = source.keys.size();
        const std::shared_ptr<const Origin> shared = std::move(origin);
        for (const auto& key : source.keys) t.by_key.emplace(key, shared);
    }
}
} // namespace

std::shared_ptr<const Origin> find(std::string_view key) {
    try {
        auto& t = table();
        std::lock_guard lock(t.mutex);
        refresh(t);
        const auto found = t.by_key.find(folded(key));
        return found == t.by_key.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

bool game_item(std::string_view key) {
    const auto& game = content_cache::catalogs();
    return game.available && game.items.contains(folded(key));
}
} // namespace dingosdk::item_origins
