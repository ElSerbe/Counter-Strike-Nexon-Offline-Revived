// ---------------------------------------------------------------- grenades
// All grenades are weapon_hegrenade (CHEGrenade); its type (+0x220, from the player's +0x219c) picks the models
// (table 0x11c781ec, 0x10 bytes a type: 0 HE, 3 Flame Grenade, 0xd Junk Grenade, 0x12 Trinity, 0x13 Timed Bomb,
// 0x15 Ignite Bomb...) and the throw: WeaponIdle 0x11050a10 calls 0x11050670 (cdecl type, thrower pev, start xyz,
// velocity xyz, time, team, event, extra), a switch: 3 the Flame Grenade's 0x1148af90, 0xc the Chain Grenade, 0xe and
// default the HE grenade's 0x1148b520, 0x12 the Trinity's, and 0x15 returns at once.
//  - Ignite Bomb (item 8886, ignitebomb, type 0x15): its thrower was only on Nexon's servers, so the throw animation
//    played and nothing left the hand (the user, 2026-11-20). It's thrown as a Flame Grenade now (fire on landing).
//  - Junk Grenade (vxlgrenade, type 0xd): its detonation (CGrenade::DetonateVxlGrenade 0x11482ee0, the think chosen
//    when pev dmg is 769) scatters junk blocks in Studio's voxel world and blows up. The user wanted explosive props
//    around it for a few seconds that go off: botassist puts up to 5 of the game's own explosive drums around the blast
//    (func_barrelbomb, CBarrelBomb, vtable 0x119705dc: the zm_shoreline drums; any hit sets one off (TakeDamage
//    0x1147b7e0, health 0), its blast (0x11479fc0, thiscall drum, attacker pev) hurts around and sets the others off,
//    credited to whoever shot it). The ones nobody shoots go off by themselves 3-5 s later, credited to the thrower.
//    A drum's models must be precached while the map loads: botassist does it after the grenade weapon's own
//    precache (CHEGrenade::Precache 0x110504f0, every map) and only puts drums on a map it precached them for.
static const DWORD MP_GRENADE_THROW = 0x11050670, MP_HEGRENADE_PRECACHE = 0x110504f0, MP_DETONATE_VXL = 0x11482ee0;
static const DWORD MP_CREATE_NAMED = 0x113e6830, MP_VT_BARRELBOMB = 0x119705dc, MP_BARREL_KILLED = 0x11479fc0;
static const DWORD MP_PRECACHE_MODEL = 0x121d3a5c, MP_PRECACHE_SOUND = 0x121d3a60; // mp.dll's engine function pointers
static const int GREN_FLAME = 3, GREN_IGNITE = 0x15;
static const int EV_OWNER = 0x1bc, EV_SOLID_OFS = 0x11c;
static const int BARREL_SPAWN_RATIO = 0xbc, BARREL_DAMAGE = 0xc0, BARREL_RANGE = 0xc4, BARREL_BIG_RATIO = 0xc8;
static const int BARRELS_PER_GRENADE = 5, BARRELS_MAX = 25;

typedef DWORD(__cdecl* GrenadeThrowFn)(int type, DWORD pev, DWORD sx, DWORD sy, DWORD sz, DWORD vx, DWORD vy, DWORD vz,
                                       DWORD time, DWORD team, DWORD ev, DWORD extra);
typedef void(__fastcall* ThisFn)(void* self, void* edx);
typedef void(__fastcall* BarrelKilledFn)(void* self, void* edx, void* attackerPev);
typedef void* (*CreateNamedFn)(int name, int flag);
typedef int (*PrecacheFn)(const char* name);
static GrenadeThrowFn g_grenadeThrow;
static ThisFn g_heGrenadePrecache, g_detonateVxl;

struct DrumBarrel
{
    void* edict;
    void* priv;
    void* owner; // the thrower's edict
    float at;    // goes off by itself then
    int level;
};
static std::vector<DrumBarrel> g_drums;
static std::string g_drumsMap; // the map the drums were precached for

