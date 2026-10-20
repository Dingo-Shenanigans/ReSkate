# Optional Dual Stick Controls menu integration

Integrates the separately installed [Dual Stick Controls companion](https://github.com/GoalTrains/reskatedualstick/releases/tag/6.3) with ReSkate's current menu. Version 6.3 is recommended for all options; older companions ignore settings they do not support. No translator, DLL or release archive is bundled here, and no configuration is created when the companion INI is missing.

Insert adds Dual Stick after Sandbox in both windowed and fullscreen layouts. Native pause-menu Mod Options includes a Dual Stick tab. Both edit session_controls.ini beside Skate.exe, read by the companion every two seconds. The existing upstream menu navigation and controller reader are unchanged.

Session-style selects Front/Back, flick pop, no stick steering and blocked single-stick flips. XL-style selects the XL layout, release pop, physical left-stick steering and allowed single-stick flips. Both reset direction overrides, use automatic switch tracking and preserve optional catch and camera preferences. They translate Skate inputs; they do not implement Session or XL physics.

The pages expose layouts, stance, pop style, steering and its angle, single-stick flips, Manual Catch, D-pad camera sides, foot setup grinds, switch direction overrides, revert settings, pre-wind, hold-fakie compatibility, body flips, air controls, powerslides, grind exits and stock ramp controls. Manual Catch defaults off; D-pad camera sides and foot setup grinds use the companion 6.3 defaults. Some tricks settle into an angled pose rather than continuing to rotate, so catch timing still matters. Trick rotation speed is handled by Skate; the withdrawn companion stick-speed override has no control here. The companion End menu retains diagrams and additional tuning.

Five custom slots are shared with the companion. Occupied slots require confirmation before replacement. Loading merges supported gameplay/layout keys while preserving controller, base stance, menu/launch/recording preferences, unknown keys and unrelated INI sections. Older slots lacking newer options leave those active options unchanged. Numeric custom steering angles are retained exactly. Saved slots are not rewritten by choosing a style. Missing/read-only configuration files report failure rather than creating a replacement.

Dual Stick on next launch changes the saved startup choice only. Use the companion menu to toggle the running translator. Disable hold-fakie is implemented by the companion, with saved Skate settings preserved.

The Insert page uses upstream's shared cards and option rows for its windowed/fullscreen layouts. Native Mod Options uses the existing column widgets. This PR does not replace upstream controller navigation or change existing maps, addons, music or multiplayer pages.

ReSkateOverlayOwnsInputV1 is a versioned C export (unsigned long WINAPI(void)) returning whether UI/freecam/prompts own input. It allows the companion to suspend generated gestures while ReSkate menus are active and preserves the caller's last-error value. No new graphics or game-input hooks are installed by this patch.

Configure DINGOSDK_BUILD_DUAL_STICK_TESTS=ON and run ctest -C Release -R dual_stick_settings --output-on-failure. Temporary-INI tests cover presets/layouts, v6.3 features, shared and older slots, global preferences, case-insensitive keys, unrelated sections, missing files and write failures. Runtime build validation is documented in the PR. Hardware checks of both menu surfaces remain useful follow-up testing; automated tests do not establish in-game navigation or trick behaviour.
