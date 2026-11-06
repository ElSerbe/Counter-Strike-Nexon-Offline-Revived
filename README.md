# Counter-Strike Nexon: Offline Revived

Counter-Strike Nexon: Zombies played offline, on your own PC, against room bots that play every mode: a local
server (accounts, rooms, shop, inventory, missions, medals), client patches, and `botassist`, which gives the bots
an AI for each mode (Zombie Giant, Zombie Scenario, Human Scenario, Zombie Tag, Soccer, All Star, Item Battle and
more).

**It doesn't include the game.** You need Counter-Strike Nexon: Zombies installed with Steam (it's free). The
launcher patches *your* copy of the game, keeps a backup of every file it changes and can put them back.

## Playing

1. Install Counter-Strike Nexon: Zombies from Steam.
2. Download `CSNZ-Offline-Revived-Launcher.exe` from the latest release and put it in an empty folder of its own
   (the mod is installed next to it, in `mod\`).
3. Open it. The repository is private, so the launcher needs to read it: it uses, in this order, a token saved in
   it (**More > GitHub token...**, a fine-grained token with *Contents: read* on this repository), `GH_TOKEN` /
   `GITHUB_TOKEN`, or the GitHub CLI (`gh auth login`) if it's installed.
4. **Install** downloads the mod. Later, the same button says **Update** when a newer release is out (the
   launcher updates itself too).
5. **Play**: the first time Windows asks for administrator rights to add the hosts block (the game's Nexon
   addresses go to 127.0.0.1); then the launcher makes the server's certificate, patches the game, starts the
   local server and the game (which asks for administrator rights itself), and stops the server when the game
   closes.
6. Optional: **More > Export shop and profile pictures** makes the item pictures, 3D previews and medal icons of
   the in-game web pages from your game. It needs Python 3 (the launcher installs the Pillow and pefile packages).

**More** also has *Restore the original game* (Steam's files back, the mod's out of `Bin`) and *Remove the hosts
block* (to play online again).

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

## Making a release

1. Build what Git doesn't keep: `botassist.asi` (`mod/tools/botassist/README.md`), `CSNZ_Server.exe`
   (`mod/server/README.md`) and `nartool.exe`.
2. Raise `mod/VERSION`, commit, then `python mod/tools/release/build_release.py`.
3. `gh release create v<version> dist/CSNZ-Offline-Revived-<version>.zip dist/CSNZ-Offline-Revived-Launcher.exe`.

The launcher compares the release's tag with `mod\VERSION` (and with its own version) to offer the update.
