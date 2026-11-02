// botassist: Zombie Conquest (mode 56, "Zombi_teamcontrol"; rules CSOGameRulesZombiConquest, vtable 0x117919e0). Included
// by botassist.cpp at this point (one translation unit: it uses the shared helpers defined before it). Only runs in its
// own mode: ZConWatch returns and ZConBotThink / ZConMove aren't called unless GameMode() == MODE_ZCON.
//
// The mode (https://cso.fandom.com/wiki/Zombie_Conquest): T are zombies (joining T makes a player a zombie, 0x113f2f00),
// CT humans, and the teams swap sides every round. Humans capture 3 bases in turn, zombies keep them out. Everyone
// comes back after a wait. resource/ZombiConquest/ConquestInfo.json (shipped with the game) holds the bases and safe
// areas of 11 maps and the humans' 24 weapon sets.
//
// Rules facts: bases at rules +0xaac..+0xab0, 0x34 bytes each: +0 base level (0..2), +4 position, +0x10 half size, +0x1c
// active, +0x20 state (0 free, 1 capturing, 2 captured and scoring, 3 going back, 4 contested), +0x2c points, +0x30 its
// conquestareaentity. FUN_107aa6e0 counts who's inside: humans alone capture, a zombie inside contests.
// Player module 0x12 (CZombiConquestModule): +0x10 human ability (0 not chosen, 1 enhanced set, 2 own weapons), +0x14 set
// chosen, +0x18 awakening gauge (100 = ready). Commands (rules ClientCommand 0x107af710): "ZBCon_PreBuy <set>" chooses
// the set whose weapons rules vfunc 0x36c then allows (item_<id>; nothing else can be bought), "ZBCon_HMAbil <n>" from
// the second base on, "ZBCon_UseSpecial" awakening, "selzmb random" (0x1144f3c0: only classes this base allows).
//
// What botassist does:
//   - Teams: RoomBotQuota (shared, see there) gives the game the side it asks for: rooms list every AI player on the
//     host's side (the lobby's teams are merged in this mode), so the requests for T bots were refused: no zombies.
//   - ZConWatch: dead bots stay down as in Zombie 4 and Mutation (nothing runs their PlayerDeathThink once respawnable);
//     it runs it, for humans and zombies. The game keeps its own wait (rules vfunc 0x1d4). Forced respawn as a last
//     resort. botassist.log: "ZCon: ... still down".
//   - ZConBotThink: a human bot chooses a weapon set of the current base each life (the room's weapons can't be bought
//     here, shared UpdateBuying skips this mode); a zombie bot draws its class; both use skills and awakening.
//   - ZConMove: replaces only the bots' movement (the stock AI keeps aiming and shooting), see there.
//   - ApplyRoomSkill (shared): room AI players without a skill of their own play at ZCON_SKILL here: humans have a moment
//     to stop a zombie in a small area.
//
// How a base works (FUN_107aa6e0): humans inside and no zombie: 0 -> 1, and after the capture time 1 -> 2, which scores
// while a human stays inside. A zombie with them: 4, the clock stops. No human left: 1 -> 0 (the capture starts over), 2
// -> 3, and after a while 3 -> 0 (the base is lost). So a base held needs someone inside, not everyone.
static const DWORD MP_ZCON_RULES_VT = 0x117919e0, MP_ZCON_PREBUY = 0x107af350, MP_ZCON_HMABIL = 0x1080ea30;
static const int ZCON_MODULE = 0x12, ZCON_SKILL = 6;
static const float ZCON_CHASE = 1100.0f, ZCON_HEAR = 400.0f, ZCON_CHASE_PATH = 1500.0f, ZCON_FREE_DROP = 120.0f;
static const float ZCON_FLEE = 420.0f, ZCON_FLEE_INSIDE = 300.0f, ZCON_DEFEND_RING = 300.0f;
static const float ZCON_ROUTE_COST = 300.0f, ZCON_BLOCK_COST = 3000.0f;

struct ZConBase
{
    V3 pos, half;
    int level, state;
};

