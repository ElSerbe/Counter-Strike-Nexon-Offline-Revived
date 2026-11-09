// botassist: Zombie Shelter (team, mode 28; rules CSOGameRulesSHELTERTeam, vtable 0x117a1ba8, its event version 0x117a2d1c).
// Included by botassist.cpp at this point (one translation unit: it uses the shared helpers defined before it). Nothing here
// runs outside the mode: ShRules() is null unless GameMode() == MODE_SHELTER and the rules are the shelter's.
//
// The mode is the New Zombie Shelter (https://cso.fandom.com/wiki/New_Zombie_Shelter): Co-op (one shelter against the
// zombies and the bosses, maps *_pve) and Team Match (two teams, destroy the other team's shelter, maps *_pvp). Days and
// nights: by day the players hit wood ("zsht_resource") and iron ("shelter_team_mineral") with the axe, one hit one unit,
// carry it home and put it in the storage, kill zombies for money, buy at the black market ("shelter_team_buyzone") and
// build: the B menu's buildings are paid from the team's storage or from what the builder carries, some only on the map's
// fixed spots (watchtowers, worktable, generators, communication tower, concrete refinery), the rest anywhere in the
// build zones (turrets, walls, traps, storage...). The shelter goes up a tier (2 needs the worktable, 3 the communication
// tower, 4 the refinery). Away from the shelter the mental gauge drops. Each day gives skill points (combat, survival,
// engineer). At night the zombies attack the shelter, whose buildings the players repair with the axe.
//
// What the game keeps (all in mp.dll, the game's own rules run the mode offline):
//   rules +0x214 phase counter (day n = phases (2n-1, 2n)), +0x218 night (bool), +0x228 end of the phase (float, seconds)
//   rules +0x27c + team*0xc + type*4: the team's storage (type 0 wood, 1 iron, 2 power), +0x2a0 + ...: its capacity
//   rules +0x14d0 + index*0xc + type*4: what a player carries (index = entity index), +0x1590 + ...: how much they can carry
//   rules +0x16b0 + shelter*4: the shelters (CBaseEntity*), a player's shelter at player +0x31be (short, -1 none);
//     a shelter +0x100: its building row (tier at +4)
//   rules +0x16bc + index*4: a player's mental gauge (float); rules +0x26a4 + spot*4: what stands on a fixed spot
//   player +0x32ee skill points (short), +0x32a6 + id*2 skill levels; player +0xca8 money
//   a building (CBaseEntity*) +0xd4: its row of the building table (resource/shelterteam/zsht_building.cso, kBuild)
//   the tables: *(0x121d3ec4) + 0x1f0, vfunc 0x310(id) a building row (+4 tier, +0xc skill, +0x10 building needed,
//     +0x24 fixed-spot building, +0x8c its spot type), vfunc 0x318(spot) a fixed spot (+8 type, +0xc x, +0x10 y, +0x14 angle)
//   A resource takes "damage" only from the knife slot (weapon id 29 = the shelter axe): zsht_resource +0xdc hits left;
//   a mineral +0xec is up. A building takes a teammate's axe hit as a repair or as construction work (0x10894e40).
//   The menus send client commands to rules vfunc 0x24 (0x108396b0), which reads its arguments straight from the engine,
//   so ShelterCommand swaps the engine's Cmd_Args/Argv/Argc for its own around the call:
//     zsht_warehouse <type> <put 0..5> <take 0..5>   carried <-> storage (no distance check)
//     zsht_RequestSkillup <0..32>                    a skill point (0x10844830 checks the tree)
//     zsht_StaticBuildConfirm <building> <spot>      a building on a fixed spot (checks type, tier, skill, cost)
//     zsht_BuildConfirm <building> <x> <y> <z> <angle>  a building anywhere (vfunc 0x480 checks the place)
//     zsht_upgradeshelter <building>                 the shelter's next tier (25, 26, 29)
//     zsht_RequestRescue                             the rescue (communication tower, every player asks)
//
// What the bots do (Drive moves and aims them; ShelterGoal, ShelterLook, ShelterSwing, ShelterHoldFire and ShelterTarget
// are its hooks). Each bot has a job by its place in the team: builder (engineer skills), gatherer (survival) or fighter
// (combat). By day they gather, go home when full, low on mental gauge or when the night is near, put everything in the
// storage, rest, buy at the black market, repair, build (the builder: the shelter's tiers, the fixed spots, turrets and
// storage with the team's resources) and go after a boss with a gun. At night they stay in the shelter, shoot and repair.
// With the axe in hand they never "shoot" from afar: they switch to a gun, or close in only on a zombie that is near.
// On Team Match maps they also fight the other team and, from day 3, the fighters go for the other team's shelter.
// A player's orders: the shelter orders menu (radio3, the C key, or "/orders" in the chat) or the chat words /gather,
// /defend, /repair, /build, /free; the radio's follow / hold still work as everywhere.
static const int MODE_SHELTER = 28;
static const DWORD MP_SHT_RULES_VT = 0x117a1ba8, MP_SHT_EVENT_RULES_VT = 0x117a2d1c;
static const DWORD MP_SHT_TABLES = 0x121d3ec4, MP_SHOWMENU = 0x113f8940, MP_GMSG_TEXTMSG = 0x121d28b8;
static const int SH_RULES_SIZE = 0x2914;
static const int SH_PHASE = 0x214, SH_NIGHT = 0x218, SH_PHASE_END = 0x228, SH_STORAGE = 0x27c, SH_STORAGE_MAX = 0x2a0;
static const int SH_CARRIED = 0x14d0, SH_CARRY_MAX = 0x1590, SH_SHELTERS = 0x16b0, SH_SANITY = 0x16bc, SH_SPOTS = 0x26a4;
static const int SHP_SHELTER = 0x31be, SHP_SKILL_POINTS = 0x32ee, SHP_SKILLS = 0x32a6, SHP_MONEY = 0xca8, SHP_ACTIVE_ITEM = 0x101c, SH_WEAPON_ID = 0xe4;
static const int SH_BUILDING_ROW = 0xd4, SH_SHELTER_ROW = 0x100, SH_TABLE_OBJECT = 0x1f0, SH_TABLE_BUILDING = 0x310, SH_TABLE_SPOT = 0x318;
static const int SH_KNIFE_ID = 29, SH_CLIENTCOMMAND_VFUNC = 0x24, EV_MAX_HEALTH = 0x1dc;
static const int ENG_CMD_ARGS = 87, ENG_CMD_ARGC = 89;
static const float SH_THINK = 0.4f, SH_HOME_RADIUS = 260.0f, SH_GATHER_RANGE = 3200.0f, SH_REACH = 62.0f;
static const float SH_SANITY_HOME = 0.4f, SH_SANITY_BOSS = 0.2f, SH_SANITY_RESTED = 0.95f, SH_NIGHT_NEAR = 45.0f;
static const float SH_REPAIR_BELOW = 0.8f, SH_REPAIR_RANGE = 1600.0f, SH_BUY_MONEY = 3500.0f, SH_GIVE_UP = 25.0f;
static const float SH_MELEE_RANGE = 110.0f, SH_MELEE_CHASE = 380.0f, SH_BUILD_REACH = 170.0f;

