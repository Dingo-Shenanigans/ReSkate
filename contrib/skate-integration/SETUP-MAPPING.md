# Native setup/category mapping evidence

Supported executable SHA256: fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.

The source parameter type names field 0x4f49c7d0 `ToggleValues`. Its final HostSetThrowdownParameters is captured as eight native Boolean bytes, with strict count/type/value checks.

ClientSkateThrowdown_Schematics initializes native ParameterObject constants. Fields are Index (3644309812), ParameterName (1685475808), and CategoryFilter (279084462). The actual initialized constants decode to:

| Toggle index | Native parameter label | Native category bit filter |
| --- | --- | --- |
| 0 | Score Flips | 2 (category 1) |
| 1 | Score Grabs | 4 (category 2) |
| 2 | Score Grinds | 24 (categories 3 and 4) |
| 3 | Score Ground Tricks | 96 (categories 5 and 6) |
| 4 | Score Slams | 1920 (categories 7–10) |

These constants were decoded from the typed EBX DataDefinedTypeInstance FieldValues, not inferred by comparing two all-enabled arrays. A missing Index initializer is its native default of zero. ParameterName boxed strings resolve to the labels above. The user's live values had slams and cannonball disabled, agreeing with raw indices 4 and 7.

The final three switches are passed through unchanged. The menu labels are No leaving board, No session marker and Force cannonball; the native parameter strings are AllowOffBoard, AllowSessionMarkers and ForceCannonball. The detector does not reinterpret those permissions or invert their bytes.

Trick metadata comes from native DingoScorable.BaseTrickName and TrickCategory fields. The generated source table contains stable localization identifiers and numeric category bitsets, not localized English display-name heuristics or an allowlist of known tricks. Unknown identities/categories cannot match successfully. ScoreFlips follows the native category grouping, which also includes pop identities such as Ollie/Nollie and Boned Ollie/Nollie. Those foundational pops do not themselves trigger Boned Ollie suppression; that pairing rule distinguishes them from actual flip tricks.

Native extracted EBX/RES assets are investigation material only and are not distributed in either binary package or source.patch.
