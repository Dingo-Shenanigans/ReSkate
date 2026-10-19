> Current source: official ReSkate v2.1.0 (ba1245a5), protocol 52, composed from the clean user-tested protocol-50 source. Commit/release paused; new gameplay validation pending. Older historical notes below do not override this baseline.

# Native First Person source map

- integration.cpp: native settings observation, final camera composition, bail collision and input policies.
- controls_context.cpp: native SourceReader/game UI gate plus upstream movement/bail/get-up evidence.
- native_spectator.cpp / spectator_state.h: native party target/session selection and head view.
- right_stick_runtime.h / native_stick_filter.h: upstream stick policies, physical-edge skating latch, native XInput/Sony filtering.
- glance_runtime.h / native_action_bindings.h: native bindings, upstream glance state and marker memory.
- Upstream/src: attributed settings/math and accepted query/input policies. import-manifest.json records original hashes; native adaptations and exclusions are intentional.
- client_debug.cpp / client_first_person.cpp: existing native camera hand-back, cycle policy, head composition and concealment.
- Multiplayer/Hud/native_party_hooks.cpp: existing party/throwdown camera invokes the native spectator adapter.
- UI/Overlay/first_person_controls.cpp and first_person_stick_controls.cpp: native widgets and Main/Advanced controls.
- Engine/Core/Config/profile_config.h: shared original Trainer directory and JSON file handling; Trainer retains its own trainer.json format.
- Test: native adapter regressions and selected upstream policy tests. Collision test excludes upstream DLL BailView-host tests; emote test excludes the replaced stock-HID bridge contract. No excluded feature is added merely to compile a test.

Reference repository: Addons/FirstPerson/JishSource. Latest audited upstream is 1.34.0 c0d2c47; its MSC client code is not imported or compiled.