// resource/shelterteam/zsht_building.cso: id, shelter tier, category, skill needed, building needed, spot type (0 = build
// anywhere), wood, iron, power, from the storage (else from what the builder carries), class
struct ShBuildRow
{
    int id, tier, category, skill, needs, spot, wood, iron, power, global;
    const char* cls;
};
static const ShBuildRow kBuild[] = {
    { 0, 1, 1, -1, -1, 1, 20, 5, 0, 1, "shelter_team_tower" },          // observation post
    { 1, 2, 1, -1, -1, 1, 25, 15, 10, 1, "shelter_team_tower" },        // reconnaissance post
    { 2, 3, 1, -1, 16, 1, 30, 35, 10, 1, "shelter_team_tower" },        // battle post
    { 3, 1, 2, -1, -1, 0, 4, 4, 0, 0, "shelter_team_wall" },
    { 4, 2, 2, -1, -1, 0, 6, 6, 0, 0, "shelter_team_wall" },
    { 5, 3, 2, -1, 16, 0, 10, 12, 0, 0, "shelter_team_wall" },
    { 6, 1, 3, -1, -1, 0, 6, 6, 10, 0, "shelter_team_door" },
    { 7, 3, 3, -1, 16, 0, 15, 15, 10, 0, "shelter_team_door" },
    { 8, 1, 4, -1, -1, 0, 4, 4, 0, 0, "shelter_team_some" },            // alarm
    { 9, 2, 5, -1, -1, 0, 7, 11, 8, 0, "shelter_team_thron" },
    { 10, 1, 5, -1, -1, 0, 1, 2, 0, 0, "shelter_team_mine" },
    { 11, 3, 5, -1, -1, 0, 4, 5, 0, 0, "shelter_team_claymore" },
    { 12, 1, 6, -1, -1, 0, 6, 6, 12, 0, "shelter_team_turret" },        // gun tower
    { 13, 2, 6, -1, -1, 0, 12, 8, 13, 0, "shelter_team_flame_turret" },
    { 14, 3, 6, -1, -1, 0, 18, 13, 20, 0, "shelter_team_turret" },      // gatling tower
    { 15, 1, 7, 18, -1, 2, 22, 18, 15, 1, "shelter_team_worktable" },
    { 16, 3, 7, 20, 17, 4, 40, 45, 25, 1, "shelter_team_some" },        // concrete refinery
    { 17, 2, 7, 19, 15, 5, 30, 32, 20, 1, "shelter_team_comm_tower" },
    { 18, 1, 8, -1, -1, 0, 12, 6, 10, 0, "shelter_team_warehouse" },
    { 19, 1, 9, -1, -1, 6, 5, 5, 0, 0, "shelter_team_generator" },
    { 20, 2, 9, -1, -1, 6, 9, 9, 0, 0, "shelter_team_generator" },
    { 21, 3, 9, -1, -1, 6, 15, 13, 0, 0, "shelter_team_generator" },
    { 22, 1, 10, -1, -1, 0, 10, 10, 20, 0, "shelter_team_field_bed" },
    { 23, 1, 10, 12, -1, 0, 1, 1, 0, 0, "shelter_team_campfire" },
    { 24, 1, 11, -1, -1, 0, 0, 0, 0, 1, nullptr },                      // shelter tier 1
    { 25, 2, 11, -1, 15, 0, 40, 35, 15, 1, nullptr },                   // shelter tier 2
    { 26, 3, 11, -1, 17, 0, 70, 70, 20, 1, nullptr },                   // shelter tier 3
    { 27, 2, 5, -1, -1, 0, 5, 5, 0, 0, "shelter_team_icebomb" },
    { 28, 3, 6, -1, 16, 0, 15, 25, 35, 0, "shelter_team_mortar_turret" },
    { 29, 4, 11, -1, 16, 0, 125, 125, 30, 1, nullptr },                 // shelter tier 4
    { 30, 4, 1, -1, -1, 1, 50, 60, 10, 1, "shelter_team_tower" },       // shop post
    { 31, 4, 6, -1, -1, 0, 25, 28, 28, 0, "shelter_team_laser_turret" },
    { 32, 4, 6, -1, -1, 0, 30, 25, 28, 0, "shelter_team_electronic_turret" },
    { 33, 4, 9, -1, -1, 6, 20, 22, 0, 0, "shelter_team_generator" },
};
static const int SH_UPGRADES[] = { 25, 26, 29 }; // the shelter's tiers 2, 3, 4
// a formatted string for the log (four rotating buffers)
static const char* ShFmt(const char* fmt, ...)
{
    static char bufs[4][256];
    static int next = 0;
    char* b = bufs[next++ & 3];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, 256, fmt, ap);
    va_end(ap);
    return b;
}

static const ShBuildRow* ShRow(int id)
{
    for (auto& r : kBuild)
        if (r.id == id)
            return &r;
    return nullptr;
}

enum ShTask
{
    SH_NONE,
    SH_GATHER,  // hitting a resource
    SH_HOME,    // back to the shelter (full pockets, low mental gauge, night soon)
    SH_REST,    // in the shelter until its mental gauge is back
    SH_REPAIR,  // hitting a damaged (or new, unfinished) building of its team
    SH_BUY,     // to a buy zone
    SH_BOSS,    // after the boss
    SH_DEFEND,  // around the shelter (night, or nothing to do)
    SH_BUILD,   // to where it builds something
    SH_RAID,    // Team Match: at the other team's shelter
};
static const char* const g_shTaskNames[] = { "none", "gather", "home", "rest", "repair", "buy", "boss", "defend", "build", "raid" };

// what a player told the team's bots (the orders menu, the chat)
enum ShOrder
{
    SHO_FREE,   // they decide (their job)
    SHO_GATHER,
    SHO_DEFEND,
    SHO_REPAIR,
    SHO_BUILD,
};
static const char* const g_shOrderNames[] = { "free", "gather", "defend", "repair", "build" };
static ShOrder g_shOrder[4] = {}; // by team

enum ShRole
{
    SHR_BUILDER,
    SHR_GATHERER,
    SHR_FIGHTER,
};
static const char* const g_shRoleNames[] = { "builder", "gatherer", "fighter" };

// a build the builder goes for
struct ShPlan
{
    int kind = 0;       // 0 none, 1 fixed spot, 2 anywhere, 3 shelter tier
    int building = -1, spot = -1;
    V3 at{};
    int countBefore = 0; // anywhere: how many of its class the team had
    void* before = nullptr; // fixed spot: what stood there
};

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
    // the gun: when it held the axe after trying its gun slots, it has none (or no ammo) for a while
    float noGunUntil = 0, gunTriedAt = 0;
    int gunTries = 0;
    // building
    ShPlan plan;
    float planSince = 0, nextPlan = 0, sentAt = 0;
    bool sent = false;
};
static std::map<void*, ShBot> g_shBots;
static std::map<void*, float> g_shBanned;  // resources a bot couldn't reach: left alone until then
static std::map<long long, float> g_shBuildBan; // building*1000+spot that failed: not tried again until then
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

