#pragma once

#include "Engine/Vfs/mod_list.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Mod profiles: named sets of mods ("Street maps", "Server X") a player switches
// between. Every mod is installed once, in the one Mods folder; a profile is a
// saved list of which mods are on, in what order, and at what version. Loading
// one rewrites mods.json.
//
// A profile travels as r2modman does it, so the two managers read each other's:
// export.r2x (YAML) inside a zip (.r2z), and a code that names that zip on
// Thunderstore's profile service. No network here; gui_mods_profiles.cpp sends
// and fetches.
namespace dingosdk::launcher_profiles {

struct ProfileMod {
    std::string name;       // the mod's folder: Namespace-Name for a Thunderstore package
    std::string version;    // Major.Minor.Patch
    bool enabled{true};
    bool operator==(const ProfileMod&) const = default;
};

struct Profile {
    std::string name;
    std::vector<ProfileMod> mods;   // highest priority first, as mods.json lists them
    bool operator==(const Profile&) const = default;
};

struct Profiles {
    std::string active;             // the profile last loaded or saved; empty for none
    std::vector<Profile> list;

    Profile* find(std::string_view name);
    // `wanted`, or `wanted (2)`, `wanted (3)`... when a profile already has that name.
    std::string free_name(std::string_view wanted) const;
};

inline constexpr char profiles_file[] = "profiles.json";
inline constexpr std::size_t max_profiles = 64, max_profile_mods = 2000, max_name_bytes = 48;

// 1 to 48 bytes of printable text, with nothing a file name refuses (a profile
// exports as <name>.r2z) and no leading or trailing space.
bool valid_profile_name(std::string_view name);
// `name` made valid: refused characters dropped, cut to length, "Profile" when nothing is left.
std::string clean_profile_name(std::string_view name);

// <Mods>/profiles.json. A missing or unreadable file is no profiles; never throws.
Profiles load(const std::filesystem::path& mods_root) noexcept;
// Replaces the file atomically. Throws a readable message on failure.
void save(const std::filesystem::path& mods_root, const Profiles& profiles);

// The installed mods as they are now, as a profile.
Profile capture(std::string name, const mods::ModList& list);

struct Applied {
    // What mods.json should hold: the profile's installed mods in its order with its
    // switches, then every installed mod it does not name, switched off.
    std::vector<mods::ModEntry> entries;
    // The mods it switches on that are not installed, to be downloaded.
    std::vector<ProfileMod> missing;
};
Applied apply(const Profile& profile, const mods::ModList& list);
// True when loading `profile` would change nothing: the same mods on, in the same order.
bool in_use(const Profile& profile, const mods::ModList& list);

// export.r2x. parse_export throws a readable message for text that is not one.
std::string export_text(const Profile& profile);
Profile parse_export(std::string_view text);
// A .r2z: a zip holding export.r2x. r2modman's own also carry config files, which are ignored.
std::string export_archive(const Profile& profile);
Profile parse_archive(std::string_view zip);

// Thunderstore's profile service: POST the payload to create_url() as
// application/octet-stream and it answers {"key":"<code>"}; GET get_url(code)
// answers the payload back. A payload is "#r2modman\n" and the .r2z in base64.
std::wstring code_create_url();
std::wstring code_get_url(std::string_view code);
std::string code_payload(const Profile& profile);
Profile parse_code_payload(std::string_view payload);
// The code in a create answer; throws when the answer has none.
std::string parse_created_code(std::string_view json);
// A code as the service gives them: hex digits and dashes. Surrounding space is not part of it.
bool valid_code(std::string_view code);

} // namespace dingosdk::launcher_profiles
