#pragma once

#include <string>
#include <string_view>

// Failures are logged in the words that help whoever reads ReSkate.log, which
// are rarely the words that help whoever is trying to skate. This turns one
// into the other: what went wrong, and what to do about it. The raw message
// stays in the log, and the STATUS tile can still copy it.
namespace dingosdk::launcher_problem {

struct Problem {
    std::string headline;   // what went wrong, in a line
    std::string advice;     // the next thing to try
};

inline bool mentions(std::string_view raw, std::string_view needle) {
    return raw.find(needle) != std::string_view::npos;
}

// An attach failure alone does not establish that antivirus blocked the game.
inline constexpr std::string_view attach_advice =
    "Check your antivirus history for a recorded detection and keep protection enabled. Close overlays "
    "(Discord, MSI Afterburner, RTSS), then press RETRY. If it still fails, report the error and log to the ReSkate developers.";

inline Problem explain(std::string_view raw) {
    // Most specific first: the later rules match whole families of failures.
    if (mentions(raw, "must be beside ReSkateLauncher.exe"))
        return {"ReSkate is not in your skate. folder",
                "Put ReSkateLauncher.exe and ReSkate.dll beside Skate.exe, in the folder Steam opens with "
                "skate. > Manage > Browse local files."};
    if (mentions(raw, "ReSkate.dll is missing"))
        return {"ReSkate.dll is missing",
                "Extract ReSkateLauncher.exe and ReSkate.dll from the same release zip into this folder, "
                "then press RETRY."};
    if (mentions(raw, "ReSkate.dll is not an x64") || mentions(raw, "DingoSDKDebugInitialize is outside"))
        return {"ReSkate.dll is damaged",
                "Download the release zip again and extract both files over this folder. Your anti-virus may "
                "also have quarantined part of it."};
    if (mentions(raw, "Skate.exe is missing"))
        return {"Skate.exe is not in this folder",
                "Put ReSkateLauncher.exe and ReSkate.dll beside an existing Skate.exe, then press RETRY."};
    if (mentions(raw, "steam_api64.dll is missing") || mentions(raw, "steam_api64"))
        return {"The original steam_api64.dll is missing",
                "Restore the original steam_api64.dll from your supported Skate installation, then press RETRY."};
    if (mentions(raw, "This launcher is out of date"))
        return {"This launcher is out of date",
                "Download the newest ReSkate release from GitHub and extract it over this folder."};
    if (mentions(raw, "update server could not be reached"))
        return {"The ReSkate update server could not be reached",
                "Check your internet connection and press RETRY. If the game files are already correct you can "
                "turn on Offline mode in Settings and play without the check."};
    if (mentions(raw, "not supported"))
        return {"This copy of skate. is not the supported build",
                "Use an installed copy of the supported Skate build. Steam downloads are disabled in this launcher."};
    if (mentions(raw, "installing the game content cache"))
        return {"Another ReSkate launcher is busy",
                "It is installing the game data this build needs. Wait for it to finish, then press RETRY."};
    if (mentions(raw, "content cache"))
        return {"The one-time game data download failed",
                "ReSkate downloads the game's catalogues once per build. Check your internet connection and "
                "press RETRY."};
    if (mentions(raw, "need different keys"))
        return {"The menu and console share a key",
                "Open Settings > KEYS and pick a different key for one of them."};
    // ERROR_ELEVATION_REQUIRED. Windows localises the text, so match the
    // number win32_failure puts in front of it.
    if (mentions(raw, "failed (740)"))
        return {"Skate.exe is set to always run as administrator",
                "ReSkate has to start Skate itself, and Windows does not let it start a program marked to "
                "need administrator. Right-click Skate.exe, pick Properties, then Compatibility, and untick "
                "\"Run this program as an administrator\"."};
    if (mentions(raw, "LoadLibraryW is hooked")) {
        std::string advice = "A hook in Skate's loader could not be validated. Keep security software enabled, "
                             "close overlays, then press RETRY. If it still fails, report the error and log to "
                             "the ReSkate developers.";
        // The launcher named what it found in Skate; that is the useful half.
        if (const auto found = raw.find("Loaded into Skate"); found != std::string_view::npos)
            advice += " " + std::string(raw.substr(found));
        return {"Something is hooking Skate as it starts", std::move(advice)};
    }
    if (mentions(raw, "Skate closed while it was starting"))
        return {"Skate closed while it was starting", std::string(attach_advice)};
    if (mentions(raw, "timed out after"))
        return {"Skate took too long to start",
                "Wait for any disk activity or antivirus scan to finish, then press RETRY. If it keeps happening, "
                "report the error and log to the ReSkate developers. Keep antivirus protection enabled."};
    if (mentions(raw, "validated Skate.exe") || mentions(raw, "loaded-image") || mentions(raw, "DOS header") ||
        mentions(raw, "module list") || mentions(raw, "VirtualAllocEx") || mentions(raw, "NtQueryInformationProcess") ||
        mentions(raw, "DingoSDKDebugInitialize") || mentions(raw, "Remote "))
        return {"ReSkate could not attach to Skate", std::string(attach_advice)};
    // Windows reports a refused write in the user's own language, so these
    // key on the operation names and the path, which are never translated.
    const bool write_failure = mentions(raw, "create_directories") || mentions(raw, "create_directory") ||
                               mentions(raw, "Cannot write") || mentions(raw, "Cannot create") ||
                               mentions(raw, "Cannot publish") || mentions(raw, "Cannot append") ||
                               mentions(raw, "Cannot replace");
    if (write_failure)
        return {"ReSkate cannot write to its own folder",
                "Move the whole ReSkate folder somewhere else, such as C:\\Games\\ReSkate, and start it from "
                "there. Program Files and OneDrive do not let it write."};
    return {std::string(raw),
            "Press RETRY. If it keeps happening, copy the details below and share them with logs\\ReSkate.log "
            "on the ReSkate Discord."};
}

} // namespace dingosdk::launcher_problem
