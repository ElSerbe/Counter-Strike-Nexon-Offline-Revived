// botassist: Zombie War (mode 32, "Guerra Zombi"; rules CSOGameRulesZombi3ExRenewal, vtable 0x117aaecc, derived from Zombie
// 2's; modes 35, 36 and 43 share them). Included by botassist.cpp at this point (one translation unit: it uses the shared
// helpers defined before it). Only runs in its own mode: ZWarMove isn't called unless GameMode() == MODE_ZWAR.
//
// The mode: T are zombies and CT humans, in even teams, with no infection (a bitten human dies). The round ends when a side
// has nobody left (0x108765c0, CSO rules +0x263 and +0x262, alive T and CT) or by time; the sides swap at half time
// (0x10876d40). Zombies wait for a countdown at the start (0x10877180) and get stronger in the last 30 s (0x10876af0).
//
// What botassist does:
//   - Teams: RoomBotQuota (shared, see there) gives the game the side it asks for. Rooms list every AI player on the host's
//     side, so the requests for T bots were refused: no zombies, and the round ended and restarted over and over.
//   - ZB2Watch and ZB2BotThink (modes/zombie2_mutation.inl, the same rules): dead zombie bots go through the game's death
//     think, zombie bots draw a class, both sides use skills.
//   - ZWarWatch: before each round the game goes through every player in turn (BanPickSystem::Supervisor): humans pick the
//     weapon they play, zombies ban one. It gives a player 11 s and then draws one (0x1081d450, state 3); a bot never
//     answers, so everyone waited 11 s per bot. Now a bot's turn ends at once, so the game draws for it, and the pause
//     before a bot's turn is 0.3 s instead of 1.5 s. The chat still says "selection time ... is over" for each bot: that
//     line belongs to the game's draw.
//   - ZWarBuyPick: the pick isn't a gift of the server: a player's client buys it when the menu closes (item_<id>, ammo,
//     grenade and armor). A bot has no client, and UpdateBuying gave it the room's weapons over whatever it had. So in this
//     mode human bots buy their pick (shared UpdateBuying skips this mode).
//   - ZWarMove: zombie bots stay still for the humans' preparation (20 s from the round's start, FUN_1149eeb0; the game
//     hides them and takes away their collision in 0x10877180; a player can't move then, it seems by the client, but nothing held
//     a bot). Everybody is frozen during the menu (FL_FROZEN, 0x1081dcc0).
//   - ZWarMove: the movement of Zombie Conquest (ZConMoveAt) with an objective that moves. Humans hold together around a
//     place near them, which changes every so often; zombies gather at a rally point (their squads, see there) and fall on
//     where the humans are.
static const float ZWAR_HOLD_MIN = 35.0f, ZWAR_HOLD_MORE = 20.0f, ZWAR_HOLD_FAR = 1200.0f;
static const float ZWAR_ZOMBIE_WAIT = 20.0f, ZWAR_BOT_TURN = 0.3f;
// the ban/pick supervisor: +0x18 state (-1 none, 2 between turns, 3 a player's turn, 4 and 5 closing), +0x1c time of the next
// state, +0x24 round, +0x28 whose turn, +0x30..+0x34 one entry per player: short player index, short weapon chosen
static const DWORD MP_ZWAR_BANPICK = 0x11e5fb90;

static unsigned char* ZWarBanPick()
{
    unsigned char* sup = *(unsigned char**)(MP_ZWAR_BANPICK + g_mpDelta);
    return sup && Readable(sup, 0x3c) ? sup : nullptr;
}

static void* ZWarEntryPlayer(unsigned char* sup, int index)
{
    short* first = *(short**)(sup + 0x30);
    short* last = *(short**)(sup + 0x34);
    if (!first || index < 0 || index >= (int)(last - first) / 2 || !Readable(first + index * 2, 4))
        return nullptr;
    auto entOf = (EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX];
    return entOf(first[index * 2]);
}

