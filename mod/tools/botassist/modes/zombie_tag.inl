// botassist: Zombie Tag, "Zombie Hide and Seek" in CSN:Z (mode 37, rules CSOGameRulesMugoonhwa, vtable 0x1179e52c). Included by botassist.cpp
// (one translation unit). Only runs in its mode: everything here checks GameMode() == MODE_ZTAG (g_ztActive) first.
//
// The mode (https://cso.fandom.com/wiki/Zombie_Tag): half of the players become zombies (rules 0x10830460, CBasePlayer +0x3310 = 1, team 1) and
// hide among the map's AI zombies, "dummyman" (CDummyMan, spawned in a grid at 0x1082dd20; they walk to "dummygoal" points, some to the
// "playergoal"). A zombie wins by touching a playergoal (CPlayerGoal::GoalTouch -> 0x1082eba0). Humans (200 health) win by finding them.
// The fever (0x10830bc0): every few seconds a row of resource/ZombieTag/zombiefever (up to 3 actions, 3 s each, rules +0x234 + 4k) is told to
// everybody ("#CSO_ZombiTag_Fever_<action>") and all the AI zombies do it (0x1095e310 -> CDummyMan +0x12c4 = the action). The actions are
// resource/ZombieTag/ZombieTagAction: 0 nothing, 1 walk, 2 jump (hopping), 3 stop, 4 crouch, 5/6 spin left/right on the spot, 7/8 walk in
// circles, 9 walk backwards, 10..26 an emotion (hi .. ass attack). DanceThink 0x1095dab0 keeps them walking except for 0, 3, 4 and >= 10.
// The real zombies have to do the same to pass for them. Their 4 emotions are the rules' +0x680[4], the same for all.
// Commands, the same a player's keys send (ClientCommand 0x1082d960):
//   zbtag_motion 1..4  a zombie does emotion +0x680[n-1]; 5 is its smoke (once a round, rules +0x6a1 + index). Needs FL_ONGROUND.
//   zbtag_motion 1     a human's "I found the zombie": the closest zombie or AI zombie within *0x11e56a58 units, less than 26 degrees off the
//                      human's view and looking back at them (forward vectors' dot < -0.8). 1.8 s later (0x10831010) a real zombie dies,
//                      an AI zombie kills the human. One at a time: rules +0x6d4 + index * 4 is the pending time.
//   sprintON / sprintOFF  a zombie's sprint (rules +0x72f + index).
// A zombie's emotion freezes it (CBasePlayer +0x2f74, FL_FROZEN) and in this mode no key ends it (PreThink 0x114c5f20 skips zombies of mode
// 37): the bots end it with the game's own function, 0x114d60f0, when the AI zombies stop. A zombie's knife hits a human from behind and
// throws them (0x1082d5c0, no damage). Killing an AI zombie costs its killer health (0x1082e9f0).
//
// What the mode gave away and botassist evens out (every player, not only bots; all of it only in mode 37):
//   - the look: an AI zombie plays the model's zbs_idle1 / zbs_walk / crouch_idle (CDummyMan 0x1095dda0, the action table's ani_seq), a zombie
//     player the knife's ref_aim_knife over the legs' gait. ZtPlayerAnim, from a detour of CBasePlayer::SetAnimation 0x114cced0, gives a zombie
//     player the AI zombies' sequences (whole body, gaitsequence 0) when it stands, walks or jumps; attacks, flinches, emotions and death stay
//     the game's.
//   - the bumps: the AI zombies don't block anybody, a zombie player did. During each player's move (PlayerPreThink 0x113f6520 to
//     PlayerPostThink 0x113f6500, where the engine gathers what the move collides with) a zombie player doesn't block and isn't blocked by
//     another player; AddToFullPack 0x113e93b0 tells the clients the same (entity_state +0x3e solid = 0) so their prediction agrees. Traces
//     (knife, bullets) still hit it: the solids come back before PostThink runs the weapons.
//   - the weapons: humans get only a Glock (RemoveAllItems 0x114c8ac0, GiveNamedItem, GiveAmmo vtable +0x80), whatever the room loads.
//
// The stock bot AI knows the teams and nothing else, so it would beeline at the enemy. botassist plays both sides like a person:
//   zombie: walks with the crowd at the crowd's own speed, now and then by a dummygoal like they do, looks where it walks, does each fever action
//           after a human's reaction time (sometimes late, rarely the wrong emotion), mostly keeps calm when a human stares or shoots at it
//           (else smoke and sprint), sprints the last stretch when nobody is near, and now and then pokes a human from behind.
//   human: watches the zombies in sight and scores what gives a player away (running, not doing the fever's action, doing it late, jumping or
//          crouching out of turn, heading for the goal), shoots a suspect once to see how it reacts, and calls "I found the zombie" only when
//          sure enough and when it can (close, in front, looking back). It misses things and can be wrong, more so a weak one. It knows
//          nothing a player couldn't see.
static const int MODE_ZTAG = 37, ZT_FL_ONGROUND = 0x200, ZT_FL_DUCKING = 0x4000, ZT_SOLID_NOT = 0, ZT_SOLID_SLIDEBOX = 3;
static const DWORD MP_ZTAG_RULES_VT = 0x1179e52c, MP_ZTAG_END_EMOTION = 0x114d60f0, MP_ZTAG_FIND_RADIUS = 0x11e56a58;
static const DWORD MP_ZT_SETANIMATION = 0x114cced0, MP_ZT_PRETHINK = 0x113f6520, MP_ZT_POSTTHINK = 0x113f6500, MP_ZT_ADDTOFULLPACK = 0x113e93b0,
                   MP_ZT_LOOKUPSEQUENCE = 0x113d6170, MP_ZT_RESETSEQUENCE = 0x113d63f0, MP_ZT_REMOVEALLITEMS = 0x114c8ac0;
static const int ZT_IS_ZOMBIE = 0x3310, ZT_EMOTING = 0x2f74, ZT_DUMMY_STATE = 0x12c4;
static const int ZT_RULES_STARTED = 0x6b9, ZT_RULES_SPAWNED = 0x747, ZT_RULES_OVER = 0x754, ZT_RULES_EMOTIONS = 0x680, ZT_RULES_FIND = 0x6d4,
                 ZT_RULES_SMOKED = 0x6a1, ZT_RULES_SPRINT = 0x72f;
