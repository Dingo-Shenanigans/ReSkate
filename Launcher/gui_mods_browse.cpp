#include "gui_internal.h"
#include "gui_renderer.h"
#include "official_mods.h"

#include "Engine/Vfs/builtin_packs.h"

#include "mod_manager.h"
#include "updater.h"

#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Debug/trace.h"

#include <cstdio>
#include <format>

// The Mods panel's BROWSE page: packages from Thunderstore, their icons, and
// downloading and installing them on the panel's worker.
namespace dingosdk::launcher_gui::detail {
namespace {

namespace ts = thunderstore;

constexpr int listing_timeout_ms = 20000;
constexpr int download_timeout_ms = 30000;
constexpr std::size_t max_icons = 128;          // loaded icon textures, of the renderer's 160 slots
constexpr std::uint64_t max_icon_bytes = 6 * 1024 * 1024;

void log(logging::Level level, const std::string& message) {
    logging::write(level, logging::Channel::launcher, message);
}

std::string fetch(const std::wstring& url, std::uint64_t limit, const std::atomic<bool>& stop) {
    std::string body;
    launcher_update::http_stream(url, limit, listing_timeout_ms, [&](const char* data, DWORD size) {
        if (stop) throw std::runtime_error("stopped");
        body.append(data, size);
    });
    return body;
}

// The listing index and its chunks (what r2modman reads), else the plain listing.
std::vector<ts::Package> fetch_listing(const std::string& community, const std::atomic<bool>& stop) {
    try {
        const auto index = ts::parse_index(ts::gunzip(fetch(ts::listing_index_url(community), 1 << 20, stop), 4 << 20));
        std::vector<ts::Package> packages;
        for (const auto& url : index) {
            auto chunk = ts::parse_listing(ts::gunzip(fetch(url, 64ull << 20, stop), 256ull << 20));
            std::move(chunk.begin(), chunk.end(), std::back_inserter(packages));
        }
        return packages;
    } catch (const std::exception& failure) {
        if (stop) throw;
        log(logging::Level::warning, std::string("Thunderstore listing index unavailable (") + failure.what() +
            "); reading the plain listing");
    }
    return ts::parse_listing(ts::gunzip(fetch(ts::listing_url(community), 256ull << 20, stop), 256ull << 20));
}

fs::path icon_cache() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"thunderstore" / L"icons";
}

// Icon URLs end in Namespace-Name-1.2.3.png, so a file name keyed on that never goes stale.
std::wstring icon_file(std::string_view url) {
    const auto slash = url.rfind('/');
    std::string name(url.substr(slash == std::string_view::npos ? 0 : slash + 1));
    for (auto& ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
            ch = '_';
    if (name.empty() || name.front() == '.') name.insert(name.begin(), 'i');
    return wide(name);
}

std::vector<unsigned char> read_bytes(const fs::path& path) {
    std::vector<unsigned char> bytes;
    FILE* file{};
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return bytes;
    std::array<unsigned char, 65536> buffer{};
    for (std::size_t read; (read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0;)
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    std::fclose(file);
    return bytes;
}

void icon_worker(Icons& icons) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const auto cache = icon_cache();
    std::atomic<bool> never{};
    for (;;) {
        std::string url;
        {
            std::unique_lock lock(icons.mutex);
            icons.wake.wait(lock, [&] { return icons.stop || !icons.queue.empty(); });
            if (icons.stop) break;
            url = std::move(icons.queue.front());
            icons.queue.pop_front();
        }
        Icons::Decoded decoded{url};
        try {
            const auto cached = cache.empty() ? fs::path() : cache / icon_file(url);
            auto bytes = cached.empty() ? std::vector<unsigned char>() : read_bytes(cached);
            if (bytes.empty()) {
                const auto body = fetch(wide(url), max_icon_bytes, never);
                bytes.assign(body.begin(), body.end());
                if (!cached.empty()) {
                    std::error_code error;
                    fs::create_directories(cache, error);
                    FILE* file{};
                    if (!_wfopen_s(&file, cached.c_str(), L"wb") && file) {
                        std::fwrite(bytes.data(), 1, bytes.size(), file);
                        std::fclose(file);
                    }
                }
            }
            UINT width{}, height{};
            if (!decode_image(bytes, ImVec2(S(96), S(96)), decoded.pixels, width, height)) decoded.pixels.clear();
            decoded.width = width;
            decoded.height = height;
        } catch (const std::exception&) {
            decoded.pixels.clear();
        }
        std::lock_guard lock(icons.mutex);
        icons.done.push_back(std::move(decoded));
    }
    if (com) CoUninitialize();
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::string count_text(std::uint64_t value) {
    if (value >= 1'000'000) return std::format("{:.1f}M", static_cast<double>(value) / 1e6);
    if (value >= 10'000) return std::format("{}k", value / 1000);
    if (value >= 1'000) return std::format("{:.1f}k", static_cast<double>(value) / 1e3);
    return std::to_string(value);
}

std::string date_text(const std::string& iso) { return iso.substr(0, std::min<std::size_t>(iso.size(), 10)); }

void set_activity(ModsPanel& panel, std::string text) {
    std::lock_guard lock(panel.mutex);
    panel.activity = std::move(text);
}

// Downloads one package into Mods/.reskate-download, then installs it over any
// installed copy (which is deleted, its place in mods.json kept).
std::string install_package(ModsPanel& panel, const fs::path& root, const ts::Package& package) {
    const auto& version = package.latest();
    const auto folder = root / L".reskate-download";
    const auto archive = folder / (wide(package.full_name) + L".zip");
    std::error_code error;
    fs::create_directories(folder, error);
    struct Cleanup {
        fs::path folder, archive;
        ~Cleanup() { std::error_code ignored; fs::remove(archive, ignored); fs::remove(folder, ignored); }
    } cleanup{folder, archive};

    trace::Scope timing("launcher.mod_install", "task");
    timing.tag("package", package.full_name);
    timing.tag("version", version.number);
    set_activity(panel, "Downloading " + package.title() + " v" + version.number +
        (version.file_size ? "  (" + size_text(version.file_size) + ")" : std::string()));
    panel.progress = 0;
    {
        FILE* file{};
        if (_wfopen_s(&file, archive.c_str(), L"wb") || !file)
            throw std::runtime_error("Could not write the download into the Mods folder.");
        std::uint64_t received = 0;
        const auto expected = version.file_size;
        try {
            launcher_update::http_stream(wide(version.download_url), expected ? expected + (1 << 20) : 8ull << 30,
                download_timeout_ms, [&](const char* data, DWORD size) {
                    if (panel.cancel) throw std::runtime_error("Install cancelled.");
                    if (std::fwrite(data, 1, size, file) != size)
                        throw std::runtime_error("Could not write the download (is the disk full?).");
                    received += size;
                    if (expected) panel.progress = static_cast<float>(static_cast<double>(received) / static_cast<double>(expected));
                });
        } catch (...) {
            std::fclose(file);
            throw;
        }
        std::fclose(file);
        if (expected && received != expected)
            throw std::runtime_error(std::format("The download of {} stopped early ({} of {}).", package.title(),
                size_text(received), size_text(expected)));
    }

    set_activity(panel, "Installing " + package.title() + " v" + version.number);
    panel.progress = 0;
    launcher_mods::InstallOptions options;
    options.folder = ts::folder_for(package.full_name);
    options.author = package.owner;
    options.require_content = true;
    return launcher_mods::install(root, archive, true, [&panel](float fraction) { panel.progress = fraction; },
        panel.cancel, options);
}

// Search, category and the sort order, pinned packages first like the site.
// Not safe for work: Thunderstore's own mark on the package, or a category of that name.
bool nsfw(const ts::Package& package) {
    return package.nsfw || std::any_of(package.categories.begin(), package.categories.end(),
                                       [](const std::string& category) { return lower(category) == "nsfw"; });
}

std::vector<const ts::Package*> visible_packages(const Store& store, const ts::Installed& installed) {
    const auto query = lower(store.search.data());
    std::vector<const ts::Package*> result;
    for (const auto& package : store.packages) {
        const bool have = installed.contains(ts::folder_for(package.full_name));
        // NSFW packages are never listed here, installed or not: by Thunderstore's own mark,
        // or a category of that name. A deprecated one only shows once installed.
        if (nsfw(package)) continue;
        if (package.deprecated && !have) continue;
        // The official mods are installed from their own tab (official_mods.h).
        if (official_mods::official(package.full_name)) continue;
        if (!store.category.empty() && !package.in_category(store.category)) continue;
        if (!query.empty() && lower(package.title()).find(query) == std::string::npos &&
            lower(package.owner).find(query) == std::string::npos &&
            lower(package.latest().description).find(query) == std::string::npos) continue;
        result.push_back(&package);
    }
    const auto order = [&](const ts::Package* a, const ts::Package* b) {
        if (a->pinned != b->pinned) return a->pinned;
        switch (store.sort) {
        case 1: if (a->downloads != b->downloads) return a->downloads > b->downloads; break;
        case 2: if (a->date_created != b->date_created) return a->date_created > b->date_created; break;
        case 3: if (a->rating != b->rating) return a->rating > b->rating; break;
        case 4: break;
        default: if (a->date_updated != b->date_updated) return a->date_updated > b->date_updated; break;
        }
        return lower(a->title()) < lower(b->title());
    };
    std::stable_sort(result.begin(), result.end(), order);
    return result;
}

constexpr std::array<const char*, 5> sort_names{"Last updated", "Most downloaded", "Newest", "Top rated", "Name"};

// What installing `package` does now: install, update or reinstall.
std::string action_label(const ts::Package& package, const ts::Installed& installed) {
    const auto found = installed.find(ts::folder_for(package.full_name));
    if (found == installed.end()) return "INSTALL";
    if (ts::update_available(package, installed)) return "UPDATE TO v" + package.latest().number;
    return "REINSTALL";
}

} // namespace

// Placed by hand: a row lays its own icon, text and badges out.
void mod_icon(ModsPanel& panel, const thunderstore::Package* package, ImVec2 position, float size) {
    const ImVec2 end(position.x + size, position.y + size);
    auto* draw = ImGui::GetWindowDrawList();
    if (const auto id = package ? package_icon(panel, *package) : ImTextureID{})
        draw->AddImageRounded(id, position, end, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
    else
        draw->AddRectFilled(position, end, rgba(255, 255, 255, 0.06f), S(4));
}

std::string size_text(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return std::format("{:.2f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    if (bytes >= 1024ull * 1024) return std::format("{:.0f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
    return std::format("{} KB", std::max<std::uint64_t>(1, bytes / 1024));
}

void refresh_listing(ModsPanel& panel, double time, bool force) {
    auto& store = panel.store;
    // Once per launch, unless asked; a failed fetch retries after a minute.
    if (store.loading || (!force && store.fetched > -1e8 && (store.error.empty() || time - store.fetched < 60))) return;
    if (store.worker.joinable()) store.worker.join();
    store.fetched = time;
    store.loading = true;
    store.worker = std::thread([&store] {
        std::vector<ts::Package> packages;
        std::string error;
        const auto community = ts::community();
        try {
            packages = fetch_listing(community, store.stop);
            log(logging::Level::info, std::format("Thunderstore: {} package(s) in {}", packages.size(), community));
        } catch (const std::exception& failure) {
            error = failure.what();
            if (!store.stop) log(logging::Level::warning, "Thunderstore listing could not be read: " + error);
        }
        std::lock_guard lock(store.mutex);
        store.incoming = std::move(packages);
        store.incoming_error = std::move(error);
        store.arrived = true;
        store.loading = false;
    });
}

void collect_listing(ModsPanel& panel) {
    auto& store = panel.store;
    std::lock_guard lock(store.mutex);
    if (!store.arrived) return;
    store.arrived = false;
    if (store.incoming_error.empty()) {
        store.packages = std::move(store.incoming);
        store.loaded = true;
        store.error.clear();
    } else {
        store.error = std::move(store.incoming_error);
    }
    store.incoming.clear();
}

thunderstore::Installed installed_versions(const mods::ModList& list, bool enabled_only) {
    ts::Installed installed;
    for (const auto& entry : list.entries)
        if (entry.enabled || !enabled_only) installed[entry.mod.name] = entry.mod.version;
    return installed;
}

const thunderstore::Package* package_for(const Store& store, std::string_view folder) {
    for (const auto& package : store.packages)
        if (ts::folder_for(package.full_name) == folder) return &package;
    return nullptr;
}

std::vector<const thunderstore::Package*> updates(const Store& store, const thunderstore::Installed& installed) {
    std::vector<const ts::Package*> result;
    for (const auto& package : store.packages)
        if (ts::update_available(package, installed) && !official_mods::official(package.full_name))
            result.push_back(&package);
    return result;
}

void start_store_install(ModsPanel& panel, std::vector<thunderstore::Package> packages) {
    if (panel.installing || packages.empty()) return;
    {
        // A package brings what it needs with it (a mod pack is nothing else), needed mods first.
        const auto have = installed_versions(panel.list);
        std::vector<thunderstore::Package> expanded;
        for (const auto& package : packages)
            for (auto& needed : thunderstore::with_dependencies(package, panel.store.packages, have))
                // (A mod that needs an official mod has it already: it is not installed again.)
                if (!official_mods::official(needed.full_name) &&
                    std::none_of(expanded.begin(), expanded.end(),
                        [&](const auto& queued) { return queued.full_name == needed.full_name; }))
                    expanded.push_back(std::move(needed));
        packages = std::move(expanded);
    }
    if (panel.worker.joinable()) panel.worker.join();
    panel.installing = true;
    panel.cancel = false;
    panel.progress = 0;
    panel.message.clear();
    set_activity(panel, "Starting download...");
    const auto root = panel.root;
    panel.worker = std::thread([&panel, root, packages = std::move(packages)] {
        std::string name, error, note;
        std::size_t done = 0;
        // One that fails does not stop the ones after it: each is its own download, and the
        // list ends with what could not be installed. Cancelling does stop the rest.
        std::vector<std::string> failed;
        for (const auto& package : packages) {
            if (panel.cancel) break;
            try {
                const bool update = fs::exists(root / wide(ts::folder_for(package.full_name)));
                name = install_package(panel, root, package);
                ++done;
                log(logging::Level::info, std::format("Mod {} from Thunderstore: {} v{} into Mods\\{}",
                    update ? "updated" : "installed", package.full_name, package.latest().number, name));
                note = packages.size() > 1
                    ? std::format("Installed or updated {} mods. Changes apply the next time Skate starts.", done)
                    : std::format("{} {} v{}. It loads the next time Skate starts.", update ? "Updated" : "Installed",
                                  package.title(), package.latest().number);
            } catch (const std::exception& failure) {
                log(logging::Level::warning, "Thunderstore install of " + package.full_name + " failed: " + failure.what());
                if (panel.cancel) {
                    error = package.title() + ": " + failure.what();
                    break;
                }
                failed.push_back(package.title() + ": " + failure.what());
            }
        }
        if (error.empty() && !failed.empty()) {
            // The first few by name and reason; the log has every one.
            constexpr std::size_t shown = 3;
            error = packages.size() == 1 ? failed.front()
                : std::format("{} of {} could not be installed{}. ", failed.size(), packages.size(),
                              done ? std::format(" ({} were)", done) : std::string());
            if (packages.size() > 1) {
                for (std::size_t i = 0; i < failed.size() && i < shown; ++i) error += (i ? " | " : "") + failed[i];
                if (failed.size() > shown) error += std::format(" | and {} more", failed.size() - shown);
            }
        }
        std::lock_guard lock(panel.mutex);
        panel.finished = true;
        panel.finished_name = name;
        panel.finished_error = error;
        panel.finished_conflict.clear();
        panel.finished_note = note;
        panel.finished_source.clear();
        panel.activity.clear();
        panel.installing = false;
    });
}

ImTextureID package_icon(ModsPanel& panel, const thunderstore::Package& package) {
    const auto& url = package.latest().icon;
    if (url.empty() || !g_renderer) return {};
    auto& icons = panel.icons;
    auto& entry = icons.entries[url];
    entry.used = ImGui::GetFrameCount();
    if (entry.id || entry.failed || entry.queued) return entry.id;
    entry.queued = true;
    {
        std::lock_guard lock(icons.mutex);
        icons.queue.push_back(url);
    }
    if (!icons.worker.joinable()) icons.worker = std::thread([&icons] { icon_worker(icons); });
    icons.wake.notify_one();
    return {};
}

void pump_icons(ModsPanel& panel) {
    auto& icons = panel.icons;
    std::vector<Icons::Decoded> done;
    {
        std::lock_guard lock(icons.mutex);
        done.swap(icons.done);
    }
    if (!g_renderer) return;
    for (auto& decoded : done) {
        auto& entry = icons.entries[decoded.url];
        entry.queued = false;
        if (decoded.pixels.empty()) { entry.failed = true; continue; }
        // Make room by dropping the icons drawn longest ago.
        std::size_t loaded = 0;
        for (const auto& [url, other] : icons.entries) loaded += other.id ? 1 : 0;
        while (loaded >= max_icons) {
            auto oldest = icons.entries.end();
            for (auto it = icons.entries.begin(); it != icons.entries.end(); ++it)
                if (it->second.id && (oldest == icons.entries.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == icons.entries.end() || oldest->second.used >= ImGui::GetFrameCount() - 1) break;
            g_renderer->release_texture(oldest->second.id);
            oldest->second.id = {};
            --loaded;
        }
        entry.id = g_renderer->upload_texture(decoded.pixels, decoded.width, decoded.height);
        if (!entry.id) entry.failed = true;
    }
}

bool picked(const Store& store, const std::string& full_name) {
    return std::find(store.picked.begin(), store.picked.end(), full_name) != store.picked.end();
}

void pick(Store& store, const std::string& full_name, bool on) {
    const auto found = std::find(store.picked.begin(), store.picked.end(), full_name);
    if (on && found == store.picked.end()) store.picked.push_back(full_name);
    else if (!on && found != store.picked.end()) store.picked.erase(found);
}

namespace {
// How one official mod is doing, as a pill: its word and colour.
struct Standing {
    const char* word;
    ImU32 fill;
    bool trouble;
};
Standing standing_of(const official_mods::Mod& mod) {
    if (mod.working) return {mod.status == "Removing" ? "UNINSTALLING" : mod.installed.empty() ? "DOWNLOADING" : "UPDATING", color::blue, false};
    if (mod.status.empty()) return {"CHECKING", color::tile_grey, false};
    if (mod.status == "Up to date") return {"INSTALLED", color::good, false};
    if (mod.status == "Not installed") return {"NOT INSTALLED", color::tile_grey, false};
    if (mod.status == "Update available") return {"UPDATE AVAILABLE", color::blue, false};
    if (mod.status.starts_with("Uninstall failed")) return {"UNINSTALL FAILED", color::warning, true};
    return {mod.installed.empty() ? "INSTALL FAILED" : "UPDATE FAILED", mod.installed.empty() ? color::danger : color::warning, true};
}
std::string source_text(const official_mods::State& state) {
    if (state.source == "ReSkate") return "List from ReSkate's server";
    if (state.source == "the last list read") return "Offline: using the last list";
    if (state.source.empty()) return state.syncing ? "Reading the list..." : "";
    return "Using the list ReSkate came with";
}
}

void official_page(const Fonts& fonts, ModsPanel& panel, float height) {
    // (The listing gives their icons, sizes and download counts.)
    refresh_listing(panel, ImGui::GetTime());
    const auto state = official_mods::snapshot();
    const float top_y = ImGui::GetCursorPosY();

    const bool trouble = std::any_of(state.mods.begin(), state.mods.end(), [](const auto& mod) { return standing_of(mod).trouble; });
    // Which are turned off (Engine/Vfs/builtin_packs.h), read again now and then rather than every frame.
    static std::vector<std::string> off;
    static double off_read = -1e9;
    const auto read_off = [&] {
        off.clear();
        for (const auto& mod : state.mods)
            if (mods::official_off(panel.root, mod.full_name)) off.push_back(mod.full_name);
        off_read = ImGui::GetTime();
    };
    if (ImGui::GetTime() - off_read > 2.0) read_off();
    const auto have = std::count_if(state.mods.begin(), state.mods.end(), [](const auto& mod) { return !mod.installed.empty(); });
    page_banner(fonts, "OFFICIAL MODS", nullptr,
        "Mods made for ReSkate's own features, from Thunderstore. Install the ones you want here; they load after your mods. "
        "One that is not installed, or turned off, leaves its feature out of the game. Changes apply the next time you play.",
        state.syncing ? "CHECKING" : trouble ? "NEEDS ATTENTION" : state.mods.empty() ? "NONE RIGHT NOW"
                                   : std::format("{} OF {} INSTALLED", have, state.mods.size()),
        state.syncing ? color::blue : trouble ? color::warning : have ? color::good : color::tile_grey,
        source_text(state));

    // ------------------------------------------------ the mods, a card each
    ImGui::BeginChild("##official", ImVec2(0, std::max(S(80), height - (ImGui::GetCursorPosY() - top_y))), ImGuiChildFlags_NavFlattened);
    auto* draw = ImGui::GetWindowDrawList();
    if (state.mods.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", state.syncing ? "Reading the list of official mods..." : "There are no official mods right now.");
    }
    const float pad = S(18);
    const float icon = S(96);
    const float button = S(150);
    for (const auto& mod : state.mods) {
        ImGui::PushID(mod.full_name.c_str());
        const auto* package = ts::find_package(panel.store.packages, mod.full_name);
        const auto standing = standing_of(mod);
        bool on = std::find(off.begin(), off.end(), mod.full_name) == off.end();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float across = ImGui::GetContentRegionAvail().x;
        const float text_x = start.x + pad + icon + S(18);
        const float buttons_x = start.x + across - pad - button;
        const float wrap = buttons_x - S(24) - text_x;

        auto author = mod.full_name.substr(0, mod.full_name.find('-'));
        std::replace(author.begin(), author.end(), '_', ' ');
        const auto& description = !mod.description.empty() ? mod.description
                                : package ? package->latest().description : std::string();
        const auto version = !mod.installed.empty() ? mod.installed : mod.version;
        std::string facts = version.empty() ? std::string() : "v" + version;
        const auto add = [&](const std::string& fact) { facts += (facts.empty() ? "" : "  /  ") + fact; };
        if (package) {
            if (const auto size = package->latest().file_size) add(size_text(size));
            add(count_text(package->downloads) + " downloads");
            add("updated " + date_text(package->date_updated));
        }
        if (!mod.installed.empty() && !mod.version.empty() && mod.installed != mod.version) add("v" + mod.version + " is out");
        const std::string problem = standing.trouble ? mod.status : std::string();

        // Measured first, so the card is drawn behind what is on it.
        float y = start.y + pad;
        const float title_y = y;
        y += fonts.bold->FontSize + S(10);
        const float pills_y = y;
        y += fonts.caption->FontSize + S(8) + S(12);
        const float description_y = y;
        if (const float tall = text_height(fonts.body, wrap, description)) y += tall + S(10);
        const float facts_y = y;
        if (!facts.empty()) y += fonts.caption->FontSize + S(6);
        const float problem_y = y;
        if (const float tall = text_height(fonts.caption, wrap, problem)) y += tall + S(6);
        // As tall as its text, its icon, or the buttons at its right (install or update, Thunderstore,
        // and once installed, its folder and uninstall).
        const bool installed = !mod.installed.empty();
        const bool update = installed && !mod.version.empty() && mod.installed != mod.version;
        const bool removing = mod.working && mod.status == "Removing";
        const bool action = (!installed || update || mod.working) && !removing;
        const int buttons = (action ? 1 : 0) + 1 + (installed ? 2 : 0);
        const float tall = std::max({y - start.y + pad - S(6), icon + pad * 2, pad * 2 + buttons * S(40) - S(8)});

        card(draw, start, ImVec2(across, tall));
        mod_icon(panel, package, ImVec2(start.x + pad, start.y + pad), icon);

        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(text_x, title_y), color::text, mod.title.c_str());
        const float title_width = fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, mod.title.c_str()).x;
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x + title_width + S(8), title_y + S(2)), color::muted,
            ("by " + author).c_str());
        float pill_x = text_x;
        if (installed && !on) {
            badge(draw, fonts, ImVec2(pill_x, pills_y), "OFF", color::tile_grey, color::text);
            pill_x += badge_width(fonts, "OFF") + S(8);
        }
        badge(draw, fonts, ImVec2(pill_x, pills_y), standing.word, standing.fill, color::ink);
        pill_x += badge_width(fonts, standing.word) + S(8);
        if (!version.empty()) {
            badge(draw, fonts, ImVec2(pill_x, pills_y), "v" + version, color::tile_grey, color::text);
            pill_x += badge_width(fonts, "v" + version) + S(8);
        }
        if (!description.empty())
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, description_y), color::text, description.c_str(), nullptr, wrap);
        if (!facts.empty())
            draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(text_x, facts_y), color::muted, facts.c_str());
        if (!problem.empty())
            draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(text_x, problem_y), standing.fill, problem.c_str(), nullptr, wrap);

        // On or off, once installed, beside its pills: off, nothing merges it and the game neither
        // starts nor lists its feature.
        if (installed) {
            const float switch_x = pill_x + S(8), switch_y = pills_y + (fonts.caption->FontSize + S(8) - S(22)) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(switch_x, switch_y));
            if (toggle("##on", &on)) {
                if (mods::set_official_off(panel.root, mod.full_name, !on))
                    log(logging::Level::info, std::format("Official mods: {} turned {}", mod.full_name, on ? "on" : "off"));
                else
                    log(logging::Level::warning, std::format("Official mods: {} could not be turned {}", mod.full_name, on ? "on" : "off"));
                read_off();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", on ? "On. Turn it off to leave it and its feature out of the game, from the next time you play."
                                           : "Off: not loaded, and its feature is not in the game. Turn it on for the next time you play.");
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(switch_x + S(50), switch_y + (S(22) - fonts.body->FontSize) * 0.5f),
                on ? color::text : color::muted, on ? "On" : "Off");
        }

        // ------------------------------------------- at the right: install or update, then its page and folder
        float button_y = start.y + pad;
        if (action) {
            ImGui::SetCursorScreenPos(ImVec2(buttons_x, button_y));
            ImGui::BeginDisabled(mod.working || state.syncing);
            push_primary_button();
            const auto label = mod.working ? std::string(installed ? "UPDATING..." : "DOWNLOADING...")
                             : update ? "UPDATE TO v" + mod.version : std::string("INSTALL");
            if (ImGui::Button(label.c_str(), ImVec2(button, S(32)))) official_mods::install(panel.root, mod.full_name);
            pop_primary_button();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", installed ? "Download its latest version from Thunderstore"
                                                  : "Download it from Thunderstore. Its feature is in the game from the next time you play.");
            button_y += S(40);
        }
        ImGui::SetCursorScreenPos(ImVec2(buttons_x, button_y));
        if (ImGui::Button("THUNDERSTORE", ImVec2(button, S(32)))) {
            const auto dash = mod.full_name.find('-');
            open_url(package ? package->package_url
                             : std::format("https://thunderstore.io/c/{}/p/{}/{}/", ts::community(), mod.full_name.substr(0, dash),
                                   mod.full_name.substr(dash + 1)));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its page on Thunderstore");
        if (installed) {
            const auto folder = panel.root / std::string(mods::official_folder) / wide(mod.full_name);
            ImGui::SetCursorScreenPos(ImVec2(buttons_x, button_y + S(40)));
            if (ImGui::Button("OPEN FOLDER", ImVec2(button, S(32)))) open_path(folder);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where ReSkate keeps it");
            ImGui::SetCursorScreenPos(ImVec2(buttons_x, button_y + S(80)));
            ImGui::BeginDisabled(mod.working || state.syncing);
            if (ImGui::Button(removing ? "UNINSTALLING..." : "UNINSTALL", ImVec2(button, S(32))))
                official_mods::uninstall(panel.root, mod.full_name);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Delete it. Its feature is gone from the game from the next time you play; Install brings it back.");
        }

        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + tall + S(12)));
        ImGui::Dummy(ImVec2(across, 0));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void browse_page(Launcher& launcher, const Fonts& fonts, ModsPanel& panel, float height, bool installing) {
    auto& store = panel.store;
    const auto installed = installed_versions(panel.list);

    const float top = ImGui::GetCursorPosY();
    {
        const auto waiting = store.loaded ? updates(store, installed).size() : 0;
        const auto have = std::count_if(store.packages.begin(), store.packages.end(),
            [&](const ts::Package& package) { return installed.contains(ts::folder_for(package.full_name)); });
        page_banner(fonts, "GET MODS", nullptr,
            "Mods for ReSkate from Thunderstore. Open one to read about it, or tick several and install them together. "
            "Official mods are installed from the OFFICIAL tab, so they are not listed here.",
            !store.loaded ? (store.loading || store.error.empty() ? "LOADING" : "OFFLINE")
                : waiting ? std::format("{} {} WAITING", waiting, waiting == 1 ? "UPDATE" : "UPDATES")
                          : std::format("{} INSTALLED", have),
            !store.loaded ? (store.loading || store.error.empty() ? color::blue : color::danger) : waiting ? color::blue : color::good,
            !store.loaded ? (store.loading || store.error.empty() ? "Reading Thunderstore..." : "Thunderstore could not be reached")
                          : std::format("{} mods on Thunderstore", count_text(store.packages.size())));
    }

    // ------------------------------------------------ search, category, sort, refresh
    const float combo = S(190), refresh = S(110);
    ImGui::SetNextItemWidth(std::max(S(140),
        ImGui::GetContentRegionAvail().x - (combo + S(10)) * 2 - refresh - S(10)));
    ImGui::InputTextWithHint("##search", "Search mods", store.search.data(), store.search.size());
    ImGui::SameLine();
    std::vector<std::string> categories;
    for (const auto& package : store.packages) {
        if (nsfw(package)) continue;   // (and no category that only they have)
        for (const auto& category : package.categories)
            if (std::find(categories.begin(), categories.end(), category) == categories.end()) categories.push_back(category);
    }
    std::sort(categories.begin(), categories.end());
    ImGui::SetNextItemWidth(combo);
    if (ImGui::BeginCombo("##category", store.category.empty() ? "All categories" : store.category.c_str())) {
        if (ImGui::Selectable("All categories", store.category.empty())) store.category.clear();
        for (const auto& category : categories)
            if (ImGui::Selectable(category.c_str(), store.category == category)) store.category = category;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo);
    if (ImGui::BeginCombo("##sort", sort_names[static_cast<std::size_t>(std::clamp(store.sort, 0, 4))])) {
        for (int i = 0; i < static_cast<int>(sort_names.size()); ++i)
            if (ImGui::Selectable(sort_names[static_cast<std::size_t>(i)], store.sort == i)) store.sort = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(store.loading);
    if (ImGui::Button("Refresh", ImVec2(refresh, 0))) refresh_mods(launcher, panel);
    ImGui::EndDisabled();

    // ------------------------------------------------ what is ticked
    if (!store.picked.empty()) {
        ImGui::Spacing();
        const ImVec2 bar = ImGui::GetCursorScreenPos();
        const float pad = S(10), bar_tall = ImGui::GetFrameHeight() + pad * 2;
        card(ImGui::GetWindowDrawList(), bar, ImVec2(ImGui::GetContentRegionAvail().x, bar_tall), true);
        ImGui::SetCursorScreenPos(ImVec2(bar.x + S(20), bar.y + pad));
        ImGui::AlignTextToFramePadding();
        // What installing them all downloads: each one's newest version.
        std::uint64_t bytes{};
        for (const auto& name : store.picked)
            for (const auto& package : store.packages)
                if (package.full_name == name) bytes += package.latest().file_size;
        auto chosen_text = store.picked.size() == 1 ? std::string("1 mod selected")
                                                    : std::format("{} mods selected", store.picked.size());
        if (bytes) chosen_text += "  /  " + size_text(bytes) + " to download";
        ImGui::TextUnformatted(chosen_text.c_str());
        ImGui::SameLine();
        const float action = S(190);
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - action - S(90) - S(8) - pad);
        if (ImGui::Button("Clear", ImVec2(S(90), 0))) store.picked.clear();
        ImGui::SameLine();
        ImGui::BeginDisabled(installing);
        push_primary_button();
        if (ImGui::Button(store.picked.size() == 1 ? "INSTALL 1 MOD"
                : std::format("INSTALL {} MODS", store.picked.size()).c_str(), ImVec2(action, 0))) {
            std::vector<ts::Package> chosen;
            for (const auto& name : store.picked)
                for (const auto& package : store.packages)
                    if (package.full_name == name) chosen.push_back(package);
            store.picked.clear();
            start_store_install(panel, std::move(chosen));
        }
        pop_primary_button();
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(bar.x, bar.y + bar_tall));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Spacing();
    const float body = std::max(S(120), height - (ImGui::GetCursorPosY() - top));
    const auto packages = visible_packages(store, installed);

    // ------------------------------------------------ the list
    ImGui::BeginChild("##store_list", ImVec2(0, body), ImGuiChildFlags_NavFlattened);
    const auto note = [](const char* text) {
        ImGui::Spacing();
        ImGui::Indent(S(14));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(S(14));
    };
    if (!store.loaded) {
        if (store.loading || store.error.empty()) note("Loading mods from Thunderstore...");
        else {
            ImGui::Spacing();
            ImGui::Indent(S(14));
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger), "Thunderstore could not be reached: %s",
                store.error.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Try again")) refresh_listing(panel, ImGui::GetTime(), true);
            ImGui::Unindent(S(14));
        }
    } else if (store.packages.empty()) {
        note("No mods on Thunderstore yet. Made one? Package it with a manifest.json, icon.png and README.md "
             "and upload it to thunderstore.io/c/reskate.");
    } else if (packages.empty()) {
        note("No mods match your search.");
    }

    const float row = S(80) + row_gap();
    ImGui::BeginDisabled(installing);
    virtual_rows(static_cast<int>(packages.size()), [&](int) { return row; }, [&](int index, float slot) {
        const float tall = slot - row_gap();
        const auto& package = *packages[static_cast<std::size_t>(index)];
        const auto& version = package.latest();
        const auto found = installed.find(ts::folder_for(package.full_name));
        const bool update = ts::update_available(package, installed);
        // Tools (Blender add-ons, utilities) are not game mods: their page has the download.
        const bool tool = package.in_category("Tools") && !package.in_category("Mods");
        bool ticked = picked(store, package.full_name);

        ImGui::PushID(package.full_name.c_str());
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        // The row itself opens the overview; the widgets on it keep their clicks.
        begin_row();
        if (list_row("##row", width, tall, ticked)) {
            store.selected = package.full_name;
            store.overview = false;
        }
        row_buttons();
        const float right = start.x + width;
        const float text_x = start.x + S(82);
        mod_icon(panel, &package, ImVec2(start.x + S(14), start.y + S(11)), S(56));
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(text_x, start.y + S(10)), color::text,
            package.title().c_str());
        const float title_width =
            fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, package.title().c_str()).x;
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x + title_width + S(8), start.y + S(12)),
            color::muted, ("by " + package.owner).c_str());

        // ------------------------------------------- tick and install, at the right
        const float box = ImGui::GetFrameHeight();
        const float tick_x = right - S(16) - box;
        const float button = S(118);
        const float button_x = tick_x - S(12) - button;
        ImGui::SetCursorScreenPos(ImVec2(tick_x, start.y + (tall - box) * 0.5f));
        ImGui::BeginDisabled(tool);
        if (ImGui::Checkbox("##pick", &ticked)) pick(store, package.full_name, ticked);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(tool ? "A tool, not a game mod: get it from its Thunderstore page."
                                   : "Tick to install this with the others you tick");
        ImGui::SetCursorScreenPos(ImVec2(button_x, start.y + (tall - S(32)) * 0.5f));
        if (tool) {
            if (ImGui::Button("THUNDERSTORE", ImVec2(button, S(32)))) open_url(package.package_url);
        } else if (update) {
            push_primary_button();
            if (ImGui::Button("UPDATE", ImVec2(button, S(32)))) start_store_install(panel, {package});
            pop_primary_button();
        } else if (found == installed.end()) {
            push_primary_button();
            if (ImGui::Button("INSTALL", ImVec2(button, S(32)))) start_store_install(panel, {package});
            pop_primary_button();
        } else {
            const auto mark = "INSTALLED";
            badge(draw, fonts, ImVec2(button_x + button - badge_width(fonts, mark),
                start.y + (tall - fonts.caption->FontSize - S(8)) * 0.5f), mark, color::good, color::ink);
        }

        auto line = version.description;
        std::replace_if(line.begin(), line.end(),
            [](char ch) { return ch == 0x0a || ch == 0x0d || ch == 0x09; }, ' ');
        // One line, clipped rather than wrapped, so every row is the same height.
        const ImVec4 clip(text_x, start.y, button_x - S(14), start.y + tall);
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(text_x, start.y + S(32)), color::text, line.c_str(),
            nullptr, 0, &clip);
        draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(text_x, start.y + S(55)), color::muted,
            std::format("v{}{}  /  {} downloads  /  {}", version.number,
                version.file_size ? "  /  " + size_text(version.file_size) : std::string(),
                count_text(package.downloads), date_text(package.date_updated)).c_str());
        end_row();
        ImGui::PopID();
    });
    ImGui::EndDisabled();
    keep_focus_in_list();
    ImGui::EndChild();
}

