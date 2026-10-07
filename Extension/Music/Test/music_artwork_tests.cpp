#include <winsock2.h>
#include <Windows.h>
#include <winhttp.h>
#include "Extension/Music/music_artwork.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Handle {
    HINTERNET value{};
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
std::pair<DWORD, std::string> fetch(const std::string& url) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return {};
    Handle session{WinHttpOpen(L"Artwork test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0)};
    if (!session.value) return {};
    WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 3000);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    Handle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) return {};
    Handle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, nullptr, nullptr, 0)};
    if (!request.value || !WinHttpSendRequest(request.value, nullptr, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return {};
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status, &size, nullptr)) return {};
    std::string body;
    std::array<char, 1024> buffer{};
    DWORD received{};
    while (WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received) && received)
        body.append(buffer.data(), received);
    return {status, body};
}
// A minimal HTTP/2 cleartext (h2c) client, the way the game's client connects: send the connection
// preface, SETTINGS and a HEADERS frame, then collect the DATA frames of the response.
std::pair<int, std::string> fetch_h2c(unsigned short port, const std::string& path) {
    SOCKET socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value == INVALID_SOCKET) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket_value);
        return {};
    }
    constexpr char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    send(socket_value, preface, 24, 0);
    const char settings[] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
    send(socket_value, settings, 9, 0);
    // HPACK block: :method GET (indexed 2), :scheme http (indexed 6), :path as a literal without
    // indexing with the static name index 4 and a raw (non-Huffman) value.
    std::string block;
    block.push_back(static_cast<char>(0x82));
    block.push_back(static_cast<char>(0x86));
    block.push_back(static_cast<char>(0x04));
    block.push_back(static_cast<char>(path.size()));
    block.insert(block.end(), path.begin(), path.end());
    const char headers[] = {0, 0, static_cast<char>(block.size()), 1, 4, 0, 0, 0, 1};
    send(socket_value, headers, 9, 0);
    send(socket_value, block.data(), static_cast<int>(block.size()), 0);
    std::string buffer, body;
    int status = 0;
    for (;;) {
        char chunk[16384];
        const int got = recv(socket_value, chunk, sizeof(chunk), 0);
        if (got <= 0) break;
        buffer.append(chunk, got);
        bool done = false;
        while (buffer.size() >= 9) {
            const auto length = (std::size_t(static_cast<unsigned char>(buffer[0])) << 16) |
                                (std::size_t(static_cast<unsigned char>(buffer[1])) << 8) | std::size_t(static_cast<unsigned char>(buffer[2]));
            const auto type = static_cast<unsigned char>(buffer[3]);
            const auto flags = static_cast<unsigned char>(buffer[4]);
            const auto stream = ((static_cast<unsigned>(static_cast<unsigned char>(buffer[5])) << 24) |
                                 (static_cast<unsigned>(static_cast<unsigned char>(buffer[6])) << 16) |
                                 (static_cast<unsigned>(static_cast<unsigned char>(buffer[7])) << 8) |
                                 static_cast<unsigned>(static_cast<unsigned char>(buffer[8]))) & 0x7fffffffu;
            if (buffer.size() < 9 + length) break;
            const std::string payload = buffer.substr(9, length);
            buffer.erase(0, 9 + length);
            if (type == 0x1 && stream == 1 && !payload.empty() && static_cast<unsigned char>(payload[0]) == 0x88) status = 200;
            if (type == 0x0 && stream == 1) {
                body += payload;
                if (flags & 0x1) done = true;
            }
        }
        if (done) break;
    }
    closesocket(socket_value);
    return {status, body};
}
}
int main() try {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / (L"ReSkateArtworkTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"mod" / L"artwork");
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    // A real one-pixel grayscale PNG. Bytes returned by HTTP must match it exactly.
    constexpr unsigned char png[]{
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,
        0,0,0,11,73,68,65,84,120,218,99,100,248,15,0,1,5,1,1,39,24,227,102,0,0,0,0,73,69,78,68,174,66,96,130};
    const std::string bytes(reinterpret_cast<const char*>(png), sizeof(png));
    { std::ofstream out(root / L"mod/artwork/cover.png", std::ios::binary); out.write(bytes.data(), bytes.size()); }
    { std::ofstream out(root / L"mod/artwork/bad.png", std::ios::binary); out << "not an image"; }
    { std::ofstream out(root / L"outside.png", std::ios::binary); out.write(bytes.data(), bytes.size()); }
    dingosdk::profile_runtime::MusicArtworkServer server;
    const auto url = server.add(root / L"mod", "artwork/cover.png");
    check(url.starts_with("http://127.0.0.1:"), "server uses an ephemeral loopback URL");
    check(server.add(root / L"mod", "artwork/cover.png") == url, "repeated registration keeps the URL");
    check(server.add(root / L"mod", "../outside.png").empty(), "parent traversal is refused");
    check(server.add(root / L"mod", "artwork/missing.png").empty(), "missing image is ignored");
    check(server.add(root / L"mod", "artwork/bad.png").empty(), "non-PNG bytes are ignored");
    if (!url.empty()) {
        const auto [status, body] = fetch(url);
        check(status == 200 && body == bytes, "HTTP returns the exact registered PNG");
        const auto base = url.substr(0, url.find('/', 7));
        check(fetch(base + "/music-art/unknown.png").first == 404, "unknown route is not served");
        check(fetch(base + "/outside.png").first == 404, "HTTP cannot read arbitrary files");
        const auto colon = url.find(':', 7), slash = url.find('/', colon);
        const auto port = static_cast<unsigned short>(std::stoi(url.substr(colon + 1, slash - colon - 1)));
        const auto [h2status, h2body] = fetch_h2c(port, url.substr(slash));
        check(h2status == 200 && h2body == bytes, "h2c returns the exact registered PNG");
    }
    std::error_code ec;
    fs::create_directory_symlink(root, root / L"mod" / L"escape", ec);
    if (!ec) check(server.add(root / L"mod", "escape/outside.png").empty(), "symlink escape is refused");
    else std::cout << "SKIP: symlink test needs Windows symlink privileges\n";
    // Cross the production 64 MiB cache budget. Padding preserves the PNG header;
    // these fixtures exercise byte storage/transport, not an image decoder.
    std::string large = bytes;
    large.resize(1024 * 1024, '\0');
    std::string first, last;
    for (unsigned i = 0; i < 65; ++i) {
        const auto relative = "artwork/large-" + std::to_string(i) + ".png";
        { std::ofstream out(root / L"mod" / relative, std::ios::binary); out.write(large.data(), large.size()); }
        const auto registered = server.add(root / L"mod", relative);
        check(!registered.empty(), "artwork beyond 64 MiB still registers");
        if (i == 0) first = registered;
        if (i == 64) last = registered;
    }
    check(server.add(root / L"mod", "artwork/large-0.png") == first, "evicted artwork keeps its registered URL");
    if (!first.empty()) {
        const auto [status, body] = fetch(first);
        check(status == 200 && body == large, "evicted artwork reloads with exact bytes");
    }
    if (!last.empty()) {
        const auto [status, body] = fetch(last);
        check(status == 200 && body == large, "artwork registered beyond the budget is served");
    }
    if (!url.empty()) {
        const auto colon = url.find(':', 7), slash = url.find('/', colon);
        const auto port = static_cast<unsigned short>(std::stoi(url.substr(colon + 1, slash - colon - 1)));
        const auto [status, body] = fetch_h2c(port, url.substr(slash));
        check(status == 200 && body == bytes, "h2c reloads evicted artwork with exact bytes");
    }
    // Replacing one file must not consume another image's worth of budget forever.
    const auto replacement = root / L"mod/artwork/replacement.png";
    std::string replacement_url;
    const auto stamp = fs::file_time_type::clock::now();
    for (unsigned i = 0; i < 65; ++i) {
        large.back() = static_cast<char>(i);
        { std::ofstream out(replacement, std::ios::binary); out.write(large.data(), large.size()); }
        fs::last_write_time(replacement, stamp + std::chrono::seconds(i));
        const auto registered = server.add(root / L"mod", "artwork/replacement.png");
        check(!registered.empty(), "replacing artwork does not exhaust the cache budget");
        if (i == 0) replacement_url = registered;
        else check(registered == replacement_url, "replacement retains its stable URL");
    }
    if (!replacement_url.empty()) {
        const auto [status, body] = fetch(replacement_url);
        check(status == 200 && body == large, "replacement serves the latest registered bytes");
    }
    // large-1 was evicted; a reload must validate the registered file again.
    const auto invalid_url = server.add(root / L"mod", "artwork/large-1.png");
    { std::ofstream out(root / L"mod/artwork/large-1.png", std::ios::binary); out << "not an image"; }
    if (!invalid_url.empty()) check(fetch(invalid_url).first == 404, "evicted artwork refuses invalid replacement bytes");
    const auto removed_url = server.add(root / L"mod", "artwork/large-2.png");
    fs::remove(root / L"mod/artwork/large-2.png");
    if (!removed_url.empty()) check(fetch(removed_url).first == 404, "missing evicted artwork returns 404");
    ec.clear();
    fs::create_symlink(root / L"outside.png", root / L"mod/artwork/large-2.png", ec);
    if (!ec && !removed_url.empty()) check(fetch(removed_url).first == 404, "evicted artwork refuses a new symlink target");
    else if (ec) std::cout << "SKIP: reload symlink test needs Windows symlink privileges\n";
    std::string oversized = bytes;
    oversized.resize(4 * 1024 * 1024 + 1, '\0');
    { std::ofstream out(root / L"mod/artwork/oversized.png", std::ios::binary); out.write(oversized.data(), oversized.size()); }
    check(server.add(root / L"mod", "artwork/oversized.png").empty(), "individual images remain limited to 4 MiB");
    if (!failures) std::cout << "music artwork tests passed\n";
    return failures ? 1 : 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
