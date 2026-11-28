// ---------------------------------------------------------------- the scenario modes' orders menu
// The co-op modes against monsters (CoopModes: Zombie Scenario, Human Scenario, Zombie Disaster Zone...) had only the
// radio menus' orders for every bot at once. The C key (radio3) opens botassist's own menu there now (Zombie Shelter
// keeps its own menu; its chat words below work there too):
//   1. A bot follows me: the nearest bot not already with you gets "cover me" with you as its leader (each press, one
//      more); the others go on as they were
//   2. Shoot my target: what's under your crosshair (a monster, a boss, an objective or anything breakable; or the one
//      whose box the aim crosses before a wall; or the nearest within SO_FOCUS_CONE degrees of it) becomes every bot's
//      target while they can see it, until it dies or SO_FOCUS_TIME s pass (a boss: 2 minutes)
//   3. Hold position: every bot stays where it is (it still turns and shoots)
//   4. Follow me: every bot back to following
//   5. Hunt freely: every bot goes after the monsters
//   6. Aim lines on/off: the lasers that show where each bot aims and what it shoots (red firing, green with a target,
//      white none, yellow its aim point); off by default (botassist.ini AimLines=0)
//   7. Bring the bots to me: every living bot of your team is put next to you (bots left behind where they can't
//      climb or jump, and where no zombie comes any more, stayed there for good): free places around you that you
//      can see, in a ring, then the nav areas near you; mp's UTIL_SetOrigin (0x11519ec0), no speed left
// Chat: /follower, /focus (/target), /hold, /follow, /free, /lines, /bring (/tp).
static const float SO_FOCUS_TIME = 20.0f, SO_FOCUS_CONE = 6.0f, SO_FOCUS_RANGE = 6000.0f;
static struct
{
    void* e = nullptr;
    float until = 0, span = 20.0f;
} g_soFocus;
static std::set<void*> g_soMenuOpen;

static bool SoMenuMode()
{
    return CoopMode() && GameMode() != 28; // (Zombie Shelter: its own orders menu)
}

// what "shoot my target" can mark: a monster the bots fight, an objective they break (zbsbreak), or anything else
// breakable the players can damage (func_breakable...). Until 2026-11-28 only monsters: the objects in the way could
// never be marked (user's report)
static bool SoMarkable(void* e)
{
    if (!e)
        return false;
    unsigned char* v = Vars(e);
    int flags = F<int>(v, EV_FLAGS);
    if (flags & FL_CLIENT)
        return false;
    const char* cls = ClassName(e);
    if (IsLivingMonster(e))
        return !(cls && IsIgnored(cls));
    if (IsObjective(e))
        return true;
    if (F<float>(v, EV_HEALTH) <= 0 || F<float>(v, EV_TAKEDAMAGE) == 0.0f || !cls)
        return false;
    return !strncmp(cls, "func_breakable", 14) || !strncmp(cls, "func_pushable", 13) || !strcmp(cls, "zbsbreak");
}
static float SoMaxHealth(void* e)
{
    unsigned char* v = Vars(e);
    float m = F<float>(v, 0x1dc); // max_health
    return m > F<float>(v, EV_HEALTH) ? m : F<float>(v, EV_HEALTH);
}

// ScanMonsters: the players' focus target comes first for every bot that sees it
static void* FocusTarget(void* bot, V3 eye, void* best)
{
    void* f = g_soFocus.e;
    if (!f)
        return best;
    if (Now() > g_soFocus.until || Now() < g_soFocus.until - g_soFocus.span - 1.0f || !SoMarkable(f))
    {
        g_soFocus.e = nullptr;
        return best;
    }
    return Len(Center(f) - eye) < SO_FOCUS_RANGE && CanSee(bot, eye, f) ? f : best;
}

static bool SoIsFocus(void* e)
{
    return e && e == g_soFocus.e && Now() <= g_soFocus.until && SoMarkable(e);
}

