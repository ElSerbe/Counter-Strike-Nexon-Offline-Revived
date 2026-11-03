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
//   - One character per team: the rules read a room option (vfunc 0x28c) into +0x2a8, a variant where the kills on a team's table pick its
//     character (0x1082a7c0) and nobody draws. The rooms here shouldn't use it: AS_LADDER_CHECK makes the constructor (0x108298a0) leave it off.
//   - Fake players' speed: the rules give a character its speed and gravity only past 0x114bd6a0, which fake players fail, so they kept their
//     weapon's: -1 with the zombies' knife, 0 with the bazooka. AsFixSpeed does it for them.
//   - The kart, ball, chicken and little snowman stand 16-19 units off the ground: their models go down to z -18 or so, the others to -36. AsGround
//     lowers the root bones of the loaded model, which the client shares on this listen server.
//   - The light zombies' knife does no damage here (why isn't known): AsClaw hits with the pawns' melee function (0x10663300).
//   - Spawn points run out with 32 players: AsUnstack moves a fake player off anyone it stands on.
//   - Bots: a character is state of AllStarPlayer (+0x3700 on, the class humans get). A CCSBot has other fields there, and the rules skip it
//     everywhere (player vfunc 0x21c), so bots were plain soldiers. As in Zombie Giant, the room's AI players join as fake players:
//     a fake client with a real player object (shared CreateZgPlayer, MarkZgPlayers, UpdateZgPlayers, and their engine guards), so the
//     rules draw them characters like any player. They have no AI: DriveAllStarPlayer plays them. Room weapons don't apply: each character
//     has its own. They hunt with Gun Deathmatch's routes (GdmPickTarget, GdmPickVia, GdmWalk), close-combat characters run at the enemy, the
//     rest keep moving around it, and they use their skills when they see someone.
static const DWORD MP_ALLSTAR_RULES_VT = 0x1179d918, MP_ALLSTAR_COMMAND = 0x10664940;
static const DWORD AS_LADDER_CHECK = 0x10829958, MP_ALLSTAR_SET_NEXT = 0x106640d0, MP_ALLSTAR_APPLY_NEXT = 0x10663d00;
static const DWORD MP_ALLSTAR_PAWN_SPEED = 0x10663710, MP_ALLSTAR_STATUS_ROW = 0x10659290, MP_ALLSTAR_MELEE = 0x10663300, MP_UTIL_SETORIGIN = 0x11519ec0;
static const DWORD MP_ENG_GET_MODEL_PTR = 0x121d3b94;
static const int AS_RULES_LADDER = 0x2a8, AS_CHARACTER = 0x3700, AS_NEXT_CHARACTER = 0x3704, AS_CHARACTERS = 20, AS_RESET_MAXSPEED_VFUNC = 0x1ac;
static const int AS_ZOMBIE3 = 4, AS_ZOMBIE4 = 5, AS_CHICKEN = 11, AS_BALL = 12, AS_SNOW = 18, AS_KART = 20;
static const int AS_IN_ATTACK2 = 0x800, AS_EV_BUTTON = 0x1a4, AS_EV_GRAVITY = 0x12c, AS_ROW_GRAVITY = 0x3c, AS_PAWN = 0x3718, AS_PAWN_KART_VEHICLE = 0x20;
static const int AS_EV_EDICT = 0x238, AS_MDL_NUMBONES = 140, AS_MDL_BONEINDEX = 144, AS_MDL_DONE_MARK = 71, AS_BONE_SIZE = 112, AS_BONE_PARENT = 32, AS_BONE_POS_Z = 72;
static const DWORD AS_DEDICATED_CHECKS[] = { 0x1082bfb4, 0x1082bd0b }; // jz past the table: 6 bytes each
static const float AS_RETARGET = 1.0f, AS_ENGAGE = 900.0f, AS_MELEE_REACH = 130.0f, AS_GUN_RANGE = 1400.0f, AS_FIRE_TIME = 1.2f, AS_FIRE_PAUSE = 0.15f;
static const float AS_SKILL_MIN = 3.0f, AS_SKILL_MORE = 5.0f, AS_SKILL_RANGE = 900.0f, AS_TURN = 14.0f, AS_FIRE_ANGLE = 12.0f, AS_RESPAWN_AFTER = 3.5f;
static const float AS_SNOW_BLAST = 220.0f, AS_KART_TURBO_FROM = 450.0f, AS_NO_CHARACTER_FOR = 2.0f, AS_FROZEN_FOR = 12.0f, AS_BOT_RUN = 400.0f;
static const float AS_FLEE_FROM = 900.0f, AS_FLEE_STEP = 300.0f, AS_HIDE_UNTIL = 800.0f, AS_CHICKEN_FLEE = 3.0f, AS_SECOND_HOLD = 0.3f, AS_PLAYERS_EVERY = 0.05f;
static const float AS_CLAW_DAMAGE = 400.0f, AS_CLAW_RADIUS = 100.0f, AS_CLAW_REACH = 48.0f, AS_CLAW_EVERY = 0.6f, AS_MELEE_HURT_A = 20.0f, AS_MELEE_HURT_B = 100.0f, AS_MELEE_HURT_C = 200.0f;
static const float AS_UNSTACK_EVERY = 0.5f, AS_UNSTACK_RANGE = 800.0f, AS_UNSTACK_LEVEL = 120.0f, AS_HULL_WIDE = 32.0f, AS_HULL_TALL = 72.0f, AS_STAND_ABOVE_NAV = 37.0f;

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
    unsigned char* jump = (unsigned char*)(AS_LADDER_CHECK + delta);
    if (!Readable(jump, 2) || (jump[0] != 0x75 && jump[0] != 0xeb) || jump[1] != 0x02)
    {
        Log("All Star: unexpected code: characters may go by team");
        return;
    }
    if (jump[0] == 0x75)
    {
        DWORD old;
        VirtualProtect(jump, 1, PAGE_EXECUTE_READWRITE, &old);
        jump[0] = 0xeb; // jnz -> jmp: to the xor that leaves it off
        VirtualProtect(jump, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), jump, 2);
    }
}

