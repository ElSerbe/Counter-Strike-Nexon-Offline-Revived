// botassist: Zombie 4: Darkness (mode 29, ZB4). Included by botassist.cpp at this point (one translation unit: it uses the shared helpers
// defined before it). Only runs in its own mode: Z4Watch returns unless GameMode() == MODE_Z4, Z4BotSkills is called only in that mode.

// ---------------------------------------------------------------- Zombie 4: Darkness (mode 29)
// CSOGameRulesZombie4Reboot (vtable 0x11968564). A zombie killed at night falls into a groggy state (player +0x2eef,
// entered at 0x11465b00 from CBasePlayer::Killed) and gets up 5 s later (the rules' death think, vfunc 0x118,
// 0x11462970). A zombie killed by day dies and respawns 7 s later (rules vfunc 0x1d4) through
// CBasePlayer::PlayerDeathThink (0x114c3cb0) -> respawn (0x113fa200, cdecl pev, copy corpse), unless the lethal hit
// came from a human with Concentrated Fire on (attacker +0x2ef8) by day and not from a grenade: the rules' damage
// hook (vfunc 0x1b8, 0x11462340) then sets victim +0x2ee8 = 1 and the "can respawn" check (0x114bdf00) lets it die
// for good. Day / night: rules +0x6e8 (1 = night), copied to each player's +0x2eec.
// The human skills are client commands handled by the rules' ClientCommand (vfunc 0x24, 0x11461460):
// "zb4 AdrShot" = Concentrated Fire (key 6: needs adrenaline +0x2f10 >= 30 of 100, burns it until it is gone),
// "zb4 Kick" = kick (key 5: along the view, knocks a zombie back; on a groggy zombie it takes some of its max health
// and gives adrenaline).
static const int MODE_Z4 = 29;
static const DWORD MP_Z4_VTABLE = 0x11968564, MP_RESPAWN = 0x113fa200;
static const int Z4_ZOMBIE = 0x2166, Z4_FINAL = 0x2ee8, Z4_GROGGY = 0x2eef, Z4_NIGHT = 0x2eec, Z4_ADRSHOT = 0x2ef8,
                 Z4_ADRENALINE = 0x2f10, Z4_RULES_NIGHT = 0x6e8;

static bool Z4Night()
{
    unsigned char* r = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    return r && Readable(r, 0x6f0) && *(DWORD*)r == MP_Z4_VTABLE + g_mpDelta && *(short*)(r + Z4_RULES_NIGHT) == 1;
}

static std::string Z4DeadState(void* e)
{
    unsigned char* v = Vars(e);
    unsigned char* p = (unsigned char*)PrivateOf(e);
    char buf[240];
    snprintf(buf, sizeof(buf), "dead flag %d, health %.0f, killed for good +0x2ee8 %d, groggy %d, night %d, died at %.1f / "
             "+0xf78 %.1f, next think %.1f, joining +0xcc0 %d, team %d",
             F<int>(v, EV_DEADFLAG), F<float>(v, EV_HEALTH), p ? *(int*)(p + Z4_FINAL) : -1, p ? p[Z4_GROGGY] : -1,
             p ? *(short*)(p + Z4_NIGHT) : -1, p ? *(float*)(p + 0xf74) : -1.0f, p ? *(float*)(p + 0xf78) : -1.0f,
             F<float>(v, 0x114), p ? *(int*)(p + 0xcc0) : -1, p ? *(int*)(p + 0x6c) : -1);
    return buf;
}

static bool Z4Respawn(void* e)
{
    __try
    {
        typedef void(__cdecl * RespawnFn)(void* pev, int copyCorpse);
        unsigned char* v = Vars(e);
        *(int*)(v + 0x1a4) = 0;          // pev->button, as PlayerDeathThink leaves it
        *(float*)(v + 0x114) = -1.0f;    // pev->nextthink
        ((RespawnFn)(MP_RESPAWN + g_mpDelta))(v, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Every dead zombie that wasn't killed for good should be up again within seconds (groggy 5 s, dead 7 s): one still
// down well after that goes through the game's own death think, then its respawn, and the log tells its state.
struct Z4Dead
{
    float since = 0, lastTry = 0;
    int tries = 0;
};
static std::map<void*, Z4Dead> g_z4Dead;

static void Z4Watch()
{
    static float next = 0;
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_Z4)
    {
        g_z4Dead.clear();
        return;
    }
    float now = Now();
    if (now >= next - 10.0f && now < next)
        return;
    next = now + 0.5f;
    std::set<void*> seen;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        if (!(F<int>(v, EV_FLAGS) & FL_CLIENT))
            return;
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (!p || !Readable(p, 0x2f20))
            return;
        const char* name = String(F<int>(v, EV_NETNAME));
        bool dead = F<int>(v, EV_DEADFLAG) != 0 || F<float>(v, EV_HEALTH) <= 0;
        auto it = g_z4Dead.find(e);
        if (!dead)
        {
            if (it != g_z4Dead.end())
            {
                Log("ZB4: zombie '%s' is up again after %.0f s%s", name, now - it->second.since,
                    it->second.tries ? " (botassist's help)" : "");
                g_z4Dead.erase(it);
            }
            return;
        }
        if (!p[Z4_ZOMBIE] && it == g_z4Dead.end())
            return; // a human killed (infected humans come back as zombies by themselves)
        seen.insert(e);
        Z4Dead& d = g_z4Dead[e];
        if (d.since == 0)
        {
            d.since = now;
            Log("ZB4: zombie '%s' down (%s)%s", name, Z4DeadState(e).c_str(),
                *(int*)(p + Z4_FINAL) == 1 ? ": killed for good by Concentrated Fire" : "");
        }
        if (*(int*)(p + Z4_FINAL) == 1)
            return;
        float wait = p[Z4_GROGGY] ? 9.0f : 11.0f;
        if (now - d.since < wait || now - d.lastTry < 2.0f)
            return;
        d.lastTry = now;
        std::string before = Z4DeadState(e);
        bool ok;
        const char* how;
        if (d.tries++ < 2)
        {
            ok = CallPlayerDeathThink(p);
            if (ok && F<int>(v, EV_DEADFLAG) == 3)
                ok = CallPlayerDeathThink(p);
            how = "death think";
        }
        else
        {
            // not between rounds (the CSO match object: +0x27c started, +0x27d over)
            unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
            if (!cso || !Readable(cso, 0x280) || cso[0x27c] != 1 || cso[0x27d])
                return;
            ok = Z4Respawn(e);
            how = "respawn";
        }
        if (d.tries <= 4)
            Log("ZB4: zombie '%s' still down %.0f s after dying (%s): %s%s -> %s", name, now - d.since, before.c_str(), how,
                ok ? "" : " FAILED", Z4DeadState(e).c_str());
    });
    for (auto it = g_z4Dead.begin(); it != g_z4Dead.end();)
        it = seen.count(it->first) ? std::next(it) : g_z4Dead.erase(it);
}

