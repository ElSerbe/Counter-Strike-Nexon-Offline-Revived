// botassist: Zombie Escape (mode 20). Included by botassist.cpp at this point (one translation unit: it uses the shared
// helpers defined before it, Zombie Conquest's ZConSafeYaw among them). Only runs in its own mode: ZeMove isn't called unless
// GameMode() == MODE_ZE.
//
// The mode (cso.fandom.com/wiki/Zombie_Escape and the maps' entities): one player becomes the origin zombie, the humans run
// from the start along a scripted route to an escape vehicle, holding at defence points while a countdown opens the way
// (ze_counter, "#CSO_ZE_DEFENSE<n>"), pressing buttons (func_button / ze_button) and breaking barriers (ze_breakable). Humans
// win when they reach the exit (ze_exit) and the vehicle leaves; zombies win by infecting everyone or reaching it first.
// The route is measured by 20 boxes, ze_status "id" 1..20, in order: the game's progress bar ("spaces" on the wiki).
// Venice has no ze_status: three escape points (ze_exit, ze_hotspot, ze_button for the radio).
//
// The maps' own .nav files are all the same placeholder (another map's mesh), so the game's bots had no way around. The
// meshes come from tools/botassist/gen_nav.py (the map's clip hulls, doors and breakables open) and live in
// Bin\botassist_nav, where nav_tools.inl's HookedLoadFile serves them to the game and to botassist.
//
// What botassist does (the stock AI keeps aiming and shooting at zombies; ZeMove replaces the movement, like Zombie War):
//   - Humans go from route box to route box (the next one after the highest they reached), then into the exit. A button near
//     the box they are on or the next one, still where the map put it (not pressed), is pressed by one bot. A barrier in the
//     way is shot (when no zombie is in sight). Pushing on a closed door, they turn round and hold: a defence countdown opens
//     it. With no way on the mesh to the next box (a lift, a train, a jump the mesh lacks), they follow the human furthest
//     along the route, and failing that walk as close as the mesh goes.
//   - Zombies chase a human they see or hear, else go where the humans are furthest along.
// The map's boxes come from its .bsp (the entity lump and the model bounds), read through the engine's file system.
static const int MODE_ZE = 20;
static const float ZE_HEAR = 600.0f, ZE_BUTTON_NEAR = 700.0f, ZE_BUTTON_ZONE_PAD = 350.0f, ZE_BREAK_AHEAD = 140.0f;
static const float ZE_ZOMBIE_SEEN = 1500.0f;

struct ZeBox
{
    V3 lo, hi;
    V3 spot;          // a point of the nav mesh inside it
    bool hasSpot = false;
};
struct ZeButton
{
    V3 lo, hi;        // where the map puts it
    std::string cls;
    void* claimedBy = nullptr;
    float claimUntil = 0;
    bool pressed = false;
};
struct ZeBot
{
    int zone = 0;     // the highest route box reached in this life
    float lastMoveAt = -100;
    V3 lastMovePos{};
    int task = 0;     // 0 route, 1 button, 2 barrier, 3 hold at a door, 4 follow, 5 exit
    int stuck = 0;
    float stuckAt = 0, holdUntil = 0, jumpAt = -100, dodgeUntil = 0, dodgeYaw = 0, nextLog = 0;
    float yaw = 0;
    bool yawSet = false;
    int passSide = 1;
};
static std::map<void*, ZeBot> g_zeBots;
static std::map<void*, int> g_zeZone; // every player's highest route box in this life
static struct
{
    int level = -1;
    std::string map;
    bool loaded = false;
    std::vector<ZeBox> zones, exits;
    std::vector<V3> counters;
    std::vector<ZeButton> buttons;
    int leadZone = 0;
    V3 leadPos{};
    void* leader = nullptr;
    float nextUpdate = 0, nextLog = 0;
    int humans = 0, zombies = 0;
} g_ze;

