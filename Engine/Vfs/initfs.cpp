#include "initfs.h"
#include "native_db.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/initfs.h"
#include "Engine/Core/Platform/path_text.h"

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>

namespace dingosdk::initfs {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
constexpr std::size_t maximum_archive = 64 * 1024 * 1024;
constexpr std::uintptr_t key_rva = addr::initfs::key;
constexpr std::uintptr_t decrypt_reference_rva = addr::initfs::decrypt_reference;
constexpr auto decrypt_reference = addr::initfs::decrypt_reference_prefix;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("InitFS: " + message); }
void crypto_check(NTSTATUS status, const char* operation) {
    if (status < 0) fail(std::string(operation) + " failed (status " + std::to_string(status) + ")");
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}
Bytes read_file(const fs::path& path, std::size_t maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) fail("Cannot open " + path_utf8(path));
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximum) fail("File exceeds size limit: " + path_utf8(path));
    Bytes bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        fail("Cannot read " + path_utf8(path));
    return bytes;
}

struct Secret {
    std::array<unsigned char, 16> bytes{};
    ~Secret() { SecureZeroMemory(bytes.data(), bytes.size()); }
    Secret() = default;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
};
struct Aes {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE key{};
    Bytes object;
    ~Aes() {
        if (key) BCryptDestroyKey(key);
        if (!object.empty()) SecureZeroMemory(object.data(), object.size());
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};
void read_key(const fs::path& executable, Secret& key) {
    // Full build identity is checked before any fixed RVA is used.
    launcher::validate_game_file(executable);
    std::ifstream input(executable, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x1000)
        fail("Invalid executable DOS header");
    input.seekg(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    input.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    if (!input || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.NumberOfSections > 96)
        fail("Invalid executable PE header");
    input.seekg(static_cast<std::streamoff>(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader);
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    input.read(reinterpret_cast<char*>(sections.data()), static_cast<std::streamsize>(sections.size() * sizeof(IMAGE_SECTION_HEADER)));
    if (!input) fail("Truncated executable section table");
    const auto read_rva = [&](std::uintptr_t rva, std::span<unsigned char> output) {
        for (const auto& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const auto offset = rva - section.VirtualAddress;
            if (offset > section.SizeOfRawData || output.size() > section.SizeOfRawData - offset) continue;
            input.seekg(static_cast<std::streamoff>(section.PointerToRawData) + static_cast<std::streamoff>(offset));
            if (!input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size())))
                fail("Cannot read executable contract");
            return;
        }
        fail("Executable contract is outside file-backed sections");
    };
    std::array<unsigned char, decrypt_reference.size()> actual{};
    read_rva(decrypt_reference_rva, actual);
    if (actual != decrypt_reference) fail("InitFS key reference does not match the supported build");
    read_rva(key_rva, key.bytes);
}
Bytes decrypt(std::span<const unsigned char> encrypted, const Secret& secret) {
    if (encrypted.empty() || encrypted.size() % 16 || encrypted.size() > maximum_archive)
        fail("Invalid AES payload length");
    Aes aes;
    crypto_check(BCryptOpenAlgorithmProvider(&aes.algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0), "Open AES");
    crypto_check(BCryptSetProperty(aes.algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0), "Set AES-CBC");
    ULONG size{}, written{};
    crypto_check(BCryptGetProperty(aes.algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0), "Get AES object size");
    aes.object.resize(size);
    crypto_check(BCryptGenerateSymmetricKey(aes.algorithm, &aes.key, aes.object.data(), size,
        const_cast<PUCHAR>(secret.bytes.data()), static_cast<ULONG>(secret.bytes.size()), 0), "Create AES key");
    Secret iv;
    std::copy(secret.bytes.begin(), secret.bytes.end(), iv.bytes.begin());
    Bytes output(encrypted.size());
    crypto_check(BCryptDecrypt(aes.key, const_cast<PUCHAR>(encrypted.data()), static_cast<ULONG>(encrypted.size()),
        nullptr, iv.bytes.data(), static_cast<ULONG>(iv.bytes.size()), output.data(), static_cast<ULONG>(output.size()),
        &written, BCRYPT_BLOCK_PADDING), "Decrypt InitFS");
    output.resize(written);
    return output;
}

struct Entry { std::string name, source; Bytes payload; };
using Entries = std::map<std::string, Entry>;
void apply_archive(const fs::path& path, const Secret& key, Entries& effective, Json& sources) {
    if (!fs::is_regular_file(path)) return;
    auto input = read_file(path, maximum_archive);
    if (input.size() <= native_db::envelope_size ||
        std::memcmp(input.data(), native_db::magic, sizeof(native_db::magic)))
        fail("Unrecognized native envelope in " + path_utf8(path));
    const auto body = std::span<const unsigned char>(input).subspan(native_db::envelope_size);
    std::size_t consumed{};
    auto root = native_db::read(body, "InitFS", &consumed);
    if (consumed != body.size()) fail("Trailing data in native envelope");
    Bytes plaintext;
    if (const auto* encrypted = root.field("encrypted")) {
        if (encrypted->type != 19) fail("Encrypted payload has the wrong DB type");
        plaintext = decrypt(encrypted->bytes, key);
        root = native_db::read(plaintext, "InitFS", &consumed);
        if (consumed != plaintext.size()) fail("Trailing data in decrypted InitFS");
    }
    if (root.type != 1) fail("InitFS root must contain a file list");
    Entries entries;
    bool inherit = false;
    for (const auto& stub : root.children) {
        const auto* file = stub.field("$file");
        if (!file || file->type != 2) fail("Missing $file envelope");
        const auto* name = file->field("name");
        const auto* payload = file->field("payload");
        if (!name || name->type != 7 || !payload || payload->type != 19) fail("Invalid InitFS file entry");
        if (name->text == "__fsinternal__") {
            const auto metadata = native_db::read(payload->bytes, "InitFS");
            const auto* field = metadata.field("inheritContent");
            if (field && field->type != 6) fail("Invalid inheritContent metadata");
            inherit = field && field->boolean;
            continue;
        }
        // Alternate filesystem destinations cannot be represented by this overlay.
        if (const auto* file_system = file->field("fs"); file_system &&
            (file_system->type != 7 || (!file_system->text.empty() && file_system->text != "/data")))
            fail("Unsupported InitFS filesystem destination for " + name->text);
        Entry entry{name->text, path_utf8(path), Bytes(payload->bytes.begin(), payload->bytes.end())};
        if (!entries.emplace(lower(name->text), std::move(entry)).second) fail("Duplicate InitFS virtual path");
    }
    if (!inherit) effective.clear();
    for (auto& [name, entry] : entries) effective.insert_or_assign(name, std::move(entry));
    sources.push_back(Json{{"path", path_utf8(path)}, {"sha256", launcher::sha256_file(path)}, {"inherit", inherit}});
}

bool within(const fs::path& root, const fs::path& path) {
    auto parent = root.begin(), child = path.begin();
    for (; parent != root.end(); ++parent, ++child) {
        if (child == path.end() || _wcsicmp(parent->c_str(), child->c_str())) return false;
    }
    return true;
}
bool write_atomic(const fs::path& root, const fs::path& path, std::span<const unsigned char> bytes, bool replace) {
    if (!within(root, fs::weakly_canonical(path))) fail("Output path escapes the export directory");
    if (!replace && fs::exists(path)) {
        if (!fs::is_regular_file(path)) fail("Output path is not a regular file: " + path_utf8(path));
        return false;
    }
    fs::create_directories(path.parent_path());
    static std::atomic<unsigned long> sequence{};
    const auto temporary = fs::path(path.wstring() + L".reskate-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp");
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) fail("Cannot create output file: " + path_utf8(path));
    DWORD written{};
    const bool ready = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ready) { DeleteFileW(temporary.c_str()); fail("Cannot write output file: " + path_utf8(path)); }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) return true;
    const auto error = GetLastError();
    DeleteFileW(temporary.c_str());
    if (!replace && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) && fs::is_regular_file(path)) return false;
    fail("Cannot publish output file: " + path_utf8(path));
}
Json read_preferences(const fs::path& root) {
    const auto path = root / L"ReSkate.settings.json";
    if (!fs::exists(path)) return Json::object();
    const auto bytes = read_file(path, 65536);
    auto settings = Json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!settings.is_object()) fail("ReSkate.settings.json must contain an object");
    if (settings.contains("loose_files") && !settings["loose_files"].is_boolean())
        fail("ReSkate.settings.json: loose_files must be true or false");
    return settings;
}
}

