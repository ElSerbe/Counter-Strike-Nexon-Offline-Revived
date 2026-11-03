// botassist: Beast (mode 27, "Snowman" in the game's files; rules CSOGameRulesSnowman, vtable 0x117a35d4). Included by botassist.cpp at
// this point (one translation unit: it uses the shared helpers defined before it). Only runs in its own mode: BeastWatch and
// BeastBotThink return, and DriveBeast isn't called, unless GameMode() == MODE_BEAST.
//
// The mode (https://cso.fandom.com/wiki/Beast): two teams, 3 s respawn. A player gets an ampoule at each respawn and from the bodies
// (entity "ampoule"); with 3, E turns it into a beast (500 HP, melee, stomp, double jump, double W rolls). On maps with a tower
// (sm_blizzard) a team wins by destroying the other's core, which a barrier protects until its controller goes down, for 20 s;
// on the others (DM_port, zm_requiem) it is a team deathmatch. Each player has a struct at [0x11e596a8][index - 1]: +4 is a beast, +0xc
// ampoules. Rules +0x260/+0x264 are the T/CT controllers and +0x268/+0x26c the cores (their team at pev +0x1d8, health at +0x178).
//
// The game has no AI for any of this (bots here are stock CCSBots, the old bot manager), so:
//   - Bot class: the mode's objects only take players of class 2 (Ampoule's touch 0x1084e8f0, the core's damage 0x1084d290) and a
//     CCSBot's class (vfunc 0x30) is 15, so bots stood on the crystals without getting them and couldn't hurt a core. In this mode
//     InstallBeastBotClass makes it 2, everywhere else it stays 15.
//   - Teams: the room lists its AI players on one side and T bots were refused. RoomBotQuota (shared, as in Super Soldier) evens them.
//   - BeastBotThink: a bot turns into a beast like a player's E does (rules vfunc 0x1f0, thiscall(player)), when its health is low, a
//     beast is near, or after waiting a while with its 3 ampoules, as the wiki advises.
//   - Weapons: a beast only has its claws. BeastIsBeast keeps it out of the room's weapons and its model (UpdateBuying, OnBotCommand, hook).
//   - DriveBeast moves them (GdmPickVia/GdmWalk, Gun Deathmatch's routes): humans pick up ampoules close by; a role per bot (attack,
//     hunt, defend) sends them to the enemy controller, then its core while the barrier is down, or to their own controller. They shoot
//     or punch objectives. Beasts run at enemies, punch, stomp, and roll (double W, as a player does, through the buttons).
//     The stock AI keeps aiming and shooting at enemies while they are humans.
static const DWORD MP_BEAST_RULES_VT = 0x117a35d4, MP_BEAST_PLAYERS = 0x11e596a8;
static const int BEAST_TR_CONTROLLER = 0x260, BEAST_CT_CONTROLLER = 0x264, BEAST_TR_CORE = 0x268, BEAST_CT_CORE = 0x26c, BEAST_TRANSFORM_VFUNC = 0x1f0;
static const float BEAST_THINK = 0.5f, BEAST_WAIT_MIN = 6.0f, BEAST_WAIT_MORE = 14.0f, BEAST_LOW_HEALTH = 60.0f, BEAST_NEAR = 600.0f;
static const float BEAST_AMPOULE_RANGE = 900.0f, BEAST_REACH = 110.0f, BEAST_STOMP_RANGE = 170.0f, BEAST_STOMP_EVERY = 4.0f, BEAST_ROLL_RANGE = 280.0f;
static const float BEAST_ROLL_EVERY = 7.0f, BEAST_ENGAGE = 800.0f, BEAST_SHOOT_RANGE = 700.0f, BEAST_MELEE_OBJECTIVE = 120.0f, BEAST_DEFEND_RANGE = 450.0f;
static const unsigned short BEAST_IN_FORWARD = 0x8, BEAST_IN_ATTACK2 = 0x800;

