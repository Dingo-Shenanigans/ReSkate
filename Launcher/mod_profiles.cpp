#include "Launcher/mod_profiles.h"

#include "Engine/Core/Json/json.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace dingosdk::launcher_profiles {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

constexpr char export_name[] = "export.r2x";
constexpr char payload_prefix[] = "#r2modman";
constexpr std::size_t max_export_bytes = 1024 * 1024, max_payload_bytes = 8 * 1024 * 1024;
constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Folder names are compared as Windows compares them.
std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& ch : result)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool semantic(std::string_view version) {
    int parts = 1;
    bool digit = false;
    for (const char ch : version) {
        if (ch == '.') {
            if (!digit) return false;
            ++parts;
            digit = false;
        } else if (ch >= '0' && ch <= '9') digit = true;
        else return false;
    }
    return digit && parts == 3;
}

// A YAML scalar as plain text: quotes taken off, and a double-quoted one's escapes undone.
std::string scalar(std::string_view value) {
    value = trimmed(value);
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        std::string out;
        const auto inner = value.substr(1, value.size() - 2);
        for (std::size_t i = 0; i < inner.size(); ++i) {
            out += inner[i];
            if (inner[i] == '\'' && i + 1 < inner.size() && inner[i + 1] == '\'') ++i;
        }
        return out;
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string out;
        const auto inner = value.substr(1, value.size() - 2);
        for (std::size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '\\' && i + 1 < inner.size()) ++i;
            out += inner[i];
        }
        return out;
    }
    // An unquoted value ends at a comment.
    if (const auto comment = value.find(" #"); comment != std::string_view::npos) value = trimmed(value.substr(0, comment));
    return std::string(value);
}

std::string yaml_quoted(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + '"';
}

std::wstring widen(std::string_view text) { return std::wstring(text.begin(), text.end()); }

struct Zip {
    mz_zip_archive value{};
    bool reader{};
    ~Zip() {
        if (reader) mz_zip_reader_end(&value);
    }
};

// A zip holding one file, stored as it is. miniz is built here without its writer, and a
// profile is a few kilobytes of text.
std::string stored_zip(std::string_view name, std::string_view data) {
    std::string out;
    const auto put = [&](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out += static_cast<char>((value >> (8 * i)) & 0xff);
    };
    const auto crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size()));
    const auto size = static_cast<std::uint32_t>(data.size()), length = static_cast<std::uint32_t>(name.size());
    constexpr std::uint32_t dos_date = 0x21; // 1 January 1980: the format's zero
    // Local header, then the file.
    put(0x04034b50, 4); put(20, 2); put(0, 2); put(0, 2); put(0, 2); put(dos_date, 2);
    put(crc, 4); put(size, 4); put(size, 4); put(length, 2); put(0, 2);
    out += name;
    out += data;
    // Central directory with its one entry.
    const auto central = static_cast<std::uint32_t>(out.size());
    put(0x02014b50, 4); put(20, 2); put(20, 2); put(0, 2); put(0, 2); put(0, 2); put(dos_date, 2);
    put(crc, 4); put(size, 4); put(size, 4); put(length, 2); put(0, 2); put(0, 2); put(0, 2); put(0, 2); put(0, 4); put(0, 4);
    out += name;
    const auto central_size = static_cast<std::uint32_t>(out.size()) - central;
    put(0x06054b50, 4); put(0, 2); put(0, 2); put(1, 2); put(1, 2); put(central_size, 4); put(central, 4); put(0, 2);
    return out;
}

std::string base64(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto at = [&](std::size_t offset) {
            return i + offset < bytes.size() ? static_cast<unsigned char>(bytes[i + offset]) : 0u;
        };
        const unsigned block = (at(0) << 16) | (at(1) << 8) | at(2);
        out += base64_digits[(block >> 18) & 63];
        out += base64_digits[(block >> 12) & 63];
        out += i + 1 < bytes.size() ? base64_digits[(block >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? base64_digits[block & 63] : '=';
    }
    return out;
}

std::string unbase64(std::string_view text) {
    std::string out;
    unsigned block{};
    int bits{};
    for (const char ch : text) {
        if (ch == '=' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        const auto digit = base64_digits.find(ch);
        if (digit == std::string_view::npos) fail("The profile code holds text that is not a profile.");
        block = (block << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((block >> bits) & 0xff);
        }
    }
    return out;
}

} // namespace

Profile* Profiles::find(std::string_view name) {
    for (auto& profile : list)
        if (folded(profile.name) == folded(name)) return &profile;
    return nullptr;
}

std::string Profiles::free_name(std::string_view wanted) const {
    const auto taken = [&](std::string_view name) {
        return std::any_of(list.begin(), list.end(), [&](const Profile& p) { return folded(p.name) == folded(name); });
    };
    std::string name(wanted);
    for (int number = 2; taken(name); ++number) {
        const auto suffix = std::format(" ({})", number);
        name = std::string(wanted.substr(0, max_name_bytes - std::min(max_name_bytes, suffix.size()))) + suffix;
    }
    return name;
}

bool valid_profile_name(std::string_view name) {
    if (name.empty() || name.size() > max_name_bytes || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x20 && byte != 0x7f && std::string_view("<>:\"/\\|?*").find(ch) == std::string_view::npos;
    });
}