// ---- the map's boxes, from its .bsp
static std::vector<std::map<std::string, std::string>> ZeEntities(const char* text, int len)
{
    std::vector<std::map<std::string, std::string>> out;
    std::map<std::string, std::string> cur;
    bool in = false;
    int i = 0;
    auto quoted = [&](std::string& s) {
        s.clear();
        while (i < len && text[i] != '"')
            i++;
        if (i >= len)
            return false;
        i++;
        while (i < len && text[i] != '"')
            s += text[i++];
        i++;
        return true;
    };
    while (i < len)
    {
        char c = text[i];
        if (c == '{')
        {
            in = true;
            cur.clear();
            i++;
        }
        else if (c == '}')
        {
            if (in)
                out.push_back(cur);
            in = false;
            i++;
        }
        else if (c == '"' && in)
        {
            std::string k, v;
            if (!quoted(k) || !quoted(v))
                break;
            cur[k] = v;
        }
        else
            i++;
    }
    return out;
}

static V3 ZeParseV3(const std::string& s)
{
    V3 v{ 0, 0, 0 };
    sscanf(s.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

// a nav point inside the box: the area nearest its middle (the box's floor, not its centre, which may be in the air)
static bool ZeSpotIn(const V3& lo, const V3& hi, V3& out)
{
    V3 mid = (lo + hi) * 0.5f;
    float best = 1e30f;
    bool found = false;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        V3 c = a.center;
        float x = c.x < lo.x ? lo.x : c.x > hi.x ? hi.x : c.x, y = c.y < lo.y ? lo.y : c.y > hi.y ? hi.y : c.y;
        if (a.hi[0] < lo.x || a.lo[0] > hi.x || a.hi[1] < lo.y || a.lo[1] > hi.y || c.z < lo.z - 48.0f || c.z > hi.z ||
            NavAreaHurts((int)i))
            continue;
        V3 p{ x, y, AreaZ(a, x, y) };
        float d = Len2D(p - mid);
        if (d < best)
        {
            best = d;
            out = p;
            found = true;
        }
    }
    return found;
}

static void ZeLoad()
{
    g_ze.level = g_nav.level;
    g_ze.map = g_nav.map;
    g_ze.loaded = false;
    g_ze.zones.clear();
    g_ze.exits.clear();
    g_ze.counters.clear();
    g_ze.buttons.clear();
    g_zeZone.clear();
    g_zeBots.clear();
    std::string path = "maps/" + g_nav.map + ".bsp";
    int len = 0;
    unsigned char* d = g_LoadFile(path.c_str(), &len);
    if (!d || len < 4 + 15 * 8 || *(int*)d != 30)
    {
        Log("ZE: couldn't read %s for the escape route", path.c_str());
        if (d)
            g_FreeFile(d);
        return;
    }
    int* lumps = (int*)(d + 4);
    int entOff = lumps[0], entLen = lumps[1], modOff = lumps[28], modLen = lumps[29];
    if (entOff < 0 || entOff + entLen > len || modOff < 0 || modOff + modLen > len)
    {
        g_FreeFile(d);
        return;
    }
    auto ents = ZeEntities((const char*)d + entOff, entLen);
    int models = modLen / 64;
    auto box = [&](const std::map<std::string, std::string>& e, V3& lo, V3& hi) {
        auto m = e.find("model");
        if (m == e.end() || m->second.size() < 2 || m->second[0] != '*')
            return false;
        int idx = atoi(m->second.c_str() + 1);
        if (idx <= 0 || idx >= models)
            return false;
        float* f = (float*)(d + modOff + idx * 64);
        V3 org{ 0, 0, 0 };
        auto o = e.find("origin");
        if (o != e.end())
            org = ZeParseV3(o->second);
        lo = V3{ f[0], f[1], f[2] } + org;
        hi = V3{ f[3], f[4], f[5] } + org;
        return true;
    };
    std::map<int, ZeBox> byId;
    for (auto& e : ents)
    {
        auto c = e.find("classname");
        if (c == e.end())
            continue;
        const std::string& cls = c->second;
        V3 lo, hi;
        if (cls == "ze_status" && box(e, lo, hi))
        {
            auto id = e.find("id");
            ZeBox b;
            b.lo = lo;
            b.hi = hi;
            byId[id != e.end() ? atoi(id->second.c_str()) : (int)byId.size() + 1] = b;
        }
        else if ((cls == "ze_exit" || cls == "ze_hotspot") && box(e, lo, hi))
        {
            ZeBox b;
            b.lo = lo;
            b.hi = hi;
            g_ze.exits.push_back(b);
        }
        else if ((cls == "func_button" || cls == "func_rot_button" || cls == "ze_button") && box(e, lo, hi))
        {
            ZeButton b;
            b.lo = lo;
            b.hi = hi;
            b.cls = cls;
            g_ze.buttons.push_back(b);
        }
        else if (cls == "ze_counter")
        {
            auto o = e.find("origin");
            if (o != e.end())
                g_ze.counters.push_back(ZeParseV3(o->second));
        }
    }
    g_FreeFile(d);
    for (auto& kv : byId)
        g_ze.zones.push_back(kv.second);
    int spots = 0;
    for (auto& z : g_ze.zones)
        spots += (z.hasSpot = ZeSpotIn(z.lo, z.hi, z.spot)) ? 1 : 0;
    for (auto& x : g_ze.exits)
        spots += (x.hasSpot = ZeSpotIn(x.lo, x.hi, x.spot)) ? 1 : 0;
    g_ze.loaded = true;
    Log("ZE: %s: %d route boxes, %d exits, %d buttons, %d defence counters; %d of them on the navigation mesh (%d areas%s)",
        g_nav.map.c_str(), (int)g_ze.zones.size(), (int)g_ze.exits.size(), (int)g_ze.buttons.size(), (int)g_ze.counters.size(),
        spots, (int)g_nav.areas.size(), FileThere(NavFileOf(g_nav.map)) ? ", botassist_nav" : ", the game's: not this map's");
}

static bool ZeInBox(V3 p, const ZeBox& b, float pad)
{
    return p.x >= b.lo.x - pad && p.x <= b.hi.x + pad && p.y >= b.lo.y - pad && p.y <= b.hi.y + pad && p.z >= b.lo.z - 40.0f &&
           p.z <= b.hi.z + pad;
}

static float ZeBoxDist(V3 p, V3 lo, V3 hi)
{
    float dx = p.x < lo.x ? lo.x - p.x : p.x > hi.x ? p.x - hi.x : 0;
    float dy = p.y < lo.y ? lo.y - p.y : p.y > hi.y ? p.y - hi.y : 0;
    float dz = p.z < lo.z ? lo.z - p.z : p.z > hi.z ? p.z - hi.z : 0;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static bool ZeIsZombie(void* e)
{
    unsigned char* q = (unsigned char*)PrivateOf(e);
    return q && Readable(q, 0x2170) && q[0x2166];
}

// every player's progress, the leading human, the buttons' state: twice a second
static void ZeUpdate()
{
    if (g_ze.level != g_nav.level || g_ze.map != g_nav.map)
        ZeLoad();
    float now = Now();
    if (now < g_ze.nextUpdate && now > g_ze.nextUpdate - 5.0f)
        return;
    g_ze.nextUpdate = now + 0.5f;
    g_ze.leadZone = 0;
    g_ze.leader = nullptr;
    g_ze.humans = g_ze.zombies = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            return;
        if (!Alive(e))
        {
            g_zeZone.erase(e);
            return;
        }
        bool zombie = ZeIsZombie(e);
        (zombie ? g_ze.zombies : g_ze.humans)++;
        V3 p = Origin(e);
        int& z = g_zeZone[e];
        for (size_t i = 0; i < g_ze.zones.size(); i++)
            if ((int)i + 1 > z && ZeInBox(p, g_ze.zones[i], 8.0f))
                z = (int)i + 1;
        if (!zombie && (z > g_ze.leadZone || !g_ze.leader))
        {
            g_ze.leadZone = z;
            g_ze.leadPos = p;
            g_ze.leader = e;
        }
    });
    // a button has moved off its place once pressed (they don't come back: "wait" -1), or its entity is gone
    for (auto& b : g_ze.buttons)
    {
        if (b.pressed)
            continue;
        V3 mid = (b.lo + b.hi) * 0.5f;
        void* ent = nullptr;
        ForEntitiesNear(mid, 48.0f, [&](void* e) {
            if (!ent && b.cls == ClassName(e))
                ent = e;
        });
        if (!ent)
            continue;
        V3 lo = Vec((float*)(Vars(ent) + EV_ABSMIN)), hi = Vec((float*)(Vars(ent) + EV_ABSMAX));
        if (Len(lo - b.lo) > 3.0f || Len(hi - b.hi) > 3.0f)
        {
            b.pressed = true;
            Log("ZE: the %s at (%.0f %.0f %.0f) was pressed", b.cls.c_str(), mid.x, mid.y, mid.z);
        }
    }
    if (now >= g_ze.nextLog)
    {
        g_ze.nextLog = now + 15.0f;
        static int reports = 0;
        if (reports++ < 80)
            Log("ZE: %d humans, %d zombies; the leading human is on route box %d of %d", g_ze.humans, g_ze.zombies,
                g_ze.leadZone, (int)g_ze.zones.size());
    }
}

