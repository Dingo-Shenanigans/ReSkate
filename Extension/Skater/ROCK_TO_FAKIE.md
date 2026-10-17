# Rock to Fakie

skate. has no input for a rock to fakie, but the move is already in the game: ride up to a coping,
Board Stall or Lip Stall on it, and press **B** to roll back in fakie. With this on, the game calls
that what it is.

## Using it

Off until you switch it on, in the console (`~`): `rocktofakie on|off|status`. The choice is kept.

- **The trick ticker** names a Board Stall or a Lip Stall **Rock to Fakie** while you hold it. A
  separate BS / FS prefix the ticker adds is the game's and stays.
- **Leaving the stall with B** records the trick as the game's own **fakie_rock** instead of the
  stall. B must be pressed while the stall is held, and the stall left within a moment of it, as it
  is when B is what ends it.
- Other tricks, other labels, other players' tickers and the game's string database are untouched.
- `rocktofakie status` shows whether both parts are hooked, and how many labels and tricks it has
  changed this game.

## How it works

Two hooks, installed when the game's code matches the supported build
(`Engine/Game/Build/20260929/rock_to_fakie.h`); while it is off both pass everything on unchanged.

- **The label.** The ticker asks a virtual localization method (`0x2e1ac10`) to write a stall's
  label into the ticker's own buffer. After the game's method has written it, the text is replaced
  only when the local skater's physics state is a held stall (400 / 401, from No Bail's state
  watch), the text is exactly `Board Stall` or `Lip Stall`, the call came through the ticker's route
  (three return addresses checked on the unwound stack) and the buffer has room. Nothing else the
  method writes is touched; a stall label seen on any other route is counted in the status and left.
- **The trick.** The trick recorder (`0x47803a0`) copies each 104-byte trick packet into its history.
  When a Board Stall (id 9) or Lip Stall (id 12) packet arrives as the stall is left (400 / 401 to
  604 within 0.1 s) and B was pressed within 0.125 s, the recorder gets a copy carrying id 187, the
  game's `fakie_rock`. The game's own packet is never written.

B is read on the client tick from ReSkate's controller reader (XInput B, Circle on a PlayStation
pad), so it only counts while the game window has focus and no ReSkate menu is open.

## Limits

- Tied to the supported game build: on any other, the status says which part is unavailable.
- It does not detect a real coping contact; any Board Stall or Lip Stall counts, wherever it is done.
- Whether the remapped id reaches every scoring and history path (and what a multiplayer host makes
  of it) has not been checked; the label change is local only.
