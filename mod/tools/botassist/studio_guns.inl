// ---------------------------------------------------------------- Studio guns in the other modes
// Studio's block guns (weapon_vxllonggun = rifle, weapon_vxlshortgun = pistol, weapon_vxlminigun) share one PrimaryAttack
// shape (0x112578b0 / 0x1125a860 / 0x112596f0, thiscall weapon): the shot event, the next-shot time (0.2 s; the minigun
// spins up from 0.3 s to 0.05 s), then a line from the gun (2000 units, the pistol 100) and Studio's "put a block"
// (0x11257d50) or, in the remove mode that the right click switches (weapon +0x1c0: 0 build, 1 remove; 0x11257aa0),
// "take a block away" (0x11258360). Outside a Studio map (the voxel maps load vxl_empty) there's no voxel world: they
// did nothing, and the "put a block" read a missing record and closed the game (issue #34, the minigun bought in
// Zombie Disaster Zone; apply_patches.py guards that read too).
// Studio's other "pistols" read the same per-player Studio record (data manager [0x121d3d78]() vfunc 0x68(player
// index)), which is null off Studio's maps:
//  - the marker (weapon_vxlmarker, item 8413): 0x11258660 (its Deploy 0x112586f0, PrimaryAttack 0x11258df0 and
//    SecondaryAttack 0x11258f80 call it) read record +0x60 unchecked: the game closed as soon as it was drawn (the user,
//    2026-11-28, mp.dll+0x12586a7); its PrimaryAttack's worker 0x11258af0 uses it too;
//  - the scanner (weapon_vxlscanner, 992): PrimaryAttack/SecondaryAttack = 0x1125a1a0(0/1) (thiscall, ret 4): the
//    timing (0.7 s), the player's animation, the shot event (bparam1 = 0/1), then the record;
//  - the junk gun (weapon_vxljunkgun, 993): PrimaryAttack 0x11256b90 needs a round (+0x160 > 0, else the empty
//    sound), plays the shot and hits 200 units ahead through 0x112571b0 (cdecl, 3 args, the record; a round is taken
//    when it returns true).
// Off Studio's maps botassist skips those record users (the marker's state stays 0) and gives the guns their own use.
// The user (2026-11-28): make them playable in the other modes, like the Junk Grenade's drums: shots that throw random
// objects which blow up on the enemies, or blocks that explode or can be put on the map like Studio's; then ammo
// (minigun 200, pistol 50, rifle 150) and the remaining pistols. So, on any map but Studio's:
//  - Rifle (150 rounds): throws one of Studio's own objects (models/studio_obj: sofas, TVs, a piano, fridges, a giant
//    duck, a small car...) in an arc; it blows up on what it hits. Right click (the remove mode): the objects bounce
//    and blow up 1.6 s later, like grenades (at once on an enemy).
//  - Minigun (200): a stream of small Studio objects (phones, clocks, globes, trophies, ammo boxes...), small blasts;
//    it keeps the game's spin-up. Right click: bouncing ones, 1 s.
//  - Pistol (50, a round a block): puts Studio blocks (40-unit cells on the world's 40 grid, on the face you aim at,
//    up to 400 units: they stack like Studio's) that are solid and take damage; a block blows up when it's broken
//    (credited to its owner, so a zombie breaking a human's block is hurt by it). Up to 12 a player (the oldest goes).
//    Right click (the remove mode): a shot at one of your blocks sets it off; anywhere else sets them all off one
//    after another.
//  - Scanner (40): a small object that homes in on the enemy nearest to where it flies; right click: three at once
//    (three rounds).
//  - Marker (20): puts a mine (the marker's own models/w_vxlbuildmarker.mdl) where you aim, up to 400 units; it goes
//    off when an enemy comes near (after 0.8 s; 6 a player, 90 s each). Right click: set your mines off.
//  - Junk gun (60): a short-range spray of five small objects that blow up (a shotgun of junk).
// Rounds are kept per weapon (bought again = full) and shown in the weapon's clip (+0x160, which the junk gun's own
// code reads); empty, the gun only clicks.
// The objects are info_targets the engine moves (MOVETYPE_NOCLIP, botassist adds the gravity and checks the path each
// frame: walls, floors, players, monsters, blocks); the blocks are info_targets with a box (SOLID_BBOX) and health:
// CBaseEntity::TakeDamage (0x113e80a0) takes it off and Killed (0x113e7390) clears takedamage and removes them. The
// models are precached with the drums at each map load (grenade_fixes.inl), only the ones the file system has.
static const DWORD MP_VXL_LONG_PRIMARY = 0x112578b0, MP_VXL_SHORT_PRIMARY = 0x1125a860, MP_VXL_MINI_PRIMARY = 0x112596f0;
static const DWORD MP_VXL_PUT = 0x11257d50, MP_VXL_TAKE = 0x11258360;
static const DWORD MP_VXL_MARKER_STATE = 0x11258660, MP_VXL_MARKER_WORK = 0x11258af0, MP_VXL_MARKER_PRIMARY = 0x11258df0,
                   MP_VXL_MARKER_SECONDARY = 0x11258f80, MP_VXL_SCANNER_WORK = 0x1125a1a0, MP_VXL_JUNK_PRIMARY = 0x11256b90,
                   MP_VXL_JUNK_HIT = 0x112571b0;