// a barrier in the way: a breakable that can be damaged, close and ahead
static void* ZeBarrierAhead(void* bot, V3 me, V3 toward)
{
    V3 dir = toward - me;
    dir.z = 0;
    float l = Len2D(dir);
    if (l < 1.0f)
        return nullptr;
    dir = dir * (1.0f / l);
    void* found = nullptr;
    float best = 1e9f;
    ForEntitiesNear(me, ZE_BREAK_AHEAD + 120.0f, [&](void* e) {
        const char* cls = ClassName(e);
        if (strcmp(cls, "ze_breakable") && strcmp(cls, "func_breakable"))
            return;
        unsigned char* v = Vars(e);
        if (F<float>(v, EV_HEALTH) <= 0 || F<float>(v, EV_TAKEDAMAGE) == 0.0f || !F<int>(v, EV_SOLID))
            return;
        V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
        float d = ZeBoxDist(me, lo, hi);
        V3 to = (lo + hi) * 0.5f - me;
        to.z = 0;
        float tl = Len2D(to);
        if (d > ZE_BREAK_AHEAD || hi.z < me.z - 36.0f || lo.z > me.z + 60.0f)
            return;
        if (tl > 1.0f && (to.x * dir.x + to.y * dir.y) / tl < 0.3f && d > 24.0f)
            return; // not ahead (unless the bot is against it)
        if (d < best)
        {
            best = d;
            found = e;
        }
    });
    return found;
}

