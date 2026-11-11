"""Writes the contents of every decoder of the game into CSNZ_Server's ItemBox.json.

The game has 44 decoders (Item.csv class LotteryKeyItem). The server opens a decoder from its ItemBox.json entry and sends
every entry to the game (metadata ItemBox), whose Decoder window lists the decoders with a 3D model (Item.csv "mdl",
basicmatrix: 21 of them) and their contents; the older ones (Code Decoders...) are opened from the inventory.

Contents come from Item.csv: weapons by ItemGrade (1 normal ... 5 transcendence, 6 above it; 8 = special items, left out),
characters (Class) by ItemGrade, costumes, zombie skins and weapon paints. Only items with an English name in the game's
text (resource/cso_na_en.txt) and that can be in the inventory (InGameItem 1) go in. Each decoder has a main pool and a
second one: a small chance of a permanent item from each, then 30-day (or 10-day) and short (7 or 3 days) items from both.
The Transcendence Decoder (8292) keeps its hand-made entry.

ItemBox.json format (luckyitemmanager.cpp): "<decoder>": {"Rate": {"<percent>": {"Grade": 1, "Duration": [days, 0 =
permanent], "Items": [...]}}}; a group is drawn by its percent, then an item and a duration from it at random. The
percents of one decoder are object keys: they must differ.

Usage: python gen_decoders.py            (needs nartool for the game's names)
"""
import json
import os
import random
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_shop_assets as esa  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEMBOX = os.path.join(ROOT, "server", "CSNZ_Server", "bin", "ItemBox.json")
KEEP = [8292]  # hand-made entries kept as they are
WEAPON_CATEGORIES = {"1", "2", "3", "4", "5", "6", "11"}
COSTUMES = {"HeadCostume", "BackCostume", "ArmCostume", "PelvisCostume", "PetCostume"}


def load():
    items = esa.load_items()
    out = tempfile.mkdtemp(prefix="gen_decoders_")
    esa.run_nartool(["resource/cso_na_en.txt"], out)
    names = esa.load_names(os.path.join(out, "resource", "cso_na_en.txt"))

    def name(i):
        res = items[i]["resource"]
        return names.get(("csow_" + res).lower()) or names.get(("cso_item_name_" + res).lower())

    return items, name


def pools(items, name):
    p = {k: [] for k in ("w1", "w2", "w3", "w4", "w5", "w6", "c1", "c2", "c3", "c4", "c5", "cost", "zskin", "paint")}
    for i, it in items.items():
        row = it["row"]
        if not name(i) or it["resource"].startswith("vxl"):
            continue
        if it["class"] == "Equipment" and row.get("InGameItem") != "1":
            continue
        grade = row.get("ItemGrade", "0").strip()
        cls = it["class"]
        if cls == "Equipment" and it["category"] in WEAPON_CATEGORIES and grade in "123456" and grade:
            p["w" + grade].append(i)
        elif cls == "Class" and grade in "12345" and grade:
            p["c" + grade].append(i)
        elif cls in COSTUMES:
            p["cost"].append(i)
        elif cls == "ZombieSkinCostume":
            p["zskin"].append(i)
        elif cls == "WeaponPaintItem":
            p["paint"].append(i)
    for k in p:
        p[k].sort()
    # the game's grades: 1 Normal, 2 Advanced, 3 Rare, 4 Unique, 5 Transcendence, 6 Epic (the red ones)
    p["epicg"] = p["w6"]
    p["top"] = p["w5"]
    p["epic"] = p["w4"]  # (Unique grade; "epic" here only means the tier under Transcendence)
    p["uniq"] = p["w3"]
    p["rare"] = p["w2"]
    p["norm"] = p["w1"]
    p["chTop"] = p["c5"]
    p["chMid"] = p["c3"] + p["c4"]
    p["chLow"] = p["c1"] + p["c2"]
    return p


