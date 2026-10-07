#include <winsock2.h>
#include <Windows.h>
#include "music_artwork.h"
#include "hpack_tables.h"
#include "Engine/Vfs/mod_music.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <deque>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dingosdk::profile_runtime {
namespace {
constexpr std::size_t max_image = 4 * 1024 * 1024, max_cached = 64 * 1024 * 1024;
constexpr unsigned short fixed_port = 47823; // stable origin so the game's HTTP cache is reused
constexpr std::size_t max_frame = 16384;

// HTTP/2 (RFC 7540) cleartext, because the game's HTTP client speaks h2c prior-knowledge.
constexpr char h2_preface[24] = {'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0',
                                 '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'};
constexpr std::uint8_t f_data = 0x0, f_headers = 0x1, f_settings = 0x4, f_ping = 0x6,
                       f_goaway = 0x7, f_window_update = 0x8, f_continuation = 0x9;
constexpr std::uint8_t flag_ack = 0x1, flag_end_stream = 0x1, flag_end_headers = 0x4, flag_padded = 0x8, flag_priority = 0x20;

struct Socket {
    SOCKET value = INVALID_SOCKET;
    Socket() = default;
    explicit Socket(SOCKET socket) : value(socket) {}
    Socket(Socket&& other) noexcept : value(other.value) { other.value = INVALID_SOCKET; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) { if (value != INVALID_SOCKET) closesocket(value); value = other.value; other.value = INVALID_SOCKET; }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};

bool send_all(SOCKET socket, const char* bytes, std::size_t count) {
    while (count) {
        const auto sent = send(socket, bytes, static_cast<int>(count), 0);
        if (sent <= 0) return false;
        bytes += sent;
        count -= static_cast<std::size_t>(sent);
    }
    return true;
}
bool png_ok(const std::vector<char>& bytes) {
    constexpr unsigned char magic[]{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33) return false;
    for (std::size_t i = 0; i < 8; ++i)
        if (static_cast<unsigned char>(bytes[i]) != magic[i]) return false;
    if (std::string_view(bytes.data() + 12, 4) != "IHDR") return false;
    const auto dimension = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = offset; i < offset + 4; ++i) value = (value << 8) | static_cast<unsigned char>(bytes[i]);
        return value;
    };
    const auto w = dimension(16), h = dimension(20);
    return w && h && w <= 2048 && h <= 2048;
}

