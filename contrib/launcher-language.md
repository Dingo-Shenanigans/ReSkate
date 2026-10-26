# Launcher languages

The launcher shows its text in the language picked in Settings > ADVANCED > Language. Until one
is picked it follows Windows: a Windows shown in Turkish opens the launcher in Turkish, when the
launcher has that language. Text a language has no translation for shows in English.

Code: [language.h](../Launcher/language.h) and [language.cpp](../Launcher/language.cpp); the
languages are in [Launcher/languages](../Launcher/languages).

## How text is looked up

By its English, as the source writes it: `tr("Ready to skate")`, or `trf("{} mods did not load",
count)` when it has values in it. English needs no file, and a translator reads each sentence whole.
A table whose labels are translated where they are drawn marks them with `translatable("...")`.
Log lines, file names, settings keys and the game's own texts are not translated.

## A language

A JSON object of English to translation, with `"@language"` the language's name in itself:

```json
{
  "@language": "Türkçe",
  "Ready to skate": "Kaymaya hazır",
  "{} mods did not load": "{} mod yüklenmedi"
}
```

- The file is named after the language's code: `tr.json`, `pt-br.json`. That is the code saved in
  `ReSkateLauncher.settings.json` (`"language"`) and matched against Windows' own language.
- A translation keeps every `{}` (or `{:.0f}`, `{0}`) of the English, in any order. One that does
  not shows in English.
- Keep it as short as the English: buttons and tiles have a fixed width. Write ALL CAPS where the
  English is, with the language's own capitals (Turkish İ and I).
- An empty translation, or a line left out, shows in English.

[en-template.json](../Launcher/languages/en-template.json) lists every text to translate, each
with an empty translation: copy it to `<code>.json` and fill it in.

## Trying one, and shipping it

A `languages\<code>.json` beside ReSkateLauncher.exe is read when the launcher starts: it adds a
language, or overrides lines of one built in. Restart the launcher to see a change.

To ship it, put it in `Launcher/languages/`: every file there except the template is built into
ReSkateLauncher.exe (cmake/Apps.cmake), so a release needs no file beside it.

When the launcher's text changes, make en-template.json again with
`node contrib/launcher_language_template.js`.
