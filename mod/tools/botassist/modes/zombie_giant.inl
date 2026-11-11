// botassist: Zombie Giant (mode 30). Included by botassist.cpp at this point (one translation unit: it uses the shared helpers
// defined before it). Only runs in its own mode: GameMode() == MODE_ZOMBIE_GIANT / ZgActive() at every entry point.

// ---------------------------------------------------------------- Zombie Giant (mode 30) with room bots
// CSOGameRulesZombieBoss (vtable 0x117a7a4c) only counts humans: player vfunc 0x21c ("is a bot", 1 in the bot
// classes' 0x106626c0) makes 0x1091c670 skip bots, so a lone player ends the match (0x1085f660: one human left).
// The mode keeps bots of its own (br_bot soldiers, 3 per giant: 0x1085f850 adds them through the bot manager and
// kicks extra bots, 0x1131a210 picks the bot to kick). Room bots count as these soldiers: the mode asks for no more
// bots (any it asks for become room AI players, extra ones are refused) and never kicks them.
// Humans in this mode are ZombieBossPlayer objects (0x3778 bytes, giant state from +0x3710), bots are CCSBot
// (their own fields from +0x3700): any code that treats a bot as a human there reads and writes bot memory as
// giant state and crashes. So room bots can't be the giant; they answer "not a bot" only where the mode counts
// players (end of match) or picks the bot to kick, and play as the human side.
static const DWORD MP_BOT_VTABLES[] = { 0x1193f25c, 0x1193dcc8, 0x1198f294, 0x1179b8a4 }; // CBot, CCSBot, ZBBot, CPlayRoomBot
static const DWORD MP_ISBOT_TRUE = 0x106626c0;
static const DWORD MP_ZG_KICK_FN = 0x1131a210, MP_ZG_KICK_END = 0x1131a340;
static const DWORD MP_ZG_ISHUMAN_FN = 0x1091c670, MP_ZG_ISHUMAN_END = 0x1091c6b0;
static const DWORD MP_ZG_ENDCHECK_RET = 0x1085f6b8; // 0x1085f660 after its call to 0x1091c670
static const DWORD MP_ZG_RULES_VTABLE = 0x117a7a4c, MP_ZG_EQUIP_SLOT = 98; // 0x10860740: br_bot model + famas/m32
static const DWORD MP_DM_EQUIP = 0x10743240; // the parent's (CSOGameRulesDM) spawn equipment
static DWORD g_zgDelta;

static bool IsRoomBotEdict(void* edict)
{
    for (auto& b : g_bots)
        if (b.edict == edict)
        {
            if (b.roomBot < 0)
                return false;
            EnterCriticalSection(&g_matchLock);
            // a fake player plays its room AI player (weapons) but is a player: the mode's equipment (rules slot 98,
            // 0x10860740) is what turns it into the giant it was picked as (0x1091c9f0: type = next)
            bool ok = b.roomBot < (int)g_matchBots.size() && g_matchBots[b.roomBot].edict == edict &&
                      !g_matchBots[b.roomBot].fakePlayer;
            LeaveCriticalSection(&g_matchLock);
            return ok;
        }
    return false;
}

static bool IsRoomBotPlayer(unsigned char* self)
{
    unsigned char* pev = self ? *(unsigned char**)(self + 8) : nullptr;
    void* edict = pev ? *(void**)(pev + 0x238) : nullptr;
    return edict && IsRoomBotEdict(edict);
}

// mp.dll omits frame pointers: look for a return address in the stack above this call
static bool StackHasReturn(DWORD lo, DWORD hi, int depth)
{
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    DWORD* top = (DWORD*)((NT_TIB*)NtCurrentTeb())->StackBase;
    for (int i = 0; i < depth && sp + i < top; i++)
    {
        DWORD a = sp[i] - g_zgDelta;
        if (a >= lo && a < hi)
            return true;
    }
    return false;
}

static int __fastcall HookedIsBot(unsigned char* self, void*)
{
    if (g_gameMode != MODE_ZOMBIE_GIANT)
        return 1;
    DWORD ret = (DWORD)_ReturnAddress() - g_zgDelta;
    bool counting = (ret >= MP_ZG_KICK_FN && ret < MP_ZG_KICK_END)
        || (ret >= MP_ZG_ISHUMAN_FN && ret < MP_ZG_ISHUMAN_END && StackHasReturn(MP_ZG_ENDCHECK_RET, MP_ZG_ENDCHECK_RET + 1, 64));
    return counting && IsRoomBotPlayer(self) ? 0 : 1;
}

// room bots spawn with the normal equipment (their loadout is bought afterwards), not as br_bot soldiers
typedef void(__thiscall* EquipFn)(void* rules, void* player);
static EquipFn g_zgEquip, g_dmEquip;
static void __fastcall HookedZgEquip(void* rules, void*, unsigned char* player)
{
    if (IsRoomBotPlayer(player))
        g_dmEquip(rules, player);
    else
        g_zgEquip(rules, player);
}

static void PatchSlot(void** slot, void* fn)
{
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, 4, old, &old);
}

static void HookZombieGiant(DWORD delta)
{
    g_zgDelta = delta;
    int n = 0;
    for (DWORD vt : MP_BOT_VTABLES)
    {
        void** slot = (void**)(vt + delta) + 135;
        if (!Readable(slot, 4) || *slot != (void*)(MP_ISBOT_TRUE + delta))
            continue;
        PatchSlot(slot, (void*)HookedIsBot);
        n++;
    }
    void** equip = (void**)(MP_ZG_RULES_VTABLE + delta) + MP_ZG_EQUIP_SLOT;
    if (Readable(equip, 4) && *equip == (void*)(0x10860740 + delta))
    {
        g_zgEquip = (EquipFn)*equip;
        g_dmEquip = (EquipFn)(MP_DM_EQUIP + delta);
        PatchSlot(equip, (void*)HookedZgEquip);
    }
    Log("Zombie Giant: room bots count as players (%d bot classes hooked, equipment %s)", n, g_zgEquip ? "hooked" : "NOT hooked");
}

// ---------------------------------------------------------------- Zombie Giant: fake players who can be the giant
// Bots can't be the giant (see above), so the last ZombieGiantPlayers= of the room's AI players (botassist.ini) join
// as fake *players*: pfnCreateFakeClient, the game's player factory (DLL function 0x113ef1d0: a ZombieBossPlayer in
// this mode) and ClientPutInServer (0x113efbb0), like the bot manager does for its bots but without a bot class.
// The mode counts them as humans, so the giant rotates among you and them (0x10861d50). Nothing runs them, so
// botassist does, once per frame from the bots' RunPlayerMove: as soldiers they keep their distance from the giant
// and shoot it; as the giant they walk to the closest human on the navigation mesh, attack ("BossAttack1/2") and now
// and then use a random skill ("BossSkill_1".."4"): the giant's commands (rules ClientCommand 0x10861490 ->
// 0x10923b60 -> the pawn's handler 0x1090cb50, which checks the stamina and the cool down).
static const DWORD MP_CREATE_PLAYER = 0x113ef1d0, MP_PUT_IN_SERVER = 0x113efbb0;
// "jointeam" only works while the team menu is open: like the bot manager (0x113069d1), set the team, the menu
// state (+0xe10 1, +0xcc0 4) and call HandleMenu_ChooseTeam(player, team) and HandleMenu_ChooseAppearance
static const DWORD MP_CHOOSETEAM = 0x113f2f00;
typedef int (*ChooseTeamFn)(void* player, int team);
static const DWORD MP_ZOMBIEBOSSPLAYER_VT = 0x11958834;
static const DWORD MP_ALLSTARPLAYER_VT = 0x11958ab0; // All Star's fake players (modes/all_star.inl) use this file's machinery too
static const int ENG_CREATEFAKECLIENT = 108, ENG_FREEENTPRIVATEDATA = 70;
static const int ZG_GIANT_TYPE = 0x3710; // ZombieBossPlayer: 1, 2, 4, 6 giants; 0, 3, 5 humans
static const int IN_ATTACK2 = 0x800, IN_USE = 0x20, IN_FORWARD = 0x8, IN_BACK = 0x10, IN_MOVELEFT = 0x200, IN_MOVERIGHT = 0x400;
static const int PLAYER_LAST_MOVEMENT = 0xccc;
// Joining players get maxspeed 1 (ClientPutInServer, like Counter-Strike's freeze) until the game gives them their
// real speed; in this mode the bots and fake players never got it (speed 0..1 while walking). pev->maxspeed is at
// +0x240 (entvars maxspeed, CSO layout); engine function 107 is pfnSetClientMaxspeed(edict, speed).
static const int EV_MAXSPEED = 0x240, ENG_SETCLIENTMAXSPEED = 107;
typedef void (*SetClientMaxspeedFn)(void* edict, float speed);
static SetClientMaxspeedFn g_SetClientMaxspeed;
static float g_zgYourMaxspeed = 0; // yours: once you can move, the freeze is over // idle kick (mp_autokick, 0x114c6b7b) after the rules' idle time

struct ZgPlayer
{
    std::string name;
    int character = 0;
    void* edict = nullptr;
    bool tried = false, wasAlive = false;
    void* target = nullptr;
    std::vector<V3> path;
    size_t wp = 0;
    float yaw = 0, pitch = 0;
    float buyAt = 0, deadSince = 0, nextRespawn = 0, nextTarget = 0, nextPath = 0, nextAttack = 0, nextSkill = 0;
    float nextStrafe = 0, nextFire = 0, sprintUntil = 0, nextSprint = 0, stuckCheck = 0;
    float nextReport = 0, lastThink = 0, lastFwd = 0, lastSide = 0, nextPickReport = 0;
    float nextRanged = 0, aimUntil = 0, nextBreakScan = 0, breakUntil = 0, nextUse = 0, slowSince = 0;
    float busySince = 0, climbUntil = 0;
    int kitType = 0, pending = -1, ladder = -1; // the giant's kit, an aimed move waiting to face the target
    bool pendingSkill = false, simpleGiant = false;
    void* ignoreBreak = nullptr; // a breakable it couldn't break (for a while)
    void* avoidTarget = nullptr; // a target it couldn't reach (for a while)
    float ignoreBreakUntil = 0, avoidUntil = 0, detourUntil = 0, detourYaw = 0, stuckRoundsSince = 0;
    int stuckRounds = 0;
    float nextReload = 0, stockAt = 0, nextSlot = 0;
    int stockId = 0, stockBefore = 0, emptyStage = 0;
    std::vector<int> stocked;             // weapons whose ammunition this life already got
    std::unordered_map<int, int> maxClip; // the fullest clip seen per weapon id
    void* breakTarget = nullptr; // a zg_breakable / func_breakable in the giant's way
    int stuckCount = 0;
    bool lastMove = false;
    V3 stuckFrom{};
    int strafe = 1;
};
static std::vector<ZgPlayer> g_zgPlayers;
static std::vector<std::string> g_zgKickNames; // fake players botassist kicks itself

// everything Zombie Giant only acts in that mode, or while its fake players are still connected (they leave on the
// next map): the other modes' bots never see it
static bool ZgActive() { return g_gameMode == MODE_ZOMBIE_GIANT || !g_zgPlayers.empty(); }
static int g_zgLevel = -1, g_zgMarkLevel = -1;
static float g_zgFrame = -1, g_zgStart = 0;

