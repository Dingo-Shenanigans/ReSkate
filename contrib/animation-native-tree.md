# Native animation inventory and decoder

The native tree inventories the installed game's animation data across every TOC in
`Data/Win32`. It exposes typed documents, dependencies, source resources and supported
local-pose previews in F8. Name categories are navigation hints. References describe
authoring dependencies; they do not establish the game's runtime execution order.

## Reproduce

Build the `dingosdk_anim_graph_probe`, `dingosdk_runtime` and
`dingosdk_animation_studio_tests` Release targets. Use a cache and output directory
outside the game. Python requires the existing animator-studio dependencies, including
NumPy and the FBX reader for differential reference checks.

```powershell
build/vs2022-x64/Release/dingosdk_anim_graph_probe.exe --inventory GAME CACHE
python contrib/animator-studio/decode_native_tree.py --cache CACHE --game GAME --cli PATH/reskate_cli.exe --library CORRECTED_BASE_LIBRARY --dof-names DOF_NAMES.json --output STAGED_LIBRARY --workers 4 --direct
```

The base library must use corrected `native-piec-v1-extent` positions. Generate its DOF
dictionary through `native_piecewise.native_dof_names` with the existing CLI and tools.
The bulk decoder checks the supplied dictionary against the extracted native rig and
validates every base clip against current source poses before preserving its bindings.
Changed controller, resource, mapping, rig or decoder fingerprints invalidate cached
results. Successful cached files are hash checked. `--retry-failed` retries rejected
assets; `--catalog-only` refreshes metadata without scheduling new pose conversions.

Generated files stay local and must not be committed: `.clip` files and `catalog.json`
serve previews; `native-catalog.json` and `native-tree.json` serve the inventory;
`native-links.jsonl` holds static references; `native-documents` contains typed EBX;
`native-resources` contains encoded source bytes and descriptors. `results` is a build
cache and need not be installed. `decode-summary.json` reports coverage and failures.
Back up the installed library and runtime before copying the staged library into the
isolated test game's `ReSkate.Tricks/library`; install `ReSkate.dll` while the game is
closed. Preserve personal profiles, presets and recordings outside that library.

## Decoder evidence

### Existing installations: outdated position decoding

Building or copying the PR's DLL does not regenerate `ReSkate.Tricks/library`.
Generated game-specific clips and catalogs are local data and are not included in
the Git branch. The error "This native library has outdated position decoding"
means the selected native entry lacks the supported `position_codec` value
`native-piec-v1-extent` (or has another value). It does not diagnose a game crash.

Install a matching corrected library, including both `catalog.json` and its actual
`.clip` payloads. For a base-library replacement, close the game and back up its
existing `ReSkate.Tricks/library` folder first. Leave personal `.json`/`.clip`
files directly inside `ReSkate.Tricks`, `presets` and `recorded` intact. A 101-clip
base package does not contain the expanded 4,004-preview inventory. Keep any
expanded library backup if testing a base package.

Developers can repair a compatible existing source library into a new staging
directory using the original game's compressed resources:

```powershell
build/vs2022-x64/Release/dingosdk_anim_graph_probe.exe --inventory GAME CACHE
python contrib/animator-studio/repair_library_positions.py --game GAME --probe build/vs2022-x64/Release/dingosdk_anim_graph_probe.exe --library OLD_LIBRARY --output NEW_REPAIRED_LIBRARY --asset-index CACHE/native-asset-types.json
build/vs2022-x64/Release/dingosdk_animation_studio_tests.exe --native-catalog NEW_REPAIRED_LIBRARY
```

Check `repair-report.json` for failures and completeness before replacing anything;
unsupported sources require a fresh supported extraction. Restart after installing,
or use Reload native library if the game was already open. Do not merely add the
codec marker to old metadata: that bypasses the version check while retaining the
incorrect translations that caused board/skater clipping.

Native PIEC vectors use minimum plus normalized value times extent. Short two-key
vectors contain two actual endpoints. Quaternion codec 1 stores three big-endian
16-bit components with omitted-component bits in the first two words. Inline constants
and codec 2 use a little-endian 64-bit 21/21/20-bit layout and a two-bit omitted index.
Unanimated transforms come from the native default pose, with all 395 joints mapped
through real DOF bindings. Sampling uses shortest-arc quaternion interpolation.

The decoded corpus was compared with 620 independently exported FBX references,
including the original 101 clips: worst rotation difference was 0.026984 degrees and
worst component position difference was 0.000000373 metres after correcting the known
exporter vector-extent defect. All 63 codec-2 references also passed, with maximum
rotation difference 0.023714 degrees. Synthetic tests cover omitted components,
64-bit constants, wide timestamps, interpolation, vector endpoints and malformed input.

```powershell
python -m unittest discover -s contrib/animator-studio -p 'test_native*.py'
ctest --test-dir build/vs2022-x64 -C Release --output-on-failure
build/vs2022-x64/Release/dingosdk_animation_studio_tests.exe --native-index STAGED_LIBRARY/native-catalog.json
build/vs2022-x64/Release/dingosdk_animation_studio_tests.exe --native-catalog STAGED_LIBRARY
build/vs2022-x64/Release/dingosdk_animation_studio_tests.exe --native-documents STAGED_LIBRARY
python contrib/animator-studio/validate_native_decode.py --cache CACHE --game GAME --cli PATH/reskate_cli.exe --dof-names DOF_NAMES.json --report REPORT.json --codec 2
```

## Coverage and limits

For the inspected installation: 23 TOCs, 17,258 named Dingo assets plus 2,081 channel
binding documents, 6,172 clip controllers, 260,846 static references and 4,004 supported
previews. These include variants, locomotion and small movements, rather than 4,004
distinct tricks. The tree's search accepts names and types; editable-only filtering
limits navigation to available previews. Inspect data opens paginated fields and can
follow document and resource references without running native code.

The TRICKS tab has a live search field above the list (Ctrl+F to focus, Clear to
reset). Search spans all native folders, accepts words in any order, ignores case
and punctuation, and matches readable names, common aliases such as tre flip,
original source names and types. The editable-only filter still applies. Folder
navigation resumes when search is cleared. `native_asset_names.h` groups clips by
movement family and explicit phase, and separates graph/rig data. Known name tokens
are expanded; unknown codes and variant numbers remain visible. Duplicate labels
gain context and stable variant numbers, without merging or changing asset IDs.
Readable names/folders are generated at load time, so existing local libraries need
no destructive rename or re-extraction. Validate an actual inventory with:

```powershell
build/vs2022-x64/Release/dingosdk_animation_studio_tests.exe --native-labels STAGED_LIBRARY/native-catalog.json READABLE_REPORT.json
```

The remaining 2,168 clip controllers have explicit unsupported reasons: 1,794 VBR
sources, 333 different or missing rigs (mostly board-only), 39 trimmed clips and two
source-timing cases. Their original metadata/resources remain inspectable. A board-only
skeleton cannot safely be passed to the skater's 395-joint preview backend.

New previews have no invented takeoff/landing markers or live trick bindings. Existing
verified bindings remain intact. Editing a newly decoded grind or transition in F8 does
not automatically wire it into gameplay. Native graph evaluation, alternate rigs, VBR,
trim semantics and source replacement selection remain separate work. Build and offline
reader checks do not replace visual testing of the menu and game.
