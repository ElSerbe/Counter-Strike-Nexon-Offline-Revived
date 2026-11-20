// botassist: Gun Deathmatch (mode 22, "GunDeath"; rules CSOGameRulesGunDeath, vtable 0x11961354). Included by botassist.cpp at
// this point (one translation unit: it uses the shared helpers defined before it). Only runs in its own mode: GdmWatch
// returns and GdmMove isn't called unless GameMode() == MODE_GDM.
//
// The mode: two teams, everyone respawns, every few kills the rules give the next level's weapon, strong ones first, down to
// the Glock 18 (level 18, 60 kills) and then an HE grenade; a grenade kill wins the match. The levels are in
// resource/GDMWeaponList.cso (GDMWeaponListCash.cso for "Lucky Gunman"), read into two vectors at rules +0x2e0 and +0x2ec, 0x24
// bytes a row: level, type (0 fixed, 1 draw), kills, 3 weapon ids (Item.csv column 5, not item ids), 3 chances. FUN_114285c0
// draws from a level's row when a player reaches it.
//
// What botassist does:
//   - Weapons: those rows only name 2 or 3 weapons (about 45 in all, of the game's hundreds). botassist detours the draw
//     (FUN_114285c0, thiscall rules, player, rows; player +0x2f88 kills, +0x2f8c level, +0x2f90 weapon id): just before
//     it, the row the player is reaching gets one weapon drawn for that player alone from the whole of
//     botassist_gdm_weapons.txt, any kind at any level, never one the player already had this match (2026-11-20, the
//     user: the same weapons came up again and again; the rows used to be refilled every 2 s by kind, so everybody
//     levelling up in those 2 s drew from the same 3). The Glock 18 and the grenade at the end stay as the game has them.
//     If the detour can't go in, GdmWatch still fills the rows every 2 s by kind. The vectors are the server's: the
//     client's table isn't touched, a player's weapon goes to its HUD by id.
//   - Broken weapons: some weapons of this client's mp.dll have no working server code: the shot plays on the client and
//     nothing comes out (the user's "Arcana 2", and bots shooting without hurting anyone). GdmTrack watches the weapon in
//     every player's hand: 4 s of trigger over 4 presses with rounds in the clip, not one round used and no hit; or 40
//     rounds fired by everyone with it and not one hit. That weapon goes into botassist_gdm_broken.txt (never drawn again,
//     in every match; delete a line to try it again) and whoever holds it gets another weapon at once.
//   - Bots' weapons: the rules give them by running the buy command as the player (FUN_113166e0, internal-buy flag +0x18b4
//     set), and botassist's rule for room AI players dropped it (see OnBotCommand) while UpdateBuying bought the room's
//     weapons, so bots didn't play the levels at all. Here room weapons are skipped, and only the stock AI's own
//     purchases are dropped.
//   - GdmMove: bots hunt instead of holding a spot (the stock AI keeps aiming and shooting): by the nav mesh to the closest
//     enemy, in sight first; with one close they circle it.
//   - Routes: going for the closest enemy made one big group. Enemies somebody already goes for count as farther, and on the way
//     most bots go through a place of their own first (GdmPickVia: away from teammates and from where they're heading, not too
//     far off), hunting straight only for a while after it or once an enemy is in sight and near. Super Soldier moves the same.
static const DWORD MP_GDM_RULES_VT = 0x11961354, MP_GDM_GIVE = 0x114285c0;
static const float GDM_REDRAW = 2.0f, GDM_RETARGET = 1.0f, GDM_CLOSE = 320.0f, GDM_TOO_CLOSE = 140.0f, GDM_STRAFE = 0.9f;
static const float GDM_ENGAGE = 900.0f, GDM_VIA_REACHED = 180.0f, GDM_VIA_TIME = 20.0f, GDM_SHARED_TARGET = 500.0f;
static const int GDM_TIERS[] = { 4, 6, 4, 4, 4, 6, 4, 4, 3, 3, 2, 5, 6, 3, 1, 3, 1 }; // level 1.., the game's kinds; 18: Glock 18
static const int GDM_DRAWN_LEVELS = sizeof(GDM_TIERS) / sizeof(GDM_TIERS[0]); // the rows botassist draws (not the Glock, grenade)
static const int GDM_ROW = 0x24, GDM_KILLS = 0x2f88, GDM_LEVEL = 0x2f8c, GDM_WEAPON = 0x2f90;
static const int GDM_EV_BUTTON = 0x1a4, GDM_ITEM_CLIP = 0x160; // entvars button (mp.dll's save table), weapon's clip
static const float GDM_DEAD_HELD = 4.0f;
static const int GDM_DEAD_PRESSES = 4, GDM_DEAD_SHOTS = 40;

