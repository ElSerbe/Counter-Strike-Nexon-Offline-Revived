// ---------------------------------------------------------------- holdout spots (Zombie, Zombie 2, Zombie Hero and its versions)
// The human bots all walked to the one group around the human with the most others near (ZombieGroupMove), so the whole
// team ended up in a single place. Real players spread over the map in a few groups, each holding a place of its own
// (a dead end, a corner, high ground). In these modes the human bots do that now:
//  - each map gets up to HOLD_SPOTS holdout spots from its nav mesh: areas of the mesh's main part, big enough to stand
//    on, not in a hurt zone, scored by how closed in they are (few links, few areas around) and how high they are over
//    what's around; the best ones at least HOLD_SEPARATION apart
//  - each new life a human bot picks one of the first K spots (K: one per HOLD_GROUP humans, 2 to 6): the nearest one
//    that isn't full, so the groups are about the same size
//  - it walks there (shooting what it meets: only its legs are driven) and holds its own place there (the spot's
//    areas, one per bot), standing still while it shoots; zombies closer than HOLD_FLEE make it back off a little
//  - alone at its spot (the rest of its group turned or died), it joins the spot with the most humans left
//  - a few bots walk the map alone instead (one per HOLD_ROAM_PER humans, from HOLD_ROAM_FROM humans): to a random
//    place of the mesh's main part HOLD_ROAM_MIN to HOLD_ROAM_MAX away, then another
// Under a player's orders, or the hero with the chainsaw, it's left as before. Zombie Escape has its own route.
static const int HOLD_SPOTS = 8, HOLD_GROUP = 4, HOLD_ROAM_PER = 8, HOLD_ROAM_FROM = 6;
static const float HOLD_ROAM_MIN = 700.0f, HOLD_ROAM_MAX = 2500.0f, HOLD_ROAM_TIME = 25.0f;
static const float HOLD_SEPARATION = 900.0f, HOLD_RADIUS = 220.0f, HOLD_FLEE = 170.0f, HOLD_MIN_SIZE = 40.0f;

struct HoldSpot
{
    V3 at;
    int area;
    std::vector<int> stand; // areas around it to stand on
};
struct HoldBot
{
    int spot = -1, place = 0;
    float nextCheck = 0, nextLog = 0;
    bool roam = false; // walks the map alone
    V3 roamGoal{};
    float roamUntil = 0;
};
static std::vector<int> g_holdMain; // areas of the mesh's main part (where the walkers go)
static std::vector<HoldSpot> g_holdSpots;
static int g_holdLevel = -1;
static std::map<void*, HoldBot> g_holdBots;

static bool HoldoutMode()
{
    int m = GameMode();
    return m == 8 || m == 9 || m == 14 || m == 44 || m == 45 || m == 58;
}

static void HoldPickSpots()
{
    g_holdLevel = g_nav.level;
    g_holdSpots.clear();
    g_holdBots.clear();
    g_holdMain.clear();
    size_t n = (std::min)(g_navBaseAreas ? g_navBaseAreas : g_nav.areas.size(), g_nav.areas.size());
    if (!g_nav.ok || n < 20)
        return;
    // the mesh's main part (links both ways)
    std::vector<std::vector<int>> links(n);
    for (size_t i = 0; i < n; i++)
        for (int d = 0; d < 4; d++)
            for (int j : g_nav.areas[i].adj[d])
                if (j >= 0 && (size_t)j < n)
                {
                    links[i].push_back(j);
                    links[j].push_back((int)i);
                }
    std::vector<int> part(n, -1);
    int best = -1, bestSize = 0;
    for (size_t s = 0, id = 0; s < n; s++)
    {
        if (part[s] >= 0)
            continue;
        std::vector<int> todo{ (int)s };
        part[s] = (int)id;
        int size = 0;
        while (!todo.empty())
        {
            int a = todo.back();
            todo.pop_back();
            size++;
            for (int b : links[a])
                if (part[b] < 0)
                {
                    part[b] = (int)id;
                    todo.push_back(b);
                }
        }
        if (size > bestSize)
        {
            bestSize = size;
            best = (int)id;
        }
        id++;
    }
    // scores
    struct Cand
    {
        int area;
        float score;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < n; i++)
        if (part[i] == best && !NavAreaHurts((int)i) && !(g_nav.areas[i].attr & (NAV_JUMP | NAV_CROUCH)))
            g_holdMain.push_back((int)i);
    for (size_t i = 0; i < n; i++)
    {
        const NavArea& a = g_nav.areas[i];
        if (part[i] != best || NavAreaHurts((int)i) || InHurtZone(a.center) || (a.attr & (NAV_JUMP | NAV_CROUCH)))
            continue;
        if (a.hi[0] - a.lo[0] < HOLD_MIN_SIZE || a.hi[1] - a.lo[1] < HOLD_MIN_SIZE)
            continue;
        int around = 0;
        float zSum = 0;
        int zN = 0;
        for (size_t j = 0; j < n; j++)
        {
            float d = Len2D(g_nav.areas[j].center - a.center);
            if (d < 350.0f && fabsf(g_nav.areas[j].center.z - a.center.z) < 120.0f)
                around++;
            if (d < 1000.0f)
            {
                zSum += g_nav.areas[j].center.z;
                zN++;
            }
        }
        float deg = (float)links[i].size() / 2.0f; // (each link counted from both ends)
        float height = zN ? a.center.z - zSum / zN : 0;
        float score = (deg <= 1.5f ? 2.0f : deg <= 2.5f ? 1.2f : deg <= 3.5f ? 0.5f : 0.0f) +
                      (std::max)(-1.0f, (std::min)(2.0f, height / 120.0f)) - around / 12.0f;
        cands.push_back({ (int)i, score });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score > y.score; });
    for (const Cand& c : cands)
    {
        V3 at = g_nav.areas[c.area].center;
        bool apart = true;
        for (auto& s : g_holdSpots)
            apart = apart && Len2D(s.at - at) >= HOLD_SEPARATION;
        if (!apart)
            continue;
        HoldSpot s;
        s.at = at;
        s.area = c.area;
        for (size_t j = 0; j < n; j++)
        {
            const NavArea& b = g_nav.areas[j];
            if (Len2D(b.center - at) < HOLD_RADIUS && fabsf(b.center.z - at.z) < 60.0f && part[j] == best && !NavAreaHurts((int)j))
                s.stand.push_back((int)j);
        }
        if (s.stand.empty())
            s.stand.push_back(c.area);
        g_holdSpots.push_back(s);
        if ((int)g_holdSpots.size() >= HOLD_SPOTS)
            break;
    }
    std::string list;
    for (auto& s : g_holdSpots)
    {
        char b[64];
        snprintf(b, sizeof(b), "%s(%.0f %.0f %.0f)", list.empty() ? "" : ", ", s.at.x, s.at.y, s.at.z);
        list += b;
    }
    Log("holdout: %d spots on %s for the human bots' groups: %s", (int)g_holdSpots.size(), g_nav.map.c_str(), list.c_str());
}