static float Rnd() { return (float)rand() / RAND_MAX; }

// the last room AI players of the list are played by fake players (once per match, before bots take them)
static void MarkZgPlayers()
{
    bool allStar = GameMode() == MODE_ALLSTAR; // all of them there: only real players get characters
    if ((cfg.zgPlayers == 0 && !allStar) || !g_CreateFakeClient || (GameMode() != MODE_ZOMBIE_GIANT && !allStar) || g_matchFetch != 2 ||
        !g_matchKnown || g_zgMarkLevel == g_nav.level)
        return;
    g_zgMarkLevel = g_nav.level;
    int n = 0;
    EnterCriticalSection(&g_matchLock);
    for (int i = (int)g_matchBots.size() - 1; i >= 0 && (allStar || cfg.zgPlayers < 0 || n < cfg.zgPlayers); i--)
        if (!g_matchBots[i].edict)
        {
            g_matchBots[i].fakePlayer = true;
            n++;
        }
    LeaveCriticalSection(&g_matchLock);
    if (n)
        Log("Zombie Giant: %d room AI players join as players who can be the giant", n);
}

static bool IsBotClass(void* pl)
{
    DWORD vt = pl ? *(DWORD*)pl - g_zgDelta : 0;
    for (DWORD b : MP_BOT_VTABLES)
        if (vt == b)
            return true;
    return false;
}

// a new match: fake players still connected from the last one leave (the server normally drops them)
static void ZgNewLevel()
{
    for (auto& z : g_zgPlayers)
    {
        if (!z.edict)
            continue;
        unsigned char* v = Vars(z.edict);
        void* pl = PrivateOf(z.edict);
        const char* name = String(F<int>(v, EV_NETNAME));
        if (pl && (F<int>(v, EV_FLAGS) & FL_CLIENT) && !IsBotClass(pl) && name && z.name == name && !strchr(name, '"'))
        {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "kick \"%s\"\n", name);
            g_zgKickNames.push_back(name);
            g_ServerCommand(cmd);
            Log("Zombie Giant: '%s' from the last match leaves", name);
        }
    }
    g_zgPlayers.clear();
}

static void CreateZgPlayer(ZgPlayer& z)
{
    z.tried = true;
    void* e = g_CreateFakeClient(z.name.c_str());
    if (!e)
    {
        Log("Zombie Giant: couldn't add the player '%s' (server full?)", z.name.c_str());
        return;
    }
    g_FreeEntPrivateData(e);
    ((EdictFn)(MP_CREATE_PLAYER + g_mpDelta))(e);
    void* pl = PrivateOf(e);
    bool allStar = GameMode() == MODE_ALLSTAR;
    if (!pl || *(DWORD*)pl != (allStar ? MP_ALLSTARPLAYER_VT : MP_ZOMBIEBOSSPLAYER_VT) + g_mpDelta)
    {
        Log("Zombie Giant: '%s' isn't a Zombie Giant player (vtable 0x%08lx): removed", z.name.c_str(),
            pl ? *(DWORD*)pl - g_mpDelta : 0);
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "kick \"%s\"\n", z.name.c_str());
        g_zgKickNames.push_back(z.name);
        g_ServerCommand(cmd);
        return;
    }
    char* info = g_GetInfoKeyBuffer ? g_GetInfoKeyBuffer(e) : nullptr;
    if (info)
        g_SetClientKeyValue(g_IndexOfEdict(e), info, "*bot", "1"); // the scoreboard shows it like the bots
    ((EdictFn)(MP_PUT_IN_SERVER + g_mpDelta))(e);
    F<int>(Vars(e), EV_FLAGS) |= FL_FAKECLIENT;
    unsigned char* p = (unsigned char*)pl;
    int side = 2;
    if (allStar)
    {
        // All Star has two sides: the emptier one
        int count[3] = { 0, 0, 0 };
        ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* o) {
            int t = o != e && (F<int>(Vars(o), EV_FLAGS) & FL_CLIENT) ? TeamOf(o) : 0;
            if (t == 1 || t == 2)
                count[t]++;
        });
        side = count[1] <= count[2] ? 1 : 2;
    }
    *(int*)(p + PLAYER_TEAM) = side;
    *(int*)(p + 0xe10) = 1;
    *(int*)(p + 0xcc0) = 4;
    int joined = ((ChooseTeamFn)(MP_CHOOSETEAM + g_mpDelta))(pl, side);
    int id = z.character > 0 && CharacterFits(z.character, side) ? z.character
             : !cfg.characters[side].empty()                      ? cfg.characters[side][rand() % cfg.characters[side].size()]
                                                                  : 0;
    int number = id ? CharacterClassNumber(id) : 0;
    char cls[16];
    snprintf(cls, sizeof(cls), "%d", number > 0 ? number : 1);
    if (joined)
        ((ChooseAppearanceFn)(MP_CHOOSEAPPEARANCE + g_mpDelta))(pl, number > 0 ? number : 1);
    z.edict = e;
    EnterCriticalSection(&g_matchLock);
    for (size_t i = 0; i < g_matchBots.size(); i++)
        if (g_matchBots[i].fakePlayer && !g_matchBots[i].edict && g_matchBots[i].name == z.name)
        {
            g_matchBots[i].edict = e;
            StateFor(e).roomBot = (int)i;
            break;
        }
    LeaveCriticalSection(&g_matchLock);
    Log("Zombie Giant: '%s' joined as a player who can be the giant (team choice %s, now team %d, character %d, class %s)",
        z.name.c_str(), joined ? "accepted" : "REFUSED", *(int*)(p + PLAYER_TEAM), id, cls);
}

// the closest living player of the other team, visible ones first
static void* ZgTarget(void* e, int team, void* avoid = nullptr)
{
    V3 me = Eye(e);
    void* best = nullptr;
    float bestScore = 1e30f;
    ForEntitiesNear(me, 8000.0f, [&](void* o) {
        if (o == e || o == avoid || !(F<int>(Vars(o), EV_FLAGS) & FL_CLIENT) || !Alive(o))
            return;
        int t = TeamOf(o);
        if ((t != 1 && t != 2) || t == team)
            return;
        float d = Len(Center(o) - me);
        float score = CanSee(e, me, o) ? d : d * 1.5f + 500.0f;
        if (score < bestScore)
        {
            bestScore = score;
            best = o;
        }
    });
    return best;
}

// the yaw to walk at towards a point, along the navigation mesh
static bool ZgPathYaw(ZgPlayer& z, V3 me, V3 goal, float now, float reach, float& yaw)
{
    if (now >= z.nextPath || z.wp >= z.path.size())
    {
        if (!g_nav.ok || !FindPath(me, goal, z.path))
            z.path.assign(1, goal);
        z.wp = 0;
        z.nextPath = now + 1.0f;
    }
    while (z.wp + 1 < z.path.size() && Len2D(z.path[z.wp] - me) < reach)
        z.wp++;
    V3 d = z.path[z.wp] - me;
    if (Len2D(d) < 8.0f)
        return false;
    yaw = YawOf(d);
    return true;
}

// the human side (room bots and fake players) fights together: where they gather, refreshed twice a second
static V3 g_zgRally{};
static int g_zgAllies = 0;
static float g_zgNextRally = 0;
static std::unordered_map<void*, ZgPlayer> g_zgBots; // room bots on the human side, driven like the soldiers

static bool IsZgPlayerClass(void* pl) { return pl && *(DWORD*)pl == MP_ZOMBIEBOSSPLAYER_VT + g_mpDelta; }

static void ZgWatchClient(void* e, unsigned char* pl, float now, bool census);

static bool ZgSwimming(void* e);
static V3 ZgDry(V3 p);
static V3 ZgFeet(void* e);

static void ZgUpdateTeam(float now)
{
    if (now < g_zgNextRally)
        return;
    g_zgNextRally = now + 0.5f;
    static float nextCensus = 0;
    bool census = now >= nextCensus || now < nextCensus - 20.0f;
    if (census)
        nextCensus = now + 15.0f;
    V3 sum{};
    int n = 0;
    float c[3] = { 0, 0, 0 };
    void* e = nullptr;
    for (int guard = 0; guard < 2048; guard++)
    {
        e = g_FindEntityInSphere(e, c, 16384.0f);
        if (!e || !strcmp(ClassName(e), "worldspawn"))
            break;
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            continue;
        unsigned char* pl = (unsigned char*)PrivateOf(e);
        if (!pl)
            continue;
        ZgWatchClient(e, pl, now, census);
        if (IsHuman(e))
            g_zgYourMaxspeed = Alive(e) ? F<float>(Vars(e), EV_MAXSPEED) : g_zgYourMaxspeed;
        if (Alive(e) && *(int*)(pl + PLAYER_TEAM) == 2 && !ZgSwimming(e))
        {
            sum = sum + ZgFeet(e);
            n++;
        }
    }
    g_zgAllies = n;
    if (n)
        g_zgRally = ZgDry(sum * (1.0f / n));
}

// a soldier left at the joining speed (1) once you can move gets a rifleman's speed
static void ZgUnfreeze(void* e, const char* who)
{
    float speed = F<float>(Vars(e), EV_MAXSPEED);
    if (!g_SetClientMaxspeed || speed <= 0.0f || speed >= 50.0f || g_zgYourMaxspeed < 100.0f)
        return;
    g_SetClientMaxspeed(e, 240.0f);
    static int reports = 0;
    if (reports++ < 20)
        Log("Zombie Giant: %s '%s' had maxspeed %.0f (you %.0f): set to 240 -> %.0f", who,
            String(F<int>(Vars(e), EV_NETNAME)), speed, g_zgYourMaxspeed, F<float>(Vars(e), EV_MAXSPEED));
}

// What only the giants can break (zg_breakable: forklifts, containers..., 136 of them on zg_splash) and the
// ordinary func_breakable stop a giant that walks into them. When the giant hardly moves and one is right in front
// of it (within its reach, towards where it walks), it faces it and hits it (BossAttack1) until it breaks.
static bool ZgBreakable(void* o)
{
    const char* cls = ClassName(o);
    unsigned char* v = Vars(o);
    return cls && (!strcmp(cls, "zg_breakable") || !strcmp(cls, "func_breakable")) && F<float>(v, EV_HEALTH) > 0.0f &&
           F<float>(v, EV_TAKEDAMAGE) != 0.0f && F<int>(v, EV_SOLID) != 0; // broken ones are left not solid
}