struct GdmWeapon
{
    int id, tier;
    std::string cls; // weapon_<name>, lower case
};
static std::vector<GdmWeapon> g_gdmAll;              // every weapon that can be drawn
static std::map<int, std::vector<int>> g_gdmPool;    // tier -> weapon ids (the rows filled by kind, without the detour)
static std::set<std::string> g_gdmBroken;            // weapons that do nothing (botassist_gdm_broken.txt)
static std::map<void*, std::set<int>> g_gdmHad;      // player -> weapons it got this match
static void* g_gdmRules = nullptr;                   // the draw's last arguments (to give another weapon)
static int* g_gdmRows = nullptr;

static std::string GdmLower(std::string s)
{
    for (auto& ch : s)
        ch = (char)tolower((unsigned char)ch);
    return s;
}

static void GdmBrokenPath(char* path, size_t n)
{
    snprintf(path, n, "%sbotassist_gdm_broken.txt", g_dir);
}

static void GdmLoadPool()
{
    char path[MAX_PATH];
    GdmBrokenPath(path, sizeof(path));
    if (FILE* f = fopen(path, "r"))
    {
        char line[256], cls[128];
        while (fgets(line, sizeof(line), f))
            if (line[0] != ';' && sscanf(line, "%127s", cls) == 1)
                g_gdmBroken.insert(GdmLower(cls));
        fclose(f);
    }
    snprintf(path, sizeof(path), "%sbotassist_gdm_weapons.txt", g_dir);
    FILE* f = fopen(path, "r");
    int total = 0, broken = 0;
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            int id, tier;
            char res[128] = "";
            if (line[0] == ';' || sscanf(line, "%d %d ; %127s", &id, &tier, res) < 2)
                continue;
            std::string cls = res[0] ? "weapon_" + GdmLower(res) : "";
            if (!cls.empty() && g_gdmBroken.count(cls))
            {
                broken++;
                continue;
            }
            g_gdmAll.push_back({ id, tier, cls });
            g_gdmPool[tier].push_back(id);
            total++;
        }
        fclose(f);
    }
    Log("GDM: %d weapons in botassist_gdm_weapons.txt (%d left out: botassist_gdm_broken.txt)%s", total, broken,
        total ? "" : ": the mode keeps the game's own list");
}

static const char* GdmClassOf(int id)
{
    for (auto& w : g_gdmAll)
        if (w.id == id)
            return w.cls.c_str();
    return "?";
}

// one weapon for this player: any of the list it hasn't had this match (all of them again once it had them all)
static int GdmDrawFor(void* player)
{
    std::set<int>& had = g_gdmHad[player];
    for (int pass = 0; pass < 2; pass++)
    {
        std::vector<int> can;
        for (auto& w : g_gdmAll)
            if (!had.count(w.id))
                can.push_back(w.id);
        if (!can.empty())
        {
            int id = can[((unsigned)rand() * 32768u + (unsigned)rand()) % can.size()];
            had.insert(id);
            return id;
        }
        had.clear();
    }
    return 0;
}

