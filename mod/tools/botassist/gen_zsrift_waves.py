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
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import csodec  # noqa: E402

TABLE = "maps/zbs_wave_zsrift_splinter"
OUT = HERE / "tables" / "maps" / "ZBS_WAVE_zsrift_splinter.csv"

# wave id -> {zombie type: weight}; the weights are the zombiRatio column (one byte each)
NORMAL = {1: 4, 2: 4}
MIXES = {
    1: {**NORMAL, 10: 1, 4: 1},
    2: {1: 3, 2: 3, 10: 2, 4: 1, 7: 1},
    3: {1: 3, 2: 3, 10: 2, 11: 2, 6: 1, 7: 1},
    4: {1: 2, 2: 2, 10: 2, 11: 2, 5: 1, 6: 1, 9: 1, 3: 1},
    5: {1: 2, 2: 2, 10: 2, 11: 2, 5: 2, 6: 1, 8: 1, 3: 1, 12: 1},
    3001: {1: 2, 2: 2, 10: 2, 11: 2, 5: 1, 6: 1},
}
for spare in (6, 7, 8):
    MIXES[spare] = MIXES[5]


def read_original(game: Path) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(HERE.parent / "nartool" / "nartool.exe"), str(game / "Bin"), str(game), tmp, TABLE + ".cso"],
                       check=True, capture_output=True)
        data = (Path(tmp) / (TABLE + ".cso")).read_bytes()
    return csodec.decrypt(data).rstrip(b"\0").decode("ascii")


def main() -> None:
    game = Path(sys.argv[1] if len(sys.argv) > 1 else "D:/SteamLibrary/steamapps/common/CSNZ")
    lines = read_original(game).splitlines()
    head = lines[0].split(",")
    wave, zombi, ratio = head.index("waveID"), head.index("zombi"), head.index("zombiRatio")
    out = [lines[0]]
    for line in lines[1:]:
        row = line.split(",")
        mix = MIXES.get(int(row[wave]))
        # the boss's own row (one type, the boss) stays as it is
        if mix and row[zombi].split() != ["30800"]:
            row[zombi] = " ".join(str(t) for t in mix)
            row[ratio] = " ".join(str(w) for w in mix.values())
        out.append(",".join(row))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out) + "\n", encoding="ascii", newline="\n")
    print(f"{OUT}: {len(out) - 1} spawn rows")


if __name__ == "__main__":
    main()
