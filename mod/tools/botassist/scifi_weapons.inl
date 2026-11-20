// ---------------------------------------------------------------- Gravity Repulsor, Giga Break, IGNITE-10: their special functions
// The server code of these weapons was only on Nexon's dedicated servers: this client's mp.dll keeps their animations
// (and client.dll their effects) but not what they do. botassist adds it, from the CSO wiki's descriptions (the user,
// 2026-11-21: "all their functions"; cso.fandom.com/wiki/Gravity_Repulsor, /Giga_Break, /IGNITE-10) and the user's
// videos. Numbers by mode (ByMode: scenario / zombie / the others) follow the game's own damage (iteminfo.json).
//
// Gravity Repulsor (weapon_leapstrikegun)
//  - each shot (gun_fixes.inl: a bullet and a small blast where it lands) also sends a pulse around the player that
//    pushes and hurts the enemies next to him; while hovering the shots hit harder.
//  - anti-gravity energy charges by itself (10 s); right click with it full: Repulsor Leap (hurts and pushes the
//    enemies around, then throws the player where he aims); when the leap ends with the gun in hand: Anti-Graviton
//    (hovers 6 s, shooting allowed); right click while hovering, or by itself when the hover ends with the gun in
//    hand: Gravity Strike (straight down; landing hurts, pushes and stuns a wide area; no damage taken until 1.5 s
//    after landing). The gauge restarts at 20% plus 5% for each enemy hit (at most 80%).
// Giga Break (weapon_beamgun)
//  - left click: bullets with a blast; every 6th shot in a row releases a bigger one.
//  - right click: swings that hit what's in front; the second of a combo also sends an energy wave forward (and in
//    zombie modes and Super Soldier a small black hole where it ends) and creates Overflow-Alpha (4 s).
//  - special ammunition charges by itself (30 s); R with it full: Overflow-Beta (10 s).
//  - in Overflow (the weapon's state +0x1c0 = 1/2, end time +0x1cc, the game's own animations): left click = Overflow
//    Burst (a big blast where you aim), right click = Overflow Crash (a dash that crashes into the area in front, no
//    damage taken meanwhile).
//  - zombie modes and Zombie Scenario: the Wingman shoots up to 2 enemies every 0.8 s; Overflow Shield (after 80 s
//    with it, 60 in scenario, and 20 hits): a hit that would kill is stopped, the zombies around pushed and hurt;
//    then 60 s before it can come back.
// IGNITE-10 (the Ignite Bomb, grenade type 0x15)
//  - thrown, it lies on the ground like a mine (up to 20 s) and blows up when an enemy comes near (or hits it);
//  - right click: Ignite Blast, it goes off in front of the player at once (damage and a strong push; one bomb).
static const int SF_EV_BUTTON = 0x1a4, SF_IN_ATTACK2 = 0x800, SF_FL_ONGROUND = 0x200, SF_FL_KILLME = 0x40000000;
static const int SF_EV_DMGTIME = 0x214, SF_GV_MAXCLIENTS = 0x90, SF_ENG_ENTOFINDEX = 77;
static const int WPN_STATE_GB = 0x1c0, WPN_STATE_END_GB = 0x1cc, WPN_IDLE = 0x144, WPN_AMMO_TYPE = 0x158;
static const int WPN_START_THROW = 0x5c, WPN_RELEASE_THROW = 0x60, WPN_GREN_TYPE = 0x220, PL_AMMO = 0x1034;
static const int WPN_VT_RETIRE = 0x2ac / 4;
static const DWORD MP_SET_MODEL = 0x121d3a64; // mp.dll's engine pointer: SetModel(edict, model)
typedef void* (*SfEntOfIndexFn)(int index);
typedef void (*SfSetModelFn)(void* edict, const char* model);

