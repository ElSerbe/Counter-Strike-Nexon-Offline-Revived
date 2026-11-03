// botassist: All Star (mode 40; rules CSOGameRulesAllStar, vtable 0x1179d918). Included by botassist.cpp at this point (one translation unit:
// it uses the shared helpers defined before it). Only runs in its own mode: AsWatch returns, and DriveAllStarPlayer isn't called, unless
// GameMode() == MODE_ALLSTAR.
//
// The mode (https://cso.fandom.com/wiki/All_Star): team deathmatch where each life is one of 20 characters from other modes (+0x3700 of the
// player, drawn at respawn by the rules, 0x1082c760), with its own weapons and skills (commands AllStarAttack1 and 2, 0x10664940).
//
// What blocked it:
//   - The characters' numbers: AllStar_Status-Dedi.csv and AllStar_Skill-Dedi.csv are only read on a dedicated server (0x1082bf40, 0x1082bca0:
//     the check at AS_DEDICATED_CHECKS) and never shipped, so on a listen server characters had 0 health and no model. InstallAllStarTables
//     skips the check and gen_all_star.py writes the files, in botassist_tables.
//   - Bots: a character is state of AllStarPlayer (+0x3700 on, the class humans get). A CCSBot has other fields there, and the rules skip it
//     everywhere (player vfunc 0x21c), so bots were plain soldiers. As in Zombie Giant, the room's AI players join as fake players:
//     a fake client with a real player object (shared CreateZgPlayer, MarkZgPlayers, UpdateZgPlayers, and their engine guards), so the
//     rules draw them characters like any player. They have no AI: DriveAllStarPlayer plays them. Room weapons don't apply: each character
//     has its own. They hunt with Gun Deathmatch's routes (GdmPickTarget, GdmPickVia, GdmWalk), close-combat characters run at the enemy, the
//     rest keep moving around it, and they use their skills when they see someone.
static const DWORD MP_ALLSTAR_RULES_VT = 0x1179d918, MP_ALLSTAR_COMMAND = 0x10664940;
static const DWORD AS_DEDICATED_CHECKS[] = { 0x1082bfb4, 0x1082bd0b }; // jz past the table: 6 bytes each
static const float AS_RETARGET = 1.0f, AS_ENGAGE = 900.0f, AS_MELEE_REACH = 130.0f, AS_GUN_RANGE = 1400.0f, AS_FIRE_TIME = 1.2f, AS_FIRE_PAUSE = 0.15f;
static const float AS_SKILL_MIN = 3.0f, AS_SKILL_MORE = 5.0f, AS_SKILL_RANGE = 900.0f, AS_TURN = 14.0f, AS_FIRE_ANGLE = 12.0f, AS_RESPAWN_AFTER = 3.5f;

static void InstallAllStarTables(DWORD delta)
{
    int done = 0;
    for (DWORD at : AS_DEDICATED_CHECKS)
    {
        unsigned char* p = (unsigned char*)(at + delta);
        if (!Readable(p, 6))
            continue;
        if (p[0] == 0x90 && p[1] == 0x90)
        {
            done++;
            continue;
        }
        if (p[0] != 0x0f || p[1] != 0x84)
            continue;
        DWORD old;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
        memset(p, 0x90, 6);
        VirtualProtect(p, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 6);
        done++;
    }
    if (done != 2)
        Log("All Star: unexpected code: character tables not loaded (%d of 2)", done);
}

static bool AsMelee(int character)
{
    switch (character)
    {
    case 4: case 5: case 7: case 8: case 10: case 11: case 12: case 16: case 17: case 18: case 19:
        return true; // zombies, the Joker, monsters and the like
    }
    return false;
}

