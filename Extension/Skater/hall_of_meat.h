#pragma once
#include <array>
#include <cstdint>
#include <vector>

// Hall of Meat: the local skater's bail impacts, as the game's own cause
// collector and sensitive body-contact report report them (contracts in
// Engine/Game/Build/20260929/no_bail.h). The bail hooks feed observe_cause and
// observe_wipeout from the game's threads; the menu reads recent().
namespace dingosdk::hall_of_meat {

// One recorded bail cause: the game's reason code and its impact magnitude.
struct Impact {
    std::int32_t reason{};
    float magnitude{};
};

// A wipeout: the causes recorded around it and the game's body-contact flag
// (a configured body bone hit a solid object during the bail). Per-bone
// attribution lands here once the contacts records are mapped.
struct Bail {
    std::uint64_t at{};                // GetTickCount64() of the wipeout
    float magnitude{};                 // the pending impacts' magnitudes, added up
    bool body_contact{};
    std::array<Impact, 12> impacts{};  // arrival order, oldest first
    std::size_t impact_count{};
};

// Feed from the bail hooks, local skater only. noexcept: nothing allocates,
// and a cause that does not fit is dropped rather than reported.
void observe_cause(std::int32_t reason, float magnitude) noexcept;
// Snapshot the pending causes when the wipeout flag arrives for the local
// skater; body_contact is the game's report read in the same step. True when
// this opened a new bail — the ragdoll's follow-up wipeout steps are folded
// into the bail just recorded. `recorded` receives the new bail when non-null.
bool observe_wipeout(bool body_contact, Bail* recorded = nullptr) noexcept;
// The kept bails, newest first. Bounded; the menu thread may allocate.
std::vector<Bail> recent();
// The menu's Clear button: drop the history and any pending causes.
void forget() noexcept;
}
