#include "word_filter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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

// Ordinary words with a bad word inside; a match that one of these covers does not count. One
// that is a listed entry with its spaces taken out ("the rapist") does not count either.
constexpr std::string_view allowed_words[]{
    "scunthorpe", "dickens",   "dickinson", "dickson",   "dickies",   "cocktail",  "cockpit",   "peacock",
    "cockroach",  "cockatoo",  "cockatiel", "cockney",   "cockle",    "cockerel",  "cockburn",  "cocky",
    "hancock",    "hitchcock", "woodcock",  "babcock",   "shuttlecock", "ashkenazi", "shiitake", "shitake",
    "matsushita", "yamashita", "kinoshita", "morishita", "takeshita", "swank",     "swanky",    "woodpecker",
    "clitheroe",  "snigger",   "pussycat",  "pissarro",  "retardant", "cocker",    "spike",     "spiky",
    "spiking",    "snicker",   "snickers",  "knicker",   "knickers",  "triggers",  "diggers",   "riggers",
    "therapist", "specialist", "cumin",    "christmas", "christian",
    "christopher", "christina", "christine"};

// A listed word this long is not found inside an ordinary word by accident ("nigger",
// "faggot"; allowed_words has the few exceptions), so it counts anywhere in a longer one. A
// shorter one ("fuck", "shit", but also a fragment like "spik" or "coon") is inside too many
// ordinary words: it counts only when the rest of the word is what people put around a swear
// word, so "motherfuckers", "shithead" and "bullshit" do, and "spike", "raccoon", "script" and
// "bigger" do not.
constexpr std::size_t counts_anywhere = 6;
// What may come before a short listed word, and after it.
constexpr std::string_view before_words[]{
    "a",    "an",    "the",   "my",     "ur",   "your", "you",  "u",     "i",     "im",    "yo",      "no",
    "big",  "lil",   "little", "fat",   "dumb", "old",  "hot",  "mega",  "super", "ultra", "hella",   "total",
    "anti", "un",    "non",   "pro",    "ex",   "mc",   "mr",   "sir",   "king",  "lord",  "god",     "mother",
    "mutha", "motha", "bull", "horse",  "dog",  "ape",  "bat",  "dip",   "jack",  "chicken", "cluster", "mind",
    "brain", "butt", "pig",   "skull",  "stupid", "ugly", "dirty", "lazy", "crazy"};
constexpr std::string_view after_words[]{
    "s",    "es",   "z",    "ed",   "er",   "ers",  "erz",  "ing",  "ings", "in",   "y",    "ie",
    "ies",  "ier",  "ish",  "a",    "ah",   "as",   "az",   "o",    "os",   "ism",  "ist",  "ists",
    "head", "heads", "face", "faces", "hole", "holes", "wad", "tard", "bag", "boy",  "boi",  "girl",
    "man",  "stick", "wit", "wits", "lord", "king", "nut",  "less", "ness", "ery",  "fest", "show",
    "storm", "load", "stain", "off", "up",  "it",   "me",   "you",  "u",    "ya"};

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

std::string folded(std::string_view line) {
    std::string word;
    for (const char c : line)
        if (const char letter = fold(c)) word += letter;
    return word;
}

