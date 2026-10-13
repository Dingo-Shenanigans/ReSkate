#pragma once
#include <span>
#include <string>
#include <string_view>

// The bad-word filter: a list of words matched against text after folding case and look-alike
// characters, so "Sh1t", "@$$" and "f u c k" count too. The words are the ReSkate backend's
// (set_word_lists), in two lists: the filtered words, and the ones not allowed in chat at all.
// Both are filtered. Until they arrive there are none, and nothing is filtered.
//
// - Words of three letters or fewer, and a few longer ones that are common inside ordinary
//   words ("hell" in hello, "arse" in parse), only match a whole word.
// - A word of six letters or more also matches anywhere inside a longer one, unless an ordinary
//   word covers it there ("snigger", "retardant").
// - A word of four or five letters matches inside a longer one only when the rest of it is what
//   goes around a swear word ("fuckface", "motherfuckers", "bullshit"), so not in "spike",
//   "raccoon" or "script".
//
// Used for dedicated server names (the server refuses to list one, the client hides it) and,
// as a player option, to mask chat. A dedicated server and a lobby's host do not pass on a chat
// message with a word that is not allowed at all, and warn and then kick who said it.
namespace dingosdk::text {
bool contains_bad_words(std::string_view text);
// One of the words not allowed at all. There are none until set_word_lists gives some.
bool contains_forbidden_words(std::string_view text);
// The lists in use from now on (any thread).
void set_word_lists(std::span<const std::string> filtered, std::span<const std::string> forbidden);
// Back to no lists: nothing filtered or forbidden but a server's own words.
void reset_word_lists();
// A dedicated server's own words, on top of the lists above: more filtered words, and more
// that are not allowed at all (Server/server_words.h). The server sends its filtered ones to
// its players (Packet words), whose games give them here while they are on it, and none again
// when they leave.
// - `use_given` false (a server's "use_global_word_filter") leaves the backend's lists out:
//   only the server's own words are filtered and forbidden.
void set_own_words(std::span<const std::string> filtered, std::span<const std::string> forbidden, bool use_given = true);

// `text` with each letter of every bad word replaced by '*'; same length, other text untouched.
std::string mask_bad_words(std::string_view text);
} // namespace dingosdk::text
