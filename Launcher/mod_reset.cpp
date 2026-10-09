#include "mod_manager.h"
#include "text_encoding.h"

#include "Engine/Vfs/mod_list.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <memory>
#include <optional>
#include <utility>

namespace fs = std::filesystem;

namespace dingosdk::launcher_mods {
namespace {
std::atomic<bool> operation_busy{};

using launcher_text::utf8;

std::string shown(const fs::path& path) { return utf8(path.wstring()); }

[[noreturn]] void refused(const fs::path& path, const std::string& reason) {
    throw std::runtime_error(shown(path) + ": " + reason);
}

// Extended paths avoid MAX_PATH truncation in both CreateFile and enumeration.
fs::path native(const fs::path& path) {
    const auto text = path.wstring();
    if (text.starts_with(L"\\\\?\\")) return path;
    if (text.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + text.substr(2);
    return L"\\\\?\\" + text;
}

bool same(const fs::path& a, const fs::path& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

fs::path checked_path(const fs::path& path) {
    if (path.empty() || !path.is_absolute() || path.wstring().starts_with(L"\\\\?\\") ||
        path.wstring().starts_with(L"\\\\.\\")) refused(path, "unsafe or unresolved reset path");
    for (const auto& part : path.relative_path()) {
        const auto value = part.wstring();
        if (value == L".." || value == L"." || value.find(L':') != std::wstring::npos ||
            (!value.empty() && (value.back() == L'.' || value.back() == L' ')))
            refused(path, "unsafe path component");
    }
    const auto result = path.lexically_normal();
    if (same(result, result.root_path())) refused(path, "a filesystem root cannot be reset");
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"USERPROFILE", profile, 32768);
    if (length && length < 32768 && same(result, fs::path(profile).lexically_normal()))
        refused(path, "the user profile cannot be reset");
    return result;
}

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Entry {
    std::unique_ptr<Handle> handle;
    BY_HANDLE_FILE_INFORMATION info{};
    bool directory() const { return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    bool link() const { return (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
    ResetIdentity identity() const {
        return {info.dwVolumeSerialNumber,
            (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
    }
};

Entry open(const fs::path& path, bool deleting = false, bool missing_ok = false, bool exclusive = false) {
    Entry entry;
    const auto handle = CreateFileW(native(path).c_str(), FILE_READ_ATTRIBUTES | (deleting ? DELETE : 0) |
        (exclusive ? GENERIC_READ : 0), exclusive ? 0 : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (missing_ok && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return entry;
        refused(path, "cannot open safely (Windows error " + std::to_string(error) + ")");
    }
    entry.handle = std::make_unique<Handle>(handle);
    if (!GetFileInformationByHandle(handle, &entry.info))
        refused(path, "cannot inspect (Windows error " + std::to_string(GetLastError()) + ")");
    return entry;
}

// Pin every ancestor, starting at the volume/share root. Denying write/delete
// sharing prevents reparse mutation and rename while child paths are opened. OPEN_REPARSE_POINT
// ensures a newly introduced junction is inspected, never traversed.
std::vector<Entry> pin(const fs::path& directory, bool missing_ok = false) {
    std::vector<Entry> chain;
    auto at = directory.root_path();
    chain.push_back(open(at));
    for (const auto& part : directory.relative_path()) {
        at /= part;
        auto entry = open(at, false, missing_ok);
        if (!entry.handle) return {};
        chain.push_back(std::move(entry));
        if (!chain.back().directory() || chain.back().link())
            refused(at, "reset ancestors must be ordinary directories, without reparse points");
    }
    return chain;
}

std::vector<fs::path> children(const fs::path& path) {
    std::vector<fs::path> result;
    std::error_code error;
    fs::directory_iterator it(native(path), error), end;
    while (!error && it != end) {
        const auto name = it->path().filename();
        checked_path(path / name);
        result.push_back(path / name);
        it.increment(error);
    }
    if (error) refused(path, "cannot enumerate: " + error.message());
    return result;
}

struct GamePins {
    std::vector<Entry> directories;
    Entry executable;
    ResetIdentity identity() const { return directories.back().identity(); }
};

GamePins pin_game(const fs::path& game) {
    checked_path(game);
    auto chain = pin(game);
    // Keep the executable open exclusively throughout inspection/deletion.
    // This closes the usual external CreateProcess race after the process
    // snapshot, without modifying the game or requesting DELETE access to it.
    auto skate = open(game / L"Skate.exe", false, false, true);
    if (skate.directory() || skate.link()) refused(game, "Skate.exe must be an ordinary file in the game installation");
    return {std::move(chain), std::move(skate)};
}

bool known_root(const fs::path& game, const fs::path& target) {
    checked_path(target);
    return same(target, game / L"Mods") || same(target, game / L"ModData" / L"Default" / L"Mods");
}

bool marker(const fs::path& path, const fs::path& root) {
    auto name = path.filename().wstring();
    for (auto& ch : name) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    if (same(path.parent_path(), root)) {
        return name == L"mods.json" || name == L"mods.json.tmp" || name == L".reskate-excluded.json" ||
            name == L".reskate-excluded.json.tmp" || name == L".reskate" || name == L".reskate-install" ||
            name == L".reskate-download";
    }
    if (same(path.parent_path().parent_path(), root)) {
        return name == L"layout.toc" || name == L"reskate-levels.json" || name == L"manifest.json" ||
            name == L"reskate-mod.json" || name == L".reskate-studio-patch";
    }
    return same(path.parent_path().parent_path().parent_path(), root) &&
        same(path.parent_path().filename(), L"parks") && name.ends_with(L".park.json");
}

std::uintmax_t inspect(const fs::path& path, const fs::path& root, ResetPlan& plan, bool& managed, std::size_t depth = 0) {
    if (depth > 512) refused(path, "directory nesting is too deep to inspect safely");
    auto entry = open(path);
    if (entry.link()) {
        plan.excluded.push_back(shown(path) + " (reparse target excluded; only the link itself will be removed)");
        return 0;
    }
    if (marker(path, root)) managed = true;
    if (!entry.directory()) return (static_cast<std::uintmax_t>(entry.info.nFileSizeHigh) << 32) | entry.info.nFileSizeLow;
    if (same(path.parent_path(), root) && mods::valid_mod_name(utf8(path.filename().wstring()))) ++plan.mod_count;
    std::uintmax_t bytes{};
    for (const auto& child : children(path)) bytes += inspect(child, root, plan, managed, depth + 1);
    return bytes;
}

void erase_tree(const fs::path& path, ResetResult& result, const Progress& progress, std::size_t depth = 0) {
    try {
        // Bound recursion before opening a child, including maliciously deep trees.
        if (depth > 512) refused(path, "directory nesting is too deep to reset safely");
        auto entry = open(path, true, true);
        if (!entry.handle) return;
        if (progress) progress(-1);
        if (entry.directory() && !entry.link())
            for (const auto& child : children(path)) erase_tree(child, result, progress, depth + 1);
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(entry.handle->value, FileDispositionInfo, &disposition, sizeof(disposition)))
            refused(path, "could not permanently delete (Windows error " + std::to_string(GetLastError()) +
                "). Close applications using this path or check its permissions, then retry.");
        ++result.removed_items;
        if (!entry.directory() && !entry.link())
            result.removed_bytes += (static_cast<std::uintmax_t>(entry.info.nFileSizeHigh) << 32) | entry.info.nFileSizeLow;
    } catch (const std::exception& error) {
        result.errors.push_back(error.what());
        result.remaining.push_back(path);
    }
}
} // namespace

OperationLease::OperationLease(Operation operation) : operation_(operation) {
    bool expected = false;
    held_ = operation_busy.compare_exchange_strong(expected, true);
}
OperationLease::~OperationLease() { if (held_) operation_busy = false; }
OperationLease::OperationLease(OperationLease&& other) noexcept
    : operation_(other.operation_), held_(std::exchange(other.held_, false)) {}
bool OperationLease::busy() { return operation_busy; }

std::string factory_reset_blocker() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return "Could not check running games. Try again before resetting mods.";
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (!Process32FirstW(snapshot.value, &process)) return "Could not inspect running games. Try again before resetting mods.";
    do {
        if (CompareStringOrdinal(process.szExeFile, -1, L"Skate.exe", -1, TRUE) == CSTR_EQUAL)
            return "Close Skate before resetting mods, including games started outside this launcher.";
    } while (Process32NextW(snapshot.value, &process));
    if (GetLastError() != ERROR_NO_MORE_FILES) return "Could not finish checking running games. Try again.";
    return {};
}

ResetPlan prepare_factory_reset(const fs::path& game_directory, const OperationLease* lease) {
    ResetPlan plan;
    std::optional<OperationLease> owned;
    if (!lease) { owned.emplace(Operation::reset); lease = &*owned; }
    try {
        if (!lease->permits(Operation::reset)) throw std::runtime_error("Another mod or launcher operation is still running.");
        if (const auto reason = factory_reset_blocker(); !reason.empty()) throw std::runtime_error(reason);
        plan.game_directory = checked_path(game_directory);
        auto game = pin_game(plan.game_directory);
        plan.game_identity = game.identity();
        if (const auto reason = factory_reset_blocker(); !reason.empty()) throw std::runtime_error(reason);
        const auto active = mods_root(plan.game_directory);
        for (const auto& root : {plan.game_directory / L"Mods", plan.game_directory / L"ModData" / L"Default" / L"Mods"}) {
            try {
                // Check existence without traversing an untrusted ancestor first.
                auto parent = plan.game_directory;
                std::vector<Entry> ancestors;
                bool missing = false;
                const auto relative = root.parent_path().lexically_relative(plan.game_directory);
                for (const auto& part : relative) {
                    if (part == L".") continue;
                    parent /= part;
                    auto entry = open(parent, false, true);
                    if (!entry.handle) { missing = true; break; }
                    if (!entry.directory() || entry.link()) refused(parent, "unsafe reset ancestor");
                    ancestors.push_back(std::move(entry));
                }
                if (missing) continue;
                auto entry = open(root, false, true);
                if (!entry.handle) continue;
                if (!entry.directory() || entry.link()) refused(root, "Mods root must be an ordinary directory");
                bool managed = same(root, active);
                const auto count = plan.mod_count;
                const auto bytes = inspect(root, root, plan, managed);
                if (!managed) {
                    plan.mod_count = count;
                    plan.excluded.push_back(shown(root) + " (inactive layout has no ReSkate ownership markers)");
                    continue;
                }
                plan.targets.push_back({root, bytes, entry.identity()});
                plan.estimated_bytes += bytes;
            } catch (const std::exception& error) {
                plan.errors.push_back(error.what());
                plan.excluded.push_back(shown(root) + " (could not safely inspect)");
            }
        }
    } catch (const std::exception& error) { plan.errors.push_back(error.what()); }
    return plan;
}

ResetResult execute_factory_reset(const ResetPlan& plan, const Progress& progress, const OperationLease* lease,
                                  const std::function<void(const std::string&)>& activity) {
    ResetResult result;
    result.errors = plan.errors;
    std::optional<OperationLease> owned;
    if (!lease) { owned.emplace(Operation::reset); lease = &*owned; }
    try {
        if (!lease->permits(Operation::reset)) throw std::runtime_error("Another mod or launcher operation is still running.");
        if (!result.errors.empty()) return result;
        if (const auto reason = factory_reset_blocker(); !reason.empty()) throw std::runtime_error(reason);
        auto game = pin_game(plan.game_directory);
        if (game.identity() != plan.game_identity) refused(plan.game_directory, "game directory changed; inspect again");
        if (const auto reason = factory_reset_blocker(); !reason.empty()) throw std::runtime_error(reason);
        // Reject forged/traversing paths before any of the targets are modified.
        for (const auto& target : plan.targets)
            if (!known_root(plan.game_directory, target.path)) refused(target.path, "not an authorised Mods root");
        for (const auto& target : plan.targets) {
            try {
                auto ancestors = pin(target.path.parent_path(), true);
                if (ancestors.empty()) continue; // The root and its parents are already gone.
                auto entry = open(target.path, false, true);
                if (!entry.handle) continue;
                if (!entry.directory() || entry.link() || entry.identity() != target.identity)
                    refused(target.path, "Mods directory changed; inspect again");
                // Close the read handle to acquire DELETE access. Ancestors
                // remain pinned; the new handle must still match the plan.
                entry.handle.reset();
                auto locked = open(target.path, true, true);
                if (!locked.handle) continue;
                if (!locked.directory() || locked.link() || locked.identity() != target.identity)
                    refused(target.path, "Mods directory changed before deletion; inspect again");
                for (const auto& child : children(target.path)) {
                    if (activity) {
                        const auto name = child.filename().wstring();
                        activity(name == L".reskate" ? "Removing generated mod data..."
                            : name == L".reskate-install" || name == L".reskate-download" ? "Cleaning temporary files..."
                            : "Removing installed mods and metadata...");
                    }
                    erase_tree(child, result, progress);
                }
                if (activity) activity("Finalising reset...");
                FILE_DISPOSITION_INFO disposition{TRUE};
                if (!SetFileInformationByHandle(locked.handle->value, FileDispositionInfo, &disposition, sizeof(disposition)))
                    refused(target.path, "Mods directory could not be removed (Windows error " + std::to_string(GetLastError()) + ")");
                ++result.removed_items;
            } catch (const std::exception& error) {
                result.errors.push_back(error.what());
                result.remaining.push_back(target.path);
            }
        }
        result.success = result.errors.empty();
        if (result.success && progress) progress(1);
    } catch (const std::exception& error) {
        result.errors.push_back(error.what());
        for (const auto& target : plan.targets) result.remaining.push_back(target.path);
    }
    return result;
}
} // namespace dingosdk::launcher_mods
