#pragma once
#include <cstddef>
#include <string>
#include <string_view>

// The chat word lists the ReSkate backend keeps (its admin panel's Words page), which come in
// the same answer as the identity lists (developer_identity.h): "filtered_words" and
// "forbidden_words". They take the place of the list the game was built with
// (Engine/Core/Text/word_filter.h) once the backend has any; an answer without them changes
// nothing, so an empty panel never switches the filter off.
namespace dingosdk::multiplayer {
struct WordListsRead {
    bool given{};   // the answer had the lists
    bool changed{}; // and they are not the ones already in use
    std::size_t filtered{}, forbidden{};
};
// Puts the lists of the backend's answer in use. Throws on an answer that is not JSON, or
// whose lists are not lists of words.
WordListsRead use_word_lists(std::string_view json);

// Saying a word that is not allowed at all: the message is not passed on, and the player is
// told so with a warning. After `warnings` of them, the next one gets them kicked.
inline constexpr unsigned word_warnings_default = 3, word_warnings_most = 10;
inline constexpr std::string_view word_kick_notice = "Kicked for using words that are not allowed here.";
// What the player is told at their `count`th warning of `warnings`.
std::string word_warning(unsigned count, unsigned warnings);
// And where nobody is warned or kicked (a server's "word_warnings": 0): the message is still
// not passed on.
inline constexpr std::string_view word_blocked_notice = "Your message was not sent: it has a word that is not allowed here.";
} // namespace dingosdk::multiplayer
