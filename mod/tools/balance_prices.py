"""Raises the medal prices of the weapons that are good at everything (Data/Warbonds.json), so they take longer to get.

Why: the weapons that are strong in every mode are the ones everybody picks, and the rest of the arsenal goes unused.
There are no public usage statistics for Counter-Strike Online / Nexon (searched 2026-11-11: nothing beyond forum
complaints about epic-grade "pay to win" weapons), so the measure is the game's own: each weapon's recommendation grade
for PvP, Zombie and Scenario (0-6, the bars of the native shop; web/shop/iteminfo.json "grades", from Item.csv by
export_shop_assets.py). Their sum says how good a weapon is everywhere.

Rule (sum of the three grades):
  15+ (top in every mode)  x2.5, at least 450      12-14  x2.0, at least 300
  10-11                   x1.6, at least 200       8-9    x1.25, at least 100
  up to 7: unchanged.
A weapon already priced at twice its floor or more (the Thanatos, Janus and Vulcanus sets got x3.5 before) keeps its
price. Prices are capped at 900 and rounded up to end in 0 or 5. Repeatable items (Supply Depot) aren't touched.

The prices before the first run are kept in tools/data/warbond_base_prices.json and every run starts from them, so
running it again doesn't raise anything twice. Usage: python balance_prices.py [--dry-run]
"""
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WARBONDS = os.path.join(ROOT, "server", "CSNZ_Server", "bin", "Data", "Warbonds.json")
INFO = os.path.join(ROOT, "web", "shop", "iteminfo.json")
BASE = os.path.join(ROOT, "tools", "data", "warbond_base_prices.json")

TIERS = [(15, 2.5, 450), (12, 2.0, 300), (10, 1.6, 200), (8, 1.25, 100)]
CAP = 900

ENTRY = re.compile(r'(\{\s*"ItemID":\s*(\d+),\s*"Cost":\s*)(\d+)([^}]*\})')


def round5(value):
    return int(math.ceil(value / 5.0) * 5)


def new_price(base, grades):
    total = sum(grades)
    for need, factor, floor in TIERS:
        if total >= need:
            if base >= floor * 2:
                return base
            return min(CAP, max(round5(base * factor), floor))
    return base


def main():
    dry = "--dry-run" in sys.argv
    text = open(WARBONDS, encoding="utf-8").read()
    info = json.load(open(INFO, encoding="utf-8"))
    if os.path.exists(BASE):
        base = {int(k): v for k, v in json.load(open(BASE, encoding="utf-8")).items()}
    else:
        base = {int(m.group(2)): int(m.group(3)) for m in ENTRY.finditer(text) if "Repeatable" not in m.group(4)}
        if not dry:
            os.makedirs(os.path.dirname(BASE), exist_ok=True)
            json.dump({str(k): v for k, v in sorted(base.items())}, open(BASE, "w", encoding="utf-8"), indent=1)
    changes = []

    def repl(m):
        item, cost, rest = int(m.group(2)), int(m.group(3)), m.group(4)
        entry = info.get(str(item), {})
        if "Repeatable" in rest or entry.get("kind") != "weapon" or item not in base:
            return m.group(0)
        price = new_price(base[item], entry.get("grades") or [0, 0, 0])
        if price != cost:
            changes.append((item, cost, price, entry.get("grades")))
        return m.group(1) + str(price) + rest

    out = ENTRY.sub(repl, text)
    for item, old, price, grades in sorted(changes, key=lambda c: -c[2]):
        print(f"{item:>5}  {old:>4} -> {price:>4}  grades {grades}")
    print(f"{len(changes)} prices changed" + (" (dry run)" if dry else ""))
    if not dry and changes:
        open(WARBONDS, "w", encoding="utf-8", newline="").write(out)


if __name__ == "__main__":
    main()
