# ReSkate

Steam sign-in and depot download code has been removed from this fork for privacy reasons. Installed-game checks remain.

After updating source, run `./patches/Apply-SteamPrivacyPatch.ps1 -SourcePath <ReSkate-source-folder>`, then rebuild. The script stops if the patch no longer matches.

## Download the supported game build manually

1. Make sure Steam is running and you're signed in to an account with authorized access to **skate.** in its library.
2. Press **Win + R**, paste `steam://open/console`, and press Enter.
3. In Steam's **Console** tab, paste this in the box at the bottom and press Enter:

   ```text
   download_depot 3354750 3354751 4621099302092747785
   ```

Steam handles the download through your account. Availability of this older build depends on Steam and the publisher.
