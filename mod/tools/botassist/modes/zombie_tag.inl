// botassist: Zombie Tag, "Zombie Hide and Seek" in CSN:Z (mode 37, rules CSOGameRulesMugoonhwa, vtable 0x1179e52c). Included by botassist.cpp
// (one translation unit). Only runs in its mode: DriveZtBot is called only when GameMode() == MODE_ZTAG.
//
// The mode (https://cso.fandom.com/wiki/Zombie_Tag): half of the players become zombies (rules 0x10830460, CBasePlayer +0x3310 = 1, team 1) and
// hide among the map's AI zombies, "dummyman" (CDummyMan, spawned in a grid at 0x1082dd20; they walk to "dummygoal" points, some to the
// "playergoal"). A zombie wins by touching a playergoal (CPlayerGoal::GoalTouch -> 0x1082eba0). Humans (200 health, a knife) win by finding
// them. Every few seconds of "fever" (0x10830bc0) the AI zombies stop and all do one emotion (CDummyMan +0x12c4: 1 walking, else the
// emotion id, 10..26): the real zombies have to do the same one to pass for them. Their 4 emotions are the rules' +0x680[4], the same for all.
// Commands, the same a player's keys send (ClientCommand 0x1082d960):
//   zbtag_motion 1..4  a zombie does emotion +0x680[n-1]; 5 is its smoke (once a round, rules +0x6a1 + index). Needs FL_ONGROUND.
//   zbtag_motion 1     a human's "I found the zombie": the closest zombie or AI zombie within *0x11e56a58 units, less than 26 degrees off the
//                      human's view and looking back at them (forward vectors' dot < -0.8). 1.8 s later (0x10831010) a real zombie dies,
//                      an AI zombie kills the human. One at a time: rules +0x6d4 + index * 4 is the pending time.
//   sprintON / sprintOFF  a zombie's sprint (rules +0x72f + index).
// A zombie's emotion freezes it (CBasePlayer +0x2f74, FL_FROZEN) and in this mode no key ends it (PreThink 0x114c5f20 skips zombies of mode
// 37): the bots end it with the game's own function, 0x114d60f0, when the AI zombies walk again. A zombie's knife hits a human from behind and
// throws them (0x1082d5c0, no damage). Killing an AI zombie costs its killer health (0x1082e9f0).
//
// The stock bot AI knows the teams and nothing else, so it would beeline at the enemy. botassist plays both sides like a person:
//   zombie: walks with the crowd at the crowd's own speed, now and then by a dummygoal like they do, looks where it walks, does the fever's
//           emotion after a human's reaction time (sometimes late or wrong), keeps calm or panics when a human stares or hits it (smoke,
//           sprint), sprints the last stretch when nobody is near, and pokes a human from behind now and then.
//   human: watches the zombies in sight and scores what gives a player away (running, walking or not dancing during the fever, dancing late,
//          heading for the goal, standing still), hits a suspect once to see how it reacts, and calls "I found the zombie" only when sure and
//          when it can (in front, looking back). It can be wrong, like anyone. It knows nothing a player couldn't see.
static const int MODE_ZTAG = 37, ZT_FL_ONGROUND = 0x200;
static const DWORD MP_ZTAG_RULES_VT = 0x1179e52c, MP_ZTAG_END_EMOTION = 0x114d60f0, MP_ZTAG_FIND_RADIUS = 0x11e56a58;
static const int ZT_IS_ZOMBIE = 0x3310, ZT_EMOTING = 0x2f74, ZT_DUMMY_STATE = 0x12c4;
static const int ZT_RULES_STARTED = 0x6b9, ZT_RULES_SPAWNED = 0x747, ZT_RULES_OVER = 0x754, ZT_RULES_EMOTIONS = 0x680, ZT_RULES_FIND = 0x6d4,
                 ZT_RULES_SMOKED = 0x6a1, ZT_RULES_SPRINT = 0x72f;
static const float ZT_FIND_CONE = 0.9f, ZT_FIND_FACING = -0.8f, ZT_FIND_RADIUS_DEFAULT = 150.0f, ZT_SIGHT = 1500.0f, ZT_FOV_COS = 0.6f;
static const float ZT_SUSPECT = 4.0f, ZT_SURE = 8.0f, ZT_GOAL_NEAR = 450.0f, ZT_STARE = 1.2f;