static unsigned char* BeastRules()
{
    if (!g_mpDelta || GameMode() != MODE_BEAST)
        return nullptr;
    unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    return rules && Readable(rules, 0x270) && *(DWORD*)rules == MP_BEAST_RULES_VT + g_mpDelta ? rules : nullptr;
}

static unsigned char* BeastPlayer(void* edict)
{
    if (!g_IndexOfEdict || !edict)
        return nullptr;
    int index = g_IndexOfEdict(edict);
    unsigned char** players = *(unsigned char***)(MP_BEAST_PLAYERS + g_mpDelta);
    if (index < 1 || index > 64 || !players || !Readable(players + index - 1, 4))
        return nullptr;
    unsigned char* s = players[index - 1];
    return s && Readable(s, 0x10) ? s : nullptr;
}

static bool BeastIsBeast(void* bot)
{
    if (!BeastRules())
        return false;
    unsigned char* s = BeastPlayer(bot);
    return s && s[4] != 0;
}

// CCSBot's vtable, slot of vfunc 0x30: its class
static const DWORD MP_CCSBOT_VT = 0x1193dcc8, MP_CCSBOT_CLASS = 0x107f8c40;
static int __fastcall BeastBotClass(void*)
{
    return GameMode() == MODE_BEAST ? 2 : 15;
}

static void InstallBeastBotClass(DWORD delta)
{
    void** slot = (void**)(MP_CCSBOT_VT + delta) + 0x30 / 4;
    if (!Readable(slot, 4) || *slot == (void*)BeastBotClass)
        return;
    if (*slot != (void*)(MP_CCSBOT_CLASS + delta))
    {
        Log("CCSBot vtable: unexpected class function, not hooked");
        return;
    }
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (void*)BeastBotClass;
    VirtualProtect(slot, 4, old, &old);
}

struct BeastBot
{
    float next = 0, threeAt = 0, waitFor = 0, stompAt = 0, rollAt = 0, roamAt = 0, lastAlive = 0, fireAt = 0;
    int rollStep = 0;
    V3 roam = { 0, 0, 0 };
    bool hasRoam = false;
};
static std::map<void*, BeastBot> g_beastBots;
static std::map<void*, GdmBot> g_beastWalk;

// An objective's edict, or null if it has none or is down
static void* BeastEntity(unsigned char* rules, int offset, bool mustBeUp)
{
    unsigned char* e = *(unsigned char**)(rules + offset);
    unsigned char* pev = e && Readable(e, 0x10) ? *(unsigned char**)(e + 8) : nullptr;
    if (!pev || !Readable(pev, 0x240) || (mustBeUp && *(float*)(pev + EV_HEALTH) <= 0.0f))
        return nullptr;
    return *(void**)(pev + EV_CONTAINING_ENTITY);
}

static V3 BeastCenter(void* e)
{
    unsigned char* v = Vars(e);
    return (Vec((float*)(v + EV_ABSMIN)) + Vec((float*)(v + EV_ABSMAX))) * 0.5f;
}

