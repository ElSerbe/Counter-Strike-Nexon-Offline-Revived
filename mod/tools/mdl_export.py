"""GoldSrc studio model (.mdl, versions 10 and CSO's 20) -> posed static mesh for the web shop's 3D preview.

The character is posed with one frame of an aiming sequence and the weapon (p_ model) follows the character's
bones with the same names, the way the engine merges bones of attached models. Output: one JSON with triangle
lists per texture (positions, normals, uvs; GoldSrc units, Z up) plus the textures as PNG.
"""
import json
import re
import math
import os
import struct
import zlib

STUDIO_NF_MASKED = 0x40

HDR_FIELDS = ["flags", "numbones", "boneindex", "numbonecontrollers", "bonecontrollerindex", "numhitboxes",
              "hitboxindex", "numseq", "seqindex", "numseqgroups", "seqgroupindex", "numtextures", "textureindex",
              "texturedataindex", "numskinref", "numskinfamilies", "skinindex", "numbodyparts", "bodypartindex",
              "numattachments", "attachmentindex"]


def _cstr(data, off, size):
    return data[off:off + size].split(b"\0")[0].decode("latin1")


# CSO encrypts the vertices and textures of version 20/21 models with ICE (keys from hw.dll 0x2e30ac0/0x2e30ae0,
# applied by Mod_LoadModel -> 0x2429820 -> 0x25f41c0)
_ICE_KEYS = {20: "32a621e0ab6bf42c93c6f196fb387568ba701386e0b371f4e39b07220cfe883a",
             21: "227a196f7b867de08cc6f196fb387568887a788678866770d991073a1474fe22"}


def _decrypt_cso(data, h, version):
    import ice
    key = ice.IceKey(4, bytes.fromhex(_ICE_KEYS[version]))
    buf = bytearray(data)
    for i in range(h["numtextures"]):
        o = h["textureindex"] + i * 80
        _flags, w, hgt, index = struct.unpack_from("<4i", buf, o + 64)
        ice.decrypt_region(key, buf, index, w * hgt + 0x300)
    for bp in range(h["numbodyparts"]):
        nummodels, _base, modelindex = struct.unpack_from("<3i", buf, h["bodypartindex"] + bp * 76 + 64)
        for m in range(nummodels):
            mo = modelindex + m * 112
            numverts, _vertinfo, vertindex = struct.unpack_from("<3i", buf, mo + 80)
            ice.decrypt_region(key, buf, vertindex, numverts * 12)
    return bytes(buf)


class Model:
    def __init__(self, data):
        if data[:4] != b"IDST":
            raise ValueError("not a studio model")
        self.version = struct.unpack_from("<i", data, 4)[0]
        self.h = dict(zip(HDR_FIELDS, struct.unpack_from("<%di" % len(HDR_FIELDS), data, 8 + 64 + 4 + 60)))
        if self.version in _ICE_KEYS:
            data = _decrypt_cso(data, self.h, self.version)
        self.d = data
        self.bones = []
        for i in range(self.h["numbones"]):
            o = self.h["boneindex"] + i * 112
            name = _cstr(data, o, 32)
            parent = struct.unpack_from("<i", data, o + 32)[0]
            value = struct.unpack_from("<6f", data, o + 64)
            scale = struct.unpack_from("<6f", data, o + 88)
            self.bones.append({"name": name, "parent": parent, "value": value, "scale": scale})
        self.seqs = []
        for i in range(self.h["numseq"]):
            o = self.h["seqindex"] + i * 176
            label = _cstr(data, o, 32)
            numframes = struct.unpack_from("<i", data, o + 56)[0]
            numblends, animindex = struct.unpack_from("<2i", data, o + 120)
            seqgroup = struct.unpack_from("<i", data, o + 156)[0]
            self.seqs.append({"name": label, "numframes": numframes, "numblends": numblends,
                              "animindex": animindex, "seqgroup": seqgroup})

    # ----- animation -----
    def _anim_value(self, base, offset, frame):
        if offset == 0:
            return 0.0
        d = self.d
        p = base + offset
        k = frame
        while True:
            valid, total = d[p], d[p + 1]
            if total == 0:
                return 0.0
            if total > k:
                break
            k -= total
            p += (valid + 1) * 2
        if valid > k:
            return float(struct.unpack_from("<h", d, p + (k + 1) * 2)[0])
        return float(struct.unpack_from("<h", d, p + valid * 2)[0])

    def local_transforms(self, seq_name, frame=0, blend=None):
        seq = next((s for s in self.seqs if s["name"].lower() == seq_name.lower()), None)
        if seq is None or seq["seqgroup"] != 0:
            seq = next(s for s in self.seqs if s["seqgroup"] == 0)
        if blend is None:
            blend = seq["numblends"] // 2          # aim blends: the middle one looks straight ahead
        nb = len(self.bones)
        base = seq["animindex"] + blend * nb * 12
        frame = min(frame, max(0, seq["numframes"] - 1))
        out = []
        for i, b in enumerate(self.bones):
            a = base + i * 12
            offsets = struct.unpack_from("<6H", self.d, a)
            v = [b["value"][j] + self._anim_value(a, offsets[j], frame) * b["scale"][j] for j in range(6)]
            out.append((v[0:3], v[3:6]))
        return out


