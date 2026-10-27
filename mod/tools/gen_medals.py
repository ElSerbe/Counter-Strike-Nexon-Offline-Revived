"""Writes CSNZ_Server's Data/Medals.json and the medal icons (web/profile/medals/<id>.png) from the game's medals
(resource/title/title.csv, names and descriptions from resource/cso_na_en.txt, icons resource/title/<res>_l.tga).

Offline, every medal gets a condition that can be met by playing (events, seasons, rankings, clans and payments
included): RULES below, one entry per medal resource. A condition is a list of requirements that must all hold:
    ("c", counter, target[, modes])   a statistic of the player (see COUNTERS), in some game modes or in all
    ("level", n)                      the character's level
    ("own", pattern, count)           owns `count` items whose resource name matches the regex (Item.csv)
    ("medals", n)                     n other medals earned
    ("points", n)                     holds n medals (the shop currency) at once
The server (CMedalManager) checks them after every match and gives the medal like the game did (achievement list +
Title packet), so it shows in the game's own medal list and can be worn.

Usage: python gen_medals.py            (needs the game, tools/nartool and Item.csv of the server)
"""
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import export_shop_assets as esa  # noqa: E402  (nartool, TGA -> PNG)

ITEM_CSV = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data", "Item.csv")
OUT_JSON = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data", "Medals.json")
OUT_ICONS = os.path.join(MOD, "web", "profile", "medals")
WORK = os.path.join(os.environ.get("TEMP", HERE), "csnz_medals")

# game mode groups (GameModeList.csv ids)
ORIGINAL = [0, 3, 6, 7, 23, 42]
TDM = [2, 5, 19]
DM = [1, 4, 22]
ZOMBIE = [8, 9, 14, 29, 35, 36, 44, 45, 54, 56, 58]
ZOMBIE_CLASSIC = [8, 9, 14]
ZOMBIE_ALL = ZOMBIE + [20, 24, 30, 32, 37, 43]
ZS = [15]
SCENARIO = [15, 17, 28, 52, 53, 59]
HUMAN_SCENARIO = [17]
SOCCER = [16]
ESCAPE = [20]
ZOMBIE_Z = [45]
EVOLUTION = [54]
PROP_HUNT = [49]
STUDIO = [38, 39]
SUPER_SOLDIER = [57]
BOT_MODES = [3, 4, 5]
BOT_ZOMBIE = [24]
GROUP_NAMES = {  # how a mode list reads in the condition text
    tuple(ORIGINAL): "Original modes", tuple(TDM): "Team Deathmatch modes", tuple(DM): "Deathmatch modes",
    tuple(ZOMBIE): "Zombie modes", tuple(ZOMBIE_CLASSIC): "Zombie (Original, Mutation, Hero)",
    tuple(ZOMBIE_ALL): "any zombie mode", tuple(ZS): "Zombie Scenario", tuple(SCENARIO): "scenario modes",
    tuple(HUMAN_SCENARIO): "Human Scenario", tuple(SOCCER): "Soccer", tuple(ESCAPE): "Zombie Escape",
    tuple(ZOMBIE_Z): "Zombie Z", tuple(EVOLUTION): "Zombie Evolution", tuple(PROP_HUNT): "Prop Hunt",
    tuple(STUDIO): "Studio modes", tuple(SUPER_SOLDIER): "Super Soldier", tuple(BOT_MODES): "AI modes",
    tuple(BOT_ZOMBIE): "Zombie (AI)", tuple(ORIGINAL + TDM + DM): "Original, Deathmatch and Team Deathmatch",
}

