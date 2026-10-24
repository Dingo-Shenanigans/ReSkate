#include "challenge_registry.h"
#include "custom_modes.h"
#include <algorithm>
#include <array>

namespace dingosdk::throwdowns {
Palette palette_for(std::string_view id) noexcept {
    std::uint32_t hash=2166136261U;
    for(const auto c:id) hash=(hash^static_cast<unsigned char>(c))*16777619U;
    // Authored Throwdown hues: fixed assignment across sessions and reloads.
    constexpr std::array<std::uint32_t,6> hues{0xffff8000U,0xffff5df2U,0xff00cfffU,0xff60d64bU,0xffed935bU,0xffdfae78U};
    return {hues[hash%hues.size()],0xfff3f0e5U,0xfff2cc52U,0xff171717U,0xffffffffU};
}
Registration Registry::add(Definition d) {
    // A plugin mode's id is "<plugin folder>/<id>": printable, no spaces.
    const auto valid_id=[](std::string_view id) {
        return !id.empty() && id.size()<=128 && std::all_of(id.begin(),id.end(),[](unsigned char c) { return c>0x20 && c<0x7f; });
    };
    if(!valid_id(d.id) || d.name.empty() || d.name.size()>64 || d.description.size()>1024 || d.donor>2)
        return {false,false,"Invalid challenge identity, description or artwork family."};
    if(d.palette.primary==0)d.palette=palette_for(d.id);
    if(const auto* existing=find(d.id)) {
        if(*existing!=d)
            return {false,false,"Challenge identifier already belongs to a different definition."};
        return {true,false,{}}; // Repeated initialization is idempotent.
    }
    definitions_.push_back(std::move(d));++revision_;
    return {true,true,{}};
}
const Definition* Registry::find(std::string_view id) const noexcept {
    const auto it=std::find_if(definitions_.begin(),definitions_.end(),[&](const auto& d){return d.id==id;});
    return it==definitions_.end()?nullptr:&*it;
}
// The stock S.K.A.T.E., Spot Battle and Skate Jam, each unless a plugin mode takes its place, then
// every other plugin mode by its order (ties: as registered). Rebuilt when the plugins' modes
// change; read on the UI thread.
Registry& registry() {
    static Registry value;
    static std::uint64_t built = ~0ULL;
    const auto revision = multiplayer::custom_throwdowns::revision();
    if (revision == built) return value;
    built = revision;
    auto modes = multiplayer::custom_throwdowns::modes();
    std::stable_sort(modes.begin(), modes.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
    const auto plugin_definition = [](const multiplayer::custom_throwdowns::Mode& mode) {
        Definition d;
        d.id = mode.key();
        d.name = mode.name;
        d.description = mode.description;
        d.logo = mode.art;
        d.plugin = mode.plugin;
        d.flag = mode.flag;
        d.adapter = Adapter::plugin;
        d.donor = mode.donor();
        d.replaces = mode.replaces;
        d.order = mode.order;
        d.palette = palette_for(d.id);
        if (mode.color) d.palette.primary = mode.color;
        d.min_players = mode.min_players;
        d.max_players = mode.max_players;
        d.players = mode.players;
        d.cursor_height = mode.cursor_height;
        return d;
    };
    auto& result = value;
    result.clear();
    std::vector<std::string> placed;
    struct Stock {
        const char *tile, *id, *name, *description, *logo;
        Adapter adapter;
        unsigned donor;
    };
    constexpr std::array<Stock, 3> stock{{
        {"skate", "native.skate", "S.K.A.T.E.", "Match the leader's trick or take a letter.", "ThrowdownSkate", Adapter::native_skate, 0},
        {"spot", "native.spot", "Spot Battle", "Earn the highest score in a single round.", "SpotBattle", Adapter::native_spot, 1},
        {"jam", "native.jam", "Skate Jam", "Accumulate points across all tricks.", "ThrowdownerActive", Adapter::native_jam, 2},
    }};
    for (const auto& s : stock) {
        const auto replacing = std::find_if(modes.begin(), modes.end(), [&](const auto& m) { return m.replaces == s.tile; });
        if (replacing != modes.end()) {
            result.add(plugin_definition(*replacing));
            placed.push_back(replacing->key());
            continue;
        }
        Definition d;
        d.id = s.id;
        d.name = s.name;
        d.description = s.description;
        d.logo = s.logo;
        d.adapter = s.adapter;
        d.donor = s.donor;
        d.palette = palette_for(d.id);
        result.add(std::move(d));
    }
    for (const auto& mode : modes)
        if (std::find(placed.begin(), placed.end(), mode.key()) == placed.end()) result.add(plugin_definition(mode));
    return value;
}
}