struct SfPlayer
{
    int buttons = 0;
    float energy = 0;      // Gravity Repulsor, 0-100
    int phase = 0;         // 0, 1 leap, 2 hover, 3 strike
    float phaseAt = 0, invulnUntil = 0;
    int leapHits = 0;
    bool energyTold = false;
    float beta = 0;        // Giga Break special ammunition, 0-100
    bool betaTold = false;
    float lastSwing = -100, lastShot = -100;
    int shotRun = 0;
    float crashUntil = 0, crashAt = 0;
    float overflowUntil = 0; // Giga Break: when its Overflow ends (botassist keeps the weapon's own timer with it)
    float heldSecs = 0;    // Giga Break in hand (shield)
    int hits = 0;
    bool shieldReady = false;
    float shieldBack = 0, nextWingman = 0;
    float blastAt = 0;     // IGNITE-10: the Ignite Blast's moment
    void* blastWeapon = nullptr;
    float lastFrame = 0;
    int level = -1;
};
static SfPlayer g_sf[33];

struct SfPull
{
    V3 at;
    void* owner;
    float until;
};
static std::vector<SfPull> g_sfPulls;

struct SfMine
{
    void* edict;
    void* priv;
    void* owner;
    float since, until;
    bool armed;
    int level;
};
static std::vector<SfMine> g_sfMines;

static void ShPrint(void* player, const char* text); // modes/zombie_shelter.inl: a line in the middle of the screen
static void Beam(V3 a, V3 b, int r, int g, int bl, int life);