# decoder: (main pools, second pools, % permanent from main, % permanent from second, long days, % long, short days)
TOP, EPIC, UNIQ, RARE = ("top",), ("epic",), ("uniq",), ("rare",)
SPECS = {
    165: ("Decoder", ("rare", "uniq"), ("norm",), 0.5, 2.0, 7, 25, 3),
    173: ("Code Decoder", EPIC, UNIQ, 0.5, 1.5, 30, 28, 10),
    396: ("Code A Decoder", TOP, EPIC, 0.5, 1.5, 30, 28, 10),
    533: ("Code B Decoder", UNIQ, RARE, 1.0, 3.0, 30, 26, 7),
    552: ("Event Code A Decoder", TOP, EPIC, 0.5, 1.5, 30, 28, 10),
    555: ("Event Code Decoder", EPIC, UNIQ, 0.5, 1.5, 30, 28, 10),
    576: ("Event Code B Decoder", UNIQ, RARE, 1.0, 3.0, 30, 26, 7),
    774: ("Mileage Decoder", UNIQ, RARE, 1.0, 3.0, 30, 26, 7),
    803: ("Event Mileage Decoder", UNIQ, RARE, 1.0, 3.0, 30, 26, 7),
    806: ("Supreme Decoder", TOP, EPIC, 0.75, 2.0, 30, 27, 7),
    871: ("Transcendence Decoder (old)", TOP, EPIC, 0.75, 2.0, 30, 27, 7),
    872: ("GP Mileage Decoder", UNIQ, RARE, 1.0, 3.0, 30, 26, 7),
    886: ("Transcendence Decoder 2 (old)", TOP, EPIC, 0.75, 2.0, 30, 27, 7),
    955: ("Premium Decoder C", RARE, ("norm",), 1.5, 4.0, 30, 25, 7),
    956: ("Premium Decoder B", UNIQ, RARE, 1.25, 3.5, 30, 25, 7),
    957: ("Premium Decoder A", EPIC, UNIQ, 1.0, 3.0, 30, 26, 7),
    958: ("Premium Decoder S", TOP, EPIC, 0.75, 2.5, 30, 27, 7),
    975: ("Premium Decoder SB", EPIC, UNIQ, 1.5, 4.0, 30, 25, 7),
    976: ("Premium Decoder SA", TOP, EPIC, 1.25, 3.5, 30, 25, 7),
    985: ("Premium Class Decoder", ("chMid",), ("chLow",), 1.0, 3.0, 30, 26, 7),
    2198: ("Epic Decoder", ("epicg",), TOP, 0.5, 1.5, 30, 28, 10),
    2599: ("Epic Decoder", ("epicg",), TOP, 0.5, 1.5, 30, 28, 10),
    8097: ("Premium Decoder E", EPIC, UNIQ, 1.0, 3.0, 30, 26, 7),
    8261: ("Premium Class Decoder S", ("chTop",), ("chMid",), 1.0, 3.0, 30, 26, 7),
    8293: ("Transcendence Decoder P", TOP, EPIC, 1.0, 3.0, 30, 30, 10),
    8308: ("Epic Grade Decoder", EPIC, UNIQ, 1.0, 3.0, 30, 28, 10),
    8309: ("Epic Grade Decoder P", EPIC, UNIQ, 2.0, 4.0, 30, 30, 10),
    8310: ("Unique Grade Decoder", UNIQ, RARE, 1.5, 4.0, 30, 26, 7),
    8311: ("Class Decoder", ("chTop", "chMid"), ("chLow",), 1.0, 3.0, 30, 26, 7),
    8312: ("Class Decoder P", ("chTop", "chMid"), ("chLow",), 2.0, 4.0, 30, 28, 10),
    8453: ("Event Decoder", ("cost", "zskin"), UNIQ, 1.5, 2.0, 30, 26, 7),
    8616: ("Transcendence Decoder M", TOP, EPIC, 0.5, 1.5, 30, 28, 10),
    8617: ("Class Decoder M", ("chTop", "chMid"), ("chLow",), 1.0, 3.0, 30, 26, 7),
    8898: ("Transcendence Decoder E1", TOP, EPIC, 0.5, 1.5, 30, 28, 10),
    9053: ("All-in-one Transcendence Decoder", ("top", "chTop"), ("epic", "chMid"), 0.75, 2.0, 30, 28, 10),
    9084: ("Paint Decoder", ("paint",), ("paint",), 100.0, 0.0, 0, 0, 0),
    9116: ("Transcendence Decoder E2", TOP, EPIC, 0.5, 1.5, 30, 28, 10),
    9144: ("Black Friday Decoder", ("top", "epic"), UNIQ, 1.0, 3.0, 30, 28, 10),
    9184: ("All-in-one Decoder", ("top", "chTop"), ("epic", "chMid"), 0.75, 2.0, 30, 28, 10),
    9330: ("Code Decoder ZHC", TOP, EPIC, 0.5, 1.5, 30, 28, 10),
    9633: ("Snow Flower Decoder", ("cost", "epic"), UNIQ, 1.0, 3.0, 30, 26, 7),
    9706: ("Legend Decoder", ("epicg", "top"), ("chTop",), 3.0, 5.0, 30, 30, 10),
    9707: ("All Star Decoder", ("epic", "chMid"), UNIQ, 1.0, 3.0, 30, 26, 7),
}


# The game gets every decoder's list in one packet (64 KB, 26 bytes an item): each decoder holds at most MAIN_ITEMS of its
# main pool and SECOND_ITEMS of its second one, a different pick per decoder (seeded by its id), so the Decoder window
# shows exactly what can come out
MAIN_ITEMS, SECOND_ITEMS = 24, 16


