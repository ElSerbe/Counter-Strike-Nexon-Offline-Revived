"""All Star (mode 40): writes tables/resource/allstar/AllStar_Status-Dedi.csv and AllStar_Skill-Dedi.csv, the characters' numbers. Nexon kept
these two files on its dedicated servers: the game only loads them there (mp.dll 0x1082bf40 and 0x1082bca0), so on a listen server every
character had 0 health and no model. botassist makes the game load them (see modes/all_star.inl).

Nexon's values are unknown. Health and armor follow what a 2017 video of the mode shows (https://www.youtube.com/watch?v=k4vrXqxH-YU: XT-300 4500/3000,
light zombie 3500/2000, soldiers about 1000/1000) and the game's class panel (resource/allstar/AllStar_CharInfo: "low HP", "slow", "moves swiftly").
mp.dll multiplies gun damage by 4 here (0x1082a930).

Status columns, one row per character and team (id 1-20 as in resource/allstar/AllStar_CharInfo):
  0 id, 1 team, 2 model name, 3 health, 4 armor, 5 max speed, 6 gravity, 7 model path, 8 field of view,
  9 third person view, 10 added to the view height, 11-15 third person camera (client.dll's MPToCL 0xbf: cam_idealyaw, cam_idealdist, camera pitch,
  cam_thirdperson_offset, one more it doesn't read), 16-19 sent to the HUD, 20 skin (pev +0x120),
  21-22 flags (+0x3709, +0x370a), 23 flag (+0x3708), 24 weight of the random draw at respawn.
Skill columns: 0 id, 1 name, 2-16 fifteen numbers: what each pawn's attacks use (0x106591a0; see SKILLS). They are real values, not multipliers:
a 1 there means a bomb flying at speed 1 and doing 1 damage.

Usage: python gen_all_star.py"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "tables", "resource", "allstar")

# id, name, health, armor, speed, models: (name, path) for T and for CT
P = "models/player/%s/%s.mdl"
A = "models/allstar/%s/%s.mdl"


def player(name):
    return (name, P % (name, name))


def allstar(name):
    return (name, A % (name, name))


CHARACTERS = [
    (1, "hero", 1000, 1000, 260, player("hero"), player("hero")),
    (2, "heroine", 1000, 1000, 270, player("heroine"), player("heroine")),
    (3, "BazookaMan", 1000, 1000, 250, player("allstarbrtr"), player("allstarbrct")),
    (4, "speed_zombi_host", 3500, 2000, 320, player("speed_zombi_host"), player("speed_zombi_host")),
    (5, "z4_light", 3500, 2000, 310, player("z4_light"), player("z4_light")),
    (6, "allstarbr", 3000, 1000, 260, player("allstarbrtr"), player("allstarbrct")),
    (7, "joker", 500, 0, 300, player("joker"), player("joker")),
    (8, "allstarrevenant", 3000, 2000, 220, allstar("allstarrevenant"), allstar("allstarrevenant")),
    (9, "allstargoliath", 4500, 3000, 200, allstar("allstargoliath"), allstar("allstargoliath")),
    (10, "allstarsnowman", 4000, 3000, 260, allstar("allstarsnowmantr"), allstar("allstarsnowmanct")),
    (11, "allstarchicken", 500, 0, 340, allstar("allstarchicken"), allstar("allstarchicken")),
    (12, "allstarball", 15, 0, 320, allstar("allstarball"), allstar("allstarball")),
    (13, "allstarrex", 4000, 2000, 240, allstar("allstarrex"), allstar("allstarrex")),
    (14, "allstarmogi", 1000, 0, 320, allstar("allstarmogi"), allstar("allstarmogi")),
    (15, "allstardron", 1500, 1000, 320, allstar("allstardron"), allstar("allstardron")),
    (16, "allstarlightboss", 4000, 2000, 300, allstar("allstarlightboss"), allstar("allstarlightboss")),
    (17, "allstarhector", 4500, 3000, 210, allstar("allstarhector"), allstar("allstarhector")),
    (18, "allstarsnow", 2000, 0, 280, allstar("allstarsnow"), allstar("allstarsnow")),
    (19, "allstarfrozen", 5000, 3000, 250, allstar("allstarfrozen"), allstar("allstarfrozen")),
    (20, "allstarkartrider", 2000, 1000, 320,
     ("allstarkartrider_upper_tr", "models/allstar/allstarkartridertr/allstarkartrider_upper_tr.mdl"),
     ("allstarkartrider_upper_ct", "models/allstar/allstarkartriderct/allstarkartrider_upper_ct.mdl")),
]
# First person: the normal soldiers with base game weapons and the 2 light zombies (the video shows their arms; mp.dll 0x106641b7 only lets classes
# 4 and 5 keep their model with the flag off). The rest are third person.
FIRST_PERSON = {1, 2, 3, 4, 5, 6}
# the team's skin of the models that have one: Hector's are red and blue
TEAM_SKIN = {17: {1: 0, 2: 1}}
# who jumps high ("high mobility and jump capabilities", "move quickly using high jumps"). Fake players get it from botassist
GRAVITY = {4: 0.7, 5: 0.7, 14: 0.5, 15: 0.5}
# the camera is close and low in the video. Big ones further, small ones closer
CAMERA_DISTANCE = {7: 90, 8: 130, 9: 150, 10: 130, 11: 80, 12: 80, 13: 130, 14: 90, 16: 130, 17: 150, 19: 140}
CAMERA_DEFAULT, CAMERA_PITCH = 110, 15
# What each pawn does with its 15 numbers (nothing else reads them). Nexon's numbers are unknown: these are in the scale of the health above.
SKILLS = {
    # 4 LightZombieZB3 (0x10661b70): 5, 6 seconds to get each zombie grenade back
    4: [0, 0, 0, 0, 0, 8, 8],
    # 6 BattleRushMan: 4, 5 the rules' 2 settings (0x1082b890); other modes set them to 25 and 3000
    6: [0, 0, 0, 0, 25, 3000],
    # 8 Revenant (0x10664e10): slash damage, radius, reach; fireball damage, radius IN METERS (BaseMissile multiplies it by 39.37), speed
    8: [900, 160, 80, 1200, 5, 900],
    # 9 XT-300 (0x1065eba0): rockets: damage, seconds between bursts of 3, radius IN METERS; gun damage, seconds between shots
    9: [800, 3, 6, 60, 0.1],
    # 10 Beast (0x10659590): swing damage, radius, push up, push away; stomp damage, seconds between stomps, radius, push up, push away
    10: [800, 140, 150, 500, 1200, 8, 400, 300, 500],
    # 11 Chicken (0x1065b4e0): seconds between eggs
    11: [5],
    # 12 SoccerBall (0x106668e0): every so many seconds it scores this many points and loses that much health
    12: [5, 1],
    # 13 DrRex (0x1065bad0): tentacles: damage, rows, columns, radius, seconds between uses, row spacing; 6-11 the same for its stronger attack
    13: [300, 3, 3, 40, 4, 60, 500, 5, 3, 50, 8, 70],
    # 14 Mosquito (0x10661f40): seconds between bites of everyone near, radius, damage, health it drinks
    14: [0.5, 300, 150, 100],
    # 15 Drone (0x1065b9f0): damage per shot, seconds between shots
    15: [50, 0.1],
    # 16 SDLightZombieBoss (0x10665d50): 2 area attacks: damage, seconds between uses, radius, push up, push away (the second one, 5-9, plus its cone)
    16: [900, 5, 200, 300, 300, 700, 8, 400, 300, 300, 60],
    # 17 Hector (0x1065f880): damage per shot, seconds between shots: "slow but steady sniping"
    17: [700, 1.5],
    # 18 Snowman (0x10666780): its suicide blast: damage, radius
    18: [1500, 350],
    # 19 FrozenWrath (0x1065d860): swing damage, radius, angle, seconds per swing; thrown club damage, 2 of its timings, speed;
    # spin damage, radius, wind up, seconds between spins; seconds a combo waits for the next hit, hits to a spin
    19: [900, 180, 120, 0.8, 1000, 1.5, 1.5, 1000, 1500, 350, 1, 10, 3, 3],
    # 20 Kartrider (0x10660760): seconds at speed before the turbo is ready, that speed; turbo speed, seconds; seconds between bombs,
    # the bomb's speed up and down, how far it lands, seconds going up, seconds waiting, height it falls from; blast: seconds between hits,
    # hits, radius, damage, push
    20: [1, 200, 650, 3, 3, 800, 500, 0.4, 0.3, 300, 0.5, 4, 220, 300, 200],
}


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "AllStar_Status-Dedi.csv"), "w", newline="") as f:
        f.write(",".join("c%d" % i for i in range(25)) + "\r\n")
        for cid, _name, health, armor, speed, tr, ct in CHARACTERS:
            for team, (model, path) in ((1, tr), (2, ct)):
                third = cid not in FIRST_PERSON
                camera = [0, CAMERA_DISTANCE.get(cid, CAMERA_DEFAULT), CAMERA_PITCH, 0, 0] if third else [0] * 5
                skin = TEAM_SKIN.get(cid, {}).get(team, 0)
                row = [cid, team, model, health, armor, speed, GRAVITY.get(cid, 1), path, 90, int(third), 0] + camera + [0] * 4 + [skin, 1, 0, 0, 1]
                f.write(",".join(str(v) for v in row) + "\r\n")
    with open(os.path.join(OUT, "AllStar_Skill-Dedi.csv"), "w", newline="") as f:
        f.write(",".join("c%d" % i for i in range(17)) + "\r\n")
        for cid, name, *_ in CHARACTERS:
            numbers = SKILLS.get(cid, [])
            f.write(",".join(str(v) for v in [cid, name] + numbers + [0] * (15 - len(numbers))) + "\r\n")
    print(len(CHARACTERS), "characters")


if __name__ == "__main__":
    main()
