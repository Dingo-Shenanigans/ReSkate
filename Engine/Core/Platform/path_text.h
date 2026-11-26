#pragma once

#include <filesystem>
#include <string>

namespace dingosdk {

// A path as narrow text.
//
// std::filesystem::path::string() converts to the process's ANSI code page and
// throws when a character has no mapping there: "No mapping for the Unicode
// character exists in the target multi-byte code page". A folder named in
// Cyrillic or CJK is enough, and so is an invisible one -- a directory called
// U+200E (left-to-right mark) has been seen in the wild. The failure lands far
// from the cause, because the path is usually only being written into a log or
// an error message at the time.
//
// UTF-8 can represent every path Windows allows, so every path that becomes a
// narrow string goes through here instead.
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// A path the Windows file functions take at any length.
//
// Past 260 characters (248 for a folder to create) CreateFileW, CreateDirectoryW and the rest
// fail, unless the process is long-path aware and the machine has long paths turned on, which
// neither the launcher nor the game can count on. The "\\?\" form is taken at any length. It
// is a path as the file system has it: whole, with backslashes and no "." or "..", so it is made
// absolute and normal first; the merge joins "Win32/levels/..." onto its folders, and "/" is
// not a separator in that form. A shorter path is given back as it is, so what the logs show and
// how everything else behaves does not change. A folder to go through (`always`) is given in the
// long form whatever its length, since what is under it can be longer; what it lists then comes
// back in that form too. ReSkate Studio's tests saw the merge fail past 260 characters on
// 2026-11-18, run from a deep temp folder.
inline std::filesystem::path long_path(const std::filesystem::path& path, bool always = false) {
#if defined(_WIN32)
    const auto& text = path.native();
    // One already long (a folder gone through in that form, with more joined on) only needs
    // its separators made the file system's.
    if (text.starts_with(L"\\\\?\\")) return std::filesystem::path(path).make_preferred();
    if (!always && text.size() < 248) return path;
    std::error_code error;
    auto whole = std::filesystem::absolute(path, error);
    if (error) return path;
    whole = whole.lexically_normal();
    const auto& normal = whole.native();
    if (normal.starts_with(L"\\\\?\\")) return whole;
    if (normal.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\" + normal.substr(2));
    return std::filesystem::path(L"\\\\?\\" + normal);
#else
    return path;
#endif
}

} // namespace dingosdk