static const int ZT_EV_MODELINDEX = 0xc4, ZT_EV_SEQUENCE = 0x138, ZT_EV_GAITSEQUENCE = 0x13c, ZT_EV_FRAME = 0x140, ZT_ES_SOLID = 0x3e;
static const float ZT_FIND_CONE = 0.9f, ZT_FIND_FACING = -0.8f, ZT_FIND_RADIUS_DEFAULT = 150.0f, ZT_SIGHT = 1500.0f, ZT_FOV_COS = 0.6f;
static const float ZT_SUSPECT = 4.5f, ZT_SURE = 10.0f, ZT_GOAL_NEAR = 450.0f, ZT_STARE = 2.0f, ZT_GRACE = 1.6f;
// the fever's actions (ZombieTagAction)
enum
{
    ZT_ACT_NONE = 0, ZT_ACT_WALK = 1, ZT_ACT_JUMP = 2, ZT_ACT_STOP = 3, ZT_ACT_CROUCH = 4, ZT_ACT_SPIN_L = 5, ZT_ACT_SPIN_R = 6,
    ZT_ACT_CIRCLE_L = 7, ZT_ACT_CIRCLE_R = 8, ZT_ACT_BACK = 9, ZT_ACT_EMOTION = 10
};
static const char* ZtActName(int a)
{
    static const char* names[] = { "nothing", "walk", "jump", "stop", "crouch", "spin left", "spin right", "circle left", "circle right", "walk backwards" };
    return a >= 0 && a < ZT_ACT_EMOTION ? names[a] : "an emotion";
}
static bool ZtActStill(int a) { return a == ZT_ACT_NONE || a == ZT_ACT_STOP || a == ZT_ACT_CROUCH || a == ZT_ACT_SPIN_L || a == ZT_ACT_SPIN_R || a >= ZT_ACT_EMOTION; }

static bool g_ztActive = false; // mode 37 with its rules, set each frame by ZtFrame (read by the hooks below)

struct ZtActor
{
    void* e;
    bool player;   // (only for the bookkeeping: the human bots score what they see, never this)
    V3 pos, vel;
    int state;     // AI zombie: its action (1 walking); player: -1
    bool emoting, ducking, air;
    float health;
};
static struct
{
    int level = -1;
    float next = 0;
    std::vector<ZtActor> actors;
    std::vector<V3> playerGoals, dummyGoals;
    float crowdSpeed = 60.0f; // how fast the AI zombies walk
    bool fever = false;
    float feverSince = -100, feverEnded = -100;
    int action = ZT_ACT_WALK;  // what the AI zombies do now (the fever's current action, else walk)
    float actionSince = -100;
    bool reported = false;
} g_zt;

struct ZtSuspect
{
    float score = 0, bias = 0;
    float seenAt = -100, hitAt = -100, hitHealth = -1, airAt = -100;
    bool reacted = false;
};
struct ZtBot
{
    bool init = false;
    float lastThink = 0, yaw = 0, pitch = 0;
    const char* doing = "";
    float nextReport = 0;
    // zombie
    V3 via{};
    bool viaSet = false, viaDone = false;
    int goalIndex = -1;
    int act = ZT_ACT_WALK, curAct = ZT_ACT_WALK; // the action seen, the one it does (after its reaction)
    float actAt = 0, endAt = 0, jumpAt = 0;
    int emoteSlot = -1, emotedAct = -1;
    bool emoted = false;
    float stare = 0, lastHealth = -1, hurtAt = -100, panicUntil = 0, calmUntil = 0, pokeAt = -100;
    bool sprint = false, smoked = false;
    float sprintCmdAt = -100;
    float speedJitter = 1.0f;
    float spin = 0;          // degrees per second it turns by itself (spin / circle)
    bool ownMove = false;    // moves by fwd/side below, not along a path
    float ownFwd = 0;
    // human
    std::unordered_map<void*, ZtSuspect> sus;
    void* target = nullptr;
    float nextObserve = 0, nextLook = 0, shotAt = -100, findAt = -100;
    V3 watchSpot{};
    float watchUntil = 0;
    void* lookAt = nullptr;
};
static std::unordered_map<void*, ZtBot> g_ztBots;

static unsigned char* ZtRules()
{
    unsigned char* r = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    return r && Readable(r, 0x770) && *(DWORD*)r == MP_ZTAG_RULES_VT + g_mpDelta ? r : nullptr;
}
static bool ZtIsZombie(void* e)
{
    unsigned char* p = (unsigned char*)PrivateOf(e);
    return p && Readable(p + ZT_IS_ZOMBIE, 1) && p[ZT_IS_ZOMBIE];
}
static int ZtIndex(void* e) { return g_IndexOfEdict ? g_IndexOfEdict(e) : 0; }
static float ZtFindRadius()
{
    float** p = (float**)(MP_ZTAG_FIND_RADIUS + g_mpDelta);
    if (Readable(p, 4) && *p && Readable(*p, 4) && **p > 20.0f && **p < 1000.0f)
        return **p;
    return ZT_FIND_RADIUS_DEFAULT;
}
// 0 (worst) .. 1 (best) from the room's bot skill
static float ZtSkill(void* bot)
{
    BotState& st = StateFor(bot);
    MatchBot* rb = RoomBotOf(bot, st);
    Skill s = CurrentSkill(rb ? rb->skill : -1);
    float k = (0.5f - s.reaction) / 0.35f;
    return k < 0 ? 0 : k > 1 ? 1 : k;
}
// which way it faces: the game's checks read pev->angles (the model), not the view
static V3 ZtForward(void* e)
{
    float* angles = (float*)(Vars(e) + EV_ANGLES);
    return Forward(0, angles[1]);
}
static int ZtMaxClients() { return *(int*)((DWORD)*g_pGlobals + GV_MAXCLIENTS); }
static void* ZtClient(int i)
{
    auto entOf = (EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX];
    void* e = entOf(i);
    return e && PrivateOf(e) && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) ? e : nullptr;
}

