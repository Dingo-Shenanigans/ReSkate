# ReSkate INS menu development handover

Prepared 2026-10-18. Read this before editing the native menu. The recipient has the same project directory.

## Current 2.1.0 upgrade (2026-10-18; supersedes the shipping baseline below)

Current local merge candidate: `Development\JishIntegration-2.1.0-Clean-20261018-004608`. Exact official v2.1.0 tag: **ba1245a5af51892e90f2be8637dd69a7e20f5b77**, protocol **52**. Merge source is the clean, user-tested `Development\JishIntegration-Protocol50-20261018-001617`, based on 86aa8d38; it is NOT the old worktree or the emote observer source. Report: `Integration\Release210CleanUpdate\report.json`. This is a source composition, not a Git worktree. Commit/release publication is explicitly paused by the user pending the upgrade and testing; README must be shown before any commit.

ReSkate 2.1.0 changed the native menu substantially. Keep its window/full-screen menu, controller handling, typed-value sliders, grouped binds and lifecycle. Skater now has FREECAM / FIRST PERSON / CHEATS tabs; First Person is at **INS > Skater > First Person**, not the older Camera tab. Settings > Controls has GENERAL / CAMERA / CHEATS AND BOOSTS / ONLINE binding groups. Added FP binds sit directly below First Person in CAMERA. Do not restore the obsolete old page hierarchy. Native full-screen/controller support is now part of the base; do not import the separate INS-controller addon.

Use current `slider_float` / `slider_int` helpers declared in skate_menu_internal.h instead of raw ImGui sliders: these implement 2.1.0 typed values and console/controller behavior. The FP Reset helper composes around the native float slider. Keep native bind record/clear rows for each window/full-screen style; preserve the modifier action's controller-only rule. Native menu action is preserved while earlier FP array indices are retained; saved binds are keyed by action name. Native physical read filtering composes upstream grind-exit filtering and FP right-stick capture on both XInput and completed HID reads.

Drone follow owns the camera while leaving skater controls active; exclude it from the FP adapter's camera ownership, following the upstream free-camera distinction. Preserve native asynchronous Trainer saving and sampled keyboard path. No emote code or observers are in this candidate.

Official launcher package is the published v2.1.0 ReSkateLauncher.exe, not a newly branded launcher. Official release ZIP SHA256: `40dbeb1144c0089de85256fcb89aec1bb98280ce8b04a34dab96183e0d0e9c5d`; launcher SHA256: `1181d0edc9cc60ac544a3447ba52ce549524bcf12240e9fb72f37b98e26123fb`. The launcher Settings option **Install ReSkate updates** must be OFF to keep the custom runtime. It compares hashes and can replace the DLL even if the displayed version matches. Use the existing setting; do not invent a second updater or modify the official EXE. Package contents requested: ReSkate.dll, ReSkateLauncher.exe, scripts/ and its contents.

The following 2.0.7 material remains a useful source/format reference; paths and layout rules explicitly changed above take precedence. The original fallback remains preserved.

## Earlier 2.0.7 project and version authority

Project root: `C:\Users\Jon\Desktop\ReSkate\!JishFPMFork\ControllableMenuFORK`.

Current release-compatible composition: `Development\JishIntegration-Protocol50-20261018-001617` under that root. It is a composed source directory, **not a Git worktree**. Composition report: `Integration\Protocol50Update\report.json`. Its upstream base is **86aa8d38fb5e76f6fc7be632a4bacec4f54b82c3**, the last mainline commit before protocol 51, verified by fetching upstream/main on this date. It is newer than the original v2.0.7 tag but retains protocol 50. The next protocol-changing commit is `149da6f72e356f2d237f92244edebeb7831dfa7c`.

Original native integration worktree: `Development\JishIntegration`, HEAD `f68122d`, parent release tag v2.0.7 at `0f8af627105c1079ef25f36ad83b159ae9d1df1f`, with substantial uncommitted native work. Double-tap emote removal was applied there and carried into the new composition. Preserve its other uncommitted changes. It is not yet rebased to 86aa8d38.

Known working user fallback: `TestBuilds\FirstPerson-Native-v2.0.7-EmoteChord-20261015-222530`, protocol 50. Keep it available.

Do not mistake `Development\JishIntegration-LatestMain-20261016-025057` for the shipping baseline: it uses upstream eeeb751 and protocol 52. Its observers and emote research are experimental. Version text "2.0.7" alone does not establish multiplayer compatibility. Never force a newer protocol number down to 50; select genuinely compatible source.

