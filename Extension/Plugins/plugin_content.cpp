#include "plugin_content.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace dingosdk::plugins::content {
namespace {
// The names mod_list.h's scan_mods takes for a mod folder, as long as read_content takes.
bool mod_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ';
    });
}
bool same_name(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        return lower(x) == lower(y);
    });
}
// manifest.json's "name", 1 to 64 characters, or empty when there is none to read.
std::string manifest_name(const std::filesystem::path& directory) {
    const auto file = directory / "manifest.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || std::filesystem::file_size(file, error) > 32 * 1024 || error) return {};
    try {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const auto root = Json::parse(text.str());
        if (!root.is_object() || !root.contains("name") || !root.at("name").is_string()) return {};
        const auto& name = root.at("name").string();
        return name.size() <= 64 ? name : std::string{};
    } catch (const std::exception&) {
        return {};
    }
}
} // namespace

std::vector<Mod> mod_folders(const std::filesystem::path& mods_root) {
    std::vector<Mod> mods;
    std::error_code error;
    for (std::filesystem::directory_iterator it(mods_root, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry;
        if (!it->is_directory(entry)) continue;
        const auto utf8 = it->path().filename().u8string();
        std::string name(utf8.begin(), utf8.end());
        if (!mod_name(name)) continue;
        auto title = manifest_name(it->path());
        mods.push_back({name, title.empty() ? name : std::move(title), it->path()});
    }
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return mods;
}

void list(const std::vector<Mod>& mods, std::string_view folder, std::vector<Game::ContentFile>& files) {
    for (const auto& mod : mods) {
        const auto root = mod.directory / std::filesystem::path(std::string(folder));
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) continue;
        std::vector<Game::ContentFile> found;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, error), end;
             !error && it != end; it.increment(error)) {
            if (it.depth() >= 4) it.disable_recursion_pending();
            if (!it->is_regular_file(error)) continue;
            const auto relative = it->path().lexically_relative(root).generic_wstring();
            std::string path;
            bool plain = true;
            for (const auto c : relative) {
                if (c < 0x20 || c > 0x7e) plain = false;
                path.push_back(static_cast<char>(c));
            }
            if (!plain || path.ends_with(".tmp")) continue; // a name a plugin could not ask for again
            found.push_back({mod.name, mod.title, std::move(path), static_cast<std::uint64_t>(it->file_size(error))});
            if (files.size() + found.size() >= most_files) break;
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        for (auto& file : found) files.push_back(std::move(file));
        if (files.size() >= most_files) break;
    }
}

std::string read(const std::vector<Mod>& mods, std::string_view mod, std::string_view folder, std::string_view path, std::string& text) {
    const auto found = std::find_if(mods.begin(), mods.end(), [&](const Mod& each) { return same_name(each.name, mod); });
    if (found == mods.end()) return "no such mod is on";
    const auto root = found->directory / std::filesystem::path(std::string(folder));
    const auto file = root / std::filesystem::path(std::string(path));
    std::error_code error;
    const auto real_root = std::filesystem::weakly_canonical(root, error);
    const auto real_file = error ? std::filesystem::path{} : std::filesystem::weakly_canonical(file, error);
    if (error) return "not found";
    const auto relative = real_file.lexically_relative(real_root);
    if (relative.empty() || *relative.begin() == "..") return "outside the mod's folder";
    if (!std::filesystem::is_regular_file(file, error)) return "not found";
    const auto size = std::filesystem::file_size(file, error);
    if (error) return "the file cannot be read";
    if (size > most_bytes) return "the file is larger than 1 MB";
    std::ifstream in(file, std::ios::binary);
    text.resize(static_cast<std::size_t>(size));
    if (!in || !in.read(text.data(), static_cast<std::streamsize>(size))) return "the file cannot be read";
    return {};
}
} // namespace dingosdk::plugins::content
