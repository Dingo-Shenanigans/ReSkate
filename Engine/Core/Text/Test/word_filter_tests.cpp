#include "Engine/Core/Text/word_filter.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
}

void bad(std::string_view text) { expect(dingosdk::text::contains_bad_words(text), std::string("bad: ") + std::string(text)); }
void clean(std::string_view text) { expect(!dingosdk::text::contains_bad_words(text), std::string("clean: ") + std::string(text)); }
void masks(std::string_view text, std::string_view expected) {
    const auto masked = dingosdk::text::mask_bad_words(text);
    expect(masked == expected, std::string("mask: \"") + std::string(text) + "\" -> \"" + masked + "\", wanted \"" +
                                   std::string(expected) + "\"");
}
} // namespace

int main() {
    // Listed words, any case, whole or inside longer words.
    bad("fuck");
    bad("FUCK this server");
    bad("Fuckface Skaters");
    bad("motherfuckers only");
    bad("Shithead's park");
    bad("big ass ramps");
    bad("Bitch Please");
    // Look-alikes and spelled-out letters.
    bad("sh1t");
    bad("@$$ hats");
    bad("f u c k");
    bad("F.U.C.K. yeah");
    bad("b17ch");
    bad("evil ass rape server");
    bad("Rapist Crew");
    bad("gangrape lobby");
    // Ordinary words that contain a listed word, and server names people use.
    clean("Hello World");
    clean("Shell Shock Skatepark");
    clean("Classic Skate Session");
    clean("Pass the Grass");
    clean("Scunthorpe Skaters");
    clean("Cocktail Hour");
    clean("Peacock Plaza");
    clean("Dickies Team Session");
    clean("Arsenal FC fans");
    clean("Sparse Parsec");
    clean("Saturday Night Session");
    clean("Reputation Skate");
    clean("San Vansterdam 24/7");
    clean("Titanic Ledges");
    clean("Cumulus Bowl");
    clean("Essex Street League");
    clean("Pakistan Plaza");
    clean("Scrapyard DIY");
    clean("Grape Street Bowl");
    clean("Scraped Knees Crew");
    clean("Trapeze Transfers");
    clean("Draping the rails");
    clean("Physical Therapist Pipe");
    clean("ReSkate server");
    clean("");
    // Masking keeps everything else and the length.
    masks("what the fuck dude", "what the **** dude");
    masks("shit happens", "**** happens");
    masks("sh1t", "****");
    masks("f u c k", "* * * *");
    masks("Hello friends", "Hello friends");
    masks("fuckface", "****face");
    // The backend's lists take the built-in one's place: its words are no longer filtered,
    // theirs are, and the ones not allowed at all are both filtered and told apart.
    {
        using namespace dingosdk::text;
        const auto expect = [&](bool ok, const char* what) {
            if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
        };
        expect(!contains_forbidden_words("what the fuck"), "a word was forbidden before any list said so");
        const std::vector<std::string> filtered{"Darn", "heck"}, forbidden{"Zorblat", "@$$hat"};
        set_word_lists(filtered, forbidden);
        expect(!contains_bad_words("what the fuck") && contains_bad_words("oh D4RN it") && contains_bad_words("check"),
               "the given filtered words are not the ones filtered");
        expect(contains_forbidden_words("you z0rblat") && contains_forbidden_words("megazorblatter") && contains_forbidden_words("a s s h a t") &&
                   !contains_forbidden_words("darn heck") && !contains_forbidden_words("zorb lat"),
               "the words not allowed at all are not matched as the filter matches");
        expect(mask_bad_words("darn you zorblat") == "**** you *******", "a forbidden word is not masked like a filtered one");
        set_word_lists({}, {});
        expect(!contains_bad_words("darn zorblat fuck") && !contains_forbidden_words("zorblat") && mask_bad_words("darn") == "darn",
               "empty lists still filter");
        reset_word_lists();
        expect(contains_bad_words("what the fuck") && !contains_bad_words("darn") && !contains_forbidden_words("zorblat"),
               "the built-in list did not come back");
    }
    if (failures == 0) std::printf("word filter: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
