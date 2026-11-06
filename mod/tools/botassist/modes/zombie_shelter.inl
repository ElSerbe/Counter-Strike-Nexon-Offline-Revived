// botassist: Zombie Shelter (team, mode 28; rules CSOGameRulesSHELTERTeam, vtable 0x117a1ba8, its event version 0x117a2d1c).
// Included by botassist.cpp at this point (one translation unit: it uses the shared helpers defined before it). Nothing here
// runs outside the mode: ShRules() is null unless GameMode() == MODE_SHELTER and the rules are the shelter's.
//
// The mode (https://cso.fandom.com/wiki/Zombie_Shelter): days and nights. By day the players leave the shelter to hit wood
// (entity "zsht_resource") and iron / money rocks ("shelter_team_mineral") with the axe, kill zombies for money, buy weapons at
// the map's buy zones ("shelter_team_buyzone") and bring what they carry back to the shelter's warehouse. Away from the shelter
// their sanity drops, and at 0 it hurts. Each day gives skill points for a skill tree. At night the zombies attack the shelter,
// whose buildings (walls, doors, turrets, generator...) the players build and repair with the axe. From some day on a boss
// shows up somewhere on the map.
//
// What the game keeps (all in mp.dll, the game's own rules run the mode offline):
//   rules +0x214 phase counter (day n = phases (2n-1, 2n)), +0x218 night (bool), +0x228 end of the phase (seconds, int)
//   rules +0x27c + team*0xc + type*4: the team's storage (type 0 wood, 1 iron, 2 power), +0x2a0 + ...: its capacity
//   rules +0x14d0 + index*0xc + type*4: what a player carries (index = entity index), +0x1590 + ...: how much they can carry
//   rules +0x16b0 + shelter*4: the shelters (CBaseEntity*), a player's shelter at player +0x31be (short, -1 none)
//   rules +0x16bc + index*4: a player's sanity (float, drops away from the shelter: rules vfunc 0x458 = 0x10840650)
//   player +0x32ee skill points (short), +0x32a6 + id*2 skill levels; player +0xca8 money
//   A resource takes "damage" only from the knife slot (weapon id 29 = the shelter axe): zsht_resource +0xdc hits left,
//   +0xd8 type; a mineral +0xec is up. A building takes a teammate's axe hit as a repair (0x10894e40).
//   The players' menus send client commands to rules vfunc 0x24 (0x108396b0): "zsht_warehouse <type> <put 0..5> <take 0..5>"
//   moves what a player carries into the storage (no distance check), "zsht_RequestSkillup <id>" spends a skill point
//   (0x10844830 checks it). That function reads its arguments straight from the engine (g_engfuncs.pfnCmd_Argv, not the
//   bots' fake-command arguments), so ShelterCommand swaps the engine's Cmd_Args/Argv/Argc for its own around the call.
//
// What the bots do (Drive moves and aims them; ShelterGoal, ShelterLook and ShelterSwing are its hooks): by day they gather
// with the axe, go home when their pockets are full, their sanity gets low or the night is near, put everything in the
// warehouse, rest until their sanity is back, buy weapons at a buy zone with their money, repair damaged buildings, and go
// after a boss when one is on the map. At night they stay in the shelter, shoot the zombies and repair what gets broken.
// They spend their skill points by themselves. A player's radio order (follow, hold...) still comes first.
static const int MODE_SHELTER = 28;
static const DWORD MP_SHT_RULES_VT = 0x117a1ba8, MP_SHT_EVENT_RULES_VT = 0x117a2d1c;
static const int SH_RULES_SIZE = 0x2914;
static const int SH_PHASE = 0x214, SH_NIGHT = 0x218, SH_PHASE_END = 0x228, SH_STORAGE = 0x27c, SH_STORAGE_MAX = 0x2a0;
static const int SH_CARRIED = 0x14d0, SH_CARRY_MAX = 0x1590, SH_SHELTERS = 0x16b0, SH_SANITY = 0x16bc;
static const int SHP_SHELTER = 0x31be, SHP_SKILL_POINTS = 0x32ee, SHP_MONEY = 0xca8, SHP_ACTIVE_ITEM = 0x101c, SH_WEAPON_ID = 0xe4;
static const int SH_KNIFE_ID = 29, SH_CLIENTCOMMAND_VFUNC = 0x24, EV_MAX_HEALTH = 0x1dc;
static const int ENG_CMD_ARGS = 87, ENG_CMD_ARGC = 89;
static const float SH_THINK = 0.4f, SH_HOME_RADIUS = 260.0f, SH_GATHER_RANGE = 3200.0f, SH_REACH = 62.0f;
static const float SH_SANITY_HOME = 0.4f, SH_SANITY_BOSS = 0.2f, SH_SANITY_RESTED = 0.95f, SH_NIGHT_NEAR = 45.0f;
static const float SH_REPAIR_BELOW = 0.8f, SH_REPAIR_RANGE = 1600.0f, SH_BUY_MONEY = 3500.0f, SH_GIVE_UP = 25.0f;

