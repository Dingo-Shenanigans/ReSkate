# Optional Dual Stick Controls menu integration

Based on ReSkate main 6df4f9a7bd4045aa74c853f088ff445011933175.
Requires the separately installed Dual Stick Controls v5.133 or later companion and its session_controls.ini beside Skate.exe. No translator or DLL is bundled here; no configuration is created when the companion INI is missing.

Insert adds Dual Stick immediately below Trainer. The native pause-menu Mod Options includes a Dual Stick tab. Both edit the same INI read by the companion every two seconds.

Session-style selects Front/Back, flick pop, no stick steering and blocked single-stick flips. Skater XL-style selects the XL layout, release pop, physical left-stick steering and allowed single-stick flips. Both reset direction overrides and use automatic switch tracking. They translate Skate inputs; they do not implement the physics of Session or XL.

Custom settings expose three stick layouts (Front/Back, Left/Right Foot, Skater XL), pop style, three steering choices, single-stick flips, switch axis overrides, revert input/window, pre-wind handling/strength, body flips, air controls, powerslides, grind bump-outs, and Disable hold-fakie. Stance and stock ramp controls are separate. The companion End menu retains its diagrams, hotkey and window options.

Five custom slots are shared with the companion. Occupied slots require confirmation before replacement. Loading restores supported gameplay and layout keys, including XL directions and hold-fakie compatibility. Controller connection, base stance, menu/launch preferences, recording and unknown tuning remain global. Saved slots are not automatically rewritten by choosing a style. The INI helper preserves unknown active keys and unrelated sections, reports write failures and refuses missing-file writes.

Dual Stick on next launch changes the saved startup choice only. Use the companion menu to toggle the running translator. Disable hold-fakie is implemented by the companion, with saved Skate settings preserved.

The overlay uses ReSkate's raw multi-device reader for ImGui navigation only while visible and foreground. Ownership loss releases all navigation axes/buttons. Sidebar and option tiles show focus; A/Cross enters the right panel; B/Circle backs out through editing, popups and panels to the sidebar. Insert closes the overlay. Existing pages, including Music, remain present.

ReSkateOverlayOwnsInputV1 is a versioned C export (unsigned long WINAPI(void)) returning whether UI/freecam/prompts own input. It allows the companion to suspend generated gestures while ReSkate menus are active. No new graphics or game-input hooks are installed by this patch.

Configure DINGOSDK_BUILD_DUAL_STICK_TESTS=ON and run CTest -R "dual_stick_settings|gamepad_navigation". The temporary-INI tests check exact preset keys, layout changes, direction overrides, shared slots, global preferences, missing/read-only files and unknown-key preservation. Navigation tests check analog/button feed and release on ownership loss. In-game validation against the current upstream UI remains necessary before merging.