// a character of its own for the next life
static void AsDrawNext(unsigned char* pl)
{
    typedef void(__fastcall * SetNextFn)(void* player, void* edx, int character);
    ((SetNextFn)(MP_ALLSTAR_SET_NEXT + g_mpDelta))(pl, nullptr, 1 + (int)((rand() ^ GetTickCount()) % AS_CHARACTERS));
}

// it spawned before it had a character: gives it one now, as the game does when a player changes (0x1082b300)
static void AsGiveCharacter(unsigned char* pl)
{
    if (!*(int*)(pl + AS_NEXT_CHARACTER))
        AsDrawNext(pl);
    typedef void(__fastcall * ApplyNextFn)(void* player);
    ((ApplyNextFn)(MP_ALLSTAR_APPLY_NEXT + g_mpDelta))(pl);
}

typedef char(__fastcall* AsCommandFn)(void* player, void* edx, const char* command);

// How each class fights, from its text in the class panel and what its pawn reads: the attack button in slot 4 (+0x10), the commands in slot 9 (+0x24)
struct AsClass
{
    bool closeIn;     // runs at the enemy instead of moving around it
    float reach;      // how far its attack button works
    int button;       // 0: it has none
    float secondFrom; // how far it uses the other mouse button, now and then (0: never)
    float attack1Reach, attack2Reach, attack2From; // AllStarAttack1 and 2 (0: never)
    float skillEvery; // seconds between uses (0: a random time)
};
static const AsClass AS_CLASSES[AS_CHARACTERS + 1] = {
    { false, AS_GUN_RANGE, IN_ATTACK, 0, 0, 0, 0, 0 },         // no character yet: a soldier
    { false, AS_GUN_RANGE, IN_ATTACK, 900, 0, 0, 0, 0 },       // 1 Hero: rifle, the right button fires a projectile
    { true, 500, IN_ATTACK, 250, 0, 0, 0, 0 },                 // 2 Heroine: shotgun, the right button fires all its bullets
    { false, AS_GUN_RANGE, IN_ATTACK, 0, 0, 0, 0, 0 },         // 3 Bazooka
    { true, 90, IN_ATTACK, 0, 0, 0, 0, 0 },                    // 4 Light Zombie
    { true, 90, IN_ATTACK, 600, 0, 0, 0, 0 },                  // 5 Light Zombie 2: the right button is a high jump
    { false, AS_GUN_RANGE, IN_ATTACK, 0, 0, 0, 0, 0 },         // 6 Battle Rush: rifle
    { true, 90, IN_ATTACK, 90, 0, 0, 0, 0 },                   // 7 Joker: quick and strong staff hits
    { true, 170, IN_ATTACK, 0, 0, 900, 250, 0 },               // 8 Revenant: slash, fire at range
    { false, AS_GUN_RANGE, AS_IN_ATTACK2, 0, 1000, 0, 0, 0 },  // 9 XT-300: machine gun on the right button, 3 rockets
    { true, 170, IN_ATTACK, 0, 0, 400, 0, 0 },                 // 10 Snowbeast: arms, a stomp when close
    { true, 0, 0, 0, 250, 0, 0, 0 },                           // 11 Chicken: lays a grenade and has to run
    { false, 0, 0, 0, 0, 0, 0, 0 },                            // 12 Soccer ball: scores while it lives, so it hides
    { false, 0, 0, 0, 900, 900, 0, 0 },                        // 13 Dr. Rex: tentacles
    { true, 0, 0, 0, 0, 0, 0, 0 },                             // 14 Mosquito: drinks from whoever is near
    { false, AS_GUN_RANGE, IN_ATTACK, 0, 0, 0, 0, 0 },         // 15 Drone: machine gun
    { true, 200, IN_ATTACK, 400, 0, 0, 0, 0 },                 // 16 Light Zombie Boss: stomp, a kick on the right button
    { false, 2500, IN_ATTACK, 0, 0, 0, 0, 0 },                 // 17 Hector: laser, a sniper
    { true, 0, 0, 0, AS_SNOW_BLAST, 0, 0, 0 },                 // 18 Snowman: blows itself up next to someone
    { true, 0, 0, 0, 170, 900, 250, 1.0f },                    // 19 Frozen Wrath: swings by repeating the command, throws the bat
    { false, 0, 0, 0, AS_SKILL_RANGE, 1e6f, AS_KART_TURBO_FROM, 0 }, // 20 Kartrider: water bomb, turbo to close in
};

