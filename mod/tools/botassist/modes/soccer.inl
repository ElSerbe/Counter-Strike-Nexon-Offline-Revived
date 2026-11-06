// botassist: Soccer (mode 16; rules CSOGameRulesSoccer, vtable 0x11964f64, code 0x1143c720-0x11442000; ball and goals 0x114f6000-0x114fd000).
// Included by botassist.cpp (one translation unit). Only runs in its mode: DriveSoccerBot returns false unless GameMode() == MODE_SOCCER.
//
// The mode (https://cso.fandom.com/wiki/Soccer): two teams score with a ball; maps sc_soccer01 (Striker, up to 2 balls) and sc_cage. The
// client's create-room map list for mode 16 is MapList.csv's `map_soccer` column (GameUI table 0x109753c8: mode -> column), which only had
// vxl_empty: the server's MapList.csv now marks both maps.
// What the game does (the RE facts the bots use):
//   - balls: entity "soccerball" (CSoccerBall, vtable 0x1197d7d8). The rules keep two (+0x268, +0x26c); +0xd5 on (in play), +0xd0 the holder's
//     player index (0 none), +0xd1 the one before, +0xd2 the last to kick it, +0x158 when the holder is punished (99999 damage) for keeping it
//     (15 s, BallThink 0x114f7110). Touching a free ball takes it (BallTouch 0x114f7ba0 -> 0x114f8080); touching the holder's ball may steal
//     it, more likely with agility. Holding sets the player's +0x20ad bit 8.
//   - goals: "trgoal" (team 1's net) and "ctgoal" (team 2's), CSoccerGoalNet +0xb0 = the team it belongs to. The ball in a net scores for the
//     last kicker's team when that's the other team (GoalTouch 0x114f9090). A player touching the other team's net drops the ball and takes
//     damage each second. "trgoalpoint" / "ctgoalpoint" mark the nets too.
//   - keys: attack with the ball (rules 0x1143ead0) starts a charge (+0x20cc = now) when the stamina (+0x20b0, 0..20) allows a special shot,
//     else shoots at once; letting go (PlayerThink 0x1143f800) shoots: a normal shot under 1 s (0x1143ede0, power by strength +0x20ab), a
//     special shot after (0x114fabf0 power 2000, costs stamina). The shot goes along the view, flat. Secondary attack with the ball passes
//     to the teammate most in front (0x114f95a0, else a normal shot); without it, it calls for a pass. Attack without the ball is the hands'
//     hit (tackle, CKnifeHelperSoccer 0x110b6600), a rush when sprinting.
//   - commands (rules ClientCommand 0x1143d090): sc_01 / sc_02 sprint on / off (needs stamina > 1), sc_03 / sc_04 / sc_05 buy a level of
//     stamina (+0x20aa, more health) / strength (+0x20ab) / agility (+0x20ac) with the round's money, up to 10.
//   - rules +0x2c0: a goal was just scored (kick-off pause).
//
// The stock bot AI only knows how to fight, so DriveSoccerBot plays: per team one goalkeeper (with 3 or more players), the player nearest
// each ball goes for it (a human counts: no bot steals a teammate's run), the rest spread out ahead of the ball in attack and between the
// ball and their goal in defence. With the ball a bot dribbles to the other goal, dodging whoever is in front, passes when pressed and a
// teammate is ahead, and shoots from its range (a special shot now and then); it never keeps the ball near the 15 s penalty. Against a
// holder it tackles. It sprints for far balls and buys upgrades with its money.
static const int MODE_SOCCER = 16;
static const DWORD MP_SOCCER_RULES_VT = 0x11964f64;
static const int SC_BALL_HOLDER = 0xd0, SC_BALL_KICKER = 0xd2, SC_BALL_ON = 0xd5, SC_BALL_PUNISH_AT = 0x158;
static const int SC_TEAM = 0x6c, SC_PL_STAMINA = 0x20b0, SC_PL_FLAGS = 0x20ad, SC_PL_CHARGE = 0x20cc, SC_NET_TEAM = 0xb0;
static const int SC_RULES_GOAL_PAUSE = 0x2c0;
static const float SC_TACKLE = 70.0f;

