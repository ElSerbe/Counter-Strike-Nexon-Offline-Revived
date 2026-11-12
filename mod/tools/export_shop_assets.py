"""Exports what the web shop needs from the game files: item icons (as PNG) and English item names.

Icons come from gfx/vgui/basket/<resource>.tga (the native shop's item pictures), names from
resource/cso_na_en.txt. Files are read with tools/nartool/nartool.exe, which uses the game's own FileSystem_Nar.dll.

3D models for the preview: the character (models/player/<res>/<res>.mdl) or the third-person weapon
(models/p_<res>.mdl), exported by mdl_export.py, plus the default characters of a new account. Costumes
(models/costume/<res>.mdl) are worn by the player's character: their root bone follows one of its bones (the web
viewer's COSTUME_ROOTS). Zombie skins are whole zombie models (models/player/<res>_origin/<res>_origin.mdl).

Usage:
  python export_shop_assets.py            items of the battle passes (Data/Warbonds.json), the Black Market
                                          (Data/Missions.json), the Rift Exchange (Data/RiftExchange.json)
                                          and every character
  python export_shop_assets.py --all      every weapon and character in Item.csv
Writes web/shop/icons/<itemID>.png, web/shop/models/* (+ models/index.json) and
server/CSNZ_Server/bin/Data/ItemNames.json.
"""
import csv
import atexit
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import mdl_export
import nar_index

MOD = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# the game folder (the one with Bin and Data): CSNZ_GAME, set by the launcher, or the developer's install
GAME = os.environ.get("CSNZ_GAME") or r"D:\SteamLibrary\steamapps\common\CSNZ"
SERVER_DATA = os.path.join(MOD, "server", "CSNZ_Server", "bin", "Data")
ICON_DIR = os.path.join(MOD, "web", "shop", "icons")
NARTOOL = os.path.join(MOD, "tools", "nartool", "nartool.exe")
MODEL_DIR = os.path.join(MOD, "web", "shop", "models")
PROMO_DIR = os.path.join(MOD, "web", "shop", "promo")
SOUND_DIR = os.path.join(MOD, "web", "shop", "sounds")
# shop sound -> game file: the weapon ascend gauge while an unlock is held, a calm "task complete" for items and
# the gachapon "weapon revealed" sting for battle passes
# item.wav: getting an item, the crafting window's soft "item made" chime (task_complete.wav, the old one, was too harsh)
SOUNDS = {"charge.wav": "sound/weaponascend/gauge2.wav", "item.wav": "sound/events/create/success.wav",
          "pass.wav": "sound/gachapon/open_weapon_item_top_sss.wav"}
DEFAULT_WEAPON = "m4a1"            # held by characters shown on their own
WEAPON_CATEGORIES = ("1", "2", "3", "4", "5", "6", "11")
COSTUME_CLASSES = ("HeadCostume", "BackCostume", "PelvisCostume", "FaceCostume", "PetCostume", "ArmCostume")
ARCHIVES = ("cstrike", "dstrike", "estrike", "fstrike")
# third-person models whose names can't be derived from the item's resource name (found in mp.dll / client.dll)
MODEL_OVERRIDES = {"ak47l": "models/p_ak47_long.mdl", "deagled": "models/p_ddeagle.mdl", "mp7a1d": "models/p_dmp7a1.mdl"}
# Weapon skins have no model of their own: hw.dll redirects them to the base weapon's model and loads its textures
# as models/texture/<prefix><name>. resource -> (base model, archive holding the model version the skin was
# painted for, {base texture name: skin texture}).
SKINS = {
    "xm1014snow": ("models/p_xm1014.mdl", "fstrike", {"xm1014.bmp": "models/texture/snow_xm1014.bmp"}),
    "nataknifesnow": ("models/p_strong_knife.mdl", "estrike", {"cso_knife.bmp": "models/texture/snow_cso_knife_p.bmp"}),
}


