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
//   - GdmWatch: those rows only name 2 or 3 weapons (about 45 in all, of the game's hundreds). Now every 2 s each row but the
//     last gets 3 new weapons of its level's kind (rifles, sniper rifles... as in the game's order) from
//     botassist_gdm_weapons.txt, so any weapon can come up at any level up. The vectors are the server's: the client's
//     table isn't touched, a player's weapon goes to its HUD by id.
//   - Bots' weapons: the rules give them by running the buy command as the player (FUN_113166e0, internal-buy flag +0x18b4
//     set), and botassist's rule for room AI players dropped it (see OnBotCommand) while UpdateBuying bought the room's
//     weapons, so bots didn't play the levels at all. Here room weapons are skipped, and only the stock AI's own
//     purchases are dropped.
//   - GdmMove: bots hunt instead of holding a spot (the stock AI keeps aiming and shooting): by the nav mesh to the closest
//     enemy, in sight first; with one close they circle it.
static const DWORD MP_GDM_RULES_VT = 0x11961354;
static const float GDM_REDRAW = 2.0f, GDM_RETARGET = 1.0f, GDM_CLOSE = 320.0f, GDM_TOO_CLOSE = 140.0f, GDM_STRAFE = 0.9f;
static const int GDM_TIERS[] = { 4, 6, 4, 4, 4, 6, 4, 4, 3, 3, 2, 5, 6, 3, 1, 3, 1 }; // level 1.., the game's kinds; 18: Glock 18

static std::map<int, std::vector<int>> g_gdmPool; // tier -> weapon ids

static void GdmLoadPool()
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist_gdm_weapons.txt", g_dir);
    FILE* f = fopen(path, "r");
    int total = 0;
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            int id, tier;
            if (line[0] != ';' && sscanf(line, "%d %d", &id, &tier) == 2)
            {
                g_gdmPool[tier].push_back(id);
                total++;
            }
        }
        fclose(f);
    }
    Log("GDM: %d weapons in botassist_gdm_weapons.txt%s", total, total ? "" : ": the mode keeps the game's own list");
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
    float now = Now();
    if (now >= next - 10.0f && now < next)
        return;
    next = now + GDM_REDRAW;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        GdmLoadPool();
    }
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
    int strafe = 1, stuck = 0, level = -1;
    bool yawSet = false;
};
static std::map<void*, GdmBot> g_gdmBots;

static void GdmPickTarget(void* bot, GdmBot& z, V3 me, V3 eye, int team)
{
    float bestSeen = 1e9f, best = 1e9f;
    void* seen = nullptr;
    void* any = nullptr;
    ForEntitiesNear(me, 1e6f, [&](void* e) {
        if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170) || *(int*)(q + 0x6c) == team || (*(int*)(q + 0x6c) != 1 && *(int*)(q + 0x6c) != 2))
            return;
        float d = Len(Origin(e) - me);
        if (d < best)
        {
            best = d;
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
    if (level != z.level)
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
        V3 wp;
        if (!NextWaypoint(bot, st, me, at, 32.0f, wp))
            wp = at;
        wayYaw = wantYaw = YawOf(wp - me);
        if (me.z - 36.0f - wp.z + 32.0f > maxDrop)
            maxDrop = min(me.z - 36.0f - wp.z + 32.0f, HURT_DROP_MAX);
    }
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
        // stuck: a jump and a step aside, then back; after a few tries another target
        st.lastProgress = now;
        st.nextRepath = 0;
        z.stuck++;
        z.strafe = -z.strafe;
        z.dodgeYaw = z.stuck % 2 ? wayYaw + 90.0f * z.strafe : wayYaw + 180.0f;
        z.dodgeUntil = now + 0.5f;
        jump = jumpSafe;
        if (z.stuck >= 4)
            z.target = nullptr;
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
