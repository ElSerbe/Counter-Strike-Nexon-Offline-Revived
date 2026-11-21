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
//  - right click: swings that hit what's in front; the first one's energy wave goes forward; the second one in a row
//    (in zombie modes and Super Soldier also a small black hole) and the game itself starts Overflow-Alpha.
//  - special ammunition (weapon +0x168) charges by itself in 30 s; both mouse buttons together with it full: the game
//    itself starts Overflow-Beta (its 0x10bab850).
//  - in Overflow (the weapon's state +0x1c0 = 1/2, countdown +0x1cc, the game's own animations): left click = Overflow
//    Burst (one far target hit very hard), right click = Overflow Crash (a jump forward that the game ends with a fall,
//    its state +0x178; the landing crashes into the area; no damage taken meanwhile).
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
    bool lsgEx = false;    // the Abyss Repulsor (its own effects)
    float nextFly = 0;     // Anti-Graviton: the next ring under the feet
    bool energyTold = false;
    float beta = 0;        // Giga Break special ammunition, 0-100
    bool betaTold = false;
    float lastSwing = -100, lastShot = -100;
    int shotRun = 0;
    float crashUntil = 0, waveAt = 0;
    float overflowUntil = 0; // Giga Break: when its Overflow ends (botassist keeps the weapon's own timer with it)
    float heldSecs = 0;    // Giga Break in hand (shield)
    int hits = 0;
    bool shieldReady = false;
    float shieldBack = 0, nextWingman = 0;
    float blastAt = 0;     // IGNITE-10: the Ignite Blast's moment
    void* blastWeapon = nullptr;
    float lastFrame = 0;
    void* drone = nullptr; // the Wingman's drone (an info_target), its private data
    void* droneEdict = nullptr;
    bool droneLe = false;
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
static void SfSound(void* edict, const char* sample);

static float SfLsgBonus(void* player)
{
    int i = SfIndex(player);
    return i && g_sf[i].phase == 2 ? 1.5f : 1.0f;
}

// The game's own effect files (the user, 2026-11-21: "its files must be in the game, leapstrikegun is its ID"): the
// packs (Data/Packer) still have sprites/ef_leapstrikegun_shoot.spr (a blue hexagon grid, 26 frames), _hit.spr (the
// violet burst, 15), _jump.spr (a blue shock ring, 30), _fly.spr (a cyan ring, 30) and models/ef_leapstrikegun_landing.mdl
// (the landing blast: its textures jumpfx, lendexplo_ground, lendexplo1/2; one 30-frame sequence); the Abyss Repulsor
// has the same set as ef_leapstrikegunex_* (red). Their users, the anti-gravity round and the dive, were server
// entities that are gone (leapstrikegun_proj / _dive: their names are precached by 0x10e65e11, no class). The user's
// video shows the round's violet burst where it lands (the hit sprite) and the blast on landing; botassist plays the
// sprites with the TE_EXPLOSION the game's barrels use (no sound, light or particles) and the landing model as a short
// lived entity. Sprites are 256 px: TE_EXPLOSION's scale is in tenths, 10 = 256 units.
static const int LSG_FX_SHOOT = 0, LSG_FX_HIT = 1, LSG_FX_JUMP = 2, LSG_FX_FLY = 3;
static int g_lsgSprite[2][4] = {};
static bool g_lsgRing[2] = {}; // the flat ring (ef_leapstrikegun_explo_h.spr, made by model_aliases.inl) precached
static const char* kLsgRing[2] = { "sprites/ef_leapstrikegun_explo_h.spr", "sprites/ef_leapstrikegunex_explo_h.spr" };
static std::string g_lsgFxMap; // the map they were precached for
static const int SF_EV_SEQUENCE = 0x138, SF_EV_FRAME = 0x140, SF_EV_ANIMTIME = 0x144, SF_EV_FRAMERATE = 0x148;
static const int SF_EV_SCALE = 0x154, SF_EV_RENDERMODE = 0x160, SF_EV_RENDERAMT = 0x164; // (mp.dll's entvars save table)

