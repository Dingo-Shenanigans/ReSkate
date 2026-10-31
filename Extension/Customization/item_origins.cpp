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
    bool file_ok{}; // the merge file was read; an empty map then means no mod lists the item
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
        // The file is gone. A missing merge is not "no mod lists this", so it must not stay ready.
        if (t.read && !t.file_ok && t.by_key.empty()) return;
        t.by_key.clear();
        t.written = {};
        t.size = 0;
        t.read = true;
        t.file_ok = false;
        return;
    }
    if (t.read && t.file_ok && written == t.written && size == t.size) return;
    // A file that cannot be parsed is kept as the last good read (mod_item_sources.h). Clearing it
    // would make every game item look untouched while a mod's list was simply unreadable.
    const auto sources = mods::read_item_sources_checked(file);
    if (!sources) {
        t.read = true;
        if (!t.file_ok) t.by_key.clear();
        return;
    }
    t.written = written;
    t.size = size;
    t.read = true;
    t.file_ok = true;
    t.by_key.clear();
    // The first that lists an item is the one the game has (highest priority first).
    for (const auto& source : *sources) {
        auto origin = std::make_shared<Origin>();
        origin->title = source.title.empty() ? source.mod : source.title;
        std::replace(origin->title.begin(), origin->title.end(), '_', ' ');
        origin->author = source.author;
        origin->version = source.version;
        origin->folder = source.mod;
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

Lookup lookup(std::span<const std::string> keys) {
    Lookup out;
    try {
        auto& t = table();
        std::lock_guard lock(t.mutex);
        refresh(t);
        const auto& catalogues = content_cache::catalogs();
        // Either half missing: the answer is not ready, and no key is reported as a game item.
        out.ready = t.file_ok && catalogues.available;
        if (!out.ready) return out;
        out.facts.resize(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto key = folded(keys[i]);
            auto& fact = out.facts[i];
            if (const auto found = t.by_key.find(key); found != t.by_key.end() && found->second) {
                const auto& origin = *found->second;
                fact.from_mod = true;
                fact.folder = origin.folder;
                fact.title = origin.title;
                fact.author = origin.author;
                fact.version = origin.version;
            }
            fact.game = catalogues.items.contains(key);
        }
    } catch (...) {}
    return out;
}
} // namespace dingosdk::item_origins
