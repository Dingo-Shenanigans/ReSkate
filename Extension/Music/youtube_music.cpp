#include "youtube_music.h"
#include "app_audio_capture.h"
#include "spotify_cover.h"
#include "local_music_ui.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Extension/Boot/user_data_redirect.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <ShlObj.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#if DINGOSDK_SPOTIFY_DEVICE
// SpotifyHelper/src/stream.rs, linked into the DLL (cmake/Runtime.cmake).
extern "C" {
std::uint64_t reskate_stream_play(const char* program, const char* arguments, void (*sound)(const float* samples, std::uint32_t frames, std::uint32_t rate),
    void (*ended)(std::uint64_t stream, const char* how));
std::uint64_t reskate_stream_play_file(const char* path, void (*sound)(const float* samples, std::uint32_t frames, std::uint32_t rate), void (*ended)(std::uint64_t stream, const char* how));
std::uint64_t reskate_stream_play_url(const char* address, void (*sound)(const float* samples, std::uint32_t frames, std::uint32_t rate), void (*ended)(std::uint64_t stream, const char* how),
    void (*titled)(std::uint64_t stream, const char* title));
std::uint32_t reskate_file_about(const char* path, char* artist, char* title, std::uint32_t text_room, std::uint8_t* picture, std::uint32_t picture_room);
void reskate_stream_stop();
}
#endif

namespace dingosdk::youtube_music {
namespace {
constexpr wchar_t agent[] = L"ReSkate";
constexpr wchar_t tool_address[] = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
constexpr wchar_t runtime_address[] = L"https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-windows-x86_64.exe";
constexpr wchar_t sums_address[] = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS";
constexpr std::size_t most_songs = 100, most_files = 200, most_kept = 48, most_found = 12;
// How many playlists of a kind there is room for.
constexpr std::size_t room(Kind kind) { return kind == Kind::radio ? 24 : 12; }
constexpr const char* kind_name(Kind kind) { return kind == Kind::files ? "files" : kind == Kind::radio ? "radio" : "youtube"; }

std::mutex mutex; // everything below it
std::condition_variable wake;
std::deque<std::string> commands; // "tools", "add <link>", "covers"
std::vector<Playlist> library;
std::string problem_text, browser_name;
Kind problem_of{};
std::vector<Found> found_stations; // of the last search for radio stations
std::string searched_for;
bool is_busy{}, have_tools{}, have_cookies{};
std::atomic<bool> started{}, is_on{}, paused{}, say_notices{true};
std::atomic<unsigned> sound_rate{44100}, fetched_percent{};
https::Watch tool_watch;
// What is playing.
std::size_t playing_list{}, playing_song{};
std::uint64_t playing_stream{}, begun_number{};
int failures{};
external_player::Track track;
std::atomic<std::uint64_t> changes{1}, wanted_stream{}, begun_at{}, heard_stream{};

void say(logging::Level level, const std::string& text) noexcept {
    try {
        logging::log(level, logging::Channel::music, "YouTube Music: {}", text);
    } catch (...) {}
}
void problem(Kind kind, const std::string& text) {
    {
        std::lock_guard lock(mutex);
        problem_text = text;
        problem_of = kind;
    }
    if (!text.empty()) say(logging::Level::warning, text);
}
void problem(const std::string& text) { problem(Kind::youtube, text); }
// (With the mutex held.)
std::size_t count_of(Kind kind) {
    return static_cast<std::size_t>(std::count_if(library.begin(), library.end(), [&](const Playlist& playlist) { return playlist.kind == kind; }));
}
std::filesystem::path home() { return redirected_local_app_data().parent_path(); }
std::filesystem::path tool() { return home() / L"bin" / L"yt-dlp.exe"; }
std::filesystem::path cookies_file() { return home() / L"youtube-cookies.txt"; }
std::filesystem::path kept_file() { return home() / L"youtube-music.json"; }
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    std::string out(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}
// A video's id is eleven letters, digits, '-' and '_'.
bool video_id(const std::string& id) {
    return id.size() == 11 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}
// Text the music screen can show as it is: no control characters, and not over long.
std::string shown(std::string text) {
    std::erase_if(text, [](unsigned char c) { return c < 0x20; });
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (text.size() > 120) {
        text.resize(120);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back(); // (not in the middle of a character)
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0) text.pop_back();
    }
    return text;
}
// The id of a song that is not a video: made of where it is.
std::string made_id(const std::string& source) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : source) hash = (hash ^ c) * 1099511628211ull;
    return std::format("{:016x}", hash);
}
// An address on the web, with nothing in it but what one is written with.
bool good_address(const std::string& address) {
    return (address.starts_with("http://") || address.starts_with("https://")) && address.size() > 10 && address.size() <= 400 &&
        std::none_of(address.begin(), address.end(), [](unsigned char c) { return c <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c >= 0x7f; });
}

// A JavaScript runtime for yt-dlp, which YouTube's pages need worked out when they are asked
// for as a signed-in player (without one, every song fails once there is a sign-in: seen
// 2026-10-12). Node.js when it is installed, or Deno in ReSkate's bin folder, as they are the
// quicker (a song begins in about four seconds); else QuickJS, which this fetches into the bin
// folder itself (about seven seconds: both timed 2026-10-12). The name yt-dlp knows it by and
// where it is; empty when there is none.
std::filesystem::path quickjs() { return home() / L"bin" / L"qjs.exe"; }
std::pair<std::string, std::string> javascript() {
    std::error_code ignored;
    wchar_t path[MAX_PATH]{};
    if (SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, path, nullptr)) return {"node", narrow(path)};
    if (const auto deno = home() / L"bin" / L"deno.exe"; std::filesystem::exists(deno, ignored)) return {"deno", narrow(deno.wstring())};
    if (std::filesystem::exists(quickjs(), ignored)) return {"quickjs", narrow(quickjs().wstring())};
    return {};
}
// (With the mutex held.) What yt-dlp is run with for the player's YouTube sign-in: their
// cookies.txt when one is kept, else the browser they picked, else nothing.
std::vector<std::string> sign_in() {
    std::vector<std::string> arguments;
    if (!have_cookies && browser_name.empty()) return arguments; // (no sign-in: yt-dlp asks YouTube in a way that needs no runtime, and is quicker)
    if (const auto [name, path] = javascript(); !name.empty()) arguments = {"--no-js-runtimes", "--js-runtimes", name + ":" + path};
    if (have_cookies) arguments.insert(arguments.end(), {"--cookies", narrow(cookies_file().wstring())});
    else arguments.insert(arguments.end(), {"--cookies-from-browser", browser_name});
    return arguments;
}
bool good_browser(const std::string& browser) {
    for (const char* known : {"", "firefox", "edge", "chrome", "brave", "opera", "vivaldi"})
        if (browser == known) return true;
    return false;
}

