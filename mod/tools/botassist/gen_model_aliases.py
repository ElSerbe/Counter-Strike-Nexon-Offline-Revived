"""Writes botassist_model_aliases.txt: weapons whose models this client doesn't have, and the model to show instead.

Some weapons of Item.csv name models the game files don't contain (mp.dll asks for models/v_<res>.mdl). Most of them
are weapon skins (the Trans-Gear 19s1*, the Halloween y19s4*, the "b" editions...): no model of their own, hw.dll's
skin table loads the base weapon's model with the skin's textures (models/texture/@<res>_<texture>); those are left
to the game (an alias could stop it from using the skin). The others (7 in 2026-11) were invisible. botassist
(model_aliases.inl) opens the alias's model instead when the game asks for a missing one (v_, p_ and w_), so they
show as their base weapon (the plain TAR-21 for the Halloween TAR-21...). One line per weapon:
"<missing resource> <alias resource>".

The alias is the first of these whose v_ model exists: the name without its season prefix (y19s4tar21 -> tar21) or
its "b" edition letter (bpython -> python, balrog7b -> balrog7), without "ex"/a trailing number, a few known ones,
else a plain weapon of the same type (pistol deagle, shotgun m3, SMG mp5, rifle ak47, machine gun m249, melee knife,
grenade hegrenade). Files are checked with tools/nartool/nartool.exe (the game's own file system: .nar and Packer).

Usage: python gen_model_aliases.py   (needs the game; CSNZ_GAME or the developer's install)"""
import csv
import os
import sys
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(os.path.dirname(HERE))
GAME = os.environ.get("CSNZ_GAME") or r"D:\SteamLibrary\steamapps\common\CSNZ"
NARTOOL = os.path.join(MOD, "tools", "nartool", "nartool.exe")
ITEM_CSV = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data", "Item.csv")
KNOWN = {"bbison": ["bison"], "y19s4hammer": ["hammer"], "19s1infinityex2": ["infinityex2"], "y19s2mauserc96": ["mauserc96"],
         "19s1usas12": ["usas12camo"], "y24s2skull2": ["skull2"], "y24s3crow9": ["crow9"], "y26s1balrog9": ["balrog9"],
         "y24s2mg3": ["mg3"], "y25s1turbulent1": ["turbulent1"], "y20s2scarb": ["scar"], "y24s1nataknife": ["nataknife"],
         "y23s3katanad": ["katanad", "katana"], "y25s4katana": ["katana"], "y23s2tomahawk": ["tomahawk"],
         "y23s1crowbarcraft": ["crowbarcraft"], "y24s2hammer": ["hammer"], "nataknifesnow": ["nataknife", "knife"]}
FALLBACK = {"1": "deagle", "2": "m3", "3": "mp5", "4": "ak47", "5": "m249", "6": "hegrenade", "11": "knife"}


def candidates(res, cat):
    out = list(KNOWN.get(res, []))
    s = re.sub(r"^(y\d\ds\d|\d\ds\d)", "", res)
    if s != res:
        out.append(s)
    if res.startswith("b") and len(res) > 3:
        out.append(res[1:])
    if res.endswith("b"):
        out.append(res[:-1])
    for suffix in ("ex", "zf", "snow", "venom", "w"):
        if res.endswith(suffix) and len(res) > len(suffix) + 2:
            out.append(res[: -len(suffix)])
    m = re.match(r"([a-z]+)\d+$", s)
    if m:
        out.append(m.group(1))
    out.append(FALLBACK.get(cat, "knife"))
    seen = []
    for c in out:
        if c and c != res and c not in seen:
            seen.append(c)
    return seen


def existing(paths):
    """the game paths that exist (nartool extracts them into a temporary folder)"""
    out = tempfile.mkdtemp(prefix="aliases_")
    found = set()
    for i in range(0, len(paths), 150):
        r = subprocess.run([NARTOOL, os.path.join(GAME, "Bin"), GAME, out] + paths[i:i + 150], capture_output=True, text=True)
        found |= {m.group(1) for m in re.finditer(r"^\[ok\] (\S+)", r.stdout, re.M)}
    return found


mp = open(os.path.join(GAME, "Bin", "mp.dll"), "rb").read().lower()
weapons = []
for r in csv.reader(open(ITEM_CSV, encoding="latin1")):
    if r and r[0].isdigit() and len(r) > 5 and r[3] in ("Equipment", "PeriodicItem") and r[4] in FALLBACK:
        res = r[2].lower()
        if ("models/v_%s.mdl" % res).encode() in mp:
            weapons.append((res, r[4]))
weapons = sorted(set(weapons))
# weapon skins: the game shows them itself (export_shop_assets reads hw.dll's skin table; a "@<res>_" texture prefix)
sys.path.insert(0, os.path.join(MOD, "tools"))
sys.argv = sys.argv[:1]
import export_shop_assets  # noqa: E402

skins = export_shop_assets.skin_redirects()
engine = open(os.path.join(GAME, "Bin", "hw.dll"), "rb").read().lower() + open(os.path.join(GAME, "Bin", "client.dll"), "rb").read().lower()
weapons = [(res, cat) for res, cat in weapons
           if "models/v_%s.mdl" % res not in skins and ("@%s_" % res).encode() not in engine]
have = existing(["models/v_%s.mdl" % res for res, _ in weapons])
missing = [(res, cat) for res, cat in weapons if "models/v_%s.mdl" % res not in have]
cands = {res: candidates(res, cat) for res, cat in missing}
have |= existing(sorted({"models/v_%s.mdl" % c for cs in cands.values() for c in cs}))
lines = []
for res, cat in missing:
    alias = next((c for c in cands[res] if "models/v_%s.mdl" % c in have), None)
    if alias:
        lines.append(f"{res} {alias}")
    else:
        print("no model to show for", res)
with open(os.path.join(HERE, "botassist_model_aliases.txt"), "w", encoding="ascii") as f:
    f.write("; weapons whose models this client lacks: <missing resource> <resource whose models are shown instead>\n")
    f.write("; generated by gen_model_aliases.py (botassist: model_aliases.inl applies them to v_, p_ and w_ models)\n")
    for line in lines:
        f.write(line + "\n")
print(len(weapons), "weapons with their own model (skins left out),", len(missing), "missing,", len(lines), "aliased")