# statistics the server keeps (CMedalManager), and how a condition on them reads
COUNTERS = {
    "Matches": "Play {n} matches", "Wins": "Win {n} matches", "Kills": "Score {n} kills", "Deaths": "Die {n} times",
    "Headshots": "Score {n} headshots", "MeleeKills": "Score {n} melee kills", "GrenadeKills": "Score {n} grenade kills",
    "PistolKills": "Score {n} pistol kills", "ShotgunKills": "Score {n} shotgun kills", "SmgKills": "Score {n} SMG kills",
    "RifleKills": "Score {n} rifle kills", "MgKills": "Score {n} machine gun kills", "BotKills": "Defeat {n} AI soldiers",
    "ZombieKills": "Kill {n} zombies", "Infections": "Infect {n} humans", "MonsterKills": "Kill {n} monsters",
    "TopPlayer": "Finish first {n} times", "Seconds": "Play for {h} hours", "BestKills": "Score {n} kills in one match",
    "Bombs": "Plant or defuse {n} bombs", "Hostages": "Rescue {n} hostages", "DropBoxes": "Pick up {n} supply boxes",
    "DaysPlayed": "Play on {n} different days", "OrdersDone": "Complete {n} orders", "PerfectDays":
    "Complete every daily order on {n} days", "RequestsDone": "Complete {n} mission requests", "EventsDone":
    "Complete {n} event missions", "BlackMarketBuys": "Buy {n} items at the Black Market", "Unlocks":
    "Unlock {n} battle pass items", "PassesUnlocked": "Unlock {n} battle passes", "PassesCompleted":
    "Complete {n} battle passes", "WeeklyOrdersDone": "Complete {n} weekly orders",
}

# zombie masters: one ladder of infections
INFECT = [50, 100, 200, 300, 400, 500, 750, 1000, 1250, 1500, 2000, 2500, 3000, 4000, 5000, 6000, 7500, 10000]
ZOMBIE_MASTERS = ["normalzombiemaster", "speedzombiemaster", "heavyzombiemaster", "zombidoctormaster",
                  "pczombiemaster", "deimosmaster", "deimos2master", "witchmaster", "undertakermaster",
                  "boomerzombiemaster", "residentzombiemaster", "chinazombiemaster", "telleportzombiemaster",
                  "revivalzombiemaster", "boosterzombimaster", "passzombimaster", "flyingzombiemaster",
                  "akshazombiemaster", "spiderzbmaster", "meatwallzbmaster"]
# scenario bosses: one ladder of scenario clears (wins in Zombie / Human Scenario and Shelter)
BOSSES = ["PhobosMaster", "OberonMaster", "DioneChase", "FireFighter", "LastSoldier", "TitanAttacker",
          "ScorpionChef", "ErikaGuardian", "AFCleader", "WrathController", "MenuController", "Sherlock",
          "madnessCloser", "mechpilot", "exterminator", "Killjack", "Killcronobotics", "tyrantcrabrestaurant",
          "thisismine", "SharkSpinexpert", "Comanchekiller", "OspreyKiller", "XT-300Killer", "asura"]
BOSS_STEPS = [10, 20, 30, 40, 50, 60, 75, 90, 105, 120, 140, 160, 180, 200, 225, 250, 275, 300, 330, 360, 400,
              450, 500, 600]
LEVELS = {"lv100": 10, "lv500": 20, "lv1000": 30, "lv5000": 40, "lv10000": 50, "lv20000": 55, "lv30000": 60,
          "lv40000": 64, "lv50000": 68, "lv60000": 70, "lv70000": 71, "lv80000": 72}
SEASON_BUY = {"season1buy": 1, "season2buy": 2, "season3buy": 3, "season4buy": 4, "season5buy": 5, "season6buy": 6,
              "season7buy": 7}
SEASON_FIN = {"season1fin": 1, "season2fin": 2, "season3fin": 3, "season4fin": 4, "season5fin": 5, "season6fin": 6,
              "season7fin": 7}
WEEKLY = {"season17_01": 10, "season17_01m": 25, "season18_01": 40, "season18_01m": 60, "season18_02": 80,
          "season18_02m": 100}


def c(counter, target, modes=None):
    return ("c", counter, target, modes)


