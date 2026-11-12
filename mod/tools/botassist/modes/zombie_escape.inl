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
//   - A wall or door in the way that a breakable somewhere else opens (the map fires it: Hitchhiking's start wall goes when
//     the pipe above it is shot, through a multi_manager): they shoot that breakable, backing off until they see it.
//   - A low passage (no room to stand ahead, room crouching): they crouch. A ledge to jump onto is walked at straight (no
//     steering round what the body bumps), and a barrier low enough to clear (a rail, a fence: up to 46 u above the feet)
//     is jumped over instead of shot.
//   - A new round (a human back in the first box with a later one reached): everyone's progress and the buttons start over.
//   - Zombie modes' backing off and regrouping (zombie_humans.inl) don't run here: the way out is forward.
//   - A barrier is shot even with zombies in sight when none is close (ZE_BARRIER_SAFE) or the bot has been held at it a
//     while: Hitchhiking's rail on the truck (1500 HP) was never shot because some zombie was always in sight.
//   - Progress counts box by box: the next box or the one after (a route passing the corner of a later box, Hitchhiking's
//     box 6 beside the way from 2 to 3, made the leader "on box 6" at box 3).
//   - Only what stands in the body's way is a barrier: a breakable under the feet (the planks over Hitchhiking's pool) is
//     floor, never shot. In water a bot swims up and climbs out (jump held). Jumps and crouches cost more on the way
//     (ZeRouteCost): flat ground round a car beats climbing over it.
//   - Zombies chase a human they see or hear, else go where the humans are furthest along.
// The map's boxes come from its .bsp (the entity lump and the model bounds), read through the engine's file system.
static const int MODE_ZE = 20;
static const float ZE_HEAR = 600.0f, ZE_BUTTON_NEAR = 700.0f, ZE_BUTTON_ZONE_PAD = 350.0f, ZE_BREAK_AHEAD = 140.0f;
static const float ZE_ZOMBIE_SEEN = 1500.0f, ZE_TRIGGER_NEAR = 260.0f, ZE_TRIGGER_BACK = 520.0f, ZE_BARRIER_SAFE = 600.0f;

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
    bool seen = false;    // its entity's bounds at the start (an engine's bounds differ a little from the .bsp's)
};
// a breakable that opens the way when it breaks: what it fires (through multi_managers and relays) turns off walls or
// opens doors, the blockers
struct ZeBlock
{
    V3 lo, hi;
    std::string cls;
};
struct ZeTrigger
{
    V3 lo, hi;
    std::vector<ZeBlock> blocks;
    float retryAt = 0;    // not there (broken this round): look again later (a new round brings it back)
    bool told = false;
};
struct ZeBot
{
    int zone = 0;     // the highest route box reached in this life
    float lastMoveAt = -100;
    V3 lastMovePos{};
    int task = 0;     // 0 route, 1 button, 2 barrier, 3 hold at a door, 4 follow, 5 exit
    int stuck = 0;
    float stuckAt = 0, holdUntil = 0, jumpAt = -100, dodgeUntil = 0, dodgeYaw = 0, nextLog = 0;
    float barrierSince = 0;   // held at a barrier since (0: not at one)
    const void* toldButton = nullptr;
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
    std::vector<ZeTrigger> triggers;
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
    g_ze.triggers.clear();
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
    // the breakables that open the way: follow what they fire to the walls and doors it turns off or opens
    std::map<std::string, std::vector<size_t>> named;
    for (size_t i = 0; i < ents.size(); i++)
    {
        auto t = ents[i].find("targetname");
        if (t != ents[i].end())
            named[t->second].push_back(i);
    }
    static const char* notTargets[] = { "classname", "targetname", "origin", "spawnflags", "wait", "angles", "angle", "delay",
                                        "master", "target", "killtarget" };
    for (auto& e : ents)
    {
        auto c = e.find("classname");
        if (c == e.end() || (c->second != "ze_breakable" && c->second != "func_breakable"))
            continue;
        auto sf = e.find("spawnflags");
        if (sf != e.end() && (atoi(sf->second.c_str()) & 1))
            continue; // "only trigger": it can't be shot
        ZeTrigger trig;
        if (!box(e, trig.lo, trig.hi))
            continue;
        std::vector<std::string> todo;
        std::set<std::string> done;
        auto fires = [&](const std::map<std::string, std::string>& n) {
            for (const char* k : { "target", "killtarget" })
            {
                auto f = n.find(k);
                if (f != n.end() && !f->second.empty())
                    todo.push_back(f->second);
            }
        };
        fires(e);
        while (!todo.empty() && done.size() < 64)
        {
            std::string name = todo.back();
            todo.pop_back();
            if (!done.insert(name).second)
                continue;
            for (size_t idx : named[name])
            {
                auto& n = ents[idx];
                const std::string& cls = n.count("classname") ? n.at("classname") : std::string();
                V3 lo, hi;
                if ((cls == "func_wall_toggle" || cls == "func_door" || cls == "func_door_rotating" || cls == "func_wall") &&
                    box(n, lo, hi))
                    trig.blocks.push_back({ lo, hi, cls });
                if (cls == "multi_manager")
                {
                    for (auto& kv : n)
                    {
                        bool skip = false;
                        for (const char* k : notTargets)
                            skip = skip || kv.first == k;
                        if (!skip)
                            todo.push_back(kv.first.substr(0, kv.first.find('#'))); // "name#2": the same target again
                    }
                }
                else
                    fires(n);
            }
        }
        if (!trig.blocks.empty())
            g_ze.triggers.push_back(trig);
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
    Log("ZE: %s: %d route boxes, %d exits, %d buttons, %d breakables that open the way, %d defence counters; %d of them on "
        "the navigation mesh (%d areas)", g_nav.map.c_str(), (int)g_ze.zones.size(), (int)g_ze.exits.size(),
        (int)g_ze.buttons.size(), (int)g_ze.triggers.size(), (int)g_ze.counters.size(), spots, (int)g_nav.areas.size());
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
    // a new round: the players are back at the start (in the first box) with a later box reached; everyone starts over,
    // and the buttons are back where the map puts them
    bool restart = false;
    if (!g_ze.zones.empty())
        ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
            if (!restart && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) && Alive(e) && g_zeZone.count(e) && g_zeZone[e] > 2 &&
                ZeInBox(Origin(e), g_ze.zones[0], 0.0f))
                restart = true;
        });
    if (restart)
    {
        g_zeZone.clear();
        g_zeBots.clear();
        for (auto& b : g_ze.buttons)
            b.pressed = b.seen = false, b.claimedBy = nullptr;
        for (auto& t : g_ze.triggers)
            t.retryAt = 0;
        Log("ZE: a new round: the route starts over");
    }
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
            if ((int)i + 1 > z && (int)i + 1 <= z + 2 && ZeInBox(p, g_ze.zones[i], 8.0f))
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
        if (!b.seen)
        {
            // its place as the engine has it (a rotating button's box is grown to its turning radius, every box by a unit)
            b.seen = true;
            b.lo = lo;
            b.hi = hi;
            continue;
        }
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

