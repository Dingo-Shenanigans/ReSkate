#include "patch_notes.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Vfs/https_download.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// The patch notes' copy on disk and their fetch from GitHub (patch_notes.h). GitHub lets an address
// make 60 unauthenticated API requests an hour, and the news shares them (live_news.cpp), so the
// releases are asked for at most every 30 minutes, and the copy on disk stands in between, offline
// and when GitHub cannot be reached.
namespace dingosdk::news {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t most_bytes = 4 * 1024 * 1024;
constexpr auto fetch_every = std::chrono::minutes(30);

std::mutex mutex;
std::shared_ptr<const PatchNotes> current = std::make_shared<const PatchNotes>(); // under the mutex
bool working{}, disk_read{};                                                     // under the mutex
Clock::time_point next_fetch{};                                                  // under the mutex

fs::path cache_file() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"news" / L"releases.json";
}
template <class Change> void publish(Change change) {
    std::lock_guard lock(mutex);
    auto next = std::make_shared<PatchNotes>(*current);
    change(*next);
    ++next->serial;
    current = std::move(next);
}

// Reads the copy on disk the first time (`from_disk`), then asks GitHub unless that copy is new
// enough or ReSkate runs offline.
void work(bool from_disk) {
    const auto file = cache_file();
    bool fresh{};
    if (from_disk && !file.empty()) {
        try {
            std::error_code error;
            if (fs::is_regular_file(file, error) && fs::file_size(file, error) <= most_bytes) {
                std::ifstream in(file, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto releases = parse_releases(text);
                publish([&](PatchNotes& notes) { notes.releases = std::move(releases), notes.read = true; });
                const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, error);
                fresh = !error && age >= fs::file_time_type::duration::zero() && age < fetch_every;
            }
        } catch (const std::exception& e) {
            logging::log(logging::Level::info, logging::Channel::news, "Patch notes: the copy on disk is unreadable ({}).", e.what());
        }
    }
    if (!fresh && !launcher::offline_mode()) {
        publish([](PatchNotes& notes) { notes.fetching = true; });
        https::Download result;
        std::string error;
        std::vector<Release> releases;
        std::optional<std::string> body;
        try {
            body = https::get_text(releases_url, most_bytes, 15, L"ReSkate-PatchNotes/1", &result);
            if (body) releases = parse_releases(*body);
            else error = "GitHub could not be reached (HTTP " + std::to_string(result.http_status) + ", error " + std::to_string(result.error) + ").";
        } catch (const std::exception& e) {
            error = std::string("GitHub's answer is unreadable: ") + e.what();
            body.reset();
        }
        if (body && !file.empty()) {
            // Written beside and then moved over the old copy, so a copy is never half written.
            std::error_code ignored;
            fs::create_directories(file.parent_path(), ignored);
            auto partial = file;
            partial += L".partial";
            {
                std::ofstream out(partial, std::ios::binary | std::ios::trunc);
                out.write(body->data(), static_cast<std::streamsize>(body->size()));
            }
            fs::rename(partial, file, ignored);
        }
        if (error.empty()) logging::log(logging::Level::info, logging::Channel::news, "Patch notes: {} releases from GitHub.", releases.size());
        else logging::log(logging::Level::info, logging::Channel::news, "Patch notes: {}", error);
        publish([&](PatchNotes& notes) {
            notes.fetching = false;
            notes.error = error;
            if (error.empty()) notes.releases = std::move(releases), notes.read = true;
        });
    }
    std::lock_guard lock(mutex);
    working = false;
}
} // namespace

std::shared_ptr<const PatchNotes> patch_notes() {
    std::lock_guard lock(mutex);
    const auto now = Clock::now();
    if (!working && now >= next_fetch) {
        working = true;
        next_fetch = now + fetch_every;
        const bool from_disk = !std::exchange(disk_read, true);
        try {
            std::thread(work, from_disk).detach();
        } catch (...) {
            working = false;
        }
    }
    return current;
}
} // namespace dingosdk::news
