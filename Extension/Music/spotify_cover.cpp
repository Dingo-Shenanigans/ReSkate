#include "spotify_cover.h"
#include "music_artwork.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "ole32")

namespace dingosdk::spotify_cover {
namespace {
std::mutex mutex;
std::string ready;
std::atomic<std::uint64_t> asked{~0ull};
std::atomic<bool> busy{};

template <class T> struct Com {
    T* p{};
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// Any picture Windows can read, written as a PNG no larger than 512 across.
bool write_png(const std::vector<std::uint8_t>& image, const std::filesystem::path& file) {
    Com<IWICImagingFactory> factory;
    Com<IStream> input;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> converted;
    Com<IWICBitmapScaler> scaled;
    Com<IWICStream> output;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> target;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    input.p = SHCreateMemStream(image.data(), static_cast<UINT>(image.size()));
    UINT width{}, height{};
    if (!input.p || FAILED(factory->CreateDecoderFromStream(input.p, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converted)) ||
        FAILED(converted->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converted->GetSize(&width, &height)) || !width || !height)
        return false;
    IWICBitmapSource* source = converted.p;
    if (const UINT longest = width > height ? width : height; longest > 512) {
        width = width * 512 / longest;
        height = height * 512 / longest;
        if (!width || !height || FAILED(factory->CreateBitmapScaler(&scaled)) ||
            FAILED(scaled->Initialize(converted.p, width, height, WICBitmapInterpolationModeFant)))
            return false;
        source = scaled.p;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(factory->CreateStream(&output)) && SUCCEEDED(output->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(output.p, WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&target, nullptr)) && SUCCEEDED(target->Initialize(nullptr)) && SUCCEEDED(target->SetSize(width, height)) &&
           SUCCEEDED(target->SetPixelFormat(&format)) && SUCCEEDED(target->WriteSource(source, nullptr)) && SUCCEEDED(target->Commit()) &&
           SUCCEEDED(encoder->Commit());
}

void make(std::vector<std::uint8_t> image) noexcept {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    try {
        // A folder of this run's covers; those of an earlier run are cleared out of it once.
        static const auto folder = [] {
            auto path = std::filesystem::temp_directory_path() / L"ReSkateSpotify";
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
            std::filesystem::create_directories(path, ignored);
            return path;
        }();
        static unsigned number{};
        const auto name = std::format("cover{}-{:x}.png", ++number, GetTickCount64());
        if (write_png(image, folder / name)) {
            if (auto address = profile_runtime::mod_music_artwork_url(folder, name); !address.empty()) {
                std::lock_guard lock(mutex);
                ready = std::move(address);
            } else {
                logging::log(logging::Level::warning, logging::Channel::music, "Spotify station: the cover could not be served to the game.");
            }
        } else {
            logging::log(logging::Level::warning, logging::Channel::music, "Spotify station: the cover Windows gave ({} bytes) could not be read as a picture.", image.size());
        }
    } catch (...) {}
    if (initialized) CoUninitialize();
    busy.store(false);
}
} // namespace

void want(const std::vector<std::uint8_t>& image, std::uint64_t revision) noexcept {
    if (image.empty() || asked.load() == revision || busy.exchange(true)) return;
    asked.store(revision);
    try {
        std::thread(make, image).detach();
    } catch (...) {
        busy.store(false);
    }
}

std::string serve(const std::vector<std::uint8_t>& image, const std::string& name) {
    try {
        const auto folder = std::filesystem::temp_directory_path() / L"ReSkateSpotifyStation";
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        if (image.empty() || !write_png(image, folder / name)) return {};
        return profile_runtime::mod_music_artwork_url(folder, name);
    } catch (...) {
        return {};
    }
}

namespace {
// A picture built into the DLL (cmake/Runtime.cmake), served under `name`.
std::string built_in(const wchar_t* resource_name, const char* name) {
    try {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&station_url), &module))
            return {};
        const auto resource = FindResourceW(module, resource_name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        const auto handle = resource ? LoadResource(module, resource) : nullptr;
        const auto* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
        if (!data) return {};
        // (A folder of its own: the covers' folder is cleared as the first of them is made.)
        const auto folder = std::filesystem::temp_directory_path() / L"ReSkateSpotifyStation";
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        {
            std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
            file.write(data, SizeofResource(module, resource));
            if (!file) return {};
        }
        return profile_runtime::mod_music_artwork_url(folder, name);
    } catch (...) {
        return {};
    }
}
} // namespace

std::string station_url() {
    static const std::string address = built_in(L"SPOTIFY_PNG", "spotify.png");
    return address;
}
std::string liked_url() {
    static const std::string address = built_in(L"LIKED_PNG", "liked.png");
    return address;
}

std::string url() {
    std::lock_guard lock(mutex);
    return ready;
}
} // namespace dingosdk::spotify_cover
