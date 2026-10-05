#include "server_radio.h"
#include "server_text.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <opus.h>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
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
// Names are UTF-8 everywhere in the server; on Windows a path's narrow string is the ANSI code page.
fs::path utf8_path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }
std::string utf8_name(const fs::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
// One line of a tool's output, without the carriage return Windows programs end it with.
std::string_view line_of(std::string_view text) {
    auto line = text.substr(0, text.find('\n'));
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
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
    // On Windows a colon in a name is a drive or an alternate data stream: never a file to play.
    const bool colon = source.find(':') != std::string_view::npos;
#ifdef _WIN32
    if (colon || source.front() == '-') {
#else
    if ((colon && source.find("://") != std::string_view::npos) || source.front() == '-') {
#endif
        error = "The radio plays http and https URLs, or files in the server's Radio folder.";
        return {};
    }
    // A file or folder in the Radio folder, named relative to it: never outside it, whether
    // by an absolute path, "..", or a link that points elsewhere.
    const auto relative = utf8_path(source);
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
    // Only plain files and folders: not a device, a pipe or a socket that happens to be there.
    if (failed || !(fs::is_regular_file(target, failed) || fs::is_directory(target, failed)) || !inside()) {
        error = "No file or folder called \"" + std::string(source) + "\" in " + utf8_name(folder) + ".";
        return {};
    }
    return utf8_name(target);
}

namespace {
constexpr std::size_t queue_frames = 150;    // 3 s encoded ahead of playback
constexpr std::uint64_t frame_us = 20000;
constexpr std::uint64_t lead_us = 200000;    // released this far ahead of real time
constexpr std::size_t batch_frames = 5;
constexpr int bitrate = 96000;
constexpr std::size_t max_listing = 1 << 20; // what yt-dlp may print about one source
constexpr std::array<std::string_view, 9> audio_files{".mp3", ".ogg", ".opus", ".flac", ".wav", ".m4a", ".aac", ".webm", ".mka"};

// The tools run from an argument list, never through a shell. A running one is a Child: stop and
// skip end it together with anything it started (yt-dlp runs helpers of its own).
#ifdef _WIN32
// Its job object (which holds it and everything it starts), the process and the read end of its stdout.
struct Child {
    HANDLE job{}, process{}, out{};
    bool operator==(const Child &) const = default;
};
std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}
// The tool's .exe in a folder on PATH, as a full path. Only an .exe: CreateProcess runs a .bat or
// .cmd through cmd.exe, which would parse the arguments again (the "BatBadBut" kind of injection).
// Relative PATH entries are skipped, so the server's own folder never stands in for a tool.
std::wstring find_program(std::string_view name) {
    std::wstring path(32767, L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
    const auto file = widen(name) + L".exe";
    for (std::wstring_view rest = path; !rest.empty();) {
        const auto semicolon = rest.find(L';');
        auto folder = rest.substr(0, semicolon);
        rest = semicolon == std::wstring_view::npos ? std::wstring_view{} : rest.substr(semicolon + 1);
        if (folder.size() >= 2 && folder.front() == L'"' && folder.back() == L'"') folder = folder.substr(1, folder.size() - 2);
        const fs::path candidate = fs::path(folder) / file;
        std::error_code failed;
        if (!folder.empty() && candidate.is_absolute() && fs::is_regular_file(candidate, failed)) return candidate.wstring();
    }
    return {};
}
bool installed(const char *program) { return !find_program(program).empty(); }
// One argument as CommandLineToArgvW (and the C runtime) reads it back: quoted, with the
// backslashes before a quote doubled.
std::wstring quote(const std::wstring &argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    for (auto at = argument.begin();; ++at) {
        std::size_t backslashes{};
        for (; at != argument.end() && *at == L'\\'; ++at) ++backslashes;
        if (at == argument.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        out.append(*at == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        out.push_back(*at);
    }
    out.push_back(L'"');
    return out;
}
// stdout piped back, stdin and stderr on NUL, no console window, and nothing else of the server's
// inherited: the handle list holds just those two.
bool start(const std::vector<std::string> &args, Child &child) {
    const auto program = find_program(args.front());
    if (program.empty()) return false;
    std::wstring line = quote(program);
    for (std::size_t i = 1; i < args.size(); ++i) line += L' ' + quote(widen(args[i]));
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read{}, write{};
    if (!CreatePipe(&read, &write, &inherit, 0)) return false;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    const HANDLE null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                    OPEN_EXISTING, 0, nullptr);
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    std::array<HANDLE, 2> handles{write, null};
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> storage(size);
    const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool ok = null != INVALID_HANDLE_VALUE && job &&
              SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, static_cast<DWORD>(sizeof limits)) &&
              InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    const bool listed = ok;
    ok = ok && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof handles,
                                         nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null;
    startup.StartupInfo.hStdOutput = write;
    startup.StartupInfo.hStdError = null;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    // Suspended until it is in the job, so nothing it starts can escape the job.
    ok = ok && CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE,
                              CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                              &startup.StartupInfo, &info);
    if (listed) DeleteProcThreadAttributeList(attributes);
    CloseHandle(write);
    if (null != INVALID_HANDLE_VALUE) CloseHandle(null);
    if (ok && !AssignProcessToJobObject(job, info.hProcess)) {
        TerminateProcess(info.hProcess, 1);
        ok = false;
    }
    if (info.hThread) {
        if (ok) ResumeThread(info.hThread);
        CloseHandle(info.hThread);
    }
    if (!ok) {
        if (info.hProcess) CloseHandle(info.hProcess);
        if (job) CloseHandle(job);
        CloseHandle(read);
        return false;
    }
    child = {job, info.hProcess, read};
    return true;
}
// Bytes read, or 0 once the tool has closed its output (or ended).
std::size_t read_some(const Child &child, char *buffer, std::size_t size) {
    DWORD count{};
    return ReadFile(child.out, buffer, static_cast<DWORD>(size), &count, nullptr) ? count : 0;
}
void terminate(const Child &child) {
    if (child.job) TerminateJobObject(child.job, 1);
}
// Waits for the tool to end and returns its exit code; the job stays open for release().
int wait_for(const Child &child) {
    CloseHandle(child.out);
    WaitForSingleObject(child.process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.process, &code);
    CloseHandle(child.process);
    return static_cast<int>(code);
}
void release(const Child &child) { CloseHandle(child.job); } // ends whatever the tool left running
#else
// Its process group (the tool leads it) and the read end of its stdout.
struct Child {
    pid_t pid{};
    int out = -1;
    bool operator==(const Child &) const = default;
};
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
// Its own process group, stdin and stderr closed off, stdout piped back.
bool start(const std::vector<std::string> &args, Child &child) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
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
        return false;
    }
    child = {pid, fds[0]};
    return true;
}
std::size_t read_some(const Child &child, char *buffer, std::size_t size) {
    for (;;) {
        const auto count = read(child.out, buffer, size);
        if (count >= 0) return static_cast<std::size_t>(count);
        if (errno != EINTR) return 0;
    }
}
// SIGKILL, like the job object on Windows: ffmpeg handles one SIGTERM only between reads, so in
// the middle of a reconnect it would keep stop or play (which wait for the worker) on the server's
// main loop for seconds. Its output is thrown away anyway.
void terminate(const Child &child) {
    if (child.pid > 0) ::kill(-child.pid, SIGKILL);
}
int wait_for(const Child &child) {
    close(child.out);
    int status{};
    while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
void release(const Child &) {}
#endif

// yt-dlp never reads its config files (they can run commands). On Windows it would write to a
// pipe in the ANSI code page.
std::vector<std::string> ytdlp(std::initializer_list<std::string> rest) {
    std::vector<std::string> args{"yt-dlp", "--ignore-config", "--no-warnings"};
#ifdef _WIN32
    args.insert(args.end(), {"--encoding", "utf-8"});
#endif
    args.insert(args.end(), rest);
    return args;
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
    bool ytdlp{}, ffmpeg{}; // looked up again on each play: installing a tool needs no restart

    mutable std::mutex mutex;
    std::condition_variable room; // the worker waits here for space in the queue
    std::deque<Item> queue;
    std::deque<std::string> notices;
    std::thread worker;
    bool cancel{}, worker_done{}, active{};
    Child child; // the running tool
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
            terminate(child);
        }
        room.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        queue.clear();
        cancel = worker_done = active = false;
        child = {};
        next_at = 0;
        playing_title.clear();
    }

    bool spawn(const std::vector<std::string> &args, Child &started_child) {
        if (!start(args, started_child)) return false;
        std::lock_guard lock(mutex);
        child = started_child;
        if (cancel) terminate(started_child); // stopped while it started
        return true;
    }
    int reap(const Child &done) {
        const auto code = wait_for(done);
        {
            std::lock_guard lock(mutex);
            if (child == done) child = {};
        }
        release(done);
        return code;
    }
    std::string capture(const std::vector<std::string> &args) {
        Child tool;
        if (!spawn(args, tool)) return {};
        std::string text;
        char buffer[4096];
        for (std::size_t count; (count = read_some(tool, buffer, sizeof buffer)) != 0;)
            if (text.size() < max_listing) text.append(buffer, count);
        return reap(tool) == 0 ? text : std::string{};
    }

    std::vector<Entry> entries(const std::string &input) {
        std::vector<Entry> result;
        if (web_url(input)) {
            if (!ytdlp) return {{input, input}};
            // One line per video ("<url>\t<title>"): a playlist lists them all (the first 500 of a
            // channel), a page lists itself. A single video has no flat "url", only its page's.
            const auto listing = capture(server::ytdlp({"--flat-playlist", "--playlist-end", "500", "--print",
                                                        "%(webpage_url,url)s\t%(title)s", "--", input}));
            for (std::string_view rest = listing; !rest.empty();) {
                const auto end = rest.find('\n');
                const auto line = line_of(rest);
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
        const auto path = utf8_path(input);
        if (fs::is_directory(path, failed)) {
            for (const auto &file : fs::directory_iterator(path, failed))
                if (file.is_regular_file(failed) &&
                    std::find(audio_files.begin(), audio_files.end(), lower(utf8_name(file.path().extension()))) != audio_files.end())
                    result.push_back({utf8_name(file.path()), utf8_name(file.path().stem())});
            std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) { return a.input < b.input; });
        } else {
            result.push_back({input, utf8_name(path.stem())});
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
        // Numbered before yt-dlp resolves it: a skip meanwhile ends the song that is playing, never
        // this one's yt-dlp.
        std::uint32_t track{};
        {
            std::lock_guard lock(mutex);
            track = worker_track = next_track++;
        }
        std::string input = entry.input;
        if (remote && ytdlp) {
            // The page's audio stream: the best audio-only format, or whatever it has.
            const auto direct = capture(server::ytdlp({"--no-playlist", "-f", "bestaudio/best", "-g", "--", entry.input}));
            input = std::string(trim(line_of(direct)));
            if (!web_url(input)) return 0;
        }
        int error{};
        auto *encoder = opus_encoder_create(radio_rate, radio_channels, OPUS_APPLICATION_AUDIO, &error);
        if (!encoder || error != OPUS_OK) return 0;
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
        std::vector<std::string> args{"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"};
        // A source read at playback pace keeps its connection open for the whole song, and YouTube's
        // servers drop such connections after a minute or two: resume at the same byte (a Range
        // request) rather than ending the song there. 15 s without a byte counts as a dropped
        // connection too, so a silent one is resumed (or, on a live stream that cannot resume, ends)
        // instead of hanging the radio. A network that stays down still ends the song after a few
        // seconds of retries. Not reconnect_at_eof (it waits out the backoff at every real end),
        // reconnect_streamed (it restarts a source that cannot seek from its first byte) or
        // reconnect_on_network_error (ffmpeg 4.4 and later only, and it multiplies the retries).
        // HLS segments only get the timeout: ffmpeg does not pass the rest on to them.
        if (remote)
            args.insert(args.end(), {"-reconnect", "1", "-reconnect_delay_max", "5", "-rw_timeout", "15000000"});
        // Remote sources may not reach local files, local ones may not reach the network.
        args.insert(args.end(), {"-protocol_whitelist", remote ? "http,https,tcp,tls,crypto,hls" : "file", "-i",
                                 remote ? input : "file:" + input, "-vn", "-ac", "2", "-ar", std::to_string(radio_rate),
                                 "-f", "s16le", "pipe:1"});
        Child tool;
        const bool running = spawn(args, tool);
        std::size_t queued{};
        if (running) {
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
            for (std::size_t count; open && (count = read_some(tool, buffer, sizeof buffer)) != 0;) {
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
            if (!open) terminate(tool); // skipped or stopped: ffmpeg may still be writing
            reap(tool);
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
            // A skip only ends a song that has already queued frames, so no frames at all is a failure.
            if (!play(entry, remote) && !cancelled())
                notice("Radio: could not play " + entry.title +
                       (remote && !ytdlp ? " (a page like YouTube needs yt-dlp on the server)." : "."));
        }
        std::lock_guard lock(mutex);
        worker_done = true;
    }
};

Radio::Radio(fs::path folder) : state_(std::make_unique<State>()) {
    state_->folder = std::move(folder);
    state_->ffmpeg = installed("ffmpeg");
    state_->ytdlp = installed("yt-dlp");
}
Radio::~Radio() { state_->halt(); }

std::string Radio::play(std::string_view source) {
    auto &s = *state_;
    std::string error;
    const auto input = check_source(source, s.folder, error);
    if (input.empty()) return error;
    const bool ffmpeg = installed("ffmpeg"), ytdlp = installed("yt-dlp");
    if (!ffmpeg) return "The radio needs ffmpeg installed on the server.";
    s.halt();
    {
        std::lock_guard lock(s.mutex);
        s.ffmpeg = ffmpeg;
        s.ytdlp = ytdlp;
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
    if (!s.active) return "Nothing is playing.";
    // Only a song of this session that is playing now: while the first one is still being looked
    // up, or the next one after a skip, there is nothing to skip (and the tool at work belongs to
    // what comes next). The next song may already be buffering.
    if (s.playing_track <= s.skipped) return "Nothing to skip yet.";
    s.skipped = s.playing_track;
    std::erase_if(s.queue, [&](const State::Item &item) { return item.track <= s.skipped; });
    if (s.worker_track <= s.skipped) terminate(s.child);
    s.room.notify_all();
    return "Radio: skipped " + (s.playing_title.empty() ? std::string("the song") : s.playing_title) + ".";
}
std::string Radio::stop() {
    auto &s = *state_;
    {
        std::lock_guard lock(s.mutex);
        if (!s.active) return "Nothing is playing.";
    }
    s.halt();
    return "Radio stopped.";
}
std::string Radio::status() const {
    auto &s = *state_;
    std::lock_guard lock(s.mutex);
    std::string tools = std::string(s.ffmpeg ? "ffmpeg" : "no ffmpeg") + (s.ytdlp ? ", yt-dlp" : ", no yt-dlp");
    if (!s.active) return "Nothing is playing (" + tools + "). Radio folder: " + utf8_name(s.folder);
    const auto seconds = s.frames * frame_us / 1000000;
    const auto kbps = seconds ? s.bytes * 8 / 1000 / seconds : 0;
    // One short line each: an admin reads this in chat, where a long line is cut off.
    return "Radio: " + (s.playing_title.empty() ? std::string("starting") : "playing " + s.playing_title) + "\nFrom " +
           s.source + "\n" + std::to_string(seconds) + " s out, " + std::to_string(kbps) + " kbps, " +
           std::to_string(s.queue.size() * frame_us / 1000) + " ms buffered (" + tools + ")";
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
} // namespace dingosdk::server