std::string clean_profile_name(std::string_view name) {
    std::string out;
    for (const char ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) continue;
        out += ch;
    }
    while (out.size() > max_name_bytes) {
        // Never cut a UTF-8 character in half.
        do out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("Profile") : out;
}

Profiles load(const fs::path& mods_root) noexcept {
    Profiles profiles;
    try {
        std::ifstream input(mods_root / profiles_file, std::ios::binary);
        if (!input) return profiles;
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto root = Json::parse(text, JsonLimits{8 * 1024 * 1024, 8, 1024 * 1024});
        if (!root.is_object() || !root.contains("profiles") || !root.at("profiles").is_array()) return profiles;
        for (const auto& row : root.at("profiles")) {
            if (!row.is_object() || profiles.list.size() >= max_profiles) continue;
            Profile profile;
            profile.name = row.value("name", "");
            if (!valid_profile_name(profile.name) || profiles.find(profile.name)) continue;
            if (row.contains("mods") && row.at("mods").is_array())
                for (const auto& item : row.at("mods")) {
                    if (!item.is_object() || profile.mods.size() >= max_profile_mods) continue;
                    ProfileMod mod{item.value("name", ""), item.value("version", ""), item.value("enabled", true)};
                    if (mods::valid_mod_name(mod.name)) profile.mods.push_back(std::move(mod));
                }
            profiles.list.push_back(std::move(profile));
        }
        profiles.active = root.value("active", "");
        if (!profiles.find(profiles.active)) profiles.active.clear();
    } catch (...) {
        profiles = {};
    }
    return profiles;
}

void save(const fs::path& mods_root, const Profiles& profiles) {
    auto list = Json::array();
    for (const auto& profile : profiles.list) {
        auto mods = Json::array();
        for (const auto& mod : profile.mods) {
            Json row = Json::object();
            row["name"] = mod.name;
            row["version"] = mod.version;
            row["enabled"] = mod.enabled;
            mods.push_back(std::move(row));
        }
        Json row = Json::object();
        row["name"] = profile.name;
        row["mods"] = std::move(mods);
        list.push_back(std::move(row));
    }
    Json root = Json::object();
    root["active"] = profiles.active;
    root["profiles"] = std::move(list);
    const auto target = mods_root / profiles_file;
    auto temporary = target;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << root.dump(2) << '\n';
        if (!output.flush()) fail("Could not write the profiles file in the Mods folder.");
    }
    std::error_code error;
    fs::rename(temporary, target, error);
    if (error) {
        fs::remove(temporary, error);
        fail("Could not replace the profiles file in the Mods folder.");
    }
}

Profile capture(std::string name, const mods::ModList& list) {
    Profile profile;
    profile.name = std::move(name);
    for (const auto& entry : list.entries) {
        if (profile.mods.size() >= max_profile_mods) break;
        profile.mods.push_back({entry.mod.name, semantic(entry.mod.version) ? entry.mod.version : std::string("0.0.0"),
                                entry.enabled});
    }
    return profile;
}

Applied apply(const Profile& profile, const mods::ModList& list) {
    Applied result;
    std::map<std::string, const mods::ModEntry*> installed;
    for (const auto& entry : list.entries) installed.emplace(folded(entry.mod.name), &entry);
    std::set<std::string> used;
    for (const auto& mod : profile.mods) {
        const auto key = folded(mod.name);
        const auto found = installed.find(key);
        if (found == installed.end()) {
            if (mod.enabled) result.missing.push_back(mod);
            continue;
        }
        if (!used.insert(key).second) continue;
        result.entries.push_back(*found->second);
        result.entries.back().enabled = mod.enabled;
    }
    for (const auto& entry : list.entries)
        if (!used.contains(folded(entry.mod.name))) {
            result.entries.push_back(entry);
            result.entries.back().enabled = false;
        }
    return result;
}

bool in_use(const Profile& profile, const mods::ModList& list) {
    const auto applied = apply(profile, list);
    if (!applied.missing.empty()) return false;
    const auto enabled = [](const std::vector<mods::ModEntry>& entries) {
        std::vector<std::string> names;
        for (const auto& entry : entries)
            if (entry.enabled) names.push_back(folded(entry.mod.name));
        return names;
    };
    return enabled(applied.entries) == enabled(list.entries);
}

std::string export_text(const Profile& profile) {
    std::string out = "profileName: " + yaml_quoted(profile.name) + "\nmods:\n";
    for (const auto& mod : profile.mods) {
        std::array<int, 3> parts{};
        if (semantic(mod.version)) {
            std::size_t at{};
            for (auto& part : parts) {
                const auto end = std::min(mod.version.find('.', at), mod.version.size());
                std::from_chars(mod.version.data() + at, mod.version.data() + end, part);
                at = end + 1;
            }
        }
        out += std::format("  - name: {}\n    version:\n      major: {}\n      minor: {}\n      patch: {}\n    enabled: {}\n",
                           yaml_quoted(mod.name), parts[0], parts[1], parts[2], mod.enabled ? "true" : "false");
    }
    if (profile.mods.empty()) out.replace(out.size() - 1, 1, " []\n");
    return out;
}