void keep() {
    try {
        Json lists = Json::array();
        {
            std::lock_guard lock(mutex);
            for (const auto& playlist : library) {
                Json songs = Json::array();
                for (const auto& song : playlist.songs) {
                    Json entry = Json::object();
                    entry["id"] = song.id;
                    entry["artist"] = song.artist;
                    entry["title"] = song.title;
                    if (!song.source.empty()) entry["source"] = song.source;
                    songs.push_back(std::move(entry));
                }
                Json entry = Json::object();
                entry["name"] = playlist.name;
                entry["url"] = playlist.url;
                entry["kind"] = kind_name(playlist.kind);
                entry["songs"] = std::move(songs);
                lists.push_back(std::move(entry));
            }
        }
        Json json = Json::object();
        json["playlists"] = std::move(lists);
        {
            std::lock_guard lock(mutex);
            json["browser"] = browser_name;
        }
        std::error_code ignored;
        std::filesystem::create_directories(home(), ignored);
        std::ofstream(kept_file(), std::ios::binary | std::ios::trunc) << json.dump(2);
    } catch (...) {}
}
void read_kept() {
    try {
        std::ifstream in(kept_file(), std::ios::binary);
        if (!in) return;
        const auto json = Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        if (const auto browser = json.value("browser", ""); good_browser(browser)) {
            std::lock_guard lock(mutex);
            browser_name = browser;
        }
        if (!json.contains("playlists") || !json.at("playlists").is_array()) return;
        std::vector<Playlist> read;
        const auto& lists = json.at("playlists");
        for (std::size_t i = 0; i < lists.size() && read.size() < most_kept; ++i) {
            const auto& list = lists.at(i);
            if (!list.is_object() || !list.contains("songs") || !list.at("songs").is_array()) continue;
            const auto kind_text = list.value("kind", "youtube");
            const Kind kind = kind_text == "files" ? Kind::files : kind_text == "radio" ? Kind::radio : Kind::youtube;
            Playlist playlist{shown(list.value("name", "")), list.value("url", ""), {}, {}, kind};
            const auto& songs = list.at("songs");
            for (std::size_t s = 0; s < songs.size() && playlist.songs.size() < most_files; ++s) {
                if (!songs.at(s).is_object()) continue;
                Song song{songs.at(s).value("id", ""), shown(songs.at(s).value("artist", "")), shown(songs.at(s).value("title", "")), songs.at(s).value("source", "")};
                const bool good = kind == Kind::youtube ? video_id(song.id) : !song.id.empty() && (kind == Kind::radio ? good_address(song.source) : !song.source.empty());
                if (good && !song.artist.empty() && !song.title.empty()) playlist.songs.push_back(std::move(song));
            }
            const auto same = static_cast<std::size_t>(std::count_if(read.begin(), read.end(), [&](const Playlist& other) { return other.kind == kind; }));
            if (!playlist.name.empty() && !playlist.songs.empty() && same < room(kind)) read.push_back(std::move(playlist));
        }
        std::lock_guard lock(mutex);
        library = std::move(read);
    } catch (...) {}
}

