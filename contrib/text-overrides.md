# Text Overrides

The player and mods can change any text the game shows, or add text it lacks, with a
`reskate-strings.json`: a translation, a renamed menu, a fixed typo. The text goes into the game's
own string-override store (the one the backend's `client_strings_v1` fills in retail), which every
lookup checks before the shipped string database, so it takes the place of the game's text
wherever that is shown. A dump of every string the game has gives translators something to start
from.

Code: [text_overrides.cpp](../Extension/Localization/text_overrides.cpp) (reading the files,
storing the text, the dump), called from the client tick beside the S.K.A.T.E. strings in
[native_throwdowns.cpp](../Extension/Throwdowns/native_throwdowns.cpp), which shares its hash and
store helpers; the ids the dump learns come from the key-based translate hook in
[local_buildkit_labels.cpp](../Extension/Objects/local_buildkit_labels.cpp). The addresses and
fingerprints are in [localization.h](../Engine/Game/Build/20260929/localization.h).

## Where the files are

1. `%LOCALAPPDATA%\ReSkate\reskate-strings.json`: the player's own, read first.
2. `Mods\<mod>\reskate-strings.json` of each mod that is on, highest in the load order first.

The first file to name a string wins; a later file's text for it is left out. A file that is not
valid JSON is logged and skipped, and the others still load.

## The format

One JSON object, UTF-8 (a byte order mark is allowed):

```json
{
  "_about": "Turkish UI, first pass",
  "ID_SOME_STRING": "Some text",
  "#1a2b3c4d": "Text known only by its hash"
}
```

- A key is the string's id, as the game asks for it.
- A key `#` and up to 8 hex digits names a string by its hash (the game's: `h = h * 33 + byte`
  from `0xffffffff`). Most of the game's text is known only that way. ReSkate stores it under an
  id of its own with that hash, which the store treats as the same string.
- Keys starting with `_` are notes and are ignored, as is anything that is not a string.
- The text keeps the game's markup (`[font ...]...[/font]`, `{0:s}` and the like) as it is.

## Live reload

The files are looked at every 2 seconds. An edited, added or removed file is read again and its
text stored, so a translation can be checked in game without a restart. Text is stored a few
hundred strings a tick, so a large file takes a moment to show everywhere. The log (`logs\ReSkate.log`)
says how many overrides each file gave and how many were stored.

## The dump

With the environment variable `RESKATE_STRINGS_DUMP` set (to anything) before the game starts,
ReSkate writes `%LOCALAPPDATA%\ReSkate\strings-dump.json` in the same format: every string of the
loaded string database, read from its stored form to UTF-8, and rewritten when more is seen. A key
is the id where one is known (asked for by the game while it ran, or named in a
`reskate-strings.json`), otherwise `#hash`. Text that would not read as UTF-8 is repaired and the
log names it. Copy the entries to translate into a `reskate-strings.json`.

## Limits

- Overrides apply to the language the game is running in; a translation made from the English
  dump is meant for the game set to English. The text is stored again when the game loads another
  database.
- Where the game shows text in capitals, the uppercasing is the game's own and does not know every
  language's rules (Turkish `i` to `İ`, for one); translators should write those strings in
  capitals themselves.
- A string taken out of every file keeps its overridden text until the game restarts.
- If the game's string store or database code differs from the supported build, nothing is
  stored (or, for the dump, only the ids seen are written) and the log says so.
