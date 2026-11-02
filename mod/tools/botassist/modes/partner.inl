// botassist: Partner (mode 50, "partnermode"; rules CSOGameRulesPartner, vtable 0x11781bfc). Included by botassist.cpp at this
// point (one translation unit: it uses the shared helpers defined before it). Only runs in its own mode: every function
// here returns at once unless GameMode() == MODE_PTN.
//
// The mode (https://cso.fandom.com/wiki/Partner): bomb defuse where each player has an AI partner, chosen after the class
// ("selptn"), that only follows that player's orders (Z menu: cover, plant/defuse, hold, move, follow, report). The game has
// it all: at a player's spawn the rules (0x107434d0, 0x10744560) ask the bot manager for a bot, store it in a table at rules
// +0x284 + entity index * 0xc {partner, partner class, player class}, and fill the bot's blackboard (+0x374c: ownerIndex,
// skill, reaction, teamwork, aggression, accuracyMin/Max, health, speed). resource/partner/partner_bot_bt.xml plays it, a
// branch per "command" (1 cover, 2 plant/defuse, 3 hold, 4 move to destinationX/Y/Z, 5 follow, 6 help), each fighting first.
//
// What blocked it with room bots, and what botassist does:
//   - PtnCreatingPartner: the partner comes through AddBot, and RoomBotQuota refused it as a bot beyond the room's list (or
//     gave it a room AI player's place). rules +0x4f8 holds the owner's index while it is created: those pass. PtnIsPartner
//     keeps partners out of the room's bots (RoomBotOf, shared buying and characters): they stay the game's.
//   - PtnWatch: the mode's bot module never asks for bots (the real game had only players and their partners), so the room's AI
//     players were never added and the round never started. They are added here once a player has a team, keeping two client
//     slots per player (player and partner). A list with one side empty is split so both sides have someone.
//   - PtnBotThink: a room bot runs the same tree, with nobody to give it orders, so it gets them here: the C4 carrier goes to
//     plant, the closest bot picks up a dropped C4 or defuses, the others go to the bomb sites, hold a while and hunt.
//   - Owner: the rules' PlayerKilled (0x107430b0) returns before the base code when a bot's ownerIndex isn't a valid player (no
//     death notice, no score). A room bot is its own owner, which also gives it the kills.
//   - Skill: the blackboard's values come from the bot's room skill.
//   - ptn_villa picks one bomb target a round (rules +0x4f0, its center at +0x4fc) and players choose where to deploy
//     ("ptn_spawn", 0x107458e0): room bots draw a place.
//   - Orders: "radio1..3" are the partner's here, so botassist's own order menu stays closed (see OnPlayerCommand).
static const DWORD MP_PTN_RULES_VT = 0x11781bfc, MP_ZBBOT_MANAGER = 0x12183000, MP_ZBBOT_MANAGER_VT = 0x11991964;
static const DWORD MP_PTN_SPAWN = 0x107458e0, MP_BOT_PATH_CLEAR = 0x1156e3c0;
static const int PTN_TABLE = 0x284, PTN_TABLE_STEP = 0xc, PTN_CREATING = 0x4f8, PTN_ONE_TARGET = 0x4f0, PTN_TARGET_POS = 0x4fc;
static const int PTN_TARGETS_FIRST = 0x4a4, PTN_TARGETS_LAST = 0x4a8, PLAYER_BB = 0x374c, PLAYER_PATH = 0x3750, PLAYER_C4_SLOT = 0x1008;
static const int ZB_BOMB_PLANTED = 0x18, ZB_BOMB_DROPPED = 0x19, ZB_BOMB_DEFUSING = 0x1c;
static const int PTN_CMD_PLANT = 2, PTN_CMD_HOLD = 3, PTN_CMD_MOVE = 4;
static const float PTN_THINK = 1.0f, PTN_ARRIVED = 150.0f, PTN_GUARD_RANGE = 500.0f, PTN_SITE_RANGE = 450.0f, PTN_DEST_TIME = 30.0f;

static unsigned char* PtnRules()
{
    if (!g_mpDelta || GameMode() != MODE_PTN)
        return nullptr;
    unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    return rules && Readable(rules, 0x508) && *(DWORD*)rules == MP_PTN_RULES_VT + g_mpDelta ? rules : nullptr;
}

static bool PtnCreatingPartner()
{
    unsigned char* rules = PtnRules();
    return rules && *(int*)(rules + PTN_CREATING) >= 0;
}