struct ZtActor
{
    void* e;
    bool player;   // (only for the bookkeeping: the human bots score what they see, never this)
    V3 pos, vel;
    int state;     // AI zombie: 1 walking, else its emotion; player: 1 or 0 (emotion)
    bool emoting;
    float health;
};
static struct
{
    int level = -1;
    float next = 0;
    std::vector<ZtActor> actors;
    std::vector<V3> playerGoals, dummyGoals;
    float crowdSpeed = 110.0f; // how fast the AI zombies walk
    bool fever = false;
    float feverSince = -100, feverEnded = -100;
    int feverEmotion = 0;
    bool reported = false;
} g_zt;

struct ZtSuspect
{
    float score = 0, bias = 0;
    float seenAt = -100, hitAt = -100, hitHealth = -1;
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
    float emoteAt = 0, endAt = 0;
    int emoteSlot = -1;
    bool emoted = false;
    float stare = 0, lastHealth = -1, hurtAt = -100, panicUntil = 0, calmUntil = 0, pokeAt = -100;
    bool sprint = false, smoked = false;
    float sprintCmdAt = -100;
    float speedJitter = 1.0f;
    // human
    std::unordered_map<void*, ZtSuspect> sus;
    void* target = nullptr;
    float nextObserve = 0, nextLook = 0, slashAt = -100, findAt = -100;
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
    int walkers = 0, dancers = 0, dummies = 0;
    std::map<int, int> emotions;
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
            unsigned char* p = (unsigned char*)PrivateOf(e);
            if (!a.player)
            {
                a.state = p && Readable(p + ZT_DUMMY_STATE, 4) ? *(int*)(p + ZT_DUMMY_STATE) : 1;
                a.emoting = a.state != 1;
                dummies++;
                if (a.emoting)
                {
                    dancers++;
                    emotions[a.state]++;
                }
                else if (Len2D(a.vel) > 20.0f)
                {
                    speedSum += Len2D(a.vel);
                    walkers++;
                }
            }
            else
            {
                a.emoting = p && p[ZT_EMOTING];
                a.state = a.emoting ? 0 : 1;
            }
            g_zt.actors.push_back(a);
        }
    });
    if (walkers >= 3)
        g_zt.crowdSpeed += (speedSum / walkers - g_zt.crowdSpeed) * 0.2f;
    bool fever = dummies >= 3 && dancers * 2 >= dummies;
    if (fever && !g_zt.fever)
    {
        g_zt.feverSince = now;
        int best = 0;
        for (auto& kv : emotions)
            if (kv.second > best)
                best = kv.second, g_zt.feverEmotion = kv.first;
        static int reports = 0;
        if (reports++ < 40)
            Log("Zombie Tag: fever, the AI zombies do emotion %d (%d of %d)", g_zt.feverEmotion, dancers, dummies);
    }
    if (!fever && g_zt.fever)
        g_zt.feverEnded = now;
    g_zt.fever = fever;
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

