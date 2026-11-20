// ---------------------------------------------------------------- the scenario modes' orders menu
// The co-op modes against monsters (CoopModes: Zombie Scenario, Human Scenario, Zombie Disaster Zone...) had only the
// radio menus' orders for every bot at once. The C key (radio3) opens botassist's own menu there now (Zombie Shelter
// keeps its own menu; its chat words below work there too):
//   1. A bot follows me: the nearest bot not already with you gets "cover me" with you as its leader (each press, one
//      more); the others go on as they were
//   2. Shoot my target: what's under your crosshair (or the nearest monster within SO_FOCUS_CONE degrees of it) becomes
//      every bot's target while they can see it, until it dies or SO_FOCUS_TIME s pass
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
    float until = 0;
} g_soFocus;
static std::set<void*> g_soMenuOpen;

static bool SoMenuMode()
{
    return CoopMode() && GameMode() != 28; // (Zombie Shelter: its own orders menu)
}

// ScanMonsters: the players' focus target comes first for every bot that sees it
static void* FocusTarget(void* bot, V3 eye, void* best)
{
    void* f = g_soFocus.e;
    if (!f)
        return best;
    if (Now() > g_soFocus.until || Now() < g_soFocus.until - SO_FOCUS_TIME - 1.0f || !IsLivingMonster(f))
    {
        g_soFocus.e = nullptr;
        return best;
    }
    return Len(Center(f) - eye) < SO_FOCUS_RANGE && CanSee(bot, eye, f) ? f : best;
}

// what the player aims at: the entity under the crosshair, else the nearest living monster close to the aim
static void* SoAimedMonster(void* player)
{
    unsigned char* v = Vars(player);
    V3 eye = Eye(player), va = Vec((float*)(v + EV_VANGLE)), punch = Vec((float*)(v + EV_PUNCHANGLE));
    V3 dir = Forward(va.x + punch.x, va.y + punch.y);
    TraceResult tr;
    Trace(eye, eye + dir * 8192.0f, player, 0, tr);
    if (tr.pHit && IsLivingMonster(tr.pHit))
        return tr.pHit;
    void* best = nullptr;
    float bestAngle = SO_FOCUS_CONE;
    ForEntitiesNear(eye, SO_FOCUS_RANGE, [&](void* e) {
        if (!IsLivingMonster(e))
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
    return best;
}

static void SoFocus(void* player)
{
    void* e = SoAimedMonster(player);
    if (!e)
    {
        ShPrint(player, "Bots: no monster where you aim");
        return;
    }
    g_soFocus.e = e;
    g_soFocus.until = Now() + SO_FOCUS_TIME;
    for (auto& b : g_bots)
        b.nextScan = 0; // pick it now
    const char* cls = ClassName(e);
    char text[160];
    snprintf(text, sizeof(text), "Bots: shoot that %s!", cls && *cls ? cls : "monster");
    ShPrint(player, text);
    Log("scenario orders: '%s' marks %s (%.0f u away, %.0f health) as every bot's target",
        String(F<int>(Vars(player), EV_NETNAME)), cls ? cls : "?", Len(Origin(e) - Origin(player)), F<float>(Vars(e), EV_HEALTH));
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

// "Hunt freely" with nothing in sight: the nearest living monster of the whole map that walks (not the aircraft), so
// the bots go and find them (Human Scenario's soldiers stay far from the players; the bots stood still). Looked up
// every 2 s per bot.
static bool HuntGoal(void* bot, BotState& st, V3 pos, V3& goal)
{
    (void)bot;
    if (!CoopMode() || !g_nav.ok)
        return false;
    static std::unordered_map<int, std::pair<float, V3>> cache; // slot -> (next look, goal; x NAN: none)
    float now = Now();
    auto& c = cache[st.slot];
    if (now >= c.first || now < c.first - 3.0f)
    {
        c.first = now + 2.0f;
        float best = 1e18f;
        V3 found{ NAN, 0, 0 };
        ForEntitiesNear(pos, 16000.0f, [&](void* e) {
            if (!IsLivingMonster(e) || HsFlying(e) || HsIsMissile(e))
                return;
            float d = Len(Center(e) - pos);
            if (d < best)
                best = d, found = Origin(e);
        });
        c.second = found;
    }
    if (c.second.x != c.second.x) // (NAN: no monster left)
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