static bool PtnIsPartner(void* bot)
{
    unsigned char* rules = PtnRules();
    if (!rules || !bot)
        return false;
    if (g_creatingBot && *(int*)(rules + PTN_CREATING) >= 0)
        return true;
    void* player = PrivateOf(bot);
    for (int i = 1; player && i < 34; i++)
        if (*(void**)(rules + PTN_TABLE + i * PTN_TABLE_STEP) == player)
            return true;
    return false;
}

// the game's blackboards (the bot's +0x374c, the manager's +0x10): thiscall vtable functions of their +4
typedef int(__fastcall* BbGetIntFn)(void* self, void* edx, const char* key, int def);
typedef float(__fastcall* BbGetFloatFn)(void* self, void* edx, const char* key, float def);
typedef void(__fastcall* BbSetIntFn)(void* self, void* edx, const char* key, int value);
typedef void(__fastcall* BbSetFloatFn)(void* self, void* edx, const char* key, float value, int extra);
static int BbInt(void* bb, const char* key, int def) { return ((BbGetIntFn)(*(void***)bb)[0x28 / 4])(bb, nullptr, key, def); }
static float BbFloat(void* bb, const char* key) { return ((BbGetFloatFn)(*(void***)bb)[0x2c / 4])(bb, nullptr, key, 0.0f); }
static void BbSet(void* bb, const char* key, int value) { ((BbSetIntFn)(*(void***)bb)[0x4c / 4])(bb, nullptr, key, value); }
static void BbSet(void* bb, const char* key, float value) { ((BbSetFloatFn)(*(void***)bb)[0x50 / 4])(bb, nullptr, key, value, 0); }

static void* PtnBotBoard(unsigned char* p)
{
    unsigned char* bb = Readable(p, 0x3788) ? *(unsigned char**)(p + PLAYER_BB) : nullptr;
    return bb && Readable(bb, 8) ? *(void**)(bb + 4) : nullptr;
}

static unsigned char* PtnManager()
{
    unsigned char* mgr = *(unsigned char**)(MP_ZBBOT_MANAGER + g_mpDelta);
    return mgr && Readable(mgr, 0x18) && *(DWORD*)mgr == MP_ZBBOT_MANAGER_VT + g_mpDelta ? mgr : nullptr;
}

static void* PtnGameBoard()
{
    unsigned char* mgr = PtnManager();
    unsigned char* bb = mgr ? *(unsigned char**)(mgr + 0x10) : nullptr;
    return bb && Readable(bb, 8) ? *(void**)(bb + 4) : nullptr;
}

static V3 PtnNavPoint(V3 around, float range)
{
    for (int tries = 0; tries < 40 && !g_nav.areas.empty(); tries++)
    {
        const NavArea& a = g_nav.areas[rand() % g_nav.areas.size()];
        if (Len2D(a.center - around) < range && fabsf(a.center.z - around.z) < 200.0f)
            return a.center + V3{ 0, 0, 36 };
    }
    int area = AreaAt(around);
    return area >= 0 ? g_nav.areas[area].center + V3{ 0, 0, 36 } : around;
}

// The bomb sites, read at each round: where a site's nav areas are, to plant inside it. A site without an area inside it
// keeps the area closest to its center.
struct PtnSite
{
    V3 center;
    std::vector<V3> spots;
};
static struct
{
    float round = -1;
    int level = -1, attack = 0;
    std::vector<PtnSite> sites;
} g_ptn;

static void PtnAddSite(V3 center, V3 lo, V3 hi)
{
    PtnSite site;
    site.center = center;
    for (auto& a : g_nav.areas)
        if (a.center.x >= lo.x && a.center.x <= hi.x && a.center.y >= lo.y && a.center.y <= hi.y &&
            a.center.z >= lo.z - 72.0f && a.center.z <= hi.z + 72.0f)
            site.spots.push_back(a.center + V3{ 0, 0, 36 });
    if (site.spots.empty())
    {
        int area = AreaAt(center);
        if (area >= 0)
            site.spots.push_back(g_nav.areas[area].center + V3{ 0, 0, 36 });
    }
    if (!site.spots.empty())
        g_ptn.sites.push_back(site);
}

