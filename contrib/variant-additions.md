# Private additions beside a selected variant (development)

Existing `variants.displaces` declarations keep their existing behaviour. A
separate, optional `variant_additions` object in `reskate-plugin.json` permits
the catalogue to check cooperation for exact native bundle names:

```json
{"variant_additions":{"accept":["win32/test/shared"]}}
```

The adding pack separately declares:

```json
{"variant_additions":{"contribute":["win32/test/shared"]}}
```

Each list has at most 16 names of at most 260 printable ASCII characters. Names
start with `win32/`, are compared in lower case, and cannot contain traversal,
backslashes, wildcards or duplicates. A malformed object grants neither side;
it never removes the variant author's displacement rule. These are content
declarations, not new Luau permissions.

For every displaced bundle the adding pack delivers, both declarations must
match. The catalogue reads native manifests using the merge's region reader,
including CAS manifests not stored first. It checks archive placement bounds,
native asset counts, and bounded EBX decoding. The adding pack must preserve
every original name, GUID/RID, hash, type, metadata and logical size. Its new
names and native identities must be distinct from the game's originals, the
selected variant, and other enabled prospective contributors to that bundle.
Two prospective packs colliding with each other are both refused, independent
of priority. Unreadable input is refused with a diagnostic.

The first implementation requires private EBX imports and typed resource
references to resolve within the selected bundle plus that pack's additions.
It does not admit cross-pack dependencies, infer every reference in opaque
native resource formats, or provide a global cross-bundle identity resolver.
Authors remain responsible for their native resource dependencies. Payloads
and native delivery locations are not rewritten. Shader graphs continue to
follow the separate shader-lookup merge rules; this declaration never grants
permission to copy a graph into another bundle.

Preservation checks cover the bundle's assets, not TOC-level chunks. As with
ordinary mods, a donor can still replace a TOC chunk; authors must account for
that separately. Admission timings are included in the catalogue notes.

The non-first manifest reader fix also applies to mods without opt-in. Their
payload slots are corrected, and pass-through regions write the manifest first.

An accepted contribution is projected to its private additions only. Stock
assets carried as authoring scaffold are not merged from that pack. The
selected variant is authoritative over originals, including intentional
omissions. Private additions referring to a removed original are refused.
Chunk records retain their original manifest indexes before the projection,
so filtering does not shift another chunk's metadata into their place.

Admission is recomputed by `load_catalog` at each launch and live apply. A
different variant is checked afresh. Decisions are part of the merge
fingerprint. Cooperation with multiple variant owners is conservatively
refused, and an addition is set aside if its verified variant is subsequently
removed. After every retry or live-apply removal, orphan donors are withdrawn
and variant authority is rebuilt from surviving donors. A bundle that becomes
unreadable after admission is reported against its contributor, not the whole
merge.

Tests cover native catalogue admission and a changed selection, strict default
behaviour, malformed opt-in, native renamed GUID collisions, conflicting
prospective packs, non-first manifests, and replacement/removal preservation
in both priority orders, plus retries after losing a donor, its selected
variant, or a previously readable TOC. Native rendering remains an author
validation step; archive checks alone do not establish it.
