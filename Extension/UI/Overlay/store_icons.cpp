#include "overlay_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

// Thunderstore packages' icons for the Mod Manager page (mod_manager_page.inl), by their URL. They
// are kept on disk where the launcher keeps them (%LOCALAPPDATA%\ReSkate\thunderstore\icons, named
// as gui_mods_browse.cpp names them), so an icon either of them fetched is not fetched again. Three
// worker threads read, fetch and decode them (WIC, 256 pixels square), the one asked for last first:
// it is what the player is looking at now. The render thread uploads one a frame into the SRV heap's
// store icon slots, and an icon not drawn for 2 s gives its slot up when another needs it. Only the
// tiles on screen ask (mod_manager_page.inl), so the slots are enough for a full page of them.
namespace dingosdk::overlay::detail {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t most_icons = 192;          // known at once, decoded or waiting
constexpr std::size_t workers = 3;
constexpr double retry_seconds = 20;             // an icon that could not be had is tried again after this
constexpr std::uint64_t most_icon_bytes = 4 * 1024 * 1024;
constexpr double idle_seconds = 2;

struct Entry {
    enum class Stage { waiting, decoded, uploaded, failed } stage{Stage::waiting};
    CardPixels pixels;
    int slot{-1};
    ImTextureID id{};
    double drawn{}, failed{};
};
struct Cache {
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, Entry, std::less<>> entries;
    std::deque<std::string> queue;
    std::size_t started{};
};
Cache& cache() {
    static auto* value = new Cache;
    return *value;
}
double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
fs::path icon_folder() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"thunderstore" / L"icons";
}
// Icon URLs end in Namespace-Name-1.2.3.png, so a file named after that never goes stale.
std::wstring icon_file(std::string_view url) {
    const auto slash = url.rfind('/');
    std::string name(url.substr(slash == std::string_view::npos ? 0 : slash + 1));
    for (auto& ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
            ch = '_';
    if (name.empty() || name.front() == '.') name.insert(name.begin(), 'i');
    return std::wstring(name.begin(), name.end());
}
// Only Thunderstore's own hosts: a listing names its icons, and nothing else is fetched from here.
bool thunderstore_host(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) return false;
    auto host = url.substr(scheme.size());
    host = host.substr(0, host.find('/'));
    return host == "thunderstore.io" || host == "gcdn.thunderstore.io" || host.ends_with(".thunderstore.io");
}

void decode_loop() {
    auto& c = cache();
    const auto folder = icon_folder();
    for (;;) {
        std::string url;
        {
            std::unique_lock lock(c.mutex);
            c.wake.wait(lock, [&] { return !c.queue.empty(); });
            url = std::move(c.queue.back());
            c.queue.pop_back();
            // Asked for long enough ago that it went out of view again: let it wait for the next ask.
            const auto found = c.entries.find(url);
            if (found == c.entries.end()) continue;
            if (seconds_now() - found->second.drawn > idle_seconds) {
                c.entries.erase(found);
                continue;
            }
        }
        CardPixels pixels;
        bool ok{};
        try {
            const auto cached = folder.empty() ? fs::path() : folder / icon_file(url);
            std::string bytes;
            std::error_code error;
            if (!cached.empty() && fs::is_regular_file(cached, error) && fs::file_size(cached, error) <= most_icon_bytes) {
                std::ifstream in(cached, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (bytes.empty() && thunderstore_host(url)) {
                if (auto body = https::get_text(std::wstring(url.begin(), url.end()), most_icon_bytes, 20, L"ReSkate")) {
                    bytes = std::move(*body);
                    if (!cached.empty() && !bytes.empty()) {
                        fs::create_directories(folder, error);
                        std::ofstream out(cached, std::ios::binary | std::ios::trunc);
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    }
                }
            }
            ok = !bytes.empty() && decode_picture(bytes, pixels, 256);
        } catch (...) {
            ok = false;
        }
        std::lock_guard lock(c.mutex);
        const auto found = c.entries.find(url);
        if (found == c.entries.end()) continue;
        if (!ok) found->second.stage = Entry::Stage::failed, found->second.failed = seconds_now();
        else {
            found->second.pixels = std::move(pixels);
            found->second.stage = Entry::Stage::decoded;
        }
    }
}
}

ImTextureID store_icon(const std::string& url) {
    if (url.empty()) return {};
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const double now = seconds_now();
    auto found = c.entries.find(url);
    if (found == c.entries.end()) {
        if (c.entries.size() >= most_icons) {
            // Make room: an icon that failed long enough ago, or the one drawn longest ago and idle.
            auto oldest = c.entries.end();
            for (auto it = c.entries.begin(); it != c.entries.end(); ++it)
                if (it->second.slot < 0 && it->second.stage != Entry::Stage::waiting && now - it->second.drawn >= idle_seconds &&
                    (oldest == c.entries.end() || it->second.drawn < oldest->second.drawn))
                    oldest = it;
            if (oldest == c.entries.end()) return {};
            c.entries.erase(oldest);
        }
        found = c.entries.emplace(url, Entry{}).first;
        c.queue.push_back(url);
        if (c.started < workers && c.queue.size() > c.started) {
            try { std::thread(decode_loop).detach(); ++c.started; } catch (...) {}
        }
        c.wake.notify_one();
    } else if (found->second.stage == Entry::Stage::failed && now - found->second.failed >= retry_seconds) {
        found->second.stage = Entry::Stage::waiting;
        c.queue.push_back(url);
        c.wake.notify_one();
    }
    auto& entry = found->second;
    entry.drawn = now;
    return entry.stage == Entry::Stage::uploaded ? entry.id : ImTextureID{};
}

std::optional<StoreIconUpload> take_store_icon() {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    // The most recently asked for first: what is on screen now.
    auto waiting = c.entries.end();
    for (auto it = c.entries.begin(); it != c.entries.end(); ++it)
        if (it->second.stage == Entry::Stage::decoded && (waiting == c.entries.end() || it->second.drawn > waiting->second.drawn)) waiting = it;
    if (waiting == c.entries.end()) return std::nullopt;
    std::array<bool, store_icon_slots> used{};
    for (const auto& [url, entry] : c.entries)
        if (entry.slot >= 0) used[static_cast<std::size_t>(entry.slot)] = true;
    int slot = -1;
    for (int i = 0; i < static_cast<int>(store_icon_slots); ++i)
        if (!used[static_cast<std::size_t>(i)]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        // Every slot is in use: the icon drawn longest ago gives it up, if it is idle. Its pixels
        // are gone, so drawn again it is read again.
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
    waiting->second.stage = Entry::Stage::waiting; // until store_icon_uploaded
    return StoreIconUpload{waiting->first, slot, waiting->second.pixels};
}

void store_icon_uploaded(const std::string& url, int slot, ImTextureID id, bool later) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto found = c.entries.find(url);
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
        return;
    }
    entry.stage = Entry::Stage::uploaded;
    entry.id = id;
    entry.pixels.rgba.clear();
    entry.pixels.rgba.shrink_to_fit();
}

void store_icons_lost() {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    // Drawn again, they are read again; a read under way finds its entry gone and is dropped.
    for (auto it = c.entries.begin(); it != c.entries.end();)
        it = it->second.stage == Entry::Stage::waiting && it->second.slot < 0 ? std::next(it) : c.entries.erase(it);
}
}
