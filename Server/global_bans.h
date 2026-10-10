#pragma once
#include <cstddef>
#include <string>
#include <string_view>

// The ReSkate backend's ban list (its admin panel's Bans page), which a dedicated
// server enforces beside its own: Host::tick turns those players away
// (multiplayer::reskate_banned). The server reads the list at startup and every
// ten minutes; until it has, and while the backend cannot be reached, it bans
// nobody by it.
namespace dingosdk::server {
struct BanListCheck {
    bool ok{};
    bool changed{};       // the ban list is not the one read before (or is the first)
    std::size_t banned{}; // players on it
    // The chat word lists that came with it (word_lists.h): whether they are new, and their sizes.
    bool words_changed{};
    std::size_t filtered_words{}, forbidden_words{};
    std::string problem;  // why it could not be read, or empty
};
// Puts the backend's answer (the JSON of developer_identity.h) in use. An answer
// that is not the lists changes nothing.
BanListCheck use_ban_list(std::string_view answer);
// Asks the backend and does the same; blocks for a few seconds at most. Windows
// asks over WinHTTP; Linux runs curl, which has to be installed.
BanListCheck read_global_bans();
} // namespace dingosdk::server
