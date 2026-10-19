#include "text_overrides.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/localization.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Boot/user_data_redirect.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

// Text overrides: the player's and mods' reskate-strings.json stored in the game's own
// string-override store, the one the backend's client_strings_v1 fills in retail.
namespace dingosdk::text_overrides {
namespace {
using Address = std::uintptr_t;
namespace loc = addr::localization;
namespace fs = std::filesystem;
constexpr wchar_t file_name[] = L"reskate-strings.json";
// Entries stored per tick: set() converts and registers every glyph, so a whole
// translation at once would be a visible hitch.
constexpr std::size_t batch = 256;
template<class T> T read(Address at) {
    T result{};
    if (!memory::peek(at, result)) throw std::runtime_error("String database memory unavailable");
    return result;
}
fs::path data_folder() { return redirected_local_app_data().parent_path(); }

struct Entry {
    std::string id; // as set() is given it: the file's id, or one with the file's hash
    std::string text;
    bool named{}; // the file gave the id itself, not a #hash
};
struct Source {
    fs::path path;
    std::string owner; // for the log
};
// The player's own file first, then each enabled mod's, highest priority first.
std::vector<Source> sources() {
    std::vector<Source> result;
    try { result.push_back({data_folder() / file_name, "your own strings"}); } catch (...) {}
    for (const auto& mod : mods::catalog().mods) result.push_back({mod.directory / file_name, "mod " + mod.name});
    return result;
}
std::vector<fs::file_time_type> stamps(const std::vector<Source>& list) {
    std::vector<fs::file_time_type> result;
    for (const auto& source : list) {
        std::error_code error;
        const auto stamp = fs::last_write_time(source.path, error);
        result.push_back(error ? fs::file_time_type::min() : stamp);
    }
    return result;
}
// Adds the file's entries whose hash no earlier file took. A bad file is logged and skipped.
void read_source(const Source& source, std::vector<Entry>& entries, std::set<std::uint32_t>& taken) {
    std::error_code error;
    if (!fs::is_regular_file(source.path, error)) return;
    try {
        std::ifstream in(source.path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3); // Notepad's byte order mark
        const auto json = Json::parse(bytes, JsonLimits{16 * 1024 * 1024, 4, 1 << 20});
        if (!json.is_object()) throw std::runtime_error("it must be one JSON object of \"ID\": \"text\"");
        unsigned added{}, skipped{};
        for (const auto& [key, value] : json.items()) {
            if (key.empty() || key.starts_with('_')) continue;
            if (!value.is_string()) { ++skipped; continue; }
            std::string id = key;
            if (key.starts_with('#')) {
                std::uint32_t hash{};
                const auto digits = std::string_view(key).substr(1);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), hash, 16);
                if (digits.empty() || digits.size() > 8 || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
                    ++skipped;
                    continue;
                }
                id = id_for_hash(hash);
            }
            if (!taken.insert(string_hash(id)).second) continue;
            entries.push_back({std::move(id), value.string(), !key.starts_with('#')});
            ++added;
        }
        logging::log(logging::Level::info, logging::Channel::ui, "Strings: {} text overrides from {} ({}).",
            added, source.owner, path_utf8(source.path));
        if (skipped) logging::log(logging::Level::warning, logging::Channel::ui,
            "Strings: {} entries in {} skipped (not text, or a bad #hash).", skipped, path_utf8(source.path));
    } catch (const std::exception& e) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Strings: {} skipped ({}): {}",
            source.owner, path_utf8(source.path), e.what());
    }
}

struct State {
    ULONGLONG next{};
    bool verified{}, refused{}, read_once{};
    std::vector<fs::file_time_type> stamps; // of the files `entries` was read from
    std::vector<Entry> entries;
    Address database{};  // the one the entries were last stored against
    bool dirty{};        // the entries need storing (read again, or a new database)
    bool again{};        // store them all once more at the next check (see apply_text_overrides)
    bool storing{};
    std::size_t next_entry{};
    unsigned stored{};
};
State& state() { static auto* value = new State; return *value; }

// UTF-8 checks and repair: the dump holds only valid UTF-8, one bad string never loses the rest.
void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xc0 | c >> 6); out += static_cast<char>(0x80 | (c & 0x3f)); }
    else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}