// The rules' way, for those it leaves out. A max speed of 1 is an attack or the freeze time holding it: left alone
static void AsFixSpeed(unsigned char* v, unsigned char* pl, int character)
{
    if (!character)
        return;
    typedef float(__fastcall * PawnSpeedFn)(void* player);
    float speed = ((PawnSpeedFn)(MP_ALLSTAR_PAWN_SPEED + g_mpDelta))(pl);
    if (speed > 1.0f && F<float>(v, EV_MAXSPEED) != 1.0f)
        F<float>(v, EV_MAXSPEED) = speed;
    typedef unsigned char*(__cdecl * StatusRowFn)(int character, int team);
    float gravity = *(float*)(((StatusRowFn)(MP_ALLSTAR_STATUS_ROW + g_mpDelta))(character, *(int*)(pl + PLAYER_TEAM)) + AS_ROW_GRAVITY);
    if (F<float>(v, AS_EV_GRAVITY) == 1.0f && gravity > 0)
        F<float>(v, AS_EV_GRAVITY) = gravity;
}

static float AsModelDrop(int character)
{
    switch (character)
    {
    case AS_CHICKEN: return 17.0f;
    case AS_BALL: return 16.0f;
    case AS_SNOW: return 19.0f;
    case AS_KART: return 18.0f;
    }
    return 0;
}

