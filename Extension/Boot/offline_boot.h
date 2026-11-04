#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {

struct OfflineBootObservation {
    std::string json;
};

// The production offline boot path: ReSkateLauncher always sets
// RESKATE_GLOBAL_OFFLINE=1. Hooks the inspected shared OnlineEnabled
// function and replaces its result only for five exact, inspected caller return
// RVAs. It requires an initialized Detours hook service and reads the variable
// exactly once, on the first initialization attempt. A successfully activated
// hook is retained for the process lifetime.
bool start_offline_boot(std::uintptr_t image_base) noexcept;
// Whether the launcher asked for the offline boot path: RESKATE_GLOBAL_OFFLINE=1,
// or RESKATE_DEBUG_TEST_GLOBAL_OFFLINE=1, the name launchers set before it was
// renamed, so a new ReSkate.dll started by an older launcher still boots offline.
bool offline_boot_requested() noexcept;
// Permit only the onboarding event sender after the durable local adapter is active.
void set_local_profile_event_provider(std::uintptr_t image_base, bool active) noexcept;
// A validated local category provider may service only the Object Browser constructor.
using LocalObjectBrowserReady = bool (*)() noexcept;
void set_local_object_browser_provider(std::uintptr_t image_base, LocalObjectBrowserReady ready) noexcept;
OfflineBootObservation offline_boot_observation();

} // namespace dingosdk