static void* SfEdictOf(int i)
{
    SfEntOfIndexFn fn = (SfEntOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[SF_ENG_ENTOFINDEX];
    return fn ? fn(i) : nullptr;
}

static int SfIndex(void* edict)
{
    int i = g_IndexOfEdict ? g_IndexOfEdict(edict) : 0;
    return i >= 1 && i <= 32 ? i : 0;
}

static bool SfLive(void* e)
{
    return e && *(int*)e == 0 /* edict_t free */ && Alive(e) && F<float>(Vars(e), EV_TAKEDAMAGE) != 0.0f;
}

static bool SfEnemy(void* e, void* attacker)
{
    if (!e || e == attacker || !SfLive(e))
        return false;
    int flags = F<int>(Vars(e), EV_FLAGS);
    if (flags & FL_CLIENT)
        return TeamOf(e) != TeamOf(attacker);
    return (flags & FL_MONSTER) != 0;
}

static const char* SfActiveClass(void* player, void** weaponOut)
{
    unsigned char* p = (unsigned char*)PrivateOf(player);
    void* w = p ? *(void**)(p + PL_ACTIVE_ITEM) : nullptr;
    if (weaponOut)
        *weaponOut = w;
    unsigned char* pev = w && Readable(w, 12) ? *(unsigned char**)((unsigned char*)w + 8) : nullptr;
    return pev ? ClassName(*(void**)(pev + EV_CONTAINING_ENTITY)) : "";
}

static V3 SfAim(void* player)
{
    float f[3];
    AimVectors(Vars(player), f);
    return Vec(f);
}

static void SfKnock(void* e, V3 from, float force, float up)
{
    V3 d = Origin(e) - from;
    d.z = 0;
    float l = Len(d);
    d = l > 1 ? d * (1.0f / l) : V3{ 1, 0, 0 };
    float* v = (float*)(Vars(e) + EV_VELOCITY);
    v[0] = d.x * force;
    v[1] = d.y * force;
    if (up > 0)
        v[2] = up;
}

// the game's blast at `at` (TE_EXPLOSION + RadiusDamage, the attacker unhurt) and a push for the enemies around;
// returns how many enemies were in reach
static int SfArea(V3 at, void* attacker, float damage, float radius, float knock, float up, int scale)
{
    int n = 0;
    ForEntitiesNear(at, radius, [&](void* e) {
        if (!SfEnemy(e, attacker))
            return;
        n++;
        if (knock > 0)
            SfKnock(e, at, knock, up);
    });
    float a[3] = { at.x, at.y, at.z };
    GunBlast(a, Vars(attacker), damage, radius, scale);
    return n;
}

static void SfSay(void* player, const char* text)
{
    if (!(F<int>(Vars(player), EV_FLAGS) & FL_FAKECLIENT))
        ShPrint(player, text);
}

// ---------------------------------------------------------------- Gravity Repulsor
static float SfLsgBonus(void* player)
{
    int i = SfIndex(player);
    return i && g_sf[i].phase == 2 ? 1.5f : 1.0f;
}

// called by gun_fixes.inl after each shot: the pulse around the player
static void SfLsgShot(void* player)
{
    SfArea(Origin(player), player, ByMode(6, 80, 120) * SfLsgBonus(player), 120.0f, 260.0f, 0, 0);
}

// The video (the user's, "CSN:S Gravity Repulsor Review", 2026-11-21) and the game's own help panel: the gauge fills
// in about 10 s (+9-10%/s on the HUD); the leap takes the player up (to the training room's ceiling); hovering lasts
// about 6 s and he can shoot meanwhile; when it ends with the gun in hand the Gravity Strike goes off by itself (panel:
// "right-click during Anti-Graviton to use directly, or it automatically activates if you're using a weapon when the
// Anti-Graviton ends"); after landing the gauge was at about 25%. Hover and fall go through the player's own gravity
// (pev +0x12c: the client predicts it, no jerks), the leap is one push.
static const int SF_EV_GRAVITY = 0x12c;
static const float LSG_CHARGE_PER_S = 10.0f, LSG_HOVER_S = 6.0f, LSG_LEAP_SPEED = 1000.0f, LSG_LEAP_UP = 320.0f;

static void SfLsgStrike(void* player, SfPlayer& s, float now)
{
    unsigned char* v = Vars(player);
    float* vel = (float*)(v + EV_VELOCITY);
    vel[0] *= 0.2f;
    vel[1] *= 0.2f;
    vel[2] = -1600.0f;
    F<float>(v, SF_EV_GRAVITY) = 3.0f; // straight down, fast
    s.phase = 3;
    s.phaseAt = now;
    s.invulnUntil = now + 4.0f; // until it lands (then 1.5 s more)
}

static void SfLsgFrame(void* player, SfPlayer& s, bool active, int pressed, float now, float dt)
{
    unsigned char* v = Vars(player);
    float* vel = (float*)(v + EV_VELOCITY);
    bool ground = (F<int>(v, EV_FLAGS) & SF_FL_ONGROUND) != 0;
    if (s.energy < 100 && s.phase == 0)
    {
        s.energy = (std::min)(100.0f, s.energy + dt * LSG_CHARGE_PER_S);
        if (s.energy >= 100 && active && !s.energyTold)
        {
            s.energyTold = true;
            SfSay(player, "Gravity Repulsor: energy 100% - right click: Repulsor Leap");
        }
    }
    if (active && (pressed & SF_IN_ATTACK2))
    {
        if (s.phase == 0 && s.energy >= 100)
        {
            // Repulsor Leap: the enemies around are hurt and pushed, then the player is thrown where he aims
            s.leapHits = SfArea(Origin(player), player, ByMode(30, 500, 700), 200.0f, 650.0f, 250.0f, 20);
            V3 aim = SfAim(player);
            vel[0] = aim.x * LSG_LEAP_SPEED;
            vel[1] = aim.y * LSG_LEAP_SPEED;
            vel[2] = (std::max)(aim.z * LSG_LEAP_SPEED, 0.0f) + LSG_LEAP_UP;
            s.energy = 0;
            s.energyTold = false;
            s.phase = 1;
            s.phaseAt = now;
            return;
        }
        if (s.phase == 2)
        {
            SfLsgStrike(player, s, now); // Gravity Strike, used directly
            return;
        }
        if (s.phase == 0)
        {
            char t[96];
            snprintf(t, sizeof(t), "Gravity Repulsor: energy %d%%", (int)s.energy);
            SfSay(player, t);
        }
    }
    if (s.phase == 1 && ((vel[2] <= 60.0f && now - s.phaseAt > 0.2f) || now - s.phaseAt > 1.5f))
    {
        if (active)
        {
            s.phase = 2; // Anti-Graviton: hovers, free to move and shoot
            s.phaseAt = now;
            vel[2] = 0;
            F<float>(v, SF_EV_GRAVITY) = 0.001f;
            SfSay(player, "Anti-Graviton - right click: Gravity Strike");
        }
        else
        {
            s.phase = 0;
            s.energy = (std::min)(80.0f, s.leapHits * 10.0f);
        }
    }
    else if (s.phase == 2)
    {
        if (!active)
        {
            F<float>(v, SF_EV_GRAVITY) = 1.0f;
            s.phase = 0;
            s.energy = (std::min)(80.0f, s.leapHits * 10.0f);
        }
        else if (now - s.phaseAt >= LSG_HOVER_S || ground)
            SfLsgStrike(player, s, now); // the hover ended with the gun in hand: the strike goes off by itself
        else if (vel[2] < -40.0f || vel[2] > 40.0f)
            vel[2] *= 0.5f;
    }
    else if (s.phase == 3)
    {
        if (ground || now - s.phaseAt > 3.0f)
        {
            F<float>(v, SF_EV_GRAVITY) = 1.0f;
            int hits = SfArea(Origin(player) - V3{ 0, 0, 30 }, player, ByMode(150, 4000, 6000), 320.0f, 900.0f, 300.0f, 50);
            s.invulnUntil = now + 1.5f;
            s.phase = 0;
            s.energy = (std::min)(80.0f, 20.0f + (s.leapHits + hits) * 5.0f);
        }
        else if (vel[2] > -1600.0f)
            vel[2] = -1600.0f;
    }
}

// ---------------------------------------------------------------- Giga Break
static bool SfBlackHoleMode()
{
    int mode = GameMode();
    return mode == 57 /* Super Soldier */ || (ZombieMode(mode) && !CoopMode());
}

static bool SfWingmanMode()
{
    return ZombieMode(GameMode());
}

static void SfEnterOverflow(SfPlayer& s, void* weapon, float secs)
{
    unsigned char* w = (unsigned char*)weapon;
    *(int*)(w + WPN_STATE_GB) = 1;
    *(float*)(w + WPN_STATE_END_GB) = secs; // the weapon's times count down to 0 (UTIL_WeaponTimeBase is 0)
    s.overflowUntil = Now() + secs;
    if (g_sendWeaponAnim)
        g_sendWeaponAnim(weapon, nullptr, 6 /* change_idle_charge */, 0);
}

// The user's video ("CSN:S Review Giga Break & Giga Break LE", training room against a zombie target, 2026-11-21):
// basic shots ~865 each (bullet + blast), and after a run of shots a bigger release (~1290); right click = melee swings
// (~96 each), the combo ends with a wide energy wave and the weapon changes (Overflow-Alpha, ~4 s); in Overflow the
// right click dashes into the target behind a hexagon shield (Overflow Crash, ~1920) and the left click hits one far
// target very hard (Overflow Burst, ~17000). The special ammunition gauge (bottom right) fills ~3.3%/s (30 s).
// Zombie numbers from the video; scenario x3.5 and the others x0.05 (the game's 2400 / 680 / 32 for a bullet).
static const float GB_ALPHA_S = 4.0f, GB_BETA_S = 10.0f, GB_BETA_PER_S = 3.33f;
static const int GB_RELEASE_EVERY = 6;

static float SfGb(float zombie, float pvp)
{
    return ByMode(pvp, zombie, zombie * 3.5f);
}

// gun_fixes.inl, after the game's PrimaryAttack: normalShot = a round taken, burst = an Overflow Burst played (9/10)
static void SfBgPrimary(void* player, void* weapon, bool normalShot, int anim, bool burst)
{
    int i = SfIndex(player);
    if (!i)
        return;
    SfPlayer& s = g_sf[i];
    float now = Now();
    float hit[3];
    if (normalShot)
    {
        if (!GunBullet(weapon, 0.008f, (int)ByMode(32, 680, 2400), 2, BULLET_762, hit))
            return;
        GunBlast(hit, Vars(player), SfGb(185, 10), 70.0f, 4);
        s.shotRun = now - s.lastShot < 1.0f ? s.shotRun + 1 : 1;
        s.lastShot = now;
        if (s.shotRun % GB_RELEASE_EVERY == 0) // consecutive basic attacks release energy
            SfArea(Vec(hit), player, SfGb(1290, 60), 150.0f, 300.0f, 150.0f, 25);
    }
    else if (burst && GunBullet(weapon, 0.0f, (int)SfGb(2000, 60), 0, BULLET_762, hit))
        SfArea(Vec(hit), player, SfGb(15000, 220), 140.0f, 500.0f, 200.0f, 45); // Overflow Burst
    (void)anim;
}

// the swings' energy wave: what's along the aim line up to 700 units
static void SfBgWave(void* player)
{
    V3 eye = Eye(player), aim = SfAim(player);
    TraceResult tr;
    Trace(eye, eye + aim * 700.0f, player, 1, tr);
    float len = tr.flFraction * 700.0f;
    for (float d = 80.0f; d < len; d += 110.0f)
        SfArea(eye + aim * d, player, SfGb(300, 25) / 2, 90.0f, 350.0f, 120.0f, d < 100 ? 20 : 0);
    if (SfBlackHoleMode()) // zombie modes and Super Soldier: a small black hole where the wave ends
        g_sfPulls.push_back({ eye + aim * (std::max)(0.0f, len - 30.0f), player, Now() + 1.5f });
}

// gun_fixes.inl, after the game's SecondaryAttack when it did something
static void SfBgSecondary(void* player, void* weapon, int anim)
{
    int i = SfIndex(player);
    if (!i)
        return;
    SfPlayer& s = g_sf[i];
    float now = Now();
    if (anim == 2 || anim == 3)
    {
        // a swing: what's in front of the player
        V3 front = Eye(player) + SfAim(player) * 80.0f;
        SfArea(front, player, SfGb(96, 35), 120.0f, 250.0f, 100.0f, 0);
        if (anim == 3 && now - s.lastSwing < 2.0f && *(int*)((unsigned char*)weapon + WPN_STATE_GB) == 0)
        {
            SfBgWave(player); // the combo's end: the wave, then Overflow-Alpha
            SfEnterOverflow(s, weapon, GB_ALPHA_S);
        }
        s.lastSwing = now;
    }
    else if (anim == 0xb)
    {
        // Overflow Crash: a dash behind the shield, then the crash
        V3 aim = SfAim(player);
        float* vel = (float*)(Vars(player) + EV_VELOCITY);
        float l = Len2D(aim);
        aim = l > 0.1f ? V3{ aim.x / l, aim.y / l, 0 } : V3{ 1, 0, 0 };
        vel[0] = aim.x * 1300.0f;
        vel[1] = aim.y * 1300.0f;
        vel[2] = 140.0f;
        s.crashAt = now + 0.35f;
        s.crashUntil = now + 0.35f;
        s.invulnUntil = now + 0.9f;
    }
}

static void SfBgFrame(void* player, SfPlayer& s, void* weapon, bool active, int pressed, float now, float dt)
{
    if (!active)
        return;
    unsigned char* w = (unsigned char*)weapon;
    int state = *(int*)(w + WPN_STATE_GB);
    s.heldSecs += dt;
    if (s.overflowUntil > 0)
    {
        if (state == 0)
            s.overflowUntil = 0; // the game ended it (its last Burst)
        else if (now >= s.overflowUntil)
        {
            *(int*)(w + WPN_STATE_GB) = 0;
            *(float*)(w + WPN_STATE_END_GB) = 0;
            s.overflowUntil = 0;
            state = 0;
            if (g_sendWeaponAnim)
                g_sendWeaponAnim(weapon, nullptr, 7 /* change_charge_idle */, 0);
        }
        else
            *(float*)(w + WPN_STATE_END_GB) = s.overflowUntil - now;
    }
    if (state == 0 && s.beta < 100)
    {
        s.beta = (std::min)(100.0f, s.beta + dt * GB_BETA_PER_S);
        if (s.beta >= 100 && !s.betaTold)
        {
            s.betaTold = true;
            SfSay(player, "Giga Break: special ammunition charged - R: Overflow-Beta");
        }
    }
    if ((pressed & IN_RELOAD) && state == 0 && s.beta >= 100)
    {
        s.beta = 0;
        s.betaTold = false;
        SfEnterOverflow(s, weapon, GB_BETA_S);
    }
    if (s.crashAt && now >= s.crashAt)
    {
        s.crashAt = 0;
        V3 front = Origin(player) + SfAim(player) * 60.0f;
        SfArea(front, player, SfGb(1920, 90), 200.0f, 700.0f, 250.0f, 35);
        if (g_sendWeaponAnim)
            g_sendWeaponAnim(weapon, nullptr, 13 /* charge_shootB_end */, 0);
    }
    if (SfWingmanMode() && now >= s.nextWingman)
    {
        s.nextWingman = now + 0.8f;
        V3 eye = Eye(player);
        std::vector<std::pair<float, void*>> targets;
        ForEntitiesNear(eye, 900.0f, [&](void* e) {
            if (!SfEnemy(e, player))
                return;
            TraceResult tr;
            if (Trace(eye, Center(e), player, 1, tr))
                targets.push_back({ Len(Center(e) - eye), e });
        });
        std::sort(targets.begin(), targets.end());
        for (size_t k = 0; k < targets.size() && k < 2; k++)
        {
            V3 c = Center(targets[k].second);
            Beam(eye + V3{ 0, 0, 20 }, c, 255, 60, 60, 2);
            float at[3] = { c.x, c.y, c.z };
            unsigned char* pv = Vars(player);
            float take = F<float>(pv, EV_TAKEDAMAGE);
            F<float>(pv, EV_TAKEDAMAGE) = 0;
            ((RadiusDamageFn)(MP_RADIUS_DAMAGE + g_mpDelta))(at[0], at[1], at[2], pv, pv, ByMode(15, 150, 300), 40.0f, 0, 2);
            F<float>(pv, EV_TAKEDAMAGE) = take;
        }
    }
    if (SfWingmanMode() && !s.shieldReady && now >= s.shieldBack &&
        s.heldSecs >= (CoopMode() ? 60.0f : 80.0f) && s.hits >= 20)
    {
        s.shieldReady = true;
        SfSay(player, "Giga Break: Overflow Shield ready");
    }
}

// ---------------------------------------------------------------- IGNITE-10
static void SfIgniteExplode(V3 at, void* owner, bool impact)
{
    if (!owner || *(int*)owner != 0)
        return;
    if (impact)
        SfArea(at, owner, ByMode(60, 750, 750), 95.0f, 1250.0f, 200.0f, 12);
    else
        SfArea(at + V3{ 0, 0, 12 }, owner, ByMode(120, 1500, 1500), 170.0f, 300.0f, 150.0f, 30);
}

// grenade_fixes.inl: the bomb the game just threw (as an HE grenade)
static void SfIgniteThrown(void* grenade, void* ownerPev)
{
    unsigned char* g = (unsigned char*)grenade;
    unsigned char* pev = g && Readable(g + 8, 4) ? *(unsigned char**)(g + 8) : nullptr;
    if (!pev || strcmp(String(*(int*)pev), "grenade"))
        return;
    void* edict = *(void**)(pev + EV_CONTAINING_ENTITY);
    std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    if (!g_drumsMap.empty() && map == g_drumsMap)
        (*(SfSetModelFn*)(MP_SET_MODEL + g_mpDelta))(edict, "models/w_ignitebomb.mdl");
    *(float*)(pev + SF_EV_DMGTIME) = Now() + 60.0f; // its own fuse never runs out: it's a mine
    void* owner = ownerPev ? *(void**)((unsigned char*)ownerPev + EV_CONTAINING_ENTITY) : nullptr;
    g_sfMines.push_back({ edict, grenade, owner, Now(), Now() + 20.0f, false, g_nav.level });
}

static void SfMinesFrame(float now)
{
    for (size_t i = 0; i < g_sfMines.size();)
    {
        SfMine m = g_sfMines[i];
        bool gone = m.level != g_nav.level || *(int*)m.edict != 0 || PrivateOf(m.edict) != m.priv;
        if (!gone)
        {
            unsigned char* v = Vars(m.edict);
            F<float>(v, SF_EV_DMGTIME) = now + 60.0f;
            V3 at = Origin(m.edict);
            float* vel = (float*)(v + EV_VELOCITY);
            if (!m.armed && ((F<int>(v, EV_FLAGS) & SF_FL_ONGROUND) || Len(Vec(vel)) < 20.0f) && now - m.since > 0.3f)
                g_sfMines[i].armed = m.armed = true;
            float reach = m.armed ? 110.0f : 60.0f;
            bool boom = now >= m.until || !m.owner || *(int*)m.owner != 0;
            if (!boom)
                ForEntitiesNear(at, reach, [&](void* e) {
                    if (!boom && SfEnemy(e, m.owner))
                        boom = true;
                });
            if (boom)
            {
                if (m.owner && *(int*)m.owner == 0)
                    SfIgniteExplode(at, m.owner, false);
                F<int>(v, EV_FLAGS) |= SF_FL_KILLME;
                gone = true;
            }
        }
        if (gone)
            g_sfMines.erase(g_sfMines.begin() + i);
        else
            i++;
    }
}

static void SfIgniteFrame(void* player, SfPlayer& s, void* weapon, bool active, int pressed, float now)
{
    unsigned char* w = (unsigned char*)weapon;
    if (s.blastWeapon && now >= s.blastAt)
    {
        void* bw = s.blastWeapon;
        s.blastWeapon = nullptr;
        SfIgniteExplode(Eye(player) + SfAim(player) * 75.0f, player, true);
        unsigned char* p = (unsigned char*)PrivateOf(player);
        if (p && *(void**)(p + PL_ACTIVE_ITEM) == bw)
        {
            int* ammo = (int*)(p + PL_AMMO + *(int*)((unsigned char*)bw + WPN_AMMO_TYPE) * 4);
            if (*ammo > 0)
                (*ammo)--;
            if (*ammo <= 0)
                ((void(__fastcall*)(void*, void*))(*(void***)bw)[WPN_VT_RETIRE])(bw, nullptr);
        }
        return;
    }
    if (!active || !(pressed & SF_IN_ATTACK2) || *(int*)(w + WPN_GREN_TYPE) != GREN_IGNITE || s.blastWeapon)
        return;
    unsigned char* p = (unsigned char*)PrivateOf(player);
    int ammo = p ? *(int*)(p + PL_AMMO + *(int*)(w + WPN_AMMO_TYPE) * 4) : 0;
    if (ammo <= 0 || *(float*)(w + WPN_START_THROW) != 0.0f || *(float*)(w + WPN_NEXT_PRIMARY) > 0.0f)
        return; // no bomb, or the pin is being pulled
    if (g_sendWeaponAnim)
        g_sendWeaponAnim(weapon, nullptr, 4 /* bmode */, 0);
    *(float*)(w + WPN_NEXT_PRIMARY) = 1.0f;
    *(float*)(w + WPN_NEXT_SECONDARY) = 1.0f;
    *(float*)(w + WPN_IDLE) = 1.0f;
    s.blastWeapon = weapon;
    s.blastAt = now + 0.3f;
}

// ---------------------------------------------------------------- every frame
static void SfFrame()
{
    if (!g_pGlobals || !*g_pGlobals || !g_IndexOfEdict)
        return;
    float now = Now();
    int maxClients = *(int*)((DWORD)*g_pGlobals + SF_GV_MAXCLIENTS);
    for (int i = 1; i <= maxClients && i <= 32; i++)
    {
        SfPlayer& s = g_sf[i];
        if (s.level != g_nav.level)
        {
            s = SfPlayer();
            s.level = g_nav.level;
        }
        void* e = SfEdictOf(i);
        if (!e || *(int*)e != 0 || !PrivateOf(e) || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            continue;
        float dt = s.lastFrame > 0 && now > s.lastFrame && now - s.lastFrame < 1.0f ? now - s.lastFrame : 0.0f;
        s.lastFrame = now;
        if (!Alive(e))
        {
            if (s.phase >= 2)
                F<float>(Vars(e), SF_EV_GRAVITY) = 1.0f; // (hovering or falling: the gravity back)
            s.phase = 0;
            s.blastWeapon = nullptr;
            continue;
        }
        int buttons = F<int>(Vars(e), SF_EV_BUTTON);
        int pressed = buttons & ~s.buttons;
        s.buttons = buttons;
        void* weapon = nullptr;
        const char* cls = SfActiveClass(e, &weapon);
        __try
        {
            SfLsgFrame(e, s, !strcmp(cls, "weapon_leapstrikegun"), pressed, now, dt);
            SfBgFrame(e, s, weapon, !strcmp(cls, "weapon_beamgun"), pressed, now, dt);
            if (weapon)
                SfIgniteFrame(e, s, weapon, !strcmp(cls, "weapon_hegrenade"), pressed, now);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static int faults = 0;
            if (faults++ < 5)
                Log("sci-fi weapons: exception 0x%08lX", GetExceptionCode());
        }
    }
    __try
    {
        SfMinesFrame(now);
        for (size_t k = 0; k < g_sfPulls.size();)
        {
            SfPull& p = g_sfPulls[k];
            if (now >= p.until || !p.owner || *(int*)p.owner != 0)
            {
                g_sfPulls.erase(g_sfPulls.begin() + k);
                continue;
            }
            ForEntitiesNear(p.at, 220.0f, [&](void* e) {
                if (!SfEnemy(e, p.owner))
                    return;
                V3 d = p.at - Origin(e);
                float l = Len(d);
                if (l < 1)
                    return;
                float* v = (float*)(Vars(e) + EV_VELOCITY);
                v[0] = d.x / l * 280.0f;
                v[1] = d.y / l * 280.0f;
            });
            k++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// weapon_balance.inl's TakeDamage hook (every player, and the monsters of the co-op modes): no damage while a move
// protects the player; the Overflow Shield stops a hit that would kill; hits by a Giga Break count for the shield
static float SfOnDamage(void* victimPriv, void* attackerPev, float damage)
{
    unsigned char* vp = (unsigned char*)victimPriv;
    unsigned char* vpev = vp ? *(unsigned char**)(vp + 8) : nullptr;
    if (!vpev)
        return damage;
    void* victim = *(void**)(vpev + EV_CONTAINING_ENTITY);
    float now = Now();
    int vi = (F<int>(vpev, EV_FLAGS) & FL_CLIENT) ? SfIndex(victim) : 0;
    if (vi)
    {
        SfPlayer& s = g_sf[vi];
        if (now < s.invulnUntil)
            return 0;
        if (s.shieldReady && damage >= F<float>(vpev, EV_HEALTH))
        {
            s.shieldReady = false;
            s.shieldBack = now + 60.0f;
            s.hits = 0;
            s.heldSecs = 0;
            SfArea(Origin(victim), victim, ByMode(60, 1000, 1500), 250.0f, 900.0f, 250.0f, 30);
            SfSay(victim, "Giga Break: Overflow Shield!");
            return 0;
        }
    }
    void* attacker = attackerPev ? *(void**)((unsigned char*)attackerPev + EV_CONTAINING_ENTITY) : nullptr;
    int ai = attacker && (F<int>((unsigned char*)attackerPev, EV_FLAGS) & FL_CLIENT) ? SfIndex(attacker) : 0;
    if (ai && attacker != victim && damage > 0 && !strcmp(SfActiveClass(attacker, nullptr), "weapon_beamgun"))
        g_sf[ai].hits++;
    return damage;
}

static float SafeSfOnDamage(void* victimPriv, void* attackerPev, float damage)
{
    __try
    {
        return SfOnDamage(victimPriv, attackerPev, damage);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return damage;
    }
}
