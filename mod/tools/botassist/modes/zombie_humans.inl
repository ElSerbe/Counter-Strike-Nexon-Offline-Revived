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
//
// Cloaked zombies: the stock AI drops any enemy whose cloaking module (3) is active (FUN_112f34a0, 0x10880320), and doesn't
// even see one with alpha under 1 (CCSBot::IsVisible, 0x11311ff0), so a cloaked zombie bit them untouched. ZombieCloakAim:
// one in sight within CLOAK_SHOOT_RANGE gets aimed at and shot by botassist, after a moment to notice it (not with a visible
// zombie closer: the stock AI has that one). Backing off needs nothing: ZombieFleeMove counts every zombie by line of sight.
static const float FLEE_FREE_DROP = 120.0f;
static const float CLOAK_SHOOT_RANGE = 300.0f, CLOAK_NOTICE = 0.25f, CLOAK_TURN = 14.0f, CLOAK_FIRE_CONE = 10.0f;
static const int EV_RENDERMODE = 0x160, EV_RENDERAMT = 0x164, RENDER_TRANS_ALPHA = 4, MODULE_CLOAKING = 3;
static const DWORD MP_CLOAK_ACTIVE = 0x10880320;
struct ZfBot
{
    V3 goal{};
    bool has = false, onPath = false;
    float goalUntil = 0, nextJump = 0, nextLog = 0, nextDropLog = 0, cloakSince = 0, cloakLast = 0;
    int cloakShots = 0;
    float nextGroupLog = 0;
    void* aware = nullptr;   // ZombieAwareAim's zombie
    float awareSince = 0, awareLast = -100, awareErr = 0;
    int awareTurns = 0;
};
static std::map<void*, ZfBot> g_zf;

// Every human bot looked over all the map's entities up to three times per move (backing off, regrouping, cloaked
// zombies) and traced a line of sight to every zombie each time: most of botassist's time with many bots. Now the
// living players are listed once per server frame (ZfPlayers) and a line of sight is kept ZF_SIGHT_KEEP s per pair.
struct ZfPlayer
{
    void* e;
    unsigned char* q; // its CBasePlayer
    V3 at;
    bool zombie;
};
static std::vector<ZfPlayer> g_zfPlayers;
static float g_zfPlayersAt = -1;
static const float ZF_SIGHT_KEEP = 0.15f;
struct ZfSight
{
    float at;
    bool seen;
};
static std::unordered_map<unsigned long long, ZfSight> g_zfSight;

static const std::vector<ZfPlayer>& ZfPlayers()
{
    float now = Now();
    if (now == g_zfPlayersAt)
        return g_zfPlayers;
    if (now < g_zfPlayersAt || g_zfSight.size() > 4096)
        g_zfSight.clear(); // a new map, or old pairs piling up
    g_zfPlayersAt = now;
    g_zfPlayers.clear();
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || F<int>(Vars(e), EV_DEADFLAG) != 0)
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (q && Readable(q, 0x2170))
            g_zfPlayers.push_back({ e, q, Origin(e), q[0x2166] != 0 });
    });
    return g_zfPlayers;
}

static bool ZfSees(void* bot, V3 eye, void* target)
{
    float now = Now();
    unsigned long long key = ((unsigned long long)(DWORD)bot << 32) | (DWORD)target;
    auto it = g_zfSight.find(key);
    if (it != g_zfSight.end() && now >= it->second.at && now - it->second.at < ZF_SIGHT_KEEP)
        return it->second.seen;
    bool seen = CanSee(bot, eye, target);
    g_zfSight[key] = { now, seen };
    return seen;
}

static bool ZombieMode(int mode); // botassist.cpp: the modes with zombies
static bool HeroWithChainsaw(void* bot); // modes/zombie_hero.inl

// (only modes with zombies: the regrouping ran in Team Deathmatch too, and every bot gathered round one player)
static bool ZombieFleeMode()
{
    int m = GameMode();
    return cfg.zombieFlee && ZombieMode(m) && !CoopMode() && m != MODE_ZOMBIE_GIANT && m != MODE_ZHE && m != MODE_ZCON &&
           m != MODE_ZWAR;
}