static int ShStorage(unsigned char* rules, int team, int type)
{
    return team >= 0 && team < 4 ? *(int*)(rules + SH_STORAGE + team * 0xc + type * 4) : 0;
}

static int ShStorageMax(unsigned char* rules, int team, int type)
{
    return team >= 0 && team < 4 ? *(int*)(rules + SH_STORAGE_MAX + team * 0xc + type * 4) : 0;
}

static int ShDay(unsigned char* rules)
{
    return (*(int*)(rules + SH_PHASE) + 1) / 2;
}

static bool ShNight(unsigned char* rules)
{
    return rules[SH_NIGHT] != 0;
}

// seconds left in this day or night, or -1 if the rules' end time doesn't look like one (a float, the game's clock)
static float ShPhaseLeft(unsigned char* rules)
{
    float left = *(float*)(rules + SH_PHASE_END) - Now();
    return left > -5.0f && left < 3600.0f ? left : -1.0f;
}

static int ShWeaponId(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    unsigned char* w = p && Readable(p + SHP_ACTIVE_ITEM, 4) ? *(unsigned char**)(p + SHP_ACTIVE_ITEM) : nullptr;
    return w && Readable(w + SH_WEAPON_ID, 4) ? *(int*)(w + SH_WEAPON_ID) : 0;
}

// the axe or another melee tool (machete, crowbar, claw hammer) in hand
static bool ShMeleeHeld(void* bot)
{
    if (ShWeaponId(bot) == SH_KNIFE_ID)
        return true;
    const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    return m && (strstr(m, "knife") || strstr(m, "axe") || strstr(m, "machete") || strstr(m, "crowbar") || strstr(m, "hammer"));
}

static int ShSkillLevel(void* bot, int id)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    return p && id >= 0 && id < 33 && Readable(p + SHP_SKILLS + id * 2, 2) ? *(short*)(p + SHP_SKILLS + id * 2) : 0;
}

// the CBaseEntity's edict (pev at +8 in this game's entities)
static void* ShEdictOf(unsigned char* ent)
{
    unsigned char* pev = ent && Readable(ent, 0x10) ? *(unsigned char**)(ent + 8) : nullptr;
    return pev && Readable(pev, 0x240) ? *(void**)(pev + EV_CONTAINING_ENTITY) : nullptr;
}

static unsigned char* ShShelterEntity(unsigned char* rules, int shelter)
{
    if (shelter < 0 || shelter > 2)
        return nullptr;
    unsigned char* e = *(unsigned char**)(rules + SH_SHELTERS + shelter * 4);
    return e && Readable(e, 0x110) ? e : nullptr;
}

static int ShShelterOf(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    return p && Readable(p + SHP_SHELTER, 2) ? *(short*)(p + SHP_SHELTER) : -1;
}

// the center of the bot's shelter, false if it has none
static bool ShHome(unsigned char* rules, void* bot, V3& home)
{
    void* edict = ShEdictOf(ShShelterEntity(rules, ShShelterOf(bot)));
    if (!edict)
        return false;
    home = Center(edict);
    return true;
}

// the shelter's tier (1..4), from its building row
static int ShShelterTier(unsigned char* rules, int shelter)
{
    unsigned char* e = ShShelterEntity(rules, shelter);
    unsigned char* row = e ? *(unsigned char**)(e + SH_SHELTER_ROW) : nullptr;
    int tier = row && Readable(row, 8) ? *(int*)(row + 4) : 1;
    return tier >= 1 && tier <= 4 ? tier : 1;
}

// Team Match: the other team's shelter, null on a co-op map
static void* ShEnemyShelter(unsigned char* rules, void* bot)
{
    int mine = ShShelterOf(bot);
    for (int s = 0; s < 3; s++)
        if (s != mine)
            if (void* e = ShEdictOf(ShShelterEntity(rules, s)))
                return F<float>(Vars(e), EV_HEALTH) > 0.0f ? e : nullptr;
    return nullptr;
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

static int ShTeamOfEntity(void* e)
{
    unsigned char* p = (unsigned char*)PrivateOf(e);
    return p && Readable(p + PLAYER_TEAM, 4) ? *(int*)(p + PLAYER_TEAM) : 0;
}

// a building of a team: a shelter_team_* entity with health (not a zone, a trigger or a missile)
static bool ShBuildingAny(void* e)
{
    const char* cls = ClassName(e);
    if (!cls || strncmp(cls, "shelter_team_", 13) || strstr(cls, "trigger") || strstr(cls, "buyzone") ||
        strstr(cls, "mineral") || strstr(cls, "missile") || strstr(cls, "ammo"))
        return false;
    unsigned char* v = Vars(e);
    return F<float>(v, EV_HEALTH) > 0.0f && F<float>(v, EV_MAX_HEALTH) > 0.0f && F<float>(v, EV_TAKEDAMAGE) != 0.0f;
}

static bool ShBuilding(void* e, int team)
{
    return ShBuildingAny(e) && ShTeamOfEntity(e) == team;
}

// Runs a shelter client command as the bot, the way the menus send it: rules vfunc 0x24 with the engine's argument functions
// pointing at ours for the time of the call
static const char* g_shArgv[8];
static int g_shArgc = 0;
static char g_shArgs[128];
static const char* __cdecl ShCmdArgv(int i) { return i >= 0 && i < g_shArgc ? g_shArgv[i] : ""; }
static int __cdecl ShCmdArgc() { return g_shArgc; }
static const char* __cdecl ShCmdArgs() { return g_shArgs; }

static bool ShelterCommand(void* bot, const char* a0, const char* a1 = nullptr, const char* a2 = nullptr, const char* a3 = nullptr,
                           const char* a4 = nullptr, const char* a5 = nullptr)
{
    unsigned char* rules = ShRules();
    void* player = PrivateOf(bot);
    if (!rules || !player)
        return false;
    const char* in[6] = { a0, a1, a2, a3, a4, a5 };
    g_shArgc = 0;
    g_shArgs[0] = 0;
    for (int i = 0; i < 6 && in[i]; i++)
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
        Log("Shelter: '%s' puts %d wood and %d iron in the storage (still carries %d / %d)", String(F<int>(Vars(bot), EV_NETNAME)),
            before[0] - ShCarried(rules, bot, 0), before[1] - ShCarried(rules, bot, 1), ShCarried(rules, bot, 0), ShCarried(rules, bot, 1));
}

// takes from the storage until it carries `wood` and `iron` (false: the storage hasn't got them)
static bool ShWithdraw(unsigned char* rules, void* bot, int wood, int iron)
{
    int want[2] = { wood, iron };
    for (int type = 0; type < 2; type++)
        for (int guard = 0; guard < 40 && ShCarried(rules, bot, type) < want[type]; guard++)
        {
            int had = ShCarried(rules, bot, type);
            char take[4];
            snprintf(take, sizeof(take), "%d", want[type] - had > 5 ? 5 : want[type] - had);
            ShelterCommand(bot, "zsht_warehouse", type ? "1" : "0", "0", take);
            if (ShCarried(rules, bot, type) <= had)
                break;
        }
    return ShCarried(rules, bot, 0) >= wood && ShCarried(rules, bot, 1) >= iron;
}

// the job: the first bot of a team builds, then gatherers and fighters in turn
static ShRole ShRoleOf(BotState& st)
{
    int k = st.slot % 3;
    return k == 0 ? SHR_BUILDER : k == 1 ? SHR_GATHERER : SHR_FIGHTER;
}

// a skill point, by job: engineer for the builder (the worktable needs Beginner Engineer), survival for gatherers, combat
// for fighters (Beginner Gun Mastery lets them use submachine guns and shotguns); the game checks the tree, the levels and
// the exclusive picks, so it tries the job's list in order, then random ones
static void ShSpendSkill(void* bot, BotState& st)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p + SHP_SKILL_POINTS, 2) || *(short*)(p + SHP_SKILL_POINTS) <= 0)
        return;
    static const int builder[] = { 18, 24, 21, 18, 19, 22, 24, 21, 20, 26, 29, 18, 19, 20, 25, 32, 23, 5, 0 };
    static const int gatherer[] = { 9, 13, 15, 9, 10, 16, 13, 9, 10, 12, 28, 11, 14, 31, 17, 5, 0 };
    static const int fighter[] = { 0, 5, 4, 0, 1, 5, 6, 0, 1, 2, 7, 3, 27, 6, 2, 30, 8, 13 };
    ShRole role = ShRoleOf(st);
    const int* list = role == SHR_BUILDER ? builder : role == SHR_GATHERER ? gatherer : fighter;
    int n = role == SHR_BUILDER ? (int)(sizeof(builder) / 4) : role == SHR_GATHERER ? (int)(sizeof(gatherer) / 4) : (int)(sizeof(fighter) / 4);
    short before = *(short*)(p + SHP_SKILL_POINTS);
    for (int i = 0; i < n + 8; i++)
    {
        char id[8];
        snprintf(id, sizeof(id), "%d", i < n ? list[i] : rand() % 33);
        ShelterCommand(bot, "zsht_RequestSkillup", id);
        if (*(short*)(p + SHP_SKILL_POINTS) < before)
        {
            static int reports = 0;
            if (reports++ < 80)
                Log("Shelter: '%s' (%s) learns skill %s (%d points left)", String(F<int>(Vars(bot), EV_NETNAME)), g_shRoleNames[role], id,
                    *(short*)(p + SHP_SKILL_POINTS));
            return;
        }
    }
}