struct ScBall
{
    void* e;
    V3 pos, vel;
    int holder;     // player index, 0 none
    int holderTeam; // 0 none
    float punishAt;
};
struct ScGoal
{
    bool ok = false;
    V3 center{}, aim{}, side{}; // side: across the goal mouth
    float width = 200.0f;
};
struct ScPlayer
{
    void* e;
    int index, team;
    V3 pos;
    bool bot;
};
static struct
{
    int level = -1;
    float next = 0;
    std::vector<ScBall> balls;
    std::vector<ScPlayer> players;
    ScGoal goal[3]; // by the team the net belongs to
    void* keeper[3] = {};
    bool pause = false;
    bool reported = false;
} g_sc;

struct ScBot
{
    bool init = false;
    float lastThink = 0, yaw = 0, pitch = 0;
    const char* doing = "";
    float nextReport = 0, nextUpgrade = 0, sprintAt = -100;
    bool sprint = false;
    int heldBall = 0;            // 1 while holding
    float heldSince = 0, shootRange = 600.0f, aimSide = 0;
    int kick = 0;                // 0 none, 1 attack held (shot), 2 attack2 (pass)
    float kickUntil = 0, kickAt = -100, tackleAt = -100, dodge = 0, dodgeUntil = 0;
};
static std::unordered_map<void*, ScBot> g_scBots;

static unsigned char* ScRules()
{
    unsigned char* r = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    return r && Readable(r, 0x300) && *(DWORD*)r == MP_SOCCER_RULES_VT + g_mpDelta ? r : nullptr;
}
static int ScTeam(void* e)
{
    unsigned char* p = (unsigned char*)PrivateOf(e);
    return p && Readable(p + SC_TEAM, 4) ? *(int*)(p + SC_TEAM) : 0;
}
static int ScIndex(void* e) { return g_IndexOfEdict ? g_IndexOfEdict(e) : 0; }
static float ScSkill(void* bot)
{
    BotState& st = StateFor(bot);
    MatchBot* rb = RoomBotOf(bot, st);
    Skill s = CurrentSkill(rb ? rb->skill : -1);
    float k = (0.5f - s.reaction) / 0.35f;
    return k < 0 ? 0 : k > 1 ? 1 : k;
}
static V3 ScBoxCenter(void* e)
{
    unsigned char* v = Vars(e);
    return (Vec((float*)(v + EV_ABSMIN)) + Vec((float*)(v + EV_ABSMAX))) * 0.5f;
}
// a point on the walkable field near p
static V3 ScOnField(V3 p)
{
    if (!g_nav.ok || g_nav.areas.empty())
        return p;
    int ai = AreaAt(p);
    if (ai < 0)
        return p;
    const NavArea& a = g_nav.areas[ai];
    if (p.x >= a.lo[0] && p.x <= a.hi[0] && p.y >= a.lo[1] && p.y <= a.hi[1])
        return V3{ p.x, p.y, a.center.z };
    return a.center;
}

