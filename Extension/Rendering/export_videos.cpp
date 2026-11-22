#include "export_videos.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/UI/game_window.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace dingosdk::export_videos {
namespace {
namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
fs::path from_utf8(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }
fs::path local_app_data() {
    PWSTR raw{};
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) result = raw;
    CoTaskMemFree(raw);
    return result;
}
// Where the game writes its videos: under the local app data ReSkate gives it (ReSkate\Game), else Windows' own.
std::vector<fs::path> game_folders() {
    std::vector<fs::path> folders;
    const auto local = local_app_data();
    if (local.empty()) return folders;
    folders.push_back(local / L"ReSkate" / L"Game" / L"Skate" / L"data" / L"Videos");
    folders.push_back(local / L"Skate" / L"data" / L"Videos");
    return folders;
}
fs::path game_folder() {
    std::error_code error;
    const auto folders = game_folders();
    for (const auto& folder : folders)
        if (fs::is_directory(folder, error)) return folder;
    return folders.empty() ? fs::path() : folders.front();
}
bool a_video(const fs::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".mp4" || extension == L".webm";
}
// A name a plugin gave: one file's, no folder in it.
bool plain_name(std::string_view name) {
    return !name.empty() && name.size() <= 260 && name.find_first_of("/\\:*?\"<>|") == std::string_view::npos && name != "." && name != "..";
}
// The game still has it while it cannot be opened alone (it is writing or finishing it).
bool let_go(const fs::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}
// A name in the folder not taken yet: "name.mp4", else "name (2).mp4", ...
fs::path free_name(const fs::path& folder, const fs::path& name) {
    std::error_code error;
    auto target = folder / name;
    for (int n = 2; fs::exists(target, error) && n < 1000; ++n) target = folder / (name.stem().wstring() + std::format(L" ({})", n) + name.extension().wstring());
    return target;
}

struct State {
    std::mutex mutex;
    bool loaded{};
    std::map<std::string, std::string, std::less<>> folders; // plugin -> its folder (UTF-8)
    std::map<std::string, std::vector<Moved>, std::less<>> results;
    std::set<std::string, std::less<>> moving; // the game's videos being moved, by name
    std::vector<Video> listed;
    std::chrono::steady_clock::time_point listed_at{};
};
State& state() {
    static auto* s = new State; // (never destroyed: threads of its own may still finish after the game closes)
    return *s;
}
fs::path folders_file() {
    const auto local = local_app_data();
    return local.empty() ? fs::path() : local / L"ReSkate" / L"plugin_video_folders.json";
}
// Under the lock.
void load(State& s) {
    if (std::exchange(s.loaded, true)) return;
    try {
        const auto file = folders_file();
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        std::stringstream text;
        text << in.rdbuf();
        const auto json = Json::parse(text.str());
        if (!json.is_object()) return;
        for (const auto& [plugin, folder] : json.items())
            if (folder.is_string()) s.folders[plugin] = folder.string();
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::plugins, "Plugins' video folders could not be read: {}.", error.what());
    }
}
void save(const State& s) {
    try {
        const auto file = folders_file();
        if (file.empty()) return;
        Json json = Json::object();
        for (const auto& [plugin, folder] : s.folders) json[plugin] = folder;
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        std::ofstream(file, std::ios::binary | std::ios::trunc) << json.dump(2) << "\n";
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::plugins, "Plugins' video folders could not be saved: {}.", error.what());
    }
}
std::atomic<bool> picking{};
}

std::vector<Video> list() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - s.listed_at < std::chrono::milliseconds(500)) return s.listed;
    s.listed_at = now;
    s.listed.clear();
    std::error_code error;
    const auto folder = game_folder();
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code one;
        if (!it->is_regular_file(one) || !a_video(it->path())) continue;
        Video video;
        video.name = utf8(it->path().filename());
        video.bytes = it->file_size(one);
        const auto written = it->last_write_time(one);
        if (!one)
            video.written = std::chrono::duration<double>(std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch()).count();
        video.ready = !s.moving.contains(video.name) && let_go(it->path());
        s.listed.push_back(std::move(video));
        if (s.listed.size() >= 500) break;
    }
    std::sort(s.listed.begin(), s.listed.end(), [](const Video& a, const Video& b) { return a.written > b.written; });
    return s.listed;
}

std::string folder(std::string_view plugin) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    load(s);
    const auto found = s.folders.find(plugin);
    return found == s.folders.end() ? std::string() : found->second;
}