// distance along the ray (from, unit dir) to where it enters the box, or -1 if it misses
static float SoRayBox(V3 from, V3 dir, V3 lo, V3 hi)
{
    float t0 = 0, t1 = 1e9f;
    float o[3] = { from.x, from.y, from.z }, d[3] = { dir.x, dir.y, dir.z }, a[3] = { lo.x, lo.y, lo.z }, b[3] = { hi.x, hi.y, hi.z };
    for (int i = 0; i < 3; i++)
    {
        if (fabsf(d[i]) < 1e-6f)
        {
            if (o[i] < a[i] || o[i] > b[i])
                return -1;
            continue;
        }
        float n = (a[i] - o[i]) / d[i], f = (b[i] - o[i]) / d[i];
        if (n > f)
            std::swap(n, f);
        t0 = n > t0 ? n : t0;
        t1 = f < t1 ? f : t1;
        if (t0 > t1)
            return -1;
    }
    return t0;
}

// what the player aims at: the entity under the crosshair; else one whose box the line of sight crosses before the
// first wall (a boss's parts or hitboxes can stop the trace short of the boss itself: the user couldn't mark the
// boss, 2026-11-28); else the nearest living monster close to the aim
static void* SoAimedMonster(void* player, std::string* why)
{
    unsigned char* v = Vars(player);
    V3 eye = Eye(player), va = Vec((float*)(v + EV_VANGLE)), punch = Vec((float*)(v + EV_PUNCHANGLE));
    V3 dir = Forward(va.x + punch.x, va.y + punch.y);
    TraceResult tr;
    Trace(eye, eye + dir * 8192.0f, player, 0, tr);
    if (tr.pHit && SoMarkable(tr.pHit))
        return tr.pHit;
    TraceResult wall;
    Trace(eye, eye + dir * 8192.0f, player, 1, wall); // (the world only)
    float wallDist = wall.flFraction * 8192.0f + 48.0f;
    void* best = nullptr;
    float bestT = 1e30f;
    ForEntitiesNear(eye, SO_FOCUS_RANGE, [&](void* e) {
        if (!SoMarkable(e))
            return;
        unsigned char* ev = Vars(e);
        V3 lo = Vec((float*)(ev + EV_ABSMIN)) - V3{ 8, 8, 8 }, hi = Vec((float*)(ev + EV_ABSMAX)) + V3{ 8, 8, 8 };
        float t = SoRayBox(eye, dir, lo, hi);
        if (t >= 0 && t < wallDist && t < bestT)
        {
            bestT = t;
            best = e;
        }
    });
    if (best)
        return best;
    float bestAngle = SO_FOCUS_CONE;
    ForEntitiesNear(eye, SO_FOCUS_RANGE, [&](void* e) {
        if (!SoMarkable(e))
            return;
        V3 to = Center(e) - eye;
        float l = Len(to);
        if (l < 1.0f)
            return;
        float dot = (to.x * dir.x + to.y * dir.y + to.z * dir.z) / l;
        float angle = acosf(dot > 1.0f ? 1.0f : dot < -1.0f ? -1.0f : dot) * RAD;
        if (angle < bestAngle && CanSee(player, eye, e))
        {
            bestAngle = angle;
            best = e;
        }
    });
    if (!best && why)
    {
        char b[200];
        if (tr.pHit && strcmp(ClassName(tr.pHit), "worldspawn"))
        {
            unsigned char* hv = Vars(tr.pHit);
            snprintf(b, sizeof(b), "the aim hits %s (flags 0x%x, health %.0f, takedamage %.0f) %.0f u away", ClassName(tr.pHit),
                     F<int>(hv, EV_FLAGS), F<float>(hv, EV_HEALTH), F<float>(hv, EV_TAKEDAMAGE), tr.flFraction * 8192.0f);
        }
        else
            snprintf(b, sizeof(b), "the aim hits the world %.0f u away", tr.flFraction * 8192.0f);
        *why = b;
    }
    return best;
}