static void ZWarWatch()
{
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_ZWAR)
        return;
    unsigned char* sup = ZWarBanPick();
    if (!sup)
        return;
    int state = *(int*)(sup + 0x18), turn = *(int*)(sup + 0x28);
    float now = Now();
    float& next = *(float*)(sup + 0x1c);
    auto isBot = [](void* e) { return e && (F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT) != 0; };
    if (state == 3 && next > now && isBot(ZWarEntryPlayer(sup, turn)))
    {
        next = now;
        static int reports = 0;
        if (reports++ < 30)
            Log("ZWar: bot '%s' ends its turn in the ban/pick menu: the game draws for it",
                String(F<int>(Vars(ZWarEntryPlayer(sup, turn)), EV_NETNAME)));
    }
    else if (state == 2 && next > now + ZWAR_BOT_TURN && isBot(ZWarEntryPlayer(sup, turn + 1)))
        next = now + ZWAR_BOT_TURN;
}

struct ZWarBot
{
    int round = -1, tries = 0;
    float buyAt = 0;
};
static std::map<void*, ZWarBot> g_zwarBots;

// a human bot buys what it picked, as a player's client does when the menu closes
static void ZWarBuyPick(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    unsigned char* sup = ZWarBanPick();
    if (!p || !Readable(p, 0x2170) || !sup || p[0x2166] || *(int*)(sup + 0x18) != -1 ||
        (F<int>(Vars(bot), EV_FLAGS) & FL_FROZEN))
        return;
    ZWarBot& z = g_zwarBots[bot];
    float now = Now();
    int round = *(int*)(sup + 0x24);
    if (z.round != round)
    {
        z.round = round;
        z.tries = 0;
        z.buyAt = now;
    }
    if (z.tries >= 3 || now < z.buyAt)
        return;
    int count = (int)(*(short**)(sup + 0x34) - *(short**)(sup + 0x30)) / 2, item = 0;
    for (int i = 0; i < count; i++)
        if (ZWarEntryPlayer(sup, i) == bot)
            item = (*(short**)(sup + 0x30))[i * 2 + 1];
    z.tries++;
    z.buyAt = now + 2.0f;
    if (item <= 0)
        return;
    std::string held = SlotClass(bot, 1);
    unsigned char wasInternal = p[0x18b4];
    p[0x18b4] = 1; // the internal-buy flag: see UpdateBuying
    if (z.tries == 1)
        FreeSlot(bot, 1);
    FakeCommand(bot, "client_buy_open");
    BuyAs(bot, item);
    FakeCommand(bot, "primammo");
    FakeCommand(bot, "primammo");
    FakeCommand(bot, "secammo");
    FakeCommand(bot, "hegren");
    FakeCommand(bot, "vesthelm");
    FakeCommand(bot, "client_buy_close");
    p[0x18b4] = wasInternal;
    FakeCommand(bot, "slot1");
    if (!SlotClass(bot, 1).empty())
        z.tries = 3;
    static int reports = 0;
    if (reports++ < 40)
        Log("ZWar: human bot '%s' buys its pick, item %d: had %s, now primary %s", String(F<int>(Vars(bot), EV_NETNAME)), item,
            held.c_str(), SlotClass(bot, 1).c_str());
}

static struct
{
    V3 hold{}, humans{};
    bool hasHold = false, hasHumans = false;
    float next = 0, holdUntil = 0, nextLog = 0;
    int level = -1;
} g_zwar;

// a nav area for the humans to hold: the closest to them, or a walkable one some way off
static bool ZWarPickHold(V3 humans, bool nearest, V3& out)
{
    float best = 1e9f;
    int found = -1;
    if (nearest)
    {
        for (size_t i = 0; i < g_nav.areas.size(); i++)
        {
            float d = Len(g_nav.areas[i].center - humans);
            if (d < best && !NavAreaHurts((int)i))
            {
                best = d;
                found = (int)i;
            }
        }
    }
    else
    {
        std::vector<V3> path;
        for (int k = 0; k < 40 && found < 0; k++)
        {
            int i = rand() % (int)g_nav.areas.size();
            float d = Len2D(g_nav.areas[i].center - humans);
            if (d >= 400.0f && d <= 900.0f && !NavAreaHurts(i) && FindPath(humans, g_nav.areas[i].center, path))
                found = i;
        }
    }
    if (found < 0)
        return false;
    out = g_nav.areas[found].center;
    return true;
}

