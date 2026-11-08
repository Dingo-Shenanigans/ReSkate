#pragma once
#include "skate_menu_internal.h"
#include <array>
#include <string>
#include <string_view>

// Shared by the Multiplayer page's files (multiplayer_*.cpp).
namespace dingosdk::overlay::menu::multiplayer_detail {
// Sends a private multiplayer action; the password is wiped once it is accepted.
bool send_private(SkateMenu &menu, const char *action, const std::string &argument,
                  std::array<char, 65> &password, bool use_password = true);
std::string map_label(const Model &model, std::string_view path);
// SERVERS tab and its password prompt (multiplayer_lobbies.cpp).
void join_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks);
void password_popup(SkateMenu &menu, const Model &model);
// SESSION and VOICE tabs (multiplayer_session.cpp).
void session_page(SkateMenu &menu, const Model &model);
// BANS tab and the Ban button's confirmation (multiplayer_session.cpp).
void bans_page(SkateMenu &menu, const Model &model);
void ban_popup(SkateMenu &menu);
void voice_controls(SkateMenu &menu, const MultiplayerModel &mp);
} // namespace dingosdk::overlay::menu::multiplayer_detail
