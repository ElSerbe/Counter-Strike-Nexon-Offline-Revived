"""Writes CSNZ_Server's Data/Missions.json for the Missions page (taskbar Missions button): mission requests made
from the game's own daily / special quests (their NPCs, names and stories, resource/cso_na_en.txt), weekly event
missions, and the Black Market stock (the game's BlackBadgeShop.csv, paid with Black Badges). Also exports the NPC
portraits (web/missions/npc/<npc>.png) and the Black Market item icons (web/shop/icons/<id>.png, shared).

A requirement is a statistic of the player (CMedalManager counters, see gen_medals.COUNTERS) that must grow by the
target after the mission is taken: ("Counter", target[, modes]).

Usage: python gen_missions.py
"""
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import export_shop_assets as esa  # noqa: E402
from gen_medals import (ORIGINAL, TDM, DM, ZOMBIE, ZOMBIE_CLASSIC, ZOMBIE_ALL, ZS, SCENARIO, HUMAN_SCENARIO,  # noqa: E402
                        SOCCER, BOT_MODES, PROP_HUNT, describe)

COSTUME_MIN_PRICE = 300  # Black Badges: cosmetics are the Black Market's expensive goods
DATA = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data")
OUT_JSON = os.path.join(DATA, "Missions.json")
NPC_DIR = os.path.join(MOD, "web", "missions", "npc")
WORK = os.path.join(os.environ.get("TEMP", HERE), "csnz_missions")
NPCS = ["DoctorA", "Evelyn", "Felix", "Jacob", "Victor", "Zim", "Chen", "Rowen"]
SHELTER = [28, 52]


def c(counter, target, modes=None):
    return ("c", counter, target, modes)


# game quest -> (requirements, black badges, medals)
DAILY = {
    1: ([c("Wins", 3)], 4, 15), 2: ([c("Headshots", 5)], 3, 10), 3: ([c("Headshots", 7, BOT_MODES)], 3, 10),
    4: ([c("Bombs", 3, ORIGINAL)], 5, 15), 5: ([c("Hostages", 5, ORIGINAL)], 5, 15), 6: ([c("Wins", 2, ORIGINAL)], 4, 15),
    7: ([c("GrenadeKills", 1, ORIGINAL + TDM + DM)], 3, 10),
    8: ([c("MeleeKills", 1, ORIGINAL + TDM + DM), c("PistolKills", 1, ORIGINAL + TDM + DM)], 4, 10),
    9: ([c("Kills", 25, TDM + DM)], 3, 10), 10: ([c("Wins", 2, [14])], 5, 15), 11: ([c("ZombieKills", 20, [14])], 4, 15),
    12: ([c("MeleeKills", 3, ZOMBIE_CLASSIC)], 5, 15), 13: ([c("Headshots", 5, [9, 14])], 4, 10),
    14: ([c("ZombieKills", 30, [9, 14])], 4, 15),
    15: ([c("ZombieKills", 10, ZOMBIE_CLASSIC), c("Infections", 10, ZOMBIE_CLASSIC)], 6, 20),
    16: ([c("MonsterKills", 150, ZS)], 5, 15), 17: ([c("Wins", 1, ZS)], 5, 15), 18: ([c("Wins", 1, HUMAN_SCENARIO)], 5, 15),
    19: ([c("MonsterKills", 200, HUMAN_SCENARIO)], 5, 15), 20: ([c("Wins", 1, SOCCER), c("Matches", 2, SOCCER)], 5, 15),
    21: ([c("Kills", 50, ORIGINAL + TDM)], 4, 15), 22: ([c("MonsterKills", 100, ZS)], 4, 15),
    23: ([c("Seconds", 20 * 60)], 2, 5), 24: ([c("Seconds", 40 * 60)], 3, 10), 25: ([c("Seconds", 60 * 60)], 4, 15),
    26: ([c("Wins", 1, SHELTER)], 6, 20), 27: ([c("Matches", 2, SHELTER)], 4, 15), 28: ([c("ZombieKills", 30, [14])], 4, 15),
    29: ([c("Infections", 10, ZOMBIE_CLASSIC)], 4, 15), 30: ([c("Matches", 3, ZOMBIE_CLASSIC)], 3, 10),
    31: ([c("Infections", 5, [9, 14])], 3, 10), 32: ([c("Matches", 2, PROP_HUNT)], 4, 10),
}
SPECIAL = {
    1: ("Original", [c("Kills", 15, ORIGINAL)], 3, 10), 2: ("Deathmatch", [c("Kills", 60, DM)], 4, 15),
    3: ("Team Deathmatch", [c("Kills", 45, TDM)], 4, 15), 4: ("Original (AI)", [c("Kills", 50, [3])], 4, 15),
    5: ("Deathmatch (AI)", [c("Kills", 120, [4])], 5, 15), 6: ("Team Deathmatch (AI)", [c("Kills", 80, [5])], 5, 15),
    7: ("Zombie", [c("ZombieKills", 10, [8])], 3, 10), 8: ("Zombie Mutation", [c("ZombieKills", 10, [9])], 3, 10),
    9: ("Zombie Hero", [c("ZombieKills", 5, [14])], 3, 10), 10: ("Zombie hunt", [c("ZombieKills", 50, ZOMBIE_ALL)], 5, 15),
    11: ("Zombie Exterminate", [c("ZombieKills", 30, [32])], 5, 15), 12: ("Zombie Scenario", [c("MonsterKills", 300, ZS)], 5, 20),
    13: ("Human Scenario", [c("MonsterKills", 150, HUMAN_SCENARIO)], 5, 20), 15: ("Gun Deathmatch", [c("Kills", 45, [22])], 4, 15),
    16: ("Light Original", [c("Kills", 45, [23])], 4, 15), 17: ("Zombie modes", [c("ZombieKills", 30, ZOMBIE)], 4, 15),
    19: ("Zombie Shelter", [c("MonsterKills", 300, SHELTER)], 5, 20), 21: ("Zombie Union", [c("ZombieKills", 10, [29])], 3, 10),
    22: ("Zombie Giant", [c("Kills", 3, [30])], 4, 10),
}

