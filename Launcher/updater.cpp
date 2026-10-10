#include "updater.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "launcher_update_config.h"
#include "text_encoding.h"
#include "Engine/Core/Platform/path_text.h"

#include <winhttp.h>
#include <miniz.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace dingosdk::launcher_update {
namespace {

constexpr std::uint64_t max_config_bytes = 256 * 1024;
constexpr std::uint64_t max_download_bytes = 512ull * 1024 * 1024;

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Internet {
    HINTERNET value{};
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};

using launcher_text::utf8;

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("Launcher config contains invalid UTF-8");
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), length);
    return output;
}

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void info(std::string_view message) {
    logging::write(logging::Level::info, logging::Channel::launcher, message);
}

std::wstring quote_argument(std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
    std::wstring output(1, L'\"');
    std::size_t slashes{};
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(character);
            slashes = 0;
        } else {
            output.append(slashes, L'\\');
            slashes = 0;
            output.push_back(character);
        }
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

// Streams an HTTPS GET into `sink`, following up to five HTTPS redirects.
void http_get(const std::wstring& url, std::uint64_t limit, int timeout_ms,
              const std::function<void(const char*, DWORD)>& sink) {
    const auto win32 = [](std::string_view action) {
        fail(std::format("{} failed (Windows error {})", action, GetLastError()));
    };
    Internet session{WinHttpOpen(L"ReSkate-Launcher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (!session.value) win32("WinHttpOpen");
    if (!WinHttpSetTimeouts(session.value, timeout_ms, timeout_ms, timeout_ms, timeout_ms)) win32("WinHttpSetTimeouts");
    std::wstring current = url;
    for (int hop = 0; hop < 6; ++hop) {
        if (current.empty() || current.size() >= 4096 || current.find_first_of(L"\r\n\t #") != std::wstring::npos)
            fail("Download URL is invalid");
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
            parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(current.c_str(), static_cast<DWORD>(current.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength ||
            parts.dwUserNameLength || parts.dwPasswordLength) fail("Download URL must be HTTPS: " + utf8(current));
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring resource = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
        if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
        if (!connection.value) win32("WinHttpConnect");
        Internet request{WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr, nullptr,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (!request.value) win32("WinHttpOpenRequest");
        DWORD redirects = WINHTTP_DISABLE_REDIRECTS;
        DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &redirects, sizeof(redirects)) ||
            !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon))) win32("WinHttpSetOption");
        std::wstring headers = L"Cache-Control: no-cache\r\n";
        if (host == L"api.github.com")
            headers += L"X-GitHub-Api-Version: 2022-11-28\r\nAccept: application/vnd.github+json\r\n";
        if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1L), nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr)) win32("HTTPS request");
        DWORD status{}, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                nullptr, &status, &size, nullptr)) win32("WinHttpQueryHeaders");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            DWORD bytes{};
            WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, nullptr);
            std::wstring location(bytes / sizeof(wchar_t), L'\0');
            if (!bytes || !WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                    location.data(), &bytes, nullptr)) fail(std::format("{} redirected without a location", utf8(current)));
            location.resize(bytes / sizeof(wchar_t));
            current = std::move(location);
            continue;
        }
        // Unauthenticated, each player may ask GitHub's API 60 times an hour.
        if (host == L"api.github.com" && (status == 403 || status == 429))
            fail(std::format("GitHub is limiting release checks from this network (HTTP {}); try again later", status));
        if (status != 200) fail(std::format("{} returned HTTP {}", utf8(current), status));
        std::array<char, 65536> buffer{};
        std::uint64_t total{};
        for (;;) {
            DWORD read{};
            if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) win32("WinHttpReadData");
            if (!read) break;
            if (total + read > limit) fail("Download is larger than expected: " + utf8(current));
            sink(buffer.data(), read);
            total += read;
        }
        return;
    }
    fail("Too many redirects: " + utf8(url));
}

