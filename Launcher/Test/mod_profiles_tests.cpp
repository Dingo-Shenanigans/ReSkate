// Mod profiles: storage, applying one to the installed mods, and r2modman's export formats.
#include "Launcher/mod_profiles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

using namespace dingosdk;
namespace profiles = launcher_profiles;

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

mods::ModList installed(std::initializer_list<std::tuple<const char*, const char*, bool>> rows) {
    mods::ModList list;
    for (const auto& [name, version, enabled] : rows) {
        mods::ModEntry entry;
        entry.mod.name = name;
        entry.mod.version = version;
        entry.enabled = enabled;
        list.entries.push_back(std::move(entry));
    }
    return list;
}

std::vector<std::string> order(const std::vector<mods::ModEntry>& entries) {
    std::vector<std::string> names;
    for (const auto& entry : entries) names.push_back(entry.mod.name + (entry.enabled ? "+" : "-"));
    return names;
}

void names() {
    check(profiles::valid_profile_name("Street maps"), "An ordinary name was refused");
    for (const auto* name : {"", " lead", "trail ", "dot.", "a/b", "a\\b", "what?", "a:b", "tab\there"})
        check(!profiles::valid_profile_name(name), "A name that cannot be a file name was accepted");
    check(!profiles::valid_profile_name(std::string(49, 'a')), "An overlong name was accepted");
    check(profiles::clean_profile_name("  My: <best> pack?. ") == "My best pack", "A name was not cleaned");
    check(profiles::clean_profile_name("???") == "Profile", "A name with nothing usable had no fallback");
    check(profiles::valid_profile_name(profiles::clean_profile_name(std::string(80, 'x'))), "A long name was not cut to fit");
    profiles::Profiles set;
    set.list = {{"Pack", {}}, {"Pack (2)", {}}};
    check(set.free_name("Other") == "Other" && set.free_name("pack") == "pack (3)", "A free name was not found");
    check(set.find("PACK") == &set.list[0], "Profiles are not found whatever the case");
}

void applying() {
    const auto list = installed({{"Team-A", "1.0.0", true}, {"Team-B", "2.1.0", false}, {"Local Mod", "wip", true}});
    const auto captured = profiles::capture("Now", list);
    check(captured.mods.size() == 3 && captured.mods[1] == profiles::ProfileMod{"Team-B", "2.1.0", false} &&
          captured.mods[2].version == "0.0.0", "The installed mods were not captured as they are");
    check(profiles::in_use(captured, list), "A profile just captured is not in use");

    const profiles::Profile profile{"Pack", {{"team-b", "2.1.0", true}, {"Team-C", "1.2.3", true}, {"Team-D", "1.0.0", false},
                                             {"Team-A", "1.0.0", false}, {"Team-B", "2.1.0", false}}};
    const auto applied = profiles::apply(profile, list);
    check(order(applied.entries) == std::vector<std::string>{"Team-B+", "Team-A-", "Local Mod-"},
          "A profile's order and switches were not applied, or a mod outside it stayed on");
    check(applied.missing.size() == 1 && applied.missing[0].name == "Team-C" && applied.missing[0].version == "1.2.3",
          "The mods to download are not the missing ones that are switched on");
    check(!profiles::in_use(profile, list), "A profile with a mod to download counts as in use");
    auto after = list;
    after.entries = applied.entries;
    const profiles::Profile have{"Have", {{"Team-B", "2.1.0", true}, {"Team-A", "1.0.0", false}}};
    check(profiles::in_use(have, after) && !profiles::in_use(have, list), "In use does not follow which mods are on");
}

void storage(const std::filesystem::path& root) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    check(profiles::load(root).list.empty(), "A Mods folder with no profiles file has profiles");
    profiles::Profiles set;
    set.list = {{"Street", {{"Team-A", "1.0.0", true}, {"Team-B", "2.0.0", false}}}, {"Empty", {}}};
    set.active = "Street";
    profiles::save(root, set);
    const auto read = profiles::load(root);
    check(read.active == "Street" && read.list == set.list, "Profiles did not survive saving and loading");
    std::ofstream(root / profiles::profiles_file) << R"({"active":"Gone","profiles":[{"name":"ok","mods":[{"name":"../x"},{"name":"Fine"}]},)"
                                                     R"({"name":"bad/name"},{"name":"ok"},7]})";
    const auto loose = profiles::load(root);
    check(loose.list.size() == 1 && loose.list[0].mods.size() == 1 && loose.list[0].mods[0].name == "Fine" &&
          loose.list[0].mods[0].enabled && loose.active.empty(), "A damaged profiles file was not read loosely");
    std::ofstream(root / profiles::profiles_file) << "not json";
    check(profiles::load(root).list.empty(), "An unreadable profiles file was not treated as none");
}

