#pragma once
#include "share_code.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Skate Synchros messages (contrib/outfit-sharing.md). They travel on a Steam connection of their
// own between the two players, not through the session's host or server: a server never carries
// or sees anyone's outfit files, and who is on the other end is Steam's authenticated identity, so
// no message says who sent it.
//
// A viewer asks; the wearer answers:
//   Hello (session, proofs of the codes the viewer holds)  ->  Welcome (allowed or not)
//   Want (the worn items the viewer lacks)                  ->  Contents (each item's files, or why not)
//   Fetch (the files it does not have yet, by content id)   ->  Piece... (each file in pieces)
//   either side: Refuse (why it stops)
// Every message is one Steam message: u8 kind, u8 version, then its fields, little-endian.
// Anything malformed, oversized or out of order ends the exchange.
namespace dingosdk::multiplayer::synchros {
inline constexpr std::uint8_t wire_version = 1;
inline constexpr std::uint16_t steam_virtual_port = 7347;  // "SYNC" on a phone pad, near enough

inline constexpr std::size_t max_items = 64;              // worn items asked for at once
inline constexpr std::size_t max_files_per_item = 4096;
inline constexpr std::size_t max_name = 255;
inline constexpr std::size_t max_resource_meta = 64;
inline constexpr std::size_t max_fetch = 8192;            // files asked for at once
inline constexpr std::size_t max_piece = 16 * 1024;       // bytes of a file per Piece
inline constexpr std::uint32_t max_file = 32u << 20;      // one file
inline constexpr std::uint64_t max_outfit = 64ull << 20;  // everything sent for one request
inline constexpr std::size_t max_reason = 200;
// Steam's largest message (k_cbMaxSteamNetworkingSocketsMessageSizeSend). A Contents that would be
// larger is sent as several, an item or a few to each.
inline constexpr std::size_t max_message = 512 * 1024;

using ContentId = std::array<std::uint8_t, 20>;  // SHA-1 of the file as stored (encoded)

enum class Kind : std::uint8_t { hello = 1, welcome = 2, want = 3, contents = 4, fetch = 5, piece = 6, refuse = 7 };

struct Hello {
    std::uint64_t session{};
    std::vector<Proof> proofs;  // at most max_entered_codes
};
struct Welcome {
    bool allowed{};
};
struct Want {
    std::vector<std::string> items;  // asset keys from the wearer's outfit packet
};

// One file of an item: where it goes and what it is, so the viewer can rebuild the bundles
// without the wearer's mods.
enum class FileKind : std::uint8_t { ebx = 0, resource = 1, chunk = 2, toc_chunk = 3 };
struct File {
    ContentId id{};
    std::uint32_t size{};          // encoded, as sent
    FileKind kind{};
    std::string name;              // ebx and resource name; empty for chunks
    std::string bundle;            // the bundle it is in ("win32/..."); empty for a toc chunk
    std::string superbundle;       // "win32/items", "win32/globals"
    std::uint64_t original_size{};
    std::uint32_t resource_type{};
    std::uint64_t resource_id{};
    std::vector<std::uint8_t> resource_meta;
    std::array<std::uint8_t, 16> chunk_guid{};
    std::uint32_t logical_offset{}, logical_size{};
    bool operator==(const File&) const = default;
};
// Why an item is not sent.
enum class Withheld : std::uint8_t {
    none = 0,
    not_worn = 1,       // not in the outfit the wearer has on now
    store_item = 2,     // a copy of something the game sells
    too_large = 3,      // over max_outfit
    game_item = 4,      // the game's own: the viewer has it
    unreadable = 5,
};
// A preset of the item and the bundle that loads it, for the viewer's bundle-reference table
// (Engine/Resource/bundle_ref_table.h: with_presets). The bundle is an index into the game's own
// bundle list, the same on every PC with this build.
struct TableRow {
    std::string table;   // the table resource ("characters/customization/configs/cas_main_bundlereftable")
    std::string path;    // the preset
    std::uint32_t bundle{};
    bool operator==(const TableRow&) const = default;
};
struct ItemContents {
    std::string item;
    Withheld withheld{};
    std::vector<File> files;      // empty when withheld
    std::vector<TableRow> rows;   // empty when withheld
};
struct Contents {
    std::vector<ItemContents> items;
};
struct Fetch {
    std::vector<ContentId> files;
};
struct Piece {
    ContentId id{};
    std::uint32_t offset{}, total{};
    std::vector<std::uint8_t> bytes;
};
struct Refuse {
    std::string reason;
};
using Message = std::variant<Hello, Welcome, Want, Contents, Fetch, Piece, Refuse>;

// Throws std::length_error when the message would be over max_message.
std::vector<std::uint8_t> encode(const Message& message);
// Nothing when the bytes are not a well-formed message of this version within the limits.
std::optional<Message> decode(std::span<const std::uint8_t> bytes);

// The total of every file in a Contents, to hold against max_outfit.
std::uint64_t contents_bytes(const Contents& contents) noexcept;
} // namespace dingosdk::multiplayer::synchros
