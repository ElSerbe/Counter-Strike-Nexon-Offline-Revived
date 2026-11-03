"""Item Battle (mode 19) on more maps: writes tables/maps/FUN_ITEMBOX_<map>.csv (where its item boxes are, the table the game only has
for 6 maps) from each map's navigation mesh, and sets MapList.csv's tdm_item column (the mode's map list) on those maps.

Boxes go on open areas of the map's main connected part, spread as far from each other as possible: wide enough for players to pass each
other, not dead ends (bots piled up in a 50 unit wide alley), and not over or under another area when the map has enough others. Row: item id (0 a random item,
1 Power AT4CS, 2 Power Ripper, both behind a yellow barricade), seconds to come back, x, y, z. A map has 2 special boxes near its center.

Usage: python gen_item_battle.py <game root> <work dir>"""
import csv
import math
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.join(HERE, "..", "..")
MAP_LIST = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data", "MapList.csv")
NARTOOL = os.path.join(MOD, "tools", "nartool", "nartool.exe")
OUT = os.path.join(HERE, "tables", "maps")
GAME_TABLES = {"cs_assault", "cs_estate", "cs_italy", "de_dust", "de_dust2", "fun_checkmate"}  # the game's own
MIN_BOXES, MAX_BOXES, BOX_ABOVE_FLOOR, AREA_PER_BOX, MIN_WIDTH, MIN_WAYS = 14, 40, 36.0, 400000.0, 96, 2


def load_nav(path):
    b = open(path, "rb").read()
    o = 0

    def g(f):
        nonlocal o
        v = struct.unpack_from("<" + f, b, o)
        o += struct.calcsize("<" + f)
        return v[0] if len(v) == 1 else v

    assert g("I") == 0xFEEDFACE
    ver = g("I")
    if ver >= 4:
        g("I")
    if ver >= 5:
        for _ in range(g("H")):
            n = g("H")
            o += n
    areas = []
    for _ in range(g("I")):
        aid = g("I")
        g("B")
        lo, hi, ne, sw = g("fff"), g("fff"), g("f"), g("f")
        adj = []
        for _d in range(4):
            adj += [g("I") for _ in range(g("I"))]
        n = g("B")
        o += n * 17
        n = g("B")
        o += n * 14
        for _ in range(g("I")):
            o += 10
            n = g("B")
            o += n * 5
        if ver >= 5:
            g("H")
        areas.append(dict(id=aid, lo=lo, hi=hi, z=(lo[2] + hi[2] + ne + sw) / 4, adj=adj))
    return areas


def main_part(areas):
    """the biggest connected part of the mesh: the rest can't be walked to"""
    by_id = {a["id"]: a for a in areas}
    seen, best = set(), []
    for a in areas:
        if a["id"] in seen:
            continue
        part, todo = [], [a["id"]]
        seen.add(a["id"])
        while todo:
            cur = by_id[todo.pop()]
            part.append(cur)
            for n in cur["adj"]:
                if n in by_id and n not in seen:
                    seen.add(n)
                    todo.append(n)
        if len(part) > len(best):
            best = part
    return best


