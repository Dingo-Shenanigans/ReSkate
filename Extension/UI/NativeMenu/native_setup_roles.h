#pragma once
#include <string_view>

namespace dingosdk::multiplayer::menu_data {
enum class NativeSetupRole { other, players, timer, rounds };

// The stock Spot Battle setup rows, by their authored localization keys or the
// English captions the page can already show when it becomes ready. Only the
// stock captions: once relabelled from a plugin's setup document a row is known
// by its presenter, not its text.
inline NativeSetupRole native_setup_role(std::string_view text) noexcept {
    if(text=="ID_SETTINGS_PLAYERS" || text=="Players")return NativeSetupRole::players;
    if(text=="ID_LABEL_TURNTIMER" || text=="ID_LABEL_DURATION" || text=="Turn Timer" ||
       text=="Timer" || text=="Duration")return NativeSetupRole::timer;
    if(text=="ID_LABEL_ROUNDS" || text=="Rounds")return NativeSetupRole::rounds;
    return NativeSetupRole::other;
}
// A stock row's id in a setup document's hide_native list and setup_row events.
inline constexpr std::string_view native_setup_role_id(NativeSetupRole role) noexcept {
    return role==NativeSetupRole::players?"players":role==NativeSetupRole::timer?"timer":
        role==NativeSetupRole::rounds?"rounds":"";
}
} // namespace dingosdk::multiplayer::menu_data
