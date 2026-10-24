#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Throwdown modes that plugins add to the game's Throwdowns page. A mode is a card on the picker
// (cloned from a stock card), the game's own Spot Battle setup page relabelled from the plugin's
// setup document (throwdown_kit.h), and its native start flag: the plugin runs the mode itself.
// Thread-safe; the menu reads it on the UI thread, plugins write it from theirs.
namespace dingosdk::multiplayer::custom_throwdowns {
struct Mode {
    std::string plugin, id, name, description;
    // The picker logo: a texture or image asset the plugin's mod adds, by path
    // (a bare name is a Throwdown_ModeSelect_ContentResources record). Empty:
    // the donor card's art.
    std::string art;
    // The placement-preview prefab the native flag shows ("World/.../duel_Placement",
    // or an InstanceId name of the plugin's mod). Empty: Spot Battle's own.
    std::string flag;
    // The stock card it is cloned from: "skate", "spot" or "jam" (throwdown_kit.h).
    std::string card = "spot";
    // The stock throwdown whose place on the picker it takes: "skate", "spot"
    // or "jam"; empty: after them.
    std::string replaces;
    std::uint32_t color{}; // the card's tint; 0: one picked from the mode's id
    int order{};           // among the plugin modes on the picker, lowest first
    int min_players = 1, max_players = 6, players = 4;
    // > 0: the placement cursor of its flag may rise this far (metres), so the flag can stand on
    // anything (kit::flag::Options::cursor_height); 0: the game's own ceiling.
    float cursor_height{};
    std::string key() const { return plugin + "/" + id; }
    // 0 S.K.A.T.E., 1 Spot Battle, 2 Skate Jam (challenge_registry.h's donors).
    unsigned donor() const noexcept { return card == "skate" ? 0U : card == "jam" ? 2U : 1U; }
};

// Empty when accepted, else why not.
std::string set_mode(Mode mode);
void remove_mode(std::string_view plugin, std::string_view id);
void remove_plugin(std::string_view plugin);
std::vector<Mode> modes();
std::optional<Mode> find(std::string_view key);
// Changes whenever a mode is set or removed.
std::uint64_t revision() noexcept;
} // namespace dingosdk::multiplayer::custom_throwdowns
