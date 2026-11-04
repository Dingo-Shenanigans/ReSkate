# Outfit sharing

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

### Measured so far

From the TOC of a real deck pack (`snowgrave-azzie_Community_Decks_Pack` 0.5.5: 18 decks, grip
tapes and stickers, a 64.6 MB cas):

| Part | Size | Sent for one worn deck |
|---|---|---|
| Shared bundle, the mod's own files | 2.75 MB (180 files) | the deck's definition, preset and collection entry |
| Item bundles, the mod's own files | 0.66–14.24 MB each (5 bundles) | the files the deck's closure names |
| TOC chunks (texture and mesh data) | 67 MB (157 chunks) | the chunks its textures and meshes name |
| Game files the bundles refer to | over 100 MB | none |

Mods do not keep one bundle per item: this one puts many decks in one 14 MB bundle, under a game
item's bundle name. So the closure has to be cut per file inside the bundles, not per bundle,
and following it needs the cas contents (EBX references, resource chunk ids), not only the TOC.
The per-item size is the next thing to measure.

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
