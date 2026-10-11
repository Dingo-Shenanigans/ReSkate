#pragma once
#include <cstdint>

// The local skater's tattoos, clothing and board are composited into textures once, when the items
// are put on, at the compositor quality of that moment. A change of Texture Quality (issue #160) or
// Texture Filtering leaves them as they were until another item is chosen. So after such a change,
// once no game menu is up, the outfit (not the body) and the board's items are taken off for a
// moment and given back: the game puts them on again and composites them at the current quality.
// Other players' skaters are left as they are.
namespace dingosdk::texture_refresh {
// Any thread: asks for a refresh at the next moment no menu is up.
void request() noexcept;
// Any thread: a refresh has the skater or board until it has given them back.
bool busy() noexcept;
// Client thread.
void on_client_tick(std::uintptr_t base, std::uintptr_t client) noexcept;
}