static void SfLsgPrecache(PrecacheFn model, PrecacheFn sound)
{
    static const char* kSprites[2][4] = {
        { "sprites/ef_leapstrikegun_shoot.spr", "sprites/ef_leapstrikegun_hit.spr", "sprites/ef_leapstrikegun_jump.spr",
          "sprites/ef_leapstrikegun_fly.spr" },
        { "sprites/ef_leapstrikegunex_shoot.spr", "sprites/ef_leapstrikegunex_hit.spr",
          "sprites/ef_leapstrikegunex_jump.spr", "sprites/ef_leapstrikegunex_fly.spr" } };
    for (int g = 0; g < 2; g++)
        for (int k = 0; k < 4; k++)
            g_lsgSprite[g][k] = model(kSprites[g][k]);
    model("models/ef_leapstrikegun_landing.mdl");
    for (int g = 0; g < 2; g++) // only when the file could be made: a missing model stops the map
        if ((g_lsgRing[g] = GameFileExists(kLsgRing[g])))
            model(kLsgRing[g]);
    model("models/ef_leapstrikegunex_landing.mdl");
    sound("weapons/leapstrikegun-1_exp.wav");
    sound("weapons/leapstrikegunex-1_exp.wav");
    sound("weapons/leapstrikegun_change_fly_exp.wav");
    g_lsgFxMap = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
}

static bool SfLsgFxReady()
{
    return !g_lsgFxMap.empty() && g_lsgFxMap == String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
}

// one of the gun's sprites, played once at `at`
static void SfLsgSprite(bool ex, int which, V3 at, int scale, int framerate)
{
    int index = g_lsgSprite[ex ? 1 : 0][which];
    if (!index || !SfLsgFxReady() || !g_MessageBegin || !g_WriteByte || !g_WriteShort || !g_WriteCoord || !g_MessageEnd)
        return;
    float a[3] = { at.x, at.y, at.z };
    g_MessageBegin(4 /* MSG_PVS */, 23 /* SVC_TEMPENTITY */, a, nullptr);
    g_WriteByte(3); // TE_EXPLOSION (CSO's: 4 bytes after the sprite, see GunBlast)
    g_WriteCoord(a[0]);
    g_WriteCoord(a[1]);
    g_WriteCoord(a[2]);
    g_WriteShort(index);
    g_WriteByte(scale);
    g_WriteByte(framerate);
    g_WriteByte(2 | 4 | 8); // no light, no sound, no particles
    g_WriteByte(0);
    g_MessageEnd();
}

// short-lived entities: the landing blast's model, the sound of a round where it lands
struct SfFx
{
    void* edict;
    void* priv;
    float until;
    int level;
    float start = 0, fps = 0; // a sprite: its frame follows the clock (sprites don't play by themselves)
    int frames = 0;
};
static std::vector<SfFx> g_sfFx;

static void SfFxSpawn(V3 at, const char* model, const char* sound, float life)
{
    if (!SfLsgFxReady())
        return;
    static const char kClass[] = "info_target";
    const char* base = *(const char**)((DWORD)*g_pGlobals + GV_STRINGBASE);
    void* edict = ((CreateNamedFn)(MP_CREATE_NAMED + g_mpDelta))((int)(kClass - base), 0);
    void* priv = edict ? PrivateOf(edict) : nullptr;
    if (!priv)
        return;
    unsigned char* v = Vars(edict);
    float* o = (float*)(v + EV_ORIGIN);
    o[0] = at.x, o[1] = at.y, o[2] = at.z;
    if (model)
    {
        (*(SfSetModelFn*)(MP_SET_MODEL + g_mpDelta))(edict, model);
        F<int>(v, EV_SOLID_OFS) = 0;
        F<int>(v, SF_EV_SEQUENCE) = 0;
        F<float>(v, SF_EV_FRAME) = 0;
        F<float>(v, SF_EV_ANIMTIME) = Now();
        F<float>(v, SF_EV_FRAMERATE) = 1.0f;
    }
    if (sound)
        SfSound(edict, sound);
    g_sfFx.push_back({ edict, priv, Now() + life, g_nav.level });
}

// the round's flat ring where it lands: the oriented sprite on an entity lying flat (pitch 90), additive
static void SfLsgRing(bool ex, V3 at, float scale)
{
    if (!g_lsgRing[ex ? 1 : 0] || !SfLsgFxReady())
        return;
    SfFxSpawn(at, kLsgRing[ex ? 1 : 0], nullptr, 0.5f);
    if (g_sfFx.empty())
        return;
    SfFx& f = g_sfFx.back();
    unsigned char* v = Vars(f.edict);
    float* ang = (float*)(v + EV_ANGLES);
    ang[0] = 90.0f, ang[1] = 0, ang[2] = 0;
    F<float>(v, SF_EV_FRAMERATE) = 0;
    F<float>(v, SF_EV_SCALE) = scale;
    F<int>(v, SF_EV_RENDERMODE) = 5; // kRenderTransAdd
    F<float>(v, SF_EV_RENDERAMT) = 255.0f;
    f.start = Now();
    f.fps = 30.0f;
    f.frames = 15;
}

