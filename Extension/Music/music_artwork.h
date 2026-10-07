#pragma once
#include <filesystem>
#include <memory>
#include <string>

namespace dingosdk::profile_runtime {
// Serves registered mod PNGs on loopback. The 64 MiB byte cache reloads evicted
// images from their registered canonical files; HTTP paths never name arbitrary files.
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
