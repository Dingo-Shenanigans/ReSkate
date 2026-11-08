#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

// The songs' titles the trick feed leaves out (trick_feed.cpp). With in-game music on, the game's
// music writes the song's title to the field the trick list writes a trick's name to (0x0002), in
// another model, so every title came to plugins as an "n" note (reported 2026-11-03).
//
// 2.1.4-2 told the two apart by the UI model record of each write, and took a record as the trick
// list's once it wrote a side, landing, stance or modifier. But each field of a model has a record
// of its own (a field's handle resolves to the field's own record and type, as native_menu_data.cpp
// checks), so the name's record never became known and every name and spin was left out: plugins
// got only sides, landings and stances (seen on 2.1.4-2, 2026-11-04). Plain flips also write
// nothing but a name and spins on their entry ("Kickflip" with "180"), their landing on the next.
//
// So the titles are told apart by what they are instead: every song the game begins (its asset's
// title, and "artist - title") and what a Spotify station shows is kept here, and a name that reads
// as one of them is left out. A song not seen begin (none should be) comes out as it did before
// 2.1.4-2, rather than every trick being lost. Pure, so it tests.
namespace dingosdk::tricks::feed {
class SongTitles {
public:
    static constexpr std::size_t most = 64;

    // A song begun or shown: its title, and its artist when known.
    void add(std::string_view artist, std::string_view title) {
        const auto t = trimmed(title);
        if (t.empty()) return;
        keep(std::string(t));
        if (const auto a = trimmed(artist); !a.empty()) keep(std::string(a) + " - " + std::string(t));
    }
    // Whether a name the trick list's field was given is one of the songs' titles.
    bool is_song(std::string_view text) const {
        const auto t = trimmed(text);
        if (t.empty()) return false;
        for (const auto& title : titles_)
            if (same(title, t)) return true;
        return false;
    }
    std::size_t size() const noexcept { return titles_.size(); }

private:
    static std::string_view trimmed(std::string_view text) noexcept {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
        return text;
    }
    static char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
    static bool same(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (lower(a[i]) != lower(b[i])) return false;
        return true;
    }
    // The newest last; a title heard again moves to the end, the oldest goes when there are too many.
    void keep(std::string title) {
        for (auto it = titles_.begin(); it != titles_.end(); ++it)
            if (same(*it, title)) {
                titles_.erase(it);
                break;
            }
        titles_.push_back(std::move(title));
        if (titles_.size() > most) titles_.pop_front();
    }
    std::deque<std::string> titles_;
};
} // namespace dingosdk::tricks::feed
