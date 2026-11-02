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
//   - ZConMove: replaces only the bots' movement (the stock AI keeps aiming and shooting): humans go to the active base
//     and stay inside it; zombies go there too, but chase on their own (stock AI) with a human in sight.
static const DWORD MP_ZCON_RULES_VT = 0x117919e0, MP_ZCON_PREBUY = 0x107af350, MP_ZCON_HMABIL = 0x1080ea30;
static const int ZCON_MODULE = 0x12;
static const float ZCON_CHASE = 900.0f, ZCON_FREE_DROP = 120.0f;

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
    float spotUntil = 0, yaw = 0, lastSeen = 0;
    bool yawSet = false, sawEnemy = false;
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
    }
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    if (zombie && !z.picked && now - z.zombieSince > 1.0f)
    {
        z.picked = true;
        if (!p[0x2e39])
        {
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
    V3 knee = me - V3{ 0, 0, 18 };
    for (float t : turns)
    {
        float y = yaw + t;
        if (t != 0 && !Trace(knee, knee + V3{ cosf(y / RAD) * 40.0f, sinf(y / RAD) * 40.0f, 0 }, bot, 1, tr))
            continue;
        if (!DropAhead(bot, me, y, 72.0f, t == 0 ? maxDrop : ZCON_FREE_DROP))
        {
            out = y;
            return true;
        }
    }
    return false;
}

// to the active base, or around inside it. Returns true when it set the movement; viewYaw is turned along the way when
// no enemy is near (the stock AI has nothing to aim at then).
static bool ZConMove(void* bot, BotState& st, float& viewYaw, bool& setView, float& fwd, float& side, unsigned short& buttons)
{
    setView = false;
    if (st.ordered && st.order != ORDER_INDEPENDENT)
        return false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    ZConBase base;
    if (!p || !Readable(p, 0x2170) || !ZConActiveBase(base))
        return false;
    ZConBot& z = g_zconBots[bot];
    float now = Now();
    bool zombie = p[0x2166] != 0;
    V3 me = Origin(bot), eye = Eye(bot);
    // an enemy in sight: fighting comes first for a zombie (the stock AI chases), and the view stays the stock AI's
    float nearest = 1e9f;
    ForEntitiesNear(me, ZCON_CHASE, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || (q[0x2166] != 0) == zombie)
            return;
        float d = Len(Origin(e) - me);
        if (d < nearest && CanSee(bot, eye, e))
            nearest = d;
    });
    bool enemy = nearest < ZCON_CHASE;
    if (enemy)
        z.lastSeen = now;
    if (zombie && enemy)
    {
        z.yawSet = false;
        return false;
    }
    V3 toBase = base.pos - me;
    bool inside = fabsf(toBase.x) <= base.half.x && fabsf(toBase.y) <= base.half.y && fabsf(toBase.z) <= base.half.z;
    // a spot in the base (its inner part), another now and then
    if (now >= z.spotUntil || Len2D(z.spot - base.pos) > Len2D(base.half) + 1.0f)
    {
        z.spot = base.pos + V3{ (Rnd() * 1.4f - 0.7f) * base.half.x, (Rnd() * 1.4f - 0.7f) * base.half.y, 0 };
        z.spotUntil = now + (inside ? 2.0f + Rnd() * 3.0f : 6.0f);
    }
    float arrive = inside ? 48.0f : 40.0f;
    V3 wp;
    if (!NextWaypoint(bot, st, me, z.spot, arrive, wp))
    {
        // on the spot: stay (humans capture by being here)
        fwd = side = 0;
        z.yawSet = false;
        return true;
    }
    // NextWaypoint walks straight at the goal when FindPath finds no way: a one-point path to another area
    bool pathed = st.path.size() > 1 || AreaAt(me) == AreaAt(z.spot);
    float moveYaw;
    if (!ZConSafeYaw(bot, me, ClearYaw(bot, me, YawOf(wp - me), 40.0f), pathed ? HURT_DROP_MAX : ZCON_FREE_DROP, moveYaw))
    {
        z.spotUntil = 0;
        fwd = side = 0;
        return true;
    }
    if (wp.z > me.z + 24.0f && Len2D(wp - me) < 120.0f && !DropAhead(bot, me, moveYaw, 120.0f, ZCON_FREE_DROP))
        buttons |= IN_JUMP;
    // stuck against something: a jump and another spot
    if (Len2D(me - st.lastPos) > 24.0f)
    {
        st.lastPos = me;
        st.lastProgress = now;
    }
    else if (now - st.lastProgress > 1.0f)
    {
        st.lastProgress = now;
        st.nextRepath = 0;
        z.spotUntil = 0;
        if (!DropAhead(bot, me, moveYaw, 120.0f, ZCON_FREE_DROP))
            buttons |= IN_JUMP;
    }
    // no enemy for a moment: it looks where it walks
    if (!enemy && now - z.lastSeen > 1.5f)
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
