"""Exports what the room's AI player editor (web/bots) needs from the game files: the weapons and characters an
AI player can use, with English names and icons.

Writes web/bots/catalog.json and the icons it lacks into web/shop/icons/<itemID>.png (shared with the shop).
  primary / secondary: botassist_weapons.txt (what bots can buy; category 1 = pistol)
  melee: Item.csv category 11 (knives, axes...)
  characters: botassist_characters.txt (class items with their team)

Usage: python export_bot_catalog.py
"""
import json
import os
import tempfile

import export_shop_assets as shop

BOTASSIST = os.path.join(shop.MOD, "tools", "botassist")
OUT = os.path.join(shop.MOD, "web", "bots", "catalog.json")
CATEGORY_NAMES = {1: "Pistol", 2: "Shotgun", 3: "SMG", 4: "Rifle", 5: "Machine gun", 11: "Melee", 7: "Character"}


def read_list(name):
    """'<id> <number> ; comment' lines -> [(id, number)]"""
    out = []
    for line in open(os.path.join(BOTASSIST, name), encoding="latin1"):
        if line.startswith(";"):
            continue
        parts = line.split(";")[0].split()
        if len(parts) >= 2 and parts[0].isdigit():
            out.append((int(parts[0]), int(parts[1])))
    return out


def main():
    items = shop.load_items()
    weapons = read_list("botassist_weapons.txt")
    characters = read_list("botassist_characters.txt")
    melee = [i for i, it in items.items() if it["category"] == "11" and it["class"] in ("Equipment", "PeriodicItem")]

    entries = {}
    for item_id, cat in weapons:
        entries[item_id] = {"slot": "secondary" if cat == 1 else "primary", "category": cat}
    for item_id in melee:
        entries[item_id] = {"slot": "melee", "category": 11}
    for item_id, team in characters:
        entries[item_id] = {"slot": "character", "category": 7, "team": team}

    available = shop.nar_files()
    candidates = {}
    for item_id in entries:
        res = items.get(item_id, {}).get("resource")
        if not res:
            continue
        cands = [f"gfx/vgui/basket/{res}.tga"]
        for cut in range(len(res) - 1, 3, -1):
            path = f"gfx/vgui/basket/{res[:cut]}.tga"
            if path in available:
                cands.append(path)
                break
        candidates[item_id] = cands

    os.makedirs(shop.ICON_DIR, exist_ok=True)
    made = 0
    with tempfile.TemporaryDirectory() as tmp:
        wanted = sorted({c for cands in candidates.values() for c in cands})
        # one nartool call per chunk keeps the command line short
        for i in range(0, len(wanted), 400):
            shop.run_nartool(wanted[i:i + 400], tmp)
        shop.run_nartool(["resource/cso_na_en.txt"], tmp)
        loc = shop.load_names(os.path.join(tmp, "resource", "cso_na_en.txt"))
        for item_id, cands in candidates.items():
            path = next((c for c in cands if os.path.exists(os.path.join(tmp, c))), None)
            if not path:
                continue
            dst = os.path.join(shop.ICON_DIR, f"{item_id}.png")
            if not os.path.exists(dst):
                try:
                    open(dst, "wb").write(shop.tga_to_png(open(os.path.join(tmp, path), "rb").read()))
                    made += 1
                except ValueError as e:
                    print(f"skip {path}: {e}")
                    continue
            entries[item_id]["icon"] = True

    catalog = {"primary": [], "secondary": [], "melee": [], "character": []}
    for item_id, e in sorted(entries.items()):
        it = items.get(item_id, {})
        res = it.get("resource", "")
        name = loc.get(f"cso_item_name_{res}") or loc.get(f"cso_{res}") or it.get("name") or str(item_id)
        row = {"id": item_id, "name": name, "category": CATEGORY_NAMES.get(e["category"], "")}
        if e.get("icon"):
            row["icon"] = f"/shop/icons/{item_id}.png"
        if "team" in e:
            row["team"] = e["team"]
        catalog[e["slot"]].append(row)
    for rows in catalog.values():
        rows.sort(key=lambda r: (r["category"], r["name"].lower()))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(catalog, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
    print({k: len(v) for k, v in catalog.items()}, f"{made} new icons -> {shop.ICON_DIR}", f"catalog -> {OUT}")


if __name__ == "__main__":
    main()