bool is_sha256(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

bool is_decimal(std::string_view value) {
    return !value.empty() && value.size() <= 20 && value.find_first_not_of("0123456789") == std::string_view::npos;
}

RemoteFile parse_file(const Json& root, std::string_view key, bool required) {
    RemoteFile file;
    if (!root.contains(key) || root.at(key).is_null()) {
        if (required) fail(std::format("Launcher config is missing \"{}\"", key));
        return file;
    }
    const auto& section = root.at(key);
    file.version = section.value("version", "");
    file.url = widen(section.at("url").string());
    file.sha256 = section.at("sha256").string();
    file.size = section.at("size").get<std::uint64_t>();
    if (!file.url.starts_with(L"https://") && !file.url.starts_with(L"asset:"))
        fail(std::format("Launcher config \"{}.url\" must be HTTPS or asset:<release asset name>", key));
    if (!is_sha256(file.sha256)) fail(std::format("Launcher config \"{}.sha256\" must be 64 lowercase hex digits", key));
    if (!file.size || file.size > max_download_bytes) fail(std::format("Launcher config \"{}.size\" is out of range", key));
    return file;
}

fs::path local_app_data() {
    std::array<wchar_t, 32768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!length || length >= local.size()) fail("LOCALAPPDATA is unavailable");
    return fs::path(local.data());
}

struct Zip {
    std::vector<unsigned char> bytes;
    mz_zip_archive value{};
    explicit Zip(const fs::path& archive) {
        const auto length = fs::file_size(archive);
        std::ifstream input(archive, std::ios::binary);
        bytes.resize(static_cast<std::size_t>(length));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            fail("Cannot read " + utf8(archive.filename().wstring()));
        if (!mz_zip_reader_init_mem(&value, bytes.data(), bytes.size(), 0))
            fail(utf8(archive.filename().wstring()) + " is not a ZIP");
    }
    ~Zip() { mz_zip_reader_end(&value); }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;
};

// Writes one ZIP entry to `output`.
void extract_index(Zip& zip, mz_uint index, const mz_zip_archive_file_stat& entry, const fs::path& output) {
    Handle file{CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) fail("Cannot create " + path_utf8(output));
    struct Sink { HANDLE file; std::uint64_t count{}; } sink{file.value};
    const auto write = [](void* opaque, mz_uint64 offset, const void* buffer, size_t count) -> size_t {
        auto& target = *static_cast<Sink*>(opaque);
        DWORD written{};
        if (offset != target.count || !WriteFile(target.file, buffer, static_cast<DWORD>(count), &written, nullptr) ||
            written != count) return 0;
        target.count += count;
        return count;
    };
    if (!mz_zip_reader_extract_to_callback(&zip.value, index, write, &sink, 0) ||
        sink.count != entry.m_uncomp_size || !FlushFileBuffers(file.value))
        fail(std::format("Cannot extract {}", entry.m_filename));
}

// Writes exactly one named ZIP entry to `output`; entry names never become paths.
void extract_entry(const fs::path& archive, const char* name, const fs::path& output) {
    Zip zip(archive);
    const int index = mz_zip_reader_locate_file(&zip.value, name, nullptr, 0);
    mz_zip_archive_file_stat entry{};
    if (index < 0 || !mz_zip_reader_file_stat(&zip.value, static_cast<mz_uint>(index), &entry) ||
        entry.m_is_directory || entry.m_is_encrypted) fail(std::format("DepotDownloader archive has no {}", name));
    extract_index(zip, static_cast<mz_uint>(index), entry, output);
}

// A ZIP entry name as a path inside the install folder, or empty when it
// could escape it (absolute, drive-qualified, or with . or .. parts).
fs::path safe_entry_path(std::string_view name) {
    if (name.empty() || name.size() > 260 || name.front() == '/' || name.front() == '\\' ||
        name.find(':') != std::string_view::npos) return {};
    fs::path result;
    std::size_t start{};
    for (;;) {
        const auto end = name.find_first_of("/\\", start);
        const auto part = name.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty() || part == "." || part == "..") return {};
        result /= fs::path(widen(part));
        if (end == std::string_view::npos) return result;
        start = end + 1;
    }
}

} // namespace

