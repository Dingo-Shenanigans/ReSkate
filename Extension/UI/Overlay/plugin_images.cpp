#include "overlay_internal.h"
#include "Engine/Core/Log/logging.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// Pictures plugins draw (reskate.overlay's d:image), from files in their own packages: the plugin
// host checked each path is inside its package. One worker thread decodes them (WIC, at most 512
// pixels a side and 2 MB a file); the render thread uploads one a frame into the SRV heap's plugin
// slots, and a picture not drawn for 2 s gives its slot up when another needs it.
namespace dingosdk::overlay::detail {
namespace {
constexpr std::size_t most_pictures = 32;      // known at once, decoded or waiting
constexpr std::uintmax_t most_file_bytes = 2 * 1024 * 1024;
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
double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
            const std::filesystem::path file(std::u8string(path.begin(), path.end()));
            std::error_code code;
            const auto size = std::filesystem::file_size(file, code);
            if (code) why = "the file cannot be read";
            else if (size > most_file_bytes) why = "it is larger than 2 MB";
            else {
                std::ifstream in(file, std::ios::binary);
                std::string bytes(static_cast<std::size_t>(size), '\0');
                if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) why = "the file cannot be read";
                else if (!decode_picture(bytes, pixels, 0)) why = "it is not a PNG or JPG of at most 512 x 512 pixels";
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
    const double now = seconds_now();
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
                warn_once(c, path, "plugins draw more than 32 pictures at once");
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

std::optional<PluginImageUpload> take_plugin_image() {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto waiting = std::find_if(c.entries.begin(), c.entries.end(), [](const auto& e) { return e.second.stage == Entry::Stage::decoded; });
    if (waiting == c.entries.end()) return std::nullopt;
    std::array<bool, plugin_image_slots> used{};
    for (const auto& [path, entry] : c.entries)
        if (entry.slot >= 0) used[static_cast<std::size_t>(entry.slot)] = true;
    int slot = -1;
    for (int i = 0; i < static_cast<int>(plugin_image_slots); ++i)
        if (!used[static_cast<std::size_t>(i)]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        // Every slot is in use: the one drawn longest ago gives it up, if it is idle.
        const double now = seconds_now();
        auto oldest = c.entries.end();
        for (auto it = c.entries.begin(); it != c.entries.end(); ++it)
            if (it->second.slot >= 0 && now - it->second.drawn >= idle_seconds &&
                (oldest == c.entries.end() || it->second.drawn < oldest->second.drawn))
                oldest = it;
        if (oldest == c.entries.end()) return std::nullopt;
        slot = oldest->second.slot;
        c.entries.erase(oldest);
    }
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
