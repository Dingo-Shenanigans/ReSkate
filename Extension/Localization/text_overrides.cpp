#include "text_overrides.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/localization.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>

// The plugins' texts (reskate.strings) in the game's own string-override store, the one the
// backend's client_strings_v1 fills in retail.
namespace dingosdk::text_overrides {
namespace {
using Address = std::uintptr_t;
namespace loc = addr::localization;
// Texts stored per tick: set() converts and registers every glyph, so a whole translation at
// once would be a visible hitch.
constexpr std::size_t batch = 256;
template<class T> T read(Address at) {
    T result{};
    if (!memory::peek(at, result)) throw std::runtime_error("String database memory unavailable");
    return result;
}
template<std::size_t N> bool code_matches(Address at, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> code{};
    return memory::peek_bytes(at, code.data(), N) && std::memcmp(code.data(), expected.data(), N) == 0;
}

struct Text {
    std::string owner;
    std::string id; // as set() is given it: the key's id, or one with the key's hash
    std::string text;
};
struct State {
    Address base{};
    bool verified{}, refused{};
    ULONGLONG next{};
    // Each id's texts, by hash, the last owner's shown; and how many each owner holds.
    std::map<std::uint32_t, std::vector<Text>> texts;
    std::map<std::string, std::size_t, std::less<>> held;
    // Ids whose shown text is still to be stored, once each.
    std::deque<std::uint32_t> queue;
    std::set<std::uint32_t> queued;
    Address database{}; // the one the texts were last stored against
    bool again{};       // store them all once more at the next check (see apply_text_overrides)
    unsigned stored{}, tried{};
    // The owners that asked for Turkish casing, and the hook that applies it.
    std::set<std::string, std::less<>> turkish_owners;
    enum class CaseHook { none, installed, refused } case_hook{};
};
State& state() { static auto* value = new State; return *value; }

void enqueue(State& s, std::uint32_t hash) {
    if (s.queued.insert(hash).second) s.queue.push_back(hash);
}
void enqueue_all(State& s) {
    for (const auto& entry : s.texts) enqueue(s, entry.first);
}
// The id a key stands for ("#1a2b3c4d": one with that hash), or empty for a key that is neither.
std::string id_of(std::string_view key) {
    if (!key.starts_with('#')) return std::string(key);
    const auto digits = key.substr(1);
    std::uint32_t hash{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), hash, 16);
    if (digits.empty() || digits.size() > 8 || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return {};
    return id_for_hash(hash);
}

// ---- casing: the UI's own capitalization, with Turkish rules when a plugin asks ----
using CaseConvert = std::uint32_t (*)(const char16_t* source, std::size_t length, char16_t* target, std::size_t capacity,
                                      int mode, const char* locale);
CaseConvert original_convert{};
std::atomic<bool> turkish{};

// Called on whichever thread lays out UI text. Without the rules, or in a mode other than lower or
// upper (title and reverse case come back here one character at a time), the game converts alone.
std::uint32_t case_convert_hook(const char16_t* source, std::size_t length, char16_t* target, std::size_t capacity, int mode,
                                const char* locale) {
    if (!turkish.load(std::memory_order_relaxed) || (mode != loc::case_lower && mode != loc::case_upper) || !source || !target)
        return original_convert(source, length, target, capacity, mode, locale);
    const char16_t changed = mode == loc::case_upper ? u'i' : u'I';
    if (std::find(source, source + length, changed) == source + length)
        return original_convert(source, length, target, capacity, mode, locale);
    // The target may be the source, so the source is kept to see which characters were i or I.
    std::array<char16_t, 512> small{};
    std::vector<char16_t> large;
    const char16_t* kept = small.data();
    if (length <= small.size()) {
        std::copy_n(source, length, small.begin());
    } else {
        try { large.assign(source, source + length); } catch (...) { return original_convert(source, length, target, capacity, mode, locale); }
        kept = large.data();
    }
    const auto result = original_convert(kept, length, target, capacity, mode, locale);
    // The game writes target[k] for source[k] while k is within the capacity, less a terminator's.
    const std::size_t terminator = !locale || !*locale ? 1 : 0;
    const auto written = std::min(length, capacity > terminator ? capacity - terminator : 0);
    for (std::size_t k = 0; k < written; ++k) target[k] = turkish_case(kept[k], target[k], mode);
    return result;
}

// The converter and its tables as this build has them: i uppercases to I and dotless i to I,
// dotted I lowercases to i.
bool case_convert_matches(Address base) noexcept {
    try {
        return code_matches(base + loc::case_convert.rva, loc::case_convert.bytes) &&
               read<std::uint16_t>(base + loc::case_upper_table + 2 * 0x69) == 0x49 &&
               read<std::uint16_t>(base + loc::case_upper_table + 2 * 0x131) == 0x49 &&
               read<std::uint16_t>(base + loc::case_lower_table + 2 * 0x130) == 0x69;
    } catch (...) { return false; }
}
void install_case_hook(State& s, Address base) {
    s.case_hook = State::CaseHook::refused;
    if (!case_convert_matches(base)) {
        logging::write(logging::Level::warning, logging::Channel::ui,
            "Strings: the game's text casing differs from the supported build; Turkish casing is off.");
        return;
    }
    auto* target = reinterpret_cast<void*>(base + loc::case_convert.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&case_convert_hook), reinterpret_cast<void**>(&original_convert)) != HookOk) {
        logging::write(logging::Level::warning, logging::Channel::ui, "Strings: the text casing hook could not be prepared.");
        return;
    }
    {
        HookBatch hooks;
        if (hook_enable(target) != HookOk) {
            logging::write(logging::Level::warning, logging::Channel::ui, "Strings: the text casing hook could not be installed.");
            return;
        }
    }
    s.case_hook = State::CaseHook::installed;
    logging::write(logging::Level::info, logging::Channel::ui, "Strings: Turkish casing (i to \xc4\xb0, I to \xc4\xb1) installed.");
}
void update_casing(const State& s) {
    turkish.store(!s.turkish_owners.empty(), std::memory_order_relaxed);
}
} // namespace

