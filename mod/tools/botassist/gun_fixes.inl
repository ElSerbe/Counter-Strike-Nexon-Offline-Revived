// ---------------------------------------------------------------- guns with no bullet (every mode)
// Arcana II (weapon_burstdischarge and its ex, adv, 1, 4, 6 versions, one class CBurstDischarge): its fire function
// (0x10c208f0, thiscall weapon, float spread; called by PrimaryAttack 0x10c1b5b0) takes a round, plays the animation,
// the shot event and the kick, and never fires a bullet: the AK-47's (0x10f9ecc0) calls CBaseEntity::FireBullets3
// (0x113fc970) between the event and the kick, this one doesn't. Nexon's dedicated servers must have done the damage;
// here the shots hurt nobody (the user, Gun Deathmatch 2026-11-20: "it shoots, nothing happens"). botassist detours the
// fire function: when it took a round, a bullet goes out the way the AK-47's does (gun position = player vfunc 0x210,
// direction gpGlobals->v_forward that the function has just set, 8192 units, penetration 2, 7.62 bullets), with the
// damage below (the game's numbers for it aren't in this mp.dll). Its special "burst discharge" mode still does nothing.
static const DWORD MP_BD_FIRE = 0x10c208f0, MP_FIREBULLETS3 = 0x113fc970;
static const int WPN_PLAYER = 0xd0, WPN_CLIP = 0x160, PL_SHARED_RAND = 0xc54, PL_VT_GUNPOS = 0x210 / 4;
static const int BD_PENETRATION = 2, BD_BULLET = 0xb;
static const float BD_DISTANCE = 8192.0f, BD_RANGE_MOD = 0.98f;

typedef void(__fastcall* BdFireFn)(void* weapon, void* edx, float spread);
typedef float*(__fastcall* GunPositionFn)(void* player, void* edx, float* out);
typedef float*(__fastcall* FireBullets3Fn)(void* player, void* edx, float* out, float sx, float sy, float sz, float dx,
                                           float dy, float dz, float spread, float distance, int penetration,
                                           int bulletType, int damage, float rangeMod, void* pevAttacker, int pistol,
                                           int sharedRand, int zero1, int zero2);
static BdFireFn g_bdFire;

// damage of one bullet by version (the better versions hit harder, as in the shop)
static int BdDamage(void* weapon)
{
    unsigned char* pev = Readable((unsigned char*)weapon + 8, 4) ? *(unsigned char**)((unsigned char*)weapon + 8) : nullptr;
    const char* cls = pev && Readable(pev, 4) ? String(*(int*)pev) : nullptr;
    if (cls && (strstr(cls, "adv") || strstr(cls, "6")))
        return 38;
    if (cls && (strstr(cls, "ex") || strstr(cls, "4")))
        return 35;
    return 32;
}

static void BdBullet(void* weapon, float spread)
{
    unsigned char* player = *(unsigned char**)((unsigned char*)weapon + WPN_PLAYER);
    if (!player || !Readable(player, PL_SHARED_RAND + 4) || !g_pGlobals || !*g_pGlobals)
        return;
    void** vt = *(void***)player;
    unsigned char* pev = *(unsigned char**)(player + 8);
    if (!vt || !Readable(vt + PL_VT_GUNPOS, 4) || !pev)
        return;
    float src[3] = { 0, 0, 0 }, out[3];
    ((GunPositionFn)vt[PL_VT_GUNPOS])(player, nullptr, src);
    const float* fwd = (const float*)((DWORD)*g_pGlobals + 0x28); // gpGlobals->v_forward
    ((FireBullets3Fn)(MP_FIREBULLETS3 + g_mpDelta))(player, nullptr, out, src[0], src[1], src[2], fwd[0], fwd[1], fwd[2],
                                                   spread, BD_DISTANCE, BD_PENETRATION, BD_BULLET, BdDamage(weapon),
                                                   BD_RANGE_MOD, pev, 0, *(int*)(player + PL_SHARED_RAND), 0, 0);
}

static void __fastcall HookedBdFire(void* weapon, void* edx, float spread)
{
    int clip = Readable((unsigned char*)weapon + WPN_CLIP, 4) ? *(int*)((unsigned char*)weapon + WPN_CLIP) : 0;
    g_bdFire(weapon, edx, spread);
    __try
    {
        if (*(int*)((unsigned char*)weapon + WPN_CLIP) < clip)
            BdBullet(weapon, spread);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("Arcana II: exception 0x%08lX firing its bullet", GetExceptionCode());
    }
}

static void FixArcanaII(DWORD delta)
{
    unsigned char* fn = (unsigned char*)(MP_BD_FIRE + delta);
    const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8 }; // push ebp; mov ebp, esp; and esp, -8
    const unsigned char fb[] = { 0x55, 0x8b, 0xec };                      // FireBullets3's start
    unsigned char* fire = (unsigned char*)(MP_FIREBULLETS3 + delta);
    if (!Readable(fn, sizeof(code)) || fn[0] == 0xe9)
        return;
    if (memcmp(fn, code, sizeof(code)) || !Readable(fire, sizeof(fb)) || memcmp(fire, fb, sizeof(fb)))
    {
        Log("Arcana II: mp.dll isn't the expected one, its shots left as they are (no bullet)");
        return;
    }
    g_bdFire = (BdFireFn)DetourCdecl(fn, sizeof(code), (void*)HookedBdFire);
    Log("Arcana II (burstdischarge): its shots fire a bullet now (the game's fire function had none)");
}
