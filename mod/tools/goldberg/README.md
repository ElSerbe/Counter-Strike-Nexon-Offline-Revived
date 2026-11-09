# Goldberg Steam Emulator's steam_api.dll

`steam_api.dll` here is a build of the **Goldberg Steam Emulator** (32-bit, built 2023-05-09; sha256
`eaeaf26f60229ae4d1a9aae07d03ab4e93ea599dd883eb810d6adebcf0e52e14`). It's free software under the GNU LGPL v3
(`LICENSE-LGPL-3.0.txt`, which builds on the GNU GPL v3 in `LICENSE-GPL-3.0.txt`). Its source code:
https://gitlab.com/Mr_Goldberg/goldberg_emulator. It isn't part of this project's own code and isn't modified.

**Why it's here.** Counter-Strike Nexon from the archive.org copy (launcher: *Get the game*) closes by itself at
startup when the Steam account doesn't have the game: the game asks Steam for its app and Steam refuses. The launcher
puts this DLL in the game's `Bin` instead of Valve's, only for a game that isn't a Steam library install, so the
game starts without asking Steam. Valve's original is kept next to the Steam bridge
(`mod\tools\steambridge\out\steam_api.dll`): online play always goes through the real Steam with it.
