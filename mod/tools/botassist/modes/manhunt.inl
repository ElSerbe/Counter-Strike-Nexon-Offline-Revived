// botassist: Manhunt (mode 51, ZHE). Included by botassist.cpp at this point (one translation unit: it uses the shared helpers
// defined before it). Only runs in its own mode: DriveZheBot is called only when GameMode() == MODE_ZHE.

// ---------------------------------------------------------------- Manhunt (mode 51, ZHE) with room bots
// One random player is the zombie (CSOGameRulesZHE 0x107a6290 shuffles every living client, bots included, and marks
// CBasePlayer +0x2167; the zombie then has +0x2166). The humans repair the map's generators (esc_generator,
// CZHEGenerator: one of the 3 spots of each of the 7 groups is used) and run to an exit (esc_exitzone, its touch
// function is set once enough generators work). The stock bot AI knows none of it, so botassist drives every bot:
// - humans repair (hold +use looking at a generator; let go while it overheats, state 5, or it blows: state 6),
//   run from the zombie when it comes, pick up a teammate who is down (hold +use looking at them), heal themselves
//   when hurt and safe (+use looking at the floor), and run to an open exit;
// - the zombie hunts the humans it can see or hear, else the generators being repaired, and hits a generator that
//   has progress when nobody is around. Humans who are down are left alone (the mode's own rule).
// Facts (mp.dll, preferred base): CBasePlayer module by id 0x1087f310 (thiscall player, id; 9 = CZHEHumanModule,
// state at +0x18: 0 healthy, 1 / 2 hurt, 3 down (dies after 180 s unless picked up), 5 escaped);
// CZHEGenerator +0x1bc state (0..3 progress, 4 repaired, 5 overheating: stop, 6 blown), +0x190 how many repair it,
// +0x1c4 gauge. The exit zones (esc_exitzone) always let humans out: the map's doors (func_door exit_door1..4) block
// them until the rules fire ms_exit_active (multisource, subtotal 5), which opens two exits at random.
static const DWORD MP_PLAYER_MODULE = 0x1087f310;
static const int ZHE_IS_ZOMBIE = 0x2166, ZHE_HUMAN_MODULE = 9, ZHE_HUMAN_STATE = 0x18;
static const int ZHE_GEN_STATE = 0x1bc, ZHE_GEN_USERS = 0x190, ZHE_GEN_GAUGE = 0x1c4;
static const int EV_EFFECTS = 0x128, EF_NODRAW = 0x80, EV_TARGETNAME = 0x1fc, ZHE_ENT_TOUCH = 0x50;
static const int ZHE_DOWN = 3, ZHE_ESCAPED = 5, ZHE_ZOMBIE = 100;
typedef void*(__thiscall* PlayerModuleFn)(void* player, int id);

struct ZheGen
{
    void* e;
    V3 pos;
    int state, users;
    float gauge;
    bool active;
};
struct ZheExit
{
    void* e;
    V3 pos;
    bool open;
};
struct ZhePlayer
{
    void* e;
    V3 pos;
    int state; // ZheState
};
struct ZheBox
{
    void* e;
    V3 pos;
};
static struct
{
    int level = -1;
    float nextScan = 0;
    std::vector<ZheGen> gens;
    std::vector<ZheExit> exits;
    std::vector<ZhePlayer> players;
    std::vector<ZheBox> boxes; // item boxes (customwb) not opened yet
    std::vector<V3> spots; // every generator spot and exit: places to run to
    bool reported = false;
    bool anyOpen = false, openSaid = false;
    int repaired = -1;
} g_zhe;

enum ZheZombieMode
{
    ZZ_PATROL,      // walking around near the humans, looking around
    ZZ_INVESTIGATE, // a generator made noise: going there
    ZZ_CHASE,       // a human in sight (or just lost): after them
    ZZ_SEARCH,      // lost them: where they were last seen, looking around, then gives up
};

struct ZheBot
{
    float lastThink = 0, yaw = 0, pitch = 0;
    bool init = false;
    float sweep = 0;                // looking around while walking: amplitude in degrees
    void* gen = nullptr;            // generator it repairs
    float lastGauge = -1, gaugeCheck = 0, nudgeUntil = 0, nudgeYaw = 0;
    float fleeUntil = 0;
    V3 fleeGoal{};
    float zombieSeenAt = -100;
    V3 zombiePos{};
    float hideUntil = 0, nextJump = 0;
    void* useTarget = nullptr;      // what +use is held on (reported once when it doesn't take)
    bool useReported = false;
    float useDownAt = 0, useReleaseUntil = 0; // +use pressed since (0: up); let go until (to press it again)
    int useRetries = 0;
    void* helping = nullptr;        // the teammate it goes to pick up (said once)
    void* box = nullptr;            // item box it walks to
    float boxUntil = 0;
    std::vector<void*> boxesTried;
    float nextDrop = 0, nextSwitch = 0, nextThrow = 0, throwAt = 0, nextInvReport = 0;
    int throwStage = 0;             // 1 taking the grenade out, 2 holding the pin, 3 thrown
    void* throwItem = nullptr;
    int throwClass = 0;
    float throwReadyAt = 0;
    bool fireToggle = false;
    bool fleeing = false;           // on the way to somewhere out of the zombie's sight
    float gunUntil = 0;             // the gun out until (else the knife)
    // the zombie
    int zmode = ZZ_PATROL;
    void* target = nullptr;         // the human it chases
    V3 lastSeen{};
    float lastSeenAt = -100, seenSince = -1, giveUpAt = 0, lookAroundUntil = 0, nextAttack = 0, aimError = 0;
    float nextTrack = 0;            // next time it picks up their trail while searching
    V3 via{};                       // a detour on the way (it doesn't always take the shortest route)
    bool hasVia = false;
    V3 patrolGoal{};
    float patrolUntil = 0, nextAlertCheck = 0, lookBase = 0, lookStart = 0;
    float speedScale = 1;           // walks while patrolling
    float patrolSpeed = 0.75f;
    V3 leaveFrom{};                 // where it just downed someone: patrols away from there until leaveUntil
    float leaveUntil = 0;
    void* hitGen = nullptr;         // generator it investigates / smashes
    float hitUntil = 0;
    std::unordered_map<void*, float> genDoneUntil; // generators it just dealt with
    // movement
    V3 stuckFrom{};
    float stuckCheck = 0, detourUntil = 0, detourYaw = 0;
    int stuckCount = 0;
    float nextReport = 0;
    const char* doing = "";
    int lastState = -2;
};
static std::unordered_map<void*, ZheBot> g_zheBots;

