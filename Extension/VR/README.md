# ReSkate VR (OpenXR)

> **Status: experimental.** Tested on a Meta Quest 3 through Virtual Desktop. The pose math has unit
> tests (`Test/vr_math_tests.cpp`, built with `DINGOSDK_BUILD_VR_TESTS`).

This puts ReSkate's first-person camera in a PC VR headset through OpenXR. It works with
Virtual Desktop (VDXR), Meta Horizon Link and SteamVR.

## How it works

skate. draws one view per frame, so VR uses **alternate eye rendering**:

1. **Camera.** On each frame, the first-person camera (`Extension/Skater/client_first_person.cpp`) is moved
   to the headset pose of the eye whose turn it is: `vr::apply_camera`. The native FOV is set to one
   symmetric frustum that covers both eyes (`vr::camera_fov`).
2. **Present.** At Present, the overlay's DX12 hook calls `vr::on_present`. That pairs the back buffer
   with the camera frame that drew it, copies it into that eye's OpenXR swapchain with a small shader,
   and calls `xrEndFrame`. Then `vr::after_present` calls `xrWaitFrame` and `xrBeginFrame`, and the
   headset paces the game.
3. **Display.** The compositor shows each eye's newest image, reprojected to the current head pose.

Each eye updates at half the game's frame rate, and the game runs at the headset's refresh rate.

