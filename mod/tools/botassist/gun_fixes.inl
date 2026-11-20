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

// ---------------------------------------------------------------- more guns whose shots did nothing
// The same missing piece in two more sci-fi guns (the user, 2026-11-20: "it plays the animation and fires nothing"):
//  - Gravity Repulsor (weapon_leapstrikegun, the shop's "Leap Strike Gun"): its PrimaryAttack (0x10e660c0, thiscall)
//    takes a round, plays shoot1/shoot2 and the kick (vfunc 0x2f4 = KickBack 0x11529eb0) and nothing else. Its
//    projectile (leapstrikegun_proj, "-1_exp" sound) has no server code. Each shot now fires a bullet and a small
//    blast where it lands.
//  - Giga Break (weapon_beamgun, the shop's "Beam Gun"): its shot (0x10bab520, called by PrimaryAttack 0x10ba0a40)
//    takes a round, sets the flash and kicks, no bullet; the right click (0x10ba0ff0: shootB1/shootB2 = anims 2/3, or
//    0xb in its charged mode) and the charged left click (anims 9/10) only play animations. Now: bullets, and the right
//    click / charged shots blast where you aim.
// A blast is the game's own: TE_EXPLOSION with its fireball sprite and RadiusDamage (0x114040c0, cdecl x, y, z,
// inflictor, attacker, damage, radius, class to ignore, damage bits), the shooter left out of it.
// Damage per mode, from the game's own numbers (Item data, web/shop/iteminfo.json): Gravity Repulsor 16 / 176 zombie /
// 430 scenario, Giga Break 32 / 680 / 2400 a bullet; the blasts are scaled the same way.
static const DWORD MP_LSG_PRIMARY = 0x10e660c0, MP_BG_PRIMARY = 0x10ba0a40, MP_BG_SECONDARY = 0x10ba0ff0;
static const DWORD MP_RADIUS_DAMAGE = 0x114040c0, MP_FIREBALL_SPRITE = 0x122070c4;
static const int WPN_NEXT_PRIMARY = 0x134, WPN_NEXT_SECONDARY = 0x13c, PL_EV_WEAPONANIM = 0x248;
static const int BULLET_50AE = 13, BULLET_762 = 0xb;
typedef void(__fastcall* WeaponFn)(void* weapon, void* edx);
typedef void (*RadiusDamageFn)(float x, float y, float z, void* inflictor, void* attacker, float damage, float radius,
                               int ignoreClass, int bits);
static WeaponFn g_lsgPrimary, g_bgPrimary, g_bgSecondary;
static bool ZombieMode(int mode);
static bool CoopMode();

// the weapon's number for the mode being played: scenario (co-op) modes, zombie modes, the others
static float ByMode(float pvp, float zombie, float scenario)
{
    return CoopMode() ? scenario : ZombieMode(GameMode()) ? zombie : pvp;
}

static void AimVectors(unsigned char* pev, float* fwd)
{
    float p = (*(float*)(pev + EV_VANGLE) + *(float*)(pev + EV_PUNCHANGLE)) / RAD;
    float y = (*(float*)(pev + EV_VANGLE + 4) + *(float*)(pev + EV_PUNCHANGLE + 4)) / RAD;
    fwd[0] = cosf(p) * cosf(y);
    fwd[1] = cosf(p) * sinf(y);
    fwd[2] = -sinf(p);
}