enum ShTask
{
    SH_NONE,
    SH_GATHER,  // hitting a resource
    SH_HOME,    // back to the shelter (full pockets, low sanity, night soon)
    SH_REST,    // in the shelter until its sanity is back
    SH_REPAIR,  // hitting a damaged building of its team
    SH_BUY,     // to a buy zone
    SH_BOSS,    // after the boss
    SH_DEFEND,  // around the shelter (night, or nothing to do)
};
static const char* const g_shTaskNames[] = { "none", "gather", "home", "rest", "repair", "buy", "boss", "defend" };

struct ShBot
{
    ShTask task = SH_NONE;
    void* object = nullptr; // what it hits (a resource or a building), else null
    float objectSince = 0, next = 0, switchAt = 0, roamAt = 0, nextSkill = 0, maxSanity = 0, lastReport = 0, swingSince = 0;
    float lastObjectHp = -1;
    int boughtDay = -1, slotTry = 0, swings = 0;
    V3 goal{}, roam{};
    bool hasGoal = false;
    float arrive = 60.0f;
};
static std::map<void*, ShBot> g_shBots;
static std::map<void*, float> g_shBanned; // resources a bot couldn't reach: left alone until then
static int g_shLevel = -1;

static unsigned char* ShRules()
{
    if (!g_mpDelta || GameMode() != MODE_SHELTER)
        return nullptr;
    unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    if (!rules || !Readable(rules, SH_RULES_SIZE))
        return nullptr;
    DWORD vt = *(DWORD*)rules - g_mpDelta;
    return vt == MP_SHT_RULES_VT || vt == MP_SHT_EVENT_RULES_VT ? rules : nullptr;
}

static bool ShelterActive()
{
    return ShRules() != nullptr;
}

// the shelter's own entities are never targets (turrets, walls...): their classes start with shelter_ or zsht_
static bool ShelterFriendlyClass(const char* cls)
{
    return cls && GameMode() == MODE_SHELTER && (!strncmp(cls, "shelter_", 8) || !strncmp(cls, "zsht_", 5));
}

static int ShIndex(void* e)
{
    int i = g_IndexOfEdict ? g_IndexOfEdict(e) : 0;
    return i >= 1 && i < 16 ? i : 0; // the mode's per-player arrays have 16 places
}

static float ShSanity(unsigned char* rules, void* bot)
{
    int i = ShIndex(bot);
    return i ? *(float*)(rules + SH_SANITY + i * 4) : 0.0f;
}

static int ShCarried(unsigned char* rules, void* bot, int type)
{
    int i = ShIndex(bot);
    return i ? *(int*)(rules + SH_CARRIED + i * 0xc + type * 4) : 0;
}

static int ShCarryMax(unsigned char* rules, void* bot, int type)
{
    int i = ShIndex(bot);
    return i ? *(int*)(rules + SH_CARRY_MAX + i * 0xc + type * 4) : 0;
}