// The length of the valid UTF-8 sequence at text[at], or 0 (overlong, surrogate, past U+10FFFF).
std::size_t utf8_sequence(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned lead = byte(at);
    if (lead < 0x80) return 1;
    std::size_t size{};
    char32_t c{};
    if (lead >= 0xc2 && lead <= 0xdf) { size = 2; c = lead & 0x1f; }
    else if (lead >= 0xe0 && lead <= 0xef) { size = 3; c = lead & 0x0f; }
    else if (lead >= 0xf0 && lead <= 0xf4) { size = 4; c = lead & 0x07; }
    else return 0;
    if (at + size > text.size()) return 0;
    for (std::size_t i = 1; i < size; ++i) {
        if ((byte(at + i) & 0xc0) != 0x80) return 0;
        c = c << 6 | (byte(at + i) & 0x3f);
    }
    if ((size == 3 && (c < 0x800 || (c >= 0xd800 && c <= 0xdfff))) || (size == 4 && (c < 0x10000 || c > 0x10ffff))) return 0;
    return size;
}
bool valid_utf8(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const auto size = utf8_sequence(text, at);
        if (!size) return false;
        at += size;
    }
    return true;
}
// Every byte that starts no valid sequence becomes U+FFFD.
std::string repair_utf8(std::string_view text) {
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        if (const auto size = utf8_sequence(text, at)) { out.append(text.substr(at, size)); at += size; }
        else { append_utf8(out, 0xfffd); ++at; }
    }
    return out;
}
constexpr char hex_digits[] = "0123456789abcdef";
std::string hex_snippet(std::string_view bytes) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size() && i < 32; ++i) {
        if (i) out += ' ';
        out += hex_digits[static_cast<unsigned char>(bytes[i]) >> 4];
        out += hex_digits[static_cast<unsigned char>(bytes[i]) & 0xf];
    }
    if (bytes.size() > 32) out += " ...";
    return out;
}
std::string hash_key(std::uint32_t hash) {
    std::string key = "#00000000";
    for (std::size_t i = 8; i >= 1; --i, hash >>= 4) key[i] = hex_digits[hash & 0xf];
    return key;
}

// A database's charset: what its stored form decodes against (see localization.h).
struct Charset {
    std::vector<std::uint16_t> characters;
    bool latin1{};
};
Charset read_charset(Address at, bool latin1) {
    Charset result{{}, latin1};
    if (!at) return result;
    // Pairs reach index 0x7f * 128 + 0x7f; nothing past that is ever read.
    const auto count = std::min<std::uint32_t>(read<std::uint32_t>(at), 0x4000);
    result.characters.resize(count);
    if (count && !memory::peek_bytes(at + 4, result.characters.data(), count * sizeof(std::uint16_t)))
        result.characters.clear();
    return result;
}
// The stored form as UTF-8, read as the game's decoder (0x3325ac0) reads it; what that would
// show as '_' becomes U+FFFD. UTF-16 pairs are joined; a lone surrogate is U+FFFD too.
std::string decode_stored(const Charset& charset, std::string_view stored) {
    std::string out;
    char32_t high{};
    const auto character = [&](std::size_t index) -> std::uint16_t {
        return index < charset.characters.size() ? charset.characters[index] : 0;
    };
    const auto emit = [&](char32_t unit) {
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (high) append_utf8(out, 0xfffd);
            high = unit;
            return;
        }
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            append_utf8(out, high ? 0x10000 + ((high - 0xd800) << 10) + (unit - 0xdc00) : 0xfffd);
            high = 0;
            return;
        }
        if (high) { append_utf8(out, 0xfffd); high = 0; }
        append_utf8(out, unit);
    };
    for (std::size_t i = 0; i < stored.size(); ++i) {
        const auto byte = static_cast<unsigned char>(stored[i]);
        if (byte < 0x80 || charset.latin1) { emit(byte); continue; }
        const auto c = character(byte);
        if (c >= 0x80) { emit(c); continue; }
        if (!c) { emit(byte); continue; }
        if (i + 1 >= stored.size()) { emit(0xfffd); break; }
        const auto next = static_cast<unsigned char>(stored[++i]);
        if (next < 0x80) { emit(0xfffd); continue; }
        const auto paired = character(static_cast<std::size_t>(c) * 128 + next - 0x80);
        emit(paired ? paired : 0xfffd);
    }
    if (high) append_utf8(out, 0xfffd);
    return out;
}

