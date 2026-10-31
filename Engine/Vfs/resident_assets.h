#pragma once
#include "mod_list.h"
#include "Engine/Core/Json/json.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace dingosdk::mods {
// Shared by the merge and the plugin manifest validator: a malformed shader
// declaration must not make the host accept roots that the merge discards.
inline std::vector<ResidentAssets> parse_resident_assets(const Json& list) {
    const auto name_of = [](const Json& value) {
        if (!value.is_string() || value.string().empty() || value.string().size() > 260)
            throw std::runtime_error("a name is 1 to 260 characters");
        auto name = value.string();
        for (auto& c : name) {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e || c == '\\')
                throw std::runtime_error("names are printable ASCII with / between folders");
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        }
        return name;
    };
    const auto names_of = [&](const Json& names, std::size_t most) {
        if (!names.is_array() || names.size() > most)
            throw std::runtime_error("roots is a list of at most " + std::to_string(most) + " names");
        std::vector<std::string> out;
        for (const auto& item : names) out.push_back(name_of(item));
        return out;
    };
    if (!list.is_array() || list.size() > 16) throw std::runtime_error("resident is a list of at most 16 bundles");
    std::vector<ResidentAssets> result;
    for (const auto& row : list) {
        if (!row.is_object() || !row.contains("bundle")) throw std::runtime_error("each resident entry names its bundle");
        ResidentAssets entry;
        entry.bundle = name_of(row.at("bundle"));
        if (row.contains("roots")) entry.roots = names_of(row.at("roots"), 512);
        if (row.contains("shaders")) {
            const auto& shaders = row.at("shaders");
            if (!shaders.is_array() || shaders.size() > 32) throw std::runtime_error("shaders is a list of at most 32 depots");
            for (const auto& shader : shaders) {
                if (!shader.is_object() || !shader.contains("name") || !shader.contains("roots"))
                    throw std::runtime_error("each shader names its depot and its roots");
                entry.shaders.push_back({name_of(shader.at("name")), names_of(shader.at("roots"), 64)});
                if (entry.shaders.back().roots.empty()) throw std::runtime_error("a shader needs its roots");
            }
        }
        result.push_back(std::move(entry));
    }
    return result;
}
}