// a closed door right ahead: still at its place, solid, between the bot and where it goes
static bool ZeDoorAhead(void* bot, V3 me, float yaw)
{
    bool found = false;
    V3 ahead = me + V3{ cosf(yaw / RAD) * 64.0f, sinf(yaw / RAD) * 64.0f, 0 };
    ForEntitiesNear(ahead, 120.0f, [&](void* e) {
        const char* cls = ClassName(e);
        if (strcmp(cls, "func_door") && strcmp(cls, "func_door_rotating") && strcmp(cls, "func_wall_toggle"))
            return;
        unsigned char* v = Vars(e);
        if (!F<int>(v, EV_SOLID))
            return;
        if (ZeBoxDist(ahead, Vec((float*)(v + EV_ABSMIN)), Vec((float*)(v + EV_ABSMAX))) < 40.0f)
            found = true;
    });
    return found;
}

// a nearby visible zombie (for a human): the stock AI has it, botassist leaves the aim alone
static bool ZeZombieInSight(void* bot, V3 eye)
{
    bool seen = false;
    ForEntitiesNear(eye, ZE_ZOMBIE_SEEN, [&](void* e) {
        if (seen || e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || !ZeIsZombie(e))
            return;
        seen = CanSee(bot, eye, e);
    });
    return seen;
}