// Runs yt-dlp and gives what it wrote, or nothing when it could not be run or took too long.
std::optional<std::string> run_tool(const std::vector<std::string>& arguments, DWORD milliseconds) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read{}, write{};
    if (!CreatePipe(&read, &write, &inherit, 0)) return std::nullopt;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + tool().wstring() + L"\"";
    for (const auto& argument : arguments) {
        // (None of what is passed has a quote in it: a link is checked before it is.)
        command += L" \"" + wide(argument) + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    const auto nothing = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    startup.hStdInput = startup.hStdError = nothing;
    PROCESS_INFORMATION process{};
    const bool made = CreateProcessW(tool().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0;
    CloseHandle(write);
    if (nothing != INVALID_HANDLE_VALUE) CloseHandle(nothing);
    if (!made) {
        CloseHandle(read);
        return std::nullopt;
    }
    // (Taken out of the way if it takes too long: its pipe closing ends the reading below.)
    std::thread([process = process.hProcess, milliseconds] {
        if (WaitForSingleObject(process, milliseconds) == WAIT_TIMEOUT) TerminateProcess(process, 1);
        CloseHandle(process);
    }).detach();
    CloseHandle(process.hThread);
    std::string output;
    char buffer[8192];
    for (DWORD got{}; ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got && output.size() < 16 * 1024 * 1024;) output.append(buffer, got);
    CloseHandle(read);
    return output;
}

// A song's artist and title out of what yt-dlp says of a video: a title of the form
// "artist - title" is taken apart, and otherwise the channel is the artist.
Song song_of(const Json& entry) {
    Song song{entry.value("id", ""), {}, {}};
    auto title = entry.value("title", "");
    auto channel = entry.contains("channel") && entry.at("channel").is_string() ? entry.at("channel").string() : entry.contains("uploader") && entry.at("uploader").is_string() ? entry.at("uploader").string() : std::string();
    if (channel.ends_with(" - Topic")) channel.resize(channel.size() - 8);
    if (const auto dash = title.find(" - "); dash != std::string::npos && dash > 0 && dash + 3 < title.size()) {
        song.artist = shown(title.substr(0, dash));
        song.title = shown(title.substr(dash + 3));
    } else {
        song.artist = shown(channel.empty() ? std::string("YouTube") : channel);
        song.title = shown(title);
    }
    return song;
}

// A link is one of YouTube's, and has nothing in it that a command line would take amiss.
bool good_link(const std::string& link) {
    if (link.size() > 300 || std::any_of(link.begin(), link.end(), [](unsigned char c) { return c <= 0x20 || c == '"' || c == '\\' || c == '^' || c == '%' || c >= 0x7f; })) return false;
    for (const char* start : {"https://www.youtube.com/", "https://youtube.com/", "https://music.youtube.com/", "https://m.youtube.com/", "https://youtu.be/"})
        if (link.starts_with(start)) return true;
    return false;
}

void add_link(const std::string& link) {
    if (!good_link(link)) return problem("that is not a link to a YouTube video or playlist");
    std::vector<std::string> arguments{"--flat-playlist", "-J", "--no-warnings"};
    {
        std::lock_guard lock(mutex);
        for (auto& argument : sign_in()) arguments.push_back(std::move(argument));
    }
    arguments.push_back(link);
    const auto output = run_tool(arguments, 90000);
    if (!output || output->empty()) return problem("yt-dlp could not read that link");
    Playlist playlist{{}, link, {}, {}};
    try {
        const auto json = Json::parse(*output);
        playlist.name = shown(json.value("title", ""));
        if (json.contains("entries") && json.at("entries").is_array()) {
            const auto& entries = json.at("entries");
            for (std::size_t i = 0; i < entries.size() && playlist.songs.size() < most_songs; ++i)
                if (entries.at(i).is_object())
                    if (auto song = song_of(entries.at(i)); video_id(song.id) && !song.artist.empty() && !song.title.empty() &&
                        std::none_of(playlist.songs.begin(), playlist.songs.end(), [&](const Song& other) { return other.artist == song.artist && other.title == song.title; }))
                        playlist.songs.push_back(std::move(song));
        } else if (auto song = song_of(json); video_id(song.id) && !song.artist.empty() && !song.title.empty()) {
            playlist.songs.push_back(std::move(song));
        }
    } catch (...) {
        return problem("what yt-dlp said of that link could not be read");
    }
    if (playlist.name.empty() || playlist.songs.empty()) return problem("there is nothing to play at that link");
    {
        std::lock_guard lock(mutex);
        if (count_of(Kind::youtube) >= room(Kind::youtube) || library.size() >= most_kept) {
            problem_text = "there is room for no more than twelve";
            problem_of = Kind::youtube;
            return;
        }
        library.push_back(playlist);
        problem_text.clear();
    }
    keep();
    say(logging::Level::info, std::format("added \"{}\" ({} song(s)).", playlist.name, playlist.songs.size()));
}

// --- the player's own files ---

bool sound_file(const std::filesystem::path& file) {
    auto extension = file.extension().wstring();
    for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* known : {L".mp3", L".m4a", L".aac", L".ogg", L".oga", L".flac", L".wav"})
        if (extension == known) return true;
    return false;
}
// A file as a song: what its tags say, and for one that says nothing its name, which is
// taken apart when it is of the form "artist - title" (a track's number before it is left out).
Song file_song(const std::filesystem::path& file, const std::string& folder) {
    Song song;
    song.source = narrow(file.wstring());
    song.id = made_id(song.source);
#if DINGOSDK_SPOTIFY_DEVICE
    char artist[256]{}, title[256]{};
    reskate_file_about(song.source.c_str(), artist, title, sizeof(artist), nullptr, 0);
    song.artist = shown(artist);
    song.title = shown(title);
#endif
    if (song.title.empty()) {
        auto name = narrow(file.stem().wstring());
        std::size_t digits{};
        while (digits < name.size() && digits < 3 && std::isdigit(static_cast<unsigned char>(name[digits]))) ++digits;
        if (digits && digits < name.size() && (name[digits] == '.' || name[digits] == '-' || name[digits] == '_' || (name[digits] == ' ' && (name[0] == '0' || name.find(" - ") == digits)))) {
            auto rest = name.find_first_not_of(" .-_", digits);
            if (rest != std::string::npos) name.erase(0, rest);
        }
        if (const auto dash = name.find(" - "); dash != std::string::npos && dash > 0 && dash + 3 < name.size()) {
            if (song.artist.empty()) song.artist = shown(name.substr(0, dash));
            song.title = shown(name.substr(dash + 3));
        } else {
            song.title = shown(name);
        }
    }
    if (song.artist.empty()) song.artist = folder;
    return song;
}
void add_folder_now(std::string given) {
    std::erase(given, '"');
    while (!given.empty() && given.back() == ' ') given.pop_back();
    while (!given.empty() && given.front() == ' ') given.erase(given.begin());
    std::filesystem::path folder;
    if (given.empty()) {
        // (None given: the player's Music folder.)
        PWSTR known{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &known)) && known) folder = known;
        CoTaskMemFree(known);
    } else {
        folder = wide(given);
    }
    std::error_code failed;
    if (folder.empty() || !folder.is_absolute() || !std::filesystem::is_directory(folder, failed))
        return problem(Kind::files, "that is not a folder on this computer: give its whole path, such as C:\\Users\\you\\Music");
    folder = folder.lexically_normal();
    if (!folder.has_filename()) folder = folder.parent_path(); // (it was written with a slash at its end)
    std::vector<std::filesystem::path> files;
    std::filesystem::recursive_directory_iterator walk(folder, std::filesystem::directory_options::skip_permission_denied, failed);
    for (const std::filesystem::recursive_directory_iterator end; !failed && walk != end && files.size() < 5000; walk.increment(failed)) {
        if (walk.depth() >= 4) walk.disable_recursion_pending();
        std::error_code ignored;
        if (walk->is_regular_file(ignored) && sound_file(walk->path())) files.push_back(walk->path());
    }
    std::sort(files.begin(), files.end());
    Playlist playlist{shown(narrow(folder.filename().wstring())), narrow(folder.wstring()), {}, {}, Kind::files};
    if (playlist.name.empty()) playlist.name = "Local Files";
    for (const auto& file : files) {
        if (playlist.songs.size() >= most_files) break;
        if (auto song = file_song(file, playlist.name); !song.artist.empty() && !song.title.empty() &&
            std::none_of(playlist.songs.begin(), playlist.songs.end(), [&](const Song& other) { return other.artist == song.artist && other.title == song.title; }))
            playlist.songs.push_back(std::move(song));
    }
    if (playlist.songs.empty()) return problem(Kind::files, "there is no music this can play in that folder (MP3, M4A, AAC, Ogg, FLAC, WAV)");
    {
        std::lock_guard lock(mutex);
        // (A folder that is there already is read again: that is how new files in it are taken in.)
        const auto same = std::find_if(library.begin(), library.end(), [&](const Playlist& other) { return other.kind == Kind::files && _stricmp(other.url.c_str(), playlist.url.c_str()) == 0; });
        if (same != library.end()) {
            *same = playlist;
        } else if (count_of(Kind::files) >= room(Kind::files) || library.size() >= most_kept) {
            problem_text = "there is room for no more than twelve folders";
            problem_of = Kind::files;
            return;
        } else {
            library.push_back(playlist);
        }
        problem_text.clear();
    }
    keep();
    say(logging::Level::info, std::format("added the folder \"{}\" ({} of its {} file(s)).", playlist.name, playlist.songs.size(), files.size()));
}

