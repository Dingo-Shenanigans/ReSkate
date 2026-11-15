#include "plugin_server_scope.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>

namespace dingosdk::plugins::server_scope {
namespace {
// One writer at a time: the download's thread, the discovery thread and the menu all change it.
std::mutex file_mutex;

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

Folders read_file(const std::filesystem::path& mods_root) {
    Folders folders;
    try {
        std::ifstream input(mods_root / file_name, std::ios::binary);
        if (!input) return folders;
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto json = Json::parse(text, {1 << 20, 4, 65536});
        if (!json.is_array()) return folders;
        for (const auto& folder : json)
            if (folder.is_string() && !folder.string().empty()) folders.insert(lower(folder.string()));
    } catch (const std::exception&) {} // one that cannot be read marks nothing
    return folders;
}

bool write_file(const std::filesystem::path& mods_root, const Folders& folders) {
    const auto file = mods_root / file_name;
    auto temporary = file;
    temporary += ".tmp";
    std::error_code ignored;
    try {
        if (folders.empty()) {
            std::filesystem::remove(file, ignored);
            return !std::filesystem::exists(file, ignored);
        }
        auto json = Json::array();
        for (const auto& folder : folders) json.push_back(folder);
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            const auto text = json.dump(2);
            if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("cannot write it");
        }
        std::filesystem::rename(temporary, file);
        return true;
    } catch (const std::exception&) {
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}

// "Owner-Name-1.2.0" without its version: neither the owner nor the name holds a '-'
// (protocol.h: valid_map_package), so what follows a second one is the version.
std::string_view without_version(std::string_view package) {
    const auto first = package.find('-');
    if (first == std::string_view::npos) return package;
    return package.substr(0, package.find('-', first + 1));
}
}

Folders read(const std::filesystem::path& mods_root) noexcept {
    try {
        std::lock_guard lock(file_mutex);
        return read_file(mods_root);
    } catch (...) { return {}; }
}

bool set(const std::filesystem::path& mods_root, std::string_view folder, bool server_only) noexcept {
    try {
        if (folder.empty()) return false;
        std::lock_guard lock(file_mutex);
        auto folders = read_file(mods_root);
        const auto key = lower(folder);
        if (folders.contains(key) == server_only) return true;
        if (server_only) folders.insert(key);
        else folders.erase(key);
        return write_file(mods_root, folders);
    } catch (...) { return false; }
}

void keep_only(const std::filesystem::path& mods_root, const std::vector<std::string>& present) noexcept {
    try {
        std::lock_guard lock(file_mutex);
        const auto folders = read_file(mods_root);
        if (folders.empty()) return;
        Folders here;
        for (const auto& folder : present) here.insert(lower(folder));
        Folders kept;
        for (const auto& folder : folders)
            if (here.contains(folder)) kept.insert(folder);
        if (kept.size() != folders.size()) write_file(mods_root, kept);
    } catch (...) {}
}

bool names(std::string_view package, std::string_view folder, std::string_view title, std::string_view author) {
    const auto wanted = lower(without_version(package));
    if (wanted.empty()) return false;
    return wanted == lower(folder) || (!title.empty() && !author.empty() && wanted == lower(author) + '-' + lower(title));
}

bool runs(const Folders& server_only, const std::vector<std::string>& required, std::string_view folder, std::string_view title,
          std::string_view author) {
    if (!server_only.contains(lower(folder))) return true;
    return std::any_of(required.begin(), required.end(), [&](const std::string& package) { return names(package, folder, title, author); });
}
}