std::string id_for_hash(std::uint32_t hash) {
    // Seven bytes b0..b6 hash to 0xffffffff * 33^7 + sum b_i * 33^(6-i) (mod 2^32). With
    // b_i = '@' + d_i and d_i in 0..32, what remains is d's base-33 value, and since
    // 33^7 > 2^32 seven digits hold any remainder.
    std::uint32_t power = 1, ones = 0;
    for (int i = 0; i < 7; ++i) { ones += power; power *= 33; }
    std::uint64_t rest = static_cast<std::uint32_t>(hash - 0xffffffffU * power - 0x40U * ones);
    std::string id(7, '@');
    for (int i = 6; i >= 0; --i) {
        id[static_cast<std::size_t>(i)] = static_cast<char>(0x40 + rest % 33);
        rest /= 33;
    }
    return id;
}
bool string_store_matches(Address base) noexcept {
    return base && code_matches(base + loc::override_store, loc::override_store_prefix) &&
           code_matches(base + loc::override_set, loc::override_set_prefix) &&
           code_matches(base + loc::string_exists, loc::string_exists_prefix);
}
Address string_database(Address base) noexcept {
    try {
        if (!read<Address>(base + loc::database_loaded)) return 0;
        const auto table = read<Address>(base + loc::databases);
        const auto last = read<std::int32_t>(base + loc::database_last);
        for (std::int32_t i = 0; table && i <= last && i < 64; ++i) {
            const auto row = table + 16 * static_cast<Address>(i);
            if (read<std::uint32_t>(row)) continue;
            const auto entry = read<Address>(row + 8);
            return entry ? read<Address>(entry + 0x10) : 0;
        }
    } catch (...) {}
    return 0;
}

std::string set(std::string_view owner, const std::vector<std::pair<std::string, std::string>>& texts) {
    auto& s = state();
    if (s.refused) return "the game's text store differs from the supported build";
    std::vector<Text> next;
    next.reserve(texts.size());
    for (const auto& [key, text] : texts) {
        auto id = id_of(key);
        if (id.empty()) return "\"" + key + "\" is not an id or a #hash";
        next.push_back({std::string(owner), std::move(id), text});
    }
    // An id the owner sets again replaces its own text and does not count twice.
    std::size_t added{};
    std::set<std::uint32_t> seen;
    for (const auto& text : next) {
        const auto hash = string_hash(text.id);
        if (!seen.insert(hash).second) continue;
        const auto found = s.texts.find(hash);
        const bool mine = found != s.texts.end() &&
            std::any_of(found->second.begin(), found->second.end(), [&](const Text& t) { return t.owner == owner; });
        added += !mine;
    }
    const auto held = s.held.find(owner);
    if ((held == s.held.end() ? 0 : held->second) + added > most_texts)
        return "a plugin sets at most " + std::to_string(most_texts) + " strings";
    for (auto& text : next) {
        const auto hash = string_hash(text.id);
        auto& list = s.texts[hash];
        std::erase_if(list, [&](const Text& t) { return t.owner == owner; });
        list.push_back(std::move(text));
        enqueue(s, hash);
    }
    s.held[std::string(owner)] += added;
    return {};
}

