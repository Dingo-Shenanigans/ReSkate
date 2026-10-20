#include "language.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

namespace dingosdk::launcher_language {
namespace {

namespace fs = std::filesystem;

struct Table {
    Language language;
    std::map<std::string, std::string, std::less<>> lines;   // English -> translation
};

// Every language, read once; never changed or freed after, so a translation handed out stays.
struct Tables {
    std::vector<std::unique_ptr<Table>> tables;   // English (no lines) first
    std::vector<Language> list;
};

// A language's lines into `table`: the file's own over any it had.
void read_lines(Table& table, std::string_view text, std::string_view source) {
    try {
        const auto json = Json::parse(text);
        for (const auto& [english, translation] : json.items()) {
            if (!translation.is_string()) continue;
            if (english == "@language") {
                if (!translation.string().empty()) table.language.name = translation.string();
            } else if (!english.empty() && english.front() != '@' && !translation.string().empty()) {
                table.lines[english] = translation.string();
            }
        }
    } catch (const std::exception& failure) {
        logging::log(logging::Level::warning, logging::Channel::launcher, "Ignoring launcher language {}: {}", source, failure.what());
    }
}

Table& table_for(Tables& all, std::string code) {
    std::ranges::transform(code, code.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto& table : all.tables)
        if (table->language.code == code) return *table;
    auto& added = *all.tables.emplace_back(std::make_unique<Table>());
    added.language = {code, code};
    return added;
}

Tables read_tables() {
    Tables all;
    all.tables.push_back(std::make_unique<Table>(Table{{"en", "English"}, {}}));
    // Embedded: LANGUAGE_<CODE> RCDATA (cmake/Apps.cmake), "_" standing for "-".
    std::vector<std::wstring> names;
    EnumResourceNamesW(nullptr, MAKEINTRESOURCEW(10) /* RT_RCDATA */, [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR found) -> BOOL {
        if (!IS_INTRESOURCE(name) && std::wstring_view(name).starts_with(L"LANGUAGE_"))
            reinterpret_cast<std::vector<std::wstring>*>(found)->emplace_back(name);
        return TRUE;
    }, reinterpret_cast<LONG_PTR>(&names));
    for (const auto& name : names) {
        const auto resource = FindResourceW(nullptr, name.c_str(), MAKEINTRESOURCEW(10));
        const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        const auto* data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
        if (!data) continue;
        std::string code;
        for (const wchar_t c : std::wstring_view(name).substr(9)) code += c == L'_' ? '-' : static_cast<char>(c);
        read_lines(table_for(all, code), std::string_view(data, SizeofResource(nullptr, resource)), code);
    }
    // languages\<code>.json beside the exe; the template is not a language.
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    std::error_code error;
    for (fs::directory_iterator file(fs::path(self).parent_path() / L"languages", error), end; !error && file != end; file.increment(error)) {
        const auto& path = file->path();
        if (path.extension() != L".json" || path.stem().wstring().find(L"template") != std::wstring::npos) continue;
        std::ifstream input(path, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto stem = path.stem().wstring();
        std::string code;
        for (const wchar_t c : stem) code += static_cast<char>(c < 0x80 ? c : L'_');
        read_lines(table_for(all, code), text, code);
    }
    std::sort(all.tables.begin() + 1, all.tables.end(),
              [](const auto& a, const auto& b) { return a->language.code < b->language.code; });
    for (const auto& table : all.tables) all.list.push_back(table->language);
    return all;
}

Tables& tables() {
    static Tables all = read_tables();
    return all;
}

std::atomic<const Table*> g_chosen{};

const Table& current() {
    const auto* table = g_chosen.load();
    return table ? *table : *tables().tables.front();
}

// A format string's placeholders, in order: "{}", "{:.1f}"... ("{{" and "}}" are text).
std::vector<std::string_view> placeholders(std::string_view text) {
    std::vector<std::string_view> found;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '{' && at + 1 < text.size() && text[at + 1] == '{') { ++at; continue; }
        if (text[at] != '{') continue;
        const auto end = text.find('}', at);
        if (end == std::string_view::npos) break;
        found.push_back(text.substr(at, end + 1 - at));
        at = end;
    }
    return found;
}

} // namespace

const std::vector<Language>& languages() { return tables().list; }

void choose(std::string_view code) {
    for (const auto& table : tables().tables)
        if (table->language.code == code) { g_chosen = table.get(); return; }
    g_chosen = tables().tables.front().get();
}

const std::string& chosen() { return current().language.code; }

std::string system_language() {
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> name{};
    if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name.data(), static_cast<int>(name.size()), 0))
        return {};
    std::string locale;   // "tr-TR" -> "tr-tr"
    for (const wchar_t* c = name.data(); *c; ++c) locale += static_cast<char>(std::tolower(static_cast<unsigned char>(*c & 0x7f)));
    // The whole name ("pt-br"), else the language alone ("tr").
    for (const auto& wanted : {locale, locale.substr(0, locale.find('-'))})
        for (const auto& language : languages())
            if (language.code == wanted && wanted != "en") return wanted;
    return {};
}

const char* tr(const char* english) {
    const auto& lines = current().lines;
    if (lines.empty() || !english) return english;
    const auto found = lines.find(std::string_view(english));
    return found == lines.end() ? english : found->second.c_str();
}

std::string vformat(std::string_view english, std::format_args arguments) {
    const auto& lines = current().lines;
    if (const auto found = lines.find(english); found != lines.end()) {
        auto wanted = placeholders(english), given = placeholders(found->second);
        std::ranges::sort(wanted);
        std::ranges::sort(given);
        if (wanted == given) {
            try {
                return std::vformat(found->second, arguments);
            } catch (const std::format_error&) {}
        }
    }
    return std::vformat(english, arguments);
}

} // namespace dingosdk::launcher_language
