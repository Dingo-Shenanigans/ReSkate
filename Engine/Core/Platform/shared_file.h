#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace dingosdk {
// Bounded reads share delete access so a writer can rename the old document.
class SharedTextFile {
public:
    explicit SharedTextFile(const std::filesystem::path& path)
#ifdef _WIN32
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open content document");
#else
        : file_(path, std::ios::binary | std::ios::ate) {
        if (!file_) throw std::runtime_error("Cannot open content document");
#endif
    }
    SharedTextFile(const SharedTextFile&) = delete;
    SharedTextFile& operator=(const SharedTextFile&) = delete;
    ~SharedTextFile() {
#ifdef _WIN32
        CloseHandle(handle_);
#endif
    }
    std::string read(std::size_t limit) {
#ifdef _WIN32
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle_, &length) || length.QuadPart < 0 ||
            static_cast<unsigned long long>(length.QuadPart) > limit || limit > MAXDWORD)
            throw std::runtime_error("Content document exceeds its read bound");
        std::string text(static_cast<std::size_t>(length.QuadPart), '\0');
        DWORD got{};
        if (!ReadFile(handle_, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) || got != text.size())
            throw std::runtime_error("Cannot finish content document read");
#else
        const auto length = file_.tellg();
        if (length < 0 || static_cast<std::uintmax_t>(length) > limit) throw std::runtime_error("Content document exceeds its read bound");
        std::string text(static_cast<std::size_t>(length), '\0');
        file_.seekg(0);
        if (!file_.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("Cannot finish content document read");
#endif
        return text;
    }
private:
#ifdef _WIN32
    HANDLE handle_;
#else
    std::ifstream file_;
#endif
};
inline std::string read_shared_text(const std::filesystem::path& path, std::size_t limit) {
    return SharedTextFile(path).read(limit);
}
}
