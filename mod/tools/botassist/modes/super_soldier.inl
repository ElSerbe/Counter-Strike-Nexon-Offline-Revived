// botassist: Super Soldier (mode 57, "SuperSoldier"; rules CSOGameRulesSuperSoldier, vtable 0x11789d2c). Included by
// botassist.cpp at this point (one translation unit: it uses the shared helpers defined before it). Only runs in its own mode:
// SSWatch returns unless GameMode() == MODE_SS.
//
// The mode (https://cso.fandom.com/wiki/Super_Soldier): team deathmatch where each player is an Expert (Fighter, Striker,
// Supporter, Sniper, chosen at every spawn) with skills, S.V.P. boxes appear around the map, and a revived player is
// invincible for a moment. The game has its own AI for it: bots run the behavior trees of
// resource/supersoldier/supersoldier_bot_bt.xml (buying by Expert from supersoldier_bot.json, skills, targets, movement).
//
// What botassist does: little, to leave that AI's weapons, skills and aim alone.
//   - Teams: the match waits until both sides have players (rules vfunc 0x104, 0x10778350), and the room lists every AI
//     player on the host's side, so the requests for T bots were refused. RoomBotQuota (shared, see there) balances them:
//     here the game's request doesn't decide the side, it asks for CT until the room's list is used up.
//   - Weapons: their BuyWeapon node buys by Expert. Room weapons are skipped here (UpdateBuying) and their purchases aren't
//     dropped (OnBotCommand).
//   - Movement: their Move node goes straight for a target, so everybody ended up in one group. GdmMove (Gun Deathmatch's: routes
//     of their own, spreading over the map) moves them instead, called from the shared bot hook; with no target it leaves it to them.
//   - SSWatch: botassist.log "SS: n T and m CT alive" and each bot's Expert, to read a test.
static const int SS_MODULE = 0x17;

static void SSWatch()
{
    static float next = 0;
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != MODE_SS)
        return;
    float now = Now();
    if (now >= next - 60.0f && now < next)
        return;
    next = now + 15.0f;
    static int reports = 0;
    if (reports >= 60)
        return;
    reports++;
    int alive[3] = { 0, 0, 0 }, dead = 0;
    std::string experts;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            return;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (!q || !Readable(q, 0x2170))
            return;
        int team = *(int*)(q + 0x6c);
        if (team != 1 && team != 2)
            return;
        if (!Alive(e))
            dead++;
        else
            alive[team]++;
        unsigned char* mod = (unsigned char*)((PlayerModuleFn)(MP_PLAYER_MODULE + g_mpDelta))(q, SS_MODULE);
        char one[16];
        snprintf(one, sizeof(one), "%d ", mod && Readable(mod, 0x18) ? *(int*)(mod + 0x14) : -1);
        experts += one;
    });
    Log("SS: %d T and %d CT alive, %d dead; Experts: %s", alive[1], alive[2], dead, experts.c_str());
}