void http_stream(const std::wstring& url, std::uint64_t limit, int timeout_ms,
                 const std::function<void(const char*, DWORD)>& sink) {
    http_get(url, limit, timeout_ms, sink);
}

bool binary_updates_enabled() noexcept { return launcher_binary_updates; }

Config parse_config(std::string_view text) {
    const auto root = Json::parse(text, JsonLimits{max_config_bytes, 8, 4096});
    if (!root.is_object()) fail("Launcher config must be a JSON object");
    const auto schema = root.value("schema", 0);
    if (schema != 1) fail(std::format("Launcher config schema {} is not supported; update the launcher manually", schema));
    Config config;
    config.launcher = parse_file(root, "launcher", false);
    config.runtime = parse_file(root, "runtime", false);
    config.depot_downloader = parse_file(root, "depot_downloader", true);
    config.server = parse_file(root, "server", false);
    if (!config.server.url.empty()) {
        config.server_exe_sha256 = root.at("server").at("exe_sha256").string();
        if (!is_sha256(config.server_exe_sha256))
            fail("Launcher config \"server.exe_sha256\" must be 64 lowercase hex digits");
    }
    const auto& game = root.at("game");
    config.game.app_id = game.at("app_id").get<std::uint32_t>();
    config.game.depot_id = game.at("depot_id").get<std::uint32_t>();
    config.game.manifest_id = game.at("manifest_id").string();
    config.game.build_id = game.value("build_id", "");
    config.game.skate_sha256 = game.at("skate_sha256").string();
    if (!config.game.app_id || !config.game.depot_id || !is_decimal(config.game.manifest_id))
        fail("Launcher config \"game\" needs app_id, depot_id and a decimal manifest_id string");
    if (!is_sha256(config.game.skate_sha256)) fail("Launcher config \"game.skate_sha256\" must be 64 lowercase hex digits");
    return config;
}

