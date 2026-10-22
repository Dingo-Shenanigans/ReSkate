#pragma once
#include <cstdint>
#include <string>
namespace dingosdk::bmx {
// Private explicit synthetic-source quick-switch rehearsal, never ordinary riding.
bool request_quick_switch_test() noexcept;
// Requested from the command dispatcher; acquisition happens on the next eligible client tick.
void request_mount(bool mount) noexcept;
// Toggle BMX and the native skateboard. Y/Triangle still dismounts to walking.
void request_toggle() noexcept;
void request_interaction(bool carry=false) noexcept;
// Unattended is an explicit, bounded development-root-only synthetic input request.
bool request_selftest(bool unattended = false) noexcept;
bool request_chassis_pause_test() noexcept;
// Explicit private-profile native skateboard fall observation; not a BMX bail.
void request_native_fall_observation() noexcept;
// One explicit typed-intent diagnostic in its separate private impact lab.
bool request_getup_test() noexcept;
bool mounted() noexcept;
std::string status();
std::string source_status();
void tick(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready) noexcept;
void stop() noexcept;
}
