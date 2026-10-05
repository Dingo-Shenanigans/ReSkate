#pragma once

// This fork always excludes the upstream Steam sign-in/depot implementation.
// Keep the upstream sections behind this guard so their edits merge normally.
#ifdef RESKATE_STEAM_DOWNLOADS_DISABLED
#error RESKATE_STEAM_DOWNLOADS_DISABLED is controlled by Fork/steam_privacy.h
#endif
#define RESKATE_STEAM_DOWNLOADS_DISABLED 1

namespace dingosdk::steam_privacy {

inline constexpr const char* check_label = "CHECK AGAIN";
inline constexpr const char* check_detail = "Check the installed game files";
inline constexpr const char* settings_check_label = "Check installed game";
inline constexpr const char* unsupported_detail =
    "Use an installed copy of the supported Skate build. Steam downloads are disabled in this launcher.";
inline constexpr const char* missing_detail =
    "Place ReSkateLauncher.exe and ReSkate.dll beside an existing Skate.exe, then check again.";
inline constexpr const char* headless_error =
    "Skate.exe is missing or is not the supported build. "
    "Use an installed copy of the supported Skate build; Steam downloads are disabled in this launcher.";

} // namespace dingosdk::steam_privacy