RULES = {
    # account
    "Firststep": ("Account", [("level", 10)]),
    "Takecare": ("Account", [c("Seconds", 24 * 3600)]),
    "Olduser": ("Account", [c("DaysPlayed", 30)]),
    "Csomania": ("Account", [c("Seconds", 200 * 3600)]),
    "PcDaily": ("Account", [c("DaysPlayed", 7)]),
    "10thbirthcelebrate": ("Account", [c("DaysPlayed", 10)]),
    "csogs2018": ("Account", [c("DaysPlayed", 50)]),
    "kingdomtitle": ("Account", [c("DaysPlayed", 100)]),
    "HardPlayer": ("Account", [c("Matches", 1000)]),
    "Veteran": ("Account", [c("Matches", 2000)]),
    "bestmanner": ("Account", [c("Matches", 100)]),
    "richking": ("Account", [("points", 2000)]),
    "limitpass": ("Account", [("level", 72)]),
    "lv90000": ("Account", [("level", 72), c("Wins", 1000)]),
    "BestClan": ("Account", [c("Wins", 500)]),
    "ClanMaster": ("Account", [c("Wins", 1000)]),
    "BestClanTop": ("Account", [c("Wins", 2000)]),
    "ReturnHero": ("Account", [c("DaysPlayed", 20)]),
    "Csofriends": ("Account", [("medals", 25)]),
    "CSONamed": ("Account", [("medals", 50)]),
    "Motherofsuccess": ("Account", [c("Deaths", 5000)]),
    # ranking: finishing first
    "Rank100": ("Ranking", [c("TopPlayer", 100)]),
    "5thPlace": ("Ranking", [c("TopPlayer", 25)]),
    "4thPlace": ("Ranking", [c("TopPlayer", 50)]),
    "3rdPlace": ("Ranking", [c("TopPlayer", 150)]),
    "2ndPlace": ("Ranking", [c("TopPlayer", 300)]),
    "1stPlace": ("Ranking", [c("TopPlayer", 500)]),
    "Rookie": ("Ranking", [c("TopPlayer", 200)]),
    "Champ": ("Ranking", [c("TopPlayer", 1000)]),
    "ex_rookie": ("Ranking", [c("TopPlayer", 400)]),
    "ex_champ": ("Ranking", [c("TopPlayer", 1500)]),
    "2014nationalChamp": ("Ranking", [c("TopPlayer", 750)]),
    "2014WorldChamp": ("Ranking", [c("TopPlayer", 2000)]),
    "2016nationalChamp": ("Ranking", [c("TopPlayer", 1250)]),
    "2016WorldChamp": ("Ranking", [c("TopPlayer", 3000)]),
    "bronze": ("Ranking", [c("Wins", 25)]),
    "silver": ("Ranking", [c("Wins", 75)]),
    "gold": ("Ranking", [c("Wins", 150)]),
    "platinum": ("Ranking", [c("Wins", 300)]),
    "diamond": ("Ranking", [c("Wins", 600)]),
    "vip": ("Ranking", [c("Wins", 1200)]),
    "vvip": ("Ranking", [c("Wins", 2500)]),
    "leaguediamond": ("Ranking", [c("Wins", 3000)]),
    "leaguemaster": ("Ranking", [c("Wins", 5000)]),
    "tdmeventhonor": ("Team Deathmatch", [c("Kills", 5000, TDM)]),
    # combat (any mode)
    "Headhunter": ("Combat", [c("Headshots", 1000)]),
    "Ace": ("Combat", [c("Headshots", 5000)]),
    "SwordMaster": ("Combat", [c("MeleeKills", 1000)]),
    "Avenger": ("Combat", [c("Kills", 5000)]),
    "MultiKill": ("Combat", [c("BestKills", 50)]),
    "KillerF": ("Combat", [c("ShotgunKills", 1000)]),
    "300Kite": ("Combat", [c("RifleKills", 3000)]),
    "adballoon": ("Combat", [c("SmgKills", 2000)]),
    "HiddenChaser": ("Combat", [c("PistolKills", 1000)]),
    "DemolitionMan": ("Combat", [c("GrenadeKills", 500)]),
    "battlefield": ("Combat", [c("MgKills", 2000)]),
    "claytargetshooting": ("Combat", [c("BotKills", 2000)]),
    "icanfly": ("Combat", [c("BotKills", 10000)]),
    "AllroundSoldier": ("Combat", [c(k, 100) for k in ("PistolKills", "ShotgunKills", "SmgKills", "RifleKills",
                                                      "MgKills", "MeleeKills")]),
    "weaponmaster": ("Combat", [c(k, 1000) for k in ("PistolKills", "ShotgunKills", "SmgKills", "RifleKills",
                                                    "MgKills", "MeleeKills")]),
    "Berserk": ("Combat", [c("BestKills", 80)]),
    # original
    "BombPro": ("Original", [c("Bombs", 100, ORIGINAL)]),
    "GoHome": ("Original", [c("Hostages", 100, ORIGINAL)]),
    "Conqueror": ("Original", [c("Wins", 100, ORIGINAL)]),
    "MetalHero": ("Original", [c("Wins", 300, ORIGINAL)]),
    "OriginalHQ": ("Original", [c("Matches", 500, ORIGINAL)]),
    "Assistant": ("Original", [c("Wins", 50, BOT_MODES)]),
    "BotMaster": ("Original", [c("Wins", 200, BOT_MODES)]),
    "agent": ("Original", [c("Wins", 50, SUPER_SOLDIER)]),
    # team deathmatch / deathmatch
    "TDMWinner": ("Team Deathmatch", [c("Wins", 100, TDM)]),
    "DeathmatchHQ": ("Team Deathmatch", [c("Matches", 500, ORIGINAL + TDM + DM)]),
    "bingbing": ("Team Deathmatch", [c("Wins", 50, DM)]),
    # zombie
    "Survivor": ("Zombie", [c("Wins", 200, ZOMBIE_CLASSIC)]),
    "ZombieHunter": ("Zombie", [c("ZombieKills", 2000, ZOMBIE_CLASSIC)]),
    "Busters": ("Zombie", [c("ZombieKills", 10000, ZOMBIE_ALL)]),
    "Darwin": ("Zombie", [c("Infections", 500, ZOMBIE_CLASSIC)]),
    "Infecter": ("Zombie", [c("Infections", 250, ZOMBIE_ALL)]),
    "Phoenix": ("Zombie", [c("Wins", 500, ZOMBIE_ALL)]),
    "hellozombie": ("Zombie", [c("Matches", 300, ZOMBIE_ALL)]),
    "ZombieHQ": ("Zombie", [c("Matches", 500, ZOMBIE_ALL)]),
    "BotZMaster": ("Zombie", [c("Wins", 200, BOT_ZOMBIE)]),
    "MasterofEscape": ("Zombie", [c("Wins", 50, ESCAPE)]),
    # scenario
    "SoloCompany": ("Scenario", [c("Seconds", 50 * 3600, ZS)]),
    "SoloCompany2": ("Scenario", [c("Wins", 150, ZS)]),
    "KillingMachine": ("Scenario", [c("MonsterKills", 10000, SCENARIO)]),
    "MillionKill": ("Scenario", [c("MonsterKills", 50000, SCENARIO)]),
    "LostcityMaster": ("Scenario", [c("Wins", 100, ZS)]),
    "2gateMaster": ("Scenario", [c("Wins", 200, ZS)]),
    "TrapMaster": ("Scenario", [c("Wins", 400, ZS)]),
    "ZsMaster": ("Scenario", [c("Wins", 25, ZS)]),
    "Zs2Master": ("Scenario", [c("Wins", 50, ZS)]),
    "Zs3Master": ("Scenario", [c("Wins", 75, ZS)]),
    "Zs4Master": ("Scenario", [c("Wins", 125, ZS)]),
    "Zs5Master": ("Scenario", [c("Wins", 175, ZS)]),
    "Zs6Master": ("Scenario", [c("Wins", 250, ZS)]),
    "SpecialForce": ("Scenario", [c("Wins", 50, HUMAN_SCENARIO)]),
    "bodyguard": ("Scenario", [c("Wins", 20, HUMAN_SCENARIO)]),
    "ZombiePveHQ": ("Scenario", [c("Matches", 500, SCENARIO)]),
    "pointbox": ("Scenario", [c("DropBoxes", 100)]),
    "Lastone": ("Scenario", [c("Wins", 30, [28, 52])]),
    "Destroyer": ("Scenario", [c("Wins", 100, [28, 52])]),
    "timeattack1": ("Scenario", [c("Wins", 60, SCENARIO)]),
    "timeattack2": ("Scenario", [c("Wins", 120, SCENARIO)]),
    "ex_timeattack1": ("Scenario", [c("Wins", 250, SCENARIO)]),
    "ex_timeattack2": ("Scenario", [c("Wins", 500, SCENARIO)]),
    # soccer
    "Striker": ("Soccer", [c("Wins", 50, SOCCER)]),
    "AttackMF": ("Soccer", [c("Matches", 30, SOCCER)]),
    "DefenceMF": ("Soccer", [c("Matches", 60, SOCCER)]),
    "CenterBack": ("Soccer", [c("Matches", 100, SOCCER)]),
    "GoalKeeper": ("Soccer", [c("TopPlayer", 10, SOCCER)]),
    "Ribero": ("Soccer", [c("Wins", 100, SOCCER)]),
    "PlayMaker": ("Soccer", [c("Wins", 200, SOCCER)]),
    "Fantasista": ("Soccer", [c("TopPlayer", 50, SOCCER)]),
    "forward": ("Soccer", [c("TopPlayer", 100, SOCCER)]),
    # zombie z
    "mutation": ("Zombie Z", [c("Matches", 50, ZOMBIE_Z)]),
    "transformation": ("Zombie Z", [c("Matches", 150, ZOMBIE_Z)]),
    "tabu": ("Zombie Z", [c("MeleeKills", 100, ZOMBIE_Z)]),
    "clean": ("Zombie Z", [c("Wins", 10, ZOMBIE_Z)]),
    "goldspoon": ("Zombie Z", [c("Wins", 100, ZOMBIE_Z)]),
    "soilspoon": ("Zombie Z", [c("Matches", 300, ZOMBIE_Z)]),
    "bomber": ("Zombie Z", [c("GrenadeKills", 100, ZOMBIE_Z)]),
    "starhero": ("Zombie Z", [c("TopPlayer", 25, ZOMBIE_Z)]),
    "Shopaholic": ("Zombie Z", [c("Kills", 2000, ZOMBIE_Z)]),
    "bareknuckle": ("Zombie Z", [c("MeleeKills", 10, ZOMBIE_Z)]),
    "bombaway": ("Zombie Z", [c("Infections", 300, ZOMBIE_Z)]),
    "finalz": ("Zombie Z", [c("Wins", 300, ZOMBIE_Z)]),
    "zdoctor": ("Zombie Z", [c("Seconds", 50 * 3600, ZOMBIE_Z)]),
    "agentz": ("Zombie Z", [c("Seconds", 150 * 3600, ZOMBIE_Z)]),
    "transexpert": ("Zombie Z", [c("Infections", 50, ZOMBIE_Z)]),
    "zbombkiller": ("Zombie Z", [c("Deaths", 100, ZOMBIE_Z)]),
    "dominant": ("Zombie Z", [c("ZombieKills", 100, ZOMBIE_Z)]),
    "weakperish": ("Zombie Z", [c("ZombieKills", 500, ZOMBIE_Z)]),
    "huntingsense": ("Zombie Z", [c("ZombieKills", 1, ZOMBIE_Z)]),
    "legendaryz": ("Zombie Z", [c("BestKills", 30, ZOMBIE_Z)]),
    "Icarus": ("Zombie Z", [c("Matches", 100, ZOMBIE_Z)]),
    "jointime": ("Zombie Z", [c("Infections", 20, ZOMBIE_Z)]),
    "supersoldierz": ("Zombie Z", [c("TopPlayer", 50, ZOMBIE_Z)]),
    "superstarz": ("Zombie Z", [c("TopPlayer", 10, ZOMBIE_Z)]),
    "zbegins": ("Zombie Z", [c("Matches", 5, ZOMBIE_Z)]),
    "togetherz": ("Zombie Z", [c("Wins", 5, ZOMBIE_Z)]),
    "masterz": ("Zombie Z", [c("ZombieKills", 1000, ZOMBIE_Z)]),
    "zheadshot": ("Zombie Z", [c("Headshots", 1000, ZOMBIE_Z)]),
    "whatthez": ("Zombie Z", [c("Deaths", 15, ZOMBIE_Z)]),
    "Doppelganger": ("Zombie Z", [c("Kills", 100, ZOMBIE_Z)]),
    # zombie evolution
    "spiderzbchange": ("Zombie Evolution", [c("Matches", 100, EVOLUTION)]),
    "meatwallzbchange": ("Zombie Evolution", [c("Wins", 100, EVOLUTION)]),
    "zombie5hmbuff": ("Zombie Evolution", [c("ZombieKills", 1000, EVOLUTION)]),
    # prop hunt
    "superstar": ("Prop Hunt", [c("Wins", 100, PROP_HUNT)]),
    "eagleeye": ("Prop Hunt", [c("Kills", 100, PROP_HUNT)]),
    "chanceman": ("Prop Hunt", [c("Kills", 200, PROP_HUNT)]),
    "tracker": ("Prop Hunt", [c("Matches", 200, PROP_HUNT)]),
    "actor": ("Prop Hunt", [c("Matches", 500, PROP_HUNT)]),
    # studio
    "bestcreator": ("Studio", [c("Seconds", 5 * 3600, STUDIO)]),
    "globalbestcreator": ("Studio", [c("Seconds", 20 * 3600, STUDIO)]),
    "contestcreator": ("Studio", [c("Seconds", 3600, STUDIO)]),
    "bestguide": ("Studio", [c("Seconds", 50 * 3600, STUDIO)]),
    # missions and the shop
    "Give1004": ("Missions", [c("OrdersDone", 100)]),
    "GodOfMission": ("Missions", [c("OrdersDone", 500)]),
    "Perfectionist": ("Missions", [c("PerfectDays", 10)]),
    "Specialist": ("Missions", [c("RequestsDone", 50)]),
    "FriendHenry": ("Missions", [c("Unlocks", 50)]),
    "Enhancer": ("Missions", [c("Unlocks", 100)]),
    "recycle": ("Missions", [c("BlackMarketBuys", 20)]),
    "mix": ("Missions", [c("BlackMarketBuys", 50)]),
    "buffclass19s1title": ("Missions", [c("EventsDone", 10)]),
    "buffclass19s2title": ("Missions", [c("EventsDone", 25)]),
    "buffclass19s3title": ("Missions", [c("EventsDone", 50)]),
    "buffclass19s4title": ("Missions", [c("EventsDone", 75)]),
    "buffclass20s2title": ("Missions", [c("EventsDone", 100)]),
    "buffclass20s3title": ("Missions", [c("EventsDone", 150)]),
    # collections
    "skull": ("Collection", [("own", r"^skull", 5)]),
    "balrog": ("Collection", [("own", r"^balrog", 5)]),
    "janus": ("Collection", [("own", r"^janus", 5)]),
    "darkknight": ("Collection", [("own", r"^buffclass(a|b)$", 2)]),
    "paladin": ("Collection", [("own", r"^buffclass(choijiyoon|yuri)$", 2)]),
    "bloodhunter": ("Collection", [("own", r"bloodhunter", 2)]),
    "lycan": ("Collection", [("own", r"lycan", 2)]),
    "buffphoenix": ("Collection", [("own", r"phoenix", 2)]),
    "buffclassteam": ("Collection", [("own", r"^buffclass", 5)]),
    "samzangmonk": ("Collection", [("own", r"", 150)]),
    "dorothy": ("Collection", [("own", r"", 250)]),
    "mayfly": ("Collection", [("own", r"", 25)]),
    "extenstion": ("Collection", [("own", r"", 50)]),
    "ohyeah": ("Collection", [("own", r"", 100)]),
}
for i, res in enumerate(ZOMBIE_MASTERS):
    RULES[res] = ("Zombie", [c("Infections", INFECT[min(i, len(INFECT) - 1)], ZOMBIE_ALL)])
