#pragma once

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The launcher in the player's language. Text is looked up by its English, as the source
// writes it, so English needs no file and a translator sees each sentence whole. A language is
// a JSON object of {"English": "translation"}, with "@language" its own name: the ones shipped
// are embedded in the exe (Launcher/languages/<code>.json), and languages\<code>.json beside
// ReSkateLauncher.exe adds a language or overrides lines of an embedded one. Text a language
// leaves out, or leaves empty, shows in English. contrib/launcher-language.md has the rest.
namespace dingosdk::launcher_language {

struct Language {
    std::string code;   // "tr": its file's name
    std::string name;   // in itself: "Türkçe"
};
// English first, then every language found, by code.
const std::vector<Language>& languages();
// Shows `code` from now on; English for "" or a language there is none of. Any thread.
void choose(std::string_view code);
// The language shown now.
const std::string& chosen();
// The code of the language Windows shows its own text in, when the launcher has it; else "".
std::string system_language();

// Marks English that is translated where it is shown (a table's labels), for the template.
constexpr const char* translatable(const char* english) { return english; }
// `english` in the chosen language, or `english` itself. A translation lives as long as the
// process, so it can be kept and shown on any thread.
const char* tr(const char* english);
// std::format with the translation of `english`. One whose placeholders differ from the
// English's is not used.
std::string vformat(std::string_view english, std::format_args arguments);
template<class... Args> std::string trf(std::format_string<Args...> english, Args&&... arguments) {
    return vformat(english.get(), std::make_format_args(arguments...));
}

} // namespace dingosdk::launcher_language
