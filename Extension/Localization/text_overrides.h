#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The game's localized text, as plugins change it (reskate.strings): each id's text stored in the
// game's own string-override store, which every lookup checks before the shipped database, so it
// replaces text the game ships as well as adding text it lacks.
namespace dingosdk::text_overrides {
// The game's localized-text id hash (h = h * 33 + byte from 0xffffffff, 0x3325670).
constexpr std::uint32_t string_hash(std::string_view id) noexcept {
    std::uint32_t hash = 0xffffffffU;
    for (const char c : id) hash = hash * 33 + static_cast<unsigned char>(c);
    return hash;
}
// A 7-character id that hashes to `hash`, for text known only by its hash: the store keys
// every entry by the hash of the id set() is given, so any id with that hash replaces it.
std::string id_for_hash(std::uint32_t hash);
// True when the string-override store, its set() and hasString match the supported build.
bool string_store_matches(std::uintptr_t base) noexcept;
// The string database the store's text is encoded against, or 0 while none is loaded.
std::uintptr_t string_database(std::uintptr_t base) noexcept;

// The most texts one owner holds, and the longest text: set() converts it into a buffer of 0x400
// bytes (localization.h), so at most 511 UTF-16 units.
inline constexpr std::size_t most_texts = 65536;
inline constexpr std::size_t most_units = 511;
// Game thread. The owner's (a plugin's) texts by key: an id, or "#" and 1 to 8 hex digits for a
// string known only by its hash (keys and texts checked by the caller). The last owner to set an
// id shows; its text is stored a batch a tick by apply_text_overrides. Empty, or why not.
std::string set(std::string_view owner, const std::vector<std::pair<std::string, std::string>>& texts);
// Game thread: the owner's texts are no longer kept. An id another owner set shows that owner's
// text again; any other keeps the owner's text until the game restarts, since the store has no
// removal (nor does a new database bring the shipped text back for it until then).
void release(std::string_view owner);
// Game thread: whether the game has text for the key (in the store or the database); nullopt
// before the store is checked, or when it differs from the supported build.
std::optional<bool> exists(std::string_view key);
// Casing rules for the text the UI capitalizes itself (a style's Uppercase, Lowercase, Title or
// Reverse): "tr" (Turkish and Azerbaijani) uppercases i to dotted I (U+0130) and lowercases I
// to dotless i (U+0131), as the game's own mapping (localization.h case_convert) has no language rules; "" is
// the game's mapping again. Game thread. The rules hold while any owner that asked for them is
// loaded (release gives them up); the hook is installed at the next tick after the first asks.
// Empty when taken, else why not.
std::string set_case_rules(std::string_view owner, std::string_view rules);
// What the Turkish rules make of one character `source` that the game's mapping in `mode`
// (localization.h case_lower or case_upper) turned into `converted`.
constexpr char16_t turkish_case(char16_t source, char16_t converted, int mode) noexcept {
    if (mode == 2 && source == u'i' && converted == u'I') return char16_t{0x130};
    if (mode == 1 && source == u'I' && converted == u'i') return char16_t{0x131};
    return converted;
}
// Game thread, every client tick (after apply_throwdown_strings): stores what changed, 256
// texts a tick, so a whole translation is no hitch. Text is stored once a database is loaded,
// and all of it again when another database loads (another language). Also installs the casing
// hook once casing rules are asked for.
void apply_text_overrides(std::uintptr_t base) noexcept;
} // namespace dingosdk::text_overrides