// ---- minimal HPACK decoder (RFC 7541), enough to read the request's :path ----------------------
using Octets = std::span<const unsigned char>;
bool decode_integer(Octets data, std::size_t& at, unsigned prefix_bits, std::uint64_t& out) {
    if (at >= data.size()) return false;
    const std::uint64_t mask = (1u << prefix_bits) - 1;
    std::uint64_t value = data[at++] & mask;
    if (value < mask) { out = value; return true; }
    unsigned shift = 0;
    for (;;) {
        if (at >= data.size()) return false;
        const auto byte = data[at++];
        value += std::uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift > 56) return false;
    }
    out = value;
    return true;
}
bool huffman_decode(std::string_view input, std::string& out) {
    struct Node { std::int32_t child[2] = {-1, -1}; std::int32_t symbol = -1; };
    static const auto tree = [] {
        std::vector<Node> nodes(1);
        for (std::size_t symbol = 0; symbol < hpack::huffman_bits.size(); ++symbol) {
            const auto bits = hpack::huffman_bits[symbol];
            const auto code = hpack::huffman_code[symbol];
            if (!bits) continue;
            std::int32_t node = 0;
            for (int i = bits - 1; i >= 0; --i) {
                const auto bit = (code >> i) & 1;
                if (nodes[node].child[bit] < 0) {
                    nodes[node].child[bit] = static_cast<std::int32_t>(nodes.size());
                    nodes.emplace_back();
                }
                node = nodes[node].child[bit];
            }
            nodes[node].symbol = static_cast<std::int32_t>(symbol);
        }
        return nodes;
    }();
    std::int32_t node = 0;
    for (const auto byte : input) {
        for (int i = 7; i >= 0; --i) {
            node = tree[node].child[(static_cast<unsigned char>(byte) >> i) & 1];
            if (node < 0) return false;
            if (tree[node].symbol >= 0) {
                if (tree[node].symbol == 256) return false; // EOS must not appear in a value
                out.push_back(static_cast<char>(tree[node].symbol));
                node = 0;
            }
        }
    }
    return true; // trailing padding bits (EOS prefix) are ignored
}
bool decode_string(Octets data, std::size_t& at, std::string& out) {
    if (at >= data.size()) return false;
    const bool huffman = data[at] & 0x80;
    std::uint64_t length = 0;
    if (!decode_integer(data, at, 7, length) || at + length > data.size()) return false;
    const std::string_view bytes(reinterpret_cast<const char*>(data.data() + at), static_cast<std::size_t>(length));
    at += length;
    return huffman ? huffman_decode(bytes, out) : (out.assign(bytes), true);
}
// Returns the request's :path, or empty on a malformed block. Dynamic table state is per connection.
std::string decode_request_path(Octets block) {
    std::deque<std::pair<std::string, std::string>> dynamic;
    const auto name_at = [&](std::uint64_t index, std::string& name) {
        if (index == 0) return false;
        if (index <= 61) { name = hpack::static_table[index - 1].name; return true; }
        const auto position = index - 62;
        if (position >= dynamic.size()) return false;
        name = dynamic[position].first;
        return true;
    };
    const auto value_at = [&](std::uint64_t index, std::string& name, std::string& value) {
        if (index == 0) return false;
        if (index <= 61) { name = hpack::static_table[index - 1].name; value = hpack::static_table[index - 1].value; return true; }
        const auto position = index - 62;
        if (position >= dynamic.size()) return false;
        name = dynamic[position].first;
        value = dynamic[position].second;
        return true;
    };
    std::size_t at = 0;
    while (at < block.size()) {
        const auto byte = block[at];
        std::string name, value;
        if (byte & 0x80) {
            std::uint64_t index = 0;
            if (!decode_integer(block, at, 7, index) || !value_at(index, name, value)) return {};
        } else if ((byte & 0xc0) == 0x40) {
            std::uint64_t index = 0;
            if (!decode_integer(block, at, 6, index)) return {};
            if (index == 0) { if (!decode_string(block, at, name)) return {}; }
            else if (!name_at(index, name)) return {};
            if (!decode_string(block, at, value)) return {};
            dynamic.emplace_front(name, value);
        } else if ((byte & 0xe0) == 0x20) {
            std::uint64_t size = 0;
            if (!decode_integer(block, at, 5, size)) return {};
            if (size == 0) dynamic.clear();
            continue;
        } else {
            std::uint64_t index = 0;
            if (!decode_integer(block, at, 4, index)) return {};
            if (index == 0) { if (!decode_string(block, at, name)) return {}; }
            else if (!name_at(index, name)) return {};
            if (!decode_string(block, at, value)) return {};
        }
        if (name == ":path") return value;
    }
    return {};
}

