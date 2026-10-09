#include "launcher_support.h"
#include "launcher_support_internal.h"
#include "path_text.h"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fstream>
#include <openssl/sha.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace dingosdk::launcher {
#ifdef _WIN32
namespace {
using detail::fail;
using detail::Handle;

class Algorithm {
public:
    ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    BCRYPT_ALG_HANDLE value{};
};

class Hash {
public:
    ~Hash() { if (value) BCryptDestroyHash(value); }
    BCRYPT_HASH_HANDLE value{};
};

template<class T>
T object_at(const std::byte* data, std::size_t size, std::size_t offset) {
    if (offset > size || sizeof(T) > size - offset) fail("Truncated PE image");
    T output{};
    std::memcpy(&output, data + offset, sizeof(output));
    return output;
}

class MappedFile {
public:
    explicit MappedFile(const fs::path& path) {
        file_.reset(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file_.get() == INVALID_HANDLE_VALUE) fail("Cannot open PE image");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_.get(), &length) || length.QuadPart <= 0 ||
            static_cast<unsigned long long>(length.QuadPart) >
                static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
            fail("Invalid PE image size");
        size_ = static_cast<std::size_t>(length.QuadPart);
        mapping_.reset(CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!mapping_.get()) fail("Cannot map PE image");
        data_ = static_cast<const std::byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, 0));
        if (!data_) fail("Cannot read mapped PE image");
    }
    ~MappedFile() { if (data_) UnmapViewOfFile(data_); }
    const std::byte* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
private:
    Handle file_;
    Handle mapping_;
    const std::byte* data_{};
    std::size_t size_{};
};

struct ParsedPe {
    PeFileInfo info;
    IMAGE_NT_HEADERS64 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
};

ParsedPe parse_pe(const std::byte* data, std::size_t size) {
    const auto dos = object_at<IMAGE_DOS_HEADER>(data, size, 0);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        fail("File is not a PE image");
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    const auto nt = object_at<IMAGE_NT_HEADERS64>(data, size, nt_offset);
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        fail("File is not a PE32+ image");
    if (!nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96)
        fail("Invalid PE section table");
    const auto sections_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections;
    sections.reserve(nt.FileHeader.NumberOfSections);
    for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index)
        sections.push_back(object_at<IMAGE_SECTION_HEADER>(data, size,
            sections_offset + index * sizeof(IMAGE_SECTION_HEADER)));
    return {{nt.FileHeader.Machine, nt.FileHeader.Characteristics,
             nt.OptionalHeader.SizeOfImage, nt.OptionalHeader.SizeOfHeaders, true},
            nt, std::move(sections)};
}

std::size_t rva_offset(const ParsedPe& pe, std::size_t file_size, std::uint32_t rva,
                       std::size_t required) {
    if (rva < pe.info.headers_size) {
        if (rva > file_size || required > file_size - rva) fail("PE RVA is outside the file");
        return rva;
    }
    for (const auto& section : pe.sections) {
        const std::uint64_t start = section.VirtualAddress;
        const std::uint64_t end = start + std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva < start || rva >= end) continue;
        const auto delta = static_cast<std::uint64_t>(rva) - start;
        if (delta > section.SizeOfRawData || required > section.SizeOfRawData - delta)
            fail("PE RVA has no file data");
        const auto offset = static_cast<std::uint64_t>(section.PointerToRawData) + delta;
        if (offset > file_size || required > file_size - offset) fail("PE RVA is outside the file");
        return static_cast<std::size_t>(offset);
    }
    fail("PE RVA does not belong to a section");
}

template<class T>
T rva_object(const MappedFile& file, const ParsedPe& pe, std::uint32_t rva) {
    return object_at<T>(file.data(), file.size(), rva_offset(pe, file.size(), rva, sizeof(T)));
}

std::string rva_string(const MappedFile& file, const ParsedPe& pe, std::uint32_t rva) {
    const auto offset = rva_offset(pe, file.size(), rva, 1);
    std::string output;
    for (std::size_t index = offset; index < file.size() && output.size() <= 4096; ++index) {
        const auto value = static_cast<char>(file.data()[index]);
        if (!value) return output;
        output.push_back(value);
    }
    fail("Unterminated PE export name");
}

// Opens a file to read all of it. A file that was written a moment ago (a download) is often
// held by the antivirus scanning it, so a sharing violation is waited out for a few seconds.
// A file that still cannot be opened is named along with the reason: with ReSkate.dll that is
// nearly always an antivirus blocking it, and "cannot open file" told the player nothing.
Handle open_to_read(const fs::path& path) {
    DWORD error{};
    for (int attempt = 0; attempt < 50; ++attempt) {
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.get() != INVALID_HANDLE_VALUE) return file;
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) break;
        Sleep(100);
    }
    const auto name = path_utf8(path.filename());
    const auto folder = path_utf8(path.parent_path());
    std::string message;
    switch (error) {
    case ERROR_VIRUS_INFECTED:
    case ERROR_VIRUS_DELETED:
        message = "Your antivirus blocked " + name + ". Add the folder " + folder +
                  " to its exclusions (in Windows Security: Virus & threat protection > Exclusions), then try again.";
        break;
    case ERROR_ACCESS_DENIED:
        message = name + " cannot be read: access was denied. An antivirus is probably blocking it: add the folder " + folder +
                  " to its exclusions, then try again.";
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        message = name + " is in use by another program, usually an antivirus scan. Close Skate if it is running, wait a moment and try again; "
                  "if it keeps happening, add the folder " + folder + " to your antivirus exclusions.";
        break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        message = name + " is missing from " + folder + ". If it was just downloaded, an antivirus removed it: add the folder to its "
                  "exclusions, then try again.";
        break;
    default:
        message = "Cannot read " + name + " in " + folder;
        break;
    }
    throw std::runtime_error(message + " (Windows error " + std::to_string(error) + ")");
}
} // namespace