// just before the game's draw: the row the player is reaching names one weapon, drawn for this player
static void GdmPickFor(unsigned char* player, int* rows)
{
    if (g_gdmAll.empty() || !Readable(player, GDM_WEAPON + 4) || !Readable(rows, 8))
        return;
    unsigned char* first = (unsigned char*)rows[0];
    int count = first && rows[1] > rows[0] ? (rows[1] - rows[0]) / GDM_ROW : 0;
    if (count < 2 || !Readable(first, count * GDM_ROW))
        return;
    int kills = *(int*)(player + GDM_KILLS), i = count - 1;
    while (i >= 0 && kills < *(int*)(first + i * GDM_ROW + 8))
        i--;
    if (i < 0 || i >= GDM_DRAWN_LEVELS || i >= count - 1)
        return; // before level 1, or the Glock and the grenade at the end
    unsigned char* row = first + i * GDM_ROW;
    if (*(int*)(row + 4) != 1 || *(int*)row == *(int*)(player + GDM_LEVEL))
        return; // a fixed level, or no level up (nothing is given)
    int id = GdmDrawFor(player);
    if (!id)
        return;
    for (int k = 0; k < 3; k++)
    {
        *(int*)(row + 0xc + k * 4) = id;
        *(float*)(row + 0x18 + k * 4) = k == 2 ? 34.0f : 33.0f;
    }
    unsigned char* pev = Readable(player + 8, 4) ? *(unsigned char**)(player + 8) : nullptr;
    const char* name = pev && Readable(pev + EV_NETNAME, 4) ? String(*(int*)(pev + EV_NETNAME)) : nullptr;
    bool human = pev && !(*(int*)(pev + EV_FLAGS) & FL_FAKECLIENT);
    static int reports = 0;
    if (human || reports++ < 40)
        Log("GDM: '%s' level %d draws weapon %d (%s), %d had this match", name ? name : "?", *(int*)row, id, GdmClassOf(id),
            (int)g_gdmHad[player].size());
}

typedef void*(__fastcall* GdmGiveFn)(void* rules, void* edx, unsigned char* player, int* rows);
static GdmGiveFn g_gdmGive;

static void SafeGdmPickFor(unsigned char* player, int* rows)
{
    __try
    {
        GdmPickFor(player, rows);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("GDM: exception 0x%08lX drawing a weapon (the game's row used)", GetExceptionCode());
    }
}

static void* __fastcall HookedGdmGive(void* rules, void* edx, unsigned char* player, int* rows)
{
    if (GameMode() == MODE_GDM)
    {
        g_gdmRules = rules;
        g_gdmRows = rows;
        SafeGdmPickFor(player, rows);
    }
    return g_gdmGive(rules, edx, player, rows);
}

static void GdmHookGive()
{
    unsigned char* fn = (unsigned char*)(MP_GDM_GIVE + g_mpDelta);
    const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x08 }; // push ebp; mov ebp, esp; sub esp, 8
    if (!Readable(fn, sizeof(code)) || (fn[0] == 0xe9 && g_gdmGive))
        return;
    static DWORD warned = 0;
    if (memcmp(fn, code, sizeof(code)))
    {
        if (warned != g_mpDelta + 1)
            Log("GDM: the level-up draw has unexpected code: rows filled by kind every 2 s instead");
        warned = g_mpDelta + 1;
        return;
    }
    g_gdmGive = (GdmGiveFn)DetourCdecl(fn, sizeof(code), (void*)HookedGdmGive);
    Log("GDM: level-up draw detoured: every player draws its own weapon from the whole list, no repeats");
}

// ---- weapons that do nothing
static void ShPrint(void* player, const char* text); // zombie_shelter.inl: a line in the middle of the screen

struct GdmHand
{
    std::string cls;
    int clip = -1, presses = 0;
    float held = 0, last = 0;
    bool down = false, fired = false;
};
static GdmHand g_gdmHands[65]; // by client index
struct GdmUse
{
    int shots = 0, hits = 0;
};
static std::map<std::string, GdmUse> g_gdmUse; // weapon -> rounds fired and hits by everyone this match