static void SoFocus(void* player)
{
    std::string why;
    void* e = SoAimedMonster(player, &why);
    if (!e)
    {
        ShPrint(player, "Bots: nothing to shoot where you aim");
        Log("scenario orders: '%s' found nothing to mark: %s", String(F<int>(Vars(player), EV_NETNAME)), why.c_str());
        return;
    }
    // a boss stays marked until it dies (2 minutes at most), anything else 20 s
    float maxHp = SoMaxHealth(e);
    g_soFocus.e = e;
    g_soFocus.span = maxHp >= 100000.0f ? 120.0f : SO_FOCUS_TIME;
    g_soFocus.until = Now() + g_soFocus.span;
    for (auto& b : g_bots)
        b.nextScan = 0; // pick it now
    const char* cls = ClassName(e);
    const char* what = IsLivingMonster(e) ? (maxHp >= 100000.0f ? "boss" : "monster") : "object";
    char text[160];
    snprintf(text, sizeof(text), "Bots: shoot that %s!", what);
    ShPrint(player, text);
    Log("scenario orders: '%s' marks %s %s (%.0f u away, %.0f health) as every bot's target",
        String(F<int>(Vars(player), EV_NETNAME)), what, cls ? cls : "?", Len(Origin(e) - Origin(player)), F<float>(Vars(e), EV_HEALTH));
}

static void SoFollower(void* player)
{
    int team = TeamOf(player);
    V3 at = Origin(player);
    BotState* pick = nullptr;
    float bestD = 1e30f;
    int already = 0;
    for (auto& b : g_bots)
    {
        if (!b.edict || !(F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT) || !Alive(b.edict) ||
            (team && TeamOf(b.edict) != team))
            continue;
        if (b.ordered && b.order == ORDER_ESCORT && b.commander == player)
        {
            already++;
            continue;
        }
        float d = Len(Origin(b.edict) - at);
        if (d < bestD)
        {
            bestD = d;
            pick = &b;
        }
    }
    if (!pick)
    {
        ShPrint(player, already ? "Bots: every bot already follows you" : "Bots: no bot to follow you");
        return;
    }
    pick->order = ORDER_ESCORT;
    pick->ordered = true;
    pick->commander = player;
    pick->path.clear();
    const char* name = String(F<int>(Vars(pick->edict), EV_NETNAME));
    char text[160];
    snprintf(text, sizeof(text), "%s follows you (%d with you)", name ? name : "A bot", already + 1);
    ShPrint(player, text);
    Log("scenario orders: bot '%s' follows '%s' closely (%d with them)", name ? name : "?",
        String(F<int>(Vars(player), EV_NETNAME)), already + 1);
    if (Voices() != "off")
        FakeCommand(pick->edict, "roger");
}

static void SoAimLines(void* player)
{
    g_aimLines = !g_aimLines;
    ShPrint(player, g_aimLines ? "Bots' aim lines: on" : "Bots' aim lines: off");
    Log("scenario orders: aim lines %s", g_aimLines ? "on" : "off");
}

static const DWORD SO_UTIL_SETORIGIN = 0x11519ec0;
static const int HULL_HUMAN_SO = 1;

// a place for a body at `at` (player origin height): nothing solid there, and seen from `eye`
static bool SoFreeSpot(void* player, V3 eye, V3 at, const std::vector<V3>& taken)
{
    for (auto& t : taken)
        if (Len(t - at) < 40.0f)
            return false;
    if (!g_TraceHull)
        return false;
    TraceResult tr;
    float a[3] = { at.x, at.y, at.z };
    memset(&tr, 0, sizeof(tr));
    g_TraceHull(a, a, 0, HULL_HUMAN_SO, player, &tr);
    if (tr.fAllSolid || tr.fStartSolid)
        return false;
    TraceResult seen;
    return Trace(eye, at, player, 1, seen);
}