// the living humans of the bot's side (not zombies), other than the bot, within `range`
static std::vector<void*> ZfHumans(void* bot, float range)
{
    std::vector<void*> out;
    V3 me = Origin(bot);
    for (const ZfPlayer& pl : ZfPlayers())
        if (pl.e != bot && !pl.zombie && Len(pl.at - me) <= range)
            out.push_back(pl.e);
    return out;
}

// a nav area 250 to 900 u away in the direction away from the zombies (dir: unit, 2D), farthest from them, and toward
// the other humans (alone, a bot died alone: the user asked them to stay in groups)
static bool ZfRetreatSpot(V3 me, V3 dir, const std::vector<V3>& zombies, V3& out, const std::vector<V3>& friends = {})
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
        if (!friends.empty())
        {
            float fd = 1e9f;
            for (auto& f : friends)
                fd = min(fd, Len2D(a.center - f));
            score -= 0.5f * min(fd, 1500.0f);
        }
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
    for (const ZfPlayer& pl : ZfPlayers())
    {
        void* e = pl.e;
        if (e == bot || !pl.zombie)
            continue;
        V3 at = pl.at;
        float d = Len(at - me);
        if (d > range || (d > 200.0f && !ZfSees(bot, eye, e)))
            continue;
        V3 away = me - at;
        away.z = 0;
        float l = Len2D(away);
        if (l < 1.0f)
            continue;
        push = push + away * ((1.2f - d / range) / l); // the closer, the more it counts
        zombies.push_back(at);
        if (d < nearest)
        {
            nearest = d;
            nearestZ = e;
        }
    }
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
        std::vector<V3> friends;
        for (void* h : ZfHumans(bot, 2000.0f))
            friends.push_back(Origin(h));
        z.onPath = ZfRetreatSpot(me, dir, zombies, z.goal, friends);
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

// No zombie close: a human bot far from the others walks back to them (the stock AI sent them off alone, and they died
// alone). The group is around the human with the most humans within 400 units; within 350 of it, or with a zombie in
// sight within 700 (the stock AI fights it), the stock AI moves the bot again. Not under a player's orders.
static bool ZombieGroupMove(void* bot, BotState& st, float viewYaw, float& fwd, float& side, unsigned short& buttons)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || p[0x2166] || st.ordered)
        return false;
    std::vector<void*> humans = ZfHumans(bot, 6000.0f);
    if (humans.empty())
        return false;
    V3 me = Origin(bot), eye = Eye(bot);
    void* anchor = nullptr;
    int bestN = -1;
    float bestD = 1e9f;
    for (void* h : humans)
    {
        int n = 0;
        for (void* o : humans)
            n += o != h && Len(Origin(o) - Origin(h)) < 400.0f;
        float d = Len(Origin(h) - me);
        if (n > bestN || (n == bestN && d < bestD))
        {
            bestN = n;
            bestD = d;
            anchor = h;
        }
    }
    if (!anchor || bestD < 350.0f)
        return false;
    bool zombieSeen = false;
    for (const ZfPlayer& pl : ZfPlayers())
        if (!zombieSeen && pl.zombie && Len(pl.at - me) <= 700.0f && ZfSees(bot, eye, pl.e))
            zombieSeen = true;
    if (zombieSeen)
        return false;
    float now = Now();
    V3 goal = Origin(anchor), wp;
    if (!NextWaypoint(bot, st, me, goal, 40.0f, wp))
        wp = goal;
    float moveYaw = WalkYaw(bot, me, YawOf(wp - me), 40.0f);
    if (DropAhead(bot, me, moveYaw, 72.0f, HURT_DROP_MAX))
        return false; // (the stock AI knows its way better than a fall)
    if (LedgeAhead(bot, me, moveYaw) && !DropAhead(bot, me, moveYaw, 120.0f, FLEE_FREE_DROP))
        buttons |= IN_JUMP;
    if (Len2D(me - st.lastPos) > 24.0f)
    {
        st.lastPos = me;
        st.lastProgress = now;
    }
    else if (now - st.lastProgress > 1.5f)
    {
        st.lastProgress = now;
        st.nextRepath = 0;
        buttons |= IN_JUMP;
    }
    float rel = (moveYaw - viewYaw) / RAD;
    fwd = cosf(rel) * 250.0f;
    side = -sinf(rel) * 250.0f;
    ZfBot& z = g_zf[bot];
    if (now >= z.nextGroupLog)
    {
        z.nextGroupLog = now + 10.0f;
        static int reports = 0;
        if (reports++ < 100)
            Log("zombie modes: bot '%s' alone, %.0f u from the group around '%s' (%d humans with it): goes back to it",
                String(F<int>(Vars(bot), EV_NETNAME)), bestD, String(F<int>(Vars(anchor), EV_NETNAME)), bestN + 1);
    }
    return true;
}