struct Dump {
    std::mutex mutex;
    std::map<std::uint32_t, std::string> names;      // hash -> id, where an id is known
    std::map<std::uint32_t, std::string> translated; // hash -> the translate's raw text
    bool dirty{}, announced{};
    // Game thread only.
    std::map<std::uint32_t, std::string> database;   // hash -> UTF-8 text, from the walk
    std::vector<std::uint64_t> shape;                // what the walk was taken from
    bool walk_checked{}, walk_refused{};
    std::set<std::string> reported;                  // ids whose text needed repair
};
Dump& dump() { static auto* value = new Dump; return *value; }
bool dumping() {
    static const bool wanted = GetEnvironmentVariableW(L"RESKATE_STRINGS_DUMP", nullptr, 0) != 0;
    return wanted;
}
template<std::size_t N> bool code_matches(Address at, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> code{};
    return memory::peek_bytes(at, code.data(), N) && std::memcmp(code.data(), expected.data(), N) == 0;
}
constexpr std::int32_t max_rows = 100; // the rows table is 0x640 bytes
constexpr std::uint32_t max_banks = 4096;
constexpr std::int32_t max_pairs = 1 << 20;
// The database's rows, banks and pair counts: the walk is taken again when they change.
std::vector<std::uint64_t> database_shape(Address base) {
    std::vector<std::uint64_t> shape;
    if (!read<Address>(base + loc::database_loaded)) return shape;
    const auto rows = read<Address>(base + loc::databases);
    const auto last = std::min(read<std::int32_t>(base + loc::database_last), max_rows - 1);
    shape.push_back(rows);
    shape.push_back(read<std::uint32_t>(base + loc::latin1_mode));
    for (std::int32_t i = 0; rows && i <= last; ++i) {
        const auto entry = read<Address>(rows + 16 * static_cast<Address>(i) + 8);
        shape.push_back(entry);
        if (!entry) continue;
        const auto banks = read<std::uint32_t>(entry);
        const auto bank_table = read<Address>(entry + 8);
        shape.push_back(read<Address>(entry + 0x10));
        shape.push_back(banks);
        for (std::uint32_t b = 0; bank_table && b < banks && b < max_banks; ++b) {
            const auto bank = bank_table + 0x98 * static_cast<Address>(b);
            shape.push_back(read<Address>(bank + 0x90));
            shape.push_back(read<std::uint32_t>(bank + 0x80));
        }
    }
    return shape;
}
// Every string the database lookup can find, the first row and bank to have a hash winning,
// as the lookup reads them.
std::map<std::uint32_t, std::string> walk_database(Address base) {
    std::map<std::uint32_t, std::string> result;
    if (!read<Address>(base + loc::database_loaded)) return result;
    const auto rows = read<Address>(base + loc::databases);
    const auto last = std::min(read<std::int32_t>(base + loc::database_last), max_rows - 1);
    const bool latin1 = read<std::uint32_t>(base + loc::latin1_mode) != 0;
    std::vector<char> text(64 * 1024);
    std::vector<std::array<std::uint32_t, 2>> pairs;
    for (std::int32_t i = 0; rows && i <= last; ++i) {
        try {
            const auto entry = read<Address>(rows + 16 * static_cast<Address>(i) + 8);
            if (!entry) continue;
            const auto charset = read_charset(read<Address>(entry + 0x10), latin1);
            const auto banks = std::min(read<std::uint32_t>(entry), max_banks);
            const auto bank_table = read<Address>(entry + 8);
            for (std::uint32_t b = 0; bank_table && b < banks; ++b) {
                const auto bank = bank_table + 0x98 * static_cast<Address>(b);
                const auto pair_table = read<Address>(bank + 0x90);
                const auto count = read<std::int32_t>(bank + 0x80);
                const auto text_base = read<Address>(bank + 0x88);
                if (!pair_table || count <= 0 || count > max_pairs) continue;
                pairs.resize(static_cast<std::size_t>(count));
                if (!memory::peek_bytes(pair_table, pairs.data(), pairs.size() * sizeof(pairs[0]))) continue;
                for (const auto& [hash, offset] : pairs) {
                    if (result.contains(hash)) continue;
                    const auto size = memory::peek_cstring(text_base + offset, text.data(), text.size());
                    if (size < 0) continue;
                    result.emplace(hash, decode_stored(charset, std::string_view(text.data(), static_cast<std::size_t>(size))));
                }
            }
        } catch (...) {}
    }
    return result;
}
// Game thread, at each check: walks the database again when its shape changed.
void refresh_database_walk(Address base) noexcept {
    auto& d = dump();
    if (d.walk_refused) return;
    try {
        if (!d.walk_checked) {
            d.walk_checked = true;
            if (!code_matches(base + loc::database_find, loc::database_find_prefix) ||
                !code_matches(base + loc::database_find_walk, loc::database_find_walk_bytes) ||
                !code_matches(base + loc::stored_decode, loc::stored_decode_bytes)) {
                d.walk_refused = true;
                logging::log(logging::Level::warning, logging::Channel::ui,
                    "Strings: the game's string database differs from the supported build; the dump holds only translated ids.");
                return;
            }
        }
        auto shape = database_shape(base);
        if (shape.empty() || shape == d.shape) return;
        d.shape = std::move(shape);
        auto walked = walk_database(base);
        if (walked.empty()) return;
        std::size_t named{};
        {
            std::lock_guard lock(d.mutex);
            for (const auto& entry : walked) named += d.names.contains(entry.first);
            d.database = std::move(walked);
            d.dirty = true;
        }
        logging::log(logging::Level::info, logging::Channel::ui,
            "Strings: {} strings read from the game's string database for the dump ({} with a known id).",
            d.database.size(), named);
    } catch (...) {}
}
// Game thread, at most every 2 s: rewrites the dump when something new was seen.
void write_dump(Address base) noexcept {
    if (!dumping()) return;
    auto& d = dump();
    try {
        Json value = Json::object();
        bool announce{};
        std::map<std::string, std::string> rows;
        {
            std::lock_guard lock(d.mutex);
            if (!d.dirty) return;
            d.dirty = false;
            announce = !d.announced;
            d.announced = true;
            const auto key = [&](std::uint32_t hash) {
                const auto name = d.names.find(hash);
                return name != d.names.end() ? name->second : hash_key(hash);
            };
            for (const auto& [hash, text] : d.database) rows.emplace(key(hash), text);
            // The translate's text, for ids the walk lacks: UTF-8 already, or else the stored
            // form of the loaded database.
            std::optional<Charset> charset;
            for (const auto& [hash, raw] : d.translated) {
                if (d.database.contains(hash)) continue;
                if (valid_utf8(raw)) { rows.emplace(key(hash), raw); continue; }
                if (!charset) {
                    charset.emplace();
                    try {
                        if (const auto database = string_database(base))
                            *charset = read_charset(database, read<std::uint32_t>(base + loc::latin1_mode) != 0);
                    } catch (...) {}
                }
                rows.emplace(key(hash), charset->characters.empty() ? raw : decode_stored(*charset, raw));
            }
        }
        for (auto& [id, text] : rows) {
            if (!valid_utf8(text)) {
                if (d.reported.insert(id).second) logging::log(logging::Level::warning, logging::Channel::ui,
                    "Strings: dump text of {} is not UTF-8 and was repaired: {}", id, hex_snippet(text));
                text = repair_utf8(text);
            }
            value[id] = text;
        }
        const auto target = data_folder() / L"strings-dump.json", temporary = data_folder() / L"strings-dump.json.tmp";
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << value.dump(2) << '\n';
            if (!out) throw std::runtime_error("cannot write the dump");
        }
        fs::rename(temporary, target, error);
        if (error) throw std::runtime_error(error.message());
        if (announce) logging::log(logging::Level::info, logging::Channel::ui,
            "Strings: RESKATE_STRINGS_DUMP is set; the game's strings go to {} ({} so far).", path_utf8(target), rows.size());
    } catch (const std::exception& e) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Strings: dump not written: {}", e.what());
    } catch (...) {}
}
} // namespace

