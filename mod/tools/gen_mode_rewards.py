"""Writes the contents of the mode rewards' boxes into CSNZ_Server's ItemRewards.json, between two marker comments.

These items exist in the game (Item.csv gives each its rewardID) but had no contents, so opening one gave nothing:
- EXP Medals: the EXP in their in-game text.
- Parts Reward Boxes and Zombie Scenario's Daily Reward Boxes: random parts of the grades in their in-game text
  (parts and grades from Data/weaponparts.csv).
- Dr. A's bags: one of the items on cso.fandom.com/wiki/Dr._A's_Trunk, for 30 days.

Then it checks Data/ModeRewards.json: every item must exist and every box must have contents.
Usage: python gen_mode_rewards.py
"""
import csv
import json
import os
import re

MOD = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(MOD, "server", "CSNZ_Server", "bin")
DATA = os.path.join(BIN, "Data")
REWARDS = os.path.join(BIN, "ItemRewards.json")
BEGIN = "\t// >>> gen_mode_rewards.py (begin)"
END = "\t// <<< gen_mode_rewards.py (end)"
CONSUMABLES = [258, 259, 260, 300]  # Zombie Scenario's Small and Large Medical Kit, Battle Revival, Round Retry


def loose(text):
    """JSON with comments and trailing commas, as the server reads it."""
    text = re.sub(r'("(?:[^"\\]|\\.)*")|//[^\n]*', lambda m: m.group(1) or "", text)
    return json.loads(re.sub(r",(\s*[}\]])", r"\1", text))


def parts_by_tier():
    tiers = {}
    with open(os.path.join(DATA, "weaponparts.csv"), encoding="latin1", newline="") as f:
        for row in csv.reader(f):
            if len(row) > 1 and row[0].isdigit() and row[1].isdigit():
                tiers.setdefault(int(row[1]), set()).add(int(row[0]))
    return tiers


def one_of(ids):
    return {"100": {str(i): {"Count": 1, "Duration": 0} for i in sorted(ids)}}


def timed_one_of(ids, days):
    return {"100": {str(i): {"Count": 1, "Duration": days} for i in ids}}


def main():
    tiers = parts_by_tier()
    rank = lambda low, high: set().union(*(tiers.get(t, set()) for t in range(low, high + 1)))
    new = {}
    for reward, exp in ((1012, 4000), (1013, 2000), (1014, 1000)):
        new[reward] = {"Title": "ExpCoupon_TITLE", "Description": "ExpCoupon_MSG", "Localized": True, "Exp": [exp]}
    # the Grade N Parts Reward Boxes (rewardIDs from Item.csv)
    for reward, grade in ((2191, 1), (2183, 2), (2180, 3), (2181, 4), (2182, 5), (2184, 6)):
        new[reward] = {"Title": "Parts Reward Box", "Description": "You obtained the following part:", "RandomItems": [one_of(rank(grade, grade))]}
    # Zombie Scenario's Parts Reward Boxes 01 to 05
    for reward, low, high in ((3809, 1, 2), (3810, 1, 3), (3811, 2, 3), (3812, 2, 4), (3813, 2, 5)):
        new[reward] = {"Title": "Parts Reward Box", "Description": "You obtained the following part:", "RandomItems": [one_of(rank(low, high))]}
    # the Daily Reward Boxes: 5 of each consumable, and a random part per grade range
    ranges = [(1, 3), (2, 5), (3, 6)]
    for grade, reward in enumerate((3950, 3951, 3952, 3953)):
        new[reward] = {"Title": "Daily Reward Box", "Description": "You obtained the following items:",
                       "Items": {str(i): {"Count": 5, "Duration": 0} for i in CONSUMABLES}}
        if grade:
            new[reward]["RandomItems"] = [one_of(rank(*r)) for r in ranges[:grade]]
    # Dr. A's bags
    for reward, analyzer in ((1009, 464), (1036, 465), (1038, 547)):
        new[reward] = {"Title": "Dr. A's Bag", "Description": "You obtained the following item:",
                       "RandomItems": [timed_one_of([285, 286, 287, 288, analyzer], 30)]}
    new[1015] = {"Title": "Dr. A's Bag (Human)", "Description": "You obtained the following item:",
                 "RandomItems": [timed_one_of([350, 351, 352, 353], 30)]}

    text = open(REWARDS, encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in text else "\n"
    body = json.dumps({str(k): v for k, v in sorted(new.items())}, indent="\t")
    lines = [("\t" + l) for l in body.split("\n")[1:-1]]
    block = nl.join([BEGIN] + lines + [END])
    if BEGIN in text:
        start, stop = text.index(BEGIN), text.index(END) + len(END)
        text = text[:start] + block + text[stop:]
    else:
        last = text.rstrip().rfind("}")
        head = text[:last].rstrip()
        text = head + ("" if head.endswith(",") else ",") + nl + block + nl + text[last:]
    rewards = loose(text)  # stops here, before writing, if the result isn't readable
    with open(REWARDS, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"{len(new)} rewards in ItemRewards.json; parts per grade: { {k: len(v) for k, v in sorted(tiers.items())} }")

    # the check
    with open(os.path.join(DATA, "Item.csv"), encoding="latin1", newline="") as f:
        rows = [r for r in csv.reader(f) if r]
    head = next(r for r in rows if r[0] == ";ID")
    reward_col = head.index("rewardID")
    item_reward = {int(r[0]): int(r[reward_col] or 0) for r in rows if r[0].isdigit() and len(r) > reward_col}
    cfg = loose(open(os.path.join(DATA, "ModeRewards.json"), encoding="utf-8").read())
    used = set()

    def collect(rule):
        for key in ("Clear", "ClearOneOf", "Top3"):
            used.update(rule.get(key, []))

    for mode in cfg["Modes"]:
        collect(mode)
        used.update(mode.get("ClearBox", {}).get("Items", []))
        for rule in mode.get("Maps", {}).values():
            collect(rule)
    for s in cfg["Sets"]:
        used.update(s["Items"])
        used.add(s["Bag"])
    used.update(cfg["AnyMode"]["OneOf"])
    for reward in rewards.values():
        if not isinstance(reward, dict):
            continue
    bad = [i for i in sorted(used) if i not in item_reward]
    empty = [i for i in sorted(used) if item_reward.get(i) and str(item_reward[i]) not in rewards]
    inside = set()
    for k in new:
        r = rewards[str(k)]
        inside.update(int(i) for i in r.get("Items", {}))
        for group in r.get("RandomItems", []):
            for chance in group.values():
                inside.update(int(i) for i in chance)
    bad += [i for i in sorted(inside) if i not in item_reward]
    print("items used:", len(used), "| not in Item.csv:", bad, "| boxes without contents:", empty)
    if bad or empty:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
