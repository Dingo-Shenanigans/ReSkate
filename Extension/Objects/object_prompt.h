#pragma once
#include <cstdint>

// The game's card on a placed object names who placed it by asking for the local player's
// info, which is right in the game's own sessions (it only shows on your objects there) and
// wrong in ReSkate's, where other players' objects are spawned locally: every object read
// as your own. While that card's script runs, the local-player lookup answers with the owner
// of the object the card is for instead (native_party_hooks.cpp), which the script holds in
// one of its locals.
namespace dingosdk::object_prompt {
// The card's script (flow_buildkit aa058fc, OnBegin), as the expression VM names it (the
// hash at resource + 0x10).
inline constexpr std::uint32_t graph = 0x25bf9270;
} // namespace dingosdk::object_prompt