std::uint32_t string_hash(std::string_view id) noexcept {
    std::uint32_t hash = 0xffffffffU;
    for (const unsigned char c : id) hash = hash * 33 + c;
    return hash;
}
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

void apply_text_overrides(Address base) noexcept {
    auto& s = state();
    if (!base || s.refused) return;
    try {
        const auto now = GetTickCount64();
        if (!s.storing && now >= s.next) {
            s.next = now + 2000;
            if (!s.verified) {
                if (!string_store_matches(base)) {
                    s.refused = true;
                    logging::write(logging::Level::warning, logging::Channel::ui,
                        "Strings: the game's string store differs from the supported build; reskate-strings.json is not used.");
                    return;
                }
                s.verified = true;
            }
            write_dump(base);
            const auto list = sources();
            auto current = stamps(list);
            if (!s.read_once || current != s.stamps) {
                s.read_once = s.dirty = true;
                s.stamps = std::move(current);
                s.entries.clear();
                std::set<std::uint32_t> taken;
                for (const auto& source : list) read_source(source, s.entries, taken);
                if (dumping()) {
                    auto& d = dump();
                    std::lock_guard lock(d.mutex);
                    for (const auto& entry : s.entries)
                        if (entry.named && d.names.try_emplace(string_hash(entry.id), entry.id).second) d.dirty = true;
                }
            }
            // Stored before a database is loaded, the text would be kept empty. A new database
            // (another language) needs it stored again; so does the check after that, since
            // apply_throwdown_strings stores its own text again for the new database too and
            // would otherwise win for an id both name.
            const auto loaded = string_database(base);
            if (!loaded) return;
            if (dumping()) refresh_database_walk(base);
            if (loaded != s.database) {
                s.database = loaded;
                s.again = s.dirty = true;
            } else if (s.again) {
                s.again = false;
                s.dirty = true;
            }
            if (!s.dirty) return;
            s.dirty = false;
            if (s.entries.empty()) return;
            s.storing = true;
            s.next_entry = 0;
            s.stored = 0;
        }
        if (!s.storing) return;
        const auto store = reinterpret_cast<Address (*)()>(base + loc::override_store)();
        if (!store) { s.storing = false; return; }
        const auto set = reinterpret_cast<bool (*)(Address, const char*, const char*, int)>(base + loc::override_set);
        const auto end = std::min(s.entries.size(), s.next_entry + batch);
        for (; s.next_entry < end; ++s.next_entry) {
            const auto& entry = s.entries[s.next_entry];
            if (set(store, entry.id.c_str(), entry.text.c_str(), 0)) ++s.stored;
        }
        if (s.next_entry < s.entries.size()) return;
        s.storing = false;
        logging::log(s.stored == s.entries.size() ? logging::Level::info : logging::Level::warning, logging::Channel::ui,
            "Strings: {} of {} text overrides stored in the game's string store.", s.stored, s.entries.size());
    } catch (...) { s.storing = false; }
}

void observe_translation(const char* key, const char* text, std::uint32_t capacity) noexcept {
    if (!dumping() || !key || !text || !capacity) return;
    try {
        std::array<char, 256> id{};
        std::size_t size = 0;
        for (; size < id.size(); ++size) {
            if (!memory::peek(reinterpret_cast<Address>(key) + size, id[size])) return;
            if (!id[size]) break;
        }
        if (!size || size == id.size()) return;
        // The game's stored form, not UTF-8 (see localization.h); write_dump decodes it.
        const std::string_view value(text, strnlen(text, capacity));
        const auto hash = string_hash(std::string_view(id.data(), size));
        auto& d = dump();
        std::lock_guard lock(d.mutex);
        if (d.names.size() >= 1000000) return;
        if (d.names.try_emplace(hash, id.data(), size).second) d.dirty = true;
        if (d.translated.size() < 100000 && d.translated.try_emplace(hash, value).second) d.dirty = true;
    } catch (...) {}
}
} // namespace dingosdk::text_overrides
