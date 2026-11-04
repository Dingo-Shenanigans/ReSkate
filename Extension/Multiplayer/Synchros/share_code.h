#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Skate Synchros share codes (contrib/outfit-sharing.md): a player gives their code to someone who
// is not a Steam friend, and that person then sees their modded outfit.
//
// A code is "RSK-" and twelve Crockford base32 characters in three groups, "RSK-7Q4M-K2XD-9PW3":
// eleven random (55 bits) and a check character, so a mistyped code is told apart from one that
// simply belongs to nobody here. Reading one ignores case, dashes and spaces, and takes O for 0
// and I or L for 1, as Crockford's alphabet does.
//
// The code itself never crosses the network. Each side keeps its key (SHA-256 of the code's twelve
// characters), and a viewer shows it holds a wearer's code with a proof: HMAC-SHA256 under that key
// of the session and both players' SteamIDs. A proof is only good for that pair in that session, so
// it cannot be replayed by anyone else or later.
namespace dingosdk::multiplayer::synchros {
using CodeKey = std::array<std::uint8_t, 32>;
using Proof = std::array<std::uint8_t, 32>;

inline constexpr std::string_view code_prefix = "RSK-";
inline constexpr std::size_t code_symbols = 12;  // eleven random and the check

// A new code, from the system's random generator. Throws when none is available.
std::string make_code();
// The twelve characters of a code as typed ("rsk 7q4m-k2xd 9pw3", "7Q4MK2XD9PW3"), canonical and
// upper case, or nothing when it is not a code: wrong length, a character outside the alphabet,
// or a check character that does not match.
std::optional<std::string> read_code(std::string_view typed);
// "RSK-7Q4M-K2XD-9PW3" from the twelve characters.
std::string show_code(std::string_view symbols);
// The key a code is kept and proved with. `symbols` as read_code gives them.
CodeKey code_key(std::string_view symbols);

// What a viewer sends to show it holds the wearer's code.
Proof code_proof(const CodeKey& key, std::uint64_t session, std::uint64_t wearer, std::uint64_t viewer);
// The wearer's check of a proof against its own current code, in constant time.
bool proof_matches(const CodeKey& own, const Proof& proof, std::uint64_t session, std::uint64_t wearer,
                   std::uint64_t viewer) noexcept;
} // namespace dingosdk::multiplayer::synchros