static void AsThink(void* e, ZgPlayer& z, BotState& st, unsigned char* pl, float now, float& fwd, float& side, unsigned short& buttons)
{
    unsigned char* v = Vars(e);
    GdmBot& g = g_gdmBots[e];
    V3 me = Origin(e), eye = Eye(e);
    int team = *(int*)(pl + PLAYER_TEAM);
    g.team = team;
    if (F<float>(v, EV_MAXSPEED) <= 1.0f || (F<int>(v, EV_FLAGS) & FL_FROZEN))
    {
        st.lastPos = me;
        st.lastProgress = now;
        return;
    }
    if (now >= g.retargetAt || !g.target)
    {
        g.retargetAt = now + AS_RETARGET;
        GdmPickTarget(e, g, me, eye, team);
    }
    if (g.target && (!(F<int>(Vars(g.target), EV_FLAGS) & FL_CLIENT) || !Alive(g.target)))
        g.target = nullptr;
    if (!g.target)
        return;
    int character = *(int*)(pl + 0x3700);
    bool melee = AsMelee(character);
    V3 at = Origin(g.target);
    float d = Len(at - me);
    bool seen = CanSee(e, eye, g.target);
    V3 goal = at;
    bool stay = false;
    if (seen)
    {
        g.lastSeen = now;
        // aims like its room skill: the worse, the wider
        MatchBot* rb = RoomBotOf(e, st);
        int skill = rb && rb->skill >= 0 ? rb->skill : (int)g_difficulty;
        float error = (8 - skill) * 0.7f;
        V3 aim = Center(g.target);
        z.yaw = Approach(z.yaw, YawOf(aim - eye) + (Rnd() - 0.5f) * error, AS_TURN);
        z.pitch += AngleDiff(PitchOf(aim - eye) + (Rnd() - 0.5f) * error, z.pitch) * 0.4f;
        bool facing = fabsf(AngleDiff(z.yaw, YawOf(aim - eye))) < AS_FIRE_ANGLE + error;
        if (facing && d < (melee ? AS_MELEE_REACH : AS_GUN_RANGE))
        {
            if (now >= z.nextFire)
            {
                buttons |= IN_ATTACK;
                if (now >= z.nextFire + AS_FIRE_TIME)
                    z.nextFire = now + AS_FIRE_PAUSE; // a short pause: semi-automatic guns fire again, empty ones reload
            }
        }
        if (d < AS_SKILL_RANGE && now >= z.nextSkill)
        {
            z.nextSkill = now + AS_SKILL_MIN + Rnd() * AS_SKILL_MORE;
            typedef char(__fastcall * AsCommandFn)(void* player, void* edx, const char* command);
            ((AsCommandFn)(MP_ALLSTAR_COMMAND + g_mpDelta))(pl, nullptr, rand() % 2 ? "AllStarAttack1" : "AllStarAttack2");
        }
        if (melee)
            stay = d < 56.0f;
        else if (d < GDM_CLOSE)
        {
            if (now >= g.strafeAt)
            {
                g.strafeAt = now + GDM_STRAFE * (0.6f + Rnd() * 0.8f);
                g.strafe = rand() % 2 ? 1 : -1;
            }
            float around = YawOf(at - me) + g.strafe * (d < GDM_TOO_CLOSE ? 120.0f : 70.0f);
            goal = me + V3{ cosf(around / RAD), sinf(around / RAD), 0 } * 120.0f;
        }
    }
    if (!(seen && d < AS_ENGAGE))
    {
        if (!g.hasVia && now >= g.viaAt)
            GdmPickVia(e, g, me, at);
        if (g.hasVia && (Len2D(g.via - me) < GDM_VIA_REACHED || now >= g.viaUntil))
        {
            g.hasVia = false;
            g.viaAt = now + 3.0f + Rnd() * 7.0f;
        }
        if (g.hasVia)
            goal = g.via;
    }
    if (stay)
    {
        st.lastPos = me;
        st.lastProgress = now;
        return;
    }
    V3 wp;
    if (!NextWaypoint(e, st, me, goal, 32.0f, wp))
        wp = goal;
    float wayYaw = YawOf(wp - me), maxDrop = ZCON_FREE_DROP;
    if (me.z - 36.0f - wp.z + 32.0f > maxDrop)
        maxDrop = min(me.z - 36.0f - wp.z + 32.0f, HURT_DROP_MAX);
    bool setView = false;
    float view = z.yaw;
    GdmWalk(e, st, g, me, now, wayYaw, wayYaw, maxDrop, view, setView, fwd, side, buttons);
    if (setView)
    {
        z.yaw = view;
        z.pitch *= 0.9f;
    }
    if (g.stuckNow && g.stuck >= 4)
    {
        g.hasVia = false;
        g.target = nullptr;
        static int reports = 0;
        if (reports++ < 30)
            Log("All Star: '%s' stuck at %.0f %.0f %.0f going to %.0f %.0f %.0f", z.name.c_str(), me.x, me.y, me.z, goal.x, goal.y, goal.z);
    }
}

