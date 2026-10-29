"""Writes tables/maps/ZBS_ENC_zsrift_splinter_dedi.json: Zombie Disaster Zone's random round events.

mp.dll (0x107633d0) reads maps/ZBS_ENC_<map>_dedi.json with jsoncons; Nexon kept the file on its dedicated servers,
so without it the mode had no hazards, special zombies or modifiers. botassist serves this one instead (file
overrides). Every round the mode draws rift level / ZSRiftEventDivisor events (botassist.ini; the game's 32), at most
10, each one weighted by Prob among those allowed: Prob > 0, not active yet, no active event of the same
ExceptionGroup (-1: none), MinAppearDifficulty <= rift level, and AppearFirstRound / AppearNormalRound /
AppearBossRound for the kind of round. Categories and their entries, in the order mp.dll reads them (each group must
list all of them: a missing key throws):
  Environment (any subset): Hurricane, Magneticfield, Highgravity, Petrifiedfigure, Firebomb, Airstrike, Vigilanteye
  Round_MonsterAdd: AddPassZombie, AddPsychoZombie, AddDeimosZombie, AddBoomerZombie, AddHealZombie,
      AddRevivalZombie, AddUndertakerZombie, AddGoldenZombie (ZombieType = id in maps/zbs_zt_zsrift_splinter)
  Round_MonsterChange: ChangeLightZombieRound, ChangeHeavyZombieRound, ChangeEliteZombieRound, ChangeMidBossRound
  Player: Primary/Secondary/Third/Melee_Attack_Decrease, Max_MoveSpeed_Limit, Weapon_Reset (all off here)
  Monster: Common_HP_Dec_Attack_Inc, Common_HP_Inc_Attack_Dec, MoveSpeed_Inc
Hazard values marked "by level" are lists of {"Difficulty": first rift level, "Value": v}: the entry with the highest
Difficulty not above the rift level applies. Damage = DamageBase + DamageIncrease per level (scaled by the game).

Usage: python gen_zsrift_events.py
"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parent / "tables" / "maps" / "ZBS_ENC_zsrift_splinter_dedi.json"


def lv(*pairs):
    """by level: lv(1, 2, 8, 3) -> 2 from level 1, 3 from level 8"""
    return [{"Difficulty": pairs[i], "Value": pairs[i + 1]} for i in range(0, len(pairs), 2)]


def common(prob, min_level=1, group=-1, first=True, normal=True, boss=True, **extra):
    return {"ExceptionGroup": group, "MinAppearDifficulty": min_level, "AppearNormalRound": normal,
            "AppearBossRound": boss, "AppearFirstRound": first, "Prob": prob, **extra}


def hazard(prob, min_level=1, delay=8.0, **extra):
    return common(prob, min_level, first=False, ActiveDelay=delay, **extra)


ENVIRONMENT = {
    "Hurricane": hazard(10, 3, SpawnCount=lv(1, 2, 8, 3, 15, 4), Radius=lv(1, 110), Height=lv(1, 220),
                        DamageIntervalTime=lv(1, 1.0), DamageBase=6, DamageIncrease=1, MoveSpeed=lv(1, 110, 10, 150),
                        Duration=lv(1, 20), RespawnDelay=lv(1, 15)),
    "Magneticfield": hazard(10, 2, SpawnCount=lv(1, 4), Size=lv(1, 140), DamageIntervalTime=lv(1, 1.0), DamageBase=8,
                            DamageIncrease=1.5),
    "Highgravity": hazard(6, 4, Gravity=lv(1, 1.6)),
    "Petrifiedfigure": hazard(8, 5, SpawnCount=lv(1, 6), SpawnRadius=lv(1, 300), EffectRadius=lv(1, 250),
                              DecMoveSpeed=lv(1, 30), DecMoveSpeedTickTime=lv(1, 1.0), MinMoveSpeed=lv(1, 120),
                              Duration=lv(1, 15), RespawnDelay=lv(1, 20)),
    "Firebomb": hazard(10, 1, SpawnCount=lv(1, 3, 10, 5), SpawnRadius=lv(1, 600), EffectRadius=lv(1, 150),
                       DamageIntervalTime=lv(1, 1.0), DamageBase=8, DamageIncrease=1, HealBase=20, HealIncrease=2,
                       HealthBase=300, HealthIncrease=30, Duration=lv(1, 30), RespawnDelay=lv(1, 20)),
    "Airstrike": hazard(8, 3, Radius=lv(1, 200), DamageBase=40, DamageIncrease=4, RespawnDelay=lv(1, 15),
                        LaunchDelay=lv(1, 5.0)),
    "Vigilanteye": hazard(6, 6, DamageIntervalTime=lv(1, 1.0), DamageBase=5, DamageIncrease=1, Duration=lv(1, 5),
                          RespawnDelay=lv(1, 12)),
}

# special zombies joining the round: (key, zombie type, kill points, first rift level, weight)
ADDS = [("AddPassZombie", 3, 1, 3, 8), ("AddPsychoZombie", 4, 1, 1, 12), ("AddDeimosZombie", 5, 1, 4, 8),
        ("AddBoomerZombie", 6, 1, 2, 10), ("AddHealZombie", 7, 1, 2, 10), ("AddRevivalZombie", 8, 2, 5, 6),
        ("AddUndertakerZombie", 9, 1, 1, 10), ("AddGoldenZombie", 12, 5, 1, 5)]
# rounds of one kind of zombie (one at a time: same ExceptionGroup)
CHANGES = [("ChangeLightZombieRound", 10, 1, 1, 6), ("ChangeHeavyZombieRound", 11, 1, 2, 6),
           ("ChangeEliteZombieRound", 8, 2, 4, 4), ("ChangeMidBossRound", 100, 15, 6, 3)]


def main() -> None:
    data = {
        "Environment": ENVIRONMENT,
        "Round_MonsterAdd": {"Type": 1, **{
            key: common(prob, level, SubType=i, ZombieType=zt, StartTime=5.0, EndTime=999999.0, IntervalTime=15.0,
                        SpawnCount=2, KillCount=kills)
            for i, (key, zt, kills, level, prob) in enumerate(ADDS)}},
        "Round_MonsterChange": {"Type": 2, **{
            key: common(prob, level, group=10, first=False, boss=False, SubType=i, ZombieType=zt, KillCount=kills)
            for i, (key, zt, kills, level, prob) in enumerate(CHANGES)}},
        # player penalties: off (Prob 0, never drawn); the group must still list every entry
        "Player": {"Type": 3,
                   "Primary_Attack_Decrease": common(0, 3, group=20, SubType=0, Value=0.3),
                   "Secondary_Attack_Decrease": common(0, 3, group=20, SubType=1, Value=0.3),
                   "Third_Attack_Decrease": common(0, 3, group=20, SubType=2, Value=0.3),
                   "Melee_Attack_Decrease": common(0, 3, group=20, SubType=3, Value=0.3),
                   "Max_MoveSpeed_Limit": common(0, 4, SubType=4, Value=100.0),
                   "Weapon_Reset": common(0, 8, first=False, boss=False, SubType=5)},
        "Monster": {"Type": 4,
                    "Common_HP_Dec_Attack_Inc": common(8, 1, group=30, SubType=0, HP_Ratio=0.7, Attack_Ratio=1.3),
                    "Common_HP_Inc_Attack_Dec": common(8, 1, group=30, SubType=1, HP_Ratio=1.3, Attack_Ratio=0.7),
                    "MoveSpeed_Inc": common(6, 2, SubType=2, MoveSpeed_Ratio=1.15)},
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(data, indent=1) + "\n", encoding="ascii", newline="\n")
    print(OUT)


if __name__ == "__main__":
    main()
