"""Writes CSNZ_Server's Data/SteamAchievements.json: the game's Steam achievements and what unlocks them offline.

The list comes from Steam's own copy of the achievement schema (appcache/stats/UserGameStatsSchema_273110.bin, a
binary KeyValues file). Achievement AQ_<2000+id> / AQS<2000+id> is the game's medal <id> (title.csv), with the
progress stat SA<2000+id>: those unlock with the medal (Data/Medals.json). The newer ones have no medal in the
client; EXTRA gives them an offline condition in the medal format (see gen_medals.py).

The server sends them with the game's Steam packet (hw.dll Packet_Steam, id 192): SetStat for the progress, then
SetAchievement + StoreStats.

Usage: python gen_steam_achievements.py [path to UserGameStatsSchema_273110.bin]
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from gen_medals import (SCENARIO, TDM, ZOMBIE, ZOMBIE_ALL, ZOMBIE_CLASSIC, ZS, c, describe)  # noqa: E402

SCHEMA = r"C:\Program Files (x86)\Steam\appcache\stats\UserGameStatsSchema_273110.bin"
MEDALS = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data", "Medals.json")
OUT = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data", "SteamAchievements.json")

H = 3600
# achievements without a medal in the game: an offline condition each (all its requirements must hold)
EXTRA = {
    "AQS2267": [c("DaysPlayed", 5)],                   # 5 consecutive logins
    "AQS2268": [c("Matches", 10)],                     # 10 logins
    "AQS2269": [c("OrdersDone", 30)],                  # decode 30 decoders
    "AQS2273": [c("Wins", 35, SCENARIO)],              # scenario bosses: scenario clears
    "AQ_2280": [c("RequestsDone", 10)],                # mission chain tutorial
    "AQS2296": [c("Wins", 45, SCENARIO)],
    "AQ_2297": [c("Wins", 300, ZS)],                   # Zombie Scenario season 7
    "AQS2298": [c("Wins", 55, SCENARIO)],
    "AQS2304": [c("Unlocks", 40)],                     # part combining
    "AQS2322": [c("Wins", 65, SCENARIO)],
    "AQS2323": [c("Wins", 80, SCENARIO)],
    "AQ_2335": [c("Wins", 350, ZS)],                   # Zombie Scenario season 8
    "AQS2346": [c("Wins", 10, ZS)],                    # Zombie Scenario clears with restrictions
    "AQS2347": [c("Wins", 20, ZS)],
    "AQS2348": [c("Wins", 30, ZS)],
    "AQS2349": [c("Wins", 40, ZS)],
    "AQS2350": [c("Wins", 50, ZS)],
    "AQS2351": [c("Wins", 60, ZS)],
    "AQS2352": [c("Wins", 70, ZS)],
    "AQS2353": [c("Wins", 80, ZS)],
    "AQS2354": [c("Wins", 90, ZS)],
    "AQS2355": [c("Wins", 100, ZS)],
    "AQS2356": [c("MeleeKills", 500, SCENARIO)],      # melee-only clears
    "AQS2357": [c("MeleeKills", 1000, SCENARIO)],
    "AQS2358": [c("Infections", 300, ZOMBIE_ALL)],
    "AQS2359": [c("ZombieKills", 3000, ZOMBIE_ALL)],
    "AQS2369": [c("Wins", 95, SCENARIO)],
    "AQS2382": [c("Wins", 110, SCENARIO)],
    "AQS2401": [c("Wins", 130, SCENARIO)],
    "AQS2478": [c("Seconds", 100 * H, SCENARIO)],     # 500 h in Zombie Scenario
    "AQS2477": [c("Seconds", 40 * H, SCENARIO)],      # 200 h
    "AQS2476": [c("Wins", 150, ZS)],
    "AQS2475": [c("Wins", 250, SCENARIO)],
    "AQS2474": [c("Wins", 500, SCENARIO)],
    "AQ_2462": [c("RequestsDone", 25)],                # explorations
    "AQ_2461": [c("EventsDone", 10)],
    "AQS2460": [c("BlackMarketBuys", 30)],
    "AQS2459": [c("Matches", 500)],                    # commendations received
    "AQS2458": [c("Matches", 1500)],                   # commendations given
    "AQS2457": [c("BlackMarketBuys", 5)],              # auction house
    "AQS2456": [c("Unlocks", 25)],                     # weapon painting
    "AQS2448": [c("Kills", 10000)],                    # reloads
    "AQS2444": [c("TopPlayer", 60)],                   # 3rd or better 200 times
    "AQS2443": [c("TopPlayer", 100)],
    "AQS2442": [c("Matches", 300, ZOMBIE_ALL)],        # recover 50,000 HP
    "AQS2441": [c("Wins", 100, ZOMBIE)],               # morale boost as a human
    "AQS2440": [c("MeleeKills", 300, ZOMBIE_ALL)],     # rounds won without guns
    "AQ_2439": [c("Wins", 25, ZOMBIE_ALL)],            # pacifist
    "AQ_2438": [c("Deaths", 1000, ZOMBIE_ALL)],        # hot bath
    "AQS2437": [c("GrenadeKills", 300)],               # fire bombs
    "AQS2436": [c("DropBoxes", 100)],                  # supply boxes as a human
    "AQS2435": [c("Infections", 1500, ZOMBIE_ALL)],    # zombie bomb damage
    "AQS2434": [c("Infections", 3000, ZOMBIE_ALL)],    # zombie bombs thrown
    "AQS2427": [c("Matches", 300, SCENARIO)],          # weapons bought in Zombie Scenario
    "AQS2416": [c("DaysPlayed", 60)],                  # explorer
    "AQS2415": [c("ZombieKills", 1500, ZOMBIE_ALL)],   # human skills
    "AQS2410": [c("Matches", 200, ZOMBIE_ALL)],        # perks
}

# achievements whose own goal is something the server counts: they keep that goal (the Steam bar shows the real
# count) instead of unlocking with their medal, whose goal is another counter or number
STEAM_GOAL = {
    "AQS2007": ("Wins", ZOMBIE_CLASSIC),               # survive in Zombie Mode
    "AQS2008": ("ZombieKills", ZOMBIE_CLASSIC),
    "AQS2010": ("MeleeKills", None),
    "AQS2026": ("Wins", TDM),
    "AQS2063": ("Wins", [20]),                         # escape in Zombie Escape
    "AQS2074": ("Infections", [20]),
    "AQS2239": ("Kills", [49]),                        # objects killed as a seeker
}


def parse_kv(buf, i=0):
    out = {}
    while True:
        t = buf[i]
        i += 1
        if t in (8, 11):
            return out, i
        j = buf.index(0, i)
        key = buf[i:j].decode("utf-8", "replace")
        i = j + 1
        if t == 0:
            v, i = parse_kv(buf, i)
        elif t == 1:
            j = buf.index(0, i)
            v = buf[i:j].decode("utf-8", "replace")
            i = j + 1
        elif t in (2, 3):
            v = struct.unpack_from("<i" if t == 2 else "<f", buf, i)[0]
            i += 4
        elif t == 7:
            v = struct.unpack_from("<Q", buf, i)[0]
            i += 8
        else:
            raise ValueError(f"KeyValues type {t} at {i}")
        out[key] = v


def condition(req):
    """A gen_medals requirement as Data/Medals.json writes it."""
    _, counter, target, modes = req
    cond = {"Counter": counter, "Target": target}
    if modes:
        cond["Modes"] = modes
    return cond


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else SCHEMA
    schema, _ = parse_kv(open(path, "rb").read())
    stats = schema["273110"]["stats"]
    stat_max = {v["name"]: v.get("max", 0) for v in stats.values() if v.get("type") == "INT" and "name" in v}
    medals = {m["ID"] for m in json.load(open(MEDALS, encoding="utf-8"))["Medals"]}

    out, missing = [], []
    for block in stats.values():
        for bit in block.get("bits", {}).values():
            name = bit["name"]
            num = int(name[3:])
            ach = {"Name": name, "Display": bit["display"]["name"].get("english", name),
                   "Desc": bit["display"].get("desc", {}).get("english", "")}
            progress = bit.get("progress") or {}
            stat = progress.get("value", {}).get("operand1")
            if stat:
                # Steam takes the stat up to its own max; the achievement's bar ends at max_val
                limit = int(stat_max.get(stat) or 0)
                bar = int(progress.get("max_val") or 0)
                ach["Stat"] = stat
                ach["Max"] = min(bar, limit) if bar and limit else bar or limit
            if name in STEAM_GOAL and ach.get("Max"):
                counter, modes = STEAM_GOAL[name]
                req = c(counter, ach["Max"], modes)
                ach["Text"] = describe(req, {})
                ach["Conditions"] = [condition(req)]
            elif num - 2000 in medals:
                ach["Medal"] = num - 2000
            elif name in EXTRA:
                ach["Text"] = "; ".join(describe(r, {}) for r in EXTRA[name])
                ach["Conditions"] = [condition(r) for r in EXTRA[name]]
            else:
                missing.append(name)
                continue
            out.append(ach)
    if missing:
        sys.exit("no medal and no EXTRA condition for: " + ", ".join(missing))
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump({"Version": 1, "Achievements": out}, f, indent=1, ensure_ascii=False)
    print(f"{len(out)} achievements ({sum('Medal' in a for a in out)} medals, {len(out) - sum('Medal' in a for a in out)} own conditions) -> {OUT}")


if __name__ == "__main__":
    main()
