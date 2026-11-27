// ---------------------------------------------------------------- rounds won in the zombie PvP modes (the server)
// In the zombie PvP modes the sides change every round (infections), so "won the match" by the room's team meant little
// for the orders and missions that ask to win (the user, 2026-11-27: count rounds won instead). Which side won a round,
// and which side each player was on then, only the game knows: the win counters it sends the server don't say
// (2026-11-27 test, Zombie Classic: a zombie win moved one counter, the next one another, later ones only a third).
//
// Every round's end goes through CHalfLifeMultiplay::TerminateRound 0x11491680 (cdecl: float delay, int win status;
// the zombie modes from their round end 0x11448b90, vtable +0x20c, Hero and Mutation through it too): it stores the
// status at rules +0x274 and sets +0x309 (the round is ending; nothing else sets it). Status 1 the CTs (humans) won,
// 2 the Terrorists (zombies), 3 a draw, 4 the side with more rounds. botassist hooks it and tells the server then: the
// winning side and each person's (not bots) side by name, POST /api/roundend; the server counts a round won or lost
// for each (CGameMatch::OnRoundEnd). Only the hosting game runs the rules, so only the host's botassist sends it.
// (MP_TERMINATE_ROUND: modes/zombie_disaster_zone.inl)
static const DWORD MP_GAMERULES_PTR = 0x121d343c;
static const int RULES_ROUND_ENDING = 0x309, RULES_CT_WINS = 0x278, RULES_T_WINS = 0x27a;
typedef void(__cdecl* TerminateRoundFn)(float delay, int status);
static TerminateRoundFn g_TerminateRoundTramp;

static bool RoundZombieModeBa(int mode)
{
    static const int modes[] = { 8, 9, 14, 20, 24, 29, 30, 32, 35, 36, 37, 43, 44, 45, 51, 54, 56, 57, 58 }; // (= server)
    for (int m : modes)
        if (m == mode)
            return true;
    return false;
}

static DWORD WINAPI PostRoundEnd(LPVOID arg)
{
    std::string* body = (std::string*)arg;
    bool ok = HttpPost(L"/api/roundend", body->c_str());
    if (!ok)
        Log("round end: the server didn't answer (%s)", body->c_str());
    delete body;
    return 0;
}

// the people (not bots) and their side now, 1 T 2 CT: {"<name>":1,...}
static std::string PeopleSides(int& n)
{
    std::string json = "{";
    n = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS);
        if (!(flags & FL_CLIENT) || (flags & FL_FAKECLIENT))
            return;
        const char* name = String(F<int>(Vars(e), EV_NETNAME));
        int team = TeamOf(e);
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
    return json + "}";
}

// right after the game ended a round (the rules already hold the result); ending: the round was already ending before
// this call (a second call for the same round counts once)
static void OnRoundTerminated(int status, bool ending, unsigned char* rules)
{
    int mode = GameMode();
    if (!RoundZombieModeBa(mode) || ending)
        return;
    int winner = status == 1 ? 2 : status == 2 ? 1 : 0;
    if (status == 4 && rules)
    {
        short ct = *(short*)(rules + RULES_CT_WINS), t = *(short*)(rules + RULES_T_WINS);
        winner = ct > t ? 2 : t > ct ? 1 : 0;
    }
    int n;
    std::string sides = PeopleSides(n);
    Log("round end: mode %d, status %d -> %s won; people %s", mode, status,
        winner == 2 ? "the humans" : winner == 1 ? "the zombies" : "nobody", sides.c_str());
    if (!n)
        return;
    std::string* body = new std::string("{\"winner\":" + std::to_string(winner) + ",\"teams\":" + sides + "}");
    HANDLE h = CreateThread(NULL, 0, PostRoundEnd, body, 0, NULL);
    if (h)
        CloseHandle(h);
    else
        delete body;
}

static void __cdecl HookedTerminateRound(float delay, int status)
{
    unsigned char* rules = nullptr;
    bool ending = false;
    __try
    {
        rules = *(unsigned char**)(MP_GAMERULES_PTR + g_mpDelta);
        ending = rules && rules[RULES_ROUND_ENDING];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        rules = nullptr;
    }
    g_TerminateRoundTramp(delay, status);
    __try
    {
        OnRoundTerminated(status, ending, rules);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("round end: exception 0x%08lX (ignored)", GetExceptionCode());
    }
}

static void InstallRoundEndHook(DWORD delta)
{
    if (g_TerminateRoundTramp)
        return;
    unsigned char* fn = (unsigned char*)(MP_TERMINATE_ROUND + delta);
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68 }; // push ebp; mov ebp, esp; push -1; push imm32
    if (Readable(fn, 10) && !memcmp(fn, code, sizeof(code)))
        g_TerminateRoundTramp = (TerminateRoundFn)DetourCdecl(fn, 10, (void*)HookedTerminateRound);
    Log("round end hook: %s", g_TerminateRoundTramp ? "on (zombie rounds won go to the server)" : "NOT hooked (unexpected code)");
}
