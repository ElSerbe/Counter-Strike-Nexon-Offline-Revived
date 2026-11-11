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
//   a mineral +0xec is up, +0xe4 hits taken of +0xe8. The minerals are the map's scrap (cars, ATMs, walls: type 1 gives
//   iron or wood, type 0 money) and give nothing without Advanced Harvest Mastery (skill 10; TakeDamage 0x10894350 checks
//   player +0x32ba); the wood and iron piles (zsht_resource, TakeDamage 0x10895970) need only the axe.
//   A building takes a teammate's axe hit as a repair or as construction work (0x10894e40).
//   The day is a clock, not a timer: see ShPhaseLeft (rules +0x21c hour, +0x224 state, +0x228 next tick / end of night).
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
// With the axe in hand they never "shoot" from afar: they draw their gun, or close in only on a zombie that is near.
// Hurt, they heal where something heals (ShHealPlace: not the shelter itself). Skills: the team's bots' skills menu (orders
// 7, "/skills") and auto level-up by job (ShSpendSkill), see "skills".
// On Team Match maps they also fight the other team and, from day 3, the fighters go for the other team's shelter.
// A player's orders: the shelter orders menu (radio3, the C key, or "/orders" in the chat) or the chat words /gather,
// /defend, /repair, /build, /free; the radio's follow / hold still work as everywhere.
static const int MODE_SHELTER = 28;
static const DWORD MP_SHT_RULES_VT = 0x117a1ba8, MP_SHT_EVENT_RULES_VT = 0x117a2d1c;
static const DWORD MP_SHT_TABLES = 0x121d3ec4, MP_SHOWMENU = 0x113f8940, MP_GMSG_TEXTMSG = 0x121d28b8;
static const int SH_RULES_SIZE = 0x2914;
static const int SH_PHASE = 0x214, SH_NIGHT = 0x218, SH_PHASE_END = 0x228, SH_STORAGE = 0x27c, SH_STORAGE_MAX = 0x2a0;
static const int SH_CLOCK_HOUR = 0x21c, SH_CLOCK_STATE = 0x224;
static const DWORD MP_SHT_SET_TICK = 0x11e58338, MP_SHT_SET_TICK_DIV = 0x11e57b58, MP_SHT_SET_TOP = 0x11e58068;
static const DWORD MP_SHT_SET_NIGHT_AT = 0x11e582a8, MP_SHT_SET_NIGHT_LEN = 0x11e58e30;
static const int SH_SKILL_SCAVENGE = 10; // Advanced Harvest Mastery: the map's scrap (cars, ATMs, walls: shelter_team_mineral) gives nothing without it
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
    SH_BREAK,   // breaking a barricade (zbsbreak): heavy axe hits, or its gun when it has a better one
    SH_HUNT,    // nothing to gather by day: after the zombies around the shelter (money for a gun)
    SH_HEAL,    // hurt: at a place that heals (an Emergency Medical Facility, a campfire, the shelter from tier 3)
    SH_DEMOLISH, // Electric Engineering: taking down its team's building (heavy axe hits) to build it again for less power
};
static const char* const g_shTaskNames[] = { "none", "gather", "home", "rest", "repair", "buy", "boss", "defend", "build", "raid",
                                             "break", "hunt", "heal", "demolish" };

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
    int kind = 0;       // 0 none, 1 fixed spot, 2 anywhere, 3 shelter tier, 4 rebuild (take down `demolish`, then as 2)
    int building = -1, spot = -1;
    V3 at{};
    int countBefore = 0; // anywhere: how many of its class the team had
    void* before = nullptr; // fixed spot: what stood there
    float yaw = -1;         // anywhere: its angle (a rebuild keeps the old one's), else a random right angle
    void* demolish = nullptr; // rebuild: the old building (its edict) and its private data (an edict gets reused)
    void* demolishPriv = nullptr;
    int powerBefore = 0;
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
    // gathering: since when it hits its resource in reach, and what it had then; what gave nothing is left alone
    float gatherNear = 0;
    int gatherHits = 0, gatherCarried = 0, gatherMoney = 0;
    std::map<void*, float> useless;
    // a zombie it went for with the axe: it finishes the fight instead of turning to the next thing
    void* fight = nullptr;
    float fightUntil = 0;
    // badly hurt: home once, a rest there (20 s), then back to work until it heals
    float hurtRestUntil = 0;
    // what it told its team in the chat, by kind of message: when (ShSay)
    std::map<int, float> saidAt;
    // a barricade it breaks: its health when it last went down, and when (no progress for a while: given up)
    float breakHp = -1, breakProgressAt = 0;
    // why it found nothing to gather (logged now and then)
    float whyAt = -100;
    // a gun with ammo in its primary or pistol slot (each think)
    bool gunOwned = false;
    int gunReports = 0;
    // healing: its health when it last went up at the place, and when; places seen not to heal, left out until then
    float healHp = -1, healSince = 0;
    std::map<void*, float> noHeal;
};
static std::map<void*, int> g_shBanCount; // resources a bot couldn't reach: how often (each time banned longer)
static std::map<void*, bool> g_shReachable; // resources: whether a bot can stand where its axe reaches them (by level)
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
// one of the mode's settings (the rules' think reads them through lazily made objects; the value's pointer at these)
static bool ShSetting(DWORD at, float& value)
{
    float** pp = (float**)(at + g_mpDelta);
    if (!Readable(pp, 4) || !*pp || !Readable(*pp, 4))
        return false;
    value = **pp;
    return value == value && value > -100000.0f && value < 100000.0f;
}