static void ZWarUpdate()
{
    float now = Now();
    if (g_zwar.level != g_nav.level)
    {
        g_zwar.level = g_nav.level;
        g_zwar.hasHold = false;
        g_zwar.next = 0;
    }
    if (now >= g_zwar.next - 10.0f && now < g_zwar.next)
        return;
    g_zwar.next = now + 0.5f;
    V3 sum{ 0, 0, 0 };
    std::vector<V3> at;
    int humans = 0, zombies = 0, dead = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !q || !Readable(q, 0x2170))
            return;
        if (!Alive(e))
            dead++;
        else if (q[0x2166])
            zombies++;
        else
        {
            at.push_back(Origin(e));
            sum = sum + at.back();
            humans++;
        }
    });
    g_zwar.hasHumans = humans > 0;
    if (humans)
    {
        // the human closest to the middle of them (the middle itself could be in a wall)
        V3 middle = sum * (1.0f / humans);
        g_zwar.humans = at[0];
        for (V3 a : at)
            if (Len(a - middle) < Len(g_zwar.humans - middle))
                g_zwar.humans = a;
        // a new round puts them elsewhere: a place near them again
        bool lost = g_zwar.hasHold && Len(g_zwar.hold - g_zwar.humans) > ZWAR_HOLD_FAR;
        if (!g_zwar.hasHold || lost || now >= g_zwar.holdUntil)
        {
            V3 hold;
            if (ZWarPickHold(g_zwar.humans, !g_zwar.hasHold || lost, hold))
            {
                g_zwar.hold = hold;
                g_zwar.hasHold = true;
            }
            g_zwar.holdUntil = now + ZWAR_HOLD_MIN + Rnd() * ZWAR_HOLD_MORE;
        }
    }
    if (now >= g_zwar.nextLog)
    {
        g_zwar.nextLog = now + 15.0f;
        static int reports = 0;
        if (reports++ < 60)
            Log("ZWar: %d humans and %d zombies alive, %d dead, humans hold (%.0f %.0f %.0f)", humans, zombies, dead,
                g_zwar.hold.x, g_zwar.hold.y, g_zwar.hold.z);
    }
}

static bool ZWarMove(void* bot, BotState& st, float& viewYaw, bool& setView, float& fwd, float& side, unsigned short& buttons)
{
    setView = false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
    if (!p || !Readable(p, 0x2170) || !g_nav.ok || !cso || !Readable(cso, 0x280) || cso[0x27c] != 1 || cso[0x27d])
        return false;
    V3 me = Origin(bot);
    float now = Now();
    unsigned char* sup = ZWarBanPick();
    if (sup && *(int*)(sup + 0x18) != -1)
    {
        st.lastPos = me;
        st.lastProgress = now;
        return false; // the menu: everybody is frozen
    }
    if (p[0x2166] && now < *(float*)(cso + 0x248) + ZWAR_ZOMBIE_WAIT)
    {
        // the humans' preparation
        st.lastPos = me;
        st.lastProgress = now;
        fwd = side = 0;
        buttons &= ~(IN_ATTACK | IN_JUMP | IN_DUCK);
        return true;
    }
    ZWarUpdate();
    if (!g_zwar.hasHumans || !g_zwar.hasHold)
        return false;
    ZConBase objective;
    objective.level = 0;
    objective.state = 2; // once there, some of the humans walk around the place (ZConPickSpot)
    if (p[0x2166])
    {
        objective.pos = g_zwar.humans;
        objective.half = V3{ 300.0f, 300.0f, 150.0f };
    }
    else
    {
        objective.pos = g_zwar.hold;
        objective.pos.z += 36.0f;
        objective.half = V3{ 220.0f, 220.0f, 100.0f };
    }
    return ZConMoveAt(bot, st, objective, 0.0f, ZCON_RALLY_ZOMBIE, viewYaw, setView, fwd, side, buttons);
}
