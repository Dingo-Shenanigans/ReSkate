#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace dingosdk::text_overrides {
// The game's localized-text id hash (h = h * 33 + byte from 0xffffffff, 0x3325670).
std::uint32_t string_hash(std::string_view id) noexcept;
// A 7-character id that hashes to `hash`, for text known only by its hash: the store keys
// every entry by the hash of the id set() is given, so any id with that hash replaces it.
std::string id_for_hash(std::uint32_t hash);
// True when the string-override store, its set() and hasString match the supported build.
bool string_store_matches(std::uintptr_t base) noexcept;
// The string database the store's text is encoded against, or 0 while none is loaded.
std::uintptr_t string_database(std::uintptr_t base) noexcept;

// Game thread, every client tick (after apply_throwdown_strings): stores the text of every
// reskate-strings.json in the game's string-override store, layer 0, which every lookup
// checks before the shipped database -- so it replaces text the game ships as well as adding
// text it lacks. Read from %LOCALAPPDATA%\ReSkate\reskate-strings.json (the player's own,
// first) and then each enabled mod's folder (Mods/<mod>/reskate-strings.json, highest
// priority first); the first file to name an id wins. A file is one JSON object of
// {"ID_SOME_STRING": "UTF-8 text"}; a key "#1a2b3c4d" names a string by its hash, and keys
// starting with "_" are notes. Rechecked every 2 s: an edited file is read again and a new
// database (another language) gets the text stored again. An id taken out of a file keeps
// its text until the game restarts.
void apply_text_overrides(std::uintptr_t base) noexcept;
// RESKATE_STRINGS_DUMP set: %LOCALAPPDATA%\ReSkate\strings-dump.json gets every string of the
// loaded string database (walked once per change, decoded from its stored form to UTF-8), in
// the same format, ready to be translated. Keys are ids where known (seen by the key-based
// translate 0x331d8d0, or named in a reskate-strings.json), otherwise "#hash". This records
// the ids the translate resolves, and its text for ids the database lacks. Any thread; a
// no-op otherwise.
void observe_translation(const char* key, const char* text, std::uint32_t capacity) noexcept;
} // namespace dingosdk::text_overrides
