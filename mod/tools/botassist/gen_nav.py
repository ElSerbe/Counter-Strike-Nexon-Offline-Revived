"""Builds a bot navigation mesh (.nav, version 5: the format of the game's own files, see botassist.cpp LoadNav) from a
map's geometry, for maps whose shipped mesh is wrong. The Zombie Escape maps all ship the same placeholder (another
map's mesh, 30461 bytes), so the game's bots and botassist had nowhere to walk there.

How: the map's clip hulls (the shapes the engine moves players with: hull 1 standing, hull 3 crouching) are traced like
the engine does (SV_RecursiveHullCheck). From the spawn points and the mode's own places (route zones, buttons, the exit,
ladders) a flood fill walks a grid of GRID units: a step is walkable when a crouching body fits across at step height
(18) and finds floor not steeper than 45 degrees within MAX_DROP below; a higher floor up to JUMP_HEIGHT is a jump
(the area there gets NAV_JUMP); a cell with no room to stand is NAV_CROUCH. The world and the brush entities that stay
put (func_wall, func_conveyor, func_rotating...) are solid; doors, breakables, trains, buttons and toggled walls are not,
so the way is there when the bots open, break or ride them (botassist's mode code handles that). Flat or evenly sloped
blocks of cells become one area; neighbours are linked by direction (one way for drops and jumps).

Usage: python gen_nav.py <map.bsp> [more.bsp ...] -o <dir>     (writes <dir>/<map>.navtxt; --binary: <map>.nav)
       python gen_nav.py --game ze_bloodcastle ze_skyline ... -o <dir>   (extracts the maps from the game first)
"""
import argparse
import math
import os
import re
import struct
import sys
from collections import deque

GRID = 16.0
STEP = 18.0
JUMP_HEIGHT = 56.0       # a crouch jump onto a ledge
MAX_DROP = 220.0         # deeper than this is a fall that hurts: not a way
FLOOR_SEARCH = 2000.0   # a drop into water can be deep
MIN_NORMAL_Z = 0.7
MAX_CELLS = 600000
MAX_AREA_CELLS = 16      # cells on a side of one area
PLANE_TOLERANCE = 3.0

CONTENTS_EMPTY, CONTENTS_SOLID = -1, -2
DIST_EPSILON = 0.03125
HULL_STAND, HULL_CROUCH = 1, 3
STAND_HALF, CROUCH_HALF = 36.0, 18.0   # origin above the feet
NAV_CROUCH, NAV_JUMP = 1, 2

SOLID_ENTITIES = {"func_wall", "func_conveyor", "func_rotating", "func_guntarget", "func_mortar_field"}
# what can be stood on for a while: the floor search also finds these (where the map puts them at the start), the way
# across doesn't (they open, break, move or fall into place: a crate that drops to make a ramp, a lift, a boat)
FLOOR_ENTITIES = {"func_door", "func_door_rotating", "func_train", "func_tracktrain", "func_breakable", "ze_breakable",
                  "func_wall_toggle", "func_plat", "func_platrot", "func_pushable", "func_button", "func_rot_button",
                  "ze_button", "func_pendulum", "momentary_door"}
# NORTH -y, EAST +x, SOUTH +y, WEST -x (Condition Zero's NavDirType)
DIRS = [(0, -1), (1, 0), (0, 1), (-1, 0)]


class Bsp:
    def __init__(self, path):
        self.path = path
        d = open(path, "rb").read()
        self.size = len(d)
        version = struct.unpack_from("<I", d, 0)[0]
        if version != 30:
            raise ValueError(f"{path}: BSP version {version}, not 30")
        lumps = [struct.unpack_from("<II", d, 4 + 8 * i) for i in range(15)]

        def lump(i):
            off, ln = lumps[i]
            return d[off:off + ln]

        self.entities = [dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', b))
                         for b in re.findall(r"\{([^{}]*)\}", lump(0).decode("latin1"))]
        p = lump(1)
        self.planes = [struct.unpack_from("<4fi", p, i) for i in range(0, len(p), 20)]
        c = lump(9)
        self.clip = [struct.unpack_from("<ihh", c, i) for i in range(0, len(c), 8)]
        m = lump(14)
        self.models = []
        for i in range(0, len(m), 64):
            v = struct.unpack_from("<9f4i3i", m, i)
            self.models.append({"mins": v[0:3], "maxs": v[3:6], "origin": v[6:9], "head": v[9:13]})