static void SfFxFrame(float now)
{
    for (size_t i = 0; i < g_sfFx.size();)
    {
        SfFx& f = g_sfFx[i];
        bool alive = f.level == g_nav.level && *(int*)f.edict == 0 && PrivateOf(f.edict) == f.priv;
        if (alive && now < f.until)
        {
            if (f.frames)
                F<float>(Vars(f.edict), SF_EV_FRAME) = (std::min)((float)(f.frames - 1), (now - f.start) * f.fps);
            i++;
            continue;
        }
        if (alive)
            F<int>(Vars(f.edict), EV_FLAGS) |= SF_FL_KILLME;
        g_sfFx.erase(g_sfFx.begin() + i);
    }
}

static bool SfLsgIsEx(void* weapon)
{
    unsigned char* pev = weapon && Readable((unsigned char*)weapon + 8, 4) ? *(unsigned char**)((unsigned char*)weapon + 8)
                                                                          : nullptr;
    return pev && !strcmp(String(*(int*)pev), "weapon_leapstrikegunex");
}

// called by gun_fixes.inl after each shot: the round's burst where it lands (the hit sprite, the hexagons, its
// "-1_exp" sound) and the pulse around the player (hurts and pushes the enemies next to him)
static void SfLsgShot(void* player, void* weapon, const float* hit)
{
    bool ex = SfLsgIsEx(weapon);
    V3 at = Vec(hit);
    SfLsgSprite(ex, LSG_FX_HIT, at, 6, 30);
    SfLsgSprite(ex, LSG_FX_SHOOT, at, 4, 40);
    SfLsgRing(ex, at, 0.8f);
    SfFxSpawn(at, nullptr, ex ? "weapons/leapstrikegunex-1_exp.wav" : "weapons/leapstrikegun-1_exp.wav", 1.0f);
    SfArea(Origin(player), player, ByMode(6, 80, 120) * SfLsgBonus(player), 150.0f, 260.0f, 0, 0);
}

// The video (the user's, "CSN:S Gravity Repulsor Review", 2026-11-21) and the game's own help panel: the gauge fills
// in about 10 s (+9-10%/s on the HUD); the leap takes the player up (to the training room's ceiling); hovering lasts
// about 6 s and he can shoot meanwhile; when it ends with the gun in hand the Gravity Strike goes off by itself (panel:
// "right-click during Anti-Graviton to use directly, or it automatically activates if you're using a weapon when the
// Anti-Graviton ends"); after landing the gauge was at about 25%. Hover and fall go through the player's own gravity
// (pev +0x12c: the client predicts it, no jerks), the leap is one push.
static const int SF_EV_GRAVITY = 0x12c;
static const float LSG_CHARGE_PER_S = 10.0f, LSG_HOVER_S = 6.0f, LSG_LEAP_SPEED = 1000.0f, LSG_LEAP_UP = 320.0f;

// What the game still has (2026-11-21): client.dll's LeapStrikeGunEffectManager draws the gauge and the charged look
// (the purple lightning, the change_* sounds) from the weapon's +0x168 (0-100; 100 = charged) and its state +0x1c0
// (2 and up: the flying sounds); mp.dll resets +0x168 when the gun is dropped (0x10e662e0: "when dropped and picked up
// again the energy resets") and has CLeapStrikeGunModule, the player movement of the leap (1), the hover (2) and the
// dive (3), but what started it, the anti-gravity rounds (leapstrikegun_proj) and the dive (leapstrikegun_dive) are
// gone. botassist keeps the gauge and the state in the weapon so the game shows them, and does the moves itself.
// Gravity Repulsor and Abyss Repulsor (weapon_leapstrikegunex: CLeapStrikeGunEX shares every function but its
// precache and spawn).
static const int WPN_LSG_ENERGY = 0x168, WPN_LSG_STATE = 0x1c0;
static const DWORD MP_EMIT_SOUND = 0x114fe930; // cdecl edict, channel, sample, volume, attenuation, flags, pitch
typedef void (*EmitSoundFn)(void* edict, int channel, const char* sample, float vol, float attn, int flags, int pitch);

static bool SfIsLsg(const char* cls)
{
    return !strcmp(cls, "weapon_leapstrikegun") || !strcmp(cls, "weapon_leapstrikegunex");
}