static bool SoSetOrigin(void* e, V3 at)
{
    float origin[3] = { at.x, at.y, at.z };
    __try
    {
        typedef void(__cdecl * SetOriginFn)(void* vars, float* origin);
        ((SetOriginFn)(SO_UTIL_SETORIGIN + g_mpDelta))(Vars(e), origin);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A bot stuck about 20 s in a row in a co-op mode (NavProgress: 12 times without leaving a 64-unit circle) is put on the
// nearest free walkable place 80 to 900 units away: a Zombie Shelter bot spawned on the shelter's roof (z 145, the
// others at 36) and stood there the whole match, "nothing in the way of its body" 37 times.
static bool CoopUnstick(void* bot, BotState& st, V3 pos)
{
    if (!g_nav.ok || !g_TraceHull)
        return false;
    int here = AreaAt(pos);
    float best = 1e9f;
    V3 to{};
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        if ((int)i == here || NavAreaHurts((int)i))
            continue;
        V3 at = g_nav.areas[i].center + V3{ 0, 0, 36 };
        float d = Len(at - pos);
        if (d < 80.0f || d > 900.0f || d >= best)
            continue;
        TraceResult tr;
        float a[3] = { at.x, at.y, at.z };
        memset(&tr, 0, sizeof(tr));
        g_TraceHull(a, a, 0, HULL_HUMAN_SO, bot, &tr);
        if (tr.fAllSolid || tr.fStartSolid)
            continue;
        best = d;
        to = at;
    }
    if (best > 1e8f || !SoSetOrigin(bot, to))
        return false;
    F<float>(Vars(bot), EV_VELOCITY) = F<float>(Vars(bot), EV_VELOCITY + 4) = F<float>(Vars(bot), EV_VELOCITY + 8) = 0;
    st.path.clear();
    st.navStuck = 0;
    st.navAnchor = to;
    st.navCrumbs.clear();
    static int reports = 0;
    if (reports++ < 40)
        Log("unstuck: bot %d '%s' stuck at (%.0f %.0f %.0f) for ~20 s: put at (%.0f %.0f %.0f), %.0f units away", st.slot,
            String(F<int>(Vars(bot), EV_NETNAME)), pos.x, pos.y, pos.z, to.x, to.y, to.z, best);
    return true;
}

// "Hunt freely" with nothing in sight: the nearest living monster that walks (not the aircraft or missiles) within
// HUNT_RANGE, so the bots go and find them (Human Scenario's soldiers stay far from the players; the bots stood still).
// One look at the monsters every 2 s for all the bots (not one per bot), and each bot keeps its goal 2 s, so its path
// isn't worked out again more often than when it follows a player.
static const float HUNT_RANGE = 6000.0f, HUNT_LOOK = 2.0f;
static bool HuntGoal(void* bot, BotState& st, V3 pos, V3& goal)
{
    (void)bot;
    if (!CoopMode() || !g_nav.ok)
        return false;
    float now = Now();
    static std::vector<V3> monsters;
    static float nextLook = 0;
    if (now >= nextLook || now < nextLook - HUNT_LOOK - 1.0f)
    {
        nextLook = now + HUNT_LOOK;
        monsters.clear();
        ForEntitiesNear(pos, 1e6f, [&](void* e) {
            if (IsLivingMonster(e) && !HsFlying(e) && !HsIsMissile(e))
                monsters.push_back(Origin(e));
        });
    }
    static std::unordered_map<int, std::pair<float, V3>> cache; // slot -> (next choice, goal; x NAN: none)
    auto& c = cache[st.slot];
    if (now >= c.first || now < c.first - HUNT_LOOK - 1.0f)
    {
        c.first = now + HUNT_LOOK;
        float best = HUNT_RANGE * HUNT_RANGE;
        V3 found{ NAN, 0, 0 };
        for (auto& m : monsters)
        {
            V3 d = m - pos;
            float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 < best)
                best = d2, found = m;
        }
        c.second = found;
    }
    if (c.second.x != c.second.x) // (NAN: none within range)
        return false;
    goal = c.second;
    return true;
}