// the base being fought for; false between rounds or outside this mode's rules
static bool ZConActiveBase(ZConBase& out)
{
    __try
    {
        unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
        unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
        if (!rules || !cso || !Readable(rules, 0xb00) || *(DWORD*)rules != MP_ZCON_RULES_VT + g_mpDelta ||
            cso[0x27c] != 1 || cso[0x27d])
            return false;
        unsigned char* a = *(unsigned char**)(rules + 0xaac);
        unsigned char* end = *(unsigned char**)(rules + 0xab0);
        for (; a && a < end; a += 0x34)
            if (a[0x1c])
            {
                out.level = *(int*)a;
                out.pos = Vec((float*)(a + 4));
                out.half = Vec((float*)(a + 0x10));
                out.state = *(int*)(a + 0x20);
                return true;
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

struct ZConDead
{
    float since = 0, respawnableSince = 0, lastTry = 0;
    int tries = 0;
};
static std::map<void*, ZConDead> g_zconDead;

static void ZConWatch()
{
    static float next = 0;
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_ZCON)
    {
        g_zconDead.clear();
        return;
    }
    float now = Now();
    if (now >= next - 10.0f && now < next)
        return;
    next = now + 0.25f;
    std::set<void*> seen;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        if ((F<int>(v, EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) != (FL_CLIENT | FL_FAKECLIENT))
            return;
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (!p || !Readable(p, 0x2170))
            return;
        const char* name = String(F<int>(v, EV_NETNAME));
        int deadFlag = F<int>(v, EV_DEADFLAG);
        auto it = g_zconDead.find(e);
        if (deadFlag == 0 && F<float>(v, EV_HEALTH) > 0)
        {
            if (it != g_zconDead.end())
            {
                if (it->second.tries)
                    Log("ZCon: %s '%s' is up again after %.0f s (botassist's help)", p[0x2166] ? "zombie" : "human", name,
                        now - it->second.since);
                g_zconDead.erase(it);
            }
            return;
        }
        seen.insert(e);
        ZConDead& d = g_zconDead[e];
        if (d.since == 0)
            d.since = now;
        if (deadFlag == 3 && d.respawnableSince == 0)
            d.respawnableSince = now;
        bool due = (deadFlag == 3 && now - d.respawnableSince >= 1.0f) || now - d.since >= 8.0f;
        if (!due || now - d.lastTry < 1.0f)
            return;
        unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
        if (!cso || !Readable(cso, 0x280) || cso[0x27c] != 1 || cso[0x27d])
            return; // between rounds the game brings everyone back
        d.lastTry = now;
        int before = deadFlag;
        bool forced = now - d.since >= 30.0f;
        bool ok = forced ? Z4Respawn(e) : CallPlayerDeathThink(p);
        if (!forced && ok && F<int>(v, EV_DEADFLAG) == 3)
            ok = CallPlayerDeathThink(p);
        if (d.tries++ < 3 || forced)
            Log("ZCon: %s '%s' still down %.0f s after dying (dead flag %d): %s%s -> dead flag %d, health %.0f",
                p[0x2166] ? "zombie" : "human", name, now - d.since, before, forced ? "forced respawn" : "death think",
                ok ? "" : " FAILED", F<int>(v, EV_DEADFLAG), F<float>(v, EV_HEALTH));
    });
    for (auto it = g_zconDead.begin(); it != g_zconDead.end();)
        it = seen.count(it->first) ? std::next(it) : g_zconDead.erase(it);
}

// ConquestInfo.json's weapon sets by base: first set and how many (limits), and their items: primary, pistol, melee, grenade
static const int ZCON_SET_FIRST[3] = { 0, 6, 15 }, ZCON_SET_COUNT[3] = { 6, 9, 9 };
static const int ZCON_SETS[24][4] = {
    { 8193, 270, 816, 31 },   { 8113, 768, 776, 31 },    { 8295, 8219, 363, 31 },   { 964, 8219, 8088, 31 },
    { 893, 768, 776, 31 },    { 564, 270, 816, 31 },     { 8396, 8767, 8164, 8886 }, { 8414, 8192, 8611, 8886 },
    { 8132, 8767, 8164, 8104 }, { 8890, 978, 8384, 8104 }, { 8539, 8192, 8384, 31 },  { 8221, 768, 8611, 31 },
    { 8851, 8767, 8384, 31 }, { 8078, 768, 841, 8886 },  { 8752, 978, 841, 984 },   { 8094, 8351, 8753, 8148 },
    { 8525, 8194, 8047, 8820 }, { 8327, 8108, 8835, 8886 }, { 8936, 8720, 8753, 8148 }, { 8955, 8720, 8865, 8886 },
    { 8385, 8351, 8426, 8820 }, { 8956, 8108, 8835, 8886 }, { 8563, 8351, 8865, 8148 }, { 8666, 8194, 8611, 8820 },
};

struct ZConBot
{
    float buyAt = 0, nextSkill = 0, nextSpecial = 0, nextCheck = 0, zombieSince = 0;
    int buyTries = 0, set = -1;
    bool wasZombie = false, picked = false;
    // ZConMove
    V3 spot{};
    float spotUntil = 0, yaw = 0, lastSeen = 0, fleeUntil = 0, noChaseUntil = 0, nerve = 0;
    float routeUntil = 0, dodgeUntil = 0, dodgeYaw = 0, jumpAt = 0, stuckAt = 0, nextStuckLog = 0;
    int spotBase = -1, stuck = 0, passSide = 0;
    bool yawSet = false;
    std::vector<float> route, blocked; // nav area costs of its own way (ZConNewRoute) and of ways found blocked
};
static std::map<void*, ZConBot> g_zconBots;

// a human bot's weapons: the set is chosen, then bought as the game buys its own item_<id> (internal-buy flag, see
// UpdateBuying). What it carried of the old set is thrown away first, or the game wouldn't replace it.
static void ZConBuySet(void* bot, unsigned char* p, ZConBot& z, int level)
{
    if (level < 0 || level > 2)
        level = 0;
    z.set = ZCON_SET_FIRST[level] + rand() % ZCON_SET_COUNT[level];
    // "ZBCon_PreBuy <set>" and "ZBCon_HMAbil <n>" read their argument straight from the engine (0x121d3bb8), not through
    // the bots' fake command arguments (0x113f1e50): a bot's would be whatever the player typed last. So their handlers
    // are called directly: 0x107af350 (thiscall rules, player, set), 0x1080ea30 (thiscall module, ability).
    typedef void(__thiscall * PreBuyFn)(void* rules, void* player, int set);
    typedef void(__thiscall * HmAbilFn)(void* module, int ability);
    void* rules = *(void**)(MP_GAMERULES + g_mpDelta);
    unsigned char* mod = (unsigned char*)((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(p, ZCON_MODULE);
    if (!rules || !mod)
        return;
    if (level > 0 && *(int*)(mod + 0x10) == 0)
        ((HmAbilFn)(MP_ZCON_HMABIL + g_mpDelta))(mod, 1);
    ((PreBuyFn)(MP_ZCON_PREBUY + g_mpDelta))(rules, p, z.set);
    unsigned char wasInternal = p[0x18b4];
    p[0x18b4] = 1;
    FreeSlot(bot, 1);
    FreeSlot(bot, 2);
    FakeCommand(bot, "client_buy_open");
    for (int item : ZCON_SETS[z.set])
        BuyAs(bot, item);
    FakeCommand(bot, "primammo");
    FakeCommand(bot, "primammo");
    FakeCommand(bot, "secammo");
    FakeCommand(bot, "client_buy_close");
    p[0x18b4] = wasInternal;
    FakeCommand(bot, "slot1");
    static int reports = 0;
    if (reports++ < 40)
        Log("ZCon: human bot '%s' takes weapon set %d of base %d: %d %d %d %d -> primary %s",
            String(F<int>(Vars(bot), EV_NETNAME)), z.set, level + 1, ZCON_SETS[z.set][0], ZCON_SETS[z.set][1],
            ZCON_SETS[z.set][2], ZCON_SETS[z.set][3], SlotClass(bot, 1).c_str());
}

static void ZConBotThink(void* bot, BotState& st)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2f00))
        return;
    ZConBot& z = g_zconBots[bot];
    float now = Now();
    bool zombie = p[0x2166] != 0;
    if (st.newLife || zombie != z.wasZombie)
    {
        z.wasZombie = zombie;
        z.zombieSince = now;
        z.picked = false;
        z.buyAt = zombie ? 0 : now + 0.3f;
        z.buyTries = 0;
        z.nextSkill = now + 5.0f;
        z.nextSpecial = now + 10.0f;
        z.spotUntil = z.routeUntil = z.fleeUntil = z.noChaseUntil = 0;
        z.stuck = 0;
    }
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    if (zombie && !z.picked && now - z.zombieSince > 1.0f)
    {
        z.picked = true;
        if (!p[0x2e39])
        {
            if (g_zbSkillRows)
                memset(p + 0x2171, 1, 27); // the draw only takes owned classes, of those this base allows (see Mutation)
            FakeCommand(bot, "selzmb", "random");
            static int reports = 0;
            if (reports++ < 40)
                Log("ZCon: zombie bot '%s' draws a class: now class %d", name, (signed char)p[0x218e]);
        }
    }
    if (!zombie && z.buyAt && now >= z.buyAt)
    {
        ZConBase base;
        ZConBuySet(bot, p, z, ZConActiveBase(base) ? base.level : 0);
        z.buyAt = ++z.buyTries < 3 && SlotClass(bot, 1).empty() ? now + 2.0f : 0;
    }
    if (now < z.nextCheck)
        return;
    z.nextCheck = now + 0.5f;
    if (now < z.nextSkill && now < z.nextSpecial)
        return;
    // the closest enemy in sight
    V3 me = Origin(bot), eye = Eye(bot);
    float range = zombie ? 700.0f : 400.0f, best = 1e9f;
    ForEntitiesNear(me, range, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || (q[0x2166] != 0) == zombie)
            return;
        float d = Len(Origin(e) - me);
        if (d < best && CanSee(bot, eye, e))
            best = d;
    });
    if (best > range)
        return;
    if (now >= z.nextSpecial)
    {
        z.nextSpecial = now + 5.0f;
        unsigned char* mod = (unsigned char*)((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(p, ZCON_MODULE);
        if (mod && *(int*)(mod + 0x18) >= 100)
        {
            FakeCommand(bot, "ZBCon_UseSpecial");
            static int reports = 0;
            if (reports++ < 30)
                Log("ZCon: bot '%s' (%s) awakens", name, zombie ? "zombie" : "human");
        }
    }
    if (now >= z.nextSkill)
    {
        z.nextSkill = now + 4.0f + Rnd() * 4.0f;
        if (Rnd() < 0.5f)
            return;
        FakeCommand(bot, "useitem", zombie ? (Rnd() < 0.25f ? "zb_skill1" : "zb_skill0") : "hm_skill0");
    }
}

// a way close to `yaw` free of walls and of deadly drops, within what the bot may drop; false: none
static bool ZConSafeYaw(void* bot, V3 me, float yaw, float maxDrop, float& out)
{
    static const float turns[] = { 0, 30, -30, 60, -60, 90, -90, 120, -120, 150, -150 };
    TraceResult tr;
    V3 head = me + V3{ 0, 0, 20 };
    for (float t : turns)
    {
        float y = yaw + t;
        if (t != 0 && !Trace(head, head + V3{ cosf(y / RAD) * 40.0f, sinf(y / RAD) * 40.0f, 0 }, bot, 1, tr))
            continue;
        if (!DropAhead(bot, me, y, 72.0f, t == 0 ? maxDrop : ZCON_FREE_DROP))
        {
            out = y;
            return true;
        }
    }
    return false;
}

// a point on the nav mesh in the base's box grown by `pad` (a random point of the box could be a wall or thin air);
// outside: only areas whose centre is out of the box itself
static bool ZConNavSpot(const ZConBase& base, float pad, bool outside, V3& out)
{
    float hx = base.half.x + pad, hy = base.half.y + pad;
    std::vector<int> areas;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        if (NavAreaHurts((int)i) || a.hi[0] <= base.pos.x - hx || a.lo[0] >= base.pos.x + hx || a.hi[1] <= base.pos.y - hy ||
            a.lo[1] >= base.pos.y + hy || fabsf(a.center.z - base.pos.z) > base.half.z + (outside ? 60.0f : 0.0f))
            continue;
        if (outside && fabsf(a.center.x - base.pos.x) <= base.half.x && fabsf(a.center.y - base.pos.y) <= base.half.y)
            continue;
        areas.push_back((int)i);
    }
    if (areas.empty())
        return false;
    const NavArea& a = g_nav.areas[areas[rand() % areas.size()]];
    // kept 16 u from the box's edges: the game counts the origin inside it
    float x0 = max(a.lo[0], base.pos.x - hx + 16.0f), x1 = min(a.hi[0], base.pos.x + hx - 16.0f);
    float y0 = max(a.lo[1], base.pos.y - hy + 16.0f), y1 = min(a.hi[1], base.pos.y + hy - 16.0f);
    out.x = x1 > x0 ? x0 + Rnd() * (x1 - x0) : a.center.x;
    out.y = y1 > y0 ? y0 + Rnd() * (y1 - y0) : a.center.y;
    out.z = AreaZ(a, out.x, out.y);
    return true;
}