def load_items():
    items = {}
    header = []
    with open(os.path.join(SERVER_DATA, "Item.csv"), encoding="latin1") as f:
        for row in csv.reader(f):
            if row and row[0] == ";ID":
                header = [h.lstrip(";") for h in row]
            if row and row[0].isdigit() and len(row) > 4:
                items[int(row[0])] = {"name": row[1].strip(), "resource": row[2].strip().lower(), "class": row[3],
                                      "category": row[4].strip(), "row": dict(zip(header, row))}
    return items


def _num(value):
    m = re.match(r"\s*(-?\d+)", value or "")
    return int(m.group(1)) if m else 0


def item_info(item, loc):
    """Tooltip data like the native inventory: description, weapon sheet or character stats."""
    row, res = item["row"], item["resource"]
    info = {}
    desc = loc.get(f"cso_item_desc_{res}")
    if desc:
        info["desc"] = desc.replace("\\n", "\n")
    if item["class"] == "Class":
        info["kind"] = "character"
        info["team"] = {"1": "TR", "2": "CT"}.get(row.get("Team", "").strip(), "")
        info["character"] = {"hp": _num(row.get("Damage")), "attack": _num(row.get("HitRate")),
                             "speed": _num(row.get("Reaction")), "protection": _num(row.get("FiringSpeed")),
                             "ammo": _num(row.get("Weight"))}
    elif item["class"] == "Equipment" and item["category"] in WEAPON_CATEGORIES:
        info["kind"] = "weapon"
        grades = []
        for key, value in row.items():
            if key.startswith("ItemGradeForMode"):
                grades = [_num(g) for g in value.split(",")] if value else []
        info["grades"] = grades
        info["weapon"] = {"damage": _num(row.get("Damage")), "zombieDamage": _num(row.get("zombie_damage")),
                          "scenarioDamage": _num(row.get("scen_damage")), "accuracy": _num(row.get("HitRate")),
                          "recoil": _num(row.get("Reaction")), "rate": _num(row.get("FiringSpeed")),
                          "weight": _num(row.get("Weight")), "knockback": _num(row.get("knockback")),
                          "stun": _num(row.get("delay")), "upgrades": _num(row.get("EnhWeapon"))}
    elif item["class"] == "PeriodicItem" and item["category"] == "8":
        info["kind"] = "zombie"
    return info


def warbond_item_ids():
    text = open(os.path.join(SERVER_DATA, "Warbonds.json"), encoding="utf-8").read()
    cfg = json.loads(re.sub(r"(?m)^\s*//.*$", "", text))
    return {i["ItemID"] for w in cfg["Warbonds"] for p in w["Pages"] for i in p["Items"]}


def market_item_ids():
    text = open(os.path.join(SERVER_DATA, "Missions.json"), encoding="utf-8").read()
    cfg = json.loads(re.sub(r"(?m)^\s*//.*$", "", text))
    return {i["ItemID"] for i in cfg.get("BlackMarket", {}).get("Items", [])}


def rift_item_ids():
    """The Rift Exchange's cosmetics (Data/RiftExchange.json, gen_rift_exchange.py)."""
    path = os.path.join(SERVER_DATA, "RiftExchange.json")
    if not os.path.exists(path):
        return set()
    return {i["ItemID"] for i in json.load(open(path, encoding="utf-8")).get("Items", [])}


def warbond_art():
    """{warbond ID: "[archive:]game path"} of the passes' promotional art (Warbonds.json "Art")."""
    text = open(os.path.join(SERVER_DATA, "Warbonds.json"), encoding="utf-8").read()
    cfg = json.loads(re.sub(r"(?m)^\s*//.*$", "", text))
    return {w["ID"]: w["Art"] for w in cfg["Warbonds"] if w.get("Art")}


