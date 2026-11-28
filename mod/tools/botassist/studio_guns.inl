// ---------------------------------------------------------------- Studio guns in the other modes
// Studio's block guns (weapon_vxllonggun = rifle, weapon_vxlshortgun = pistol, weapon_vxlminigun) share one PrimaryAttack
// shape (0x112578b0 / 0x1125a860 / 0x112596f0, thiscall weapon): the shot event, the next-shot time (0.2 s; the minigun
// spins up from 0.3 s to 0.05 s), then a line from the gun (2000 units, the pistol 100) and Studio's "put a block"
// (0x11257d50) or, in the remove mode that the right click switches (weapon +0x1c0: 0 build, 1 remove; 0x11257aa0),
// "take a block away" (0x11258360). Outside a Studio map (the voxel maps load vxl_empty) there's no voxel world: they
// did nothing, and the "put a block" read a missing record and closed the game (issue #34, the minigun bought in
// Zombie Disaster Zone; apply_patches.py guards that read too).
// The user (2026-11-28): make them playable in the other modes, like the Junk Grenade's drums: shots that throw random
// objects which blow up on the enemies, or blocks that explode or can be put on the map like Studio's. So, on any map
// but Studio's (the voxel calls are skipped there, the game's own event, animation and timing stay):
//  - Rifle: throws one of Studio's own objects (models/studio_obj: sofas, TVs, a piano, fridges, a giant duck, a small
//    car...) in an arc; it blows up on what it hits. Right click (the remove mode): the objects bounce and blow up
//    1.6 s later, like grenades (at once on an enemy).
//  - Minigun: a stream of small Studio objects (phones, clocks, globes, trophies, ammo boxes...), small blasts; it
//    keeps the game's spin-up. Right click: bouncing ones, 1 s.
//  - Pistol: puts Studio blocks (40-unit cells on the world's 40 grid, on the face you aim at, up to 400 units: they
//    stack like Studio's) that are solid and take damage; a block blows up when it's broken (credited to its owner, so
//    a zombie breaking a human's block is hurt by it). Up to 12 a player (the oldest goes). Right click (the remove
//    mode): a shot at one of your blocks sets it off; anywhere else sets them all off one after another.
// The objects are info_targets the engine moves (MOVETYPE_NOCLIP, botassist adds the gravity and checks the path each
// frame: walls, floors, players, monsters, blocks); the blocks are info_targets with a box (SOLID_BBOX) and health:
// CBaseEntity::TakeDamage (0x113e80a0) takes it off and Killed (0x113e7390) clears takedamage and removes them. The
// models are precached with the drums at each map load (grenade_fixes.inl), only the ones the file system has.
static const DWORD MP_VXL_LONG_PRIMARY = 0x112578b0, MP_VXL_SHORT_PRIMARY = 0x1125a860, MP_VXL_MINI_PRIMARY = 0x112596f0;
static const DWORD MP_VXL_PUT = 0x11257d50, MP_VXL_TAKE = 0x11258360;
static const DWORD SG_UTIL_SETORIGIN = 0x11519ec0, SG_UTIL_SETSIZE = 0x11519ee0; // cdecl (pev, origin) / (pev, mins, maxs)
static const int VXL_MODE = 0x1c0, VXL_NEXT_IDLE = 0x144;
static const int SG_EV_MOVETYPE = 0x118, SG_EV_AVELOCITY = 0x5c, SG_EV_DEADFLAG = 0x194;
static const int SG_MOVETYPE_NONE = 0, SG_MOVETYPE_NOCLIP = 8, SG_SOLID_NOT = 0, SG_SOLID_BBOX = 2;
static const int SG_FL_KILLME = 0x40000000;
static const float SG_GRAVITY = 800.0f, SG_CELL = 40.0f, SG_REACH = 400.0f;
static const int SG_BLOCKS_EACH = 12, SG_OBJECTS_MAX = 64;

enum SgKind
{
    SG_RIFLE,
    SG_PISTOL,
    SG_MINIGUN
};