// ---- HTTP/2 framing ----------------------------------------------------------------------------
std::uint32_t be32(const char* p) {
    return (std::uint32_t(static_cast<unsigned char>(p[0])) << 24) | (std::uint32_t(static_cast<unsigned char>(p[1])) << 16) |
           (std::uint32_t(static_cast<unsigned char>(p[2])) << 8) | std::uint32_t(static_cast<unsigned char>(p[3]));
}
bool send_frame(SOCKET socket, std::uint8_t type, std::uint8_t flags, std::uint32_t stream, std::span<const char> payload) {
    std::array<char, 9> header{};
    const auto length = payload.size();
    header[0] = static_cast<char>((length >> 16) & 0xff);
    header[1] = static_cast<char>((length >> 8) & 0xff);
    header[2] = static_cast<char>(length & 0xff);
    header[3] = static_cast<char>(type);
    header[4] = static_cast<char>(flags);
    header[5] = static_cast<char>((stream >> 24) & 0x7f);
    header[6] = static_cast<char>((stream >> 16) & 0xff);
    header[7] = static_cast<char>((stream >> 8) & 0xff);
    header[8] = static_cast<char>(stream & 0xff);
    return send_all(socket, header.data(), header.size()) && (payload.empty() || send_all(socket, payload.data(), payload.size()));
}
// HPACK literals without indexing (no Huffman). Static-table names use the indexed-name form.
void append_indexed_name(std::vector<char>& out, unsigned name_index, std::string_view value) {
    if (name_index < 15) out.push_back(static_cast<char>(name_index));
    else { out.push_back(0x0f); out.push_back(static_cast<char>(name_index - 15)); }
    out.push_back(static_cast<char>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
void append_new_name(std::vector<char>& out, std::string_view name, std::string_view value) {
    out.push_back(0x00);
    out.push_back(static_cast<char>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(static_cast<char>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
std::string etag_for(const std::vector<char>& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ull; }
    char text[24];
    std::snprintf(text, sizeof(text), "\"%016llx\"", static_cast<unsigned long long>(hash));
    return text;
}
// :status 200 (indexed 8), content-type (31), content-length (28), cache-control (24), etag.
std::vector<char> response_header_block(std::size_t length, const std::string& etag) {
    std::vector<char> out;
    out.push_back(static_cast<char>(0x88));
    append_indexed_name(out, 31, "image/png");
    append_indexed_name(out, 28, std::to_string(length));
    append_indexed_name(out, 24, "public, max-age=31536000, immutable");
    if (!etag.empty()) append_new_name(out, "etag", etag);
    return out;
}
constexpr char status_404_block = static_cast<char>(0x80 | 13); // :status 404 (static index 13)

struct Buffered {
    SOCKET socket{};
    std::string buffer;
    bool take(std::size_t count, std::string& out) {
        while (buffer.size() < count) {
            std::array<char, 16384> chunk{};
            const auto received = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (received <= 0) return false;
            buffer.append(chunk.data(), static_cast<std::size_t>(received));
        }
        out.assign(buffer, 0, count);
        buffer.erase(0, count);
        return true;
    }
};

std::string sanitize(std::string_view value) {
    std::string out;
    for (const auto c : value) out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return out;
}
std::string mod_folder_name(const std::filesystem::path& mod) {
    const auto name = mod.filename().wstring();
    std::string out;
    out.reserve(name.size());
    for (const auto c : name) out.push_back(static_cast<char>(c));
    return out;
}
} // namespace

struct MusicArtworkServer::Impl {
    struct Image {
        std::filesystem::path file;
        std::shared_ptr<const std::vector<char>> bytes;
        std::list<std::string>::iterator position;
    };
    bool winsock = false;
    Socket listener;
    unsigned short port = 0;
    std::mutex mutex;
    std::map<std::string, Image, std::less<>> images; // registered routes survive byte-cache eviction
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::string>> paths;
    std::list<std::string> cached; // most recently used first
    std::size_t cached_bytes = 0; // active responses can retain their own shared bytes
    std::vector<std::jthread> workers;

    Impl() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        winsock = true;
        listener = bind_listener(fixed_port);
        if (listener.value == INVALID_SOCKET) listener = bind_listener(0); // port taken: fall back
        if (listener.value == INVALID_SOCKET) return;
        const auto count = std::clamp(std::thread::hardware_concurrency(), 4u, 8u);
        for (unsigned i = 0; i < count; ++i)
            workers.emplace_back([this](std::stop_token stop) { serve(stop); });
    }
    ~Impl() {
        for (auto& worker : workers) worker.request_stop();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        if (winsock) WSACleanup();
    }

    Socket bind_listener(unsigned short wanted) {
        Socket bound{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (bound.value == INVALID_SOCKET) return {};
        const BOOL exclusive = TRUE;
        if (setsockopt(bound.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) return {};
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(wanted);
        if (bind(bound.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(bound.value, SOMAXCONN) != 0) return {};
        int size = sizeof(address);
        if (getsockname(bound.value, reinterpret_cast<sockaddr*>(&address), &size) != 0) return {};
        u_long nonblocking = 1; // several pollers share the listener; a racing accept must not block
        ioctlsocket(bound.value, FIONBIO, &nonblocking);
        port = ntohs(address.sin_port);
        return bound;
    }

    void serve(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(listener.value, &ready);
            timeval timeout{0, 100000};
            if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
            if (!FD_ISSET(listener.value, &ready)) continue;
            Socket client{accept(listener.value, nullptr, nullptr)};
            if (client.value == INVALID_SOCKET) continue;
            u_long blocking = 0;
            ioctlsocket(client.value, FIONBIO, &blocking); // accepted sockets inherit the listener mode
            const DWORD milliseconds = 2000;
            setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
            setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
            try { handle(client.value); } catch (...) {}
        }
    }

    void handle(SOCKET client) {
        std::array<char, sizeof(h2_preface)> peek{};
        for (;;) {
            const auto available = recv(client, peek.data(), static_cast<int>(peek.size()), MSG_PEEK);
            if (available <= 0) return;
            if (static_cast<std::size_t>(available) >= peek.size()) break;
        }
        if (std::memcmp(peek.data(), h2_preface, sizeof(h2_preface)) == 0) serve_h2c(client);
        else respond(client);
    }

    static std::shared_ptr<const std::vector<char>> read_png(const std::filesystem::path& file) {
        // A registered canonical file must not become a symlink to an unregistered file.
        if (std::filesystem::canonical(file) != file) return {};
        const auto size = std::filesystem::file_size(file);
        if (size > max_image) return {};
        std::ifstream input(file, std::ios::binary);
        auto bytes = std::make_shared<std::vector<char>>(static_cast<std::size_t>(size));
        if (!input.read(bytes->data(), static_cast<std::streamsize>(size)) || !png_ok(*bytes)) return {};
        return bytes;
    }

    // Called with mutex held. Keep the byte cache bounded without invalidating any URLs.
    void cache(const std::string& route, Image& image, std::shared_ptr<const std::vector<char>> bytes) {
        if (image.bytes) {
            cached_bytes -= image.bytes->size();
            cached.erase(image.position);
            image.bytes.reset();
        }
        while (cached_bytes + bytes->size() > max_cached && !cached.empty()) {
            auto& oldest = images.at(cached.back());
            cached_bytes -= oldest.bytes->size();
            oldest.bytes.reset();
            cached.pop_back();
        }
        cached.push_front(route);
        image.position = cached.begin();
        image.bytes = std::move(bytes);
        cached_bytes += image.bytes->size();
    }

    std::shared_ptr<const std::vector<char>> find(std::string_view route) {
        std::lock_guard lock(mutex);
        const auto found = images.find(route);
        if (found == images.end()) return {};
        auto& image = found->second;
        if (image.bytes) {
            cached.splice(cached.begin(), cached, image.position);
        } else {
            try {
                auto bytes = read_png(image.file);
                if (!bytes) return {};
                cache(found->first, image, std::move(bytes));
            } catch (const std::exception&) { return {}; }
        }
        return image.bytes;
    }

    // Graceful close: closing with unread frames queued makes Windows send RST and the client can
    // drop the response tail.
    static void finish(SOCKET client) {
        shutdown(client, SD_SEND);
        const DWORD drain_ms = 300;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&drain_ms), sizeof(drain_ms));
        std::array<char, 1024> discard{};
        while (recv(client, discard.data(), static_cast<int>(discard.size()), 0) > 0) {}
    }

    void serve_h2c(SOCKET client) {
        Buffered reader{client, {}};
        std::string preface;
        if (!reader.take(sizeof(h2_preface), preface) || !send_frame(client, f_settings, 0, 0, {})) return;
        const auto read_frame = [&](std::uint8_t& type, std::uint8_t& flags, std::uint32_t& stream, std::string& payload) {
            std::string header;
            if (!reader.take(9, header)) return false;
            const auto length = (std::size_t(static_cast<unsigned char>(header[0])) << 16) |
                                (std::size_t(static_cast<unsigned char>(header[1])) << 8) | std::size_t(static_cast<unsigned char>(header[2]));
            type = static_cast<std::uint8_t>(header[3]);
            flags = static_cast<std::uint8_t>(header[4]);
            stream = be32(header.data() + 5) & 0x7fffffffu;
            payload.clear();
            return length == 0 || reader.take(length, payload);
        };
        std::uint32_t request_stream = 0;
        std::int64_t connection_window = 65535, stream_window = 65535;
        std::string block;
        for (;;) {
            std::uint8_t type{}, flags{}; std::uint32_t stream{}; std::string payload;
            if (!read_frame(type, flags, stream, payload)) return;
            if (type == f_settings) {
                if (!(flags & flag_ack)) {
                    for (std::size_t at = 0; at + 6 <= payload.size(); at += 6)
                        if (((std::uint16_t(static_cast<unsigned char>(payload[at])) << 8) | static_cast<unsigned char>(payload[at + 1])) == 4)
                            stream_window = be32(payload.data() + at + 2);
                    if (!send_frame(client, f_settings, flag_ack, 0, {})) return;
                }
            } else if (type == f_ping && !(flags & flag_ack)) {
                if (!send_frame(client, f_ping, flag_ack, 0, payload)) return;
            } else if (type == f_window_update) {
                if (payload.size() >= 4) (stream == 0 ? connection_window : stream_window) += be32(payload.data()) & 0x7fffffffu;
            } else if (type == f_headers) {
                request_stream = stream;
                std::size_t at = 0;
                if ((flags & flag_padded) && at < payload.size()) at += 1 + static_cast<unsigned char>(payload[at]);
                if (flags & flag_priority) at += 5;
                if (at < payload.size()) block.append(payload, at, std::string::npos);
                if (flags & flag_end_headers) break;
            } else if (type == f_continuation) {
                block.append(payload);
                if (flags & flag_end_headers) break;
            } else if (type == f_goaway) {
                return;
            }
        }
        if (!request_stream) return;
        const auto route = decode_request_path(Octets(reinterpret_cast<const unsigned char*>(block.data()), block.size()));
        const auto image = route.empty() ? nullptr : find(route);
        if (!image) {
            send_frame(client, f_headers, flag_end_headers | flag_end_stream, request_stream, std::span<const char>(&status_404_block, 1));
            send_frame(client, f_goaway, 0, 0, {});
            finish(client);
            return;
        }
        const auto headers = response_header_block(image->size(), etag_for(*image));
        if (!send_frame(client, f_headers, flag_end_headers, request_stream, std::span<const char>(headers.data(), headers.size()))) return;
        std::size_t sent = 0;
        while (sent < image->size()) {
            if (connection_window <= 0 || stream_window <= 0) {
                std::uint8_t type{}, flags{}; std::uint32_t stream{}; std::string payload;
                if (!read_frame(type, flags, stream, payload)) return;
                if (type == f_window_update) {
                    if (payload.size() >= 4) (stream == 0 ? connection_window : stream_window) += be32(payload.data()) & 0x7fffffffu;
                } else if (type == f_ping && !(flags & flag_ack)) {
                    if (!send_frame(client, f_ping, flag_ack, 0, payload)) return;
                } else if (type == f_settings && !(flags & flag_ack)) {
                    if (!send_frame(client, f_settings, flag_ack, 0, {})) return;
                } else if (type == f_goaway) {
                    return;
                }
                continue;
            }
            const auto chunk = std::min({std::size_t(connection_window), std::size_t(stream_window), max_frame, image->size() - sent});
            const bool last = sent + chunk == image->size();
            if (!send_frame(client, f_data, last ? flag_end_stream : std::uint8_t(0), request_stream,
                            std::span<const char>(image->data() + sent, chunk))) return;
            sent += chunk;
            connection_window -= static_cast<std::int64_t>(chunk);
            stream_window -= static_cast<std::int64_t>(chunk);
        }
        send_frame(client, f_goaway, 0, 0, {});
        finish(client);
    }

    void respond(SOCKET client) {
        std::string request;
        std::array<char, 1024> buffer{};
        const auto started = GetTickCount64();
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            if (GetTickCount64() - started > 2000) return;
            const auto received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) return;
            request.append(buffer.data(), static_cast<std::size_t>(received));
        }
        const auto line = request.substr(0, request.find("\r\n"));
        std::shared_ptr<const std::vector<char>> image;
        if (request.find("\r\n\r\n") != std::string::npos && line.starts_with("GET ")) {
            const auto space = line.find(' ', 4);
            if (space != std::string::npos && (line.substr(space) == " HTTP/1.1" || line.substr(space) == " HTTP/1.0"))
                image = find(line.substr(4, space - 4));
        }
        if (!image) {
            constexpr std::string_view missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, missing.data(), missing.size());
            return;
        }
        const auto header = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + std::to_string(image->size()) +
            "\r\nCache-Control: public, max-age=31536000, immutable\r\nConnection: close\r\n\r\n";
        if (send_all(client, header.data(), header.size())) send_all(client, image->data(), image->size());
    }
};

