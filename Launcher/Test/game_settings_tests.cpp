// The game's settings in its save container: reading them, and rewriting only the ones asked for.
// With a folder as its argument, also checks every container found there (nothing is written).
#include "Launcher/game_settings.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>

using namespace dingosdk;
namespace graphics = launcher_game_settings;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

template<class F> std::string error_of(F&& run) {
    try { run(); } catch (const std::exception& failure) { return failure.what(); }
    return {};
}

using Bytes = std::vector<unsigned char>;

void put(Bytes& out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }
void put_u64(Bytes& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void put_varint(Bytes& out, std::uint64_t value) {
    do {
        const auto byte = static_cast<unsigned char>(value & 0x7f);
        value >>= 7;
        out.push_back(value ? static_cast<unsigned char>(byte | 0x80) : byte);
    } while (value);
}
void entry(Bytes& out, unsigned char type, std::string_view name, const Bytes& value) {
    out.push_back(type);
    put(out, name);
    out.push_back(0);
    out.insert(out.end(), value.begin(), value.end());
}
Bytes text_value(std::string_view text) {
    Bytes out;
    put_varint(out, text.size() + 1);
    put(out, text);
    out.push_back(0);
    return out;
}

// A container as the game writes one: displayname, header and body sections, the body last,
// each with a 16-byte head (payload length, a time), and stale bytes after the end.
graphics::Container container(std::string_view options) {
    Bytes entries;
    entry(entries, 8, "InputDevice", {0, 0, 0, 0});
    entry(entries, 6, "HDR", {1});
    entry(entries, 9, "LevelShaderVer_BAM", {1, 2, 3, 4, 5, 6, 7, 8});
    entry(entries, 7, "TestKey-String", text_value("Value"));
    entry(entries, 0xc, "TestKey-Double", {0, 0, 0, 0, 0, 0, 0, 0});
    entry(entries, 7, "TierManagerUserSettings", text_value(options));
    entry(entries, 0xb, "MinimumKeyframeGap", {0x9a, 0x99, 0x19, 0x3e});
    entries.push_back(0);
    Bytes object{0x82};
    put_varint(object, entries.size());
    object.insert(object.end(), entries.begin(), entries.end());

    graphics::Container out;
    const auto section = [&](std::string_view name, const Bytes& payload) {
        const auto offset = out.data.size();
        put_u64(out.data, payload.size());
        put_u64(out.data, 0x18dd17435ae2e148ULL);
        out.data.insert(out.data.end(), payload.begin(), payload.end());
        Bytes row(0x50, 0);
        std::memcpy(row.data(), name.data(), name.size());
        Bytes numbers;
        put_u64(numbers, 1);
        put_u64(numbers, out.data.size() - offset);
        put_u64(numbers, offset);
        put_u64(numbers, out.data.size());
        std::memcpy(row.data() + 32, numbers.data(), numbers.size());
        return row;
    };
    Bytes name;
    put(name, "Save Data Backup");
    name.push_back(0);
    const auto display = section("displayname", name);
    const auto header = section("header", {0x82, 0x01, 0x00});
    const auto body = section("body", object);
    // (The game lists the body first.)
    for (const auto* row : {&body, &display, &header}) out.index.insert(out.index.end(), row->begin(), row->end());
    put(out.data, std::string_view("\x00\x3e\x00\x00", 4));
    return out;
}

graphics::Options read_options(const graphics::Container& from) {
    return graphics::parse_options(std::get<std::string>(graphics::read_values(from).at(graphics::options_key)));
}
graphics::Container with_options(const graphics::Container& from, const graphics::Options& options) {
    return graphics::with_values(from, {{graphics::options_key, graphics::join_options(options)}});
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char** argv) {
    const std::string text = "Post Process;post_process_ultra|Global Graphics Quality;Custom|Texture Quality;texture_quality_ultra|VSync;v_sync_off";
    {
        const auto options = graphics::parse_options(text);
        check(options.size() == 4 && options[2] == graphics::Option{"Texture Quality", "texture_quality_ultra"}, "options parsed");
        check(graphics::join_options(options) == text, "options joined as they were");
        check(!error_of([] { graphics::parse_options("no separator"); }).empty(), "malformed options refused");
        check(!error_of([] { graphics::join_options({{"A", "b|c"}}); }).empty(), "a value with a separator refused");
    }
    {
        const auto before = container(text);
        auto options = read_options(before);
        check(graphics::join_options(options) == text, "options read from a container");

        // Longer and shorter than before, and long enough that the sizes take another byte.
        for (const std::string value : {std::string("texture_quality_low"), std::string("x"), std::string(300, 'y')}) {
            options[2].value = value;
            const auto after = with_options(before, options);
            check(read_options(after) == options, "rewritten options read back");
            // The rest of the object is untouched: writing the old options again gives the old bytes.
            const auto again = with_options(after, graphics::parse_options(text));
            check(again.index == before.index, "index restored by writing the old options");
            check(again.data.size() == before.data.size() - 4 &&
                  std::equal(again.data.begin(), again.data.end(), before.data.begin()), "data restored, without the stale tail");
        }
        // Sections before the body are byte for byte the same.
        options[2].value = "texture_quality_medium";
        const auto after = with_options(before, options);
        check(std::equal(after.data.begin(), after.data.begin() + 0x34, before.data.begin()), "sections before the body kept");
        check(std::equal(after.index.begin() + 0x50, after.index.end(), before.index.begin() + 0x50), "other index rows kept");
    }
    {
        // The other kinds of value, each changed on its own and together.
        const auto before = container(text);
        const auto values = graphics::read_values(before);
        check(values.at("HDR") == graphics::Value(true) && values.at("InputDevice") == graphics::Value(std::int32_t{0}) &&
              values.at("TestKey-String") == graphics::Value(std::string("Value")) &&
              std::get<float>(values.at("MinimumKeyframeGap")) > 0.149f, "values of every kind read");
        check(!values.contains("LevelShaderVer_BAM") && !values.contains("TestKey-Double"), "kinds not edited are not offered");
        const graphics::Values changes{{"HDR", false}, {"InputDevice", std::int32_t{-7}}, {"MinimumKeyframeGap", 0.25f},
                                       {"TestKey-String", std::string("A longer value than before")}};
        const auto after = graphics::with_values(before, changes);
        const auto stored = graphics::read_values(after);
        bool all = true;
        for (const auto& [key, value] : changes) all = all && stored.at(key) == value;
        check(all, "changed values read back");
        check(read_options(after) == graphics::parse_options(text), "values not changed are kept");
        graphics::Values back;
        for (const auto& [key, value] : changes) back[key] = values.at(key);
        const auto again = graphics::with_values(after, back);
        check(again.index == before.index && std::equal(again.data.begin(), again.data.end(), before.data.begin()),
              "changing them back gives the old bytes");
        check(!error_of([&] { graphics::with_values(before, {{"HDR", std::int32_t{1}}}); }).empty(), "a value of the wrong kind is refused");
        check(!error_of([&] { graphics::with_values(before, {{"NotASetting", true}}); }).empty(), "a setting the game has not saved is refused");
        check(graphics::with_values(before, {}).index == before.index, "no changes is the same container");
    }
    {
        auto broken = container(text);
        broken.index[32 + 8] ^= 1;
        check(!error_of([&] { read_options(broken); }).empty(), "an index that does not match is refused");
        auto none = container(text);
        const std::string key = "TierManagerUserSettings";
        const auto at = std::search(none.data.begin(), none.data.end(), key.begin(), key.end());
        *at = 'X';
        check(!error_of([&] { read_options(none); }).empty(), "a container without the options is refused");
        graphics::Container empty;
        check(!error_of([&] { read_options(empty); }).empty(), "an empty container is refused");
    }
    {
        // Every option offered has a value list, and quality levels resolve.
        bool ok = true, level{};
        for (const auto& setting : graphics::graphics_options()) {
            ok = ok && !setting.choices.empty() && *setting.name && *setting.label;
            if (std::string_view(setting.name) == "Texture Quality")
                level = std::string_view(graphics::level_value(setting, 0)) == "texture_quality_low" &&
                        std::string_view(graphics::level_value(setting, 3)) == "texture_quality_ultra";
            if (std::string_view(setting.name) == "VSync") ok = ok && !graphics::level_value(setting, 1);
        }
        check(ok && level, "offered graphics options are well formed");
        bool items = true;
        for (const auto& item : graphics::items()) {
            items = items && *item.page && *item.section && *item.key && *item.label;
            if (item.kind == graphics::Kind::whole || item.kind == graphics::Kind::real) items = items && item.most > item.least && item.step > 0;
            if (item.kind == graphics::Kind::choice) items = items && !item.choices.empty();
        }
        check(items && graphics::items().size() > 60, "offered settings are well formed");
    }
    {
        // On disk: two containers of the options and one save that is something else.
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / "reskate-graphics-settings-test";
        fs::remove_all(root);
        const auto write = [](const fs::path& file, const Bytes& bytes) {
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        const auto first = container(text);
        for (const char* name : {"8e3010f", "ff1f3ef"}) {
            write(root / "data" / "123" / "title" / name / "DATA", first.data);
            write(root / "data" / "123" / "title" / name / "INDEX", first.index);
        }
        write(root / "data" / "123" / "title" / "other" / "DATA", {1, 2, 3});
        write(root / "data" / "123" / "title" / "other" / "INDEX", {4, 5, 6});
        auto saved = graphics::load(root / "data");
        check(saved.folders.size() == 2 && std::get<std::string>(saved.values.at(graphics::options_key)) == text, "containers found on disk");
        auto options = graphics::parse_options(text);
        options[3].value = "v_sync_on";
        graphics::save(saved, {{graphics::options_key, graphics::join_options(options)}, {"HDR", false}}, root / "backup");
        const auto again = graphics::load(root / "data");
        check(again.folders.size() == 2 && again.values.at("HDR") == graphics::Value(false) &&
              graphics::parse_options(std::get<std::string>(again.values.at(graphics::options_key))) == options, "values stored in every container");
        for (const char* name : {"8e3010f", "ff1f3ef"})
            check(read_options({read_file(root / "data" / "123" / "title" / name / "DATA"),
                                read_file(root / "data" / "123" / "title" / name / "INDEX")}) == options, "each container holds them");
        check(read_file(root / "data" / "123" / "title" / "other" / "DATA") == Bytes{1, 2, 3}, "another save left alone");
        check(read_file(root / "backup" / "123" / "8e3010f" / "DATA") == first.data &&
              read_file(root / "backup" / "123" / "ff1f3ef" / "INDEX") == first.index, "untouched files backed up");
        // The backup is of the files as they first were, not of each later change.
        graphics::save(again, {{"HDR", true}}, root / "backup");
        check(read_file(root / "backup" / "123" / "8e3010f" / "DATA") == first.data, "backup kept from the first change");
        check(graphics::load(root / "missing").folders.empty(), "no saves is not an error");
        fs::remove_all(root);
    }
    // Real containers: read, rewrite with the same options, expect the same bytes.
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path folder(argv[i]);
        const graphics::Container real{read_file(folder / "DATA"), read_file(folder / "INDEX")};
        try {
            const auto options = read_options(real);
            const auto values = graphics::read_values(real);
            // Every value written back as it is, at once.
            const auto same = graphics::with_values(real, values);
            std::size_t offered{};
            for (const auto& item : graphics::items()) offered += values.contains(item.key);
            std::cout << "  " << values.size() << " values, " << offered << " of " << graphics::items().size() << " offered settings saved here\n";
            for (const auto& item : graphics::items()) {
                const auto found = values.find(item.key);
                if (found == values.end()) { std::cout << "  not saved: " << item.key << '\n'; continue; }
                const bool kind = item.kind == graphics::Kind::toggle ? std::holds_alternative<bool>(found->second)
                                : item.kind == graphics::Kind::real ? std::holds_alternative<float>(found->second)
                                : std::holds_alternative<std::int32_t>(found->second);
                check(kind, item.key);
            }
            const bool equal = same.index == real.index && same.data.size() <= real.data.size() &&
                               std::equal(same.data.begin(), same.data.end(), real.data.begin());
            check(equal, "a real container is rebuilt byte for byte");
            std::cout << folder.string() << ": " << options.size() << " options, "
                      << (equal ? "rebuilt identically" : "REBUILT DIFFERENTLY") << '\n';
        } catch (const std::exception& failure) {
            check(false, failure.what());
        }
    }
    if (failures) return 1;
    std::cout << "graphics settings tests passed\n";
    return 0;
}