static void ScScan(float now)
{
    if (g_sc.level != g_nav.level)
    {
        g_sc = {};
        g_sc.level = g_nav.level;
        g_scBots.clear();
    }
    if (now < g_sc.next && now > g_sc.next - 1.0f)
        return;
    g_sc.next = now + 0.1f;
    unsigned char* rules = ScRules();
    g_sc.pause = rules && rules[SC_RULES_GOAL_PAUSE];
    g_sc.balls.clear();
    g_sc.players.clear();
    V3 netBox[3][2] = {};
    bool net[3] = {}, point[3] = {};
    V3 pointAt[3] = {};
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* cls = ClassName(e);
        if (!cls)
            return;
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (!strcmp(cls, "soccerball"))
        {
            if (!p || !Readable(p, SC_BALL_PUNISH_AT + 4) || !p[SC_BALL_ON])
                return;
            ScBall b{};
            b.e = e;
            b.pos = Origin(e);
            b.vel = Vec((float*)(Vars(e) + EV_VELOCITY));
            b.holder = p[SC_BALL_HOLDER];
            b.punishAt = *(float*)(p + SC_BALL_PUNISH_AT);
            g_sc.balls.push_back(b);
        }
        else if (!strcmp(cls, "trgoal") || !strcmp(cls, "ctgoal"))
        {
            int team = p && Readable(p + SC_NET_TEAM, 4) ? *(int*)(p + SC_NET_TEAM) : 0;
            if (team != 1 && team != 2)
                team = cls[0] == 't' ? 1 : 2;
            net[team] = true;
            netBox[team][0] = Vec((float*)(Vars(e) + EV_ABSMIN));
            netBox[team][1] = Vec((float*)(Vars(e) + EV_ABSMAX));
        }
        else if (!strcmp(cls, "trgoalpoint") || !strcmp(cls, "ctgoalpoint"))
        {
            int team = cls[0] == 't' ? 1 : 2;
            point[team] = true;
            pointAt[team] = Origin(e);
        }
        else if (!strcmp(cls, "player") && Alive(e) && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
        {
            int team = ScTeam(e);
            if (team == 1 || team == 2)
                g_sc.players.push_back(ScPlayer{ e, ScIndex(e), team, Origin(e), (F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT) != 0 });
        }
    });
    for (auto& b : g_sc.balls)
        for (auto& pl : g_sc.players)
            if (pl.index == b.holder)
                b.holderTeam = pl.team;
    for (int t = 1; t <= 2; t++)
    {
        ScGoal& g = g_sc.goal[t];
        g.ok = net[t];
        if (!net[t])
            continue;
        g.center = (netBox[t][0] + netBox[t][1]) * 0.5f;
        g.aim = point[t] && Len2D(pointAt[t] - g.center) < 600.0f ? pointAt[t] : g.center;
    }
    if (g_sc.goal[1].ok && g_sc.goal[2].ok)
        for (int t = 1; t <= 2; t++)
        {
            ScGoal& g = g_sc.goal[t];
            V3 axis = g_sc.goal[3 - t].center - g.center;
            axis.z = 0;
            float len = Len2D(axis);
            if (len < 1)
                continue;
            axis = axis * (1.0f / len);
            g.side = V3{ -axis.y, axis.x, 0 };
            V3 size = netBox[t][1] - netBox[t][0];
            g.width = fabsf(size.x * g.side.x) + fabsf(size.y * g.side.y);
            if (g.width < 60.0f)
                g.width = 60.0f;
        }
    // goalkeepers: a bot each, the one nearest its goal, kept while it lives (3 or more on the team)
    for (int t = 1; t <= 2; t++)
    {
        int count = 0;
        bool kept = false;
        for (auto& pl : g_sc.players)
            if (pl.team == t)
            {
                count++;
                kept |= pl.e == g_sc.keeper[t];
            }
        if (count < 3 || !g_sc.goal[t].ok)
        {
            g_sc.keeper[t] = nullptr;
            continue;
        }
        if (kept)
            continue;
        float best = 1e30f;
        g_sc.keeper[t] = nullptr;
        for (auto& pl : g_sc.players)
            if (pl.team == t && pl.bot && Len2D(pl.pos - g_sc.goal[t].center) < best)
                best = Len2D(pl.pos - g_sc.goal[t].center), g_sc.keeper[t] = pl.e;
    }
    if (!g_sc.reported && !g_sc.balls.empty())
    {
        g_sc.reported = true;
        Log("Soccer: %d ball(s) in play, goals %s / %s (widths %.0f / %.0f), %d players", (int)g_sc.balls.size(), g_sc.goal[1].ok ? "T found" : "T MISSING",
            g_sc.goal[2].ok ? "CT found" : "CT MISSING", g_sc.goal[1].width, g_sc.goal[2].width, (int)g_sc.players.size());
    }
}

static const ScPlayer* ScPlayerOf(void* e)
{
    for (auto& pl : g_sc.players)
        if (pl.e == e)
            return &pl;
    return nullptr;
}
static const ScPlayer* ScByIndex(int index)
{
    for (auto& pl : g_sc.players)
        if (pl.index == index)
            return &pl;
    return nullptr;
}
// the player of a team nearest a point, the goalkeeper aside (nullptr: none)
static const ScPlayer* ScNearest(int team, V3 at, void* skip = nullptr)
{
    const ScPlayer* best = nullptr;
    float bestD = 1e30f;
    for (auto& pl : g_sc.players)
        if (pl.team == team && pl.e != g_sc.keeper[team] && pl.e != skip && Len2D(pl.pos - at) < bestD)
            bestD = Len2D(pl.pos - at), best = &pl;
    return best;
}