static void ZtScan(float now)
{
    if (g_zt.level != g_nav.level)
    {
        g_zt = {};
        g_zt.level = g_nav.level;
        g_ztBots.clear();
    }
    if (now < g_zt.next && now > g_zt.next - 1.0f)
        return;
    g_zt.next = now + 0.1f;
    g_zt.actors.clear();
    g_zt.playerGoals.clear();
    g_zt.dummyGoals.clear();
    float speedSum = 0;
    int walkers = 0, acting = 0, dummies = 0;
    std::map<int, int> actions;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* cls = ClassName(e);
        if (!cls)
            return;
        unsigned char* v = Vars(e);
        if (!strcmp(cls, "playergoal"))
            g_zt.playerGoals.push_back(Center(e));
        else if (!strcmp(cls, "dummygoal"))
            g_zt.dummyGoals.push_back(Center(e));
        else if (!strcmp(cls, "dummyman") || (!strcmp(cls, "player") && ZtIsZombie(e)))
        {
            if (!Alive(e))
                return;
            ZtActor a{};
            a.e = e;
            a.player = cls[0] == 'p';
            a.pos = Origin(e);
            a.vel = Vec((float*)(v + EV_VELOCITY));
            a.health = F<float>(v, EV_HEALTH);
            int flags = F<int>(v, EV_FLAGS);
            a.air = !(flags & ZT_FL_ONGROUND);
            unsigned char* p = (unsigned char*)PrivateOf(e);
            if (!a.player)
            {
                a.state = p && Readable(p + ZT_DUMMY_STATE, 4) ? *(int*)(p + ZT_DUMMY_STATE) : ZT_ACT_WALK;
                a.emoting = a.state >= ZT_ACT_EMOTION;
                a.ducking = a.state == ZT_ACT_CROUCH;
                dummies++;
                if (a.state != ZT_ACT_WALK && a.state != ZT_ACT_NONE)
                {
                    acting++;
                    actions[a.state]++;
                }
                else if (Len2D(a.vel) > 20.0f)
                {
                    speedSum += Len2D(a.vel);
                    walkers++;
                }
            }
            else
            {
                a.state = -1;
                a.emoting = p && p[ZT_EMOTING];
                a.ducking = (flags & ZT_FL_DUCKING) != 0;
            }
            g_zt.actors.push_back(a);
        }
    });
    if (walkers >= 3)
        g_zt.crowdSpeed += (speedSum / walkers - g_zt.crowdSpeed) * 0.2f;
    bool fever = dummies >= 3 && acting * 2 >= dummies;
    int action = ZT_ACT_WALK;
    if (fever)
    {
        int best = 0;
        for (auto& kv : actions)
            if (kv.second > best)
                best = kv.second, action = kv.first;
    }
    if (fever && !g_zt.fever)
        g_zt.feverSince = now;
    if (!fever && g_zt.fever)
        g_zt.feverEnded = now;
    g_zt.fever = fever;
    if (action != g_zt.action)
    {
        g_zt.action = action;
        g_zt.actionSince = now;
        static int reports = 0;
        if (fever && reports++ < 60)
            Log("Zombie Tag: fever, the AI zombies %s (action %d, %d of %d)", action >= ZT_ACT_EMOTION ? "do an emotion" : ZtActName(action), action,
                actions[action], dummies);
    }
    if (!g_zt.reported && dummies > 0)
    {
        g_zt.reported = true;
        Log("Zombie Tag: %d AI zombies walking at %.0f u/s, %d player goals, %d dummy goals, find radius %.0f", dummies, g_zt.crowdSpeed,
            (int)g_zt.playerGoals.size(), (int)g_zt.dummyGoals.size(), ZtFindRadius());
    }
}

static int ZtNearestGoal(V3 me)
{
    int best = -1;
    float bestDist = 1e30f;
    for (size_t i = 0; i < g_zt.playerGoals.size(); i++)
        if (Len(g_zt.playerGoals[i] - me) < bestDist)
            bestDist = Len(g_zt.playerGoals[i] - me), best = (int)i;
    return best;
}

// a living human looking at this point (within `cone` degrees) from closer than `range`, in sight
static void* ZtHumanStaring(void* bot, V3 at, float range, float coneDeg)
{
    void* found = nullptr;
    ForEntitiesNear(at, range, [&](void* e) {
        if (found || e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || ZtIsZombie(e))
            return;
        V3 eye = Eye(e), d = at - eye;
        float len = Len(d);
        if (len < 1)
            return;
        float* va = (float*)(Vars(e) + EV_VANGLE);
        V3 f = Forward(va[0], va[1]);
        if ((f.x * d.x + f.y * d.y + f.z * d.z) / len < cosf(coneDeg / RAD))
            return;
        TraceResult tr;
        if (Trace(eye, at, e, 0, tr) || tr.pHit == bot)
            found = e;
    });
    return found;
}

static void ZtCommand(void* bot, const char* cmd, const char* arg = nullptr)
{
    FakeCommand(bot, cmd, arg);
}

static void ZtSetSprint(void* bot, ZtBot& z, bool on, float now)
{
    if (z.sprint == on || now - z.sprintCmdAt < 0.5f)
        return;
    z.sprint = on;
    z.sprintCmdAt = now;
    ZtCommand(bot, on ? "sprintON" : "sprintOFF");
}

