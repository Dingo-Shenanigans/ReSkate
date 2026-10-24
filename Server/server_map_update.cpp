#include "server_map_update.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Vfs/thunderstore_package.h"
#include "Extension/Multiplayer/Net/protocol.h"
#ifdef _WIN32
#include "Engine/Vfs/https_download.h"
#else
#include <cstdio>
#endif
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dingosdk::server {
namespace {
namespace fs = std::filesystem;
namespace package = thunderstore_package;

constexpr char community[] = "reskate";
constexpr std::uint64_t largest_answer = 1024 * 1024;

#ifdef _WIN32
std::string get_text(const std::wstring &url) {
    https::Download result;
    auto answer = https::get_text(url, largest_answer, 20, L"ReSkateServer/1", &result);
    if (!answer) throw std::runtime_error("HTTP " + std::to_string(result.http_status) + ", error " + std::to_string(result.error));
    return std::move(*answer);
}
#else
// curl does the TLS, as for the team's lists (global_bans.cpp). The address is made here from a
// package's name, which is letters, digits, "_", "-" and "." only; anything else is refused
// before it reaches a command line.
std::string get_text(const std::wstring &url) {
    std::string address;
    for (const auto wide : url) {
        const char c = wide < 0x80 ? static_cast<char>(wide) : '?';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' || c == ':'))
            throw std::runtime_error("the package's address is not one to ask for");
        address += c;
    }
    const auto command = "curl --silent --show-error --fail --location --max-time 20 --max-filesize " + std::to_string(largest_answer) +
                         " --proto =https --proto-redir =https --user-agent ReSkateServer/1 " + address + " 2>&1";
    auto *pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("curl could not be started");
    std::string output;
    char buffer[4096];
    for (std::size_t count; (count = fread(buffer, 1, sizeof(buffer), pipe)) > 0;)
        if (output.size() < largest_answer) output.append(buffer, count);
    if (pclose(pipe) != 0) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        std::replace(output.begin(), output.end(), '\n', ' ');
        throw std::runtime_error(output.empty() ? "curl failed" : output);
    }
    return output;
}
#endif

std::string read(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return in ? text.str() : std::string();
}
bool hidden(const fs::path &folder) {
    const auto name = folder.filename().string();
    return name.empty() || name.front() == '.';
}
} // namespace

std::string map_version(const fs::path &mod) {
    try {
        const auto text = read(mod / "manifest.json");
        return text.empty() ? std::string() : Json::parse(text).value("version_number", "");
    } catch (const std::exception &) {
        return {};
    }
}

void set_map_version(const fs::path &mod, const std::string &version) {
    const auto file = mod / "manifest.json";
    auto root = Json::object();
    // (A manifest that cannot be read is not written over: its mod is left as it is.)
    if (const auto text = read(file); !text.empty()) {
        root = Json::parse(text);
        if (!root.is_object()) throw std::runtime_error("its manifest.json is not an object");
    }
    root["version_number"] = version;
    // Written beside it and moved over it, so a manifest is never half written.
    auto fresh = file;
    fresh += ".new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        out << root.dump(2) << '\n';
        if (!out.flush()) throw std::runtime_error("its manifest.json could not be written");
    }
    std::error_code moved;
    fs::rename(fresh, file, moved);
    if (moved) {
        std::error_code ignored;
        fs::remove(fresh, ignored);
        throw std::runtime_error("its manifest.json could not be replaced (" + moved.message() + ")");
    }
}

std::string required_plugin_problem(const std::string &package_name, bool &reached) {
    reached = false;
    const auto name = package::parse_name(package_name);
    if (!name) return "it is not a Thunderstore package (Owner-Name, or Owner-Name-1.2.3)";
    auto bare = *name;
    bare.version.clear();
    std::string answer;
    try {
        answer = get_text(package::details_url(bare));
    } catch (const std::exception &failure) {
        return std::string("Thunderstore could not be asked about it (") + failure.what() + ")";
    }
    reached = true;
    const auto choice = package::choose(answer, bare, community, package::Kind::plugin);
    if (!choice.ok) return choice.reason.empty() ? "Thunderstore does not list it as a plugin" : choice.reason;
    if (!name->version.empty() && multiplayer::map_version_newer(name->version, choice.newest))
        return "Thunderstore's newest version is " + choice.newest + ", older than " + name->version;
    return {};
}