// modes/zombie_holdout.inl: Zombie, Zombie 2 and Zombie Hero's human bots hold a few spots in groups
static bool HoldoutMode();
static float HoldoutFleeRange(void* bot, float normal);
static bool HoldoutMove(void* bot, BotState& st, float viewYaw, float& fwd, float& side, unsigned short& buttons, bool& handled);

static bool ZombieFlee(void* bot, BotState& st, float viewYaw, float& fwd, float& side, unsigned short& buttons)
{
    // (Zombie Escape, mode 20: the way out is forward along the route, ZeMove's; backing off and regrouping held the bots
    // back in corners)
    if (!ZombieFleeMode() || GameMode() == 20)
        return false;
    // (Zombie Hero: the hero with the chainsaw goes at the zombies, the game's AI moves it)
    if (HeroWithChainsaw(bot))
        return false;
    if (HoldoutMode())
    {
        // holding a spot: only a zombie right there makes it back off
        float range = HoldoutFleeRange(bot, cfg.zombieFleeDistance);
        if (ZombieFleeMove(bot, st, viewYaw, range, fwd, side, buttons))
            return true;
        bool handled = false;
        bool moved = HoldoutMove(bot, st, viewYaw, fwd, side, buttons, handled);
        if (handled)
            return moved;
    }
    return ZombieFleeMove(bot, st, viewYaw, cfg.zombieFleeDistance, fwd, side, buttons) || ZombieGroupMove(bot, st, viewYaw, fwd, side, buttons);
}