static int ShDay(unsigned char* rules)
{
    return (*(int*)(rules + SH_PHASE) + 1) / 2;
}

static bool ShNight(unsigned char* rules)
{
    return rules[SH_NIGHT] != 0;
}

// seconds left in this day or night, or -1 if the rules' end time doesn't look like one
static float ShPhaseLeft(unsigned char* rules)
{
    float left = (float)*(int*)(rules + SH_PHASE_END) - Now();
    return left > -5.0f && left < 3600.0f ? left : -1.0f;
}

static int ShWeaponId(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    unsigned char* w = p && Readable(p + SHP_ACTIVE_ITEM, 4) ? *(unsigned char**)(p + SHP_ACTIVE_ITEM) : nullptr;
    return w && Readable(w + SH_WEAPON_ID, 4) ? *(int*)(w + SH_WEAPON_ID) : 0;
}

// the center of the bot's shelter, false if it has none
static bool ShHome(unsigned char* rules, void* bot, V3& home)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p + SHP_SHELTER, 2))
        return false;
    short shelter = *(short*)(p + SHP_SHELTER);
    if (shelter < 0 || shelter > 2)
        return false;
    unsigned char* e = *(unsigned char**)(rules + SH_SHELTERS + shelter * 4);
    unsigned char* pev = e && Readable(e, 0x10) ? *(unsigned char**)(e + 8) : nullptr;
    if (!pev || !Readable(pev, 0x240))
        return false;
    void* edict = *(void**)(pev + EV_CONTAINING_ENTITY);
    if (!edict)
        return false;
    home = Center(edict);
    return true;
}

static V3 ShClosest(V3 from, void* e)
{
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    return V3{ from.x < lo.x ? lo.x : from.x > hi.x ? hi.x : from.x, from.y < lo.y ? lo.y : from.y > hi.y ? hi.y : from.y,
               from.z < lo.z ? lo.z : from.z > hi.z ? hi.z : from.z };
}

static bool ShInBox(V3 at, void* e, float margin)
{
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    return at.x >= lo.x - margin && at.x <= hi.x + margin && at.y >= lo.y - margin && at.y <= hi.y + margin &&
           at.z >= lo.z - 72.0f - margin && at.z <= hi.z + margin;
}

// a resource that can still be hit
static bool ShResourceUp(void* e)
{
    const char* cls = ClassName(e);
    unsigned char* p = (unsigned char*)PrivateOf(e);
    if (!cls || !p || !Readable(p, 0x110))
        return false;
    if (!strcmp(cls, "zsht_resource"))
        return *(int*)(p + 0xdc) > 0;
    if (!strcmp(cls, "shelter_team_mineral"))
        return p[0xec] != 0;
    return false;
}

// a building of the bot's team: a shelter_team_* entity with health (not a zone, a trigger or a missile)
static bool ShBuilding(void* e, int team)
{
    const char* cls = ClassName(e);
    if (!cls || strncmp(cls, "shelter_team_", 13) || strstr(cls, "trigger") || strstr(cls, "buyzone") ||
        strstr(cls, "mineral") || strstr(cls, "missile"))
        return false;
    unsigned char* v = Vars(e);
    if (F<float>(v, EV_HEALTH) <= 0.0f || F<float>(v, EV_MAX_HEALTH) <= 0.0f || F<float>(v, EV_TAKEDAMAGE) == 0.0f)
        return false;
    unsigned char* p = (unsigned char*)PrivateOf(e);
    return p && Readable(p + PLAYER_TEAM, 4) && *(int*)(p + PLAYER_TEAM) == team;
}

// Runs a shelter client command as the bot, the way the menus send it: rules vfunc 0x24 with the engine's argument functions
// pointing at ours for the time of the call
static const char* g_shArgv[6];
static int g_shArgc = 0;
static char g_shArgs[96];
static const char* __cdecl ShCmdArgv(int i) { return i >= 0 && i < g_shArgc ? g_shArgv[i] : ""; }
static int __cdecl ShCmdArgc() { return g_shArgc; }
static const char* __cdecl ShCmdArgs() { return g_shArgs; }

