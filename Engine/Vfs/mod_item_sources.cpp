#include "mod_item_sources.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Core/Platform/shared_file.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace dingosdk::mods {
namespace fs = std::filesystem;

std::optional<std::vector<ItemSource>> read_item_sources_checked(const fs::path& file) noexcept {
    std::vector<ItemSource> sources;
    try {
        const auto text = read_shared_text(file, 64u << 20);
        const auto root = Json::parse(text, {64u << 20, 32, 1u << 24});
        const auto& mods = root.at("mods");
        for (std::size_t i = 0; i < mods.size(); ++i) {
            const auto& entry = mods.at(i);
            ItemSource source{entry.at("mod").string(), entry.value("title", std::string()), entry.value("author", std::string()),
                              entry.value("version", std::string()), entry.value("folder", std::string()), {}};
            const auto& keys = entry.at("items");
            for (std::size_t k = 0; k < keys.size(); ++k) source.keys.push_back(keys.at(k).string());
            sources.push_back(std::move(source));
        }
    } catch (...) {
        return std::nullopt;
    }
    return sources;
}

std::vector<ItemSource> read_item_sources(const fs::path& file) noexcept {
    return read_item_sources_checked(file).value_or(std::vector<ItemSource>{});
}

void write_item_sources(const fs::path& file, const std::vector<ItemSource>& sources) {
    auto mods = Json::array();
    for (const auto& source : sources) {
        auto entry = Json::object();
        entry["mod"] = source.mod;
        entry["title"] = source.title;
        entry["author"] = source.author;
        entry["version"] = source.version;
        entry["folder"] = source.folder;
        entry["items"] = Json(source.keys);
        mods.push_back(std::move(entry));
    }
    auto root = Json::object();
    root["schema"] = 1u;
    root["mods"] = std::move(mods);
    const auto text = root.dump(1);
    // Whole or not at all: the game may read it while a merge runs beside it.
    auto temporary = file;
    temporary += L".new";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.flush()) throw std::runtime_error("Cannot write " + path_utf8(temporary));
    }
    std::error_code error;
    fs::rename(temporary, file, error);
    if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("Cannot write " + path_utf8(file));
    }
}

std::string item_source_name(const ItemSource& source) {
    std::string name = source.title.empty() ? source.mod : source.title;
    std::replace(name.begin(), name.end(), '_', ' ');
    if (!source.author.empty()) name += " by " + source.author;
    return name;
}

} // namespace dingosdk::mods
