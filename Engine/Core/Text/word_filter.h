#pragma once
#include <span>
#include <string>
#include <string_view>

// The bad-word filter: a list of words matched against text after folding case and look-alike
// characters, so "Sh1t", "@$$" and "f u c k" count too. The list is the one in bad_words.txt
// (built in) until the ReSkate backend's lists arrive (set_word_lists), which are two: the
// filtered words, and the ones not allowed in chat at all. Both are filtered.
//
// - Words of three letters or fewer, and a few longer ones that are common inside ordinary
//   words ("hell" in hello, "arse" in parse), only match a whole word.
// - Every other word also matches inside a longer one ("fuckface"), unless an ordinary word
//   covers it there ("Scunthorpe", "cocktail").
//
// Used for dedicated server names (the server refuses to list one, the client hides it) and,
// as a player option, to mask chat. A dedicated server and a lobby's host do not pass on a chat
// message with a word that is not allowed at all, and warn and then kick who said it.
namespace dingosdk::text {
bool contains_bad_words(std::string_view text);
// One of the words not allowed at all. There are none until set_word_lists gives some.
bool contains_forbidden_words(std::string_view text);
// The lists in use from now on (any thread), in place of the built-in one.
void set_word_lists(std::span<const std::string> filtered, std::span<const std::string> forbidden);
// Back to the built-in list, with nothing forbidden.
void reset_word_lists();

// `text` with each letter of every bad word replaced by '*'; same length, other text untouched.
std::string mask_bad_words(std::string_view text);
} // namespace dingosdk::text