// one bullet from the player holding `weapon` where it aims; `hit` gets where the aim line ends (for a blast)
static bool GunBullet(void* weapon, float spread, int damage, int penetration, int bullet, float* hit)
{
    unsigned char* player = *(unsigned char**)((unsigned char*)weapon + WPN_PLAYER);
    if (!player || !Readable(player, PL_SHARED_RAND + 4))
        return false;
    void** vt = *(void***)player;
    unsigned char* pev = *(unsigned char**)(player + 8);
    if (!vt || !Readable(vt + PL_VT_GUNPOS, 4) || !pev)
        return false;
    float src[3] = { 0, 0, 0 }, out[3], fwd[3];
    ((GunPositionFn)vt[PL_VT_GUNPOS])(player, nullptr, src);
    AimVectors(pev, fwd);
    ((FireBullets3Fn)(MP_FIREBULLETS3 + g_mpDelta))(player, nullptr, out, src[0], src[1], src[2], fwd[0], fwd[1], fwd[2],
                                                   spread, BD_DISTANCE, penetration, bullet, damage, BD_RANGE_MOD, pev,
                                                   0, *(int*)(player + PL_SHARED_RAND), 0, 0);
    if (hit)
    {
        TraceResult tr;
        V3 from = Vec(src), dir = Vec(fwd);
        Trace(from, from + dir * BD_DISTANCE, *(void**)(pev + EV_CONTAINING_ENTITY), 0, tr);
        V3 end = Vec(tr.vecEndPos) - dir * 12.0f; // a little off the wall, so the blast isn't inside it
        hit[0] = end.x, hit[1] = end.y, hit[2] = end.z;
    }
    return true;
}

// the game's explosion at `at`: sprite, sound, damage around; the shooter (attackerPev) isn't hurt by his own blast
static void GunBlast(const float* at, unsigned char* attackerPev, float damage, float radius, int scale)
{
    if (g_MessageBegin && g_WriteByte && g_WriteShort && g_WriteCoord && g_MessageEnd)
    {
        g_MessageBegin(4 /* MSG_PVS */, 23 /* SVC_TEMPENTITY */, at, nullptr);
        g_WriteByte(3); // TE_EXPLOSION
        g_WriteCoord(at[0]);
        g_WriteCoord(at[1]);
        g_WriteCoord(at[2]);
        g_WriteShort(*(int*)(MP_FIREBALL_SPRITE + g_mpDelta));
        g_WriteByte(scale);
        g_WriteByte(15);
        g_WriteByte(0);
        g_MessageEnd();
    }
    float take = *(float*)(attackerPev + EV_TAKEDAMAGE);
    *(float*)(attackerPev + EV_TAKEDAMAGE) = 0;
    ((RadiusDamageFn)(MP_RADIUS_DAMAGE + g_mpDelta))(at[0], at[1], at[2], attackerPev, attackerPev, damage, radius, 0,
                                                     0x1000000 /* the barrels' and grenades' blast bit */);
    *(float*)(attackerPev + EV_TAKEDAMAGE) = take;
}

static unsigned char* WeaponOwnerPev(void* weapon)
{
    unsigned char* player = *(unsigned char**)((unsigned char*)weapon + WPN_PLAYER);
    return player && Readable(player + 8, 4) ? *(unsigned char**)(player + 8) : nullptr;
}

static void GunFixFault(const char* what, DWORD code)
{
    static int faults = 0;
    if (faults++ < 5)
        Log("%s: exception 0x%08lX firing its shot", what, code);
}

static void __fastcall HookedLsgPrimary(void* weapon, void* edx)
{
    int clip = Readable((unsigned char*)weapon + WPN_CLIP, 4) ? *(int*)((unsigned char*)weapon + WPN_CLIP) : 0;
    g_lsgPrimary(weapon, edx);
    __try
    {
        float hit[3];
        unsigned char* pev = WeaponOwnerPev(weapon);
        if (pev && *(int*)((unsigned char*)weapon + WPN_CLIP) < clip &&
            GunBullet(weapon, 0.012f, (int)ByMode(16, 176, 430), 1, BULLET_50AE, hit))
            GunBlast(hit, pev, ByMode(20, 150, 300), 90.0f, 6);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GunFixFault("Gravity Repulsor", GetExceptionCode());
    }
}