static bool ZombieCloaked(void* e, unsigned char* q)
{
    unsigned char* v = Vars(e);
    if (F<int>(v, EV_RENDERMODE) == RENDER_TRANS_ALPHA && F<float>(v, EV_RENDERAMT) < 200.0f)
        return true;
    void* mod = ((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(q, MODULE_CLOAKING);
    return mod && (((int(__fastcall*)(void*))(MP_CLOAK_ACTIVE + g_mpDelta))(mod) & 0xff) != 0;
}

// true: aim was set and the bot shoots (the caller writes it where the bot looks)
static bool ZombieCloakAimOnly(void* bot, float* aim, unsigned short& buttons)
{
    int m = GameMode();
    if (!ZombieFleeMode() && !(cfg.zombieFlee && (m == MODE_ZCON || m == MODE_ZWAR)))
        return false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || p[0x2166])
        return false;
    V3 me = Origin(bot), eye = Eye(bot);
    float nearest = 1e9f, nearestCloaked = 1e9f;
    void* cloaked = nullptr;
    for (const ZfPlayer& pl : ZfPlayers())
    {
        void* e = pl.e;
        unsigned char* q = pl.q;
        float d = Len(pl.at - me);
        if (e == bot || !pl.zombie || d > CLOAK_SHOOT_RANGE || !ZfSees(bot, eye, e))
            continue;
        nearest = min(nearest, d);
        if (d < nearestCloaked && ZombieCloaked(e, q))
        {
            nearestCloaked = d;
            cloaked = e;
        }
    }
    ZfBot& z = g_zf[bot];
    float now = Now();
    if (!cloaked || nearest < nearestCloaked)
        return false;
    if (now - z.cloakLast > 0.5f)
        z.cloakSince = now; // a new one: a moment to notice it
    z.cloakLast = now;
    if (now - z.cloakSince < CLOAK_NOTICE)
        return false;
    V3 to = Center(cloaked) - eye;
    float yaw = YawOf(to), pitch = PitchOf(to);
    float dy = AngleDiff(yaw, aim[1]), dp = pitch - aim[0];
    aim[1] += dy > CLOAK_TURN ? CLOAK_TURN : dy < -CLOAK_TURN ? -CLOAK_TURN : dy;
    aim[0] += dp > CLOAK_TURN ? CLOAK_TURN : dp < -CLOAK_TURN ? -CLOAK_TURN : dp;
    if (fabsf(dy) < CLOAK_FIRE_CONE && fabsf(dp) < CLOAK_FIRE_CONE)
    {
        buttons |= IN_ATTACK;
        static int reports = 0;
        if (z.cloakShots++ == 0 && reports++ < 30)
            Log("zombie flee: bot '%s' shoots at the cloaked zombie '%s', %.0f u away", String(F<int>(Vars(bot), EV_NETNAME)),
                String(F<int>(Vars(cloaked), EV_NETNAME)), nearestCloaked);
    }
    return true;
}

// ---- watching their back (the zombie PvP modes)
// The stock AI only sees what's in front of it, and a bot backing off (ZombieFleeMove walks it away, whichever way it
// looks) or walking to its spot kept looking elsewhere while a zombie came up behind and caught it. The nearest zombie
// in sight (line of sight, all around) within AWARE_RANGE is now looked at after AWARE_NOTICE s, turning AWARE_TURN
// degrees a move, and shot once the aim is within AWARE_FIRE_CONE (a small aim error, new for each zombie), unless the
// stock AI already aims within AWARE_CONE of it. The bot's legs keep their own way: backing off still backs off.
static const float AWARE_RANGE = 650.0f, AWARE_CONE = 25.0f, AWARE_NOTICE = 0.3f, AWARE_TURN = 12.0f, AWARE_FIRE_CONE = 6.0f;

static bool ZombieAwareAim(void* bot, float* aim, unsigned short& buttons)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || p[0x2166])
        return false;
    V3 me = Origin(bot), eye = Eye(bot);
    void* target = nullptr;
    float nearest = AWARE_RANGE;
    for (const ZfPlayer& pl : ZfPlayers())
    {
        if (pl.e == bot || !pl.zombie)
            continue;
        float d = Len(pl.at - me);
        if (d < nearest && ZfSees(bot, eye, pl.e))
        {
            nearest = d;
            target = pl.e;
        }
    }
    ZfBot& z = g_zf[bot];
    float now = Now();
    if (!target)
    {
        z.aware = nullptr;
        return false;
    }
    V3 to = Center(target) - eye;
    float yaw = YawOf(to), pitch = PitchOf(to);
    if (fabsf(AngleDiff(yaw, aim[1])) < AWARE_CONE && fabsf(pitch - aim[0]) < AWARE_CONE)
    {
        z.awareLast = now; // the stock AI has it
        return false;
    }
    if (target != z.aware || now - z.awareLast > 0.5f)
    {
        z.aware = target; // a new one: a moment to notice it
        z.awareSince = now;
        z.awareErr = (Rnd() - 0.5f) * 3.0f;
    }
    z.awareLast = now;
    if (now - z.awareSince < AWARE_NOTICE)
        return false;
    yaw += z.awareErr;
    float dy = AngleDiff(yaw, aim[1]), dp = pitch - aim[0];
    aim[1] += dy > AWARE_TURN ? AWARE_TURN : dy < -AWARE_TURN ? -AWARE_TURN : dy;
    aim[0] += dp > AWARE_TURN ? AWARE_TURN : dp < -AWARE_TURN ? -AWARE_TURN : dp;
    if (fabsf(dy) < AWARE_FIRE_CONE && fabsf(dp) < AWARE_FIRE_CONE)
        buttons |= IN_ATTACK;
    if (z.awareTurns++ == 0)
    {
        static int reports = 0;
        if (reports++ < 40)
            Log("zombie modes: bot '%s' turns to the zombie '%s' %.0f u away (%.0f degrees off its aim)",
                String(F<int>(Vars(bot), EV_NETNAME)), String(F<int>(Vars(target), EV_NETNAME)), nearest, fabsf(dy));
    }
    return true;
}

// true: aim was set and the bot shoots (the caller writes it where the bot looks)
static bool ZombieCloakAim(void* bot, float* aim, unsigned short& buttons)
{
    if (ZombieCloakAimOnly(bot, aim, buttons))
        return true;
    int m = GameMode();
    if (!ZombieFleeMode() && !(cfg.zombieFlee && (m == MODE_ZCON || m == MODE_ZWAR)))
        return false;
    if (!g_roomOpts.watchBack)
        return false; // the room's option: human bots don't turn to zombies they see
    return ZombieAwareAim(bot, aim, buttons);
}
