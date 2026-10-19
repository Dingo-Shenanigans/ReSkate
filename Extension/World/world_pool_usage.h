#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {
// With RESKATE_GPU_DIAGNOSTICS on: how full the world pools ReSkate enlarges get (entity slots,
// render instances, spatial blocks per bucket), against the game's own sizes, said in the log as
// each grows. A level that needs more than the game's own size of one of them depends on
// ReSkate's enlargement, and on whatever else in the game was sized to match. Read on a thread of
// its own, once a second. Installs before executable entry; off without diagnostics.
bool start_world_pool_usage(std::uintptr_t base, std::string& error);
} // namespace dingosdk