static void DriveAllStarPlayer(ZgPlayer& z, float now, float dt)
{
    void* e = z.edict;
    unsigned char* v = Vars(e);
    unsigned char* pl = (unsigned char*)PrivateOf(e);
    if (!pl || !(F<int>(v, EV_FLAGS) & FL_CLIENT))
    {
        Log("All Star: '%s' left the game", z.name.c_str());
        z.edict = nullptr;
        return;
    }
    if (*(DWORD*)pl != MP_ALLSTARPLAYER_VT + g_mpDelta)
        return;
    float fwd = 0, side = 0;
    unsigned short buttons = 0;
    *(float*)(pl + PLAYER_LAST_MOVEMENT) = now; // never idle-kicked
    *(int*)(pl + 0x2f54) = 0;                   // see DriveZgPlayer: respawning strangers get kicked
    if (!Alive(e))
    {
        if (z.wasAlive)
            z.deadSince = now;
        z.wasAlive = false;
        if (now - z.deadSince > AS_RESPAWN_AFTER && now >= z.nextRespawn)
        {
            FakeCommand(e, "respawn");
            z.nextRespawn = now + 3.0f;
        }
    }
    else
    {
        if (!z.wasAlive)
        {
            z.wasAlive = true;
            z.yaw = F<float>(v, EV_VANGLE + 4);
            z.pitch = 0;
            z.nextSkill = now + AS_SKILL_MIN + Rnd() * AS_SKILL_MORE;
            static int reports = 0;
            if (reports++ < 60)
                Log("All Star: '%s' spawns as character %d (team %d, health %.0f)", z.name.c_str(), *(int*)(pl + 0x3700), *(int*)(pl + PLAYER_TEAM), F<float>(v, EV_HEALTH));
        }
        AsThink(e, z, StateFor(e), pl, now, fwd, side, buttons);
    }
    float ang[3] = { z.pitch, AngleDiff(z.yaw, 0.0f), 0 };
    float* va = (float*)(v + EV_VANGLE);
    float* an = (float*)(v + EV_ANGLES);
    va[0] = ang[0];
    va[1] = ang[1];
    va[2] = 0;
    an[0] = ang[0] / -3.0f;
    an[1] = ang[1];
    an[2] = 0;
    int ms = (int)(dt * 1000.0f);
    g_RunPlayerMove(e, ang, fwd, side, 0, buttons, 0, (unsigned char)(ms < 1 ? 1 : ms > 100 ? 100 : ms));
}

// The fake players' frame: no stock bot runs in this mode, so nothing else would call UpdateZgPlayers
static void AsWatch()
{
    static float next = 0;
    unsigned char* rules = g_mpDelta ? *(unsigned char**)(MP_GAMERULES + g_mpDelta) : nullptr;
    if (!rules || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_ALLSTAR || !Readable(rules, 4) || *(DWORD*)rules != MP_ALLSTAR_RULES_VT + g_mpDelta)
        return;
    LoadNav();
    UpdateHurtZones();
    UpdateMatchBots();
    UpdateZgPlayers();
    float now = Now();
    if (now >= next - 60.0f && now < next)
        return;
    next = now + 15.0f;
    static int reports = 0;
    if (reports++ >= 100)
        return;
    int alive[3] = { 0, 0, 0 }, dead = 0;
    std::string characters;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            return;
        int team = TeamOf(e);
        if (team != 1 && team != 2)
            return;
        if (!Alive(e))
            dead++;
        else
            alive[team]++;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        char one[12];
        snprintf(one, sizeof(one), "%d ", q && *(DWORD*)q == MP_ALLSTARPLAYER_VT + g_mpDelta ? *(int*)(q + 0x3700) : -1);
        characters += one;
    });
    Log("All Star: %d T and %d CT alive, %d dead; characters: %s", alive[1], alive[2], dead, characters.c_str());
}
