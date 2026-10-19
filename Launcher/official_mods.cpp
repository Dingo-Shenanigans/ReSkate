#include "official_mods.h"
#include "mod_manager.h"
#include "text_encoding.h"
#include "thunderstore.h"
#include "updater.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/builtin_packs.h"
#include "Engine/Vfs/mod_list.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace dingosdk::official_mods {
namespace {
using launcher_text::utf8;
using launcher_text::wide;

constexpr char list_url[] = "https://api.reskate.dev/api/v1/official-mods";
constexpr char list_file[] = "official-mods.json";   // the last list read, in the official folder
constexpr int list_timeout_ms = 15000;
constexpr int download_timeout_ms = 30000;

struct Shared {
    std::mutex mutex;
    std::condition_variable done;
    State state;
    bool started{};
    std::atomic<bool> cancel{};
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
void log(logging::Level level, const std::string& text) { logging::write(level, logging::Channel::launcher, text); }

std::string get(const std::string& url, std::uint64_t limit, int timeout_ms) {
    std::string body;
    launcher_update::http_stream(wide(url), limit, timeout_ms, [&](const char* data, DWORD size) { body.append(data, size); });
    return body;
}
std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}
// "Namespace-Name" with a name of letters, digits and underscores on each side: nothing else may
// become a folder name or a URL path here.
bool plain_full_name(std::string_view name) {
    const auto dash = name.find('-');
    if (dash == std::string_view::npos || !dash || dash + 1 >= name.size() || name.find('-', dash + 1) != std::string_view::npos)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
// An https URL on thunderstore.io or one of its hosts: nothing else is downloaded.
bool thunderstore_url(std::string_view url) {
    if (!url.starts_with("https://")) return false;
    const auto host = url.substr(8, url.find('/', 8) == std::string_view::npos ? std::string_view::npos : url.find('/', 8) - 8);
    return host == "thunderstore.io" || host.ends_with(".thunderstore.io");
}
std::string title_of(std::string_view full_name) {
    std::string title(full_name.substr(full_name.find('-') + 1));
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}
fs::path official_root(const fs::path& mods_root) { return mods_root / std::string(mods::official_folder); }
std::string stamp_of(const fs::path& folder) { return trimmed(read_text(folder / std::string(mods::official_stamp))); }
void write_stamp(const fs::path& folder, const std::string& version) {
    std::ofstream(folder / std::string(mods::official_stamp), std::ios::binary | std::ios::trunc) << (version.empty() ? "0" : version) << '\n';
}
// The version a mod's own manifest.json gives, or empty.
std::string manifest_version(const fs::path& folder) {
    try {
        const auto manifest = Json::parse(read_text(folder / L"manifest.json"));
        return manifest.is_object() ? manifest.value("version_number", "") : std::string{};
    } catch (...) {
        return {};
    }
}
void update(const std::string& full_name, const std::function<void(Mod&)>& change) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    for (auto& mod : s.state.mods)
        if (mod.full_name == full_name) change(mod);
}
// The latest version Thunderstore lists for a mod the list named without one.
void ask_thunderstore(Mod& mod) {
    const auto dash = mod.full_name.find('-');
    const auto url = std::format("{}/api/experimental/package/{}/{}/", thunderstore::site, mod.full_name.substr(0, dash),
                                 mod.full_name.substr(dash + 1));
    const auto package = Json::parse(get(url, 4ull << 20, list_timeout_ms));
    const auto& latest = package.at("latest");
    const auto download = latest.value("download_url", "");
    if (!thunderstore_url(download)) throw std::runtime_error("Thunderstore gave no download for it");
    mod.version = latest.value("version_number", "");
    mod.download_url = download;
    if (mod.description.empty()) mod.description = latest.value("description", "");
    if (mod.icon.empty()) mod.icon = latest.value("icon", "");
}
// Downloads `mod` and puts it in place of the version there, whole or not at all.
void download(const fs::path& root, const Mod& mod, const std::atomic<bool>& cancel) {
    const auto cache = root / L".reskate-download";
    std::error_code error;
    fs::create_directories(cache, error);
    const auto archive = cache / wide(mod.full_name + ".zip");
    struct Cleanup {
        fs::path archive;
        ~Cleanup() { std::error_code ignored; fs::remove(archive, ignored); }
    } cleanup{archive};
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("the download could not be written");
        launcher_update::http_stream(wide(mod.download_url), 4ull << 30, download_timeout_ms, [&](const char* data, DWORD size) {
            if (cancel) throw std::runtime_error("stopped");
            file.write(data, size);
            if (!file) throw std::runtime_error("the download could not be written (is the disk full?)");
        });
    }
    // Installed beside the one in place, then swapped in.
    const auto staging = mod.full_name + ".new";
    fs::remove_all(root / wide(staging), error);
    launcher_mods::InstallOptions options;
    options.folder = staging;
    options.author = mod.full_name.substr(0, mod.full_name.find('-'));
    options.require_content = true;
    launcher_mods::install(root, archive, false, {}, cancel, options);
    const auto target = root / wide(mod.full_name);
    fs::remove_all(target, error);
    fs::rename(root / wide(staging), target, error);
    if (error) {
        fs::remove_all(root / wide(staging), error);
        throw std::runtime_error("the new version could not be put in place");
    }
    write_stamp(target, mod.version);
}