typedef int(__cdecl* SgPutFn)(void* a, void* b, void* c, void* d, void* e, void* f);
typedef int(__cdecl* SgTakeFn)(void* a, void* b, void* c, void* d, void* e);
typedef void (*SgSetOriginFn)(void* pev, const float* origin);
typedef void (*SgSetSizeFn)(void* pev, const float* mins, const float* maxs);
static WeaponFn g_sgLong, g_sgShort, g_sgMini;
static SgPutFn g_sgPut;
static SgTakeFn g_sgTake;

// Studio's objects (models/studio_obj, in fstrike.nar): big ones for the rifle, small ones for the minigun, and for
// the blocks the ones that fill one 40-unit cell standing on its centre (their bottom at the origin)
static const char* const kSgRifleModels[] = {
    "models/studio_obj/obj_1psofa01.mdl",        "models/studio_obj/obj_tv01.mdl",
    "models/studio_obj/obj_piano01.mdl",         "models/studio_obj/obj_refrigerator03_off.mdl",
    "models/studio_obj/obj_washing.mdl",         "models/studio_obj/obj_oven01.mdl",
    "models/studio_obj/obj_safebox01.mdl",       "models/studio_obj/obj_chair01.mdl",
    "models/studio_obj/obj_beachball01.mdl",     "models/studio_obj/obj_duck03.mdl",
    "models/studio_obj/obj_smollcar01.mdl",      "models/studio_obj/obj_horse01.mdl",
    "models/studio_obj/obj_gameconsole01.mdl",   "models/studio_obj/obj_coffeemachine.mdl",
};
static const char* const kSgMiniModels[] = {
    "models/studio_obj/obj_phone01.mdl",       "models/studio_obj/obj_globe01.mdl",
    "models/studio_obj/obj_trophy01.mdl",      "models/studio_obj/obj_c401.mdl",
    "models/studio_obj/obj_ammunition01.mdl",  "models/studio_obj/obj_ammunition02.mdl",
    "models/studio_obj/obj_supplybox01.mdl",   "models/studio_obj/obj_notebook01.mdl",
    "models/studio_obj/obj_violin01.mdl",      "models/studio_obj/obj_spoonholder01.mdl",
    "models/studio_obj/obj_moviecamera01.mdl", "models/studio_obj/obj_basketball01.mdl",
};
static const char* const kSgBlockModels[] = {
    "models/studio_obj/obj_oakbox01.mdl",    "models/studio_obj/obj_1psofa01.mdl",
    "models/studio_obj/obj_seechair01.mdl",  "models/studio_obj/obj_chair_sofa01.mdl",
};

struct SgObject
{
    void* edict;
    void* priv;
    void* owner; // the shooter's edict
    V3 pos;      // where it was last frame
    float born, fuse, grav;
    int kind;
    bool bounce, resting;
    int level;
};
struct SgBlock
{
    void* edict;
    void* priv;
    void* owner;
    V3 center;
    float boomAt; // set off (right click), 0 = standing
    int level;
};
static std::vector<SgObject> g_sgObjects;
static std::vector<SgBlock> g_sgBlocks;
static std::vector<const char*> g_sgRifleMdl, g_sgMiniMdl, g_sgBlockMdl; // the ones precached on this map
static std::string g_sgMap;
static float g_sgLastFrame = 0;
static unsigned g_sgTold[33]; // the hint shown once a map to each player, per gun (bits)
static int g_sgToldLevel = -1;

static bool SgVoxelMap()
{
    const char* map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    return map && !_strnicmp(map, "vxl", 3);
}

static bool SgReady()
{
    std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    return !g_sgMap.empty() && map == g_sgMap;
}

// grenade_fixes.inl: while the map loads. A model the file system can't open isn't precached (a missing one would
// end the map load), the answers kept for the next maps
static void SgPrecache(PrecacheFn model, PrecacheFn sound)
{
    (void)sound;
    static std::map<std::string, bool> found;
    auto add = [&](const char* const* list, size_t n, std::vector<const char*>& out) {
        out.clear();
        for (size_t i = 0; i < n; i++)
        {
            auto it = found.find(list[i]);
            if (it == found.end())
            {
                bool ok = false;
                if (g_LoadFile && g_FreeFile)
                {
                    int len = 0;
                    unsigned char* data = g_LoadFile(list[i], &len);
                    ok = data && len > 0;
                    if (data)
                        g_FreeFile(data);
                }
                it = found.emplace(list[i], ok).first;
                if (!ok)
                    Log("Studio guns: %s isn't in the game files, left out", list[i]);
            }
            if (!it->second)
                continue;
            model(list[i]);
            out.push_back(list[i]);
        }
    };
    add(kSgRifleModels, _countof(kSgRifleModels), g_sgRifleMdl);
    add(kSgMiniModels, _countof(kSgMiniModels), g_sgMiniMdl);
    add(kSgBlockModels, _countof(kSgBlockModels), g_sgBlockMdl);
    g_sgMap = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
}

