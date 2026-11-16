# Goldberg Steam Emulator's steam_api.dll

The **Goldberg Steam Emulator** isn't in this repository or in the release zip. When a game needs it, the launcher
downloads its author's release from GitLab,
https://gitlab.com/Mr_Goldberg/goldberg_emulator/uploads/2524331e488ec6399c396cf48bbe9903/Goldberg_Lan_Steam_Emu_v0.2.5.zip
(v0.2.5, 5 MB). It takes the 32-bit `steam_api.dll` at the zip's root (built 2019-08-12; sha256
`d45e99b9f588670a9cf8316243e98d64bce96303915ec3fa0dc2bfdb42099a45`; any other file is refused) and keeps it here.
It's free software under the GNU LGPL v3 (`LICENSE-LGPL-3.0.txt`, which builds on the GNU GPL v3 in
`LICENSE-GPL-3.0.txt`). Its source code: https://gitlab.com/Mr_Goldberg/goldberg_emulator. It isn't part of this
project's own code and isn't modified.

**Why it's used.** Counter-Strike Nexon from the archive.org copy (launcher: *Get the game*) closes by itself at
startup when the Steam account doesn't have the game: the game asks Steam for its app and Steam refuses. On Play, the
launcher puts this DLL in the game's `Bin` instead of Valve's, only for a game that isn't a Steam library install, so
the game starts without asking Steam. Valve's original is kept next to the Steam bridge
(`mod\tools\steambridge\out\steam_api.dll`): online play always goes through the real Steam with it.

A game that already has the Goldberg build the mod shipped in v1.1.12 to v1.1.15 (2023-05-09) keeps it.
`CSNZ-Offline-Revived-Launcher.exe --check --goldberg` tests the download (result in `launcher-check.txt`).