static void SoBring(void* player)
{
    int team = TeamOf(player);
    V3 me = Origin(player), eye = Eye(player);
    float yaw = F<float>(Vars(player), EV_VANGLE + 4);
    std::vector<V3> spots;
    for (float r : { 70.0f, 120.0f, 170.0f, 230.0f })
        for (int k = 0; k < 12; k++)
        {
            float a = (yaw + 180.0f + k * 30.0f) / RAD; // behind you first
            V3 at = me + V3{ cosf(a) * r, sinf(a) * r, 0 };
            if (SoFreeSpot(player, eye, at, spots))
                spots.push_back(at);
        }
    if (g_nav.ok)
        for (auto& area : g_nav.areas)
        {
            V3 at = area.center + V3{ 0, 0, 36 };
            if (Len(at - me) < 500.0f && fabsf(at.z - me.z) < 100.0f && SoFreeSpot(player, eye, at, spots))
                spots.push_back(at);
        }
    int moved = 0, left = 0;
    size_t next = 0;
    for (auto& b : g_bots)
    {
        if (!b.edict || !(F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT) || !Alive(b.edict) ||
            (team && TeamOf(b.edict) != team))
            continue;
        if (Len(Origin(b.edict) - me) < 300.0f)
            continue; // already here
        if (next >= spots.size())
        {
            left++;
            continue;
        }
        V3 at = spots[next++];
        if (!SoSetOrigin(b.edict, at))
        {
            Log("scenario orders: bringing a bot failed");
            return;
        }
        F<float>(Vars(b.edict), EV_VELOCITY) = F<float>(Vars(b.edict), EV_VELOCITY + 4) = F<float>(Vars(b.edict), EV_VELOCITY + 8) = 0;
        b.path.clear();
        b.lastPos = at;
        b.buyingOrigin = at; // (not a respawn: UpdateBuying takes a jump this far for one)
        b.lastProgress = Now();
        b.hasRoam = false;
        if (b.order == ORDER_HOLD)
            b.holdSpot = at; // (holding: from here now)
        moved++;
    }
    char text[160];
    snprintf(text, sizeof(text), moved ? "Bots: %d brought to you%s" : "Bots: none to bring%s", moved,
             left ? " (no room for all of them here)" : "");
    ShPrint(player, text);
    Log("scenario orders: %d bots brought to '%s' (%d free places, %d without one)", moved,
        String(F<int>(Vars(player), EV_NETNAME)), (int)spots.size(), left);
}

static void SoShowMenu(void* player)
{
    char text[480];
    snprintf(text, sizeof(text),
             "Orders for the bots\n(now: %s)\n\n1. A bot follows me\n2. Shoot my target\n3. Hold position\n4. Follow me\n"
             "5. Hunt freely\n6. Aim lines: %s\n7. Bring the bots to me\n\n0. Close\n\nChat: /follower /focus /hold /follow /free /lines /bring",
             OrderName(g_teamOrder), g_aimLines ? "on" : "off");
    ShMenuText(player, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 9), text);
    g_soMenuOpen.insert(player);
}

// true: the command stops here (the menu took it)
static bool ScenarioPlayerCommand(void* player, const std::string& c)
{
    if (!CoopMode())
    {
        g_soMenuOpen.clear();
        return false;
    }
    if (c == "radio3" && SoMenuMode())
    {
        SoShowMenu(player);
        return true;
    }
    if (c == "menuselect" && g_soMenuOpen.count(player))
    {
        g_soMenuOpen.erase(player);
        const char* a = g_CmdArgv(1);
        int key = a ? atoi(a) : 0;
        if (key == 1)
            SoFollower(player);
        else if (key == 2)
            SoFocus(player);
        else if (key == 3)
            GiveOrder(ORDER_HOLD, player);
        else if (key == 4)
            GiveOrder(ORDER_FOLLOW, player);
        else if (key == 5)
            GiveOrder(ORDER_INDEPENDENT, player);
        else if (key == 6)
            SoAimLines(player);
        else if (key == 7)
            SoBring(player);
        return true;
    }
    if (c.compare(0, 5, "radio") == 0)
        g_soMenuOpen.erase(player);
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
        if (t == "follower" || t == "bodyguard" || t == "escort")
            SoFollower(player);
        else if (t == "focus" || t == "target" || t == "shoot")
            SoFocus(player);
        else if (t == "lines" || t == "aimlines")
            SoAimLines(player);
        else if (t == "bring" || t == "tp" || t == "teleport" || t == "comehere")
            SoBring(player);
        else if ((t == "orders" || t == "menu" || t == "bots") && SoMenuMode())
            SoShowMenu(player);
        // (/hold, /follow, /free: the shared chat orders)
    }
    return false;
}