static bool ShelterCommand(void* bot, const char* a0, const char* a1 = nullptr, const char* a2 = nullptr, const char* a3 = nullptr)
{
    unsigned char* rules = ShRules();
    void* player = PrivateOf(bot);
    if (!rules || !player)
        return false;
    const char* in[4] = { a0, a1, a2, a3 };
    g_shArgc = 0;
    g_shArgs[0] = 0;
    for (int i = 0; i < 4 && in[i]; i++)
    {
        g_shArgv[g_shArgc++] = in[i];
        if (i)
        {
            size_t n = strlen(g_shArgs);
            snprintf(g_shArgs + n, sizeof(g_shArgs) - n, "%s%s", i > 1 ? " " : "", in[i]);
        }
    }
    void** table = (void**)(MP_ENGFUNCS + g_mpDelta);
    void* saved[3] = { table[ENG_CMD_ARGS], table[ENG_CMD_ARGV], table[ENG_CMD_ARGC] };
    table[ENG_CMD_ARGS] = (void*)ShCmdArgs;
    table[ENG_CMD_ARGV] = (void*)ShCmdArgv;
    table[ENG_CMD_ARGC] = (void*)ShCmdArgc;
    bool done = false;
    __try
    {
        typedef int(__fastcall * ClientCommandFn)(void* rules, void* edx, void* player, const char* cmd);
        done = ((ClientCommandFn)(*(void***)rules)[SH_CLIENTCOMMAND_VFUNC / 4])(rules, nullptr, player, a0) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("Shelter: exception 0x%08lX running %s", GetExceptionCode(), a0);
    }
    table[ENG_CMD_ARGS] = saved[0];
    table[ENG_CMD_ARGV] = saved[1];
    table[ENG_CMD_ARGC] = saved[2];
    return done;
}

// everything the bot carries into the storage, 5 at a time as the warehouse menu does
static void ShDeposit(unsigned char* rules, void* bot)
{
    int before[2] = { ShCarried(rules, bot, 0), ShCarried(rules, bot, 1) };
    for (int type = 0; type < 2; type++)
        for (int guard = 0; guard < 40 && ShCarried(rules, bot, type) > 0; guard++)
        {
            int had = ShCarried(rules, bot, type);
            ShelterCommand(bot, "zsht_warehouse", type ? "1" : "0", "5", "0");
            if (ShCarried(rules, bot, type) >= had)
                break; // the storage is full
        }
    static int reports = 0;
    if ((before[0] || before[1]) && reports++ < 80)
        Log("Shelter: '%s' puts %d wood and %d iron in the warehouse (still carries %d / %d)", String(F<int>(Vars(bot), EV_NETNAME)),
            before[0] - ShCarried(rules, bot, 0), before[1] - ShCarried(rules, bot, 1), ShCarried(rules, bot, 0), ShCarried(rules, bot, 1));
}

// a skill point: a few random skills until the game takes one (it checks the tree, the levels and the exclusive tier picks)
static void ShSpendSkill(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p + SHP_SKILL_POINTS, 2) || *(short*)(p + SHP_SKILL_POINTS) <= 0)
        return;
    short before = *(short*)(p + SHP_SKILL_POINTS);
    for (int tries = 0; tries < 8; tries++)
    {
        char id[8];
        snprintf(id, sizeof(id), "%d", rand() % 33);
        ShelterCommand(bot, "zsht_RequestSkillup", id);
        if (*(short*)(p + SHP_SKILL_POINTS) < before)
        {
            static int reports = 0;
            if (reports++ < 60)
                Log("Shelter: '%s' learns skill %s (%d points left)", String(F<int>(Vars(bot), EV_NETNAME)), id, *(short*)(p + SHP_SKILL_POINTS));
            return;
        }
    }
}