static void SfSound(void* edict, const char* sample)
{
    __try
    {
        ((EmitSoundFn)(MP_EMIT_SOUND + g_mpDelta))(edict, 1 /* CHAN_WEAPON */, sample, 1.0f, 0.8f, 0, 100);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void SfLsgSetState(void* weapon, int phase)
{
    if (weapon)
        *(int*)((unsigned char*)weapon + WPN_LSG_STATE) = phase;
}

static void SfLsgStrike(void* player, void* weapon, SfPlayer& s, float now)
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
    SfLsgSetState(weapon, 3);
}

static void SfLsgFrame(void* player, SfPlayer& s, void* weapon, bool active, int pressed, float now, float dt)
{
    unsigned char* v = Vars(player);
    float* vel = (float*)(v + EV_VELOCITY);
    bool ground = (F<int>(v, EV_FLAGS) & SF_FL_ONGROUND) != 0;
    int* energy = active ? (int*)((unsigned char*)weapon + WPN_LSG_ENERGY) : nullptr;
    if (active)
        s.lsgEx = SfLsgIsEx(weapon);
    if (energy && s.phase == 0 && *energy < 100)
    {
        s.energy += dt * LSG_CHARGE_PER_S; // (fractions; the weapon keeps whole percents)
        int add = (int)s.energy;
        if (add > 0)
        {
            s.energy -= add;
            *energy = (std::min)(100, *energy + add);
        }
    }
    if (energy && (pressed & SF_IN_ATTACK2))
    {
        if (s.phase == 0 && *energy >= 100)
        {
            // Repulsor Leap: the enemies around are hurt and pushed, then the player is thrown where he aims
            s.leapHits = SfArea(Origin(player), player, ByMode(30, 500, 700), 200.0f, 650.0f, 250.0f, 0);
            SfLsgSprite(s.lsgEx, LSG_FX_JUMP, Origin(player) - V3{ 0, 0, 20 }, 10, 30);
            V3 aim = SfAim(player);
            vel[0] = aim.x * LSG_LEAP_SPEED;
            vel[1] = aim.y * LSG_LEAP_SPEED;
            vel[2] = (std::max)(aim.z * LSG_LEAP_SPEED, 0.0f) + LSG_LEAP_UP;
            *energy = 0;
            s.energy = 0;
            s.phase = 1;
            s.phaseAt = now;
            SfLsgSetState(weapon, 1);
            return;
        }
        if (s.phase == 2)
        {
            SfLsgStrike(player, weapon, s, now); // Gravity Strike, used directly
            return;
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
            SfLsgSetState(weapon, 2);
        }
        else
        {
            s.phase = 0;
            s.energy = 0;
        }
    }
    else if (s.phase == 2)
    {
        if (!active)
        {
            F<float>(v, SF_EV_GRAVITY) = 1.0f;
            s.phase = 0;
        }
        else if (now - s.phaseAt >= LSG_HOVER_S || ground)
            SfLsgStrike(player, weapon, s, now); // the hover ended with the gun in hand: the strike goes off by itself
        else
        {
            if (vel[2] < -40.0f || vel[2] > 40.0f)
                vel[2] *= 0.5f;
            if (now >= s.nextFly)
            {
                s.nextFly = now + 1.0f;
                SfLsgSprite(s.lsgEx, LSG_FX_FLY, Origin(player) - V3{ 0, 0, 34 }, 4, 30);
            }
        }
    }
    else if (s.phase == 3)
    {
        if (ground || now - s.phaseAt > 3.0f)
        {
            F<float>(v, SF_EV_GRAVITY) = 1.0f;
            int hits = SfArea(Origin(player) - V3{ 0, 0, 30 }, player, ByMode(150, 4000, 6000), 320.0f, 900.0f, 300.0f, 0);
            SfSound(*(void**)(v + EV_CONTAINING_ENTITY), "weapons/leapstrikegun_change_fly_exp.wav");
            TraceResult tr;
            Trace(Origin(player), Origin(player) - V3{ 0, 0, 120 }, player, 1, tr);
            V3 floor = Vec(tr.vecEndPos) + V3{ 0, 0, 2 };
            SfFxSpawn(floor, s.lsgEx ? "models/ef_leapstrikegunex_landing.mdl" : "models/ef_leapstrikegun_landing.mdl",
                      nullptr, 1.1f);
            SfLsgSprite(s.lsgEx, LSG_FX_HIT, floor + V3{ 0, 0, 40 }, 14, 30);
            s.invulnUntil = now + 1.5f;
            s.phase = 0;
            SfLsgSetState(weapon, 0);
            if (energy) // energy back for the enemies the leap and the strike hit, at most 80%
                *energy = (std::max)(*energy, (std::min)(80, 20 + (s.leapHits + hits) * 5));
        }
        else if (vel[2] > -1600.0f)
            vel[2] = -1600.0f;
    }
    if (energy && s.phase == 0 && *(int*)((unsigned char*)weapon + WPN_LSG_STATE) != 0)
        SfLsgSetState(weapon, 0);
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


// The user's video ("CSN:S Review Giga Break & Giga Break LE", training room against a zombie target, 2026-11-21):
// basic shots ~865 each (bullet + blast), and after a run of shots a bigger release (~1290); right click = melee swings
// (~96 each), the combo ends with a wide energy wave and the weapon changes (Overflow-Alpha, ~4 s); in Overflow the
// right click dashes into the target behind a hexagon shield (Overflow Crash, ~1920) and the left click hits one far
// target very hard (Overflow Burst, ~17000). The special ammunition gauge (bottom right) fills ~3.3%/s (30 s).
// Zombie numbers from the video; scenario x3.5 and the others x0.05 (the game's 2400 / 680 / 32 for a bullet).
// The rest is the game's own (mp.dll, 2026-11-21): SecondaryAttack 0x10ba0ff0 plays shootB1 (2) or, pressed again in
// time, shootB2 (3) and sets +0x64 = 2/3 with an end time +0x1dc; when it ends 0x10ba1570 sends shootB1's event (the
// wave) or, after shootB2, starts Overflow-Alpha (+0x1c0 = 1, +0x1cc its length). 0x10bab850 (every frame) marks the
// weapon charged (pev +0x120) when the special ammunition (+0x168, vfunc 0x2ec) reaches its prop value (+0x204 prop,
// +0x230) and, with both buttons down (player +0xec4 & 0x801), starts Overflow-Beta (+0x1c0 = 2) and spends it. In
// Overflow the right click sets +0x178 = 1 (Crash), 0x10baba70 then pulls the player down and on landing sets 3 and
// plays the end. What the server code doesn't have any more: the charging, every damage, the Crash's jump. The
// user (2026-11-21): in the video's first 7 s it's only shots and a reload.
static const float GB_BETA_FULL_S = 30.0f;
static const int GB_RELEASE_EVERY = 2, WPN_GB_SPECIAL = 0x168, WPN_GB_CRASH = 0x178, WPN_PROP = 0x204,
                 PROP_GB_SPECIAL_MAX = 0x230;
static const DWORD MP_PROP_GET = 0x106444d0; // float thiscall(prop value, 0)
typedef float(__fastcall* PropGetFn)(void* value, void* edx, int index);

// The Giga Break's effects are client.dll's own: its event handler (0x2564420, event_args +0x30 fparam1, +0x40 bparam1,
// +0x44 bparam2 = which effect) plays them with the game's sprites, models and sounds: 0 the basic shot (beamgun-1,
// the client's bullets; bparam1 1 = the hexagon hit ef_beamgun_shootA_hit.spr + beamgun-1_exp1 at fparam1 units along
// the aim, 4 = the charged hit), 1 shootB1's wave, 2 shootB2's black hole (ef_beamgun_shootB2_exp/landx, exp2 sprite,
// beamgun-2, at fparam1), 3 the Overflow Burst (charge_shootA + chargeshootAhit sprites, beamgun-4/-4_exp, at fparam1),
// 4 the Crash's jump (1) and landing (3), 5 the Wingman's shots (targets' entity indexes in iparam1, iparam2, fparam1,
// fparam2; beamgun-5), 6 Overflow-Beta's aura, 7 the Overflow Shield (ef_beamgun_skill). The server sends them with
// PLAYBACK_EVENT_FULL ([0x121d3c5c], the weapon's event index +0x218); the shots' sending was in the server code that is
// gone, so botassist sends 0, 2, 3, 5 and 7 (1, 4 and 6 the game still sends). The user (2026-11-21): "every two shots
// or so it blows up on the target with a hexagonal sprite opening".
static const DWORD MP_PLAYBACK_EVENT = 0x121d3c5c, MP_VEC3_ORIGIN = 0x121d3a3c;
static const int WPN_EVENT = 0x218;
enum { GBEV_SHOT = 0, GBEV_BLACKHOLE = 2, GBEV_BURST = 3, GBEV_WINGMAN = 5, GBEV_SHIELD = 7 };
typedef void (*PlaybackEventFn)(int flags, void* invoker, unsigned short ev, float delay, const float* origin,
                                const float* angles, float f1, float f2, int i1, int i2, int b1, int b2);

static void SfGbEvent(void* player, void* weapon, int which, float f1, float f2, int i1, int i2, int b1)
{
    PlaybackEventFn fn = *(PlaybackEventFn*)(MP_PLAYBACK_EVENT + g_mpDelta);
    unsigned short ev = weapon ? *(unsigned short*)((unsigned char*)weapon + WPN_EVENT) : 0;
    if (!fn || !ev)
        return;
    const float* zero = (const float*)(MP_VEC3_ORIGIN + g_mpDelta);
    fn(0 /* everyone, the shooter too: his client doesn't play these by itself */, player, ev, 0.0f, zero, zero, f1, f2,
       i1, i2, b1, which);
}

static float SfGbSpecialMax(void* weapon)
{
    unsigned char* prop = *(unsigned char**)((unsigned char*)weapon + WPN_PROP);
    if (!prop || !Readable(prop + PROP_GB_SPECIAL_MAX, 8))
        return 0;
    return ((PropGetFn)(MP_PROP_GET + g_mpDelta))(prop + PROP_GB_SPECIAL_MAX, nullptr, 0);
}

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
        float dist = Len(Vec(hit) - Eye(player));
        s.shotRun = now - s.lastShot < 1.0f ? s.shotRun + 1 : 1;
        s.lastShot = now;
        // consecutive basic attacks release energy: every second shot in a row the hexagon blows up on the target
        bool release = s.shotRun % GB_RELEASE_EVERY == 0;
        SfGbEvent(player, weapon, GBEV_SHOT, dist, 0, 0, 0, release ? 1 : 0);
        unsigned char* pv = Vars(player);
        float take = F<float>(pv, EV_TAKEDAMAGE);
        F<float>(pv, EV_TAKEDAMAGE) = 0; // (the game's own explosion sprite comes with the event: no TE_EXPLOSION)
        ((RadiusDamageFn)(MP_RADIUS_DAMAGE + g_mpDelta))(hit[0], hit[1], hit[2], pv, pv, SfGb(185, 10), 70.0f, 0,
                                                         0x1000000);
        F<float>(pv, EV_TAKEDAMAGE) = take;
        if (release)
        {
            ForEntitiesNear(Vec(hit), 140.0f, [&](void* e) {
                if (SfEnemy(e, player))
                    SfKnock(e, Vec(hit), 250.0f, 120.0f);
            });
            F<float>(pv, EV_TAKEDAMAGE) = 0;
            ((RadiusDamageFn)(MP_RADIUS_DAMAGE + g_mpDelta))(hit[0], hit[1], hit[2], pv, pv, SfGb(425, 25), 140.0f, 0,
                                                             0x1000000);
            F<float>(pv, EV_TAKEDAMAGE) = take;
        }
    }
    else if (burst && GunBullet(weapon, 0.0f, (int)SfGb(2000, 60), 0, BULLET_762, hit))
    {
        SfGbEvent(player, weapon, GBEV_BURST, Len(Vec(hit) - Eye(player)), 0, 0, 0, 0);
        SfArea(Vec(hit), player, SfGb(15000, 220), 140.0f, 500.0f, 200.0f, 0); // Overflow Burst (its own sprites)
    }
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
        SfArea(eye + aim * d, player, SfGb(300, 25) / 2, 90.0f, 350.0f, 120.0f, 0); // (the wave is the game's effect)
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
        // a swing: what's in front of the player; the first one's wave follows (shootB1's event), the second one in
        // zombie modes and Super Soldier leaves a black hole (the game starts Overflow-Alpha by itself after it)
        V3 front = Eye(player) + SfAim(player) * 80.0f;
        SfArea(front, player, SfGb(96, 35), 120.0f, 250.0f, 100.0f, 0);
        if (anim == 2)
            s.waveAt = now + 0.45f;
        else if (SfBlackHoleMode())
        {
            g_sfPulls.push_back({ Eye(player) + SfAim(player) * 250.0f, player, now + 1.5f });
            SfGbEvent(player, weapon, GBEV_BLACKHOLE, 250.0f, 0, 0, 0, 0);
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
        vel[0] = aim.x * 900.0f;
        vel[1] = aim.y * 900.0f;
        vel[2] = 330.0f; // the jump; the game pulls him down and lands it (+0x178 = 3)
        s.crashUntil = now + 3.0f;
        s.invulnUntil = now + 3.0f;
    }
}