std::string pick_folder(std::string_view plugin, std::string_view title) {
    if (picking.exchange(true)) return "a folder picker is open already";
    std::wstring caption;
    {
        const auto wide = from_utf8(title.empty() ? std::string_view("Where your videos go") : title).wstring();
        caption = wide.substr(0, 120);
    }
    const auto start_in = [&] {
        const auto chosen = folder(plugin);
        return chosen.empty() ? game_folder() : from_utf8(chosen);
    }();
    try {
        std::thread([owner = std::string(plugin), caption, start_in] {
            // (Nothing may leave a thread of its own: the game would close.)
            try {
                std::string chosen;
                if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
                    {
                        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
                        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                            DWORD options{};
                            (void)dialog->GetOptions(&options);
                            (void)dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                            (void)dialog->SetTitle(caption.c_str());
                            (void)dialog->SetOkButtonLabel(L"Use this folder");
                            Microsoft::WRL::ComPtr<IShellItem> start;
                            std::error_code error;
                            if (!start_in.empty() && fs::is_directory(start_in, error) &&
                                SUCCEEDED(SHCreateItemFromParsingName(start_in.c_str(), nullptr, IID_PPV_ARGS(&start))))
                                (void)dialog->SetFolder(start.Get());
                            if (SUCCEEDED(dialog->Show(nullptr))) {
                                Microsoft::WRL::ComPtr<IShellItem> item;
                                PWSTR path{};
                                if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                                    chosen = utf8(fs::path(path));
                                    CoTaskMemFree(path);
                                }
                            }
                        }
                    }
                    CoUninitialize();
                }
                if (const auto window = static_cast<HWND>(game_window())) SetForegroundWindow(window);
                if (!chosen.empty()) {
                    auto& s = state();
                    std::lock_guard lock(s.mutex);
                    load(s);
                    s.folders[owner] = chosen;
                    save(s);
                    logging::log(logging::Level::info, logging::Channel::plugins, "Plugin {}: its videos go to {}.", owner, chosen);
                }
            } catch (...) {
            }
            picking.store(false);
        }).detach();
    } catch (...) {
        picking.store(false);
        return "the folder picker could not open";
    }
    return {};
}

void forget_folder(std::string_view plugin) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    load(s);
    if (s.folders.erase(std::string(plugin))) save(s);
}

std::string open_folder(std::string_view plugin) {
    const auto chosen = folder(plugin);
    const auto path = chosen.empty() ? game_folder() : from_utf8(chosen);
    if (path.empty()) return "there is no folder to open";
    try {
        std::thread([path] {
            try {
                std::error_code error;
                fs::create_directories(path, error);
                ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } catch (...) {
            }
        }).detach();
    } catch (...) {
        return "File Explorer could not be opened";
    }
    return {};
}

std::string move(std::string_view plugin, std::string_view name) {
    if (!plain_name(name)) return "a video is named by its file's name";
    const auto chosen = folder(plugin);
    if (chosen.empty()) return "the player has picked no folder for its videos (reskate.videos.pick_folder)";
    const auto source = game_folder() / from_utf8(name);
    std::error_code error;
    if (!a_video(source) || !fs::is_regular_file(source, error)) return "the game has no such video";
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (s.moving.contains(name)) return "it is being moved already";
        if (!let_go(source)) return "the game is still writing it";
        s.moving.insert(std::string(name));
        s.listed_at = {};
    }
    try {
        std::thread([owner = std::string(plugin), video = std::string(name), source, to = from_utf8(chosen)] {
            Moved result{video, {}, {}};
            try {
                std::error_code error;
                fs::create_directories(to, error);
                const auto target = free_name(to, source.filename());
                if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
                    result.to = utf8(target.filename());
                    logging::log(logging::Level::info, logging::Channel::plugins, "Plugin {}: the video {} went to {}.", owner, video, utf8(target));
                } else {
                    result.why = std::format("Windows could not move it (error {})", GetLastError());
                }
            } catch (const std::exception& error) {
                result.why = error.what();
            } catch (...) {
                result.why = "it could not be moved";
            }
            try {
                auto& s = state();
                std::lock_guard lock(s.mutex);
                s.moving.erase(video);
                s.listed_at = {};
                auto& results = s.results[owner];
                if (results.size() < 64) results.push_back(std::move(result));
            } catch (...) {
            }
        }).detach();
    } catch (...) {
        std::lock_guard lock(s.mutex);
        s.moving.erase(std::string(name));
        return "the move could not start";
    }
    return {};
}

std::vector<Moved> moved(std::string_view plugin) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto found = s.results.find(plugin);
    if (found == s.results.end()) return {};
    auto results = std::move(found->second);
    s.results.erase(found);
    return results;
}

void forget(std::string_view plugin) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (const auto found = s.results.find(plugin); found != s.results.end()) s.results.erase(found);
}
}