// ---------------------------------------------------------------- building
// the game's fixed building spots of this map (zbs_sb_<map>.cso through the tables object)
struct ShSpot
{
    int id, type;
    V3 at;
};
static std::vector<ShSpot> g_shSpots;
static int g_shSpotsLevel = -1;

static void* ShTables()
{
    unsigned char* g = *(unsigned char**)(MP_SHT_TABLES + g_mpDelta);
    unsigned char* t = g && Readable(g + SH_TABLE_OBJECT, 4) ? *(unsigned char**)(g + SH_TABLE_OBJECT) : nullptr;
    return t && Readable(t, 4) ? t : nullptr;
}

static unsigned char* ShTableCall(int vfunc, int arg)
{
    void* t = ShTables();
    if (!t)
        return nullptr;
    unsigned char* r = nullptr;
    __try
    {
        typedef unsigned char*(__fastcall * GetFn)(void* self, void* edx, int id);
        r = ((GetFn)(*(void***)t)[vfunc / 4])(t, nullptr, arg);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        r = nullptr;
    }
    return r && Readable(r, 0x90) ? r : nullptr;
}

static void ShLoadSpots()
{
    if (g_shSpotsLevel == g_nav.level)
        return;
    g_shSpotsLevel = g_nav.level;
    g_shSpots.clear();
    for (int id = 1; id < 40; id++)
    {
        unsigned char* s = ShTableCall(SH_TABLE_SPOT, id);
        if (!s)
            continue;
        ShSpot spot{ id, *(int*)(s + 8), V3{ *(float*)(s + 0xc), *(float*)(s + 0x10), 0 } };
        if (spot.type < 1 || spot.type > 6 || fabsf(spot.at.x) > 16384.0f || fabsf(spot.at.y) > 16384.0f)
            continue;
        g_shSpots.push_back(spot);
    }
    std::string list;
    for (auto& s : g_shSpots)
        list += ShFmt(" %d(type %d at %.0f %.0f)", s.id, s.type, s.at.x, s.at.y);
    Log("Shelter: %d fixed building spots:%s", (int)g_shSpots.size(), list.c_str());
    // the table's own rows next to ours (a check of kBuild against the game's data)
    std::string rows;
    for (auto& r : kBuild)
        if (unsigned char* row = ShTableCall(SH_TABLE_BUILDING, r.id))
        {
            int tier = *(int*)(row + 4), skill = *(int*)(row + 0xc), needs = *(int*)(row + 0x10), spot = *(int*)(row + 0x8c);
            if (tier != r.tier || (r.spot && spot != r.spot))
                rows += ShFmt(" %d(tier %d spot %d skill %d needs %d)", r.id, tier, spot, skill, needs);
        }
    if (!rows.empty())
        Log("Shelter: building rows different from botassist's table:%s", rows.c_str());
}

// what stands on a fixed spot: its building row, -1 nothing, -2 something unknown
static int ShSpotBuilding(unsigned char* rules, int spot, void** entity = nullptr)
{
    unsigned char* e = spot > 0 && spot < 64 ? *(unsigned char**)(rules + SH_SPOTS + spot * 4) : nullptr;
    if (entity)
        *entity = e;
    if (!e || !Readable(e, SH_BUILDING_ROW + 4))
        return -1;
    int id = *(int*)(e + SH_BUILDING_ROW);
    return id >= 0 && id < 64 ? id : -2;
}

// the team's buildings by row (built or being built) and by class
static void ShTeamBuildings(int team, std::map<int, int>& byRow, std::map<std::string, int>& byClass)
{
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!ShBuilding(e, team))
            return;
        byClass[ClassName(e)]++;
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (p && Readable(p + SH_BUILDING_ROW, 4))
        {
            int id = *(int*)(p + SH_BUILDING_ROW);
            if (id >= 0 && id < 64)
                byRow[id]++;
        }
    });
}

static bool ShCanAfford(unsigned char* rules, void* bot, int team, const ShBuildRow& r)
{
    if (ShStorage(rules, team, 2) < r.power)
        return false;
    if (r.global)
        return ShStorage(rules, team, 0) >= r.wood && ShStorage(rules, team, 1) >= r.iron;
    int wood = ShCarried(rules, bot, 0) + ShStorage(rules, team, 0), iron = ShCarried(rules, bot, 1) + ShStorage(rules, team, 1);
    return wood >= r.wood && iron >= r.iron && ShCarryMax(rules, bot, 0) >= r.wood && ShCarryMax(rules, bot, 1) >= r.iron;
}

static bool ShRowAllowed(void* bot, const ShBuildRow& r, int tier, std::map<int, int>& byRow)
{
    if (r.tier > tier || (r.skill >= 0 && ShSkillLevel(bot, r.skill) <= 0))
        return false;
    return r.needs < 0 || byRow[r.needs] > 0 || (r.needs == 15 && byRow[15] > 0);
}