def _quat(angles):
    sy, cy = math.sin(angles[2] * .5), math.cos(angles[2] * .5)
    sp, cp = math.sin(angles[1] * .5), math.cos(angles[1] * .5)
    sr, cr = math.sin(angles[0] * .5), math.cos(angles[0] * .5)
    return (sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy)


def _matrix(q, pos):
    x, y, z, w = q
    return [[1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * w * z, 2 * x * z + 2 * w * y, pos[0]],
            [2 * x * y + 2 * w * z, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * w * x, pos[1]],
            [2 * x * z - 2 * w * y, 2 * y * z + 2 * w * x, 1 - 2 * x * x - 2 * y * y, pos[2]]]


def _concat(a, b):
    return [[a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (a[r][3] if c == 3 else 0)
             for c in range(4)] for r in range(3)]


def pose(model, seq_name, frame=0, parent_bones=None):
    """World matrices per bone. parent_bones: {name: matrix} from the model this one is attached to."""
    world = []
    for i, (pos, ang) in enumerate(model.local_transforms(seq_name, frame)):
        name = model.bones[i]["name"]
        if parent_bones and name in parent_bones:
            world.append(parent_bones[name])
            continue
        m = _matrix(_quat(ang), pos)
        p = model.bones[i]["parent"]
        world.append(_concat(world[p], m) if p >= 0 else m)
    return world


def _xform(m, v):
    return [m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] + m[r][3] for r in range(3)]


def _rot(m, v):
    return [m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] for r in range(3)]


def textures(model):
    d, h = model.d, model.h
    out = []
    for i in range(h["numtextures"]):
        o = h["textureindex"] + i * 80
        name = _cstr(d, o, 64)
        flags, w, hgt, index = struct.unpack_from("<4i", d, o + 64)
        real = re.search(r"#(\d{3})(\d{3})", name)
        uw, uh = (int(real.group(1)), int(real.group(2))) if real else (w, hgt)
        out.append({"name": name, "flags": flags, "width": w, "height": hgt, "index": index,
                    "uv_width": uw, "uv_height": uh, "external": bool(real)})
    return out


def texture_png(model, tex):
    w, h, idx = tex["width"], tex["height"], tex["index"]
    pixels = model.d[idx:idx + w * h]
    pal = model.d[idx + w * h: idx + w * h + 768]
    masked = tex["flags"] & STUDIO_NF_MASKED
    rows = []
    for y in range(h):
        row = bytearray(b"\0")
        for x in range(w):
            c = pixels[y * w + x]
            r, g, b = pal[c * 3:c * 3 + 3]
            row += bytes((r, g, b, 0 if masked and c == 255 else 255))
        rows.append(bytes(row))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def _png(width, height, rgba_rows):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rgba_rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bmp_png(data, masked=False):
    """External CSO model textures (models/texture/*.bmp): 8-bit palettized or 24-bit, bottom-up."""
    offset = struct.unpack_from("<I", data, 10)[0]
    hdr_size, w, h, _planes, bpp = struct.unpack_from("<IiiHH", data, 14)
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    pal = data[14 + hdr_size:offset] if bpp == 8 else b""
    rows = []
    for y in range(h):
        src = y if top_down else h - 1 - y
        line = data[offset + src * stride: offset + src * stride + stride]
        row = bytearray()
        for x in range(w):
            if bpp == 8:
                c = line[x]
                b, g, r = pal[c * 4:c * 4 + 3]
                a = 0 if masked and c == 255 else 255
            else:
                b, g, r = line[x * 3:x * 3 + 3]
                a = 255
            row += bytes((r, g, b, a))
        rows.append(row)
    return _png(w, h, rows)


