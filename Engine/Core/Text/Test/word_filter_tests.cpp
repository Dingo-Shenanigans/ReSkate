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
    // The filter has no words of its own: until a list arrives nothing is filtered. These stand
    // in for the backend's.
    clean("what the fuck");
    {
        const std::vector<std::string> listed{"fuck", "fucking", "motherfucker", "shit", "ass", "bitch", "rape", "raped",
                                              "raping", "rapist", "gangrape", "dick", "cock", "cunt", "hell", "arse",
                                              "perse", "tit", "cum", "sex", "paki", "crap", "turd", "puta"};
        dingosdk::text::set_word_lists(listed, {});
    }
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
    bad("bullshit");
    bad("dumbfucks");
    bad("shitty park");
    bad("fuckkkk");
    bad("xxfuckxx");
    bad("Dickhead Crew");
    bad("absofuckinglutely");
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
    // New lists take the place of the ones before: those words are no longer filtered, the new
    // ones are, and the ones not allowed at all are both filtered and told apart.
    {
        using namespace dingosdk::text;
        const auto expect = [&](bool ok, const char* what) {
            if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
        };
        expect(!contains_forbidden_words("what the fuck"), "a word was forbidden before any list said so");
        const std::vector<std::string> filtered{"Darn", "heck"}, forbidden{"Zorblat", "@$$hat"};
        set_word_lists(filtered, forbidden);
        expect(!contains_bad_words("what the fuck") && contains_bad_words("oh D4RN it") && contains_bad_words("heckin") &&
                   !contains_bad_words("check"),
               "the given filtered words are not the ones filtered");
        expect(contains_forbidden_words("you z0rblat") && contains_forbidden_words("megazorblatter") && contains_forbidden_words("a s s h a t") &&
                   !contains_forbidden_words("darn heck") && !contains_forbidden_words("zorb lat"),
               "the words not allowed at all are not matched as the filter matches");
        expect(mask_bad_words("darn you zorblat") == "**** you *******", "a forbidden word is not masked like a filtered one");
        // Short entries, and fragments of words, as the backend's lists have them: they are in
        // many ordinary words, and count only as the word being said.
        const std::vector<std::string> fragments{"gger", "igger", "spik", "nega", "negar", "crip", "coon", "coons", "raper",
                                                 "nige", "niger", "nicker", "nickers", "hom0", "nigger", "the rapist"};
        set_word_lists({}, fragments);
        for (const char* ordinary :
             {"a bigger spike", "Spikes and spiked rails", "spiky", "trigger", "logger", "swagger", "debugger", "negative",
              "vinegar", "renegade", "script", "description", "raccoon", "raccoons", "tycoon", "cocoon", "snicker", "snickers", "knickers", "triggers",
              "scraper", "Nigeria", "Nigel", "homosexual", "my therapist", "therapists"})
            if (contains_forbidden_words(ordinary)) std::printf("FAIL: ordinary word forbidden: %s\n", ordinary), ++failures;
        for (const char* said : {"coon", "c00ns", "spiks", "the spik", "bigcoon", "xxcoonxx", "crips", "niggers", "bniggerz", "sandnigger",
                                 "homo", "homos", "rapers"})
            if (!contains_forbidden_words(said)) std::printf("FAIL: not forbidden: %s\n", said), ++failures;
        expect(mask_bad_words("bigger coons") == "bigger *****", "a short word is not masked only where it is said");
        // A number is not a word written with look-alike digits: a time, a score.
        const std::vector<std::string> short_words{"ss", "ass", "tit"};
        set_word_lists(short_words, {});
        expect(mask_bad_words("TallyMark finished 4th in 2:55.0!") == "TallyMark finished 4th in 2:55.0!" && !contains_bad_words("455 points, 717 to go") &&
                   !contains_bad_words("5 5 5"),
               "a number is filtered as a word");
        expect(contains_bad_words("you a55") && contains_bad_words("@55") && contains_bad_words("t1t"), "a word with digits for letters is not filtered");
        set_word_lists({}, {});
        expect(!contains_bad_words("darn zorblat fuck") && !contains_forbidden_words("zorblat") && mask_bad_words("darn") == "darn",
               "empty lists still filter");
        reset_word_lists();
        expect(!contains_bad_words("what the fuck darn") && !contains_forbidden_words("zorblat"), "words are filtered without a list");
    }
    if (failures == 0) std::printf("word filter: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