// --- radio ---

std::string encoded(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}
// Stations by their name, from the radio-browser.info directory (a list kept by its
// community, free to ask), the most listened to first; those this can play.
void search_now(const std::string& asked) {
    auto query = shown(asked);
    if (query.size() > 60) query.resize(60);
    if (query.size() < 2) return problem(Kind::radio, "type at least two letters of a station's name");
    std::optional<std::string> text;
    for (const wchar_t* server : {L"de1", L"de2"}) {
        text = https::get_text(std::wstring(L"https://") + server + L".api.radio-browser.info/json/stations/search?limit=40&hidebroken=true&order=clickcount&reverse=true&name=" + wide(encoded(query)),
            1024 * 1024, 15, agent);
        if (text) break;
    }
    if (!text) return problem(Kind::radio, "the station directory (radio-browser.info) could not be reached");
    std::vector<Found> found;
    try {
        const auto list = Json::parse(*text);
        for (std::size_t i = 0; list.is_array() && i < list.size() && found.size() < most_found; ++i) {
            try {
                const auto& entry = list.at(i);
                if (!entry.is_object()) continue;
                // (Sound sent in pieces over many addresses, HLS, is not played here.)
                if (entry.contains("hls") && entry.at("hls").is_number() && entry.at("hls").get<std::int64_t>() != 0) continue;
                const auto codec = entry.value("codec", "");
                if (codec != "MP3" && codec != "AAC" && codec != "AAC+" && codec != "OGG") continue;
                auto address = entry.value("url_resolved", "");
                if (!good_address(address)) address = entry.value("url", "");
                Found station{shown(entry.value("name", "")), address, entry.value("favicon", ""), {}};
                if (!good_address(station.address) || station.name.empty()) continue;
                if (!good_address(station.icon) || !station.icon.starts_with("https://")) station.icon.clear();
                if (std::any_of(found.begin(), found.end(), [&](const Found& other) { return other.address == station.address || other.name == station.name; })) continue;
                station.about = shown(entry.value("country", ""));
                station.about += (station.about.empty() ? "" : ", ") + codec;
                if (entry.contains("bitrate") && entry.at("bitrate").is_number() && entry.at("bitrate").get<std::int64_t>() > 0)
                    station.about += " " + std::to_string(entry.at("bitrate").get<std::int64_t>()) + " kbps";
                found.push_back(std::move(station));
            } catch (...) {} // (one that is not as the others are is passed over)
        }
    } catch (...) {
        return problem(Kind::radio, "what the station directory said could not be read");
    }
    std::lock_guard lock(mutex);
    problem_of = Kind::radio;
    problem_text = found.empty() ? "no station of that name that this can play was found" : "";
    found_stations = std::move(found);
    searched_for = query;
}
// A station is a playlist of one row, which is the station.
void add_station_now(const std::string& told) {
    std::string name, address, icon;
    try {
        const auto json = Json::parse(told);
        name = shown(json.value("name", ""));
        address = json.value("address", "");
        icon = json.value("icon", "");
    } catch (...) {
        return;
    }
    if (!good_address(address)) return problem(Kind::radio, "that is not a station's address: one begins with http:// or https://");
    if (!good_address(icon) || !icon.starts_with("https://")) icon.clear();
    if (name.empty()) {
        // (None given: the name of where it is.)
        const auto from = address.find("//") + 2;
        name = address.substr(from, address.find_first_of("/:?", from) - from);
    }
    Playlist playlist{name, icon, {}, {Song{made_id(address), name, "Live Radio", address}}, Kind::radio};
    {
        std::lock_guard lock(mutex);
        problem_of = Kind::radio;
        if (std::any_of(library.begin(), library.end(), [&](const Playlist& other) { return other.kind == Kind::radio && !other.songs.empty() && other.songs.front().source == address; })) {
            problem_text = "that station is there already";
            return;
        }
        if (count_of(Kind::radio) >= room(Kind::radio) || library.size() >= most_kept) {
            problem_text = "there is room for no more than twenty-four stations";
            return;
        }
        library.push_back(playlist);
        problem_text.clear();
    }
    keep();
    say(logging::Level::info, std::format("added the station \"{}\".", name));
}

// --- pictures ---

