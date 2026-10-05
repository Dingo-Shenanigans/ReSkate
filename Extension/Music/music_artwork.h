#pragma once
#include <filesystem>
#include <memory>
#include <string>

namespace dingosdk::profile_runtime {
// Serves only registered PNG bytes on an ephemeral loopback port. No filesystem HTTP routes.
class MusicArtworkServer {
public:
    MusicArtworkServer();
    ~MusicArtworkServer();
    MusicArtworkServer(const MusicArtworkServer&) = delete;
    MusicArtworkServer& operator=(const MusicArtworkServer&) = delete;
    std::string add(const std::filesystem::path& mod, const std::string& relative);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::string mod_music_artwork_url(const std::filesystem::path& mod, const std::string& relative);
}
