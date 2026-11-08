// The trick feed's song titles (Extension/Skater/trick_feed_songs.h), apart from the game: a song's
// title that reaches the trick list's name field is told apart from a trick's name, and a trick's
// name always goes out.
#include "Extension/Skater/trick_feed_songs.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dingosdk::tricks::feed;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main() {
    try {
        SongTitles songs;
        // Before any song begins, every name is a trick's (as before 2.1.4-2).
        check(!songs.is_song("Kickflip") && !songs.is_song("Uptown Top Ranking"), "a name was a song before any began");
        // A song begun: its title and "artist - title" are songs, whatever the case and spaces.
        songs.add("Althea & Donna", "Uptown Top Ranking");
        check(songs.is_song("Uptown Top Ranking") && songs.is_song(" uptown top ranking ") && songs.is_song("Althea & Donna - Uptown Top Ranking"),
            "a begun song's title was not a song");
        // Tricks still go out: plain flips (only a name on their entry), spins and grinds alike.
        for (const char* trick : {"Kickflip", "360 Flip", "Revert", "Crooked", "Pop-Shuvit", "Christ Air", "Uptown"})
            check(!songs.is_song(trick), "a trick's name was taken for a song");
        // An empty or blank text is never a song, nor is a song without a title kept.
        songs.add("Someone", "  ");
        check(!songs.is_song("") && !songs.is_song("   ") && !songs.is_song("Someone - "), "a blank title was kept");
        // A song without an artist (a Spotify station's when it has none) keeps its title only.
        songs.add("", "Doomswitch");
        check(songs.is_song("Doomswitch") && !songs.is_song(" - Doomswitch"), "a song without an artist was kept wrong");
        // Only so many are kept: the oldest goes first, a song heard again stays.
        SongTitles few;
        few.add("", "First");
        for (std::size_t i = 0; i < SongTitles::most; ++i) {
            few.add("", "Song " + std::to_string(i));
            few.add("", "First"); // heard again: moves to the newest
        }
        check(few.size() == SongTitles::most, "more titles than the most were kept");
        check(few.is_song("First") && !few.is_song("Song 0") && few.is_song("Song " + std::to_string(SongTitles::most - 1)),
            "the oldest title did not go first");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "trick feed song tests passed\n";
    return 0;
}