std::vector<std::uint8_t> fetched(const std::wstring& address) {
    const auto image = https::get_text(address, 2 * 1024 * 1024, 10, agent);
    return image ? std::vector<std::uint8_t>(image->begin(), image->end()) : std::vector<std::uint8_t>{};
}
// The picture of a song, for its playlist's tile or for what is playing: its video's, the one
// in its file's tags, or its station's.
std::vector<std::uint8_t> picture(Kind kind, const Song& song, const std::string& icon) {
    if (kind == Kind::youtube) return fetched(L"https://i.ytimg.com/vi/" + wide(song.id) + L"/hqdefault.jpg");
    if (kind == Kind::radio) return icon.starts_with("https://") ? fetched(wide(icon)) : std::vector<std::uint8_t>{};
#if DINGOSDK_SPOTIFY_DEVICE
    std::vector<std::uint8_t> image(4 * 1024 * 1024);
    char artist[8]{}, title[8]{};
    image.resize(reskate_file_about(song.source.c_str(), artist, title, sizeof(artist), image.data(), static_cast<std::uint32_t>(image.size())));
    return image;
#else
    return {};
#endif
}
// Each playlist's tile is its first song's picture (of a folder, the first of its first few
// files that has one). Returns how many tiles got one, and (`missing`) how many still have none.
std::size_t make_covers(std::size_t* missing = nullptr) {
    struct Wanted {
        std::size_t index;
        Kind kind;
        std::string icon;
        std::vector<Song> songs;
    };
    std::vector<Wanted> wanted;
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < library.size(); ++i) {
            const auto& playlist = library[i];
            if (!playlist.artwork.empty() || playlist.songs.empty()) continue;
            const auto count = std::min<std::size_t>(playlist.songs.size(), playlist.kind == Kind::files ? 4 : 1);
            wanted.push_back({i, playlist.kind, playlist.url, {playlist.songs.begin(), playlist.songs.begin() + static_cast<std::ptrdiff_t>(count)}});
        }
    }
    std::size_t made{};
    for (const auto& want : wanted)
        for (const auto& song : want.songs) {
            const auto image = picture(want.kind, song, want.icon);
            if (image.empty()) continue;
            const auto address = spotify_cover::serve(image, std::string(kind_name(want.kind)) + "-" + song.id + ".png");
            if (address.empty()) continue;
            std::lock_guard lock(mutex);
            if (want.index < library.size() && !library[want.index].songs.empty() && library[want.index].songs.front().id == want.songs.front().id) {
                library[want.index].artwork = address;
                ++made;
            }
            break;
        }
    if (missing) *missing = wanted.size() - std::min(made, wanted.size());
    return made;
}

std::string hex_sha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    unsigned char digest[32]{};
    if (bytes.empty() || BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) != 0) return {};
    std::string hex;
    for (const auto byte : digest) hex += std::format("{:02x}", byte);
    return hex;
}
// yt-dlp, from its own releases, checked against the sums published beside it.
void fetch_tool() {
    std::error_code ignored;
    std::filesystem::create_directories(tool().parent_path(), ignored);
    const auto fetched = tool().parent_path() / L"yt-dlp.fetching";
    std::filesystem::remove(fetched, ignored);
    tool_watch.received.store(0);
    tool_watch.total.store(0);
    std::atomic<bool> fetching{true};
    std::thread progress([&] {
        while (fetching.load()) {
            const auto total = tool_watch.total.load();
            fetched_percent.store(total ? static_cast<unsigned>(tool_watch.received.load() * 100 / total) : 0);
            Sleep(200);
        }
    });
    const auto result = https::get(tool_address, fetched, 96ull * 1024 * 1024, 600, agent, &tool_watch);
    fetching.store(false);
    progress.join();
    fetched_percent.store(0);
    if (!result.ok) {
        std::filesystem::remove(fetched, ignored);
        return problem(std::format("yt-dlp could not be fetched ({}, {})", result.http_status, result.error));
    }
    const auto sums = https::get_text(sums_address, 64 * 1024, 30, agent);
    const auto sum = hex_sha256(fetched);
    if (!sums || sum.empty() || sums->find(sum + "  yt-dlp.exe") == std::string::npos) {
        std::filesystem::remove(fetched, ignored);
        return problem("what was fetched is not the yt-dlp its releases say it is: it was not kept");
    }
    std::filesystem::remove(tool(), ignored);
    std::filesystem::rename(fetched, tool(), ignored);
    const bool there = std::filesystem::exists(tool(), ignored);
    {
        std::lock_guard lock(mutex);
        have_tools = there;
        if (there) problem_text.clear();
    }
    if (there) say(logging::Level::info, "yt-dlp was fetched.");
    else problem("yt-dlp could not be put in its place");
}