// ZheState: CZHEHumanModule state, ZHE_ZOMBIE for the zombie, -1 unknown (no module yet)
static int ZheState(void* edict)
{
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(edict);
        if (!pl)
            return -1;
        if (pl[ZHE_IS_ZOMBIE])
            return ZHE_ZOMBIE;
        unsigned char* mod = (unsigned char*)((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(pl, ZHE_HUMAN_MODULE);
        return mod ? *(int*)(mod + ZHE_HUMAN_STATE) : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static bool ZheStanding(int state) { return state >= 0 && state < ZHE_DOWN; }

static void ZheScan(float now)
{
    if (g_zhe.level != g_nav.level)
    {
        g_zhe = {};
        g_zhe.level = g_nav.level;
        g_zheBots.clear();
    }
    if (now < g_zhe.nextScan && now > g_zhe.nextScan - 2.0f)
        return;
    g_zhe.nextScan = now + 0.3f;
    g_zhe.gens.clear();
    g_zhe.exits.clear();
    g_zhe.players.clear();
    g_zhe.boxes.clear();
    bool spots = g_zhe.spots.empty();
    std::vector<V3> openDoors, doors;
    ForEntitiesNear({ 0, 0, 0 }, 32000.0f, [&](void* e) {
        unsigned char* v = Vars(e);
        const char* cls = ClassName(e);
        if (!cls)
            return;
        if (!strcmp(cls, "esc_generator"))
        {
            unsigned char* p = (unsigned char*)PrivateOf(e);
            if (!p)
                return;
            // the spots not used this round stay hidden (EF_NODRAW from the generator's spawn)
            ZheGen g{ e, Origin(e), *(int*)(p + ZHE_GEN_STATE), *(int*)(p + ZHE_GEN_USERS), *(float*)(p + ZHE_GEN_GAUGE),
                      !(F<int>(v, EV_EFFECTS) & EF_NODRAW) };
            g_zhe.gens.push_back(g);
            if (spots)
                g_zhe.spots.push_back(g.pos);
        }
        else if (!strcmp(cls, "esc_exitzone"))
        {
            V3 c = Center(e);
            c.z = Vec((float*)(v + EV_ABSMIN)).z + 36.0f;
            g_zhe.exits.push_back({ e, c, false });
            if (spots)
                g_zhe.spots.push_back(c);
        }
        else if (!strcmp(cls, "customwb"))
        {
            // an item box (resource/ZHE/zheweaponbox_N.json: a gun, a grenade, a toolbox or nothing), opened by
            // walking into it (CCustomWeaponBox::CustomWBTouch 0x108e9bb0); once opened it is hidden, its touch gone
            unsigned char* p = (unsigned char*)PrivateOf(e);
            if (p && !(F<int>(v, EV_EFFECTS) & EF_NODRAW) && *(void**)(p + ZHE_ENT_TOUCH))
                g_zhe.boxes.push_back({ e, Origin(e) });
        }
        else if (!strcmp(cls, "func_door") && !strncmp(Str(F<int>(v, EV_TARGETNAME)), "exit_door", 9))
        {
            // a brush door's origin is its offset from where the map placed it: moved = open
            doors.push_back(Center(e));
            if (Len(Origin(e)) > 8.0f)
                openDoors.push_back(Center(e));
        }
        else if ((F<int>(v, EV_FLAGS) & FL_CLIENT) && Alive(e))
            g_zhe.players.push_back({ e, Origin(e), ZheState(e) });
    });
    // each exit zone belongs to its nearest door
    for (auto& x : g_zhe.exits)
    {
        float best = 1e9f;
        bool open = false;
        for (auto& d : doors)
            if (Len(d - x.pos) < best)
                best = Len(d - x.pos);
        for (auto& d : openDoors)
            open |= Len(d - x.pos) <= best + 1.0f;
        x.open = open;
    }
    int active = 0, repaired = 0;
    for (auto& g : g_zhe.gens)
    {
        active += g.active;
        repaired += g.active && g.state == 4;
    }
    bool open = false;
    for (auto& x : g_zhe.exits)
        open |= x.open;
    if (!active)
        g_zhe.reported = false; // between rounds every spot is hidden
    else if (!g_zhe.reported)
    {
        g_zhe.reported = true;
        Log("Manhunt: %d generator spots (%d in use), %d exits, %d players", (int)g_zhe.gens.size(), active,
            (int)g_zhe.exits.size(), (int)g_zhe.players.size());
        for (auto& g : g_zhe.gens)
            DLog("Manhunt generator at %.0f %.0f %.0f: state %d, gauge %.1f, users %d, effects 0x%x, solid %d", g.pos.x,
                 g.pos.y, g.pos.z, g.state, g.gauge, g.users, F<int>(Vars(g.e), EV_EFFECTS), F<int>(Vars(g.e), EV_SOLID));
        for (auto& p : g_zhe.players)
            Log("Manhunt: '%s' is %s", String(F<int>(Vars(p.e), EV_NETNAME)),
                p.state == ZHE_ZOMBIE ? "the zombie" : p.state < 0 ? "not in the game yet" : "a human");
    }
    if (repaired != g_zhe.repaired)
    {
        if (g_zhe.repaired >= 0)
            Log("Manhunt: %d of %d generators repaired", repaired, active);
        g_zhe.repaired = repaired;
    }
    if (open != g_zhe.anyOpen)
    {
        g_zhe.anyOpen = open;
        Log("Manhunt: the exits are %s", open ? "open" : "closed");
    }
}

static const ZhePlayer* ZheZombieNear(V3 me)
{
    const ZhePlayer* best = nullptr;
    for (auto& p : g_zhe.players)
        if (p.state == ZHE_ZOMBIE && (!best || Len(p.pos - me) < Len(best->pos - me)))
            best = &p;
    return best;
}

// a nav area centre between minR and maxR (2D) from `around`, about its height
static bool ZheRandomArea(V3 around, float minR, float maxR, V3& out)
{
    if (!g_nav.ok || g_nav.areas.empty())
        return false;
    for (int tries = 0; tries < 80; tries++)
    {
        const NavArea& a = g_nav.areas[rand() % g_nav.areas.size()];
        float d = Len2D(a.center - around);
        if (d >= minR && d <= maxR && fabsf(a.center.z - around.z) < 200.0f)
        {
            out = a.center;
            return true;
        }
    }
    return false;
}

// a point off to the side of the straight way there: the long way round
static bool ZheDetour(V3 me, V3 goal, V3& via)
{
    float len = Len2D(goal - me);
    return ZheRandomArea((me + goal) * 0.5f, len * 0.3f, len * 0.7f, via);
}

// somewhere to run to: away from the zombie, the way there not past it, best out of its sight (a building, a wall in
// between) and not too far
static V3 ZheFleeGoal(void* bot, V3 me, V3 zombie)
{
    V3 away = me - zombie;
    float al = Len2D(away) > 1 ? Len2D(away) : 1;
    V3 best = me + away * (600.0f / al);
    float bestScore = -1e30f;
    V3 zeye = zombie + V3{ 0, 0, 28 };
    TraceResult tr;
    for (int i = 0; i < 60; i++)
    {
        V3 s;
        if (!ZheRandomArea(me, 300.0f, 1400.0f, s))
            break;
        V3 d = s - me;
        float dl = Len2D(d);
        float dot = (d.x * away.x + d.y * away.y) / (dl * al);
        // the straight way there: how close it passes the zombie
        float t = ((zombie.x - me.x) * d.x + (zombie.y - me.y) * d.y) / (dl * dl);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float pass = Len2D(me + d * t - zombie);
        float zd = Len2D(s - zombie);
        float score = (zd < 1500.0f ? zd : 1500.0f) - 0.5f * dl + dot * 250.0f;
        if (dot < -0.25f)
            score -= 2500.0f; // back towards it: only when nothing else
        if (pass < 250.0f && pass < al * 0.8f)
            score -= 2000.0f; // past it
        if (!Trace(zeye, s + V3{ 0, 0, 28 }, bot, 1, tr))
            score += 1500.0f; // out of its sight
        if (score > bestScore)
        {
            bestScore = score;
            best = s;
        }
    }
    return best;
}

// the generator to repair: close, not near the zombie, a teammate there is a plus (repairs go faster together)
static void* ZhePickGen(void* bot, V3 me, V3 zombie, bool zombieKnown)
{
    void* best = nullptr;
    float bestScore = 1e30f;
    for (auto& g : g_zhe.gens)
    {
        if (!g.active || g.state == 4)
            continue;
        float score = Len(g.pos - me);
        int mates = 0;
        for (auto& b : g_zheBots)
            if (b.first != bot && b.second.gen == g.e)
                mates++;
        score += mates * 500.0f - g.users * 200.0f - g.gauge * 2.0f;
        if (zombieKnown && Len2D(zombie - g.pos) < 700.0f)
            score += 2000.0f;
        if (score < bestScore)
        {
            bestScore = score;
            best = g.e;
        }
    }
    return best;
}

static const ZheGen* ZheGenOf(void* e)
{
    for (auto& g : g_zhe.gens)
        if (g.e == e)
            return &g;
    return nullptr;
}

// Using things the way the game checks it:
// - CBasePlayer::PlayerUse (0x114c4ea0) takes, among the usable entities within 64 u of the player's origin (their
//   box), the one the view (v_angle) points at best from the eyes: aim at the middle of its box;
// - while +use stays down, the generator (0x108e9380) and the human module (0x10803140, a teammate hurt or down) keep
//   the player only with no attack button and when 0x11516f90 passes: the target's position (vfunc 0x128) seen from
//   the player's, normalised in 3D, then only x/y against the model's facing (pev->angles), within the cone
//   *(float*)[0x11e49458] degrees. Too close to something lower (a teammate down) fails it: step back a little.
static const DWORD MP_ZHE_USE_CONE = 0x11e49458;
typedef float*(__thiscall* EntVecFn)(void* self);

static bool ZheUsePoint(void* edict, V3& out)
{
    __try
    {
        void* p = PrivateOf(edict);
        if (!p)
            return false;
        float* v = ((EntVecFn)(*(void***)p)[0x128 / 4])(p);
        if (!v)
            return false;
        out = Vec(v);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static float ZheUseConeDeg()
{
    __try
    {
        float* p = *(float**)(MP_ZHE_USE_CONE + g_mpDelta);
        if (p && *p >= 1.0f && *p <= 180.0f)
            return *p;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 45.0f;
}

// from p to the closest point of e's box
static float ZheBoxDist(V3 p, void* e)
{
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    V3 c{ p.x < lo.x ? lo.x : p.x > hi.x ? hi.x : p.x, p.y < lo.y ? lo.y : p.y > hi.y ? hi.y : p.y,
          p.z < lo.z ? lo.z : p.z > hi.z ? hi.z : p.z };
    return Len(p - c);
}

// x/y share of the way from the player's use point to the target's (the cone test without the facing)
static float ZheUseRatio(void* bot, void* target)
{
    V3 a, t;
    if (!ZheUsePoint(bot, a) || !ZheUsePoint(target, t))
        return 1.0f;
    V3 d = t - a;
    float l = Len(d);
    return l > 1 ? Len2D(d) / l : 1.0f;
}

// hold +use on target (a generator, a teammate): gets in reach, aims at the middle of its box, steps back when too
// close for the cone test; false while still on the way
static bool ZheUseOn(void* bot, ZheBot& z, void* target, V3 me, float now, V3& goal, bool& hasGoal, float& arrive,
                     bool& act, V3& lookAt, unsigned short& b)
{
    V3 c = Center(target);
    float reach = ZheBoxDist(me, target);
    if (z.useTarget != target)
    {
        z.useTarget = target;
        z.useReported = false;
        z.useDownAt = 0;
        z.useRetries = 0;
    }
    act = true;
    lookAt = c;
    if (reach > 52.0f)
    {
        goal = c;
        hasGoal = true;
        arrive = Len2D(c - me) - reach + 40.0f;
        if (arrive < 12.0f)
            arrive = 12.0f;
        return false;
    }
    float need = cosf(ZheUseConeDeg() / RAD) + 0.05f;
    if (need < 1.0f && reach < 40.0f && ZheUseRatio(bot, target) < need)
    {
        V3 away = me - c;
        away.z = 0;
        float al = Len2D(away);
        away = al > 1 ? away * (1.0f / al) : V3{ -cosf(z.yaw / RAD), -sinf(z.yaw / RAD), 0 };
        goal = me + away * 48.0f;
        hasGoal = true;
        arrive = 8;
    }
    // the press counts only when aimed (PlayerUse picks what the view points at as +use goes down): aim first,
    // and after a let-go (to press again) wait
    V3 d = c - Eye(bot);
    bool aimed = fabsf(AngleDiff(YawOf(d), z.yaw)) < 7.0f && fabsf(PitchOf(d) - z.pitch) < 10.0f;
    if (!aimed || now < z.useReleaseUntil)
    {
        z.useDownAt = 0;
        return true;
    }
    if (z.useDownAt == 0)
        z.useDownAt = now;
    b |= IN_USE;
    return true;
}

// +use held on something that doesn't take it: the numbers, once per target
static void ZheUseReport(void* bot, ZheBot& z, void* target, const char* what, int users)
{
    if (z.useReported)
        return;
    z.useReported = true;
    unsigned char* v = Vars(bot);
    DLog("Manhunt bot '%s': +use on the %s does nothing: box %.0f u away, x/y share %.2f (cone %.0f deg needs %.2f), "
         "users %d, view %.0f %.0f, model %.0f %.0f",
         String(F<int>(v, EV_NETNAME)), what, ZheBoxDist(Origin(bot), target), ZheUseRatio(bot, target), ZheUseConeDeg(),
         cosf(ZheUseConeDeg() / RAD), users, F<float>(v, EV_VANGLE), F<float>(v, EV_VANGLE + 4), F<float>(v, EV_ANGLES),
         F<float>(v, EV_ANGLES + 4));
}

// Radio: the stock AI's chatter is off in Manhunt (bot_chatter off, its radio commands dropped in OnBotCommand);
// the bots say what fits: the zombie spotted, taking hits, down, going to help, a generator done, the exits open.
// One message every 2.5 s at most, from all the bots together.
static float g_zheRadioAt = -100;
static void ZheRadio(void* bot, int menu, int item, const char* what)
{
    float now = Now();
    if (now - g_zheRadioAt < 2.5f && now >= g_zheRadioAt)
        return;
    g_zheRadioAt = now;
    char m[8], i[4];
    snprintf(m, sizeof(m), "radio%d", menu);
    snprintf(i, sizeof(i), "%d", item);
    FakeCommand(bot, m);
    FakeCommand(bot, "menuselect", i);
    DLog("Manhunt bot '%s' radio: %s", String(F<int>(Vars(bot), EV_NETNAME)), what);
}

// The player's items (CBasePlayer::DropPlayerItem 0x114b4e60): CSNZ keeps entity pointers as handles of 8 bytes
// {control block, pointer}, valid while the block's second dword is set. m_rgpPlayerItems: 6 slots 16 bytes apart from
// player +0xfb8 (slot 1 primary, 2 pistol, 3 melee, 4 grenades), each a chain through the item's m_pNext handle at
// +0xd8; the active item's handle at +0x1018. Weapon +0xe4 its id, +0x158 the ammo type, +0x160 the clip; the
// player's reserve per ammo type at +0x1034.
struct ZheItem
{
    void* w;
    int cls; // classname (string_t)
    int slot, type, clip, reserve;
};

static int ZheItemsRaw(void* bot, ZheItem* out, int max)
{
    int n = 0;
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(bot);
        if (!pl)
            return 0;
        for (int sl = 0; sl < 6; sl++)
        {
            unsigned char* it = ItemHandle(pl + PLAYER_ITEMS + 16 * sl);
            for (int guard = 0; it && guard < 8 && n < max; guard++)
            {
                ZheItem& i = out[n++];
                i.w = it;
                unsigned char* pev = *(unsigned char**)(it + 8);
                i.cls = pev && Readable(pev, 4) ? *(int*)pev : 0;
                i.slot = sl;
                i.type = *(int*)(it + 0x158);
                i.clip = *(int*)(it + 0x160);
                i.reserve = i.type > 0 && i.type < 0x102 ? *(int*)(pl + PLAYER_AMMO + 4 * i.type) : 0;
                it = ItemHandle(it + ITEM_NEXT);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return n;
}

static void* ZheActiveItem(void* bot)
{
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(bot);
        return pl ? ItemHandle(pl + PLAYER_ACTIVE_ITEM) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static bool ZheThrowable(const ZheItem& i)
{
    const char* c = String(i.cls);
    return i.slot == 4 || strstr(c, "grenade") || strstr(c, "flashbang") || strstr(c, "bomb");
}
static bool ZheGun(const ZheItem& i)
{
    return (i.slot == 1 || i.slot == 2) && i.type > 0 && i.type < 0x102 && i.clip >= 0 && !ZheThrowable(i);
}

// thrown away toward yaw, a little up (the drop flies along pev->angles' forward): off to the side behind, out of the
// way it runs, so it doesn't walk over it again; the weapon box also loses its touch (empty, nobody needs it)
static void ZheDropToward(void* bot, float yaw)
{
    std::vector<void*> before;
    V3 me = Origin(bot);
    ForEntitiesNear(me, 300.0f, [&](void* e) {
        const char* c = ClassName(e);
        if (c && !strcmp(c, "weaponbox"))
            before.push_back(e);
    });
    float* va = (float*)(Vars(bot) + EV_VANGLE);
    float* an = (float*)(Vars(bot) + EV_ANGLES);
    float saved[6] = { va[0], va[1], va[2], an[0], an[1], an[2] };
    va[0] = an[0] = -20.0f;
    va[1] = an[1] = AngleDiff(yaw, 0.0f);
    FakeCommand(bot, "cl_drop");
    memcpy(va, saved, sizeof(float) * 3);
    memcpy(an, saved + 3, sizeof(float) * 3);
    ForEntitiesNear(me, 300.0f, [&](void* e) {
        const char* c = ClassName(e);
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (c && p && !strcmp(c, "weaponbox") && std::find(before.begin(), before.end(), e) == before.end())
            *(void**)(p + ZHE_ENT_TOUCH) = nullptr;
    });
}

// weapons: an empty gun in hand is thrown away (there is no ammunition to be had), off to the side behind; with the
// zombie in sight and close a gun with ammunition shoots it (it slows down: ground to win), a grenade is thrown at it
// now and then (out, pin, throw); the rest of the time the knife is out (runs faster)
static void ZheFight(void* bot, ZheBot& z, const ZhePlayer* zom, bool inSight, V3 me, V3 eye, float now, bool& act,
                     V3& lookAt, unsigned short& b)
{
    ZheItem inv[16];
    int n = ZheItemsRaw(bot, inv, 16);
    if (!n)
        return;
    void* active = ZheActiveItem(bot);
    const ZheItem* hand = nullptr;
    const ZheItem* knife = nullptr;
    for (int k = 0; k < n; k++)
    {
        if (inv[k].w == active)
            hand = &inv[k];
        if (inv[k].slot == 3 && !knife)
            knife = &inv[k];
    }
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    if (now >= z.nextInvReport)
    {
        z.nextInvReport = now + 30.0f;
        char list[512] = "";
        for (int k = 0; k < n; k++)
        {
            char one[96];
            snprintf(one, sizeof(one), "%s%s (slot %d, clip %d, reserve %d)", k ? ", " : "", String(inv[k].cls),
                     inv[k].slot, inv[k].clip, inv[k].reserve);
            strncat(list, one, sizeof(list) - strlen(list) - 1);
        }
        DLog("Manhunt bot '%s' carries %s; in hand %s", name, list, hand ? String(hand->cls) : "nothing");
    }
    if (hand && ZheGun(*hand) && hand->clip == 0 && hand->reserve == 0 && now >= z.nextDrop && z.throwStage == 0)
    {
        float runYaw = z.fleeing ? YawOf(z.fleeGoal - me) : z.yaw;
        float yaw = runYaw + 180.0f + (Rnd() < 0.5f ? 60.0f : -60.0f);
        Log("Manhunt: bot '%s' throws away its empty %s", name, String(hand->cls));
        ZheDropToward(bot, yaw);
        z.nextDrop = now + 1.0f;
        z.nextSwitch = now + 0.5f;
        return;
    }
    V3 zpos = zom ? zom->pos : z.zombiePos;
    float d = Len(zpos - eye);
    if (z.throwStage)
    {
        // a grenade: out (ready after its deploy, ~0.75 s), the pin held a moment aiming a bit above the zombie, then
        // let go
        act = true;
        lookAt = zpos + V3{ 0, 0, 20.0f + d * 0.12f };
        if (z.throwStage == 1)
        {
            if (active == z.throwItem && z.throwReadyAt == 0)
                z.throwReadyAt = now + 0.9f;
            if (active == z.throwItem && now >= z.throwReadyAt)
            {
                z.throwStage = 2;
                z.throwAt = now + 0.6f;
            }
            else if (active != z.throwItem && now > z.throwAt + 2.0f)
            {
                z.throwStage = 0; // it wouldn't come out
                z.nextThrow = now + 5.0f;
            }
        }
        else if (z.throwStage == 2)
        {
            b |= IN_ATTACK;
            if (now >= z.throwAt)
            {
                z.throwStage = 3;
                z.throwAt = now + 1.0f;
                Log("Manhunt: bot '%s' throws a %s at the zombie (%.0f u)", name, String(z.throwClass), d);
            }
        }
        else if (now >= z.throwAt)
        {
            z.throwStage = 0;
            z.nextThrow = now + 8.0f;
            for (int k = 0; k < n; k++)
                if (inv[k].w == z.throwItem)
                    Log("Manhunt: bot '%s' still has its %s after the throw", name, String(z.throwClass));
        }
        return;
    }
    bool close = zom && inSight && d < 700.0f;
    if (close)
    {
        z.gunUntil = now + 2.0f; // the gun stays out a moment after losing it
        // a grenade, not too close
        for (int k = 0; k < n; k++)
            if (ZheThrowable(inv[k]) && d > 200.0f && now >= z.nextThrow)
            {
                z.throwStage = 1;
                z.throwItem = inv[k].w;
                z.throwClass = inv[k].cls;
                z.throwAt = now;
                z.throwReadyAt = 0;
                FakeCommand(bot, String(inv[k].cls));
                return;
            }
    }
    // the gun in hand if it has ammunition, else another one (the primary first)
    const ZheItem* gun = hand && ZheGun(*hand) && hand->clip + hand->reserve > 0 ? hand : nullptr;
    for (int k = 0; k < n && !gun; k++)
        if (ZheGun(inv[k]) && inv[k].clip + inv[k].reserve > 0 && (inv[k].slot == 1 || !gun))
            gun = &inv[k];
    if (gun && now < z.gunUntil)
    {
        if (gun != hand)
        {
            if (now >= z.nextSwitch)
            {
                FakeCommand(bot, String(gun->cls));
                z.nextSwitch = now + 1.0f;
            }
            return;
        }
        if (!close)
            return;
        act = true;
        lookAt = Center(zom->e) + V3{ (Rnd() - 0.5f) * 14.0f, (Rnd() - 0.5f) * 14.0f, 8.0f };
        if (gun->clip == 0)
        {
            b |= IN_RELOAD;
            return;
        }
        z.fireToggle = !z.fireToggle; // pressed every other frame: semi-automatic ones fire too
        if (z.fireToggle)
            b |= IN_ATTACK;
        return;
    }
    // nothing to shoot: the knife (faster on foot)
    if (knife && hand != knife && now >= z.nextSwitch)
    {
        FakeCommand(bot, String(knife->cls));
        z.nextSwitch = now + 1.5f;
    }
}

// a std::list of player indexes (nodes: next, prev, the index at +8) from its head: is the bot in it
static bool ZheListHas(unsigned char* head, void* bot)
{
    __try
    {
        if (!head || !g_IndexOfEdict)
            return false;
        int index = g_IndexOfEdict(bot);
        unsigned char* node = *(unsigned char**)head;
        for (int guard = 0; node && node != head && guard < 40; guard++)
        {
            if (*(int*)(node + 8) == index)
                return true;
            node = *(unsigned char**)node;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

// the generator's repairers (CZHEGenerator +0x18c): is the bot one of them
static bool ZheGenHas(void* gen, void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(gen);
    return p && Readable(p + 0x18c, 4) && ZheListHas(*(unsigned char**)(p + 0x18c), bot);
}

// the ones healing / picking up a human (CZHEHumanModule +0x24, their count at +0x28, 0x10803140 adds them): is the
// bot one of them
static bool ZheHelpedBy(void* patient, void* bot)
{
    unsigned char* mod = nullptr;
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(patient);
        if (pl)
            mod = (unsigned char*)((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(pl, ZHE_HUMAN_MODULE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod = nullptr;
    }
    return mod && Readable(mod + 0x24, 4) && ZheListHas(*(unsigned char**)(mod + 0x24), bot);
}

// the bot going to heal / pick up this human, if any (one is enough: the others carry on)
static void* ZheHelperOf(void* patient, float now)
{
    for (auto& o : g_zheBots)
        if (o.second.helping == patient && now - o.second.lastThink < 0.5f && Alive(o.first))
            return o.first;
    return nullptr;
}

// is a spot out of the zombie's sight (a wall, a building in between)
static bool ZheHidden(void* bot, V3 spot, V3 zombie)
{
    TraceResult tr;
    return !Trace(zombie + V3{ 0, 0, 28 }, spot + V3{ 0, 0, 28 }, bot, 1, tr);
}

// a human bot: what to do, where to go and look
static void ZheHumanThink(void* bot, ZheBot& z, int state, V3 me, V3 eye, float now, V3& goal, bool& hasGoal, float& arrive,
                          bool& act, V3& lookAt, float& lookPitch, unsigned short& b)
{
    void* wasHelping = z.helping;
    z.helping = nullptr; // claimed again below while it still goes to help
    const ZhePlayer* zom = ZheZombieNear(me);
    bool inSight = false;
    if (zom)
    {
        // heard close by (the heartbeat, its steps; while running it looks back now and then), seen in front, or in
        // plain sight not far
        float zd = Len(zom->pos - me);
        float rel = fabsf(AngleDiff(YawOf(zom->pos - me), z.yaw));
        bool los = zd < 1400.0f && CanSee(bot, eye, zom->e);
        inSight = los && state != ZHE_DOWN;
        if (zd < 550.0f || (z.fleeing && zd < 900.0f) || los && (rel < 75.0f || zd < 700.0f))
        {
            if (now - z.zombieSeenAt > 12.0f && state != ZHE_DOWN)
                ZheRadio(bot, 3, 2, "enemy spotted");
            z.zombieSeenAt = now;
            z.zombiePos = zom->pos;
        }
    }
    // weapons: shoot / throw at it when it is in sight, close; the knife out otherwise
    if (state != ZHE_DOWN)
        ZheFight(bot, z, zom, inSight, me, eye, now, act, lookAt, b);
    bool fighting = act;
    float since = now - z.zombieSeenAt, zdist = Len(z.zombiePos - me);
    z.sweep = 25; // looks around while walking

    // healed or picked up by a teammate close by: still (the help stops when the patient moves)
    void* helper = state >= 1 && state <= ZHE_DOWN ? ZheHelperOf(bot, now) : nullptr;
    if (helper && Len(Origin(helper) - me) < 200.0f)
    {
        z.doing = state == ZHE_DOWN ? "down, being picked up" : "being healed";
        z.fleeing = false;
        act = true;
        lookAt = Eye(helper);
        b = 0;
        return;
    }
    if (state == ZHE_DOWN)
    {
        z.fleeing = false;
        z.gen = nullptr;
        if (since < 3.0f && zdist < 1000.0f)
        {
            // crawl away from it, out of its sight
            if (now >= z.fleeUntil || Len2D(z.fleeGoal - me) < 80.0f)
            {
                z.fleeGoal = ZheFleeGoal(bot, me, z.zombiePos);
                z.fleeUntil = now + 4.0f;
            }
            goal = z.fleeGoal;
            hasGoal = true;
            arrive = 60;
            z.doing = "crawling away from the zombie";
            return;
        }
        // crawl to the closest teammate standing, who can pick it up (still once one comes)
        const ZhePlayer* mate = nullptr;
        for (auto& p : g_zhe.players)
            if (p.e != bot && ZheStanding(p.state) && (!mate || Len(p.pos - me) < Len(mate->pos - me)))
                mate = &p;
        if (mate && Len(mate->pos - me) > 160.0f && !helper)
        {
            goal = mate->pos;
            hasGoal = true;
            arrive = 140;
        }
        z.doing = helper ? "down, a teammate comes" : "down, waiting for help";
        return;
    }

    // run from the zombie to somewhere out of its sight (behind a building, a wall), the whole way there; arrived and
    // out of sight: hide; seen again or it comes closer to the spot than the bot: somewhere else
    bool threat = since < 1.0f && zdist < (now < z.hideUntil ? 700.0f : 1100.0f);
    if (threat && !z.fleeing)
    {
        z.fleeing = true;
        z.fleeGoal = ZheFleeGoal(bot, me, z.zombiePos);
        z.fleeUntil = now + 7.0f;
        z.hideUntil = 0;
    }
    if (z.fleeing)
    {
        bool reached = Len2D(z.fleeGoal - me) < 90.0f;
        bool cutOff = since < 0.5f && Len2D(z.zombiePos - z.fleeGoal) < Len2D(me - z.fleeGoal);
        if (reached || now >= z.fleeUntil || cutOff)
        {
            // still on its heels (seen or heard in the last 3 s): keep running, somewhere else
            if (since > 3.0f && zdist > 700.0f && ZheHidden(bot, me, z.zombiePos))
            {
                z.fleeing = false;
                z.hideUntil = now + 4.0f + Rnd() * 5.0f; // out of sight: hide a while
            }
            else if (since > 10.0f)
                z.fleeing = false; // long gone
            else
            {
                z.fleeGoal = ZheFleeGoal(bot, me, z.zombiePos);
                z.fleeUntil = now + 7.0f;
            }
        }
    }
    if (z.fleeing)
    {
        goal = z.fleeGoal;
        hasGoal = true;
        arrive = 60;
        z.sweep = 0;
        z.doing = fighting ? "running from the zombie, shooting" : "running from the zombie";
        z.gen = nullptr;
        // right behind: jump about (harder to hit, up onto things)
        if (zdist < 260.0f && since < 0.5f && now >= z.nextJump && !fighting)
        {
            b |= IN_JUMP;
            z.nextJump = now + 0.9f + Rnd() * 1.2f;
        }
        return;
    }
    if (fighting)
    {
        z.doing = "fighting the zombie";
        return;
    }
    // hidden: crouched, still, watching the way it would come
    if (now < z.hideUntil)
    {
        z.doing = "hiding";
        act = true;
        lookAt = z.zombiePos + V3{ 0, 0, 40 };
        b |= IN_DUCK;
        return;
    }
    bool calm = since > 8.0f || zdist > 1500.0f;
    // the exits are open: out
    const ZheExit* exit = nullptr;
    for (auto& x : g_zhe.exits)
        if (x.open && (!exit || Len(x.pos - me) < Len(exit->pos - me)))
            exit = &x;
    if (exit)
    {
        if (!g_zhe.openSaid)
        {
            g_zhe.openSaid = true;
            ZheRadio(bot, 2, 1, "go go go (the exits are open)");
        }
        goal = exit->pos;
        hasGoal = true;
        arrive = 8;
        z.doing = "escaping";
        return;
    }
    // a teammate down (first) or hurt (when calm): pick them up / heal them, one bot each, unless the zombie waits there
    const ZhePlayer* patient = nullptr;
    for (auto& p : g_zhe.players)
    {
        if (p.e == bot || p.state < 1 || p.state > ZHE_DOWN || (p.state != ZHE_DOWN && !calm))
            continue;
        if (Len(p.pos - me) > (p.state == ZHE_DOWN ? 3500.0f : 1500.0f))
            continue;
        if (now - z.zombieSeenAt < 8.0f && Len(z.zombiePos - p.pos) < 500.0f)
            continue;
        void* other = ZheHelperOf(p.e, now);
        if (other && other != bot)
            continue;
        if (!patient || (p.state == ZHE_DOWN) > (patient->state == ZHE_DOWN) ||
            (p.state == ZHE_DOWN) == (patient->state == ZHE_DOWN) && Len(p.pos - me) < Len(patient->pos - me))
            patient = &p;
    }
    if (patient)
    {
        z.gen = nullptr;
        z.helping = patient->e;
        bool down = patient->state == ZHE_DOWN;
        if (wasHelping != patient->e)
            ZheRadio(bot, 3, 1, down ? "affirmative (going to pick a teammate up)" : "affirmative (going to heal a teammate)");
        if (ZheUseOn(bot, z, patient->e, me, now, goal, hasGoal, arrive, act, lookAt, b))
        {
            z.doing = down ? "picking a teammate up" : "healing a teammate";
            z.sweep = 0;
            // pressed and not among its helpers: let go and press again
            if (z.useDownAt > 0 && now - z.useDownAt > 0.8f && !ZheHelpedBy(patient->e, bot))
            {
                z.useReleaseUntil = now + 0.25f;
                z.useDownAt = 0;
                if (++z.useRetries % 4 == 0)
                    ZheUseReport(bot, z, patient->e, down ? "teammate down" : "teammate hurt", 0);
            }
        }
        else
            z.doing = down ? "going to pick a teammate up" : "going to heal a teammate";
        return;
    }
    // item boxes: walked into to open (a gun, a grenade, a toolbox): close ones always, farther ones while it has no
    // gun with ammunition or no grenade
    if (calm)
    {
        ZheItem inv[16];
        int n = ZheItemsRaw(bot, inv, 16);
        bool armed = false, nade = false;
        for (int k = 0; k < n; k++)
        {
            armed |= ZheGun(inv[k]) && inv[k].clip + inv[k].reserve > 0;
            nade |= ZheThrowable(inv[k]);
        }
        float reach = armed && nade ? 220.0f : 650.0f;
        const ZheBox* bx = nullptr;
        for (auto& x : g_zhe.boxes)
        {
            if (std::find(z.boxesTried.begin(), z.boxesTried.end(), x.e) != z.boxesTried.end())
                continue;
            float d = Len2D(x.pos - me);
            if (d < reach && fabsf(x.pos.z - me.z) < 90.0f && (!bx || d < Len2D(bx->pos - me)))
                bx = &x;
        }
        if (bx)
        {
            if (z.box != bx->e)
            {
                z.box = bx->e;
                z.boxUntil = now + 10.0f;
            }
            if (Len2D(bx->pos - me) < 16.0f || now > z.boxUntil)
                z.boxesTried.push_back(bx->e); // opened (it goes away), empty or out of reach: not again
            else
            {
                goal = bx->pos;
                hasGoal = true;
                arrive = 6;
                z.doing = "opening an item box";
                return;
            }
        }
    }
    // repair
    const ZheGen* g = z.gen ? ZheGenOf(z.gen) : nullptr;
    if (g && g->state == 4 && z.lastGauge >= 0)
        ZheRadio(bot, 3, 4, "sector clear (a generator is done)");
    if (!g || !g->active || g->state == 4)
    {
        z.gen = ZhePickGen(bot, me, z.zombiePos, now - z.zombieSeenAt < 15.0f);
        g = z.gen ? ZheGenOf(z.gen) : nullptr;
        z.lastGauge = -1;
    }
    if (!g)
    {
        // nothing left to repair and the exits still shut: stay near the others
        z.doing = "waiting for the exits";
        const ZhePlayer* mate = nullptr;
        for (auto& p : g_zhe.players)
            if (p.e != bot && ZheStanding(p.state) && (!mate || Len(p.pos - me) < Len(mate->pos - me)))
                mate = &p;
        if (mate && Len(mate->pos - me) > 250.0f)
        {
            goal = mate->pos;
            hasGoal = true;
            arrive = 150;
        }
        return;
    }
    if (g->state == 5 || g->state == 6)
    {
        // hands off while it overheats (or after it blew), next to it
        if (Len2D(g->pos - me) > 140.0f)
        {
            goal = g->pos;
            hasGoal = true;
            arrive = 100;
        }
        act = true;
        lookAt = Center(g->e);
        z.doing = g->state == 5 ? "waiting: the generator overheats" : "waiting: the generator blew";
        z.lastGauge = -1;
        return;
    }
    if (!ZheUseOn(bot, z, g->e, me, now, goal, hasGoal, arrive, act, lookAt, b))
    {
        z.doing = "going to a generator";
        z.lastGauge = -1;
        return;
    }
    z.doing = "repairing";
    z.sweep = 0;
    if (now < z.nudgeUntil)
    {
        // a sidestep: another angle on it
        goal = me + V3{ cosf(z.nudgeYaw / RAD), sinf(z.nudgeYaw / RAD), 0 } * 40.0f;
        hasGoal = true;
        arrive = 4;
    }
    if (z.lastGauge < 0)
        z.lastGauge = g->gauge;
    // pressed and not among its repairers: let go and press again; a few misses: another spot
    if (z.useDownAt > 0 && now - z.useDownAt > 0.8f && !ZheGenHas(g->e, bot))
    {
        z.useReleaseUntil = now + 0.25f;
        z.useDownAt = 0;
        if (++z.useRetries % 4 == 0)
        {
            ZheUseReport(bot, z, g->e, "generator", g->users);
            z.nudgeYaw = YawOf(me - g->pos) + (Rnd() < 0.5f ? 70.0f : -70.0f);
            z.nudgeUntil = now + 0.5f;
        }
    }
}

// what the zombie notices: right next to it always, else in its field of view and in plain sight (crouched humans
// far away go unseen)
static const int FL_DUCKING_ = 0x4000;
static bool ZheZombieSees(void* bot, ZheBot& z, V3 eye, const ZhePlayer& p)
{
    float d = Len(p.pos - eye);
    if (d < 350.0f)
        return true;
    if (d > 3000.0f || fabsf(AngleDiff(YawOf(p.pos - eye), z.yaw)) > 70.0f)
        return false;
    if ((F<int>(Vars(p.e), EV_FLAGS) & FL_DUCKING_) && d > 1100.0f)
        return false;
    return CanSee(bot, eye, p.e);
}

// what it hears: a human running (not crouched, not walking) close by
static bool ZheZombieHears(V3 me, const ZhePlayer& p)
{
    float d = Len(p.pos - me);
    if (d > 900.0f || (F<int>(Vars(p.e), EV_FLAGS) & FL_DUCKING_))
        return false;
    return Len2D(Vec((float*)(Vars(p.e) + EV_VELOCITY))) > 170.0f;
}

// the zombie: patrols near the humans looking around, goes to generators that make noise, chases whoever it sees
// (not always the shortest way, it can guess wrong where they went) and gives up a few seconds after losing them
static void ZheZombieThink(void* bot, ZheBot& z, V3 me, V3 eye, float now, V3& goal, bool& hasGoal, float& arrive, bool& act,
                           V3& lookAt, float& lookPitch, unsigned short& b)
{
    // who is in sight
    const ZhePlayer* seen = nullptr;
    float seenD = 1e30f;
    for (auto& p : g_zhe.players)
    {
        if (!ZheStanding(p.state) || !ZheZombieSees(bot, z, eye, p))
            continue;
        float d = Len(p.pos - me) * (p.e == z.target ? 0.6f : 1.0f); // keeps its prey
        if (d < seenD)
        {
            seenD = d;
            seen = &p;
        }
    }
    if (!seen)
    {
        z.seenSince = -1;
        if (z.zmode != ZZ_CHASE && z.zmode != ZZ_SEARCH)
            for (auto& p : g_zhe.players)
                if (ZheStanding(p.state) && ZheZombieHears(me, p))
                {
                    Log("Manhunt: the zombie '%s' hears '%s' running (%.0f u)", String(F<int>(Vars(bot), EV_NETNAME)),
                        String(F<int>(Vars(p.e), EV_NETNAME)), Len(p.pos - me));
                    z.zmode = ZZ_SEARCH;
                    z.target = p.e;
                    z.lastSeen = p.pos;
                    z.giveUpAt = now + 10.0f + Rnd() * 6.0f;
                    z.nextTrack = now + 2.0f;
                    z.lookAroundUntil = 0;
                    z.hasVia = false;
                    break;
                }
    }
    else
    {
        if (z.seenSince < 0)
            z.seenSince = now;
        // a moment to react, unless already after them
        bool hunting = z.zmode == ZZ_CHASE || z.zmode == ZZ_SEARCH;
        if (hunting || now - z.seenSince > 0.4f || Len(seen->pos - me) < 250.0f)
        {
            if (z.zmode != ZZ_CHASE || z.target != seen->e)
            {
                z.hasVia = !hunting && Len(seen->pos - me) > 700.0f && Rnd() < 0.3f && ZheDetour(me, seen->pos, z.via);
                Log("Manhunt: the zombie '%s' spots '%s' (%.0f u)%s", String(F<int>(Vars(bot), EV_NETNAME)),
                    String(F<int>(Vars(seen->e), EV_NETNAME)), Len(seen->pos - me), z.hasVia ? ", takes the long way" : "");
            }
            z.zmode = ZZ_CHASE;
            z.target = seen->e;
            z.lastSeen = seen->pos;
            z.lastSeenAt = now;
        }
    }

    if (z.zmode == ZZ_CHASE)
    {
        if (!z.target || !Alive(z.target) || !ZheStanding(ZheState(z.target)))
        {
            // down or gone: on with the patrol, away from there for a while (room to pick them up)
            if (z.target && Alive(z.target))
            {
                z.leaveFrom = Origin(z.target);
                z.leaveUntil = now + 15.0f + Rnd() * 15.0f;
            }
            z.zmode = ZZ_PATROL;
            z.target = nullptr;
            z.patrolUntil = 0;
        }
        else if (now - z.lastSeenAt > 1.5f)
        {
            // out of sight: after them where they were (or a wrong guess now and then), and it keeps at it a long while
            z.zmode = ZZ_SEARCH;
            V3 guess;
            if (Rnd() < 0.2f && ZheRandomArea(z.lastSeen, 200.0f, 550.0f, guess))
                z.lastSeen = guess;
            z.giveUpAt = now + 14.0f + Rnd() * 10.0f;
            z.nextTrack = now + 2.0f + Rnd() * 2.0f;
            z.lookAroundUntil = 0;
            z.hasVia = false;
        }
    }
    if (z.zmode == ZZ_CHASE)
    {
        V3 tp = Origin(z.target);
        float d = Len(tp - me);
        if (z.hasVia && (Len2D(z.via - me) < 120.0f || d < 500.0f))
            z.hasVia = false;
        goal = z.hasVia ? z.via : tp;
        hasGoal = true;
        arrive = 20;
        z.doing = z.hasVia ? "chasing (the long way)" : "chasing";
        if (d < 110.0f)
        {
            // aims with a small error: it can miss
            V3 c = Center(z.target);
            float ay = YawOf(c - eye) + z.aimError, d2 = Len2D(c - eye);
            act = true;
            lookAt = { eye.x + cosf(ay / RAD) * d2, eye.y + sinf(ay / RAD) * d2, c.z };
            if (d < 85.0f && now >= z.nextAttack)
            {
                b |= IN_ATTACK;
                z.nextAttack = now + 0.55f + Rnd() * 0.35f;
                z.aimError = (Rnd() - 0.5f) * 30.0f;
            }
        }
        return;
    }
    if (z.zmode == ZZ_SEARCH)
    {
        if (now >= z.giveUpAt)
        {
            Log("Manhunt: the zombie '%s' lost them, patrols again", String(F<int>(Vars(bot), EV_NETNAME)));
            z.zmode = ZZ_PATROL;
            z.target = nullptr;
            z.patrolUntil = 0;
        }
        else
        {
            // on their trail: now and then it picks up where they really are (closer if it hears them running), or
            // takes a wrong turn
            if (now >= z.nextTrack && z.target && Alive(z.target) && ZheStanding(ZheState(z.target)))
            {
                z.nextTrack = now + 2.5f + Rnd() * 2.5f;
                V3 tp = Origin(z.target), guess;
                float r = Rnd();
                const char* how = nullptr;
                if (Len(tp - me) < 900.0f && r < 0.8f)
                {
                    z.lastSeen = tp;
                    how = "hears them";
                }
                else if (r < 0.7f && ZheRandomArea(tp, 0.0f, 450.0f, guess))
                {
                    z.lastSeen = guess;
                    how = "picks up the trail";
                }
                else if (ZheRandomArea(z.lastSeen, 300.0f, 900.0f, guess))
                {
                    z.lastSeen = guess;
                    how = "takes a wrong turn";
                }
                if (how)
                {
                    z.lookAroundUntil = 0;
                    DLog("Manhunt bot '%s': the zombie %s (%.0f u from them)", String(F<int>(Vars(bot), EV_NETNAME)), how,
                         Len(z.lastSeen - tp));
                }
            }
        }
        if (z.zmode == ZZ_SEARCH && z.lookAroundUntil == 0 && Len2D(z.lastSeen - me) > 60.0f)
        {
            goal = z.lastSeen;
            hasGoal = true;
            arrive = 50;
            z.sweep = 35;
            z.doing = "searching where they went";
            return;
        }
        else if (z.zmode == ZZ_SEARCH)
        {
            // there: stands and looks around
            if (z.lookAroundUntil == 0)
            {
                z.lookAroundUntil = now + 1.5f;
                z.lookBase = z.yaw;
                z.lookStart = now;
            }
            float ly = z.lookBase + sinf((now - z.lookStart) * 1.8f) * 110.0f;
            act = true;
            lookAt = eye + V3{ cosf(ly / RAD), sinf(ly / RAD), 0 } * 100.0f;
            z.doing = "looking around";
            return;
        }
    }

    // generators make noise: the ones being repaired now and then (more often close by), an overheating one always
    if (now >= z.nextAlertCheck)
    {
        z.nextAlertCheck = now + 1.0f;
        for (auto& g : g_zhe.gens)
        {
            if (!g.active || g.state == 4 || g.e == z.hitGen && z.zmode == ZZ_INVESTIGATE)
                continue;
            auto done = z.genDoneUntil.find(g.e);
            if (done != z.genDoneUntil.end() && now < done->second)
                continue;
            float d = Len(g.pos - me);
            bool heard = g.state == 5 ? d < 3500.0f
                                      : g.users > 0 && Rnd() < (d < 1200.0f ? 0.35f : d < 2500.0f ? 0.12f : 0.0f);
            if (heard)
            {
                z.zmode = ZZ_INVESTIGATE;
                z.hitGen = g.e;
                z.hitUntil = 0;
                z.hasVia = d > 900.0f && Rnd() < 0.3f && ZheDetour(me, g.pos, z.via);
                Log("Manhunt: the zombie '%s' hears a generator %.0f u away (%s)%s", String(F<int>(Vars(bot), EV_NETNAME)),
                    d, g.state == 5 ? "overheating" : "being repaired", z.hasVia ? ", takes the long way" : "");
                break;
            }
        }
    }
    if (z.zmode == ZZ_INVESTIGATE)
    {
        const ZheGen* g = ZheGenOf(z.hitGen);
        if (!g || g->state == 4)
        {
            z.zmode = ZZ_PATROL;
            z.patrolUntil = 0;
        }
        else
        {
            if (z.hasVia && Len2D(z.via - me) < 120.0f)
                z.hasVia = false;
            if (Len2D(g->pos - me) > 90.0f)
            {
                goal = z.hasVia ? z.via : g->pos;
                hasGoal = true;
                arrive = 70;
                z.sweep = 20;
                z.doing = "going to a noisy generator";
                return;
            }
            // there, nobody in sight: smashes it if it has progress, then patrols around
            if (z.hitUntil == 0)
                z.hitUntil = now + (g->gauge > 0 ? 3.0f : 0.3f);
            if (now < z.hitUntil && g->state != 6)
            {
                act = true;
                lookAt = Center(g->e);
                z.doing = "smashing a generator";
                if (now >= z.nextAttack)
                {
                    b |= IN_ATTACK;
                    z.nextAttack = now + 0.5f;
                }
                return;
            }
            z.genDoneUntil[g->e] = now + 25.0f;
            z.zmode = ZZ_PATROL;
            z.patrolUntil = 0;
        }
    }

    // patrol: a wide, random round about the humans (it never knows exactly where): mostly a ring 700 to 2400 u
    // around one of them, now and then closer, now and then anywhere on the map; it keeps off the humans who are down
    // (and where it just downed one) most of the time, so the others can pick them up. At each spot it stops and
    // looks around a moment.
    if (now >= z.patrolUntil)
    {
        std::vector<const ZhePlayer*> humans, down;
        for (auto& p : g_zhe.players)
            if (ZheStanding(p.state))
                humans.push_back(&p);
            else if (p.state == ZHE_DOWN)
                down.push_back(&p);
        V3 around = humans.empty() ? me : humans[rand() % humans.size()]->pos;
        float r = Rnd(), minR = 700.0f, maxR = 2400.0f;
        if (r < 0.2f)
            minR = 300.0f, maxR = 900.0f; // closer: tension
        else if (r > 0.85f)
            around = me, minR = 800.0f, maxR = 4000.0f; // off somewhere else
        bool keepOff = Rnd() < 0.85f;
        V3 s = me, best = me;
        bool found = false;
        for (int tries = 0; tries < 12 && !found; tries++)
        {
            if (!ZheRandomArea(around, minR, maxR, s) && !ZheRandomArea(me, 400.0f, 2000.0f, s))
                break;
            best = s;
            found = true;
            if (keepOff)
            {
                for (auto* d : down)
                    found &= Len2D(d->pos - s) > 900.0f;
                if (now < z.leaveUntil)
                    found &= Len2D(z.leaveFrom - s) > 1100.0f;
            }
        }
        z.patrolGoal = best;
        z.patrolUntil = now + 10.0f + Rnd() * 15.0f;
        z.lookAroundUntil = 0;
        z.speedScale = 0.6f + Rnd() * 0.3f;
        z.patrolSpeed = z.speedScale;
    }
    if (Len2D(z.patrolGoal - me) < 100.0f)
    {
        // there: a look around, then on
        if (z.lookAroundUntil == 0)
        {
            z.lookAroundUntil = now + 1.0f + Rnd() * 2.5f;
            z.lookBase = z.yaw;
            z.lookStart = now;
        }
        if (now >= z.lookAroundUntil)
            z.patrolUntil = 0;
        float ly = z.lookBase + sinf((now - z.lookStart) * 1.5f) * 90.0f;
        act = true;
        lookAt = eye + V3{ cosf(ly / RAD), sinf(ly / RAD), 0 } * 100.0f;
        z.doing = "patrolling: looking around";
        return;
    }
    goal = z.patrolGoal;
    hasGoal = true;
    arrive = 60;
    z.sweep = 40;
    z.speedScale = z.patrolSpeed;
    z.doing = "patrolling near the humans";
}

// The exit zones only let out what classifies as a player (CZHEExitZone::ExitTouch 0x108ea1e0: vfunc 0x30 == 2), and
// the bots classify as 15 (CCSBot 0x107f8c40): botassist lets a bot out the way ExitTouch does, once its box is in an
// open exit's zone: the human module's escape (0x108017a0: state 5, spectator flags, the "escaped" message), then the
// rules' check (g_pGameRules vfunc 0x100).
static const DWORD MP_ZHE_ESCAPE = 0x108017a0;
typedef void(__thiscall* ZheEscapeFn)(void* module);
typedef void(__thiscall* RulesCheckFn)(void* rules);

static bool ZheEscape(void* bot)
{
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(bot);
        void* mod = pl ? ((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(pl, ZHE_HUMAN_MODULE) : nullptr;
        if (!mod)
            return false;
        ((ZheEscapeFn)(MP_ZHE_ESCAPE + g_mpDelta))(mod);
        void* rules = *(void**)(MP_GAMERULES + g_mpDelta);
        if (rules)
            ((RulesCheckFn)((*(void***)rules)[0x100 / 4]))(rules);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool ZheInExit(void* bot, V3 me)
{
    for (auto& x : g_zhe.exits)
    {
        if (!x.open)
            continue;
        unsigned char* v = Vars(x.e);
        V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
        if (me.x + 16 > lo.x && me.x - 16 < hi.x && me.y + 16 > lo.y && me.y - 16 < hi.y && me.z + 36 > lo.z &&
            me.z - 36 < hi.z)
            return true;
    }
    return false;
}

// a room bot in Manhunt: botassist plays it (true: aim, movement and buttons replaced)
static bool DriveZheBot(void* bot, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (GameMode() != MODE_ZHE || !Alive(bot) || g_edictToPrivate < 0)
        return false;
    float now = Now();
    ZheScan(now);
    int state = ZheState(bot);
    if (state < 0)
        return false;
    if (state == ZHE_ESCAPED)
    {
        fwd = side = 0; // out: still, the stock AI kept away
        buttons = 0;
        return true;
    }
    if (state != ZHE_ZOMBIE && ZheInExit(bot, Origin(bot)))
    {
        bool ok = ZheEscape(bot);
        static int reports = 0;
        if (reports++ < 40)
            Log("Manhunt: bot '%s' %s", String(F<int>(Vars(bot), EV_NETNAME)),
                ok ? "escapes through an exit" : "is in an exit but couldn't be let out");
        fwd = side = 0;
        buttons = 0;
        return true;
    }
    ZheBot& z = g_zheBots[bot];
    BotState& st = StateFor(bot);
    if (!z.init || now < z.lastThink - 1.0f || now - z.lastThink > 2.0f)
    {
        z = ZheBot();
        z.init = true;
        z.yaw = F<float>(Vars(bot), EV_VANGLE + 4);
        z.stuckFrom = Origin(bot);
        z.stuckCheck = now + 1.0f;
        st.path.clear();
    }
    float dt = now - z.lastThink;
    dt = dt <= 0 || dt > 0.25f ? 0.02f : dt;
    z.lastThink = now;
    if (state != z.lastState)
    {
        static int reports = 0;
        if (reports++ < 60)
            Log("Manhunt: bot '%s' is now %s", String(F<int>(Vars(bot), EV_NETNAME)),
                state == ZHE_ZOMBIE ? "the zombie" : state == 0 ? "healthy" : state == ZHE_DOWN ? "down" : "hurt");
        if (z.lastState >= 0 && z.lastState < state && state != ZHE_ZOMBIE)
            state == ZHE_DOWN ? ZheRadio(bot, 3, 3, "need backup (down)")
                              : ZheRadio(bot, 1, 6, "taking fire, need assistance (hurt)");
        z.lastState = state;
    }
    V3 me = Origin(bot), eye = Eye(bot);
    V3 goal{}, lookAt{};
    bool hasGoal = false, act = false;
    float arrive = 40, lookPitch = 0;
    unsigned short b = 0;
    z.sweep = 0;
    z.speedScale = 1;
    if (state == ZHE_ZOMBIE)
        ZheZombieThink(bot, z, me, eye, now, goal, hasGoal, arrive, act, lookAt, lookPitch, b);
    else
        ZheHumanThink(bot, z, state, me, eye, now, goal, hasGoal, arrive, act, lookAt, lookPitch, b);

    float moveYaw = z.yaw;
    bool move = false;
    V3 wp;
    if (hasGoal && NextWaypoint(bot, st, me, goal, arrive, wp))
    {
        moveYaw = YawOf(wp - me);
        move = true;
    }
    else if (hasGoal && z.fleeing && Len2D(goal - me) > arrive + 20.0f)
        z.fleeUntil = now; // no way there: another spot next time
    if (move && now < z.detourUntil)
        moveYaw = z.detourYaw;
    // walking: looks along the way, glancing left and right
    float phase = (float)(((DWORD)bot >> 4) % 7);
    float lookYaw = move ? moveYaw + sinf(now * 1.3f + phase) * z.sweep : z.yaw;
    float wantPitch = 0;
    if (act)
    {
        V3 d = lookAt - eye;
        lookYaw = YawOf(d);
        wantPitch = lookPitch != 0 ? lookPitch : PitchOf(d);
    }
    z.yaw = Approach(z.yaw, lookYaw, 400.0f * dt);
    float k = dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f;
    z.pitch += (wantPitch - z.pitch) * k;
    float f = 0, s = 0;
    if (move)
    {
        float rel = AngleDiff(moveYaw, z.yaw) / RAD;
        f = cosf(rel) * 400.0f * z.speedScale;
        s = -sinf(rel) * 400.0f * z.speedScale;
    }
    // stuck: a jump, then a way round
    if (now >= z.stuckCheck)
    {
        if (move && Len2D(me - z.stuckFrom) < 24.0f)
            z.stuckCount++;
        else
            z.stuckCount = 0;
        if (z.stuckCount == 2)
            b |= IN_JUMP;
        else if (z.stuckCount >= 3)
        {
            z.detourYaw = z.yaw + (Rnd() < 0.5f ? 100.0f : -100.0f);
            z.detourUntil = now + 1.2f;
            st.nextRepath = 0;
            z.stuckCount = 0;
        }
        z.stuckFrom = me;
        z.stuckCheck = now + 1.0f;
    }
    if (f > 50.0f)
        b |= IN_FORWARD;
    else if (f < -50.0f)
        b |= IN_BACK;
    if (s > 50.0f)
        b |= IN_MOVERIGHT;
    else if (s < -50.0f)
        b |= IN_MOVELEFT;
    yaw = AngleDiff(z.yaw, 0.0f);
    pitch = z.pitch < -89.0f ? -89.0f : z.pitch > 89.0f ? 89.0f : z.pitch;
    fwd = f;
    side = s;
    buttons = (unsigned short)((buttons & IN_DUCK) | b);
    if (now >= z.nextReport)
    {
        z.nextReport = now + 10.0f;
        const ZheGen* g = z.gen ? ZheGenOf(z.gen) : z.hitGen ? ZheGenOf(z.hitGen) : nullptr;
        DLog("Manhunt bot '%s' (%s, state %d) at %.0f %.0f %.0f: %s; goal %.0f u away; generator state %d gauge %.1f "
             "users %d",
             String(F<int>(Vars(bot), EV_NETNAME)), state == ZHE_ZOMBIE ? "zombie" : "human", state, me.x, me.y, me.z,
             z.doing, hasGoal ? Len(goal - me) : 0.0f, g ? g->state : -1, g ? g->gauge : 0.0f, g ? g->users : 0);
    }
    return true;
}
