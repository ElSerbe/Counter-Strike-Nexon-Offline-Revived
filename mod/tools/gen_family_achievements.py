"""Family (clan) achievements for the offline server, from the game's own list.

The game's Family Achievements window lists resource/clan/ClanAchieve.csv (74 achievements: key, category, parent,
maxScore, rewardScore) with the texts CSO_ClanAchieve_<key>_name/_text/_reward. Online they asked for 16 family
members in one match or 100,000 wins. Offline (the user's rules, 2026-11-20):
- "with N Family members" counts the real players in the match: enough when every real player there is in the family
  (alone: always enough). Bots don't count.
- the huge counts are lowered (TARGETS) so they can be reached playing;
- each achievement gives the game's own reward where the item exists (or a close one) and medals (rewardScore x 3);
  "Family EXP/Point Buff +N%" raises the family's EXP and point bonus.

Writes:
- server/CSNZ_Server/bin/Data/FamilyAchievements.json  (CFamilyManager: conditions, targets, rewards, texts)
- tools/botassist/tables/resource/clan/ClanAchieve.csv  (the window's targets: botassist opens it instead of the game's)

Usage: python gen_family_achievements.py   (needs nartool for the game's csv and texts)
"""
import csv
import io
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_shop_assets as esa  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, "server", "CSNZ_Server", "bin", "Data", "FamilyAchievements.json")
OUT_CSV = os.path.join(ROOT, "tools", "botassist", "tables", "resource", "clan", "ClanAchieve.csv")

# game modes (GameModeList.csv)
ORIGINAL = [0, 3, 6, 7, 23, 42]
DM = [1, 4]
TDM = [2, 5]
ZB_CLASSIC = [8, 24, 35, 36]
ZB_MUTANT = [9, 43]
ZB_HERO = [14, 44, 45, 58]
SCENARIO = [15]
LEAGUES = {"oritop": ORIGINAL, "zbtop": ZB_CLASSIC + ZB_MUTANT + ZB_HERO, "pvetop": SCENARIO + [17, 33, 59], "dmtop": DM + TDM}

# the game's numbers -> offline ones
TARGETS = {100000: 100, 200000: 150, 1000: 30, 1000000: 3000, 10000: 1000, 100: 20}
KILL_TARGETS = {"cthunter": 1500, "trhunter": 1500, "dmslayers": 3000, "tdmslayers": 3000, "mainwppro": 1000,
                "subwppro": 500, "meleewppro": 200, "throwwppro": 100, "traitor": 1000, "schoolrage": 1000}
SCEN_MAPS = {"doublegate": "zs_2gate"}

# reward text -> items (id, count, days; 0 days = permanent); the game's items where they exist
REWARDS = [
    (r"Emblem Change|Mark change|Change family mark", [(91, 1, 0)]),
    (r"Low[- ]class Power Amplifier", [(625, 1, 30)]),
    (r"Power Amplifier", [(624, 1, 30)]),
    (r"Stamina Drug", [(621, 1, 30)]),
    (r"Money Extractor", [(2087, 1, 0)]),                        # 3,000 Mileage Coupon
    (r"B-? ?BOX", [(173, 3, 0)]),                                # Advanced Decoder x3
    (r"World'?s? Best Set", [(396, 1, 0)]),                      # Code A Decoder
    (r"C4 Countdown", [(112, 1, 30)]),
    (r"Damage Detail", [(110, 1, 30), (111, 1, 30)]),
    (r"Kill Indication", [(111, 1, 30)]),
    (r"Battle Respawn", [(76, 1, 30)]),                          # Shorten Respawn Time
    (r"Big Emergency Medicine", [(259, 30, 0)]),
    (r"Small Emergency Medicine", [(258, 30, 0)]),
    (r"Blueprint", [(1033, 1, 0)]),
    (r"Durable", [(5104, 1, 0)]),
    (r"Mileage Coupon", [(1046, 1, 0)]),
    (r"Zombies?: Original popular|Zombie Classic Set", [(482, 1, 30), (834, 1, 30)]),   # BALROG-V, THANATOS-3
    (r"Zombies?: Mutant popular|Zombie Mutant Set", [(8221, 1, 30), (379, 1, 30)]),     # Laser Fist, Blaster
    (r"Zombies?: Hero popular|Zombie Hero Set", [(415, 1, 30), (901, 1, 30)]),          # Tempest, VICTORY Korea
    (r"Battle Guns?|Battle Gun Set", [(8891, 1, 30)]),
    (r"Pistol Set", [(8194, 1, 30), (149, 1, 30)]),
    (r"Variable Set", [(8330, 1, 30)]),
    (r"Zombie Buff Set", [(620, 1, 30), (623, 1, 30)]),
    (r"Cooperation (Token|Badge)", [(8145, 1, 0)]),
]
# "20 x Item Duration Extender(s)", "Item Duration Extension Coupon x100": the game's extender (676), at most 20
EXTENDER = re.compile(r"(\d+) x Item Duration Extender|Duration Extension Coupon x(\d+)", re.I)


