// botassist: Zombie 2: Mutation (mode 9, "map_zombi_expand"; rules CSOGameRulesZombi2). Included by botassist.cpp at
// this point (one translation unit: it uses the shared helpers defined before it). Only runs in its own mode:
// ZB2Watch returns and ZB2BotThink isn't called unless GameMode() == MODE_ZB2.
//
// Zombies regenerate standing still, evolve after infections (game side) and respawn after dying. Bots that died as
// zombies stayed down: their player think stops while dead (as in Zombie 4: Darkness: dead flag 3 "respawnable" and
// nothing runs CBasePlayer::PlayerDeathThink for them), so ZB2Watch runs it for a bot zombie that is respawnable for a
// second, or still dead 10 s after dying (the game decides what happens: respawn, spectator...).
// The rules' ClientCommand (Zombi2 vtable slot 0x2c, 0x1144dcc0) takes "selzmb <class>" (vfunc 0x454: "random" picks
// one, vfunc 0x4e0; once per zombie life, player +0x2e39 set) and "useitem zb_skill<0..3>" / "useitem hm_skill<0..3>"
// (0x1144f710: the zombie's G skill, the human's skills; cooldowns and costs are checked there). Bots:
//   - a new zombie bot picks a random class a moment after turning;
//   - a zombie bot with a human in sight within 700 u uses its skill now and then;
//   - a human bot with a zombie within 350 u uses its first skill (sprint where the mode has it) now and then.
static const int MODE_ZB2 = 9;

struct ZB2Dead
{
    float since = 0, respawnableSince = 0, lastTry = 0;
    int tries = 0;
};
static std::map<void*, ZB2Dead> g_zb2Dead;

static void ZB2Watch()
{
    static float next = 0;
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_ZB2)
    {
        g_zb2Dead.clear();
        return;
    }
    float now = Now();
    if (now >= next - 10.0f && now < next)
        return;
    next = now + 0.25f;
    std::set<void*> seen;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        if ((F<int>(v, EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) != (FL_CLIENT | FL_FAKECLIENT))
            return;
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (!p || !Readable(p, 0x2170))
            return;
        const char* name = String(F<int>(v, EV_NETNAME));
        int deadFlag = F<int>(v, EV_DEADFLAG);
        auto it = g_zb2Dead.find(e);
        if (deadFlag == 0 && F<float>(v, EV_HEALTH) > 0)
        {
            if (it != g_zb2Dead.end())
            {
                if (it->second.tries)
                    Log("ZB2: zombie '%s' is up again after %.0f s (botassist's help)", name, now - it->second.since);
                g_zb2Dead.erase(it);
            }
            return;
        }
        if (!p[0x2166] && it == g_zb2Dead.end())
            return; // a human: infected humans come back as zombies by themselves
        seen.insert(e);
        ZB2Dead& d = g_zb2Dead[e];
        if (d.since == 0)
            d.since = now;
        if (deadFlag == 3 && d.respawnableSince == 0)
            d.respawnableSince = now;
        bool due = (deadFlag == 3 && now - d.respawnableSince >= 1.0f) || now - d.since >= 10.0f;
        if (!due || now - d.lastTry < 1.0f)
            return;
        d.lastTry = now;
        int before = deadFlag;
        bool ok = CallPlayerDeathThink(p);
        if (ok && F<int>(v, EV_DEADFLAG) == 3)
            ok = CallPlayerDeathThink(p);
        if (d.tries++ < 3)
            Log("ZB2: zombie '%s' still down %.0f s after dying (dead flag %d, next think %.1f): death think%s -> dead flag "
                "%d, health %.0f", name, now - d.since, before, F<float>(v, 0x114), ok ? "" : " FAILED",
                F<int>(v, EV_DEADFLAG), F<float>(v, EV_HEALTH));
    });
    for (auto it = g_zb2Dead.begin(); it != g_zb2Dead.end();)
        it = seen.count(it->first) ? std::next(it) : g_zb2Dead.erase(it);
}

struct ZB2Bot
{
    bool wasZombie = false;
    float zombieSince = 0, nextSkill = 0, nextCheck = 0;
    bool picked = false;
};
static std::map<void*, ZB2Bot> g_zb2Bots;

static void ZB2BotThink(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2f00))
        return;
    ZB2Bot& z = g_zb2Bots[bot];
    float now = Now();
    if (now < z.nextCheck && now > z.nextCheck - 5.0f)
        return;
    z.nextCheck = now + 0.25f;
    bool zombie = p[0x2166] != 0;
    if (zombie != z.wasZombie)
    {
        z.wasZombie = zombie;
        z.zombieSince = now;
        z.picked = false;
        z.nextSkill = now + 3.0f;
    }
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    // a new zombie: a random class (the game takes it once per zombie life)
    if (zombie && !z.picked && now - z.zombieSince > 1.0f)
    {
        z.picked = true;
        if (!p[0x2e39])
        {
            FakeCommand(bot, "selzmb", "random");
            static int reports = 0;
            if (reports++ < 40)
                Log("ZB2: zombie bot '%s' picks a random class: now class %d", name, (signed char)p[0x218e]);
        }
    }
    if (now < z.nextSkill)
        return;
    // the closest enemy in sight
    V3 me = Origin(bot), eye = Eye(bot);
    float range = zombie ? 700.0f : 350.0f, best = 1e9f;
    ForEntitiesNear(me, range, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || (q[0x2166] != 0) == zombie)
            return;
        float d = Len(Origin(e) - me);
        if (d < best && CanSee(bot, eye, e))
            best = d;
    });
    if (best > range)
        return;
    z.nextSkill = now + 4.0f + Rnd() * 4.0f;
    if (Rnd() < 0.5f)
        return;
    FakeCommand(bot, "useitem", zombie ? "zb_skill0" : "hm_skill0");
    DLog("ZB2 bot '%s': %s skill (enemy %.0f u away)", name, zombie ? "zombie" : "human", best);
}