static bool SgOwnerOk(void* owner)
{
    return owner && *(int*)owner == 0 && PrivateOf(owner) && (F<int>(Vars(owner), EV_FLAGS) & FL_CLIENT);
}

static float SgRand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rand() % 10001) / 10000.0f;
}

static void* SgSpawnTarget(V3 at, const char* model)
{
    static const char kClass[] = "info_target";
    const char* base = *(const char**)((DWORD)*g_pGlobals + GV_STRINGBASE);
    void* edict = ((CreateNamedFn)(MP_CREATE_NAMED + g_mpDelta))((int)(kClass - base), 0);
    if (!edict || !PrivateOf(edict))
        return nullptr;
    unsigned char* v = Vars(edict);
    (*(SfSetModelFn*)(MP_SET_MODEL + g_mpDelta))(edict, model);
    float o[3] = { at.x, at.y, at.z };
    ((SgSetOriginFn)(SG_UTIL_SETORIGIN + g_mpDelta))(v, o);
    return edict;
}

static void SgKill(void* edict)
{
    unsigned char* v = Vars(edict);
    F<float>(v, EV_TAKEDAMAGE) = 0;
    F<int>(v, EV_SOLID_OFS) = SG_SOLID_NOT;
    F<int>(v, EV_FLAGS) |= SG_FL_KILLME;
}

static void SgBlast(V3 at, void* owner, int kind)
{
    if (!SgOwnerOk(owner))
        return;
    switch (kind)
    {
    case SG_RIFLE:
        SfArea(at, owner, ByMode(65, 800, 1100), 150.0f, 320.0f, 160.0f, 22);
        break;
    case SG_MINIGUN:
        SfArea(at, owner, ByMode(20, 170, 240), 85.0f, 0, 0, 8);
        break;
    default: // a block
        SfArea(at, owner, ByMode(90, 1000, 1400), 170.0f, 420.0f, 220.0f, 28);
        break;
    }
}

// ---------------------------------------------------------------- the thrown objects
static void SgThrow(void* player, int kind, bool bounce)
{
    std::vector<const char*>& list = kind == SG_RIFLE ? g_sgRifleMdl : g_sgMiniMdl;
    if (list.empty())
        return;
    unsigned char* p = (unsigned char*)PrivateOf(player);
    void** vt = p ? *(void***)p : nullptr;
    if (!vt || !Readable(vt + PL_VT_GUNPOS, 4))
        return;
    float src[3] = { 0, 0, 0 };
    ((GunPositionFn)vt[PL_VT_GUNPOS])(p, nullptr, src);
    V3 eye = Eye(player), dir = SfAim(player);
    if (kind == SG_MINIGUN)
    {
        dir = dir + V3{ SgRand(-0.035f, 0.035f), SgRand(-0.035f, 0.035f), SgRand(-0.025f, 0.025f) };
        dir = dir * (1.0f / Len(dir));
    }
    V3 start = Vec(src) + dir * 18.0f;
    TraceResult tr;
    if (!Trace(eye, start, player, 0, tr)) // a wall in the gun's face: it goes off right there
    {
        SgBlast(Vec(tr.vecEndPos) - dir * 10.0f, player, kind);
        return;
    }
    // too many in the air: the oldest goes (quietly)
    if ((int)g_sgObjects.size() >= SG_OBJECTS_MAX)
    {
        if (*(int*)g_sgObjects[0].edict == 0 && PrivateOf(g_sgObjects[0].edict) == g_sgObjects[0].priv)
            SgKill(g_sgObjects[0].edict);
        g_sgObjects.erase(g_sgObjects.begin());
    }
    void* e = SgSpawnTarget(start, list[rand() % list.size()]);
    if (!e)
        return;
    unsigned char* v = Vars(e);
    F<int>(v, EV_SOLID_OFS) = SG_SOLID_NOT;
    F<int>(v, SG_EV_MOVETYPE) = SG_MOVETYPE_NOCLIP;
    float speed = kind == SG_RIFLE ? 1100.0f : 1700.0f;
    V3 vel = dir * speed + V3{ 0, 0, kind == SG_RIFLE ? 130.0f : 40.0f };
    V3 pv = Vec((float*)(Vars(player) + EV_VELOCITY));
    vel = vel + pv * 0.5f;
    float* fv = (float*)(v + EV_VELOCITY);
    fv[0] = vel.x, fv[1] = vel.y, fv[2] = vel.z;
    float* ang = (float*)(v + EV_ANGLES);
    ang[0] = SgRand(0, 360), ang[1] = SgRand(0, 360), ang[2] = 0;
    float* av = (float*)(v + SG_EV_AVELOCITY);
    float spin = kind == SG_RIFLE ? 360.0f : 720.0f;
    av[0] = SgRand(-spin, spin), av[1] = SgRand(-spin, spin), av[2] = SgRand(-spin, spin);
    float now = Now();
    float fuse = bounce ? (kind == SG_RIFLE ? 1.6f : 1.0f) : 6.0f;
    g_sgObjects.push_back({ e, PrivateOf(e), player, start, now, now + fuse, kind == SG_RIFLE ? 0.55f : 0.25f, kind,
                            bounce, false, g_nav.level });
}