// the boss: a living monster far bigger than the others (health or height), anywhere on the map
static void* ShFindBoss(V3 around)
{
    void* best = nullptr;
    float bestHp = 0;
    ForEntitiesNear(around, 16384.0f, [&](void* e) {
        if (!IsLivingMonster(e) || IsIgnored(ClassName(e)))
            return;
        unsigned char* v = Vars(e);
        float hp = F<float>(v, EV_MAX_HEALTH) > F<float>(v, EV_HEALTH) ? F<float>(v, EV_MAX_HEALTH) : F<float>(v, EV_HEALTH);
        float tall = F<float>(v, EV_ABSMAX + 8) - F<float>(v, EV_ABSMIN + 8);
        if ((hp >= 20000.0f || tall > 150.0f) && hp > bestHp)
        {
            bestHp = hp;
            best = e;
        }
    });
    return best;
}

// what the bot does now; sets b.task, b.object and where to go
static void ShDecide(unsigned char* rules, void* bot, BotState& st, ShBot& b, V3 me, float now)
{
    V3 home;
    bool hasHome = ShHome(rules, bot, home);
    if (!hasHome)
        home = me;
    int team = TeamOf(bot);
    float sanity = ShSanity(rules, bot);
    if (sanity > b.maxSanity)
        b.maxSanity = sanity;
    float sane = b.maxSanity > 0 ? sanity / b.maxSanity : 1.0f;
    bool night = ShNight(rules);
    float left = ShPhaseLeft(rules);
    bool atHome = Len2D(me - home) < SH_HOME_RADIUS;
    bool full = false, carries = false;
    for (int t = 0; t < 2; t++)
    {
        int c = ShCarried(rules, bot, t), m = ShCarryMax(rules, bot, t);
        full |= m > 0 && c >= m;
        carries |= c > 0;
    }
    if (atHome && carries)
        ShDeposit(rules, bot);

    // a damaged building of the team near the shelter (or near the bot)
    void* damaged = nullptr;
    float damagedDist = 1e9f;
    float below = night ? 0.9f : SH_REPAIR_BELOW;
    ForEntitiesNear(home, SH_REPAIR_RANGE, [&](void* e) {
        if (!ShBuilding(e, team))
            return;
        unsigned char* v = Vars(e);
        if (F<float>(v, EV_HEALTH) >= F<float>(v, EV_MAX_HEALTH) * below)
            return;
        float d = Len(ShClosest(me, e) - me);
        if (d < damagedDist)
        {
            damagedDist = d;
            damaged = e;
        }
    });

    ShTask task = SH_GATHER;
    void* object = nullptr;
    void* boss = nullptr;
    if (night)
        task = damaged && damagedDist < 1200.0f && !st.target ? SH_REPAIR : SH_DEFEND;
    else if (b.task == SH_REST && atHome && sane < SH_SANITY_RESTED)
        task = SH_REST;
    else if ((boss = ShFindBoss(home)) != nullptr && sane > SH_SANITY_BOSS && F<float>(Vars(bot), EV_HEALTH) > 40.0f)
        task = SH_BOSS;
    else if (sane < SH_SANITY_HOME || full || (left >= 0 && left < SH_NIGHT_NEAR))
        task = atHome ? (sane < 0.85f ? SH_REST : SH_DEFEND) : SH_HOME;
    else if (damaged && (st.slot % 3 == 0 || damagedDist < 600.0f))
        task = SH_REPAIR;
    else
    {
        unsigned char* p = (unsigned char*)PrivateOf(bot);
        int money = p && Readable(p + SHP_MONEY, 4) ? *(int*)(p + SHP_MONEY) : 0;
        if (money >= SH_BUY_MONEY && b.boughtDay != ShDay(rules))
            task = SH_BUY;
    }
    if (task == SH_REPAIR)
        object = damaged;
    else if (task == SH_BOSS)
        object = boss;
    else if (task == SH_BUY)
    {
        void* zone = nullptr;
        float best = 1e9f;
        ForEntitiesNear(me, 16384.0f, [&](void* e) {
            const char* cls = ClassName(e);
            float d = Len(Center(e) - me);
            if (cls && !strcmp(cls, "shelter_team_buyzone") && d < best)
            {
                best = d;
                zone = e;
            }
        });
        if (!zone)
        {
            b.boughtDay = ShDay(rules); // no buy zone on this map
            task = SH_GATHER;
        }
        object = zone;
    }
    if (task == SH_GATHER)
    {
        // keep its resource while it is up and reachable, else the closest free one
        if (b.task == SH_GATHER && b.object && ShResourceUp(b.object) && now - b.objectSince < SH_GIVE_UP)
            object = b.object;
        else
        {
            if (b.object && b.task == SH_GATHER && ShResourceUp(b.object))
            {
                g_shBanned[b.object] = now + 60.0f; // not reached in time
                static int reports = 0;
                if (reports++ < 30)
                    Log("Shelter: '%s' couldn't reach a %s, leaving it for a minute", String(F<int>(Vars(bot), EV_NETNAME)), ClassName(b.object));
            }
            std::set<void*> taken;
            for (auto& kv : g_shBots)
                if (kv.first != bot && kv.second.task == SH_GATHER && kv.second.object)
                    taken.insert(kv.second.object);
            float best = 1e9f;
            ForEntitiesNear(home, SH_GATHER_RANGE, [&](void* e) {
                if (!ShResourceUp(e) || taken.count(e))
                    return;
                auto ban = g_shBanned.find(e);
                if (ban != g_shBanned.end() && now < ban->second)
                    return;
                float d = Len(Center(e) - me);
                if (d < best)
                {
                    best = d;
                    object = e;
                }
            });
            if (!object)
                task = carries ? SH_HOME : SH_DEFEND;
        }
    }
    if (task != b.task || object != b.object)
    {
        b.objectSince = now;
        b.lastObjectHp = -1;
        b.swings = 0;
    }
    b.task = task;
    b.object = object;

    // where to go
    b.hasGoal = true;
    switch (task)
    {
    case SH_GATHER:
    case SH_REPAIR:
    {
        V3 c = ShClosest(me, object);
        b.goal = V3{ c.x, c.y, me.z };
        b.arrive = 28.0f;
        break;
    }
    case SH_BUY:
        b.goal = Center(object);
        b.arrive = 24.0f;
        break;
    case SH_BOSS:
        b.goal = Origin(object);
        b.arrive = 350.0f;
        break;
    case SH_HOME:
        b.goal = home;
        b.arrive = SH_HOME_RADIUS * 0.5f;
        break;
    default: // rest, defend: somewhere in the shelter, another spot now and then
        if (now >= b.roamAt || Len2D(b.roam - home) > SH_HOME_RADIUS)
        {
            b.roamAt = now + 6.0f + Rnd() * 8.0f;
            b.roam = PtnNavPoint(home, SH_HOME_RADIUS * 0.8f);
        }
        b.goal = b.roam;
        b.arrive = 50.0f;
        break;
    }
}