// a barrier in the way: a breakable that can be damaged, where the body goes next (along the line to `toward`, the
// body's width either side, from a step above the feet to the head). One under the feet is floor: the planks of a bridge.
static void* ZeBarrierAhead(void* bot, V3 me, V3 toward)
{
    V3 dir = toward - me;
    dir.z = 0;
    float l = Len2D(dir);
    if (l < 1.0f)
        return nullptr;
    dir = dir * (1.0f / l);
    float reach = l < ZE_BREAK_AHEAD ? l + 24.0f : ZE_BREAK_AHEAD;
    float feet = me.z - ((F<int>(Vars(bot), EV_FLAGS) & 0x4000 /* FL_DUCKING */) ? 18.0f : 36.0f);
    void* found = nullptr;
    float best = 1e9f;
    ForEntitiesNear(me, ZE_BREAK_AHEAD + 400.0f, [&](void* e) {
        const char* cls = ClassName(e);
        if (strcmp(cls, "ze_breakable") && strcmp(cls, "func_breakable"))
            return;
        unsigned char* v = Vars(e);
        if (F<float>(v, EV_HEALTH) <= 0 || F<float>(v, EV_TAKEDAMAGE) == 0.0f || !F<int>(v, EV_SOLID))
            return;
        V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
        if (hi.z <= feet + 18.0f || lo.z >= feet + 72.0f)
            return; // under the feet (walked over) or above the head
        for (float s = 0; s <= reach; s += 8.0f)
        {
            V3 p = me + dir * s;
            if (p.x >= lo.x - 16.0f && p.x <= hi.x + 16.0f && p.y >= lo.y - 16.0f && p.y <= hi.y + 16.0f)
            {
                if (s < best)
                {
                    best = s;
                    found = e;
                }
                break;
            }
        }
    });
    return found;
}