static void ScCommand(void* bot, const char* cmd) { FakeCommand(bot, cmd); }
static void ScSprint(void* bot, ScBot& z, bool on, float now)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (on && p && *(float*)(p + SC_PL_STAMINA) < 3.0f)
        on = false; // out of breath
    if (z.sprint == on || now - z.sprintAt < 0.5f)
        return;
    z.sprint = on;
    z.sprintAt = now;
    ScCommand(bot, on ? "sc_01" : "sc_02");
}

// ---------------------------------------------------------------- with the ball
static void ScWithBall(void* bot, ScBot& z, const ScBall& ball, int team, V3 me, float now, float skill, V3& goal, bool& hasGoal, float& speed,
                       V3& look, bool& hasLook, unsigned short& b)
{
    const ScGoal& their = g_sc.goal[3 - team];
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    float stamina = p ? *(float*)(p + SC_PL_STAMINA) : 0;
    if (!z.heldBall)
    {
        z.heldBall = 1;
        z.heldSince = now;
        z.shootRange = 450.0f + skill * 350.0f + Rnd() * 200.0f;
        z.aimSide = (Rnd() - 0.5f) * 0.6f; // where across the goal mouth
    }
    // a kick under way: keep facing where it goes until the key is let go
    if (z.kick)
    {
        hasLook = z.kick == 1; // (a pass went the moment the key was pressed)
        look = their.aim + their.side * (their.width * z.aimSide);
        if (z.kick == 1 && now < z.kickUntil)
            b |= IN_ATTACK;
        speed = 120.0f;
        goal = their.aim;
        hasGoal = true;
        z.doing = z.kick == 1 ? "shooting" : "passing";
        return;
    }
    if (!their.ok)
    {
        z.doing = "with the ball, no goal found";
        return;
    }
    V3 target = their.aim + their.side * (their.width * z.aimSide);
    V3 toGoal = target - me;
    float dist = Len2D(toGoal);
    float held = now - z.heldSince;
    float left = ball.punishAt - now;
    // pressed: the nearest opponent in front
    const ScPlayer* foe = nullptr;
    float foeD = 1e30f;
    V3 dir = toGoal * (1.0f / (dist > 1 ? dist : 1));
    for (auto& pl : g_sc.players)
    {
        if (pl.team == team)
            continue;
        V3 d = pl.pos - me;
        float l = Len2D(d);
        if (l < 260.0f && (d.x * dir.x + d.y * dir.y) / (l > 1 ? l : 1) > 0.3f && l < foeD)
            foeD = l, foe = &pl;
    }
    // a teammate ahead, free: the pass goes to the one most in front of the view
    const ScPlayer* mate = nullptr;
    float mateScore = 0;
    for (auto& pl : g_sc.players)
    {
        if (pl.team != team || pl.e == bot)
            continue;
        V3 d = pl.pos - me;
        float l = Len2D(d);
        float ahead = Len2D(target - me) - Len2D(target - pl.pos);
        if (l < 150.0f || l > 1100.0f || ahead < 150.0f)
            continue;
        float free = 1e9f;
        for (auto& o : g_sc.players)
            if (o.team != team && Len2D(o.pos - pl.pos) < free)
                free = Len2D(o.pos - pl.pos);
        if (free < 180.0f)
            continue;
        float score = ahead + free;
        if (score > mateScore)
            mateScore = score, mate = &pl;
    }
    bool shoot = dist < z.shootRange || left < 3.0f || held > 9.0f;
    bool pass = !shoot && mate && ((foe && foeD < 150.0f + skill * 60.0f) || (held > 6.0f && Rnd() < 0.02f));
    if (shoot || pass)
    {
        V3 at = shoot ? target : mate->pos;
        float off = fabsf(AngleDiff(YawOf(at - me), z.yaw));
        look = at;
        hasLook = true;
        goal = shoot ? target : me;
        hasGoal = shoot;
        speed = 100.0f;
        z.doing = shoot ? "turning to shoot" : "turning to pass";
        if (off < 6.0f + (1.0f - skill) * 6.0f && now - z.kickAt > 0.8f)
        {
            z.kickAt = now;
            if (shoot)
            {
                // a special shot (held over a second) when the stamina allows, now and then
                bool special = stamina > 16.0f && dist > 300.0f && Rnd() < 0.25f + skill * 0.2f;
                z.kick = 1;
                z.kickUntil = now + (special ? 1.15f : 0.05f);
                b |= IN_ATTACK;
            }
            else
            {
                z.kick = 2;
                z.kickUntil = now + 0.05f;
                b |= IN_ATTACK2;
            }
            static int reports = 0;
            if (reports++ < 60)
                Log("Soccer: bot '%s' %s from %.0f units (held %.1f s)", String(F<int>(Vars(bot), EV_NETNAME)),
                    shoot ? (z.kickUntil - now > 1.0f ? "takes a special shot" : "shoots") : "passes", shoot ? dist : Len2D(mate->pos - me), held);
        }
        return;
    }
    // dribble to the goal, around whoever stands in front
    if (foe && now >= z.dodgeUntil)
    {
        V3 d = foe->pos - me;
        float cross = dir.x * d.y - dir.y * d.x;
        z.dodge = cross > 0 ? -1.0f : 1.0f;
        z.dodgeUntil = now + 0.8f;
    }
    V3 aim = target;
    if (now < z.dodgeUntil)
    {
        V3 sideV{ -dir.y, dir.x, 0 };
        aim = me + dir * 200.0f + sideV * (z.dodge * 160.0f);
    }
    goal = aim;
    hasGoal = true;
    speed = 260.0f;
    look = aim;
    hasLook = true;
    z.doing = foe ? "dribbling past a defender" : "dribbling to the goal";
}