void sync(const fs::path& mods_root) {
    auto& s = shared();
    const auto root = official_root(mods_root);
    std::error_code error;
    fs::create_directories(root, error);

    // The list: ReSkate's, else the last one read, else ReSkate's own.
    std::vector<Mod> list;
    std::string source;
    bool authoritative = true;
    try {
        const auto body = get(list_url, 4ull << 20, list_timeout_ms);
        list = parse_list(body);
        if (list.empty() && body.find("\"mods\"") == std::string::npos) throw std::runtime_error("not a list of official mods");
        std::ofstream(root / list_file, std::ios::binary | std::ios::trunc) << body;
        source = "ReSkate";
    } catch (const std::exception& failure) {
        log(logging::Level::warning, std::string("Official mods: the list could not be read from ReSkate (") + failure.what() + ")");
        list = parse_list(read_text(root / list_file));
        source = "the last list read";
        if (list.empty() && !fs::exists(root / list_file, error)) {
            list = fallback_list();
            source = "ReSkate's own list";
            authoritative = false;
        }
    }
    for (auto& mod : list) mod.installed = stamp_of(root / wide(mod.full_name));
    {
        std::lock_guard lock(s.mutex);
        s.state.mods = list;
        s.state.source = source;
    }
    log(logging::Level::info, std::format("Official mods: {} from {}", list.size(), source));

    for (const auto& line : adopt_and_retire(mods_root, list)) log(logging::Level::info, "Official mods: " + line);
    for (auto& mod : list) {
        if (s.cancel) break;
        mod.installed = stamp_of(root / wide(mod.full_name));
        update(mod.full_name, [&](Mod& shown) { shown.installed = mod.installed; shown.working = true; });
        std::string status;
        try {
            if (mod.version.empty() || mod.download_url.empty()) ask_thunderstore(mod);
            if (mod.installed != mod.version) {
                update(mod.full_name, [&](Mod& shown) {
                    shown.version = mod.version;
                    shown.status = mod.installed.empty() ? "Downloading v" + mod.version : "Updating to v" + mod.version;
                });
                download(root, mod, s.cancel);
                log(logging::Level::info, std::format("Official mods: {} v{} {}", mod.full_name, mod.version,
                                                      mod.installed.empty() ? "installed" : "updated from v" + mod.installed));
                mod.installed = mod.version;
            }
            status = "Up to date";
        } catch (const std::exception& failure) {
            log(logging::Level::warning, std::format("Official mods: {} could not be brought up to date: {}", mod.full_name, failure.what()));
            status = mod.installed.empty() ? "Not downloaded yet: " + std::string(failure.what())
                                           : "Could not check for an update; v" + mod.installed + " stays";
        }
        update(mod.full_name, [&](Mod& shown) {
            shown = mod;
            shown.status = status;
            shown.working = false;
        });
    }
    // An official mod the list no longer names goes (it came from ReSkate, not the player).
    if (authoritative && !s.cancel) {
        std::set<std::string> named;
        for (const auto& mod : list) named.insert(mod.full_name);
        for (fs::directory_iterator at(root, error), end; !error && at != end; at.increment(error)) {
            const auto name = utf8(at->path().filename().wstring());
            if (!at->is_directory(error) || name.starts_with('.') || named.contains(name)) continue;
            std::error_code ignored;
            fs::remove_all(at->path(), ignored);
            log(logging::Level::info, "Official mods: " + name + " is no longer official and was removed");
        }
    }
}
} // namespace

