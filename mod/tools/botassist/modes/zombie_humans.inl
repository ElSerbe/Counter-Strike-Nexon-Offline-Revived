// botassist: human bots against zombie players (the PvP zombie modes: Original, Mutation, Hero, Zombie 4: Darkness,
// Zombie Escape...). Included by botassist.cpp at this point (one translation unit: it uses the shared helpers
// defined before it). Only runs where zombies are players: not in the co-op/monster modes, Zombie Giant or Manhunt
// (they drive their bots themselves), Zombie Conquest (its ZConMove calls ZombieFleeMove itself, with its own range), and
// only for a bot that isn't a zombie (player +0x2166) with one close.
//
// The stock AI stands its ground and shoots until the zombie bites. With a zombie in sight within ZombieFleeDistance
// (botassist.ini, or any within 200 u even unseen) the bot keeps the stock AI's aim and fire (only its movement is
// replaced) and backs off: toward a nav area away from the zombies, scored by how far it leads from them, picked
// again every 1.5 to 2.5 s or once reached; a jump now and then with one on its heels. No zombie close: the stock
// AI moves it again.
// It must not kill itself running: the retreat spot is never in a hurt zone, the way there is a nav path (shared
// FindPath: no hurt areas, no deadly drops, drops cost extra so stairs win), and before each step the ground ahead is
// probed (DropAhead): no hurt zone below and no drop deeper than the path allows (FLEE_FREE_DROP without a path). A
// way that fails is turned away from; with no safe way the bot stands and fights. Jumps only on safe ground.
static const float FLEE_FREE_DROP = 120.0f;
struct ZfBot
{
    V3 goal{};
    bool has = false, onPath = false;
    float goalUntil = 0, nextJump = 0, nextLog = 0, nextDropLog = 0;
};
static std::map<void*, ZfBot> g_zf;

static bool ZombieFleeMode()
{
    int m = GameMode();
    return cfg.zombieFlee && !CoopMode() && m != MODE_ZOMBIE_GIANT && m != MODE_ZHE && m != MODE_ZCON;
}

// a nav area 250 to 900 u away in the direction away from the zombies (dir: unit, 2D), farthest from them
static bool ZfRetreatSpot(V3 me, V3 dir, const std::vector<V3>& zombies, V3& out)
{
    if (!g_nav.ok || g_nav.areas.empty())
        return false;
    float best = -1e9f;
    for (int k = 0; k < 60; k++)
    {
        int ai = rand() % (int)g_nav.areas.size();
        const NavArea& a = g_nav.areas[ai];
        if (NavAreaHurts(ai) || InHurtZone(a.center))
            continue;
        V3 to = a.center - me;
        float d = Len2D(to);
        if (d < 250.0f || d > 900.0f || fabsf(to.z) > 120.0f)
            continue;
        float dot = (to.x * dir.x + to.y * dir.y) / d;
        if (dot < 0.25f)
            continue;
        float zd = 1e9f;
        for (auto& z : zombies)
            zd = min(zd, Len2D(a.center - z));
        float score = dot * 400.0f + min(zd, 1200.0f) - 0.3f * d;
        if (score > best)
        {
            best = score;
            out = a.center;
        }
    }
    return best > -1e9f;
}

