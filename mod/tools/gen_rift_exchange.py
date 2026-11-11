"""Writes CSNZ_Server's Data/RiftExchange.json: the cosmetics of the Rift Exchange (Missions page), paid with the
Rift Tickets that clearing Zombie Disaster Zone gives. Every cosmetic of the game's Item.csv, one item per resource:
costumes (head, back, pelvis, arm, face, pet), zombie skins, tattoos, weapon paints, nameplates, kill taunts,
zombification effects, damage fonts, chat colours and sprays, also the ones the battle passes and the Black Market sell
and the ones without a shop picture. Also exports their icons (web/shop/icons/<id>.png, shared with the shop); run
export_shop_assets.py afterwards for the 3D preview models.

The server's rules (tickets per clear, stock size, holds) are in Data/ZSRift.json.

Usage: python gen_rift_exchange.py
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import export_shop_assets as esa  # noqa: E402
from gen_missions import pretty_name  # noqa: E402

DATA = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data")
OUT_JSON = os.path.join(DATA, "RiftExchange.json")
WORK = os.path.join(os.environ.get("TEMP", HERE), "csnz_riftexchange")

# Item.csv class -> (kind shown on the page, price in Rift Tickets). A clear gives 3 tickets at level 1, 12 at
# level 10 and 22 at level 20 (Data/ZSRift.json), so an item takes about 2 to 5 clears at levels 10 to 20.
KINDS = {
    "HeadCostume": ("Head", 45), "BackCostume": ("Back", 50), "PelvisCostume": ("Pelvis", 35),
    "ArmCostume": ("Arm", 30), "FaceCostume": ("Face", 30), "PetCostume": ("Pet", 60),
    "ZombieSkinCostume": ("Zombie Skin", 55), "Tattoo": ("Tattoo", 25),
    "WeaponPaintItem": ("Weapon Paint", 40), "LobbyBG": ("Nameplate", 35), "CombatInfoItem": ("Kill Taunt", 45),
    "zbRespawnEffect": ("Zombification", 45),
}


def kind_of(it):
    """(kind, price) of a cosmetic, None for anything else; periodic items by resource (damage fonts, chat colours,
    sprays)"""
    cls, res = it.get("class"), it.get("resource", "").lower()
    if cls in KINDS:
        return KINDS[cls]
    if cls == "PeriodicItem":
        if res.startswith("floatingdamageskin"):
            return ("Damage Font", 35)
        if res.startswith("chatcolor_"):
            return ("Chat Colour", 35)
        if "spray" in res or res.endswith("spay"):
            return ("Spray", 15)
    return None


def load_json(path):
    text = open(path, encoding="utf-8").read()
    return json.loads(re.sub(r"(?m)^\s*//.*$", "", text))


def item_name(res, kind):
    """A name for an item the English text lacks: y24s1pelviscos -> S24-1 Pelvis Costume, cosy24cathat_head ->
    Cathat (Head)"""
    m = re.match(r"^y(\d\d)s(\d)(head|back|pelvis|pet|arm)cos(\w*)$", res, re.I)
    if m:
        return f"S{m.group(1)}-{m.group(2)} {m.group(3).title()} Costume" + (f" {m.group(4).upper()}" if m.group(4) else "")
    return pretty_name(re.sub(r"^(cos)?y\d\d", "", res, flags=re.I), kind)


def main():
    items = esa.load_items()
    item_names = load_json(os.path.join(DATA, "ItemNames.json"))

    os.makedirs(WORK, exist_ok=True)
    available = esa.nar_files()
    picks, seen = [], set()
    for item_id in sorted(items):
        it = items[item_id]
        res = it.get("resource", "")
        if not kind_of(it) or not res.strip() or res.lower() in seen:
            continue
        seen.add(res.lower())
        picks.append(item_id)

    # icons: the exact picture first (newer items live in Data/Packer, which has no file list), then a shorter name
    cands = {}
    for item_id in picks:
        res = items[item_id]["resource"]
        cands[item_id] = [f"gfx/vgui/basket/{res}.tga"] + [f"gfx/vgui/basket/{res[:cut]}.tga"
                                                            for cut in range(len(res) - 1, 3, -1)
                                                            if f"gfx/vgui/basket/{res[:cut]}.tga" in available][:1]
    esa.run_nartool(sorted({c for cs in cands.values() for c in cs}) + ["resource/cso_na_en.txt"], WORK)
    raw = open(os.path.join(WORK, "resource", "cso_na_en.txt"), "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    strings = {}
    for k, v in re.findall(r'"([^"]+)"\s+"([^"]*)"', text):
        strings.setdefault(k.lower(), v)

    os.makedirs(esa.ICON_DIR, exist_ok=True)
    out = []
    for item_id in picks:
        src = next((os.path.join(WORK, c) for c in cands[item_id] if os.path.exists(os.path.join(WORK, c))), None)
        icon = os.path.join(esa.ICON_DIR, f"{item_id}.png")
        if src and not os.path.exists(icon):
            try:
                open(icon, "wb").write(esa.tga_to_png(open(src, "rb").read()))
            except ValueError:
                pass
        it = items[item_id]
        res = it["resource"]
        kind, price = kind_of(it)
        name = (item_names.get(str(item_id)) or strings.get(f"cso_item_name_{res.lower()}") or strings.get(f"cso_{res.lower()}")
                or item_name(res, kind))
        out.append({"ItemID": item_id, "Name": name, "Kind": kind, "Price": price})

    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump({"Version": 1, "Items": out}, f, indent=1, ensure_ascii=False)
    counts = {}
    for o in out:
        counts[o["Kind"]] = counts.get(o["Kind"], 0) + 1
    print(len(out), "Rift Exchange items", counts, "->", OUT_JSON)


if __name__ == "__main__":
    main()