static DWORD __cdecl HookedGrenadeThrow(int type, DWORD pev, DWORD sx, DWORD sy, DWORD sz, DWORD vx, DWORD vy, DWORD vz,
                                        DWORD time, DWORD team, DWORD ev, DWORD extra)
{
    if (type == GREN_IGNITE)
        type = GREN_FLAME; // its own thrower is missing (see above)
    return g_grenadeThrow(type, pev, sx, sy, sz, vx, vy, vz, time, team, ev, extra);
}

static void __fastcall HookedHeGrenadePrecache(void* self, void* edx)
{
    g_heGrenadePrecache(self, edx);
    __try
    {
        PrecacheFn model = *(PrecacheFn*)(MP_PRECACHE_MODEL + g_mpDelta);
        PrecacheFn sound = *(PrecacheFn*)(MP_PRECACHE_SOUND + g_mpDelta);
        if (!model || !sound)
            return;
        // CBarrelBomb::Precache's list (0x1147adc0)
        model("models/zm_shoreline/drum_01.mdl");
        model("models/zm_shoreline/drum_02.mdl");
        model("models/metalplategibs.mdl");
        sound("debris/bustmetal1.wav");
        sound("debris/bustmetal2.wav");
        g_drumsMap = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Junk Grenade: exception 0x%08lX precaching the drums", GetExceptionCode());
    }
}

static bool DrumAlive(const DrumBarrel& d)
{
    return *(int*)d.edict == 0 /* edict_t free */ && PrivateOf(d.edict) == d.priv &&
           F<float>(Vars(d.edict), EV_TAKEDAMAGE) != 0.0f;
}

// a drum standing at `ground` (its bottom); nullptr when it couldn't be made
static void* SpawnDrum(V3 ground, float yaw)
{
    static const char kClass[] = "func_barrelbomb";
    const char* base = *(const char**)((DWORD)*g_pGlobals + GV_STRINGBASE);
    void* edict = ((CreateNamedFn)(MP_CREATE_NAMED + g_mpDelta))((int)(kClass - base), 0);
    if (!edict)
        return nullptr;
    unsigned char* p = (unsigned char*)PrivateOf(edict);
    if (!p || *(DWORD*)p != MP_VT_BARRELBOMB + g_mpDelta)
    {
        Log("Junk Grenade: func_barrelbomb isn't the expected class, no drums");
        return nullptr;
    }
    *(float*)(p + BARREL_SPAWN_RATIO) = 1.0f;
    *(float*)(p + BARREL_DAMAGE) = 110.0f;
    *(float*)(p + BARREL_RANGE) = 180.0f;
    *(float*)(p + BARREL_BIG_RATIO) = 0.25f; // a big one: the other drum model, twice the damage
    unsigned char* v = Vars(edict);
    // its box is (-24 -24 -86, 24 24 -20): the origin stands 86 units above the drum's bottom
    float* o = (float*)(v + EV_ORIGIN);
    o[0] = ground.x, o[1] = ground.y, o[2] = ground.z + 87.0f;
    ((float*)(v + EV_ANGLES))[1] = yaw;
    ((ThisFn)(*(void***)p)[0xc / 4])(p, nullptr); // Spawn: model, size, solid, takes damage
    return edict;
}

static void PutDrumsAround(V3 center, void* owner)
{
    std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    if (g_drumsMap.empty() || map != g_drumsMap)
        return; // not precached on this map
    g_drums.erase(std::remove_if(g_drums.begin(), g_drums.end(),
                                 [](const DrumBarrel& d) { return d.level != g_nav.level || !DrumAlive(d); }),
                  g_drums.end());
    float now = Now(), turn = (float)(rand() % 360) / RAD;
    int made = 0;
    for (int k = 0; k < BARRELS_PER_GRENADE && (int)g_drums.size() < BARRELS_MAX; k++)
    {
        float a = turn + k * (6.2832f / BARRELS_PER_GRENADE), dist = 60.0f + rand() % 40;
        V3 dir = { cosf(a), sinf(a), 0 };
        V3 from = center + V3{ 0, 0, 16 }, spot = from + dir * dist;
        TraceResult tr;
        if (!Trace(from, spot, nullptr, 1, tr))
            spot = Vec(tr.vecEndPos) - dir * 30.0f; // a wall: closer
        if (Len(spot - from) < 30.0f)
            continue;
        Trace(spot + V3{ 0, 0, 24 }, spot - V3{ 0, 0, 256 }, nullptr, 1, tr);
        if (tr.fStartSolid || tr.flFraction >= 1.0f)
            continue;
        V3 ground = Vec(tr.vecEndPos);
        // room for a drum: no wall, player or other drum where it would stand
        float c[3] = { ground.x, ground.y, ground.z + 37.0f };
        TraceResult hull;
        memset(&hull, 0, sizeof(hull));
        if (g_TraceHull)
        {
            g_TraceHull(c, c, 0, 1 /* human hull */, nullptr, &hull);
            if (hull.fStartSolid || hull.fAllSolid)
                continue;
        }
        void* e = SpawnDrum(ground, a * RAD);
        if (!e)
            break;
        g_drums.push_back({ e, PrivateOf(e), owner, now + 3.0f + k * 0.35f + (rand() % 100) / 100.0f, g_nav.level });
        made++;
    }
    static int logs = 0;
    if (made && logs++ < 20)
        Log("Junk Grenade: %d drums around the blast (%d standing)", made, (int)g_drums.size());
}

