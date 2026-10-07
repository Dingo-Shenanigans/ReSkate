#include "Extension/Music/local_music_playback_policy.h"
#include <cstdio>

namespace policy = dingosdk::profile_runtime::music_playback;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
    };

    check(policy::cursor_sync_sentinel == 0x7fffffffu,
          "native cursor synchronization sentinel is INT_MAX, distinct from queue -1");

    // The queue's global candidate asset pool can contain hundreds of songs while
    // the selected native playlist contains only its authored subset.
    std::array<std::uint8_t, 32> two_track_members{};
    two_track_members[0] = 0x12;
    two_track_members[16] = 0x34;
    const std::array<std::uint8_t, 16> second_track{0x34};
    std::uint32_t member_position{};
    check(policy::find_member_position(two_track_members.data(), 2, second_track, member_position) &&
          member_position == 1,
          "active current-song GUID is resolved against a playlist subset, independent of candidate-pool size");

    auto plan = policy::plan_cursor(false, policy::native_default_mode,
        policy::cursor_sync_sentinel, true, false, false, 0, 0);
    check(plan.mode == policy::native_sequential_mode && plan.set_mode,
          "sequential is selected when shuffle is off");
    check(plan.set_cursor && plan.cursor == 0, "a new sequential playlist starts at authored entry zero");

    plan = policy::plan_cursor(true, policy::native_sequential_mode, 4,
        false, false, true, 3, 6);
    check(plan.mode == policy::native_default_mode && plan.set_mode && !plan.set_cursor,
          "shuffle restores native default selection without moving its cursor");

    plan = policy::plan_cursor(false, policy::native_default_mode,
        policy::cursor_sync_sentinel, false, true, true, 2, 6);
    check(plan.set_mode && plan.set_cursor && plan.cursor == policy::cursor_sync_sentinel,
          "a pending explicit song request keeps native GUID synchronization");

    plan = policy::plan_cursor(false, policy::native_default_mode,
        policy::cursor_sync_sentinel, false, false, true, 0, 6);
    check(plan.set_mode && plan.cursor == 1,
          "turning shuffle off mid-play continues after the current first track");

    plan = policy::plan_cursor(false, policy::native_default_mode,
        policy::cursor_sync_sentinel, false, false, true, 5, 6);
    check(plan.cursor == 0, "sequential playback wraps after the final playlist entry");

    plan = policy::plan_cursor(false, policy::native_sequential_mode, 3,
        false, false, true, 2, 6);
    check(!plan.set_mode && !plan.set_cursor && plan.cursor == 3,
          "an active sequential cursor keeps progressing without being reseeded");

    plan = policy::plan_cursor(false, policy::native_sequential_mode, 3,
        true, false, true, 2, 6);
    check(plan.set_cursor && plan.cursor == 0,
          "a newly selected playlist starts at its first entry even if its current GUID matches");

    plan = policy::plan_cursor(false, policy::native_sequential_mode, 0,
        true, false, false, 0, 2);
    check(plan.set_cursor && plan.cursor == 0,
          "new playlist identity explicitly seeds entry zero even when the prior cursor was also zero");

    plan = policy::plan_cursor(false, policy::native_default_mode,
        policy::cursor_sync_sentinel, false, false, false, 0, 6);
    check(plan.cursor == 0, "an unresolvable current track safely restarts at the first entry");

    plan = policy::plan_cursor(false, policy::native_sequential_mode,
        policy::cursor_sync_sentinel, false, false, false, 0, 6);
    check(!plan.set_mode && plan.set_cursor && plan.cursor == 0,
          "a stale sync sentinel without a pending click cannot strand sequential selection");

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