std::optional<Config> fetch_config() {
    std::array<wchar_t, 32768> local{};
    const auto local_length = GetEnvironmentVariableW(L"RESKATE_LAUNCHER_CONFIG_FILE", local.data(), static_cast<DWORD>(local.size()));
    if (local_length && local_length < local.size()) {
        // Test an unpublished config before attaching it to a release.
        const fs::path path(std::wstring(local.data(), local_length));
        try {
            std::ifstream input(path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (!input && !input.eof()) fail("Cannot read " + utf8(path.wstring()));
            logging::write(logging::Level::info, logging::Channel::launcher, L"Using local launcher config: " + path.wstring());
            return parse_config(text);
        } catch (const std::exception& exception) {
            logging::log(logging::Level::warning, logging::Channel::launcher, "Local launcher config rejected: {}", exception.what());
            return std::nullopt;
        }
    }
    try {
        // Public releases: the newest release carries launcher.json and the files it pins. Only
        // this lookup uses GitHub's rate-limited API; the files come from the release's download links.
        const auto latest = std::wstring(L"https://api.github.com/repos/") + launcher_release_repo + L"/releases/latest";
        logging::write(logging::Level::info, logging::Channel::launcher, L"Checking GitHub releases: " + std::wstring(launcher_release_repo));
        std::string text;
        http_get(latest, 4 * 1024 * 1024, 8000, [&](const char* data, DWORD count) { text.append(data, count); });
        const auto release = Json::parse(text);
        std::map<std::string, std::wstring, std::less<>> assets;
        if (release.contains("assets"))
            for (const auto& asset : release.at("assets"))
                assets[asset.value("name", "")] = widen(asset.value("browser_download_url", ""));
        const auto manifest = assets.find("launcher.json");
        if (manifest == assets.end()) fail("The latest release has no launcher.json asset");
        std::string json;
        http_get(manifest->second, max_config_bytes, 8000, [&](const char* data, DWORD count) { json.append(data, count); });
        auto config = parse_config(json);
        for (auto* file : {&config.launcher, &config.runtime, &config.depot_downloader, &config.server}) {
            if (!file->url.starts_with(L"asset:")) continue;
            const auto found = assets.find(utf8(std::wstring_view(file->url).substr(6)));
            if (found == assets.end()) fail("The latest release is missing asset " + utf8(file->url.substr(6)));
            file->url = found->second;
        }
        logging::log(logging::Level::info, logging::Channel::launcher,
            "Release {}: launcher {}, runtime {}, game depot {} manifest {}.", release.value("tag_name", "?"),
            config.launcher.version.empty() ? "-" : config.launcher.version,
            config.runtime.version.empty() ? "-" : config.runtime.version,
            config.game.depot_id, config.game.manifest_id);
        return config;
    } catch (const std::exception& exception) {
        logging::log(logging::Level::warning, logging::Channel::launcher,
            "Release check unavailable ({}); continuing with local files.", exception.what());
        return std::nullopt;
    }
}

namespace {
// &lt; &amp; &#39; &#x27; ... as the characters they stand for; anything else is left as it is.
std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        const auto end = text[at] == '&' ? text.find(';', at) : std::string_view::npos;
        if (end == std::string_view::npos || end - at > 9) { out.push_back(text[at++]); continue; }
        const auto name = text.substr(at + 1, end - at - 1);
        unsigned long code{};
        if (name == "lt") code = '<';
        else if (name == "gt") code = '>';
        else if (name == "amp") code = '&';
        else if (name == "quot") code = '"';
        else if (name == "apos") code = '\'';
        else if (name == "nbsp") code = ' ';
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string digits(name.substr(hex ? 2 : 1));
            char* stop{};
            code = digits.empty() ? 0 : std::strtoul(digits.c_str(), &stop, hex ? 16 : 10);
            if (!stop || *stop || code > 0x10ffff) code = 0;
        }
        if (!code) { out.push_back(text[at++]); continue; }
        if (code < 0x80) out.push_back(static_cast<char>(code));
        else if (code < 0x800) out += {static_cast<char>(0xc0 | code >> 6), static_cast<char>(0x80 | (code & 0x3f))};
        else if (code < 0x10000)
            out += {static_cast<char>(0xe0 | code >> 12), static_cast<char>(0x80 | (code >> 6 & 0x3f)),
                    static_cast<char>(0x80 | (code & 0x3f))};
        else
            out += {static_cast<char>(0xf0 | code >> 18), static_cast<char>(0x80 | (code >> 12 & 0x3f)),
                    static_cast<char>(0x80 | (code >> 6 & 0x3f)), static_cast<char>(0x80 | (code & 0x3f))};
        at = end + 1;
    }
    return out;
}

// The HTML GitHub renders a release's notes to, back as the plain markdown the launcher
// draws: paragraphs, headings and list items keep their lines, every other tag is dropped.
std::string html_to_markdown(std::string_view html) {
    std::string out;
    const auto line = [&](int blank) {
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        int have{};
        for (auto it = out.rbegin(); it != out.rend() && *it == '\n'; ++it) ++have;
        for (; !out.empty() && have < blank; ++have) out.push_back('\n');
    };
    for (std::size_t at = 0; at < html.size();) {
        if (html[at] != '<') {
            out.push_back(html[at++]);
            continue;
        }
        const auto end = html.find('>', at);
        if (end == std::string_view::npos) break;
        auto tag = html.substr(at + 1, end - at - 1);
        at = end + 1;
        const bool closing = tag.starts_with('/');
        if (closing) tag.remove_prefix(1);
        std::string name(tag.substr(0, tag.find_first_of(" \t\r\n/")));
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool heading = name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
        if (name == "br") line(1);
        else if (name == "li" && !closing) line(1), out += "- ";
        else if (heading && !closing) line(2), out += "## ";
        else if (name == "p" || name == "div" || name == "pre" || name == "ul" || name == "ol" || name == "blockquote" || heading)
            line(2);
        else if (name == "li" || name == "tr") line(1);
    }
    return unescape(out);
}

