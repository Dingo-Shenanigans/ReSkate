#include "Server/suspicion.h"
#include <cstdlib>
#include <iostream>
#include <string>

using dingosdk::server::Suspicion;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
constexpr std::uint64_t second = 1000000;
}

int main() {
    {
        Suspicion s;
        check(s.add(second, Suspicion::Signal::object_on_player, 2) == Suspicion::Level::clean, "Two objects near a player restricted someone");
        check(s.add(2 * second, Suspicion::Signal::name_spoof) == Suspicion::Level::clean, "A spoofed name and two objects restricted someone");
        check(s.level(600 * second) == Suspicion::Level::clean && s.score(600 * second) < 0.5, "The score did not decay");
    }
    {
        Suspicion s;
        auto level = Suspicion::Level::clean;
        unsigned seconds{};
        for (; seconds < 60 && level != Suspicion::Level::kick; ++seconds)
            level = s.add(seconds * second, Suspicion::Signal::object_animation, 1.0);
        check(level == Suspicion::Level::kick && seconds < 25, "Animating objects nonstop was not kicked within 25 s");
        check(s.strongest(seconds * second) == Suspicion::Signal::object_animation, "The strongest signal was wrong");
    }
    {
        Suspicion s;
        auto level = Suspicion::Level::clean;
        for (unsigned pass = 0; pass < 20; ++pass) level = s.add(second + pass * 100000, Suspicion::Signal::object_on_player, 0.2);
        check(level == Suspicion::Level::clean, "Dragging one object past a skater for two seconds restricted someone");
    }
    {
        Suspicion s;
        auto level = Suspicion::Level::clean;
        unsigned pass{};
        for (; pass < 100 && level != Suspicion::Level::kick; ++pass)
            level = s.add(second + pass * 100000, Suspicion::Signal::object_on_player, 0.5);
        check(level == Suspicion::Level::kick && pass <= 30, "Bells kept moving on a player were not kicked within 3 s");
        check(s.kicked(second) == 0 && !s.locked(2 * second), "A first kick locked the player out");
        check(s.level(2 * second) == Suspicion::Level::restricted, "A kicked player was not restricted on return");
        s.add(3 * second, Suspicion::Signal::pinned_board);
        const auto lockout = s.kicked(3 * second);
        check(lockout == 15 * 60 * second && s.locked(4 * second) && !s.locked(3 * second + lockout), "A second kick did not lock out for 15 min");
        s.kicked(5 * second);
        check(s.kicks() == 3 && s.locked(5 * second + 59 * 60 * second), "A third kick did not lock out for an hour");
    }
    {
        Suspicion s;
        auto level = Suspicion::Level::clean;
        for (unsigned i = 0; i < 3600; ++i) level = s.add(i * second, Suspicion::Signal::bone_scale, 0.05);
        check(level == Suspicion::Level::clean, "A slightly resized skater alone got restricted");
    }
    {
        Suspicion s;
        auto level = Suspicion::Level::clean;
        for (unsigned i = 0; i < 3600; ++i) level = s.add(i * second, Suspicion::Signal::bone_scale, 4.0);
        check(level == Suspicion::Level::restricted, "A big-head mod alone was not just restricted");
        check(s.add(3600 * second, Suspicion::Signal::object_on_player, 6) == Suspicion::Level::kick, "Resizing plus objects on players was not a kick");
    }
    return 0;
}