static bool ZombieFleeMove(void* bot, BotState& st, float viewYaw, float range, float& fwd, float& side, unsigned short& buttons)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || p[0x2166])
        return false;
    V3 me = Origin(bot), eye = Eye(bot);
    V3 push{ 0, 0, 0 };
    std::vector<V3> zombies;
    float nearest = 1e9f;
    void* nearestZ = nullptr;
    ForEntitiesNear(me, range, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || !q[0x2166])
            return;
        V3 at = Origin(e);
        float d = Len(at - me);
        if (d > 200.0f && !CanSee(bot, eye, e))
            return;
        V3 away = me - at;
        away.z = 0;
        float l = Len2D(away);
        if (l < 1.0f)
            return;
        push = push + away * ((1.2f - d / range) / l); // the closer, the more it counts
        zombies.push_back(at);
        if (d < nearest)
        {
            nearest = d;
            nearestZ = e;
        }
    });
    ZfBot& z = g_zf[bot];
    if (zombies.empty() || Len2D(push) < 0.01f)
    {
        z.has = false;
        return false;
    }
    float now = Now();
    V3 dir = push * (1.0f / Len2D(push));
    V3 toGoal = z.goal - me;
    bool stale = !z.has || now >= z.goalUntil || Len2D(toGoal) < 80.0f ||
                 (toGoal.x * dir.x + toGoal.y * dir.y) < 0; // the zombies came round: it would run into them
    if (stale)
    {
        z.has = true;
        z.onPath = ZfRetreatSpot(me, dir, zombies, z.goal);
        if (!z.onPath)
            z.goal = me + dir * 300.0f;
        z.goalUntil = now + 1.5f + Rnd();
    }
    // the way: a nav path to a retreat spot (vetted by FindPath), else straight away from the zombies
    V3 wp;
    // (NextWaypoint walks straight at the goal when FindPath finds no way: a one-point path to another area)
    bool pathed = z.onPath && NextWaypoint(bot, st, me, z.goal, 40.0f, wp) && Len(st.pathGoal - z.goal) < 1.0f &&
                  (st.path.size() > 1 || AreaAt(me) == AreaAt(z.goal));
    if (!pathed)
    {
        wp = me + dir * 100.0f;
        if (z.onPath)
            z.goalUntil = min(z.goalUntil, now + 0.3f); // no path there (a drop, a hurt zone): another spot soon
    }
    float maxDrop = pathed ? HURT_DROP_MAX : FLEE_FREE_DROP;
    float wantYaw = WalkYaw(bot, me, YawOf(wp - me), 40.0f), moveYaw = wantYaw;
    bool safe = !DropAhead(bot, me, moveYaw, 72.0f, maxDrop);
    if (!safe)
    {
        static const float turns[] = { 30, -30, 60, -60, 90, -90, 120, -120, 150, -150 };
        for (float t : turns)
        {
            float y = wantYaw + t;
            V3 head = me + V3{ 0, 0, 20 };
            TraceResult tr;
            if (Trace(head, head + V3{ cosf(y / RAD) * 40.0f, sinf(y / RAD) * 40.0f, 0 }, bot, 1, tr) &&
                !DropAhead(bot, me, y, 72.0f, FLEE_FREE_DROP))
            {
                moveYaw = y;
                safe = true;
                break;
            }
        }
        z.goalUntil = min(z.goalUntil, now + 0.5f);
        if (now >= z.nextDropLog)
        {
            z.nextDropLog = now + 5.0f;
            DLog("zombie flee: bot '%s' stops at a drop / hurt zone (%s)", String(F<int>(Vars(bot), EV_NETNAME)),
                 safe ? "turns away" : "no safe way, stands");
        }
        if (!safe)
        {
            fwd = side = 0;
            return true;
        }
    }
    bool jumpSafe = !DropAhead(bot, me, moveYaw, 120.0f, FLEE_FREE_DROP);
    if (jumpSafe && LedgeAhead(bot, me, moveYaw))
        buttons |= IN_JUMP;
    if (jumpSafe && nearest < 160.0f && now >= z.nextJump)
    {
        z.nextJump = now + 1.0f + Rnd();
        if (Rnd() < 0.4f)
            buttons |= IN_JUMP;
    }
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
        z.goalUntil = 0;
        if (jumpSafe)
            buttons |= IN_JUMP;
    }
    float rel = (moveYaw - viewYaw) / RAD;
    fwd = cosf(rel) * 250.0f;
    side = -sinf(rel) * 250.0f;
    if (now >= z.nextLog)
    {
        z.nextLog = now + 5.0f;
        DLog("zombie flee: bot '%s' backs off from '%s' (%.0f u, %d zombies close) toward (%.0f %.0f), %.0f u away",
             String(F<int>(Vars(bot), EV_NETNAME)), nearestZ ? String(F<int>(Vars(nearestZ), EV_NETNAME)) : "?", nearest,
             (int)zombies.size(), z.goal.x, z.goal.y, Len2D(z.goal - me));
    }
    return true;
}

static bool ZombieFlee(void* bot, BotState& st, float viewYaw, float& fwd, float& side, unsigned short& buttons)
{
    return ZombieFleeMode() && ZombieFleeMove(bot, st, viewYaw, cfg.zombieFleeDistance, fwd, side, buttons);
}
