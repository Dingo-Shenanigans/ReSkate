#include "project_slot.h"

#include "Engine/Core/Json/json.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace dingosdk::graphics_creator {
namespace {
constexpr std::size_t dds_header_bytes = 148;
constexpr std::size_t board_payload_bytes = 1'398'160;

std::filesystem::path environment_path(const wchar_t* name) {
    std::array<wchar_t, 32768> value{};
    const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    return std::filesystem::path(std::wstring(value.data(), length));
}

std::filesystem::path game_directory() {
    if (const auto configured = environment_path(L"RESKATE_GAME_ROOT"); !configured.empty()) return configured;
    std::array<wchar_t, 32768> value{};
    const auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    return std::filesystem::path(std::wstring(value.data(), length)).parent_path();
}

std::filesystem::path first_file(std::initializer_list<std::filesystem::path> candidates) {
    for (const auto& candidate : candidates)
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate)) return candidate;
    return {};
}

std::vector<std::byte> read_file(const std::filesystem::path& path, std::size_t maximum = 64 * 1024 * 1024) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("The board slot file could not be opened.");
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > maximum)
        throw std::runtime_error("The board slot file is outside its supported size.");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream && !bytes.empty()) throw std::runtime_error("The board slot file could not be read.");
    return bytes;
}

std::string sha256_hex(std::span<const std::byte> bytes) {
    std::array<unsigned char, 32> digest{};
    if (bytes.size() > std::numeric_limits<ULONG>::max() ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()),
            digest.data(), static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("SHA-256 validation is unavailable.");
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 15];
    }
    return result;
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::uint32_t u32(const std::vector<std::byte>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("The encoded board DDS is truncated.");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void validate_dds(const std::vector<std::byte>& bytes) {
    if (bytes.size() != dds_header_bytes + board_payload_bytes || u32(bytes, 0) != 0x20534444 ||
        u32(bytes, 4) != 124 || u32(bytes, 12) != 2048 || u32(bytes, 16) != 512 || u32(bytes, 28) != 12 ||
        u32(bytes, 84) != 0x30315844 || u32(bytes, 128) != 99)
        throw std::runtime_error("The encoded board texture is not a 512x2048 BC7 sRGB DDS with 12 mips.");
}

std::filesystem::path descriptor_project(const std::filesystem::path& descriptor, const Json& root) {
    if (!root.contains("project") || !root.at("project").is_string())
        throw std::runtime_error("The board slot descriptor has no project filename.");
    const auto filename = root.at("project").string();
    if (filename.empty() || !std::ranges::all_of(filename, [](unsigned char c) {
            return c >= 0x20 && c < 0x7f && c != '/' && c != '\\' && c != ':';
        }))
        throw std::runtime_error("The board slot project filename is unsafe.");
    const auto relative = std::filesystem::path(filename);
    if (relative.filename() != relative) throw std::runtime_error("The board slot project filename is unsafe.");
    return descriptor.parent_path() / relative;
}

std::wstring quoted(const std::filesystem::path& value) {
    return L"\"" + value.wstring() + L"\"";
}

bool run_texconv(const std::filesystem::path& executable, const std::filesystem::path& source,
                 const std::filesystem::path& output_directory, std::filesystem::path& encoded,
                 std::string& problem) {
    std::filesystem::create_directories(output_directory);
    encoded = output_directory / source.filename();
    encoded.replace_extension(L".dds");
    std::filesystem::remove(encoded);
    auto command = quoted(executable) + L" -y -f BC7_UNORM_SRGB -m 0 -w 512 -h 2048 -o " +
        quoted(output_directory) + L" " + quoted(source);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, executable.parent_path().c_str(), &startup, &process)) {
        problem = "Microsoft texconv could not be started.";
        return false;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 180000);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
    else GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || code != 0 || !std::filesystem::is_regular_file(encoded)) {
        problem = wait == WAIT_TIMEOUT ? "Microsoft texconv timed out." : "Microsoft texconv rejected the board image.";
        return false;
    }
    return true;
}
} // namespace

std::string sha256_file_hex(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    return sha256_hex(bytes);
}