std::string set_case_rules(std::string_view owner, std::string_view rules) {
    auto& s = state();
    if (rules != "tr" && !rules.empty()) return "casing rules are \"tr\" or \"\"";
    if (rules.empty()) {
        if (const auto found = s.turkish_owners.find(owner); found != s.turkish_owners.end()) s.turkish_owners.erase(found);
    } else {
        if (s.refused || s.case_hook == State::CaseHook::refused) return "the game's text casing differs from the supported build";
        s.turkish_owners.emplace(owner);
    }
    update_casing(s);
    return {};
}

void release(std::string_view owner) {
    auto& s = state();
    if (const auto found = s.turkish_owners.find(owner); found != s.turkish_owners.end()) {
        s.turkish_owners.erase(found);
        update_casing(s);
    }
    const auto held = s.held.find(owner);
    if (held == s.held.end()) return;
    s.held.erase(held);
    for (auto it = s.texts.begin(); it != s.texts.end();) {
        auto& list = it->second;
        const bool shown = !list.empty() && list.back().owner == owner;
        std::erase_if(list, [&](const Text& t) { return t.owner == owner; });
        if (list.empty()) {
            it = s.texts.erase(it);
            continue;
        }
        if (shown) enqueue(s, it->first);
        ++it;
    }
}

std::optional<bool> exists(std::string_view key) {
    auto& s = state();
    if (!s.verified || s.refused || !s.base) return std::nullopt;
    const auto id = id_of(key);
    if (id.empty()) return false;
    try {
        return reinterpret_cast<bool (*)(std::uint32_t)>(s.base + loc::string_exists)(string_hash(id));
    } catch (...) { return std::nullopt; }
}

void apply_text_overrides(Address base) noexcept {
    auto& s = state();
    if (!base || s.refused) return;
    try {
        const auto now = GetTickCount64();
        if (now >= s.next) {
            s.next = now + 2000;
            if (!s.verified) {
                if (!string_store_matches(base)) {
                    s.refused = true;
                    logging::write(logging::Level::warning, logging::Channel::ui,
                        "Strings: the game's string store differs from the supported build; plugins cannot change the game's text.");
                    return;
                }
                s.base = base;
                s.verified = true;
            }
            // Installed only once a plugin asks, so the game's casing is untouched otherwise.
            if (!s.turkish_owners.empty() && s.case_hook == State::CaseHook::none) install_case_hook(s, base);
            // Stored before a database is loaded, the text would be kept empty. A new database
            // (another language) needs it all stored again; so does the check after that, since
            // apply_throwdown_strings stores its own text again for the new database too and
            // would otherwise win for an id both name.
            if (const auto loaded = string_database(base); loaded && loaded != s.database) {
                s.database = loaded;
                s.again = true;
                enqueue_all(s);
            } else if (loaded && s.again) {
                s.again = false;
                enqueue_all(s);
            }
        }
        if (s.queue.empty() || !s.database || !string_database(base)) return;
        const auto store = reinterpret_cast<Address (*)()>(base + loc::override_store)();
        if (!store) return;
        const auto set_text = reinterpret_cast<bool (*)(Address, const char*, const char*, int)>(base + loc::override_set);
        for (std::size_t i = 0; i < batch && !s.queue.empty(); ++i) {
            const auto hash = s.queue.front();
            s.queue.pop_front();
            s.queued.erase(hash);
            const auto found = s.texts.find(hash);
            if (found == s.texts.end() || found->second.empty()) continue;
            const auto& text = found->second.back();
            ++s.tried;
            if (set_text(store, text.id.c_str(), text.text.c_str(), 0)) ++s.stored;
        }
        if (!s.queue.empty()) return;
        logging::log(s.stored == s.tried ? logging::Level::info : logging::Level::warning, logging::Channel::ui,
            "Strings: {} of {} plugin texts stored in the game's string store.", s.stored, s.tried);
        s.stored = s.tried = 0;
    } catch (...) {}
}
} // namespace dingosdk::text_overrides