def pick(ids, n, seed):
    ids = sorted(ids)
    if len(ids) <= n:
        return ids
    return sorted(random.Random(seed).sample(ids, n))


def groups(spec, p, seed):
    _, main, second, permMain, permSecond, longDays, longRate, shortDays = spec
    mainPool = {i for k in main for i in p[k]}
    secondPool = {i for k in second for i in p[k]} - mainPool
    a = pick(mainPool, MAIN_ITEMS if secondPool else MAIN_ITEMS + SECOND_ITEMS, seed)  # (one pool: all 40 from it)
    b = pick(secondPool, SECOND_ITEMS, seed * 7 + 1)
    both = sorted(set(a) | set(b))
    out = []
    if permMain:
        out.append([permMain, 0, a])
    if permSecond and b:
        out.append([permSecond, 0, b])
    if longRate:
        out.append([longRate, longDays, both])
    rest = round(100.0 - sum(g[0] for g in out), 4)
    if rest > 0:
        out.append([rest, shortDays, both])
    # the percents are object keys: make them differ
    seen = set()
    for g in out:
        while round(g[0], 4) in seen:
            g[0] = round(g[0] - 0.01, 4)
        seen.add(round(g[0], 4))
    return [g for g in out if g[2]]


def fmt(rate):
    return ("%.4f" % rate).rstrip("0").rstrip(".")


def block_of(text, key):
    """the raw text of "key": {...} in text (comments kept)"""
    m = re.search(r'\n\t"%s": \{' % key, text)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start() + 1:i + 1]
        i += 1
    return None


def main():
    items, name = load()
    p = pools(items, name)
    print("pools:", {k: len(v) for k, v in p.items()})
    old = open(ITEMBOX, encoding="utf-8").read()
    parts = []
    for key in KEEP:
        b = block_of(old, key)
        if b:
            parts.append(b)
    decoders = sorted(i for i, it in items.items() if it["class"] == "LotteryKeyItem")
    missing = [i for i in decoders if i not in SPECS and i not in KEEP]
    if missing:
        print("decoders without a recipe (left out):", missing)
    for i in decoders:
        if i in KEEP or i not in SPECS:
            continue
        spec = SPECS[i]
        lines = ['\t"%d": {' % i, "\t\t// %s (%s): generated by tools/gen_decoders.py" % (name(i) or spec[0], items[i]["resource"]),
                 '\t\t"Rate": {']
        rates = []
        for rate, days, ids in groups(spec, p, i):
            rates.append('\t\t\t"%s": {\r\n\t\t\t\t"Grade": 1,\r\n\t\t\t\t"Duration": [%d],\r\n\t\t\t\t"Items": [%s]\r\n\t\t\t}'
                         % (fmt(rate), days, ", ".join(str(x) for x in ids)))
        lines.append(",\r\n".join(rates))
        lines.append("\t\t}")
        lines.append("\t}")
        parts.append("\r\n".join(lines))
        print("%5d %-34s %s" % (i, (name(i) or spec[0])[:34], " | ".join("%s%% %s %d" % (fmt(r), "perm" if d == 0 else "%dd" % d, len(x))
                                                                      for r, d, x in groups(spec, p, i))))
    text = '{\r\n\t"Version": 1,\r\n' + ",\r\n".join(parts) + "\r\n}\r\n"
    json.loads(re.sub(r"//[^\r\n]*", "", text))  # still valid
    open(ITEMBOX, "w", encoding="utf-8", newline="").write(text)
    print("ItemBox.json: %d decoders" % (len(parts)))


# The Decoder window lists only decoders with a 3D model (Item.csv "mdl"). The server sends its Data/Item.csv to the game
# (metadata Item), so the decoders without one (the Code Decoders, Mileage, Premium... 23 of 44) get the Transcendence
# Decoder's there: basicmatrix.
SERVER_ITEMS = os.path.join(ROOT, "server", "CSNZ_Server", "bin", "Data", "Item.csv")


def give_models():
    data = open(SERVER_ITEMS, "rb").read()
    lines = data.split(b"\n")
    header = next(l for l in lines if l.startswith(b";ID")).rstrip(b"\r").split(b",")
    mdl, cls = header.index(b"mdl"), header.index(b"ClassName")
    changed = 0
    for n, line in enumerate(lines):
        if not line[:1].isdigit() or b'"' in line:
            continue
        cr = line.endswith(b"\r")
        f = line.rstrip(b"\r").split(b",")
        if len(f) > mdl and f[cls] == b"LotteryKeyItem" and not f[mdl]:
            f[mdl] = b"basicmatrix"
            lines[n] = b",".join(f) + (b"\r" if cr else b"")
            changed += 1
    if changed:
        open(SERVER_ITEMS, "wb").write(b"\n".join(lines))
    print("Item.csv: %d decoders got the decoder model" % changed)


if __name__ == "__main__":
    main()
    give_models()