static void PtnReadSites(unsigned char* rules, float round)
{
    if (g_ptn.round == round && g_ptn.level == g_nav.level)
        return;
    g_ptn.round = round;
    g_ptn.level = g_nav.level;
    g_ptn.sites.clear();
    bool one = rules[PTN_ONE_TARGET] != 0;
    V3 only = Vec((float*)(rules + PTN_TARGET_POS));
    unsigned char** first = *(unsigned char***)(rules + PTN_TARGETS_FIRST);
    unsigned char** last = *(unsigned char***)(rules + PTN_TARGETS_LAST);
    for (unsigned char** t = first; t && t < last; t++)
    {
        unsigned char* pev = *t && Readable(*t, 0x10) ? *(unsigned char**)(*t + 8) : nullptr;
        if (!pev || !Readable(pev, 0xf0))
            continue;
        V3 lo = Vec((float*)(pev + 0xd4)), hi = Vec((float*)(pev + 0xe0));
        V3 center = (lo + hi) * 0.5f;
        if (!one || Len(center - only) < 8.0f)
            PtnAddSite(center, lo, hi);
    }
    g_ptn.attack = g_ptn.sites.empty() ? 0 : rand() % (int)g_ptn.sites.size();
    static int reports = 0;
    if (reports++ < 30)
    {
        int spots = 0;
        for (auto& s : g_ptn.sites)
            spots += (int)s.spots.size();
        Log("Partner: new round, %d bomb site(s) in reach, %d places to plant", (int)g_ptn.sites.size(), spots);
    }
}

// the room bot of a team closest to a place
static void* PtnClosestBot(int team, V3 at)
{
    void* best = nullptr;
    float bestDist = 1e9f;
    ForEntitiesNear(at, 1e6f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS);
        if (!(flags & FL_CLIENT) || !(flags & FL_FAKECLIENT) || !Alive(e) || PtnIsPartner(e) || TeamOf(e) != team)
            return;
        float d = Len(Origin(e) - at);
        if (d < bestDist)
        {
            bestDist = d;
            best = e;
        }
    });
    return best;
}

static bool PtnNearestEnemy(void* bot, int team, V3& out)
{
    V3 me = Origin(bot);
    float best = 1e9f;
    ForEntitiesNear(me, 1e6f, [&](void* e) {
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || !TeamOf(e) || TeamOf(e) == team)
            return;
        float d = Len(Origin(e) - me);
        if (d < best)
        {
            best = d;
            out = Origin(e);
        }
    });
    return best < 1e9f;
}

struct PtnBot
{
    float round = -1, next = 0, holdUntil = 0, plantTry = 0, destAt = 0;
    int cmd = 0, spot = 0;
    V3 dest = { 0, 0, 0 };
    bool hasDest = false;
};
static std::map<void*, PtnBot> g_ptnBots;

static void PtnOrder(unsigned char* p, void* board, PtnBot& z, int cmd, V3 dest)
{
    bool newDest = cmd != PTN_CMD_HOLD && (!z.hasDest || Len(dest - z.dest) > 32.0f);
    bool newCmd = BbInt(board, "command", 0) != cmd;
    if (newDest)
    {
        z.dest = dest;
        z.hasDest = true;
        z.destAt = Now();
        BbSet(board, "destinationX", dest.x);
        BbSet(board, "destinationY", dest.y);
        BbSet(board, "destinationZ", dest.z);
    }
    if (newCmd)
        BbSet(board, "command", cmd);
    z.cmd = cmd;
    if (newDest || newCmd)
    {
        typedef void(__fastcall * PathClearFn)(void* path);
        void* path = *(void**)(p + PLAYER_PATH);
        if (path)
            ((PathClearFn)(MP_BOT_PATH_CLEAR + g_mpDelta))(path);
    }
}

