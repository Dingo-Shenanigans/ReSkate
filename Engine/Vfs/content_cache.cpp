#include "content_cache.h"
#include "content_cache_internal.h"
#include "Engine/Game/Build/supported_build.h"
#ifdef _WIN32
#include <Windows.h>
#include "Engine/Core/Platform/user_data_paths.h"
#endif
#include <array>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace dingosdk::content_cache {
const Pack& supported_pack() {
    static const Pack pack{std::wstring(supported_build::content_cache_url),
        std::string(supported_build::content_cache_sha256), supported_build::content_cache_bytes,
        std::wstring(supported_build::steam_build_id.begin(), supported_build::steam_build_id.end())};
    return pack;
}

std::filesystem::path directory(const Pack& pack) {
#ifdef _WIN32
    return user_data::root() / L"cache" / pack.build;
#else
    // Linux: XDG cache or ~/.cache. Dedicated servers rarely need this
    // (world layers come from world-layers.json), but scan caching uses it.
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0]) {
        std::filesystem::path base(xdg);
        if (base.is_absolute()) return base / "ReSkate" / "cache" / pack.build;
    }
    if (const char *home = std::getenv("HOME"); home && home[0])
        return std::filesystem::path(home) / ".cache" / "ReSkate" / "cache" / pack.build;
    throw std::runtime_error("HOME is unavailable");
#endif
}

bool installed(const std::filesystem::path& folder, const Pack& pack) {
    // The marker is written last, after every file is in place.
    std::ifstream marker(folder / detail::marker_name, std::ios::binary);
    std::string recorded;
    return marker && std::getline(marker, recorded) && recorded == pack.sha256;
}

bool installed() {
    try { return installed(directory()); }
    catch (...) { return false; }
}
}