# weekly events (one at a time, in this order): missions with rewards, and a bonus for finishing all of them
EVENTS = [
    ("Operation Headhunter", "Command wants marksmen. Every clean headshot this week counts toward the operation.",
     [([c("Headshots", 50)], 10, 30), ([c("Headshots", 150)], 15, 40), ([c("RifleKills", 200)], 10, 30),
      ([c("TopPlayer", 3)], 15, 40)], 25, 100),
    ("Outbreak Week", "The infection is spreading across every zone. Hold the line, or join the horde.",
     [([c("ZombieKills", 100, ZOMBIE_ALL)], 10, 30), ([c("Infections", 50, ZOMBIE_ALL)], 10, 30),
      ([c("Wins", 5, ZOMBIE_ALL)], 15, 40), ([c("Matches", 10, ZOMBIE_ALL)], 10, 30)], 25, 100),
    ("Scenario Siege", "The labs are overrun. Clear the scenarios and bring the survivors home.",
     [([c("MonsterKills", 1000, SCENARIO)], 10, 30), ([c("Wins", 3, SCENARIO)], 15, 40),
      ([c("MonsterKills", 3000, SCENARIO)], 15, 40), ([c("DropBoxes", 10)], 10, 30)], 25, 100),
    ("Arms Race", "A field test for every weapon class. Show the engineers what each one can do.",
     [([c("PistolKills", 30)], 8, 25), ([c("SmgKills", 60)], 8, 25), ([c("ShotgunKills", 40)], 8, 25),
      ([c("MgKills", 60)], 8, 25), ([c("MeleeKills", 15)], 10, 30)], 25, 100),
    ("Demolition Week", "Bomb sites, hostages and objectives: the Original front needs specialists.",
     [([c("Bombs", 10, ORIGINAL)], 12, 35), ([c("Hostages", 10, ORIGINAL)], 12, 35), ([c("Wins", 5, ORIGINAL)], 12, 35),
      ([c("Kills", 150, ORIGINAL)], 10, 30)], 25, 100),
    ("Iron Will", "Show up, fight, finish. This week rewards the ones who keep coming back.",
     [([c("Matches", 15)], 10, 30), ([c("Wins", 8)], 12, 35), ([c("Seconds", 5 * 3600)], 12, 35),
      ([c("Kills", 400)], 10, 30)], 25, 100),
    ("Team Deathmatch Cup", "Points table open: every team kill and every win goes on the board.",
     [([c("Kills", 300, TDM)], 10, 30), ([c("Wins", 5, TDM)], 12, 35), ([c("TopPlayer", 3, TDM)], 15, 40),
      ([c("Headshots", 80, TDM)], 10, 30)], 25, 100),
    ("Close Quarters", "No room for long sights. Get up close and make it count.",
     [([c("MeleeKills", 25)], 12, 35), ([c("ShotgunKills", 80)], 10, 30), ([c("GrenadeKills", 15)], 10, 30),
      ([c("BotKills", 300)], 10, 30)], 25, 100),
]


def pretty_name(res, kind):
    """A readable name for an item the English text lacks (newer Korean season items): cosagenth_pet -> Agenth (Pet),
    y25s3sflaser -> SF Laser (S25-3)"""
    slot = ""
    m = re.match(r"^(.*?)_(pet|head|back|pelvis)$", res, re.I)
    if m:
        res, slot = m.group(1), m.group(2).title()
    season = ""
    m = re.match(r"^y(\d\d)s(\d)(.*)$", res, re.I)
    if m:
        season, res = f"S{m.group(1)}-{m.group(2)}", m.group(3)
    res = re.sub(r"^(cos|cso|chn)+", "", res, flags=re.I)
    words = re.sub(r"(\d+)", r" \1", res).split()
    base = " ".join(w.upper() if len(w) <= 2 else w[:1].upper() + w[1:] for w in words) or res
    extra = ", ".join(x for x in (slot, season) if x)
    return f"{base} ({extra})" if extra else base