static bool ShBanned(int building, int spot, float now)
{
    auto it = g_shBuildBan.find(building * 1000LL + spot);
    return it != g_shBuildBan.end() && now < it->second;
}

// a place to build anywhere near the shelter: on the ground, in the open, a few steps from its walls
static bool ShFreeSpot(V3 home, V3& out)
{
    for (int tries = 0; tries < 12; tries++)
    {
        float a = Rnd() * 6.2831853f, r = 200.0f + Rnd() * 320.0f;
        V3 p{ home.x + cosf(a) * r, home.y + sinf(a) * r, home.z + 64.0f };
        TraceResult tr;
        Trace(p, p - V3{ 0, 0, 400 }, nullptr, 1, tr);
        if (tr.flFraction >= 1.0f || tr.fStartSolid)
            continue;
        V3 ground = Vec(tr.vecEndPos);
        // room around it (no wall within 48 units at chest height)
        bool open = true;
        for (int d = 0; d < 4 && open; d++)
        {
            float b = d * 1.5707963f;
            TraceResult t2;
            Trace(ground + V3{ 0, 0, 40 }, ground + V3{ cosf(b) * 48.0f, sinf(b) * 48.0f, 40 }, nullptr, 0, t2);
            open = t2.flFraction >= 1.0f;
        }
        if (open && AreaAt(ground + V3{ 0, 0, 36 }) >= 0)
        {
            out = ground;
            return true;
        }
    }
    return false;
}

// The builder's next build: the shelter's next tier, else a fixed spot (worktable, generators, communication tower, posts,
// refinery), else turrets around the shelter (more with each tier), a storage when the storage is full, a recovery bed.
static bool ShPlanBuild(unsigned char* rules, void* bot, V3 home, float now, ShPlan& plan)
{
    int team = TeamOf(bot);
    int tier = ShShelterTier(rules, ShShelterOf(bot));
    std::map<int, int> byRow;
    std::map<std::string, int> byClass;
    ShTeamBuildings(team, byRow, byClass);
    ShLoadSpots();

    // the shelter's next tier
    if (tier < 4)
    {
        const ShBuildRow* r = ShRow(SH_UPGRADES[tier - 1]);
        if (r && (r->needs < 0 || byRow[r->needs] > 0) && ShCanAfford(rules, bot, team, *r) && !ShBanned(r->id, 0, now))
        {
            plan = ShPlan{ 3, r->id, -1, home };
            return true;
        }
    }
    // the fixed spots, in this order of their type: worktable, generator, communication tower, post, refinery
    static const int typeOrder[] = { 2, 6, 5, 1, 4 };
    for (int type : typeOrder)
        for (auto& s : g_shSpots)
        {
            if (s.type != type)
                continue;
            void* standing = nullptr;
            int cur = ShSpotBuilding(rules, s.id, &standing);
            if (cur == -2)
                continue;
            const ShBuildRow* have = cur >= 0 ? ShRow(cur) : nullptr;
            const ShBuildRow* best = nullptr;
            for (auto& r : kBuild)
                if (r.spot == type && (!have || r.tier > have->tier) && ShRowAllowed(bot, r, tier, byRow) &&
                    ShCanAfford(rules, bot, team, r) && !ShBanned(r.id, s.id, now) && (!best || r.tier < best->tier))
                    best = &r;
            if (best)
            {
                plan = ShPlan{ 1, best->id, s.id, V3{ s.at.x, s.at.y, home.z } };
                plan.before = standing;
                return true;
            }
        }
    // turrets: two, and two more each tier; the best one this tier allows
    int turrets = byClass["shelter_team_turret"] + byClass["shelter_team_flame_turret"] + byClass["shelter_team_mortar_turret"] +
                  byClass["shelter_team_laser_turret"] + byClass["shelter_team_electronic_turret"];
    std::vector<int> wanted;
    if (turrets < 2 * tier)
        for (int id : { 32, 31, 28, 14, 13, 12 })
            wanted.push_back(id);
    // a storage when the storage is full (up to three), a recovery bed
    bool full = ShStorage(rules, team, 0) >= ShStorageMax(rules, team, 0) || ShStorage(rules, team, 1) >= ShStorageMax(rules, team, 1);
    if (full && byClass["shelter_team_warehouse"] < 3)
        wanted.insert(wanted.begin(), 18);
    if (byClass["shelter_team_field_bed"] == 0)
        wanted.push_back(22);
    for (int id : wanted)
    {
        const ShBuildRow* r = ShRow(id);
        if (!r || !ShRowAllowed(bot, *r, tier, byRow) || !ShCanAfford(rules, bot, team, *r) || ShBanned(id, 0, now))
            continue;
        V3 at;
        if (!ShFreeSpot(home, at))
            return false;
        plan = ShPlan{ 2, id, -1, at };
        plan.countBefore = byClass[r->cls];
        return true;
    }
    return false;
}

// at the place: what it carries for the build, then the game's command; the result is checked on the next decisions
static void ShSendBuild(unsigned char* rules, void* bot, ShBot& b, float now)
{
    const ShBuildRow* r = ShRow(b.plan.building);
    if (!r)
        return;
    int team = TeamOf(bot);
    if (!r->global && !ShWithdraw(rules, bot, r->wood, r->iron))
    {
        g_shBuildBan[r->id * 1000LL + (b.plan.spot > 0 ? b.plan.spot : 0)] = now + 60.0f;
        b.plan.kind = 0;
        return;
    }
    char id[8], spot[8];
    snprintf(id, sizeof(id), "%d", r->id);
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    if (b.plan.kind == 3)
    {
        ShelterCommand(bot, "zsht_upgradeshelter", id);
        Log("Shelter: '%s' upgrades the shelter (row %d, storage %d wood %d iron %d power)", name, r->id, ShStorage(rules, team, 0),
            ShStorage(rules, team, 1), ShStorage(rules, team, 2));
    }
    else if (b.plan.kind == 1)
    {
        snprintf(spot, sizeof(spot), "%d", b.plan.spot);
        ShelterCommand(bot, "zsht_StaticBuildConfirm", id, spot);
        Log("Shelter: '%s' builds row %d (%s) on spot %d", name, r->id, r->cls ? r->cls : "", b.plan.spot);
    }
    else
    {
        char x[16], y[16], z[16];
        snprintf(x, sizeof(x), "%.0f", b.plan.at.x);
        snprintf(y, sizeof(y), "%.0f", b.plan.at.y);
        snprintf(z, sizeof(z), "%.0f", b.plan.at.z);
        ShelterCommand(bot, "zsht_BuildConfirm", id, x, y, z, ShFmt("%d", rand() % 4 * 90));
        Log("Shelter: '%s' builds row %d (%s) at %s %s %s", name, r->id, r->cls, x, y, z);
    }
    b.sent = true;
    b.sentAt = now;
}