// what a way costs more on Zombie Escape's meshes: jumping up (a car, a crate) and crouching, so flat ground round them wins
static std::vector<float> g_zeCost;
static const std::vector<float>* ZeRouteCost()
{
    if (g_zeCost.size() != g_nav.areas.size())
    {
        g_zeCost.assign(g_nav.areas.size(), 0.0f);
        for (size_t i = 0; i < g_nav.areas.size(); i++)
            g_zeCost[i] = ((g_nav.areas[i].attr & NAV_JUMP) ? 150.0f : 0.0f) + ((g_nav.areas[i].attr & NAV_CROUCH) ? 80.0f : 0.0f);
    }
    return &g_zeCost;
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

// a zombie within `range` (seen, or close enough to be heard): the barrier can wait
static bool ZeZombieClose(void* bot, V3 eye, float range)
{
    bool close = false;
    ForEntitiesNear(eye, range, [&](void* e) {
        if (close || e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || !ZeIsZombie(e))
            return;
        close = Len(Origin(e) - eye) < 250.0f || CanSee(bot, eye, e);
    });
    return close;
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

// the entity of a class at a .bsp model's place (its middle; the engine's sphere search goes by the middle too)
static void* ZeEntityAt(V3 lo, V3 hi, const char* cls, const char* cls2 = nullptr)
{
    V3 c = (lo + hi) * 0.5f;
    void* found = nullptr;
    ForEntitiesNear(c, 48.0f, [&](void* e) {
        const char* k = ClassName(e);
        if (found || (strcmp(k, cls) && (!cls2 || strcmp(k, cls2))))
            return;
        V3 a = Vec((float*)(Vars(e) + EV_ABSMIN)), b = Vec((float*)(Vars(e) + EV_ABSMAX));
        if (Len((a + b) * 0.5f - c) < 8.0f)
            found = e;
    });
    return found;
}

// a breakable to shoot because a wall or door it opens is close by and still shut; blockAt: that blocker's middle
static void* ZeTriggerNear(V3 me, V3& blockAt)
{
    float now = Now();
    for (auto& t : g_ze.triggers)
    {
        if (now < t.retryAt && t.retryAt - now < 10.0f)
            continue;
        const ZeBlock* blocker = nullptr;
        for (auto& b : t.blocks)
            if (ZeBoxDist(me, b.lo, b.hi) < ZE_TRIGGER_NEAR)
            {
                void* be = ZeEntityAt(b.lo, b.hi, b.cls.c_str());
                if (be && (b.cls != "func_wall_toggle" || F<int>(Vars(be), EV_SOLID)))
                {
                    blocker = &b;
                    break;
                }
            }
        if (!blocker)
            continue;
        void* e = ZeEntityAt(t.lo, t.hi, "ze_breakable", "func_breakable");
        if (!e || F<float>(Vars(e), EV_HEALTH) <= 0 || F<float>(Vars(e), EV_TAKEDAMAGE) == 0.0f)
        {
            t.retryAt = now + 5.0f;
            continue;
        }
        blockAt = (blocker->lo + blocker->hi) * 0.5f;
        if (!t.told)
        {
            t.told = true;
            V3 c = (t.lo + t.hi) * 0.5f;
            Log("ZE: humans shoot the %s at (%.0f %.0f %.0f): it opens the %s at (%.0f %.0f %.0f)", ClassName(e), c.x, c.y, c.z,
                blocker->cls.c_str(), blockAt.x, blockAt.y, blockAt.z);
        }
        return e;
    }
    return nullptr;
}

// no room to stand towards yaw but room crouching: a low passage
static bool ZeLowAhead(void* bot, V3 me, float yaw)
{
    if (!g_TraceHull)
        return false;
    float feet = me.z - ((F<int>(Vars(bot), EV_FLAGS) & 0x4000) /* FL_DUCKING */ ? 18.0f : 36.0f);
    V3 dir = YawDir(yaw) * 40.0f;
    TraceResult tr;
    float a[3] = { me.x, me.y, feet + 37.0f }, b[3] = { me.x + dir.x, me.y + dir.y, feet + 37.0f };
    memset(&tr, 0, sizeof(tr));
    g_TraceHull(a, b, 1, 1, bot, &tr); // standing
    if (!tr.fStartSolid && !tr.fAllSolid && tr.flFraction >= 0.99f)
        return false;
    float c[3] = { me.x, me.y, feet + 19.0f }, d[3] = { me.x + dir.x, me.y + dir.y, feet + 19.0f };
    memset(&tr, 0, sizeof(tr));
    g_TraceHull(c, d, 1, HULL_DUCK, bot, &tr); // crouching
    return !tr.fStartSolid && !tr.fAllSolid && tr.flFraction >= 0.99f;
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
            if (z.toldButton != button)
            {
                z.toldButton = button;
                V3 m = (button->lo + button->hi) * 0.5f;
                static int reports = 0;
                if (reports++ < 60)
                    Log("ZE: '%s' goes to press the %s at (%.0f %.0f %.0f), from (%.0f %.0f %.0f)",
                        String(F<int>(Vars(bot), EV_NETNAME)), button->cls.c_str(), m.x, m.y, m.z, me.x, me.y, me.z);
            }
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
                    Log("ZE: '%s' presses the %s at (%.0f %.0f %.0f)", String(F<int>(Vars(bot), EV_NETNAME)), button->cls.c_str(),
                        mid.x, mid.y, mid.z);
                return true;
            }
            goal = mid;
            goal.z = button->lo.z;
            arrive = 24.0f;
        }
    }

    V3 wp;
    g_navRouteCost = ZeRouteCost();
    bool there = !NextWaypoint(bot, st, me, goal, arrive, wp, true);
    bool pathed = there || st.path.size() > 1 || AreaAt(me) == AreaAt(goal);
    if (!pathed && !zombie && z.task == 0 && g_ze.leader && g_ze.leader != bot && Len(g_ze.leadPos - me) > 200.0f)
    {
        // no way on the mesh to the next box: after the one who got furthest
        z.task = 4;
        there = !NextWaypoint(bot, st, me, g_ze.leadPos, 120.0f, wp, true);
    }
    g_navRouteCost = nullptr;
    if (there)
    {
        fwd = side = 0;
        z.yawSet = false;
        st.lastProgress = now;
        return true;
    }
    int ride = RideFlagAhead(st.pathFlags, st.pathIndex);
    if (ride && (st.pathFlags[st.pathIndex] == ride || Len2D(wp - me) < 300.0f))
    {
        // a lift, a train or a teleport: NavTraverse (NavRideStep) gets it on and keeps it there
        z.task = 6;
        fwd = side = 0;
        st.lastProgress = now;
        return true;
    }
    float wayYaw = YawOf(wp - me);

    // a low passage: crouch through it
    if (ZeLowAhead(bot, me, wayYaw))
        st.duckUntil = now + 0.4f;

    // a wall or door in the way that a breakable elsewhere opens (Hitchhiking's pipe): shoot it, from where it can be seen
    if (!zombie && !ZeZombieInSight(bot, eye))
    {
        V3 blockAt;
        void* trigger = ZeTriggerNear(me, blockAt);
        if (trigger)
        {
            V3 d = Center(trigger) - eye;
            viewYaw = YawOf(d);
            viewPitch = PitchOf(d);
            setView = true;
            z.task = 7;
            st.lastProgress = now;
            st.lastPos = me;
            if (CanSee(bot, eye, trigger))
            {
                if (fmodf(now, 0.2f) < 0.1f)
                    buttons |= IN_ATTACK;
                fwd = side = 0;
                return true;
            }
            // hidden behind the wall it opens: back off until it shows over it
            V3 away = me - blockAt;
            away.z = 0;
            if (Len2D(away) > ZE_TRIGGER_BACK)
            {
                fwd = side = 0;
                return true;
            }
            float moveYaw = YawOf(away);
            ZConSafeYaw(bot, me, WalkYaw(bot, me, moveYaw, 40.0f), ZCON_FREE_DROP, moveYaw);
            float rel = (moveYaw - viewYaw) / RAD;
            fwd = cosf(rel) * 250.0f;
            side = -sinf(rel) * 250.0f;
            return true;
        }
    }

    int bodyFlags = F<int>(Vars(bot), EV_FLAGS);
    float feetZ = me.z - ((bodyFlags & 0x4000 /* FL_DUCKING */) ? 18.0f : 36.0f);
    // a barrier between it and where it goes: a low one (a rail, a fence) jumped over, else shot down (a human with no
    // zombie to shoot at)
    if (!zombie)
    {
        void* barrier = ZeBarrierAhead(bot, me, wp);
        if (!barrier && st.pathIndex < st.path.size() && Len2D(st.path[st.pathIndex] - wp) > 32.0f)
            barrier = ZeBarrierAhead(bot, me, st.path[st.pathIndex]); // backing off: the barrier is still where the way goes
        if (!barrier)
            z.barrierSince = 0;
        else if (z.barrierSince == 0)
            z.barrierSince = now;
        if (barrier)
        {
            V3 blo = Vec((float*)(Vars(barrier) + EV_ABSMIN)), bhi = Vec((float*)(Vars(barrier) + EV_ABSMAX));
            float over = bhi.z - feetZ;
            if (over > 0.0f && over <= 46.0f)
            {
                z.task = 2;
                st.lastProgress = now;
                st.lastPos = me;
                float rel = (wayYaw - viewYaw) / RAD;
                fwd = cosf(rel) * 250.0f;
                side = -sinf(rel) * 250.0f;
                if (ZeBoxDist(me, blo, bhi) < 40.0f && (bodyFlags & 0x200 /* FL_ONGROUND */) && now - z.jumpAt > 0.7f)
                {
                    z.jumpAt = now;
                    buttons |= IN_JUMP;
                }
                if (now - z.jumpAt > 0.1f && now - z.jumpAt < 0.5f)
                    buttons |= IN_DUCK;
                return true;
            }
        }
        bool heldLong = barrier && now - z.barrierSince > 2.5f;
        if (barrier && (!ZeZombieInSight(bot, eye) || !ZeZombieClose(bot, eye, ZE_BARRIER_SAFE) || heldLong))
        {
            static int reports = 0;
            if (reports < 40 && now - z.barrierSince < 0.05f)
            {
                reports++;
                V3 c = Center(barrier);
                Log("ZE: '%s' shoots the %s at (%.0f %.0f %.0f) in its way (health %.0f)", String(F<int>(Vars(bot), EV_NETNAME)),
                    ClassName(barrier), c.x, c.y, c.z, F<float>(Vars(barrier), EV_HEALTH));
            }
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

    // a ledge to jump onto (the waypoint up to a jump above the feet, close): straight at it. Steering round what the body
    // bumps (the ledge's own face, a rail on it) sent the jump sideways, off it (Hitchhiking's truck)
    bool climb = now >= st.navBackUntil && wp.z > feetZ + 14.0f && wp.z < feetZ + 64.0f && Len2D(wp - me) < 96.0f;
    float wantYaw = climb ? wayYaw : WalkYaw(bot, me, wayYaw, 40.0f);
    // in water (Hitchhiking's pool, below its edge): jump held swims up, and at the surface against the edge climbs out
    bool inWater = (bodyFlags & 0x10 /* FL_INWATER */) != 0;
    if (inWater)
    {
        buttons |= IN_JUMP;
        st.lastProgress = now;
        st.lastPos = me;
        wantYaw = wayYaw;
    }
    if (now < z.dodgeUntil && !climb)
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
        else if ((bodyFlags & 0x4000 /* FL_DUCKING */) || now < st.duckUntil)
        {
            // crouched in a low passage (Hitchhiking's diagonal duct, hardly wider than the body): turning round or jumping
            // throws it off its line; a slight turn to one side, then the other, slides it along the wall
            z.passSide = -z.passSide;
            z.dodgeYaw = wayYaw + 25.0f * z.passSide;
            z.dodgeUntil = now + 0.4f;
            st.duckUntil = now + 0.6f;
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
