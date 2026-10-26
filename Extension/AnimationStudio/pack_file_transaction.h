#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::anim_studio {
// Pack updates are prepared beside the library. No installed file changes until every new file is written and
// every old file has a backup. An error during replacement puts the old files back; failed recovery keeps its backups.
class PackFileTransaction {
public:
    explicit PackFileTransaction(const std::filesystem::path& root) : root_(std::filesystem::absolute(root).lexically_normal()) {
        static std::atomic<unsigned long long> serial{};
        const auto parent = root_ / L".pack-transactions";
        std::filesystem::create_directories(parent);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
            work_ = parent / (std::to_string(id) + "-" + std::to_string(serial.fetch_add(1)));
            if (std::filesystem::create_directory(work_)) return;
        }
        throw std::runtime_error("Could not prepare the pack update");
    }
    PackFileTransaction(const PackFileTransaction&) = delete;
    PackFileTransaction& operator=(const PackFileTransaction&) = delete;
    ~PackFileTransaction() { if (!keep_backup_) clean(); }

    void write(const std::filesystem::path& path, std::string_view data) {
        const auto relative = relative_path(path);
        const auto staged = work_ / L"new" / relative;
        std::filesystem::create_directories(staged.parent_path());
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) throw std::runtime_error("Could not prepare " + path.filename().string() + "; the installed pack is unchanged");
        changes_[relative].remove = false;
    }
    void remove(const std::filesystem::path& path) { changes_[relative_path(path)].remove = true; }
    bool removing(const std::filesystem::path& path) const {
        const auto found = changes_.find(relative_path(path));
        return found != changes_.end() && found->second.remove;
    }
    void commit() {
        // Finish all backups first: even running out of space here leaves the installed files untouched.
        for (auto& [relative, change] : changes_) {
            const auto destination = root_ / relative;
            change.existed = std::filesystem::exists(destination);
            if (change.existed) {
                if (!std::filesystem::is_regular_file(destination)) throw std::runtime_error("A pack file is not a regular file");
                const auto backup = work_ / L"old" / relative;
                std::filesystem::create_directories(backup.parent_path());
                std::filesystem::copy_file(destination, backup);
            }
        }
        try {
            for (auto& [relative, change] : changes_) {
                const auto destination = root_ / relative;
                if (change.remove) {
                    if (change.existed) {
                        change.applied = true;
                        std::filesystem::remove(destination);
                    }
                } else {
                    std::filesystem::create_directories(destination.parent_path());
                    std::filesystem::rename(work_ / L"new" / relative, destination);
                    change.applied = true;
                }
            }
        } catch (...) {
            if (!restore()) {
                keep_backup_ = true;
                throw std::runtime_error("The pack update failed; recovery files were kept in " + work_.string());
            }
            throw;
        }
        clean();
    }
private:
    struct Change { bool remove{}, existed{}, applied{}; };
    std::filesystem::path root_, work_;
    std::map<std::filesystem::path, Change> changes_;
    bool keep_backup_{};
    std::filesystem::path relative_path(const std::filesystem::path& path) const {
        const auto relative = std::filesystem::absolute(path).lexically_normal().lexically_relative(root_);
        if (relative.empty() || relative == L"." || relative.has_root_path()) throw std::runtime_error("Invalid pack destination");
        for (const auto& part : relative)
            if (part == L".." || part == L".pack-transactions") throw std::runtime_error("Pack destination is outside its library");
        return relative;
    }
    bool restore() noexcept {
        bool restored = true;
        for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
            const auto& [relative, change] = *it;
            if (!change.applied) continue;
            std::error_code error;
            const auto destination = root_ / relative;
            if (change.existed) std::filesystem::rename(work_ / L"old" / relative, destination, error);
            else std::filesystem::remove(destination, error);
            restored = restored && !error;
        }
        return restored;
    }
    void clean() noexcept {
        // work_ was newly created by this transaction, strictly under root_/.pack-transactions.
        std::error_code ignored;
        std::filesystem::remove_all(work_, ignored);
    }
};

// Shell cancellation is a failure, never permission to permanently delete the remaining files.
template <class Recycle>
void recycle_pack_files(const std::vector<std::filesystem::path>& paths, Recycle&& recycle) {
    if (!recycle(paths))
        throw std::runtime_error("The pack was not removed: recycling failed or was cancelled. Remaining files were kept; some files may already be in the Recycle Bin.");
}
} // namespace dingosdk::anim_studio
