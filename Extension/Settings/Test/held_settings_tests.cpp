#include "Extension/Settings/named_settings.h"
#include <iostream>
#include <map>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
std::map<std::string, std::string, std::less<>> game;
bool refuse_writes{};
std::optional<std::string> read(std::string_view name) {
    const auto found = game.find(name);
    return found != game.end() ? std::optional(found->second) : std::nullopt;
}
std::string write(std::string_view name, std::string_view value) {
    if (value == "refused" || refuse_writes) return "error: refused";
    game[std::string(name)] = std::string(value);
    return "ok";
}
using Settings = std::vector<std::pair<std::string_view, std::string_view>>;
} // namespace

int main() {
    using dingosdk::HeldSettings, dingosdk::still_held;
    check(still_held("60", "60") && still_held("60.000", "60") && still_held("1", "1.0"), "the same number is still held");
    check(!still_held("30", "60") && !still_held("0.5", "1"), "a value someone changed is not");
    check(!still_held("", "60") && !still_held("fast", "60"), "a value that is not a number is not");
    const auto fresh = [] {
        game = {{"Rate", "30"}, {"Scale", "1"}};
        return HeldSettings(read, write);
    };
    std::string error;
    auto held = fresh();
    check(held.hold(Settings{{"Rate", "60"}, {"Scale", "1.0"}}, error) && game["Rate"] == "60", "a setting is set");
    check(held.release().empty() && game["Rate"] == "30" && game["Scale"] == "1",
          "and put back; a setting that held the value already is left alone");
    held = fresh();
    (void)held.hold(Settings{{"Rate", "60"}}, error);
    game["Rate"] = "45";
    check(held.release().empty() && game["Rate"] == "45", "a setting someone changed stays as they left it");
    held = fresh();
    (void)held.hold(Settings{{"Rate", "60"}}, error);
    game.erase("Rate");
    check(held.release() == std::vector<std::string>{"Rate"}, "a setting that cannot be read back is named");
    held = fresh();
    check(!held.hold(Settings{{"Rate", "60"}, {"Missing", "1"}}, error) && error == "the game's Missing setting could not be read",
          "an unreadable setting is not held");
    check(held.release().empty() && game["Rate"] == "30", "the settings held before it are put back");
    check(!held.hold(Settings{{"Rate", "refused"}}, error) && error.starts_with("the game's Rate could not be set to refused"),
          "a refused write says why");
    held = fresh();
    (void)held.hold(Settings{{"Rate", "60"}}, error);
    refuse_writes = true;
    check(held.release() == std::vector<std::string>{"Rate"} && game["Rate"] == "60", "a setting whose put-back is refused is named");
    refuse_writes = false;
    if (!failures) std::cout << "held settings tests passed\n";
    return failures ? 1 : 0;
}
