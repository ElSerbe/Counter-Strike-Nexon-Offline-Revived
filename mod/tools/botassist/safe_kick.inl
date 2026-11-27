// ---------------------------------------------------------------- kicking a bot without leaving monsters aimed at it
// Zombie Scenario's monsters (CMonster, its AI CBaseZombieImprov at +0x1294) keep their target as a plain pointer
// (improv +0x1d2c). Each think they drop it when it's no longer a player that counts (vfunc 0xbc: not FL_SPECTATOR) and
// it's hidden (EF_NODRAW), or when its flags are just FL_DORMANT (mp.dll 0x1135c9df..0x1135ca82). A bot kicked while
// a monster aimed at it left the pointer to a freed player, and the monster's next think called through it: the game
// closed (issue #53: a person joining took bot 'Diego''s place, mp.dll+0x135c9fa calls a null vfunc, 2026-11-27).
// So a bot about to be kicked is first made a hidden spectator, and kicked a second later, when every monster has
// let it go.
static const int FL_SPECTATOR_BA = 1 << 26, EV_EFFECTS_KICK = 0x128, EF_NODRAW_KICK = 128;

struct PendingKick
{
    std::string name;
    float at;
};
static std::vector<PendingKick> g_pendingKicks;

// the bot's edict by its name, or null
static void* BotByName(const char* name)
{
    void* found = nullptr;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (found || !(F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT))
            return;
        const char* n = String(F<int>(Vars(e), EV_NETNAME));
        if (n && !strcmp(n, name))
            found = e;
    });
    return found;
}

// hides the bot from monsters now and kicks it in a second (by name: the "kick" command)
static void KickSoon(void* bot, const char* name)
{
    if (!name || !*name || strchr(name, '"'))
        return;
    for (auto& k : g_pendingKicks)
        if (k.name == name)
            return;
    if (!bot)
        bot = BotByName(name);
    if (bot)
    {
        unsigned char* v = Vars(bot);
        F<int>(v, EV_FLAGS) |= FL_SPECTATOR_BA;
        F<int>(v, EV_EFFECTS_KICK) |= EF_NODRAW_KICK;
        F<float>(v, EV_TAKEDAMAGE) = 0.0f;
    }
    g_pendingKicks.push_back({ name, Now() + 1.0f });
}

// on the game's thread, every frame: the kicks whose second is up (or whose clock went back: a new map)
static void PendingKicksFrame()
{
    if (g_pendingKicks.empty() || !g_ServerCommand)
        return;
    float now = Now();
    for (size_t i = 0; i < g_pendingKicks.size();)
    {
        auto& k = g_pendingKicks[i];
        if (now < k.at && now >= k.at - 5.0f)
        {
            i++;
            continue;
        }
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "kick \"%s\"\n", k.name.c_str());
        g_ServerCommand(cmd);
        g_pendingKicks.erase(g_pendingKicks.begin() + i);
    }
}
