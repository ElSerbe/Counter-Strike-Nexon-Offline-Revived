// botassist: Human Scenario (mode 17; rules CSOGameRulesHumanScen, vtable 0x11962378). Included by botassist.cpp at this point (one translation
// unit: it uses the shared helpers defined before it). Every function here does nothing outside its mode: GameMode() == MODE_HS.
//
// The mode (https://cso.fandom.com/wiki/Human_Scenario): the players against computer-controlled soldiers and aircraft. It runs on the zombie
// scenario's code: the soldiers come from the maps' zombiespawn points as monsters, so the shared companion AI (ScanMonsters and the rest) fights them.
//
// What blocked it: the drones and the Osprey are airbot_drone and airbot_osprey, and botassist's Ignore list leaves out "airbot" and "drone" (friendly
// NPCs in other modes). With nothing else on the map at the start of hs_blaze, the bots weren't even driven. The soldiers are monster_entity
// (models/monster/bot_light, bot_rocket, bot_sniper, bot_heavy, bot_heavy_boss). The shared code asks here:
//   - IsIgnored: HsIsEnemyClass. That includes airbot_hmissile: their homing missile has 10 health and can be shot down.
//   - ScanMonsters: HsScanRadius, its fights are at middle to long range and the aircraft stay far. HsTooFar and HsMissileFirst: a missile
//     in range comes before anything else, a far one isn't worth a bullet.
//   - AimPoint and the combat movement: HsFlying. An aircraft has no feet to aim at and backing away from what flies overhead does nothing.
//     HsAimLead: bullets arrive at once, and leading a fast missile the way the zombies are led aims ahead of it.
//   - Firing: HsClearShotAt. A small aircraft is rarely right on the line, and the shot only has to reach it.
//   - Independent order: HsNoChase. Nobody can walk to an aircraft, and with targets 6000 units away a bot would stay behind shooting at them.
// HsWatch logs each kind of enemy once, and whether bots shoot it.
static const int MODE_HS = 17, HS_EV_MODEL = 0xc8;
static const float HS_SCAN_RADIUS = 6000.0f, HS_WATCH_EVERY = 2.0f, HS_REACH_MARGIN = 20.0f, HS_MISSILE_RANGE = 1800.0f, HS_AIR_LEAD = 0.01f, HS_ZOMBIE_LEAD = 0.06f,
                   HS_CHASE_RANGE = 1200.0f;
static const char* const HS_MISSILE_CLASS = "airbot_hmissile";
static const char* const HS_ENEMY_CLASSES[] = { "airbot_drone", "airbot_osprey", HS_MISSILE_CLASS };
static const char* const HS_AIRCRAFT_MODELS[] = { "flydron", "osprey", "comanche", "apache", "dron_missile" };

static bool HsIsEnemyClass(const char* classname)
{
    if (GameMode() != MODE_HS)
        return false;
    for (const char* c : HS_ENEMY_CLASSES)
        if (!strcmp(classname, c))
            return true;
    return false;
}

static float HsScanRadius()
{
    return GameMode() == MODE_HS ? max(cfg.radius, HS_SCAN_RADIUS) : cfg.radius;
}

static bool HsFlying(void* e)
{
    if (GameMode() != MODE_HS)
        return false;
    const char* model = String(F<int>(Vars(e), HS_EV_MODEL));
    if (!model)
        return false;
    for (const char* m : HS_AIRCRAFT_MODELS)
        if (strstr(model, m))
            return true;
    return false;
}

static bool HsIsMissile(void* e)
{
    return e && GameMode() == MODE_HS && !strcmp(ClassName(e), HS_MISSILE_CLASS);
}

static bool HsTooFar(void* e, float dist)
{
    return dist > HS_MISSILE_RANGE && HsIsMissile(e);
}

static float HsAimLead(void* target)
{
    return HsFlying(target) ? HS_AIR_LEAD : HS_ZOMBIE_LEAD;
}

// the closest missile in sight, or what the bot was going to shoot: it stays on a missile until that one is gone
static void* HsMissileFirst(void* bot, V3 eye, void* best)
{
    if (GameMode() != MODE_HS || HsIsMissile(best))
        return best;
    void* missile = nullptr;
    float closest = HS_MISSILE_RANGE;
    ForEntitiesNear(eye, HS_MISSILE_RANGE, [&](void* e) {
        if (!HsIsMissile(e) || !IsLivingMonster(e))
            return;
        float d = Len(Center(e) - eye);
        if (d < closest && CanSee(bot, eye, e))
        {
            closest = d;
            missile = e;
        }
    });
    return missile ? missile : best;
}

static bool HsNoChase(void* threat, void* target, V3 eye)
{
    if (GameMode() != MODE_HS)
        return false;
    return HsFlying(threat) || (target && Len(Center(target) - eye) > HS_CHASE_RANGE);
}

// the line along the bot's view gets as far as the aircraft, with no teammate in the way
static bool HsClearShotAt(void* target, const TraceResult& tr, float lineLength, float targetDist)
{
    if (!HsFlying(target) || tr.flFraction * lineLength < targetDist - HS_REACH_MARGIN)
        return false;
    return !(tr.pHit && (F<int>(Vars(tr.pHit), EV_FLAGS) & FL_CLIENT));
}

static void HsWatch()
{
    if (GameMode() != MODE_HS || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0)
        return;
    static float next = 0;
    float now = Now();
    if (now >= next - 60.0f && now < next)
        return;
    next = now + HS_WATCH_EVERY;
    static std::vector<std::string> seen;
    static int seenLevel = -1;
    if (seenLevel != g_nav.level)
    {
        seenLevel = g_nav.level;
        seen.clear();
    }
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        int flags = F<int>(v, EV_FLAGS);
        const char* cls = ClassName(e);
        const char* model = String(F<int>(v, HS_EV_MODEL));
        if ((flags & FL_CLIENT) || !(flags & FL_MONSTER))
            return;
        std::string kind = std::string(cls ? cls : "?") + " " + (model ? model : "?");
        if (std::find(seen.begin(), seen.end(), kind) != seen.end())
            return;
        seen.push_back(kind);
        Log("Human Scenario: enemy %s, health %.0f, %s, %s", kind.c_str(), F<float>(v, EV_HEALTH), IsIgnored(cls ? cls : "") ? "bots ignore it" : "bots shoot it",
            HsFlying(e) ? "aircraft" : "on the ground");
    });
}
