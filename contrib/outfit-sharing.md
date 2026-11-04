# Skate Synchros: outfit sharing

Players see each other's modded outfits in multiplayer, as the wearer sees them, without
installing the wearer's mods. Today a peer's item this PC does not have becomes a stand-in
(`Extension/Multiplayer/Remote/stand_ins.h`), so a modded outfit is only ever seen by its wearer.

Status: built end to end, not yet tried in the game. Codes, groups and who sees whom; the
messages and the exchange between two players on Steam side connections of their own; the item
cutter; the cache; received outfits made into a mod per wearer and merged after everything else
(read back on 29 real items, one of each of 29 mods); settings in `Mods/.synchros/settings.json`
and a `synchros` console command. Still to come: the settings page in the menu, and showing a
received item without waiting for the next level load.

## What it does

- **Opt in, both ends.** Two settings, both off: *Share my outfit* and *Show shared outfits*.
  With either off nothing is sent or asked for, and stand-ins work as they do today.
- **Codes are the only way in.** Nobody sees a player's modded outfit without a code from them,
  Steam friend or not. A **personal code** lets whoever has it see its owner's outfit; a **group
  code** lets everyone who has it see everyone else's who has it. Everyone else sees stand-ins.
- **Only what is worn.** The wearer's PC sends the pieces of the items being worn, never a pack.
  A pack of thousands of items costs the viewer the few the wearer has on.
- **Any mod.** The files come from the wearer's PC, so a mod not on Thunderstore (made by the
  player, passed around privately) shares like any other. Plugin code is never sent: a look that
  needs a plugin the viewer lacks (a shader plugin's deck) shows as it does without it.
- **Seen, never owned.** A received item dresses the player it came from, on this PC. It is not
  in the wardrobe, cannot be worn, and is not saved to the profile.

## Who an outfit is shared with

`Extension/Multiplayer/Synchros/sharing_rules.h`. A wearer sends its outfit to a viewer when
*Share my outfit* is on, the viewer is not blocked, and in this session the viewer proved either

- the wearer's current **personal code** (the viewer is then paired for good, until removed), or
- a **group code** both hold (let in for this session only, so leaving a group takes effect at once).

### Codes

`share_code.h`. A personal code is `RSK-7Q4M-K2XD-9PW3`, a group code `RSG-...`: twelve Crockford
base32 characters, eleven random (55 bits) and a check character that catches a mistyped or two
swapped characters. Made on the PC and kept in the profile; no SteamID in them.

- **The code never crosses the wire.** A code says nothing about whose it is, so a viewer with codes
  offers a proof of each to every player in the session: HMAC-SHA256 under the code's key of the
  session id and both SteamIDs. A proof is good only for that pair in that session, so it cannot be
  replayed by another player or later, and it reveals nothing about the code.
- **Personal:** one way. Giving someone your code lets them see your outfit, not theirs. *New code*
  replaces it: nobody new can pair with the old one; players already paired stay until removed.
- **Groups**, like Mare's Syncshells but with no server: a name (the player's own label, never
  sent) and a code. Whoever makes one gives the code out; whoever enters it is in. Leaving is
  removing it. Without a server nobody can be thrown out of a code they hold, so the PC that made
  the group offers *New group code*, to give to everyone still meant to be in it; whoever keeps
  only the old one no longer matches anyone who moved. (That button is a courtesy, not a lock:
  anyone with the code could make a group of their own with it.)
- **Blocking** wins over codes and groups, both ways, at once.

No server is involved: the checks are between the two PCs, on their own Steam connection.

## Only what is worn

The outfit packet (`Extension/Multiplayer/Remote/cosmetics.h`, `Appearance`) already names
every worn item by asset. A viewer that lacks one asks the wearer for it. The wearer answers
with the item's **closure**: the item's definition and everything it is made of that comes from
a mod rather than the game.

- *What an item is made of* is what the store-copy check already works out
  (`Engine/Vfs/mod_store_copies.h`, `ItemContent`): its looks (appearance presets, decal
  textures) and shapes (meshes, morph presets). The closure follows those to their EBX, their
  resources, and the chunks the resources name.
- **Anything the game ships is left out**: every viewer has it. Only files the winning mod
  provides are sent.
- **Files are named by content** (SHA-1, which the bundles carry already). The viewer says which
  it has, from earlier, from another player, or from its own mods, and only the rest is sent.
- **Limits:** 64 MB per outfit and 2 GB of cache by default, least recently used first out.
  A closure over the limit is not sent and that item stays a stand-in.
- Sent over the session's Steam connection on a low-priority lane, so play and voice are not
  held up. A player who leaves stops the transfer.

### Measured

`Tools/OutfitProbe` follows each item a mod ships to the files of the mod's own it is made of
(EBX imports, the looks and bundles it names by path, resources, and the chunks they name) and
adds up what would be sent. Run on 45 installed mods, 2026-11-04 (sizes as stored, compressed):