RequiredPluginCheck::Done RequiredPluginCheck::tick(const std::vector<std::string> &required) {
    Done done;
    if (work_.valid()) {
        if (work_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return done;
        done = work_.get();
        done.finished = true;
    }
    if (!work_.valid() && (!started_ || required != checked_)) {
        started_ = true;
        checked_ = required;
        if (required.empty()) return done;
        work_ = std::async(std::launch::async, [required] {
            Done result;
            for (const auto &plugin : required) {
                bool reached{};
                auto problem = required_plugin_problem(plugin, reached);
                if (problem.empty()) continue;
                if (problem.ends_with('.')) problem.pop_back();
                if (reached) {
                    result.refused.insert(plugin);
                    result.lines.push_back("Required plugin " + plugin + " is left out: " + problem + ". Players are only asked for plugins.");
                } else {
                    result.lines.push_back("Required plugin " + plugin + " could not be checked: " + problem + ". Players' games check it themselves.");
                }
            }
            if (result.lines.empty())
                result.lines.push_back("Required plugins: " + std::to_string(required.size()) + " checked on Thunderstore; players without them are offered them as they join.");
            return result;
        });
    }
    return done;
}

std::vector<MapUpdate> update_maps(const fs::path &mods, const std::string &in_play) {
    std::vector<MapUpdate> result;
    std::error_code error;
    std::vector<fs::path> folders;
    for (const auto &entry : fs::directory_iterator(mods, error))
        if (entry.is_directory(error) && !hidden(entry.path()) && fs::exists(entry.path() / "reskate-levels.json", error))
            folders.push_back(entry.path());
    for (const auto &path : folders) {
        const auto folder = path.filename().string();
        const auto name = package::parse_name(folder);
        if (!name || !name->version.empty()) continue; // not a Thunderstore package's folder
        MapUpdate update;
        update.folder = folder;
        update.from = map_version(path);
        try {
            const auto choice = package::choose(get_text(package::details_url(*name)), *name, community);
            if (!choice.ok) {
                update.problem = choice.reason.empty() ? "Thunderstore does not list it as a map" : choice.reason;
                result.push_back(std::move(update));
                continue;
            }
            update.to = choice.version;
            if (update.to == update.from) continue; // already the newest: nothing to say
            if (folder == in_play) {
                update.waiting = true;
                result.push_back(std::move(update));
                continue;
            }
            set_map_version(path, update.to);
            update.updated = true;
        } catch (const std::exception &e) {
            update.problem = e.what();
            // (A map copied in by hand, or one its author took down.)
            if (update.problem.find("404") != std::string::npos) update.problem = "it is not on Thunderstore under that name";
        }
        result.push_back(std::move(update));
    }
    return result;
}

MapUpdater::Done MapUpdater::tick(bool enabled, unsigned minutes, const fs::path &mods, const std::string &in_play, bool asked) {
    Done done;
    const auto now = std::chrono::steady_clock::now();
    asked_ = asked_ || asked;
    if (work_.valid() && work_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        bool again_soon{};
        std::vector<MapUpdate> found;
        try {
            found = work_.get();
        } catch (const std::exception &e) {
            done.lines.push_back(std::string("Map versions: the check failed (") + e.what() + ").");
            again_soon = true;
        }
        for (const auto &update : found) {
            if (update.updated) {
                done.changed = true;
                done.lines.push_back("Map versions: " + update.folder + " is now at " + update.to + (update.from.empty() ? "" : " (was " + update.from + ")") +
                                     ", Thunderstore's newest; players are sent there.");
                said_.erase(update.folder);
            } else if (update.waiting) {
                again_soon = true;
                if (said_.insert(update.folder + " waits for " + update.to).second)
                    done.lines.push_back("Map versions: " + update.folder + " " + update.to + " is out, and waits until its map is not being played.");
            } else if (!update.problem.empty() && said_.insert(update.folder + ": " + update.problem).second) {
                done.lines.push_back("Map versions: " + update.folder + " was left as it is: " + update.problem);
            }
        }
        next_ = now + (again_soon ? std::chrono::minutes(5) : std::chrono::minutes(std::max(minutes, 5U)));
    }
    if (!work_.valid() && ((enabled && now >= next_) || asked_)) {
        asked_ = false;
        next_ = now + std::chrono::minutes(std::max(minutes, 5U));
        work_ = std::async(std::launch::async, update_maps, mods, in_play);
    }
    return done;
}
} // namespace dingosdk::server