Profile parse_export(std::string_view text) {
    if (text.size() > max_export_bytes) fail("The profile is too large to read.");
    Profile profile;
    bool named{}, listed{};
    std::array<int, 3> version{};
    const auto finish = [&] {
        if (!profile.mods.empty() && profile.mods.back().version.empty())
            profile.mods.back().version = std::format("{}.{}.{}", version[0], version[1], version[2]);
    };
    for (std::size_t at = 0; at < text.size();) {
        const auto end = std::min(text.find('\n', at), text.size());
        auto line = text.substr(at, end - at);
        at = end + 1;
        const bool top = !line.empty() && line.front() != ' ' && line.front() != '\t' && line.front() != '-';
        line = trimmed(line);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '-') {
            // The next mod. What follows the dash is its first field.
            finish();
            if (profile.mods.size() >= max_profile_mods) fail("The profile lists more mods than can be read.");
            profile.mods.emplace_back();
            version = {};
            line = trimmed(line.substr(1));
            if (line.empty()) continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const auto key = trimmed(line.substr(0, colon));
        const auto value = scalar(line.substr(colon + 1));
        if (top && key == "profileName") {
            profile.name = value;
            named = true;
        } else if (top && key == "mods") {
            listed = true;
        } else if (!profile.mods.empty()) {
            auto& mod = profile.mods.back();
            const auto number = [&](int& part) { std::from_chars(value.data(), value.data() + value.size(), part); };
            if (key == "name") mod.name = value;
            else if (key == "major") number(version[0]);
            else if (key == "minor") number(version[1]);
            else if (key == "patch") number(version[2]);
            else if (key == "enabled") mod.enabled = value != "false";
        }
    }
    finish();
    if (!named || !listed) fail("That is not a mod profile (it has no profileName and mods).");
    profile.name = clean_profile_name(profile.name);
    // A name that could not be a mod folder here is not a mod this launcher can hold.
    std::erase_if(profile.mods, [](const ProfileMod& mod) { return !mods::valid_mod_name(mod.name); });
    return profile;
}

std::string export_archive(const Profile& profile) {
    return stored_zip(export_name, export_text(profile));
}

Profile parse_archive(std::string_view zip_bytes) {
    // r2modman also accepts the bare export.r2x where a .r2z is asked for.
    if (!zip_bytes.starts_with("PK")) return parse_export(zip_bytes);
    Zip zip;
    if (!mz_zip_reader_init_mem(&zip.value, zip_bytes.data(), zip_bytes.size(), 0)) fail("The profile file is not a readable archive.");
    zip.reader = true;
    const auto index = mz_zip_reader_locate_file(&zip.value, export_name, nullptr, 0);
    mz_zip_archive_file_stat entry{};
    if (index < 0 || !mz_zip_reader_file_stat(&zip.value, static_cast<mz_uint>(index), &entry) || entry.m_is_directory ||
        entry.m_is_encrypted)
        fail("The profile file has no export.r2x in it.");
    if (entry.m_uncomp_size > max_export_bytes) fail("The profile is too large to read.");
    std::string text(static_cast<std::size_t>(entry.m_uncomp_size), '\0');
    if (!mz_zip_reader_extract_to_mem(&zip.value, static_cast<mz_uint>(index), text.data(), text.size(), 0))
        fail("The profile file is damaged.");
    return parse_export(text);
}

std::wstring code_create_url() { return L"https://thunderstore.io/api/experimental/legacyprofile/create/"; }

std::wstring code_get_url(std::string_view code) {
    return L"https://thunderstore.io/api/experimental/legacyprofile/get/" + widen(code) + L"/";
}

std::string code_payload(const Profile& profile) {
    return std::string(payload_prefix) + "\n" + base64(export_archive(profile));
}

Profile parse_code_payload(std::string_view payload) {
    if (payload.size() > max_payload_bytes) fail("The profile is too large to read.");
    if (!payload.starts_with(payload_prefix)) fail("That code does not lead to a mod profile.");
    return parse_archive(unbase64(payload.substr(sizeof(payload_prefix) - 1)));
}

std::string parse_created_code(std::string_view json) {
    const auto root = Json::parse(json, JsonLimits{64 * 1024, 4, 1024});
    const auto code = root.is_object() ? root.value("key", "") : std::string();
    if (!valid_code(code)) fail("Thunderstore answered without a profile code.");
    return code;
}

bool valid_code(std::string_view code) {
    return code.size() >= 8 && code.size() <= 64 && std::all_of(code.begin(), code.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == '-';
    });
}

} // namespace dingosdk::launcher_profiles