for i, res in enumerate(BOSSES):
    RULES[res] = ("Scenario", [c("Wins", BOSS_STEPS[min(i, len(BOSS_STEPS) - 1)], SCENARIO)])
for res, lv in LEVELS.items():
    RULES[res] = ("Account", [("level", lv)])
for res, n in SEASON_BUY.items():
    RULES[res] = ("Seasons", [c("PassesUnlocked", n)])
for res, n in SEASON_FIN.items():
    RULES[res] = ("Seasons", [c("PassesCompleted", n)])
for res, n in WEEKLY.items():
    RULES[res] = ("Seasons", [c("WeeklyOrdersDone", n)])

GROUP_ORDER = ["Account", "Ranking", "Combat", "Original", "Team Deathmatch", "Zombie", "Scenario", "Soccer",
               "Zombie Z", "Zombie Evolution", "Prop Hunt", "Studio", "Missions", "Collection", "Seasons"]
# names the English text lacks
NAMES = {"timeattack1": "Time Attack", "timeattack2": "Time Attack Pro", "ex_timeattack1": "Time Attack Expert",
         "ex_timeattack2": "Time Attack Legend", "forward": "Forward", "2014nationalChamp": "National Champion 2014",
         "2014WorldChamp": "World Champion 2014", "2016nationalChamp": "National Champion 2016",
         "2016WorldChamp": "World Champion 2016", "ex_rookie": "Challenger Expert", "ex_champ": "Champion Expert",
         "10thbirthcelebrate": "10th Anniversary", "season18_01": "18 - Season 1", "season18_01m": "18 - Season 1 Elite",
         "season18_02": "18 - Season 2", "season18_02m": "18 - Season 2 Elite", "globalbestcreator": "Global Creator",
         "richking": "Rich King"}


