#include "loose_files.h"
#include "Engine/Vfs/initfs.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/loose_files.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <stdexcept>

namespace dingosdk {
namespace {
namespace fs = std::filesystem;
using Open = void* (*)(void* vfs, unsigned flags, const char* name);
using Exists = bool (*)(void* vfs, const char* name);
using DirectoryFactory = void* (*)(const char* root);
using Mount = bool (*)(void* vfs, void* file_system, const char* mount_point);
using GlobalVfs = void* (*)();
namespace loose = addr::loose_files;
constexpr std::uintptr_t open_rva = loose::vfs_open, exists_rva = loose::vfs_exists;
constexpr std::uintptr_t create_directory_rva = loose::create_directory, mount_rva = loose::vfs_mount,
    global_vfs_rva = loose::global_vfs;
constexpr char mount_point[] = "/__reskate_loose";
std::atomic<Open> original_open{};
std::atomic<Exists> original_exists{};
std::mutex install_mutex;
std::uintptr_t installed_base{};
thread_local bool inside_overlay{};

struct State {
    fs::path root;
    std::string native_root;
    DirectoryFactory create_directory{};
    Mount mount{};
    GlobalVfs global_vfs{};
    std::mutex mount_mutex;
    void* mounted_vfs{};
    void* directory{};
    bool mount_failed{};
};
// Hooks and the native mount live until process exit, like the pinned runtime.
State& state() { static auto* value = new State; return *value; }

// Supported game build, checked after runtime image validation.
bool validate_contract(std::uintptr_t base) {
    struct Contract { std::uintptr_t rva; std::span<const unsigned char> bytes; };
    const Contract contracts[]{
        {open_rva, loose::vfs_open_prefix}, {exists_rva, loose::vfs_exists_prefix},
        {create_directory_rva, loose::create_directory_prefix}, {mount_rva, loose::vfs_mount_prefix},
        {global_vfs_rva, loose::global_vfs_prefix}, {loose::mount_reference, loose::mount_reference_prefix}
    };
    for (const auto& contract : contracts) {
        std::array<unsigned char, 32> actual{};
        SIZE_T count{};
        if (contract.bytes.size() > actual.size() || // a longer prefix would overrun actual
            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + contract.rva),
                actual.data(), contract.bytes.size(), &count) || count != contract.bytes.size() ||
            std::memcmp(actual.data(), contract.bytes.data(), contract.bytes.size())) return false;
    }
    return base != 0;
}

bool copy_name(const char* name, char* copy, std::size_t capacity) noexcept {
    if (!name) return false;
    __try {
        for (std::size_t index = 0; index < capacity; ++index) {
            copy[index] = name[index];
            if (!copy[index]) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}

struct OverlayScope {
    OverlayScope() { inside_overlay = true; }
    ~OverlayScope() { inside_overlay = false; }
};
struct Candidate {
    std::string relative;
    bool present{};
};
Candidate find_loose(const char* name) {
    std::array<char, 1025> text{};
    if (!copy_name(name, text.data(), text.size())) return {};
    auto relative = initfs::loose_relative_path(text.data());
    if (!relative) return {};
    const auto file = state().root / initfs::utf8_path(*relative);
    const auto attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        throw std::runtime_error("Cannot inspect " + *relative + " (Win32 " + std::to_string(error) + ")");
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) throw std::runtime_error("Loose file is a directory: " + *relative);
    return {std::move(*relative), true};
}

void* mounted_vfs() {
    auto& value = state();
    std::lock_guard lock(value.mount_mutex);
    if (value.mounted_vfs) return value.mounted_vfs;
    if (value.mount_failed) return nullptr;
    auto* vfs = value.global_vfs();
    if (!vfs) return nullptr; // Engine allocator/VFS is not initialized yet; try again on its next read.
    value.directory = value.create_directory(value.native_root.c_str());
    if (value.directory) {
        // Keep one reference for the process lifetime, independent of the mount.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(value.directory) + 8));
        if (value.mount(vfs, value.directory, mount_point)) {
            value.mounted_vfs = vfs;
            logging::write(logging::Level::info, logging::Channel::assets, "Loose Lua/config directory mounted.");
            return vfs;
        }
    }
    value.mount_failed = true;
    logging::write(logging::Level::error, logging::Channel::assets, "Cannot mount loose Lua/config directory.");
    return nullptr;
}

void* open_file(void* vfs, unsigned flags, const char* name) {
    const auto saved_error = GetLastError();
    const auto original = original_open.load(std::memory_order_acquire);
    // Bit 0 = read, bit 1 = write, bit 2 = binary. Never redirect saves/writes.
    if (inside_overlay || !(flags & 1) || (flags & 2)) return original(vfs, flags, name);
    const OverlayScope scope;
    try {
        const auto candidate = find_loose(name);
        if (candidate.present) {
            auto* files = mounted_vfs();
            if (!files) {
                logging::log(logging::Level::error, logging::Channel::assets,
                    "Cannot load {}: loose filesystem is unavailable.", candidate.relative);
                SetLastError(ERROR_NOT_READY);
                return nullptr;
            }
            const auto path = std::string(mount_point) + '/' + candidate.relative;
            SetLastError(saved_error);
            // Return the engine's own file object with its allocator, refcount,
            // read/seek/close implementation and the caller's original flags.
            auto* file = original(files, flags, path.c_str());
            logging::log(file ? logging::Level::info : logging::Level::error, logging::Channel::assets,
                "Loose file {}: {}", file ? "loaded" : "failed", candidate.relative);
            return file;
        }
    } catch (const std::exception& error) {
        logging::log(logging::Level::error, logging::Channel::assets, "Loose file read failed: {}", error.what());
        SetLastError(ERROR_READ_FAULT);
        return nullptr;
    }
    SetLastError(saved_error);
    return original(vfs, flags, name);
}
bool file_exists(void* vfs, const char* name) {
    const auto saved_error = GetLastError();
    const auto original = original_exists.load(std::memory_order_acquire);
    if (inside_overlay) return original(vfs, name);
    const OverlayScope scope;
    try {
        if (find_loose(name).present) { SetLastError(saved_error); return true; }
    } catch (const std::exception& error) {
        logging::log(logging::Level::error, logging::Channel::assets, "Loose file lookup failed: {}", error.what());
        SetLastError(ERROR_READ_FAULT);
        return false;
    }
    SetLastError(saved_error);
    return original(vfs, name);
}
}