The isolated installation is `game`. Do not overwrite it automatically or mix two ReSkate runtimes. A native feature replaces `ReSkate.dll`; it is not a companion DLL injected alongside ReSkate. A runtime-only change normally needs a DLL build/package, not a new launcher EXE. Build the launcher only for actual launcher changes or a required interface change.

## Non-negotiable project precedents

1. Use ReSkate's existing implementation, values, menu helpers, lifecycle, action binds and persistence first.
2. If absent, adapt Jish's implementation.
3. Write custom behavior only when neither provides the needed mechanism; explain the gap.

This must literally extend the existing ReSkate INS menu. Do not emulate it in a second ImGui window, a companion renderer or a separate swapchain. Avoid duplicated settings with independent owners. Moving an existing control must not change its action code. Keep controller-menu navigation work separate from the First Person addon until explicitly combined. Existing user authorization is local development only; discuss GitHub changes after testing.

All new First Person binds belong in **Settings > Controls > Action Binds**, immediately below native First Person, and default unbound. No bind recorder in Camera. Bindings remain global; camera profiles do not import or own them. Cleared bindings must remain cleared. No legacy import/conversion machinery.

## Source map (relative to the current source directory)

| Source | Responsibility |
| --- | --- |
| `Extension/UI/Overlay/skate_menu.h` | Persistent menu/UI state, selections, edit buffers and fonts |
| `Extension/UI/Overlay/skate_menu.cpp` | Menu shell, page dispatch, fonts and common layout helpers |
| `Extension/UI/Overlay/skate_menu_internal.h` | Internal helper and page declarations; authoritative helper signatures |
| `Extension/UI/Overlay/skate_menu_skater.cpp` | Skater tabs and Camera/First Person placement |
| `Extension/UI/Overlay/skate_menu_settings.cpp` | Settings and native binding UI |
| `Extension/UI/Overlay/first_person_controls.cpp` | FP profile/main/view/glance controls and Reset slider helper |
| `Extension/UI/Overlay/first_person_stick_controls.cpp` | On/off-board camera controls |
| `Extension/UI/Overlay/skate_style.h` | Overlay palette aliases and shared visual details |
| `Extension/UI/skate_theme.h` | Shared native palette, rough tiles and decoration used by menu/launcher |
| `Extension/Trainer/trainer_page.cpp` | Tuning layout, profiles, sliders and trailing Reset patterns to reuse |
| `Extension/Trainer/trainer.cpp` | Trainer state and profile operations; inspect before inventing storage |
| `Engine/Core/Config/profile_config.h` | Shared profile storage machinery |
| `Extension/UI/Overlay/overlay.h` | Model, callbacks, requests and UI-facing contracts |
| `Extension/UI/Overlay/overlay_render.cpp` | Existing renderer lifecycle and native menu draw call |
| `Extension/UI/Overlay/overlay_input.cpp` | Native menu/controller sampling |
| `Extension/UI/Overlay/input_capture.cpp` | Game input routing/filtering; not a place for decorative UI code |
| `Extension/Skater/FirstPerson/integration.cpp` | Backend adapter, native camera queue, settings readiness and stick capture |
| `Runtime/client_tick.cpp` | Existing engine-thread model/request processing |
| `cmake/UI.cmake`, `cmake/FirstPerson.cmake` | Source/target registration; check actual target ownership |

For additional history see root `NATIVE_FORK_MODDING_HANDOVER.md`, source `contrib/first-person/{DEVELOPMENT.md,SOURCE_MAP.md,DLL_TO_NATIVE_CHANGES.md}`, and `Integration/Protocol50Update/report.json`. Historical documents may describe removed features: current source and this handover's version/status statements take precedence.

## Native look and responsive layout

The menu already applies its style and fonts. Reuse these; do not add another theme or hardcode a competing palette. `skate_theme.h` defines blue `(1,131,255)`, white `(245,245,245)`, muted grey `(150,152,158)`, dark tiles and warning colours. `rough_rect` draws the existing irregular tile edges. Use `menu.heading`/`menu.bold` through existing helpers instead of loading another font.

`px(value)` applies the current menu scale, and is valid only while drawing. Use it for dimensions. Existing `field` stacks labels on narrow layouts (available width below px(440)); otherwise it uses a px(180) label column. Prefer available-content widths over window/screen widths. Cards resize vertically to their contents and do not scroll independently; the containing page owns scrolling. `page_body_height(menu)` leaves room for feedback.

Do not replace `toggle_row` with a checkbox for a normal native option: it supplies the full-width tile, ON/OFF block, disabled state, tooltip and navigation focus outline. Do not hand-draw an interactive element without an actual ImGui widget backing it.

