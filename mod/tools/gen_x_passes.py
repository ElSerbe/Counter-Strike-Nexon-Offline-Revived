"""Adds two battle passes to Data/Warbonds.json: "X Weapons" and "Space Weapons" (IDs 59 and 60).

They hold the weapons the Random Weapons maps can give (Data/RandomWeaponList.json) that no shop sold: extremely
strong ones, so each costs 5 times its normal price (the user's rule, 2026-11-19). The normal price is what
balance_prices.py would give a weapon of that type: the median base price of its type in the passes
(tools/data/warbond_base_prices.json), raised by its recommendation grades (Item.csv "ItemGradeForMode"). The
science-fiction ones (SPACE) go in their own pass. Four weapons a page; a page opens after spending 40% of the
earlier pages' medals in the pass. Running it again replaces both passes.

Usage: python gen_x_passes.py"""
import csv
import json
import os
import re
import statistics
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(TOOLS)
DATA = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data")
sys.path.insert(0, TOOLS)
import balance_prices  # noqa: E402

X_ID, SPACE_ID = 59, 60
FACTOR = 5
SKIP = {214, 438}  # the plain Frag Grenade, the MP7A1 extended magazine (a part)
# also in Space Weapons, at their usual price: the Star Chaser pair and the Tiamat plasma rifles (they were in the
# Trans-Gear pass, now "2019 Season 1" with only that season's items)
SPACE_EXTRA = {8113: 165, 8078: 265, 8395: 165, 8396: 265}
SPACE = {"beamgun", "halogun", "halogunex", "lasersg", "voidpistolex", "wondercannonex", "sgdrillvortex",
         "transformgunadv", "leapstrikegun", "y21s1jetgunma", "y21s1jetgunmd", "sfturret", "sfsnipermb", "sfmgzhc",
         "restrictionpistol"}
# English names for the weapons the game's texts don't name
NAMES = {
    "dgaxe": "Dread Axe", "dgaxeex": "Dread Axe EX", "y26s1m32": "M32 MGL (2026)", "beamgun": "Beam Gun",
    "transformgunadv": "Transform Gun ADV", "speargunex": "Spear Gun EX", "deserteagleg": "Desert Eagle Gold",
    "bisonfox": "Bizon Fox", "sgdrillvortex": "Drill Vortex", "asusas12": "USAS-12 AS", "wondercannonex": "Heaven Splitter EX",
    "halogunex": "Halo Gun EX", "winggunex": "Wing Gun EX", "shiftexpar": "Shift EX", "huntbow": "Hunter's Bow",
    "soulreaper": "Soul Reaper", "chainmg": "Chain MG", "mgsm": "MG-SM", "m3dragonex": "M3 Dragon EX",
    "forgesword": "Forge Sword", "chainsr": "Chain Sniper", "halogun": "Halo Gun", "winggun": "Wing Gun",
    "lasersg": "Laser Shotgun", "pianogunex": "Piano Gun EX", "divinespear": "Divine Spear", "revivegun": "Revive Gun",
    "buffsg552ex": "SG552 Buff EX", "swordbombard": "Bombard Sword", "crossbowex21": "Crossbow EX21",
    "rocketpistol": "Rocket Pistol", "gilboaexw": "Gilboa EX W", "bufffiveseven": "Five-seveN Buff",
    "leapstrikegun": "Leap Strike Gun", "m1887craftb": "M1887 Craft B", "restrictionpistol": "Restriction Pistol",
    "voidpistolex": "Void Avenger EX", "y21s1jetgunmd": "Jet Gun MD", "dvhammer": "Divine Hammer",
    "infinityex2zhc": "Infinity EX2 (Hero)", "whipflail": "Whip Flail", "summonknifeex": "Summon Knife EX",
    "divinetitanex": "Divine Titan EX", "sfturret": "SF Turret", "divinetitan": "Divine Titan", "sbmine": "SB Mine",
    "boww": "Bow W", "m134w": "M134 W", "qbs09b": "QBS-09 B", "destroyerb": "Destroyer B", "pp2000b": "PP-2000 B",
    "sfsnipermb": "SF Sniper MB", "dualkrissb": "Dual Kriss B", "mg36b": "MG36 B", "hdaggerb": "Hunting Dagger B",
    "wa2000desert": "WA2000 Desert", "pgmb": "PGM B", "bisonb": "Bizon B", "crossbowcls": "Classic Crossbow",
    "ak47l3": "AK-47 L3", "infinityex2desert": "Infinity EX2 Desert", "m95desert": "M95 Desert", "m60desert": "M60 Desert",
    "hammerdesert": "Hammer Desert", "spas12desert": "SPAS-12 Desert", "m2desert": "M2 Desert", "mg3desert": "MG3 Desert",
    "y21s4janusa": "Janus A", "y21s1jetgunma": "Jet Gun MA", "infinityex1zhc": "Infinity EX1 (Hero)",
    "m79gzhc": "M79 Gold (Hero)", "m134exzhc": "M134 EX (Hero)", "m134_xmaszhc": "M134 Xmas (Hero)",
    "sheepsword": "Sheep Sword", "cannonzhc": "Black Dragon Cannon (Hero)", "gatlingzhc": "Gatling (Hero)",
    "skull1zhc": "Skull-1 (Hero)", "thompsongoldzhc": "Thompson Gold (Hero)", "lugerszhc": "Silver Luger (Hero)",
    "skullaxezhc": "Skull Axe (Hero)", "m249exzhc": "M249 EX (Hero)", "mg3_xmaszhc": "MG3 Xmas (Hero)",
    "spas12ex2zhc": "SPAS-12 EX2 (Hero)", "sfmgzhc": "SF MG (Hero)", "skull11zhc": "Skull-11 (Hero)",
    "skull5zhc": "Skull-5 (Hero)", "dbarrelzhc": "Double Barrel (Hero)", "m95xmaszhc": "M95 Xmas (Hero)",
    "skull3zhc": "Skull-3 (Hero)", "infinityzhc": "Infinity (Hero)", "m79zhc": "M79 (Hero)",
    "mastercombatknifezhc": "Master Combat Knife (Hero)", "holybombzhc": "Holy Bomb (Hero)", "y20s1musket": "Musket",
}


