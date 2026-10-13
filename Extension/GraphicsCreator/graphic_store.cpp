#include "graphic_store.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dingosdk::graphics_creator {
namespace {
constexpr std::size_t maximum_document_bytes = 1024 * 1024;

std::filesystem::path document_path(const std::filesystem::path& directory, std::string_view name) {
    if (!valid_file_name(name))
        throw std::runtime_error("Use 1-64 letters, numbers, spaces, underscores or hyphens for the graphic name.");
    return directory / (std::string(name) + ".graphic.json");
}

std::string read(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("Could not open graphic document.");
    if (size > maximum_document_bytes) throw std::runtime_error("Graphic document exceeds 1 MB.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open graphic document.");
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream && !stream.eof()) throw std::runtime_error("Could not read graphic document.");
    text.resize(static_cast<std::size_t>(stream.gcount()));
    return text;
}

void replace(const std::filesystem::path& from, const std::filesystem::path& to, std::string_view message) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(std::string(message));
}
} // namespace

bool valid_file_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    for (const auto c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
              c == '_' || c == '-')) return false;
    std::string upper(name);
    for (auto& c : upper) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return false;
    return !(upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
             upper[3] >= '1' && upper[3] <= '9');
}

void save(const std::filesystem::path& directory, std::string_view name, const GraphicDocument& document) {
    const auto path = document_path(directory, name);
    const auto text = encode(document);
    std::filesystem::create_directories(directory);
    auto temporary = path; temporary += L".tmp";
    auto backup = path; backup += L".bak";
    auto backup_temporary = backup; backup_temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("Could not write graphic document. Previous save retained.");
        }
    }
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        std::filesystem::copy_file(path, backup_temporary, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("Could not back up graphic document. Previous save retained.");
        }
        try { replace(backup_temporary, backup, "Could not publish graphic backup. Previous save retained."); }
        catch (...) { std::filesystem::remove(temporary); std::filesystem::remove(backup_temporary); throw; }
    }
    try { replace(temporary, path, "Could not replace graphic document. Previous save retained."); }
    catch (...) { std::filesystem::remove(temporary); throw; }
}

LoadedGraphic load(const std::filesystem::path& directory, std::string_view name) {
    const auto path = document_path(directory, name);
    try { return {decode(read(path)), false}; }
    catch (...) {
        auto backup = path; backup += L".bak";
        try { return {decode(read(backup)), true}; }
        catch (...) { throw; }
    }
}

std::vector<std::string> list(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end && names.size() < 256; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto file = it->path().filename().string();
        constexpr std::string_view suffix = ".graphic.json";
        if (!file.ends_with(suffix)) continue;
        const auto name = file.substr(0, file.size() - suffix.size());
        if (valid_file_name(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace dingosdk::graphics_creator