static void ZgBreakThrough(void* e, ZgPlayer& z, V3 me, bool move, float moveYaw, float now, float& lookYaw,
                           float& lookPitch)
{
    unsigned char* v = Vars(e);
    float speed = Len2D(Vec((float*)(v + EV_VELOCITY)));
    if (!move || speed > 90.0f)
        z.slowSince = now;
    if (z.breakTarget && (now >= z.breakUntil || !ZgBreakable(z.breakTarget)))
    {
        static int reports = 0;
        if (reports++ < 30)
            Log("Zombie Giant: '%s' (giant) %s the %s in its way", String(F<int>(v, EV_NETNAME)),
                ZgBreakable(z.breakTarget) ? "gives up on" : "broke", ClassName(z.breakTarget));
        if (ZgBreakable(z.breakTarget))
        {
            z.ignoreBreak = z.breakTarget; // it doesn't break (or isn't really in the way): not again soon
            z.ignoreBreakUntil = now + 20.0f;
        }
        z.breakTarget = nullptr;
        z.nextPath = 0;
    }
    if (!z.breakTarget && move && now - z.slowSince > 0.4f && now >= z.nextBreakScan)
    {
        z.nextBreakScan = now + 0.3f;
        V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
        float radius = (hi.x - lo.x) * 0.5f;
        V3 c = (lo + hi) * 0.5f;
        float best = 1e30f;
        ForEntitiesNear(c, radius + 400.0f, [&](void* o) {
            if (o == e || !ZgBreakable(o) || (o == z.ignoreBreak && now < z.ignoreBreakUntil))
                return;
            unsigned char* ov = Vars(o);
            V3 olo = Vec((float*)(ov + EV_ABSMIN)), ohi = Vec((float*)(ov + EV_ABSMAX));
            V3 oc = (olo + ohi) * 0.5f;
            if (ohi.z < lo.z + 8.0f || olo.z > hi.z) // under its feet or over its head
                return;
            // the gap between the two boxes, sideways
            float gx = fmaxf(0.0f, fmaxf(olo.x - hi.x, lo.x - ohi.x));
            float gy = fmaxf(0.0f, fmaxf(olo.y - hi.y, lo.y - ohi.y));
            float gap = sqrtf(gx * gx + gy * gy);
            if (gap > 120.0f || fabsf(AngleDiff(YawOf(oc - c), moveYaw)) > 70.0f)
                return;
            if (gap < best)
            {
                best = gap;
                z.breakTarget = o;
            }
        });
        if (z.breakTarget)
        {
            z.breakUntil = now + 6.0f;
            static int reports = 0;
            if (reports++ < 30)
                Log("Zombie Giant: '%s' (giant) is blocked by a %s (health %.0f, %.0f units away): breaking it",
                    String(F<int>(v, EV_NETNAME)), ClassName(z.breakTarget), F<float>(Vars(z.breakTarget), EV_HEALTH), best);
        }
    }
    if (!z.breakTarget)
        return;
    V3 oc = Center(z.breakTarget), eye = Eye(e);
    lookYaw = YawOf(oc - eye);
    lookPitch = PitchOf(oc - eye) * 0.5f;
    if (fabsf(AngleDiff(lookYaw, z.yaw)) < 25.0f && now >= z.nextAttack)
    {
        FakeCommand(e, "BossAttack1");
        z.nextAttack = now + 0.9f + Rnd() * 0.4f;
    }
}

// ---------------------------------------------------------------- Zombie Giant: water and ladders
// The middle of the Zombie Giant maps is a water channel (zg_splash: func_water from z -256 up to -92, with a
// trigger_push current) left only by a few ladders (func_ladder), which the navigation mesh doesn't link: soldiers
// who went in stayed there. The water and the ladders are read from the map's entities once per match; the soldiers'
// paths avoid the water, their goals (the giant, the group) are moved to the closest dry ground, and a soldier in the
// water swims to the closest ladder out and climbs it (looking up, walking forward).
struct ZgBox
{
    V3 lo, hi;
};
static std::vector<ZgBox> g_zgWater, g_zgLadders;
static std::vector<char> g_navWet; // per navigation area: its floor is under water (FindPath avoids it)
static int g_zgWorldLevel = -1, g_navWetLevel = -1;

static bool InWaterBox(const ZgBox& w, V3 p, float below)
{
    return p.x >= w.lo.x && p.x <= w.hi.x && p.y >= w.lo.y && p.y <= w.hi.y && p.z <= w.hi.z - below &&
           p.z >= w.lo.z - 80.0f;
}

// a point on the floor (feet) under water
static bool ZgWet(V3 p)
{
    for (auto& w : g_zgWater)
        if (InWaterBox(w, p, 4.0f))
            return true;
    return false;
}

static V3 ZgFeet(void* e)
{
    V3 o = Origin(e);
    o.z = F<float>(Vars(e), EV_ABSMIN + 8);
    return o;
}

// the floor `dist` units away towards yaw is under water (or a drop into it)
static bool ZgWetAhead(void* e, float yaw, float dist)
{
    if (g_zgWater.empty())
        return false;
    V3 me = Origin(e);
    V3 p = me + V3{ cosf(yaw / RAD), sinf(yaw / RAD), 0 } * dist;
    TraceResult tr;
    Trace(p, p + V3{ 0, 0, -800.0f }, e, 1, tr);
    return ZgWet(Vec(tr.vecEndPos));
}

static void ZgScanWorld()
{
    if (g_zgWorldLevel == g_nav.level)
        return;
    g_zgWorldLevel = g_nav.level;
    g_zgWater.clear();
    g_zgLadders.clear();
    ForEntitiesNear({ 0, 0, 0 }, 32768.0f, [&](void* o) {
        const char* cls = ClassName(o);
        bool water = cls && !strcmp(cls, "func_water"), ladder = cls && !strcmp(cls, "func_ladder");
        if (!water && !ladder)
            return;
        unsigned char* v = Vars(o);
        ZgBox b{ Vec((float*)(v + EV_ABSMIN)), Vec((float*)(v + EV_ABSMAX)) };
        (water ? g_zgWater : g_zgLadders).push_back(b);
    });
    g_navWet.assign(g_nav.areas.size(), 0);
    int wet = 0;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
        wet += g_navWet[i] = ZgWet(g_nav.areas[i].center);
    g_navWetLevel = g_nav.level;
    Log("Zombie Giant: %d water volumes, %d ladders, %d of %d navigation areas under water", (int)g_zgWater.size(),
        (int)g_zgLadders.size(), wet, (int)g_nav.areas.size());
}

// extra path cost of an area under water (only while the Zombie Giant water is known)
static float NavWetCost(int area)
{
    return g_navWetLevel == g_nav.level && area < (int)g_navWet.size() && g_navWet[area] ? 2000.0f : 0.0f;
}

// the closest dry ground to a point under water (the point itself when it's dry)
static V3 ZgDry(V3 p)
{
    if (!ZgWet(p) || g_navWet.size() != g_nav.areas.size())
        return p;
    float best = 1e30f;
    V3 out = p;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        if (g_navWet[i])
            continue;
        float d = Len(g_nav.areas[i].center - p);
        if (d < best)
        {
            best = d;
            out = g_nav.areas[i].center;
        }
    }
    return out;
}

static bool ZgSwimming(void* e)
{
    return (F<int>(Vars(e), EV_FLAGS) & FL_INWATER) || ZgWet(ZgFeet(e));
}

// a soldier in the water: to the closest ladder that leaves it (or the closest dry ground), and up the ladder.
// true: fwd / look set
static bool ZgLeaveWater(void* e, ZgPlayer& z, float now, float& moveYaw, float& lookYaw, float& lookPitch, bool& move)
{
    V3 me = Origin(e);
    bool swimming = ZgSwimming(e);
    bool climbing = now < z.climbUntil;
    if (!swimming && !climbing)
    {
        z.ladder = -1;
        return false;
    }
    if (z.ladder < 0 || z.ladder >= (int)g_zgLadders.size())
    {
        float best = 1e30f;
        for (size_t i = 0; i < g_zgLadders.size(); i++)
        {
            const ZgBox& l = g_zgLadders[i];
            V3 foot{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, l.lo.z };
            bool fromWater = false;
            for (auto& w : g_zgWater) // its foot is in the water, its top above the surface
                fromWater |= InWaterBox(w, foot, 0.0f) && l.hi.z > w.hi.z;
            float d = Len2D(foot - me);
            if (fromWater && d < best)
            {
                best = d;
                z.ladder = (int)i;
            }
        }
        static int reports = 0;
        if (reports++ < 30)
            Log("Zombie Giant: '%s' is in the water: %s", String(F<int>(Vars(e), EV_NETNAME)),
                z.ladder >= 0 ? "heading for a ladder" : "no ladder out, heading for dry ground");
    }
    move = true;
    if (z.ladder < 0)
    {
        moveYaw = YawOf(ZgDry(me) - me);
        lookYaw = moveYaw;
        lookPitch = 0;
        return true;
    }
    const ZgBox& l = g_zgLadders[z.ladder];
    V3 c{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, me.z };
    float gx = fmaxf(0.0f, fmaxf(l.lo.x - me.x, me.x - l.hi.x)), gy = fmaxf(0.0f, fmaxf(l.lo.y - me.y, me.y - l.hi.y));
    moveYaw = YawOf(c - me);
    lookYaw = moveYaw;
    if (sqrtf(gx * gx + gy * gy) < 40.0f || climbing)
    {
        // on the ladder: look up and walk into it; a little more once out of the water to step off at the top
        lookPitch = -70.0f;
        if (me.z < l.hi.z - 8.0f)
            z.climbUntil = now + 0.8f;
    }
    else
        lookPitch = 0;
    return true;
}

