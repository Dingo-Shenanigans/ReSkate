#pragma once
#include "Extension/Multiplayer/Net/radio_frames.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::server {
// The server's radio: an admin plays a source (`radio play <source>`) and the server turns it
// into 20 ms Opus frames (Extension/Multiplayer/Net/radio_frames.h), released in real time.
// A source is the http(s) URL of an audio stream (an internet radio station, an MP3), or a file
// or folder inside the server's Radio folder. ffmpeg decodes it. Web pages and video sites are not
// sources: the server owner sends what plays to every player, so it should be audio they may share.
//
// Admin text ends up in ffmpeg's arguments, so it never sees a shell: it runs from an argument
// list, and only opens the protocols the source needs.
//
// Runs on Linux and Windows servers. On Windows only ffmpeg.exe from a folder on PATH is started,
// never a .bat or .cmd (server_radio.cpp, find_program).
class Radio {
  public:
    explicit Radio(std::filesystem::path folder);
    ~Radio();
    Radio(const Radio &) = delete;
    Radio &operator=(const Radio &) = delete;

    // Replies for the console or the admin who asked.
    std::string play(std::string_view source);
    std::string skip();
    std::string stop();
    std::string status() const;

    struct Output {
        std::vector<multiplayer::RadioBatch> batches; // frames due now, in order
        std::vector<std::string> notices;             // "Now playing ...", problems; for chat and the log
    };
    // Frames due by `now_us`, up to a fifth of a second ahead. Never throws.
    Output poll(std::uint64_t now_us) noexcept;

    // What `play` accepts: the URL as given, or the absolute path of a file or folder inside
    // `folder`. Empty, with `error` set, for anything else.
    static std::string check_source(std::string_view source, const std::filesystem::path &folder, std::string &error);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace dingosdk::server