def round10(v):
    return int(round(v / 10.0) * 10)


def main():
    rows = {int(r[0]): r for r in csv.reader(open(os.path.join(DATA, "Item.csv"), encoding="latin1")) if r and r[0].isdigit()}
    random_list = json.load(open(os.path.join(DATA, "RandomWeaponList.json"), encoding="utf-8"))
    text = open(os.path.join(DATA, "Warbonds.json"), encoding="utf-8").read()
    # drop our two passes from an earlier run
    text = re.sub(r",\s*\{\s*\"ID\": (%d|%d), [^\n]*\n(?:.*\n)*?\t\t\}(?=\s*[,\]])" % (X_ID, SPACE_ID), "", text)
    sold = {int(x) for x in re.findall(r'"ItemID":\s*(\d+)', text)}
    for name in ("MileageShop.csv", "EventShop.csv", "BlackBadgeShop.csv", "HonorMoneyShop.csv", "EpicPieceShop.csv"):
        for r in csv.reader(open(os.path.join(DATA, name), encoding="latin1")):
            sold |= {int(c) for c in r[:6] if c.strip().isdigit()}
    sold |= {int(x) for x in re.findall(r'"ItemID":\s*(\d+)', open(os.path.join(DATA, "RiftExchange.json"), encoding="utf-8").read())}
    base = {int(k): v for k, v in json.load(open(os.path.join(TOOLS, "data", "warbond_base_prices.json"))).items()}
    by_type = {}
    for item, price in base.items():
        r = rows.get(item)
        if r and r[3] in ("Equipment", "PeriodicItem"):
            by_type.setdefault(r[4], []).append(price)
    median = {t: statistics.median(v) for t, v in by_type.items()}

    def price(item):
        r = rows[item]
        try:
            grades = [int(g) for g in r[48].split(",")[:3]]
        except ValueError:
            grades = [0, 0, 0]
        normal = balance_prices.new_price(median.get(r[4], 90), grades)
        return balance_prices.round5(normal * FACTOR), normal

    x, space = [], []
    for key in random_list:
        if not key.isdigit():
            continue
        item = int(key)
        r = rows.get(item)
        if not r or item in sold or item in SKIP or item in SPACE_EXTRA or r[3] not in ("Equipment", "PeriodicItem"):
            continue
        res = r[2].lower()
        cost, normal = price(item)
        entry = {"ItemID": item, "Cost": cost}
        if res in NAMES:
            entry["Name"] = NAMES[res]
        (space if res in SPACE else x).append((r[4], cost, entry))

    for item, cost in SPACE_EXTRA.items():
        space.append((rows[item][4], cost, {"ItemID": item, "Cost": cost}))

    def pages(entries):
        entries = sorted(entries, key=lambda e: (e[1], e[0]))
        out, spent = [], 0
        for i in range(0, len(entries), 4):
            chunk = entries[i:i + 4]
            out.append({"Unlock": round10(spent * 0.4), "Items": [e[2] for e in chunk]})
            spent += sum(e[1] for e in chunk)
        return out

    def block(wid, name, cost, desc, theme, entries):
        lines = ["\t\t{", f'\t\t\t"ID": {wid}, "Name": "{name}", "Type": "Collection", "Cost": {cost},',
                 f'\t\t\t"Description": "{desc}",', f'\t\t\t"Image": "", "Theme": ["{theme[0]}", "{theme[1]}"],',
                 '\t\t\t"Pages": [']
        ps = pages(entries)
        for n, p in enumerate(ps):
            items = ", ".join(json.dumps(i) for i in p["Items"]).replace('{"', '{ "').replace('}', ' }').replace('":', '":').replace(", \"", ", \"")
            lines.append(f'\t\t\t\t{{ "Unlock": {p["Unlock"]}, "Items": [')
            lines.append(f"\t\t\t\t\t{items}")
            lines.append("\t\t\t\t] }" + ("," if n < len(ps) - 1 else ""))
        lines += ["\t\t\t]", "\t\t}"]
        return "\n".join(lines)

    new = ",\n" + block(X_ID, "X Weapons", 200,
                        "The arsenal of the Random Weapons maps that no shop sells: extremely powerful weapons, at five times the usual price.",
                        ("#1a0808", "#ff4a3a"), x)
    new += ",\n" + block(SPACE_ID, "Space Weapons", 150,
                         "Beams, voids and halos: the science-fiction weapons of the Random Weapons maps at five times the usual price, with the Star Chaser pair and the Tiamat plasma rifles.",
                         ("#060a1f", "#7ad7ff"), space)
    end = text.rfind("\t\t}\n\t]")
    if end < 0:
        end = text.rfind("\t\t}\r\n\t]")
    assert end >= 0, "the end of the Warbonds list wasn't found"
    end = text.index("}", end) + 1
    text = text[:end] + new + text[end:]
    open(os.path.join(DATA, "Warbonds.json"), "w", encoding="utf-8", newline="").write(text)
    for label, entries in (("X Weapons", x), ("Space Weapons", space)):
        costs = [e[1] for e in entries]
        print(f"{label}: {len(entries)} weapons, {min(costs)}-{max(costs)} medals each, {sum(costs)} in all")


if __name__ == "__main__":
    main()
