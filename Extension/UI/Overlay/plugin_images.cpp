#include "overlay_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/clock.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// Pictures plugins draw (reskate.overlay's d:image), from files in their own packages and data
// folders: the plugin host checked each path is inside one of them. One worker thread decodes them
// (WIC, at most 4096 pixels a side and 16 MB a file); the render thread uploads one a frame into the
// SRV heap's plugin slots, and a picture not drawn for 2 s gives its slot up when another needs it.
// A picture's name here is its file's path, and for one that can change (a data folder's) a "|" and
// a stamp after it, so a file written again is another picture.
namespace dingosdk::overlay::detail {
namespace {
constexpr std::size_t most_pictures = 256;     // known at once, decoded or waiting
constexpr std::uintmax_t most_file_bytes = 16 * 1024 * 1024;
constexpr UINT most_side = 4096;
// What the pictures on the GPU may hold together, as RGBA: past it, one waits for an idle one to go.
constexpr std::uint64_t most_gpu_bytes = 768ull * 1024 * 1024;
constexpr double idle_seconds = 2;

struct Entry {
    enum class Stage { waiting, decoded, uploaded, failed } stage{Stage::waiting};
    CardPixels pixels;
    int slot{-1};
    ImTextureID id{};
    double drawn{};
};
struct Cache {
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, Entry, std::less<>> entries;
    std::deque<std::string> queue;
    std::set<std::string, std::less<>> warned;
    bool worker{};
};
Cache& cache() {
    static auto* value = new Cache;
    return *value;
}
void warn_once(Cache& c, const std::string& path, const char* why) {
    if (!c.warned.insert(path).second) return;
    logging::log(logging::Level::warning, logging::Channel::plugins, "A plugin's picture {} is not drawn: {}.", path, why);
}

void decode_loop() {
    auto& c = cache();
    for (;;) {
        std::string path;
        {
            std::unique_lock lock(c.mutex);
            c.wake.wait(lock, [&] { return !c.queue.empty(); });
            path = std::move(c.queue.front());
            c.queue.pop_front();
        }
        CardPixels pixels;
        const char* why = nullptr;
        try {
            const auto name = std::string_view(path).substr(0, path.rfind('|'));
            const std::filesystem::path file(std::u8string(name.begin(), name.end()));
            std::error_code code;
            const auto size = std::filesystem::file_size(file, code);
            if (code) why = "the file cannot be read";
            else if (size > most_file_bytes) why = "it is larger than 16 MB";
            else {
                std::ifstream in(file, std::ios::binary);
                std::string bytes(static_cast<std::size_t>(size), '\0');
                if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) why = "the file cannot be read";
                else if (!decode_picture_within(bytes, pixels, most_side)) why = "it is not a PNG or JPG of at most 4096 x 4096 pixels";
            }
        } catch (...) {
            why = "the file cannot be read";
        }
        std::lock_guard lock(c.mutex);
        const auto found = c.entries.find(path);
        if (found == c.entries.end()) continue;
        if (why) {
            found->second.stage = Entry::Stage::failed;
            warn_once(c, path, why);
        } else {
            found->second.pixels = std::move(pixels);
            found->second.stage = Entry::Stage::decoded;
        }
    }
}
}

ImTextureID plugin_image(const std::string& path, ImVec2& size) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const double now = steady_seconds();
    auto found = c.entries.find(path);
    if (found == c.entries.end()) {
        if (c.entries.size() >= most_pictures) {
            // Make room: a picture that failed, or the one drawn longest ago and idle.
            auto oldest = c.entries.end();
            for (auto it = c.entries.begin(); it != c.entries.end(); ++it) {
                if (it->second.stage == Entry::Stage::failed) {
                    oldest = it;
                    break;
                }
                if (now - it->second.drawn >= idle_seconds && it->second.slot < 0 &&
                    (oldest == c.entries.end() || it->second.drawn < oldest->second.drawn))
                    oldest = it;
            }
            if (oldest == c.entries.end()) {
                warn_once(c, path, "plugins draw more than 256 pictures at once");
                return {};
            }
            c.entries.erase(oldest);
        }
        found = c.entries.emplace(path, Entry{}).first;
        c.queue.push_back(path);
        if (!std::exchange(c.worker, true)) std::thread(decode_loop).detach();
        c.wake.notify_one();
    }
    auto& entry = found->second;
    entry.drawn = now;
    if (entry.stage != Entry::Stage::uploaded) return {};
    size = ImVec2(static_cast<float>(entry.pixels.width), static_cast<float>(entry.pixels.height));
    return entry.id;
}

bool plugin_pixels(const std::string& key, CardPixels pixels) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    auto found = c.entries.find(key);
    if (found == c.entries.end()) {
        // (ReSkate's own, "reskate:", are few and are not counted against the plugins' 32.)
        if (c.entries.size() >= most_pictures && !key.starts_with("reskate:")) {
            // Make room as a file's picture does: one that failed, or idle and not on the GPU.
            const double now = steady_seconds();
            auto oldest = c.entries.end();
            for (auto it = c.entries.begin(); it != c.entries.end(); ++it)
                if (it->second.stage == Entry::Stage::failed ||
                    (now - it->second.drawn >= idle_seconds && it->second.slot < 0 && (oldest == c.entries.end() || it->second.drawn < oldest->second.drawn)))
                    oldest = it;
            if (oldest == c.entries.end()) return false;
            c.entries.erase(oldest);
        }
        found = c.entries.emplace(key, Entry{}).first;
    }
    // (One already on the GPU gives its slot up: the new pixels go to a slot of their own, so the
    // frames still drawing the old texture are not disturbed.)
    found->second = Entry{};
    found->second.pixels = std::move(pixels);
    found->second.stage = Entry::Stage::decoded;
    found->second.drawn = steady_seconds();
    return true;
}

