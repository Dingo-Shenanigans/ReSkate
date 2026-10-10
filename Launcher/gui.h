#pragma once

#include "launch.h"

#include <string>
#include <vector>

namespace dingosdk::launcher_gui {

// Runs the launcher until it is closed. The saved setting can retain it hidden
// while Skate runs and show it again after the game exits.
// `arguments` are the launcher's own command-line arguments, kept for a
// restart after a self-update.
int run(const launcher_app::Session& session, const std::vector<std::wstring>& arguments);

// The Settings page's "Install ReSkate updates" choice; scripted (--no-gui) launches honour it too.
bool updates_enabled(const launcher_app::Session& session);

// What pressing Play would start the game with: the Settings page's saved choices. A scripted
// launch that names none of its own (a bare --no-gui, as Discord runs for a join) uses these.
launcher::LaunchOptions saved_launch_options(const launcher_app::Session& session);

// The Settings page's "Send crash reports" choice, for this launcher and the game it starts.
// Call before open_session, which starts crash reporting with the log.
void apply_crash_report_setting() noexcept;

} // namespace dingosdk::launcher_gui