namespace {

// ---------------------------------------------------------------- READMEs

constexpr std::uint64_t max_readme_bytes = 512 * 1024;

void readme_worker(Readmes& readmes) {
    std::atomic<bool> never{};
    for (;;) {
        Readmes::Job job;
        {
            std::unique_lock lock(readmes.mutex);
            readmes.wake.wait(lock, [&] { return readmes.stop || !readmes.queue.empty(); });
            if (readmes.stop) break;
            job = std::move(readmes.queue.front());
            readmes.queue.pop_front();
        }
        Readmes::Done done{job.key, {}, false};
        try {
            done.markdown = ts::parse_readme(fetch(job.url, max_readme_bytes, never));
        } catch (const std::exception& failure) {
            done.failed = true;
            logging::write(logging::Level::warning, logging::Channel::launcher,
                std::string("Could not get the README of ") + job.key + ": " + failure.what());
        }
        std::lock_guard lock(readmes.mutex);
        readmes.done.push_back(std::move(done));
    }
}

// The entry to draw, asked for or read the first time. Null: nothing to show.
const Readmes::Entry* readme_for(ModsPanel& panel, const ts::Package* package, const mods::Mod* mod) {
    auto& readmes = panel.readmes;
    {
        std::lock_guard lock(readmes.mutex);
        for (auto& done : readmes.done) {
            auto& entry = readmes.entries[done.key];
            entry.loading = false;
            entry.failed = done.failed;
            entry.readme = ts::readme_lines(done.markdown);
        }
        readmes.done.clear();
    }
    // An installed mod's own README.md is what is on this PC, so it comes first.
    if (mod) {
        const auto key = "folder:" + mod->name + ":" + mod->version;
        auto found = readmes.entries.find(key);
        if (found == readmes.entries.end()) {
            Readmes::Entry entry;
            std::error_code error;
            const auto file = mod->directory / "README.md";
            if (const auto size = fs::file_size(file, error); !error && size > 0 && size <= max_readme_bytes) {
                const auto bytes = read_bytes(file);
                std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
                entry.readme = ts::readme_lines(text);
            }
            found = readmes.entries.emplace(key, std::move(entry)).first;
        }
        if (!found->second.readme.lines.empty()) return &found->second;
    }
    if (!package) return nullptr;
    const auto key = package->latest().full_name;
    auto found = readmes.entries.find(key);
    if (found == readmes.entries.end()) {
        found = readmes.entries.emplace(key, Readmes::Entry{true, false, {}}).first;
        {
            std::lock_guard lock(readmes.mutex);
            readmes.queue.push_back({key, ts::readme_url(*package)});
        }
        if (!readmes.worker.joinable()) readmes.worker = std::thread([&readmes] { readme_worker(readmes); });
        readmes.wake.notify_one();
    }
    return &found->second;
}

} // namespace