def place(name, areas, rng):
    part = main_part(areas)
    def spots(width, ways, levels):
        out = []
        for a in part:
            w, h = a["hi"][0] - a["lo"][0], a["hi"][1] - a["lo"][1]
            if w < width or h < width or abs(a["hi"][2] - a["lo"][2]) >= 12 or len(set(a["adj"])) < ways:
                continue
            c = ((a["lo"][0] + a["hi"][0]) / 2, (a["lo"][1] + a["hi"][1]) / 2, a["z"])
            if not levels and any(o is not a and o["lo"][0] <= c[0] <= o["hi"][0] and o["lo"][1] <= c[1] <= o["hi"][1]
                                  and abs(o["z"] - c[2]) > 16 for o in part):
                continue
            out.append(c)
        return out

    # the strict rules first; a map without enough such places (small, or a mesh of tiny areas) relaxes them
    cand = []
    for width, ways, levels in ((MIN_WIDTH, MIN_WAYS, False), (MIN_WIDTH, MIN_WAYS, True), (64, MIN_WAYS, True), (40, 1, True)):
        cand = spots(width, ways, levels)
        if len(cand) >= MIN_BOXES:
            break
    else:
        return None
    total = sum((a["hi"][0] - a["lo"][0]) * (a["hi"][1] - a["lo"][1]) for a in part)
    count = max(MIN_BOXES, min(MAX_BOXES, int(total / AREA_PER_BOX)))
    cx, cy = sum(c[0] for c in cand) / len(cand), sum(c[1] for c in cand) / len(cand)
    # farthest first: each new box goes where the closest box is farthest
    points = [min(cand, key=lambda c: math.hypot(c[0] - cx, c[1] - cy))]
    while len(points) < min(count, len(cand)):
        points.append(max(cand, key=lambda c: min(math.dist(c, p) for p in points)))
    # the 2 special ones: the sampled spots closest to the center
    order = sorted(points, key=lambda p: math.hypot(p[0] - cx, p[1] - cy))
    special = {order[0]: 1, order[1]: 2}
    rows = []
    for p in points:
        kind = special.get(p, 0)
        back = 300 if kind else rng.choice((30, 30, 60))
        rows.append((kind, back, round(p[0]), round(p[1]), round(p[2] + BOX_ABOVE_FLOOR)))
    return rows


def main():
    root, work = sys.argv[1], sys.argv[2]
    bin_dir = os.path.join(root, "Bin")
    os.makedirs(OUT, exist_ok=True)
    raw = open(MAP_LIST, encoding="latin1", newline="").read()
    rows = list(csv.reader(raw.splitlines()))
    head = [c.strip() for c in rows[0]]
    assert head[-1] == "tdm_item"
    wanted = {l.strip().lower() for l in open(os.path.join(HERE, "gen_item_battle_maps.txt")) if l.strip() and l[0] != ";"}
    maps, taken = {}, set()
    for r in rows[1:]:
        # a map can have several rows (events, variants): its first one
        name = r[2].strip() if len(r) > 2 and r[0].isdigit() else ""
        if name.lower() in wanted and name.lower() not in taken:
            taken.add(name.lower())
            maps[r[0]] = name
    lst = os.path.join(work, "navs.lst")
    with open(lst, "w") as f:
        f.write("".join("maps/%s.nav\n" % m for m in maps.values()))
    subprocess.run([NARTOOL, bin_dir, root, work, "@" + lst], check=False, stdout=subprocess.DEVNULL)
    good = {}
    for mid, name in maps.items():
        nav = os.path.join(work, "maps", name + ".nav")
        if not os.path.exists(nav):
            continue
        if name in GAME_TABLES:
            good[mid] = name
            continue
        rng = random.Random(name)
        placed = place(name, load_nav(nav), rng)
        if not placed:
            print("too small, left out:", name)
            continue
        with open(os.path.join(OUT, "FUN_ITEMBOX_%s.csv" % name), "w", newline="") as f:
            f.write("item,back,x,y,z\r\n")
            for row in placed:
                f.write("%d,%d,%d,%d,%d\r\n" % row)
        good[mid] = name
    # the mode's map list: the column keeps the maps it had first
    lines = raw.split("\r\n")
    order = 3 + 1
    for i, line in enumerate(lines):
        mid = line.split(",", 1)[0]
        if mid not in good:
            continue
        keep, last = line.rsplit(",", 1)
        if last.strip() in ("", "0"):
            lines[i] = "%s,%d" % (keep, order)
            order += 1
    open(MAP_LIST, "w", encoding="latin1", newline="").write("\r\n".join(lines))
    print(len(good), "maps in the mode's list")


if __name__ == "__main__":
    main()