static const DWORD SG_UTIL_SETORIGIN = 0x11519ec0, SG_UTIL_SETSIZE = 0x11519ee0; // cdecl (pev, origin) / (pev, mins, maxs)
static const int VXL_MODE = 0x1c0, VXL_NEXT_IDLE = 0x144, PL_VT_SETANIMATION = 0x22c / 4;
static const int SG_EV_MOVETYPE = 0x118, SG_EV_AVELOCITY = 0x5c, SG_EV_DEADFLAG = 0x194;
static const int SG_MOVETYPE_NONE = 0, SG_MOVETYPE_NOCLIP = 8, SG_SOLID_NOT = 0, SG_SOLID_BBOX = 2;
static const int SG_FL_KILLME = 0x40000000;
static const float SG_GRAVITY = 800.0f, SG_CELL = 40.0f, SG_REACH = 400.0f;
static const int SG_BLOCKS_EACH = 12, SG_MINES_EACH = 6, SG_OBJECTS_MAX = 80;

enum SgKind
{
    SG_RIFLE,
    SG_PISTOL,
    SG_MINIGUN,
    SG_SCANNER,
    SG_MARKER,
    SG_JUNK,
    SG_KINDS
};
static const char* const kSgClass[SG_KINDS] = { "weapon_vxllonggun", "weapon_vxlshortgun", "weapon_vxlminigun",
                                                "weapon_vxlscanner", "weapon_vxlmarker",   "weapon_vxljunkgun" };
static const int kSgRounds[SG_KINDS] = { 150, 50, 200, 40, 20, 60 };

typedef int(__cdecl* SgPutFn)(void* a, void* b, void* c, void* d, void* e, void* f);
typedef int(__cdecl* SgTakeFn)(void* a, void* b, void* c, void* d, void* e);
typedef int(__cdecl* SgJunkHitFn)(void* a, void* b, void* c);
typedef void(__fastcall* SgScanFn)(void* weapon, void* edx, int mode);
typedef void (*SgSetOriginFn)(void* pev, const float* origin);
typedef void (*SgSetSizeFn)(void* pev, const float* mins, const float* maxs);
typedef void(__fastcall* SgSetAnimFn)(void* player, void* edx, int anim);
static WeaponFn g_sgLong, g_sgShort, g_sgMini, g_sgMarkerState, g_sgMarkerWork, g_sgMarkerPrimary, g_sgMarkerSecondary,
    g_sgJunkPrimary;
static SgPutFn g_sgPut;
static SgTakeFn g_sgTake;
static SgJunkHitFn g_sgJunkHit;
static SgScanFn g_sgScan;

// Studio's objects (models/studio_obj, in fstrike.nar): big ones for the rifle, small ones for the minigun, the
// scanner and the junk gun, and for the blocks the ones that fill one 40-unit cell standing on its centre (their bottom
// at the origin)
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
static const char* const kSgMineModels[] = { "models/w_vxlbuildmarker.mdl" };

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
    void* target = nullptr; // the scanner's: the enemy it flies to
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
struct SgMine
{
    void* edict;
    void* priv;
    void* owner;
    V3 at;
    float armAt, until, boomAt;
    int level;
};
struct SgRounds
{
    void* edict; // the weapon's: a weapon bought again is another entity (or the same one with another serial)
    int serial;
    int left;
    int kind;
};
static std::vector<SgObject> g_sgObjects;
static std::vector<SgBlock> g_sgBlocks;
static std::vector<SgMine> g_sgMines;
static std::map<void*, SgRounds> g_sgRounds; // weapon (private data) -> its rounds
static int g_sgRoundsLevel = -1;
static std::vector<const char*> g_sgRifleMdl, g_sgMiniMdl, g_sgBlockMdl, g_sgMineMdl; // the ones precached on this map
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
    add(kSgMineModels, _countof(kSgMineModels), g_sgMineMdl);
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
    case SG_SCANNER:
        SfArea(at, owner, ByMode(55, 650, 900), 120.0f, 250.0f, 120.0f, 16);
        break;
    case SG_MARKER:
        SfArea(at, owner, ByMode(100, 1200, 1600), 180.0f, 450.0f, 260.0f, 30);
        break;
    case SG_JUNK:
        SfArea(at, owner, ByMode(16, 130, 180), 75.0f, 120.0f, 60.0f, 7);
        break;
    default: // a block
        SfArea(at, owner, ByMode(90, 1000, 1400), 170.0f, 420.0f, 220.0f, 28);
        break;
    }
}