// ---------------------------------------------------------------- the bots count as players of their team
// The win check (0x1143cda0, 0x100 in the rules' table) counts each team's living players: those passing 0x114bd6a0 and not IsBot (vfunc
// 0x21c). A team of bots counted as empty and the other team won when the countdown ended. 0x114bd6a0 asks the engine for the player's room
// user (interface 0x121d3cf0, vfunc 100, pev +0x2d8), which a bot doesn't have; it also picks the pass target (0x114f95a0). In this mode a
// bot on team 1 or 2 now passes it the way a player with a room user does (+0xe10 not 1 nor 3), and the check's IsBot skip (jnz at
// 0x1143cfa7) is gone.
static const DWORD MP_SC_COUNTS = 0x114bd6a0, MP_SC_SKIP_BOTS = 0x1143cfa7;
static const int SC_PL_JOIN_STATE = 0xe10;
typedef int(__fastcall* ScCountsFn)(void* player, void* edx);
static ScCountsFn g_scCounts;
static bool g_scActive = false; // mode 16 with its rules, set each frame by ScFrame

static int ScBotCounts(unsigned char* pl)
{
    __try
    {
        unsigned char* pev = *(unsigned char**)(pl + 8);
        if (!pev)
            return 0;
        int flags = F<int>(pev, EV_FLAGS);
        int team = *(int*)(pl + SC_TEAM), join = *(int*)(pl + SC_PL_JOIN_STATE);
        return (flags & FL_FAKECLIENT) && flags >= 0 && (team == 1 || team == 2) && join != 1 && join != 3;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
static int __fastcall ScHookedCounts(void* pl, void* edx)
{
    int r = g_scCounts(pl, edx);
    if ((r & 0xff) || !g_scActive || !pl || !ScBotCounts((unsigned char*)pl))
        return r;
    static int reports = 0;
    if (reports++ < 3)
        Log("Soccer: a bot counted as a player of its team (the game's check said no)");
    return 1;
}
static void InstallScHooks(DWORD delta)
{
    unsigned char* fn = (unsigned char*)(MP_SC_COUNTS + delta);
    static const unsigned char code[] = { 0x56, 0x57, 0x8b, 0xf1, 0xff, 0x15 }; // push esi; push edi; mov esi, ecx; call [abs]
    if (!g_scCounts && Readable(fn, 10) && !memcmp(fn, code, sizeof(code)))
        g_scCounts = (ScCountsFn)DetourCdecl(fn, 10, (void*)ScHookedCounts);
    Log("Soccer: team count hook %s", g_scCounts ? "installed" : "NOT installed (unexpected code)");
    // and the win check itself skips bots (IsBot, vfunc 0x21c): its "jnz skip" goes. 0x1143cda0 is only CSOGameRulesSoccer's (slot 0x100).
    unsigned char* jnz = (unsigned char*)(MP_SC_SKIP_BOTS + delta);
    static const unsigned char before[] = { 0x85, 0xc0, 0x75, 0x4d }; // test eax, eax; jnz +0x4d
    if (Readable(jnz - 2, 4) && !memcmp(jnz - 2, before, sizeof(before)))
    {
        DWORD old;
        VirtualProtect(jnz, 2, PAGE_EXECUTE_READWRITE, &old);
        jnz[0] = 0x90;
        jnz[1] = 0x90;
        VirtualProtect(jnz, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), jnz, 2);
        Log("Soccer: the win check counts bots");
    }
    else if (!(Readable(jnz, 2) && jnz[0] == 0x90 && jnz[1] == 0x90))
        Log("Soccer: the win check NOT patched (unexpected code)");
}

// ---------------------------------------------------------------- every player: the hands and nothing else
// Soccer is played with the hands only (the knife, which the mode turns into CKnifeHelperSoccer), but room loadouts came on top, and with
// a gun in hand the attack key fires it. Anybody holding more loses it all and gets the knife back (ZtHandleSet / ZtStripToKnife, the same
// as in Zombie Tag: the slot layout and the functions are the player's, not the mode's).
static void ScFrame()
{
    g_scActive = g_mpDelta && GameMode() == MODE_SOCCER && g_edictToPrivate >= 0 && ScRules() != nullptr;
    if (!g_scActive)
        return;
    static float nextTry[65];
    float now = Now();
    int n = ZtMaxClients();
    for (int i = 1; i <= n && i <= 64; i++)
    {
        void* e = ZtClient(i);
        unsigned char* pl = e ? (unsigned char*)PrivateOf(e) : nullptr;
        if (!pl || !Alive(e) || !Readable(pl + ZT_PL_SLOTS, ZT_PL_KNIFE_TYPE + 4 - ZT_PL_SLOTS))
            continue;
        int other = -1;
        for (int slot = 0; slot < 6 && other < 0; slot++)
            if (slot != ZT_PL_KNIFE_SLOT && ZtHandleSet(pl, ZT_PL_SLOTS + slot * 16 + 4))
                other = slot;
        if (other < 0 || (now < nextTry[i] && now > nextTry[i] - 5.0f))
            continue;
        nextTry[i] = now + 0.5f;
        const char* m = String(F<int>(Vars(e), EV_WEAPONMODEL));
        std::string had = m && *m ? m : "nothing in hand";
        bool ok = ZtStripToKnife(pl, *(int*)(pl + ZT_PL_KNIFE_TYPE));
        if (ok && (F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT))
            FakeCommand(e, "weapon_knife");
        static int reports = 0;
        if (reports++ < 40)
            Log("Soccer: '%s' had a weapon in slot %d (%s): %s", String(F<int>(Vars(e), EV_NETNAME)), other, had.c_str(), ok ? "now only the hands" : "FAILED");
    }
}

// ---------------------------------------------------------------- every bot
static bool DriveSoccerBot(void* bot, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (GameMode() != MODE_SOCCER || !Alive(bot) || g_edictToPrivate < 0)
        return false;
    unsigned char* rules = ScRules();
    if (!rules)
        return false;
    float now = Now();
    ScScan(now);
    ScBot& z = g_scBots[bot];
    BotState& st = StateFor(bot);
    if (!z.init || now < z.lastThink - 1.0f || now - z.lastThink > 2.0f)
    {
        z = ScBot();
        z.init = true;
        z.yaw = F<float>(Vars(bot), EV_VANGLE + 4);
        z.nextUpgrade = now + 3.0f + Rnd() * 5.0f;
        st.path.clear();
    }
    float dt = now - z.lastThink;
    dt = dt <= 0 || dt > 0.25f ? 0.02f : dt;
    z.lastThink = now;
    const ScPlayer* self = ScPlayerOf(bot);
    int team = self ? self->team : ScTeam(bot);
    if (team != 1 && team != 2)
        return false;
    int index = ScIndex(bot);
    float skill = ScSkill(bot);
    V3 me = Origin(bot);
    V3 goal{}, look{};
    bool hasGoal = false, hasLook = false;
    float speed = 250.0f, arrive = 30.0f;
    unsigned short b = 0;
    const ScGoal& mine = g_sc.goal[team];
    const ScGoal& their = g_sc.goal[3 - team];

    // the ball it's about: the one it holds, else the nearest
    const ScBall* ball = nullptr;
    float ballD = 1e30f;
    for (auto& bl : g_sc.balls)
    {
        if (bl.holder == index)
        {
            ball = &bl;
            break;
        }
        if (Len2D(bl.pos - me) < ballD)
            ballD = Len2D(bl.pos - me), ball = &bl;
    }
    bool holding = ball && ball->holder == index;
    if (!holding)
    {
        z.heldBall = 0;
        if (z.kick && now >= z.kickUntil)
            z.kick = 0;
    }
    else if (z.kick && now >= z.kickUntil + 0.6f)
        z.kick = 0; // it didn't go: try again
    if (z.kick == 1 && now < z.kickUntil)
        b |= IN_ATTACK; // a charged shot: keep the key down

    // upgrades with the round's money, now and then
    if (now >= z.nextUpgrade)
    {
        z.nextUpgrade = now + 6.0f + Rnd() * 6.0f;
        static const char* ups[] = { "sc_05", "sc_04", "sc_03", "sc_05", "sc_04" };
        ScCommand(bot, ups[rand() % 5]);
    }

    bool keeper = g_sc.keeper[team] == bot && mine.ok;
    if (!ball || g_sc.balls.empty())
    {
        z.doing = "no ball in play";
        // back to its half
        if (mine.ok && their.ok)
        {
            goal = ScOnField(mine.center + (their.center - mine.center) * (keeper ? 0.05f : 0.3f));
            hasGoal = true;
            arrive = 120.0f;
        }
    }
    else if (holding)
        ScWithBall(bot, z, *ball, team, me, now, skill, goal, hasGoal, speed, look, hasLook, b);
    else
    {
        bool ours = ball->holderTeam == team, theirs = ball->holderTeam == 3 - team, free = !ball->holder;
        V3 predicted = ball->pos + ball->vel * (free ? 0.3f : 0.15f);
        const ScPlayer* nearest = ScNearest(team, predicted);
        const ScPlayer* second = ScNearest(team, predicted, nearest ? nearest->e : nullptr);
        bool chaser = nearest && nearest->e == bot;
        bool presser = theirs && second && second->e == bot;
        float toBall = Len2D(predicted - me);
        if (keeper)
        {
            // between the ball and the goal; out for it when it comes close and nobody of ours has it
            float ballToGoal = Len2D(ball->pos - mine.center);
            if (!ours && ballToGoal < 520.0f)
                chaser = true;
            else
            {
                V3 d = ball->pos - mine.aim;
                d.z = 0;
                float l = Len2D(d);
                float out = l * 0.25f < 170.0f ? l * 0.25f : 170.0f;
                goal = ScOnField(mine.aim + d * (out / (l > 1 ? l : 1)));
                hasGoal = true;
                arrive = 25.0f;
                speed = 220.0f;
                look = ball->pos;
                hasLook = true;
                z.doing = "keeping the goal";
            }
        }
        if (chaser || presser)
        {
            if (theirs)
            {
                const ScPlayer* holder = ScByIndex(ball->holder);
                V3 at = holder ? holder->pos : ball->pos;
                goal = at;
                hasGoal = true;
                arrive = 10.0f;
                look = at;
                hasLook = true;
                speed = 260.0f;
                z.doing = presser ? "pressing the ball's holder" : "tackling the ball's holder";
                if (Len2D(at - me) < SC_TACKLE && fabsf(AngleDiff(YawOf(at - me), z.yaw)) < 25.0f && now - z.tackleAt > 0.6f + (1.0f - skill) * 0.5f)
                {
                    b |= IN_ATTACK;
                    z.tackleAt = now;
                }
            }
            else if (free || !ours)
            {
                goal = predicted;
                hasGoal = true;
                arrive = 5.0f;
                look = predicted;
                hasLook = true;
                speed = 260.0f;
                z.doing = keeper ? "keeper: out for the ball" : "running for the ball";
            }
            ScSprint(bot, z, toBall > 450.0f && !keeper, now);
        }
        else
            ScSprint(bot, z, false, now);
        if (!hasGoal && mine.ok && their.ok)
        {
            // the others spread out: ahead of the ball in attack, between it and the goal in defence
            int slot = 0;
            for (auto& pl : g_sc.players)
                if (pl.team == team && pl.e != g_sc.keeper[team] && (!nearest || pl.e != nearest->e) && pl.index < index)
                    slot++;
            float lateral = ((slot % 2) ? -1.0f : 1.0f) * (1 + slot / 2) * 230.0f;
            V3 axis = their.center - mine.center;
            float fieldLen = Len2D(axis);
            axis.z = 0;
            axis = axis * (1.0f / (fieldLen > 1 ? fieldLen : 1));
            V3 sideV{ -axis.y, axis.x, 0 };
            V3 at;
            if (theirs)
            {
                // defending: on the way from the ball to our goal
                at = ball->pos + (mine.center - ball->pos) * (0.35f + 0.1f * (slot % 3));
                lateral *= 0.6f;
                z.doing = "defending";
            }
            else
            {
                at = ball->pos + axis * (300.0f + 120.0f * (slot % 3));
                z.doing = ours ? "running ahead for a pass" : "taking a place for the loose ball";
            }
            at = at + sideV * lateral;
            // not into the nets
            if (Len2D(at - their.center) < 260.0f)
                at = their.center + (at - their.center) * (260.0f / (Len2D(at - their.center) > 1 ? Len2D(at - their.center) : 1));
            goal = ScOnField(at);
            hasGoal = true;
            arrive = 60.0f;
            speed = 240.0f;
            look = ball->pos;
            hasLook = true;
            if (ours && Len2D(goal - me) > 600.0f)
                ScSprint(bot, z, true, now);
        }
    }
    if (holding)
        ScSprint(bot, z, false, now);

    // move and look (holding: the ball goes where it looks)
    float moveYaw = z.yaw;
    bool move = false;
    V3 wp;
    if (hasGoal && speed > 0 && Len2D(goal - me) > arrive)
    {
        if (NextWaypoint(bot, st, me, goal, arrive, wp))
            moveYaw = WalkYaw(bot, me, YawOf(wp - me), 40.0f);
        else
            moveYaw = YawOf(goal - me);
        move = true;
    }
    float lookYaw = hasLook ? YawOf(look - Eye(bot)) : move ? moveYaw : z.yaw;
    float lookPitch = holding ? -4.0f : hasLook ? PitchOf(look - Eye(bot)) : 0.0f;
    z.yaw = Approach(z.yaw, lookYaw, (360.0f + skill * 360.0f) * dt);
    float k = dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f;
    z.pitch += (lookPitch - z.pitch) * k;
    float f = 0, s = 0;
    if (move)
    {
        float rel = AngleDiff(moveYaw, z.yaw) / RAD;
        f = cosf(rel) * speed;
        s = -sinf(rel) * speed;
    }
    if (f > 20.0f)
        b |= IN_FORWARD;
    else if (f < -20.0f)
        b |= IN_BACK;
    if (s > 20.0f)
        b |= IN_MOVERIGHT;
    else if (s < -20.0f)
        b |= IN_MOVELEFT;
    yaw = AngleDiff(z.yaw, 0.0f);
    pitch = z.pitch < -89.0f ? -89.0f : z.pitch > 89.0f ? 89.0f : z.pitch;
    fwd = f;
    side = s;
    buttons = b;
    if (now >= z.nextReport)
    {
        z.nextReport = now + 10.0f;
        DLog("Soccer bot '%s' (team %d%s) at %.0f %.0f: %s; %d ball(s), ball %s", String(F<int>(Vars(bot), EV_NETNAME)), team, keeper ? ", keeper" : "",
             me.x, me.y, z.doing, (int)g_sc.balls.size(), !ball ? "none" : holding ? "held by it" : ball->holder ? (ball->holderTeam == team ? "ours" : "theirs") : "free");
    }
    return true;
}