// ---------------------------------------------------------------- zombie
static void ZtZombieThink(void* bot, ZtBot& z, unsigned char* rules, V3 me, float now, float dt, V3& goal, bool& hasGoal, float& speed,
                          void*& face, unsigned short& b)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    float skill = ZtSkill(bot);
    int index = ZtIndex(bot);
    bool emoting = p && p[ZT_EMOTING];
    float health = F<float>(Vars(bot), EV_HEALTH);
    if (z.lastHealth >= 0 && health < z.lastHealth - 0.5f)
        z.hurtAt = now;
    z.lastHealth = health;
    if (index > 0 && index < 33)
        z.smoked = rules[ZT_RULES_SMOKED + index] != 0;

    // ---- the fever: stop with the crowd, do the same emotion, walk again when they do
    if (emoting)
    {
        speed = 0;
        z.doing = "doing the fever's emotion";
        if (!g_zt.fever)
        {
            if (!z.endAt)
                z.endAt = now + 0.2f + Rnd() * (1.1f - skill * 0.6f);
            if (now >= z.endAt)
            {
                ZtEndEmotion(bot);
                z.endAt = 0;
            }
        }
        return;
    }
    z.endAt = 0;
    if (g_zt.fever && now > z.panicUntil)
    {
        speed = 0; // the crowd stopped
        z.doing = "stopped with the crowd for the fever";
        if (!z.emoted)
        {
            if (!z.emoteAt)
            {
                // a person's reaction, slower and less sure for a weak player: late now and then, very rarely the wrong one
                z.emoteAt = now + 0.35f + Rnd() * (1.2f - skill * 0.7f) + (Rnd() < 0.15f - skill * 0.1f ? 1.0f + Rnd() * 1.5f : 0.0f);
                z.emoteSlot = -1;
                for (int i = 0; i < 4; i++)
                    if (*(int*)(rules + ZT_RULES_EMOTIONS + i * 4) == g_zt.feverEmotion)
                        z.emoteSlot = i;
                if (z.emoteSlot < 0 || Rnd() < 0.06f - skill * 0.05f)
                    z.emoteSlot = rand() % 4;
            }
            if (now >= z.emoteAt && (F<int>(Vars(bot), EV_FLAGS) & ZT_FL_ONGROUND))
            {
                char n[4];
                snprintf(n, sizeof(n), "%d", z.emoteSlot + 1);
                ZtCommand(bot, "zbtag_motion", n);
                z.emoted = true;
                static int reports = 0;
                if (reports++ < 60)
                    Log("Zombie Tag: zombie '%s' does emotion %d (the crowd: %d) %.1f s into the fever", String(F<int>(Vars(bot), EV_NETNAME)),
                        *(int*)(rules + ZT_RULES_EMOTIONS + z.emoteSlot * 4), g_zt.feverEmotion, now - g_zt.feverSince);
            }
        }
        return;
    }
    if (!g_zt.fever)
    {
        z.emoted = false;
        z.emoteAt = 0;
    }

    // ---- humans around: a stare or a hit
    void* starer = ZtHumanStaring(bot, Center(bot), 600.0f, 12.0f);
    z.stare = starer ? z.stare + dt : (z.stare - dt * 0.5f > 0 ? z.stare - dt * 0.5f : 0);
    bool hit = now - z.hurtAt < 0.4f;
    if ((hit || z.stare > ZT_STARE * (0.6f + skill)) && now > z.panicUntil && now > z.calmUntil)
    {
        // a good player keeps playing the part, a nervous one runs
        float calm = hit ? 0.25f + skill * 0.35f : 0.45f + skill * 0.4f;
        if (Rnd() < calm)
        {
            z.calmUntil = now + 3.0f + Rnd() * 3.0f;
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
                Log("Zombie Tag: zombie '%s' %s: it runs", String(F<int>(Vars(bot), EV_NETNAME)), hit ? "was hit" : "is being watched");
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
    z.doing = z.viaDone ? "walking to the goal with the crowd" : "walking by a dummy goal like the crowd";
    float toGoal = z.goalIndex >= 0 ? Len2D(g_zt.playerGoals[z.goalIndex] - me) : 1e9f;
    bool humanNear = ZtHumanStaring(bot, Center(bot), 380.0f, 180.0f) != nullptr;
    bool dash = z.viaDone && toGoal < ZT_GOAL_NEAR && !humanNear;
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

    // ---- a human right ahead with their back turned: a poke throws them away (now and then; it gives it away)
    if (now - z.pokeAt > 6.0f && now > z.calmUntil)
        ForEntitiesNear(me, 75.0f, [&](void* e) {
            if (face || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || ZtIsZombie(e))
                return;
            V3 f = ZtForward(e), d = me - Origin(e);
            float len = Len2D(d);
            if (len < 1 || (f.x * d.x + f.y * d.y) / len > -0.3f)
                return; // it would see the poke coming
            bool inTheWay = z.goalIndex >= 0 && Len2D(g_zt.playerGoals[z.goalIndex] - Origin(e)) < toGoal;
            if ((inTheWay && Rnd() < 0.5f) || Rnd() < 0.08f)
            {
                face = e;
                z.pokeAt = now;
                b |= IN_ATTACK;
                z.doing = "poking a human from behind";
            }
            else
                z.pokeAt = now - 3.0f; // not this time
        });
}

// ---------------------------------------------------------------- human
// what a player can see of a zombie: how it moves, whether it dances with the others
static void ZtObserve(void* bot, ZtBot& z, V3 eye, float now, float skill)
{
    if (now < z.nextObserve && now > z.nextObserve - 1.0f)
        return;
    float dt = now - (z.nextObserve - 0.1f);
    dt = dt <= 0 || dt > 0.5f ? 0.1f : dt;
    z.nextObserve = now + 0.1f;
    V3 f = Forward(0, z.yaw);
    int traces = 0;
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
            s.bias = (Rnd() - 0.5f) * (1.5f - skill); // everyone has their hunches
        if (now - s.seenAt > 0.3f)
        {
            if (traces++ > 12)
                continue;
            TraceResult tr;
            if (!Trace(eye, Center(a.e), bot, 0, tr) && tr.pHit != a.e)
                continue;
        }
        s.seenAt = now;
        float speed = Len2D(a.vel), add = 0;
        if (g_zt.fever && now - g_zt.feverSince > 1.6f)
        {
            if (!a.emoting)
                add += 3.0f;
            if (speed > 40.0f)
                add += 3.0f;
        }
        else if (!g_zt.fever && now - g_zt.feverEnded < 4.0f && a.emoting)
            add += 1.2f; // still dancing after the others
        else if (!g_zt.fever)
        {
            if (speed > g_zt.crowdSpeed * 1.3f + 15.0f)
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
        // how it took the hit: an AI zombie doesn't care
        if (now - s.hitAt < 2.5f && !s.reacted && speed > g_zt.crowdSpeed * 1.4f)
        {
            s.reacted = true;
            s.score += 6.0f;
        }
        s.score += add * dt * 2.0f;
    }
    for (auto it = z.sus.begin(); it != z.sus.end();)
    {
        it->second.score *= expf(-dt / 25.0f);
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
        bool running = speedT > g_zt.crowdSpeed * 1.4f;
        // stand in front of it: "I found the zombie" needs it looking back
        V3 tf = ZtForward(best);
        goal = running ? t->pos : t->pos + tf * 70.0f;
        hasGoal = true;
        arrive = 30.0f;
        z.doing = running ? "chasing a zombie that runs" : "going to look at a suspect";
        float sure = ZT_SURE - (1.0f - skill) * 2.5f; // a weak player is sure sooner
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
                    t->player ? "a player" : "an AI zombie", s.score);
                return;
            }
        }
        // not sure yet, or it runs: hit it (a test, or to finish it)
        if (dist < 70.0f && now - z.slashAt > (running ? 0.45f : 5.0f) && (running || now - s.hitAt > 6.0f))
        {
            bool closeToDying = !t->player && t->health < 30.0f; // (anyone sees an AI zombie stagger)
            if (!closeToDying || running)
            {
                b |= IN_ATTACK;
                z.slashAt = now;
                if (now - s.hitAt > 6.0f)
                {
                    s.hitAt = now;
                    s.hitHealth = t->health;
                    s.reacted = false;
                }
                z.doing = running ? "slashing a zombie that runs" : "hitting a suspect to see how it reacts";
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
    if (hasGoal && speed > 0 && NextWaypoint(bot, st, me, goal, arrive, wp))
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
    z.yaw = Approach(z.yaw, lookYaw, (zombie ? 220.0f : 300.0f + ZtSkill(bot) * 200.0f) * dt);
    float k = dt * 6.0f > 1.0f ? 1.0f : dt * 6.0f;
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
    buttons = (unsigned short)((buttons & IN_DUCK) | b);
    if (now >= z.nextReport)
    {
        z.nextReport = now + 10.0f;
        DLog("Zombie Tag bot '%s' (%s) at %.0f %.0f %.0f: %s; crowd speed %.0f, fever %d, suspects %d", String(F<int>(Vars(bot), EV_NETNAME)),
             zombie ? "zombie" : "human", me.x, me.y, me.z, z.doing, g_zt.crowdSpeed, g_zt.fever ? 1 : 0, (int)z.sus.size());
    }
    return true;
}
