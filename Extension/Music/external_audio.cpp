#include "external_audio.h"
#include "Engine/Core/Json/json.h"
#include "Extension/Boot/user_data_redirect.h"
#include <Windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <Psapi.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace dingosdk::external_audio {
namespace {
std::mutex mutex;
std::string picked;
bool read{};
std::atomic<bool> is_on{};

template <class T> struct Com {
    T* p{};
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};
std::filesystem::path kept_file() { return redirected_local_app_data().parent_path() / L"external-audio.json"; }
// (With the mutex held.)
void read_kept() {
    if (read) return;
    read = true;
    try {
        std::ifstream in(kept_file(), std::ios::binary);
        if (!in) return;
        picked = Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())).value("app", "");
    } catch (...) {}
}
// An application's name is its file's: letters, digits and a few marks, ending in ".exe".
bool plain(const std::string& name) {
    return !name.empty() && name.size() <= 80 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' ' || c == '+'; });
}
} // namespace

std::string app() {
    std::lock_guard lock(mutex);
    read_kept();
    return picked;
}
void set_app(const std::string& executable) {
    if (!executable.empty() && !plain(executable)) return;
    std::lock_guard lock(mutex);
    read_kept();
    picked = executable;
    try {
        Json json = Json::object();
        json["app"] = picked;
        std::error_code ignored;
        std::filesystem::create_directories(kept_file().parent_path(), ignored);
        std::ofstream(kept_file(), std::ios::binary | std::ios::trunc) << json.dump(2);
    } catch (...) {}
}

std::vector<std::string> apps() {
    std::vector<std::string> found;
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    try {
        Com<IMMDeviceEnumerator> devices;
        Com<IMMDevice> device;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> sessions;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices))) &&
            SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) && SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
            int count{};
            sessions->GetCount(&count);
            for (int i = 0; i < count; ++i) {
                Com<IAudioSessionControl> control;
                Com<IAudioSessionControl2> detail;
                DWORD owner{};
                if (FAILED(sessions->GetSession(i, &control)) || FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&detail))) ||
                    FAILED(detail->GetProcessId(&owner)) || !owner || owner == GetCurrentProcessId())
                    continue;
                const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
                if (!process) continue;
                wchar_t path[MAX_PATH]{};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                    const auto name = std::filesystem::path(path).filename().wstring();
                    std::string narrow;
                    for (const auto c : name) narrow.push_back(c < 128 ? static_cast<char>(c) : '_');
                    if (plain(narrow) && std::find(found.begin(), found.end(), narrow) == found.end()) found.push_back(narrow);
                }
                CloseHandle(process);
            }
        }
    } catch (...) {}
    if (initialized) CoUninitialize();
    std::sort(found.begin(), found.end());
    return found;
}

bool on() noexcept { return is_on.load(std::memory_order_relaxed); }
void set_on(bool on) noexcept { is_on.store(on, std::memory_order_relaxed); }
} // namespace dingosdk::external_audio