// where to go next around the base. Humans: inside it; once it's captured and two others are inside, half the time
// around it. Zombies: around it, and now and then to where the closest human is.
static void ZConPickSpot(void* bot, ZConBot& z, const ZConBase& base, bool zombie, bool nearBase)
{
    float now = Now();
    bool ok = false;
    V3 me = Origin(bot);
    if (zombie && nearBase && Rnd() < 0.35f)
    {
        float best = 1e9f;
        ForEntitiesNear(me, 1e6f, [&](void* e) {
            unsigned char* q = (unsigned char*)PrivateOf(e);
            if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || !q || !Readable(q, 0x2170) || q[0x2166])
                return;
            float d = Len(Origin(e) - me);
            if (d < best)
            {
                best = d;
                z.spot = Origin(e);
                ok = true;
            }
        });
    }
    else if (!zombie && base.state == 2 && Rnd() < 0.5f)
    {
        int inside = 0;
        ForEntitiesNear(base.pos, Len(base.half) + 50.0f, [&](void* e) {
            unsigned char* q = (unsigned char*)PrivateOf(e);
            V3 at = Origin(e) - base.pos;
            if (e != bot && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) && Alive(e) && q && Readable(q, 0x2170) && !q[0x2166] &&
                fabsf(at.x) <= base.half.x && fabsf(at.y) <= base.half.y && fabsf(at.z) <= base.half.z)
                inside++;
        });
        if (inside >= 2)
            ok = ZConNavSpot(base, ZCON_DEFEND_RING, true, z.spot);
    }
    if (!ok)
        ok = ZConNavSpot(base, zombie ? 150.0f : 0.0f, false, z.spot);
    if (!ok)
    {
        z.spot = base.pos;
        z.spot.z -= base.half.z * 0.1f;
    }
    z.spotBase = base.level;
    z.spotUntil = now + (nearBase ? 2.0f + Rnd() * 3.0f : 12.0f);
}