// runs for every bot each frame (from the bot hook, after its buying): decisions, weapons, skills, buying
static void ShelterBotThink(void* bot, BotState& st)
{
    unsigned char* rules = ShRules();
    if (!rules)
        return;
    float now = Now();
    if (g_shLevel != g_nav.level)
    {
        g_shLevel = g_nav.level;
        g_shBots.clear();
        g_shBanned.clear();
    }
    ShBot& b = g_shBots[bot];
    // (the scenario modes' buying at each spawn stays: it gets a pistol there; primaries only sell at the buy zones)
    if (st.newLife)
    {
        b.task = SH_NONE;
        b.object = nullptr;
    }
    if (!Alive(bot))
        return;
    V3 me = Origin(bot);
    if (now >= b.next || now < b.next - 5.0f)
    {
        b.next = now + SH_THINK;
        bool orderedAway = st.ordered && st.order != ORDER_INDEPENDENT;
        if (!orderedAway)
            ShDecide(rules, bot, st, b, me, now);
        else
            b.hasGoal = false;
        if (now >= b.nextSkill)
        {
            b.nextSkill = now + 3.0f + Rnd() * 3.0f;
            ShSpendSkill(bot);
        }
        // at a buy zone: the shared buying runs now (UpdateBuying, st.buyAt)
        if (b.task == SH_BUY && b.object && ShInBox(me, b.object, 16.0f))
        {
            b.boughtDay = ShDay(rules);
            st.buyAt = now;
            st.buyTries = 0;
            static int reports = 0;
            unsigned char* p = (unsigned char*)PrivateOf(bot);
            if (reports++ < 40)
                Log("Shelter: '%s' buys at a buy zone (money %d)", String(F<int>(Vars(bot), EV_NETNAME)), p ? *(int*)(p + SHP_MONEY) : -1);
        }
        // the axe for hitting, a gun for fighting
        bool hitting = (b.task == SH_GATHER || b.task == SH_REPAIR) && b.object && Len(ShClosest(me, b.object) - me) < 300.0f && !st.target;
        int weapon = ShWeaponId(bot);
        if (now >= b.switchAt)
        {
            if (hitting && weapon != SH_KNIFE_ID)
            {
                FakeCommand(bot, "weapon_knife");
                b.switchAt = now + 1.0f;
            }
            else if (!hitting && weapon == SH_KNIFE_ID && (st.target || st.threat || ShNight(rules)))
            {
                FakeCommand(bot, b.slotTry++ % 2 ? "slot2" : "slot1");
                b.switchAt = now + 0.6f;
            }
        }
        if (now - b.lastReport > 20.0f)
        {
            b.lastReport = now;
            static int reports = 0;
            unsigned char* p = (unsigned char*)PrivateOf(bot);
            if (reports++ < 300)
                Log("Shelter: '%s' %s%s, sanity %.0f/%.0f, carries %d/%d wood %d/%d iron, money %d, weapon %d, skill points %d", String(F<int>(Vars(bot), EV_NETNAME)),
                    orderedAway ? "under orders" : g_shTaskNames[b.task], b.object ? (std::string(" ") + ClassName(b.object)).c_str() : "",
                    ShSanity(rules, bot), b.maxSanity, ShCarried(rules, bot, 0), ShCarryMax(rules, bot, 0), ShCarried(rules, bot, 1),
                    ShCarryMax(rules, bot, 1), p ? *(int*)(p + SHP_MONEY) : -1, weapon, p ? *(short*)(p + SHP_SKILL_POINTS) : -1);
        }
    }
}

