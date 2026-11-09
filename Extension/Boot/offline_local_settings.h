#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {

// Called only during the launcher's suspended, pre-resume initialization. An explicit
// RESKATE_OFFLINE_LOCAL_SETTINGS=1 request requires the installed offline boot path,
// Steam-offline mode and a successfully redirected, checked development data root.
// Unselected launches install nothing. A selected failure stops startup with `error`.
bool start_offline_local_settings(std::uintptr_t image_base, std::string& error) noexcept;

} // namespace dingosdk