def export_promo_art(tmp):
    """Promotional art of the battle passes -> web/shop/promo/warbond-<ID>.png"""
    os.makedirs(PROMO_DIR, exist_ok=True)
    made = 0
    for pass_id, art in warbond_art().items():
        archive, _, path = art.rpartition(":")
        out = os.path.join(tmp, "art", archive or "any")
        run_nartool([path], out, only=archive or None)
        src = os.path.join(out, path)
        if not os.path.exists(src):
            print(f"no promotional art {art} for battle pass {pass_id}")
            continue
        open(os.path.join(PROMO_DIR, f"warbond-{pass_id}.png"), "wb").write(tga_to_png(open(src, "rb").read()))
        made += 1
    return made


def export_sounds(tmp):
    """Interface sounds of the shop (hold-to-unlock) -> web/shop/sounds/"""
    os.makedirs(SOUND_DIR, exist_ok=True)
    run_nartool(sorted(SOUNDS.values()), tmp)
    for name, path in SOUNDS.items():
        src = os.path.join(tmp, path)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(SOUND_DIR, name))


def default_characters():
    text = open(os.path.join(SERVER_DATA, "..", "ServerConfig.json"), encoding="utf-8").read()
    cfg = json.loads(re.sub(r"(?m)^\s*//.*$", "", text), strict=False)
    return cfg.get("DefaultUser", {}).get("DefaultItems", [])


def longest_prefix(fmt, res, available):
    for cut in range(len(res), 3, -1):
        if fmt.format(res[:cut]) in available:
            return res[:cut]
    return None


def weapon_anim_extensions():
    """{"models/p_x.mdl": "rifle", ...}: the player animation each weapon uses, read from mp.dll. Every weapon's
    Deploy calls DefaultDeploy(v_model, p_model, anim, anim_extension, ...), so the extension string is pushed
    right before the p_ model path."""
    import pefile
    pe = pefile.PE(os.path.join(GAME, "Bin", "mp.dll"), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = next(sec for sec in pe.sections if sec.Name.startswith(b".text"))
    code = img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]
    cache = {}

    def string_at(va):
        if va not in cache:
            o, value = va - base, None
            if 0 <= o < len(img):
                raw = img[o:img.find(b"\0", o)]
                if 2 <= len(raw) < 80 and all(32 <= c < 127 for c in raw):
                    value = raw.decode()
            cache[va] = value
        return cache[va]

    pushes = [(m.start(), string_at(struct.unpack("<I", m.group(1))[0])) for m in re.finditer(rb"\x68(....)", code, re.S)]
    pushes = [(o, t) for o, t in pushes if t]
    found = {}
    for k, (off, text_) in enumerate(pushes):
        if text_.startswith("models/p_") and text_.endswith(".mdl"):
            for off2, prev in reversed(pushes[max(0, k - 3):k]):
                if off - off2 < 40 and re.fullmatch(r"[a-z0-9_]{2,16}", prev):
                    found.setdefault(text_.lower(), prev)
                    break
    return found


def weapon_pose(item, model_paths, extensions):
    """The aim pose the preview shows a weapon with: the game's own animation when the character models have it,
    otherwise one that fits the kind of weapon."""
    ext = next((extensions[m.lower()] for m in model_paths if m.lower() in extensions), None)
    if ext and "ref_aim_" + ext in mdl_export.AIM_POSES:
        return "ref_aim_" + ext
    name = item["name"].lower()
    category = item["category"]
    if category == "6":
        # launchers, flamethrowers, chainsaws... are held like a heavy machine gun; grenades are thrown
        return "ref_aim_grenade" if re.search(r"grenade|bomb|mine|claymore", name) else "ref_aim_m249"
    if category == "11":
        return "ref_aim_hammer" if re.search(r"hammer|axe|stick|scythe|glaive|lance|crescent|spear", name) else "ref_aim_knife"
    if re.search(r"\bdual\b", name) and category in ("1", "3"):
        return "ref_aim_dualpistols"
    return None


def weapon_model_candidates(res, available):
    out = ([MODEL_OVERRIDES[res]] if res in MODEL_OVERRIDES else []) + [f"models/p_{res}.mdl", f"models/p_{res}a.mdl"]
    p_models = sorted(a for a in available if a.startswith("models/p_") and a.endswith(".mdl"))
    for cut in range(len(res), 3, -1):
        hits = [a for a in p_models if a.startswith("models/p_" + res[:cut])]
        if hits:
            out.append(min(hits, key=len))
            break
    return out