// Drive's hooks. ShelterGoal: where the bot goes when no player ordered it around (true: handled, wantMove set)
static bool ShelterGoal(void* bot, BotState& st, V3 pos, V3& goal, float& arrive, bool& wantMove)
{
    if (!ShRules())
        return false;
    auto it = g_shBots.find(bot);
    if (it == g_shBots.end() || !it->second.hasGoal)
        return false;
    ShBot& b = it->second;
    goal = b.goal;
    arrive = b.arrive;
    wantMove = Len2D(goal - pos) >= arrive || fabsf(goal.z - pos.z) >= 64.0f;
    return true;
}

// where it looks while hitting something (nothing to shoot): at the closest point of the thing
static bool ShelterLook(void* bot, BotState& st, V3 eye, float& yaw, float& pitch)
{
    if (st.target || !ShRules())
        return false;
    auto it = g_shBots.find(bot);
    if (it == g_shBots.end())
        return false;
    ShBot& b = it->second;
    if ((b.task != SH_GATHER && b.task != SH_REPAIR) || !b.object || Len(ShClosest(eye, b.object) - eye) > 220.0f)
        return false;
    V3 c = ShClosest(eye, b.object);
    V3 mid = Center(b.object);
    V3 at = V3{ c.x * 0.7f + mid.x * 0.3f, c.y * 0.7f + mid.y * 0.3f, c.z };
    yaw = YawOf(at - eye);
    pitch = PitchOf(at - eye);
    return true;
}