// a place to go when there's no job to do: the site, then hunting
static void PtnRoam(void* bot, unsigned char* p, void* board, PtnBot& z, int team, float now)
{
    V3 me = Origin(bot);
    // (a place it can't reach in that time is given up)
    if (z.hasDest && z.cmd == PTN_CMD_MOVE && Len(z.dest - me) > PTN_ARRIVED && now - z.destAt < PTN_DEST_TIME)
    {
        PtnOrder(p, board, z, PTN_CMD_MOVE, z.dest);
        return;
    }
    if (z.cmd == PTN_CMD_MOVE && z.hasDest)
        z.holdUntil = now + (team == 2 ? 6.0f + Rnd() * 8.0f : 1.0f + Rnd() * 3.0f); // arrived: defenders stay longer
    if (now < z.holdUntil)
    {
        PtnOrder(p, board, z, PTN_CMD_HOLD, z.dest);
        return;
    }
    V3 enemy;
    bool hunt = rand() % 100 < (team == 2 ? 40 : 55);
    if (hunt && PtnNearestEnemy(bot, team, enemy))
    {
        PtnOrder(p, board, z, PTN_CMD_MOVE, PtnNavPoint(enemy, 200.0f));
        return;
    }
    V3 goal = me;
    if (!g_ptn.sites.empty())
    {
        size_t site = team == 1 && rand() % 100 < 70 ? (size_t)g_ptn.attack : (size_t)rand() % g_ptn.sites.size();
        goal = g_ptn.sites[site].center;
    }
    PtnOrder(p, board, z, PTN_CMD_MOVE, PtnNavPoint(goal, PTN_SITE_RANGE));
}

// Runs for every bot, dead or alive, in the frame of its move
static void PtnBotThink(void* bot)
{
    unsigned char* rules = PtnRules();
    if (!rules || !PtnManager() || PtnIsPartner(bot))
        return;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    void* board = p ? PtnBotBoard(p) : nullptr;
    unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
    if (!board || !cso || !Readable(cso, 0x280) || !g_IndexOfEdict)
        return;
    if (BbInt(board, "ownerIndex", 0) == 0)
    {
        // its own owner (see the top), and its abilities as a partner's, from its room skill
        BotState& st = StateFor(bot);
        MatchBot* rb = RoomBotOf(bot, st);
        int skill = rb && rb->skill >= 0 ? rb->skill : (int)g_difficulty;
        skill = skill < 0 ? 0 : skill > 7 ? 7 : skill;
        BbSet(board, "ownerIndex", g_IndexOfEdict(bot));
        BbSet(board, "skill", 40 + skill * 8);
        BbSet(board, "reaction", 35 + skill * 8);
        BbSet(board, "teamwork", 70);
        BbSet(board, "aggression", 50 + skill * 6);
        BbSet(board, "accuracyMin", 0.55f + skill * 0.05f);
        BbSet(board, "accuracyMax", 1.5f - skill * 0.07f);
        static int reports = 0;
        if (reports++ < 40)
            Log("Partner: room bot '%s' plays with skill %d", String(F<int>(Vars(bot), EV_NETNAME)), skill);
    }
    if (!Alive(bot))
        return;
    PtnBot& z = g_ptnBots[bot];
    float now = Now();
    float round = *(float*)(cso + 0x248);
    int team = *(int*)(p + 0x6c);
    if ((team != 1 && team != 2) || !cso[0x27c])
        return;
    PtnReadSites(rules, round);
    if (z.round != round)
    {
        z = PtnBot();
        z.round = round;
        z.spot = rand();
        z.next = now + 0.2f + Rnd() * PTN_THINK;
        if (rules[PTN_ONE_TARGET] && now < round + *(int*)(cso + 0x250))
        {
            typedef void(__fastcall * PtnSpawnFn)(void* rules, void* edx, void* player, int place);
            ((PtnSpawnFn)(MP_PTN_SPAWN + g_mpDelta))(rules, nullptr, p, rand() % (team == 1 ? 4 : 2));
        }
    }
    if (now < z.next)
        return;
    z.next = now + PTN_THINK;
    void* game = PtnGameBoard();
    int bomb = game ? BbInt(game, "bombState", 0) : 0;
    V3 me = Origin(bot);
    if (bomb == ZB_BOMB_PLANTED || bomb == ZB_BOMB_DEFUSING)
    {
        V3 planted = { BbFloat(game, "plantedBombPosX"), BbFloat(game, "plantedBombPosY"), BbFloat(game, "plantedBombPosZ") };
        // a CT defuses, the closest one; the others keep the place (the tree sets the destination to the bomb)
        if (team == 2 && (BbInt(game, "bombDefuser", 0) == g_IndexOfEdict(bot) ||
                          (bomb == ZB_BOMB_PLANTED && PtnClosestBot(2, planted) == bot)))
        {
            PtnOrder(p, board, z, PTN_CMD_PLANT, planted);
            return;
        }
        if (Len(z.dest - planted) > PTN_GUARD_RANGE * 1.5f || !z.hasDest || z.cmd == PTN_CMD_PLANT)
            PtnOrder(p, board, z, PTN_CMD_MOVE, PtnNavPoint(planted, PTN_GUARD_RANGE));
        else if (Len(z.dest - me) < PTN_ARRIVED)
            PtnOrder(p, board, z, PTN_CMD_HOLD, z.dest);
        return;
    }
    if (team == 1)
    {
        int slot = *(int*)(p + PLAYER_C4_SLOT);
        bool hasC4 = slot && Readable((void*)slot, 8) && *(int*)(slot + 4);
        if (hasC4 && !g_ptn.sites.empty())
        {
            const PtnSite& site = g_ptn.sites[g_ptn.attack % g_ptn.sites.size()];
            V3 spot = site.spots[z.spot % site.spots.size()];
            // there and not planting: the tree plants inside a bomb zone within 64 units of the destination
            if (Len(spot - me) < 80.0f)
            {
                if (!z.plantTry)
                    z.plantTry = now;
                else if (now - z.plantTry > 6.0f)
                {
                    z.spot++;
                    z.plantTry = 0;
                    spot = site.spots[z.spot % site.spots.size()];
                    static int reports = 0;
                    if (reports++ < 20)
                        Log("Partner: bot '%s' couldn't plant there: next place", String(F<int>(Vars(bot), EV_NETNAME)));
                }
            }
            PtnOrder(p, board, z, PTN_CMD_PLANT, spot);
            return;
        }
        if (bomb == ZB_BOMB_DROPPED && game)
        {
            void* c4 = ((EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX])(BbInt(game, "bombEntIndex", 0));
            if (c4 && PtnClosestBot(1, Origin(c4)) == bot)
            {
                PtnOrder(p, board, z, PTN_CMD_PLANT, Origin(c4)); // the tree goes for the C4 first
                return;
            }
        }
    }
    PtnRoam(bot, p, board, z, team, now);
}

