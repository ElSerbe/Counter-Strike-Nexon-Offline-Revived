"""Writes tables/resource/zombi/ZombieSkillProperty_Dedi/ZombieSkillProperty_<Skill>.csv: the zombie class skills.

hw.dll's ZombieSkillTable (SharedDataMgr +0x1778, vtable 0x2c21ac4) loads these 32 files (vfunc 1, 0x26af410) only
on a dedicated server, and Nexon kept them there: offline the table was empty, every zombie skill did nothing
("useitem zb_skill0" finds no row) and the skills that check their row every frame crashed the game (Stamper,
Banshee, Flying zombie...). botassist runs the loader itself and serves these files (file overrides).

Each row is keyed by (Mode, EvolveType, Round). The lookup (vfunc 2, 0x26af140) takes (game mode, the zombie's
evolution level, round), maps modes 0x18/0x29/0x2c/0x37 to 14 and 0x23/0x24/0x2b to 0x20, then falls back to
round 0, mode 14, mode 14 round 0 and finally mode 14 / level 0 / round 0. So rows for mode 14, round 0 and levels
0..2 serve every zombie mode. Every column below must exist (a missing one throws and the game closes); Mode, Round,
EvolveType and Trap's NumTrap are integers, the rest numbers. Columns come from the row converters (hw 0x26b0050..):
times in seconds, distances in units, "...ReduceRate" a fraction taken off (0 none, 0.3 = 30 % less; buff field
+0x104, clamped 0..1).

Which class uses which skill (mp 0x1093da60, class: G skill [second skill]): 0 Regular: Crazy (berserk) - 1 Light:
Cloacking [JumpBuff] - 2 Heavy: Trap [ArmorUp] - 3 Psycho: Smoke [Heal] - 4 Voodoo: Heal [Heal] - 5 Deimos: Shock
[Heal] - 6 Ganymede: Rush [ArmorUp] - 7 Undertaker: Pile [ArmorUp] - 8 Banshee: Bat [JumpBuff] - 9 Jiangshi:
Stiffen [Heal] - 10 Boomer: SelfDestruct [ArmorUp] - 11 Sting Finger: Penetration [JumpBuff] - 12 Revival
[ShieldBuf] - 13 Telleport [ShieldBuf] - 14 Booster: Boost [JumpBuff] - 15 Bomb carrier: BombCreate [Crazy] -
16 Flying: Flying [Crazy] - 17 Aksha: Fireball [Heal] - 21: Shield [Crazy].
Nexon's values are unknown: these follow the wiki where it says something and are moderate guesses elsewhere.
Usage: python gen_zombie_skills.py
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent / "tables" / "resource" / "zombi" / "ZombieSkillProperty_Dedi"
LEVELS = (0, 1, 2)  # evolution levels: origin, host, evolved

# column order as the game reads it (after Mode, Round, EvolveType)
COLUMNS = {
    "Crazy": "Cooltime Endtime Damage DamageRadius DamageHeight DamageTick SwingDist SwingAngle DamageReduceRate "
             "VelModKnockbackReduceRate HealthReduction",
    "JumpBuff": "Cooltime Endtime DamageReduceRate VelModKnockbackReduceRate",
    "ArmorUp": "Cooltime ArmorPoint Radius RadiusArmorPoint",
    "Heal": "Cooltime HealthPoint Radius RadiusHealthPoint",
    "ShieldBuf": "Cooltime Endtime ShieldPoint Range",
    "Cloacking": "Cooltime Endtime Alpha RecoverSpeed Damage DamageRadius DamageHeight DamageTick ExposureAlpha "
                 "ExposureRadius ExposureHeight DamageReduceRate VelModKnockbackReduceRate",
    "Trap": "Cooltime Lifetime Velocity NumTrap Holdtime RenderAlpha ThrowSpeed Damage DamageTick Radius "
            "DamageReduceRate VelModKnockbackReduceRate",
    "Smoke": "Cooltime Endtime Radius DamageReduceRate NumSmoke HumanSight Damage DamageTick VelModKnockbackReduceRate "
             "DamageReduceRateHuman DamageReduceRateBuff VelModKnockbackReduceRateBuff",
    "VoodooHeal": "Cooltime Endtime Distance Amount Tick DamageReduceRate VelModKnockbackReduceRate MoveSpeedBonus "
                  "AmountSelf DamageReduceRateSelf VelModKnockbackReduceRateSelf MoveSpeedBonusSelf HealRate "
                  "RecoverArmor",
    "Shock": "Cooltime FlyVel KillTime Radius ShockTime Damage Speed SpeedEndtime DamageReduceRate "
             "VelModKnockbackReduceRate Endtime Size TailCnt TailLockOnDist TailLockOnAngle",
    "Rush": "Cooltime Endtime Damage DamageRadius DamageHeight DamageReduceRate VelModKnockbackReduceRate "
            "HealthReduction",
    "Pile": "Cooltime Lifetime HP StompRange StompSlow Damage ArmorHealAmount ArmorHealTick ArmorHealRadius TickSlow "
            "DamageReduceRate VelModKnockbackReduceRate KnockbackPower",
    "Bat": "Cooltime Lifetime PullPower Speed Radius Damage DamageTick DamageTouch DamageReduceRate "
           "VelModKnockbackReduceRate Scale_X Scale_Y Scale_Z",
    "Stiffen": "Cooltime Endtime Damage DamageRadius DamageHeight DamageTick DamageReduceRate "
               "VelModKnockbackReduceRate HealthReduction",
    "SelfDestruct": "Cooltime Endtime Damage Radius KickPowerRatio DamageRatioConst DamageReduceRate VelModReduceRate "
                    "KnockbackReduceRate",
    "Penetration": "Cooltime Distance Damage",
    "Revival": "Cooltime Endtime Damage DamageRadius DamageHeight DamageTick VelModKnockbackReduceRate MoveSpeedBonus "
               "Heal",
    "Telleport": "Cooltime ThrowVelocity Damage DamageRadius DamageHeight DamageTick",
    "Boost": "SpeedThreshold GaugeMax GaugeTick GaugeForUseMin GaugeInterval GaugeTimeCharge SpeedMin SpeedMax "
             "SpeedMaxBonus Damage DamageRadius DamageHeight DamageTick DamageReduceRate VelModKnockbackReduceRate "
             "Endtime",
    "BombCreate": "CreateHP DropLimit Damage",
    "Flying": "Cooltime WingHP FlyingPower FlyingPower2 JumpEnd FlyGravity FlySpeed DiveSpeed DiveLimit "
              "DiveIgnoreKnockback",
    "Fireball": "Cooltime HP Speed Damage DmgNeed DmgDist KnockbackPower KnockbackRatio DamageReduceRate "
                "VelModKnockbackReduceRate BuffEndTime",
    "DogShoot": "Cooltime ShootAngle ShootSpeed ShootDelay DogHealth DogArmor DogMoveSpeed DogGravity DogDamage "
                "DogDamageDist DogCycleTime DogDuration DogDetectRadius DogActiveRadius DogWaitRadius "
                "DamageReduceRate VelModKnockbackReduceRate BuffEndTime",
    "ViolentRush": "Cooltime Duration RushSpeed RushDamage KnockBackMeter KnockBackDamage KnockBackPushPower "
                   "KnockBackUpPower AirborneMeter AirbornePower FixMeter StopAngle CorrectRate HoldAngle HoldMeter "
                   "DamageReduceRate VelModKnockbackReduceRate",
    "WebShooter": "Cooltime ShootDuration ShootSpeed WireDuration WireSpeed WireCloseMeter WireReduceMeter "
                  "WireReduceRatio MaxMeter DamageReduceRate VelModKnockbackReduceRate",
    "WebBomb": "Cooltime ShootSpeed ExplodeRadius ExplodeDamage SlowRadius SlowSpeed SlowGravity SlowDuration "
               "DamageReduceRate VelModKnockbackReduceRate BuffEndTime",
    "Protect": "Cooltime GaugeMax GaugeStart GaugeUseTickTime GaugeUsePerTick GaugeAddDelayTime GaugeAddTickTime "
               "GaugeAddPerTick DamageReduceRate VelModKnockbackReduceRate BuffEndTime",
    "ChargeSlash": "Cooltime UseDelay MaxDistMeter PullRadius Damage PullPowerXY PullPowerZ EndDelay DamageReduceRate "
                   "VelModKnockbackReduceRate BuffEndTime",
    "Claw": "Cooltime FlyVel KillTime Damage Size StartOffset",
    "Confuse": "Cooltime Confusetime ShootSpeed ConfuseSequenceRunRadius ConfuseSequenceWalkRadius",
    "Defending": "Cooltime Endtime DamageReduceRate VelModKnockbackReduceRate",
    "Shield": "Cooltime Duration Amount1 DamageReduceRate1 VelModKnockbackReduceRate1 Amount2 DamageReduceRate2 "
              "VelModKnockbackReduceRate2 Amount3 DamageReduceRate3 VelModKnockbackReduceRate3 MaxSpeed",
}


def default(col):
    """a moderate value from the column's name, for columns a skill below doesn't set"""
    c = col.lower()
    if c == "cooltime":
        return 12
    if "reducerate" in c:
        return 0
    if c.endswith("tick") or "ticktime" in c or c == "tick":
        return 1
    if "ratio" in c or c.startswith("scale_") or c in ("tickslow", "stompslow", "slowspeed", "slowgravity"):
        return 1
    if "time" in c or c in ("duration", "lifetime", "holdtime", "endtime", "confusetime"):
        return 5
    if c.startswith("num") or c.endswith("cnt") or c == "droplimit":
        return 1
    if "damage" in c or c in ("healthreduction", "dmgneed"):
        return 0
    if "radius" in c or "range" in c or "dist" in c or "meter" in c or "height" in c or c == "size":
        return 150
    if "speed" in c or "vel" in c or "power" in c:
        return 500
    if "gauge" in c:
        return 100
    if "alpha" in c:
        return 30
    if "angle" in c:
        return 45
    if "bonus" in c:
        return 0.1
    return 300  # health / armour / amounts


# per skill: column -> value, or (origin, host, evolved)
SKILLS = {
    # berserk: faster, costs 500 health (only the Regular zombie pays it)
    "Crazy": {"Cooltime": 10, "Endtime": (8, 10, 10), "HealthReduction": 500, "SwingDist": 0, "SwingAngle": 0,
              "DamageRadius": 0, "DamageHeight": 0},
    "JumpBuff": {"Cooltime": 15, "Endtime": 5},
    "ArmorUp": {"Cooltime": 30, "ArmorPoint": (500, 800, 1000), "Radius": 300, "RadiusArmorPoint": (200, 300, 400)},
    "Heal": {"Cooltime": 15, "HealthPoint": (1000, 1500, 2000), "Radius": 300, "RadiusHealthPoint": (500, 700, 1000)},
    "ShieldBuf": {"Cooltime": 30, "Endtime": 5, "ShieldPoint": 1000, "Range": 300},
    # invisible while still / walking, seen again when attacking close
    "Cloacking": {"Cooltime": 15, "Endtime": (10, 12, 15), "Alpha": 15, "RecoverSpeed": 100, "DamageRadius": 0,
                  "DamageHeight": 0, "ExposureAlpha": 80, "ExposureRadius": 100, "ExposureHeight": 50},
    # traps humans in place for a few seconds
    "Trap": {"Cooltime": 15, "Lifetime": 60, "Velocity": 0, "NumTrap": (1, 2, 3), "Holdtime": (3, 4, 4),
             "RenderAlpha": 80, "ThrowSpeed": 0, "Radius": 40},
    # a smoke cloud that blinds humans
    "Smoke": {"Cooltime": 15, "Endtime": 10, "Radius": 200, "NumSmoke": 1, "HumanSight": 100},
    "VoodooHeal": {"Cooltime": 15, "Endtime": 5, "Distance": 300, "Amount": 500, "Tick": 1, "AmountSelf": 500,
                   "HealRate": 1, "RecoverArmor": 0, "MoveSpeedBonus": 0, "MoveSpeedBonusSelf": 0},
    # a projectile that makes the human drop their weapon
    "Shock": {"Cooltime": 15, "FlyVel": 1200, "KillTime": 2, "Radius": 50, "ShockTime": 2, "Speed": 1,
              "SpeedEndtime": 0, "Endtime": 2, "Size": 1, "TailCnt": 0, "TailLockOnDist": 0, "TailLockOnAngle": 0},
    "Rush": {"Cooltime": 15, "Endtime": 5, "Damage": 0, "DamageRadius": 0, "DamageHeight": 0, "HealthReduction": 0},
    # a coffin wall that slows humans around it
    "Pile": {"Cooltime": 15, "Lifetime": 15, "HP": (1000, 1500, 2000), "StompRange": 200, "StompSlow": 0.6,
             "ArmorHealAmount": 100, "ArmorHealTick": 1, "ArmorHealRadius": 200, "TickSlow": 0.8,
             "KnockbackPower": 300},
    # a bat that grabs a human and pulls them in
    "Bat": {"Cooltime": 15, "Lifetime": 4, "PullPower": 350, "Speed": 600, "Radius": 40, "DamageTouch": 0},
    # hardening: takes less damage and knockback for a while
    "Stiffen": {"Cooltime": 15, "Endtime": 5, "DamageRadius": 0, "DamageHeight": 0, "DamageReduceRate": 0.5,
                "VelModKnockbackReduceRate": 0.5, "HealthReduction": 0},
    # explodes, pushing humans away
    "SelfDestruct": {"Cooltime": 20, "Endtime": 1, "Damage": 100, "Radius": 250, "KickPowerRatio": 1,
                     "DamageRatioConst": 1},
    "Penetration": {"Cooltime": 15, "Distance": 300, "Damage": 100},
    "Revival": {"Cooltime": 30, "Endtime": 5, "DamageRadius": 0, "DamageHeight": 0, "MoveSpeedBonus": 0.1,
                "Heal": 1000},
    "Telleport": {"Cooltime": 20, "ThrowVelocity": 800, "DamageRadius": 0, "DamageHeight": 0},
    "Boost": {"SpeedThreshold": 250, "GaugeMax": 100, "GaugeTick": 1, "GaugeForUseMin": 30, "GaugeInterval": 0.1,
              "GaugeTimeCharge": 5, "SpeedMin": 250, "SpeedMax": 400, "SpeedMaxBonus": 50, "DamageRadius": 0,
              "DamageHeight": 0, "Endtime": 5},
    "BombCreate": {"CreateHP": 1000, "DropLimit": 3, "Damage": 0},
    "Flying": {"Cooltime": 15, "WingHP": 1000, "FlyingPower": 300, "FlyingPower2": 200, "JumpEnd": 3,
               "FlyGravity": 0.5, "FlySpeed": 300, "DiveSpeed": 800, "DiveLimit": 2, "DiveIgnoreKnockback": 1},
    "Fireball": {"Cooltime": 15, "HP": 500, "Speed": 900, "Damage": 50, "DmgNeed": 0, "DmgDist": 150,
                 "KnockbackPower": 300, "KnockbackRatio": 1, "BuffEndTime": 3},
    "DogShoot": {"Cooltime": 20, "ShootAngle": 30, "ShootSpeed": 600, "ShootDelay": 0.5, "DogHealth": 500,
                 "DogArmor": 0, "DogMoveSpeed": 300, "DogGravity": 1, "DogDamage": 20, "DogDamageDist": 60,
                 "DogCycleTime": 1, "DogDuration": 10, "DogDetectRadius": 600, "DogActiveRadius": 400,
                 "DogWaitRadius": 100, "BuffEndTime": 3},
    "ViolentRush": {"Cooltime": 20, "Duration": 2, "RushSpeed": 600, "RushDamage": 50, "KnockBackMeter": 100,
                    "KnockBackDamage": 20, "KnockBackPushPower": 500, "KnockBackUpPower": 200, "AirborneMeter": 100,
                    "AirbornePower": 300, "FixMeter": 50, "StopAngle": 60, "CorrectRate": 0.5, "HoldAngle": 45,
                    "HoldMeter": 50},
    "WebShooter": {"Cooltime": 15, "ShootDuration": 1, "ShootSpeed": 1500, "WireDuration": 3, "WireSpeed": 800,
                   "WireCloseMeter": 50, "WireReduceMeter": 50, "WireReduceRatio": 0.5, "MaxMeter": 1000},
    "WebBomb": {"Cooltime": 15, "ShootSpeed": 900, "ExplodeRadius": 200, "ExplodeDamage": 20, "SlowRadius": 200,
                "SlowSpeed": 0.5, "SlowGravity": 1, "SlowDuration": 3, "BuffEndTime": 3},
    "Protect": {"Cooltime": 15, "GaugeMax": 100, "GaugeStart": 100, "GaugeUseTickTime": 0.1, "GaugeUsePerTick": 2,
                "GaugeAddDelayTime": 2, "GaugeAddTickTime": 0.1, "GaugeAddPerTick": 1, "DamageReduceRate": 0.5,
                "BuffEndTime": 3},
    "ChargeSlash": {"Cooltime": 15, "UseDelay": 0.5, "MaxDistMeter": 300, "PullRadius": 200, "Damage": 50,
                    "PullPowerXY": 400, "PullPowerZ": 200, "EndDelay": 0.5, "BuffEndTime": 3},
    "Claw": {"Cooltime": 15, "FlyVel": 1200, "KillTime": 2, "Damage": 50, "Size": 1, "StartOffset": 20},
    "Confuse": {"Cooltime": 15, "Confusetime": 3, "ShootSpeed": 900, "ConfuseSequenceRunRadius": 200,
                "ConfuseSequenceWalkRadius": 100},
    "Defending": {"Cooltime": 15, "Endtime": 5, "DamageReduceRate": 0.5, "VelModKnockbackReduceRate": 0.5},
    # three shield charges, each one taking part of the damage
    "Shield": {"Cooltime": 15, "Duration": 6, "Amount1": 500, "DamageReduceRate1": 0.3,
               "VelModKnockbackReduceRate1": 0.3, "Amount2": 500, "DamageReduceRate2": 0.3,
               "VelModKnockbackReduceRate2": 0.3, "Amount3": 500, "DamageReduceRate3": 0.3,
               "VelModKnockbackReduceRate3": 0.3, "MaxSpeed": 260},
}


def fmt(v):
    return str(int(v)) if float(v).is_integer() else f"{v:g}"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for skill, cols in COLUMNS.items():
        cols = cols.split()
        values = SKILLS.get(skill, {})
        unknown = set(values) - set(cols)
        assert not unknown, (skill, unknown)
        lines = [",".join(["Mode", "Round", "EvolveType"] + cols)]
        for i, level in enumerate(LEVELS):
            row = [14, 0, level]
            for c in cols:
                v = values.get(c, default(c))
                row.append(v[i] if isinstance(v, tuple) else v)
            lines.append(",".join(fmt(v) for v in row))
        path = OUT / f"ZombieSkillProperty_{skill}.csv"
        path.write_bytes(("\r\n".join(lines) + "\r\n").encode("ascii"))
    print(f"{len(COLUMNS)} tables written to {OUT}")


if __name__ == "__main__":
    main()
