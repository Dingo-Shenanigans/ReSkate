# Skate Synchros: outfit sharing

Players see each other's modded outfits in multiplayer, as the wearer sees them, without
installing the wearer's mods. Today a peer's item this PC does not have becomes a stand-in
(`Extension/Multiplayer/Remote/stand_ins.h`), so a modded outfit is only ever seen by its wearer.

Status: design, with the first measurements. Nothing here is built yet.

## What it does

- **Opt in, both ends.** Two settings, both off: *Share my outfit* and *Show shared outfits*.
  With either off nothing is sent or asked for, and stand-ins work as they do today.
- **Only people you choose.** An outfit is shared with mutual Steam friends, and with anyone
  given your **share code**. Everyone else sees stand-ins.
- **Only what is worn.** The wearer's PC sends the pieces of the items being worn, never a pack.
  A pack of thousands of items costs the viewer the few the wearer has on.
- **Any mod.** The files come from the wearer's PC, so a mod not on Thunderstore (made by the
  player, passed around privately) shares like any other.
- **Seen, never owned.** A received item dresses the player it came from, on this PC. It is not
  in the wardrobe, cannot be worn, and is not saved to the profile.

## Who an outfit is shared with

A wearer shares with a peer when *Share my outfit* is on and either

- the peer is a Steam friend (`k_EFriendRelationshipFriend`) and *Friends* is ticked, or
- the peer has this player's current share code.

A viewer asks a peer for its outfit when *Show shared outfits* is on, the peer is not blocked,
and the peer is a friend or a player whose code the viewer entered.

### Share codes

A code is the player's own, shown on the settings page: `RSK-7Q4M-K2XD-9PW3` (Crockford base32,
60 bits of randomness, no SteamID in it). It is made on this PC and kept in the profile.

- Entering a code stores it against nobody yet. When the viewer meets a player in a session, it
  asks with a keyed proof of each code it holds (HMAC-SHA256 of the code over the session id and
  both SteamIDs), so the code itself never crosses the wire and a proof cannot be replayed in
  another session or by another player. The wearer answers only to a proof of its own current
  code, and then remembers that SteamID as paired.
- *New code* replaces the code: nobody new can pair with the old one. Players already paired
  stay paired until removed from the list on the same page.
- A code shares one way: giving someone your code lets them see your outfit, not theirs.

No server is involved; the check is between the two PCs, over the session's connection.

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

The received files are kept in a cache outside `Mods/` (`Mods/.reskate/shared-outfits/`), as a
layer the merge reads after every enabled mod and never lists, so it can never win over a mod
the player installed.

- **New items need the shared bundle's definitions**, so for now they show after the next level
  load (`contrib/live-apply-without-reload.md`, the table "What each kind of cosmetic change
  needs"). Until then the player wears stand-ins. The plan in that note for new items without a
  reload would make them show at once.
- **Kept out of the wardrobe:** the local catalogue (`Extension/Customization/local_cosmetic_catalog`)
  skips every key from the layer, so a received item is never owned, listed or wearable.
- The layer is used to dress remote skaters only. Nothing in it is sent on, saved, or counted.

## What is refused

- **Store items.** The wearer runs the store-copy check on each worn item before sending, and the
  viewer runs it again on what it receives (`StoreItems::original`, `StoreMeshes`). A copy of a
  store item is neither sent nor shown, and the viewer drops the whole closure.
- **Anything but cosmetics.** A closure holds item definitions, presets, textures, meshes and
  their chunks. Scripts, plugins, settings, levels and UI are not part of any closure, and a
  received file of any other kind drops the closure.
- **Blocked players.** Blocking a player on the session list stops their outfit at once and
  drops what was received from them.

## Settings page

```
Outfit sharing
  [ ] Share my outfit
        [x] with Steam friends
        My code  RSK-7Q4M-K2XD-9PW3   [Copy] [New code]
  [ ] Show shared outfits
        Codes I entered   [ RSK-____-____-____ ] [Add]
        Paired players    Sandos (code), azzie (friend)   [Remove]
  Cache  412 MB of 2 GB   [Empty]
```

## Work, in order

1. **Closure** (Engine/Vfs): from a mod's TOC and cas, the files one item needs, minus the game's.
   Tested against real packs; this settles the per-item size.
2. **Layer** (Engine/Vfs merge): a cache of received files merged after the mods, its keys kept
   out of the local catalogue.
3. **Transfer** (Extension/Multiplayer): ask, have-list, send in pieces on a low-priority lane,
   resume, limits.
4. **Who** (Extension/Multiplayer): friends, share codes and their proofs, the paired list, block.
5. **Settings page** and the remote skater dressing again when an item arrives.

## Open questions for the maintainers

- `map_download.h` keeps cosmetics out on purpose ("no cosmetics, no settings, no scripts").
  This is a different route (from a chosen peer, cosmetics only, never owned): is that acceptable?
- Is a merge layer read after the mods the right place, or should received items load as a
  delta bundle once that exists?