static void AsGround(void* edict, float drop)
{
    typedef unsigned char*(__cdecl * ModelPtrFn)(void* edict);
    ModelPtrFn modelPtr = *(ModelPtrFn*)(MP_ENG_GET_MODEL_PTR + g_mpDelta);
    unsigned char* model = modelPtr && edict ? modelPtr(edict) : nullptr;
    if (!model || !Readable(model, AS_MDL_BONEINDEX + 4) || memcmp(model, "IDST", 4) || model[AS_MDL_DONE_MARK])
        return;
    unsigned char* bones = model + *(int*)(model + AS_MDL_BONEINDEX);
    int count = *(int*)(model + AS_MDL_NUMBONES);
    if (!Readable(bones, count * AS_BONE_SIZE))
        return;
    for (int i = 0; i < count; i++)
    {
        unsigned char* bone = bones + i * AS_BONE_SIZE;
        if (*(int*)(bone + AS_BONE_PARENT) == -1)
            *(float*)(bone + AS_BONE_POS_Z) -= drop;
    }
    model[AS_MDL_DONE_MARK] = 1;
    Log("All Star: model '%s' lowered %.0f units to the ground", (const char*)model + 8, drop);
}

static void AsGroundPlayer(void* e, unsigned char* pl, int character)
{
    float drop = AsModelDrop(character);
    if (!drop)
        return;
    AsGround(e, drop);
    if (character != AS_KART)
        return;
    unsigned char* pawn = *(unsigned char**)(pl + AS_PAWN);
    unsigned char* vehicle = pawn && Readable(pawn, AS_PAWN_KART_VEHICLE + 4) ? *(unsigned char**)(pawn + AS_PAWN_KART_VEHICLE) : nullptr;
    unsigned char* vars = vehicle && Readable(vehicle, 12) ? *(unsigned char**)(vehicle + 8) : nullptr;
    if (vars && Readable(vars, AS_EV_EDICT + 4))
        AsGround(*(void**)(vars + AS_EV_EDICT), drop);
}

static void AsClaw(unsigned char* pl)
{
    typedef void(__cdecl * MeleeFn)(void* player, float damage, float radius, float reach, float a, float b, float c);
    ((MeleeFn)(MP_ALLSTAR_MELEE + g_mpDelta))(pl, AS_CLAW_DAMAGE, AS_CLAW_RADIUS, AS_CLAW_REACH, AS_MELEE_HURT_A, AS_MELEE_HURT_B, AS_MELEE_HURT_C);
}

static bool AsSomeoneAt(void* e, V3 at)
{
    bool found = false;
    ForEntitiesNear(at, AS_HULL_TALL * 2.0f, [&](void* o) {
        if (o == e || found || !(F<int>(Vars(o), EV_FLAGS) & FL_CLIENT) || !Alive(o))
            return;
        V3 d = Origin(o) - at;
        found = fabsf(d.x) < AS_HULL_WIDE && fabsf(d.y) < AS_HULL_WIDE && fabsf(d.z) < AS_HULL_TALL;
    });
    return found;
}

// to the closest free place on the nav mesh
static void AsUnstack(void* e, V3 me, ZgPlayer& z)
{
    float best = AS_UNSTACK_RANGE;
    V3 spot = me;
    for (auto& area : g_nav.areas)
    {
        V3 c = area.center + V3{ 0, 0, AS_STAND_ABOVE_NAV };
        float d = Len(c - me);
        if (d < AS_HULL_WIDE * 2.0f || d >= best || fabsf(c.z - me.z) > AS_UNSTACK_LEVEL || AsSomeoneAt(e, c))
            continue;
        best = d;
        spot = c;
    }
    if (best >= AS_UNSTACK_RANGE)
        return;
    typedef void(__cdecl * SetOriginFn)(void* vars, float* origin);
    float origin[3] = { spot.x, spot.y, spot.z };
    ((SetOriginFn)(MP_UTIL_SETORIGIN + g_mpDelta))(Vars(e), origin);
    F<float>(Vars(e), EV_VELOCITY) = F<float>(Vars(e), EV_VELOCITY + 4) = F<float>(Vars(e), EV_VELOCITY + 8) = 0;
    static int reports = 0;
    if (reports++ < 20)
        Log("All Star: '%s' was standing on someone: moved %.0f units", z.name.c_str(), best);
}