ImTextureID plugin_pixels_id(const std::string& key) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto found = c.entries.find(key);
    if (found == c.entries.end()) return {};
    found->second.drawn = steady_seconds();
    return found->second.stage == Entry::Stage::uploaded ? found->second.id : ImTextureID{};
}

bool plugin_pixels_waiting(const std::string& key) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto found = c.entries.find(key);
    if (found == c.entries.end()) return false;
    found->second.drawn = steady_seconds(); // (wanted still: it is not the idle one that makes room)
    return found->second.stage == Entry::Stage::decoded || found->second.stage == Entry::Stage::waiting;
}

void plugin_pixels_drop(const std::string& key) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    // (Its texture stays in its slot until the next picture takes the slot, which waits for the
    // frames that may still sample it, as for any picture here.)
    c.entries.erase(key);
}

std::optional<PluginImageUpload> take_plugin_image() {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    std::array<bool, plugin_image_slots> used{};
    for (const auto& [path, entry] : c.entries)
        if (entry.slot >= 0) used[static_cast<std::size_t>(entry.slot)] = true;
    // ReSkate's own pictures (the game's map for a plugin's minimap, "reskate:" names) have the last
    // two slots to themselves and go first. They had neither: the first picture waiting was the only
    // one looked at, in the order of the names, so one plugin picture that could get no slot (sixteen
    // already drawn: a HUD's icons, a menu's) kept every picture after it waiting, and the map's name
    // comes after every file's. The minimap then showed on some runs and not on others (seen 2026-11-16).
    constexpr int reserved = 2;
    const auto own = [](const std::string& name) { return name.starts_with("reskate:"); };
    const double now = steady_seconds();
    const auto free_slot = [&](int limit) {
        for (int i = 0; i < limit; ++i)
            if (!used[static_cast<std::size_t>(i)]) return i;
        return -1;
    };
    // The slot of the picture drawn longest ago among those that may give theirs up; -1 for none.
    const auto give_up = [&](int limit, bool idle_only) {
        auto oldest = c.entries.end();
        for (auto it = c.entries.begin(); it != c.entries.end(); ++it)
            if (it->second.slot >= 0 && it->second.slot < limit && it->second.stage == Entry::Stage::uploaded &&
                (!idle_only || now - it->second.drawn >= idle_seconds) && (idle_only || !own(it->first)) &&
                (oldest == c.entries.end() || it->second.drawn < oldest->second.drawn))
                oldest = it;
        if (oldest == c.entries.end()) return -1;
        const int slot = oldest->second.slot;
        c.entries.erase(oldest);
        return slot;
    };
    const int all = static_cast<int>(plugin_image_slots);
    auto waiting = c.entries.end();
    int slot = -1;
    for (auto it = c.entries.begin(); it != c.entries.end() && waiting == c.entries.end(); ++it) {
        if (it->second.stage != Entry::Stage::decoded || !own(it->first)) continue;
        slot = free_slot(all);
        if (slot < 0) slot = give_up(all, true);
        if (slot < 0) slot = give_up(all, false);   // (a plugin's picture makes way: it is made again when next drawn)
        if (slot >= 0) waiting = it;
    }
    for (auto it = c.entries.begin(); it != c.entries.end() && waiting == c.entries.end(); ++it) {
        if (it->second.stage != Entry::Stage::decoded || own(it->first)) continue;
        slot = free_slot(all - reserved);
        // Every slot is in use: the one drawn longest ago gives it up, if it is idle.
        if (slot < 0) slot = give_up(all - reserved, true);
        if (slot >= 0) waiting = it;
        else break; // (no slot for a plugin's picture this frame: the next would find none either)
    }
    if (waiting == c.entries.end()) return std::nullopt;
    // The memory the pictures on the GPU hold: one more than fits waits (its slot stays free).
    std::uint64_t held = 0;
    for (const auto& [name, entry] : c.entries)
        if (entry.slot >= 0) held += static_cast<std::uint64_t>(entry.pixels.width) * entry.pixels.height * 4;
    if (held + static_cast<std::uint64_t>(waiting->second.pixels.width) * waiting->second.pixels.height * 4 > most_gpu_bytes &&
        give_up(all, true) < 0)
        return std::nullopt;
    waiting->second.slot = slot;
    waiting->second.stage = Entry::Stage::waiting; // until plugin_image_uploaded
    return PluginImageUpload{waiting->first, slot, waiting->second.pixels};
}

void plugin_image_uploaded(const std::string& path, int slot, ImTextureID id, bool later) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto found = c.entries.find(path);
    if (found == c.entries.end() || found->second.slot != slot) return;
    auto& entry = found->second;
    if (later) {
        entry.stage = Entry::Stage::decoded;
        entry.slot = -1;
        return;
    }
    if (!id) {
        entry.stage = Entry::Stage::failed;
        entry.slot = -1;
        warn_once(c, path, "the GPU could not take it");
        return;
    }
    entry.stage = Entry::Stage::uploaded;
    entry.id = id;
    entry.pixels.rgba.clear(); // the size stays, for drawing
    entry.pixels.rgba.shrink_to_fit();
}

void plugin_images_lost() {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    c.entries.clear(); // drawn again, they decode again
}
}
