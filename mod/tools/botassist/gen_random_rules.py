"""Random Rule maps (ModeEvent ZombieRandomRuleMode): writes tables/resource/ModeEvent/RandomRule_Dedi.csv (Zombie Hero rules,
CSOGameRulesZombi3RandomRule) and RandomRule_Classic_Dedi.csv (Zombie Classic rules, CSOGameRulesZombiRandomRule), the rule list
each round's rule is drawn from. Nexon kept both on its dedicated servers. Without them the table was empty: every round
drew rule 0, which doesn't exist, and the round start panel showed "#CSO_RandomRule_Title_0".

mp.dll: the mode's setup stores the path (Hero 0x10792c80 -> rules +0xa68; Classic 0x107b2860 -> +0x5e0) and the match start
(Hero 0x10799350) makes the table (g_CSO +0x1f0 vfunc 0x228) and loads it (table vfunc 0x4), hw.dll's .csv loader that
botassist serves from botassist_tables (table overrides). The draw (Hero 0x10791c60) is weighted by ratio times a per-rule
factor (rules +0xa88 + id * 4, ids 0-61). The last round always plays rule 35 (rules +0xa84).

Columns: 0 ID, 1 ratio (the draw's weight, 0 = never), 2 apply2 (0, 1 or 2: rows with 0 are sent to the client as a list at
the match's start, and when the draw gives one of them the game draws again among the rows with 1), 3 name (unused), 4-8 param1..5
(the rule's own numbers).

The rows are Nexon's, from JusicP/Launcher_CSNZ (GPL-3.0, https://github.com/JusicP/Launcher_CSNZ, DediCsv.h g_RandomRule and
g_RandomRule_Classic). The tables have 61 rules; this client has titles, descriptions and icons only for 1-53 (cso_na_*.txt,
resource/ZombieRandomRule/icon_*_<id>), so rules above LAST_SHOWN get ratio 0 and are never drawn (rule 54 would show
"#CSO_RandomRule_Title_54").

Usage: python gen_random_rules.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "tables", "resource", "ModeEvent")
LAST_SHOWN = 53  # the highest rule id this client can show

HEADER = "ID,ratio,apply2,name,param1,param2,param3,param4,param5"

HERO = """\
1,35,1,fireball,450,1500,300,600,
2,75,0,weaponshare,,,,,
3,35,1,death_explosion,40,7,,,
4,0,0,unity_is_strength,55304,1,100,,
5,0,0,spread_of_virus,66365,1,10,,
6,85,2,heroes_rise,,,,,
7,35,2,multiple restrictions,0.35,0.5,,,
8,10,2,Sara_time,,,,,
9,0,0,close_combat,378053,1,,,
10,0,0,Chance_of_resurrection,,,,,
11,0,0,Human Zombie Alternate,0.5,,,,
12,25,0,precision shooting,1,,,,
13,50,0,pistol expert,3,,,,
14,50,0,Sniper_rifle_expert,3,,,,
15,25,0,bomb expert,3.25,6,10,,
16,50,0,Equipment expert,2.25,,,,
17,0,0,classic expert,2,,,,
18,35,0,Headhunter,3,,,,
19,0,0,Fighting near and far,635479,3,0.7,,
20,50,0,Flotation Expert,3,,,,
21,0,0,road of thorns,2.25,1,100,,
22,50,0,run human,2.25,0.7,1,100,
23,0,0,Squat Expert,2.25,,,,
24,60,2,random weapon,,,,,
25,60,2,dragon among men,,,,,
26,35,2,ghostly shadows,0.2,50,,,
27,75,0,Fried chicken delivery,20,10,1.75,,
28,100,0,head so strong,,,,,
29,90,0,shotgun battle,2.85,,,,
30,40,0,mobile strike team,3,,,,
31,0,2,Spacewalk,,,,,
32,25,1,lower posture,0.5,1000,,,
33,25,1,double jump,500,,,,
34,25,2,Boomer Battle,1.25,3,3,,
35,0,2,Secrets of top students,1,,,,
36,40,0,One shot left,4.45,,,,
37,35,2,bouncing,2,1,390,,
38,75,0,brute force attack,0.2,4,,,
39,15,0,Attack from behind,3,,,,
40,35,1,zombie cooltime,5,10,15,,
41,35,1,revenant,2,0.5,15000,5000,
42,25,2,Resident Battle,,,,,
43,50,2,gravity,120,,,,
44,25,0,The price of bullets,10,10.5,,,
45,35,0,the power behind,3.25,,,,
46,40,0,Multiples of 3,1.42,2,2.65,,
47,25,1,shock wave,10,130,170,3,25
48,35,0,shopping addiction,,,,,
49,60,2,Focus on vision,1.42,,,,
50,25,1,the power of cooperation,6,15,,,
51,65,0,Rifleman,2.25,,,,
52,25,0,baptism of bullets,2.25,,,,
53,50,0,cp,1,20,20000,,
54,25,2,I am the legend,6,10000,10000,3,
55,15,2,first touch,,,,,
56,60,0,Civilian Artifact,3,,,,
57,60,0,Accuracy expert,2,,,,
58,50,0,sprint expert,3,40,,,
59,35,0,skill master,,,,,
60,25,2,biochemical attack,160,4,295,15,25
61,85,0,Unity is powerful,6,0.14,13,,
"""

CLASSIC = """\
1,25,1,fireball,300,800,300,600,
2,100,0,weaponshare,,,,,
3,30,1,death_explosion,40,7,,,
4,45,0,unity_is_strength,55304,1,100,,
5,60,0,spread_of_virus,66365,1,10,,
6,0,2,heroes_rise,,,,,
7,45,2,multiple restrictions,0.1,0.5,,,
8,0,2,Sara_time,,,,,
9,10,0,close_combat,378053,1,,,
10,80,2,Chance_of_resurrection,,,,,
11,60,0,Human Zombie Alternate,0.5,,,,
12,45,0,precision shooting,1.47495,,,,
13,30,0,pistol expert,5,,,,
14,30,0,Sniper_rifle_expert,5,,,,
15,30,0,bomb expert,20,120,10,,
16,0,0,Equipment expert,5,,,,
17,0,0,classic expert,2,,,,
18,15,0,Headhunter,4,,,,
19,15,0,Fighting near and far,635479,4,0.5,,
20,70,0,Flotation Expert,4,,,,
21,45,0,road of thorns,5,1,100,,
22,30,0,run human,5,0.5,1,100,
23,55,0,Squat Expert,3,,,,
24,25,2,random weapon,,,,,
25,0,2,dragon among men,,,,,
26,15,2,ghostly shadows,0.2,50,,,
27,55,0,Fried chicken delivery,20,10,3,,
28,100,0,head so strong,,,,,
29,25,0,shotgun battle,3,,,,
30,55,0,mobile strike team,4,,,,
31,30,2,Spacewalk,,,,,
32,40,1,lower posture,1,200,,,
33,30,1,double jump,500,,,,
34,0,2,Boomer Battle,1.2,3,3,,
35,0,2,Secrets of top students,1,,,,
36,25,0,One shot left,4,,,,
37,60,2,bouncing,2,1,390,,
38,40,0,brute force attack,0.2,4,,,
39,25,0,Attack from behind,4,,,,
40,0,1,zombie cooltime,5,10,15,,
41,30,1,revenant,2,0.5,15000,5000,
42,0,2,Resident Battle,,,,,
43,55,2,gravity,120,,,,
44,40,0,The price of bullets,10,50,,,
45,55,0,the power behind,4,,,,
46,15,0,Multiples of 3,2,4,7,,
47,30,1,shock wave,10,130,170,3,25
48,0,0,shopping addiction,,,,,
49,45,2,Focus on vision,2,,,,
50,30,1,the power of cooperation,6,15,,,
51,45,0,Rifleman,3.75,,,,
52,10,0,baptism of bullets,3.5,,,,
53,30,0,cp,1,5,20000,,
54,0,2,I am the legend,6,10000,10000,3,
55,0,2,first touch,,,,,
56,40,0,Civilian Artifact,4,,,,
57,30,0,Accuracy expert,60,,,,
58,25,0,sprint expert,5,120,,,
59,0,0,skill master,,,,,
60,0,2,biochemical attack,160,4,295,15,25
61,60,0,Unity is powerful,6,0.6,13,,
"""


def rows(text):
    out = []
    for line in text.splitlines():
        cells = line.split(",")
        assert len(cells) == 9, line
        if int(cells[0]) > LAST_SHOWN:
            cells[1] = "0"
        out.append(",".join(cells))
    assert [int(r.split(",")[0]) for r in out] == list(range(1, 62))
    return out


def write(name, text):
    lines = [HEADER] + rows(text)
    path = os.path.join(OUT, name)
    with open(path, "w", newline="") as f:
        f.write("\n".join(lines) + "\n")
    drawn = sum(1 for r in lines[1:] if float(r.split(",")[1]) > 0)
    print(f"{path}: {len(lines) - 1} rules, {drawn} can be drawn")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    write("RandomRule_Dedi.csv", HERO)
    write("RandomRule_Classic_Dedi.csv", CLASSIC)