// the mode never asks for bots: the room's AI players are added here
static void PtnWatch()
{
    static float next = 0, nextReport = 0;
    unsigned char* rules = PtnRules();
    if (!rules || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0)
        return;
    float now = Now();
    if (now >= next - 5.0f && now < next)
        return;
    next = now + 0.5f;
    int humans[3] = { 0, 0, 0 }, alive[3] = { 0, 0, 0 }, partners = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS);
        int team = (flags & FL_CLIENT) ? TeamOf(e) : 0;
        if (team != 1 && team != 2)
            return;
        if (!(flags & FL_FAKECLIENT))
            humans[team]++;
        else if (PtnIsPartner(e))
            partners++;
        if (Alive(e))
            alive[team]++;
    });
    if (now >= nextReport || now < nextReport - 30.0f)
    {
        static int reports = 0;
        nextReport = now + 15.0f;
        void* game = PtnGameBoard();
        if (reports++ < 80 && humans[1] + humans[2])
            Log("Partner: %d T and %d CT alive, %d players, %d partners, bomb state %d", alive[1], alive[2], humans[1] + humans[2],
                partners, game ? BbInt(game, "bombState", 0) : -1);
    }
    if (!humans[1] && !humans[2])
        return; // the sides are decided once a player has a team
    LoadNav();
    UpdateMatchBots();
    unsigned char* mgr = PtnManager();
    if (!mgr || g_matchFetch != 2 || !g_matchKnown)
        return;
    int want[3] = { 0, 0, 0 };
    EnterCriticalSection(&g_matchLock);
    for (auto& m : g_matchBots)
        if (!m.fakePlayer)
            want[m.team == 1 ? 1 : 2]++;
    LeaveCriticalSection(&g_matchLock);
    int added = g_matchAdded[1] + g_matchAdded[2], total = want[1] + want[2];
    int room = *(int*)((DWORD)*g_pGlobals + GV_MAXCLIENTS) - (humans[1] + humans[2]) * 2;
    if (added >= total)
        return;
    if (added >= room)
    {
        static int reports = 0;
        if (reports++ < 3)
            Log("Partner: %d of the room's %d AI players fit, each player and partner take a slot", room, total);
        return;
    }
    int t;
    if (want[1] + humans[1] == 0 || want[2] + humans[2] == 0)
        t = humans[1] * 2 + g_matchAdded[1] <= humans[2] * 2 + g_matchAdded[2] ? 1 : 2; // one side would be empty: even them
    else
        t = g_matchAdded[1] < want[1] ? 1 : 2;
    ((AddBotFn)(*(void***)mgr)[2])(mgr, nullptr, t == 1 ? 0 : 1);
}