// another weapon for a player now (its level drawn again)
static void GdmRegive(void* e, unsigned char* p)
{
    if (!g_gdmGive || !g_gdmRules || !g_gdmRows || !Readable(p, GDM_WEAPON + 4))
        return;
    int keep = *(int*)(p + GDM_LEVEL);
    *(int*)(p + GDM_LEVEL) = -1;
    HookedGdmGive(g_gdmRules, nullptr, p, g_gdmRows);
    if (*(int*)(p + GDM_LEVEL) == -1)
        *(int*)(p + GDM_LEVEL) = keep; // nothing given
    else if (!(F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT))
        ShPrint(e, "That weapon doesn't work in offline play: here's another one");
}

static void GdmBroken(const std::string& cls, const char* why)
{
    if (!g_gdmBroken.insert(cls).second)
        return;
    char path[MAX_PATH];
    GdmBrokenPath(path, sizeof(path));
    bool fresh = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
    if (FILE* f = fopen(path, "a"))
    {
        if (fresh)
            fputs("; Gun Deathmatch: weapons that do nothing in offline play (found by botassist); never drawn. Delete a line "
                  "to try that weapon again\n", f);
        fprintf(f, "%s ; %s\n", cls.c_str(), why);
        fclose(f);
    }
    g_gdmAll.erase(std::remove_if(g_gdmAll.begin(), g_gdmAll.end(), [&](const GdmWeapon& w) { return w.cls == cls; }),
                   g_gdmAll.end());
    g_gdmPool.clear();
    for (auto& w : g_gdmAll)
        g_gdmPool[w.tier].push_back(w.id);
    Log("GDM: %s %s: never drawn again (botassist_gdm_broken.txt), whoever holds it gets another weapon", cls.c_str(), why);
    auto entOf = (EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX];
    for (int i = 1; i <= 64; i++)
        if (g_gdmHands[i].cls == cls)
        {
            void* e = entOf(i);
            unsigned char* p = e ? (unsigned char*)PrivateOf(e) : nullptr;
            g_gdmHands[i] = GdmHand();
            if (p)
                GdmRegive(e, p);
        }
}

// every frame: the weapon in each player's hand, its clip and the trigger
static void GdmTrack()
{
    float now = Now();
    auto entOf = (EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX];
    int maxClients = *(int*)((DWORD)*g_pGlobals + GV_MAXCLIENTS);
    for (int i = 1; i <= maxClients && i <= 64; i++)
    {
        GdmHand& h = g_gdmHands[i];
        float dt = (std::min)(0.1f, (std::max)(0.0f, now - h.last));
        void* e = entOf(i);
        unsigned char* p = e ? (unsigned char*)PrivateOf(e) : nullptr;
        if (!p || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e) || !Readable(p, GDM_WEAPON + 4))
        {
            h = GdmHand();
            h.last = now;
            continue;
        }
        h.last = now;
        unsigned char* w = ItemHandle(p + PLAYER_ACTIVE_ITEM);
        std::string cls = WbHeld(p);
        int clip = w ? *(int*)(w + GDM_ITEM_CLIP) : -1;
        if (cls != h.cls)
        {
            h = GdmHand();
            h.cls = cls;
            h.clip = clip;
            h.last = now;
            continue;
        }
        if (cls.empty())
            continue;
        if (clip >= 0 && h.clip >= 0 && clip < h.clip)
        {
            h.fired = true;
            g_gdmUse[cls].shots += (std::min)(5, h.clip - clip);
        }
        h.clip = clip;
        bool attack = (F<int>(Vars(e), GDM_EV_BUTTON) & IN_ATTACK) != 0;
        if (attack && clip > 0)
        {
            h.held += dt;
            if (!h.down)
                h.presses++;
        }
        h.down = attack;
        const GdmUse& u = g_gdmUse[cls];
        if (!h.fired && h.held >= GDM_DEAD_HELD && h.presses >= GDM_DEAD_PRESSES)
            GdmBroken(cls, "fires nothing (4 s of trigger with rounds in the clip, not one used)");
        else if (u.shots >= GDM_DEAD_SHOTS && !u.hits && !g_wbOrig.empty())
            GdmBroken(cls, "hurts nobody (40 rounds fired, not one hit)");
    }
}