// after the command: built (then it works on it with the axe until it's finished) or banned for a while
static void ShCheckBuild(unsigned char* rules, void* bot, ShBot& b, float now)
{
    if (!b.sent || now - b.sentAt < 0.8f)
        return;
    const ShBuildRow* r = ShRow(b.plan.building);
    bool ok = false;
    void* placed = nullptr;
    if (b.plan.kind == 3)
        ok = ShShelterTier(rules, ShShelterOf(bot)) >= (r ? r->tier : 9);
    else if (b.plan.kind == 1)
    {
        void* now_ = nullptr;
        ok = ShSpotBuilding(rules, b.plan.spot, &now_) == b.plan.building;
        if (ok)
            placed = ShEdictOf((unsigned char*)now_);
    }
    else if (r)
    {
        std::map<int, int> byRow;
        std::map<std::string, int> byClass;
        ShTeamBuildings(TeamOf(bot), byRow, byClass);
        ok = byClass[r->cls] > b.plan.countBefore;
    }
    static int reports = 0;
    if (reports++ < 120)
        Log("Shelter: '%s' build of row %d %s", String(F<int>(Vars(bot), EV_NETNAME)), b.plan.building, ok ? "done" : "refused by the game");
    if (!ok)
        g_shBuildBan[b.plan.building * 1000LL + (b.plan.spot > 0 ? b.plan.spot : 0)] = now + (b.plan.kind == 2 ? 20.0f : 90.0f);
    b.sent = false;
    b.plan.kind = 0;
    b.nextPlan = now + (ok ? 2.0f : 4.0f);
    if (placed)
    {
        b.task = SH_REPAIR; // a fixed-spot building starts unfinished: work on it
        b.object = placed;
    }
}

// the boss: a living monster with boss health, anywhere on the map (a big ordinary zombie isn't one)
static void* ShFindBoss(V3 around)
{
    void* best = nullptr;
    float bestHp = 0;
    ForEntitiesNear(around, 16384.0f, [&](void* e) {
        if (!IsLivingMonster(e) || IsIgnored(ClassName(e)))
            return;
        unsigned char* v = Vars(e);
        float hp = F<float>(v, EV_MAX_HEALTH) > F<float>(v, EV_HEALTH) ? F<float>(v, EV_MAX_HEALTH) : F<float>(v, EV_HEALTH);
        if (hp >= 20000.0f && hp > bestHp)
        {
            bestHp = hp;
            best = e;
        }
    });
    return best;
}

// the closest damaged building of the team around `from`
static void* ShFindDamaged(int team, V3 from, V3 me, float range, float below, float& dist)
{
    void* damaged = nullptr;
    dist = 1e9f;
    ForEntitiesNear(from, range, [&](void* e) {
        if (!ShBuilding(e, team))
            return;
        unsigned char* v = Vars(e);
        if (F<float>(v, EV_HEALTH) >= F<float>(v, EV_MAX_HEALTH) * below)
            return;
        float d = Len(ShClosest(me, e) - me);
        if (d < dist)
        {
            dist = d;
            damaged = e;
        }
    });
    return damaged;
}