static bool SgEnemyHit(void* hit, void* owner)
{
    if (!hit || hit == owner || *(int*)hit != 0)
        return false;
    int flags = F<int>(Vars(hit), EV_FLAGS);
    return (flags & (FL_CLIENT | FL_MONSTER)) != 0 && F<float>(Vars(hit), EV_TAKEDAMAGE) != 0.0f;
}

// one object, one frame; false when it's gone
static bool SgObjectFrame(SgObject& o, float now, float dt)
{
    if (o.level != g_nav.level || *(int*)o.edict != 0 || PrivateOf(o.edict) != o.priv)
        return false;
    unsigned char* v = Vars(o.edict);
    if (!SgOwnerOk(o.owner)) // the thrower left
    {
        SgKill(o.edict);
        return false;
    }
    V3 cur = Origin(o.edict);
    float* fv = (float*)(v + EV_VELOCITY);
    V3 vel = Vec(fv);
    if (!o.resting && Len(cur - o.pos) > 0.1f)
    {
        TraceResult tr;
        Trace(o.pos, cur, o.owner, 0, tr);
        if (tr.flFraction < 1.0f || tr.fStartSolid)
        {
            V3 hit = Vec(tr.vecEndPos), n = Vec(tr.vecPlaneNormal);
            V3 dir = cur - o.pos;
            dir = dir * (1.0f / Len(dir));
            bool enemy = SgEnemyHit(tr.pHit, o.owner);
            if (!o.bounce || enemy || tr.fStartSolid || Len(n) < 0.5f)
            {
                SgKill(o.edict);
                SgBlast(hit - dir * 10.0f, o.owner, o.kind);
                return false;
            }
            // a bounce: back to the hit point, the speed mirrored on the surface and slowed
            float d = vel.x * n.x + vel.y * n.y + vel.z * n.z;
            vel = (vel - n * (2.0f * d)) * 0.5f;
            V3 back = hit + n * 2.0f;
            float o3[3] = { back.x, back.y, back.z };
            ((SgSetOriginFn)(SG_UTIL_SETORIGIN + g_mpDelta))(v, o3);
            cur = back;
            if (n.z > 0.7f && Len(vel) < 90.0f)
            {
                o.resting = true;
                vel = { 0, 0, 0 };
                float* av = (float*)(v + SG_EV_AVELOCITY);
                av[0] = av[1] = av[2] = 0;
            }
        }
    }
    if (now >= o.fuse || now < o.born - 1.0f)
    {
        SgKill(o.edict);
        SgBlast(cur + V3{ 0, 0, 8 }, o.owner, o.kind);
        return false;
    }
    if (!o.resting)
        vel.z -= SG_GRAVITY * o.grav * dt;
    fv[0] = vel.x, fv[1] = vel.y, fv[2] = vel.z;
    o.pos = cur;
    return true;
}