def target_of(key, game):
    k = key[4:] if key.startswith("cso_") else key
    tail = k.split("_", 1)[-1]
    if tail in KILL_TARGETS:
        return KILL_TARGETS[tail]
    return TARGETS.get(game, game)


def condition(key, children):
    """what one match adds: {"type": ..., ...} (the server's CFamilyManager::Progress)"""
    k = key[4:]
    if children:
        return {"type": "children", "of": children}
    m = re.match(r"ori_(ct|tr)(ruler|winner|group|play|hunter)$", k)
    if m:
        team = 2 if m.group(1) == "ct" else 1
        what = {"ruler": "win", "winner": "win", "group": "play", "play": "play", "hunter": "kills"}[m.group(2)]
        return {"type": what, "modes": ORIGINAL, "team": team, "members": 16 if m.group(2) in ("ruler", "group") else 2}
    m = re.match(r"zb_(ori|mu|he)(winner|group)$", k)
    if m:
        modes = {"ori": ZB_CLASSIC, "mu": ZB_MUTANT, "he": ZB_HERO}[m.group(1)]
        return {"type": "play", "modes": modes, "members": 16 if m.group(2) == "group" else 2}
    m = re.match(r"zbs_(hard|normal|easy)s$", k)
    if m:
        return {"type": "win", "modes": SCENARIO, "difficulty": {"easy": 0, "normal": 2, "hard": 4}[m.group(1)]}
    m = re.match(r"zbs_(\w+)$", k)
    if m:
        return {"type": "win", "modes": SCENARIO + [33], "map": SCEN_MAPS.get(m.group(1), "zs_" + m.group(1)), "members": 10}
    m = re.match(r"(ori|zb|pve|dm)top$", k)
    if m:
        return {"type": "play", "modes": LEAGUES[k], "note": "1st in the league: offline, playing its matches"}
    if k in ("dm_dmslayers", "dm_tdmslayers"):
        return {"type": "kills", "modes": DM if k == "dm_dmslayers" else TDM}
    m = re.match(r"dm_(main|sub|melee|throw)wppro$", k)
    if m:
        return {"type": "kills", "modes": DM + TDM, "weapon": {"main": "primary", "sub": "secondary", "melee": "melee",
                                                              "throw": "grenade"}[m.group(1)]}
    if k == "dm_traitor":
        return {"type": "kills", "modes": DM + TDM}
    if k in ("ms_ateam", "ms_zbkiller", "ms_zbspro"):
        return {"type": "orders", "period": "Weekly"}
    if k == "ms_pro":
        return {"type": "orders", "period": "Daily"}
    if k == "evt_schoolrage":
        return {"type": "kills", "modes": ORIGINAL + TDM, "members": 4}
    if k == "evt_randjourney":
        return {"type": "win", "modes": SCENARIO, "members": 2}
    raise SystemExit("no condition for " + key)


def describe(cond, target):
    """the window's text for the offline rule"""
    t = cond["type"]
    if t == "children":
        return None
    modes = cond.get("modes", [])
    name = ("Original" if modes == ORIGINAL else "Deathmatch" if modes == DM else "Team Deathmatch" if modes == TDM else
            "DM/TDM" if modes == DM + TDM else "Original/TDM" if modes == ORIGINAL + TDM else
            "Zombie Classic" if modes == ZB_CLASSIC else "Zombie Mutant" if modes == ZB_MUTANT else
            "Zombie Hero" if modes == ZB_HERO else "Zombie Scenario" if modes[:1] == SCENARIO else "zombie modes"
            if set(modes) & set(ZB_CLASSIC) else "PvE modes")
    team = {1: " as TR", 2: " as CT"}.get(cond.get("team"), "")
    weapon = {"primary": " with primary weapons", "secondary": " with pistols", "melee": " with melee weapons",
              "grenade": " with grenades"}.get(cond.get("weapon"), "")
    who = "\n(every real player in the match from the family)" if cond.get("members") else ""
    n = "{:,}".format(target)
    if t == "win" and cond.get("map"):
        return "Clear the mission with your family" + who
    if t == "win" and "difficulty" in cond:
        return "Clear Zombie Scenario on %s or harder %s time%s" % ({0: "any difficulty", 2: "Normal", 4: "Hard"}[cond["difficulty"]], n, "" if target == 1 else "s")
    if t == "win":
        return "Win %s %s time%s%s%s" % (name, n, "" if target == 1 else "s", team, who)
    if t == "play":
        return "Play %s %s time%s%s%s" % (name, n, "" if target == 1 else "s", team, who)
    if t == "kills":
        return "Get %s kills in %s%s%s%s" % (n, name, weapon, team, who)
    if t == "orders":
        return "Complete %s %s orders" % (n, cond["period"].lower())
    return None