When first person is off (loading, third person) or a menu is open (the ReSkate overlay, the console, chat,
or the game's own main and pause menus), the game image is shown on a flat screen in the headset instead.

## Stereo modes

SKATER > VR > Stereo (`vr.stereo`):
- **Alternate eyes** (0): the camera moves to each eye in turn (each eye ~30 Hz with a 60 Hz camera).
- **Side-by-side input** (1): a ReShade stereo shader (SuperDepth3D, Side by Side) draws both eyes into
  each frame; ReSkate splits it after the game's Present.
- **Depth stereo** (2, recommended): built in. The camera renders from between the eyes; both eyes are
  made from each frame and the game's depth buffer (copied when the game clears it), so both update with
  every camera frame. Depth strength, Edge widening and Gap fill tune it. Narrow gaps behind near objects
  are guessed (the single view never saw them). Lens effects (vignette, chromatic aberration) are turned
  off while VR runs.

## Using it

1. Put `openxr_loader.dll` beside `Skate.exe`: the Windows x64 loader from the Khronos OpenXR SDK
   release 1.1.63 (`External/manifest.json`).
2. Start your headset's PC VR app: Virtual Desktop, Meta Horizon Link or SteamVR.
3. Launch through ReSkate. Then press **Insert** and go to SKATER > **VR**. Turn on **VR**, then turn on
   **First person** in the CAMERA tab.
4. Recommended game settings:
   - V-Sync off, frame generation off, motion blur off.
   - Windowed in the launcher (Settings > DISPLAY > Windowed, e.g. 2880 × 2880). Only a windowed start
     adds the square VR sizes (1440×1440 to 3840×3840) to the game's resolution list. Pick one in the game's display options: the image is copied to each eye, so a
     square image near the headset's per-eye size (2880×2880 for a Quest 3) is sharpest. Lower it if the
     frame rate drops.
   - HDR off.
5. **F8**, or both controller stick clicks held for a second, recenters the view.

If depth looks wrong or doubled, change **Eye pairing** (`vr.frame_lag`, under Advanced).

### Presets

SKATER > VR > Presets: **First person** (the defaults), **Third person** (behind the skater, like the
game's camera) and **Tiny world** (third person from far and high with a giant's eye separation: the
skater looks like a small figure in a model world). Your own settings can be saved as presets too.

### Camera

- **Through the skater's eyes** (first person): the view follows the skater's head through continuous
  One Euro filters (`math::eyes_heading`, `math::eyes_position`): slow wobble (push twist, stride bob)
  is smoothed, fast moves (spins, coffins, carves, ollies) pass at once, and nothing switches on or off,
  so the camera does not jump. **Crouch** sets how much crouching lowers the view.
- **Third person** follows the direction of travel, like the game's camera (**Follow** sets how lazily).
- **Take over the camera** (default): while the headset runs, first person (which carries all VR views)
  turns on by itself, also after a map change.
- **Walk where you look** (on foot): the view turns only with your head and the right stick (**Stick
  turning**: snap or smooth), so the stick walks where you look.
- **Flip & bail view**: when the skater goes head over heels (bails, back and front flips), the view moves
  just behind them with the whole body shown; the right stick orbits. **Grab view** does the same while
  grabbing in the air; **Show body in grabs** keeps first person and shows the whole skater instead.
- Third person and the outside views stay clear of the ground the skater has ridden and of the slope
  being ridden: on a ramp the camera behind pulls in towards the skater, then rises, instead of entering it.
- **You see (board) / You see (on foot)**: how much of your skater first person shows (neck down,
  shoulders down, arms and legs, or legs only). By default legs only on the board and shoulders down on
  foot, where the hands carry the board.
- **Feet only** (a fifth choice in both rows, with NxRoot's FeetOnly costume mod
  installed): while VR runs, your skater wears the mod's costume, so only the shoes show. Third person,
  the flip & bail view and grabs show the whole skater, and your saved outfit comes back when VR stops; it
  is never changed. **Others see your outfit** (on by default) sends other players your saved outfit
  instead of the costume. A hidden copy of your skater, 50 m below, wears your clothes so switching back
  to the whole skater is quick (about 30 ms instead of 0.5–0.8 s of reloading).
- **Camera offset on the board / on foot**: forward, sideways and height of the camera from the skater's
  eyes, set separately for riding (airs and grinds count) and on foot; the view moves between them in
  about half a second.
- **Chat** (its own card): **Show chat in VR** puts ReSkate's chat on a panel at the lower left of the
  view (**Chat size**, **left / right**, **up / down**) instead of in the game image; **Show chat on left
  hand controller** moves it above the left hand, facing you (at the view's spot while the controller is
  not tracked). Type with the PC keyboard as usual; with a panel the headset stays in VR while you type.
- **Comfort vignette** (off by default): darkens the edges while the view turns without your head.

### Controllers

With **VR controllers** on, the headset's controllers act as the gamepad (a gamepad keeps working):

| Controller | Game |
|---|---|
| Right A / B | A (push) / X (other push) |
| Left X / Y | Y (board on / off) / B (stop) |
| Grips | LT / RT (grabs) |
| Triggers | LB / RB |
| Right thumb on the thumbrest + left stick | D-pad |
| Menu button | Start (hold: Back) |
| Both stick clicks held 1 s | Recenter |

Look turns also read pads the game does not report through XInput pad 0 (Steam Input's virtual pad,
a DualShock 4 or DualSense through DirectInput).

### Console

| Command | Purpose |
|---|---|
| `vr` | Status: headset, rates, image pairing, threads |
| `vr.recenter` | Recenter the view |
| `vr.enabled 0/1` | Turn VR on or off |
| `vr.frame_lag 0..4` | Presents between a camera frame and its image |
| `vr.level`, `vr.positional`, `vr.crouch`, `vr.tilt_filter` | Comfort |
| `vr.turn_smoothing` | Third person: how lazily the camera follows the direction of travel |
| `vr.world_scale`, `vr.fov_scale`, `vr.fov_vertical` | Scale and projection |
| `vr.seat_forward`, `vr.seat_up`, `vr.seat_side` | Camera position relative to the skater's eyes on the board, in metres |
| `vr.seat_forward_foot`, `vr.seat_up_foot`, `vr.seat_side_foot` | The same on foot |
| `vr.stereo`, `vr.depth_strength`, `vr.edge_widening`, `vr.gap_fill` | Stereo mode and depth stereo tuning |
| `vr.lens_effects_off` | Vignette and chromatic aberration off while VR runs |
| `vr.theater_distance`, `vr.theater_width` | Flat screen |
| `vr.auto_first_person` | Take over the camera |
| `vr.walk_where_you_look`, `vr.turn_style`, `vr.look_turn`, `vr.turn_speed` | On-foot view and right-stick turns |
| `vr.hide_body`, `vr.hide_body_foot` | What first person hides on the board and on foot (4: feet only, FeetOnly mod) |
| `vr.others_see_outfit` | Feet only: other players see your saved outfit |
| `vr.chat_in_vr`, `vr.chat_on_hand` | Chat on its own panel in VR; on the left controller |
| `vr.chat_view_size`, `vr.chat_view_x`, `vr.chat_view_y` | The chat panel in the view: width and offset, in metres |
| `vr.grab_view`, `vr.grab_body` | Grabs: outside view, or the whole body in first person |
| `vr.bail_camera` | Flip & bail view |
| `vr.vignette` | Comfort vignette (0 off, 1 strong) |
| `vr.controllers` | Headset controllers as the gamepad |
| `vr.recenter_key` | Virtual-key code (119 = F8, 0 = none) |

Settings are saved in the local profile under `ReSkate.VR.*`.

## Code

| File | What it holds |
|---|---|
| `vr.h` | Public interface: settings, status, camera and Present hooks |
| `vr_math.h` | Pose and projection math, header-only (tests in `Test/vr_math_tests.cpp`) |
| `vr_camera.cpp` | Settings, and the eye camera on the engine thread |
| `vr_xr.cpp` | OpenXR loader, session, events and frame loop on the Present thread |
| `vr_blit.cpp` | Back buffer → swapchain copy (D3D12, shader compiled at run time; comfort vignette) |
| `vr_depth.cpp`, `vr_stereo.cpp` | Scene depth copy and the depth stereo pass |
| `vr_input.cpp` | Headset controllers (OpenXR actions) as XInput pad 0; the left hand's pose for the chat panel |
| `vr_costume.cpp` | Feet only: the FeetOnly costume on the local skater and the hidden clothes keeper (runtime target) |
| `vr_gamepad.cpp` | Right sticks the game's hook does not see (XInput slots, DirectInput Sony pads) |
| `vr_console.cpp`, `vr_profile.cpp` | Console commands and saved settings (runtime target) |
| `Extension/UI/Overlay/skate_menu_vr.cpp` | The SKATER > VR menu tab |

Changes to existing files are kept small:
- `overlay.h` / `overlay.cpp`: a generic present observer (`set_present_observer`), the menu-open state
  (`vr::set_ui_open`), and, for launcher windowed starts only, square VR sizes added to the DXGI display
  mode list (the engine only accepts resolutions from that list).
- `display_startup.cpp`: allows windows larger than the desktop.
- `client_first_person.cpp` and `client_debug.cpp`: two camera writes and two FOV writes go through
  `vr::`; VR takes the head pose before the first-person spring; the game's menu state goes to
  `vr::set_game_menu`.
- `input_capture.cpp`: the XInput hook reports pad 0 connected while VR controllers are on, merges them
  in, and passes right-stick values to `vr::`.
- `client_first_person.cpp` also reads the skater's physics state (as No Bail does) and shows the whole
  skater during the flip & bail view.
- `client_tick.cpp`: runs `vr::tick_costume` on the client update.
- `native_cosmetics.{h,cpp}` and `session_send.cpp`: a generic hook for the outfit other players are sent
  (`set_outfit_for_others`), used by Feet only (`mesh_tests.cpp` has its stub).
- `overlay_render.cpp`, `chat_overlay.cpp`, `overlay.cpp`: while VR wants the chat panel, the chat's draw
  lists render into a texture of their own (a second draw on the overlay's command list) instead of the
  game image, and an open chat alone does not switch the headset to the flat screen.
- Console registration, the menu tab, and CMake (`cmake/VR.cmake`, `cmake/Runtime.cmake`).

## Known limitations

- The game's HUD and the menus are part of the game image, so they sit at screen depth (the chat has
  its own panel).
- Feet only needs the FeetOnly mod. The hidden clothes keeper uses the last remote-player slot (slots
  fill from the first, so sessions never reach it).
- The game moves its camera about 60 times a second. Depth stereo builds both eyes from each frame and
  the headset reprojects in between; with Alternate eyes each eye updates at about 30 Hz.
- Depth stereo sees one view: gaps behind near objects (between board and body) are filled in, and the
  depth arrives one frame after the image.
- Temporal AA blends the two eyes in Alternate eyes mode; depth stereo is not affected.
- Only square sizes from a windowed launcher start render larger than the monitor.
- Like the rest of ReSkate, it is tied to the supported skate. build.
