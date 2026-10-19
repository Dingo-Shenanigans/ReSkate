# skate.8 log confirmation and skate.9 correction

Three archives contain 50 distinct comparison records after deduplication; 19 matched. These are recorded detections, not independent proof of the playerâ€™s intended input.

Boned Ollie/Ollie equivalence already works. No Boned Ollie change is made. Reported grind cases include differing grind identity/direction or an extra/missing component.

Joshâ€™s 20:48:39 area turn contains only Powerslide/Revert observations, then No recognized trick and a failed native submission before a core trick. Excluded movement must not consume a turn. skate.9 restores the exact test7 native category-adapter files and permission grouping; skate.8 area probing and area-specific exceptions are removed. This is source equality plus regression validation, not a claim of a completed live area match.

Trickless Air and its Pumping placeholder are excluded, alongside Powerslide. An air interval without a recognized component cannot create a matching requirement.

| # | Target capture | Defense capture | Result |
| --- | --- | --- | --- |
| 1 | spin:normal:0: / KICKFLIP / phase:grind / BLUNTSLIDE direction=ID_TRICK_FS / spin:normal:0: / VARIALKICKFLIP | spin:normal:0: / KICKFLIP / phase:grind / TAILSLIDE direction=ID_TRICK_BS / spin:normal:0: / VARIALHEELFLIP | Trick component differs: base: "ID_TRICK_BLUNTSLIDE" vs "ID_TRICK_TAILSLIDE", direction: "ID_TRICK_FS" vs "ID_TRICK_BS" |
| 2 | spin:normal:0: / KICKFLIP / phase:grind / BLUNTSLIDE direction=ID_TRICK_FS / spin:normal:0: / VARIALKICKFLIP | spin:normal:0: / OLLIE / phase:grind / BLUNTSLIDE direction=ID_TRICK_FS / spin:normal:180:FS / HEELFLIP | Trick component differs: base: "ID_TRICK_KICKFLIP" vs "ID_TRICK_OLLIE" |
| 3 | spin:normal:0: / FLIP held_rotation=360 / phase:grind / NOSEGRIND direction=ID_TRICK_BS | spin:normal:0: / FLIP held_rotation=360 | Different number of tricks or spin segments |
| 4 | spin:normal:0: / FLIP held_rotation=360 / phase:grind / NOSEGRIND direction=ID_TRICK_BS | spin:normal:0: / FLIP held_rotation=360 / phase:grind / CROOKED direction=ID_TRICK_FS | Trick component differs: base: "ID_TRICK_NOSEGRIND" vs "ID_TRICK_CROOKED", direction: "ID_TRICK_BS" vs "ID_TRICK_FS" |
| 5 | spin:normal:0: / KICKFLIP | spin:normal:0: / KICKFLIP | Match |
| 6 | spin:normal:0: / KICKFLIP | spin:normal:0: / KICKFLIP / KICKFLIP | Different number of tricks or spin segments |
| 7 | spin:normal:0: / KICKFLIP | spin:normal:0: / KICKFLIP | Match |
| 8 | spin:normal:180:BS / KICKFLIP / phase:grind / OVERWILLY direction=ID_TRICK_FS | spin:normal:0: / KICKFLIP / phase:grind / LIPSLIDE direction=ID_TRICK_BS / SMITH direction=ID_TRICK_BS | Different number of tricks or spin segments |
| 9 | spin:normal:180:BS / KICKFLIP / phase:grind / OVERWILLY direction=ID_TRICK_FS | spin:normal:0: / KICKFLIP / phase:grind / 50_50 direction=ID_TRICK_FS | Spin, direction or stance differs: target spin:normal:180:BS, attempt spin:normal:0: |
| 10 | spin:normal:180:BS / KICKFLIP / phase:grind / OVERWILLY direction=ID_TRICK_FS | spin:normal:180:BS / KICKFLIP | Different number of tricks or spin segments |
| 11 | spin:normal:0: / KICKFLIP / phase:grind / BOARDSLIDE direction=ID_TRICK_FS | spin:normal:0: / KICKFLIP / phase:grind / LIPSLIDE direction=ID_TRICK_FS | Trick component differs: base: "ID_TRICK_BOARDSLIDE" vs "ID_TRICK_LIPSLIDE" |
| 12 | spin:normal:0: / KICKFLIP / phase:grind / BOARDSLIDE direction=ID_TRICK_FS | spin:normal:0: / OLLIE / phase:grind / LIPSLIDE direction=ID_TRICK_FS | Trick component differs: base: "ID_TRICK_KICKFLIP" vs "ID_TRICK_OLLIE" |
| 13 | spin:normal:0: / KICKFLIP / phase:grind / BOARDSLIDE direction=ID_TRICK_FS | spin:normal:0: / KICKFLIP / phase:grind / BOARDSLIDE direction=ID_TRICK_FS | Match |
| 14 | spin:nollie:0: / OLLIE / phase:grind / CROOKED direction=ID_TRICK_BS | spin:nollie:0: / OLLIE / phase:grind / CROOKED direction=ID_TRICK_FS | Trick component differs: direction: "ID_TRICK_BS" vs "ID_TRICK_FS" |
| 15 | spin:normal:0: / OLLIE / phase:grind / 5_0_GRIND direction=ID_TRICK_BS / spin:normal:0: / OLLIE | spin:normal:0: / OLLIE / phase:grind / 5_0_GRIND direction=ID_TRICK_FS / spin:normal:0: / OLLIE | Trick component differs: direction: "ID_TRICK_BS" vs "ID_TRICK_FS" |
| 16 | spin:nollie:0: / OLLIE / phase:grind / NOSEGRIND direction=ID_TRICK_BS / spin:nollie:0: / KICKFLIP | spin:nollie:0: / OLLIE / phase:grind / NOSEGRIND direction=ID_TRICK_BS / spin:nollie:0: / KICKFLIP / phase:grind / 5_0_GRIND direction=ID_TRICK_BS | Different number of tricks or spin segments |
| 17 | spin:normal:0: / KICKFLIP / phase:grind / CROOKED direction=ID_TRICK_BS | spin:normal:0: / KICKFLIP | Different number of tricks or spin segments |
| 18 | spin:normal:0: / KICKFLIP / phase:grind / CROOKED direction=ID_TRICK_BS | spin:normal:0: / KICKFLIP / phase:grind / NOSESLIDE direction=ID_TRICK_BS | Trick component differs: base: "ID_TRICK_CROOKED" vs "ID_TRICK_NOSESLIDE" |
| 19 | spin:normal:0: / KICKFLIP / phase:grind / CROOKED direction=ID_TRICK_BS | spin:normal:0: / KICKFLIP / phase:grind / CROOKED direction=ID_TRICK_BS | Match |
| 20 | spin:normal:180:BS / KICKFLIP | spin:normal:180:FS / KICKFLIP | Spin, direction or stance differs: target spin:normal:180:BS, attempt spin:normal:180:FS |
| 21 | spin:normal:180:BS / KICKFLIP | spin:normal:180:BS / KICKFLIP | Match |
| 22 | spin:normal:180:BS / KICKFLIP | spin:normal:180:BS / KICKFLIP | Match |
| 23 | spin:normal:180:BS / KICKFLIP | spin:normal:180:BS / KICKFLIP | Match |
| 24 | spin:normal:180:BS / KICKFLIP late=Late | spin:normal:180:FS / KICKFLIP late=Late | Spin, direction or stance differs: target spin:normal:180:BS, attempt spin:normal:180:FS |
| 25 | spin:normal:180:BS / KICKFLIP late=Late | spin:normal:180:BS / KICKFLIP late=Late | Match |
| 26 | spin:normal:180:BS / KICKFLIP late=Late | spin:normal:180:BS / KICKFLIP late=Late | Match |
| 27 | spin:nollie:360:BS / LASERFLIP | spin:nollie:360:BS / LASERFLIP | Match |
| 28 | spin:nollie:360:BS / LASERFLIP | spin:nollie:360:BS / LASERFLIP | Match |
| 29 | spin:nollie:360:BS / LASERFLIP | spin:nollie:360:BS / POPSHUVIT held_rotation=360 direction=ID_TRICK_BS | Trick component differs: base: "ID_TRICK_LASERFLIP" vs "ID_TRICK_POPSHUVIT", direction: "" vs "ID_TRICK_BS", held_rotation: "" vs "360" |
| 30 | spin:nollie:360:BS / LASERFLIP | spin:normal:180:BS / KICKFLIP | Spin, direction or stance differs: target spin:nollie:360:BS, attempt spin:normal:180:BS |
| 31 | spin:normal:360:BS / HARDFLIP | spin:normal:360:BS / HARDFLIP | Match |
| 32 | spin:normal:360:BS / HARDFLIP | spin:normal:360:BS / HARDFLIP | Match |
| 33 | spin:normal:360:BS / HARDFLIP | spin:normal:180:FS / KICKFLIP | Spin, direction or stance differs: target spin:normal:360:BS, attempt spin:normal:180:FS |
| 34 | spin:normal:540:BS / INWARDHEELFIP held_rotation=360 | spin:normal:540:BS / INWARDHEELFIP held_rotation=360 | Match |
| 35 | spin:normal:540:BS / INWARDHEELFIP held_rotation=360 | spin:normal:540:BS / FLIP held_rotation=360 | Trick component differs: base: "ID_TRICK_INWARDHEELFIP" vs "ID_TRICK_FLIP" |
| 36 | spin:normal:540:BS / INWARDHEELFIP held_rotation=360 | spin:normal:540:BS / HARDFLIP | Trick component differs: base: "ID_TRICK_INWARDHEELFIP" vs "ID_TRICK_HARDFLIP", held_rotation: "360" vs "" |
| 37 | spin:nollie:540:FS / POPSHUVIT held_rotation=360 direction=ID_TRICK_FS | spin:nollie:360:FS / POPSHUVIT held_rotation=360 direction=ID_TRICK_BS | Spin, direction or stance differs: target spin:nollie:540:FS, attempt spin:nollie:360:FS |
| 38 | spin:nollie:540:FS / POPSHUVIT held_rotation=360 direction=ID_TRICK_FS | spin:nollie:360:FS / POPSHUVIT direction=ID_TRICK_FS | Spin, direction or stance differs: target spin:nollie:540:FS, attempt spin:nollie:360:FS |
| 39 | spin:nollie:540:FS / POPSHUVIT held_rotation=360 direction=ID_TRICK_FS | spin:nollie:180:FS / KICKFLIP | Spin, direction or stance differs: target spin:nollie:540:FS, attempt spin:nollie:180:FS |
| 40 | spin:normal:360:FS / HOSPITALFLIP | spin:normal:360:FS / HOSPITALFLIP | Match |
| 41 | spin:normal:360:FS / HOSPITALFLIP | spin:normal:360:FS / KICKFLIP | Trick component differs: base: "ID_TRICK_HOSPITALFLIP" vs "ID_TRICK_KICKFLIP" |
| 42 | spin:normal:360:FS / HOSPITALFLIP | spin:normal:180:FS / KICKFLIP | Spin, direction or stance differs: target spin:normal:360:FS, attempt spin:normal:180:FS |
| 43 | spin:normal:360:BS / IMPOSSIBLE / HEELFLIP late=Late | spin:normal:180:BS / IMPOSSIBLE underflip=ID_TRICK_UNDERFLIP | Different number of tricks or spin segments |
| 44 | spin:normal:360:BS / IMPOSSIBLE / HEELFLIP late=Late | spin:normal:360:BS / IMPOSSIBLE | Different number of tricks or spin segments |
| 45 | phase:ground / TRICKLESS_AIR | phase:ground / PUMPING | Trick component differs: base: "ID_TRICK_TRICKLESS_AIR" vs "ID_TRICK_PUMPING" |
| 46 | spin:normal:180:BS / HEELFLIP late=Late | spin:normal:180:BS / HEELFLIP late=Late | Match |
| 47 | spin:normal:540:BS / ROCKETAIRGRAB | spin:normal:540:BS / ROCKETAIRGRAB | Match |
| 48 | spin:normal:0: / HARDFLIP held_rotation=360 | spin:normal:0: / HARDFLIP | Trick component differs: held_rotation: "360" vs "" |
| 49 | spin:normal:0: / AIRWALK | spin:normal:0: / AIRWALK | Match |
| 50 | spin:normal:180:FS / OLLIE | spin:normal:180:FS / OLLIE | Match |

Hospital Flip, Impossible Underflip, laserflips, 360 flips, hardflips, late flips, grinds and grabs are present in the accepted detections. A Hospital Flip target versus Kickflip defense, different grind types/directions and different measured spins remain genuine recorded differences. Boned/pop canonicalization remains unchanged.