| Mod | Items | Mod's own files | One worn item: median | largest |
|---|---|---|---|---|
| Soulworker Character Pack (costumes) | 10 | 120.3 MB | 9.5 MB | 23.9 MB |
| Suisei and Rukia (costumes, deck, grip) | 7 | 72.4 MB | 8.9 MB | 24.4 MB |
| Cony Hawk player model | 3 | 37.0 MB | 5.8 MB | 25.0 MB |
| Varous IdolRiot Decks | 16 | 131.3 MB | 7.4 MB | 23.9 MB |
| azzie Community Decks Pack (decks, grips, stickers) | 25 | 68.0 MB | 3.1 MB | 5.8 MB |
| IdolRiot Live Evil Twins Pack | 11 | 19.4 MB | 1.4 MB | 2.9 MB |
| Kichi Aura Wheels | 88 | 23.8 MB | 0.13 MB | 0.55 MB |
| Road Rash (tattoos) | 45 | 34.8 MB | 0.64 MB | 0.94 MB |
| Animated Skulls Croptop | 1 | 3.2 MB | 0.58 MB | 0.58 MB |

A whole worn outfit (a costume or clothes, a deck, grip and wheels) comes to about 5 to 35 MB,
against hundreds of MB for the packs it comes from. Files are named by content, so an item two
packs share (the Soulworker costumes are in three of the installed packs) is sent once.

Mods do not keep one bundle per item (one deck pack puts many decks in one 14 MB bundle under a
game item's name), so the closure is cut per file, never per bundle. An item names its looks by
path (`AssetName`, `nabi_ap`, `<guid>_ap`), not by import, so those names have to be followed.

Still to check: some items come out very small (most SDEC decks about 0.05 MB, the PSDEC glow
tests 0.02 MB). Either they really are only an item and a shader preset over the game's own deck
textures, or something they name is not followed yet. A costume item names every variant preset
it has (`nabi_role21_ap` and more), so its closure may be larger than what one look needs.

## Showing a received item

The received files are kept in a cache outside `Mods/` (`Mods/.synchros/`), as a
layer the merge reads after every enabled mod and never lists, so it can never win over a mod
the player installed.

- **New items need the shared bundle's definitions**, so for now they show after the next level
  load (`contrib/live-apply-without-reload.md`, the table "What each kind of cosmetic change
  needs"). Until then the player wears stand-ins. The plan in that note for new items without a
  reload would make them show at once.
- **Kept out of the wardrobe:** the local catalogue (`Extension/Customization/local_cosmetic_catalog`)
  skips every key from the layer, so a received item is never owned, listed or wearable.
- The layer is used to dress remote skaters only. Nothing in it is sent on, saved, or counted.

## What is refused, and why it stays refused

ReSkate keeps other players' cosmetics out today for reasons that still hold. Each one, and what
keeps it true here:

- **Paid store items stay locked** (AGENTS.md, "Nothing that unlocks what the game sells").
  - The wearer only has items that passed its own merge, which leaves out any mod with a copy of
    a store item (merge check 0x5343, `Engine/Vfs/mod_store_copies.h`).
  - The viewer trusts none of that. What it receives becomes a layer of its own merge and goes
    through the **same store-copy check as an installed mod**, items and meshes alike. A copy of
    a store item is not shown; the whole outfit from that player is dropped.
  - Received items are never the viewer's: the local catalogue skips every key from the layer,
    so they are not in the wardrobe, cannot be worn, equipped, saved or sent on.
  - The game's own items are never sent (`Withheld::game_item`): the viewer has them already,
    and what the viewer owns of them is untouched.
- **Only from people you chose.** Off by default, both ends: nobody's outfit is shown without a
  code they gave you, and blocking a player drops their outfit at once and empties what was
  cached from them. Plugins are never sent.
- **The plugin sandbox is untouched.** No `reskate.*` API is added; plugins cannot read the cache,
  send files or dress other players, exactly as today.
- **The host and the server carry nothing.** Outfits go on a Steam connection of their own between
  the two players, relayed by Steam (no IP addresses exchanged), with size limits per file, per
  outfit and per message. A session's host sees no difference.
- **Mods are not handed out as packs.** Only the pieces of the items worn are sent, only to players
  holding the wearer's code, into a content-addressed cache that is not a mod folder and is
  trimmed to its size limit. (What a PC displays can always be pulled out by someone determined;
  that is as true of an installed mod.)

## Settings page

```
Skate Synchros
  [ ] Share my outfit
        My code   RSK-7Q4M-K2XD-9PW3   [Copy] [New code]
        Paired    Sandos, azzie                         [Remove]
  [ ] Show shared outfits
        Codes     [ RSK-____-____-____ ] [Add]    Sandos's code   [Remove]
  Groups
        Sk8 Crew  RSG-4K2M-...   [Copy] [New group code] [Leave]
        [ Create a group ]   [ RSG-____-____-____ ] [Join]
  Blocked   (none)
  Cache     412 MB of 2 GB   [Empty]
```

## Work, in order

1. **Closure** (Engine/Vfs): from a mod's TOC and cas, the files one item needs, minus the game's.
   Tested against real packs; this settles the per-item size.
2. **Layer** (Engine/Vfs merge): a cache of received files merged after the mods, its keys kept
   out of the local catalogue.
3. **Transfer** (Extension/Multiplayer): ask, have-list, send in pieces on a low-priority lane,
   resume, limits.
4. **Who** (Extension/Multiplayer/Synchros, done): personal and group codes, their proofs, pairing, block.
5. **Settings page** and the remote skater dressing again when an item arrives.

## Open questions for the maintainers

- `map_download.h` keeps cosmetics out on purpose ("no cosmetics, no settings, no scripts").
  This is a different route (from a chosen peer, cosmetics only, never owned): is that acceptable?
- Is a merge layer read after the mods the right place, or should received items load as a
  delta bundle once that exists?
