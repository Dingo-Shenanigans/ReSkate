#include "hall_of_meat.h"
#include <Windows.h>
#include <cmath>
#include <mutex>

namespace dingosdk::hall_of_meat {
namespace {
// A bail's ragdoll steps keep reporting the wipeout flag; the debounce folds
// them into the bail just opened. Pending causes also cover a runout (causes
// recorded, but no wipeout follows) and expire on their own, so a stumble
// never shows up inside the next crash.
constexpr std::uint64_t bail_debounce_ms = 2500, pending_ttl_ms = 3000;
constexpr std::size_t kept_bails = 8, pending_limit = 16;

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<Bail, kept_bails> history{}; // newest first
    std::size_t history_count{};
    std::array<Impact, pending_limit> pending{};
    std::array<std::uint64_t, pending_limit> pending_at{};
    std::size_t pending_count{};
    std::uint64_t last_bail{};
    bool bail_open{};
};
State& state() { static auto* value = new State; return *value; }

void prune(State& s, std::uint64_t now) noexcept {
    std::size_t kept{};
    for (std::size_t index = 0; index < s.pending_count; ++index)
        if (now - s.pending_at[index] < pending_ttl_ms) {
            s.pending[kept] = s.pending[index];
            s.pending_at[kept] = s.pending_at[index];
            ++kept;
        }
    s.pending_count = kept;
}

void record(State& s, std::uint64_t now, const std::array<std::uint8_t, bone_contact_count>& bones,
    const BoneHit* bone_hit_data, std::size_t bone_hit_data_count) noexcept {
    Bail bail;
    bail.at = now;
    bail.bone_contacts = bones;
    bail.bone_hit_count = bone_hit_data_count < max_bone_hits ? bone_hit_data_count : max_bone_hits;
    for (std::size_t index = 0; index < bail.bone_hit_count; ++index)
        bail.bone_hits[index] = bone_hit_data[index];
    for (const auto flag : bones)
        if (flag) bail.body_contact = true;
    for (std::size_t index = 0; index < s.pending_count; ++index)
        bail.magnitude += s.pending[index].magnitude;
    bail.impact_count = s.pending_count < bail.impacts.size() ? s.pending_count : bail.impacts.size();
    for (std::size_t index = 0; index < bail.impact_count; ++index)
        bail.impacts[index] = s.pending[s.pending_count - bail.impact_count + index];
    const std::size_t moved = s.history_count < kept_bails ? s.history_count + 1 : kept_bails;
    for (std::size_t index = moved - 1; index > 0; --index) s.history[index] = s.history[index - 1];
    s.history[0] = bail;
    if (s.history_count < kept_bails) ++s.history_count;
    s.last_bail = now;
    s.bail_open = true;
}
}

void observe_cause(std::int32_t reason, float magnitude) noexcept {
    if (!std::isfinite(magnitude)) return;
    auto& s = state();
    const auto now = GetTickCount64();
    AcquireSRWLockExclusive(&s.lock);
    prune(s, now);
    if (s.pending_count < pending_limit) {
        s.pending[s.pending_count] = {reason, magnitude};
        s.pending_at[s.pending_count] = now;
        ++s.pending_count;
    }
    ReleaseSRWLockExclusive(&s.lock);
}

bool observe_wipeout(const std::array<std::uint8_t, bone_contact_count>& bone_contacts,
    const BoneHit* bone_hit_data, std::size_t bone_hit_data_count, Bail* recorded) noexcept {
    auto& s = state();
    const auto now = GetTickCount64();
    bool opened{};
    Bail bail;
    AcquireSRWLockExclusive(&s.lock);
    if (!s.bail_open || now - s.last_bail >= bail_debounce_ms) {
        prune(s, now);
        record(s, now, bone_contacts, bone_hit_data, bone_hit_data_count);
        bail = s.history[0];
        opened = true;
    }
    ReleaseSRWLockExclusive(&s.lock);
    if (opened && recorded) *recorded = bail;
    return opened;
}

std::vector<Bail> recent() {
    auto& s = state();
    AcquireSRWLockShared(&s.lock);
    const std::vector<Bail> result(s.history.begin(), s.history.begin() + static_cast<std::ptrdiff_t>(s.history_count));
    ReleaseSRWLockShared(&s.lock);
    return result;
}

void forget() noexcept {
    auto& s = state();
    AcquireSRWLockExclusive(&s.lock);
    s.history_count = s.pending_count = 0;
    s.bail_open = false;
    ReleaseSRWLockExclusive(&s.lock);
}
}