void add(Lists& l, std::string_view line) {
    auto word = folded(line);
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
// Until a list is given there are no words: nothing is filtered.
std::shared_ptr<const Rules> no_words() {
    static const std::shared_ptr<const Rules> rules = std::make_shared<Rules>();
    return rules;
}

// What the rules in use are made from: the backend's lists once they arrive, and a server's own
// words. Changed under the mutex; readers take the rules made from them.
struct Sources {
    std::optional<std::pair<std::vector<std::string>, std::vector<std::string>>> given;
    std::vector<std::string> own_filtered, own_forbidden;
    bool use_given = true;
};
std::mutex sources_mutex;
Sources sources;
std::atomic<std::shared_ptr<const Rules>> made;
std::shared_ptr<const Rules> rules() {
    auto current = made.load();
    return current ? current : no_words();
}
void remake() {
    const bool backend = sources.given && sources.use_given;
    if (!backend && sources.own_filtered.empty() && sources.own_forbidden.empty()) return made.store({});
    auto r = std::make_shared<Rules>();
    if (backend) {
        for (const auto& word : sources.given->first) add(r->filtered, word);
        for (const auto& word : sources.given->second) add(r->filtered, word), add(r->forbidden, word);
    }
    for (const auto& word : sources.own_filtered) add(r->filtered, word);
    for (const auto& word : sources.own_forbidden) add(r->filtered, word), add(r->forbidden, word);
    made.store(std::shared_ptr<const Rules>(std::move(r)));
}

// A word as folded letters, with where each letter sits in the text.
struct Word {
    std::string letters;
    std::vector<std::size_t> at;
};

std::vector<Word> words(std::string_view text) {
    std::vector<Word> out;
    Word current;
    // A run of digits alone is a number, not a word written with look-alikes: a time such as
    // "2:55.0" is not "ss" (seen 2026-10-12, a race's finishing time masked in the chat).
    bool number = true;
    const auto take = [&] {
        if (!current.letters.empty() && !number) out.push_back(std::move(current));
        current = {};
        number = true;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (const char letter = fold(text[i])) {
            current.letters += letter;
            current.at.push_back(i);
            if (text[i] < '0' || text[i] > '9') number = false;
        } else {
            take();
        }
    }
    take();
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

// Letters added only to get a word past the filter: "xx", "zz".
bool padding(std::string_view letters) {
    return std::ranges::all_of(letters, [](char c) { return c == 'x' || c == 'z'; });
}
// Whether a short listed word at letters[start, start + length) is the word being said, going by
// what is around it: nothing, another listed word, or one of the usual beginnings and endings.
// A last letter said again counts as part of it ("shitty", "fuckkk").
bool said(const Lists& l, std::string_view letters, std::size_t start, std::size_t length) {
    const auto before = letters.substr(0, start);
    auto after = letters.substr(start + length);
    while (!after.empty() && after.front() == letters[start + length - 1]) after.remove_prefix(1);
    const auto fits = [&](std::string_view part, std::span<const std::string_view> usual) {
        return part.empty() || padding(part) || l.whole.contains(part) || std::ranges::find(usual, part) != usual.end();
    };
    return fits(before, before_words) && fits(after, after_words);
}

// Whether `word` has a bad word in it; with `marked`, flags each letter that belongs to one.
bool match(const Lists& l, const Word& word, std::vector<bool>* marked) {
    const std::string_view letters = word.letters;
    if (l.whole.contains(letters)) {
        if (std::ranges::find(allowed_words, letters) != std::ranges::end(allowed_words)) return false;
        if (marked) marked->assign(letters.size(), true);
        return true;
    }
    bool found{};
    if (l.inside.empty()) return false;
    for (std::size_t start = 0; start + l.shortest_inside <= letters.size(); ++start)
        for (auto length = l.shortest_inside; length <= l.longest_inside && start + length <= letters.size(); ++length) {
            if (!l.inside.contains(letters.substr(start, length)) || covered(letters, start, length)) continue;
            if (length < counts_anywhere && !said(l, letters, start, length)) continue;
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
    std::lock_guard lock(sources_mutex);
    sources.given.emplace(std::vector<std::string>(filtered.begin(), filtered.end()),
                          std::vector<std::string>(forbidden.begin(), forbidden.end()));
    remake();
}

void reset_word_lists() {
    std::lock_guard lock(sources_mutex);
    sources.given.reset();
    remake();
}

void set_own_words(std::span<const std::string> filtered, std::span<const std::string> forbidden, bool use_given) {
    std::lock_guard lock(sources_mutex);
    sources.own_filtered.assign(filtered.begin(), filtered.end());
    sources.own_forbidden.assign(forbidden.begin(), forbidden.end());
    sources.use_given = use_given;
    remake();
}

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