std::vector<Mod> parse_list(std::string_view json) {
    std::vector<Mod> list;
    try {
        const auto answer = Json::parse(json);
        if (!answer.is_object() || !answer.contains("mods") || !answer.at("mods").is_array()) return {};
        const auto& mods = answer.at("mods");
        std::set<std::string> seen;
        for (std::size_t i = 0; i < mods.size(); ++i) {
            const auto& row = mods.at(i);
            if (!row.is_object()) continue;
            Mod mod;
            mod.full_name = row.value("full_name", "");
            if (!plain_full_name(mod.full_name) || !seen.insert(mod.full_name).second) continue;
            mod.title = row.value("title", "");
            if (mod.title.empty()) mod.title = title_of(mod.full_name);
            mod.description = row.value("description", "");
            mod.icon = row.value("icon", "");
            if (!thunderstore_url(mod.icon)) mod.icon.clear();
            mod.version = row.value("version", "");
            mod.download_url = row.value("download_url", "");
            if (!thunderstore_url(mod.download_url) || mod.version.empty()) {
                mod.version.clear();
                mod.download_url.clear();
            }
            list.push_back(std::move(mod));
        }
    } catch (...) {
        return {};
    }
    return list;
}

std::vector<Mod> fallback_list() {
    std::vector<Mod> list;
    for (const auto* name : {"Fourth_Studio-Board_Break_Pack", "Fourth_Studio-Road_Rash"}) {
        Mod mod;
        mod.full_name = name;
        mod.title = title_of(name);
        list.push_back(std::move(mod));
    }
    return list;
}

std::vector<std::string> adopt_and_retire(const fs::path& mods_root, const std::vector<Mod>& official) {
    std::vector<std::string> done;
    try {
        const auto root = official_root(mods_root);
        std::error_code error;
        fs::create_directories(root, error);
        std::vector<std::string> gone;
        for (const auto& mod : official) {
            const auto copy = mods_root / wide(mod.full_name);
            if (!fs::is_directory(copy, error)) continue;
            const auto target = root / wide(mod.full_name);
            if (stamp_of(target).empty()) {
                // Installed by the player already: moved over rather than downloaded again.
                const auto version = manifest_version(copy);
                fs::remove_all(target, error);
                fs::rename(copy, target, error);
                if (!error) {
                    write_stamp(target, version);
                    gone.push_back(mod.full_name);
                    done.push_back(std::format("{} (v{}) moved from the Mods folder", mod.full_name, version.empty() ? "?" : version));
                    continue;
                }
                error.clear();
            }
            try {
                launcher_mods::remove(mods_root, mod.full_name);
                gone.push_back(mod.full_name);
                done.push_back(mod.full_name + ": the copy in the Mods folder went to the Recycle Bin");
            } catch (...) {}
        }
        for (fs::directory_iterator at(mods_root, error), end; !error && at != end; at.increment(error)) {
            const auto name = utf8(at->path().filename().wstring());
            if (!at->is_directory(error) || !mods::retired_mod(name)) continue;
            try {
                launcher_mods::remove(mods_root, name);
                gone.push_back(name);
                done.push_back(name + " is retired and went to the Recycle Bin");
            } catch (...) {}
        }
        // mods.json keeps the rest as it was; a malformed one is left for the player to see.
        if (!gone.empty()) {
            const auto list = mods::scan_mods(mods_root.parent_path());
            if (list.issue.empty()) mods::save_mod_order(mods_root, list.entries);
        }
    } catch (...) {}
    return done;
}

void start(const fs::path& mods_root) {
    auto& s = shared();
    {
        std::lock_guard lock(s.mutex);
        if (s.started) return;
        s.started = true;
        s.state.syncing = true;
    }
    std::thread([mods_root] {
        try {
            sync(mods_root);
        } catch (const std::exception& failure) {
            log(logging::Level::warning, std::string("Official mods could not be put in place: ") + failure.what());
        } catch (...) {}
        auto& s = shared();
        {
            std::lock_guard lock(s.mutex);
            s.state.syncing = false;
            s.state.synced = true;
        }
        s.done.notify_all();
    }).detach();
}

bool wait(std::chrono::milliseconds timeout) {
    auto& s = shared();
    std::unique_lock lock(s.mutex);
    return s.done.wait_for(lock, timeout, [&] { return s.state.synced; });
}

State snapshot() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.state;
}

bool official(std::string_view full_name) {
    if (mods::retired_mod(thunderstore::folder_for(full_name))) return true;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    const auto& mods = s.state.mods.empty() ? fallback_list() : s.state.mods;
    return std::any_of(mods.begin(), mods.end(), [&](const Mod& mod) { return mod.full_name == full_name; });
}
} // namespace dingosdk::official_mods