class Trace:
    __slots__ = ("allsolid", "startsolid", "fraction", "end", "normal")

    def __init__(self, end):
        self.allsolid = True
        self.startsolid = False
        self.fraction = 1.0
        self.end = end
        self.normal = (0.0, 0.0, 0.0)


class Collision:
    """Point and line tests against the world and the solid brush entities, in one hull."""

    def __init__(self, bsp, classes=None, skip=()):
        classes = SOLID_ENTITIES if classes is None else classes
        self.planes = bsp.planes
        self.clip = bsp.clip
        self.bodies = [(0, (0.0, 0.0, 0.0))]
        for e in bsp.entities:
            cls, flags = e.get("classname"), int(e.get("spawnflags", "0") or 0)
            if cls == "func_conveyor" and flags & 2 or cls == "func_rotating" and flags & 64:
                continue  # SF_CONVEYOR_NOTSOLID, SF_BRUSH_ROTATE_NOT_SOLID: the arrows on the floor, the river's flow
            if cls in classes and e.get("model", "").startswith("*"):
                idx = int(e["model"][1:])
                if 0 < idx < len(bsp.models) and idx not in skip:
                    org = tuple(float(x) for x in e.get("origin", "0 0 0").split()) if "origin" in e else (0.0, 0.0, 0.0)
                    self.bodies.append((idx, org))
        self.models = bsp.models

    def _contents(self, num, p):
        clip, planes = self.clip, self.planes
        while num >= 0:
            plane, c0, c1 = clip[num]
            nx, ny, nz, dist, typ = planes[plane]
            d = (p[typ] if typ < 3 else nx * p[0] + ny * p[1] + nz * p[2]) - dist
            num = c0 if d >= 0 else c1
        return num

    def solid(self, hull, p):
        for idx, org in self.bodies:
            q = (p[0] - org[0], p[1] - org[1], p[2] - org[2]) if idx else p
            if self._contents(self.models[idx]["head"][hull], q) == CONTENTS_SOLID:
                return True
        return False

    def _rhc(self, head, num, p1f, p2f, p1, p2, tr):
        if num < 0:
            if num != CONTENTS_SOLID:
                tr.allsolid = False
            else:
                tr.startsolid = True
            return True
        plane, c0, c1 = self.clip[num]
        nx, ny, nz, dist, typ = self.planes[plane]
        if typ < 3:
            t1, t2 = p1[typ] - dist, p2[typ] - dist
        else:
            t1 = nx * p1[0] + ny * p1[1] + nz * p1[2] - dist
            t2 = nx * p2[0] + ny * p2[1] + nz * p2[2] - dist
        if t1 >= 0 and t2 >= 0:
            return self._rhc(head, c0, p1f, p2f, p1, p2, tr)
        if t1 < 0 and t2 < 0:
            return self._rhc(head, c1, p1f, p2f, p1, p2, tr)
        frac = (t1 + DIST_EPSILON) / (t1 - t2) if t1 < 0 else (t1 - DIST_EPSILON) / (t1 - t2)
        frac = 0.0 if frac < 0 else 1.0 if frac > 1 else frac
        midf = p1f + (p2f - p1f) * frac
        mid = (p1[0] + frac * (p2[0] - p1[0]), p1[1] + frac * (p2[1] - p1[1]), p1[2] + frac * (p2[2] - p1[2]))
        side = 0 if t1 >= 0 else 1
        near, far = (c0, c1) if side == 0 else (c1, c0)
        if not self._rhc(head, near, p1f, midf, p1, mid, tr):
            return False
        if self._contents(far, mid) != CONTENTS_SOLID:
            return self._rhc(head, far, midf, p2f, mid, p2, tr)
        if tr.allsolid:
            return False
        tr.normal = (nx, ny, nz) if side == 0 else (-nx, -ny, -nz)
        while self._contents(head, mid) == CONTENTS_SOLID:
            frac -= 0.1
            if frac < 0:
                tr.fraction = midf
                tr.end = mid
                return False
            midf = p1f + (p2f - p1f) * frac
            mid = (p1[0] + frac * (p2[0] - p1[0]), p1[1] + frac * (p2[1] - p1[1]), p1[2] + frac * (p2[2] - p1[2]))
        tr.fraction = midf
        tr.end = mid
        return False

    def trace(self, hull, a, b, through=False):
        """the first hit of a body moving from a to b; through: a brush entity the body starts in is passed through
        (standing in an open door's place), the world never is"""
        best = None
        for idx, org in self.bodies:
            if idx:
                m = self.models[idx]
                lo = [min(a[k], b[k]) for k in range(3)]
                hi = [max(a[k], b[k]) for k in range(3)]
                if any(hi[k] < m["mins"][k] + org[k] - 40 or lo[k] > m["maxs"][k] + org[k] + 40 for k in range(3)):
                    continue
                pa = (a[0] - org[0], a[1] - org[1], a[2] - org[2])
                pb = (b[0] - org[0], b[1] - org[1], b[2] - org[2])
            else:
                pa, pb = a, b
            head = self.models[idx]["head"][hull]
            tr = Trace(pb)
            self._rhc(head, head, 0.0, 1.0, pa, pb, tr)
            if tr.allsolid:
                tr.startsolid = True
            if through and idx and tr.startsolid:
                continue
            if idx:
                tr.end = (tr.end[0] + org[0], tr.end[1] + org[1], tr.end[2] + org[2])
            if best is None or tr.fraction < best.fraction or tr.startsolid:
                best = tr
                if tr.startsolid:
                    break
        return best