def main():
    out = tempfile.mkdtemp(prefix="gen_family_")
    esa.run_nartool(["resource/clan/ClanAchieve.csv", "resource/cso_na_en.txt", "resource/ClanLevel.csv"], out)
    raw = open(os.path.join(out, "resource", "clan", "ClanAchieve.csv"), "rb").read()
    rows = list(csv.DictReader(io.StringIO(raw.decode("latin-1"))))
    b = open(os.path.join(out, "resource", "cso_na_en.txt"), "rb").read()
    text = b.decode("utf-16") if b[:2] in (b"\xff\xfe", b"\xfe\xff") else b.decode("utf-8", "replace")
    texts = {}
    for line in text.splitlines():
        m = re.match(r'\s*"CSO_ClanAchieve_(.+?)_(name|text|reward)"\s+"([^"]*)"', line)
        if m:
            texts[(m.group(1).lower(), m.group(2))] = m.group(3)  # the last one wins (the game's newer texts)
    children = {}
    for r in rows:
        if r["parent"]:
            children.setdefault(int(r["parent"]), []).append(int(r["ID"]))

    # the game's family levels: the EXP each level needs in all (resource/ClanLevel.csv: level, EXP)
    levels = [int(r[1]) for r in csv.reader(io.StringIO(open(os.path.join(out, "resource", "ClanLevel.csv"), "rb").read()
                                                        .decode("latin-1"))) if len(r) > 1 and r[1].strip().isdigit()]
    achievements, overrides = [], {}
    for r in rows:
        aid, key = int(r["ID"]), r["key"]
        game = int(r["maxScore"])
        kids = children.get(aid, [])
        cond = condition(key, kids)
        target = len(kids) if kids else target_of(key, game)
        reward = texts.get((key.lower(), "reward"), "")
        items = next((it for pat, it in REWARDS if re.search(pat, reward, re.I)), [])
        ext = EXTENDER.search(reward)
        if ext:
            items = [(676, min(20, int(ext.group(1) or ext.group(2))), 0)]
        buff = re.search(r"Buff \+?(\d+)%|(\d+)% Point Buff", reward)
        buff = int(buff.group(1) or buff.group(2)) if buff else 0
        a = {"ID": aid, "Key": key, "Category": int(r["category"] or 0), "Parent": int(r["parent"]) if r["parent"] else 0, "Target": target,
             "GameTarget": game, "Score": int(r["rewardScore"]), "Medals": int(r["rewardScore"]) * 3,
             "Buff": buff, "Items": [{"ItemID": i, "Count": c, "Days": d} for i, c, d in items],
             "Name": texts.get((key.lower(), "name"), key), "Reward": reward, "Condition": cond}
        achievements.append(a)
        desc = describe(cond, target)
        if desc:
            overrides["CSO_ClanAchieve_%s_text" % key] = desc
        r["maxScore"] = str(target)
    doc = {
        "_comment": "Generated by tools/gen_family_achievements.py from the game's resource/clan/ClanAchieve.csv. "
                    "Condition types: win/play/kills (modes, team 1 TR 2 CT, members: real players all from the family, "
                    "weapon, map, difficulty), orders (Daily/Weekly), children (all of them done). Target: the offline "
                    "number (GameTarget the game's). Medals and Items go to every family member who played the match "
                    "that completed it; Buff raises the family's EXP and point bonus (%). A new level gives LevelMedals x level "
                    "medals to every member online.",
        "Version": 1,
        "Exp": {"Match": 20, "Win": 15, "Kill": 1, "MaxKills": 60, "Achievement": 10},
        "Points": {"Match": 5, "Win": 5, "KillsPerPoint": 5},
        "MembersBase": 30, "MembersPerLevel": 2, "LevelMedals": 25,
        "Levels": levels,
        "Achievements": achievements,
        "Texts": overrides,
    }
    with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent="\t", ensure_ascii=False)
    os.makedirs(os.path.dirname(OUT_CSV), exist_ok=True)
    cols = list(rows[0].keys())
    with open(OUT_CSV, "w", encoding="latin-1", newline="") as f:
        w = csv.writer(f, lineterminator="\r\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([r[c] for c in cols])
    print("%d achievements, %d texts -> %s, %s" % (len(achievements), len(overrides), OUT_JSON, OUT_CSV))
    missing = [a["Key"] + ": " + a["Reward"] for a in achievements if not a["Items"] and not a["Buff"]]
    print("rewards without an item (medals only):", missing)


if __name__ == "__main__":
    main()