// a player hurt (weapon_balance.inl's TakeDamage hook): a hit for the attacker's weapon
static void GdmOnDamage(void* victim, void* attacker, float damage)
{
    if (damage <= 0 || !attacker || GameMode() != MODE_GDM || !Readable(attacker, EV_CONTAINING_ENTITY + 4))
        return;
    void* aed = *(void**)((unsigned char*)attacker + EV_CONTAINING_ENTITY);
    unsigned char* ap = aed ? (unsigned char*)PrivateOf(aed) : nullptr;
    if (!ap || ap == victim || !Readable(ap, 0x2170))
        return;
    std::string cls = WbHeld(ap);
    if (cls.empty())
        return;
    g_gdmUse[cls].hits++;
    int idx = g_IndexOfEdict ? g_IndexOfEdict(aed) : 0;
    if (idx > 0 && idx < 65 && g_gdmHands[idx].cls == cls)
        g_gdmHands[idx].fired = true;
}

static void SafeGdmOnDamage(void* victim, void* attacker, float damage)
{
    __try
    {
        GdmOnDamage(victim, attacker, damage);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void GdmDrawRows(unsigned char* rules, int offset)
{
    unsigned char* row = *(unsigned char**)(rules + offset);
    unsigned char* end = *(unsigned char**)(rules + offset + 4);
    int count = row && end > row ? (int)(end - row) / 0x24 : 0;
    for (int i = 0; i < count && i < (int)(sizeof(GDM_TIERS) / sizeof(GDM_TIERS[0])); i++, row += 0x24)
    {
        auto it = g_gdmPool.find(GDM_TIERS[i]);
        if (it == g_gdmPool.end() || it->second.empty() || !Readable(row, 0x24) || *(int*)(row + 4) != 1)
            continue;
        for (int k = 0; k < 3; k++)
        {
            *(int*)(row + 0xc + k * 4) = it->second[rand() % it->second.size()];
            *(float*)(row + 0x18 + k * 4) = k == 2 ? 34.0f : 33.0f;
        }
    }
}

static void GdmWatch()
{
    static float next = 0;
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_GDM)
        return;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        GdmLoadPool();
    }
    static int match = -1;
    if (match != g_nav.level)
    {
        // a new match: nobody has had anything yet
        match = g_nav.level;
        g_gdmHad.clear();
        g_gdmUse.clear();
        for (auto& h : g_gdmHands)
            h = GdmHand();
        g_gdmRules = nullptr;
        g_gdmRows = nullptr;
    }
    GdmHookGive();
    GdmTrack();
    float now = Now();
    if (now >= next - 10.0f && now < next)
        return;
    next = now + GDM_REDRAW;
    if (g_gdmGive)
        return; // every player draws in the detour
    unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    if (!rules || !Readable(rules, 0x300) || *(DWORD*)rules != MP_GDM_RULES_VT + g_mpDelta)
        return;
    GdmDrawRows(rules, 0x2e0);
    GdmDrawRows(rules, 0x2ec);
}

struct GdmBot
{
    void* target = nullptr;
    float retargetAt = 0, strafeAt = 0, jumpAt = 0, dodgeUntil = 0, dodgeYaw = 0, yaw = 0, lastSeen = 0;
    float aliveAt = 0, viaAt = 0, viaUntil = 0;
    int strafe = 1, stuck = 0, level = -1, team = 0;
    bool yawSet = false, hasVia = false, stuckNow = false;
    V3 via = { 0, 0, 0 };
};
static std::map<void*, GdmBot> g_gdmBots;

static void GdmPickTarget(void* bot, GdmBot& z, V3 me, V3 eye, int team)
{
    float bestSeen = 1e9f, best = 1e9f;
    std::map<void*, int> hunted; // who the team already goes for
    for (auto& kv : g_gdmBots)
        if (kv.first != bot && kv.second.team == team && kv.second.target)
            hunted[kv.second.target]++;
    void* seen = nullptr;
    void* any = nullptr;
    ForEntitiesNear(me, 1e6f, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || *(int*)(q + 0x6c) == team || (*(int*)(q + 0x6c) != 1 && *(int*)(q + 0x6c) != 2))
            return;
        float d = Len(Origin(e) - me);
        auto others = hunted.find(e);
        float shared = others == hunted.end() ? d : d + GDM_SHARED_TARGET * others->second;
        if (shared < best)
        {
            best = shared;
            any = e;
        }
        if (d < bestSeen && CanSee(bot, eye, e))
        {
            bestSeen = d;
            seen = e;
        }
    });
    z.target = seen ? seen : any;
}