// what the bot does now; sets b.task, b.object and where to go
static void ShDecide(unsigned char* rules, void* bot, BotState& st, ShBot& b, V3 me, float now)
{
    V3 home;
    bool hasHome = ShHome(rules, bot, home);
    if (!hasHome)
        home = me;
    int team = TeamOf(bot);
    ShOrder order = team >= 0 && team < 4 ? g_shOrder[team] : SHO_FREE;
    ShRole role = ShRoleOf(st);
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
    bool building = b.plan.kind != 0 && (b.task == SH_BUILD || b.sent);
    if (atHome && carries && !building)
        ShDeposit(rules, bot);
    bool hasGun = now >= b.noGunUntil;

    float damagedDist = 1e9f;
    void* damaged = ShFindDamaged(team, home, me, order == SHO_REPAIR ? SH_REPAIR_RANGE * 1.5f : SH_REPAIR_RANGE,
                                  order == SHO_REPAIR ? 0.999f : night ? 0.9f : SH_REPAIR_BELOW, damagedDist);

    ShTask task = SH_GATHER;
    void* object = nullptr;
    void* boss = nullptr;
    void* enemyShelter = ShEnemyShelter(rules, bot);
    bool wantBuild = (order == SHO_BUILD || (order == SHO_FREE && role == SHR_BUILDER)) && !night;
    if (wantBuild && b.plan.kind == 0 && now >= b.nextPlan)
    {
        b.nextPlan = now + 3.0f;
        if (ShPlanBuild(rules, bot, home, now, b.plan))
        {
            b.planSince = now;
            static int reports = 0;
            if (reports++ < 120)
                Log("Shelter: '%s' plans row %d (%s)%s", String(F<int>(Vars(bot), EV_NETNAME)), b.plan.building,
                    b.plan.kind == 3 ? "shelter tier" : ShRow(b.plan.building)->cls, b.plan.spot > 0 ? ShFmt(" on spot %d", b.plan.spot) : "");
        }
    }
    if (b.plan.kind && now - b.planSince > 45.0f && !b.sent)
    {
        g_shBuildBan[b.plan.building * 1000LL + (b.plan.spot > 0 ? b.plan.spot : 0)] = now + 60.0f; // not reached in time
        b.plan.kind = 0;
    }

    if (sane < SH_SANITY_BOSS && !atHome)
        task = SH_HOME;
    else if (order == SHO_DEFEND)
        task = damaged && damagedDist < 700.0f && !st.target ? SH_REPAIR : SH_DEFEND;
    else if (order == SHO_REPAIR)
        task = damaged ? SH_REPAIR : SH_DEFEND;
    else if (night && order != SHO_GATHER)
        task = damaged && damagedDist < 1200.0f && !st.target ? SH_REPAIR : SH_DEFEND;
    else if (b.task == SH_REST && atHome && sane < SH_SANITY_RESTED && order == SHO_FREE)
        task = SH_REST;
    else if (wantBuild && b.plan.kind)
        task = SH_BUILD;
    else if (order == SHO_BUILD)
        task = damaged ? SH_REPAIR : (carries && full ? SH_HOME : SH_GATHER); // nothing to build yet: what it needs
    else if (order == SHO_FREE && hasGun && (boss = ShFindBoss(home)) != nullptr && sane > SH_SANITY_BOSS &&
             F<float>(Vars(bot), EV_HEALTH) > 40.0f && role != SHR_GATHERER)
        task = SH_BOSS;
    else if (sane < SH_SANITY_HOME || full || (left >= 0 && left < SH_NIGHT_NEAR && order != SHO_GATHER))
        task = atHome ? (sane < 0.85f ? SH_REST : SH_DEFEND) : SH_HOME;
    else if (order == SHO_FREE && enemyShelter && role == SHR_FIGHTER && hasGun && ShDay(rules) >= 3)
        task = SH_RAID;
    else if (order == SHO_FREE && damaged && (role == SHR_BUILDER || damagedDist < 600.0f))
        task = SH_REPAIR;
    else if (order == SHO_FREE)
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
    else if (task == SH_RAID)
        object = enemyShelter;
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
    case SH_RAID:
        b.goal = Center(object);
        b.arrive = 500.0f;
        break;
    case SH_BUILD:
        b.goal = b.plan.at;
        b.arrive = b.plan.kind == 3 ? SH_HOME_RADIUS * 0.6f : SH_BUILD_REACH * 0.6f;
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

// runs for every bot each frame (from the bot hook, after its buying): decisions, weapons, skills, buying, building
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
        g_shBuildBan.clear();
        for (auto& o : g_shOrder)
            o = SHO_FREE;
    }
    ShBot& b = g_shBots[bot];
    // (the scenario modes' buying at each spawn stays: it gets a pistol there; primaries only sell at the buy zones)
    if (st.newLife)
    {
        b.task = SH_NONE;
        b.object = nullptr;
        b.noGunUntil = 0;
        b.gunTries = 0;
        b.plan.kind = 0;
        b.sent = false;
    }
    if (!Alive(bot))
        return;
    V3 me = Origin(bot);
    if (now >= b.next || now < b.next - 5.0f)
    {
        b.next = now + SH_THINK;
        bool orderedAway = st.ordered && st.order != ORDER_INDEPENDENT;
        ShCheckBuild(rules, bot, b, now);
        if (!orderedAway)
            ShDecide(rules, bot, st, b, me, now);
        else
            b.hasGoal = false;
        if (now >= b.nextSkill)
        {
            b.nextSkill = now + 3.0f + Rnd() * 3.0f;
            ShSpendSkill(bot, st);
        }
        // at the build place: the command
        if (b.task == SH_BUILD && b.plan.kind && !b.sent && Len2D(b.plan.at - me) < (b.plan.kind == 3 ? SH_HOME_RADIUS : SH_BUILD_REACH))
            ShSendBuild(rules, bot, b, now);
        // at a buy zone: the shared buying runs now (UpdateBuying, st.buyAt)
        if (b.task == SH_BUY && b.object && ShInBox(me, b.object, 16.0f))
        {
            b.boughtDay = ShDay(rules);
            st.buyAt = now;
            st.buyTries = 0;
            b.noGunUntil = 0;
            static int reports = 0;
            unsigned char* p = (unsigned char*)PrivateOf(bot);
            if (reports++ < 40)
                Log("Shelter: '%s' buys at a buy zone (money %d)", String(F<int>(Vars(bot), EV_NETNAME)), p ? *(int*)(p + SHP_MONEY) : -1);
        }
        // the axe for hitting, a gun for fighting; when its gun slots give nothing (none, or out of ammo: the game puts the
        // axe back) it stops trying for half a minute and fights only what comes close
        bool hitting = (b.task == SH_GATHER || b.task == SH_REPAIR) && b.object && Len(ShClosest(me, b.object) - me) < 300.0f && !st.target;
        bool melee = ShMeleeHeld(bot);
        if (now >= b.switchAt)
        {
            if (hitting && !melee)
            {
                FakeCommand(bot, "weapon_knife");
                b.switchAt = now + 1.0f;
            }
            else if (!hitting && melee && (st.target || st.threat || ShNight(rules)) && now >= b.noGunUntil)
            {
                if (b.gunTries >= 4)
                {
                    b.noGunUntil = now + 30.0f;
                    b.gunTries = 0;
                    static int reports = 0;
                    if (reports++ < 40)
                        Log("Shelter: '%s' has no gun to use (or no ammo): melee only for 30 s", String(F<int>(Vars(bot), EV_NETNAME)));
                }
                else
                {
                    FakeCommand(bot, b.slotTry++ % 2 ? "slot2" : "slot1");
                    b.gunTries++;
                    b.switchAt = now + 0.6f;
                }
            }
            else if (!melee)
                b.gunTries = 0;
        }
        if (now - b.lastReport > 20.0f)
        {
            b.lastReport = now;
            static int reports = 0;
            unsigned char* p = (unsigned char*)PrivateOf(bot);
            int team = TeamOf(bot);
            if (reports++ < 400)
                Log("Shelter: '%s' (%s, order %s) %s%s, sanity %.0f/%.0f, carries %d/%d wood %d/%d iron, money %d, weapon %d%s, skill points %d",
                    String(F<int>(Vars(bot), EV_NETNAME)), g_shRoleNames[ShRoleOf(st)], g_shOrderNames[team >= 0 && team < 4 ? g_shOrder[team] : 0],
                    orderedAway ? "under radio orders" : g_shTaskNames[b.task], b.object ? (std::string(" ") + ClassName(b.object)).c_str() : "",
                    ShSanity(rules, bot), b.maxSanity, ShCarried(rules, bot, 0), ShCarryMax(rules, bot, 0), ShCarried(rules, bot, 1),
                    ShCarryMax(rules, bot, 1), p ? *(int*)(p + SHP_MONEY) : -1, ShWeaponId(bot), now < b.noGunUntil ? " (no gun)" : "",
                    p ? *(short*)(p + SHP_SKILL_POINTS) : -1);
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
    // the axe in hand and a zombie close by: right up to it, the axe only reaches that far
    if (st.target && ShMeleeHeld(bot) && IsLivingMonster(st.target) && Len(Center(st.target) - pos) < SH_MELEE_CHASE &&
        b.task != SH_BUILD)
    {
        goal = Origin(st.target);
        arrive = 40.0f;
    }
    wantMove = Len2D(goal - pos) >= arrive || fabsf(goal.z - pos.z) >= 64.0f;
    return true;
}

// ShelterHoldFire: true when the bot must not "fire": a melee tool in hand and the target out of its reach
static bool ShelterHoldFire(void* bot, BotState& st)
{
    if (!st.target || !ShRules() || !ShMeleeHeld(bot))
        return false;
    return Len(ShClosest(Eye(bot), st.target) - Eye(bot)) > SH_MELEE_RANGE;
}

// ShelterTarget: Team Match: the other team's players, and its shelter when the bot raids it (after the monsters: a
// monster in sight comes first unless the enemy is much closer)
static void* ShelterTarget(void* bot, V3 eye, void* best)
{
    unsigned char* rules = ShRules();
    if (!rules || !ShEnemyShelter(rules, bot))
        return best;
    int team = TeamOf(bot);
    float bestDist = best ? Len(Center(best) - eye) : 1e9f;
    void* enemy = nullptr;
    float enemyDist = 1800.0f;
    ForEntitiesNear(eye, 1800.0f, [&](void* e) {
        if (e == bot || !Alive(e))
            return;
        bool player = IsHuman(e) || (F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT);
        if (!player || TeamOf(e) == team || TeamOf(e) <= 0)
            return;
        float d = Len(Center(e) - eye);
        if (d < enemyDist && CanSee(bot, eye, e))
        {
            enemyDist = d;
            enemy = e;
        }
    });
    if (enemy && (!best || enemyDist < bestDist * 0.6f))
        return enemy;
    auto it = g_shBots.find(bot);
    if (!best && it != g_shBots.end() && it->second.task == SH_RAID && it->second.object)
    {
        void* sh = it->second.object;
        if (Len(Center(sh) - eye) < 1500.0f && CanSee(bot, eye, sh))
            return sh;
    }
    return best;
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
    // a building that doesn't get better after a while of hitting: the game doesn't take these hits (logged)
    if (b.task == SH_REPAIR)
    {
        float hp = F<float>(Vars(b.object), EV_HEALTH);
        if (b.lastObjectHp < 0)
            b.lastObjectHp = hp;
        if (now - b.swingSince > 6.0f)
        {
            static int reports = 0;
            if (reports++ < 30)
                Log("Shelter: '%s' repairs a %s: health %.0f -> %.0f of %.0f in %.0f s", String(F<int>(Vars(bot), EV_NETNAME)), ClassName(b.object),
                    b.lastObjectHp, hp, F<float>(Vars(b.object), EV_MAX_HEALTH), now - b.swingSince);
            b.swingSince = now;
            b.lastObjectHp = hp;
        }
    }
    return true;
}

// ---------------------------------------------------------------- the player's orders
static bool g_shMenuOpen = false;

static void ShPrint(void* player, const char* text)
{
    if (!g_MessageBegin || !g_WriteString)
        return;
    int msg = *(int*)(MP_GMSG_TEXTMSG + g_mpDelta);
    if (msg <= 0)
        return;
    g_MessageBegin(1 /* MSG_ONE */, msg, nullptr, player);
    g_WriteByte(4); // HUD_PRINTCENTER
    g_WriteString(text);
    g_MessageEnd();
}

static void ShShowMenu(void* player)
{
    void* p = PrivateOf(player);
    if (!p)
        return;
    static const char* text =
        "Shelter orders for the bots\n\n"
        "1. Gather resources\n"
        "2. Defend the shelter\n"
        "3. Repair buildings\n"
        "4. Build\n"
        "5. Follow me\n"
        "6. Free: each does its job\n\n"
        "0. Close";
    int keys = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 9);
    __try
    {
        typedef void(__cdecl * ShowMenuFn)(void* player, int keys, int time, int more, const char* text);
        ((ShowMenuFn)(MP_SHOWMENU + g_mpDelta))(p, keys, -1, 0, text);
        g_shMenuOpen = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Shelter: the orders menu couldn't be shown (exception 0x%08lX)", GetExceptionCode());
    }
}