// QuickJS, from its own releases, when there is no runtime for yt-dlp on this computer.
void fetch_runtime() {
    if (!javascript().first.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(quickjs().parent_path(), ignored);
    const auto fetched = quickjs().parent_path() / L"qjs.fetching";
    std::filesystem::remove(fetched, ignored);
    const auto result = https::get(runtime_address, fetched, 32ull * 1024 * 1024, 300, agent);
    char start[2]{};
    if (result.ok) std::ifstream(fetched, std::ios::binary).read(start, 2);
    // (A Windows program, and of a size a runtime is.)
    if (!result.ok || start[0] != 'M' || start[1] != 'Z' || std::filesystem::file_size(fetched, ignored) < 256 * 1024) {
        std::filesystem::remove(fetched, ignored);
        return problem("the JavaScript runtime a YouTube sign-in needs (QuickJS) could not be fetched; Node.js, when it is installed, does as well");
    }
    std::filesystem::rename(fetched, quickjs(), ignored);
    say(logging::Level::info, "QuickJS was fetched, for songs that need the YouTube sign-in.");
}

void run() noexcept {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // (pictures are converted on this thread)
    read_kept();
    {
        std::error_code ignored;
        std::lock_guard lock(mutex);
        have_tools = std::filesystem::exists(tool(), ignored);
        have_cookies = std::filesystem::exists(cookies_file(), ignored);
    }
    try {
        bool signed_in{};
        {
            std::lock_guard lock(mutex);
            signed_in = have_tools && (have_cookies || !browser_name.empty());
        }
        if (signed_in) fetch_runtime(); // (a sign-in set by a ReSkate that did not fetch one)
    } catch (...) {}
    // Tiles whose picture could not be fetched (YouTube's are fetched from i.ytimg.com) are tried
    // again, sooner then later: fetched once at start, a game started while the connection was
    // down kept its YouTube playlists without pictures until a command (2026-10-15).
    std::size_t missing{};
    try { make_covers(&missing); } catch (...) {}
    auto retry_after = std::chrono::seconds(30);
    for (;;) {
        std::string command;
        {
            std::unique_lock lock(mutex);
            is_busy = false;
            const bool asked = missing ? wake.wait_for(lock, retry_after, [] { return !commands.empty(); })
                                       : (wake.wait(lock, [] { return !commands.empty(); }), true);
            if (!asked) {
                is_busy = true;
                lock.unlock();
                std::size_t made{};
                try { made = make_covers(&missing); } catch (...) {}
                if (made) {
                    profile_runtime::request_music_catalog_refresh();
                    retry_after = std::chrono::seconds(30);
                } else {
                    retry_after = std::min<std::chrono::seconds>(retry_after * 2, std::chrono::minutes(10));
                }
                continue;
            }
            command = std::move(commands.front());
            commands.pop_front();
            is_busy = true;
        }
        try {
            if (command == "tools") {
                fetch_tool();
                fetch_runtime();
            } else if (command == "runtime") fetch_runtime();
            if (command.starts_with("add ")) add_link(command.substr(4));
            else if (command.starts_with("folder ")) add_folder_now(command.substr(7));
            else if (command.starts_with("station ")) add_station_now(command.substr(8));
            else if (command.starts_with("search ")) {
                search_now(command.substr(7));
                continue; // (nothing of the music screen's has changed)
            }
            make_covers(&missing);
            profile_runtime::request_music_catalog_refresh();
        } catch (...) {}
    }
    if (initialized) CoUninitialize();
}
void ask(std::string command) {
    start();
    {
        std::lock_guard lock(mutex);
        if (commands.size() < 8) commands.push_back(std::move(command));
    }
    wake.notify_one();
}

// --- playing ---

// The sound of the song that is playing, on the decoder's thread. It is taken as fast as the
// game hears it; while the player has it paused, none is taken.
void sound(const float* samples, std::uint32_t frames, std::uint32_t rate) {
    const auto mine = wanted_stream.load(std::memory_order_relaxed);
    sound_rate.store(rate, std::memory_order_relaxed);
    // (The first of a song's sound: how long it was in coming, into the log.)
    if (mine && heard_stream.exchange(mine, std::memory_order_relaxed) != mine)
        say(logging::Level::debug, std::format("the song's sound began {:.1f} s after it was asked for.", static_cast<double>(GetTickCount64() - begun_at.load(std::memory_order_relaxed)) / 1000.0));
    for (int waited = 0; paused.load(std::memory_order_relaxed) || app_audio::ahead_frames() > rate * 15 / 100; ++waited) {
        if (wanted_stream.load(std::memory_order_relaxed) != mine) return; // (another song is wanted)
        if (!paused.load(std::memory_order_relaxed) && waited >= 125) return; // (the game is not listening)
        Sleep(4);
    }
    app_audio::push(samples, frames);
}
void begin(std::size_t list, std::size_t song, bool asked = false);
// Whether a song has been asked for whose sound has not begun yet. (With the mutex held.)
bool loading_now() {
    return playing_stream && heard_stream.load(std::memory_order_relaxed) != playing_stream && !paused.load() &&
        GetTickCount64() - begun_at.load(std::memory_order_relaxed) < 20000;
}
// A song is over: the next of its playlist, and round again after its last. A station that
// went quiet is turned to again.
void ended(std::uint64_t stream, const char* how) {
    try {
        std::size_t list{}, next{};
        {
            std::lock_guard lock(mutex);
            if (stream != playing_stream) return; // (it was stopped for another)
            list = playing_list;
            if (list >= library.size() || library[list].songs.empty()) return;
            const auto kind = library[list].kind;
            const std::string_view said(how ? how : "");
            bool failed = said.starts_with("failed");
            if (failed) {
                const auto& title = kind == Kind::radio ? library[list].name : library[list].songs[playing_song % library[list].songs.size()].title;
                say(logging::Level::warning, std::format("\"{}\" {}.", title, said));
                if (say_notices.load(std::memory_order_relaxed)) overlay::notify(overlay::NoticeLevel::warning, kind == Kind::radio ? "Station could not be played" : "Song could not be played, skipping", title);
                problem_of = kind;
                // (The usual reason, of YouTube's: it wants a sign-in before it plays it.)
                problem_text = kind == Kind::youtube ? "\"" + title + "\" could not be played and was passed over. YouTube asks for a sign-in for private and age-restricted songs: add your cookies.txt below."
                    : kind == Kind::files ? "\"" + title + "\" could not be played and was passed over: the file is gone, or is of a kind this does not play."
                    : "\"" + title + "\" could not be played: " + std::string(said.substr(std::min<std::size_t>(said.size(), 7))) + ".";
            }
            if (kind == Kind::radio) {
                // (One that ends as soon as it is turned to is not turned to over and over.)
                if (!failed && GetTickCount64() - begun_at.load(std::memory_order_relaxed) < 10000) failed = true;
                playing_stream = 0;
                if (failed && (said.starts_with("failed") || failures >= 3)) {
                    track.playing = false;
                    changes.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            failures = failed ? failures + 1 : 0;
            if (failures >= 4) {
                problem_of = kind;
                problem_text = kind == Kind::youtube ? "songs are not playing (yt-dlp may need to be fetched again, or YouTube is not letting them be)" : "songs are not playing";
                return;
            }
            next = (playing_song + 1) % library[list].songs.size();
        }
        begin(list, next);
    } catch (...) {}
}
// What a station says is playing ("artist - title", usually).
void titled(std::uint64_t stream, const char* title) {
    try {
        const auto text = shown(title ? title : "");
        if (text.empty()) return;
        std::lock_guard lock(mutex);
        if (stream != playing_stream || playing_list >= library.size()) return;
        if (const auto dash = text.find(" - "); dash != std::string::npos && dash > 0 && dash + 3 < text.size()) {
            track.artist = text.substr(0, dash);
            track.title = text.substr(dash + 3);
        } else {
            track.artist = library[playing_list].name;
            track.title = text;
        }
        ++track.revision;
        ++track.artwork_revision; // (the station's picture is the new song's as well)
        changes.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
}
void fetch_picture(Kind kind, Song song, std::string icon, std::uint64_t of) {
    try {
        auto image = picture(kind, song, icon);
        if (image.empty()) return;
        std::lock_guard lock(mutex);
        if (begun_number != of) return;
        track.artwork = std::move(image);
        ++track.artwork_revision;
        changes.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
}
void begin(std::size_t list, std::size_t song, bool asked) {
#if DINGOSDK_SPOTIFY_DEVICE
    Song chosen;
    Kind kind{};
    std::string icon;
    std::uint64_t of{};
    {
        std::lock_guard lock(mutex);
        if (list >= library.size() || library[list].songs.empty()) return;
        kind = library[list].kind;
        if (kind == Kind::youtube && !have_tools) return;
        song %= library[list].songs.size();
        if (playing_stream && list == playing_list && song == playing_song && GetTickCount64() - begun_at.load(std::memory_order_relaxed) < 3000) return;
        // Presses that come fast, while a song is still on its way: the same song again is
        // not begun again (it would only be longer in coming), and another is not taken up
        // until the one before has had a moment (each is a program run and a fetch begun).
        if (asked && loading_now() && ((list == playing_list && song == playing_song) || GetTickCount64() - begun_at.load(std::memory_order_relaxed) < 1200)) return;
        begun_at.store(GetTickCount64(), std::memory_order_relaxed);
        chosen = library[list].songs[song];
        icon = library[list].url;
        playing_list = list;
        playing_song = song;
        track.available = true;
        track.spotify = false;
        track.app = kind == Kind::youtube ? "YouTube Music" : kind == Kind::files ? "Local Files" : "Online Radio";
        track.can_next = track.can_previous = track.can_pause = track.can_play = true;
        track.artist = chosen.artist;
        track.title = chosen.title;
        track.album.clear();
        track.artwork.clear();
        track.playing = true;
        ++track.revision;
        of = ++begun_number;
        paused.store(false);
        // (The number the stream will have is known before it is begun: its sound is asked for it.)
        if (kind == Kind::files) {
            playing_stream = reskate_stream_play_file(chosen.source.c_str(), &sound, &ended);
        } else if (kind == Kind::radio) {
            playing_stream = reskate_stream_play_url(chosen.source.c_str(), &sound, &ended, &titled);
        } else {
            const auto program = narrow(tool().wstring());
            std::string arguments = "-f\n140/bestaudio[ext=m4a]\n--no-playlist\n--quiet\n--no-warnings\n";
            for (const auto& argument : sign_in()) arguments += argument + "\n";
            arguments += "-o\n-\nhttps://www.youtube.com/watch?v=" + chosen.id;
            playing_stream = reskate_stream_play(program.c_str(), arguments.c_str(), &sound, &ended);
        }
        wanted_stream.store(playing_stream, std::memory_order_relaxed);
    }
    changes.fetch_add(1, std::memory_order_relaxed);
    // (Said in the corner of the game, for a song that takes a moment to come: the player
    // sees that the press was taken.)
    if (asked && kind != Kind::files && say_notices.load(std::memory_order_relaxed)) overlay::notify(overlay::NoticeLevel::info, kind == Kind::radio ? "Tuning in..." : "Loading song...", kind == Kind::radio ? chosen.artist : chosen.artist + " - " + chosen.title);
    try {
        std::thread(fetch_picture, kind, chosen, icon, of).detach();
    } catch (...) {}
#else
    (void)list;
    (void)song;
    (void)asked;
#endif
}
} // namespace

void start() noexcept {
    if (started.exchange(true)) return;
    try {
        std::thread(run).detach();
    } catch (...) {}
}
State state() {
    std::lock_guard lock(mutex);
    State out;
#if DINGOSDK_SPOTIFY_DEVICE
    out.available = true;
#endif
    out.tools = have_tools;
    out.busy = is_busy;
    out.percent = fetched_percent.load();
    out.problem = problem_text;
    out.browser = browser_name;
    out.cookies = have_cookies;
    out.runtime = javascript().first;
    out.playlists = library;
    for (auto& playlist : out.playlists) playlist.usable = playlist.kind != Kind::youtube || have_tools;
    out.problem_kind = problem_of;
    out.found = found_stations;
    out.searched = searched_for;
    out.playing = is_on.load(std::memory_order_relaxed) && (playing_stream != 0 || paused.load()) && playing_list < library.size();
    out.loading = out.playing && loading_now();
    out.playing_list = playing_list;
    out.playing_song = playing_song;
    return out;
}
std::vector<Playlist> playlists() {
    std::lock_guard lock(mutex);
    auto out = library;
    for (auto& playlist : out) playlist.usable = playlist.kind != Kind::youtube || have_tools;
    return out;
}
bool has(Kind kind) {
    std::lock_guard lock(mutex);
    return (kind != Kind::youtube || have_tools) && count_of(kind) > 0;
}
std::string tile_id(std::size_t playlist) {
    std::lock_guard lock(mutex);
    const auto kind = playlist < library.size() ? library[playlist].kind : Kind::youtube;
    return (kind == Kind::files ? files_prefix : kind == Kind::radio ? radio_prefix : playlist_prefix) + std::to_string(playlist);
}
void fetch_tools() { ask("tools"); }
void add(const std::string& link) { ask("add " + link); }
void remove(std::size_t playlist) {
    {
        std::lock_guard lock(mutex);
        if (playlist >= library.size()) return;
        library.erase(library.begin() + static_cast<std::ptrdiff_t>(playlist));
        // (What is playing is told from the others by its place among them.)
        if (playlist == playing_list && playing_stream) {
#if DINGOSDK_SPOTIFY_DEVICE
            wanted_stream.store(0, std::memory_order_relaxed);
            reskate_stream_stop();
#endif
            playing_stream = 0;
            track.playing = false;
        } else if (playlist < playing_list) {
            --playing_list;
        }
    }
    keep();
    profile_runtime::request_music_catalog_refresh();
}

void add_folder(const std::string& path) {
    if (path.size() <= 1000 && std::none_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x20; })) ask("folder " + path);
}
void search_radio(const std::string& query) {
    if (query.size() <= 200) ask("search " + query);
}
void add_station(const std::string& name, const std::string& address, const std::string& icon) {
    Json json = Json::object();
    json["name"] = name;
    json["address"] = address;
    json["icon"] = icon;
    ask("station " + json.dump());
}

void set_browser(const std::string& browser) {
    if (!good_browser(browser)) return;
    {
        std::lock_guard lock(mutex);
        browser_name = browser;
        problem_text.clear();
        failures = 0;
    }
    keep();
    if (!browser.empty()) ask("runtime");
}

bool set_cookies(const std::string& text) {
    std::error_code ignored;
    if (text.empty()) {
        std::filesystem::remove(cookies_file(), ignored);
        std::lock_guard lock(mutex);
        have_cookies = false;
        return true;
    }
    // A Netscape cookie file: its first line says so, and it has YouTube's cookies in it.
    if (text.size() > 1024 * 1024 || !(text.starts_with("# Netscape HTTP Cookie File") || text.starts_with("# HTTP Cookie File")) || text.find("youtube.com") == std::string::npos ||
        text.find('\0') != std::string::npos)
        return false;
    std::filesystem::create_directories(home(), ignored);
    {
        std::ofstream out(cookies_file(), std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
    }
    {
        std::lock_guard lock(mutex);
        have_cookies = true;
        problem_text.clear();
        failures = 0;
    }
    ask("runtime");
    return true;
}

bool notices() noexcept { return say_notices.load(std::memory_order_relaxed); }
void set_notices(bool on) noexcept { say_notices.store(on, std::memory_order_relaxed); }
bool on() noexcept { return is_on.load(std::memory_order_relaxed); }
void set_on(bool on) noexcept {
    if (is_on.exchange(on, std::memory_order_relaxed) && !on) {
#if DINGOSDK_SPOTIFY_DEVICE
        // (Its song is stopped as another kind of station takes over.)
        wanted_stream.store(0, std::memory_order_relaxed);
        reskate_stream_stop();
#endif
        std::lock_guard lock(mutex);
        playing_stream = 0;
        track.playing = false;
    }
}
unsigned rate() noexcept { return sound_rate.load(std::memory_order_relaxed); }
void play(std::size_t playlist, const std::string& song) {
    std::size_t at{};
    {
        std::lock_guard lock(mutex);
        if (playlist >= library.size()) return;
        // (The same playlist again, with no song asked for, goes on from where it was.)
        if (song.empty() && playlist == playing_list && playing_stream) {
            paused.store(false);
            track.playing = true;
            changes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (std::size_t i = 0; i < library[playlist].songs.size(); ++i)
            if (library[playlist].songs[i].id == song) at = i;
        failures = 0;
    }
    begin(playlist, at, true);
}
void play_at(std::size_t playlist, std::size_t song) {
    {
        std::lock_guard lock(mutex);
        if (!is_on.load(std::memory_order_relaxed) || playlist != playing_list || playlist >= library.size() || song >= library[playlist].songs.size()) return;
        failures = 0;
    }
    begin(playlist, song, true);
}
std::vector<std::uint8_t> art(std::size_t playlist, std::size_t song) {
    Song chosen;
    {
        std::lock_guard lock(mutex);
        if (playlist >= library.size() || library[playlist].kind != Kind::files || song >= library[playlist].songs.size()) return {};
        chosen = library[playlist].songs[song];
    }
    return picture(Kind::files, chosen, {});
}
external_player::Track now() {
    std::lock_guard lock(mutex);
    return track;
}
std::uint64_t revision() noexcept { return changes.load(std::memory_order_relaxed); }
void send(external_player::Command command) noexcept {
    try {
        using external_player::Command;
        std::size_t list{}, song{}, count{};
        bool radio{};
        {
            std::lock_guard lock(mutex);
            list = playing_list;
            song = playing_song;
            count = list < library.size() ? library[list].songs.size() : 0;
            radio = count && library[list].kind == Kind::radio;
            if (command == Command::pause || command == Command::play || command == Command::toggle) {
                const bool pause = command == Command::pause || (command == Command::toggle && !paused.load());
                // A station cannot be held where it is: it is left, and turned to again.
                if (radio && pause) {
#if DINGOSDK_SPOTIFY_DEVICE
                    wanted_stream.store(0, std::memory_order_relaxed);
                    reskate_stream_stop();
#endif
                    playing_stream = 0;
                }
                if (!radio || pause) {
                    paused.store(pause);
                    track.playing = !pause && playing_stream != 0;
                    changes.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (playing_stream) return; // (it is playing already)
            } else if (radio) {
                return; // (there is no next of a station)
            }
            failures = 0;
        }
        if (!count) return;
        begin(list, radio ? song : command == Command::next ? (song + 1) % count : (song + count - 1) % count, true);
    } catch (...) {}
}
} // namespace dingosdk::youtube_music