bool plugin_developer_mode(const fs::path& root) {
    const auto settings = read_preferences(root);
    return settings.contains("plugin_developer_mode") && settings["plugin_developer_mode"].is_boolean() && settings["plugin_developer_mode"].get<bool>();
}
void set_plugin_developer_mode(const fs::path& root, bool on) {
    auto settings = read_preferences(root);
    settings["plugin_developer_mode"] = on;
    const auto text = settings.dump(2);
    write_atomic(root, root / L"ReSkate.settings.json", std::span(reinterpret_cast<const unsigned char*>(text.data()), text.size()), true);
}

fs::path utf8_path(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::optional<std::string> loose_relative_path(std::string_view input) {
    if (input.empty() || input.size() > 1024) return {};
    std::string path(input);
    std::replace(path.begin(), path.end(), '\\', '/');
    auto key = lower(path);
    for (const auto prefix : {"/data/", "/fs/", "/native_data/"}) {
        if (key.starts_with(prefix)) { path.erase(0, std::strlen(prefix)); key = lower(path); break; }
    }
    if (path.empty() || path.front() == '/' || path.back() == '/') return {};
    for (const auto ch : path) if (static_cast<unsigned char>(ch) < 32 || std::string_view(":<>\"|?*").find(ch) != std::string_view::npos) return {};
    for (std::size_t begin = 0; begin < path.size();) {
        const auto end = path.find('/', begin);
        const auto component = std::string_view(key).substr(begin, (end == std::string::npos ? path.size() : end) - begin);
        if (component.empty() || component == "." || component == ".." || component.back() == '.' || component.back() == ' ') return {};
        const auto stem = component.substr(0, component.find('.'));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9')) return {};
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    const bool scripts = key.starts_with("scripts/");
    const auto relative = scripts ? path.substr(8) : path;
    if (scripts && key.ends_with(".lua")) return "scripts/" + relative;
    if (key.ends_with(".cfg") || key.ends_with(".yaml") || key.ends_with(".yml") || key.ends_with(".ini") || key.ends_with(".json"))
        return "config/" + relative;
    return {};
}
fs::path export_manifest(const fs::path& root) { return root / L"scripts" / L".initfs-export.json"; }
bool loose_files_preference(const fs::path& root) {
    const auto settings = read_preferences(root);
    return settings.contains("loose_files") ? settings["loose_files"].get<bool>() : true;
}
bool loose_files_session_enabled(const fs::path& root) {
    std::array<wchar_t, 8> value{};
    if (GetEnvironmentVariableW(L"RESKATE_LOOSE_FILES", value.data(), static_cast<DWORD>(value.size())) == 1) {
        if (value[0] == L'0') return false;
        if (value[0] == L'1') return true;
    }
    return loose_files_preference(root);
}
fs::path data_directory(const fs::path& game_directory, const std::vector<std::wstring>& arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto equals = arguments[index].find(L'=');
        const auto name = arguments[index].substr(0, equals);
        if (_wcsicmp(name.c_str(), L"-dataPath")) continue;
        std::wstring value;
        if (equals != std::wstring::npos) value = arguments[index].substr(equals + 1);
        else if (++index < arguments.size()) value = arguments[index];
        if (value.empty()) fail("-dataPath requires a directory");
        return fs::weakly_canonical(game_directory / value);
    }
    return fs::is_directory(game_directory / L"ModData")
        ? fs::weakly_canonical(game_directory / L"ModData" / L"Default") : game_directory;
}
ExportReport export_files(const fs::path& executable, const fs::path& data_root, const fs::path& output_root,
    bool first_launch_only) {
    const auto output = fs::weakly_canonical(fs::absolute(output_root));
    const auto marker = export_manifest(output);
    ExportReport report;
    if (first_launch_only && fs::is_regular_file(marker)) { report.already_exported = true; return report; }
    Secret key;
    read_key(executable, key);
    Entries entries;
    Json sources = Json::array();
    const auto game = fs::canonical(executable).parent_path();
    const auto data = fs::weakly_canonical(data_root);
    apply_archive(game / L"Data" / L"initfs_win32", key, entries, sources);
    apply_archive(game / L"Patch" / L"initfs_win32", key, entries, sources);
    if (_wcsicmp(game.c_str(), data.c_str())) {
        apply_archive(data / L"Data" / L"initfs_win32", key, entries, sources);
        apply_archive(data / L"Patch" / L"initfs_win32", key, entries, sources);
    }
    if (entries.empty()) fail("No InitFS files were found for this installation");
    Json manifest = Json::object();
    manifest["format"] = 1;
    manifest["game_sha256"] = supported_build::game_sha256;
    manifest["sources"] = std::move(sources);
    manifest["files"] = Json::array();
    std::map<std::string, std::string> destinations;
    for (const auto& [name, entry] : entries) {
        (void)name;
        const auto relative = loose_relative_path(entry.name);
        if (!relative) { ++report.skipped; continue; }
        if (!destinations.emplace(lower(*relative), entry.name).second) fail("Colliding loose file paths");
        const bool script = relative->starts_with("scripts/");
        script ? ++report.scripts : ++report.configs;
        const auto destination = output / utf8_path(*relative);
        const bool created = write_atomic(output, destination, entry.payload, false);
        created ? ++report.created : ++report.preserved;
        const bool bytecode = entry.payload.size() >= 4 && std::memcmp(entry.payload.data(), "\x1bLua", 4) == 0;
        manifest["files"].push_back(Json{{"virtual_path", entry.name}, {"loose_path", *relative},
            {"source", entry.source}, {"bytes", entry.payload.size()}, {"bytecode", bytecode}});
    }
    manifest["scripts"] = report.scripts; manifest["configs"] = report.configs; manifest["skipped"] = report.skipped;
    const auto text = manifest.dump(2) + "\n";
    write_atomic(output, marker, std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size()), true);
    return report;
}
}