static void AsThink(void* e, ZgPlayer& z, BotState& st, unsigned char* pl, float now, float& fwd, float& side, unsigned short& buttons)
{
    unsigned char* v = Vars(e);
    GdmBot& g = g_gdmBots[e];
    V3 me = Origin(e), eye = Eye(e);
    int team = *(int*)(pl + PLAYER_TEAM);
    g.team = team;
    static std::map<void*, float> frozenSince;
    if (F<float>(v, EV_MAXSPEED) <= 1.0f || (F<int>(v, EV_FLAGS) & FL_FROZEN))
    {
        st.lastPos = me;
        st.lastProgress = now;
        // longer than a round's freeze time: nothing is going to free it
        float& since = frozenSince[e];
        if (!since || since > now)
            since = now;
        else if (now - since > AS_FROZEN_FOR)
        {
            static int reports = 0;
            if (reports++ < 10)
                Log("All Star: '%s' stayed frozen (max speed %.0f, flags 0x%x, character %d): freed", z.name.c_str(), F<float>(v, EV_MAXSPEED), F<int>(v, EV_FLAGS), *(int*)(pl + AS_CHARACTER));
            F<int>(v, EV_FLAGS) &= ~FL_FROZEN;
            typedef void(__fastcall * ResetMaxSpeedFn)(void* player);
            (*(ResetMaxSpeedFn**)pl)[AS_RESET_MAXSPEED_VFUNC / 4](pl);
            since = 0;
        }
        return;
    }
    frozenSince[e] = 0;
    if (now >= g.retargetAt || !g.target)
    {
        g.retargetAt = now + AS_RETARGET;
        GdmPickTarget(e, g, me, eye, team);
    }
    if (g.target && (!(F<int>(Vars(g.target), EV_FLAGS) & FL_CLIENT) || !Alive(g.target)))
        g.target = nullptr;
    if (!g.target)
        return;
    int character = *(int*)(pl + AS_CHARACTER);
    const AsClass& fights = AS_CLASSES[character >= 0 && character <= AS_CHARACTERS ? character : 0];
    bool melee = fights.closeIn;
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
        if (facing && fights.button && d < fights.reach)
        {
            if (now >= z.nextFire)
            {
                buttons |= fights.button;
                if (now >= z.nextFire + AS_FIRE_TIME)
                    z.nextFire = now + AS_FIRE_PAUSE; // a short pause: semi-automatic guns fire again, empty ones reload
            }
        }
        bool first = d < fights.attack1Reach, second = d < fights.attack2Reach && d >= fights.attack2From, other = d < fights.secondFrom;
        if (now >= z.nextSkill && (first || second || other))
        {
            z.nextSkill = now + (fights.skillEvery > 0 ? fights.skillEvery : AS_SKILL_MIN + Rnd() * AS_SKILL_MORE);
            if (other)
                z.aimUntil = now + AS_SECOND_HOLD;
            if (first || second)
            {
                bool useSecond = first && second ? rand() % 2 != 0 : second;
                ((AsCommandFn)(MP_ALLSTAR_COMMAND + g_mpDelta))(pl, nullptr, useSecond ? "AllStarAttack2" : "AllStarAttack1");
                if (character == AS_CHICKEN)
                    z.sprintUntil = now + AS_CHICKEN_FLEE;
            }
        }
        if (now < z.aimUntil)
            buttons |= AS_IN_ATTACK2;
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
    // the ball hides and the chicken runs from its grenade
    bool flee = (character == AS_BALL && seen && d < AS_FLEE_FROM) || (character == AS_CHICKEN && now < z.sprintUntil);
    if (flee)
    {
        float away = YawOf(me - at);
        goal = me + V3{ cosf(away / RAD), sinf(away / RAD), 0 } * AS_FLEE_STEP;
        stay = false;
    }
    else if (character == AS_BALL && d > AS_HIDE_UNTIL)
        stay = true;
    if (!seen && character == AS_KART && now >= z.nextSkill)
    {
        z.nextSkill = now + AS_SKILL_MIN + Rnd() * AS_SKILL_MORE;
        ((AsCommandFn)(MP_ALLSTAR_COMMAND + g_mpDelta))(pl, nullptr, "AllStarAttack2");
    }
    if (!flee && !(seen && d < AS_ENGAGE))
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
            AsDrawNext(pl);
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
                Log("All Star: '%s' spawns as character %d (team %d, health %.0f)", z.name.c_str(), *(int*)(pl + AS_CHARACTER), *(int*)(pl + PLAYER_TEAM), F<float>(v, EV_HEALTH));
            z.busySince = now;
        }
        if (!*(int*)(pl + AS_CHARACTER) && now - z.busySince > AS_NO_CHARACTER_FOR)
        {
            AsGiveCharacter(pl);
            z.busySince = now;
        }
        AsFixSpeed(v, pl, *(int*)(pl + AS_CHARACTER));
        if (now >= z.nextUse)
        {
            z.nextUse = now + AS_UNSTACK_EVERY;
            V3 at = Origin(e);
            if (AsSomeoneAt(e, at))
                AsUnstack(e, at, z);
        }
        AsThink(e, z, StateFor(e), pl, now, fwd, side, buttons);
        float speed = F<float>(v, EV_MAXSPEED); // the walking code asks for a fixed run: as fast as this character goes
        if (speed > 1.0f)
        {
            fwd *= speed / AS_BOT_RUN;
            side *= speed / AS_BOT_RUN;
        }
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