// how far from the box of an entity
static float BeastDistance(V3 me, void* e)
{
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    float dx = me.x < lo.x ? lo.x - me.x : me.x > hi.x ? me.x - hi.x : 0.0f;
    float dy = me.y < lo.y ? lo.y - me.y : me.y > hi.y ? me.y - hi.y : 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

// runs for every bot, in the frame of its move
static void BeastBotThink(void* bot)
{
    unsigned char* rules = BeastRules();
    if (!rules || !Alive(bot))
        return;
    BeastBot& b = g_beastBots[bot];
    float now = Now();
    if (now >= b.next - 5.0f && now < b.next)
        return;
    b.next = now + BEAST_THINK;
    unsigned char* s = BeastPlayer(bot);
    if (!s || s[4])
    {
        b.threeAt = 0;
        return;
    }
    if (*(int*)(s + 0xc) < 3)
    {
        b.threeAt = 0;
        return;
    }
    if (!b.threeAt)
    {
        b.threeAt = now;
        b.waitFor = BEAST_WAIT_MIN + Rnd() * BEAST_WAIT_MORE;
    }
    bool now_ = F<float>(Vars(bot), EV_HEALTH) < BEAST_LOW_HEALTH || now - b.threeAt > b.waitFor;
    if (!now_)
    {
        V3 me = Origin(bot);
        int team = TeamOf(bot);
        ForEntitiesNear(me, BEAST_NEAR, [&](void* e) {
            if ((F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) && Alive(e) && TeamOf(e) && TeamOf(e) != team)
            {
                unsigned char* other = BeastPlayer(e);
                now_ = now_ || (other && other[4]);
            }
        });
    }
    if (!now_)
        return;
    typedef void(__fastcall * TransformFn)(void* rules, void* edx, void* player);
    ((TransformFn)(*(void***)rules)[BEAST_TRANSFORM_VFUNC / 4])(rules, nullptr, PrivateOf(bot));
    b.threeAt = 0;
    static int reports = 0;
    if (reports++ < 60)
        Log("Beast: bot '%s' turns into a beast (health %.0f now)", String(F<int>(Vars(bot), EV_NETNAME)), F<float>(Vars(bot), EV_HEALTH));
}

static void BeastWatch()
{
    static float next = 0;
    unsigned char* rules = BeastRules();
    if (!rules || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0)
        return;
    float now = Now();
    if (now >= next - 60.0f && now < next)
        return;
    next = now + 15.0f;
    static int reports = 0;
    if (reports >= 100)
        return;
    reports++;
    int alive[3] = { 0, 0, 0 }, dead = 0, beasts = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            return;
        int team = TeamOf(e);
        if (team != 1 && team != 2)
            return;
        if (!Alive(e))
            dead++;
        else
        {
            alive[team]++;
            unsigned char* s = BeastPlayer(e);
            beasts += s && s[4] ? 1 : 0;
        }
    });
    auto hp = [&](int offset) {
        void* e = BeastEntity(rules, offset, false);
        return e ? F<float>(Vars(e), EV_HEALTH) : -1.0f;
    };
    Log("Beast: %d T and %d CT alive, %d dead, %d beasts; T controller %.0f, CT controller %.0f, T core %.0f, CT core %.0f", alive[1], alive[2],
        dead, beasts, hp(BEAST_TR_CONTROLLER), hp(BEAST_CT_CONTROLLER), hp(BEAST_TR_CORE), hp(BEAST_CT_CORE));
}

// Where a bot with nothing to fight goes, and what it hits there: by its role, on maps with a tower
static bool BeastObjective(void* bot, unsigned char* rules, int team, V3 me, BeastBot& b, float now, V3& goal, void*& hit, bool& stay)
{
    int enemyController = team == 1 ? BEAST_CT_CONTROLLER : BEAST_TR_CONTROLLER, enemyCore = team == 1 ? BEAST_CT_CORE : BEAST_TR_CORE;
    int ownController = team == 1 ? BEAST_TR_CONTROLLER : BEAST_CT_CONTROLLER;
    if (!*(void**)(rules + enemyCore))
        return false; // no tower here: team deathmatch
    int role = g_IndexOfEdict(bot) % 5; // 0 and 1 attack, 2 and 3 hunt... 4 defends; with the enemy core open all but the defender go
    void* controller = BeastEntity(rules, enemyController, true);
    void* core = BeastEntity(rules, enemyCore, true);
    if (role == 4)
    {
        void* own = BeastEntity(rules, ownController, false);
        if (!own)
            return false;
        if (!b.hasRoam || now >= b.roamAt)
        {
            b.hasRoam = true;
            b.roamAt = now + 8.0f + Rnd() * 10.0f;
            b.roam = PtnNavPoint(BeastCenter(own), BEAST_DEFEND_RANGE);
        }
        goal = b.roam;
        stay = Len2D(goal - me) < 120.0f;
        return true;
    }
    if (controller && role >= 2)
        return false;
    hit = controller ? controller : core;
    if (!hit)
        return false;
    goal = BeastCenter(hit);
    return true;
}