def requirement(req):
    _, counter, target, modes = req
    cond = {"Counter": counter, "Target": target}
    if modes:
        cond["Modes"] = modes
    return cond


def main():
    os.makedirs(WORK, exist_ok=True)
    esa.run_nartool(["resource/cso_na_en.txt", "resource/quest/quest_npcres.csv"] +
                    [f"resource/npc/{n.lower()}.tga" for n in NPCS], WORK)
    raw = open(os.path.join(WORK, "resource", "cso_na_en.txt"), "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    strings = {}
    for k, v in re.findall(r'"([^"]+)"\s+"([^"]*)"', text):
        strings.setdefault(k.lower(), v.replace("\\n", "\n"))
    npc_names = {n: strings.get(f"cso_{n.lower()}", n) for n in NPCS}

    os.makedirs(NPC_DIR, exist_ok=True)
    for n in NPCS:
        p = os.path.join(WORK, "resource", "npc", f"{n.lower()}.tga")
        if os.path.exists(p):
            open(os.path.join(NPC_DIR, f"{n.lower()}.png"), "wb").write(esa.tga_to_png(open(p, "rb").read()))

    requests = []
    for num, (reqs, badges, medals) in DAILY.items():
        key = f"cso_quest_daily{num:03d}"
        title = re.sub(r"^\[Daily\]\s*", "", strings.get(key + "_name", f"Request {num}"))
        npc = NPCS[(num - 1) % len(NPCS)]
        requests.append({"ID": num, "Title": title, "Npc": npc_names[npc], "Portrait": npc.lower(),
                         "Story": strings.get(key + "_desc", "").strip(),
                         "Text": "; ".join(describe(r, {}) for r in reqs), "Conditions": [requirement(r) for r in reqs],
                         "Badges": badges, "Medals": medals})
    for num, (mode, reqs, badges, medals) in SPECIAL.items():
        npc = NPCS[(num + 3) % len(NPCS)]
        requests.append({"ID": 100 + num, "Title": f"Special Operation: {mode}", "Npc": npc_names[npc],
                         "Portrait": npc.lower(), "Story": "A special operation from Command. Get it done and report back.",
                         "Text": "; ".join(describe(r, {}) for r in reqs), "Conditions": [requirement(r) for r in reqs],
                         "Badges": badges, "Medals": medals})

    events = []
    for i, (name, desc, missions, bonus_badges, bonus_medals) in enumerate(EVENTS):
        events.append({"ID": i + 1, "Name": name, "Description": desc, "BonusBadges": bonus_badges,
                       "BonusMedals": bonus_medals,
                       "Missions": [{"Text": "; ".join(describe(r, {}) for r in reqs),
                                     "Conditions": [requirement(r) for r in reqs], "Badges": b, "Medals": m}
                                    for reqs, b, m in missions]})

    # Black Market: the game's black badge shop (weapons 30, costumes at least COSTUME_MIN_PRICE); names from
    # ItemNames.json / the game
    items = esa.load_items()
    item_names = json.load(open(os.path.join(DATA, "ItemNames.json"), encoding="utf-8"))
    market = []
    for row in csv.reader(open(os.path.join(DATA, "BlackBadgeShop.csv"), encoding="latin1")):
        if not row or not row[0].isdigit():
            continue
        item_id, price = int(row[12]), int(row[2])
        it = items.get(item_id)
        if not it:
            continue
        res = it.get("resource", "")
        kind = "Weapon" if it.get("class") == "Equipment" else "Costume"
        if kind == "Costume":
            price = max(price, COSTUME_MIN_PRICE)  # the game's shop sells zombie skins and tattoos for 30
        name = (item_names.get(str(item_id)) or strings.get(f"cso_item_name_{res.lower()}") or strings.get(f"cso_{res.lower()}")
                or pretty_name(res, kind))
        market.append({"ItemID": item_id, "Name": name, "Price": price, "Kind": kind})
    available = esa.nar_files()
    wanted = []
    for m in market:
        res = items[m["ItemID"]]["resource"]
        if not os.path.exists(os.path.join(esa.ICON_DIR, f"{m['ItemID']}.png")):
            for cut in range(len(res), 3, -1):
                path = f"gfx/vgui/basket/{res[:cut]}.tga"
                if path in available:
                    wanted.append((m["ItemID"], path))
                    break
    if wanted:
        esa.run_nartool([p for _, p in wanted], WORK)
        os.makedirs(esa.ICON_DIR, exist_ok=True)
        for item_id, path in wanted:
            src = os.path.join(WORK, path)
            if os.path.exists(src):
                try:
                    open(os.path.join(esa.ICON_DIR, f"{item_id}.png"), "wb").write(esa.tga_to_png(open(src, "rb").read()))
                except ValueError:
                    pass

    out = {"Version": 1, "RequestOffers": 4, "RequestSlots": 3, "Requests": requests, "Events": events,
           "BlackMarket": {"Weapons": 6, "Costumes": 2, "Items": market}}
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
    print(len(requests), "requests,", len(events), "events,", len(market), "black market items ->", OUT_JSON)


if __name__ == "__main__":
    main()