// whether it swings the axe now: holding it, in reach, looking at it
static bool ShelterSwing(void* bot, BotState& st, float yaw, float pitch)
{
    if (st.target || !ShRules())
        return false;
    auto it = g_shBots.find(bot);
    if (it == g_shBots.end())
        return false;
    ShBot& b = it->second;
    if ((b.task != SH_GATHER && b.task != SH_REPAIR) || !b.object || ShWeaponId(bot) != SH_KNIFE_ID)
        return false;
    V3 eye = Eye(bot);
    V3 c = ShClosest(eye, b.object);
    if (Len(c - eye) > SH_REACH)
        return false;
    float wantYaw = YawOf(c - eye), wantPitch = PitchOf(c - eye);
    if (Len(c - eye) > 24.0f && (fabsf(AngleDiff(wantYaw, yaw)) > 25.0f || fabsf(wantPitch - pitch) > 30.0f))
        return false;
    float now = Now();
    if (!b.swings++)
        b.swingSince = now;
    // the axe is tapped (held, it was seen not to swing again): 0.3 s down, a moment up
    if (fmodf(now - b.swingSince, 0.4f) > 0.3f)
        return false;
    // a building that doesn't get better after a while of hitting: the game doesn't take these hits (logged once)
    if (b.task == SH_REPAIR)
    {
        float hp = F<float>(Vars(b.object), EV_HEALTH);
        if (b.lastObjectHp < 0)
            b.lastObjectHp = hp;
        if (now - b.swingSince > 6.0f)
        {
            static int reports = 0;
            if (reports++ < 20)
                Log("Shelter: '%s' repairs a %s: health %.0f -> %.0f of %.0f in %.0f s", String(F<int>(Vars(bot), EV_NETNAME)), ClassName(b.object),
                    b.lastObjectHp, hp, F<float>(Vars(b.object), EV_MAX_HEALTH), now - b.swingSince);
            b.swingSince = now;
            b.lastObjectHp = hp;
        }
    }
    return true;
}

// the mode's state every 15 s (log)
static void ShelterWatch()
{
    static float next = 0;
    unsigned char* rules = ShRules();
    if (!rules || !g_pGlobals || !*g_pGlobals)
        return;
    float now = Now();
    if (now >= next - 60.0f && now < next)
        return;
    next = now + 15.0f;
    static int reports = 0;
    if (reports++ >= 200)
        return;
    int monsters = 0, resources = 0, minerals = 0, buildings = 0, damaged = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* cls = ClassName(e);
        if (IsLivingMonster(e) && !IsIgnored(cls))
            monsters++;
        else if (cls && !strcmp(cls, "zsht_resource"))
            resources += ShResourceUp(e);
        else if (cls && !strcmp(cls, "shelter_team_mineral"))
            minerals += ShResourceUp(e);
        else if (ShBuilding(e, 2) || ShBuilding(e, 1))
        {
            buildings++;
            damaged += F<float>(Vars(e), EV_HEALTH) < F<float>(Vars(e), EV_MAX_HEALTH);
        }
    });
    int t = 2;
    Log("Shelter: day %d %s (phase %d, %.0f s left); storage wood %d/%d iron %d/%d power %d/%d; %d monsters, %d wood piles, %d rocks up, "
        "%d buildings (%d damaged)", ShDay(rules), ShNight(rules) ? "night" : "day", *(int*)(rules + SH_PHASE), ShPhaseLeft(rules),
        *(int*)(rules + SH_STORAGE + t * 0xc), *(int*)(rules + SH_STORAGE_MAX + t * 0xc), *(int*)(rules + SH_STORAGE + t * 0xc + 4),
        *(int*)(rules + SH_STORAGE_MAX + t * 0xc + 4), *(int*)(rules + SH_STORAGE + t * 0xc + 8), *(int*)(rules + SH_STORAGE_MAX + t * 0xc + 8),
        monsters, resources, minerals, buildings, damaged);
}