class NavBuilder:
    def __init__(self, bsp, log=print):
        self.bsp = bsp
        self.col = Collision(bsp)
        self.movers = self.find_movers()
        # the lifts' and trains' floors are areas of their own at each stop (decks), not cells of the flood
        self.ground = Collision(bsp, SOLID_ENTITIES | FLOOR_ENTITIES, skip={m["model"] for m in self.movers})
        self.log = log
        # water volumes (func_water): falling into one doesn't hurt, and a body in one swims up to its surface
        self.water = []
        for e in bsp.entities:
            if e.get("classname") == "func_water" and e.get("model", "").startswith("*"):
                m = bsp.models[int(e["model"][1:])]
                self.water.append((m["mins"], m["maxs"]))
        self.cells = {}          # (ix, iy) -> list of cell dicts {z, crouch, edges: {(dir, key)}}
        self.index = {}          # key (ix, iy, n) -> cell

    # ---- floor and bodies
    def floor(self, x, y, top):
        """the feet height of the floor under (x, y) from `top` (feet height to search down from), or None"""
        a = (x, y, top + CROUCH_HALF)
        tr = self.ground.trace(HULL_CROUCH, a, (x, y, top + CROUCH_HALF - FLOOR_SEARCH), through=True)
        if tr.startsolid or tr.fraction >= 1.0 or tr.normal[2] < MIN_NORMAL_Z:
            return None
        return tr.end[2] - CROUCH_HALF

    def water_top(self, x, y, fz):
        """the surface of the water the feet at (x, y, fz) are in, or None"""
        for lo, hi in self.water:
            if lo[0] <= x <= hi[0] and lo[1] <= y <= hi[1] and lo[2] - 4 <= fz + 8 <= hi[2]:
                return hi[2]
        return None

    def stands(self, x, y, fz):
        return not self.col.solid(HULL_STAND, (x, y, fz + STAND_HALF + 0.5))

    def cell_at(self, ix, iy, fz, create=True):
        lst = self.cells.setdefault((ix, iy), [])
        for i, c in enumerate(lst):
            if abs(c["z"] - fz) <= 6.0:
                return (ix, iy, i), False
        if not create:
            return None, False
        x, y = ix * GRID, iy * GRID
        lst.append({"z": fz, "crouch": not self.stands(x, y, fz), "edges": set(), "jump": False})
        key = (ix, iy, len(lst) - 1)
        self.index[key] = lst[-1]
        return key, True

    # ---- what carries players: lifts (func_door moving up or down, func_plat) and trains (func_train, func_tracktrain along
    # their path_corner / path_track); teleports are added in write()
    def find_movers(self):
        ents = self.bsp.entities
        named = {}
        for e in ents:
            if "targetname" in e:
                named.setdefault(e["targetname"], e)
        out = []
        for e in ents:
            cls, model = e.get("classname", ""), e.get("model", "")
            if not model.startswith("*") or cls not in ("func_door", "func_plat", "func_train", "func_tracktrain"):
                continue
            idx = int(model[1:])
            if not 0 < idx < len(self.bsp.models):
                continue
            m = self.bsp.models[idx]
            org = [float(v) for v in e["origin"].split()] if "origin" in e else [0.0, 0.0, 0.0]
            lo = [m["mins"][k] + org[k] for k in range(3)]
            hi = [m["maxs"][k] + org[k] for k in range(3)]
            size = [hi[k] - lo[k] for k in range(3)]
            if size[0] < 64 or size[1] < 64:
                continue  # a door, a bar, a small machine: not something to ride
            stops = []   # (x0, y0, x1, y1, top)
            if cls == "func_door":
                pitch = float((e.get("angles", "0 0 0").split() + ["0"])[0])
                if abs(abs(pitch) - 90) > 1:
                    continue  # it slides sideways
                sign = 1 if pitch < 0 else -1          # -90: up, 90: down
                dist = size[2] - 2 - float(e.get("lip", "0") or 0)
                stops = [(lo[0], lo[1], hi[0], hi[1], hi[2]), (lo[0], lo[1], hi[0], hi[1], hi[2] + sign * dist)]
                kind = 0
            elif cls == "func_plat":
                height = float(e.get("height", "0") or 0) or size[2] - 8
                stops = [(lo[0], lo[1], hi[0], hi[1], hi[2]), (lo[0], lo[1], hi[0], hi[1], hi[2] - height)]
                kind = 0
            else:
                # centred on each corner of its path (CFuncTrain::Activate: origin = corner - (mins + maxs) / 2)
                seen, name = set(), e.get("target")
                while name and name not in seen and name in named and len(stops) < 64:
                    seen.add(name)
                    c = named[name]
                    if "origin" not in c:
                        break
                    x, y, z = (float(v) for v in c["origin"].split())
                    stops.append((x - size[0] / 2, y - size[1] / 2, x + size[0] / 2, y + size[1] / 2, z + size[2] / 2))
                    name = c.get("target")
                kind = 1
            if len(stops) < 2 or max(abs(s[k] - stops[0][k]) for s in stops for k in (0, 1, 4)) < 24:
                continue
            # where one stands on it: its lowest floor with room to stand (inside a train car, not on its roof)
            sink = self.deck_depth(m, idx, stops)
            stops = [(x0, y0, x1, y1, top - sink) for x0, y0, x1, y1, top in stops]
            out.append({"cls": cls, "model": idx, "size": size, "stops": stops, "kind": kind, "sink": sink})
        return out

    def deck_depth(self, m, idx, stops):
        """how far below the top of a brush model the floor one stands on is: of its standable surfaces (at its middle,
        room for a standing body above), the one closest to the floors around its stops (a crane's platform, not its base;
        a train car's floor, not its roof); with none around, the top"""
        cx, cy = (m["mins"][0] + m["maxs"][0]) / 2, (m["mins"][1] + m["maxs"][1]) / 2
        crouch, stand = m["head"][HULL_CROUCH], m["head"][HULL_STAND]
        solid = lambda head, z: self.col._contents(head, (cx, cy, z)) == CONTENTS_SOLID
        sinks = []
        z = m["maxs"][2] + CROUCH_HALF + 2
        while z > m["mins"][2]:
            if not solid(crouch, z) and solid(crouch, z - 2):
                feet = z - CROUCH_HALF
                if not solid(stand, feet + STAND_HALF + 1):
                    sinks.append(m["maxs"][2] - feet)
            z -= 2
        if not sinks:
            return 0.0
        # for each candidate: the stops with floor beside them at that height (within a jump), the mover itself left out
        around = Collision(self.bsp, SOLID_ENTITIES | FLOOR_ENTITIES, skip={idx})

        def score(s):
            fit, near = 0, 0
            for x0, y0, x1, y1, top in stops:
                floor, mx, my, best = top - s, (x0 + x1) / 2, (y0 + y1) / 2, None
                for dx, dy in DIRS:
                    for off in (8, 24, 48):
                        px, py = mx + dx * ((x1 - x0) / 2 + off), my + dy * ((y1 - y0) / 2 + off)
                        tr = around.trace(HULL_CROUCH, (px, py, floor + JUMP_HEIGHT + CROUCH_HALF),
                                          (px, py, floor - JUMP_HEIGHT + CROUCH_HALF), through=True)
                        if not tr.startsolid and tr.fraction < 1.0 and tr.normal[2] >= MIN_NORMAL_Z:
                            d = abs(tr.end[2] - CROUCH_HALF - floor)
                            best = d if best is None else min(best, d)
                            near += 1
                            break
                if best is not None:
                    fit += 1
            return (fit, near, -s)
        return max(sinks, key=score)

    # ---- flood fill
    def seed_points(self):
        pts = []
        for m in self.movers:
            for x0, y0, x1, y1, top in m["stops"]:
                cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
                for dx, dy in DIRS:
                    pts.append((cx + dx * ((x1 - x0) / 2 + 32), cy + dy * ((y1 - y0) / 2 + 32), top + 40))
        for e in self.bsp.entities:
            cls = e.get("classname", "")
            if cls in ("info_player_start", "info_player_deathmatch", "zombiespawn", "info_teleport_destination") and "origin" in e:
                pts.append(tuple(float(v) for v in e["origin"].split()))
            elif e.get("model", "").startswith("*") and cls in ("ze_status", "ze_exit", "func_button", "ze_button",
                                                                 "func_rot_button", "ze_hotspot", "func_ladder", "trigger_once",
                                                                 "trigger_multiple"):
                m = self.bsp.models[int(e["model"][1:])]
                lo, hi = m["mins"], m["maxs"]
                mid = [(lo[k] + hi[k]) / 2 for k in range(3)]
                pts.append((mid[0], mid[1], mid[2]))
                if cls == "func_ladder":
                    for dx, dy in DIRS:
                        for z in (lo[2] + 40, hi[2] + 40):
                            pts.append((mid[0] + dx * ((hi[0] - lo[0]) / 2 + 24), mid[1] + dy * ((hi[1] - lo[1]) / 2 + 24), z))
        return pts

    def build(self):
        queue = deque()
        seeds = 0
        for (sx, sy, sz) in self.seed_points():
            ix, iy = round(sx / GRID), round(sy / GRID)
            fz = self.floor(ix * GRID, iy * GRID, sz)
            if fz is None or sz - fz > 400:
                continue
            key, new = self.cell_at(ix, iy, fz)
            if new:
                queue.append(key)
                seeds += 1
        while queue and len(self.index) < MAX_CELLS:
            key = queue.popleft()
            c = self.index[key]
            ix, iy, _ = key
            x, y, fz = ix * GRID, iy * GRID, c["z"]
            for d, (dx, dy) in enumerate(DIRS):
                nx, ny = x + dx * GRID, y + dy * GRID
                target = self.step(x, y, fz, nx, ny)
                jump = False
                if target is None:
                    target = self.jump(x, y, fz, nx, ny)
                    jump = target is not None
                if target is None:
                    continue
                nkey, new = self.cell_at(ix + dx, iy + dy, target)
                c["edges"].add((d, nkey))
                if jump:
                    self.index[nkey]["jump"] = True
                if new:
                    queue.append(nkey)
        self.log(f"  {seeds} seeds, {len(self.index)} cells{' (limit reached)' if len(self.index) >= MAX_CELLS else ''}")

    def step(self, x, y, fz, nx, ny):
        z = fz + STEP + CROUCH_HALF
        tr = self.col.trace(HULL_CROUCH, (x, y, z), (nx, ny, z))
        if tr.startsolid or tr.fraction < 1.0:
            return None
        gz = self.floor(nx, ny, fz + STEP)
        if gz is None or gz > fz + STEP + 0.5:
            return None
        if fz - gz > MAX_DROP and self.water_top(nx, ny, gz) is None:
            return None  # a fall that hurts, not into water
        return gz

    def jump(self, x, y, fz, nx, ny):
        height = JUMP_HEIGHT
        top = self.water_top(x, y, fz)
        if top is not None:
            height = max(height, top - fz + 24.0)   # swims up and climbs out
        up = (x, y, fz + height + CROUCH_HALF)
        tr = self.col.trace(HULL_CROUCH, (x, y, fz + CROUCH_HALF + 1), up)
        if tr.startsolid or tr.fraction < 1.0:
            return None
        tr = self.col.trace(HULL_CROUCH, up, (nx, ny, up[2]))
        if tr.startsolid or tr.fraction < 1.0:
            return None
        gz = self.floor(nx, ny, fz + height)
        if gz is None or gz <= fz + STEP or gz > fz + height:
            return None
        return gz

    # ---- areas
    def areas(self):
        """greedy rectangles of cells on one plane, same flags, linked both ways to their neighbours inside"""
        owner = {}
        rects = []
        keys = sorted(self.index, key=lambda k: (k[1], k[0], self.index[k]["z"]))

        def mutual(a, d, b):
            ca, cb = self.index[a], self.index[b]
            return (d, b) in ca["edges"] and ((d + 2) % 4, a) in cb["edges"]

        def flags(c):
            return (NAV_CROUCH if c["crouch"] else 0) | (NAV_JUMP if c["jump"] else 0)

        def neighbour(k, d):
            c = self.index[k]
            for dd, nk in c["edges"]:
                if dd == d:
                    return nk
            return None

        for start in keys:
            if start in owner:
                continue
            s = self.index[start]
            f = flags(s)
            # one row along +x
            row = [start]
            while len(row) < MAX_AREA_CELLS and not (f & NAV_JUMP):
                n = neighbour(row[-1], 1)
                if n is None or n in owner or flags(self.index[n]) != f or not mutual(row[-1], 1, n):
                    break
                if abs(self.index[n]["z"] - self.index[row[-1]]["z"]) > 8.0:
                    break
                row.append(n)
            # trim the row to an even slope
            def even(cells):
                if len(cells) < 3:
                    return True
                z0, z1 = self.index[cells[0]]["z"], self.index[cells[-1]]["z"]
                for i, k in enumerate(cells):
                    if abs(self.index[k]["z"] - (z0 + (z1 - z0) * i / (len(cells) - 1))) > PLANE_TOLERANCE:
                        return False
                return True
            while not even(row):
                row.pop()
            rows = [row]
            while len(rows) < MAX_AREA_CELLS and not (f & NAV_JUMP):
                nxt = []
                for k in rows[-1]:
                    n = neighbour(k, 2)
                    if n is None or n in owner or flags(self.index[n]) != f or not mutual(k, 2, n):
                        break
                    if nxt and not mutual(nxt[-1], 1, n):
                        break
                    if abs(self.index[n]["z"] - self.index[k]["z"]) > 8.0:
                        break
                    nxt.append(n)
                if len(nxt) != len(rows[-1]) or not even(nxt) or not self.planar(rows + [nxt]):
                    break
                rows.append(nxt)
            rid = len(rects)
            for r in rows:
                for k in r:
                    owner[k] = rid
            rects.append(rows)
        return rects, owner

    def planar(self, rows):
        h, w = len(rows), len(rows[0])
        if h < 2 or w < 2:
            return True
        z = lambda r, c: self.index[rows[r][c]]["z"]
        nw, ne, sw, se = z(0, 0), z(0, w - 1), z(h - 1, 0), z(h - 1, w - 1)
        for r in range(h):
            for c in range(w):
                u, v = c / (w - 1), r / (h - 1)
                want = (nw + u * (ne - nw)) * (1 - v) + (sw + u * (se - sw)) * v
                if abs(z(r, c) - want) > PLANE_TOLERANCE:
                    return False
        return True

    def write(self, path, text=True):
        """the mesh as the game's .nav (text=False) or as botassist's .navtxt (the same areas, one per line: kept in Git as
        text; nav_tools.inl NavFromText turns it into the .nav when the map loads). The .navtxt also has, after the areas,
        a "deck" line per lift or train stop (its area, the mover's class, size, the middle of its floor there and how far that
        floor is below its top) and a "ride" line per way
        a mover or a teleport carries players (kind 0 lift, 1 train, 2 teleport; from and to area; where on and off)."""
        rects, owner = self.areas()
        half = GRID / 2
        areas = []   # [lo.x, lo.y, nwz, hi.x, hi.y, sez, nez, swz, flags, {(dir, index)}]
        for rows in rects:
            first, last = rows[0][0], rows[-1][-1]
            c0 = self.index[first]
            f = (NAV_CROUCH if c0["crouch"] else 0) | (NAV_JUMP if c0["jump"] else 0)
            areas.append([first[0] * GRID - half, first[1] * GRID - half, self.index[rows[0][0]]["z"],
                          last[0] * GRID + half, last[1] * GRID + half, self.index[rows[-1][-1]]["z"],
                          self.index[rows[0][-1]]["z"], self.index[rows[-1][0]]["z"], f, set()])
        for k, c in self.index.items():
            a = owner[k]
            for d, nk in c["edges"]:
                b = owner.get(nk)
                if b is not None and b != a:
                    areas[a][9].add((d, b))
        base = len(areas)

        def z_at(a, x, y):
            w, h = a[3] - a[0], a[4] - a[1]
            u = min(max((x - a[0]) / w, 0), 1) if w > 0 else 0
            v = min(max((y - a[1]) / h, 0), 1) if h > 0 else 0
            north = a[2] + u * (a[6] - a[2])
            south = a[7] + u * (a[5] - a[7])
            return north + v * (south - north)

        def area_at(x, y, z):
            best = None
            for i, a in enumerate(areas[:base]):
                if a[0] - 1 <= x <= a[3] + 1 and a[1] - 1 <= y <= a[4] + 1:
                    dz = z - z_at(a, x, y)
                    if -24 <= dz <= 80 and (best is None or abs(dz) < best[0]):
                        best = (abs(dz), i)
            return best[1] if best else None

        # each stop of a lift or train: an area on its floor, linked by walking to the floor beside it
        decks, rides = [], []
        for m in self.movers:
            ids = []
            for x0, y0, x1, y1, top in m["stops"]:
                d = [x0 + 4, y0 + 4, top, x1 - 4, y1 - 4, top, top, top, 0, set()]
                di = len(areas)
                areas.append(d)
                for i, a in enumerate(areas[:base]):
                    gx = max(a[0] - d[3], d[0] - a[3], 0)
                    gy = max(a[1] - d[4], d[1] - a[4], 0)
                    if gx > 20 or gy > 20 or (gx > 0 and gy > 0):
                        continue
                    x = min(max((d[0] + d[3]) / 2, a[0]), a[3])
                    y = min(max((d[1] + d[4]) / 2, a[1]), a[4])
                    rise = top - z_at(a, x, y)
                    overlap = gx <= 0 and gy <= 0
                    if abs(rise) > (STEP if overlap else JUMP_HEIGHT):
                        continue  # (under it: only the same floor, not a shaft's bottom or what it passes over)
                    if rise > STEP + 2:
                        d[8] |= NAV_JUMP  # a jump up onto it
                    ac, dc = ((a[0] + a[3]) / 2, (a[1] + a[4]) / 2), ((d[0] + d[3]) / 2, (d[1] + d[4]) / 2)
                    if a[0] >= d[3] - 1 or (overlap and ac[0] - dc[0] >= abs(ac[1] - dc[1])):
                        dirn = 1
                    elif a[3] <= d[0] + 1 or (overlap and dc[0] - ac[0] >= abs(ac[1] - dc[1])):
                        dirn = 3
                    elif a[1] >= d[4] - 1 or (overlap and ac[1] >= dc[1]):
                        dirn = 2
                    else:
                        dirn = 0
                    d[9].add((dirn, i))
                    a[9].add(((dirn + 2) % 4, di))
                ids.append(di)
                decks.append((di, m, (x0, y0, x1, y1, top)))
            stops = m["stops"]
            centre = lambda s: ((s[0] + s[2]) / 2, (s[1] + s[3]) / 2, s[4])
            pairs = [(0, 1), (1, 0)] if m["kind"] == 0 else [(i, i + 1) for i in range(len(stops) - 1)]
            for a, b in pairs:
                rides.append((m["kind"], ids[a], ids[b], centre(stops[a]), centre(stops[b])))
        # teleports: from the trigger's floor to the destination's
        named = {e["targetname"]: e for e in self.bsp.entities if "targetname" in e}
        teleports = 0
        for e in self.bsp.entities:
            if e.get("classname") != "trigger_teleport" or not e.get("model", "").startswith("*"):
                continue
            dest = named.get(e.get("target", ""))
            if not dest or "origin" not in dest:
                continue
            m = self.bsp.models[int(e["model"][1:])]
            lo, hi = m["mins"], m["maxs"]
            dx, dy, dz = (float(v) for v in dest["origin"].split())
            gz = self.floor(dx, dy, dz)
            b = area_at(dx, dy, gz) if gz is not None else None
            # the area to step in from: one under or beside the trigger, its floor no lower than a body below it (a
            # trigger at the bottom of a pit, the void under a map: a fall, not a way)
            best = None
            for i, ar in enumerate(areas[:base]):
                gx = max(ar[0] - hi[0], lo[0] - ar[3], 0)
                gy = max(ar[1] - hi[1], lo[1] - ar[4], 0)
                if gx > 64 or gy > 64:
                    continue
                x = min(max((lo[0] + hi[0]) / 2, ar[0]), ar[3])
                y = min(max((lo[1] + hi[1]) / 2, ar[1]), ar[4])
                z = z_at(ar, x, y)
                if not lo[2] - 72 <= z <= hi[2]:
                    continue
                score = gx + gy
                if best is None or score < best[0]:
                    x = min(max(x, lo[0] + 8), hi[0] - 8) if hi[0] - lo[0] > 16 else (lo[0] + hi[0]) / 2
                    y = min(max(y, lo[1] + 8), hi[1] - 8) if hi[1] - lo[1] > 16 else (lo[1] + hi[1]) / 2
                    best = (score, i, (x, y, z))
            if best is None or b is None:
                continue
            rides.append((2, best[1], b, best[2], (dx, dy, gz)))
            teleports += 1

        out = bytearray()
        num = lambda v: ("%.2f" % v).rstrip("0").rstrip(".")
        lines = [f"# botassist navigation mesh of {os.path.basename(self.bsp.path)} (tools/botassist/gen_nav.py)",
                 f"version 5 bspsize {self.bsp.size} areas {len(areas)}"]
        out += struct.pack("<III", 0xFEEDFACE, 5, self.bsp.size)
        out += struct.pack("<H", 0)                     # places
        out += struct.pack("<I", len(areas))
        for i, a in enumerate(areas):
            out += struct.pack("<IB", i + 1, a[8])
            out += struct.pack("<8f", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7])
            row = [str(i + 1), str(a[8])] + [num(v) for v in a[:8]]
            for d in range(4):
                ids = sorted(b + 1 for dd, b in a[9] if dd == d)
                out += struct.pack("<I", len(ids)) + b"".join(struct.pack("<I", x) for x in ids)
                row.append(",".join(map(str, ids)) or "-")
            lines.append(" ".join(row))
            out += struct.pack("<BBIH", 0, 0, 0, 0)    # hiding spots, approach areas, encounter paths, place
        for di, m, (x0, y0, x1, y1, top) in decks:
            lines.append("deck %d %s %s %s %s %s %s %s %s" % (di + 1, m["cls"], num(m["size"][0]), num(m["size"][1]),
                                                          num(m["size"][2]), num((x0 + x1) / 2), num((y0 + y1) / 2), num(top),
                                                          num(m["sink"])))
        for kind, a, b, p, q in rides:
            lines.append("ride %d %d %d %s %s %s %s %s %s" % ((kind, a + 1, b + 1) + tuple(num(v) for v in p + q)))
        if text:
            open(path, "w", newline="\n").write("\n".join(lines) + "\n")
        else:
            open(path, "wb").write(out)
        self.decks, self.rides, self.area_list = decks, rides, areas
        jumps = sum(1 for c in self.index.values() if c["jump"])
        crouch = sum(1 for c in self.index.values() if c["crouch"])
        lonely = sum(1 for di, m, s in decks if not areas[di][9])
        self.log(f"  {base} areas ({crouch} crouch cells, {jumps} jump cells), {len(decks)} lift and train stops "
                 f"({lonely} with no floor beside them), {len(rides)} rides ({teleports} teleports) -> {path}")
        return len(areas)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("maps", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--game", action="store_true", help="map names: extract maps/<name>.bsp from the game first")
    ap.add_argument("--binary", action="store_true", help="write the game's .nav instead of .navtxt")
    a = ap.parse_args()
    sys.setrecursionlimit(10000)
    os.makedirs(a.out, exist_ok=True)
    paths = a.maps
    if a.game:
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        import export_shop_assets as esa
        work = os.path.join(os.environ.get("TEMP", "."), "csnz_gen_nav")
        esa.run_nartool([f"maps/{m}.bsp" for m in a.maps], work)
        paths = [os.path.join(work, "maps", f"{m}.bsp") for m in a.maps]
    for p in paths:
        name = os.path.splitext(os.path.basename(p))[0]
        print(name)
        b = NavBuilder(Bsp(p))
        b.build()
        b.write(os.path.join(a.out, name + (".nav" if a.binary else ".navtxt")), text=not a.binary)


if __name__ == "__main__":
    main()