// ---------------------------------------------------------------- Zombie Giant: each giant's moves
// The giant's commands become monster states (pawn vfunc +0x20; the states are registered in 0x1091b060):
//   Dione (type 1): Attack1 light, Attack2 heavy (a whirl), Skill1 rolling charge, Skill2 poison missile,
//                   Skill3 tentacles, Skill4 (evolved) air strike
//   Titan (2):      Attack1 light, Attack2 heavy, Skill1 rolling charge, Skill2 rockets, Skill3 mines,
//                   Skill4 (evolved) artillery
//   Revenant (4):   Attack1 light, Attack2 its heavy attack, Skill1 rolling charge, Skill2 fireball, Skill3 inferno
//                   (around itself), Skill4 (evolved) hellfire from the sky
//   Angra (6):      the dragon: Attack1 light, Attack2 heavy, Skill1 its rolling attack (comes down at the target),
//                   Skill2 breath, Skill3 tentacles, Skill4 (evolved) poison fog
// The pawn (0x109201f0, fastcall player: the pawn of the current type) holds the stamina (+0xc), the evolution
// points (+0x10, 10 to evolve), the evolved flag (+0x14) and, per skill slot 1..4, "ready" (+0x14 + slot) and the
// cool down's end (+0x28 + 4 * slot). Each state's data (0x1090c770(state): [0] skill slot, [5] cool down,
// [6] stamina cost) is what 0x1090e1f0 checks before switching to it. The current state is the state manager's
// (player +0x3738, an object inside the player) vfunc +0x14; 0 is idle. The giant uses a skill as soon as it's ready, it can pay for it and the
// target fits it, and attacks in between.
enum ZgRole
{
    ZG_MELEE,      // within reach
    ZG_HEAVY,      // within reach, slower and stronger
    ZG_MID,        // aimed, 250..900
    ZG_CHARGE,     // aimed, 280..1000: closes the gap
    ZG_PROJECTILE, // aimed, 350..2600
    ZG_AREA,       // aimed at the target, 200..2200
    ZG_SELF,       // around the giant: humans close by
    ZG_CONE,       // aimed, up to 750
};
struct ZgMove
{
    const char* cmd;
    int state, evolved; // -1: not available
    ZgRole role;
};
struct ZgKit
{
    int type;
    const char* name;
    ZgMove moves[6]; // Attack1, Attack2, Skill1..4
};
static const ZgKit g_zgKits[] = {
    { 1, "Dione", { { "BossAttack1", 3, 0x26, ZG_MELEE }, { "BossAttack2", 4, 0x27, ZG_MID }, { "BossSkill_1", 5, 5, ZG_CHARGE },
                    { "BossSkill_2", 6, 0x28, ZG_PROJECTILE }, { "BossSkill_3", 7, 0x29, ZG_AREA },
                    { "BossSkill_4", -1, 0x2a, ZG_AREA } } },
    { 2, "Titan", { { "BossAttack1", 10, 0x2b, ZG_MELEE }, { "BossAttack2", 11, 0x2c, ZG_HEAVY }, { "BossSkill_1", 0xc, 0xc, ZG_CHARGE },
                    { "BossSkill_2", 0xd, 0x2d, ZG_PROJECTILE }, { "BossSkill_3", 0xe, 0x2e, ZG_SELF },
                    { "BossSkill_4", -1, 0x2f, ZG_AREA } } },
    { 4, "Revenant", { { "BossAttack1", 0x14, 0x30, ZG_MELEE }, { "BossAttack2", 0x15, 0x31, ZG_MID },
                       { "BossSkill_1", 0x16, 0x16, ZG_CHARGE }, { "BossSkill_2", 0x17, 0x32, ZG_PROJECTILE },
                       { "BossSkill_3", 0x18, 0x33, ZG_SELF }, { "BossSkill_4", -1, 0x34, ZG_AREA } } },
    { 6, "Angra", { { "BossAttack1", 0x20, 0x35, ZG_MELEE }, { "BossAttack2", 0x21, 0x36, ZG_HEAVY },
                    { "BossSkill_1", 0x22, 0x22, ZG_CHARGE }, { "BossSkill_2", 0x23, 0x37, ZG_CONE },
                    { "BossSkill_3", 0x24, 0x38, ZG_AREA }, { "BossSkill_4", -1, 0x39, ZG_SELF } } },
};
static const DWORD MP_ZG_PAWN = 0x109201f0, MP_ZG_STATEINFO = 0x1090c770;
static const DWORD MP_GIANT_PAWN_VTABLES[] = { 0x117c18e8, 0x117c191c, 0x117c1950, 0x117c1984 };
typedef unsigned char* (__fastcall* PawnOfFn)(void* player);
typedef int* (*StateInfoFn)(int state);
typedef int(__fastcall* CurrentStateFn)(void* manager, void* edx);

struct ZgPawn
{
    bool ok = false;
    float stamina = 0;
    int evolution = 0, state = 0;
    bool evolved = false;
    bool ready[5] = {};
};