// ---------------------------------------------------------------- the thrown objects
struct SgThrowSpec
{
    float speed, lift, grav, spread, fuse, spin;
};

static SgThrowSpec SgSpec(int kind, bool bounce)
{
    switch (kind)
    {
    case SG_RIFLE:
        return { 1100.0f, 130.0f, 0.55f, 0.0f, bounce ? 1.6f : 6.0f, 360.0f };
    case SG_SCANNER:
        return { 950.0f, 0.0f, 0.0f, 0.06f, 5.0f, 540.0f };
    case SG_JUNK:
        return { 1400.0f, 30.0f, 0.3f, 0.09f, 0.7f, 900.0f };
    default: // the minigun
        return { 1700.0f, 40.0f, 0.25f, 0.035f, bounce ? 1.0f : 6.0f, 720.0f };
    }
}

static void SgThrow(void* player, int kind, bool bounce)
{
    std::vector<const char*>& list = kind == SG_RIFLE ? g_sgRifleMdl : g_sgMiniMdl;
    if (list.empty())
        return;
    unsigned char* p = (unsigned char*)PrivateOf(player);
    void** vt = p ? *(void***)p : nullptr;
    if (!vt || !Readable(vt + PL_VT_GUNPOS, 4))
        return;
    SgThrowSpec s = SgSpec(kind, bounce);
    float src[3] = { 0, 0, 0 };
    ((GunPositionFn)vt[PL_VT_GUNPOS])(p, nullptr, src);
    V3 eye = Eye(player), dir = SfAim(player);
    if (s.spread > 0)
    {
        dir = dir + V3{ SgRand(-s.spread, s.spread), SgRand(-s.spread, s.spread), SgRand(-s.spread, s.spread) * 0.7f };
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
    V3 vel = dir * s.speed + V3{ 0, 0, s.lift };
    V3 pv = Vec((float*)(Vars(player) + EV_VELOCITY));
    vel = vel + pv * 0.5f;
    float* fv = (float*)(v + EV_VELOCITY);
    fv[0] = vel.x, fv[1] = vel.y, fv[2] = vel.z;
    float* ang = (float*)(v + EV_ANGLES);
    ang[0] = SgRand(0, 360), ang[1] = SgRand(0, 360), ang[2] = 0;
    float* av = (float*)(v + SG_EV_AVELOCITY);
    av[0] = SgRand(-s.spin, s.spin), av[1] = SgRand(-s.spin, s.spin), av[2] = SgRand(-s.spin, s.spin);
    float now = Now();
    SgObject o = { e, PrivateOf(e), player, start, now, now + s.fuse, s.grav, kind, bounce, false, g_nav.level };
    g_sgObjects.push_back(o);
}

static bool SgEnemyHit(void* hit, void* owner)
{
    if (!hit || hit == owner || *(int*)hit != 0)
        return false;
    int flags = F<int>(Vars(hit), EV_FLAGS);
    return (flags & (FL_CLIENT | FL_MONSTER)) != 0 && F<float>(Vars(hit), EV_TAKEDAMAGE) != 0.0f;
}

// the scanner's object: the enemy nearest to its line of flight (in front of it, in sight, up to 1500 units)
static void* SgHomingTarget(V3 at, V3 dir, void* owner)
{
    void* best = nullptr;
    float bestScore = 1e9f;
    ForEntitiesNear(at, 1500.0f, [&](void* e) {
        if (!SfEnemy(e, owner))
            return;
        V3 to = Center(e) - at;
        float d = Len(to);
        if (d < 1.0f)
            return;
        float cosA = (to.x * dir.x + to.y * dir.y + to.z * dir.z) / d;
        if (cosA < 0.5f) // within 60 degrees
            return;
        float score = d * (2.0f - cosA);
        if (score >= bestScore)
            return;
        TraceResult tr;
        if (!Trace(at, Center(e), owner, 1, tr))
            return;
        best = e;
        bestScore = score;
    });
    return best;
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
    if (o.kind == SG_SCANNER && Len(vel) > 1.0f)
    {
        float speed = Len(vel);
        V3 dir = vel * (1.0f / speed);
        if (!o.target || !SfEnemy(o.target, o.owner))
            o.target = SgHomingTarget(cur, dir, o.owner);
        if (o.target)
        {
            V3 to = Center(o.target) - cur;
            float d = Len(to);
            if (d > 1.0f)
            {
                float k = (std::min)(1.0f, 7.0f * dt);
                V3 want = to * (1.0f / d);
                dir = dir * (1.0f - k) + want * k;
                dir = dir * (1.0f / Len(dir));
                vel = dir * speed;
            }
        }
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

// ---------------------------------------------------------------- the marker's mines
static bool SgPlaceMine(void* player)
{
    if (g_sgMineMdl.empty())
        return false;
    V3 eye = Eye(player), dir = SfAim(player);
    TraceResult tr;
    if (Trace(eye, eye + dir * SG_REACH, player, 0, tr) || tr.fStartSolid)
        return false;
    V3 n = Vec(tr.vecPlaneNormal);
    V3 at = Vec(tr.vecEndPos) + (Len(n) > 0.5f ? n : dir * -1.0f) * 1.0f;
    int mine = 0;
    for (size_t i = 0; i < g_sgMines.size(); i++)
        if (g_sgMines[i].owner == player && g_sgMines[i].boomAt == 0 && ++mine >= SG_MINES_EACH)
        {
            g_sgMines[i].boomAt = Now(); // the oldest goes off to make room
            break;
        }
    void* e = SgSpawnTarget(at, g_sgMineMdl[0]);
    if (!e)
        return false;
    unsigned char* v = Vars(e);
    F<int>(v, EV_SOLID_OFS) = SG_SOLID_NOT;
    F<int>(v, SG_EV_MOVETYPE) = SG_MOVETYPE_NONE;
    float* ang = (float*)(v + EV_ANGLES);
    if (n.z > 0.7f || Len(n) < 0.5f)
        ang[0] = 0, ang[1] = SgRand(0, 360), ang[2] = 0;
    else // on a wall or a ceiling: its top turned to the surface's normal
    {
        ang[0] = acosf((std::max)(-1.0f, (std::min)(1.0f, n.z))) * RAD;
        ang[1] = atan2f(n.y, n.x) * RAD;
        ang[2] = 0;
    }
    float now = Now();
    g_sgMines.push_back({ e, PrivateOf(e), player, at + (Len(n) > 0.5f ? n : V3{ 0, 0, 1 }) * 12.0f, now + 0.8f,
                          now + 90.0f, 0, g_nav.level });
    return true;
}

static void SgMinesOff(void* player)
{
    float at = Now();
    for (SgMine& m : g_sgMines)
        if (m.owner == player && m.boomAt == 0)
        {
            m.boomAt = at;
            at += 0.12f;
        }
}

static void SgMinesFrame(float now)
{
    for (size_t i = 0; i < g_sgMines.size();)
    {
        SgMine m = g_sgMines[i];
        bool there = m.level == g_nav.level && *(int*)m.edict == 0 && PrivateOf(m.edict) == m.priv;
        if (!there || !SgOwnerOk(m.owner))
        {
            if (there)
                SgKill(m.edict);
            g_sgMines.erase(g_sgMines.begin() + i);
            continue;
        }
        bool boom = (m.boomAt > 0 && (now >= m.boomAt || now < m.boomAt - 30.0f)) || now >= m.until ||
                    now < m.armAt - 10.0f;
        if (!boom && now >= m.armAt)
            ForEntitiesNear(m.at, 100.0f, [&](void* e) {
                if (!boom && SfEnemy(e, m.owner))
                    boom = true;
            });
        if (!boom)
        {
            i++;
            continue;
        }
        SgKill(m.edict);
        g_sgMines.erase(g_sgMines.begin() + i);
        SgBlast(m.at, m.owner, SG_MARKER);
    }
}

// ---------------------------------------------------------------- rounds
static int SgKindOf(const char* cls)
{
    for (int k = 0; k < SG_KINDS; k++)
        if (!strcmp(cls, kSgClass[k]))
            return k;
    return -1;
}

// the weapon's rounds (full for a weapon not seen before); nullptr when it can't be told
static SgRounds* SgRoundsOf(void* weapon, int kind)
{
    if (g_sgRoundsLevel != g_nav.level)
    {
        g_sgRounds.clear();
        g_sgRoundsLevel = g_nav.level;
    }
    unsigned char* pev = Readable((unsigned char*)weapon + 8, 4) ? *(unsigned char**)((unsigned char*)weapon + 8) : nullptr;
    void* edict = pev ? *(void**)(pev + EV_CONTAINING_ENTITY) : nullptr;
    if (!edict)
        return nullptr;
    int serial = ((int*)edict)[1];
    SgRounds& r = g_sgRounds[weapon];
    if (r.edict != edict || r.serial != serial || r.kind != kind)
        r = { edict, serial, kSgRounds[kind], kind };
    return &r;
}

static void SgShowRounds(void* weapon, int left)
{
    *(int*)((unsigned char*)weapon + WPN_CLIP) = left;
}

// takes `n` rounds; false (and a word to the player, now and then) when there aren't enough
static bool SgTakeRounds(void* player, void* weapon, int kind, int n)
{
    SgRounds* r = SgRoundsOf(weapon, kind);
    if (!r)
        return true;
    if (r->left < n)
    {
        static float said[33];
        int i = SfIndex(player);
        if (i && (Now() - said[i] > 3.0f || Now() < said[i]))
        {
            said[i] = Now();
            SfSay(player, "Out of ammo: buy the gun again");
        }
        SgShowRounds(weapon, r->left);
        return false;
    }
    r->left -= n;
    SgShowRounds(weapon, r->left);
    return true;
}

// ---------------------------------------------------------------- the rounds on the HUD (each player's own client)
// client.dll's HUD is Half-Life's CHudAmmo: it draws a weapon's rounds only when the weapon's ammo type is set (its
// Draw 0x2a1c2f2: return when iAmmoType < 0 and iAmmo2Type < 0; the numbers when iAmmoType > 0). The weapons' table is
// at client 0x39f1348, 0x30c bytes an entry (index = the weapon id, under 0x2e7): name at +0, iAmmoType +0x80,
// iAmmo2Type +0x84, the clip +0xa0 (CurWeapon). Only the WeaponList message sets the ammo types (client 0x2a24020: three
// shorts, ammo type, ammo 2 type, weapon id), and of Studio's guns only the junk gun has one (it has rounds in Studio).
// botassist gives the other five the junk gun's ammo types in its own client's table (every half second: a WeaponList
// would set them back), and their HUD files (sprites/weapon_vxl*.txt, without an "ammo" line but the junk gun's) the
// junk gun's ammo icon: made from the player's own game files into botassist_tables\sprites the first time they're
// opened, so no game file is in the mod.
static const DWORD CL_WEAPONS = 0x39f1348;
static const int CL_WEAPON_SIZE = 0x30c, CL_WEAPONS_MAX = 0x2e7, CL_AMMO1 = 0x80, CL_AMMO2 = 0x84;
static const char kSgAmmoLine[] = "ammo\t\t\t640 640hud304\t1\t231\t24\t24";

static std::string SgHudFile(void* self, const char* name, const char* pathID)
{
    std::string n = name;
    for (auto& ch : n)
        ch = ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
    const char* file = strrchr(n.c_str(), '/') + 1;
    bool ours = false;
    for (int k = 0; k < SG_KINDS; k++)
        if (k != SG_JUNK && n == std::string("sprites/") + kSgClass[k] + ".txt")
            ours = true;
    if (!ours || !g_fsOpen)
        return "";
    std::string dir = std::string(g_dir) + "botassist_tables\\sprites\\";
    std::string path = dir + file;
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return path;
    void* h = g_fsOpen(self, nullptr, n.c_str(), "rb", pathID);
    if (!h)
        return "";
    void** vt = *(void***)self;
    unsigned int size = ((FsSizeFn)vt[0x44 / 4])(self, nullptr, h);
    std::string text(size, '\0');
    int got = size && size < 4096 ? ((FsReadFn)vt[0x5c / 4])(self, nullptr, &text[0], (int)size, h) : 0;
    ((FsCloseFn)vt[0x34 / 4])(self, nullptr, h);
    int count = 0;
    if (got != (int)size || sscanf(text.c_str(), "%d", &count) != 1 || count < 1 || count > 30 ||
        text.find("ammo") != std::string::npos)
        return ""; // not the file expected, or it has its icon already
    size_t eol = text.find('\n');
    std::string out = std::to_string(count + 1) + (eol == std::string::npos ? "\n" : text.substr(eol));
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    out += "\r\n";
    out += kSgAmmoLine;
    out += "\r\n";
    CreateDirectoryA((std::string(g_dir) + "botassist_tables").c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return "";
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    Log("file %s: made from the game's with the junk gun's ammo icon", n.c_str());
    return path;
}

static void SgClientAmmoTick()
{
    static HMODULE seen = nullptr;
    static DWORD delta = 0, lastScan = 0;
    static int index[SG_KINDS];
    static bool told = false;
    HMODULE cl = GetModuleHandleA("client.dll");
    if (!cl)
        return;
    __try
    {
        if (cl != seen)
        {
            seen = cl;
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((unsigned char*)cl + ((IMAGE_DOS_HEADER*)cl)->e_lfanew);
            delta = (DWORD)cl - nt->OptionalHeader.ImageBase;
            for (int& i : index)
                i = -1;
            lastScan = 0;
            told = false;
        }
        unsigned char* table = (unsigned char*)(CL_WEAPONS + delta);
        bool missing = false;
        for (int i : index)
            missing |= i < 0;
        if (missing && GetTickCount() - lastScan > 5000) // (the names come with the connection)
        {
            lastScan = GetTickCount();
            for (int id = 1; id < CL_WEAPONS_MAX; id++)
            {
                const char* nm = (const char*)(table + id * CL_WEAPON_SIZE);
                if (strncmp(nm, "weapon_vxl", 10))
                    continue;
                for (int k = 0; k < SG_KINDS; k++)
                    if (index[k] < 0 && !strcmp(nm, kSgClass[k]))
                        index[k] = id;
            }
        }
        if (index[SG_JUNK] < 0)
            return;
        unsigned char* junk = table + index[SG_JUNK] * CL_WEAPON_SIZE;
        int a1 = *(int*)(junk + CL_AMMO1), a2 = *(int*)(junk + CL_AMMO2);
        static int diag = 0;
        if (diag < 2 && (diag == 0 || a1 > 0))
        {
            diag = a1 > 0 ? 2 : 1;
            Log("Studio guns: client weapon table: junk gun id %d, ammo types %d/%d; rifle id %d ammo %d", index[SG_JUNK], a1,
                a2, index[SG_RIFLE], index[SG_RIFLE] >= 0 ? *(int*)(table + index[SG_RIFLE] * CL_WEAPON_SIZE + CL_AMMO1) : 0);
        }
        if (a1 <= 0)
            return;
        int set = 0;
        for (int k = 0; k < SG_KINDS; k++)
        {
            if (k == SG_JUNK || index[k] < 0)
                continue;
            unsigned char* w = table + index[k] * CL_WEAPON_SIZE;
            if (*(int*)(w + CL_AMMO1) <= 0)
            {
                *(int*)(w + CL_AMMO1) = a1;
                *(int*)(w + CL_AMMO2) = a2;
                set++;
            }
        }
        if (set && !told)
        {
            told = true;
            Log("Studio guns: the HUD shows their rounds (the junk gun's ammo type %d/%d given to %d guns; ids %d %d %d %d %d, "
                "junk gun %d)",
                a1, a2, set, index[SG_RIFLE], index[SG_PISTOL], index[SG_MINIGUN], index[SG_SCANNER], index[SG_MARKER],
                index[SG_JUNK]);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 3)
            Log("Studio guns: exception 0x%08lX with the client's weapon table", GetExceptionCode());
    }
}

// every frame: the rounds of the Studio gun each player holds go in its clip (shown, and read by the junk gun)
static void SgRoundsFrame()
{
    int maxClients = *(int*)((DWORD)*g_pGlobals + SF_GV_MAXCLIENTS);
    for (int i = 1; i <= maxClients && i <= 32; i++)
    {
        void* e = SfEdictOf(i);
        if (!e || *(int*)e != 0 || !PrivateOf(e) || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            continue;
        void* weapon = nullptr;
        int kind = SgKindOf(SfActiveClass(e, &weapon));
        if (kind < 0 || !weapon)
            continue;
        SgRounds* r = SgRoundsOf(weapon, kind);
        if (r)
            SgShowRounds(weapon, r->left);
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
    static const char* const kHints[SG_KINDS] = {
        "Studio Rifle: throws objects that blow up - right click: bouncing ones",
        "Studio Pistol: puts blocks that blow up when broken - right click: set them off",
        "Studio Minigun: a stream of exploding junk - right click: bouncing ones",
        "Studio Scanner: objects that home in on enemies - right click: three at once",
        "Studio Marker: puts mines that go off near enemies - right click: set them off",
        "Studio Junk Gun: a short spray of exploding junk",
    };
    SfSay(player, kHints[kind]);
}

// a shot of a Studio gun off Studio's maps: what it does, and the time to the next one
static void SgFire(void* player, void* weapon, int kind, bool alt)
{
    unsigned char* w = (unsigned char*)weapon;
    SgHint(player, kind);
    float next = 0.3f;
    switch (kind)
    {
    case SG_RIFLE:
        if (SgTakeRounds(player, weapon, kind, 1))
            SgThrow(player, SG_RIFLE, alt);
        next = 0.45f;
        break;
    case SG_MINIGUN:
        if (SgTakeRounds(player, weapon, kind, 1))
            SgThrow(player, SG_MINIGUN, alt);
        next = (std::max)(*(float*)(w + WPN_NEXT_PRIMARY), 0.08f); // (the game's spin-up)
        break;
    case SG_PISTOL:
        if (alt)
        {
            SgDetonate(player);
            next = 0.5f;
        }
        else
        {
            SgRounds* r = SgRoundsOf(weapon, kind);
            if (r && r->left <= 0)
                SgTakeRounds(player, weapon, kind, 1); // (only the word)
            else if (SgPlaceBlock(player))
                SgTakeRounds(player, weapon, kind, 1);
            next = 0.3f;
        }
        break;
    case SG_SCANNER:
    {
        int n = alt ? 3 : 1;
        SgRounds* r = SgRoundsOf(weapon, kind);
        if (alt && r && r->left > 0 && r->left < 3)
            n = r->left;
        if (SgTakeRounds(player, weapon, kind, n))
            for (int k = 0; k < n; k++)
                SgThrow(player, SG_SCANNER, false);
        next = alt ? 1.2f : 0.5f;
        break;
    }
    case SG_MARKER:
    {
        SgRounds* r = SgRoundsOf(weapon, kind);
        if (r && r->left <= 0)
            SgTakeRounds(player, weapon, kind, 1);
        else if (SgPlaceMine(player))
            SgTakeRounds(player, weapon, kind, 1);
        next = 0.6f;
        break;
    }
    case SG_JUNK:
        if (SgTakeRounds(player, weapon, kind, 1))
            for (int k = 0; k < 5; k++)
                SgThrow(player, SG_JUNK, false);
        next = 0.6f;
        break;
    }
    *(float*)(w + WPN_NEXT_PRIMARY) = next;
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
    SgFire(player, weapon, kind, *(int*)(w + VXL_MODE) != 0);
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
static void __fastcall HookedSgMarkerPrimary(void* weapon, void* edx) { SgPrimary(g_sgMarkerPrimary, weapon, edx, SG_MARKER); }
static void __fastcall HookedSgJunkPrimary(void* weapon, void* edx) { SgPrimary(g_sgJunkPrimary, weapon, edx, SG_JUNK); }

// the marker's right click: its mines go off
static void __fastcall HookedSgMarkerSecondary(void* weapon, void* edx)
{
    g_sgMarkerSecondary(weapon, edx);
    __try
    {
        unsigned char* pev = WeaponOwnerPev(weapon);
        if (pev && !SgVoxelMap())
            SgMinesOff(*(void**)(pev + EV_CONTAINING_ENTITY));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GunFixFault("Studio marker", GetExceptionCode());
    }
}

// the marker's state from the Studio record: off Studio's maps there's none, the state stays 0
static void __fastcall HookedSgMarkerState(void* weapon, void* edx)
{
    if (!SgVoxelMap())
    {
        *(int*)((unsigned char*)weapon + VXL_MODE) = 0;
        return;
    }
    g_sgMarkerState(weapon, edx);
}

static void __fastcall HookedSgMarkerWork(void* weapon, void* edx)
{
    if (!SgVoxelMap())
        return;
    g_sgMarkerWork(weapon, edx);
}

static int __cdecl HookedSgJunkHit(void* a, void* b, void* c)
{
    if (!SgVoxelMap())
        return 0; // (no round taken by the game: botassist counts them)
    return g_sgJunkHit(a, b, c);
}

// the scanner's PrimaryAttack (mode 0) and SecondaryAttack (1): off Studio's maps, the game's timing, animation and
// shot event (0x1125a1a0's own), then botassist's shot instead of the record's work
static void SgScanOff(void* weapon, int mode)
{
    unsigned char* w = (unsigned char*)weapon;
    unsigned char* p = *(unsigned char**)(w + WPN_PLAYER);
    unsigned char* pev = p ? *(unsigned char**)(p + 8) : nullptr;
    if (!pev)
        return;
    void* player = *(void**)(pev + EV_CONTAINING_ENTITY);
    *(float*)(w + WPN_NEXT_PRIMARY) = 0.7f;
    *(float*)(w + WPN_NEXT_SECONDARY) = 0.7f;
    *(float*)(w + VXL_NEXT_IDLE) = 2.0f;
    ((SgSetAnimFn)(*(void***)p)[PL_VT_SETANIMATION])(p, nullptr, 5); // PLAYER_ATTACK1
    PlaybackEventFn fn = *(PlaybackEventFn*)(MP_PLAYBACK_EVENT + g_mpDelta);
    unsigned short ev = *(unsigned short*)(w + WPN_EVENT);
    const float* zero = (const float*)(MP_VEC3_ORIGIN + g_mpDelta);
    if (fn && ev)
        fn(1 /* FEV_NOTHOST */, player, ev, 0.0f, zero, zero, 0, 0, 0, 0, mode & 0xff, 0);
    if (!SgReady() || !Alive(player))
        return;
    SgFire(player, weapon, SG_SCANNER, mode != 0);
    *(float*)(w + WPN_NEXT_SECONDARY) = *(float*)(w + WPN_NEXT_PRIMARY);
}

static void __fastcall HookedSgScan(void* weapon, void* edx, int mode)
{
    if (SgVoxelMap())
    {
        g_sgScan(weapon, edx, mode);
        return;
    }
    __try
    {
        SgScanOff(weapon, mode);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GunFixFault("Studio scanner", GetExceptionCode());
    }
}

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
    if (!g_pGlobals || !*g_pGlobals || !g_IndexOfEdict)
        return;
    float now = Now();
    float dt = g_sgLastFrame > 0 && now > g_sgLastFrame ? (std::min)(now - g_sgLastFrame, 0.1f) : 0.0f;
    g_sgLastFrame = now;
    __try
    {
        if (!SgVoxelMap())
            SgRoundsFrame();
        SgClientAmmoTick();
        for (size_t i = 0; i < g_sgObjects.size();)
            if (SgObjectFrame(g_sgObjects[i], now, dt))
                i++;
            else
                g_sgObjects.erase(g_sgObjects.begin() + i);
        SgBlocksFrame(now);
        SgMinesFrame(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("Studio guns: exception 0x%08lX", GetExceptionCode());
        g_sgObjects.clear();
        g_sgBlocks.clear();
        g_sgMines.clear();
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
    const unsigned char mstate[] = { 0x55, 0x8b, 0xec, 0x51, 0x53 };     // ... push ecx; push ebx
    const unsigned char mwork[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x4c }; // ... sub esp, 0x4c
    const unsigned char mprim[] = { 0x55, 0x8b, 0xec, 0x51, 0x57 };      // ... push ecx; push edi
    const unsigned char msec[] = { 0x55, 0x8b, 0xec, 0x51, 0x53 };       // ... push ecx; push ebx
    const unsigned char scan[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x24 }; // ... sub esp, 0x24
    const unsigned char jprim[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x2c }; // ... sub esp, 0x2c
    const unsigned char jhit[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x28 };  // ... sub esp, 0x28
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
    // the marker: its record users first (without them it would close the game anyway), then its use
    if (!g_sgMarkerState)
        g_sgMarkerState = (WeaponFn)DetourChecked(MP_VXL_MARKER_STATE, mstate, sizeof(mstate), (void*)HookedSgMarkerState,
                                                  "Studio marker (state)");
    if (!g_sgMarkerWork)
        g_sgMarkerWork = (WeaponFn)DetourChecked(MP_VXL_MARKER_WORK, mwork, sizeof(mwork), (void*)HookedSgMarkerWork,
                                                 "Studio marker (work)");
    if (g_sgMarkerState && g_sgMarkerWork)
    {
        if (!g_sgMarkerPrimary)
            g_sgMarkerPrimary = (WeaponFn)DetourChecked(MP_VXL_MARKER_PRIMARY, mprim, sizeof(mprim),
                                                        (void*)HookedSgMarkerPrimary, "Studio marker");
        if (!g_sgMarkerSecondary)
            g_sgMarkerSecondary = (WeaponFn)DetourChecked(MP_VXL_MARKER_SECONDARY, msec, sizeof(msec),
                                                          (void*)HookedSgMarkerSecondary, "Studio marker (right click)");
    }
    if (!g_sgScan)
        g_sgScan = (SgScanFn)DetourChecked(MP_VXL_SCANNER_WORK, scan, sizeof(scan), (void*)HookedSgScan, "Studio scanner");
    if (!g_sgJunkHit)
        g_sgJunkHit = (SgJunkHitFn)DetourChecked(MP_VXL_JUNK_HIT, jhit, sizeof(jhit), (void*)HookedSgJunkHit,
                                                 "Studio junk gun (hit)");
    if (g_sgJunkHit && !g_sgJunkPrimary)
        g_sgJunkPrimary = (WeaponFn)DetourChecked(MP_VXL_JUNK_PRIMARY, jprim, sizeof(jprim), (void*)HookedSgJunkPrimary,
                                                  "Studio junk gun");
    Log("Studio guns outside Studio's maps: rifle %s, pistol %s, minigun %s, scanner %s, marker %s, junk gun %s "
        "(objects precached from the next map loaded)",
        g_sgLong ? "on" : "OFF", g_sgShort ? "on" : "OFF", g_sgMini ? "on" : "OFF", g_sgScan ? "on" : "OFF",
        g_sgMarkerPrimary ? "on" : "OFF", g_sgJunkPrimary ? "on" : "OFF");
}