MusicArtworkServer::MusicArtworkServer() : impl_(std::make_unique<Impl>()) {}
MusicArtworkServer::~MusicArtworkServer() = default;

std::string MusicArtworkServer::add(const std::filesystem::path& mod, const std::string& relative) {
    if (!impl_->winsock || impl_->port == 0 || !mods::music_artwork_path(relative)) return {};
    try {
        const auto root = std::filesystem::canonical(mod);
        const auto file = std::filesystem::canonical(root / std::filesystem::path(std::u8string(relative.begin(), relative.end())));
        auto a = root.begin(), b = file.begin();
        for (; a != root.end(); ++a, ++b)
            if (b == file.end() || _wcsicmp(a->c_str(), b->c_str()) != 0) return {};
        if (b == file.end()) return {};
        const auto stamp = std::filesystem::last_write_time(file);
        std::lock_guard lock(impl_->mutex);
        if (const auto found = impl_->paths.find(file); found != impl_->paths.end() && found->second.first == stamp)
            return found->second.second;
        auto bytes = Impl::read_png(file);
        if (!bytes) return {};
        // A stable, readable route so the URL is identical every launch (and the game's HTTP cache
        // can be reused) and the image identity is in the path, like the game's own CDN links.
        const auto route = "/music-art/" + sanitize(mod_folder_name(mod)) + "/" + relative;
        const auto url = "http://127.0.0.1:" + std::to_string(impl_->port) + route;
        auto& image = impl_->images[route];
        image.file = file;
        impl_->cache(route, image, std::move(bytes));
        impl_->paths[file] = {stamp, url};
        return url;
    } catch (const std::exception&) { return {}; }
}

std::string mod_music_artwork_url(const std::filesystem::path& mod, const std::string& relative) {
    // The injected runtime has process lifetime. Pin it before starting workers, so its code cannot
    // be unloaded while the image endpoint is serving requests. Avoid joining a worker from CRT
    // teardown under the loader lock. Tests use the owned server above.
    static auto* server = []() -> MusicArtworkServer* {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&mod_music_artwork_url), &module)) return nullptr;
        return new MusicArtworkServer;
    }();
    return server ? server->add(mod, relative) : std::string{};
}
} // namespace dingosdk::profile_runtime