// its own way there: the shortest one, with a stretch of it (somewhere in its middle half) made dearer so that A* goes
// round when the map has another way not much longer. Every bot draws its own stretch, some none. Lasts 30 s or a life.
static void ZConNewRoute(void* bot, ZConBot& z, BotState& st, V3 me, V3 goal, float chance)
{
    z.route.assign(g_nav.areas.size(), 0.0f);
    z.blocked.clear();
    z.routeUntil = Now() + 30.0f;
    std::vector<V3> path;
    if (Rnd() >= chance || !FindPath(me, goal, path) || path.size() < 4)
        return;
    V3 at = path[path.size() / 4 + rand() % (path.size() / 2)];
    float radius = 200.0f + Rnd() * 300.0f;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        V3 c = g_nav.areas[i].center;
        if (Len(c - at) < radius && Len(c - goal) > 350.0f && Len(c - me) > 200.0f)
            z.route[i] = ZCON_ROUTE_COST;
    }
    st.nextRepath = 0;
    static int reports = 0;
    if (reports++ < 20)
        Log("ZCon: bot '%s' goes round (%.0f %.0f %.0f) on its way to the base", String(F<int>(Vars(bot), EV_NETNAME)), at.x, at.y, at.z);
}

// a way found blocked at `wp`: dear for this bot until its next route
static void ZConBlockWay(ZConBot& z, V3 wp)
{
    if (z.route.size() != g_nav.areas.size())
        z.route.assign(g_nav.areas.size(), 0.0f);
    if (z.blocked.size() != g_nav.areas.size())
        z.blocked.assign(g_nav.areas.size(), 0.0f);
    for (size_t i = 0; i < g_nav.areas.size(); i++)
        if (Len(g_nav.areas[i].center - wp) < 100.0f)
        {
            z.route[i] += ZCON_BLOCK_COST;
            z.blocked[i] += ZCON_BLOCK_COST;
        }
}