// What every player gets, the human too
static void AsAllPlayers(float now)
{
    static float next = 0;
    static std::map<void*, float> nextClaw;
    if (now >= next - 1.0f && now < next)
        return;
    next = now + AS_PLAYERS_EVERY;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        unsigned char* pl = (F<int>(v, EV_FLAGS) & FL_CLIENT) && Alive(e) ? (unsigned char*)PrivateOf(e) : nullptr;
        if (!pl || *(DWORD*)pl != MP_ALLSTARPLAYER_VT + g_mpDelta)
            return;
        int character = *(int*)(pl + AS_CHARACTER);
        AsGroundPlayer(e, pl, character);
        float& claw = nextClaw[e];
        if ((character == AS_ZOMBIE3 || character == AS_ZOMBIE4) && (F<int>(v, AS_EV_BUTTON) & IN_ATTACK) && (now >= claw || claw - now > AS_CLAW_EVERY))
        {
            claw = now + AS_CLAW_EVERY;
            AsClaw(pl);
        }
    });
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
    AsAllPlayers(now);
    static void* ladderChecked = nullptr;
    if (ladderChecked != rules)
    {
        ladderChecked = rules;
        Log("All Star: characters %s", rules[AS_RULES_LADDER] ? "by team (the check didn't apply): turned off" : "drawn for each player");
        rules[AS_RULES_LADDER] = 0;
    }
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
        char one[24];
        snprintf(one, sizeof(one), "%d/%.0f ", q && *(DWORD*)q == MP_ALLSTARPLAYER_VT + g_mpDelta ? *(int*)(q + AS_CHARACTER) : -1, F<float>(Vars(e), EV_MAXSPEED));
        characters += one;
    });
    Log("All Star: %d T and %d CT alive, %d dead; character/max speed: %s", alive[1], alive[2], dead, characters.c_str());
}
