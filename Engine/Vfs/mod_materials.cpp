#include "Engine/Vfs/mod_materials.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace dingosdk::mods {
namespace {
constexpr std::size_t most_materials = 32, most_items = 64, most_parameters = 32;
constexpr float most_number = 100.0f; // board_material::most_number
constexpr std::array<const char*, 4> parts{"deck", "grip", "trucks", "wheels"};

bool text_ok(const std::string& value) {
    return !value.empty() && value.size() <= 255 &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
float finite(const Json& value, const char* what) {
    if (!value.is_number()) throw std::runtime_error(std::string(what) + " is not a number");
    const auto number = value.get<double>();
    if (!std::isfinite(number)) throw std::runtime_error(std::string(what) + " is not finite");
    return static_cast<float>(number);
}
MaterialParameter parameter(const Json& entry) {
    if (!entry.is_object() || !entry.contains("guid") || !entry.at("guid").is_string() || !entry.contains("type") ||
        !entry.at("type").is_string() || !entry.contains("default"))
        throw std::runtime_error("a parameter needs a guid, a type and a default");
    MaterialParameter out;
    if (!material_parameter_key(entry.at("guid").string(), out.key, out.identity))
        throw std::runtime_error("a parameter's guid is 32 hex digits");
    const auto type = entry.at("type").string();
    const auto& fallback = entry.at("default");
    auto guid = entry.at("guid").string();
    std::transform(guid.begin(), guid.end(), guid.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c);
    });
    const auto expected = (type == "colour" || type == "color") ? "59ff5e88" : "2d5cb62b";
    if (guid.substr(16, 8) != expected || guid.substr(24, 4) != "0100")
        throw std::runtime_error("parameter GUID must match its declared type and have count 1");

    if (type == "colour" || type == "color") {
        out.colour = true;
        if (!fallback.is_array() || fallback.size() != 3) throw std::runtime_error("a colour's default is [r, g, b]");
        for (std::size_t c = 0; c < 3; ++c) {
            out.fallback[c] = finite(fallback.at(c), "a colour channel");
            if (out.fallback[c] < 0 || out.fallback[c] > 1) throw std::runtime_error("a colour's channels are 0 to 1");
        }
    } else if (type == "number") {
        out.fallback[0] = finite(fallback, "a number's default");
        if (std::abs(out.fallback[0]) > most_number) throw std::runtime_error("a number is -100 to 100");
    } else {
        // An integer, a switch or a texture picks how the item is drawn: not declarable.
        throw std::runtime_error("a parameter's type is \"colour\" or \"number\"");
    }
    return out;
}
} // namespace

bool material_parameter_key(const std::string& guid, std::uint64_t& key, std::uint16_t& identity) {
    if (guid.size() != 32) return false;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int high = hex_digit(guid[i * 2]), low = hex_digit(guid[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    // As the game serializes a ParamDbKey: the hash is the GUID's first eight bytes, little-endian,
    // and the identity its last two (developer_hoodie_detail::ParameterKey).
    key = 0;
    for (std::size_t i = 0; i < 8; ++i) key |= std::uint64_t{bytes[i]} << (8 * i);
    identity = static_cast<std::uint16_t>(bytes[14] | (bytes[15] << 8));
    return key != 0;
}

std::vector<MaterialDeclaration> parse_material_declarations(const std::string& mod, const std::string& json) {
    const auto root = Json::parse(json);
    if (!root.is_object() || !root.contains("schema") || !root.at("schema").is_number_integer() || root.at("schema").get<int>() != 1 ||
        !root.contains("materials") || !root.at("materials").is_array())
        throw std::runtime_error("expected schema 1 with a materials array");
    const auto& list = root.at("materials");
    if (list.size() > most_materials) throw std::runtime_error("at most 32 materials");
    std::vector<MaterialDeclaration> result;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& entry = list.at(i);
        if (!entry.is_object() || !entry.contains("part") || !entry.at("part").is_string() || !entry.contains("items") ||
            !entry.at("items").is_array() || !entry.contains("parameters") || !entry.at("parameters").is_array())
            throw std::runtime_error("a material needs a part, items and parameters");
        MaterialDeclaration declaration{mod, entry.at("part").string(), {}, {}};
        if (std::find(parts.begin(), parts.end(), declaration.part) == parts.end())
            throw std::runtime_error("a material's part is \"deck\", \"grip\", \"trucks\" or \"wheels\"");
        const auto& items = entry.at("items");
        const auto& parameters = entry.at("parameters");
        if (items.empty() || items.size() > most_items) throw std::runtime_error("a material names 1 to 64 items");
        if (parameters.empty() || parameters.size() > most_parameters) throw std::runtime_error("a material declares 1 to 32 parameters");
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (!items.at(k).is_string() || !text_ok(items.at(k).string())) throw std::runtime_error("an item is its key, as text");
            declaration.items.push_back(items.at(k).string());
        }
        std::set<std::uint64_t> keys;
        for (std::size_t k = 0; k < parameters.size(); ++k) {
            auto p = parameter(parameters.at(k));
            if (!keys.insert(p.key).second) throw std::runtime_error("a parameter is declared twice");
            declaration.parameters.push_back(p);
        }
        result.push_back(std::move(declaration));
    }
    return result;
}

} // namespace dingosdk::mods
