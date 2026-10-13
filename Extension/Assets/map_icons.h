#pragma once

// The pictures for a dedicated server's vote for the next map. The server says which maps the
// vote will offer about a minute before it (multiplayer::next_maps); each one's icon is found
// then, off the game's thread: from its mod folder when the map is installed here, else its
// Thunderstore page's. They go to the overlay (overlay::set_map_vote_icons), which has them
// ready when the vote comes up.
namespace dingosdk::map_icons {
// Client thread, every tick: starts a fetch when the server's maps have changed.
void tick();
}
