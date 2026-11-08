#include "received.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <variant>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/sha.h>
#endif

namespace fs = std::filesystem;

namespace dingosdk::multiplayer::synchros {

ContentId content_id(std::span<const std::uint8_t> bytes) {
    ContentId out{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("No SHA-1 for a received file.");
    BCRYPT_HASH_HANDLE hash{};
    const auto made = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    NTSTATUS hashed = made;
    if (made >= 0) {
        hashed = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0);
        if (hashed >= 0) hashed = BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (hashed < 0) throw std::runtime_error("No SHA-1 for a received file.");
#else
    SHA1(bytes.data(), bytes.size(), out.data());
#endif
    return out;
}

std::string hex(const ContentId& id) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(id.size() * 2);
    for (const auto b : id) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// ---- Arriving ----

Arriving::Arriving(const File& expected) : id_(expected.id), size_(expected.size) {
    bytes_.reserve(std::min<std::size_t>(size_, max_file));
}

Arriving::Took Arriving::take(const Piece& piece) {
    if (done_ || piece.id != id_ || piece.total != size_ || size_ > max_file) return Took::refused;
    if (piece.offset != bytes_.size()) return Took::refused;  // in order, no gaps, no overlaps
    const auto left = static_cast<std::size_t>(size_) - bytes_.size();
    if (piece.bytes.empty() || piece.bytes.size() > max_piece || piece.bytes.size() > left) return Took::refused;
    if (piece.bytes.size() < max_piece && piece.bytes.size() != left) return Took::refused;  // only the last is short
    bytes_.insert(bytes_.end(), piece.bytes.begin(), piece.bytes.end());
    if (bytes_.size() < size_) return Took::more;
    done_ = true;
    if (content_id(bytes_) != id_) {
        bytes_.clear();
        return Took::refused;
    }
    return Took::whole;
}

// ---- Cache ----

namespace {

std::optional<std::vector<std::uint8_t>> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return out;
}

// Written beside, then renamed over: a file is in the cache whole or not at all.
bool write_all(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out.close();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) fs::remove(temporary, ec);
    return !ec;
}

std::optional<ContentId> id_from_hex(std::string_view text) {
    if (text.size() != 40) return std::nullopt;
    ContentId id{};
    for (std::size_t i = 0; i < 20; ++i) {
        int value = 0;
        for (int half = 0; half < 2; ++half) {
            const char c = text[i * 2 + static_cast<std::size_t>(half)];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return std::nullopt;
            value = value * 16 + digit;
        }
        id[i] = static_cast<std::uint8_t>(value);
    }
    return id;
}

// A wearer's listing: each item as a Contents message of its own, behind its length.
std::vector<std::uint8_t> encode_listing(const std::vector<ItemContents>& items) {
    std::vector<std::uint8_t> out;
    for (const auto& item : items) {
        std::vector<std::uint8_t> one;
        try {
            one = encode(Message{Contents{{item}}});
        } catch (const std::length_error&) {
            continue;  // could not have arrived in one piece either
        }
        const auto size = static_cast<std::uint32_t>(one.size());
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

std::vector<ItemContents> decode_listing(std::span<const std::uint8_t> bytes) {
    std::vector<ItemContents> items;
    std::size_t at = 0;
    while (bytes.size() - at >= 4) {
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i) size |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4;
        if (size > bytes.size() - at) return {};  // a damaged listing is no listing
        const auto message = decode(bytes.subspan(at, size));
        at += size;
        const auto* contents = message ? std::get_if<Contents>(&*message) : nullptr;
        if (!contents) return {};
        items.insert(items.end(), contents->items.begin(), contents->items.end());
    }
    return at == bytes.size() ? items : std::vector<ItemContents>{};
}

} // namespace

Cache::Cache(fs::path root, std::uint64_t limit) : root_(std::move(root)), limit_(limit) {
    std::error_code ec;
    // Left by a write that never finished.
    if (fs::exists(root_ / "files", ec))
        for (auto it = fs::recursive_directory_iterator(root_ / "files", ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->path().extension() == ".part") fs::remove(it->path(), ec);
}

fs::path Cache::file_path(const ContentId& id) const {
    const auto name = hex(id);
    return root_ / "files" / name.substr(0, 2) / name;
}

fs::path Cache::wearer_path(std::uint64_t wearer) const { return root_ / "wearers" / (std::to_string(wearer) + ".bin"); }

bool Cache::has(const ContentId& id) const {
    std::error_code ec;
    return fs::is_regular_file(file_path(id), ec);
}

bool Cache::put(const ContentId& id, std::span<const std::uint8_t> bytes) {
    if (bytes.size() > max_file || content_id(bytes) != id) return false;
    if (has(id)) return true;
    return write_all(file_path(id), bytes);
}

std::optional<std::vector<std::uint8_t>> Cache::read(const ContentId& id) {
    const auto path = file_path(id);
    auto bytes = read_all(path);
    if (!bytes) return std::nullopt;
    // A file that no longer hashes to its name was damaged on disk: it goes, and is fetched again.
    std::error_code ec;
    if (content_id(*bytes) != id) {
        fs::remove(path, ec);
        return std::nullopt;
    }
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // used: last to go
    return bytes;
}

void Cache::remember(std::uint64_t wearer, const std::vector<ItemContents>& items) {
    write_all(wearer_path(wearer), encode_listing(items));
}

std::vector<ItemContents> Cache::listing(std::uint64_t wearer) const {
    const auto bytes = read_all(wearer_path(wearer));
    return bytes ? decode_listing(*bytes) : std::vector<ItemContents>{};
}

std::vector<std::uint64_t> Cache::wearers() const {
    std::vector<std::uint64_t> out;
    std::error_code ec;
    if (!fs::is_directory(root_ / "wearers", ec)) return out;
    for (auto it = fs::directory_iterator(root_ / "wearers", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".bin") continue;
        const auto stem = it->path().stem().string();
        if (stem.empty() || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        try {
            out.push_back(std::stoull(stem));
        } catch (...) {}
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ContentId> Cache::missing(std::uint64_t wearer) const {
    std::set<ContentId> wanted;
    for (const auto& item : listing(wearer))
        for (const auto& file : item.files)
            if (!has(file.id)) wanted.insert(file.id);
    return {wanted.begin(), wanted.end()};
}

std::set<ContentId> Cache::named(const std::set<std::uint64_t>& wearers) const {
    std::set<ContentId> ids;
    for (const auto wearer : wearers)
        for (const auto& item : listing(wearer))
            for (const auto& file : item.files) ids.insert(file.id);
    return ids;
}

void Cache::remove_unnamed() {
    const auto all = wearers();
    const auto kept = named({all.begin(), all.end()});
    std::error_code ec;
    if (!fs::is_directory(root_ / "files", ec)) return;
    for (auto it = fs::recursive_directory_iterator(root_ / "files", ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto id = id_from_hex(it->path().filename().string());
        if (!id || !kept.contains(*id)) fs::remove(it->path(), ec);
    }
}

void Cache::forget(std::uint64_t wearer) {
    std::error_code ec;
    fs::remove(wearer_path(wearer), ec);
    remove_unnamed();
}

void Cache::empty() {
    std::error_code ec;
    fs::remove_all(root_ / "files", ec);
    fs::remove_all(root_ / "wearers", ec);
}

std::uint64_t Cache::size() const {
    std::uint64_t total = 0;
    std::error_code ec;
    if (!fs::is_directory(root_ / "files", ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(root_ / "files", ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

void Cache::trim(const std::set<std::uint64_t>& keep) {
    auto total = size();
    if (total <= limit_) return;
    const auto kept = named(keep);
    struct Entry {
        fs::path path;
        fs::file_time_type used;
        std::uint64_t size;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root_ / "files", ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto id = id_from_hex(it->path().filename().string());
        if (id && kept.contains(*id)) continue;
        entries.push_back({it->path(), it->last_write_time(ec), it->file_size(ec)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const auto& entry : entries) {
        if (total <= limit_) break;
        if (fs::remove(entry.path, ec)) total -= std::min(total, entry.size);
    }
}

} // namespace dingosdk::multiplayer::synchros
