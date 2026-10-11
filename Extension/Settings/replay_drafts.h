#pragma once
#include <cstdint>

// Replay drafts. The replay editor saves a draft as a pin on the recording, which is local,
// and a thumbnail and label in the game's media catalogue, which the Drafts screen lists from.
// The game makes that catalogue for the user signed in to its backend; with no backend there
// is neither the service nor a user, and a saved draft has nowhere to be listed. This gives
// the game the service and a local user, both with its own functions: the catalogue is then
// the one the game keeps on disk, and its Drafts screen works as it was built.
// A draft's footage stays in memory, as it does in the game: it does not outlive the session.
namespace dingosdk::replay_drafts {
// On the client thread, at the world model's pace, with the client's state.
void on_client_tick(std::uintptr_t base, std::uint32_t state) noexcept;
}