std::string_view between(std::string_view text, std::string_view open, std::string_view close) {
    const auto start = text.find(open);
    if (start == std::string_view::npos) return {};
    const auto from = start + open.size();
    const auto end = text.find(close, from);
    return end == std::string_view::npos ? std::string_view{} : text.substr(from, end - from);
}
} // namespace

std::vector<ReleaseNote> parse_release_feed(std::string_view feed) {
    std::vector<ReleaseNote> notes;
    for (std::size_t at = 0; notes.size() < 50;) {
        const auto start = feed.find("<entry>", at);
        const auto end = start == std::string_view::npos ? start : feed.find("</entry>", start);
        if (end == std::string_view::npos) break;
        const auto entry = feed.substr(start, end - start);
        at = end;
        ReleaseNote note;
        note.title = unescape(between(entry, "<title>", "</title>"));
        note.tag = unescape(between(entry, "/releases/tag/", "\""));
        if (note.tag.empty()) note.tag = note.title;
        note.date = std::string(between(entry, "<updated>", "</updated>").substr(0, 10));
        // <content type="html">: the notes as HTML, escaped once more to sit in the feed.
        if (const auto content = entry.find("<content"); content != std::string_view::npos)
            note.notes = html_to_markdown(unescape(between(entry.substr(content), ">", "</content>")));
        if (!note.tag.empty()) notes.push_back(std::move(note));
    }
    return notes;
}

ReleaseNote fetch_release_note() {
    // The description as it was written, markdown, from GitHub's API.
    try {
        const auto url = std::wstring(L"https://api.github.com/repos/") + launcher_release_repo + L"/releases/latest";
        std::string text;
        http_get(url, 4 * 1024 * 1024, 8000, [&](const char* data, DWORD count) { text.append(data, count); });
        const auto release = Json::parse(text);
        // (A release made without a title or a description has null there.)
        const auto field = [&](std::string_view key) {
            return release.contains(key) && release.at(key).is_string() ? release.at(key).string() : std::string();
        };
        ReleaseNote note{field("tag_name"), field("name"), field("published_at").substr(0, 10), field("body")};
        if (!note.tag.empty() && note.notes.find_first_not_of(" \t\r\n") != std::string::npos) return note;
    } catch (const std::exception& exception) {
        logging::log(logging::Level::info, logging::Channel::launcher,
            "Latest release's notes not available from GitHub's API ({}); reading the releases feed.", exception.what());
    }
    // Else what the releases page shows, from its feed: a release published without a
    // description shows its commit's message there, and the feed has no hourly limit.
    const auto url = std::wstring(L"https://github.com/") + launcher_release_repo + L"/releases.atom";
    std::string text;
    http_get(url, 4 * 1024 * 1024, 8000, [&](const char* data, DWORD count) { text.append(data, count); });
    auto notes = parse_release_feed(text);
    if (notes.empty()) fail("The repository has no releases");
    return std::move(notes.front());
}

std::string releases_page() { return "https://github.com/" + utf8(launcher_release_repo) + "/releases"; }

bool file_matches(const fs::path& path, const RemoteFile& file) {
    std::error_code error;
    return fs::is_regular_file(path, error) && fs::file_size(path, error) == file.size && !error &&
           launcher::sha256_file(path) == file.sha256;
}