static void GiveOrder(Order order, void* by);

static void ShGiveOrder(void* player, ShOrder order)
{
    int team = TeamOf(player);
    if (team < 0 || team >= 4)
        return;
    g_shOrder[team] = order;
    void* answer = nullptr;
    int n = 0;
    for (auto& b : g_bots)
    {
        if (!b.edict || !(F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT) || TeamOf(b.edict) != team)
            continue;
        b.ordered = false; // the radio's follow / hold give way to the shelter order
        b.path.clear();
        auto it = g_shBots.find(b.edict);
        if (it != g_shBots.end())
        {
            it->second.next = 0; // decide now
            it->second.task = SH_NONE;
        }
        if (!answer && Alive(b.edict))
            answer = b.edict;
        n++;
    }
    static const char* const said[] = { "Bots: each one does its job", "Bots: gathering resources", "Bots: back to defend the shelter",
                                        "Bots: repairing the buildings", "Bots: building" };
    ShPrint(player, said[order]);
    Log("Shelter: order '%s' for team %d (%d bots)", g_shOrderNames[order], team, n);
    if (cfg.voices != "off" && answer)
        FakeCommand(answer, "roger");
}

// a player's command in this mode: the orders menu (radio3 / the C key, "/orders"), its keys, the chat words, the rescue.
// True: the command stops here (the game's own radio3 menu, a key of our menu).
static bool ShelterPlayerCommand(void* player, const std::string& c)
{
    if (!ShRules())
    {
        g_shMenuOpen = false;
        return false;
    }
    if (c == "radio3")
    {
        ShShowMenu(player);
        return true;
    }
    if (c == "menuselect" && g_shMenuOpen)
    {
        g_shMenuOpen = false;
        const char* a = g_CmdArgv(1);
        int pick = a ? atoi(a) : 0;
        if (pick == 1)
            ShGiveOrder(player, SHO_GATHER);
        else if (pick == 2)
            ShGiveOrder(player, SHO_DEFEND);
        else if (pick == 3)
            ShGiveOrder(player, SHO_REPAIR);
        else if (pick == 4)
            ShGiveOrder(player, SHO_BUILD);
        else if (pick == 5)
        {
            ShGiveOrder(player, SHO_FREE);
            GiveOrder(ORDER_FOLLOW, player);
            ShPrint(player, "Bots: following you");
        }
        else if (pick == 6)
            ShGiveOrder(player, SHO_FREE);
        return true;
    }
    if (c == "menuselect" || c.compare(0, 5, "radio") == 0)
        g_shMenuOpen = false;
    if (c == "say" || c == "say_team")
    {
        const char* a = g_CmdArgv(1);
        std::string t = a ? a : "";
        for (auto& ch : t)
            ch = (char)tolower(ch);
        while (!t.empty() && (t[0] == '/' || t[0] == '!' || t[0] == ' ' || t[0] == '"'))
            t.erase(0, 1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '"'))
            t.pop_back();
        if (t == "orders" || t == "menu" || t == "bots")
            ShShowMenu(player);
        else if (t == "gather" || t == "collect")
            ShGiveOrder(player, SHO_GATHER);
        else if (t == "defend" || t == "base" || t == "home")
            ShGiveOrder(player, SHO_DEFEND);
        else if (t == "repair")
            ShGiveOrder(player, SHO_REPAIR);
        else if (t == "build")
            ShGiveOrder(player, SHO_BUILD);
        else if (t == "free" || t == "jobs")
            ShGiveOrder(player, SHO_FREE);
        return false; // the chat line still shows
    }
    // the rescue: every player has to ask for it, so the team's bots ask with the player and gather at the shelter
    if (c == "zsht_requestrescue")
    {
        int team = TeamOf(player), n = 0;
        for (auto& b : g_bots)
            if (b.edict && (F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT) && TeamOf(b.edict) == team && Alive(b.edict))
            {
                ShelterCommand(b.edict, "zsht_RequestRescue");
                n++;
            }
        Log("Shelter: a player asked for the rescue: %d bots ask too", n);
        ShGiveOrder(player, SHO_DEFEND);
    }
    return false;
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
    if (reports++ >= 300)
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
        else if (ShBuildingAny(e))
        {
            buildings++;
            damaged += F<float>(Vars(e), EV_HEALTH) < F<float>(Vars(e), EV_MAX_HEALTH);
        }
    });
    int t = 2;
    Log("Shelter: day %d %s (phase %d, %.0f s left); shelter tier %d; storage wood %d/%d iron %d/%d power %d/%d; %d monsters, %d wood piles, "
        "%d rocks up, %d buildings (%d damaged)", ShDay(rules), ShNight(rules) ? "night" : "day", *(int*)(rules + SH_PHASE), ShPhaseLeft(rules),
        ShShelterTier(rules, 0), ShStorage(rules, t, 0), ShStorageMax(rules, t, 0), ShStorage(rules, t, 1), ShStorageMax(rules, t, 1),
        ShStorage(rules, t, 2), ShStorageMax(rules, t, 2), monsters, resources, minerals, buildings, damaged);
}
