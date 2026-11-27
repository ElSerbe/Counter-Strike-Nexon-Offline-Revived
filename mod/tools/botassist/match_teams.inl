// ---------------------------------------------------------------- each player's side, for rounds won (the server)
// In the zombie PvP modes the sides change every round (infections), so "won the match" by the room's team meant little
// for the orders and missions that ask to win (the user, 2026-11-27: count rounds won instead). The server learns who
// won each round from the game (its win counters); which side each player was on then, only the game knows. This sends
// it: the people (not bots) and their team (1 T, 2 CT) by name, POST /api/matchteams, whenever it changes (an
// infection, a new round), checked twice a second. The server counts a round won for those on the winning side
// (CGameMatch::OnUpdateWinCounter). Only the hosting game runs server frames, so only the host's botassist sends it.
// It also sends "humans": the players on the human side (CT) still alive, bots included. In these modes the game's win
// counters don't say which side won a round (2026-11-27 test: a zombie win moved one counter, the next one another,
// later ones only a third), so the server takes the counter packet as "a round ended" and the winner from this count:
// a human alive at the end, the humans won; none, the zombies did.
static bool RoundZombieModeBa(int mode)
{
    static const int modes[] = { 8, 9, 14, 20, 24, 29, 30, 32, 35, 36, 37, 43, 44, 45, 51, 54, 56, 57, 58 }; // (= server)
    for (int m : modes)
        if (m == mode)
            return true;
    return false;
}

static volatile LONG g_teamsPosting = 0;

static DWORD WINAPI PostMatchTeams(LPVOID arg)
{
    std::string* body = (std::string*)arg;
    HttpPost(L"/api/matchteams", body->c_str());
    delete body;
    InterlockedExchange(&g_teamsPosting, 0);
    return 0;
}

static void MatchTeamsFrame()
{
    static float next = 0;
    static int level = -1;
    static std::string last;
    float now = Now();
    if (level != g_nav.level)
        level = g_nav.level, next = 0, last.clear();
    if (now < next && next - now <= 0.5f)
        return;
    next = now + 0.5f;
    if (!RoundZombieModeBa(GameMode()) || g_teamsPosting)
        return;
    std::string json = "{\"teams\":{";
    int n = 0, humans = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS);
        if (!(flags & FL_CLIENT))
            return;
        int team = TeamOf(e);
        if (team == 2 && Alive(e))
            humans++;
        if (flags & FL_FAKECLIENT)
            return;
        const char* name = String(F<int>(Vars(e), EV_NETNAME));
        if (!name || !*name || (team != 1 && team != 2))
            return;
        std::string esc;
        for (const char* c = name; *c; c++)
        {
            if (*c == '"' || *c == '\\')
                esc += '\\';
            if ((unsigned char)*c >= 0x20)
                esc += *c;
        }
        json += (n++ ? ",\"" : "\"") + esc + "\":" + std::to_string(team);
    });
    json += "},\"humans\":" + std::to_string(humans) + "}";
    if (!n || json == last)
        return;
    last = json;
    InterlockedExchange(&g_teamsPosting, 1);
    std::string* body = new std::string(json);
    HANDLE h = CreateThread(NULL, 0, PostMatchTeams, body, 0, NULL);
    if (h)
        CloseHandle(h);
    else
    {
        delete body;
        InterlockedExchange(&g_teamsPosting, 0);
        last.clear();
    }
}
