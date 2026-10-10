#include "word_filter.h"

#include "embedded_bad_words.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace dingosdk::text {
namespace {
// Letters fold to lower case and look-alikes to the letter they stand for; anything else
// separates words.
char fold(char c) {
    if (c >= 'a' && c <= 'z') return c;
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    switch (c) {
    case '0': return 'o';
    case '1': return 'i';
    case '3': return 'e';
    case '4': return 'a';
    case '5': return 's';
    case '7': return 't';
    case '8': return 'b';
    case '@': return 'a';
    case '$': return 's';
    case '+': return 't';
    default: return 0;
    }
}

// Longer list words that are common inside ordinary ones ("hell" in hello, "arse" in parse,
// "puta" in reputation, "turd" in saturday, "rapist" in therapist): these only count as a
// whole word.
constexpr std::string_view whole_word_only[]{
    "anus", "arse", "ayir", "bich", "breasts", "cawk", "cawks", "chuj", "cipa", "crap",
    "dego", "dike", "dupa", "ekto", "faen", "faig", "faigs", "fanny", "fart", "fitt",
    "flipping", "gays", "gayz", "gook", "hell", "hells", "hoar", "hoer", "hoor", "hore",
    "injun", "jiss", "kawk", "knob", "knobs", "knobz", "kraut", "kunt", "kunts", "kuntz",
    "kusi", "merd", "muie", "nastt", "nasty", "packi", "packie", "packy", "paki", "pakie",
    "paky", "paska", "perse", "picka", "pillu", "polac", "polak", "poop", "pric", "prik",
    "pron", "pula", "pule", "pusse", "puta", "puto", "rape", "raped", "rapes", "raping",
    "rapist", "rapists", "rautenberg", "schaffer", "screw", "screwing", "semen", "shiz", "smut", "teets",
    "teez", "tits", "titt", "turd", "woose"};

// Ordinary words with a bad word inside; a match that one of these covers does not count.
constexpr std::string_view allowed_words[]{
    "scunthorpe", "dickens",   "dickinson", "dickson",   "dickies",   "cocktail",  "cockpit",   "peacock",
    "cockroach",  "cockatoo",  "cockatiel", "cockney",   "cockle",    "cockerel",  "cockburn",  "cocky",
    "hancock",    "hitchcock", "woodcock",  "babcock",   "shuttlecock", "ashkenazi", "shiitake", "shitake",
    "matsushita", "yamashita", "kinoshita", "morishita", "takeshita", "swank",     "swanky",    "woodpecker",
    "clitheroe",  "snigger",   "pussycat",  "pissarro",  "retardant"};

struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
};
using WordSet = std::unordered_set<std::string, Hash, std::equal_to<>>;

struct Lists {
    WordSet whole;  // every listed word
    WordSet inside; // the ones that also match inside longer words
    std::size_t shortest_inside{~std::size_t{}}, longest_inside{};
};

void add(Lists& l, std::string_view line) {
    std::string word;
    for (const char c : line)
        if (const char letter = fold(c)) word += letter;
    if (word.empty()) return;
    l.whole.insert(word);
    if (word.size() > 3 && std::ranges::find(whole_word_only, word) == std::ranges::end(whole_word_only)) {
        l.shortest_inside = std::min(l.shortest_inside, word.size());
        l.longest_inside = std::max(l.longest_inside, word.size());
        l.inside.insert(std::move(word));
    }
}

// What is filtered (the forbidden words too), and what is forbidden outright.
struct Rules {
    Lists filtered, forbidden;
};
std::shared_ptr<const Rules> built_in() {
    static const auto rules = [] {
        auto r = std::make_shared<Rules>();
        std::string_view text = embedded::bad_words_list;
        while (!text.empty()) {
            const auto end = text.find('\n');
            add(r->filtered, text.substr(0, end));
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        }
        return std::shared_ptr<const Rules>(std::move(r));
    }();
    return rules;
}
std::atomic<std::shared_ptr<const Rules>> given;
std::shared_ptr<const Rules> rules() {
    auto current = given.load();
    return current ? current : built_in();
}