// ---------------------------------------------------------------- the pistol's blocks
static bool SgBlockStanding(const SgBlock& b)
{
    if (b.level != g_nav.level || *(int*)b.edict != 0 || PrivateOf(b.edict) != b.priv)
        return false;
    unsigned char* v = Vars(b.edict);
    return F<float>(v, EV_TAKEDAMAGE) != 0.0f && F<int>(v, SG_EV_DEADFLAG) == 0 && !(F<int>(v, EV_FLAGS) & SG_FL_KILLME);
}

static int SgBlocksOf(void* owner)
{
    int n = 0;
    for (const SgBlock& b : g_sgBlocks)
        if (b.owner == owner && b.boomAt == 0)
            n++;
    return n;
}

static bool SgPlaceBlock(void* player)
{
    if (g_sgBlockMdl.empty())
        return false;
    V3 eye = Eye(player), dir = SfAim(player);
    TraceResult tr;
    if (Trace(eye, eye + dir * SG_REACH, player, 0, tr) || tr.fStartSolid)
        return false; // nothing within reach
    V3 n = Vec(tr.vecPlaneNormal);
    if (Len(n) < 0.5f)
        n = dir * -1.0f;
    V3 p = Vec(tr.vecEndPos) + n * (SG_CELL * 0.5f);
    float bx = floorf(p.x / SG_CELL) * SG_CELL, by = floorf(p.y / SG_CELL) * SG_CELL, bz = floorf(p.z / SG_CELL) * SG_CELL;
    V3 base = { bx + SG_CELL * 0.5f, by + SG_CELL * 0.5f, bz }; // the cell's bottom centre
    V3 mid = base + V3{ 0, 0, SG_CELL * 0.5f };
    // the cell must be free: no wall or block in it (a hull smaller than the cell, so the neighbours don't count)...
    if (g_TraceHull)
    {
        TraceResult hull;
        memset(&hull, 0, sizeof(hull));
        float c[3] = { mid.x, mid.y, mid.z };
        g_TraceHull(c, c, 0, 3 /* 32x32x36 */, nullptr, &hull);
        if (hull.fStartSolid || hull.fAllSolid)
            return false;
    }
    // ...and nobody standing in it
    bool taken = false;
    ForEntitiesNear(mid, 90.0f, [&](void* e) {
        if (taken || !(F<int>(Vars(e), EV_FLAGS) & (FL_CLIENT | FL_MONSTER)) || !Alive(e))
            return;
        float* lo = (float*)(Vars(e) + EV_ABSMIN);
        float* hi = (float*)(Vars(e) + EV_ABSMAX);
        if (lo[0] < base.x + 20 && hi[0] > base.x - 20 && lo[1] < base.y + 20 && hi[1] > base.y - 20 && lo[2] < base.z + 40 &&
            hi[2] > base.z)
            taken = true;
    });
    if (taken)
        return false;
    // the player's oldest block goes when he has them all
    if (SgBlocksOf(player) >= SG_BLOCKS_EACH)
        for (size_t i = 0; i < g_sgBlocks.size(); i++)
            if (g_sgBlocks[i].owner == player && g_sgBlocks[i].boomAt == 0)
            {
                if (SgBlockStanding(g_sgBlocks[i]))
                    SgKill(g_sgBlocks[i].edict);
                g_sgBlocks.erase(g_sgBlocks.begin() + i);
                break;
            }
    void* e = SgSpawnTarget(base, g_sgBlockMdl[rand() % g_sgBlockMdl.size()]);
    if (!e)
        return false;
    unsigned char* v = Vars(e);
    F<int>(v, SG_EV_MOVETYPE) = SG_MOVETYPE_NONE;
    F<int>(v, EV_SOLID_OFS) = SG_SOLID_BBOX;
    ((float*)(v + EV_ANGLES))[1] = (float)(rand() % 4) * 90.0f;
    F<float>(v, EV_TAKEDAMAGE) = 1.0f; // DAMAGE_YES
    F<float>(v, EV_HEALTH) = ByMode(120, 1200, 1500);
    float mins[3] = { -20, -20, 0 }, maxs[3] = { 20, 20, 40 }, o[3] = { base.x, base.y, base.z };
    ((SgSetSizeFn)(SG_UTIL_SETSIZE + g_mpDelta))(v, mins, maxs);
    ((SgSetOriginFn)(SG_UTIL_SETORIGIN + g_mpDelta))(v, o); // (linked with its box)
    g_sgBlocks.push_back({ e, PrivateOf(e), player, mid, 0, g_nav.level });
    return true;
}

