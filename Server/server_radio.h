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
// A source is an http(s) URL, or a file or folder inside the server's Radio folder. With yt-dlp
// installed a URL may be any page it reads, a playlist included; without it, a direct audio
// stream (an internet radio station, an MP3). ffmpeg decodes everything.
//
// Admin text ends up in another program's arguments, so neither tool ever sees a shell: each
// runs from an argument list, URLs come after "--", yt-dlp ignores its config files (which can
// run commands), and ffmpeg only opens the protocols the source needs.
//
// Runs on Linux and Windows servers. On Windows only ffmpeg.exe and yt-dlp.exe from a folder on
// PATH are started, never a .bat or .cmd (server_radio.cpp, find_program).
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