// A word as folded letters, with where each letter sits in the text.
struct Word {
    std::string letters;
    std::vector<std::size_t> at;
};

std::vector<Word> words(std::string_view text) {
    std::vector<Word> out;
    Word current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (const char letter = fold(text[i])) {
            current.letters += letter;
            current.at.push_back(i);
        } else if (!current.letters.empty()) {
            out.push_back(std::move(current));
            current = {};
        }
    }
    if (!current.letters.empty()) out.push_back(std::move(current));
    // Three or more letters spelled out one at a time ("f u c k", "s.o.b.") also read as a word.
    std::vector<Word> spelled;
    for (std::size_t i = 0; i < out.size();) {
        auto end = i;
        while (end < out.size() && out[end].letters.size() == 1) ++end;
        if (end - i >= 3) {
            Word joined;
            for (auto k = i; k < end; ++k) {
                joined.letters += out[k].letters;
                joined.at.push_back(out[k].at.front());
            }
            spelled.push_back(std::move(joined));
        }
        i = end == i ? i + 1 : end;
    }
    out.insert(out.end(), std::make_move_iterator(spelled.begin()), std::make_move_iterator(spelled.end()));
    return out;
}

bool covered(std::string_view letters, std::size_t start, std::size_t length) {
    for (const auto allowed : allowed_words)
        for (auto at = letters.find(allowed); at != std::string_view::npos; at = letters.find(allowed, at + 1))
            if (at <= start && start + length <= at + allowed.size()) return true;
    return false;
}

// Whether `word` has a bad word in it; with `marked`, flags each letter that belongs to one.
bool match(const Lists& l, const Word& word, std::vector<bool>* marked) {
    const std::string_view letters = word.letters;
    if (l.whole.contains(letters)) {
        if (marked) marked->assign(letters.size(), true);
        return true;
    }
    bool found{};
    if (l.inside.empty()) return false;
    for (std::size_t start = 0; start + l.shortest_inside <= letters.size(); ++start)
        for (auto length = l.shortest_inside; length <= l.longest_inside && start + length <= letters.size(); ++length) {
            if (!l.inside.contains(letters.substr(start, length)) || covered(letters, start, length)) continue;
            if (!marked) return true;
            found = true;
            std::fill(marked->begin() + static_cast<std::ptrdiff_t>(start),
                      marked->begin() + static_cast<std::ptrdiff_t>(start + length), true);
        }
    return found;
}
} // namespace

bool contains_bad_words(std::string_view text) {
    const auto r = rules();
    for (const auto& word : words(text))
        if (match(r->filtered, word, nullptr)) return true;
    return false;
}

bool contains_forbidden_words(std::string_view text) {
    const auto r = rules();
    if (r->forbidden.whole.empty()) return false;
    for (const auto& word : words(text))
        if (match(r->forbidden, word, nullptr)) return true;
    return false;
}

void set_word_lists(std::span<const std::string> filtered, std::span<const std::string> forbidden) {
    auto r = std::make_shared<Rules>();
    for (const auto& word : filtered) add(r->filtered, word);
    for (const auto& word : forbidden) add(r->filtered, word), add(r->forbidden, word);
    given.store(std::shared_ptr<const Rules>(std::move(r)));
}

void reset_word_lists() { given.store({}); }

std::string mask_bad_words(std::string_view text) {
    const auto r = rules();
    std::string out(text);
    for (const auto& word : words(text)) {
        std::vector<bool> marked(word.letters.size());
        if (!match(r->filtered, word, &marked)) continue;
        for (std::size_t i = 0; i < marked.size(); ++i)
            if (marked[i]) out[word.at[i]] = '*';
    }
    return out;
}
} // namespace dingosdk::text