// the remove mode: the block aimed at, or all the player's blocks one after another
static void SgDetonate(void* player)
{
    V3 eye = Eye(player), dir = SfAim(player);
    TraceResult tr;
    Trace(eye, eye + dir * 2000.0f, player, 0, tr);
    float now = Now(), at = now;
    for (SgBlock& b : g_sgBlocks)
        if (b.owner == player && b.boomAt == 0 && b.edict == tr.pHit)
        {
            b.boomAt = now;
            return;
        }
    for (SgBlock& b : g_sgBlocks)
        if (b.owner == player && b.boomAt == 0)
        {
            b.boomAt = at;
            at += 0.15f;
        }
}

static void SgBlocksFrame(float now)
{
    for (size_t i = 0; i < g_sgBlocks.size();)
    {
        SgBlock b = g_sgBlocks[i];
        if (b.level != g_nav.level)
        {
            g_sgBlocks.erase(g_sgBlocks.begin() + i);
            continue;
        }
        bool standing = SgBlockStanding(b);
        bool boom = !standing; // broken: its blast
        if (standing && b.boomAt > 0 && (now >= b.boomAt || now < b.boomAt - 30.0f))
            boom = true;
        if (!boom)
        {
            i++;
            continue;
        }
        if (standing)
            SgKill(b.edict);
        else if (*(int*)b.edict == 0 && PrivateOf(b.edict) == b.priv)
            F<int>(Vars(b.edict), EV_SOLID_OFS) = SG_SOLID_NOT;
        g_sgBlocks.erase(g_sgBlocks.begin() + i); // (before the blast: it may break the next ones)
        SgBlast(b.center, b.owner, SG_PISTOL);
    }
}

// ---------------------------------------------------------------- the guns
static void SgHint(void* player, int kind)
{
    int i = SfIndex(player);
    if (!i)
        return;
    if (g_sgToldLevel != g_nav.level)
    {
        memset(g_sgTold, 0, sizeof(g_sgTold));
        g_sgToldLevel = g_nav.level;
    }
    if (g_sgTold[i] & (1u << kind))
        return;
    g_sgTold[i] |= 1u << kind;
    static const char* const kHints[] = {
        "Studio Rifle: throws objects that blow up - right click: bouncing ones",
        "Studio Pistol: puts blocks that blow up when broken - right click: set them off",
        "Studio Minigun: a stream of exploding junk - right click: bouncing ones",
    };
    SfSay(player, kHints[kind]);
}

static void SgShot(void* weapon, int kind, float nextBefore)
{
    unsigned char* w = (unsigned char*)weapon;
    unsigned char* pev = WeaponOwnerPev(weapon);
    if (!pev || !SgReady())
        return;
    if (*(float*)(w + WPN_NEXT_PRIMARY) == nextBefore)
        return; // (no shot)
    void* player = *(void**)(pev + EV_CONTAINING_ENTITY);
    if (!player || !Alive(player))
        return;
    bool alt = *(int*)(w + VXL_MODE) != 0;
    SgHint(player, kind);
    float next = 0.3f;
    switch (kind)
    {
    case SG_RIFLE:
        SgThrow(player, SG_RIFLE, alt);
        next = 0.45f;
        break;
    case SG_MINIGUN:
        SgThrow(player, SG_MINIGUN, alt);
        next = (std::max)(*(float*)(w + WPN_NEXT_PRIMARY), 0.08f); // (the game's spin-up)
        break;
    default:
        if (alt)
        {
            SgDetonate(player);
            next = 0.5f;
        }
        else
            next = SgPlaceBlock(player) ? 0.3f : 0.2f;
        break;
    }
    *(float*)(w + WPN_NEXT_PRIMARY) = next;
}

