// By hand: what the music player outside the game says it is playing, for a few seconds, and
// optionally one command to it. external_player_probe [toggle|next|previous|play|pause]
#include "Extension/Music/external_player.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace dingosdk;

int main(int argc, char** argv) {
    external_player::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (argc > 1) {
        const std::string word = argv[1];
        const auto command = word == "next" ? external_player::Command::next : word == "previous" ? external_player::Command::previous
                           : word == "play" ? external_player::Command::play : word == "pause" ? external_player::Command::pause
                                                                                               : external_player::Command::toggle;
        external_player::send(command);
        std::cout << "sent " << word << '\n';
    }
    std::uint64_t seen = ~0ull;
    for (int i = 0; i < 12; ++i) {
        if (const auto revision = external_player::revision(); revision != seen) {
            seen = revision;
            const auto track = external_player::now();
            if (!track.available) std::cout << "no player with a media session\n";
            else
                std::cout << (track.spotify ? "Spotify" : track.app) << (track.playing ? " playing: " : " paused: ") << track.artist << " - " << track.title
                          << " [" << track.album << "] cover " << track.artwork.size() << " bytes; next " << track.can_next << " previous "
                          << track.can_previous << '\n';
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    external_player::stop();
    return 0;
}
