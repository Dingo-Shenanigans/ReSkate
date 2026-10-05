#include "server_radio.h"
#include "server_text.h"
#include <algorithm>
#include <array>
#include <cctype>
#ifndef _WIN32
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <opus.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace dingosdk::server {
namespace fs = std::filesystem;
using namespace multiplayer;

namespace {
bool web_url(std::string_view text) {
    const auto scheme = lower(text.substr(0, std::min<std::size_t>(text.size(), 8)));
    return scheme.starts_with("http://") || scheme.starts_with("https://");
}
} // namespace

std::string Radio::check_source(std::string_view source, const fs::path &folder, std::string &error) {
    source = trim(source);
    if (source.empty()) {
        error = "radio play <URL, or a file or folder in the server's Radio folder>";
        return {};
    }
    // Control characters and spaces never belong in a URL or one of these names, and keep
    // anything that looks like a second argument out of the tools' command lines.
    if (source.size() > 2048 || std::any_of(source.begin(), source.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        error = "That is not a URL or a file name.";
        return {};
    }
    if (web_url(source)) {
        if (source.find(' ') != std::string_view::npos) {
            error = "A URL has no spaces.";
            return {};
        }
        return std::string(source);
    }
    if (source.find("://") != std::string_view::npos || source.front() == '-') {
        error = "The radio plays http and https URLs, or files in the server's Radio folder.";
        return {};
    }
    // A file or folder in the Radio folder, named relative to it: never outside it, whether
    // by an absolute path, "..", or a link that points elsewhere.
    const fs::path relative(std::string{source});
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
        std::any_of(relative.begin(), relative.end(), [](const fs::path &part) { return part == ".."; })) {
        error = "Name a file or folder inside the server's Radio folder.";
        return {};
    }
    std::error_code failed;
    const auto root = fs::weakly_canonical(folder, failed);
    const auto target = failed ? fs::path{} : fs::weakly_canonical(folder / relative, failed);
    const auto inside = [&] {
        auto r = root.begin(), t = target.begin();
        for (; r != root.end(); ++r, ++t)
            if (t == target.end() || *r != *t) return false;
        return t != target.end();
    };
    if (failed || !fs::exists(target, failed) || !inside()) {
        error = "No file or folder called \"" + std::string(source) + "\" in " + folder.string() + ".";
        return {};
    }
    return target.string();
}

#ifdef _WIN32
struct Radio::State {};
Radio::Radio(fs::path) : state_(std::make_unique<State>()) {}
Radio::~Radio() = default;
std::string Radio::play(std::string_view) { return "The radio runs on the Linux server only for now."; }
std::string Radio::skip() { return play({}); }
std::string Radio::stop() { return play({}); }
std::string Radio::status() const { return "The radio runs on the Linux server only for now."; }
Radio::Output Radio::poll(std::uint64_t) noexcept { return {}; }
#else
namespace {
constexpr std::size_t queue_frames = 150;    // 3 s encoded ahead of playback
constexpr std::uint64_t frame_us = 20000;
constexpr std::uint64_t lead_us = 200000;    // released this far ahead of real time
constexpr std::size_t batch_frames = 5;
constexpr int bitrate = 96000;
constexpr std::size_t max_listing = 1 << 20; // what yt-dlp may print about one source
constexpr std::array<std::string_view, 9> audio_files{".mp3", ".ogg", ".opus", ".flac", ".wav", ".m4a", ".aac", ".webm", ".mka"};

bool installed(const char *program) {
    const char *path = std::getenv("PATH");
    for (std::string_view rest = path ? path : ""; !rest.empty();) {
        const auto colon = rest.find(':');
        const auto dir = rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        if (!dir.empty() && access((std::string(dir) + "/" + program).c_str(), X_OK) == 0) return true;
    }
    return false;
}
} // namespace

struct Radio::State {
    struct Item {
        std::uint32_t track{}, index{};
        std::vector<std::uint8_t> opus;
        std::string title; // on a track's first frame
    };
    struct Entry {
        std::string input, title;
    };

    fs::path folder;
    bool ytdlp = installed("yt-dlp"), ffmpeg = installed("ffmpeg");

    mutable std::mutex mutex;
    std::condition_variable room; // the worker waits here for space in the queue
    std::deque<Item> queue;
    std::deque<std::string> notices;
    std::thread worker;
    bool cancel{}, worker_done{}, active{};
    pid_t child{}; // the running tool's process group
    std::uint32_t next_track = 1, worker_track{}, playing_track{}, skipped{};
    std::string source, playing_title;
    std::uint64_t next_at{}, frames{}, bytes{}, started{};

    void notice(std::string text) {
        std::lock_guard lock(mutex);
        notices.push_back(std::move(text));
    }
    bool cancelled() {
        std::lock_guard lock(mutex);
        return cancel;
    }
    void halt() {
        {
            std::lock_guard lock(mutex);
            cancel = true;
            if (child > 0) ::kill(-child, SIGTERM);
        }
        room.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        queue.clear();
        cancel = worker_done = active = false;
        child = 0;
        next_at = 0;
        playing_title.clear();
    }

    // Runs a tool from an argument list, never a shell: its own process group (so stop and skip
    // end it and anything it starts), stdin and stderr closed off, stdout piped back.
    pid_t spawn(const std::vector<std::string> &args, int &out) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) return -1;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        // The server ignores SIGPIPE; a tool must not inherit that, or it would outlive a closed pipe.
        sigset_t defaults, none;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);
        sigemptyset(&none);
        posix_spawnattr_setsigdefault(&attributes, &defaults);
        posix_spawnattr_setsigmask(&attributes, &none);
        posix_spawnattr_setpgroup(&attributes, 0);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
        std::vector<char *> argv;
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        pid_t pid{};
        const auto result = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(fds[1]);
        if (result != 0) {
            close(fds[0]);
            return -1;
        }
        out = fds[0];
        std::lock_guard lock(mutex);
        child = pid;
        if (cancel) ::kill(-pid, SIGTERM); // stopped while it started
        return pid;
    }
    int reap(pid_t pid, int out) {
        close(out);
        int status{};
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        std::lock_guard lock(mutex);
        if (child == pid) child = 0;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    std::string capture(const std::vector<std::string> &args) {
        int out{};
        const auto pid = spawn(args, out);
        if (pid < 0) return {};
        std::string text;
        char buffer[4096];
        for (ssize_t count; (count = read(out, buffer, sizeof buffer)) != 0;) {
            if (count < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (text.size() < max_listing) text.append(buffer, static_cast<std::size_t>(count));
        }
        return reap(pid, out) == 0 ? text : std::string{};
    }

    std::vector<Entry> entries(const std::string &input) {
        std::vector<Entry> result;
        if (web_url(input)) {
            if (!ytdlp) return {{input, input}};
            // One line per video ("<url>\t<title>"): a playlist lists them all (the first 500 of a
            // channel), a page lists itself. A single video has no flat "url", only its page's.
            const auto listing = capture({"yt-dlp", "--ignore-config", "--no-warnings", "--flat-playlist", "--playlist-end",
                                          "500", "--print", "%(webpage_url,url)s\t%(title)s", "--", input});
            for (std::string_view rest = listing; !rest.empty();) {
                const auto end = rest.find('\n');
                const auto line = rest.substr(0, end);
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
                const auto tab = line.find('\t');
                const auto url = line.substr(0, tab);
                if (!web_url(url) || url.find(' ') != std::string_view::npos) continue;
                const auto title = tab == std::string_view::npos ? url : trim(line.substr(tab + 1));
                result.push_back({std::string(url), std::string(title.empty() ? url : title)});
            }
            return result;
        }
        std::error_code failed;
        if (fs::is_directory(input, failed)) {
            for (const auto &file : fs::directory_iterator(input, failed))
                if (file.is_regular_file(failed) &&
                    std::find(audio_files.begin(), audio_files.end(), lower(file.path().extension().string())) != audio_files.end())
                    result.push_back({file.path().string(), file.path().stem().string()});
            std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) { return a.input < b.input; });
        } else {
            result.push_back({input, fs::path(input).stem().string()});
        }
        return result;
    }

    // Waits for room, then queues one encoded frame. False once this track is skipped or the radio stops.
    bool push(Item item) {
        std::unique_lock lock(mutex);
        room.wait(lock, [&] { return cancel || item.track <= skipped || queue.size() < queue_frames; });
        if (cancel || item.track <= skipped) return false;
        queue.push_back(std::move(item));
        return true;
    }

    // Decodes one entry with ffmpeg and encodes it. Returns how many frames it queued.
    std::size_t play(const Entry &entry, bool remote) {
        std::string input = entry.input;
        if (remote && ytdlp) {
            // The page's audio stream: the best audio-only format, or whatever it has.
            const auto direct = capture({"yt-dlp", "--ignore-config", "--no-warnings", "--no-playlist", "-f",
                                         "bestaudio/best", "-g", "--", entry.input});
            input = std::string(trim(std::string_view(direct).substr(0, direct.find('\n'))));
            if (!web_url(input)) return 0;
        }
        std::uint32_t track{};
        {
            std::lock_guard lock(mutex);
            track = worker_track = next_track++;
        }
        int error{};
        auto *encoder = opus_encoder_create(radio_rate, radio_channels, OPUS_APPLICATION_AUDIO, &error);
        if (!encoder || error != OPUS_OK) return 0;
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
        // Remote sources may not reach local files, local ones may not reach the network.
        int out{};
        const auto pid = spawn({"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-protocol_whitelist",
                                remote ? "http,https,tcp,tls,crypto,hls" : "file", "-i", remote ? input : "file:" + input,
                                "-vn", "-ac", "2", "-ar", std::to_string(radio_rate), "-f", "s16le", "pipe:1"},
                               out);
        std::size_t queued{};
        if (pid >= 0) {
            constexpr auto samples = radio_frame_samples * radio_channels;
            std::vector<opus_int16> pcm;
            std::vector<std::uint8_t> raw;
            std::array<unsigned char, max_radio_frame> packet{};
            bool open = true;
            const auto encode = [&] {
                const auto size = opus_encode(encoder, pcm.data(), static_cast<int>(radio_frame_samples), packet.data(),
                                              static_cast<opus_int32>(packet.size()));
                pcm.erase(pcm.begin(), pcm.begin() + samples);
                if (size <= 0) return true;
                Item item{track, static_cast<std::uint32_t>(queued), {packet.begin(), packet.begin() + size},
                          queued ? std::string{} : entry.title};
                if (!push(std::move(item))) return false;
                ++queued;
                return true;
            };
            char buffer[16384];
            for (ssize_t count; open && (count = read(out, buffer, sizeof buffer)) != 0;) {
                if (count < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                raw.insert(raw.end(), buffer, buffer + count);
                const auto whole = raw.size() / 2;
                for (std::size_t i = 0; i < whole; ++i)
                    pcm.push_back(static_cast<opus_int16>(raw[2 * i] | (raw[2 * i + 1] << 8)));
                raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(whole * 2));
                while (open && pcm.size() >= samples) open = encode();
            }
            if (open && !pcm.empty()) { // the last partial frame, padded with silence
                pcm.resize(samples);
                encode();
            }
            if (!open) ::kill(-pid, SIGTERM); // skipped or stopped: ffmpeg may still be writing
            reap(pid, out);
        }
        opus_encoder_destroy(encoder);
        return queued;
    }

    void run(std::string input) {
        const bool remote = web_url(input);
        const auto list = entries(input);
        if (list.empty())
            notice("Radio: nothing to play at " + input + (remote && !ytdlp ? " (pages and playlists need yt-dlp installed)" : "") + ".");
        for (const auto &entry : list) {
            if (cancelled()) return;
            if (!play(entry, remote) && !cancelled()) {
                bool skipped_this{};
                {
                    std::lock_guard lock(mutex);
                    skipped_this = worker_track <= skipped;
                }
                if (!skipped_this) notice("Radio: could not play " + entry.title + ".");
            }
        }
        std::lock_guard lock(mutex);
        worker_done = true;
    }
};