static void __fastcall HookedDetonateVxl(void* grenade, void* edx)
{
    V3 center = { 0, 0, 0 };
    void* owner = nullptr;
    bool ok = false;
    __try
    {
        unsigned char* pev = *(unsigned char**)((unsigned char*)grenade + 8);
        center = Vec((float*)(pev + EV_ORIGIN));
        owner = *(void**)(pev + EV_OWNER);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_detonateVxl(grenade, edx);
    if (!ok)
        return;
    __try
    {
        PutDrumsAround(center, owner);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Junk Grenade: exception 0x%08lX putting drums", GetExceptionCode());
    }
}

// every frame: the drums nobody shot go off by themselves
static void GrenadeFixFrame()
{
    if (g_drums.empty())
        return;
    float now = Now();
    for (size_t i = 0; i < g_drums.size();)
    {
        DrumBarrel d = g_drums[i];
        bool gone = true;
        __try
        {
            gone = d.level != g_nav.level || !DrumAlive(d);
            if (!gone && (now >= d.at || now < d.at - 30.0f))
            {
                unsigned char* v = Vars(d.edict);
                void* attacker = d.owner && *(int*)d.owner == 0 ? (void*)Vars(d.owner) : (void*)v;
                F<float>(v, EV_TAKEDAMAGE) = 0; // as its TakeDamage does before the blast
                F<int>(v, EV_SOLID_OFS) = 0;
                ((BarrelKilledFn)(MP_BARREL_KILLED + g_mpDelta))(d.priv, nullptr, attacker);
                gone = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            gone = true;
        }
        if (gone)
            g_drums.erase(g_drums.begin() + i);
        else
            i++;
    }
}

static void FixGrenades()
{
    const unsigned char thr[] = { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08 };       // push ebp; mov ebp, esp; mov eax, [ebp+8]
    unsigned char pre[] = { 0x56, 0x68, 0, 0, 0, 0 };                          // push esi; push "weapons/..." (string)
    *(DWORD*)(pre + 2) = 0x118c7fb0 + g_mpDelta;
    const unsigned char det[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x6c };       // push ebp; mov ebp, esp; sub esp, 0x6c
    if (!g_grenadeThrow)
        g_grenadeThrow = (GrenadeThrowFn)DetourChecked(MP_GRENADE_THROW, thr, sizeof(thr), (void*)HookedGrenadeThrow,
                                                      "Ignite Bomb");
    if (!g_heGrenadePrecache)
        g_heGrenadePrecache = (ThisFn)DetourChecked(MP_HEGRENADE_PRECACHE, pre, sizeof(pre),
                                                    (void*)HookedHeGrenadePrecache, "Junk Grenade drums");
    if (!g_detonateVxl && g_heGrenadePrecache)
        g_detonateVxl = (ThisFn)DetourChecked(MP_DETONATE_VXL, det, sizeof(det), (void*)HookedDetonateVxl, "Junk Grenade");
    Log("grenades: Ignite Bomb %s; Junk Grenade %s", g_grenadeThrow ? "thrown as a Flame Grenade" : "left as it is",
        g_detonateVxl ? "leaves explosive drums (from the next map loaded)" : "left as it is");
}
