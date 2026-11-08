# Counter-Strike Nexon: Offline Revived

Counter-Strike Nexon: Zombies played offline, on your own PC, against room bots that play every mode: a local
server (accounts, rooms, shop, inventory, missions, medals), client patches, and `botassist`, which gives the bots
an AI for each mode (Zombie Giant, Zombie Scenario, Human Scenario, Zombie Tag, Soccer, All Star, Item Battle and
more).

**It doesn't include the game.** You need Counter-Strike Nexon: Zombies installed with Steam (it's free). The
launcher patches *your* copy of the game, keeps a backup of every file it changes and can put them back.

## Playing

1. Install Counter-Strike Nexon: Zombies from Steam (the launcher points you there if it isn't installed).
   If Steam doesn't give you the game any more, the same window (also **More > Get the game**) downloads the
   copy of the same Steam version kept on the Internet Archive
   ([csnz-1362486462562801379](https://archive.org/details/csnz-1362486462562801379), Steam depot manifest
   1362486462562801379): 12.9 GB, resumable, checked against archive.org's MD5 and unpacked into the folder you
   choose (about 27 GB free needed while installing). The repository doesn't host the game.
2. Download `CSNZ-Offline-Revived-Launcher.exe` from the latest release and put it in an empty folder of its own
   (the mod is installed next to it, in `mod\`). Don't open it from the browser's downloads bar: the browser runs a new temporary
   copy each time, and the launcher would ask to install everything again (it now offers to move itself to
   `Documents\CSNZ Offline Revived` when that happens).
3. Open it. The repository is public, so it needs nothing else to find the releases.
4. **Install** downloads the mod. Later, the same button says **Update** when a newer release is out (the
   launcher updates itself too).
5. **Play**: the first time Windows asks for administrator rights to add the hosts block (the game's Nexon
   addresses go to 127.0.0.1); then the launcher makes the server's certificate, patches the game, starts the
   local server and the game (which asks for administrator rights itself), and stops the server when the game
   closes.
6. **More > Create a desktop shortcut that starts the game**: a shortcut to the launcher with `--play`, which goes
   straight into the game (the launcher closes with it). From then on the launcher is how you start the game.
7. Item pictures: after the mod is installed (or before the first Play), the launcher asks once whether to extract the
   item pictures, 3D previews, medal icons and mission pictures of the in-game web pages from your game (a few
   minutes). It doesn't ask again: not after an answer, nor when they're already there (updates keep them). The
   **Pictures** button extracts them any time. It runs the mod's Python scripts: without Python on the PC, the launcher
   offers to download the newest Python installer from python.org (checked to be signed by the Python Software
   Foundation) and installs it for your Windows user only, without administrator rights. It then adds the `pefile`
   package with pip.

### Studio maps

Studio (Create, the editor and its game rooms) works offline. Every map has a **share code** (like `ZQAH-Y6Y9`),
shown at the start of its description in the game and on the **Studio maps** page (Shop > Studio maps). With
**Online play** on, a friend types the code in the game's Studio search and gets the map from your PC over Steam
while you're online. **Publish online** on that page lists the map for every player online: the online maps show up
in the game's Studio browser and in the page's Online maps tab. A map someone downloaded can be passed on by their PC
too, and a Studio room on another player's server downloads its map by itself.

### Reporting a problem

1. Turn on **Detailed logs** under the game's folder. On the next Play the local server and the Steam bridge log
   every packet; the files get bigger, so turn it off again afterwards.
2. Play until the problem happens, then close the game.
3. **Logs > Copy the last session's logs** puts everything in the clipboard: the launcher, the Steam bridge, the
   local server and the game (`Bin\Error.log`, `botassist.log`). Paste it in your message, or send
   `mod\logs\last-session.txt`. **Logs > Open the last session's logs** shows the same text in Notepad.

The launcher finds the game in Steam's libraries by itself (Steam's manifest of app 273110). If it isn't installed,
it opens a window with **Install with Steam**, the store page, and the commands for Win+R (`steam://install/273110`
to install it, `steam://run/273110` to start it with Steam); that window closes by itself once Steam has the game.

Under the game's folder the launcher shows its state (original, patched, or not matching this release) and a
**Patch the game** / **Unpatch (original game)** button: unpatching puts Steam's files back from the backups, takes
the mod's files out of `Bin` and keeps your `botassist.ini` for the next patch. If a backup is missing it offers
Steam's *Verify integrity of game files*. **More > Remove the hosts block** is the other half of going back online.

If Steam updates the game, the patches no longer fit: the launcher says so and doesn't touch anything until a
release for the new version is out.

## The repository

| Path | What |
|---|---|
| `launcher/` | The launcher (C#, .NET Framework 4.8, no install needed on Windows 10/11). |
| `mod/server/` | The local server, based on [JusicP/CSNZ_Server](https://github.com/JusicP/CSNZ_Server) (GPL-3.0); our changes are `mod/server/patches/`. |
| `mod/tools/apply_patches.py` | The client patches (each one checks the original bytes first). |
| `mod/tools/botassist/` | The bots' AI, loaded by the game as `botassist.asi`; one file per mode in `modes/`. Its README has the reverse-engineering notes. |
| `mod/web/` | The in-game web pages (shop, profile, missions, AI player editor). |
| `mod/tools/release/build_release.py` | Builds a release into `dist/`. |

## Authors

- **ItsYuri** ([ItsYuriil](https://github.com/ItsYuriil)): the offline port and rework: bot AI, game modes, items,
  shop, missions, gameplay, new features and quality of life. Discord: https://discord.gg/fjxubwyhYv.
  Enjoying it? Support the development on Patreon: https://patreon.com/ltsYuri
- The local server is based on [JusicP/CSNZ_Server](https://github.com/JusicP/CSNZ_Server) by Jusic, Hardee,
  NekoMeow, Smilex_Gamer and xRiseless.

## Making a release

1. Build what Git doesn't keep: `botassist.asi` (`mod/tools/botassist/README.md`), `CSNZ_Server.exe`
   (`mod/server/README.md`) and `nartool.exe`.
2. Raise `mod/VERSION`, commit, then `python mod/tools/release/build_release.py`.
3. `gh release create v<version> dist/CSNZ-Offline-Revived-<version>.zip dist/CSNZ-Offline-Revived-Launcher.exe`.

The launcher compares the release's tag with `mod\VERSION` (and with its own version) to offer the update.