// The Wingman's drone: the original server put it next to the player (client.dll only shoots from it, event 5);
// the user (2026-11-21) wanted it seen. An info_target with the game's models/ef_beamgun_wingman.mdl (precached with
// the drums in grenade_fixes.inl, so only on maps loaded since botassist hooked mp.dll), flying (MOVETYPE_NOCLIP,
// not solid) to a spot beside the player's head, facing where he looks; removed when the Giga Break isn't in hand.
static const int SF_EV_MOVETYPE = 0x118, SF_MOVETYPE_NOCLIP = 8;

// Giga Break or Giga Break LE (weapon_beamgunle: CBeamGunLe shares every function of CBeamGun but its precache, its
// prop and its Wingman model; client.dll picks the LE's sprites and models itself, InGameID 0x2d6)
static bool SfIsGb(const char* cls)
{
    return !strcmp(cls, "weapon_beamgun") || !strcmp(cls, "weapon_beamgunle");
}

static bool SfDroneAlive(SfPlayer& s)
{
    return s.droneEdict && *(int*)s.droneEdict == 0 && PrivateOf(s.droneEdict) == s.drone;
}

static void SfDroneRemove(SfPlayer& s)
{
    if (SfDroneAlive(s))
        F<int>(Vars(s.droneEdict), EV_FLAGS) |= SF_FL_KILLME;
    s.drone = s.droneEdict = nullptr;
}