def bake(model, world, prefix, tex_out_dir):
    """Posed triangle lists grouped by texture. Writes the textures as <prefix>_<i>.png."""
    d, h = model.d, model.h
    texs = textures(model)
    skins = struct.unpack_from("<%dh" % h["numskinref"], d, h["skinindex"]) if h["numskinref"] else ()
    groups = {}
    for bp in range(h["numbodyparts"]):
        o = h["bodypartindex"] + bp * 76
        nummodels, _base, modelindex = struct.unpack_from("<3i", d, o + 64)
        if nummodels <= 0:
            continue
        mo = modelindex                                   # body 0 of every part (the default look)
        nummesh, meshindex, numverts, vertinfo, vertindex, numnorms, norminfo, normindex = \
            struct.unpack_from("<8i", d, mo + 72)
        verts = [struct.unpack_from("<3f", d, vertindex + i * 12) for i in range(numverts)]
        vbone = d[vertinfo:vertinfo + numverts]
        norms = [struct.unpack_from("<3f", d, normindex + i * 12) for i in range(numnorms)]
        nbone = d[norminfo:norminfo + numnorms]
        pv = [_xform(world[vbone[i]], verts[i]) for i in range(numverts)]
        pn = [_rot(world[nbone[i]], norms[i]) for i in range(numnorms)]
        for m in range(nummesh):
            numtris, triindex, skinref, _nn, _ni = struct.unpack_from("<5i", d, meshindex + m * 20)
            ti = skins[skinref] if skinref < len(skins) else skinref
            if ti >= len(texs):
                continue
            tex = texs[ti]
            g = groups.setdefault(ti, {"positions": [], "normals": [], "uvs": []})
            p = triindex
            while True:
                count = struct.unpack_from("<h", d, p)[0]
                p += 2
                if count == 0:
                    break
                fan = count < 0
                n = abs(count)
                cmds = [struct.unpack_from("<4h", d, p + k * 8) for k in range(n)]
                p += n * 8
                for k in range(2, n):
                    if fan:
                        tri = (cmds[0], cmds[k - 1], cmds[k])
                    elif k % 2 == 0:
                        tri = (cmds[k - 2], cmds[k - 1], cmds[k])
                    else:
                        tri = (cmds[k - 1], cmds[k - 2], cmds[k])
                    for vi, ni, s, t in tri:
                        g["positions"] += [round(c, 2) for c in pv[vi]]
                        g["normals"] += [round(c, 3) for c in pn[ni]]
                        g["uvs"] += [round(s / max(1, tex["width"]), 4), round(t / max(1, tex["height"]), 4)]
    meshes = []
    for ti, g in groups.items():
        tex = texs[ti]
        file_name = f"{prefix}_{ti}.png"
        open(os.path.join(tex_out_dir, file_name), "wb").write(texture_png(model, tex))
        meshes.append({"texture": file_name, "masked": bool(tex["flags"] & STUDIO_NF_MASKED), **g})
    return meshes


def bone_map(model, world):
    return {b["name"]: world[i] for i, b in enumerate(model.bones)}


AIM_POSES = ["ref_aim_carbine", "ref_aim_onehanded", "ref_aim_rifle", "ref_aim_mp5", "ref_aim_shotgun",
             "ref_aim_m249", "ref_aim_knife", "ref_aim_ak47", "ref_aim_m134", "ref_aim_grenade", "ref_aim_dualpistols",
             "ref_aim_hammer"]


def _flat(m):
    return [round(m[r][c], 4) for r in range(3) for c in range(4)]