void exporting(const std::filesystem::path& root) {
    const profiles::Profile profile{"My \"best\" pack", {{"Team-Map_One", "1.10.3", true}, {"Local Mod", "0.0.0", false}}};
    const auto text = profiles::export_text(profile);
    check(text.find("profileName: \"My \\\"best\\\" pack\"") == 0 && text.find("      minor: 10\n") != std::string::npos &&
          text.find("    enabled: false\n") != std::string::npos, "The export is not r2modman's layout");
    // clean_profile_name drops the quotes, which a file name cannot hold.
    auto expected = profile;
    expected.name = "My best pack";
    check(profiles::parse_export(text) == expected, "An export did not read back");
    check(profiles::parse_archive(profiles::export_archive(profile)) == expected, "A .r2z did not read back");
    // Left in the scratch folder, for opening with another zip reader.
    const auto archive = profiles::export_archive(profile);
    std::ofstream(root / "exported.r2z", std::ios::binary).write(archive.data(), static_cast<std::streamsize>(archive.size()));
    check(profiles::parse_code_payload(profiles::code_payload(profile)) == expected, "A code payload did not read back");
    check(profiles::code_payload(profile).starts_with("#r2modman\n"), "A code payload lacks r2modman's first line");
    check(profiles::parse_export(profiles::export_text({"Empty", {}})) == profiles::Profile{"Empty", {}},
          "A profile with no mods did not read back");

    // As r2modman's own YAML writer lays one out: unquoted, list items not indented, CRLF.
    const auto theirs = profiles::parse_export("profileName: Default\r\nmods:\r\n- name: BepInEx-BepInExPack\r\n  version:\r\n"
        "    major: 5\r\n    minor: 4\r\n    patch: 2100\r\n  enabled: true\r\n- name: 'Team-Other'\r\n  version:\r\n"
        "    major: 1\r\n    minor: 0\r\n    patch: 0\r\n  enabled: false\r\n- name: bad/name\r\n  enabled: true\r\n");
    check(theirs.name == "Default" && theirs.mods.size() == 2 &&
          theirs.mods[0] == profiles::ProfileMod{"BepInEx-BepInExPack", "5.4.2100", true} &&
          theirs.mods[1] == profiles::ProfileMod{"Team-Other", "1.0.0", false}, "r2modman's own export was misread");

    check(!error_of([] { profiles::parse_export("just: text\n"); }).empty(), "Text that is not a profile was accepted");
    check(!error_of([] { profiles::parse_archive("PK\x03\x04 not really a zip"); }).empty(), "A broken archive was accepted");
    check(!error_of([] { profiles::parse_code_payload("<html>not found</html>"); }).empty(), "A page that is not a profile was accepted");
    check(!error_of([] { profiles::parse_code_payload("#r2modman\n!!!!"); }).empty(), "A payload that is not base64 was accepted");
}

void codes() {
    check(profiles::valid_code("0193a2b4-7c1d-4e5f-8a9b-0c1d2e3f4a5b") && !profiles::valid_code("short") &&
          !profiles::valid_code("0193a2b4/../../etc") && !profiles::valid_code("0193a2b4 7c1d 4e5f"), "Profile codes are not checked");
    check(profiles::parse_created_code(R"({"key":"0193a2b4-7c1d-4e5f-8a9b-0c1d2e3f4a5b"})") == "0193a2b4-7c1d-4e5f-8a9b-0c1d2e3f4a5b",
          "The code in a create answer was not read");
    check(!error_of([] { profiles::parse_created_code(R"({"detail":"throttled"})"); }).empty(), "An answer with no code was accepted");
    check(profiles::code_get_url("abc-123") == L"https://thunderstore.io/api/experimental/legacyprofile/get/abc-123/",
          "The address of a code is wrong");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Pass a scratch folder\n";
        return 2;
    }
    names();
    applying();
    storage(std::filesystem::path(argv[1]) / L"profiles");
    exporting(std::filesystem::path(argv[1]) / L"profiles");
    codes();
    if (failures) return 1;
    std::cout << "Mod profile names, applying, storage, exports and codes passed.\n";
    return 0;
}