bool start_loose_files(std::uintptr_t base, std::string& error) {
    std::lock_guard lock(install_mutex);
    error.clear();
    if (installed_base) {
        if (installed_base == base) return true;
        error = "Loose files are already installed on another image";
        return false;
    }
    try {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("Cannot locate Skate.exe for loose files");
        auto& value = state();
        value.root = fs::canonical(fs::path(std::wstring(executable.data(), length))).parent_path();
        if (!initfs::loose_files_session_enabled(value.root)) {
            logging::write(logging::Level::info, logging::Channel::assets, "Loose Lua/config files disabled for this session.");
            return true;
        }
        if (!validate_contract(base)) { error = "Native loose-file contract does not match the supported game"; return false; }
        const auto utf8 = value.root.generic_u8string();
        value.native_root.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        value.native_root += '/';
        value.create_directory = reinterpret_cast<DirectoryFactory>(base + create_directory_rva);
        value.mount = reinterpret_cast<Mount>(base + mount_rva);
        value.global_vfs = reinterpret_cast<GlobalVfs>(base + global_vfs_rva);
    } catch (const std::exception& exception) { error = exception.what(); return false; }

    auto* open_target = reinterpret_cast<void*>(base + open_rva);
    auto* exists_target = reinterpret_cast<void*>(base + exists_rva);
    Open open_original{};
    Exists exists_original{};
    auto status = hook_prepare(open_target, reinterpret_cast<void*>(&open_file), reinterpret_cast<void**>(&open_original));
    if (status != HookOk || !open_original) {
        if (status == HookOk) hook_remove(open_target);
        error = "Cannot prepare loose file reads: " + std::to_string(status); return false;
    }
    status = hook_prepare(exists_target, reinterpret_cast<void*>(&file_exists), reinterpret_cast<void**>(&exists_original));
    if (status != HookOk || !exists_original) {
        if (status == HookOk) hook_remove(exists_target);
        hook_remove(open_target);
        error = "Cannot prepare loose file lookup: " + std::to_string(status); return false;
    }
    original_open.store(open_original, std::memory_order_release);
    original_exists.store(exists_original, std::memory_order_release);
    status = hook_queue_enable(open_target);
    if (status == HookOk) status = hook_queue_enable(exists_target);
    if (status == HookOk) status = hook_apply_queued();
    if (status != HookOk) {
        hook_remove(exists_target); hook_remove(open_target);
        original_exists.store(nullptr, std::memory_order_release);
        original_open.store(nullptr, std::memory_order_release);
        error = "Cannot enable loose Lua/config files: " + std::to_string(status); return false;
    }
    installed_base = base;
    logging::write(logging::Level::info, logging::Channel::assets,
        "Loose Lua/config overrides enabled: scripts/ and config/ beside Skate.exe; missing files use packaged content.");
    return true;
}
}
