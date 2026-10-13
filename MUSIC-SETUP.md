# ReSkate music sources: setup

ReSkate can play **Spotify**, **YouTube Music**, **your own music files**, **internet radio** and
**any app on your PC** through the in-game music player. Each one shows up as its own row in the game's music screen, with tiles you pick
like any other playlist. Pause, next, track name and cover art all work from the game.

Everything is set up from one page in your browser. Each source has numbered steps on the page
that tick off as you do them, so you can also just follow the page.

## Open the setup page

1. Start the game.
2. Press **~** to open the console.
3. Type `music setup` and press Enter. (The menu's **MUSIC** page has a button for it too, under **SOURCES**.)

Your browser opens `http://127.0.0.1:8421/`. The page only works on the PC the game is running
on, and only while the game is running.

> A new row or tile can take a game restart to show up. If you set something up and don't see
> it in the music screen, restart the game once.

---

## Spotify

**You need:** Spotify Premium. A free account cannot play to the game: if you pick **ReSkate**
with one, the game tells you so on the setup page and forgets it.

### Play Spotify through the game

1. Start the game. Windows may ask whether to allow the game through the firewall: allow it on
   private networks. Spotify uses this to find the game.
2. Open Spotify on your phone or PC, on the same network as the game.
3. Open Spotify's device list and pick **ReSkate**.

That's it. You only do this once; after that the game signs in by itself.

In the game's music screen, open the **Spotify** row and pick the **Spotify** tile. Whatever
Spotify was playing carries on through the game. If nothing was playing, it starts your Liked
Songs on shuffle.

### Show your own playlists (optional)

To get your playlists and Liked Songs as tiles, with their tracks listed, ReSkate needs a
Spotify developer app. This is free and takes a couple of minutes.

1. Go to <https://developer.spotify.com/dashboard> and sign in.
2. Create an app. Any name and description will do.
3. Add this redirect URI exactly: `http://127.0.0.1:47824/callback`
4. Copy the app's **Client ID**.
5. On the setup page, under **Spotify → Show your own playlists**, paste the Client ID and press **Save**.
6. Press **Sign in with Spotify** and approve it in the browser tab that opens.

Your first 50 playlists appear, each with its first 50 tracks. Liked Songs shows your 50 most
recent. Pressing a track in the game starts Spotify at that track.

---

## YouTube Music

**You need:** nothing installed beforehand. ReSkate downloads one small tool for you.

> Playing YouTube this way is against YouTube's terms of service. Whether to use it is up to you.

1. On the setup page, under **YouTube Music**, press **Download and set up**. This downloads
   `yt-dlp` (about 18 MB) from its official GitHub releases.
2. Paste a link to a YouTube or YouTube Music **video or playlist** and press **Add**.
   Up to 100 songs per playlist, 12 playlists.
3. In the game's music screen, open the **YouTube Music** row and pick your playlist, or press
   a song in it.

Each song takes about two seconds to start.

### Songs that get skipped

Private and age-restricted songs only play if YouTube can see you're signed in. If songs are
being skipped:

1. In your browser, signed in to YouTube, install the extension
   **Get cookies.txt LOCALLY**.
2. On youtube.com, use it to export a `cookies.txt`.
3. On the setup page, under **YouTube Music → Songs being skipped?**, choose that file.

ReSkate downloads one more small tool (QuickJS, about 2 MB) the first time you add a sign-in;
there is nothing else to install. With a sign-in set, each song takes about seven seconds to
start instead of two. If you have **Node.js** installed, ReSkate uses that instead and it is
about twice as fast.

Treat `cookies.txt` like a password: it is your YouTube sign-in. It stays on your PC, and the
**Remove cookies.txt** button deletes it. Cookies expire, so if songs start skipping again
weeks later, export a fresh file.

---

## Local files

Plays music saved on your PC: MP3, M4A, AAC, Ogg, FLAC and WAV.

1. Open the folder with your music in File Explorer, click the address bar at the top and copy
   what it shows.
2. On the setup page, under **Local files**, paste it and press **Add**. Or press
   **Or use my Music folder**.
3. In the game's music screen, open the **Local Files** row and pick the folder, or press a
   song in it.

Each folder is a tile with up to 200 songs, including the folders inside it. Song names and
covers come from the files' tags; a file with no tags is named after its file name
(`Artist - Title.mp3` works best). Up to 12 folders.

Added new files to a folder? Press **Rescan** next to it on the setup page.

---

## Online radio

Plays live internet radio stations. Each station you add is its own tile.

1. On the setup page, under **Online radio**, type part of a station's name and press
   **Search**. This searches [radio-browser.info](https://www.radio-browser.info), a free public
   directory.
2. Press **Add** next to the station you want. Up to 24 stations.
3. In the game's music screen, open the **Online Radio** row and pick the station.

The track name updates as the station changes song, when the station sends it. Radio is live,
so pausing leaves the station and playing tunes back in; there is no next or previous.

If you have a stream's address instead, open **Have a stream's address instead?** and paste it.
MP3 and AAC streams work, and `.pls` / `.m3u` links to them. Addresses ending in `.m3u8` (HLS)
are not supported.

---

## External audio

Plays the sound of any app on your PC through the game: a browser, a media player, another
music service.

1. Start the app and get it **playing something**. Only apps making sound are listed.
2. On the setup page, under **External audio**, pick the app and press **Use this app**.
3. In the game's music screen, open the **External Audio** row and pick its tile.

While it plays through the game, the app's own volume is turned right down so you don't hear
it twice. It's put back when you leave the tile or close the game.

Track name, cover, pause and next work for apps that support Windows media controls (the same
ones your keyboard's play/pause key uses). Browsers and most media players do. An app that
doesn't will still be heard through the game, but can't be paused or skipped from it.

---

## Sound settings

In the **Sound** section of the setup page:

- **Loudness:** how loud the music is through the game.
- **Keep every song at the same loudness:** turns quiet songs up and loud ones down.
- **Equaliser:** five sliders, from bass to treble, with a few presets. Up is more, down is
  less. It only changes the music you play through ReSkate, not the game's own.
- **Speakers in the world play Spotify too:** when on, speakers in the world play what you're
  playing, and grabbing music from one keeps you on your own music.

These settings are remembered between game sessions.

---

## Queue

The **Queue** section of the setup page lists the songs of the YouTube Music or Local Files
playlist you are playing, with the current one marked. Press a song to jump to it, or type in
the box to find one. Spotify keeps its own queue, in the Spotify app.

---

## Troubleshooting

| Problem | Try this |
| --- | --- |
| "ReSkate" isn't in Spotify's device list | Make sure the game is running, both are on the same network, and the game was allowed through the firewall. |
| Spotify tile is silent | Type `music` in the console. It shows whether a Spotify account is on the device. If not, pick **ReSkate** in Spotify's device list again. |
| New playlist or row isn't in the music screen | Restart the game. |
| A newly added playlist plays the wrong music | Press a song in it instead of the tile, or restart the game. |
| YouTube songs keep getting skipped | See "Songs that get skipped" above. |
| YouTube stopped working altogether | YouTube changes things often. Close the game, delete `yt-dlp.exe` from `%LOCALAPPDATA%\ReSkate\bin`, start the game and press **Download and set up** again to get the latest version. |
| A local file gets skipped | It is a kind ReSkate can't play (for example Apple Lossless or WMA), or the file has moved. Press **Rescan**. |
| A radio station won't play | Some stations only send HLS, which isn't supported. Try another entry for the same station in the search results. |
| The setup page says the game isn't answering | The game has been closed. The page only works while it's running. |

## Where things are kept

Everything is on your PC, in `%LOCALAPPDATA%\ReSkate`:

| File or folder | What it is |
| --- | --- |
| `spotify-device\` | the remembered Spotify sign-in for the in-game device |
| `spotify.json` | your Client ID and playlist sign-in (encrypted for your Windows account) |
| `youtube-music.json` | your saved YouTube playlists, music folders and radio stations |
| `music-settings.json` | loudness, equaliser and the other sound settings |
| `youtube-cookies.txt` | your YouTube sign-in, if you added one |
| `external-audio.json` | which app External Audio uses |
| `bin\yt-dlp.exe` | the YouTube tool |
| `bin\qjs.exe` | QuickJS, which the YouTube tool needs once a sign-in is set |

Deleting any of these resets that part.