static void ZtEndEmotion(void* bot)
{
    void* p = PrivateOf(bot);
    if (!p)
        return;
    __try
    {
        typedef void(__thiscall * EndFn)(void*, char);
        ((EndFn)(MP_ZTAG_END_EMOTION + g_mpDelta))(p, 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---------------------------------------------------------------- every player of the mode: the look, the bumps, the weapons
typedef void(__fastcall* ZtSetAnimationFn)(void* player, void* edx, int anim);
typedef int(__fastcall* ZtLookupSequenceFn)(void* entity, void* edx, const char* name);
typedef void(__fastcall* ZtResetSequenceFn)(void* entity, void* edx);
typedef void(__fastcall* ZtRemoveAllItemsFn)(void* player, void* edx, int removeSuit);
typedef int(__fastcall* ZtGiveAmmoFn)(void* player, void* edx, int amount, const char* name, int max);
typedef void (*ZtThinkFn)(void* edict);
typedef int (*ZtAddToFullPackFn)(void* state, int e, void* ent, void* host, int hostFlags, int player, unsigned char* set);
static ZtSetAnimationFn g_ztSetAnimation;
static ZtThinkFn g_ztPreThink, g_ztPostThink;
static ZtAddToFullPackFn g_ztAddToFullPack;

struct ZtSequences
{
    int idle = -1, walk = -1, run = -1, crouch = -1, crouchRun = -1;
};
static std::map<int, ZtSequences> g_ztSequences; // by model index

// a zombie player standing, walking or jumping: the AI zombies' sequences, whole body (true: done, the game's SetAnimation isn't run)
static bool ZtPlayerAnim(unsigned char* pl, int anim)
{
    // PLAYER_IDLE 0, PLAYER_WALK 1, PLAYER_JUMP 2, PLAYER_SUPERJUMP 3; the rest (attacks, flinches, death, ...) stays the game's
    if (anim < 0 || anim > 3 || !pl[ZT_IS_ZOMBIE] || pl[ZT_EMOTING])
        return false;
    unsigned char* pev = *(unsigned char**)(pl + 8);
    if (!pev || F<float>(pev, EV_HEALTH) <= 0.0f || F<float>(pev, EV_DEADFLAG) != 0.0f)
        return false;
    int model = *(short*)(pev + ZT_EV_MODELINDEX);
    auto it = g_ztSequences.find(model);
    if (it == g_ztSequences.end())
    {
        auto look = (ZtLookupSequenceFn)(MP_ZT_LOOKUPSEQUENCE + g_mpDelta);
        ZtSequences s;
        s.idle = look(pl, nullptr, "zbs_idle1");
        s.walk = look(pl, nullptr, "zbs_walk");
        s.run = look(pl, nullptr, "zbs_run");
        s.crouch = look(pl, nullptr, "crouch_idle");
        s.crouchRun = look(pl, nullptr, "zbs_crouchrun");
        it = g_ztSequences.emplace(model, s).first;
        Log("Zombie Tag: model %d: AI zombie sequences idle %d, walk %d, run %d, crouch %d %d", model, s.idle, s.walk, s.run, s.crouch, s.crouchRun);
    }
    const ZtSequences& s = it->second;
    if (s.idle < 0 || s.walk < 0)
        return false; // not the zombies' model
    int flags = F<int>(pev, EV_FLAGS);
    float speed = Len2D(Vec((float*)(pev + EV_VELOCITY)));
    int seq;
    if (flags & ZT_FL_DUCKING)
        seq = speed > 5.0f && s.crouchRun >= 0 ? s.crouchRun : s.crouch;
    else if (!(flags & ZT_FL_ONGROUND))
        seq = s.idle; // the AI zombies' jump plays zbs_idle1 (ZombieTagAction's ani_seq)
    else if (speed < 5.0f)
        seq = s.idle;
    else
        seq = speed > 170.0f && s.run >= 0 ? s.run : s.walk;
    if (seq < 0)
        seq = s.idle;
    if (*(int*)(pev + ZT_EV_SEQUENCE) != seq)
    {
        *(int*)(pev + ZT_EV_SEQUENCE) = seq;
        *(float*)(pev + ZT_EV_FRAME) = 0.0f;
        ((ZtResetSequenceFn)(MP_ZT_RESETSEQUENCE + g_mpDelta))(pl, nullptr);
    }
    *(int*)(pev + ZT_EV_GAITSEQUENCE) = 0;
    return true;
}

static void __fastcall ZtHookedSetAnimation(void* pl, void* edx, int anim)
{
    if (g_ztActive && pl)
    {
        bool done = false;
        __try
        {
            done = ZtPlayerAnim((unsigned char*)pl, anim);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            done = false;
        }
        if (done)
            return;
    }
    g_ztSetAnimation(pl, edx, anim);
}

// the players made non-solid for the move being run
static void* g_ztUnsolid[64];
static int g_ztUnsolidCount = 0;

static void ZtRestoreSolids()
{
    for (int i = 0; i < g_ztUnsolidCount; i++)
        if (F<int>(Vars(g_ztUnsolid[i]), EV_SOLID) == ZT_SOLID_NOT)
            F<int>(Vars(g_ztUnsolid[i]), EV_SOLID) = ZT_SOLID_SLIDEBOX;
    g_ztUnsolidCount = 0;
}

static void ZtHookedPreThink(void* e)
{
    g_ztPreThink(e);
    __try
    {
        ZtRestoreSolids();
        if (!g_ztActive || !e || !PrivateOf(e) || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            return;
        // the engine gathers what this move collides with after PlayerPreThink: a zombie player blocks nobody and nobody blocks it
        bool zombie = ZtIsZombie(e);
        int n = ZtMaxClients();
        for (int i = 1; i <= n && i <= 64 && g_ztUnsolidCount < 64; i++)
        {
            void* o = ZtClient(i);
            if (!o || o == e || !Alive(o) || F<int>(Vars(o), EV_SOLID) != ZT_SOLID_SLIDEBOX || !(zombie || ZtIsZombie(o)))
                continue;
            F<int>(Vars(o), EV_SOLID) = ZT_SOLID_NOT;
            g_ztUnsolid[g_ztUnsolidCount++] = o;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_ztUnsolidCount = 0;
    }
}

static void ZtHookedPostThink(void* e)
{
    __try
    {
        ZtRestoreSolids(); // before the weapons: the knife and the bullets hit them as before
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_ztUnsolidCount = 0;
    }
    g_ztPostThink(e);
}

static int ZtHookedAddToFullPack(void* state, int e, void* ent, void* host, int hostFlags, int player, unsigned char* set)
{
    int sent = g_ztAddToFullPack(state, e, ent, host, hostFlags, player, set);
    if (sent && player && g_ztActive && ent != host)
    {
        __try
        {
            // the client predicts its own move against the other players: it has to agree with the server
            if (ZtIsZombie(ent) || ZtIsZombie(host))
                *(short*)((unsigned char*)state + ZT_ES_SOLID) = ZT_SOLID_NOT;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    return sent;
}

static void InstallZtHooks(DWORD delta)
{
    struct Site
    {
        DWORD at;
        unsigned char code[6];
        void* hook;
        void** tramp;
        const char* name;
    } sites[] = {
        { MP_ZT_SETANIMATION, { 0x53, 0x8b, 0xdc, 0x83, 0xec, 0x08 }, (void*)ZtHookedSetAnimation, (void**)&g_ztSetAnimation, "SetAnimation" },
        { MP_ZT_PRETHINK, { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08 }, (void*)ZtHookedPreThink, (void**)&g_ztPreThink, "PlayerPreThink" },
        { MP_ZT_POSTTHINK, { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08 }, (void*)ZtHookedPostThink, (void**)&g_ztPostThink, "PlayerPostThink" },
        { MP_ZT_ADDTOFULLPACK, { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x10 }, (void*)ZtHookedAddToFullPack, (void**)&g_ztAddToFullPack, "AddToFullPack" },
    };
    std::string done;
    for (auto& s : sites)
    {
        if (*s.tramp)
            continue;
        unsigned char* fn = (unsigned char*)(s.at + delta);
        if (Readable(fn, 6) && !memcmp(fn, s.code, 6))
            *s.tramp = DetourCdecl(fn, 6, s.hook);
        done += std::string(done.empty() ? "" : ", ") + s.name + (*s.tramp ? "" : " NOT hooked (unexpected code)");
    }
    Log("Zombie Tag: player hooks: %s", done.c_str());
}

// humans: a Glock and nothing else
static void ZtEquipHumans(float now)
{
    static float nextTry[65];
    static float emptySince[65];
    int n = ZtMaxClients();
    for (int i = 1; i <= n && i <= 64; i++)
    {
        void* e = ZtClient(i);
        if (!e || !Alive(e) || ZtIsZombie(e))
        {
            emptySince[i] = 0;
            continue;
        }
        const char* m = String(F<int>(Vars(e), EV_WEAPONMODEL));
        if (m && strstr(m, "glock18"))
        {
            emptySince[i] = 0;
            continue;
        }
        if (!m || !*m)
        {
            // switching, or nothing at all: give it a moment
            if (!emptySince[i])
                emptySince[i] = now;
            if (now - emptySince[i] < 1.0f)
                continue;
        }
        if (now < nextTry[i] && now > nextTry[i] - 5.0f)
            continue;
        nextTry[i] = now + 1.0f;
        void* pl = PrivateOf(e);
        bool ok = false;
        __try
        {
            ((ZtRemoveAllItemsFn)(MP_ZT_REMOVEALLITEMS + g_mpDelta))(pl, nullptr, 0);
            ((GiveNamedItemFn)(MP_GIVENAMEDITEM + g_mpDelta))(pl, nullptr, "weapon_glock18", 1);
            ZtGiveAmmoFn giveAmmo = (ZtGiveAmmoFn)(*(void***)pl)[0x80 / 4];
            giveAmmo(pl, nullptr, 120, "9mm", 120);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (ok && (F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT))
            FakeCommand(e, "weapon_glock18");
        static int reports = 0;
        if (reports++ < 40)
            Log("Zombie Tag: human '%s' had %s: %s", String(F<int>(Vars(e), EV_NETNAME)), m && *m ? m : "nothing", ok ? "now only a Glock" : "FAILED");
    }
}

// every frame (HookedStartFrame)
static void ZtFrame()
{
    bool active = g_mpDelta && GameMode() == MODE_ZTAG && g_edictToPrivate >= 0 && ZtRules() != nullptr;
    if (!active)
    {
        if (g_ztActive)
            ZtRestoreSolids();
        g_ztActive = false;
        return;
    }
    g_ztActive = true;
    ZtRestoreSolids();
    ZtEquipHumans(Now());
}

// ---------------------------------------------------------------- zombie
static void ZtZombieThink(void* bot, ZtBot& z, unsigned char* rules, V3 me, float now, float dt, V3& goal, bool& hasGoal, float& speed,
                          void*& face, unsigned short& b)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    float skill = ZtSkill(bot);
    int index = ZtIndex(bot);
    bool emoting = p && p[ZT_EMOTING];
    int flags = F<int>(Vars(bot), EV_FLAGS);
    float health = F<float>(Vars(bot), EV_HEALTH);
    if (z.lastHealth >= 0 && health < z.lastHealth - 0.5f)
        z.hurtAt = now;
    z.lastHealth = health;
    if (index > 0 && index < 33)
        z.smoked = rules[ZT_RULES_SMOKED + index] != 0;
    z.spin = 0;
    z.ownMove = false;

    // ---- the fever: do what the AI zombies do, after a person's reaction (slower and less sure for a weak player)
    if (g_zt.action != z.act)
    {
        z.act = g_zt.action;
        z.actAt = now + 0.3f + Rnd() * (1.0f - skill * 0.6f) + (Rnd() < 0.12f - skill * 0.08f ? 0.8f + Rnd() * 1.2f : 0.0f);
        z.emoted = false;
        z.emoteSlot = -1;
    }
    if (now >= z.actAt)
        z.curAct = z.act;
    int act = now < z.panicUntil ? ZT_ACT_WALK : z.curAct;
    if (emoting)
    {
        speed = 0;
        z.doing = "doing the fever's emotion";
        if (act != z.emotedAct)
        {
            // the others stopped: so does it, a moment later
            if (!z.endAt)
                z.endAt = now + 0.1f + Rnd() * (0.6f - skill * 0.4f);
            if (now >= z.endAt)
            {
                ZtEndEmotion(bot);
                z.endAt = 0;
            }
        }
        return;
    }
    z.endAt = 0;
    if (act >= ZT_ACT_EMOTION)
    {
        speed = 0;
        z.doing = "stopped with the crowd for the fever";
        if (!z.emoted)
        {
            if (z.emoteSlot < 0)
            {
                for (int i = 0; i < 4; i++)
                    if (*(int*)(rules + ZT_RULES_EMOTIONS + i * 4) == act)
                        z.emoteSlot = i;
                if (z.emoteSlot >= 0 && Rnd() < 0.05f - skill * 0.04f)
                    z.emoteSlot = rand() % 4; // the wrong key
            }
            if (z.emoteSlot < 0)
                z.emoted = true; // not one of its 4: it can only stand still
            else if (flags & ZT_FL_ONGROUND)
            {
                char n[4];
                snprintf(n, sizeof(n), "%d", z.emoteSlot + 1);
                ZtCommand(bot, "zbtag_motion", n);
                z.emoted = true;
                z.emotedAct = act;
                static int reports = 0;
                if (reports++ < 60)
                    Log("Zombie Tag: zombie '%s' does emotion %d (the crowd: %d) %.1f s into it", String(F<int>(Vars(bot), EV_NETNAME)),
                        *(int*)(rules + ZT_RULES_EMOTIONS + z.emoteSlot * 4), act, now - g_zt.actionSince);
            }
        }
        return;
    }
    switch (act)
    {
    case ZT_ACT_NONE:
    case ZT_ACT_STOP:
        speed = 0;
        z.doing = "standing still with the crowd";
        return;
    case ZT_ACT_CROUCH:
        speed = 0;
        b |= IN_DUCK;
        z.doing = "crouching with the crowd";
        return;
    case ZT_ACT_SPIN_L:
    case ZT_ACT_SPIN_R:
        speed = 0;
        z.spin = act == ZT_ACT_SPIN_L ? 110.0f : -110.0f;
        z.doing = "spinning with the crowd";
        return;
    case ZT_ACT_CIRCLE_L:
    case ZT_ACT_CIRCLE_R:
        z.spin = act == ZT_ACT_CIRCLE_L ? 70.0f : -70.0f;
        z.ownMove = true;
        z.ownFwd = g_zt.crowdSpeed * z.speedJitter;
        z.doing = "walking in circles with the crowd";
        return;
    case ZT_ACT_BACK:
        z.ownMove = true;
        z.ownFwd = -g_zt.crowdSpeed * z.speedJitter;
        z.doing = "walking backwards with the crowd";
        return;
    case ZT_ACT_JUMP:
        if ((flags & ZT_FL_ONGROUND) && now >= z.jumpAt)
        {
            b |= IN_JUMP;
            z.jumpAt = now + 0.15f + Rnd() * 0.35f;
        }
        break; // and walks on, like them
    default:
        break;
    }

    // ---- humans around: a stare or a shot
    void* starer = ZtHumanStaring(bot, Center(bot), 600.0f, 12.0f);
    z.stare = starer ? z.stare + dt : (z.stare - dt * 0.5f > 0 ? z.stare - dt * 0.5f : 0);
    bool hit = now - z.hurtAt < 0.4f;
    if ((hit || z.stare > ZT_STARE * (0.6f + skill)) && now > z.panicUntil && now > z.calmUntil)
    {
        // a good player keeps playing the part, a nervous one runs
        float calm = hit ? 0.5f + skill * 0.35f : 0.65f + skill * 0.3f;
        if (Rnd() < calm)
        {
            z.calmUntil = now + 4.0f + Rnd() * 4.0f;
            z.stare = 0;
        }
        else
        {
            z.panicUntil = now + 3.0f + Rnd() * 3.0f;
            if (!z.smoked && Rnd() < 0.8f)
            {
                ZtCommand(bot, "zbtag_motion", "5");
                z.smoked = true;
            }
            static int reports = 0;
            if (reports++ < 60)
                Log("Zombie Tag: zombie '%s' %s: it runs", String(F<int>(Vars(bot), EV_NETNAME)), hit ? "was shot" : "is being watched");
        }
    }

    // ---- where to: like the crowd, now and then by a dummygoal, then the player goal
    if (z.goalIndex < 0 || z.goalIndex >= (int)g_zt.playerGoals.size())
        z.goalIndex = g_zt.playerGoals.empty() ? -1 : rand() % (int)g_zt.playerGoals.size();
    if (!z.viaSet)
    {
        z.viaSet = true;
        z.speedJitter = 0.94f + Rnd() * 0.1f;
        if (!g_zt.dummyGoals.empty() && Rnd() < 0.55f)
            z.via = g_zt.dummyGoals[rand() % g_zt.dummyGoals.size()];
        else
            z.viaDone = true;
    }
    if (!z.viaDone && Len2D(z.via - me) < 160.0f)
        z.viaDone = true;
    V3 target = z.viaDone && z.goalIndex >= 0 ? g_zt.playerGoals[z.goalIndex] : z.via;
    if (z.goalIndex < 0 && z.viaDone)
    {
        z.doing = "no goal on this map: walking with the crowd";
        speed = g_zt.crowdSpeed * z.speedJitter;
        return;
    }
    goal = target;
    hasGoal = true;
    speed = g_zt.crowdSpeed * z.speedJitter;
    z.doing = act == ZT_ACT_JUMP ? "hopping along with the crowd" : z.viaDone ? "walking to the goal with the crowd" : "walking by a dummy goal like the crowd";
    float toGoal = z.goalIndex >= 0 ? Len2D(g_zt.playerGoals[z.goalIndex] - me) : 1e9f;
    bool humanNear = ZtHumanStaring(bot, Center(bot), 380.0f, 180.0f) != nullptr;
    bool dash = z.viaDone && toGoal < ZT_GOAL_NEAR && !humanNear && !g_zt.fever;
    if (now < z.panicUntil || dash)
    {
        if (now < z.panicUntil && z.goalIndex >= 0)
            goal = g_zt.playerGoals[z.goalIndex], z.viaDone = true; // busted anyway: for the goal
        ZtSetSprint(bot, z, true, now);
        speed = 400.0f;
        z.doing = dash ? "sprinting the last stretch" : "busted: sprinting away";
    }
    else
        ZtSetSprint(bot, z, false, now);

    // ---- a human right ahead with their back turned: a poke throws them away (rarely; it gives it away)
    if (now - z.pokeAt > 8.0f && now > z.calmUntil && !g_zt.fever)
        ForEntitiesNear(me, 75.0f, [&](void* e) {
            if (face || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || ZtIsZombie(e))
                return;
            V3 f = ZtForward(e), d = me - Origin(e);
            float len = Len2D(d);
            if (len < 1 || (f.x * d.x + f.y * d.y) / len > -0.3f)
                return; // it would see the poke coming
            bool inTheWay = z.goalIndex >= 0 && Len2D(g_zt.playerGoals[z.goalIndex] - Origin(e)) < toGoal;
            if ((inTheWay && Rnd() < 0.3f) || Rnd() < 0.03f)
            {
                face = e;
                z.pokeAt = now;
                b |= IN_ATTACK;
                z.doing = "poking a human from behind";
            }
            else
                z.pokeAt = now - 4.0f; // not this time
        });
}

// ---------------------------------------------------------------- human
// what a player can see of a zombie: how it moves, whether it does what the others do
static void ZtObserve(void* bot, ZtBot& z, V3 eye, float now, float skill)
{
    if (now < z.nextObserve && now > z.nextObserve - 1.0f)
        return;
    float dt = now - (z.nextObserve - 0.1f);
    dt = dt <= 0 || dt > 0.5f ? 0.1f : dt;
    z.nextObserve = now + 0.1f;
    V3 f = Forward(0, z.yaw);
    int traces = 0;
    float miss = 0.35f - skill * 0.25f; // what slips by
    bool judging = now - g_zt.actionSince > ZT_GRACE; // everybody takes a moment to react
    int act = g_zt.action;
    for (auto& a : g_zt.actors)
    {
        V3 d = a.pos - eye;
        float len = Len(d);
        if (len > ZT_SIGHT || len < 1)
            continue;
        if ((f.x * d.x + f.y * d.y) / Len2D(d) < ZT_FOV_COS)
            continue;
        ZtSuspect& s = z.sus[a.e];
        if (s.seenAt < -50)
            s.bias = (Rnd() - 0.5f) * (2.5f - skill * 1.5f); // everyone has their hunches
        if (now - s.seenAt > 0.3f)
        {
            if (traces++ > 12)
                continue;
            TraceResult tr;
            if (!Trace(eye, Center(a.e), bot, 0, tr) && tr.pHit != a.e)
                continue;
        }
        s.seenAt = now;
        if (a.air)
            s.airAt = now;
        if (Rnd() < miss)
            continue;
        float speed = Len2D(a.vel), add = 0;
        if (g_zt.fever)
        {
            if (judging)
            {
                if (act >= ZT_ACT_EMOTION)
                    add += (a.emoting ? 0.0f : 3.0f) + (speed > 40.0f ? 2.0f : 0.0f);
                else if (act == ZT_ACT_CROUCH)
                    add += (a.ducking ? 0.0f : 3.0f) + (speed > 40.0f ? 2.0f : 0.0f);
                else if (ZtActStill(act))
                    add += (speed > 40.0f ? 3.0f : 0.0f) + (a.emoting ? 2.0f : 0.0f) + (a.ducking ? 1.5f : 0.0f);
                else if (act == ZT_ACT_JUMP)
                    add += (now - (s.airAt > g_zt.actionSince ? s.airAt : g_zt.actionSince) > 1.2f ? 2.5f : 0.0f) + (a.emoting ? 2.0f : 0.0f);
                else // walking in circles, backwards
                    add += (speed < 10.0f ? 2.0f : 0.0f) + (a.emoting ? 2.0f : 0.0f);
            }
        }
        else
        {
            if (a.emoting)
                add += now - g_zt.feverEnded < 4.0f ? 1.2f : 3.0f; // still dancing after the others, or dancing alone
            if (a.air && now - g_zt.feverEnded > 2.0f)
                add += 1.5f; // the AI zombies only jump when told
            if (a.ducking && now - g_zt.feverEnded > 2.0f)
                add += 1.5f;
            if (speed > g_zt.crowdSpeed * 1.4f + 20.0f)
                add += 3.5f; // the AI zombies never run
            else if (speed < 10.0f && !a.emoting && now - g_zt.feverEnded > 3.0f)
                add += 0.25f;
            int gi = ZtNearestGoal(a.pos);
            if (gi >= 0 && speed > 20.0f)
            {
                V3 to = g_zt.playerGoals[gi] - a.pos;
                float dist = Len2D(to);
                if (dist < 900.0f && (to.x * a.vel.x + to.y * a.vel.y) / (dist * speed) > 0.9f)
                    add += 0.35f * (1.0f - dist / 900.0f) * 2.0f;
            }
        }
        // how it took the shot: an AI zombie doesn't care
        if (now - s.hitAt < 2.5f && !s.reacted && speed > g_zt.crowdSpeed * 1.5f + 20.0f)
        {
            s.reacted = true;
            s.score += 5.0f;
        }
        // the farther, the less it notices
        float closeK = 1.2f - len / 1200.0f;
        closeK = closeK < 0.25f ? 0.25f : closeK > 1.0f ? 1.0f : closeK;
        s.score += add * dt * (0.8f + skill * 0.6f) * closeK;
    }
    for (auto it = z.sus.begin(); it != z.sus.end();)
    {
        it->second.score *= expf(-dt / 18.0f);
        bool alive = false;
        for (auto& a : g_zt.actors)
            alive |= a.e == it->first;
        it = alive ? ++it : z.sus.erase(it);
    }
}

static const ZtActor* ZtActorOf(void* e)
{
    for (auto& a : g_zt.actors)
        if (a.e == e)
            return &a;
    return nullptr;
}

static void ZtHumanThink(void* bot, ZtBot& z, unsigned char* rules, V3 me, V3 eye, float now, float dt, V3& goal, bool& hasGoal, float& arrive,
                         float& speed, void*& face, unsigned short& b)
{
    float skill = ZtSkill(bot);
    int index = ZtIndex(bot);
    ZtObserve(bot, z, eye, now, skill);
    speed = 250.0f;
    // the most suspicious one seen lately
    void* best = nullptr;
    float bestScore = ZT_SUSPECT;
    for (auto& kv : z.sus)
        if (now - kv.second.seenAt < 4.0f && kv.second.score + kv.second.bias > bestScore)
            bestScore = kv.second.score + kv.second.bias, best = kv.first;
    if (z.target != best && best)
    {
        static int reports = 0;
        if (reports++ < 80)
            Log("Zombie Tag: human '%s' suspects %s '%s' (score %.1f)", String(F<int>(Vars(bot), EV_NETNAME)), ZtActorOf(best) && ZtActorOf(best)->player ? "player" : "AI zombie",
                ZtActorOf(best) && ZtActorOf(best)->player ? String(F<int>(Vars(best), EV_NETNAME)) : "", bestScore);
    }
    z.target = best;
    const ZtActor* t = best ? ZtActorOf(best) : nullptr;
    if (t)
    {
        ZtSuspect& s = z.sus[best];
        float dist = Len2D(t->pos - me);
        float speedT = Len2D(t->vel);
        face = best;
        bool running = speedT > g_zt.crowdSpeed * 1.5f + 20.0f;
        // stand in front of it: "I found the zombie" needs it looking back
        V3 tf = ZtForward(best);
        goal = running ? t->pos : t->pos + tf * 60.0f;
        hasGoal = true;
        arrive = 25.0f;
        z.doing = running ? "chasing a zombie that runs" : "going to look at a suspect";
        float sure = ZT_SURE - (1.0f - skill) * 3.0f; // a weak player is sure sooner (and wrong more often)
        bool findFree = index > 0 && index < 33 && *(float*)(rules + ZT_RULES_FIND + index * 4) == 0.0f;
        if (dist < ZtFindRadius() && s.score + s.bias >= sure && findFree && now - z.findAt > 3.0f)
        {
            V3 d = t->pos - me;
            V3 myF = Forward(0, z.yaw);
            float inFront = (myF.x * d.x + myF.y * d.y) / (Len2D(d) > 1 ? Len2D(d) : 1);
            float facing = myF.x * tf.x + myF.y * tf.y;
            if (inFront > ZT_FIND_CONE + 0.02f && facing < ZT_FIND_FACING)
            {
                ZtCommand(bot, "zbtag_motion", "1");
                z.findAt = now;
                Log("Zombie Tag: human '%s' calls \"I found the zombie\" on %s (score %.1f)", String(F<int>(Vars(bot), EV_NETNAME)),
                    t->player ? "a player" : "an AI zombie", s.score + s.bias);
                return;
            }
        }
        // not sure yet: one shot to see how it reacts; it runs: shoot it down
        V3 aimD = Center(best) - eye;
        bool aimed = fabsf(AngleDiff(YawOf(aimD), z.yaw)) < 4.0f && fabsf(PitchOf(aimD) - z.pitch) < 6.0f;
        bool test = now - s.hitAt > 8.0f && dist < 450.0f;
        bool chase = running && dist < 700.0f && now - z.shotAt > 0.3f + (1.0f - skill) * 0.3f;
        if (aimed && (test || chase))
        {
            bool closeToDying = !t->player && t->health < 30.0f; // (anyone sees an AI zombie stagger)
            if (!closeToDying || running)
            {
                b |= IN_ATTACK;
                z.shotAt = now;
                if (test)
                {
                    s.hitAt = now;
                    s.hitHealth = t->health;
                    s.reacted = false;
                }
                z.doing = running ? "shooting at a zombie that runs" : "shooting a suspect to see how it reacts";
            }
        }
        return;
    }
    // nothing yet: keep an eye on the crowd near the goal it would go to
    if (now >= z.watchUntil || Len2D(z.watchSpot) == 0)
    {
        z.watchUntil = now + 6.0f + Rnd() * 6.0f;
        int gi = g_zt.playerGoals.empty() ? -1 : rand() % (int)g_zt.playerGoals.size();
        V3 c{};
        int n = 0;
        for (auto& a : g_zt.actors)
            c = c + a.pos, n++;
        if (n)
            c = c * (1.0f / n);
        V3 from = gi >= 0 ? g_zt.playerGoals[gi] : me;
        float k = 0.25f + Rnd() * 0.4f;
        z.watchSpot = n ? from + (c - from) * k : from;
        z.watchSpot.x += (Rnd() - 0.5f) * 200.0f;
        z.watchSpot.y += (Rnd() - 0.5f) * 200.0f;
    }
    goal = z.watchSpot;
    hasGoal = true;
    arrive = 80.0f;
    speed = 180.0f;
    z.doing = "watching the crowd";
    if (now >= z.nextLook || !z.lookAt || !ZtActorOf(z.lookAt))
    {
        z.nextLook = now + 1.5f + Rnd() * 2.0f;
        z.lookAt = nullptr;
        float bestD = 1e30f;
        for (auto& a : g_zt.actors)
        {
            float d = Len(a.pos - me) * (0.6f + Rnd());
            if (d < ZT_SIGHT && d < bestD)
                bestD = d, z.lookAt = a.e;
        }
    }
    face = z.lookAt;
}

// ---------------------------------------------------------------- every bot
static bool DriveZtBot(void* bot, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (GameMode() != MODE_ZTAG || !Alive(bot) || g_edictToPrivate < 0)
        return false;
    unsigned char* rules = ZtRules();
    if (!rules)
        return false;
    float now = Now();
    ZtScan(now);
    ZtBot& z = g_ztBots[bot];
    BotState& st = StateFor(bot);
    bool zombie = ZtIsZombie(bot);
    if (!z.init || now < z.lastThink - 1.0f || now - z.lastThink > 2.0f)
    {
        z = ZtBot();
        z.init = true;
        z.yaw = F<float>(Vars(bot), EV_VANGLE + 4);
        z.act = z.curAct = g_zt.action;
        st.path.clear();
        static int reports = 0;
        if (reports++ < 80)
            Log("Zombie Tag: bot '%s' plays %s", String(F<int>(Vars(bot), EV_NETNAME)), zombie ? "a zombie" : "a human");
    }
    float dt = now - z.lastThink;
    dt = dt <= 0 || dt > 0.25f ? 0.02f : dt;
    z.lastThink = now;
    V3 me = Origin(bot), eye = Eye(bot);
    V3 goal{};
    bool hasGoal = false;
    float arrive = 40.0f, speed = 0;
    void* face = nullptr;
    unsigned short b = 0;
    z.spin = 0;
    z.ownMove = false;
    if (rules[ZT_RULES_OVER])
        z.doing = "round over";
    else if (!rules[ZT_RULES_STARTED] || !rules[ZT_RULES_SPAWNED])
        z.doing = "waiting for the round";
    else if (zombie)
        ZtZombieThink(bot, z, rules, me, now, dt, goal, hasGoal, speed, face, b);
    else
        ZtHumanThink(bot, z, rules, me, eye, now, dt, goal, hasGoal, arrive, speed, face, b);

    float moveYaw = z.yaw;
    bool move = false;
    V3 wp;
    if (!z.ownMove && hasGoal && speed > 0 && NextWaypoint(bot, st, me, goal, arrive, wp))
    {
        moveYaw = WalkYaw(bot, me, YawOf(wp - me), 40.0f);
        move = true;
    }
    // where it looks: a zombie where it walks (the AI zombies never look at anyone), a human at what it watches
    float lookYaw = move ? moveYaw : z.yaw, lookPitch = 0;
    if (face)
    {
        V3 d = Center(face) - eye;
        lookYaw = YawOf(d);
        lookPitch = PitchOf(d);
    }
    if (z.spin != 0)
        z.yaw = AngleDiff(z.yaw + z.spin * dt, 0.0f); // spinning / walking in circles: it turns by itself
    else
        z.yaw = Approach(z.yaw, lookYaw, (zombie ? 220.0f : 300.0f + ZtSkill(bot) * 200.0f) * dt);
    float k = dt * 6.0f > 1.0f ? 1.0f : dt * 6.0f;
    z.pitch += (lookPitch - z.pitch) * k;
    float f = 0, s = 0;
    if (z.ownMove)
        f = z.ownFwd; // along where it faces (forwards in circles, or backwards)
    else if (move)
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
    buttons = (unsigned short)((buttons & IN_DUCK) | b);
    if (now >= z.nextReport)
    {
        z.nextReport = now + 10.0f;
        DLog("Zombie Tag bot '%s' (%s) at %.0f %.0f %.0f: %s; crowd speed %.0f, fever %d (action %d), suspects %d", String(F<int>(Vars(bot), EV_NETNAME)),
             zombie ? "zombie" : "human", me.x, me.y, me.z, z.doing, g_zt.crowdSpeed, g_zt.fever ? 1 : 0, g_zt.action, (int)z.sus.size());
    }
    return true;
}
