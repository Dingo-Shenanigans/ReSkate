// The trick feed's gate (Extension/Skater/trick_feed_gate.h), apart from the game: a song title the
// music writes to the name's field never goes out as a trick, while a trick's name does, also when
// its name comes before anything that shows its model is the trick list's.
#include "Extension/Skater/trick_feed_gate.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace dingosdk::tricks::feed;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using Gate = RecordGate<std::string>;
constexpr std::uintptr_t tricks = 0x1000, music = 0x2000;
}

int main() {
    try {
        Gate gate;
        std::vector<std::string> out;
        // The song title, written over and over from the music's model: held, then left out.
        gate.add("Uptown Top Ranking", music, false, 0, out);
        gate.add("Uptown Top Ranking", music, false, 500, out);
        check(out.empty(), "a name from a model not known to be the trick list's went out at once");
        // A trick's name, then its stance: the stance shows the model is the trick list's, and the
        // name waiting for it goes first.
        gate.add("Kickflip", tricks, false, 600, out);
        gate.add("ID_STANCE_NOLLIE", tricks, true, 610, out);
        check((out == std::vector<std::string>{"Kickflip", "ID_STANCE_NOLLIE"}), "a trick's name did not go out before its stance");
        out.clear();
        // From then on its names go out at once.
        gate.add("Boned Ollie", tricks, false, 700, out);
        check((out == std::vector<std::string>{"Boned Ollie"}), "a known trick list's name waited");
        out.clear();
        // The song's notes are left out once they have waited long enough, and never go out.
        gate.add("Beneath The Wheel", music, false, 2600, out);
        check(out.empty(), "a song title went out");
        const auto left = gate.take_left_out();
        check(left.size() == 2 && left[0] == "Uptown Top Ranking", "the song titles that waited were not left out");
        gate.expire(5000);
        check(gate.take_left_out() == std::vector<std::string>{"Beneath The Wheel"}, "a later song title was not left out");
        // Without a record (it could not be read), notes go out as they did before.
        gate.add("Ollie", 0, false, 5100, out);
        check((out == std::vector<std::string>{"Ollie"}), "a note with no record was held");
        out.clear();
        // Only so many wait at once.
        for (int i = 0; i < 70; ++i) gate.add("x", music, false, 6000, out);
        check(out.empty() && gate.take_left_out().size() == 70 - Gate::most_held, "more than the most held were kept");
        gate.clear();
        gate.add("Kickflip", tricks, false, 7000, out);
        check(out.empty(), "clearing the gate kept what it knew");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "trick feed gate tests passed\n";
    return 0;
}
