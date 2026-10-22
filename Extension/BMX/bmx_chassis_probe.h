#pragma once
#include <cstdint>

namespace dingosdk::bmx {
// One explicit development-only request per process. This is an independent
// native capsule diagnostic, never a mounted bike or a replacement rider.
// RESKATE_BMX_CHASSIS_PROBE_PULSE=1 additionally opts into a single experimental
// force/torque pulse only in an observed matching retail producer phase. The
// observed60Hz accumulator is integrated with dt, not an immediate impulse.
// Optional variant 'mass-inertia-terrain' is captured once at request time;
// it selects mass25, m/I={2,3,4}, and authored terrain-only masks. This is not proof
// of a world-wide physics-worker barrier; an unmatched request is withheld.
bool request_chassis_probe() noexcept;
// Client update thread. gameplay_ready is native readiness, not network activity.
// The caller must terminate its isolated process after the report: allocation
// destruction has not been established; stopping the capsule is not reclamation.
void tick_chassis_probe(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready) noexcept;
}