static void SgPrimary(WeaponFn original, void* weapon, void* edx, int kind)
{
    unsigned char* w = (unsigned char*)weapon;
    float next = Readable(w + WPN_NEXT_PRIMARY, 4) ? *(float*)(w + WPN_NEXT_PRIMARY) : 0;
    original(weapon, edx);
    __try
    {
        if (!SgVoxelMap())
            SgShot(weapon, kind, next);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GunFixFault("Studio guns", GetExceptionCode());
    }
}

static void __fastcall HookedSgLong(void* weapon, void* edx) { SgPrimary(g_sgLong, weapon, edx, SG_RIFLE); }
static void __fastcall HookedSgShort(void* weapon, void* edx) { SgPrimary(g_sgShort, weapon, edx, SG_PISTOL); }
static void __fastcall HookedSgMini(void* weapon, void* edx) { SgPrimary(g_sgMini, weapon, edx, SG_MINIGUN); }

// Studio's put / take a block: only on Studio's maps (there's no voxel world on the others)
static int __cdecl HookedSgPut(void* a, void* b, void* c, void* d, void* e, void* f)
{
    if (!SgVoxelMap())
        return 0;
    return g_sgPut(a, b, c, d, e, f);
}

static int __cdecl HookedSgTake(void* a, void* b, void* c, void* d, void* e)
{
    if (!SgVoxelMap())
        return 0;
    return g_sgTake(a, b, c, d, e);
}

// every frame
static void StudioGunsFrame()
{
    if (g_sgObjects.empty() && g_sgBlocks.empty())
    {
        g_sgLastFrame = 0;
        return;
    }
    float now = Now();
    float dt = g_sgLastFrame > 0 && now > g_sgLastFrame ? (std::min)(now - g_sgLastFrame, 0.1f) : 0.0f;
    g_sgLastFrame = now;
    __try
    {
        for (size_t i = 0; i < g_sgObjects.size();)
            if (SgObjectFrame(g_sgObjects[i], now, dt))
                i++;
            else
                g_sgObjects.erase(g_sgObjects.begin() + i);
        SgBlocksFrame(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("Studio guns: exception 0x%08lX", GetExceptionCode());
        g_sgObjects.clear();
        g_sgBlocks.clear();
    }
}

static void FixStudioGuns()
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sbotassist.ini", g_dir);
    if (!GetPrivateProfileIntA("BotAssist", "StudioGuns", 1, ini))
    {
        Log("Studio guns: left as they are (StudioGuns=0 in botassist.ini)");
        return;
    }
    const unsigned char prim[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x30 }; // push ebp; mov ebp, esp; sub esp, 0x30
    const unsigned char put[] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8 };  // ... and esp, -8
    const unsigned char take[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x08 }; // ... sub esp, 8
    if (!g_sgPut)
        g_sgPut = (SgPutFn)DetourChecked(MP_VXL_PUT, put, sizeof(put), (void*)HookedSgPut, "Studio guns (put a block)");
    if (!g_sgTake)
        g_sgTake = (SgTakeFn)DetourChecked(MP_VXL_TAKE, take, sizeof(take), (void*)HookedSgTake, "Studio guns (take a block)");
    if (g_sgPut && g_sgTake)
    {
        if (!g_sgLong)
            g_sgLong = (WeaponFn)DetourChecked(MP_VXL_LONG_PRIMARY, prim, sizeof(prim), (void*)HookedSgLong, "Studio rifle");
        if (!g_sgShort)
            g_sgShort = (WeaponFn)DetourChecked(MP_VXL_SHORT_PRIMARY, prim, sizeof(prim), (void*)HookedSgShort, "Studio pistol");
        if (!g_sgMini)
            g_sgMini = (WeaponFn)DetourChecked(MP_VXL_MINI_PRIMARY, prim, sizeof(prim), (void*)HookedSgMini, "Studio minigun");
    }
    Log("Studio guns: rifle %s, pistol %s, minigun %s outside Studio's maps (objects precached from the next map loaded)",
        g_sgLong ? "throws exploding objects" : "left as it is", g_sgShort ? "puts exploding blocks" : "left as it is",
        g_sgMini ? "sprays exploding junk" : "left as it is");
}