void patch_board_project_slot(const std::filesystem::path& descriptor,
                              const std::filesystem::path& encoded_dds,
                              const std::filesystem::path& output_project) {
    const auto descriptor_bytes = read_file(descriptor, 64 * 1024);
    const auto root = Json::parse(std::string(reinterpret_cast<const char*>(descriptor_bytes.data()), descriptor_bytes.size()));
    if (!root.is_object() || root.value("schema", "") != "ReSkate.Create.BoardSlot.v1" ||
        !root.contains("project_sha256") || !root.at("project_sha256").is_string() ||
        !root.contains("payload_sha256") || !root.at("payload_sha256").is_string() ||
        !root.contains("payload_offset") || !root.at("payload_offset").is_number_integer() ||
        !root.contains("payload_size") || !root.at("payload_size").is_number_integer())
        throw std::runtime_error("The board slot descriptor is invalid.");

    const auto project = descriptor_project(descriptor, root);
    if (project.lexically_normal() == output_project.lexically_normal() ||
        (std::filesystem::exists(output_project) && std::filesystem::equivalent(project, output_project)))
        throw std::runtime_error("The board slot template cannot be overwritten.");
    auto project_bytes = read_file(project);
    if (project_bytes.size() < 8 || std::memcmp(project_bytes.data(), "RSPROJT1", 8) != 0)
        throw std::runtime_error("The board slot template is not a Studio project.");
    if (lower(sha256_hex(project_bytes)) != lower(root.at("project_sha256").string()))
        throw std::runtime_error("The board slot template hash differs from its descriptor.");

    const auto offset64 = root.at("payload_offset").get<std::uint64_t>();
    const auto size64 = root.at("payload_size").get<std::uint64_t>();
    if (size64 != board_payload_bytes || offset64 > project_bytes.size() || size64 > project_bytes.size() - offset64)
        throw std::runtime_error("The board slot payload range is invalid.");
    const auto offset = static_cast<std::size_t>(offset64), size = static_cast<std::size_t>(size64);
    if (lower(sha256_hex(std::span(project_bytes).subspan(offset, size))) != lower(root.at("payload_sha256").string()))
        throw std::runtime_error("The board slot payload hash differs from its descriptor.");

    const auto dds = read_file(encoded_dds, 2 * 1024 * 1024);
    validate_dds(dds);
    std::copy_n(dds.begin() + dds_header_bytes, size, project_bytes.begin() + offset);

    std::filesystem::create_directories(output_project.parent_path());
    auto temporary = output_project;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(project_bytes.data()), static_cast<std::streamsize>(project_bytes.size()));
        stream.flush();
        if (!stream) throw std::runtime_error("The authored board project could not be written.");
    }
    if (!MoveFileExW(temporary.c_str(), output_project.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("The authored board project could not be published.");
    }
}

ProjectSlotResult build_board_project_slot(const std::filesystem::path& source_png,
                                           const std::filesystem::path& output_project) {
    ProjectSlotResult result;
    result.project = output_project;
    const auto game = game_directory();
    const auto studio = game.parent_path() / L"ReSkate Studio";
    const auto configured_descriptor = environment_path(L"RESKATE_CREATE_BOARD_SLOT");
    const auto configured_texconv = environment_path(L"RESKATE_TEXCONV");
    const auto descriptor = first_file({configured_descriptor,
        studio / L"Templates" / L"reskate_create_slot_01.slot.json",
        studio / L"reskate_create_slot_01.slot.json"});
    const auto texconv = first_file({configured_texconv, studio / L"texconv.exe", studio / L"Native" / L"texconv.exe"});
    if (descriptor.empty() || texconv.empty()) {
        result.message = "Install the ReSkate.Create board slot and Microsoft texconv in ReSkate Studio, or configure their environment paths.";
        return result;
    }
    result.attempted = true;
    std::string problem;
    if (!run_texconv(texconv, source_png, output_project.parent_path() / L"Encoded", result.encoded_dds, problem)) {
        result.message = std::move(problem);
        return result;
    }
    try {
        patch_board_project_slot(descriptor, result.encoded_dds, output_project);
        result.authored = true;
        result.message = "Board project authored. Open it in ReSkate Studio and export the FBMOD.";
    } catch (const std::exception& error) {
        result.message = error.what();
    }
    return result;
}

} // namespace dingosdk::graphics_creator
