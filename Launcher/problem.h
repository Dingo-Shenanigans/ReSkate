#pragma once

#include <string>
#include <string_view>

#include "language.h"

// Failures are logged in the words that help whoever reads ReSkate.log, which
// are rarely the words that help whoever is trying to skate. This turns one
// into the other: what went wrong, and what to do about it. The raw message
// stays in the log, and the STATUS tile can still copy it.
namespace dingosdk::launcher_problem {

using launcher_language::tr;
using launcher_language::translatable;

struct Problem {
    std::string headline;   // what went wrong, in a line
    std::string advice;     // the next thing to try
};

inline bool mentions(std::string_view raw, std::string_view needle) {
    return raw.find(needle) != std::string_view::npos;
}

// Anti-virus and overlays are behind most injection failures, so they share
// one piece of advice.
inline constexpr const char* attach_advice = translatable(
    "Anti-virus software usually blocks this. Add your skate. folder as an exclusion, close overlays "
    "(Discord, MSI Afterburner, RTSS), then press RETRY.");

inline Problem explain(std::string_view raw) {
    // Most specific first: the later rules match whole families of failures.
    if (mentions(raw, "must be beside ReSkateLauncher.exe"))
        return {tr("ReSkate's files are not together"),
                tr("Keep ReSkateLauncher.exe and ReSkate.dll together in a folder of their own (not Steam's "
                   "skate. folder) and run the launcher from there. It installs the game into that folder.")};
    if (mentions(raw, "ReSkate.dll is missing"))
        return {tr("ReSkate.dll is missing"),
                tr("Extract ReSkateLauncher.exe and ReSkate.dll from the same release zip into this folder, "
                   "then press RETRY.")};
    if (mentions(raw, "ReSkate.dll is not an x64") || mentions(raw, "DingoSDKDebugInitialize is outside"))
        return {tr("ReSkate.dll is damaged"),
                tr("Download the release zip again and extract both files over this folder. Your anti-virus may "
                   "also have quarantined part of it.")};
    if (mentions(raw, "Skate.exe is missing"))
        return {tr("Skate.exe is not in this folder"),
                tr("Run the launcher from your skate. folder, or press INSTALL to download the game here.")};
    if (mentions(raw, "steam_api64.dll is missing") || mentions(raw, "steam_api64"))
        return {tr("The original steam_api64.dll is missing"),
                tr("Verify the game files in Steam, or press DOWNLOAD to fetch the supported build again.")};
    if (mentions(raw, "This launcher is out of date"))
        return {tr("This launcher is out of date"),
                tr("Download the newest ReSkate release from GitHub and extract it over this folder.")};
    if (mentions(raw, "update server could not be reached"))
        return {tr("The ReSkate update server could not be reached"),
                tr("Check your internet connection and press RETRY. If the game files are already correct you can "
                   "turn on Offline mode in Settings and play without the check.")};
    if (mentions(raw, "not supported"))
        return {tr("This copy of skate. is not the supported build"),
                tr("Press DOWNLOAD to get the supported build with your Steam account. Only the files that differ "
                   "are downloaded.")};
    if (mentions(raw, "Steam download did not finish"))
        return {tr("The Steam download did not finish"),
                tr("Check that the account you signed in with owns skate. and that you have about 14 GB free, "
                   "then press DOWNLOAD again. Signing in again often clears it.")};
    if (mentions(raw, "installing the game content cache"))
        return {tr("Another ReSkate launcher is busy"),
                tr("It is installing the game data this build needs. Wait for it to finish, then press RETRY.")};
    if (mentions(raw, "content cache"))
        return {tr("The one-time game data download failed"),
                tr("ReSkate downloads the game's catalogues once per build. Check your internet connection and "
                   "press RETRY.")};
    if (mentions(raw, "need different keys"))
        return {tr("The menu and console share a key"),
                tr("Open Settings > KEYS and pick a different key for one of them.")};
    // ERROR_ELEVATION_REQUIRED. Windows localises the text, so match the
    // number win32_failure puts in front of it.
    if (mentions(raw, "failed (740)"))
        return {tr("Skate.exe is set to always run as administrator"),
                tr("ReSkate has to start Skate itself, and Windows does not let it start a program marked to "
                   "need administrator. Right-click Skate.exe, pick Properties, then Compatibility, and untick "
                   "\"Run this program as an administrator\". Starting ReSkateLauncher.exe as "
                   "administrator works too.")};
    if (mentions(raw, "LoadLibraryW is hooked")) {
        std::string advice = tr("Security software or an overlay is modifying Skate while it starts, and ReSkate will "
                                "not load itself through that. Allow ReSkateLauncher.exe and Skate.exe in it, or close "
                                "it, then press RETRY.");
        // The launcher named what it found in Skate; that is the useful half.
        if (const auto found = raw.find("Loaded into Skate"); found != std::string_view::npos)
            advice += " " + std::string(raw.substr(found));
        return {tr("Something is hooking Skate as it starts"), std::move(advice)};
    }
    if (mentions(raw, "Skate closed while it was starting"))
        return {tr("Skate closed while it was starting"), tr(attach_advice)};
    if (mentions(raw, "timed out after"))
        return {tr("Skate took too long to start"),
                tr("A slow disk or an anti-virus scan usually causes this. Press RETRY; if it keeps happening, add "
                   "your skate. folder as an exclusion.")};
    if (mentions(raw, "validated Skate.exe") || mentions(raw, "loaded-image") || mentions(raw, "DOS header") ||
        mentions(raw, "module list") || mentions(raw, "VirtualAllocEx") || mentions(raw, "NtQueryInformationProcess") ||
        mentions(raw, "DingoSDKDebugInitialize") || mentions(raw, "Remote "))
        return {tr("ReSkate could not attach to Skate"), tr(attach_advice)};
    // Windows reports a refused write in the user's own language, so these
    // key on the operation names and the path, which are never translated.
    const bool write_failure = mentions(raw, "create_directories") || mentions(raw, "create_directory") ||
                               mentions(raw, "Cannot write") || mentions(raw, "Cannot create") ||
                               mentions(raw, "Cannot publish") || mentions(raw, "Cannot append") ||
                               mentions(raw, "Cannot replace");
    if (write_failure)
        return {tr("ReSkate cannot write to its own folder"),
                tr("Move the whole ReSkate folder somewhere else, such as C:\\Games\\ReSkate, and start it from "
                   "there. Program Files and OneDrive do not let it write.")};
    // A message raised in the launcher's own words may have a translation.
    const std::string message(raw);
    return {tr(message.c_str()),
            tr("Press RETRY. If it keeps happening, copy the details below and share them with logs\\ReSkate.log "
               "on the ReSkate Discord.")};
}

} // namespace dingosdk::launcher_problem
