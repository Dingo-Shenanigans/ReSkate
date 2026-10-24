#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Throwdowns page's cards: the game's three stock throwdowns and every plugin mode
// (custom_modes.h), in SmokeyDawg58's logo grid. Built from the plugins' modes; nothing here knows
// a particular mode.
namespace dingosdk::throwdowns {
enum class Adapter : std::uint8_t { native_skate, native_spot, native_jam, plugin };
struct Palette {
    std::uint32_t primary{}, secondary{}, accent{}, background=0xff171717U, text=0xffffffffU;
    bool operator==(const Palette&) const = default;
};
struct Definition {
    // A plugin mode's id is its custom_modes key ("<plugin>/<id>"); the stock ones are
    // "native.skate", "native.spot" and "native.jam".
    std::string id, name, description, logo;
    std::string plugin;   // empty for the stock throwdowns
    std::string flag;     // the mode's placement-preview prefab (custom_modes Mode::flag)
    Adapter adapter{};
    unsigned donor=1;     // the stock card it is cloned from: 0 S.K.A.T.E., 1 Spot Battle, 2 Skate Jam
    std::string replaces; // "skate" | "spot" | "jam": the stock tile whose place it takes
    int order{};          // the grid order among plugin modes
    Palette palette;
    int min_players=1, max_players=6, players=4;
    float cursor_height{}; // > 0: its flag's placement cursor may rise this far (custom_modes Mode::cursor_height)
    bool operator==(const Definition&) const = default;
};
struct Registration {
    bool accepted{}, inserted{};
    std::string error;
};
class Registry {
    std::vector<Definition> definitions_;
    std::uint64_t revision_{};
public:
    Registration add(Definition definition);
    const Definition* find(std::string_view id) const noexcept;
    std::span<const Definition> definitions() const noexcept { return definitions_; }
    std::uint64_t revision() const noexcept { return revision_; }
    // Empties it for a rebuild; the revision still only grows, so a reader sees the change.
    void clear() noexcept {
        definitions_.clear();
        ++revision_;
    }
};
Palette palette_for(std::string_view stable_id) noexcept;
// Rebuilt when the plugins' modes change; read on the UI thread.
Registry& registry();
}