// The human bots' skills: Concentrated Fire on a zombie in sight that is worth finishing (hurt, or close with a full
// gauge) by day, when only it kills for good (at night only to save themselves); a kick at a zombie in their face, or
// at a groggy one at their feet (takes its max health, gives adrenaline). The view is turned to the zombie for the
// kick's trace and given back.
struct Z4Bot
{
    float nextCheck = 0, nextAdr = 0, nextKick = 0;
};
static std::map<void*, Z4Bot> g_z4Bots;

static void Z4BotSkills(void* bot)
{
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2f20) || p[Z4_ZOMBIE])
        return;
    Z4Bot& z = g_z4Bots[bot];
    float now = Now();
    if (now < z.nextCheck && now > z.nextCheck - 5.0f)
        return;
    z.nextCheck = now + 0.25f;
    V3 me = Origin(bot), eye = Eye(bot);
    void* best = nullptr;
    float bestD = 1e9f;
    void* kickAt = nullptr;
    float kickD = 1e9f;
    ForEntitiesNear(me, 1200.0f, [&](void* e) {
        unsigned char* v = Vars(e);
        if (e == bot || !(F<int>(v, EV_FLAGS) & FL_CLIENT))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2f20) || !q[Z4_ZOMBIE])
            return;
        float d = Len(Origin(e) - me);
        bool groggy = q[Z4_GROGGY] != 0;
        if (F<int>(v, EV_DEADFLAG) != 0 && !groggy)
            return;
        if (!CanSee(bot, eye, e))
            return;
        if (d < 85.0f && d < kickD && (groggy || Alive(e)))
        {
            kickAt = e;
            kickD = d;
        }
        if (!groggy && Alive(e) && d < bestD)
        {
            best = e;
            bestD = d;
        }
    });
    // Concentrated Fire
    float adr = *(float*)(p + Z4_ADRENALINE);
    bool night = Z4Night();
    if (best && !p[Z4_ADRSHOT] && now >= z.nextAdr && adr >= 30.0f)
    {
        unsigned char* v = Vars(best);
        float hp = F<float>(v, EV_HEALTH), maxHp = F<float>(v, 0x1dc);
        bool hurt = maxHp > 0 && hp <= maxHp * 0.5f;
        bool use = night ? adr >= 90.0f && bestD < 350.0f
                         : (hurt && bestD < 1000.0f) || (adr >= 90.0f && bestD < 700.0f) || (hp <= 1500.0f && bestD < 800.0f);
        if (use && Rnd() < 0.6f)
        {
            FakeCommand(bot, "zb4", "AdrShot");
            z.nextAdr = now + 4.0f;
            Log("ZB4: bot '%s' %s Concentrated Fire (adrenaline %.0f, %s): zombie '%s' %.0f u away, health %.0f/%.0f",
                String(F<int>(Vars(bot), EV_NETNAME)), p[Z4_ADRSHOT] ? "fires" : "tries", adr, night ? "night" : "day",
                String(F<int>(v, EV_NETNAME)), bestD, hp, maxHp);
        }
        else
            z.nextAdr = now + 1.0f;
    }
    // kick
    if (kickAt && now >= z.nextKick)
    {
        z.nextKick = now + 1.2f + Rnd() * 0.8f;
        bool groggy = ((unsigned char*)PrivateOf(kickAt))[Z4_GROGGY] != 0;
        if (groggy || Rnd() < 0.5f)
        {
            unsigned char* v = Vars(bot);
            float* va = (float*)(v + EV_VANGLE);
            float keep[3] = { va[0], va[1], va[2] };
            V3 to = Center(kickAt) - eye;
            va[0] = -atan2f(to.z, Len2D(to)) * RAD;
            va[1] = YawOf(to);
            FakeCommand(bot, "zb4", "Kick");
            va[0] = keep[0];
            va[1] = keep[1];
            va[2] = keep[2];
            DLog("ZB4 bot '%s': kicks %s zombie '%s' (%.0f u)", String(F<int>(v, EV_NETNAME)), groggy ? "the groggy" : "the",
                 String(F<int>(Vars(kickAt), EV_NETNAME)), kickD);
        }
    }
}