## Existing layout kit

All helpers below are in namespace `dingosdk::overlay::menu`; implementation in `skate_menu.cpp`, declaration in `skate_menu_internal.h`.

| Helper | Use |
| --- | --- |
| `section(menu, text)` | Heading font plus rule and blue accent; visual header, not a collapsible group |
| `begin_card(menu, id, title, subtitle)` / `end_card()` | Content-sized dark group with native blue edge; always pair |
| `field(menu, label, tooltip)` | Label and next-widget width/column; call immediately before widget |
| `info(menu, label, value)` | Muted label and read-only value |
| `note(text)` / `warn(text)` | Wrapped help or warning |
| `toggle_row(menu,label,hint,bool&,available,unavailable)` | Native toggle tile; returns activation/change |
| `choice(menu,id,int&,options,enabled,width)` | Short option strip; use for a few choices |
| `category_tabs(menu,int&,tabs,id)` | Existing subpage tabs; selection must persist |
| `tag(menu,text,colour)` | Small status badge |
| `primary_button(menu,label,enabled)` | Blue full-width main action |
| `feedback(menu,text)` | Timed status/result message |
| `debug_request(menu,callbacks,request)` | Queue a native debug action and report result |
| `send_console(menu,callbacks,command)` | Existing console dispatcher with feedback |

## Adding a section or collapsible group

First find the existing page function and insert there. Adding another sidebar page is a larger change: inspect the shell's page dispatch and selected-state storage rather than assuming one enum edit is sufficient.

```cpp
begin_card(menu, "my-feature", "MY FEATURE");
note("Short explanation of the feature.");
if (ImGui::TreeNodeEx("Advanced##my-feature", ImGuiTreeNodeFlags_SpanAvailWidth)) {
    // Draw controls here.
    ImGui::TreePop();
}
end_card();
```

`TreeNodeEx` creates an actual focusable/disclosable ImGui item. Use it intentionally; a purely decorative category should use `section`, not a selectable container. Do not enable navigation on a whole child/card merely to navigate the controls inside it. Earlier controller-menu experiments highlighted entire child sections; keep that addon separate and inspect its child-navigation behavior before copying it.

Balance every TreePop, EndChild, EndCombo, BeginDisabled/EndDisabled, PushID/PopID and style/font push/pop. A false TreeNode does not need TreePop. `BeginChild` still requires EndChild. Empty categories should be removed, not left as dead rows.

## Dropdowns, stable identity and transient state

```cpp
field(menu, "Mode");
if (ImGui::BeginCombo("##my-feature-mode", current_label)) {
    for (int i = 0; i < option_count; ++i) {
        const bool selected = current_index == i;
        if (ImGui::Selectable(labels[i], selected)) {
            // Submit the existing backend command; do not mutate game memory here.
        }
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}
```

Use `##id` for an invisible widget label when the visible label comes from `field`. Use stable IDs/PushID for repeated widgets; labels such as Reset repeated without a unique scope collide. State must survive frames: keep selected tabs, edit text and selected profile in `SkateMenu` or the existing page state, not stack locals reset each draw. Read current values from the model/settings snapshot. If a list refreshes, preserve selection by profile name/identity rather than blindly keeping a stale index.

## Toggles, sliders and Reset

Native toggle example (adapt request/action to an existing supported action):

```cpp
bool enabled = model_value;
if (toggle_row(menu, "Feature", "What this changes.", enabled, available)) {
    debug_request(menu, callbacks, {existing_action, enabled});
}
```

For First Person addon-owned settings, follow `option`/`submit` in `first_person_controls.cpp`: submit `fpmod::Command`, then read feedback. Shared native camera settings use existing `DebugAction` and camera host values. Do not introduce a second FOV, Spring or smoothing variable that fights the native one.

`first_person_slider(id,value,low,high,format,reset)` is a local helper with a trailing Reset button. It scopes IDs, reserves button width using actual text width and style padding, uses AlwaysClamp and reports either slider editing or Reset as changed. The caller submits that change once. Reset must use the same backend command path as manual adjustment. Confirm each reset value from authoritative defaults/recommended profile as appropriate. Every First Person slider should keep a Reset button, following Trainer's existing row pattern. See `trainer_page.cpp` for its numeric row and dial reset examples.

Avoid over-wide SameLine rows: the earlier profile Save button disappeared when the input consumed all width. Reserve the actual button width before drawing the input, and use narrow-layout stacking where necessary. Keep Save visible; disable it when the name is empty rather than hiding it.