#endif // _WIN32

std::string sha256_file(const fs::path& path) {
#ifdef _WIN32
    const auto file = open_to_read(path);

    Algorithm algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        detail::fail("Cannot initialize SHA-256");
    DWORD object_size{}, result_size{};
    if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0 ||
        result_size != sizeof(object_size)) detail::fail("Cannot query SHA-256 state size");
    std::vector<UCHAR> object(object_size);
    Hash hash;
    if (BCryptCreateHash(algorithm.value, &hash.value, object.data(), object_size,
            nullptr, 0, 0) < 0) detail::fail("Cannot create SHA-256 state");
    std::vector<UCHAR> bytes(1024 * 1024);
    for (;;) {
        DWORD count{};
        if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
            detail::fail("Cannot read file for SHA-256");
        if (!count) break;
        if (BCryptHashData(hash.value, bytes.data(), count, 0) < 0)
            detail::fail("Cannot update SHA-256");
    }
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        detail::fail("Cannot finish SHA-256");
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 15]);
    }
    return output;
#else
    // Name the file and the reason, as open_to_read does on Windows: a server admin reading
    // "cannot open file" in the log has no idea which file, or what to fix.
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        const int error = errno;
        throw std::runtime_error("Cannot open " + path_utf8(path.filename()) + " in " +
            path_utf8(path.parent_path()) + " for SHA-256: " +
            (error ? std::strerror(error) : "unknown error"));
    }
    SHA256_CTX context;
    if (!SHA256_Init(&context)) detail::fail("Cannot initialize SHA-256");
    std::vector<unsigned char> bytes(1024 * 1024);
    for (;;) {
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        if (count < 0) detail::fail("Cannot read file for SHA-256");
        if (count) {
            if (!SHA256_Update(&context, bytes.data(), static_cast<std::size_t>(count)))
                detail::fail("Cannot update SHA-256");
        }
        if (!count || input.eof()) break;
        if (input.fail() && !input.eof()) detail::fail("Cannot read file for SHA-256");
    }
    std::array<unsigned char, 32> digest{};
    if (!SHA256_Final(digest.data(), &context)) detail::fail("Cannot finish SHA-256");
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 15]);
    }
    return output;
#endif
}

#ifdef _WIN32

PeFileInfo inspect_pe_file(const fs::path& path) {
    const MappedFile file(path);
    return parse_pe(file.data(), file.size()).info;
}

std::uint32_t exported_function_rva(const fs::path& path, std::string_view export_name) {
    const MappedFile file(path);
    const auto pe = parse_pe(file.data(), file.size());
    const auto& directory = pe.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
        fail("DLL has no export directory");
    const auto exports = rva_object<IMAGE_EXPORT_DIRECTORY>(file, pe, directory.VirtualAddress);
    if (!exports.NumberOfFunctions || exports.NumberOfNames > exports.NumberOfFunctions ||
        exports.NumberOfFunctions > 65536) fail("Invalid PE export table");
    for (std::uint32_t index = 0; index < exports.NumberOfNames; ++index) {
        const auto name_rva = rva_object<std::uint32_t>(file, pe,
            exports.AddressOfNames + index * sizeof(std::uint32_t));
        if (rva_string(file, pe, name_rva) != export_name) continue;
        const auto ordinal = rva_object<std::uint16_t>(file, pe,
            exports.AddressOfNameOrdinals + index * sizeof(std::uint16_t));
        if (ordinal >= exports.NumberOfFunctions) fail("Invalid PE export ordinal");
        const auto function = rva_object<std::uint32_t>(file, pe,
            exports.AddressOfFunctions + ordinal * sizeof(std::uint32_t));
        const std::uint64_t export_end = static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size;
        if (!function || (function >= directory.VirtualAddress && function < export_end))
            fail("Initializer export is missing or forwarded");
        return function;
    }
    fail("Required initializer export is missing");
}

void validate_game_file(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size != expected_game_file_size) detail::fail("Skate.exe has the wrong file size");
    const auto image = inspect_pe_file(path);
    if (!image.pe64 || image.machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(image.characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        (image.characteristics & IMAGE_FILE_DLL) || image.image_size != expected_game_image_size)
        detail::fail("Skate.exe has the wrong PE image identity");
    if (sha256_file(path) != expected_game_sha256)
        detail::fail("Skate.exe SHA-256 does not match the supported build");
}

void validate_steam_api_file(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size != expected_steam_api_file_size)
        detail::fail("steam_api64.dll is not the supported original Steam library (wrong size)");
    const auto image = inspect_pe_file(path);
    if (!image.pe64 || image.machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(image.characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        !(image.characteristics & IMAGE_FILE_DLL))
        detail::fail("steam_api64.dll is not an x64 Windows DLL");
    if (sha256_file(path) != expected_steam_api_sha256)
        detail::fail("steam_api64.dll is not the supported original Steam library (SHA-256 mismatch)");
}

#else // _WIN32

void validate_steam_api_file(const fs::path& path) {
    // Linux uses libsteam_api.so (different size/hash than the Windows DLL).
    // Strict pinning stays Windows-only; here just require the file to exist.
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) detail::fail("Steam library not found");
}

#endif // _WIN32

} // namespace dingosdk::launcher