static bool ZeMove(void* bot, BotState& st, float& viewYaw, float& viewPitch, bool& setView, float& fwd, float& side,
                   unsigned short& buttons)
{
    setView = false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || !g_nav.ok || (st.ordered && st.order != ORDER_INDEPENDENT))
        return false;
    ZeUpdate();
    if (!g_ze.loaded)
        return false;
    ZeBot& z = g_zeBots[bot];
    float now = Now();
    V3 me = Origin(bot), eye = Eye(bot);
    bool zombie = p[0x2166] != 0;
    bool respawned = now - z.lastMoveAt > 0.5f || Len(me - z.lastMovePos) > 300.0f;
    z.lastMoveAt = now;
    z.lastMovePos = me;
    if (respawned)
    {
        z.stuck = 0;
        z.holdUntil = 0;
        z.yawSet = false;
        z.passSide = rand() % 2 ? 1 : -1;
    }
    // frozen (the start of a round, the zombies' wait): nobody is stuck
    if (F<float>(Vars(bot), EV_MAXSPEED) <= 1.0f || (F<int>(Vars(bot), EV_FLAGS) & FL_FROZEN))
    {
        st.lastPos = me;
        st.lastProgress = now;
        return false;
    }
    z.zone = g_zeZone.count(bot) ? g_zeZone[bot] : 0;

    V3 goal{};
    float arrive = 40.0f;
    void* aimAt = nullptr;   // a barrier to shoot or a button to press
    bool press = false;
    z.task = 0;
    if (zombie)
    {
        // a human seen or heard: on it; else where the humans are furthest along
        void* prey = nullptr;
        float nearest = 1e9f;
        ForEntitiesNear(me, ZE_ZOMBIE_SEEN, [&](void* e) {
            if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || ZeIsZombie(e))
                return;
            float d = Len(Origin(e) - me);
            if (d < nearest && (d < ZE_HEAR || CanSee(bot, eye, e)))
            {
                nearest = d;
                prey = e;
            }
        });
        if (prey)
            goal = Origin(prey), arrive = 16.0f;
        else if (g_ze.leader)
            goal = g_ze.leadPos, arrive = 48.0f;
        else
            return false; // no humans left: the round is over
    }
    else
    {
        // the route: the next box after the highest one reached, then an exit
        int next = z.zone + 1;
        const ZeBox* target = nullptr;
        if (next <= (int)g_ze.zones.size())
            target = &g_ze.zones[next - 1];
        else if (!g_ze.exits.empty())
        {
            float best = 1e30f;
            for (auto& x : g_ze.exits)
                if (x.hasSpot && Len(x.spot - me) < best)
                {
                    best = Len(x.spot - me);
                    target = &x;
                }
            z.task = 5;
        }
        // skip a box the mesh doesn't reach inside: the one after it
        while (target && !target->hasSpot && next < (int)g_ze.zones.size())
            target = &g_ze.zones[next++];
        if (target && target->hasSpot)
        {
            goal = target->spot;
            arrive = 32.0f;
            if (z.task == 5 && ZeInBox(me, *target, -16.0f))
            {
                // inside the exit: wait for the vehicle, out of the way of the others
                fwd = side = 0;
                st.lastProgress = now;
                return true;
            }
        }
        else if (g_ze.leader && g_ze.leader != bot)
            goal = g_ze.leadPos, z.task = 4;
        else
            return false;
        // a button by this box or the next one, not pressed yet: one bot goes and presses it
        const ZeBox* here = z.zone > 0 && z.zone <= (int)g_ze.zones.size() ? &g_ze.zones[z.zone - 1] : nullptr;
        ZeButton* button = nullptr;
        float bestButton = ZE_BUTTON_NEAR;
        for (auto& b : g_ze.buttons)
        {
            if (b.pressed || (b.claimedBy && b.claimedBy != bot && now < b.claimUntil))
                continue;
            V3 mid = (b.lo + b.hi) * 0.5f;
            bool byRoute = (target && ZeBoxDist(mid, target->lo, target->hi) < ZE_BUTTON_ZONE_PAD) ||
                           (here && ZeBoxDist(mid, here->lo, here->hi) < ZE_BUTTON_ZONE_PAD) || z.task == 5;
            float d = Len(mid - me);
            if (byRoute && d < bestButton)
            {
                bestButton = d;
                button = &b;
            }
        }
        if (button)
        {
            button->claimedBy = bot;
            button->claimUntil = now + 3.0f;
            V3 mid = (button->lo + button->hi) * 0.5f;
            z.task = 1;
            if (ZeBoxDist(eye, button->lo, button->hi) < 56.0f)
            {
                // close enough: aim at it and use it
                V3 d = mid - eye;
                viewYaw = YawOf(d);
                viewPitch = PitchOf(d);
                setView = true;
                fwd = side = 0;
                if (fmodf(now, 0.4f) < 0.2f)
                    buttons |= IN_USE;
                st.lastProgress = now;
                static int reports = 0;
                if (reports++ < 40)
                    DLog("ZE: '%s' presses the %s at (%.0f %.0f %.0f)", String(F<int>(Vars(bot), EV_NETNAME)), button->cls.c_str(),
                         mid.x, mid.y, mid.z);
                return true;
            }
            goal = mid;
            goal.z = button->lo.z;
            arrive = 24.0f;
        }
    }

    V3 wp;
    bool there = !NextWaypoint(bot, st, me, goal, arrive, wp, true);
    bool pathed = there || st.path.size() > 1 || AreaAt(me) == AreaAt(goal);
    if (!pathed && !zombie && z.task == 0 && g_ze.leader && g_ze.leader != bot && Len(g_ze.leadPos - me) > 200.0f)
    {
        // no way on the mesh to the next box: after the one who got furthest
        z.task = 4;
        there = !NextWaypoint(bot, st, me, g_ze.leadPos, 120.0f, wp, true);
    }
    if (there)
    {
        fwd = side = 0;
        z.yawSet = false;
        st.lastProgress = now;
        return true;
    }
    float wayYaw = YawOf(wp - me);

    // a barrier between it and where it goes: shot down (a human with no zombie to shoot at)
    if (!zombie)
    {
        void* barrier = ZeBarrierAhead(bot, me, wp);
        if (barrier && !ZeZombieInSight(bot, eye))
        {
            V3 c = Center(barrier);
            V3 d = c - eye;
            viewYaw = YawOf(d);
            viewPitch = PitchOf(d);
            setView = true;
            if (fmodf(now, 0.2f) < 0.1f)
                buttons |= IN_ATTACK;
            z.task = 2;
            st.lastProgress = now;
            float rel = (wayYaw - viewYaw) / RAD;
            float push = ZeBoxDist(me, Vec((float*)(Vars(barrier) + EV_ABSMIN)), Vec((float*)(Vars(barrier) + EV_ABSMAX))) > 48.0f ? 250.0f : 0.0f;
            fwd = cosf(rel) * push;
            side = -sinf(rel) * push;
            return true;
        }
    }
    // holding at a closed door (the defence countdowns open them): back to it, the stock AI shoots what comes
    if (!zombie && now < z.holdUntil)
    {
        fwd = side = 0;
        st.lastProgress = now;
        z.task = 3;
        return true;
    }

    float wantYaw = WalkYaw(bot, me, wayYaw, 40.0f);
    if (now < z.dodgeUntil)
        wantYaw = WalkYaw(bot, me, z.dodgeYaw, 40.0f);
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
    else if (now - st.lastProgress > 0.8f)
    {
        st.lastProgress = now;
        st.nextRepath = 0;
        z.stuck++;
        z.stuckAt = now;
        if (!zombie && ZeDoorAhead(bot, me, wayYaw))
        {
            z.holdUntil = now + 4.0f; // a door that hasn't opened yet: wait, then try again
            z.stuck = 0;
        }
        else
        {
            z.passSide = -z.passSide;
            z.dodgeYaw = z.stuck % 2 ? wayYaw + 90.0f * z.passSide : wayYaw + 180.0f;
            z.dodgeUntil = now + 0.5f;
            jump = jumpSafe;
        }
        static int reports = 0;
        if (z.stuck >= 3 && now >= z.nextLog && reports < 80)
        {
            reports++;
            z.nextLog = now + 6.0f;
            Log("ZE: %s '%s' stuck at (%.0f %.0f %.0f) on box %d, going to (%.0f %.0f %.0f), task %d", zombie ? "zombie" : "human",
                String(F<int>(Vars(bot), EV_NETNAME)), me.x, me.y, me.z, z.zone, wp.x, wp.y, wp.z, z.task);
        }
    }
    if (jump && now - z.jumpAt > 0.7f)
    {
        z.jumpAt = now;
        buttons |= IN_JUMP;
    }
    if (now - z.jumpAt > 0.1f && now - z.jumpAt < 0.5f)
        buttons |= IN_DUCK;
    if (!canMove)
    {
        fwd = side = 0;
        return true;
    }
    // nothing to shoot: it looks where it walks (a zombie always does)
    if (zombie || !ZeZombieInSight(bot, eye))
    {
        z.yaw = z.yawSet ? Approach(z.yaw, moveYaw, 9.0f) : viewYaw;
        z.yawSet = true;
        viewYaw = z.yaw;
        viewPitch = 0;
        setView = true;
    }
    else
        z.yawSet = false;
    float rel = (moveYaw - viewYaw) / RAD;
    fwd = cosf(rel) * 400.0f;
    side = -sinf(rel) * 400.0f;
    return true;
}