Radio::Radio(fs::path folder) : state_(std::make_unique<State>()) { state_->folder = std::move(folder); }
Radio::~Radio() { state_->halt(); }

std::string Radio::play(std::string_view source) {
    auto &s = *state_;
    std::string error;
    const auto input = check_source(source, s.folder, error);
    if (input.empty()) return error;
    if (!s.ffmpeg) return "The radio needs ffmpeg installed on the server.";
    s.halt();
    {
        std::lock_guard lock(s.mutex);
        s.source = std::string(trim(source));
        s.active = true;
        s.frames = s.bytes = s.started = 0;
        s.skipped = s.next_track - 1; // nothing older may play
    }
    s.worker = std::thread([&s, input] {
        try {
            s.run(input);
        } catch (const std::exception &e) {
            s.notice(std::string("Radio: ") + e.what());
            std::lock_guard lock(s.mutex);
            s.worker_done = true;
        }
    });
    return "Radio: starting " + s.source + (web_url(input) && !s.ytdlp ? " (as a direct stream: yt-dlp is not installed)" : "") + ".";
}
std::string Radio::skip() {
    auto &s = *state_;
    std::lock_guard lock(s.mutex);
    if (!s.active) return "The radio is off.";
    // Only what is playing now: the next song may already be buffering.
    s.skipped = std::max(s.skipped, s.playing_track);
    std::erase_if(s.queue, [&](const State::Item &item) { return item.track <= s.skipped; });
    if (s.worker_track <= s.skipped && s.child > 0) ::kill(-s.child, SIGTERM);
    s.room.notify_all();
    return "Radio: skipped " + (s.playing_title.empty() ? std::string("the song") : s.playing_title) + ".";
}
std::string Radio::stop() {
    auto &s = *state_;
    {
        std::lock_guard lock(s.mutex);
        if (!s.active) return "The radio is off.";
    }
    s.halt();
    return "Radio stopped.";
}
std::string Radio::status() const {
    auto &s = *state_;
    std::lock_guard lock(s.mutex);
    std::string tools = std::string(s.ffmpeg ? "ffmpeg" : "no ffmpeg") + (s.ytdlp ? ", yt-dlp" : ", no yt-dlp");
    if (!s.active) return "The radio is off (" + tools + "). Radio folder: " + s.folder.string();
    const auto seconds = s.frames * frame_us / 1000000;
    const auto kbps = seconds ? s.bytes * 8 / 1000 / seconds : 0;
    return "Radio: " + (s.playing_title.empty() ? std::string("starting") : "playing " + s.playing_title) + " from " +
           s.source + " | " + std::to_string(seconds) + " s out, " + std::to_string(kbps) + " kbps, " +
           std::to_string(s.queue.size() * frame_us / 1000) + " ms buffered (" + tools +
           ") | not sent to players yet: the game needs radio support first";
}
Radio::Output Radio::poll(std::uint64_t now) noexcept {
    Output out;
    auto &s = *state_;
    try {
        std::lock_guard lock(s.mutex);
        while (!s.notices.empty()) {
            out.notices.push_back(std::move(s.notices.front()));
            s.notices.pop_front();
        }
        if (!s.active) return out;
        // Real time: frames leave one per 20 ms, a little ahead. After a stall (a slow source,
        // the server held up) the clock starts again from now rather than bursting to catch up.
        if (!s.next_at || s.next_at + 1000000 < now) s.next_at = now;
        while (!s.queue.empty() && s.next_at <= now + lead_us) {
            auto item = std::move(s.queue.front());
            s.queue.pop_front();
            if (item.track <= s.skipped) continue;
            if (!item.title.empty()) {
                s.playing_title = item.title;
                out.notices.push_back("Now playing: " + item.title);
            }
            s.playing_track = item.track;
            if (out.batches.empty() || out.batches.back().track != item.track ||
                out.batches.back().frames.size() >= batch_frames ||
                out.batches.back().first + out.batches.back().frames.size() != item.index)
                out.batches.push_back({item.track, item.index, {}});
            s.bytes += item.opus.size();
            ++s.frames;
            out.batches.back().frames.push_back(std::move(item.opus));
            s.next_at += frame_us;
        }
        s.room.notify_all();
        if (s.worker_done && s.queue.empty() && s.next_at <= now) {
            s.active = false;
            s.playing_title.clear();
            out.notices.push_back("Radio: the queue has finished.");
        }
    } catch (...) {
    }
    return out;
}
#endif
} // namespace dingosdk::server