fs::path download_verified(const fs::path& target, const RemoteFile& file, const Progress& progress) {
    const fs::path temporary = target.wstring() + L".new";
    DeleteFileW(temporary.c_str());
    logging::write(logging::Level::info, logging::Channel::launcher,
        L"Downloading " + target.filename().wstring() + L" " + widen(file.version) + L" from " + file.url);
    {
        Handle output{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE)
            fail(std::format("Cannot write {} (Windows error {})", utf8(temporary.wstring()), GetLastError()));
        std::uint64_t received{};
        if (progress) progress(0, file.size);
        http_get(file.url, file.size, 30000, [&](const char* data, DWORD count) {
            received += count;
            if (progress) progress(received, file.size);
            DWORD written{};
            if (!WriteFile(output.value, data, count, &written, nullptr) || written != count)
                fail("Cannot write " + utf8(temporary.wstring()));
        });
        if (!FlushFileBuffers(output.value)) fail("Cannot flush " + utf8(temporary.wstring()));
    }
    if (!file_matches(temporary, file)) {
        DeleteFileW(temporary.c_str());
        fail("Downloaded " + utf8(target.filename().wstring()) + " does not match the SHA-256 in the launcher config");
    }
    return temporary;
}

void replace_file(const fs::path& target, const RemoteFile& file, const Progress& progress) {
    const auto verified = download_verified(target, file, progress);
    if (!MoveFileExW(verified.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(verified.c_str());
        fail(std::format("Cannot replace {} (Windows error {}); is Skate still running?",
            utf8(target.filename().wstring()), error));
    }
}

void replace_running_launcher(const fs::path& self, const RemoteFile& file, const Progress& progress) {
    const auto verified = download_verified(self, file, progress);
    const fs::path previous = self.wstring() + L".old";
    DeleteFileW(previous.c_str());
    // Windows allows renaming, but not overwriting, a running executable.
    if (!MoveFileExW(self.c_str(), previous.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(verified.c_str());
        fail(std::format("Cannot move the running launcher aside (Windows error {})", error));
    }
    if (!MoveFileExW(verified.c_str(), self.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        MoveFileExW(previous.c_str(), self.c_str(), MOVEFILE_WRITE_THROUGH);
        fail(std::format("Cannot install the launcher update (Windows error {})", error));
    }
}

void remove_previous_launcher(const fs::path& self) noexcept {
    const std::wstring previous = self.wstring() + L".old";
    // The previous process may still be exiting; a later launch retries.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (DeleteFileW(previous.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
        Sleep(100);
    }
}

std::vector<std::string> install_archive(const fs::path& archive, const fs::path& directory) {
    Zip zip(archive);
    struct Staged { fs::path target, staged; std::string name; };
    std::vector<Staged> files;
    try {
        const auto count = mz_zip_reader_get_num_files(&zip.value);
        for (mz_uint index = 0; index < count; ++index) {
            mz_zip_archive_file_stat entry{};
            if (!mz_zip_reader_file_stat(&zip.value, index, &entry)) fail("Cannot read the update archive");
            const std::string_view name = entry.m_filename;
            if (entry.m_is_directory || name.ends_with('/') || name.ends_with('\\')) continue;
            if (entry.m_is_encrypted) fail(std::format("{} in the update is encrypted", entry.m_filename));
            const auto relative = safe_entry_path(entry.m_filename);
            if (relative.empty()) fail(std::format("The update contains an unsafe path: {}", entry.m_filename));
            const auto target = directory / relative;
            fs::create_directories(target.parent_path());
            files.push_back({target, target.wstring() + L".update-new", entry.m_filename});
            extract_index(zip, index, entry, files.back().staged);
        }
    } catch (...) {
        for (const auto& file : files) DeleteFileW(file.staged.c_str());
        throw;
    }
    std::vector<std::string> installed;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        if (!MoveFileExW(file.staged.c_str(), file.target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // Windows renames, but won't overwrite, a running exe or a loaded DLL.
            const fs::path aside = file.target.wstring() + L".update-old";
            DeleteFileW(aside.c_str());
            const bool moved_aside = MoveFileExW(file.target.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!moved_aside || !MoveFileExW(file.staged.c_str(), file.target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                const auto error = GetLastError();
                // Put the current file back so a failed update never leaves it missing.
                if (moved_aside) MoveFileExW(aside.c_str(), file.target.c_str(), MOVEFILE_WRITE_THROUGH);
                for (std::size_t rest = i; rest < files.size(); ++rest) DeleteFileW(files[rest].staged.c_str());
                fail(std::format("Cannot replace {} (Windows error {})", file.name, error));
            }
        }
        installed.push_back(file.name);
    }
    return installed;
}

std::string archive_entry_sha256(const fs::path& archive, const char* name) {
    Zip zip(archive);
    const int index = mz_zip_reader_locate_file(&zip.value, name, nullptr, 0);
    mz_zip_archive_file_stat entry{};
    if (index < 0 || !mz_zip_reader_file_stat(&zip.value, static_cast<mz_uint>(index), &entry) ||
        entry.m_is_directory || entry.m_is_encrypted) return {};
    const fs::path temporary = archive.wstring() + L".check";
    struct Remove { const fs::path& path; ~Remove() { DeleteFileW(path.c_str()); } } remove{temporary};
    extract_index(zip, static_cast<mz_uint>(index), entry, temporary);
    return launcher::sha256_file(temporary);
}

void remove_replaced_files(const fs::path& directory) noexcept {
    std::error_code error;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == L".update-old") DeleteFileW(it->path().c_str());
    }
}

fs::path ensure_depot_downloader(const RemoteFile& file, const Progress& progress) {
    const auto directory = local_app_data() / L"ReSkate" / L"tools" / L"DepotDownloader" / widen(file.sha256.substr(0, 16));
    const auto executable = directory / L"DepotDownloader.exe";
    std::error_code error;
    if (fs::is_regular_file(executable, error)) return executable;
    fs::create_directories(directory);
    const auto archive = directory / L"DepotDownloader.zip";
    const auto verified = download_verified(archive, file, progress);
    const fs::path unpacked = executable.wstring() + L".tmp";
    try {
        extract_entry(verified, "DepotDownloader.exe", unpacked);
    } catch (...) {
        DeleteFileW(verified.c_str());
        DeleteFileW(unpacked.c_str());
        throw;
    }
    DeleteFileW(verified.c_str());
    if (!MoveFileExW(unpacked.c_str(), executable.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail(std::format("Cannot install DepotDownloader (Windows error {})", GetLastError()));
    logging::write(logging::Level::info, logging::Channel::launcher, L"DepotDownloader ready: " + executable.wstring());
    return executable;
}

DWORD run_depot_downloader(const fs::path& depot_downloader, const GameBuild& game,
                           const fs::path& directory, bool validate, const SteamLogin& login,
                           const std::function<void(std::string_view)>& on_line,
                           const PromptHandler& on_prompt, const std::atomic<bool>& cancel) {
    std::wstring command = quote_argument(depot_downloader.wstring()) +
        std::format(L" -app {} -depot {} -manifest {} -dir ", game.app_id, game.depot_id, widen(game.manifest_id)) +
        quote_argument(directory.wstring()) + L" -max-downloads 16";
    std::wstring user, options;
    if (login.username.empty()) options += L" -qr";
    else {
        user = L" -username " + quote_argument(widen(login.username));
        if (login.remember) options += L" -remember-password";
        if (login.prefer_code) options += L" -no-mobile";
    }
    if (validate) options += L" -validate";
    // The log is attached to crash reports, so it leaves out the Steam login name.
    logging::write(logging::Level::info, logging::Channel::launcher, L"Running DepotDownloader: " + command +
        (user.empty() ? L"" : L" -username <hidden>") + options);
    command += user + options;

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe.value, &write_pipe.value, &inherit, 0) ||
        !SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0)) fail("Cannot create the DepotDownloader pipe");
    Handle input_read, input_write;
    if (!CreatePipe(&input_read.value, &input_write.value, &inherit, 0) ||
        !SetHandleInformation(input_write.value, HANDLE_FLAG_INHERIT, 0)) fail("Cannot create the DepotDownloader pipe");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read.value;
    startup.hStdOutput = startup.hStdError = write_pipe.value;
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    PROCESS_INFORMATION process{};
    // The job ends DepotDownloader with the launcher, even if the launcher is killed.
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        fail("Cannot create the DepotDownloader job");
    if (!CreateProcessW(depot_downloader.c_str(), buffer.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, depot_downloader.parent_path().c_str(), &startup, &process))
        fail(std::format("Cannot start DepotDownloader (Windows error {})", GetLastError()));
    Handle thread{process.hThread};
    Handle child{process.hProcess};
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, ERROR_CANCELLED);
        fail(std::format("Cannot start DepotDownloader (Windows error {})", GetLastError()));
    }
    ResumeThread(thread.value);
    CloseHandle(write_pipe.value);
    write_pipe.value = INVALID_HANDLE_VALUE;
    CloseHandle(input_read.value);
    input_read.value = INVALID_HANDLE_VALUE;

    std::string pending;
    bool rejected{};
    // "\r\n" is one break even when a read splits it; an extra empty line would
    // end DepotOutput's QR code after its first row.
    bool after_cr{};
    const auto emit = [&](bool flush) {
        for (;;) {
            const auto end = pending.find_first_of("\r\n");
            if (end == std::string::npos) break;
            if (end == 0 && pending[0] == '\n' && after_cr) {
                after_cr = false;
                pending.erase(0, 1);
                continue;
            }
            after_cr = pending[end] == '\r';
            const auto line = std::string_view(pending).substr(0, end);
            if (line.find("code you have provided is incorrect") != std::string_view::npos) rejected = true;
            on_line(line);
            pending.erase(0, end + 1);
        }
        if (flush && !pending.empty()) { on_line(pending); pending.clear(); }
    };
    bool cancelled{};
    // Prompts end in ": " without a newline, so look at the unfinished line.
    const auto answer_prompt = [&] {
        if (pending.size() < 2 || !pending.ends_with(": ")) return;
        Prompt prompt;
        if (pending.find("Enter account password") != std::string::npos) prompt.kind = PromptKind::password;
        else if (pending.find("authenticator app") != std::string::npos) prompt.kind = PromptKind::authenticator_code;
        else if (pending.find("auth code sent to the email") != std::string::npos) prompt.kind = PromptKind::email_code;
        else return;
        prompt.text = pending.substr(0, pending.size() - 2);
        prompt.retry = std::exchange(rejected, false);
        logging::write(logging::Level::info, logging::Channel::launcher, "DepotDownloader asks: " + prompt.text);
        pending.clear();
        auto answer = on_prompt(prompt);
        if (!answer || cancel) {
            cancelled = true;
            TerminateProcess(child.value, ERROR_CANCELLED);
            return;
        }
        answer->push_back('\n');
        DWORD written{};
        WriteFile(input_write.value, answer->data(), static_cast<DWORD>(answer->size()), &written, nullptr);
        SecureZeroMemory(answer->data(), answer->size());
    };
    std::array<char, 16384> chunk{};
    for (;;) {
        DWORD available{};
        const bool peeked = PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr, &available, nullptr);
        if (peeked && available) {
            DWORD read{};
            if (!ReadFile(read_pipe.value, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) || !read) break;
            pending.append(chunk.data(), read);
            emit(false);
            answer_prompt();
            continue;
        }
        // A broken pipe means every writer has exited and the output is drained.
        if (!peeked) break;
        if (cancel && !cancelled) {
            cancelled = true;
            TerminateProcess(child.value, ERROR_CANCELLED);
        }
        WaitForSingleObject(child.value, 50);
    }
    emit(true);
    WaitForSingleObject(child.value, INFINITE);
    DWORD exit_code{};
    GetExitCodeProcess(child.value, &exit_code);
    logging::log(logging::Level::info, logging::Channel::launcher, "DepotDownloader exited with code {}{}",
        exit_code, cancelled ? " (cancelled)" : "");
    return exit_code;
}

} // namespace dingosdk::launcher_update
