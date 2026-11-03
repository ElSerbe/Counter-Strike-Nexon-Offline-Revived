"""All Star (mode 40): writes tables/resource/allstar/AllStar_Status-Dedi.csv and AllStar_Skill-Dedi.csv, the characters' numbers. Nexon kept
these two files on its dedicated servers: the game only loads them there (mp.dll 0x1082bf40 and 0x1082bca0), so on a listen server every
character had 0 health and no model. botassist makes the game load them (see modes/all_star.inl).

Nexon's values are unknown: health and speed follow each character's role on the wiki, in the range of the other modes.

Status columns, one row per character and team (id 1-20 as in resource/allstar/AllStar_CharInfo):
  0 id, 1 team, 2 model name, 3 health, 4 armor, 5 max speed, 6 gravity, 7 model path, 8 field of view,
  9 third person view, 10 added to the view height, 11-15 third person camera (client.dll's MPToCL 0xbf: cam_idealyaw, cam_idealdist, camera pitch,
  cam_thirdperson_offset, one more it doesn't read), 16-19 sent to the HUD, 20 pev +0x120,
  21-22 flags (+0x3709, +0x370a), 23 flag (+0x3708), 24 weight of the random draw at respawn.
Skill columns: 0 id, 1 name, 2-16 fifteen numbers: upgrades that multiply a character's values (1: none); the Chicken reads its 2 as a time.

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
    (1, "hero", 1000, 100, 260, player("hero"), player("hero")),
    (2, "heroine", 900, 100, 270, player("heroine"), player("heroine")),
    (3, "BazookaMan", 1000, 100, 250, player("allstarbrtr"), player("allstarbrct")),
    (4, "speed_zombi_host", 1500, 0, 320, player("speed_zombi_host"), player("speed_zombi_host")),
    (5, "z4_light", 1200, 0, 310, player("z4_light"), player("z4_light")),
    (6, "allstarbr", 1000, 100, 260, player("allstarbrtr"), player("allstarbrct")),
    (7, "joker", 900, 0, 300, player("joker"), player("joker")),
    (8, "allstarrevenant", 3000, 100, 240, allstar("allstarrevenant"), allstar("allstarrevenant")),
    (9, "allstargoliath", 3000, 200, 220, allstar("allstargoliath"), allstar("allstargoliath")),
    (10, "allstarsnowman", 2500, 100, 260, allstar("allstarsnowmantr"), allstar("allstarsnowmanct")),
    (11, "allstarchicken", 500, 0, 300, allstar("allstarchicken"), allstar("allstarchicken")),
    (12, "allstarball", 800, 0, 320, allstar("allstarball"), allstar("allstarball")),
    (13, "allstarrex", 2500, 100, 240, allstar("allstarrex"), allstar("allstarrex")),
    (14, "allstarmogi", 800, 0, 300, allstar("allstarmogi"), allstar("allstarmogi")),
    (15, "allstardron", 1000, 100, 280, allstar("allstardron"), allstar("allstardron")),
    (16, "allstarlightboss", 2500, 0, 300, allstar("allstarlightboss"), allstar("allstarlightboss")),
    (17, "allstarhector", 3000, 100, 230, allstar("allstarhector"), allstar("allstarhector")),
    (18, "allstarsnow", 1200, 0, 280, allstar("allstarsnow"), allstar("allstarsnow")),
    (19, "allstarfrozen", 2500, 100, 250, allstar("allstarfrozen"), allstar("allstarfrozen")),
    (20, "allstarkartrider", 1500, 100, 320,
     ("allstarkartrider_upper_tr", "models/allstar/allstarkartridertr/allstarkartrider_upper_tr.mdl"),
     ("allstarkartrider_upper_ct", "models/allstar/allstarkartriderct/allstarkartrider_upper_ct.mdl")),
]
# First person: the characters that are normal soldiers with base game weapons, and the 2 light zombies: with the flag off the game gives a
# character a soldier's model except classes 4 and 5 (mp.dll 0x106641b7), so those two must have had it off. The rest are third person.
FIRST_PERSON = {1, 2, 3, 4, 5, 6}
# the camera's distance: Beast mode's is 140, pitch 30 (client.dll MPToCL 0x5c). Big ones further, small ones closer
CAMERA_DISTANCE = {8: 200, 9: 240, 10: 180, 11: 100, 12: 100, 13: 200, 14: 120, 15: 140, 16: 180, 17: 240, 19: 200, 20: 160}
CAMERA_DEFAULT, CAMERA_PITCH = 140, 30
CHICKEN = 11
CHICKEN_EGG_SECONDS = 5


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "AllStar_Status-Dedi.csv"), "w", newline="") as f:
        f.write(",".join("c%d" % i for i in range(25)) + "\r\n")
        for cid, _name, health, armor, speed, tr, ct in CHARACTERS:
            for team, (model, path) in ((1, tr), (2, ct)):
                third = cid not in FIRST_PERSON
                camera = [0, CAMERA_DISTANCE.get(cid, CAMERA_DEFAULT), CAMERA_PITCH, 0, 0] if third else [0] * 5
                row = [cid, team, model, health, armor, speed, 1, path, 90, int(third), 0] + camera + [0] * 4 + [0, 1, 0, 0, 1]
                f.write(",".join(str(v) for v in row) + "\r\n")
    with open(os.path.join(OUT, "AllStar_Skill-Dedi.csv"), "w", newline="") as f:
        f.write(",".join("c%d" % i for i in range(17)) + "\r\n")
        for cid, name, *_ in CHARACTERS:
            first = CHICKEN_EGG_SECONDS if cid == CHICKEN else 1
            f.write(",".join(str(v) for v in [cid, name, first] + [1] * 14) + "\r\n")
    print(len(CHARACTERS), "characters")


if __name__ == "__main__":
    main()