static int HoldSpotsInUse()
{
    int humans = 0;
    for (const ZfPlayer& pl : ZfPlayers())
        humans += !pl.zombie;
    int k = (humans + HOLD_GROUP - 1) / HOLD_GROUP;
    k = (std::max)(2, (std::min)(6, k));
    return (std::min)(k, (int)g_holdSpots.size());
}

// the living human bots holding each spot
static std::vector<int> HoldMembers(void* except = nullptr)
{
    std::vector<int> count(g_holdSpots.size(), 0);
    for (const ZfPlayer& pl : ZfPlayers())
    {
        if (pl.zombie || pl.e == except)
            continue;
        auto it = g_holdBots.find(pl.e);
        if (it != g_holdBots.end() && it->second.spot >= 0 && it->second.spot < (int)count.size())
            count[it->second.spot]++;
    }
    return count;
}

static void HoldAssign(void* bot, HoldBot& h, bool join)
{
    int k = HoldSpotsInUse();
    if (k <= 0)
    {
        h.spot = -1;
        return;
    }
    std::vector<int> count = HoldMembers(bot);
    int humans = 1;
    for (int c : count)
        humans += c;
    int cap = (humans + k - 1) / k;
    V3 me = Origin(bot);
    int pick = -1;
    float bestCost = 1e30f;
    for (int i = 0; i < k; i++)
    {
        float cost = join ? -1000.0f * count[i] + Len2D(g_holdSpots[i].at - me) * 0.1f // the biggest group left
                          : Len2D(g_holdSpots[i].at - me) + (count[i] >= cap ? 4000.0f * (count[i] - cap + 1) : 0.0f);
        if (join && (count[i] == 0 || i == h.spot))
            continue;
        if (cost < bestCost)
        {
            bestCost = cost;
            pick = i;
        }
    }
    if (pick < 0)
        return;
    int was = h.spot;
    h.spot = pick;
    h.place = count[pick];
    static int reports = 0;
    if (reports++ < 80)
        Log("holdout: bot '%s' %s spot %d (%.0f %.0f), %d humans there with it, %d spots in use",
            String(F<int>(Vars(bot), EV_NETNAME)), join && was >= 0 ? "alone: joins" : "holds", pick, g_holdSpots[pick].at.x,
            g_holdSpots[pick].at.y, count[pick] + 1, k);
}