def export_skinned(model, prefix, out_dir, kind, tex_loader=None):
    """Unposed mesh for the web viewer: GoldSrc stores vertices in the space of their bone, so the viewer only
    needs one matrix per bone. kind "character": world matrices of every aim pose; kind "weapon": local matrices
    (bones with a character bone of the same name follow that bone instead)."""
    d, h = model.d, model.h
    texs = textures(model)
    skins = struct.unpack_from("<%dh" % h["numskinref"], d, h["skinindex"]) if h["numskinref"] else ()
    groups = {}
    for bp in range(h["numbodyparts"]):
        nummodels, _base, mo = struct.unpack_from("<3i", d, h["bodypartindex"] + bp * 76 + 64)
        if nummodels <= 0:
            continue
        nummesh, meshindex, numverts, vertinfo, vertindex, numnorms, norminfo, normindex = struct.unpack_from("<8i", d, mo + 72)
        verts = [struct.unpack_from("<3f", d, vertindex + i * 12) for i in range(numverts)]
        vbone = d[vertinfo:vertinfo + numverts]
        norms = [struct.unpack_from("<3f", d, normindex + i * 12) for i in range(numnorms)]
        nbone = d[norminfo:norminfo + numnorms]
        for m in range(nummesh):
            numtris, triindex, skinref, _nn, _ni = struct.unpack_from("<5i", d, meshindex + m * 20)
            ti = skins[skinref] if skinref < len(skins) else skinref
            if ti >= len(texs):
                continue
            tex = texs[ti]
            g = groups.setdefault(ti, {"positions": [], "normals": [], "uvs": [], "vbones": [], "nbones": []})
            p = triindex
            while True:
                count = struct.unpack_from("<h", d, p)[0]
                p += 2
                if count == 0:
                    break
                n = abs(count)
                cmds = [struct.unpack_from("<4h", d, p + k * 8) for k in range(n)]
                p += n * 8
                for k in range(2, n):
                    if count < 0:
                        tri = (cmds[0], cmds[k - 1], cmds[k])
                    elif k % 2 == 0:
                        tri = (cmds[k - 2], cmds[k - 1], cmds[k])
                    else:
                        tri = (cmds[k - 1], cmds[k - 2], cmds[k])
                    for vi, ni, s, t in tri:
                        g["positions"] += [round(c, 3) for c in verts[vi]]
                        g["normals"] += [round(c, 3) for c in norms[ni]]
                        g["uvs"] += [round(s / max(1, tex["uv_width"]), 4), round(t / max(1, tex["uv_height"]), 4)]
                        g["vbones"].append(vbone[vi])
                        g["nbones"].append(nbone[ni])
    meshes = []
    for ti, g in groups.items():
        tex = texs[ti]
        file_name = f"{prefix}_{ti}.png"
        masked = bool(tex["flags"] & STUDIO_NF_MASKED)
        # the loader supplies "#WWWHHH" pictures and skin overrides (it returns None to keep the embedded texture)
        external = tex_loader(tex["name"], tex["external"]) if tex_loader else None
        png = bmp_png(external, masked) if external else texture_png(model, tex)
        open(os.path.join(out_dir, file_name), "wb").write(png)
        meshes.append({"texture": file_name, "masked": masked, **g})

    out = {"kind": kind, "bones": [{"name": b["name"], "parent": b["parent"]} for b in model.bones], "meshes": meshes}
    names = {s["name"].lower() for s in model.seqs}
    if kind == "character":
        # older characters (707, guerilla, gerrard...) name the dual pistols pose ref_aim_dualpistols_1
        aliases = {p: p for p in AIM_POSES if p in names}
        if "ref_aim_dualpistols" not in aliases and "ref_aim_dualpistols_1" in names:
            aliases["ref_aim_dualpistols"] = "ref_aim_dualpistols_1"
        out["poses"] = {p: [_flat(m) for m in pose(model, seq)] for p, seq in aliases.items()}
    else:
        out["local"] = [_flat(_matrix(_quat(ang), pos)) for pos, ang in model.local_transforms(model.seqs[0]["name"])]
    with open(os.path.join(out_dir, prefix + ".json"), "w", encoding="utf-8") as f:
        json.dump(out, f, separators=(",", ":"))
    return out