// The bots' movement, always by the nav mesh (the stock AI goes straight at a human it sees: it stayed against rails and
// glass with one across a gap, and the others behind it):
//   Humans: to the active base and around in it; once captured some stay inside and others walk around it. A zombie close
//   comes first: they back off shooting (ZombieFleeMove: nearer inside the base, or they could never capture), out of the
//   base if need be, and come back.
//   Zombies: at a human in sight or heard, if there's a way not too long; else to the base and around it, each by its own
//   way (ZConNewRoute).
//   Stuck: a jump and a step aside, a step back, then that way counts as blocked for this bot and it takes another.
// Returns true when it set the movement; viewYaw is turned along the way when no enemy is near (the stock AI has nothing
// to aim at then).
static bool ZConMove(void* bot, BotState& st, float& viewYaw, bool& setView, float& fwd, float& side, unsigned short& buttons)
{
    setView = false;
    if (st.ordered && st.order != ORDER_INDEPENDENT)
        return false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    ZConBase base;
    if (!p || !Readable(p, 0x2170) || !g_nav.ok || !ZConActiveBase(base))
        return false;
    ZConBot& z = g_zconBots[bot];
    float now = Now();
    bool zombie = p[0x2166] != 0;
    V3 me = Origin(bot), eye = Eye(bot);
    if (!z.passSide)
    {
        z.passSide = rand() % 2 ? 1 : -1;
        z.nerve = 0.85f + Rnd() * 0.3f;
    }
    V3 toBase = base.pos - me;
    bool inside = fabsf(toBase.x) <= base.half.x && fabsf(toBase.y) <= base.half.y && fabsf(toBase.z) <= base.half.z;
    bool nearBase = Len2D(toBase) < Len2D(base.half) + 150.0f && fabsf(toBase.z) < base.half.z + 60.0f;
    // the closest enemy: in sight (or, for a zombie, heard)
    float nearest = 1e9f, seenAt = 1e9f;
    void* target = nullptr;
    ForEntitiesNear(me, ZCON_CHASE, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || (q[0x2166] != 0) == zombie)
            return;
        float d = Len(Origin(e) - me);
        bool seen = CanSee(bot, eye, e);
        if (seen)
            seenAt = min(seenAt, d);
        if (d < nearest && (seen || d < ZCON_HEAR))
        {
            nearest = d;
            target = e;
        }
    });
    if (seenAt < ZCON_CHASE)
        z.lastSeen = now;
    if (!zombie && cfg.zombieFlee)
    {
        float range = (inside ? ZCON_FLEE_INSIDE : ZCON_FLEE) * z.nerve;
        if (ZombieFleeMove(bot, st, viewYaw, range, fwd, side, buttons))
        {
            z.fleeUntil = now + 1.2f;
            z.yawSet = false;
            return true;
        }
    }
    V3 wp, goal = z.spot;
    bool pathed = false, there = false, chasing = false;
    if (zombie && target && now >= z.noChaseUntil)
    {
        goal = Origin(target);
        if (nearest < 160.0f && fabsf(goal.z - me.z) < 48.0f && CanSee(bot, eye, target))
        {
            wp = goal; // on it
            pathed = chasing = true;
        }
        else
        {
            g_navRouteCost = z.blocked.empty() ? nullptr : &z.blocked;
            there = !NextWaypoint(bot, st, me, goal, 32.0f, wp);
            g_navRouteCost = nullptr;
            pathed = there || st.path.size() > 1 || AreaAt(me) == AreaAt(goal);
            float length = 0;
            V3 from = me;
            for (size_t i = st.pathIndex; i < st.path.size(); i++)
            {
                length += Len(st.path[i] - from);
                from = st.path[i];
            }
            chasing = pathed && (length < ZCON_CHASE_PATH || length < nearest * 2.5f);
            if (!chasing)
            {
                z.noChaseUntil = now + 4.0f; // across a gap or a window: not worth the way round
                there = false;
            }
        }
    }
    if (!chasing && !zombie && now < z.fleeUntil)
    {
        // the zombie is out of sight for now: a bit further before going back
        goal = g_zf[bot].goal;
        there = !NextWaypoint(bot, st, me, goal, 40.0f, wp);
        pathed = there || st.path.size() > 1 || AreaAt(me) == AreaAt(goal);
        chasing = true;
    }
    if (!chasing)
    {
        if (z.spotBase != base.level || now >= z.spotUntil)
        {
            ZConPickSpot(bot, z, base, zombie, nearBase);
            if (now >= z.routeUntil || z.route.size() != g_nav.areas.size())
                ZConNewRoute(bot, z, st, me, z.spot, zombie ? 0.75f : 0.5f);
        }
        goal = z.spot;
        g_navRouteCost = &z.route;
        // (humans stop well inside the box: it only counts them there)
        there = !NextWaypoint(bot, st, me, goal, zombie ? 48.0f : 20.0f, wp);
        g_navRouteCost = nullptr;
        // NextWaypoint walks straight at the goal when FindPath finds no way: a one-point path to another area
        pathed = there || st.path.size() > 1 || AreaAt(me) == AreaAt(goal);
    }
    if (there)
    {
        // on the spot until the next one
        fwd = side = 0;
        z.yawSet = false;
        st.lastProgress = now;
        return true;
    }
    float wayYaw = YawOf(wp - me), wantYaw = WalkYaw(bot, me, wayYaw, 40.0f);
    // a team mate right ahead (a door, stairs): past it on this bot's side
    bool mateAhead = false;
    ForEntitiesNear(me, 72.0f, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || (q[0x2166] != 0) != zombie)
            return;
        V3 to = Origin(e) - me;
        float d = Len2D(to);
        if (d > 1.0f && fabsf(to.z) < 60.0f && (to.x * cosf(wantYaw / RAD) + to.y * sinf(wantYaw / RAD)) / d > 0.7f)
            mateAhead = true;
    });
    if (mateAhead)
        wantYaw = WalkYaw(bot, me, wantYaw + 45.0f * z.passSide, 40.0f);
    if (now < z.dodgeUntil)
        wantYaw = WalkYaw(bot, me, z.dodgeYaw, 40.0f);
    // how deep it may go: what the path itself drops to, no more (a corner cut over a balcony's edge isn't on the path)
    float maxDrop = ZCON_FREE_DROP;
    if (pathed && me.z - 36.0f - wp.z + 32.0f > maxDrop)
        maxDrop = min(me.z - 36.0f - wp.z + 32.0f, HURT_DROP_MAX);
    float moveYaw = wantYaw;
    bool canMove = ZConSafeYaw(bot, me, wantYaw, maxDrop, moveYaw);
    bool jumpSafe = canMove && !DropAhead(bot, me, moveYaw, 120.0f, ZCON_FREE_DROP);
    bool jump = jumpSafe && LedgeAhead(bot, me, moveYaw);
    if (Len2D(me - st.lastPos) > 24.0f)
    {
        st.lastPos = me;
        st.lastProgress = now;
        if (now - z.stuckAt > 5.0f)
            z.stuck = 0;
    }
    else if (zombie && target && nearest < 90.0f)
        st.lastProgress = now; // on a human: biting, not stuck
    else if (now - st.lastProgress > 0.8f)
    {
        st.lastProgress = now;
        st.nextRepath = 0;
        z.stuck++;
        z.stuckAt = now;
        z.passSide = -z.passSide;
        z.dodgeYaw = z.stuck % 2 ? wayYaw + 90.0f * z.passSide : wayYaw + 180.0f;
        z.dodgeUntil = now + 0.5f;
        jump = jumpSafe;
        if (z.stuck >= 2)
            ZConBlockWay(z, wp);
        if (z.stuck >= 4)
        {
            z.spotUntil = 0;
            z.noChaseUntil = now + 3.0f;
        }
        static int reports = 0;
        if (z.stuck >= 2 && now >= z.nextStuckLog && reports < 60)
        {
            reports++;
            z.nextStuckLog = now + 5.0f;
            Log("ZCon: %s '%s' stuck at (%.0f %.0f %.0f) going to (%.0f %.0f %.0f), try %d%s", zombie ? "zombie" : "human",
                String(F<int>(Vars(bot), EV_NETNAME)), me.x, me.y, me.z, wp.x, wp.y, wp.z, z.stuck,
                mateAhead ? ", a team mate ahead" : "");
        }
    }
    if (jump && now - z.jumpAt > 0.7f)
    {
        z.jumpAt = now;
        buttons |= IN_JUMP;
    }
    if (now - z.jumpAt > 0.1f && now - z.jumpAt < 0.5f)
        buttons |= IN_DUCK; // legs up: a higher ledge
    if (!canMove)
    {
        fwd = side = 0;
        z.yawSet = false;
        return true;
    }
    // no enemy for a moment: it looks where it walks
    if (seenAt >= ZCON_CHASE && now - z.lastSeen > 1.5f)
    {
        z.yaw = z.yawSet ? Approach(z.yaw, moveYaw, 9.0f) : viewYaw;
        z.yawSet = true;
        viewYaw = z.yaw;
        setView = true;
    }
    else
        z.yawSet = false;
    float rel = (moveYaw - viewYaw) / RAD;
    fwd = cosf(rel) * 400.0f;
    side = -sinf(rel) * 400.0f;
    return true;
}