// y and p: where the bot looks (the stock AI's aim coming in); returns whether it drove it
static bool DriveBeast(void* bot, BotState& st, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (st.ordered && st.order != ORDER_INDEPENDENT)
        return false;
    unsigned char* rules = BeastRules();
    unsigned char* p = rules ? (unsigned char*)PrivateOf(bot) : nullptr;
    if (!p || !Readable(p, 0x2170) || !g_nav.ok)
        return false;
    GdmBot& z = g_beastWalk[bot];
    BeastBot& b = g_beastBots[bot];
    float now = Now();
    V3 me = Origin(bot), eye = Eye(bot);
    int team = *(int*)(p + 0x6c);
    z.team = team;
    if (F<float>(Vars(bot), EV_MAXSPEED) <= 1.0f || (F<int>(Vars(bot), EV_FLAGS) & FL_FROZEN))
    {
        st.lastPos = me;
        st.lastProgress = now;
        return false;
    }
    if (now - b.lastAlive > 1.0f)
    {
        z.hasVia = false;
        z.viaAt = now + (rand() % 3 ? 0.0f : 6.0f);
        b.hasRoam = false;
    }
    b.lastAlive = now;
    unsigned char* s = BeastPlayer(bot);
    bool beast = s && s[4];
    if (now >= z.retargetAt || !z.target)
    {
        z.retargetAt = now + GDM_RETARGET;
        GdmPickTarget(bot, z, me, eye, team);
    }
    if (z.target && (!(F<int>(Vars(z.target), EV_FLAGS) & FL_CLIENT) || !Alive(z.target)))
        z.target = nullptr;
    bool seen = false;
    float d = 1e9f;
    V3 at = me;
    if (z.target)
    {
        at = Origin(z.target);
        d = Len(at - me);
        seen = CanSee(bot, eye, z.target);
        if (seen)
            z.lastSeen = now;
    }
    V3 goal = at;
    bool go = z.target != nullptr, stay = false, roll = false, aimed = false;
    void* hit = nullptr;
    if (seen && d < BEAST_ENGAGE)
    {
        // fighting: a beast goes straight at it, a human keeps moving around it while the stock AI shoots
        if (beast)
        {
            roll = d > BEAST_ROLL_RANGE;
            yaw = YawOf(at - me);
            pitch = PitchOf(at - eye);
            aimed = true;
            if (d < BEAST_REACH)
            {
                buttons |= IN_ATTACK;
                stay = d < 60.0f;
            }
            if (d < BEAST_STOMP_RANGE && now >= b.stompAt)
            {
                b.stompAt = now + BEAST_STOMP_EVERY * (0.7f + Rnd() * 0.6f);
                buttons |= BEAST_IN_ATTACK2;
            }
        }
        else if (d < GDM_CLOSE)
        {
            if (now >= z.strafeAt)
            {
                z.strafeAt = now + GDM_STRAFE * (0.6f + Rnd() * 0.8f);
                z.strafe = rand() % 2 ? 1 : -1;
            }
            float around = YawOf(at - me) + z.strafe * (d < GDM_TOO_CLOSE ? 120.0f : 70.0f);
            goal = me + V3{ cosf(around / RAD), sinf(around / RAD), 0 } * 120.0f;
        }
    }
    else
    {
        // nothing to fight: ampoules close by (humans), then its role, then hunting by a route of its own
        V3 ampoule = me;
        float bestAmpoule = BEAST_AMPOULE_RANGE;
        if (!beast && s && *(int*)(s + 0xc) < 3)
            ForEntitiesNear(me, BEAST_AMPOULE_RANGE, [&](void* e) {
                const char* name = ClassName(e);
                float there = Len(Origin(e) - me);
                if (name && !strcmp(name, "ampoule") && there < bestAmpoule)
                {
                    bestAmpoule = there;
                    ampoule = Origin(e);
                }
            });
        if (bestAmpoule < BEAST_AMPOULE_RANGE)
        {
            goal = ampoule;
            go = true;
        }
        else if (BeastObjective(bot, rules, team, me, b, now, goal, hit, stay))
            go = true;
        else if (z.target)
        {
            if (!z.hasVia && now >= z.viaAt)
                GdmPickVia(bot, z, me, at);
            if (z.hasVia && (Len2D(z.via - me) < GDM_VIA_REACHED || now >= z.viaUntil))
            {
                z.hasVia = false;
                z.viaAt = now + 3.0f + Rnd() * 7.0f;
            }
            if (z.hasVia)
                goal = z.via;
            roll = beast;
        }
        // an objective: shoots it from afar, punches it up close
        if (hit)
        {
            float there = BeastDistance(me, hit);
            V3 center = BeastCenter(hit);
            TraceResult tr;
            bool clear = Trace(eye, center, bot, 1, tr) || tr.pHit == hit;
            if (clear && there < (beast ? BEAST_MELEE_OBJECTIVE : BEAST_SHOOT_RANGE))
            {
                yaw = YawOf(center - eye);
                pitch = PitchOf(center - eye);
                aimed = true;
                if (now >= b.fireAt)
                {
                    buttons |= IN_ATTACK;
                    if (now >= b.fireAt + 1.2f)
                        b.fireAt = now + 0.15f; // a short pause, so a gun gets reloaded
                }
                stay = there < (beast ? 70.0f : 450.0f);
            }
            else
                roll = beast;
        }
    }
    if (!go)
        return false;
    if (stay)
    {
        st.lastPos = me;
        st.lastProgress = now;
        fwd = side = 0;
        return true;
    }
    V3 wp;
    if (!NextWaypoint(bot, st, me, goal, 32.0f, wp))
        wp = goal;
    float wayYaw = YawOf(wp - me), maxDrop = ZCON_FREE_DROP;
    if (me.z - 36.0f - wp.z + 32.0f > maxDrop)
        maxDrop = min(me.z - 36.0f - wp.z + 32.0f, HURT_DROP_MAX);
    bool setView = false;
    float view = yaw;
    if (aimed)
        z.lastSeen = now; // it keeps looking at what it hits
    bool walked = GdmWalk(bot, st, z, me, now, wayYaw, wayYaw, maxDrop, view, setView, fwd, side, buttons);
    if (setView)
    {
        yaw = view;
        pitch = 0;
    }
    if (z.stuckNow && z.stuck >= 4)
    {
        z.hasVia = false;
        b.hasRoam = false;
        static int reports = 0;
        if (reports++ < 40)
            Log("Beast: '%s' stuck at %.0f %.0f %.0f going to %.0f %.0f %.0f", String(F<int>(Vars(bot), EV_NETNAME)), me.x, me.y, me.z, goal.x, goal.y, goal.z);
    }
    // a beast rolls as a player does, W twice: pressed, let go, pressed
    if (beast)
    {
        if (b.rollStep == 0 && roll && now >= b.rollAt && fwd > 200.0f)
        {
            b.rollStep = 1;
            b.rollAt = now + BEAST_ROLL_EVERY * (0.7f + Rnd() * 0.6f);
        }
        if (b.rollStep > 0)
        {
            int step = b.rollStep++;
            if (step >= 3 && step <= 6)
                buttons = (unsigned short)(buttons & ~BEAST_IN_FORWARD);
            else
                buttons |= BEAST_IN_FORWARD;
            if (step > 12)
                b.rollStep = 0;
        }
        else if (fwd > 200.0f)
            buttons |= BEAST_IN_FORWARD;
    }
    return walked;
}