def skin_redirects():
    """hw.dll's weapon skin table: virtual model -> (base model, texture prefix), e.g.
    models/p_y19s4tar21.mdl -> (models/p_tar21.mdl, "@y19s4tar21_"). The game loads the base model and swaps each
    texture for models/texture/<prefix><texture name> when that file exists."""
    import pefile
    pe = pefile.PE(os.path.join(GAME, "Bin", "hw.dll"), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()

    def string_at(va):
        o = va - base
        if 0 <= o < len(img):
            s = img[o:img.find(b"\0", o)]
            if 3 <= len(s) < 80 and all(32 <= c < 127 for c in s):
                return s.decode()
        return None

    # the table is built by one function that pushes the strings in order; find it through a known entry
    anchor = img.find(b"models/p_nataknifesnow.mdl\0")
    ref = img.find(b"\x68" + struct.pack("<I", base + anchor)) if anchor >= 0 else -1
    if ref < 0:
        return {}
    pushed = []
    for m in re.finditer(rb"\x68(....)", img[max(0, ref - 0x20000):ref + 0x20000], re.S):
        text = string_at(struct.unpack("<I", m.group(1))[0])
        if text:
            pushed.append(text)
    table = {}
    for i in range(len(pushed) - 2):
        virtual, real, prefix = pushed[i:i + 3]
        if virtual.endswith(".mdl") and real.endswith(".mdl") and not prefix.endswith(".mdl"):
            table.setdefault(virtual.lower(), (real, prefix))
    return table


def _bmp_size(path):
    data = open(path, "rb").read(26)
    return struct.unpack_from("<ii", data, 18) if data[:2] == b"BM" else None


def pick_skin_version(base, prefix, tmp):
    """Several archives hold different versions of the base model; the skin was painted for one of them.
    Returns (model path, {texture name: skin texture path}) for the version whose textures the skin fits best."""
    versions = [("any", None)] + [(g, g) for g in ARCHIVES]
    for folder, only in versions:
        run_nartool([base], os.path.join(tmp, "v_" + folder), only=only)
    found = []
    for folder, _ in versions:
        path = os.path.join(tmp, "v_" + folder, base)
        if os.path.exists(path) and path not in [f[0] for f in found]:
            data = open(path, "rb").read()
            if all(data != open(f[0], "rb").read() for f in found):
                found.append((path, mdl_export.Model(data)))
    if not found:
        return None, {}
    wanted = sorted({f"models/texture/{prefix}{t['name']}" for _, m in found for t in mdl_export.textures(m)})
    run_nartool(wanted, tmp)
    best = None
    for path, model in found:
        overrides, score = {}, 0
        for t in mdl_export.textures(model):
            skin = f"models/texture/{prefix}{t['name']}"
            size = _bmp_size(os.path.join(tmp, skin)) if os.path.exists(os.path.join(tmp, skin)) else None
            if not size:
                continue
            overrides[t["name"].lower()] = skin
            # same aspect ratio as the UV space it was painted for
            w, h = t["uv_width"], t["uv_height"]
            score += 10 + (abs(size[0] / max(1, size[1]) - w / max(1, h)) < 0.05) * 5
        if overrides and (best is None or score > best[2]):
            best = (path, overrides, score)
    return (best[0], best[1]) if best else (None, {})


def export_models(items, wanted, available, tmp):
    """3D preview models: weapons as p_ models, characters as player models. Returns models/index.json data."""
    jobs, index, skins, generic = {}, {}, {}, {}
    redirects = skin_redirects()
    chars = [i for i in default_characters() if items.get(i, {}).get("class") == "Class"]
    for item_id in list(wanted) + chars:
        it = items.get(item_id)
        if not it or not it["resource"]:
            continue
        res = it["resource"]
        if res in SKINS:
            skins[str(item_id)] = res
            continue
        if f"models/p_{res}.mdl" in redirects and it["class"] == "Equipment":
            generic[str(item_id)] = (res, *redirects[f"models/p_{res}.mdl"])
            continue
        if it["class"] == "Class":
            wanted_models = [(str(item_id), "character", [f"models/player/{res}/{res}.mdl"])]
        elif it["class"] in COSTUME_CLASSES:
            wanted_models = [(str(item_id), "costume", [f"models/costume/{res}.mdl"])]
        elif it["class"] == "ZombieSkinCostume":
            wanted_models = [(str(item_id), "zombie", [f"models/player/{res}_origin/{res}_origin.mdl"])]
        elif it["class"] == "Equipment" and it["category"] in WEAPON_CATEGORIES:
            wanted_models = [(str(item_id), "weapon", weapon_model_candidates(res, available))]
        else:
            continue
        for key, kind, candidates in wanted_models:
            jobs[key] = (kind, candidates)
    jobs["default"] = ("weapon", [f"models/p_{DEFAULT_WEAPON}.mdl"])

    # ask for every candidate (newer content lives in Data/Packer, which has no file list) and keep the first found
    run_nartool(sorted({c for _, cands in jobs.values() for c in cands}), tmp)
    models = {}
    for key, (kind, candidates) in jobs.items():
        path = next((c for c in candidates if os.path.exists(os.path.join(tmp, c))), None)
        if not path:
            continue
        name = path.split("/")[2] if kind in ("character", "zombie") else os.path.basename(path)[:-4]
        if name not in models:
            # a zombie skin is a character model of its own (shown without a weapon)
            models[name] = (mdl_export.Model(open(os.path.join(tmp, path), "rb").read()),
                            "character" if kind == "zombie" else kind)
        if key != "default":
            index[key] = {"character": name, "zombie": True} if kind == "zombie" else {kind: name}
    skin_textures = {}
    for key, res in skins.items():
        base, archive, textures = SKINS[res]
        run_nartool([base], os.path.join(tmp, archive), only=archive)
        run_nartool(sorted(textures.values()), tmp)
        path = os.path.join(tmp, archive, base)
        if os.path.exists(path):
            name = "p_" + res
            models[name] = (mdl_export.Model(open(path, "rb").read()), "weapon")
            skin_textures[name] = textures
            index[key] = {"weapon": name}
    for key, (res, base, prefix) in generic.items():
        path, textures = pick_skin_version(base, prefix, tmp)
        if path:
            name = "p_" + res
            models[name] = (mdl_export.Model(open(path, "rb").read()), "weapon")
            skin_textures[name] = textures
            index[key] = {"weapon": name}
        else:
            print(f"skin {res}: no texture found for {base} with prefix {prefix}")
    # CSO keeps the real pictures of "#WWWHHH..." textures in models/texture/
    external = {f"models/texture/{t['name']}" for m, _ in models.values() for t in mdl_export.textures(m) if t["external"]}
    # (asked for even when not in the .nar index: packs have no file list)
    if external:
        run_nartool(sorted(external), tmp)

    def loader_for(overrides):
        def loader(tex_name, external):
            if tex_name.lower() in overrides:
                f = os.path.join(tmp, overrides[tex_name.lower()])
            elif external:
                f = os.path.join(tmp, "models", "texture", tex_name)
            else:
                return None
            return open(f, "rb").read() if os.path.exists(f) else None
        return loader

    os.makedirs(MODEL_DIR, exist_ok=True)
    for name, (model, kind) in models.items():
        mdl_export.export_skinned(model, name, MODEL_DIR, kind, loader_for(skin_textures.get(name, {})))
    extensions = weapon_anim_extensions()
    for key, entry in index.items():
        if "weapon" in entry and key.isdigit():
            it = items[int(key)]
            pose = weapon_pose(it, [f"models/p_{it['resource']}.mdl", f"models/{entry['weapon']}.mdl"], extensions)
            if pose:
                entry["pose"] = pose
    first_char = next((items[i]["resource"] for i in chars if items[i]["resource"] in models), "gign")
    data = {"items": {k: v for k, v in index.items()
                      if (v.get("weapon") or v.get("character") or v.get("costume")) in models},
            "defaultCharacter": first_char, "defaultWeapon": "p_" + DEFAULT_WEAPON}
    json.dump(data, open(os.path.join(MODEL_DIR, "index.json"), "w", encoding="utf-8"), indent=1)
    return data


def nar_files():
    files = set()
    for path in glob.glob(os.path.join(GAME, "Data", "*.nar")):
        for name, *_ in nar_index.read_dir(path):
            parts = name.split("/", 2)          # "", "cstrike", "gfx/..."
            if len(parts) == 3:
                files.add(parts[2].lower())
                files.add(parts[2])
    return files


# nartool --serve, one per archive choice (None = all of them), started once and kept open for every script of the
# export: the antivirus checks each start of a program it doesn't know, and thousands of starts (one per skin and
# archive) made it pop up again and again and slowed everything down.
_SERVERS = {}
BLOCKED = ("nartool.exe (the mod's reader of the game's files) was stopped or removed, most likely by an antivirus. "
           "Restore it from the antivirus' quarantine (or allow it) and extract the pictures again.")


def _close_servers():
    for p in _SERVERS.values():
        try:
            p.stdin.write("quit\n")
            p.stdin.close()
            p.wait(timeout=10)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass
    _SERVERS.clear()


atexit.register(_close_servers)


def _server(only):
    p = _SERVERS.get(only)
    if p is not None and p.poll() is None:
        return p
    if not os.path.exists(NARTOOL):
        raise RuntimeError(BLOCKED)
    env = {k: v for k, v in os.environ.items() if k != "NAR_ONLY"}
    if only:
        env["NAR_ONLY"] = only
    try:
        p = subprocess.Popen([NARTOOL, os.path.join(GAME, "Bin"), GAME, "--serve"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, env=env, encoding="utf-8", errors="replace",
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as e:
        raise RuntimeError(f"{BLOCKED} ({e})") from e
    _SERVERS[only] = p
    return p


def run_nartool(paths, out_dir, only=None):
    """Extracts game files into out_dir. only="fstrike" reads that archive alone (its version of a file)."""
    os.makedirs(out_dir, exist_ok=True)
    paths = [p for p in paths if p and "\n" not in p]
    if not paths:
        return
    answer, code = "", None
    # twice: it once closed by itself right after starting, and worked when started again
    for _ in range(2):
        p = _server(only)
        try:
            p.stdin.write(f"get\t{os.path.abspath(out_dir)}\t{len(paths)}\n" + "".join(x + "\n" for x in paths))
            p.stdin.flush()
            answer = p.stdout.readline()
        except OSError:
            answer = ""
        if answer.startswith("done"):
            return
        _SERVERS.pop(only, None)
        try:
            code = p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
    raise RuntimeError(f"{BLOCKED} (answer {answer.strip()!r}, exit code {code})")


def tga_to_png(data):
    """Uncompressed (type 2) or RLE (type 10) true-colour TGA -> RGBA PNG bytes."""
    id_len, _cmap, img_type, _, _, _, _, _, width, height, bpp, desc = struct.unpack_from("<BBBHHBHHHHBB", data)
    if img_type not in (2, 10) or bpp not in (24, 32):
        raise ValueError(f"unsupported TGA type {img_type}/{bpp}bpp")
    px = bpp // 8
    pos = 18 + id_len
    count = width * height
    raw = bytearray()
    if img_type == 2:
        raw = data[pos:pos + count * px]
    else:
        while len(raw) < count * px:
            header = data[pos]
            pos += 1
            n = (header & 0x7F) + 1
            if header & 0x80:
                raw += data[pos:pos + px] * n
                pos += px
            else:
                raw += data[pos:pos + n * px]
                pos += n * px
    # 32-bit icons carry real alpha even when the header declares 0 alpha bits; only all-zero alpha means opaque
    alpha = bpp == 32 and any(raw[i] for i in range(3, len(raw), px))
    rows = []
    for y in range(height):
        src_y = y if desc & 0x20 else height - 1 - y      # bottom-up unless the top-left origin bit is set
        row = bytearray(b"\0")                            # PNG filter: none
        start = src_y * width * px
        for x in range(width):
            b, g, r = raw[start + x * px:start + x * px + 3]
            a = raw[start + x * px + 3] if alpha else 255
            row += bytes((r, g, b, a))
        rows.append(bytes(row))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def load_names(path):
    data = open(path, "rb").read()
    text = data.decode("utf-16") if data[:2] in (b"\xff\xfe", b"\xfe\xff") else data.decode("utf-8", "replace")
    names = {}
    for key, value in re.findall(r'^\s*"([^"]+)"\s+"([^"]*)"', text, re.M):
        names.setdefault(key.lower(), value)    # first definition wins
    return names


def main():
    items = load_items()
    if "--all" in sys.argv:
        wanted = [i for i, it in items.items() if it["class"] in ("Equipment", "Class")]
    else:
        wanted = sorted(warbond_item_ids() | market_item_ids() | rift_item_ids() |
                        {i for i, it in items.items() if it["class"] == "Class"})
    available = nar_files()

    icon_candidates = {}
    for item_id in wanted:
        res = items.get(item_id, {}).get("resource")
        if not res:
            continue
        # the exact picture first (newer items keep it in Data/Packer, which has no file list), then the base
        # weapon's (xm1014snow -> xm1014)
        cands = [f"gfx/vgui/basket/{res}.tga"]
        for cut in range(len(res) - 1, 3, -1):
            path = f"gfx/vgui/basket/{res[:cut]}.tga"
            if path in available:
                cands.append(path)
                break
        icon_candidates[item_id] = cands

    with tempfile.TemporaryDirectory() as tmp:
        run_nartool(sorted({c for cands in icon_candidates.values() for c in cands}) + ["resource/cso_na_en.txt"], tmp)
        icon_paths = {i: next((c for c in cands if os.path.exists(os.path.join(tmp, c))), None)
                      for i, cands in icon_candidates.items()}
        icon_paths = {i: p for i, p in icon_paths.items() if p}

        os.makedirs(ICON_DIR, exist_ok=True)
        made = 0
        for item_id, path in icon_paths.items():
            src = os.path.join(tmp, path)
            if os.path.exists(src):
                try:
                    png = tga_to_png(open(src, "rb").read())
                except ValueError as e:
                    print(f"skip {path}: {e}")
                    continue
                open(os.path.join(ICON_DIR, f"{item_id}.png"), "wb").write(png)
                made += 1

        loc = load_names(os.path.join(tmp, "resource", "cso_na_en.txt"))
        model_index = export_models(items, wanted, available, tmp)
        promos = export_promo_art(tmp)
        export_sounds(tmp)

    infos = {str(i): item_info(items[i], loc) for i in wanted if i in items}
    json.dump(infos, open(os.path.join(MOD, "web", "shop", "iteminfo.json"), "w", encoding="utf-8"), ensure_ascii=False)

    names_path = os.path.join(SERVER_DATA, "ItemNames.json")
    names = json.load(open(names_path, encoding="utf-8")) if os.path.exists(names_path) else {}
    for item_id in wanted:
        res = items.get(item_id, {}).get("resource", "")
        name = loc.get(f"cso_item_name_{res}") or loc.get(f"cso_{res}")
        if name:
            names[str(item_id)] = name
    json.dump(names, open(names_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1, sort_keys=True)

    print(f"{made} icons -> {ICON_DIR}")
    print(f"{len(names)} names -> {names_path}")
    print(f"{len(model_index['items'])} items with a 3D model -> {MODEL_DIR}")
    print(f"{promos} promotional pictures -> {PROMO_DIR}")
    missing = [i for i in wanted if i not in icon_paths]
    if missing:
        print("no icon in the game files for:", missing[:40])


if __name__ == "__main__":
    main()