// a place of its own to go through on the way to its target: far from its teammates and the places they go through
static void GdmPickVia(void* bot, GdmBot& z, V3 me, V3 target)
{
    std::vector<V3> others;
    ForEntitiesNear(me, 1e6f, [&](void* e) {
        if (e != bot && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) && Alive(e) && TeamOf(e) == z.team)
            others.push_back(Origin(e));
    });
    for (auto& kv : g_gdmBots)
        if (kv.first != bot && kv.second.team == z.team && kv.second.hasVia)
            others.push_back(kv.second.via);
    float bestScore = -1e9f;
    for (int tries = 0; tries < 16 && !g_nav.areas.empty(); tries++)
    {
        // (half of the time a point of interest of the map, when it has them)
        V3 c = PoiSpotCount() && (rand() & 1) ? PoiSpotAt(rand() % PoiSpotCount()) : g_nav.areas[rand() % g_nav.areas.size()].center;
        if (PoiAvoidAt(c))
            continue;
        float there = Len(c - me);
        if (there < GDM_VIA_REACHED * 2.0f)
            continue;
        float apart = 1500.0f;
        for (auto& o : others)
            apart = min(apart, Len(c - o));
        float score = apart - 0.25f * (there + Len(c - target));
        if (score > bestScore)
        {
            bestScore = score;
            z.via = c + V3{ 0, 0, 36 };
            z.hasVia = true;
        }
    }
    std::vector<V3> path;
    if (z.hasVia && !FindPath(me, z.via, path))
        z.hasVia = false;
    z.viaUntil = Now() + GDM_VIA_TIME;
}

// One step towards a direction, without falling, unsticking itself (a jump and a step aside, then back). z.stuckNow: it just
// counted as stuck, z.stuck times in a row. Also used by Partner.
static bool GdmWalk(void* bot, BotState& st, GdmBot& z, V3 me, float now, float wantYaw, float wayYaw, float maxDrop, float& viewYaw,
                    bool& setView, float& fwd, float& side, unsigned short& buttons)
{
    z.stuckNow = false;
    wantYaw = WalkYaw(bot, me, wantYaw, 40.0f);
    if (now < z.dodgeUntil)
        wantYaw = WalkYaw(bot, me, z.dodgeYaw, 40.0f);
    float moveYaw = wantYaw;
    if (!ZConSafeYaw(bot, me, wantYaw, maxDrop, moveYaw))
    {
        fwd = side = 0;
        z.yawSet = false;
        return true;
    }
    bool jumpSafe = !DropAhead(bot, me, moveYaw, 120.0f, ZCON_FREE_DROP);
    bool jump = jumpSafe && LedgeAhead(bot, me, moveYaw);
    if (Len2D(me - st.lastPos) > 24.0f)
    {
        st.lastPos = me;
        st.lastProgress = now;
        z.stuck = 0;
    }
    else if (now - st.lastProgress > 0.8f)
    {
        st.lastProgress = now;
        st.nextRepath = 0;
        z.stuck++;
        z.stuckNow = true;
        z.strafe = -z.strafe;
        z.dodgeYaw = z.stuck % 2 ? wayYaw + 90.0f * z.strafe : wayYaw + 180.0f;
        z.dodgeUntil = now + 0.5f;
        jump = jumpSafe;
    }
    if (jump && now - z.jumpAt > 0.7f)
    {
        z.jumpAt = now;
        buttons |= IN_JUMP;
    }
    if (now - z.jumpAt > 0.1f && now - z.jumpAt < 0.5f)
        buttons |= IN_DUCK;
    // nobody in sight for a moment: it looks where it walks
    if (now - z.lastSeen > 1.5f)
    {
        z.yaw = z.yawSet ? Approach(z.yaw, moveYaw, 9.0f) : viewYaw;
        z.yawSet = true;
        viewYaw = z.yaw;
        setView = true;
    }
    else
        z.yawSet = false;
    float rel = (moveYaw - viewYaw) / RAD;
    fwd = cosf(rel) * 400.0f;
    side = -sinf(rel) * 400.0f;
    return true;
}

