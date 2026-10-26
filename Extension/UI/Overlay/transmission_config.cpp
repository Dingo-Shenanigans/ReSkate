#include "transmission_config.h"
#include "Engine/Core/Json/json.h"
#include "material_shader_identity.h"

#include <Windows.h>
#include <algorithm>
#include <bcrypt.h>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace dingosdk::overlay::detail
{
namespace fs = std::filesystem;
namespace
{
std::vector<char> read(const fs::path &path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("Cannot read transmission resource");
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > limit)
        throw std::runtime_error("Transmission resource exceeds its size limit");
    std::vector<char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Transmission resource read was incomplete");
    return bytes;
}
fs::path local_resource(const fs::path &root, const std::string &name)
{
    if (name.empty() || name.size() > 240 || name.find(':') != std::string::npos ||
        name.find('\\') != std::string::npos)
        throw std::runtime_error("Transmission mask must be a mod-relative path");
    const auto relative = fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(name.data()), name.size()));
    if (relative.is_absolute())
        throw std::runtime_error("Transmission mask cannot be absolute");
    for (const auto &part : relative)
        if (part == ".." || part == ".")
            throw std::runtime_error("Transmission mask path cannot traverse directories");
    const auto base = fs::canonical(root);
    const auto result = fs::canonical(base / relative);
    const auto inside = result.lexically_relative(base);
    if (inside.empty() || inside.is_absolute() || *inside.begin() == "..")
        throw std::runtime_error("Transmission mask resolves outside its mod");
    return result;
}
float number(const Json &object, const char *name, float fallback, float low, float high)
{
    if (!object.contains(name))
        return fallback;
    const auto &value = object.at(name);
    if (!value.is_number())
        throw std::runtime_error("Transmission setting must be a number");
    const auto result = value.get<float>();
    if (!std::isfinite(result) || result < low || result > high)
        throw std::runtime_error("Transmission setting is outside its supported range");
    return result;
}
} // namespace

std::string transmission_shader_key(std::span<const char> shader)
{
    if (shader.size() > 4 * 1024 * 1024)
        return {};
    std::vector<unsigned char> bytes;
    for (const char *name : {"DXIL", "ISG1", "OSG1", "PSV0", "SFI0"})
    {
        const auto chunk = shader_chunk(shader, name);
        if (chunk.empty())
            return {};
        bytes.insert(bytes.end(), name, name + 4);
        const auto size = static_cast<std::uint32_t>(chunk.size());
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<unsigned char>(size >> shift));
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()), digest.data(),
                   static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("Cannot fingerprint transmission shader");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

std::vector<TransmissionRecipe> parse_transmission_config(std::string_view text, const fs::path &mod)
{
    const auto root = Json::parse(text, {64 * 1024, 12, 4096});
    if (!root.at("schema").is_number_integer() || root.at("schema").get<std::uint64_t>() != 1)
        throw std::runtime_error("Unsupported transmission schema");
    const auto &rows = root.at("surfaces");
    if (!rows.is_array() || rows.empty() || rows.size() > 16)
        throw std::runtime_error("Transmission requires between one and sixteen surfaces");
    std::vector<TransmissionRecipe> result;
    std::size_t mask_bytes{};
    std::set<std::string> keys;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto &row = rows.at(i);
        TransmissionRecipe recipe;
        recipe.shader = row.at("shader").string();
        if (recipe.shader.size() != 64 ||
            !std::all_of(recipe.shader.begin(), recipe.shader.end(),
                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            !keys.insert(recipe.shader).second)
            throw std::runtime_error("Transmission shader fingerprint must be unique lowercase SHA-256");
        recipe.mask = read(local_resource(mod, row.at("mask").string()), 4 * 1024 * 1024);
        mask_bytes += recipe.mask.size();
        if (mask_bytes > 16 * 1024 * 1024)
            throw std::runtime_error("Transmission mask budget exceeded");
        if (transmission_shader_key(recipe.mask).empty())
            throw std::runtime_error("Transmission mask is not complete DXIL");
        for (std::uint32_t target = 0; target < 4; ++target)
            if (shader_output_component(recipe.mask, target) != (target == 3 ? 1u : 3u))
                throw std::runtime_error("Transmission mask requires float4 targets 0-2 and uint4 target 3");
        for (std::uint32_t target = 4; target < 8; ++target)
            if (shader_output_component(recipe.mask, target))
                throw std::runtime_error("Transmission mask has extra outputs");
        recipe.parameters[3] = number(row, "ior", 1.52f, 1, 2.5f);
        recipe.parameters[4] = number(row, "maximum_offset", 12, 0, 32);
        recipe.parameters[5] = number(row, "pigment_depth", .0006f, 0, .03f);
        recipe.parameters[6] = number(row, "coat", 1, 0, 1);
        recipe.parameters[7] = number(row, "reflection", 1, 0, 1);
        if (row.contains("absorption"))
        {
            const auto &values = row.at("absorption");
            if (!values.is_array() || values.size() != 3)
                throw std::runtime_error("Absorption needs three channels");
            for (std::size_t n = 0; n < 3; ++n)
            {
                if (!values.at(n).is_number())
                    throw std::runtime_error("Absorption must be numeric");
                const auto value = values.at(n).get<float>();
                if (!std::isfinite(value) || value < 0 || value > 200)
                    throw std::runtime_error("Absorption is outside 0-200");
                recipe.parameters[n] = value;
            }
        }
        result.push_back(std::move(recipe));
    }
    return result;
}

std::size_t append_transmission_recipes(std::vector<TransmissionRecipe> &destination,
                                        std::vector<TransmissionRecipe> candidates)
{
    std::set<std::string> keys;
    std::size_t bytes{}, duplicates{};
    for (const auto &recipe : destination)
    {
        keys.insert(recipe.shader);
        bytes += recipe.mask.size();
    }
    std::vector<TransmissionRecipe> additions;
    for (auto &recipe : candidates)
    {
        if (!keys.insert(recipe.shader).second)
        {
            ++duplicates;
            continue;
        }
        if (destination.size() + additions.size() >= 32 || recipe.mask.size() > 32 * 1024 * 1024 - bytes)
            throw std::runtime_error("Global transmission recipe budget exceeded; "
                                     "mod declarations not loaded");
        bytes += recipe.mask.size();
        additions.push_back(std::move(recipe));
    }
    destination.reserve(destination.size() + additions.size());
    for (auto &recipe : additions)
        destination.push_back(std::move(recipe));
    return duplicates;
}

std::vector<TransmissionRecipe> read_transmission_config(const fs::path &mod)
{
    if (!fs::exists(mod / "reskate-transmission.json"))
        return {};
    const auto bytes = read(mod / "reskate-transmission.json", 64 * 1024);
    return parse_transmission_config({bytes.data(), bytes.size()}, mod);
}
} // namespace dingosdk::overlay::detail
