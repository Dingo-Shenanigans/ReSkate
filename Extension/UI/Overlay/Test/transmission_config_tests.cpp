#include "Engine/Core/Json/json.h"
#include "Extension/UI/Overlay/transmission_config.h"
#include <Windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace dingosdk;
namespace detail = dingosdk::overlay::detail;
namespace fs = std::filesystem;
namespace
{
int failures{};
void check(bool ok, const char *message)
{
    if (!ok)
    {
        ++failures;
        std::cerr << message << '\n';
    }
}
template <class F> void rejects(F action, const char *message)
{
    try
    {
        action();
        check(false, message);
    }
    catch (const std::exception &)
    {
    }
}
void put(std::vector<char> &out, std::uint32_t value)
{
    for (unsigned n = 0; n < 32; n += 8)
        out.push_back(static_cast<char>(value >> n));
}
void set(std::vector<char> &out, std::size_t at, std::uint32_t value)
{
    std::memcpy(out.data() + at, &value, 4);
}
// Format-only fixture, deliberately not executable shader bytecode. The GPU
// test uses compiled authored shaders; parser tests need no compiler/game data.
std::vector<char> shader(bool extra = false, bool wrong = false)
{
    std::vector<char> result(32 + 6 * 4);
    std::memcpy(result.data(), "DXBC", 4);
    set(result, 28, 6);
    for (std::size_t i = 0; i < 6; ++i)
    {
        set(result, 32 + i * 4, static_cast<std::uint32_t>(result.size()));
        const char *name = std::array{"DXIL", "ISG1", "OSG1", "PSV0", "SFI0", "STAT"}[i];
        result.insert(result.end(), name, name + 4);
        if (i == 2)
        {
            const std::uint32_t count = extra ? 5u : 4u;
            put(result, 8 + 32 * count);
            put(result, count);
            put(result, 8);
            for (std::uint32_t target = 0; target < count; ++target)
            {
                std::vector<char> row(32);
                set(row, 8, target);
                set(row, 12, 64);
                set(row, 16, target == 3 && !wrong ? 1u : 3u);
                set(row, 20, target);
                set(row, 24, 15);
                result.insert(result.end(), row.begin(), row.end());
            }
        }
        else
        {
            put(result, 4);
            put(result, static_cast<std::uint32_t>(i + 1));
        }
    }
    set(result, 24, static_cast<std::uint32_t>(result.size()));
    return result;
}
void write(const fs::path &path, const std::vector<char> &bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
} // namespace
int main()
try
{
    const auto root =
        fs::temp_directory_path() / ("reskate-transmission-config-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    struct Cleanup
    {
        fs::path root;
        ~Cleanup()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
    } cleanup{root};
    auto bytes = shader();
    write(root / "mask.dxil", bytes);
    const auto key = detail::transmission_shader_key(bytes);
    check(key.size() == 64, "canonical fingerprint length");
    auto changed = bytes;
    changed.back() ^= 1;
    check(detail::transmission_shader_key(changed) == key, "debug statistics do not alter executable identity");
    changed = bytes;
    changed[64] ^= 1;
    check(detail::transmission_shader_key(changed) != key, "modified executable changes identity");
    check(detail::transmission_shader_key(std::span(bytes.data(), 12)).empty(), "truncated container refused");
    changed = bytes;
    set(changed, 32, 0xffffffff);
    check(detail::transmission_shader_key(changed).empty(), "invalid chunk offset refused");
    auto doc = Json::parse("{\"schema\":1,\"surfaces\":[{\"shader\":\"" + key + "\",\"mask\":\"mask.dxil\"}]}");
    auto parse = [&] { return detail::parse_transmission_config(doc.dump(), root); };
    auto rows = parse();
    check(rows.size() == 1 && rows[0].mask == bytes, "valid bounded mod resource loaded");
    check(rows[0].parameters[3] == 1.52f, "material defaults");
    doc["surfaces"][0]["ior"] = 1.3;
    doc["surfaces"][0]["absorption"] = Json::array({1, 2, 3});
    rows = parse();
    check(rows[0].parameters[3] == 1.3f && rows[0].parameters[2] == 3, "per-surface optical settings");
    for (const auto schema : {"1.5", "4294967297", "-1"})
    {
        auto bad = doc;
        bad["schema"] = Json::parse(schema);
        rejects([&] { detail::parse_transmission_config(bad.dump(), root); }, "invalid schema refused");
    }
    for (const auto *name : {"../mask.dxil", "C:/mask.dxil", "/mask.dxil", "folder\\mask.dxil", "mask.dxil:stream"})
    {
        auto bad = doc;
        bad["surfaces"][0]["mask"] = name;
        rejects([&] { detail::parse_transmission_config(bad.dump(), root); }, "escaping path refused");
    }
    doc["surfaces"][0]["ior"] = 3;
    rejects(parse, "unsupported IOR refused");
    doc["surfaces"][0]["ior"] = 1.52;
    auto duplicate = doc;
    duplicate["surfaces"].push_back(doc["surfaces"][0]);
    rejects([&] { detail::parse_transmission_config(duplicate.dump(), root); }, "duplicate program owner refused");
    auto partial = shader();
    std::uint32_t signature_at{};
    std::memcpy(&signature_at, partial.data() + 40, 4);
    set(partial, signature_at + 8 + 8 + 24, 7);
    write(root / "mask.dxil", partial);
    rejects(parse, "partial render target channels refused");
    write(root / "mask.dxil", shader(true));
    rejects(parse, "extra render outputs refused");
    write(root / "mask.dxil", shader(false, true));
    rejects(parse, "wrong calibration channel type refused");
    check(detail::read_transmission_config(root).empty(), "no declaration is opt-out");
    std::vector<detail::TransmissionRecipe> accepted;
    detail::TransmissionRecipe held;
    held.shader = "held";
    held.mask.resize(32 * 1024 * 1024 - 1);
    accepted.push_back(std::move(held));
    detail::TransmissionRecipe first, second;
    first.shader = "first";
    first.mask.resize(1);
    second.shader = "second";
    second.mask.resize(1);
    rejects([&] { detail::append_transmission_recipes(accepted, {first, second}); }, "global byte budget refused");
    check(accepted.size() == 1 && accepted[0].shader == "held", "over-budget mod publishes no partial declarations");
    check(detail::append_transmission_recipes(accepted, {first}) == 0 && accepted.size() == 2,
          "exact byte budget accepted");
    first.mask.resize(4 * 1024 * 1024);
    check(detail::append_transmission_recipes(accepted, {first}) == 1 && accepted.size() == 2,
          "duplicates do not consume budget or replace owner");
    accepted.clear();
    for (unsigned i = 0; i < 31; ++i)
    {
        detail::TransmissionRecipe entry;
        entry.shader = std::to_string(i);
        accepted.push_back(std::move(entry));
    }
    rejects([&] { detail::append_transmission_recipes(accepted, {first, second}); }, "global recipe budget refused");
    check(accepted.size() == 31, "recipe budget failure is atomic");
    std::cout << "Transmission declaration checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
catch (const std::exception &error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