static void __fastcall HookedBgPrimary(void* weapon, void* edx)
{
    unsigned char* w = (unsigned char*)weapon;
    int clip = Readable(w + WPN_CLIP, 4) ? *(int*)(w + WPN_CLIP) : 0;
    float next = Readable(w + WPN_NEXT_PRIMARY, 4) ? *(float*)(w + WPN_NEXT_PRIMARY) : 0;
    g_bgPrimary(weapon, edx);
    __try
    {
        unsigned char* pev = WeaponOwnerPev(weapon);
        if (!pev)
            return;
        float hit[3];
        int anim = *(int*)(pev + PL_EV_WEAPONANIM);
        float scale = ByMode(1.0f, 680.0f / 32, 2400.0f / 32);
        if (*(int*)(w + WPN_CLIP) < clip)
            GunBullet(weapon, 0.008f, (int)(32 * scale), 2, BULLET_762, nullptr);
        else if (*(float*)(w + WPN_NEXT_PRIMARY) != next && (anim == 9 || anim == 10) &&
                 GunBullet(weapon, 0.0f, (int)(96 * scale), 3, BULLET_762, hit))
            GunBlast(hit, pev, 150.0f * scale, 200.0f, 30); // charged: charge_shootA
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GunFixFault("Giga Break", GetExceptionCode());
    }
}

static void __fastcall HookedBgSecondary(void* weapon, void* edx)
{
    unsigned char* w = (unsigned char*)weapon;
    float next = Readable(w + WPN_NEXT_SECONDARY, 4) ? *(float*)(w + WPN_NEXT_SECONDARY) : 0;
    g_bgSecondary(weapon, edx);
    __try
    {
        unsigned char* pev = WeaponOwnerPev(weapon);
        if (!pev || *(float*)(w + WPN_NEXT_SECONDARY) == next)
            return;
        int anim = *(int*)(pev + PL_EV_WEAPONANIM);
        float damage = anim == 2 ? 100.0f : anim == 3 ? 140.0f : anim == 0xb ? 220.0f : 0.0f;
        float scale = ByMode(1.0f, 680.0f / 32, 2400.0f / 32), hit[3];
        if (damage > 0 && GunBullet(weapon, 0.0f, (int)(damage * 0.4f * scale), 3, BULLET_762, hit))
            GunBlast(hit, pev, damage * scale, 140.0f + damage / 2, anim == 0xb ? 40 : 25);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GunFixFault("Giga Break (right click)", GetExceptionCode());
    }
}

static void* DetourChecked(DWORD addr, const unsigned char* code, int len, void* hook, const char* what)
{
    unsigned char* fn = (unsigned char*)(addr + g_mpDelta);
    if (!Readable(fn, len) || fn[0] == 0xe9)
        return nullptr;
    if (memcmp(fn, code, len))
    {
        Log("%s: mp.dll isn't the expected one, left as it is", what);
        return nullptr;
    }
    return DetourCdecl(fn, len, hook);
}

static void FixSciFiGuns()
{
    const unsigned char lsg[] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8 }; // push ebp; mov ebp, esp; and esp, -8
    const unsigned char bg1[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c }; // push ebp; mov ebp, esp; sub esp, 0xc
    const unsigned char bg2[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x08 }; // ... sub esp, 8
    if (!g_lsgPrimary)
        g_lsgPrimary = (WeaponFn)DetourChecked(MP_LSG_PRIMARY, lsg, sizeof(lsg), (void*)HookedLsgPrimary, "Gravity Repulsor");
    if (!g_bgPrimary)
        g_bgPrimary = (WeaponFn)DetourChecked(MP_BG_PRIMARY, bg1, sizeof(bg1), (void*)HookedBgPrimary, "Giga Break");
    if (!g_bgSecondary)
        g_bgSecondary = (WeaponFn)DetourChecked(MP_BG_SECONDARY, bg2, sizeof(bg2), (void*)HookedBgSecondary, "Giga Break");
    Log("guns: Gravity Repulsor %s, Giga Break %s (their shots had no bullet)", g_lsgPrimary ? "fires" : "left as it is",
        g_bgPrimary && g_bgSecondary ? "fires (right click: blast)" : "left as it is");
}