static bool GdmMove(void* bot, BotState& st, float& viewYaw, bool& setView, float& fwd, float& side, unsigned short& buttons)
{
    setView = false;
    if (st.ordered && st.order != ORDER_INDEPENDENT)
        return false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2fa0) || !g_nav.ok)
        return false;
    GdmBot& z = g_gdmBots[bot];
    float now = Now();
    V3 me = Origin(bot), eye = Eye(bot);
    int level = *(int*)(p + 0x2f8c);
    z.team = *(int*)(p + 0x6c);
    if (now - z.aliveAt > 1.0f)
    {
        // a new life: a route of its own, or for one bot in three straight at them for a moment
        z.hasVia = false;
        z.viaAt = now + (rand() % 3 ? 0.0f : 6.0f);
    }
    z.aliveAt = now;
    if (GameMode() == MODE_GDM && level != z.level)
    {
        z.level = level;
        static int reports = 0;
        if (reports++ < 60)
            Log("GDM: bot '%s' reaches level %d: weapon %d, primary %s", String(F<int>(Vars(bot), EV_NETNAME)), level,
                *(int*)(p + 0x2f90), SlotClass(bot, 1).c_str());
    }
    if (F<float>(Vars(bot), EV_MAXSPEED) <= 1.0f || (F<int>(Vars(bot), EV_FLAGS) & FL_FROZEN))
    {
        st.lastPos = me;
        st.lastProgress = now;
        return false;
    }
    if (now >= z.retargetAt || !z.target)
    {
        z.retargetAt = now + GDM_RETARGET;
        GdmPickTarget(bot, z, me, eye, *(int*)(p + 0x6c));
    }
    if (!z.target)
        return false;
    if (!(F<int>(Vars(z.target), EV_FLAGS) & FL_CLIENT) || !Alive(z.target))
    {
        z.target = nullptr;
        return false;
    }
    V3 at = Origin(z.target);
    float d = Len(at - me);
    bool seen = CanSee(bot, eye, z.target);
    if (seen)
        z.lastSeen = now;
    float wantYaw, wayYaw;
    float maxDrop = ZCON_FREE_DROP;
    if (seen && d < GDM_CLOSE && fabsf(at.z - me.z) < 60.0f)
    {
        // close: round it, without standing still (nearer when far, away when almost touching)
        if (now >= z.strafeAt)
        {
            z.strafeAt = now + GDM_STRAFE * (0.6f + Rnd() * 0.8f);
            z.strafe = rand() % 2 ? 1 : -1;
        }
        wayYaw = YawOf(at - me);
        wantYaw = wayYaw + z.strafe * (d < GDM_TOO_CLOSE ? 120.0f : 70.0f);
    }
    else
    {
        V3 goal = at;
        if (!(seen && d < GDM_ENGAGE))
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
        }
        V3 wp;
        if (!NextWaypoint(bot, st, me, goal, 32.0f, wp))
            wp = goal;
        wayYaw = wantYaw = YawOf(wp - me);
        if (me.z - 36.0f - wp.z + 32.0f > maxDrop)
            maxDrop = min(me.z - 36.0f - wp.z + 32.0f, HURT_DROP_MAX);
    }
    bool walked = GdmWalk(bot, st, z, me, now, wantYaw, wayYaw, maxDrop, viewYaw, setView, fwd, side, buttons);
    if (z.stuckNow && z.stuck >= 4)
    {
        // stuck after a few tries: another target
        z.target = nullptr;
        z.hasVia = false;
    }
    return walked;
}