static void SfDroneFrame(void* player, SfPlayer& s, bool want, bool le)
{
    std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    if (!want || g_drumsMap.empty() || map != g_drumsMap)
    {
        SfDroneRemove(s);
        return;
    }
    if (SfDroneAlive(s) && s.droneLe != le)
        SfDroneRemove(s); // switched between the Giga Break and its LE: the other drone
    if (!SfDroneAlive(s))
    {
        static const char kClass[] = "info_target";
        const char* base = *(const char**)((DWORD)*g_pGlobals + GV_STRINGBASE);
        void* edict = ((CreateNamedFn)(MP_CREATE_NAMED + g_mpDelta))((int)(kClass - base), 0);
        void* priv = edict ? PrivateOf(edict) : nullptr;
        if (!priv)
            return;
        unsigned char* v = Vars(edict);
        float* o = (float*)(v + EV_ORIGIN);
        V3 at = Eye(player);
        o[0] = at.x, o[1] = at.y, o[2] = at.z;
        (*(SfSetModelFn*)(MP_SET_MODEL + g_mpDelta))(edict, le ? "models/ef_beamgunle_wingman.mdl"
                                                               : "models/ef_beamgun_wingman.mdl");
        s.droneLe = le;
        F<int>(v, SF_EV_MOVETYPE) = SF_MOVETYPE_NOCLIP;
        F<int>(v, EV_SOLID_OFS) = 0;
        s.drone = priv;
        s.droneEdict = edict;
    }
    // beside the head (the side the client shoots from alternates; the drone stays on the right)
    unsigned char* v = Vars(s.droneEdict);
    float yaw = F<float>(Vars(player), EV_VANGLE + 4) / RAD;
    V3 right = { sinf(yaw), -cosf(yaw), 0 };
    V3 want3 = Eye(player) + right * 28.0f + V3{ 0, 0, 14 };
    V3 cur = Origin(s.droneEdict);
    V3 d = want3 - cur;
    float* vel = (float*)(v + EV_VELOCITY);
    if (Len(d) > 300.0f) // left behind (teleport, respawn): straight there
    {
        float* o = (float*)(v + EV_ORIGIN);
        o[0] = want3.x, o[1] = want3.y, o[2] = want3.z;
        vel[0] = vel[1] = vel[2] = 0;
    }
    else
    {
        vel[0] = d.x * 12.0f;
        vel[1] = d.y * 12.0f;
        vel[2] = d.z * 12.0f;
    }
    float* ang = (float*)(v + EV_ANGLES);
    ang[0] = 0;
    ang[1] = F<float>(Vars(player), EV_VANGLE + 4);
    ang[2] = 0;
}