static bool ReadZgPawn(unsigned char* pl, ZgPawn& out)
{
    __try
    {
        unsigned char* pawn = ((PawnOfFn)(MP_ZG_PAWN + g_mpDelta))(pl);
        if (!pawn)
            return false;
        DWORD vt = *(DWORD*)pawn - g_mpDelta;
        bool giant = false;
        for (DWORD g : MP_GIANT_PAWN_VTABLES)
            giant |= vt == g;
        if (!giant)
            return false;
        out.stamina = *(float*)(pawn + 0xc);
        out.evolution = *(int*)(pawn + 0x10);
        out.evolved = pawn[0x14] != 0;
        for (int s = 1; s <= 4; s++)
            out.ready[s] = pawn[0x14 + s] != 0;
        unsigned char* manager = pl + 0x3738; // the state manager is part of the player (its vtable is there)
        out.state = ((CurrentStateFn)(*(void***)manager)[0x14 / 4])(manager, nullptr);
        out.ok = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// can the giant switch to this state now: its skill slot ready and the stamina to pay for it
static bool ZgAffordable(const ZgPawn& p, int state)
{
    __try
    {
        int* info = ((StateInfoFn)(MP_ZG_STATEINFO + g_mpDelta))(state);
        int slot = info[0];
        if (slot >= 1 && slot <= 4 && !p.ready[slot])
            return false;
        return *(float*)&info[6] <= p.stamina;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// dist: from the giant's eyes; flat: sideways only (the giant is tall: a human at its feet is far from its eyes)
static bool ZgRoleFits(ZgRole role, float dist, float flat, bool visible, int near450, int near700)
{
    switch (role)
    {
    case ZG_MELEE: return visible && flat < 320.0f;
    case ZG_HEAVY: return visible && flat < 360.0f;
    case ZG_MID: return visible && dist > 250.0f && dist < 900.0f;
    case ZG_CHARGE: return visible && dist > 280.0f && dist < 1000.0f;
    case ZG_PROJECTILE: return visible && dist > 350.0f && dist < 2600.0f;
    case ZG_AREA: return visible && dist > 200.0f && dist < 2200.0f;
    case ZG_SELF: return near450 >= 1 || near700 >= 2;
    case ZG_CONE: return visible && flat < 750.0f;
    }
    return false;
}

static bool ZgAimed(ZgRole role) { return role != ZG_SELF; }

// the giant's fighting: picks a move (skills first), turns to the target if the move is aimed, and uses it
static void ZgGiantCombat(void* e, ZgPlayer& z, unsigned char* pl, float dist, bool visible, V3 tp, V3 eye, float now,
                          float& lookYaw, float& lookPitch, unsigned short& buttons)
{
    int type = *(int*)(pl + ZG_GIANT_TYPE);
    const ZgKit* kit = nullptr;
    for (auto& k : g_zgKits)
        if (k.type == type)
            kit = &k;
    ZgPawn p;
    const char* name = String(F<int>(Vars(e), EV_NETNAME));
    float flat = Len2D(tp - eye);
    if (!kit || !ReadZgPawn(pl, p))
    {
        // the pawn couldn't be read: plain attacks within reach and a random skill now and then
        if (!z.simpleGiant)
        {
            z.simpleGiant = true;
            Log("Zombie Giant: '%s' (type %d): the pawn couldn't be read, simple attacks", name, type);
        }
        if (visible && flat < 320.0f && now >= z.nextAttack)
        {
            FakeCommand(e, Rnd() < 0.7f ? "BossAttack1" : "BossAttack2");
            z.nextAttack = now + 0.8f + Rnd() * 0.5f;
        }
        else if (visible && dist < 2200.0f && now >= z.nextSkill)
        {
            char skill[16];
            snprintf(skill, sizeof(skill), "BossSkill_%d", 1 + rand() % 3);
            FakeCommand(e, skill);
            z.nextSkill = now + 3.0f + Rnd() * 3.0f;
        }
        return;
    }
    if (z.kitType != type)
    {
        z.kitType = type;
        z.pending = -1;
        Log("Zombie Giant: '%s' is %s (stamina %.0f, evolution %d%s)", name, kit->name, p.stamina, p.evolution,
            p.evolved ? ", evolved" : "");
    }
    // evolving: with 10 points the pawn evolves when "use" is pressed (0x1090d290: player +0xec0, the buttons
    // pressed this frame, & IN_USE), which unlocks Skill4 and stronger moves
    if (!p.evolved && p.evolution >= 10 && now >= z.nextUse)
    {
        buttons |= IN_USE;
        z.nextUse = now + 1.0f;
        static int reports = 0;
        if (reports++ < 20)
            Log("Zombie Giant: '%s' (%s) presses use to evolve", name, kit->name);
    }
    // busy with a move: wait (unless a state seems stuck)
    if (p.state != 0)
    {
        if (z.busySince <= 0)
            z.busySince = now;
        if (now - z.busySince < 5.0f)
            return;
    }
    else
        z.busySince = 0;
    int near450 = 0, near700 = 0;
    ForEntitiesNear(Origin(e), 700.0f, [&](void* o) {
        if (o == e || !(F<int>(Vars(o), EV_FLAGS) & FL_CLIENT) || !Alive(o) || TeamOf(o) != 2)
            return;
        float d = Len(Center(o) - Center(e));
        near450 += d < 450.0f;
        near700 += d < 700.0f;
    });
    // a pending aimed move: fire once facing the target, give up after a moment
    if (z.pending >= 0)
    {
        const ZgMove& m = kit->moves[z.pending];
        if (!visible || now >= z.aimUntil)
            z.pending = -1;
        else if (fabsf(AngleDiff(lookYaw, z.yaw)) < (m.role == ZG_PROJECTILE ? 6.0f : 12.0f))
        {
            FakeCommand(e, m.cmd);
            static int reports = 0;
            if (reports++ < 80)
                Log("Zombie Giant: '%s' (%s) uses %s at %.0f units (stamina %.0f)", name, kit->name, m.cmd, dist, p.stamina);
            z.pending = -1;
            z.nextAttack = now + (z.pendingSkill ? 0.4f : 0.7f + Rnd() * 0.4f);
            if (!z.pendingSkill)
                z.nextRanged = now + 4.0f + Rnd() * 5.0f;
        }
        return;
    }
    if (now < z.nextAttack)
        return;
    // the skills that fit, at random
    int choice = -1, fits = 0;
    for (int i = 2; i < 6; i++)
    {
        const ZgMove& m = kit->moves[i];
        int state = p.evolved ? m.evolved : m.state;
        if (state < 0 || !ZgRoleFits(m.role, dist, flat, visible, near450, near700) || !ZgAffordable(p, state))
            continue;
        if (rand() % ++fits == 0)
            choice = i;
    }
    bool skill = choice >= 0;
    if (!skill)
    {
        // attacks: within reach the light one mostly; Attack2 from afar now and then when it's a mid-range one
        const ZgMove& a1 = kit->moves[0];
        const ZgMove& a2 = kit->moves[1];
        bool a2fits = ZgRoleFits(a2.role, dist, flat, visible, near450, near700) &&
                      (a2.role != ZG_MID || flat < 330.0f || now >= z.nextRanged);
        if (ZgRoleFits(a1.role, dist, flat, visible, near450, near700))
            choice = a2fits && Rnd() < 0.35f ? 1 : 0;
        else if (a2fits)
            choice = 1;
        if (choice >= 0 && !ZgAffordable(p, p.evolved ? kit->moves[choice].evolved : kit->moves[choice].state))
            choice = -1;
    }
    if (choice < 0)
        return;
    const ZgMove& m = kit->moves[choice];
    if (ZgAimed(m.role) && flat > 330.0f)
    {
        z.pending = choice; // turn first (ZgThink looks at the target while one is pending)
        z.pendingSkill = skill;
        z.aimUntil = now + 1.2f;
        return;
    }
    FakeCommand(e, m.cmd);
    if (skill)
    {
        static int reports = 0;
        if (reports++ < 80)
            Log("Zombie Giant: '%s' (%s) uses %s at %.0f units (stamina %.0f, %d humans close)", name, kit->name,
                m.cmd, dist, p.stamina, near700);
    }
    z.nextAttack = now + (skill ? 0.4f : 0.7f + Rnd() * 0.4f);
}

// ---------------------------------------------------------------- Zombie Giant: ammunition
// The soldiers' weapon (from CBasePlayerWeapon::UpdateClientData 0x11533920 and the AmmoX loop in 0x114df514):
// player +0x101c the active item; weapon +0xe4 its id, +0x158 the primary ammo type, +0x160 the clip; player
// +0x1034 the reserve per ammo type (0x102 types).
// Each weapon a soldier holds for the first time in a life gets its ammunition bought ("primammo" / "secammo",
// the mode's money is topped up); when the buy gives nothing (the room's weapons handed over directly have none),
// the reserve is filled directly: three clips. A soldier reloads when its clip is empty, and between fights when
// it's below half. With nothing left it takes its pistol, then its knife. (The zs_supply boxes need a charge earned
// by dealing damage: the soldiers don't go there.)
struct ZgGun
{
    bool ok = false;
    int id = 0, clip = 0, reserve = 0;
    int* reservePtr = nullptr;
};

static ZgGun ZgWeapon(void* e)
{
    ZgGun g;
    unsigned char* pl = (unsigned char*)PrivateOf(e);
    if (!pl || !Readable(pl + 0x101c, 4))
        return g;
    unsigned char* w = *(unsigned char**)(pl + 0x101c);
    if (!w || !Readable(w + 0xe4, 4) || !Readable(w + 0x158, 12))
        return g;
    int type = *(int*)(w + 0x158);
    g.clip = *(int*)(w + 0x160);
    g.id = *(int*)(w + 0xe4);
    if (type <= 0 || type >= 0x102 || g.clip < 0 || !Readable(pl + 0x1034 + 4 * type, 4))
        return g; // a knife or a grenade
    g.reservePtr = (int*)(pl + 0x1034 + 4 * type);
    g.reserve = *g.reservePtr;
    g.ok = true;
    return g;
}

// reloading and stocking up (buttons, commands); holdFire while the clip is empty
static void ZgAmmo(void* e, ZgPlayer& z, float now, bool fighting, unsigned short& buttons, bool& holdFire)
{
    ZgGun g = ZgWeapon(e);
    if (!g.ok)
        return;
    const char* name = String(F<int>(Vars(e), EV_NETNAME));
    int& top = z.maxClip[g.id];
    top = g.clip > top ? g.clip : top;
    // a weapon held for the first time in this life: its ammunition
    if (std::find(z.stocked.begin(), z.stocked.end(), g.id) == z.stocked.end())
    {
        int want = top * 3 < 30 ? 30 : top * 3 > 300 ? 300 : top * 3;
        if (z.stockId != g.id)
        {
            z.stockId = g.id;
            z.stockAt = now + 0.6f;
            z.stockBefore = g.reserve;
            unsigned char* pl = (unsigned char*)PrivateOf(e);
            if (pl && *(int*)(pl + 0xca8) < 16000)
                *(int*)(pl + 0xca8) = 16000;
            if (g.reserve < want)
                for (int i = 0; i < 3; i++)
                {
                    FakeCommand(e, "primammo");
                    FakeCommand(e, "secammo");
                }
        }
        else if (now >= z.stockAt)
        {
            bool bought = g.reserve > z.stockBefore;
            if (g.reserve < want)
                *g.reservePtr = want; // nothing to buy (or not enough): handed over like the room's weapons
            z.stocked.push_back(g.id);
            z.stockId = 0;
            static int reports = 0;
            if (reports++ < 40)
                Log("Zombie Giant: '%s' ammunition for weapon %d: %d -> %d (%s)", name, g.id, z.stockBefore, *g.reservePtr,
                    bought ? "bought" : "given");
            g.reserve = *g.reservePtr;
        }
    }
    holdFire = g.clip == 0;
    if (g.reserve > 0 && now >= z.nextReload && (g.clip == 0 || (!fighting && g.clip < top / 2)))
    {
        buttons |= IN_RELOAD;
        z.nextReload = now + (g.clip == 0 ? 0.6f : 2.5f);
    }
    if (g.clip == 0 && g.reserve == 0 && now >= z.nextSlot)
    {
        // empty: the pistol, then the knife
        z.emptyStage++;
        FakeCommand(e, z.emptyStage == 1 ? "slot2" : "slot3");
        z.nextSlot = now + 1.0f;
        static int reports = 0;
        if (reports++ < 30)
            Log("Zombie Giant: '%s' is out of ammunition (weapon %d): %s", name, g.id,
                z.emptyStage == 1 ? "the pistol" : "the knife");
    }
}

// one frame of a Zombie Giant fighter: the giant hunts the closest human, soldiers gather and shoot the giant
static void ZgThink(void* e, ZgPlayer& z, bool giant, int team, float now, float dt, float& fwd, float& side,
                    unsigned short& buttons)
{
    if (now >= z.nextTarget || (z.target && !Alive(z.target)))
    {
        z.target = ZgTarget(e, team, now < z.avoidUntil ? z.avoidTarget : nullptr);
        if (!z.target)
            z.target = ZgTarget(e, team);
        z.nextTarget = now + 0.4f;
    }
    V3 me = Origin(e), eye = Eye(e);
    float lookYaw = z.yaw, lookPitch = 0, moveYaw = z.yaw;
    bool move = false;
    bool leaving = !giant && !g_zgWater.empty() && ZgLeaveWater(e, z, now, moveYaw, lookYaw, lookPitch, move);
    bool holdFire = false;
    if (!giant)
        ZgAmmo(e, z, now, z.target && CanSee(e, eye, z.target), buttons, holdFire);
    if (z.target && !leaving)
    {
        V3 tp = Center(z.target);
        float dist = Len(tp - eye);
        bool visible = CanSee(e, eye, z.target);
        if (giant)
        {
            move = dist > 140.0f && ZgPathYaw(z, me, Origin(z.target), now, 90.0f, moveYaw);
            // aimed moves (ZgGiantCombat) turn the giant to the target first, a little high the farther it is
            bool aiming = z.pending >= 0 && visible;
            V3 aimAt = tp;
            aimAt.z += dist > 600.0f ? (dist - 600.0f) * 0.06f : 0.0f;
            lookYaw = (dist < 600.0f && visible) || aiming ? YawOf(aimAt - eye) : move ? moveYaw : z.yaw;
            lookPitch = (dist < 600.0f && visible) || aiming ? PitchOf(aimAt - eye) : 0;
            ZgGiantCombat(e, z, (unsigned char*)PrivateOf(e), dist, visible, tp, eye, now, lookYaw, lookPitch, buttons);
            if (dist > 1500.0f && now >= z.nextSprint)
            {
                z.nextSprint = now + 4.0f;
                if (Rnd() < 0.3f)
                    z.sprintUntil = now + 2.0f;
            }
            if (now < z.sprintUntil)
                buttons |= IN_ATTACK2;
        }
        else
        {
            // soldiers stay with the group (rally point) on the way, then keep 450..850 units from the giant,
            // strafing, and shoot it
            bool engaged = visible && dist < 1100.0f;
            float fromGroup = g_zgAllies > 1 ? Len2D(g_zgRally - me) : 0.0f;
            V3 goal = ZgDry(ZgFeet(z.target)); // a giant in the water: from the bank
            if (!engaged && fromGroup > 500.0f && Len(g_zgRally - Origin(z.target)) < Len(me - Origin(z.target)) + 300.0f)
                move = ZgPathYaw(z, me, g_zgRally, now, 40.0f, moveYaw);
            else if (!engaged || dist > 850.0f)
                move = ZgPathYaw(z, me, goal, now, 40.0f, moveYaw);
            else if (dist < 450.0f)
            {
                moveYaw = YawOf(me - Origin(z.target));
                move = !ZgWetAhead(e, moveYaw, 150.0f); // backing off, but not into the water
            }
            lookYaw = visible ? YawOf(tp - eye) : move ? moveYaw : z.yaw;
            lookPitch = visible ? PitchOf(tp - eye) : 0;
            if (now >= z.nextStrafe)
            {
                z.strafe = -z.strafe;
                z.nextStrafe = now + 0.8f + Rnd() * 1.4f;
            }
            if (visible && !holdFire && dist < 3000.0f && fabsf(AngleDiff(lookYaw, z.yaw)) < 8.0f && now >= z.nextFire)
            {
                buttons |= IN_ATTACK;
                z.nextFire = now + 0.12f;
            }
            if (engaged && ZgWetAhead(e, z.yaw - 90.0f * z.strafe, 120.0f)) // strafing, not into the water
                z.strafe = -z.strafe;
            if (engaged)
                side += z.strafe * 250.0f;
        }
    }
    else if (!giant && !leaving && g_zgAllies > 1 && Len2D(g_zgRally - me) > 300.0f)
        move = ZgPathYaw(z, me, g_zgRally, now, 40.0f, moveYaw); // nobody to fight: gather
    if (giant)
        ZgBreakThrough(e, z, me, move, moveYaw, now, lookYaw, lookPitch);
    if (now < z.detourUntil)
    {
        moveYaw = z.detourYaw; // around what blocked it
        move = true;
    }
    if (!giant && move && !leaving && ZgWetAhead(e, moveYaw, 70.0f))
    {
        move = false; // the next step is a drop into the water: another way
        z.nextPath = 0;
    }
    float turn = (giant ? 240.0f : 420.0f) * dt;
    z.yaw = Approach(z.yaw, move && !z.target && !z.breakTarget ? moveYaw : lookYaw, turn);
    float k = dt * 6.0f > 1.0f ? 1.0f : dt * 6.0f;
    z.pitch += (lookPitch - z.pitch) * k;
    if (move)
    {
        float rel = AngleDiff(moveYaw, z.yaw) / RAD;
        fwd += cosf(rel) * 400.0f;
        side += -sinf(rel) * 400.0f;
    }
    // the movement keys too, like a player's client: the mode's movement code reads them
    buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
    if (fwd > 50.0f)
        buttons |= IN_FORWARD;
    else if (fwd < -50.0f)
        buttons |= IN_BACK;
    if (side > 50.0f)
        buttons |= IN_MOVERIGHT;
    else if (side < -50.0f)
        buttons |= IN_MOVELEFT;
    z.lastFwd = fwd;
    z.lastSide = side;
    z.lastMove = move;
    // stuck twice in a row (2 s): a jump and another way round
    if (now >= z.stuckCheck)
    {
        if (move && Len2D(me - z.stuckFrom) < 30.0f && fabsf(me.z - z.stuckFrom.z) < 30.0f) // climbing isn't stuck
            z.stuckCount++;
        else
            z.stuckCount = 0;
        if (z.stuckCount >= 2 && !(giant && z.breakTarget)) // (a giant breaking what's in the way isn't stuck)
        {
            // stuck again and again: a jump, then a way round sideways, then another target
            if (now - z.stuckRoundsSince > 12.0f)
            {
                z.stuckRounds = 0;
                z.stuckRoundsSince = now;
            }
            z.stuckRounds++;
            if (z.stuckRounds == 1)
            {
                if (giant)
                    FakeCommand(e, "BossJump");
                else
                    buttons |= IN_JUMP;
            }
            else if (z.stuckRounds == 2 || !z.target)
            {
                z.detourYaw = z.yaw + (Rnd() < 0.5f ? 100.0f : -100.0f);
                z.detourUntil = now + 1.5f;
            }
            else
            {
                z.avoidTarget = z.target;
                z.avoidUntil = now + 10.0f;
                z.nextTarget = 0;
                z.stuckRounds = 0;
                static int reports = 0;
                if (reports++ < 30)
                    Log("Zombie Giant: '%s' can't get to '%s': another target for a while",
                        String(F<int>(Vars(e), EV_NETNAME)), String(F<int>(Vars(z.target), EV_NETNAME)));
            }
            z.strafe = -z.strafe;
            z.nextPath = 0;
            z.stuckCount = 0;
        }
        z.stuckFrom = me;
        z.stuckCheck = now + 1.0f;
    }
}

static void ZgSpawned(void* e, ZgPlayer& z, float now)
{
    z.wasAlive = true;
    z.buyAt = now + 0.7f;
    z.path.clear();
    z.target = nullptr;
    z.nextSkill = now + 4.0f + Rnd() * 6.0f;
    z.nextRanged = now + 3.0f + Rnd() * 5.0f;
    z.aimUntil = 0;
    z.breakTarget = nullptr;
    z.pending = -1;
    z.kitType = 0;
    z.busySince = 0;
    z.ladder = -1;
    z.climbUntil = 0;
    z.stocked.clear();
    z.stockId = 0;
    z.emptyStage = 0;
    z.detourUntil = 0;
    z.stuckRounds = 0;
    z.slowSince = now;
    z.yaw = F<float>(Vars(e), EV_VANGLE + 4);
    z.pitch = 0;
    z.stuckFrom = Origin(e);
    z.stuckCheck = now + 1.0f;
}

static void DriveZgPlayer(ZgPlayer& z, float now, float dt)
{
    void* e = z.edict;
    unsigned char* v = Vars(e);
    unsigned char* pl = (unsigned char*)PrivateOf(e);
    if (!pl || !(F<int>(v, EV_FLAGS) & FL_CLIENT))
    {
        Log("Zombie Giant: '%s' left the game (flags 0x%x, private %p)", z.name.c_str(), F<int>(v, EV_FLAGS), pl);
        z.edict = nullptr;
        return;
    }
    if (!IsZgPlayerClass(pl))
    {
        static int reports = 0;
        if (reports++ < 10)
            Log("Zombie Giant: '%s' is now a 0x%08lx (not a Zombie Giant player): not driven", z.name.c_str(),
                *(DWORD*)pl - g_mpDelta);
        return;
    }
    float fwd = 0, side = 0;
    unsigned short buttons = 0;
    *(float*)(pl + PLAYER_LAST_MOVEMENT) = now; // never idle-kicked
    // respawn (0x113fa200) counts the respawns of players the room doesn't know (+0x2f54) and marks every 3rd one
    // for a kick (0x1218bce8, done by StartFrame through the room manager)
    *(int*)(pl + 0x2f54) = 0;
    if (!(*(int*)(pl + PLAYER_TEAM) == 1 && *(int*)(pl + ZG_GIANT_TYPE) != 0))
        UpdateBuying(e, StateFor(e)); // the room's weapons, like the bots (not as the giant)
    int team = *(int*)(pl + PLAYER_TEAM);
    int type = *(int*)(pl + ZG_GIANT_TYPE);
    bool giant = team == 1 && (type == 1 || type == 2 || type == 4 || type == 6);
    if (!Alive(e))
    {
        if (z.wasAlive)
            z.deadSince = now;
        z.wasAlive = false;
        if (now - z.deadSince > 3.0f && now >= z.nextRespawn)
        {
            FakeCommand(e, "respawn");
            z.nextRespawn = now + 3.0f;
        }
    }
    else
    {
        if (!z.wasAlive)
        {
            ZgSpawned(e, z, now);
            static int reports = 0;
            if (reports++ < 30)
                Log("Zombie Giant: '%s' spawns as %s (type %d, team %d)", z.name.c_str(), giant ? "the giant" : "a soldier",
                    type, team);
        }
        if (!giant)
            ZgUnfreeze(e, "fake player");
        ZgThink(e, z, giant, team, now, dt, fwd, side, buttons);
        int next = *(int*)(pl + 0x3718);
        if ((team == 1 || next) && now >= z.nextPickReport)
        {
            z.nextPickReport = now + 0.5f;
            Log("ZG picked '%s': team %d, type %d, next %d, timer %.1f (now %.1f), z %.0f, vz %.0f, health %.0f, deadflag %.0f, "
                "flags 0x%x, +0xe10 %d, +0x118 %d, +0x120 %d, +0x3704 %d",
                z.name.c_str(), team, type, next, *(float*)(pl + 0x3724), now, Origin(e).z,
                F<float>(v, EV_VELOCITY + 8), F<float>(v, EV_HEALTH), F<float>(v, EV_DEADFLAG), F<int>(v, EV_FLAGS),
                *(int*)(pl + 0xe10), F<int>(v, 0x118), F<int>(v, 0x120), pl[0x3704]);
        }
        if (now >= z.nextReport)
        {
            z.nextReport = now + 15.0f;
            DLog("ZG '%s' %s at %.0f %.0f %.0f (speed %.0f, move %d fwd %.0f side %.0f), target %s (team %d, %.0f u), "
                 "%d allies", z.name.c_str(), giant ? "giant" : "soldier", Origin(e).x, Origin(e).y, Origin(e).z,
                 Len2D(Vec((float*)(v + EV_VELOCITY))), z.lastMove, z.lastFwd, z.lastSide,
                 z.target ? String(F<int>(Vars(z.target), EV_NETNAME)) : "none", z.target ? TeamOf(z.target) : 0,
                 z.target ? Len(Origin(z.target) - Origin(e)) : 0.0f, g_zgAllies);
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

// a room bot on the human side in Zombie Giant: the stock AI stands still there, so botassist drives it like the
// fake soldiers (true: fwd / side / buttons / aim replaced)
static bool DriveZgBot(void* bot, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (GameMode() != MODE_ZOMBIE_GIANT || TeamOf(bot) != 2 || !Alive(bot))
    {
        auto it = g_zgBots.find(bot);
        if (it != g_zgBots.end())
            it->second.wasAlive = false;
        return false;
    }
    float now = Now();
    ZgPlayer& z = g_zgBots[bot];
    if (!z.wasAlive || now < z.lastThink - 1.0f || now - z.lastThink > 2.0f)
        ZgSpawned(bot, z, now);
    float dt = now - z.lastThink;
    dt = dt <= 0 || dt > 0.25f ? 0.02f : dt;
    z.lastThink = now;
    float f = 0, s = 0;
    unsigned short b = 0;
    ZgUnfreeze(bot, "bot");
    ZgThink(bot, z, false, 2, now, dt, f, s, b);
    yaw = AngleDiff(z.yaw, 0.0f);
    pitch = z.pitch;
    fwd = f;
    side = s;
    buttons = (unsigned short)((buttons & ~(IN_ATTACK | IN_JUMP | IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT)) | b);
    if (now >= z.nextReport)
    {
        z.nextReport = now + 15.0f;
        DLog("ZG bot '%s' at %.0f %.0f %.0f (speed %.0f, move %d fwd %.0f side %.0f), target %s (%.0f u)",
             String(F<int>(Vars(bot), EV_NETNAME)), Origin(bot).x, Origin(bot).y, Origin(bot).z,
             Len2D(Vec((float*)(Vars(bot) + EV_VELOCITY))), z.lastMove, f, s,
             z.target ? String(F<int>(Vars(z.target), EV_NETNAME)) : "none",
             z.target ? Len(Origin(z.target) - Origin(bot)) : 0.0f);
    }
    return true;
}

// once per frame, from the bots' RunPlayerMove
static void DriveAllStarPlayer(ZgPlayer& z, float now, float dt); // modes/all_star.inl

static void UpdateZgPlayers()
{
    float now = Now();
    if (now == g_zgFrame)
        return;
    float dt = g_zgFrame < 0 || now < g_zgFrame || now - g_zgFrame > 0.25f ? 0.02f : now - g_zgFrame;
    g_zgFrame = now;
    if (g_zgLevel != g_nav.level)
    {
        ZgNewLevel();
        g_zgBots.clear();
        g_zgNextRally = 0;
        g_zgLevel = g_nav.level;
        g_zgStart = now;
    }
    bool allStar = GameMode() == MODE_ALLSTAR;
    if ((GameMode() != MODE_ZOMBIE_GIANT && !allStar) || g_edictToVars < 0)
        return;
    if (!allStar)
    {
        ZgScanWorld();
        ZgUpdateTeam(now);
    }
    if (!g_CreateFakeClient || !g_FreeEntPrivateData)
        return;
    MarkZgPlayers();
    if (g_zgPlayers.empty() && g_zgMarkLevel == g_nav.level)
    {
        EnterCriticalSection(&g_matchLock);
        for (auto& m : g_matchBots)
            if (m.fakePlayer)
            {
                ZgPlayer z;
                z.name = m.name;
                z.character = m.character;
                g_zgPlayers.push_back(z);
            }
        LeaveCriticalSection(&g_matchLock);
    }
    if (now - g_zgStart > 2.0f)
        for (auto& z : g_zgPlayers)
            if (!z.tried)
            {
                CreateZgPlayer(z);
                break; // one per frame
            }
    for (auto& z : g_zgPlayers)
        if (z.edict)
        {
            if (allStar)
                DriveAllStarPlayer(z, now, dt);
            else
                DriveZgPlayer(z, now, dt);
        }
}

// ---------------------------------------------------------------- Zombie Giant debugging (botassist.log)
// Who connects and disconnects (detours of ClientPutInServer 0x113efbb0 and ClientDisconnect 0x113ef8c0, with the
// return addresses on the stack: "mp:<address at mp.dll's preferred base>", "hw+<offset>"), every server command
// with "kick", the giant changes of every Zombie Giant player, and every 15 s each player's movement state.
static const DWORD MP_CLIENTDISCONNECT = 0x113ef8c0;
static DWORD g_hwLo, g_hwHi, g_mpLo, g_mpHi;
static EdictFn g_ClientDisconnectTramp, g_PutInServerTramp;
static ServerCommandFn g_ServerCommandOrig;
static std::unordered_map<void*, int> g_zgSeenState; // edict -> team | type << 8 | next type << 16
static float g_zgNextCensus = 0;

static void ModuleRange(const char* name, DWORD& lo, DWORD& hi)
{
    HMODULE m = GetModuleHandleA(name);
    if (!m)
        return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    lo = (DWORD)m;
    hi = lo + nt->OptionalHeader.SizeOfImage;
}

// return addresses into mp.dll / hw.dll found on the stack (a call instruction just before them)
static void StackTrace(char* out, size_t size)
{
    out[0] = 0;
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    DWORD* top = (DWORD*)((NT_TIB*)NtCurrentTeb())->StackBase;
    size_t used = 0;
    int n = 0;
    for (int i = 0; i < 4096 && sp + i < top && n < 16; i++)
    {
        DWORD a = sp[i];
        bool mp = a >= g_mpLo + 6 && a < g_mpHi, hw = a >= g_hwLo + 6 && a < g_hwHi;
        if (!mp && !hw)
            continue;
        const BYTE* p = (const BYTE*)a;
        bool call = p[-5] == 0xe8 || (p[-6] == 0xff && (p[-5] & 0x38) == 0x10) ||
                    (p[-2] == 0xff && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xff && (p[-2] & 0x38) == 0x10);
        if (!call)
            continue;
        int w = mp ? snprintf(out + used, size - used, " mp:%08lx", a - g_mpDelta)
                   : snprintf(out + used, size - used, " hw+%lx", a - g_hwLo);
        if (w <= 0 || used + w >= size)
            break;
        used += w;
        n++;
    }
}

static const char* ZgWho(void* e)
{
    for (auto& z : g_zgPlayers)
        if (z.edict == e)
            return "fake player";
    void* pl = PrivateOf(e);
    if (IsBotClass(pl))
        return "bot";
    if (pl && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) && !(F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT))
        return "you";
    return "?";
}

static void LogClientEvent(const char* what, void* e)
{
    char trace[512];
    StackTrace(trace, sizeof(trace));
    unsigned char* v = Vars(e);
    void* pl = PrivateOf(e);
    unsigned char* p = (unsigned char*)pl;
    Log("CLIENT %s: '%s' (%s, flags 0x%x, team %d, class 0x%08lx, giant %d next %d) at %.1f; stack:%s", what,
        String(F<int>(v, EV_NETNAME)), ZgWho(e), F<int>(v, EV_FLAGS), p ? *(int*)(p + PLAYER_TEAM) : -1,
        p ? *(DWORD*)p - g_mpDelta : 0, p && IsZgPlayerClass(p) ? *(int*)(p + ZG_GIANT_TYPE) : -1,
        p && IsZgPlayerClass(p) ? *(int*)(p + 0x3718) : -1, Now(), trace);
}

static void HookedClientDisconnect(void* e)
{
    __try { if (e && g_edictToVars >= 0 && ZgActive()) LogClientEvent("disconnects", e); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ClientDisconnectTramp(e);
}

static void HookedPutInServer(void* e)
{
    g_PutInServerTramp(e);
    __try { if (e && g_edictToVars >= 0 && ZgActive()) LogClientEvent("put in server", e); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void LogServerCommand(const char* cmd)
{
    char trace[512];
    StackTrace(trace, sizeof(trace));
    std::string c = cmd;
    while (!c.empty() && (c.back() == '\n' || c.back() == '\r'))
        c.pop_back();
    Log("SERVER COMMAND '%s'; stack:%s", c.c_str(), trace);
}

static void HookedServerCommand(const char* cmd)
{
    __try
    {
        static int reports = 0;
        if (cmd && ZgActive() && (strstr(cmd, "kick") || reports < 40))
        {
            reports++;
            LogServerCommand(cmd);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ServerCommandOrig(cmd);
}

// pev flags worth naming in the census
static std::string FlagNames(int f)
{
    static const struct { int bit; const char* name; } names[] = {
        { 1, "FLY" }, { 2, "SWIM" }, { 8, "CLIENT" }, { 0x10, "INWATER" }, { 0x20, "MONSTER" }, { 0x40, "GODMODE" },
        { 0x80, "NOTARGET" }, { 0x200, "ONGROUND" }, { 0x1000, "FROZEN" }, { 0x2000, "FAKECLIENT" }, { 0x4000, "DUCKING" },
        { 0x40000, "IMMUNE_WATER" }, { 0x100000, "PROXY" }, { 0x200000, "ALWAYSTHINK" }, { 0x40000000, "KILLME" },
        { (int)0x80000000, "DORMANT" } };
    std::string s;
    for (auto& n : names)
        if (f & n.bit)
            s += std::string(s.empty() ? "" : "|") + n.name;
    return s;
}

// giant changes of everyone, and a movement census every 15 s (from ZgUpdateTeam, for each client)
static void ZgWatchClient(void* e, unsigned char* pl, float now, bool census)
{
    unsigned char* v = Vars(e);
    int team = *(int*)(pl + PLAYER_TEAM);
    bool zg = IsZgPlayerClass(pl);
    int type = zg ? *(int*)(pl + ZG_GIANT_TYPE) : 0, next = zg ? *(int*)(pl + 0x3718) : 0;
    int state = (team & 0xff) | (type & 0xff) << 8 | (next & 0xff) << 16;
    auto it = g_zgSeenState.find(e);
    if (it == g_zgSeenState.end() || it->second != state)
    {
        Log("ZG state '%s' (%s): team %d, giant type %d, next %d (times giant %d, last %.1f) at %.1f, position %.0f %.0f %.0f",
            String(F<int>(v, EV_NETNAME)), ZgWho(e), team, type, next, zg ? *(int*)(pl + 0x3708) : 0,
            zg ? *(float*)(pl + 0x370c) : 0.0f, now, Origin(e).x, Origin(e).y, Origin(e).z);
        g_zgSeenState[e] = state;
        // the giant spawns on one of the rules' giant spots (0x1085fa90: rules +0x260..+0x264, those with no human
        // near); with none left it reads past the list and the giant lands outside the map
        unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
        if (rules && (!Readable(rules, 4) || *(DWORD*)rules != MP_ZG_RULES_VTABLE + g_mpDelta))
            rules = nullptr;
        if ((type == 1 || type == 2 || type == 4 || type == 6) && rules && Readable(rules + 0x260, 8))
        {
            void** spot = *(void***)(rules + 0x260);
            void** end = *(void***)(rules + 0x264);
            float best = 1e30f;
            int n = 0;
            for (; spot && spot < end && n < 256 && Readable(spot, 4); spot++, n++)
            {
                unsigned char* sp = (unsigned char*)*spot;
                unsigned char* spev = sp && Readable(sp + 8, 4) ? *(unsigned char**)(sp + 8) : nullptr;
                if (spev && Readable(spev + 8, 12))
                {
                    float d = Len2D(Vec((float*)(spev + 8)) - Origin(e));
                    best = d < best ? d : best;
                }
            }
            Log("ZG giant '%s' spawned %.0f units from the closest of %d giant spots%s", String(F<int>(v, EV_NETNAME)), best,
                n, best > 200.0f ? ": OUTSIDE THE SPAWN POINTS" : "");
        }
    }
    if (census)
        DLog("ZG census '%s' (%s): team %d, %s, health %.0f, weapon %s, flags %s, maxspeed %.0f, speed %.0f, buttons 0x%x, at %.0f %.0f %.0f",
             String(F<int>(v, EV_NETNAME)), ZgWho(e), team, zg ? "ZombieBossPlayer" : IsBotClass(pl) ? "bot class" : "other class",
             F<float>(v, EV_HEALTH), String(F<int>(v, EV_WEAPONMODEL)), FlagNames(F<int>(v, EV_FLAGS)).c_str(), F<float>(v, 0x240),
             Len2D(Vec((float*)(v + EV_VELOCITY))), F<int>(v, 0x1a4), Origin(e).x, Origin(e).y, Origin(e).z);
}

static void InstallZgDebug(DWORD delta)
{
    ModuleRange("hw.dll", g_hwLo, g_hwHi);
    ModuleRange("mp.dll", g_mpLo, g_mpHi);
    unsigned char* disc = (unsigned char*)(MP_CLIENTDISCONNECT + delta);
    unsigned char* put = (unsigned char*)(MP_PUT_IN_SERVER + delta);
    static const unsigned char discCode[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c };
    static const unsigned char putCode[] = { 0x53, 0x8b, 0xdc, 0x83, 0xec, 0x08 };
    if (!memcmp(disc, discCode, 6))
        g_ClientDisconnectTramp = (EdictFn)DetourCdecl(disc, 6, (void*)HookedClientDisconnect);
    if (!memcmp(put, putCode, 6))
        g_PutInServerTramp = (EdictFn)DetourCdecl(put, 6, (void*)HookedPutInServer);
    void** table = (void**)(MP_ENGFUNCS + delta);
    if (table[ENG_SERVERCOMMAND] != (void*)HookedServerCommand)
    {
        g_ServerCommandOrig = (ServerCommandFn)table[ENG_SERVERCOMMAND];
        DWORD old;
        VirtualProtect(&table[ENG_SERVERCOMMAND], 4, PAGE_READWRITE, &old);
        table[ENG_SERVERCOMMAND] = (void*)HookedServerCommand;
        VirtualProtect(&table[ENG_SERVERCOMMAND], 4, old, &old);
    }
    Log("debug: client connect/disconnect %s, server commands logged (hw.dll %08lx..%08lx)",
        g_ClientDisconnectTramp && g_PutInServerTramp ? "logged" : "NOT hooked (unexpected code)", g_hwLo, g_hwHi);
}

// ---------------------------------------------------------------- Zombie Giant: who becomes the giant, and staying in
// The game sorts the candidates (the human CTs) with 0x1085f810 (times giant +0x3708, then +0x370c: the time they
// joined or were last the giant) and takes the first ones: you, the first to join, were always first. The sort call
// (0x1086208a: push 0x1085f810) gets ZgGiantOrder instead: a random draw for each pick (the same keys during one
// sort, so the order stays consistent), you last when ZombieGiantYouCanBeGiant=0.
// A fake player that became the giant was dropped ~10 s later, when the mode transformed it: it sends the new giant
// many reliable messages at once, and a fake client's reliable channel (never sent) overflowed: hw.dll drops
// overflowed clients ("Reliable channel overflowed", rva 0x786a06). Reliable messages to fake players go unreliable
// (MSG_ONE -> MSG_ONE_UNRELIABLE): the unreliable buffer just discards what doesn't fit.
// The fake players are driven from the shared frame hook (HookedStartFrame -> ZgFrame) too, so they play when the
// room has no bots left (every room AI player is a fake player here).
static const DWORD MP_GIANT_SORT_PUSH = 0x1086208a, MP_GIANT_ORDER = 0x1085f810;
static const int MSG_ONE = 1, MSG_ONE_UNRELIABLE = 8;
static std::unordered_map<void*, float> g_zgGiantKeys;
static float g_zgKeysAt = -100;
static MessageBeginFn g_MessageBeginOrig;

static float ZgGiantKey(unsigned char* pl)
{
    auto it = g_zgGiantKeys.find(pl);
    if (it != g_zgGiantKeys.end())
        return it->second;
    float key = Rnd();
    unsigned char* pev = pl ? *(unsigned char**)(pl + 8) : nullptr;
    int flags = pev ? F<int>(pev, EV_FLAGS) : 0;
    bool you = (flags & FL_CLIENT) && !(flags & FL_FAKECLIENT);
    if (you && !cfg.zgPlayerGiant)
        key += 2.0f;
    g_zgGiantKeys[pl] = key;
    return key;
}

static bool __cdecl ZgGiantOrder(unsigned char* a, unsigned char* b)
{
    float now = Now();
    if (now - g_zgKeysAt > 0.5f || now < g_zgKeysAt)
    {
        g_zgGiantKeys.clear(); // a new pick: a new draw
        g_zgKeysAt = now;
        Log("Zombie Giant: drawing the giant at random");
    }
    return ZgGiantKey(a) < ZgGiantKey(b);
}

static bool IsZgFakeEdict(void* e)
{
    for (auto& z : g_zgPlayers)
        if (z.edict == e)
            return true;
    return false;
}

static void HookedMessageBegin(int dest, int type, const float* origin, void* ed)
{
    if (dest == MSG_ONE && ed && !g_zgPlayers.empty() && IsZgFakeEdict(ed))
    {
        dest = MSG_ONE_UNRELIABLE;
        static int reports = 0;
        if (reports++ < 3)
            Log("Zombie Giant: reliable messages to fake players go unreliable (message %d)", type);
    }
    g_MessageBeginOrig(dest, type, origin, ed);
}

// every server frame (from the shared frame hook): the fake players play
static void ZgFrame()
{
    if (cfg.enabled && g_edictToVars >= 0 && g_pGlobals && *g_pGlobals && (GameMode(), ZgActive()))
    {
        LoadNav();
        UpdateHurtZones();
        UpdateMatchBots();
        UpdateZgPlayers();
    }
}

static void InstallZgPlayers(DWORD delta)
{
    unsigned char* push = (unsigned char*)(MP_GIANT_SORT_PUSH + delta);
    bool order = push[0] == 0x68 && *(DWORD*)(push + 1) == MP_GIANT_ORDER + delta;
    if (order)
    {
        DWORD old;
        VirtualProtect(push + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        *(DWORD*)(push + 1) = (DWORD)ZgGiantOrder;
        VirtualProtect(push + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), push, 5);
    }
    void** table = (void**)(MP_ENGFUNCS + delta);
    if (table[ENG_MESSAGEBEGIN] != (void*)HookedMessageBegin)
    {
        g_MessageBeginOrig = (MessageBeginFn)table[ENG_MESSAGEBEGIN];
        DWORD old;
        VirtualProtect(&table[ENG_MESSAGEBEGIN], 4, PAGE_READWRITE, &old);
        table[ENG_MESSAGEBEGIN] = (void*)HookedMessageBegin;
        VirtualProtect(&table[ENG_MESSAGEBEGIN], 4, old, &old);
    }
    Log("Zombie Giant: giant drawn at random %s, fake players' reliable messages unreliable",
        order ? "on" : "NOT hooked (unexpected code)");
}

// ---------------------------------------------------------------- Zombie Giant: fake players stay in the game
// hw.dll dropped the fake players when they died or became the giant (SV_DropClient, rva 0x71ba90: client_t*,
// crash, reason format...; the client's name is at client_t +0xa3e0). Every drop is logged with its reason and the
// exact caller; a fake player is only let go on a level change, at the end of the session or when botassist kicks it.
static const DWORD HW_DROPCLIENT = 0x71ba90;
static const int HW_CLIENT_NAME = 0xa3e0;
typedef void (*DropClientFn)(void* cl, int crash, const char* fmt, DWORD a1, DWORD a2, DWORD a3, DWORD a4);
static DropClientFn g_DropClientTramp;

static bool ZgKeepClient(void* cl, const char* fmt, DWORD ret)
{
    const char* name = (const char*)cl + HW_CLIENT_NAME;
    bool fake = false;
    for (auto& z : g_zgPlayers)
        fake |= z.edict && z.name == name;
    auto own = std::find(g_zgKickNames.begin(), g_zgKickNames.end(), std::string(name));
    bool ours = own != g_zgKickNames.end();
    if (ours)
        g_zgKickNames.erase(own);
    std::string reason = fmt ? fmt : "";
    bool allowed = !fake || ours || reason.find("level change") != std::string::npos ||
                   reason.find("Session End") != std::string::npos || reason.find("Client leave") != std::string::npos ||
                   reason.find("shutting down") != std::string::npos;
    // each name and reason once (some callers retry every frame)
    static std::vector<std::string> seen;
    std::string key = std::string(name) + "|" + reason;
    bool fresh = std::find(seen.begin(), seen.end(), key) == seen.end();
    if (fresh && seen.size() < 200)
        seen.push_back(key);
    if (allowed || fresh)
        Log("ENGINE DROPS '%s' (%s): reason '%s', from hw+%lx%s", name, fake ? "fake player" : "client", reason.c_str(),
            ret - g_hwLo, allowed ? "" : ": KEPT (a fake player)");
    return !allowed;
}

// Zombie Disaster Zone: two of the room's three bots left within a second of joining, with nothing on the game's
// side. In that mode the bots' drops and room kicks are logged with their caller, and the bots stay like Zombie
// Giant's fake players (gone only on a level change, at the end of the session or when they leave the room).
// Manhunt did the same (3 of 4 bots gone at once, the round waiting for them forever): same treatment.
static const int MODE_ZSRIFT = 59;
// Zombie Shelter: of the room's three bots only the last one stayed (the game then asked for more, refused): the others
// are kept like in Zombie Disaster Zone (the rest of the mode is in modes/zombie_shelter.inl, included later)
static const int MODE_SHELTER_ROOM = 28;
static void* BotWhere(std::function<bool(void*)> match)
{
    void* found = nullptr;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!found && (F<int>(Vars(e), EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) == (FL_CLIENT | FL_FAKECLIENT) && match(e))
            found = e;
    });
    return found;
}

static bool ZsrKeepBot(void* cl, const char* fmt, DWORD ret, const char* mode)
{
    const char* name = (const char*)cl + HW_CLIENT_NAME;
    if (!BotWhere([&](void* e) { return !strcmp(String(F<int>(Vars(e), EV_NETNAME)), name); }))
        return false;
    std::string reason = fmt ? fmt : "";
    bool allowed = reason.find("level change") != std::string::npos || reason.find("Session End") != std::string::npos ||
                   reason.find("Client leave") != std::string::npos || reason.find("shutting down") != std::string::npos;
    static std::vector<std::string> seen;
    std::string key = std::string(name) + "|" + reason;
    if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 200)
    {
        seen.push_back(key);
        Log("%s: the engine drops bot '%s': reason '%s', from hw+%lx%s", mode, name, reason.c_str(), ret - g_hwLo,
            allowed ? "" : ": KEPT");
    }
    return !allowed;
}

static void HookedDropClient(void* cl, int crash, const char* fmt, DWORD a1, DWORD a2, DWORD a3, DWORD a4)
{
    DWORD ret = (DWORD)_ReturnAddress();
    bool keep = false;
    __try { keep = cl && ZgActive() && ZgKeepClient(cl, fmt, ret); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try
    {
        int mode = GameMode();
        if (!keep && cl && (mode == MODE_ZSRIFT || mode == MODE_ZHE || mode == MODE_SHELTER_ROOM))
            keep = ZsrKeepBot(cl, fmt, ret, mode == MODE_ZHE ? "Manhunt" : mode == MODE_SHELTER_ROOM ? "Shelter" : "ZSRift");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!keep)
        g_DropClientTramp(cl, crash, fmt, a1, a2, a3, a4);
}

// The fake players picked as the giant (moved to the terrorists) were kicked every frame through hw.dll's
// CGameRoomManager::KickPlayer(player index) (vtable 0x2c186f4 slot 10 -> 0x247aa10 -> SV_DropClient "Kicked"):
// the room doesn't know them. That slot is replaced: the caller is logged once, fake players are not kicked.
static const DWORD HW_ROOMMANAGER_VT = 0x2c186f4 - 0x1d00000, HW_ROOMKICK_SLOT = 10;
typedef void(__fastcall* RoomKickFn)(void* self, void* edx, int index);
static RoomKickFn g_RoomKick;

static bool ZgRoomKickBlocked(int index, DWORD ret)
{
    void* e = nullptr;
    for (auto& z : g_zgPlayers)
        if (z.edict && g_IndexOfEdict && g_IndexOfEdict(z.edict) == index)
            e = z.edict;
    static std::vector<DWORD> seen;
    if (std::find(seen.begin(), seen.end(), ret) == seen.end() && seen.size() < 50)
    {
        seen.push_back(ret);
        char from[32];
        if (ret >= g_mpLo && ret < g_mpHi)
            snprintf(from, sizeof(from), "mp:%08lx", ret - g_mpDelta);
        else if (ret >= g_hwLo && ret < g_hwHi)
            snprintf(from, sizeof(from), "hw+%lx", ret - g_hwLo);
        else
            snprintf(from, sizeof(from), "%08lx", ret);
        Log("ROOM KICK player %d (%s) from %s%s", index, e ? String(F<int>(Vars(e), EV_NETNAME)) : "not a fake player",
            from, e ? ": refused (a fake player)" : "");
    }
    return e != nullptr;
}

// Zombie Disaster Zone, Manhunt: the room kicks the bots a few seconds in (it doesn't know them); they stay
static bool BotRoomKickBlocked(int index, DWORD ret, const char* mode)
{
    void* bot = BotWhere([&](void* e) { return g_IndexOfEdict && g_IndexOfEdict(e) == index; });
    static std::vector<DWORD> seen;
    if (bot && std::find(seen.begin(), seen.end(), ret) == seen.end() && seen.size() < 50)
    {
        seen.push_back(ret);
        Log("%s: the room kicks bot '%s' (player %d) from %s+%lx: refused", mode, String(F<int>(Vars(bot), EV_NETNAME)),
            index, ret >= g_mpLo && ret < g_mpHi ? "mp" : "hw", ret >= g_mpLo && ret < g_mpHi ? ret - g_mpDelta : ret - g_hwLo);
    }
    return bot != nullptr;
}

static void __fastcall HookedRoomKick(void* self, void* edx, int index)
{
    DWORD ret = (DWORD)_ReturnAddress();
    bool blocked = false;
    __try { blocked = ZgActive() && ZgRoomKickBlocked(index, ret); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try
    {
        int mode = GameMode();
        if (!blocked && (mode == MODE_ZSRIFT || mode == MODE_ZHE || mode == MODE_SHELTER_ROOM))
            blocked = BotRoomKickBlocked(index, ret, mode == MODE_ZHE ? "Manhunt" : mode == MODE_SHELTER_ROOM ? "Shelter" : "ZSRift");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!blocked)
        g_RoomKick(self, edx, index);
}

static void InstallZgDropGuard()
{
    if (g_hwLo)
    {
        void** slot = (void**)(g_hwLo + HW_ROOMMANAGER_VT) + HW_ROOMKICK_SLOT;
        if (Readable(slot, 4) && *slot == (void*)(g_hwLo + 0x2601990 - 0x1d00000))
        {
            g_RoomKick = (RoomKickFn)*slot;
            PatchSlot(slot, (void*)HookedRoomKick);
        }
        Log("Zombie Giant: room kicks %s", g_RoomKick ? "watched, fake players kept" : "NOT hooked (unexpected code)");
    }
    if (!g_hwLo)
        return;
    unsigned char* fn = (unsigned char*)(g_hwLo + HW_DROPCLIENT);
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x04, 0x06, 0x00, 0x00 };
    if (!memcmp(fn, code, sizeof(code)))
        g_DropClientTramp = (DropClientFn)DetourCdecl(fn, sizeof(code), (void*)HookedDropClient);
    Log("Zombie Giant: engine drops %s", g_DropClientTramp ? "logged, fake players kept" : "NOT hooked (unexpected code)");
}