// The two columns of an overview, under its header: the README on the left, the facts in a panel
// of their own on the right. Both are as tall as the room above the overview's last row.
OverviewColumns overview_columns(float popup_height) {
    OverviewColumns columns;
    columns.gap = S(18);
    columns.height = std::max(S(140), popup_height - ImGui::GetCursorPosY() - S(24) - ImGui::GetFrameHeight() - S(12));
    const float room = ImGui::GetContentRegionAvail().x;
    columns.details = std::clamp(room * 0.32f, S(220), S(320));
    columns.readme = room - columns.details - columns.gap;
    return columns;
}

void begin_overview_details(const char* id, const OverviewColumns& columns) {
    ImGui::SameLine(0, columns.gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(rgba(31, 31, 34)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild(id, ImVec2(columns.details, columns.height),
        ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
}

void end_overview_details() {
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void overview_fact(const Fonts& fonts, const char* name, const std::string& value) {
    if (value.empty()) return;
    field(fonts, name, value);
    ImGui::Dummy(ImVec2(0, S(6)));
}

void draw_readme(const Fonts& fonts, const thunderstore::Readme& readme) {
    using Kind = ts::ReadmeLine::Kind;
    ImGui::PushTextWrapPos(0);
    for (const auto& line : readme.lines) {
        switch (line.kind) {
        case Kind::heading:
            if (&line != &readme.lines.front()) ImGui::Dummy(ImVec2(0, S(8)));
            ImGui::PushFont(fonts.bold);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopFont();
            break;
        case Kind::bullet:
            ImGui::Bullet();
            ImGui::TextUnformatted(line.text.c_str());
            break;
        case Kind::code:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            break;
        case Kind::rule: ImGui::Separator(); break;
        case Kind::gap: ImGui::Spacing(); break;
        case Kind::text: ImGui::TextUnformatted(line.text.c_str()); break;
        }
    }
    ImGui::PopTextWrapPos();
}

bool readme_field(const Fonts& fonts, ModsPanel& panel, const thunderstore::Package* package, const mods::Mod* mod) {
    const auto* entry = readme_for(panel, package, mod);
    if (!entry || (!entry->loading && !entry->failed && entry->readme.lines.empty())) return false;
    if (entry->loading) {
        ImGui::TextDisabled("Loading the README...");
        return true;
    }
    if (entry->failed) {
        ImGui::TextDisabled("Could not load the README. It is on the mod's Thunderstore page.");
        return true;
    }
    draw_readme(fonts, entry->readme);
    if (entry->readme.cut) ImGui::TextDisabled("The rest is on the mod's Thunderstore page.");
    return true;
}

void package_overview(const Fonts& fonts, ModsPanel& panel, ImVec2 size, bool installing) {
    auto& store = panel.store;
    if (store.selected.empty()) return;
    const ts::Package* selected = nullptr;
    for (const auto& package : store.packages)
        if (package.full_name == store.selected) selected = &package;
    if (!selected) {
        store.selected.clear();
        return;
    }
    const auto& package = *selected;
    const auto& version = package.latest();
    const auto installed = installed_versions(panel.list);
    const auto found = installed.find(ts::folder_for(package.full_name));

    if (!store.overview) {
        ImGui::OpenPopup("##package_overview");
        store.overview = true;
    }
    const ImVec2 extent(std::min(S(1080), size.x - S(80)), std::min(S(740), size.y - S(60)));
    ImGui::SetNextWindowPos(ImVec2((size.x - extent.x) * 0.5f, (size.y - extent.y) * 0.5f));
    ImGui::SetNextWindowSize(extent);
    if (!ImGui::BeginPopupModal("##package_overview", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        store.overview = false;        // dismissed with Escape
        store.selected.clear();
        return;
    }
    const auto close = [&] {
        store.overview = false;
        store.selected.clear();
        ImGui::CloseCurrentPopup();
    };
    // The icon, with the name, who made it and its one-line description beside it.
    const float icon = S(104), header_top = ImGui::GetCursorPosY();
    mod_icon(panel, &package, ImGui::GetCursorScreenPos(), icon);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + icon + S(20));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted(package.title().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("by %s", package.owner.c_str());
    if (!version.description.empty()) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextUnformatted(version.description.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), header_top + icon) + S(14));

    const bool tool = package.in_category("Tools") && !package.in_category("Mods");
    if (tool) {
        push_primary_button();
        if (ImGui::Button("GET IT ON THUNDERSTORE", ImVec2(0, S(34)))) open_url(package.package_url);
        pop_primary_button();
    } else {
        ImGui::BeginDisabled(installing);
        push_primary_button();
        if (ImGui::Button(action_label(package, installed).c_str(), ImVec2(0, S(34)))) {
            start_store_install(panel, {package});
            close();
        }
        pop_primary_button();
        ImGui::EndDisabled();
        if (!package.package_url.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Thunderstore page", ImVec2(0, S(34)))) open_url(package.package_url);
        }
    }
    if (version.website_url.starts_with("https://")) {
        ImGui::SameLine();
        if (ImGui::Button("Website", ImVec2(0, S(34)))) open_url(version.website_url);
    }
    ImGui::Spacing();

    ImGui::Dummy(ImVec2(0, S(6)));
    const auto columns = overview_columns(extent.y);
    ImGui::BeginChild("##overview_body", ImVec2(columns.readme, columns.height), ImGuiChildFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
    if (package.deprecated)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning),
            "Deprecated: its author no longer supports it.");
    if (package.nsfw)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::warning), "Marked as not safe for work.");
    if (!readme_field(fonts, panel, &package, nullptr)) ImGui::TextDisabled("This mod has no README.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    begin_overview_details("##overview_details", columns);
    overview_fact(fonts, "VERSION", "v" + version.number);
    if (found != installed.end())
        overview_fact(fonts, "INSTALLED", found->second.empty() ? std::string("yes") : "v" + found->second);
    overview_fact(fonts, "DOWNLOADS", count_text(package.downloads));
    overview_fact(fonts, "UPDATED", date_text(package.date_updated));
    if (version.file_size) overview_fact(fonts, "SIZE", size_text(version.file_size));
    std::string names;
    for (const auto& category : package.categories) names += (names.empty() ? "" : ", ") + category;
    overview_fact(fonts, "CATEGORIES", names);
    overview_fact(fonts, "FOLDER", "Mods\\" + ts::folder_for(package.full_name));
    end_overview_details();

    ImGui::SetCursorPosY(extent.y - S(24) - ImGui::GetFrameHeight());
    bool ticked = picked(store, package.full_name);
    ImGui::BeginDisabled(tool);
    if (ImGui::Checkbox("Install with the others I tick", &ticked)) pick(store, package.full_name, ticked);
    ImGui::EndDisabled();
    ImGui::SameLine(extent.x - S(28) - S(110));
    // Escape too, and so a controller's B: the modal has no other way out but CLOSE.
    if (ImGui::Button("CLOSE", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close();
    ImGui::EndPopup();
}

} // namespace dingosdk::launcher_gui::detail