// seconds to the night (by day) or to the morning (at night); -1 unknown. The day is a clock (rules think 0x10838490):
// state +0x224 1 = the hours go up one per tick (a tick every setting 0x11e58338 / 0x11e57b58 seconds, the next at +0x228)
// to above the top hour (setting 0x11e58068), state 0 = they go down to below the night hour (setting 0x11e582a8), then
// state 2 = night until +0x228 (the night lasts setting 0x11e58e30), and the hours go up again
static float ShPhaseLeft(unsigned char* rules)
{
    int state = *(int*)(rules + SH_CLOCK_STATE), hour = *(int*)(rules + SH_CLOCK_HOUR);
    float toNextTick = *(float*)(rules + SH_PHASE_END) - Now();
    if (toNextTick < 0)
        toNextTick = 0;
    if (state == 2)
        return toNextTick < 3600.0f ? toNextTick : -1.0f;
    float tick, per, top, bottom;
    if (!ShSetting(MP_SHT_SET_TICK, tick) || !ShSetting(MP_SHT_SET_TICK_DIV, per) || per <= 0 || !ShSetting(MP_SHT_SET_TOP, top) ||
        !ShSetting(MP_SHT_SET_NIGHT_AT, bottom))
        return -1.0f;
    float interval = tick / per, ticks;
    if (state == 1)
        ticks = (floorf(top) + 1 - hour) + (floorf(top) + 1 - ceilf(bottom) + 1);
    else if (state == 0)
        ticks = hour - ceilf(bottom) + 1;
    else
        return -1.0f;
    if (ticks < 0)
        ticks = 0;
    float left = ticks * interval + toNextTick;
    return left < 3600.0f ? left : -1.0f;
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

// a gun the bot can fire in an inventory slot (1 primary, 2 pistol): its class ("weapon_..."), with ammo in it or in
// reserve; null when the slot is empty or out of ammo. "weapon_<class>" draws it (the client's slot1/slot2 never reach the
// server: the bots tried those for days and kept the axe)
static const char* ShGunInSlotRaw(void* bot, int slot, int* clipOut = nullptr, int* reserveOut = nullptr)
{
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(bot);
        unsigned char* it = pl ? ItemHandle(pl + PLAYER_ITEMS + 16 * slot) : nullptr;
        if (!it)
            return nullptr;
        int type = *(int*)(it + 0x158), clip = *(int*)(it + 0x160);
        int reserve = type > 0 && type < 0x102 ? *(int*)(pl + PLAYER_AMMO + 4 * type) : 0;
        if (clipOut)
            *clipOut = clip;
        if (reserveOut)
            *reserveOut = reserve;
        if (clip <= 0 && reserve <= 0)
            return nullptr;
        unsigned char* pev = *(unsigned char**)(it + 8);
        const char* cls = pev && Readable(pev, 4) ? String(*(int*)pev) : nullptr;
        return cls && !strncmp(cls, "weapon_", 7) ? cls : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// a gun that breaks a barricade faster than the axe's heavy hits: a primary weapon (slot 1: rifles, shotguns, machine guns
// from the black market) with ammo in it or in reserve. The pistols do less than the heavy hits.
static bool ShGunBeatsAxeRaw(void* bot)
{
    return ShGunInSlotRaw(bot, 1) != nullptr;
}

// the gun it fights with: the primary, else the pistol (null: none with ammo)
static const char* ShBestGun(void* bot)
{
    const char* g = ShGunInSlotRaw(bot, 1);
    return g ? g : ShGunInSlotRaw(bot, 2);
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

// a resource that can still be hit (by anyone)
static bool ShResourceUp(void* e);

// how far along a resource is: zsht_resource hits left (+0xdc), shelter_team_mineral hits taken (+0xe4); changes with each
// hit the game takes
static int ShResourceHits(void* e)
{
    const char* cls = ClassName(e);
    unsigned char* p = (unsigned char*)PrivateOf(e);
    if (!cls || !p || !Readable(p, 0x110))
        return 0;
    return !strcmp(cls, "zsht_resource") ? *(int*)(p + 0xdc) : *(int*)(p + 0xe4);
}

// a resource this bot gets something from: the map's scrap (shelter_team_mineral: cars, ATMs, walls) only gives with Advanced
// Harvest Mastery (its TakeDamage 0x10894350 checks the skill level, player +0x32ba); the wood and iron piles (zsht_resource)
// give to anyone with the axe; and nothing it already hit for a while without getting anything
static bool ShResourceFor(void* bot, void* e, float now);

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

static int ShSkillLevel(void* bot, int id);

// a resource a bot can hit: some floor of the navigation mesh next to it (within 48 units of its box) from which the
// axe reaches it (its box between the floor and 100 above); the map's scrap on rooftops (z 192+) has none
static bool ShReachable(void* e)
{
    auto it = g_shReachable.find(e);
    if (it != g_shReachable.end())
        return it->second;
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    bool ok = g_nav.areas.empty(); // no navigation mesh: don't guess
    for (size_t i = 0; !ok && i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        if (a.hi[0] < lo.x - 48.0f || a.lo[0] > hi.x + 48.0f || a.hi[1] < lo.y - 48.0f || a.lo[1] > hi.y + 48.0f)
            continue;
        float x = lo.x < a.lo[0] ? a.lo[0] : lo.x > a.hi[0] ? a.hi[0] : lo.x;
        float y = lo.y < a.lo[1] ? a.lo[1] : lo.y > a.hi[1] ? a.hi[1] : lo.y;
        float floor = AreaZ(a, x, y);
        ok = lo.z <= floor + 100.0f && hi.z >= floor - 8.0f;
    }
    g_shReachable[e] = ok;
    return ok;
}

static int ShTeamOfEntity(void* e);

// where the zombies are: the map's zombie turrets (hostile turrets) and the living monsters, gathered once per decision
struct ShDanger
{
    std::vector<V3> turrets, monsters;
};

static ShDanger ShDangers(int team)
{
    ShDanger d;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* cls = ClassName(e);
        if (!cls)
            return;
        if (IsLivingMonster(e))
            d.monsters.push_back(Center(e));
        else if (strstr(cls, "turret") && !strstr(cls, "missile") && !strstr(cls, "ammo") && ShTeamOfEntity(e) != team &&
                 F<float>(Vars(e), EV_HEALTH) > 0.0f)
            d.turrets.push_back(Center(e));
    });
    return d;
}

// too dangerous to gather there: within 900 of a zombie turret, or (crowd) 5 zombies (8 with a gun) within 450. The bots
// fight zombies that come close now, so a crowd only puts a resource after the others (gathering looks with the crowds
// left out first); a turret always rules it out
static bool ShDangerous(const ShDanger& d, V3 at, bool hasGun, bool crowd = true)
{
    for (auto& t : d.turrets)
        if (Len(t - at) < 900.0f)
            return true;
    if (!crowd)
        return false;
    int n = 0;
    for (auto& m : d.monsters)
        n += Len(m - at) < 450.0f;
    return n >= (hasGun ? 8 : 5);
}

// what a resource gives: 0 wood, 1 iron (the piles: zsht_resource +0xd8; the scrap mostly iron), -1 money (scrap type 0)
static int ShResourceType(void* e)
{
    const char* cls = ClassName(e);
    unsigned char* p = (unsigned char*)PrivateOf(e);
    if (!cls || !p || !Readable(p, 0x110))
        return -1;
    if (!strcmp(cls, "zsht_resource"))
        return *(int*)(p + 0xd8) == 1 ? 1 : 0;
    return *(int*)(p + 0xe0) ? 1 : -1;
}

static bool ShResourceFor(void* bot, void* e, float now)
{
    if (!ShResourceUp(e) || !ShReachable(e))
        return false;
    const char* cls = ClassName(e);
    if (cls && !strcmp(cls, "shelter_team_mineral") && ShSkillLevel(bot, SH_SKILL_SCAVENGE) <= 0)
        return false;
    auto it = g_shBots.find(bot);
    if (it != g_shBots.end())
    {
        auto u = it->second.useless.find(e);
        if (u != it->second.useless.end() && now < u->second)
            return false;
    }
    return true;
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

// The bot tells its team what it's doing, in the chat (say_team), the game's own way: its ClientCommand with the engine's
// argument functions pointing at ours (like ShelterCommand). Each kind of message (key) at most once per `cooldown` s per
// bot, and 2.5 s between any two bots' messages, so the chat doesn't fill up.
enum ShSayKey
{
    SAY_FULL, SAY_HURT, SAY_SANITY, SAY_NIGHT, SAY_GATHER, SAY_BUILD, SAY_UPGRADE, SAY_REPAIR, SAY_BUY, SAY_BOSS, SAY_RAID,
    SAY_DEPOSIT, SAY_STORAGE_FULL, SAY_NO_GUN, SAY_DEFEND, SAY_REST, SAY_FIGHT, SAY_BREAK, SAY_HUNT, SAY_HEAL, SAY_NO_MEDIC,
    SAY_GUN, SAY_REBUILD
};
static float g_shLastSay = -100;

static void ShSay(void* bot, ShBot& b, int key, float cooldown, const char* fmt, ...)
{
    float now = Now();
    auto it = b.saidAt.find(key);
    if (it != b.saidAt.end() && now - it->second < cooldown && now >= it->second)
        return;
    if (now - g_shLastSay < 2.5f && now >= g_shLastSay)
        return;
    char text[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    b.saidAt[key] = now;
    g_shLastSay = now;
    g_shArgc = 2;
    g_shArgv[0] = "say_team";
    g_shArgv[1] = text;
    snprintf(g_shArgs, sizeof(g_shArgs), "\"%s\"", text);
    void** table = (void**)(MP_ENGFUNCS + g_mpDelta);
    void* saved[3] = { table[ENG_CMD_ARGS], table[ENG_CMD_ARGV], table[ENG_CMD_ARGC] };
    table[ENG_CMD_ARGS] = (void*)ShCmdArgs;
    table[ENG_CMD_ARGV] = (void*)ShCmdArgv;
    table[ENG_CMD_ARGC] = (void*)ShCmdArgc;
    __try
    {
        g_ClientCommandTrampoline(bot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("Shelter: exception 0x%08lX in a bot's chat line", GetExceptionCode());
    }
    table[ENG_CMD_ARGS] = saved[0];
    table[ENG_CMD_ARGV] = saved[1];
    table[ENG_CMD_ARGC] = saved[2];
    static int reports = 0;
    if (reports++ < 200)
        Log("Shelter: '%s' says: %s", String(F<int>(Vars(bot), EV_NETNAME)), text);
}

// a building's name for the chat: its class without "shelter_team_", underscores as spaces
static const char* ShPretty(const char* cls)
{
    static char out[64];
    if (!cls)
        return "building";
    if (!strncmp(cls, "shelter_team_", 13))
        cls += 13;
    snprintf(out, sizeof(out), "%s", cls);
    for (char* c = out; *c; c++)
        if (*c == '_')
            *c = ' ';
    return out;
}

// everything the bot carries into the storage, 5 at a time as the warehouse menu does (a type the storage has no room for
// isn't tried: that was tried every think, 20 times in a row)
static void ShDeposit(unsigned char* rules, void* bot)
{
    int before[2] = { ShCarried(rules, bot, 0), ShCarried(rules, bot, 1) };
    int team = TeamOf(bot);
    for (int type = 0; type < 2; type++)
        for (int guard = 0; guard < 40 && ShCarried(rules, bot, type) > 0 && ShStorage(rules, team, type) < ShStorageMax(rules, team, type); guard++)
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

// ---------------------------------------------------------------- skills
// resource/shelterteam/zsht_skilltree.cso: 33 skills in 3 categories (0 battle, 1 survival, 2 tech), each with its max level,
// its tier and the skill before it in the tree (the table's "depend" column names the skill each one leads to: 0 -> 1 -> 2 ->
// 8, 5 -> 6 -> 7, 9 -> 10, 15 -> 12 -> 14 -> 17, 18 -> 19 -> 20 -> 25, 21 -> 22 -> 26). Names from resource/cso_na_en.txt.
// The game's skill-up (zsht_RequestSkillup, 0x10844830) checks the points, the max level and the exclusive picks (one of
// 8/17/25, one of 30/31/32, one craft of 27/28/29); the tree's order is the client's, and the bots follow it too.
// Each player gets 3 points at the start and one more each day (the game). The bots spend theirs: first what a player
// planned for them (the skills menu, "/skills"), then, with auto level-up on (the default), by their job.
struct ShSkill
{
    int id, cat, maxLv, tier, parent;
    const char *name, *brief, *what;
};
static const ShSkill kSkills[33] = {
    { 0, 0, 3, 1, -1, "Beginner Weapon Mastery", "Weapons I", "SMGs and shotguns, more gun damage" },
    { 1, 0, 3, 2, 0, "Intermediate Weapon Mastery", "Weapons II", "assault and sniper rifles, more gun damage" },
    { 2, 0, 3, 3, 1, "Advanced Weapon Mastery", "Weapons III", "machine guns and equipment weapons" },
    { 3, 0, 3, 3, -1, "Special Weapon Mastery", "Special Wpn", "grenades +25%, melee damage x2 at Lv 3" },
    { 4, 0, 1, 1, -1, "Support Soldier", "Support", "ammo +50%" },
    { 5, 0, 3, 1, -1, "HP Enhancement", "HP", "more max HP every day" },
    { 6, 0, 3, 2, 5, "Kevlar Enhancement", "Kevlar", "armor +30/50/100%" },
    { 7, 0, 2, 3, 6, "Combat Master", "Combat Master", "invulnerable a few seconds at low HP" },
    { 8, 0, 1, 4, 2, "Air Strike Request", "Air Strike", "special: an air strike on your mark" },
    { 9, 1, 3, 1, -1, "Beginner Harvest Mastery", "Harvest I", "+1/2/3 wood and iron per hit, carry more" },
    { 10, 1, 3, 2, 9, "Advanced Harvest Mastery", "Harvest II", "cars and scrap give resources and money" },
    { 11, 1, 3, 3, -1, "Loot", "Loot", "killed zombies drop resources or money" },
    { 12, 1, 1, 2, 15, "Build Camp", "Camp", "campfire: mental gauge and HP" },
    { 13, 1, 3, 1, -1, "DEX Enhancement", "DEX", "run 5/10/20% faster" },
    { 14, 1, 3, 3, 12, "Hunter", "Hunter", "traps last longer and hit harder" },
    { 15, 1, 3, 1, -1, "Adventurer", "Adventurer", "finds wood, iron and enemies" },
    { 16, 1, 1, 3, -1, "Strong Patience", "Patience", "mental gauge drops half as fast: longer outside" },
    { 17, 1, 1, 4, 14, "Hide", "Hide", "special: hidden 15 s" },
    { 18, 2, 3, 1, -1, "Rookie Engineer", "Engineer I", "workroom; tier 1 builds faster and sturdier" },
    { 19, 2, 3, 2, 18, "Intermediate Engineer", "Engineer II", "communication tower; tier 2 builds" },
    { 20, 2, 3, 3, 19, "Advanced Engineer", "Engineer III", "concrete refinery; tier 3 builds" },
    { 21, 2, 3, 1, -1, "Blueprint Upgrade", "Blueprint", "buildings cost 1/2/3 less" },
    { 22, 2, 3, 2, 21, "Electric Engineering", "Electric", "buildings use less power" },
    { 23, 2, 1, 3, -1, "Demolition Skill", "Demolition", "more back from demolished buildings" },
    { 24, 2, 3, 1, -1, "Battlefield Repair", "Repair", "armor and turrets repair themselves" },
    { 25, 2, 1, 4, 20, "Wrecking Team", "Wrecking", "special: C4 against objects" },
    { 26, 2, 1, 3, 22, "Construction Expert", "Construction", "builds faster with a high mental gauge" },
    { 27, 0, 1, 2, -1, "Combat Craft", "Combat Craft", "crafts at the worktable (one craft skill)" },
    { 28, 1, 1, 2, -1, "Survival Craft", "Survival Craft", "crafts at the worktable (one craft skill)" },
    { 29, 2, 1, 2, -1, "Tech Craft", "Tech Craft", "crafts at the worktable (one craft skill)" },
    { 30, 0, 1, 4, -1, "Power Shoot", "Power Shoot", "special: HP, armor, damage and ammo up" },
    { 31, 1, 1, 4, -1, "Transport Resources", "Transport", "special: full pockets go to the storage" },
    { 32, 2, 1, 4, -1, "Moving Tower", "Moving Tower", "special: move built turrets" },
};
static const char* const g_shCatNames[] = { "Battle", "Survival", "Tech" };
// the skills of each category in the tree's order (tier, then id), as the menu lists them
static const int kSkillsByCat[3][11] = {
    { 0, 4, 5, 1, 6, 27, 2, 3, 7, 8, 30 },
    { 9, 13, 15, 10, 12, 28, 11, 14, 16, 17, 31 },
    { 18, 21, 24, 19, 22, 29, 20, 23, 26, 25, 32 },
};
static const int kSkillExclusive[3][3] = { { 8, 17, 25 }, { 30, 31, 32 }, { 27, 28, 29 } };

static int ShPoints(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    return p && Readable(p + SHP_SKILL_POINTS, 2) ? *(short*)(p + SHP_SKILL_POINTS) : 0;
}

static std::string ShBotName(void* bot)
{
    const char* n = String(F<int>(Vars(bot), EV_NETNAME));
    return n ? n : "";
}

// why the bot can't learn this skill now ("max", "needs ...", "has ..."), null when it can
static const char* ShSkillBlock(void* bot, int id)
{
    const ShSkill& s = kSkills[id];
    if (ShSkillLevel(bot, id) >= s.maxLv)
        return "max";
    if (s.parent >= 0 && ShSkillLevel(bot, s.parent) <= 0)
        return ShFmt("needs %s", kSkills[s.parent].brief);
    for (auto& g : kSkillExclusive)
    {
        if (g[0] != id && g[1] != id && g[2] != id)
            continue;
        for (int o : g)
            if (o != id && ShSkillLevel(bot, o) > 0)
                return ShFmt("has %s", kSkills[o].brief);
    }
    return nullptr;
}

// one point into a skill, the way the skill tree's button does it; true when the game took it
static bool ShLearn(void* bot, int id)
{
    int before = ShPoints(bot);
    if (before <= 0 || id < 0 || id >= 33)
        return false;
    char a[8];
    snprintf(a, sizeof(a), "%d", id);
    ShelterCommand(bot, "zsht_RequestSkillup", a);
    return ShPoints(bot) < before;
}

// what a player set for a bot (by its name, so it lasts through deaths, rounds and games): auto level-up, and the skills
// planned for it (one level each, learned in order as its points come); kept in botassist_shelter_skills.txt
struct ShSkillPlan
{
    bool manual = false;
    std::vector<int> queue;
};
static std::map<std::string, ShSkillPlan> g_shPlans;
static bool g_shPlansLoaded = false;

static void ShPlansLoad()
{
    if (g_shPlansLoaded)
        return;
    g_shPlansLoaded = true;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist_shelter_skills.txt", g_dir);
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        // name <tab> auto|manual <tab> id,id,...
        char* t1 = strchr(line, '\t');
        if (!t1 || line[0] == '#')
            continue;
        *t1++ = 0;
        char* t2 = strchr(t1, '\t');
        if (t2)
            *t2++ = 0;
        ShSkillPlan& p = g_shPlans[line];
        p.manual = !strncmp(t1, "manual", 6);
        p.queue.clear();
        for (char* s = t2; s && *s;)
        {
            int id = atoi(s);
            if (id >= 0 && id < 33 && isdigit((unsigned char)*s))
                p.queue.push_back(id);
            s = strchr(s, ',');
            if (s)
                s++;
        }
    }
    fclose(f);
    Log("Shelter: %d bots' skill settings loaded from botassist_shelter_skills.txt", (int)g_shPlans.size());
}

static void ShPlansSave()
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist_shelter_skills.txt", g_dir);
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# Zombie Shelter: each bot's skills (the in-game skills menu writes this): name, auto|manual, planned skill ids\n");
    for (auto& kv : g_shPlans)
    {
        if (kv.first.empty() || (!kv.second.manual && kv.second.queue.empty()))
            continue;
        fprintf(f, "%s\t%s\t", kv.first.c_str(), kv.second.manual ? "manual" : "auto");
        for (size_t i = 0; i < kv.second.queue.size(); i++)
            fprintf(f, "%s%d", i ? "," : "", kv.second.queue[i]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static ShSkillPlan& ShPlanOf(void* bot)
{
    ShPlansLoad();
    return g_shPlans[ShBotName(bot)];
}

// how many levels of a skill the plan already holds
static int ShPlanned(const ShSkillPlan& p, int id)
{
    int n = 0;
    for (int q : p.queue)
        n += q == id;
    return n;
}

// plans one more level of a skill for later, with the skills before it in the tree that it lacks first
static void ShPlanAdd(void* bot, ShSkillPlan& p, int id)
{
    int chain[8], n = 0;
    for (int at = kSkills[id].parent; at >= 0 && n < 8; at = kSkills[at].parent)
        if (ShSkillLevel(bot, at) <= 0 && !ShPlanned(p, at))
            chain[n++] = at;
    while (n > 0)
        p.queue.push_back(chain[--n]);
    p.queue.push_back(id);
}

// the job's order of skill levels with auto level-up (the k-th time a skill comes up is its level k). Everyone starts with
// Beginner Harvest Mastery (more wood and iron per hit) and soon takes Strong Patience (the mental gauge lasts twice as
// long outside); the builder then Rookie Engineer (the worktable) and the cheaper blueprints, gatherers Advanced Harvest
// Mastery (cars and scrap) and speed, fighters the weapon masteries (SMGs and shotguns need the first) and HP.
static const int kAutoBuilder[] = { 18, 21, 18, 19, 21, 22, 24, 9, 21, 16, 19, 22, 20, 18, 26, 24, 22, 19, 20, 24, 20, 23, 29, 9, 13, 5, 9, 32 };
static const int kAutoGatherer[] = { 9, 10, 9, 16, 9, 13, 10, 15, 10, 13, 5, 15, 11, 15, 13, 11, 12, 14, 11, 14, 28, 14, 31, 5, 5 };
static const int kAutoFighter[] = { 9, 0, 5, 16, 0, 13, 1, 5, 6, 4, 0, 1, 3, 5, 6, 13, 1, 7, 3, 2, 6, 7, 3, 2, 2, 30, 13 };

static void ShSkillLog(void* bot, int id, const char* how)
{
    static int reports = 0;
    if (reports++ < 120)
        Log("Shelter: '%s' learns %s (skill %d, now %d/%d; %s; %d points left)", ShBotName(bot).c_str(), kSkills[id].name, id,
            ShSkillLevel(bot, id), kSkills[id].maxLv, how, ShPoints(bot));
}

// a living player of the team (bots and people) with this skill
static bool ShTeamHasSkill(int team, int id)
{
    bool has = false;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (has || !(F<int>(Vars(e), EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) || !Alive(e) || TeamOf(e) != team)
            return;
        has = ShSkillLevel(e, id) > 0;
    });
    return has;
}

static int ShTechLevels(void* bot)
{
    int n = 0;
    for (int id : kSkillsByCat[2])
        n += ShSkillLevel(bot, id);
    return n;
}

// the team's builder: the bot whose place makes it the builder while it lives, else the living bot with the most tech
// levels (the first one when none has any): someone always builds, and it's the one with the Tech skills
static void* ShTeamBuilder(int team)
{
    void* best = nullptr;
    int bestScore = -1;
    for (auto& s : g_bots)
    {
        if (!s.edict || !(F<int>(Vars(s.edict), EV_FLAGS) & FL_FAKECLIENT) || TeamOf(s.edict) != team || !Alive(s.edict))
            continue;
        int score = (ShRoleOf(s) == SHR_BUILDER ? 1000 : 0) + ShTechLevels(s.edict) * 10 - s.slot;
        if (score > bestScore)
        {
            bestScore = score;
            best = s.edict;
        }
    }
    return best;
}

// the job it does now: the team's builder builds (another bot takes over while the builder is dead); the others keep
// their place's job, a builder by place that isn't the team's builder gathers
static ShRole ShJobOf(void* bot, BotState& st)
{
    if (ShTeamBuilder(TeamOf(bot)) == bot)
        return SHR_BUILDER;
    ShRole r = ShRoleOf(st);
    return r == SHR_BUILDER ? SHR_GATHERER : r;
}

// the job's order of skill levels that it hasn't reached yet, after what its plan holds (what auto level-up will learn,
// in order): the menu shows it and can copy it into the plan to edit it
static std::vector<int> ShAutoOrder(void* bot, ShRole role, const ShSkillPlan& plan)
{
    int lv[33];
    for (int id = 0; id < 33; id++)
        lv[id] = ShSkillLevel(bot, id);
    for (int q : plan.queue)
        if (q >= 0 && q < 33)
            lv[q]++;
    const int* list = role == SHR_BUILDER ? kAutoBuilder : role == SHR_GATHERER ? kAutoGatherer : kAutoFighter;
    int n = role == SHR_BUILDER ? (int)(sizeof(kAutoBuilder) / 4) : role == SHR_GATHERER ? (int)(sizeof(kAutoGatherer) / 4)
                                                                                          : (int)(sizeof(kAutoFighter) / 4);
    int seen[33] = {};
    std::vector<int> out;
    for (int i = 0; i < n; i++)
    {
        int id = list[i];
        int want = ++seen[id];
        if (lv[id] >= want || lv[id] >= kSkills[id].maxLv || (kSkills[id].parent >= 0 && lv[kSkills[id].parent] <= 0))
            continue;
        bool other = false;
        for (auto& g : kSkillExclusive)
            if (g[0] == id || g[1] == id || g[2] == id)
                for (int o : g)
                    other |= o != id && lv[o] > 0;
        if (other)
            continue;
        out.push_back(id);
        lv[id]++;
    }
    return out;
}

// a skill point: Rookie Engineer when nobody living in the team has it (the Workroom needs it, and nearly everything
// needs the Workroom), the player's plan, then (auto level-up) the job's order, then any skill of the job's category, then any
static void ShSpendSkill(void* bot, BotState& st)
{
    if (ShPoints(bot) <= 0)
        return;
    ShSkillPlan& plan = ShPlanOf(bot);
    if (!plan.manual && ShSkillLevel(bot, 18) <= 0 && !ShTeamHasSkill(TeamOf(bot), 18) && ShLearn(bot, 18))
    {
        ShSkillLog(bot, 18, "nobody in the team had it: the Workroom needs it");
        return;
    }
    bool changed = false;
    for (size_t i = 0; i < plan.queue.size();)
    {
        int id = plan.queue[i];
        const char* why = id >= 0 && id < 33 ? ShSkillBlock(bot, id) : "max";
        if (why && (!strcmp(why, "max") || !strncmp(why, "has ", 4)))
        {
            plan.queue.erase(plan.queue.begin() + i); // done already, or it can't be had any more
            changed = true;
            continue;
        }
        if (!why && ShLearn(bot, id))
        {
            plan.queue.erase(plan.queue.begin() + i);
            ShPlansSave();
            ShSkillLog(bot, id, "planned by a player");
            return;
        }
        i++; // waits for the one before it
    }
    if (changed)
        ShPlansSave();
    if (plan.manual)
        return;
    ShRole role = ShJobOf(bot, st);
    const int* list = role == SHR_BUILDER ? kAutoBuilder : role == SHR_GATHERER ? kAutoGatherer : kAutoFighter;
    int n = role == SHR_BUILDER ? (int)(sizeof(kAutoBuilder) / 4) : role == SHR_GATHERER ? (int)(sizeof(kAutoGatherer) / 4)
                                                                                          : (int)(sizeof(kAutoFighter) / 4);
    int seen[33] = {};
    for (int i = 0; i < n; i++)
    {
        int id = list[i];
        int want = ++seen[id];
        if (ShSkillLevel(bot, id) >= want || ShSkillBlock(bot, id))
            continue;
        if (ShLearn(bot, id))
        {
            ShSkillLog(bot, id, ShFmt("auto, %s", g_shRoleNames[role]));
            return;
        }
    }
    int cat = role == SHR_BUILDER ? 2 : role == SHR_GATHERER ? 1 : 0;
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; c < 3; c++)
        {
            if ((pass == 0) != (c == cat))
                continue;
            for (int id : kSkillsByCat[c])
                if (!ShSkillBlock(bot, id) && ShLearn(bot, id))
                {
                    ShSkillLog(bot, id, "auto, anything left");
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

// Electric Engineering (skill 22) makes new buildings use less power (0x1083b3a0: power x 0.91 / 0.75 / 0.65, at least 1),
// not the ones already built. A building keeps what it paid (+0xe8 wood, +0xec iron, +0xf0 power; taken down, 0x10842e50
// frees its power and gives back 30% of the rest, more with Demolition Skill). A teammate's hit that isn't a repair hit
// takes its health away (TakeDamage 0x10894e40: the axe's repair hits carry 0x20000000, the others need 0x10000000, and
// "lack of contribution" stops players who didn't build it): the heavy axe hit is tried, and checked. So a bot with the
// skill takes down a finished building of its team, built anywhere, that would use less power built again, when the
// storage can pay for it again, by day; then it builds it in the same place, at the same angle.
static const DWORD MP_SHT_SET_BUILT = 0x11e57840;
static std::map<void*, float> g_shRebuildBan; // buildings not to take down (failed, or being rebuilt) until then
static int g_shDemolishFails = 0;              // buildings the heavy axe didn't take down (3: no more tries this map)
static std::set<void*> g_shDemolishing;        // buildings being taken down: nobody repairs them

static float ShElectricFactor(void* bot)
{
    int lv = ShSkillLevel(bot, 22);
    return lv >= 3 ? 0.65f : lv == 2 ? 0.75f : lv == 1 ? 0.91f : 1.0f;
}

static bool ShBuiltDone(void* e)
{
    unsigned char* p = (unsigned char*)PrivateOf(e);
    float built;
    return p && Readable(p + 0xf4, 4) && ShSetting(MP_SHT_SET_BUILT, built) && (float)*(int*)(p + 0xe0) == built;
}

static bool ShPlanRebuild(unsigned char* rules, void* bot, V3 home, float now, ShPlan& plan)
{
    if (ShSkillLevel(bot, 22) <= 0 || g_shDemolishFails >= 3 || ShNight(rules))
        return false;
    float left = ShPhaseLeft(rules);
    if (left >= 0 && left < 60.0f)
        return false;
    int team = TeamOf(bot);
    float factor = ShElectricFactor(bot);
    void* best = nullptr;
    int bestSave = 1, bestRow = -1, bestPower = 0; // (2 power saved at least: each rebuild costs 70% of its resources)
    ForEntitiesNear(home, 3000.0f, [&](void* e) {
        if (!ShBuilding(e, team) || g_shDemolishing.count(e))
            return;
        auto ban = g_shRebuildBan.find(e);
        if (ban != g_shRebuildBan.end() && now < ban->second)
            return;
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (!p || !Readable(p + 0xf4, 4) || !ShBuiltDone(e))
            return;
        int rowId = *(int*)(p + SH_BUILDING_ROW);
        const ShBuildRow* r = ShRow(rowId);
        // anywhere-buildings that use power; not a storage (its contents would be lost)
        if (!r || r->spot != 0 || r->power <= 0 || r->global || rowId == 18 || !r->cls)
            return;
        int paid = *(int*)(p + 0xf0);
        int now_ = (int)(r->power * factor > 1.0f ? r->power * factor : 1.0f);
        if (paid - now_ <= bestSave || paid > 200)
            return;
        if (F<float>(Vars(e), EV_HEALTH) < F<float>(Vars(e), EV_MAX_HEALTH) * 0.9f || !ShCanAfford(rules, bot, team, *r))
            return;
        bestSave = paid - now_;
        best = e;
        bestRow = rowId;
        bestPower = paid;
    });
    if (!best)
        return false;
    plan = ShPlan{};
    plan.kind = 4;
    plan.building = bestRow;
    plan.at = Origin(best);
    plan.yaw = F<float>(Vars(best), EV_ANGLES + 4);
    plan.demolish = best;
    plan.demolishPriv = PrivateOf(best);
    plan.powerBefore = bestPower;
    g_shDemolishing.insert(best);
    Log("Shelter: '%s' (Electric Engineering %d) will take down a %s using %d power and build it again for %d", ShBotName(bot).c_str(),
        ShSkillLevel(bot, 22), ClassName(best), bestPower, bestPower - bestSave);
    return true;
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
    // a building that would use less power built again (Electric Engineering)
    if (ShPlanRebuild(rules, bot, home, now, plan))
        return true;
    // turrets: two, and two more each tier; the best one this tier allows
    int turrets = byClass["shelter_team_turret"] + byClass["shelter_team_flame_turret"] + byClass["shelter_team_mortar_turret"] +
                  byClass["shelter_team_laser_turret"] + byClass["shelter_team_electronic_turret"];
    std::vector<int> wanted;
    if (turrets < 2 * tier)
        for (int id : { 32, 31, 28, 14, 13, 12 })
            wanted.push_back(id);
    // the Emergency Medical Facility before the turrets (the only thing that heals at the start), a storage first when the
    // storage is full (up to three)
    if (byClass["shelter_team_field_bed"] == 0)
        wanted.insert(wanted.begin(), 22);
    bool full = ShStorage(rules, team, 0) >= ShStorageMax(rules, team, 0) || ShStorage(rules, team, 1) >= ShStorageMax(rules, team, 1);
    if (full && byClass["shelter_team_warehouse"] < 3)
        wanted.insert(wanted.begin(), 18);
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
        ShelterCommand(bot, "zsht_BuildConfirm", id, x, y, z, ShFmt("%d", b.plan.yaw >= 0 ? (int)b.plan.yaw : rand() % 4 * 90));
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
        if (!ShBuilding(e, team) || g_shDemolishing.count(e))
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
// a place to stand in the shelter: the navigation areas 90 to 230 units from its centre, at its height, outside the
// shelter building itself (PtnNavPoint drew 40 random areas of the whole map, none near enough, and gave the area at the
// centre: inside the building, where the bots stood stuck). Listed again every 30 s (the building grows with its tier).
static std::vector<V3> g_shRoam;
static float g_shRoamAt = -100;
static V3 g_shRoamHome{};

static V3 ShRoamPoint(unsigned char* rules, void* bot, V3 home)
{
    float now = Now();
    if (now >= g_shRoamAt + 30.0f || now < g_shRoamAt || Len2D(home - g_shRoamHome) > 50.0f)
    {
        g_shRoamAt = now;
        g_shRoamHome = home;
        g_shRoam.clear();
        // points on the floor of the navigation mesh every 40 units (not the areas' centers: the yard is a few big areas
        // whose centers were all outside the ring, and the fallback was the shelter's middle, where the bots stood still),
        // where a player's body fits (a hull there isn't in something solid)
        for (float ring = SH_HOME_RADIUS * 0.95f; g_shRoam.empty() && ring <= 600.0f; ring += 180.0f)
            for (auto& a : g_nav.areas)
            {
                if (a.hi[0] < home.x - ring || a.lo[0] > home.x + ring || a.hi[1] < home.y - ring || a.lo[1] > home.y + ring)
                    continue;
                for (float x = a.lo[0] + 16.0f; x <= a.hi[0] - 16.0f || x == a.lo[0] + 16.0f; x += 40.0f)
                    for (float y = a.lo[1] + 16.0f; y <= a.hi[1] - 16.0f || y == a.lo[1] + 16.0f; y += 40.0f)
                    {
                        V3 p{ x, y, AreaZ(a, x, y) + 36.0f };
                        float d = Len2D(p - home);
                        if (d < 80.0f || d > ring || fabsf(p.z - home.z) > 140.0f)
                            continue;
                        if (g_TraceHull)
                        {
                            TraceResult tr;
                            HullLine(p + V3{ 0, 0, BODY_LIFT }, p + V3{ 0, 0, BODY_LIFT + 1.0f }, 1, bot, tr);
                            if (tr.fStartSolid || tr.fAllSolid)
                                continue;
                        }
                        if (g_shRoam.size() < 600)
                            g_shRoam.push_back(p);
                        else
                            g_shRoam[rand() % g_shRoam.size()] = p;
                    }
            }
        static int reports = 0;
        if (reports++ < 20)
            Log("Shelter: %d places to stand around the shelter at %.0f %.0f %.0f", (int)g_shRoam.size(), home.x, home.y, home.z);
    }
    if (g_shRoam.empty())
    {
        float yaw = Rnd() * 360.0f;
        return PtnNavPoint(home + YawDir(yaw) * 180.0f, 150.0f);
    }
    return g_shRoam[rand() % g_shRoam.size()];
}

// a barricade the players break (zbsbreak, IsObjective) within `radius` (to its box, flat) of `at`, with nav floor next to
// it, that this bot hasn't given up on: the closest
static void* ShBarricadeNear(void* bot, V3 at, float radius, float now)
{
    auto it = g_shBots.find(bot);
    void* found = nullptr;
    float best = radius;
    ForEntitiesNear(at, radius + 400.0f, [&](void* e) {
        if (!IsObjective(e))
            return;
        float d = Len2D(ShClosest(at, e) - at);
        if (d >= best || !ShReachable(e))
            return;
        if (it != g_shBots.end())
        {
            auto u = it->second.useless.find(e);
            if (u != it->second.useless.end() && now < u->second)
                return;
        }
        best = d;
        found = e;
    });
    return found;
}

// a zombie to hunt by day: the one it hunts while it lives and stays near the shelter, else the closest living monster
// (not a boss: 20000+ health) within 1500 of the shelter
static void* ShPrey(void* bot, ShBot& b, V3 home, V3 me)
{
    if (b.task == SH_HUNT && b.object && IsLivingMonster(b.object) && Len2D(Origin(b.object) - home) < 2000.0f &&
        Now() - b.objectSince < 40.0f)
        return b.object;
    void* found = nullptr;
    float best = 1e9f;
    ForEntitiesNear(home, 1500.0f, [&](void* e) {
        if (!IsLivingMonster(e) || F<float>(Vars(e), EV_HEALTH) >= 20000.0f)
            return;
        float d = Len(Origin(e) - me);
        if (d < best)
        {
            best = d;
            found = e;
        }
    });
    return found;
}

// ---------------------------------------------------------------- healing
// The shelter itself doesn't heal. What does (mp.dll): the Emergency Medical Facility (shelter_team_field_bed, row 22;
// FieldBedThink 0x1088abb0: once built, entity +0xe0 == setting 0x11e57840 and +0xe4 == 0, it gives each living player of
// its team within setting 0x11e579a8 units +setting 0x11e58a88 health per think, every setting 0x11e584e8 s), the campfire
// (shelter_team_campfire, CampfireThink 0x10889390: mental gauge to the team within setting 0x11e58a40, health only to those
// with Build Camp, skill 12, via "zsht_healthrestore"), and, the player says, a zone in the shelter from tier 3 (not found
// in the code: tried, and checked). Every place is checked: 8 s there without health going up and it's left out 2 minutes.
static const DWORD MP_SHT_SET_BED_STATE = 0x11e57840, MP_SHT_SET_BED_RANGE = 0x11e579a8, MP_SHT_SET_BED_HEAL = 0x11e58a88,
                   MP_SHT_SET_BED_TICK = 0x11e584e8, MP_SHT_SET_FIRE_RANGE = 0x11e58a40;
static const int SH_SKILL_CAMP = 12;

// a medical facility that heals now: built (its state is the setting's) and not switched off
static bool ShBedWorks(void* e)
{
    unsigned char* p = (unsigned char*)PrivateOf(e);
    if (!p || !Readable(p + 0xe4, 4))
        return false;
    float built;
    if (ShSetting(MP_SHT_SET_BED_STATE, built) && (float)*(int*)(p + 0xe0) != built)
        return false;
    return p[0xe4] == 0;
}

// where the bot heals: the closest working medical facility of its team, a campfire of its team when it has Build Camp,
// else the shelter from tier 3; null: nothing heals here. `range`: how close it must stand (to the place's origin)
static void* ShHealPlace(unsigned char* rules, void* bot, ShBot& b, V3 home, V3 me, float now, V3& at, float& range)
{
    static int settingReports = 0;
    if (settingReports++ < 1)
    {
        float s[5] = { -1, -1, -1, -1, -1 };
        ShSetting(MP_SHT_SET_BED_STATE, s[0]), ShSetting(MP_SHT_SET_BED_RANGE, s[1]), ShSetting(MP_SHT_SET_BED_HEAL, s[2]);
        ShSetting(MP_SHT_SET_BED_TICK, s[3]), ShSetting(MP_SHT_SET_FIRE_RANGE, s[4]);
        Log("Shelter: medical facility settings: built state %.0f, range %.0f, +%.1f health every %.2f s; campfire range %.0f", s[0], s[1],
            s[2], s[3], s[4]);
    }
    int team = TeamOf(bot);
    bool camp = ShSkillLevel(bot, SH_SKILL_CAMP) > 0;
    void* best = nullptr;
    float bestD = 1e9f;
    ForEntitiesNear(me, 16384.0f, [&](void* e) {
        const char* cls = ClassName(e);
        if (!cls)
            return;
        bool bed = !strcmp(cls, "shelter_team_field_bed"), fire = camp && !strcmp(cls, "shelter_team_campfire");
        if ((!bed && !fire) || ShTeamOfEntity(e) != team || (bed && !ShBedWorks(e)))
            return;
        auto n = b.noHeal.find(e);
        if (n != b.noHeal.end() && now < n->second)
            return;
        float d = Len(Origin(e) - me);
        if (d < bestD)
        {
            bestD = d;
            best = e;
        }
    });
    if (best)
    {
        float r = 0;
        bool bed = !strcmp(ClassName(best), "shelter_team_field_bed");
        if (!ShSetting(bed ? MP_SHT_SET_BED_RANGE : MP_SHT_SET_FIRE_RANGE, r) || r < 40.0f)
            r = 100.0f;
        at = Origin(best);
        range = r * 0.75f;
        return best;
    }
    unsigned char* shelter = ShShelterEntity(rules, ShShelterOf(bot));
    if (shelter && ShShelterTier(rules, ShShelterOf(bot)) >= 3)
    {
        auto n = b.noHeal.find(shelter);
        if (n == b.noHeal.end() || now >= n->second)
        {
            at = home;
            range = SH_HOME_RADIUS * 0.6f;
            return shelter;
        }
    }
    return nullptr;
}

static void ShDecide(unsigned char* rules, void* bot, BotState& st, ShBot& b, V3 me, float now)
{
    V3 home;
    bool hasHome = ShHome(rules, bot, home);
    if (!hasHome)
        home = me;
    int team = TeamOf(bot);
    ShOrder order = team >= 0 && team < 4 ? g_shOrder[team] : SHO_FREE;
    ShRole role = ShJobOf(bot, st);
    float sanity = ShSanity(rules, bot);
    if (sanity > b.maxSanity)
        b.maxSanity = sanity;
    float sane = b.maxSanity > 0 ? sanity / b.maxSanity : 1.0f;
    bool night = ShNight(rules);
    float left = ShPhaseLeft(rules);
    bool atHome = Len2D(me - home) < SH_HOME_RADIUS;
    // full: of a type the storage still has room for (or of both): going home for it makes sense; a type it can't put
    // away isn't gathered any more
    bool full = false, carries = false, fullT[2] = { false, false }, room[2] = { false, false };
    int carried[2] = { 0, 0 };
    for (int t = 0; t < 2; t++)
    {
        int c = ShCarried(rules, bot, t), m = ShCarryMax(rules, bot, t);
        carried[t] = c;
        fullT[t] = m > 0 && c >= m;
        room[t] = ShStorage(rules, team, t) < ShStorageMax(rules, team, t);
        carries |= c > 0;
    }
    full = (fullT[0] && room[0]) || (fullT[1] && room[1]) || (fullT[0] && fullT[1]);
    // what it carries can go into the storage: going home for it makes sense (with the storage full it stood at home
    // with its wood for a minute, doing nothing)
    bool canDeposit = (carried[0] && room[0]) || (carried[1] && room[1]);
    if (full && !canDeposit && Len2D(me - home) < SH_HOME_RADIUS)
        full = false;
    bool building = b.plan.kind != 0 && (b.task == SH_BUILD || b.sent);
    if (atHome && carries && !building)
    {
        if ((carried[0] && room[0]) || (carried[1] && room[1]))
        {
            ShDeposit(rules, bot);
            int put0 = carried[0] - ShCarried(rules, bot, 0), put1 = carried[1] - ShCarried(rules, bot, 1);
            if (put0 + put1 > 0)
                ShSay(bot, b, SAY_DEPOSIT, 25.0f, "Dropped off %d wood and %d iron. Storage: wood %d/%d, iron %d/%d.", put0, put1,
                      ShStorage(rules, team, 0), ShStorageMax(rules, team, 0), ShStorage(rules, team, 1), ShStorageMax(rules, team, 1));
        }
        else
            ShSay(bot, b, SAY_STORAGE_FULL, 90.0f, "The storage is full (wood %d/%d, iron %d/%d). We need another storage!",
                  ShStorage(rules, team, 0), ShStorageMax(rules, team, 0), ShStorage(rules, team, 1), ShStorageMax(rules, team, 1));
    }
    bool hasGun = now >= b.noGunUntil;
    ShDanger danger = ShDangers(team);
    // hurt (below half its health by day, a third at night): to a place that heals until 90% (the shelter itself doesn't:
    // the bots stood in it hurt, doing nothing); with none it keeps working away from the fights and asks for one
    float maxHp = F<float>(Vars(bot), EV_MAX_HEALTH), hp = F<float>(Vars(bot), EV_HEALTH);
    float hpFrac = maxHp > 0.0f ? hp / maxHp : 1.0f;
    bool hurt = hpFrac < (night ? 0.35f : 0.5f) || (b.task == SH_HEAL && hpFrac < 0.9f);
    V3 healAt{};
    float healRange = 0;
    void* healPlace = hurt ? ShHealPlace(rules, bot, b, home, me, now, healAt, healRange) : nullptr;
    if (healPlace && night && Len2D(healAt - home) > 900.0f)
        healPlace = nullptr; // not out of the shelter at night
    if (healPlace)
    {
        // at the place: its health must go up (else it doesn't heal: left out for 2 minutes, logged)
        bool there = Len2D(me - healAt) < healRange && fabsf(me.z - healAt.z) < 120.0f;
        if (b.task != SH_HEAL || b.object != healPlace || !there)
        {
            b.healHp = hp;
            b.healSince = there ? now : 0;
        }
        else if (hp > b.healHp + 0.5f)
        {
            b.healHp = hp;
            b.healSince = now;
        }
        else
        {
            if (hp < b.healHp)
                b.healHp = hp;
            if (b.healSince == 0)
                b.healSince = now;
            if (now - b.healSince > 8.0f)
            {
                b.noHeal[healPlace] = now + 120.0f;
                static int reports = 0;
                if (reports++ < 30)
                    Log("Shelter: '%s' stood 8 s at the %s (%.0f u away) and its health didn't go up (%.0f/%.0f): leaves it for 2 min",
                        ShBotName(bot).c_str(), healPlace == ShShelterEntity(rules, ShShelterOf(bot)) ? "shelter (tier 3)" : ClassName(healPlace),
                        Len2D(me - healAt), hp, maxHp);
                healPlace = nullptr;
            }
        }
    }
    if (hurt && !healPlace && hpFrac < 0.5f)
        ShSay(bot, b, SAY_NO_MEDIC, 150.0f, "I'm hurt (%.0f HP) and nothing here heals. We need an Emergency Medical Facility!", hp);

    float damagedDist = 1e9f;
    void* damaged = ShFindDamaged(team, home, me, order == SHO_REPAIR ? SH_REPAIR_RANGE * 1.5f : SH_REPAIR_RANGE,
                                  order == SHO_REPAIR ? 0.999f : night ? 0.9f : SH_REPAIR_BELOW, damagedDist);

    ShTask task = SH_GATHER;
    void* object = nullptr;
    void* boss = nullptr;
    void* enemyShelter = ShEnemyShelter(rules, bot);
    bool wantBuild = (order == SHO_BUILD || (order == SHO_FREE && role == SHR_BUILDER)) && !night;
    // (another bot with Electric Engineering rebuilds too)
    bool rebuilder = !wantBuild && order == SHO_FREE && !night && ShSkillLevel(bot, 22) > 0;
    if ((wantBuild || rebuilder) && b.plan.kind == 0 && now >= b.nextPlan)
    {
        b.nextPlan = now + 3.0f;
        if (wantBuild ? ShPlanBuild(rules, bot, home, now, b.plan) : ShPlanRebuild(rules, bot, home, now, b.plan))
        {
            b.planSince = now;
            static int reports = 0;
            if (reports++ < 120)
                Log("Shelter: '%s' plans row %d (%s)%s", String(F<int>(Vars(bot), EV_NETNAME)), b.plan.building,
                    b.plan.kind == 3 ? "shelter tier" : ShRow(b.plan.building)->cls, b.plan.spot > 0 ? ShFmt(" on spot %d", b.plan.spot) : "");
            if (b.plan.kind == 3)
                ShSay(bot, b, SAY_UPGRADE, 60.0f, "We have enough resources to upgrade the shelter. I'm on it!");
            else if (b.plan.kind == 4)
                ShSay(bot, b, SAY_REBUILD, 60.0f, "Rebuilding the %s: with Electric Engineering it will use less power (%d now).",
                      ShPretty(ShRow(b.plan.building)->cls), b.plan.powerBefore);
            else
                ShSay(bot, b, SAY_BUILD, 40.0f, "We have enough for a %s. Building it.", ShPretty(ShRow(b.plan.building)->cls));
        }
    }
    // a rebuild: the old building first comes down, then it's an ordinary build in its place (given up at night, after
    // 90 s, or when the axe doesn't take it down: ShelterSwing)
    if (b.plan.kind == 4)
    {
        void* old = b.plan.demolish;
        bool gone = !old || PrivateOf(old) != b.plan.demolishPriv || !ShBuilding(old, team);
        if (gone)
        {
            g_shDemolishing.erase(old);
            std::map<int, int> byRow;
            std::map<std::string, int> byClass;
            ShTeamBuildings(team, byRow, byClass);
            const ShBuildRow* r = ShRow(b.plan.building);
            b.plan.kind = 2;
            b.plan.countBefore = r && r->cls ? byClass[r->cls] : 0;
            b.planSince = now;
            Log("Shelter: '%s' took down the %s: builds it again (storage wood %d iron %d power %d)", ShBotName(bot).c_str(),
                r && r->cls ? r->cls : "?", ShStorage(rules, team, 0), ShStorage(rules, team, 1), ShStorage(rules, team, 2));
        }
        else if (night || now - b.planSince > 90.0f || healPlace)
        {
            g_shDemolishing.erase(old);
            g_shRebuildBan[old] = now + 300.0f;
            b.plan.kind = 0;
        }
    }
    if (b.plan.kind && b.plan.kind != 4 && now - b.planSince > 45.0f && !b.sent)
    {
        g_shBuildBan[b.plan.building * 1000LL + (b.plan.spot > 0 ? b.plan.spot : 0)] = now + 60.0f; // not reached in time
        b.plan.kind = 0;
    }

    if (healPlace)
        task = SH_HEAL;
    else if (sane < SH_SANITY_BOSS && !atHome)
        task = SH_HOME;
    else if (order == SHO_DEFEND)
        task = damaged && damagedDist < 700.0f && !st.target ? SH_REPAIR : SH_DEFEND;
    else if (order == SHO_REPAIR)
        task = damaged ? SH_REPAIR : SH_DEFEND;
    else if (night && order != SHO_GATHER)
        task = damaged && damagedDist < 1200.0f && !st.target ? SH_REPAIR : SH_DEFEND;
    else if (b.task == SH_REST && atHome && sane < SH_SANITY_RESTED && order == SHO_FREE)
        task = SH_REST;
    else if ((wantBuild || rebuilder) && b.plan.kind == 4)
        task = SH_DEMOLISH;
    else if ((wantBuild || rebuilder) && b.plan.kind)
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
    if (task == SH_HEAL)
        object = healPlace;
    else if (task == SH_DEMOLISH)
        object = b.plan.demolish;
    else if (task == SH_REPAIR)
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
    // barricades (by day, when it's free to work): the one it breaks stays its job while it keeps going down (25 s
    // without progress: given up); one right next to it (in its way) is broken first; zombies it hunts stay its job
    // for a while unless something to gather turns up (the gathering below runs first then)
    if (task == SH_GATHER && !night)
    {
        float since = b.objectSince > b.breakProgressAt ? b.objectSince : b.breakProgressAt;
        auto u = b.task == SH_BREAK && b.object ? b.useless.find(b.object) : b.useless.end();
        bool usable = u == b.useless.end() || now >= u->second;
        if (b.task == SH_BREAK && b.object && IsObjective(b.object) && usable && now - since < 25.0f)
        {
            task = SH_BREAK;
            object = b.object;
        }
        else if (void* bar = ShBarricadeNear(bot, me, 140.0f, now))
        {
            task = SH_BREAK;
            object = bar;
        }
    }
    if (task == SH_GATHER && b.task == SH_GATHER && b.object && ShResourceUp(b.object))
    {
        // hitting it in reach: something must come of it (the resource's hit count, what it carries or its money); after
        // a while of nothing (a skill it lacks, a hit the game doesn't take) it leaves it alone for a few minutes
        unsigned char* p = (unsigned char*)PrivateOf(bot);
        int hits = ShResourceHits(b.object), carried = ShCarried(rules, bot, 0) + ShCarried(rules, bot, 1);
        int money = p && Readable(p + SHP_MONEY, 4) ? *(int*)(p + SHP_MONEY) : 0;
        bool inReach = Len(ShClosest(Eye(bot), b.object) - Eye(bot)) < SH_REACH + 16.0f && !st.target && ShWeaponId(bot) == SH_KNIFE_ID;
        if (!inReach || !b.gatherNear || hits != b.gatherHits || carried != b.gatherCarried || money != b.gatherMoney)
        {
            b.gatherNear = inReach ? now : 0;
            b.gatherHits = hits;
            b.gatherCarried = carried;
            b.gatherMoney = money;
        }
        else if (now - b.gatherNear > 7.0f)
        {
            b.useless[b.object] = now + 240.0f;
            b.gatherNear = 0;
            static int reports = 0;
            if (reports++ < 40)
                Log("Shelter: '%s' hit a %s for 7 s and got nothing: leaves it alone (Advanced Harvest Mastery %d)",
                    String(F<int>(Vars(bot), EV_NETNAME)), ClassName(b.object), ShSkillLevel(bot, SH_SKILL_SCAVENGE));
        }
    }
    else
        b.gatherNear = 0;
    if (task == SH_GATHER)
    {
        // keep its resource while it is up and reachable, else the closest free one
        auto typeFull = [&](void* e) {
            int t = ShResourceType(e);
            return t >= 0 && fullT[t];
        };
        if (b.task == SH_GATHER && b.object && ShResourceFor(bot, b.object, now) && now - b.objectSince < SH_GIVE_UP &&
            !ShDangerous(danger, Center(b.object), hasGun) && !typeFull(b.object))
            object = b.object;
        else
        {
            if (b.object && b.task == SH_GATHER && ShResourceFor(bot, b.object, now))
            {
                // not reached in time: left alone for a minute, then 2, 4, 8 (the same pile behind a fence pulled the
                // bots back every minute)
                int n = ++g_shBanCount[b.object];
                float minutes = (float)(1 << (n - 1 < 3 ? n - 1 : 3));
                g_shBanned[b.object] = now + 60.0f * minutes;
                static int reports = 0;
                if (reports++ < 30)
                    Log("Shelter: '%s' couldn't reach a %s, leaving it for %.0f min", String(F<int>(Vars(bot), EV_NETNAME)), ClassName(b.object), minutes);
            }
            std::set<void*> taken;
            for (auto& kv : g_shBots)
                if (kv.first != bot && kv.second.task == SH_GATHER && kv.second.object)
                    taken.insert(kv.second.object);
            // the closest one; a crowd of zombies or a type the storage has no room for puts it after the others. When the
            // strict look finds nothing (all taken, near a turret, banned), a second one over the whole map takes what
            // it can, rather than the bot standing in the shelter
            int why[8] = {}; // up, unreachable/skill/useless, taken, carrying full, turret, banned, storage full, found
            for (int pass = 0; pass < 2 && !object; pass++)
            {
                float best = 1e9f, bestCrowded = 1e9f;
                void* crowded = nullptr;
                ForEntitiesNear(home, pass ? 16384.0f : SH_GATHER_RANGE, [&](void* e) {
                    if (!ShResourceUp(e))
                        return;
                    why[0] += !pass;
                    if (!ShResourceFor(bot, e, now))
                    {
                        why[1] += !pass;
                        return;
                    }
                    if (typeFull(e))
                    {
                        why[3] += !pass;
                        return;
                    }
                    if (!pass && taken.count(e))
                    {
                        why[2]++;
                        return;
                    }
                    bool turret = ShDangerous(danger, Center(e), hasGun, false);
                    if (turret && !pass)
                    {
                        why[4]++;
                        return;
                    }
                    auto ban = g_shBanned.find(e);
                    if (ban != g_shBanned.end() && now < ban->second && !pass)
                    {
                        why[5]++;
                        return;
                    }
                    float d = Len(Center(e) - me);
                    int t = ShResourceType(e);
                    if (t >= 0 && !room[t])
                    {
                        why[6] += !pass;
                        d += 1500.0f;
                    }
                    if (pass && (turret || (ban != g_shBanned.end() && now < ban->second)))
                        d += 3000.0f;
                    bool crowd = turret || ShDangerous(danger, Center(e), hasGun);
                    if (!crowd && d < best)
                    {
                        best = d;
                        object = e;
                    }
                    else if (crowd && d < bestCrowded)
                    {
                        bestCrowded = d;
                        crowded = e;
                    }
                });
                if (!object)
                    object = crowded; // only crowded ones: the closest of them
            }
            if (!object && now - b.whyAt > 30.0f)
            {
                b.whyAt = now;
                static int reports = 0;
                if (reports++ < 60)
                    Log("Shelter: '%s' finds nothing to gather: %d up, %d it gets nothing from or can't reach, %d taken, %d of a type it "
                        "carries all it can of, %d near a zombie turret, %d banned, %d of a type the storage has no room for",
                        String(F<int>(Vars(bot), EV_NETNAME)), why[0], why[1], why[2], why[3], why[4], why[5], why[6]);
            }
            if (!object)
            {
                // nothing to gather: by day something useful instead of standing in the shelter: a barricade near the
                // shelter, the zombies around it (money for a gun), home if what it carries fits in the storage
                void* bar = !night && (left < 0 || left > SH_NIGHT_NEAR) ? ShBarricadeNear(bot, home, 1500.0f, now) : nullptr;
                void* prey = !night && (left < 0 || left > SH_NIGHT_NEAR) && !hurt ? ShPrey(bot, b, home, me) : nullptr;
                if (carries && canDeposit && !atHome)
                    task = SH_HOME;
                else if (bar)
                {
                    task = SH_BREAK;
                    object = bar;
                }
                else if (prey)
                {
                    task = SH_HUNT;
                    object = prey;
                }
                else
                    task = SH_DEFEND;
            }
        }
    }
    if (task != b.task)
    {
        unsigned char* pp = (unsigned char*)PrivateOf(bot);
        int money = pp && Readable(pp + SHP_MONEY, 4) ? *(int*)(pp + SHP_MONEY) : 0;
        switch (task)
        {
        case SH_HOME:
            if (full)
                ShSay(bot, b, SAY_FULL, 45.0f, "I can't carry any more (wood %d, iron %d). Heading back to the shelter.", carried[0], carried[1]);
            else if (sane < SH_SANITY_HOME)
                ShSay(bot, b, SAY_SANITY, 60.0f, "My mental gauge is low, going back to the shelter to rest.");
            else if (left >= 0 && left < SH_NIGHT_NEAR)
                ShSay(bot, b, SAY_NIGHT, 90.0f, "Night is coming! Back to the shelter.");
            break;
        case SH_GATHER:
            if (object)
            {
                int t = ShResourceType(object);
                ShSay(bot, b, SAY_GATHER, 60.0f, t == 0 ? "Going to get wood (%d carried)." : t == 1 ? "Going to get iron (%d carried)." :
                      "Breaking scrap for money (%d$).", t == 0 ? carried[0] : t == 1 ? carried[1] : money);
            }
            break;
        case SH_REPAIR:
            if (object)
                ShSay(bot, b, SAY_REPAIR, 45.0f, "The %s is damaged, repairing it.", ShPretty(ClassName(object)));
            break;
        case SH_BUY:
            ShSay(bot, b, SAY_BUY, 90.0f, "I have %d$, going to the black market to buy a gun.", money);
            break;
        case SH_BOSS:
            ShSay(bot, b, SAY_BOSS, 60.0f, "A boss is out there, going after it!");
            break;
        case SH_RAID:
            ShSay(bot, b, SAY_RAID, 90.0f, "Going to hit the enemy shelter!");
            break;
        case SH_BREAK:
            ShSay(bot, b, SAY_BREAK, 60.0f, ShGunBeatsAxeRaw(bot) ? "A barricade, shooting it down." :
                                                                   "A barricade in the way, breaking it with my axe.");
            break;
        case SH_HUNT:
            ShSay(bot, b, SAY_HUNT, 90.0f, "Nothing left to gather nearby. Hunting zombies around the shelter (%d$).", money);
            break;
        case SH_DEFEND:
            if (night)
                ShSay(bot, b, SAY_DEFEND, 120.0f, "Holding the shelter tonight.");
            break;
        case SH_REST:
            ShSay(bot, b, SAY_REST, 90.0f, "Resting in the shelter for a moment.");
            break;
        case SH_HEAL:
            if (object == ShShelterEntity(rules, ShShelterOf(bot)))
                ShSay(bot, b, SAY_HEAL, 60.0f, "I'm hurt (%.0f HP), healing in the shelter.", hp);
            else
                ShSay(bot, b, SAY_HEAL, 60.0f, "I'm hurt (%.0f HP), going to heal at the %s.", hp, ShPretty(ClassName(object)));
            break;
        default:
            break;
        }
    }
    if (task != b.task || object != b.object)
    {
        b.objectSince = now;
        b.lastObjectHp = -1;
        b.swings = 0;
        b.breakHp = -1;
        b.breakProgressAt = 0;
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
    case SH_DEMOLISH:
    {
        V3 c = ShClosest(me, object);
        b.goal = V3{ c.x, c.y, me.z };
        b.arrive = 20.0f;
        break;
    }
    case SH_BREAK:
    {
        // right up to it for the axe; a gun shoots it from where it sees it
        V3 c = ShClosest(me, object);
        b.goal = V3{ c.x, c.y, me.z };
        b.arrive = ShGunBeatsAxeRaw(bot) ? 250.0f : 20.0f;
        break;
    }
    case SH_HUNT:
        b.goal = Origin(object);
        b.arrive = hasGun ? 250.0f : 40.0f;
        break;
    case SH_BUILD:
        b.goal = b.plan.at;
        b.arrive = b.plan.kind == 3 ? SH_HOME_RADIUS * 0.6f : SH_BUILD_REACH * 0.6f;
        break;
    case SH_HOME:
        b.goal = home;
        b.arrive = SH_HOME_RADIUS * 0.5f;
        break;
    case SH_HEAL:
        if (object == ShShelterEntity(rules, ShShelterOf(bot)))
        {
            b.goal = home;
            b.arrive = SH_HOME_RADIUS * 0.4f;
        }
        else
        {
            // right next to it (it's solid): its closest point, well inside its range
            V3 c = ShClosest(me, object);
            b.goal = V3{ c.x, c.y, me.z };
            b.arrive = 24.0f;
        }
        break;
    default: // rest, defend: somewhere in the shelter, another spot now and then
        if (now >= b.roamAt || Len2D(b.roam - home) > SH_HOME_RADIUS || st.navStuck >= 2)
        {
            b.roamAt = now + 6.0f + Rnd() * 8.0f;
            b.roam = ShRoamPoint(rules, bot, home);
            st.navStuck = 0;
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
        g_shReachable.clear();
        g_shRebuildBan.clear();
        g_shDemolishing.clear();
        g_shDemolishFails = 0;

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
        bool hitting = (b.task == SH_GATHER || b.task == SH_REPAIR || b.task == SH_DEMOLISH || (b.task == SH_BREAK && !ShGunBeatsAxeRaw(bot))) && b.object &&
                       Len(ShClosest(me, b.object) - me) < 300.0f && !st.target;
        bool melee = ShMeleeHeld(bot);
        // its gun: the primary, else the pistol, with ammo; none: axe only (ShelterTarget, the axe fights) until it has one
        const char* gun = ShBestGun(bot);
        b.gunOwned = gun != nullptr;
        if (!gun)
            b.noGunUntil = now + 1.0f;
        if (b.gunReports < 3 && (gun || b.gunReports == 0))
        {
            b.gunReports++;
            int c1 = 0, r1 = 0, c2 = 0, r2 = 0;
            ShGunInSlotRaw(bot, 1, &c1, &r1);
            ShGunInSlotRaw(bot, 2, &c2, &r2);
            Log("Shelter: '%s' weapons: primary '%s' (clip %d, reserve %d), pistol '%s' (clip %d, reserve %d): fights with %s",
                ShBotName(bot).c_str(), SlotClass(bot, 1).c_str(), c1, r1, SlotClass(bot, 2).c_str(), c2, r2, gun ? gun : "the axe");
        }
        if (now >= b.switchAt)
        {
            if (hitting && !melee)
            {
                FakeCommand(bot, "weapon_knife");
                b.switchAt = now + 1.0f;
            }
            else if (!hitting && melee && (st.target || st.threat || ShNight(rules)))
            {
                if (!gun)
                {
                    unsigned char* pp = (unsigned char*)PrivateOf(bot);
                    int money = pp && Readable(pp + SHP_MONEY, 4) ? *(int*)(pp + SHP_MONEY) : 0;
                    if (st.target)
                    {
                        if (money < SH_BUY_MONEY)
                            ShSay(bot, b, SAY_NO_GUN, 120.0f, "I only have my axe and %d$. Getting money to buy a gun.", money);
                        else
                            ShSay(bot, b, SAY_NO_GUN, 120.0f, "Out of ammo, axe only. I'll buy a gun with my %d$.", money);
                    }
                }
                else if (now >= b.noGunUntil)
                {
                    if (b.gunTries >= 3)
                    {
                        // the game didn't put it in its hands: the axe for half a minute (logged)
                        b.noGunUntil = now + 30.0f;
                        b.gunTries = 0;
                        static int reports = 0;
                        if (reports++ < 40)
                            Log("Shelter: '%s' couldn't draw its %s (3 tries): melee only for 30 s", ShBotName(bot).c_str(), gun);
                    }
                    else
                    {
                        FakeCommand(bot, gun);
                        b.gunTries++;
                        b.switchAt = now + 0.8f;
                        if (st.target && IsLivingMonster(st.target))
                            ShSay(bot, b, SAY_GUN, 60.0f, "Zombie! Drawing my gun.");
                    }
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
            V3 home = me;
            ShHome(rules, bot, home);
            if (reports++ < 400)
                Log("Shelter: '%s' (%s, order %s) at %.0f %.0f %.0f (%.0f u from home): %s%s, sanity %.0f/%.0f, carries %d/%d wood %d/%d iron, money %d, weapon %d%s, skill points %d",
                    String(F<int>(Vars(bot), EV_NETNAME)), g_shRoleNames[ShJobOf(bot, st)], g_shOrderNames[team >= 0 && team < 4 ? g_shOrder[team] : 0],
                    me.x, me.y, me.z, Len2D(me - home),
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
    // the axe in hand and a zombie close by: right up to it, the axe only reaches that far; once it goes for one it
    // finishes it (ShelterTarget keeps it the target) unless it dies, gets 500 away or 8 s pass
    float now = Now();
    if (b.fight && (!IsLivingMonster(b.fight) || Len(Center(b.fight) - pos) > 500.0f || now > b.fightUntil || !ShMeleeHeld(bot)))
        b.fight = nullptr;
    // (only without a gun to draw: with one it shoots instead of running up to the zombie with the axe)
    if (!b.fight && st.target && ShMeleeHeld(bot) && now < b.noGunUntil && IsLivingMonster(st.target) &&
        Len(Center(st.target) - pos) < SH_MELEE_CHASE && b.task != SH_BUILD && b.task != SH_HOME && b.task != SH_HEAL)
    {
        b.fight = st.target;
        b.fightUntil = now + 8.0f;
    }
    if (b.fight)
    {
        goal = Origin(b.fight);
        arrive = 40.0f;
    }
    wantMove = Len2D(goal - pos) >= arrive || fabsf(goal.z - pos.z) >= 64.0f;
    return true;
}

// ShelterStandGround: true when the bot must not back off from a zombie (the shared combat steps away from monsters that
// come close): with the axe it has to get close to hit, and at night the shelter is held, not left
static bool ShelterStandGround(void* bot)
{
    unsigned char* rules = ShRules();
    return rules && (ShMeleeHeld(bot) || ShNight(rules));
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
static void* ShelterTargetTeams(void* bot, V3 eye, void* best);

// ShelterTarget: which target the shared combat gets. A fight the bot started with the axe stays its target. Zombies far
// away aren't targets: with the axe they only turned its head away from what it was hitting (it never swung), and across
// the map they pulled it from its task. So: none beyond 1200; with only the axe (no gun for now) none beyond the reach of
// a chase; while hitting its resource or a building none beyond 250. Then Team Match's enemies (ShelterTargetTeams).
static void* ShelterTarget(void* bot, V3 eye, void* best)
{
    unsigned char* rules = ShRules();
    if (!rules)
        return best;
    auto it = g_shBots.find(bot);
    if (it != g_shBots.end())
    {
        ShBot& b = it->second;
        float now = Now();
        if (b.fight && IsLivingMonster(b.fight) && now < b.fightUntil && Len(Center(b.fight) - eye) < 500.0f)
            return b.fight;
        // the map's barricades (zbsbreak) were the shared combat's "objectives" up to 1500 away: with the axe the bot stared
        // at one and never swung at the resource right in front of it. Only with a real gun, close, and not while hitting.
        if (best && !IsLivingMonster(best) && IsObjective(best))
        {
            bool hittingNow = (b.task == SH_GATHER || b.task == SH_REPAIR || b.task == SH_BUILD) && b.object &&
                              Len(ShClosest(eye, b.object) - eye) < 300.0f;
            if (hittingNow || !ShGunBeatsAxeRaw(bot) || Len(ShClosest(eye, best) - eye) > 600.0f)
                best = nullptr;
        }
        // breaking one: its gun shoots that one (the axe goes up to it instead: ShelterLook/ShelterSwing)
        if (b.task == SH_BREAK && b.object && IsObjective(b.object) && (!best || !IsLivingMonster(best)))
        {
            bool gun = ShGunBeatsAxeRaw(bot);
            best = gun && Len(ShClosest(eye, b.object) - eye) < 700.0f && CanSee(bot, eye, b.object) ? b.object : nullptr;
        }
        if (best && IsLivingMonster(best))
        {
            float d = Len(ShClosest(eye, best) - eye);
            bool hitting = (b.task == SH_GATHER || b.task == SH_REPAIR || b.task == SH_BUILD || b.task == SH_BREAK || b.task == SH_DEMOLISH) && b.object &&
                           Len(ShClosest(eye, b.object) - eye) < 300.0f;
            if (d > 1200.0f || (now < b.noGunUntil && d > SH_MELEE_CHASE) || (hitting && d > 250.0f))
                best = nullptr;
        }
    }
    return ShelterTargetTeams(bot, eye, best);
}

static void* ShelterTargetTeams(void* bot, V3 eye, void* best)
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
    if ((b.task != SH_GATHER && b.task != SH_REPAIR && b.task != SH_BREAK && b.task != SH_DEMOLISH) || !b.object ||
        Len(ShClosest(eye, b.object) - eye) > 220.0f)
        return false;
    V3 c = ShClosest(eye, b.object);
    V3 mid = Center(b.object);
    V3 at = V3{ c.x * 0.7f + mid.x * 0.3f, c.y * 0.7f + mid.y * 0.3f, c.z };
    yaw = YawOf(at - eye);
    pitch = PitchOf(at - eye);
    return true;
}

// which axe button it presses now (0 none): holding the axe, in reach, looking at it. Resources and buildings take the
// quick hit (attack); barricades the heavy one (attack2), which does far more to them
static int ShelterSwing(void* bot, BotState& st, float yaw, float pitch)
{
    if (st.target || !ShRules())
        return 0;
    auto it = g_shBots.find(bot);
    if (it == g_shBots.end())
        return 0;
    ShBot& b = it->second;
    if ((b.task != SH_GATHER && b.task != SH_REPAIR && b.task != SH_BREAK && b.task != SH_DEMOLISH) || !b.object ||
        ShWeaponId(bot) != SH_KNIFE_ID)
        return 0;
    V3 eye = Eye(bot);
    V3 c = ShClosest(eye, b.object);
    if (Len(c - eye) > SH_REACH)
        return 0;
    float wantYaw = YawOf(c - eye), wantPitch = PitchOf(c - eye);
    if (Len(c - eye) > 24.0f && (fabsf(AngleDiff(wantYaw, yaw)) > 25.0f || fabsf(wantPitch - pitch) > 30.0f))
        return 0;
    float now = Now();
    if (!b.swings++)
        b.swingSince = now;
    if (b.task == SH_DEMOLISH)
    {
        // its health going down is the building coming down; 10 s of heavy hits without: the game doesn't let this bot take
        // it down (it didn't build it, or the heavy hit isn't the way): left alone, and after 3 such no more tries (logged)
        float hp = F<float>(Vars(b.object), EV_HEALTH);
        if (b.breakHp < 0 || hp < b.breakHp - 0.5f)
        {
            if (b.breakHp >= 0)
                b.breakProgressAt = now;
            b.breakHp = hp;
        }
        float since = b.breakProgressAt > b.swingSince ? b.breakProgressAt : b.swingSince;
        if (now - since > 10.0f)
        {
            g_shRebuildBan[b.object] = now + 1200.0f;
            g_shDemolishing.erase(b.object);
            g_shDemolishFails++;
            Log("Shelter: '%s' hit its team's %s for 10 s with the heavy axe and it didn't come down (health %.0f): leaves it (%d such)",
                ShBotName(bot).c_str(), ClassName(b.object), hp, g_shDemolishFails);
            b.plan.kind = 0;
            b.task = SH_NONE;
            return 0;
        }
        static int reports = 0;
        if (b.breakProgressAt > 0 && reports++ < 10)
            Log("Shelter: '%s' takes down its team's %s: health %.0f", ShBotName(bot).c_str(), ClassName(b.object), hp);
        return fmodf(now - b.swingSince, 0.7f) < 0.5f ? 0x800 : 0;
    }
    if (b.task == SH_BREAK)
    {
        // its health going down is progress (ShDecide gives up after 25 s without); 10 s of heavy hits and no damage:
        // the axe does nothing to this one, left alone for 5 minutes (logged)
        float hp = F<float>(Vars(b.object), EV_HEALTH);
        if (b.breakHp < 0 || hp < b.breakHp - 0.5f)
        {
            if (b.breakHp >= 0)
                b.breakProgressAt = now;
            b.breakHp = hp;
        }
        float since = b.breakProgressAt > b.swingSince ? b.breakProgressAt : b.swingSince;
        if (now - since > 10.0f)
        {
            b.useless[b.object] = now + 300.0f;
            static int reports = 0;
            if (reports++ < 20)
                Log("Shelter: '%s' hit a barricade for 10 s with the heavy axe and it didn't go down (health %.0f): leaves it",
                    String(F<int>(Vars(bot), EV_NETNAME)), hp);
            return 0;
        }
        // the heavy hit: held 0.5 s, released 0.2 s (it swings once per press)
        return fmodf(now - b.swingSince, 0.7f) < 0.5f ? 0x800 : 0;
    }
    // the axe is tapped (held, it was seen not to swing again): 0.3 s down, a moment up
    if (fmodf(now - b.swingSince, 0.4f) > 0.3f)
        return 0;
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
    return IN_ATTACK;
}

// ---------------------------------------------------------------- the player's menus: orders and the bots' skills
// Text menus (the game's ShowMenu message, one per 175 characters: mp's helper 0x113f8940 sends one message as it is; the
// client joins the parts while "more" is set). The keys come back as "menuselect <n>". Pages: the orders, the team's
// bots, one bot (its skills, auto level-up, its plan), a category of skills (two pages of 8).
enum ShMenuPage
{
    SHM_NONE,
    SHM_ORDERS,
    SHM_BOTS,
    SHM_BOT,
    SHM_CAT,
    SHM_PLAN,  // a bot's plan: the skills it will learn, in order
    SHM_ENTRY, // one step of the plan: earlier, later, first, remove
};
struct ShMenu
{
    ShMenuPage page = SHM_NONE;
    std::vector<void*> bots; // the bots page's list, by key
    void* bot = nullptr;
    int cat = 0, sub = 0;
    int planSub = 0, entry = 0;
};
static std::map<void*, ShMenu> g_shMenus; // by player

static void ShPrintAs(void* player, int dest, const char* text)
{
    if (!g_MessageBegin || !g_WriteString)
        return;
    int msg = *(int*)(MP_GMSG_TEXTMSG + g_mpDelta);
    if (msg <= 0)
        return;
    g_MessageBegin(1 /* MSG_ONE */, msg, nullptr, player);
    g_WriteByte(dest);
    g_WriteString(text);
    g_MessageEnd();
}

static void ShPrint(void* player, const char* text)
{
    ShPrintAs(player, 4 /* HUD_PRINTCENTER */, text);
}

static void ShMenuText(void* player, int keys, const char* text)
{
    void* p = PrivateOf(player);
    if (!p)
        return;
    __try
    {
        typedef void(__cdecl * ShowMenuFn)(void* player, int keys, int time, int more, const char* text);
        ShowMenuFn fn = (ShowMenuFn)(MP_SHOWMENU + g_mpDelta);
        size_t n = strlen(text), at = 0;
        char part[180];
        do
        {
            size_t len = n - at < 175 ? n - at : 175;
            memcpy(part, text + at, len);
            part[len] = 0;
            at += len;
            fn(p, keys, -1, at < n ? 1 : 0, part);
        } while (at < n);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Shelter: a menu couldn't be shown (exception 0x%08lX)", GetExceptionCode());
    }
}

// the team's bots (in the room's order)
static std::vector<void*> ShTeamBots(void* player)
{
    std::vector<void*> out;
    int team = TeamOf(player);
    for (auto& b : g_bots)
        if (b.edict && (F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT) && TeamOf(b.edict) == team)
            out.push_back(b.edict);
    return out;
}

static BotState* ShStateOf(void* bot)
{
    for (auto& b : g_bots)
        if (b.edict == bot)
            return &b;
    return nullptr;
}

static bool ShIsTeamBot(void* player, void* bot)
{
    for (void* b : ShTeamBots(player))
        if (b == bot)
            return true;
    return false;
}

static void ShShowPage(void* player)
{
    ShMenu& m = g_shMenus[player];
    char text[1024];
    int keys = 1 << 9; // 0
    size_t n = 0;
    auto add = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(text + n, sizeof(text) - n, fmt, ap);
        va_end(ap);
        if (w > 0)
            n = n + (size_t)w < sizeof(text) ? n + (size_t)w : sizeof(text) - 1;
    };
    if ((m.page == SHM_BOT || m.page == SHM_CAT) && !ShIsTeamBot(player, m.bot))
        m.page = SHM_BOTS; // the bot left
    if (m.page == SHM_ORDERS)
    {
        add("Shelter orders for the bots\n\n1. Gather resources\n2. Defend the shelter\n3. Repair buildings\n4. Build\n5. Follow me\n"
            "6. Free: each does its job\n7. Bots' skills\n\n0. Close");
        keys |= 0x7f;
    }
    else if (m.page == SHM_BOTS)
    {
        m.bots = ShTeamBots(player);
        if (m.bots.size() > 8)
            m.bots.resize(8);
        add("Bots' skills: pick a bot\n\n");
        for (size_t i = 0; i < m.bots.size(); i++)
        {
            void* bot = m.bots[i];
            BotState* st = ShStateOf(bot);
            add("%d. %s - %s - %d points - %s\n", (int)i + 1, ShBotName(bot).c_str(), st ? g_shRoleNames[ShJobOf(bot, *st)] : "?", ShPoints(bot),
                ShPlanOf(bot).manual ? "manual" : "auto");
            keys |= 1 << i;
        }
        if (m.bots.empty())
            add("(no bots in your team)\n");
        add("\n9. Back to the orders\n0. Close");
        keys |= 1 << 8;
    }
    else if (m.page == SHM_BOT)
    {
        void* bot = m.bot;
        BotState* st = ShStateOf(bot);
        ShSkillPlan& plan = ShPlanOf(bot);
        add("%s (%s): %d skill points\n", ShBotName(bot).c_str(), st ? g_shRoleNames[ShJobOf(bot, *st)] : "?", ShPoints(bot));
        for (int c = 0; c < 3; c++)
        {
            add("%s:", g_shCatNames[c]);
            int k = 0;
            for (int id : kSkillsByCat[c])
                if (int lv = ShSkillLevel(bot, id))
                    add("%s %s %d", k++ ? "," : "", kSkills[id].brief, lv);
            add(k ? "\n" : " -\n");
        }
        add("Plan:");
        for (size_t i = 0; i < plan.queue.size() && i < 6; i++)
            add("%s %s", i ? "," : "", kSkills[plan.queue[i]].brief);
        add(plan.queue.empty() ? " -\n" : plan.queue.size() > 6 ? "...\n" : "\n");
        if (!plan.manual && st)
        {
            std::vector<int> next = ShAutoOrder(bot, ShJobOf(bot, *st), plan);
            add("Then auto:");
            for (size_t i = 0; i < next.size() && i < 4; i++)
                add("%s %s", i ? "," : "", kSkills[next[i]].brief);
            add(next.empty() ? " -\n" : next.size() > 4 ? "...\n" : "\n");
        }
        add("\n1. Battle skills\n2. Survival skills\n3. Tech skills\n4. Auto level-up: %s\n5. Edit the plan\n"
            "6. Put the job's next skills in the plan\n7. Clear the plan\n\n9. Back\n0. Close",
            plan.manual ? "OFF (turn on)" : "ON (turn off)");
        keys |= 0x7f | (1 << 8);
    }
    else if (m.page == SHM_CAT)
    {
        void* bot = m.bot;
        ShSkillPlan& plan = ShPlanOf(bot);
        int points = ShPoints(bot);
        add("%s - %s skills (%d points)\n%s\n\n", ShBotName(bot).c_str(), g_shCatNames[m.cat], points,
            points > 0 ? "Pick one to learn it now" : "Pick one to plan it for its next points");
        for (int i = 0; i < 8; i++)
        {
            int at = m.sub * 8 + i;
            if (at >= 11)
                break;
            int id = kSkillsByCat[m.cat][at];
            const char* why = ShSkillBlock(bot, id);
            int planned = ShPlanned(plan, id);
            add("%d. %s %d/%d", i + 1, kSkills[id].name, ShSkillLevel(bot, id), kSkills[id].maxLv);
            if (why && !strcmp(why, "max"))
                add(" (max)");
            else if (why)
                add(" (%s)", why);
            if (planned)
                add(" [planned x%d]", planned);
            add("\n");
            keys |= 1 << i;
        }
        add("\n9. %s\n0. Back", m.sub ? "First page" : "More skills");
        keys |= 1 << 8;
    }
    else if (m.page == SHM_PLAN)
    {
        void* bot = m.bot;
        ShSkillPlan& plan = ShPlanOf(bot);
        int pages = ((int)plan.queue.size() + 7) / 8;
        if (m.planSub >= pages)
            m.planSub = 0;
        add("%s's plan: the skills it learns next, in order\nPick one to move or remove it\n\n", ShBotName(bot).c_str());
        int lv[33];
        for (int id = 0; id < 33; id++)
            lv[id] = ShSkillLevel(bot, id);
        for (int i = 0; i < (int)plan.queue.size(); i++)
        {
            int id = plan.queue[i];
            int level = ++lv[id];
            int key = i - m.planSub * 8;
            if (key < 0 || key >= 8)
                continue;
            add("%d. %s Lv %d\n", key + 1, kSkills[id].name, level);
            keys |= 1 << key;
        }
        if (plan.queue.empty())
            add("(empty: add skills from the categories, or put the job's next skills in it)\n");
        if (pages > 1)
        {
            add("\n9. Page %d of %d", m.planSub + 1, pages);
            keys |= 1 << 8;
        }
        add("\n0. Back");
    }
    else if (m.page == SHM_ENTRY)
    {
        void* bot = m.bot;
        ShSkillPlan& plan = ShPlanOf(bot);
        if (m.entry < 0 || m.entry >= (int)plan.queue.size())
        {
            m.page = SHM_PLAN;
            ShShowPage(player);
            return;
        }
        int id = plan.queue[m.entry];
        add("%s: step %d of %d\n%s - %s\n\n1. Earlier\n2. Later\n3. First\n4. Remove it\n\n0. Back", ShBotName(bot).c_str(), m.entry + 1,
            (int)plan.queue.size(), kSkills[id].name, kSkills[id].what);
        keys |= 0xf;
    }
    else
        return;
    ShMenuText(player, keys, text);
}

static void ShOpenMenu(void* player, ShMenuPage page)
{
    g_shMenus[player].page = page;
    ShShowPage(player);
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

// a skill picked in the menu: learned now when the bot has a point (and the tree allows it), else planned for later
static void ShPickSkill(void* player, void* bot, int id)
{
    ShSkillPlan& plan = ShPlanOf(bot);
    std::string name = ShBotName(bot);
    const char* why = ShSkillBlock(bot, id);
    char msg[160];
    if (why && !strcmp(why, "max"))
        snprintf(msg, sizeof(msg), "%s: %s is already at its max level", name.c_str(), kSkills[id].name);
    else if (why && !strncmp(why, "has ", 4))
        snprintf(msg, sizeof(msg), "%s %s: only one of these can be learned", name.c_str(), why);
    else if (ShSkillLevel(bot, id) + ShPlanned(plan, id) >= kSkills[id].maxLv)
        snprintf(msg, sizeof(msg), "%s: %s is already planned up to its max level", name.c_str(), kSkills[id].name);
    else if (!why && ShLearn(bot, id))
    {
        snprintf(msg, sizeof(msg), "%s learned %s (%d/%d): %s", name.c_str(), kSkills[id].name, ShSkillLevel(bot, id), kSkills[id].maxLv,
                 kSkills[id].what);
        ShSkillLog(bot, id, "picked by a player");
    }
    else
    {
        ShPlanAdd(bot, plan, id);
        ShPlansSave();
        snprintf(msg, sizeof(msg), "%s will learn %s with the next skill points%s: %s", name.c_str(), kSkills[id].name,
                 why ? ShFmt(" (after %s)", why + 6) : "", kSkills[id].what);
        Log("Shelter: a player plans %s for '%s' (%d planned)", kSkills[id].name, name.c_str(), (int)plan.queue.size());
    }
    ShPrint(player, msg);
    ShPrintAs(player, 2 /* HUD_PRINTCONSOLE */, ShFmt("%s\n", msg));
}

static void ShMenuKey(void* player, int key)
{
    ShMenu& m = g_shMenus[player];
    ShMenuPage page = m.page;
    m.page = SHM_NONE;
    if (key == 10)
        key = 0;
    switch (page)
    {
    case SHM_ORDERS:
        if (key == 1)
            ShGiveOrder(player, SHO_GATHER);
        else if (key == 2)
            ShGiveOrder(player, SHO_DEFEND);
        else if (key == 3)
            ShGiveOrder(player, SHO_REPAIR);
        else if (key == 4)
            ShGiveOrder(player, SHO_BUILD);
        else if (key == 5)
        {
            ShGiveOrder(player, SHO_FREE);
            GiveOrder(ORDER_FOLLOW, player);
            ShPrint(player, "Bots: following you");
        }
        else if (key == 6)
            ShGiveOrder(player, SHO_FREE);
        else if (key == 7)
            ShOpenMenu(player, SHM_BOTS);
        break;
    case SHM_BOTS:
        if (key >= 1 && key <= (int)m.bots.size())
        {
            m.bot = m.bots[key - 1];
            ShOpenMenu(player, SHM_BOT);
        }
        else if (key == 9)
            ShOpenMenu(player, SHM_ORDERS);
        break;
    case SHM_BOT:
        if (key >= 1 && key <= 3)
        {
            m.cat = key - 1;
            m.sub = 0;
            ShOpenMenu(player, SHM_CAT);
        }
        else if (key == 4)
        {
            ShSkillPlan& plan = ShPlanOf(m.bot);
            plan.manual = !plan.manual;
            ShPlansSave();
            ShPrint(player, ShFmt("%s: auto level-up %s", ShBotName(m.bot).c_str(), plan.manual ? "off (only your picks)" : "on"));
            Log("Shelter: a player turns auto level-up %s for '%s'", plan.manual ? "off" : "on", ShBotName(m.bot).c_str());
            ShOpenMenu(player, SHM_BOT);
        }
        else if (key == 5)
        {
            m.planSub = 0;
            ShOpenMenu(player, SHM_PLAN);
        }
        else if (key == 6)
        {
            BotState* st = ShStateOf(m.bot);
            ShSkillPlan& plan = ShPlanOf(m.bot);
            if (st)
            {
                std::vector<int> next = ShAutoOrder(m.bot, ShJobOf(m.bot, *st), plan);
                plan.queue.insert(plan.queue.end(), next.begin(), next.end());
                ShPlansSave();
                ShPrint(player, ShFmt("%s: %d skills of the job added to the plan", ShBotName(m.bot).c_str(), (int)next.size()));
            }
            m.planSub = 0;
            ShOpenMenu(player, SHM_PLAN);
        }
        else if (key == 7)
        {
            ShPlanOf(m.bot).queue.clear();
            ShPlansSave();
            ShPrint(player, ShFmt("%s: plan cleared", ShBotName(m.bot).c_str()));
            ShOpenMenu(player, SHM_BOT);
        }
        else if (key == 9)
            ShOpenMenu(player, SHM_BOTS);
        break;
    case SHM_PLAN:
        if (key >= 1 && key <= 8 && m.planSub * 8 + key - 1 < (int)ShPlanOf(m.bot).queue.size())
        {
            m.entry = m.planSub * 8 + key - 1;
            ShOpenMenu(player, SHM_ENTRY);
        }
        else if (key == 9)
        {
            m.planSub++;
            ShOpenMenu(player, SHM_PLAN);
        }
        else if (key == 0)
            ShOpenMenu(player, SHM_BOT);
        break;
    case SHM_ENTRY:
    {
        ShSkillPlan& plan = ShPlanOf(m.bot);
        int i = m.entry, n = (int)plan.queue.size();
        if (i >= 0 && i < n && key >= 1 && key <= 4)
        {
            if (key == 1 && i > 0)
                std::swap(plan.queue[i], plan.queue[i - 1]), m.entry = i - 1;
            else if (key == 2 && i + 1 < n)
                std::swap(plan.queue[i], plan.queue[i + 1]), m.entry = i + 1;
            else if (key == 3 && i > 0)
            {
                int id = plan.queue[i];
                plan.queue.erase(plan.queue.begin() + i);
                plan.queue.insert(plan.queue.begin(), id);
                m.entry = 0;
            }
            else if (key == 4)
                plan.queue.erase(plan.queue.begin() + i);
            ShPlansSave();
            m.planSub = m.entry / 8;
            ShOpenMenu(player, key == 4 ? SHM_PLAN : SHM_ENTRY);
        }
        else
            ShOpenMenu(player, SHM_PLAN);
        break;
    }
    case SHM_CAT:
        if (key >= 1 && key <= 8 && m.sub * 8 + key - 1 < 11)
        {
            if (ShIsTeamBot(player, m.bot))
                ShPickSkill(player, m.bot, kSkillsByCat[m.cat][m.sub * 8 + key - 1]);
            ShOpenMenu(player, SHM_CAT);
        }
        else if (key == 9)
        {
            m.sub = !m.sub;
            ShOpenMenu(player, SHM_CAT);
        }
        else if (key == 0)
            ShOpenMenu(player, SHM_BOT);
        break;
    default:
        break;
    }
}

// the bots' skills in the console (the "/skills" chat word also prints them)
static void ShPrintSkills(void* player)
{
    for (void* bot : ShTeamBots(player))
    {
        std::string line = ShBotName(bot) + ShFmt(" (%d points, %s):", ShPoints(bot), ShPlanOf(bot).manual ? "manual" : "auto");
        int k = 0;
        for (int id = 0; id < 33; id++)
            if (int lv = ShSkillLevel(bot, id))
                line += ShFmt("%s %s %d", k++ ? "," : "", kSkills[id].brief, lv);
        if (!k)
            line += " none";
        ShPrintAs(player, 2 /* HUD_PRINTCONSOLE */, (line + "\n").c_str());
    }
}

// a player's command in this mode: the orders menu (radio3 / the C key, "/orders"), the skills menu ("/skills"), their
// keys, the chat words, the rescue. True: the command stops here (the game's own radio3 menu, a key of our menus).
static bool ShelterPlayerCommand(void* player, const std::string& c)
{
    if (!ShRules())
    {
        g_shMenus.clear();
        return false;
    }
    if (c == "radio3")
    {
        ShOpenMenu(player, SHM_ORDERS);
        return true;
    }
    auto menu = g_shMenus.find(player);
    bool open = menu != g_shMenus.end() && menu->second.page != SHM_NONE;
    if (c == "menuselect" && open)
    {
        const char* a = g_CmdArgv(1);
        ShMenuKey(player, a ? atoi(a) : 0);
        return true;
    }
    if ((c == "menuselect" || c.compare(0, 5, "radio") == 0) && open)
        menu->second.page = SHM_NONE;
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
            ShOpenMenu(player, SHM_ORDERS);
        else if (t == "skills" || t == "skill")
        {
            ShPrintSkills(player);
            ShOpenMenu(player, SHM_BOTS);
        }
        else if (t == "autoskills on" || t == "autoskills off")
        {
            for (void* bot : ShTeamBots(player))
                ShPlanOf(bot).manual = t == "autoskills off";
            ShPlansSave();
            ShPrint(player, t == "autoskills on" ? "Bots: auto level-up on" : "Bots: auto level-up off (only your picks)");
        }
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

// Dead bots came back no more (the user saw it). The rules' death think (vfunc 0x118, 0x1083f450, run from
// CBasePlayer::PlayerDeathThink 0x114c3cb0) turns dead flag 2 into 3 once the player's wait is over (player +0xf74 the
// death time, plus min(50, random + 3 per two deaths - 3) s, maybe halved) and respawns on 3; nothing ran it for the bots.
// From here every dead bot gets it each half second from 3 s after its death; one still dead 60 s after dying is respawned
// (mp's respawn, as Zombie 4 does).
struct ShDead
{
    float since = 0, lastTry = 0;
    int tries = 0;
};
static std::map<void*, ShDead> g_shDead;

static void ShelterRespawnBots(float now)
{
    std::set<void*> seen;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        if ((F<int>(v, EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) != (FL_CLIENT | FL_FAKECLIENT) || F<int>(v, EV_DEADFLAG) == 0)
            return;
        int team = TeamOf(e);
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (!p || team < 1 || team > 2 || !Readable(p + 0xf74, 4))
            return;
        seen.insert(e);
        ShDead& d = g_shDead[e];
        if (d.since == 0)
        {
            d.since = now;
            Log("Shelter: bot '%s' died (dead flag %d, death time %.1f, now %.1f)", ShBotName(e).c_str(), F<int>(v, EV_DEADFLAG),
                *(float*)(p + 0xf74), now);
        }
        float died = *(float*)(p + 0xf74);
        if (died <= 0 || died > now + 1.0f)
            died = d.since;
        if (now - died < 3.0f || now - d.lastTry < 0.5f)
            return;
        d.lastTry = now;
        int before = F<int>(v, EV_DEADFLAG);
        bool ok;
        const char* how;
        if (now - died < 60.0f)
        {
            if (*(int*)(p + 0xe10) == 14)
                *(int*)(p + 0xe10) = 0; // (a buy menu "open" blocks it)
            ok = CallPlayerDeathThink(p);
            if (ok && F<int>(v, EV_DEADFLAG) == 3)
                ok = CallPlayerDeathThink(p);
            how = "death think";
        }
        else
        {
            ok = Z4Respawn(e);
            how = "respawn";
        }
        int after = F<int>(v, EV_DEADFLAG);
        if (d.tries++ < 3 || after == 0 || before != after || !strcmp(how, "respawn"))
        {
            static int reports = 0;
            if (reports++ < 200)
                Log("Shelter: dead bot '%s' %.0f s after dying: %s%s, dead flag %d -> %d", ShBotName(e).c_str(), now - died, how,
                    ok ? "" : " FAILED", before, after);
        }
    });
    for (auto it = g_shDead.begin(); it != g_shDead.end();)
        it = seen.count(it->first) ? std::next(it) : g_shDead.erase(it);
}

// the mode's state every 15 s (log)
static void ShelterWatch()
{
    static float next = 0;
    unsigned char* rules = ShRules();
    if (!rules || !g_pGlobals || !*g_pGlobals)
        return;
    float now = Now();
    ShelterRespawnBots(now);
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
    static int clockReports = 0;
    float tick = -1, per = -1, top = -1, bottom = -1, nightLen = -1;
    if (clockReports++ < 12)
    {
        ShSetting(MP_SHT_SET_TICK, tick), ShSetting(MP_SHT_SET_TICK_DIV, per), ShSetting(MP_SHT_SET_TOP, top);
        ShSetting(MP_SHT_SET_NIGHT_AT, bottom), ShSetting(MP_SHT_SET_NIGHT_LEN, nightLen);
        Log("Shelter: clock state %d hour %d, next tick in %.1f s; settings tick %.2f / %.2f s, top hour %.1f, night below %.1f, night %.0f s",
            *(int*)(rules + SH_CLOCK_STATE), *(int*)(rules + SH_CLOCK_HOUR), *(float*)(rules + SH_PHASE_END) - now, tick, per, top, bottom, nightLen);
    }
    Log("Shelter: day %d %s (phase %d, %.0f s left); shelter tier %d; storage wood %d/%d iron %d/%d power %d/%d; %d monsters, %d wood piles, "
        "%d rocks up, %d buildings (%d damaged)", ShDay(rules), ShNight(rules) ? "night" : "day", *(int*)(rules + SH_PHASE), ShPhaseLeft(rules),
        ShShelterTier(rules, 0), ShStorage(rules, t, 0), ShStorageMax(rules, t, 0), ShStorage(rules, t, 1), ShStorageMax(rules, t, 1),
        ShStorage(rules, t, 2), ShStorageMax(rules, t, 2), monsters, resources, minerals, buildings, damaged);
}