def fmt(n):
    return f"{n:,}"


def describe(req, sets, count=None):
    kind = req[0]
    if kind == "level":
        return f"Reach level {req[1]}"
    if kind == "points":
        return f"Hold {fmt(req[1])} medals at once"
    if kind == "medals":
        return f"Earn {req[1]} other medals"
    if kind == "own":
        pattern, count = req[1], count if count is not None else req[2]
        return f"Own {count} items" if not pattern else f"Own {count} {sets.get(pattern, pattern)} items"
    _, counter, target, modes = req
    hours = target / 3600
    h = fmt(target // 3600) if target % 3600 == 0 else f"{hours:g}"
    text = COUNTERS[counter].format(n=fmt(target), h=h)
    if counter == "Seconds" and target < 3600:
        text = f"Play for {target // 60} minutes"
    elif counter == "Seconds" and target == 3600:
        text = "Play for 1 hour"
    if modes:
        text += " in " + GROUP_NAMES.get(tuple(modes), "modes " + ", ".join(map(str, modes)))
    return text


def main():
    os.makedirs(WORK, exist_ok=True)
    esa.run_nartool(["resource/title/title.csv", "resource/cso_na_en.txt"], WORK)
    raw = open(os.path.join(WORK, "resource", "cso_na_en.txt"), "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    strings = {k.lower(): v for k, v in re.findall(r'"([^"]+)"\s+"([^"]*)"', text)}
    titles = []
    for row in csv.reader(open(os.path.join(WORK, "resource", "title", "title.csv"), encoding="cp949", errors="replace")):
        if row and row[0].strip().isdigit():
            titles.append((int(row[0]), row[1].strip()))

    items = []
    for row in csv.reader(open(ITEM_CSV, encoding="latin1")):
        if row and row[0].isdigit() and len(row) > 4 and row[3].strip() in ("Equipment", "Class", "PeriodicItem"):
            items.append((int(row[0]), row[2].strip().lower()))
    sets = {r"^skull": "SKULL", r"^balrog": "BALROG", r"^janus": "JANUS", r"^buffclass(a|b)$": "Dark Knight",
            r"^buffclass(choijiyoon|yuri)$": "Paladin", "bloodhunter": "Crimson Hunter", "lycan": "Lycanthrope", "phoenix": "Phoenix",
            r"^buffclass": "Superhuman class"}

    medals, missing = [], []
    for mid, res in titles:
        rule = RULES.get(res)
        if not rule:
            missing.append(res)
            continue
        group, reqs = rule
        conds = []
        for req in reqs:
            if req[0] == "c":
                cond = {"Counter": req[1], "Target": req[2]}
                if req[3]:
                    cond["Modes"] = req[3]
            elif req[0] == "own":
                ids = [i for i, r in items if re.search(req[1], r)] if req[1] else []
                cond = {"Own": ids, "Count": min(req[2], len(ids)) if req[1] else req[2]}
            elif req[0] == "level":
                cond = {"Level": req[1]}
            elif req[0] == "points":
                cond = {"Points": req[1]}
            else:
                cond = {"Medals": req[1]}
            conds.append(cond)
        name = strings.get(f"cso_title_name_{res.lower()}") or NAMES.get(res) or res
        lore = (strings.get(f"cso_title_desc_{res.lower()}") or "").replace("\\n", "\n")
        lore = re.sub(r"\n*\* ?This (ribbon|medal) cannot be obtained anymore\.?", "", lore).strip()
        medals.append({"ID": mid, "Resource": res, "Name": name, "Group": group, "Lore": lore,
                       "Text": "; ".join(describe(r, sets, cond.get("Count")) for r, cond in zip(reqs, conds)),
                       "Conditions": conds})
    medals.sort(key=lambda m: (GROUP_ORDER.index(m["Group"]), m["ID"]))
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump({"Version": 1, "Groups": GROUP_ORDER, "Medals": medals}, f, indent=1, ensure_ascii=False)
    print(len(medals), "medals ->", OUT_JSON)
    # the profile page's layout preview (not served by CSNZ_Server) reads the table from next to the icons
    os.makedirs(OUT_ICONS, exist_ok=True)
    with open(os.path.join(OUT_ICONS, "preview.json"), "w", encoding="utf-8") as f:
        json.dump({"Medals": medals}, f, ensure_ascii=False)
    if missing:
        print("no rule (left out):", ", ".join(missing))

    # icons: the large version, else the small one
    os.makedirs(OUT_ICONS, exist_ok=True)
    paths = []
    for m in medals:
        paths += [f"resource/title/{m['Resource'].lower()}_l.tga", f"resource/title/{m['Resource'].lower()}.tga"]
    esa.run_nartool(paths, WORK)
    made = 0
    for m in medals:
        for name in (f"{m['Resource'].lower()}_l.tga", f"{m['Resource'].lower()}.tga"):
            p = os.path.join(WORK, "resource", "title", name)
            if os.path.exists(p):
                try:
                    png = esa.tga_to_png(open(p, "rb").read())
                except ValueError:
                    continue
                open(os.path.join(OUT_ICONS, f"{m['ID']}.png"), "wb").write(png)
                made += 1
                break
    print(made, "icons ->", OUT_ICONS)


if __name__ == "__main__":
    main()
