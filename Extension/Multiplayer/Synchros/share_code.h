#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Skate Synchros codes (contrib/outfit-sharing.md). Codes are the only way in: nobody sees a
// player's modded outfit without a code from them, Steam friend or not.
//
// Two kinds, told apart by their prefix:
//   - a personal code, "RSK-7Q4M-K2XD-9PW3": whoever has it sees its owner's outfit;
//   - a group code, "RSG-...": everyone who has it sees everyone else's who has it.
// Twelve Crockford base32 characters in three groups: eleven random (55 bits) and a check
// character, so a mistyped code is told apart from one that simply belongs to nobody here. Reading
// one ignores case, dashes and spaces, and takes O for 0 and I or L for 1.
//
// A code never crosses the network. Each side keeps its key (SHA-256 of the kind and the twelve
// characters), and a player shows it holds a code with a proof: HMAC-SHA256 under that key of the
// session and both players' SteamIDs. A proof is only good for that pair in that session, so it
// cannot be replayed by anyone else or later.
namespace dingosdk::multiplayer::synchros {
using CodeKey = std::array<std::uint8_t, 32>;
using Proof = std::array<std::uint8_t, 32>;

enum class CodeKind : std::uint8_t { personal, group };
inline constexpr std::string_view personal_prefix = "RSK";
inline constexpr std::string_view group_prefix = "RSG";
inline constexpr std::size_t code_symbols = 12;  // eleven random and the check

inline constexpr std::string_view prefix_of(CodeKind kind) noexcept {
    return kind == CodeKind::group ? group_prefix : personal_prefix;
}

// A new code, from the system's random generator, as shown ("RSK-7Q4M-K2XD-9PW3"). Throws when no
// random numbers are available.
std::string make_code(CodeKind kind);
// The twelve characters of a code of `kind` as typed ("rsk 7q4m-k2xd 9pw3", "7Q4MK2XD9PW3"),
// canonical and upper case, or nothing when it is not one: wrong length, a character outside the
// alphabet, a check character that does not match, or the other kind's prefix.
std::optional<std::string> read_code(std::string_view typed, CodeKind kind);
// The kind a typed code says it is by its prefix, if it has one.
std::optional<CodeKind> typed_kind(std::string_view typed) noexcept;
// "RSK-7Q4M-K2XD-9PW3" from the twelve characters.
std::string show_code(std::string_view symbols, CodeKind kind);
// The key a code is kept and proved with. A personal and a group code with the same characters
// have different keys.
CodeKey code_key(std::string_view symbols, CodeKind kind);

// What a player sends to show it holds a code.
Proof code_proof(const CodeKey& key, std::uint64_t session, std::uint64_t wearer, std::uint64_t viewer);
// The check of a proof against a key held here, in constant time.
bool proof_matches(const CodeKey& key, const Proof& proof, std::uint64_t session, std::uint64_t wearer,
                   std::uint64_t viewer) noexcept;
} // namespace dingosdk::multiplayer::synchros
