"""Writes tables/maps/ZBS_WAVE_zsrift_splinter.csv: Zombie Disaster Zone's waves with the Zombie Scenario zombie types.

The map's own wave table (maps/zbs_wave_zsrift_splinter.cso, read through nartool and decrypted with csodec) spawns
only zombie types 1 and 2 (zombi_host / zombi_origin) in every round, though its zombie type table
(maps/zbs_zt_zsrift_splinter.cso) also lists the scenario's special zombies. This keeps every spawn point and timing
and gives each wave a mix that grows round by round. botassist loads the result instead of the game's table
(see "table overrides" in botassist.cpp).

Zombie types (zbs_zt_zsrift_splinter): 1 zombi_host, 2 zombi_origin, 3 pass_zombi_origin (ranged), 4 pc_zombi_origin,
5 deimos, 6 boomer, 7 heal, 8 revival (400 HP), 9 zombieclimber, 10 speed, 11 heavy, 12 golden speed (1000 HP).
Rounds use waves 1..5 (zbs_zsrift_splinter: waveTypes = round), 6..8 are unused spares; 3001 is the boss's escort.

Usage: python gen_zsrift_waves.py   (game in D:/SteamLibrary/steamapps/common/CSNZ, or pass its folder)
"""
import subprocess
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import csodec  # noqa: E402

TABLE = "maps/zbs_wave_zsrift_splinter"
OUT = HERE / "tables" / "maps" / "ZBS_WAVE_zsrift_splinter.csv"

# Common zombies (types 1 and 2, one each) come out of every spawn row; a special zombie is the exception: only one row
# in SPECIAL_EVERY[wave] of each wave adds one, its type taken in turn from SPECIALS[wave] (a boomer at most once in
# each list). zombiRatio stays 1 per type, as in the game's own tables, so this holds whether the game reads it as
# weights or counts: about 5% of a wave is special in round 1, 14% in round 5. The round's random events
# (gen_zsrift_events.py) can still bring more of them.
NORMAL = [1, 2]
SPECIALS = {
    1: [10, 4],
    2: [10, 4, 7],
    3: [10, 11, 7, 6],
    4: [10, 11, 4, 5, 9, 3, 6],
    5: [10, 11, 5, 8, 3, 12, 6],
    3001: [10, 11, 5, 6],
}
SPECIAL_EVERY = {1: 10, 2: 8, 3: 6, 4: 4, 5: 3, 3001: 5}
for spare in (6, 7, 8):
    SPECIALS[spare], SPECIAL_EVERY[spare] = SPECIALS[5], SPECIAL_EVERY[5]


def read_original(game: Path) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(HERE.parent / "nartool" / "nartool.exe"), str(game / "Bin"), str(game), tmp, TABLE + ".cso"],
                       check=True, capture_output=True)
        data = (Path(tmp) / (TABLE + ".cso")).read_bytes()
    return csodec.decrypt(data).rstrip(b"\0").decode("ascii")


def main() -> None:
    game = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CSNZ_GAME") or "D:/SteamLibrary/steamapps/common/CSNZ")
    lines = read_original(game).splitlines()
    head = lines[0].split(",")
    wave, zombi, ratio = head.index("waveID"), head.index("zombi"), head.index("zombiRatio")
    out = [lines[0]]
    seen = {}
    for line in lines[1:]:
        row = line.split(",")
        w = int(row[wave])
        # the boss's own row (one type, the boss) stays as it is
        if w in SPECIALS and row[zombi].split() != ["30800"]:
            k = seen[w] = seen.get(w, -1) + 1
            mix = list(NORMAL)
            if k % SPECIAL_EVERY[w] == SPECIAL_EVERY[w] - 1:
                mix.append(SPECIALS[w][(k // SPECIAL_EVERY[w]) % len(SPECIALS[w])])
            row[zombi] = " ".join(str(t) for t in mix)
            row[ratio] = " ".join("1" for _ in mix)
        out.append(",".join(row))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(("\r\n".join(out) + "\r\n").encode("ascii"))  # the game's tables use CRLF
    print(f"{OUT}: {len(out) - 1} spawn rows")


if __name__ == "__main__":
    main()