## Profiles and persistence

Use ReSkate's Trainer/profile_config implementation. FP profiles are separate files in the same native directory: `%LOCALAPPDATA%\ReSkate\trainer\first-person-profiles.json`; live addon preferences use `first-person-settings.json`. Existing native camera values and bindings keep their native owners.

The FP menu has Profile dropdown, Load, Refresh, Reset to defaults, Name, always-visible Save and Delete profile. Recommended is a bundled choice; personal entries come from the backend. Reset is not Delete. Preserve selection on save/refresh, report backend errors and avoid saving from the render loop every frame. Existing settings submission handles persistence; do not add another database, migration layer, legacy importer, or validation framework that Trainer does not need.

Read `first_person_controls.cpp`, adapted `Upstream/src/{config.cpp,settings.cpp}`, `trainer_page.cpp`, `trainer.cpp` and `profile_config.h` together. Bindings remain global and are deliberately omitted from camera profiles. Current serialization drops removed/unknown options through normal native handling; no bespoke conversion of old emote settings is needed.

## Threading, loading and ownership

Draw functions consume snapshots and queue existing callbacks/commands. Do not do file I/O, engine writes, camera manipulation, asset lookup, hook installation or controller polling from a widget callback. Use the established engine-thread/request flow. Inspect locks before reusing APIs: render-status queries that take renderer locks can deadlock if called from input polling on the render thread.

The first-person settings adapter starts through existing overlay readiness (`start_when_ready` called in `overlay_render.cpp`); the UI shows waiting/failure text until ready. Let ReSkate own game-ready detection, renderer initialization and INS visibility. Prior companion work created a probe swapchain too early; do not repeat that architecture. These observations do not mean every menu problem is a multi-mod conflict—verify logs and lifecycle first.

No new raw XInput/DirectInput layer is needed to draw controls inside INS. Use existing ReSkate input routing. The separate INS-controller addon owns additional navigation behavior; do not silently introduce it into the First Person integration. For controller-sensitive widgets test focused versus active sliders, tab changes, disabled widgets, scrolling, and opening/closing INS.

## Current First Person structure and feature removal

Location: Skater > Camera > First Person. Main area keeps profiles and main off-board/on-board/glance toggles. Advanced groups contain View and stabilization, directional glance and stick-camera tuning, and native camera/Spring controls as the current page arranges them. Spectator, third-person bail/get-up and bail collision belong under View and stabilization. No binding UI in Camera.

Double Tap D-Pad to Emote has been removed from the shipping candidate: widget, Other category, setting/command/serialization, native routing/filtering, double-tap policy and dedicated tests. Original research captures and observer source in the separate newer-main directory remain historical evidence, not production features. Native emotes now follow the base game's controls; an overlapping First Person chord can also invoke a native emote, because no emote suppression is promised. Do not advertise the removed behavior in release notes or re-enable it from an old profile.

## Build, verification and handoff checklist

For the current candidate, configure/build in its own `build\validation`; do not reuse a CMake cache from another source directory. Runtime target is `dingosdk_runtime`, output `build\validation\Release\ReSkate.dll`. Tests are separate targets. Requesting the runtime may visit upstream dependencies; that does not require packaging a launcher EXE. Preserve upstream build rules rather than inventing a second build system.

Before delivering: verify the recorded base SHA, protocol header and upstream Net/Steam sources; inspect merge conflicts and input-filter ordering; compile edited code and tests; run focused tests; inspect source for removed feature remnants and conflict markers. Unit tests do not establish in-game menu layout or multiplayer join success. Package only the runtime and required matching scripts for a DLL update, with base/protocol/hashes/test results and explicit live-test status. Do not include stale README claims from earlier packages.

User acceptance should cover profiles (Save/Load/Refresh/Delete/Reset), all slider Reset buttons, narrow window and scaled layout, normal emotes, directional glances, on/off-board stick look, held-input modifier behavior and spectator camera. Verify live server compatibility on the actual user install. Keep the fallback package. Discuss commit contents after user validation; do not commit/push automatically.

The source workspace also contains reproducible composition/removal/build scripts under `C:\Users\Jon\Documents\ChatGPT\Skate Modding Misc`: `remove-native-double-tap.py`, `prepare-protocol50-native.py`, `build-protocol50-native.py`. These are one-shot scripts, not an automatic maintenance pipeline: inspect before running again. The removal script asserts the original worktree parent; the merge resolver expects conflict markers and is not idempotent. Future integration into Git must reconcile the composed directory with the intended branch without discarding uncommitted work.