static void SfBgFrame(void* player, SfPlayer& s, void* weapon, bool active, int pressed, float now, float dt)
{
    if (!active)
        return;
    unsigned char* w = (unsigned char*)weapon;
    int state = *(int*)(w + WPN_STATE_GB);
    s.heldSecs += dt;
    // the Overflow's countdown (+0x1cc, set by the game to its length when it starts): kept with the clock, so it
    // ends whether or not something counts it down; the game ends the state itself when it goes below 0
    if (state != 0 && s.overflowUntil == 0)
        s.overflowUntil = now + (std::max)(0.5f, *(float*)(w + WPN_STATE_END_GB));
    else if (state == 0)
        s.overflowUntil = 0;
    if (s.overflowUntil > 0)
        *(float*)(w + WPN_STATE_END_GB) = s.overflowUntil - now;
    // the special ammunition charges (its full value is the weapon's prop); full, the game shows it charged and both
    // buttons start Overflow-Beta
    if (state == 0)
    {
        float full = SfGbSpecialMax(weapon);
        int* special = (int*)(w + WPN_GB_SPECIAL);
        if (full > 0 && *special < (int)full)
        {
            s.beta += dt * full / GB_BETA_FULL_S;
            int add = (int)s.beta;
            if (add > 0)
            {
                s.beta -= add;
                *special = (std::min)((int)full, *special + add);
                if (*special >= (int)full && !s.betaTold)
                {
                    s.betaTold = true;
                    SfSay(player, "Giga Break: special ammunition charged - both mouse buttons: Overflow-Beta");
                }
            }
        }
        else if (full > 0 && *special < (int)full / 2)
            s.betaTold = false;
    }
    // the energy wave of the first swing
    if (s.waveAt && now >= s.waveAt)
    {
        s.waveAt = 0;
        SfBgWave(player);
    }
    // Overflow Crash: the game lands it (+0x178 3 or back to 0): the crash
    int crash = *(int*)(w + WPN_GB_CRASH);
    if (s.crashUntil && (crash == 3 || crash == 0 || now > s.crashUntil))
    {
        s.crashUntil = 0;
        SfArea(Origin(player) + SfAim(player) * 40.0f - V3{ 0, 0, 20 }, player, SfGb(1920, 90), 200.0f, 700.0f, 250.0f, 0);
        s.invulnUntil = now + 0.5f;
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
        int idx[2] = { 0, 0 };
        for (size_t k = 0; k < targets.size() && k < 2; k++)
            idx[k] = g_IndexOfEdict ? g_IndexOfEdict(targets[k].second) : 0;
        if (idx[0])
            SfGbEvent(player, weapon, GBEV_WINGMAN, 0, 0, idx[0], idx[1], 0); // the Wingman's own shots
        for (size_t k = 0; k < targets.size() && k < 2; k++)
        {
            V3 c = Center(targets[k].second);
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
        {
            if (s.droneEdict)
                SfDroneRemove(s); // (the player left)
            continue;
        }
        float dt = s.lastFrame > 0 && now > s.lastFrame && now - s.lastFrame < 1.0f ? now - s.lastFrame : 0.0f;
        s.lastFrame = now;
        if (!Alive(e))
        {
            if (s.phase >= 2)
                F<float>(Vars(e), SF_EV_GRAVITY) = 1.0f; // (hovering or falling: the gravity back)
            s.phase = 0;
            s.blastWeapon = nullptr;
            SfDroneRemove(s);
            continue;
        }
        int buttons = F<int>(Vars(e), SF_EV_BUTTON);
        int pressed = buttons & ~s.buttons;
        s.buttons = buttons;
        void* weapon = nullptr;
        const char* cls = SfActiveClass(e, &weapon);
        __try
        {
            SfLsgFrame(e, s, weapon, SfIsLsg(cls), pressed, now, dt);
            SfBgFrame(e, s, weapon, SfIsGb(cls), pressed, now, dt);
            SfDroneFrame(e, s, SfIsGb(cls) && SfWingmanMode(), !strcmp(cls, "weapon_beamgunle"));
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
        SfFxFrame(now);
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
            SfArea(Origin(victim), victim, ByMode(60, 1000, 1500), 250.0f, 900.0f, 250.0f, 0); // (the shield's own effect: event 7)
            void* w = nullptr;
            if (SfIsGb(SfActiveClass(victim, &w)))
                SfGbEvent(victim, w, GBEV_SHIELD, 0, 0, 0, 0, 0);
            SfSay(victim, "Giga Break: Overflow Shield!");
            return 0;
        }
    }
    void* attacker = attackerPev ? *(void**)((unsigned char*)attackerPev + EV_CONTAINING_ENTITY) : nullptr;
    int ai = attacker && (F<int>((unsigned char*)attackerPev, EV_FLAGS) & FL_CLIENT) ? SfIndex(attacker) : 0;
    if (ai && attacker != victim && damage > 0 && SfIsGb(SfActiveClass(attacker, nullptr)))
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