// a new life: a walker if there are fewer than there should be, else a spot
static void HoldNewLife(void* bot, HoldBot& h)
{
    int humans = 0, walkers = 0;
    for (const ZfPlayer& pl : ZfPlayers())
    {
        if (pl.zombie)
            continue;
        humans++;
        auto it = g_holdBots.find(pl.e);
        walkers += pl.e != bot && it != g_holdBots.end() && it->second.roam;
    }
    int want = humans >= HOLD_ROAM_FROM ? (std::max)(1, humans / HOLD_ROAM_PER) : 0;
    h.roam = walkers < want && !g_holdMain.empty();
    h.roamUntil = 0;
    if (!h.roam)
    {
        HoldAssign(bot, h, false);
        return;
    }
    h.spot = -1;
    static int reports = 0;
    if (reports++ < 40)
        Log("holdout: bot '%s' walks the map alone (%d of %d walkers, %d humans)", String(F<int>(Vars(bot), EV_NETNAME)),
            walkers + 1, want, humans);
}

// to a place on foot (path, ledges, getting unstuck); false: a drop ahead, the stock AI moves it
static bool HoldWalk(void* bot, BotState& st, float viewYaw, V3 place, float speed, float& fwd, float& side,
                     unsigned short& buttons)
{
    float now = Now();
    V3 me = Origin(bot), wp;
    if (!NextWaypoint(bot, st, me, place, 40.0f, wp))
        wp = place;
    float moveYaw = WalkYaw(bot, me, YawOf(wp - me), 40.0f);
    if (DropAhead(bot, me, moveYaw, 72.0f, HURT_DROP_MAX))
        return false;
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
    fwd = cosf(rel) * speed;
    side = -sinf(rel) * speed;
    return true;
}

static bool HoldRoam(void* bot, BotState& st, HoldBot& h, float viewYaw, float& fwd, float& side, unsigned short& buttons)
{
    float now = Now();
    V3 me = Origin(bot);
    if (now >= h.roamUntil || Len2D(h.roamGoal - me) < 80.0f)
    {
        for (int k = 0; k < 40; k++)
        {
            V3 at = g_nav.areas[g_holdMain[rand() % g_holdMain.size()]].center;
            float d = Len2D(at - me);
            if (d >= HOLD_ROAM_MIN && d <= HOLD_ROAM_MAX && !InHurtZone(at))
            {
                h.roamGoal = at;
                break;
            }
        }
        h.roamUntil = now + HOLD_ROAM_TIME;
        DLog("holdout: walker '%s' goes to (%.0f %.0f), %.0f u away", String(F<int>(Vars(bot), EV_NETNAME)), h.roamGoal.x,
             h.roamGoal.y, Len2D(h.roamGoal - me));
    }
    if (!HoldWalk(bot, st, viewYaw, h.roamGoal, 230.0f, fwd, side, buttons))
    {
        h.roamUntil = 0; // another place
        return false;
    }
    return true;
}

static bool HoldoutAtSpot(void* bot)
{
    auto it = g_holdBots.find(bot);
    if (it == g_holdBots.end() || it->second.spot < 0 || it->second.spot >= (int)g_holdSpots.size())
        return false;
    return Len2D(Origin(bot) - g_holdSpots[it->second.spot].at) < HOLD_RADIUS;
}

static float HoldoutFleeRange(void* bot, float normal)
{
    return HoldoutAtSpot(bot) ? HOLD_FLEE : normal;
}

static bool HoldoutMove(void* bot, BotState& st, float viewYaw, float& fwd, float& side, unsigned short& buttons, bool& handled)
{
    handled = false;
    if (g_holdLevel != g_nav.level)
        HoldPickSpots();
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (g_holdSpots.size() < 2 || !p || !Readable(p, 0x2170) || p[0x2166] || st.ordered)
        return false;
    handled = true; // (the single-group regrouping isn't used in these modes)
    HoldBot& h = g_holdBots[bot];
    float now = Now();
    if (st.newLife || (!h.roam && (h.spot < 0 || h.spot >= (int)g_holdSpots.size())))
        HoldNewLife(bot, h);
    if (h.roam)
        return HoldRoam(bot, st, h, viewYaw, fwd, side, buttons);
    if (now >= h.nextCheck)
    {
        h.nextCheck = now + 3.0f;
        if (HoldoutAtSpot(bot) && HoldMembers(bot)[h.spot] == 0)
            HoldAssign(bot, h, true); // its group is gone
    }
    if (h.spot < 0)
        return false;
    const HoldSpot& s = g_holdSpots[h.spot];
    V3 me = Origin(bot);
    V3 place = g_nav.areas[s.stand[h.place % s.stand.size()]].center;
    float d = Len2D(place - me);
    if (d < 48.0f && fabsf(place.z - me.z) < 64.0f)
    {
        fwd = side = 0; // holding: the stock AI aims and shoots
        return true;
    }
    // inside its spot: no running (it may be shooting)
    float speed = Len2D(s.at - me) < HOLD_RADIUS ? 150.0f : 250.0f;
    if (!HoldWalk(bot, st, viewYaw, place, speed, fwd, side, buttons))
        return false;
    if (now >= h.nextLog)
    {
        h.nextLog = now + 15.0f;
        DLog("holdout: bot '%s' walks to spot %d, %.0f u away", String(F<int>(Vars(bot), EV_NETNAME)), h.spot, d);
    }
    return true;
}
