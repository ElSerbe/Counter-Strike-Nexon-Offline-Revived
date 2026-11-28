// ---------------------------------------------------------------- match events the server can't see (orders, missions)
// Two things the server never heard from the game (user's report, 2026-11-28):
//  - Soccer's result. Its rules (CSOGameRulesSoccer, win check 0x1143cda0) end the match on mp_winlimit goals or the
//    time limit by setting the CSO match object's "over" (+0x27d), without a round end, so the game never sends the win
//    counter: every soccer match ended 0-0 for the server and "Win 1 matches in Soccer" never moved. The goals are the
//    CSO match object's words +0x280 (CT) and +0x282 (T) (0x1143def0 / 0x1143df30 add a goal, capped at 127; the win
//    check compares mp_winlimit with +0x282 for "#Terrorists_Win", +0x280 for "#CTs_Win"). This sends them when they
//    change, POST /api/teamscore {"ct":N,"t":N}; the server takes them for who won when no win counter came.
//  - A planted bomb. The game's event for a bomb (the server's OnGameEvent) comes when it goes off (the planter) or is
//    defused (the defuser), so "plant or defuse N bombs" counted a plant only when the bomb went off. This sends a plant
//    when a person's C4 appears ("grenade" with models/w_c4.mdl, pev->owner the planter) and a defuse when a round ends
//    with the CTs' win (TerminateRound status 1, round_end.inl) while a planted C4 is still there: the person nearest to
//    it (the defuser stands within use range) if no bot is nearer. POST /api/bomb {"event":"plant"|"defuse","name":..};
//    the server counts each bomb once with the game's own event (CGameMatch::OnBombReport).
// Only the hosting game runs the rules, so only the host's botassist sends these.
static const int ME_EV_MODEL = 0xc8, ME_EV_OWNER = 0x1bc;
static const int CSO_SCORE_CT = 0x280, CSO_SCORE_T = 0x282;

struct MePost
{
    const wchar_t* path;
    std::string body;
};

static DWORD WINAPI MePostThread(LPVOID arg)
{
    MePost* p = (MePost*)arg;
    if (!HttpPost(p->path, p->body.c_str()))
        Log("match events: the server didn't answer (%ls %s)", p->path, p->body.c_str());
    delete p;
    return 0;
}

static void MePostAsync(const wchar_t* path, const std::string& body)
{
    MePost* p = new MePost{ path, body };
    HANDLE h = CreateThread(NULL, 0, MePostThread, p, 0, NULL);
    if (h)
        CloseHandle(h);
    else
        delete p;
}

static std::string MeJsonString(const char* s)
{
    std::string out = "\"";
    for (const char* c = s; c && *c; c++)
    {
        if (*c == '"' || *c == '\\')
            out += '\\';
        if ((unsigned char)*c >= 0x20)
            out += *c;
    }
    return out + "\"";
}

static const char* MePersonName(void* e)
{
    if (!e)
        return nullptr;
    int flags = F<int>(Vars(e), EV_FLAGS);
    if (!(flags & FL_CLIENT) || (flags & FL_FAKECLIENT))
        return nullptr;
    const char* name = String(F<int>(Vars(e), EV_NETNAME));
    return name && *name ? name : nullptr;
}

// ---- Soccer's score
static void SoccerScoreFrame()
{
    static int level = -1, lastCt = -1, lastT = -1;
    static float next = 0, resend = 0;
    if (GameMode() != 16)
        return;
    float now = Now();
    int lv = CheckClock();
    if (lv != level)
        level = lv, lastCt = lastT = -1, next = resend = 0;
    if (now < next && next - now <= 0.5f)
        return;
    next = now + 0.25f;
    unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
    if (!cso || !Readable(cso, 0x290))
        return;
    int ct = *(unsigned short*)(cso + CSO_SCORE_CT), t = *(unsigned short*)(cso + CSO_SCORE_T);
    // on a change, and again every 30 s (a match the server opened after the first report)
    bool changed = ct != lastCt || t != lastT;
    if (!changed && now < resend && resend - now <= 30.0f)
        return;
    resend = now + 30.0f;
    if (changed)
        Log("soccer: score CT %d, T %d", ct, t);
    lastCt = ct, lastT = t;
    MePostAsync(L"/api/teamscore", "{\"ct\":" + std::to_string(ct) + ",\"t\":" + std::to_string(t) + "}");
}

// ---- bombs planted and defused
struct MeBomb
{
    void* e;
    V3 at;
};
static std::vector<MeBomb> g_meBombs;

static bool MeIsPlantedC4(void* e)
{
    if (strcmp(ClassName(e), "grenade"))
        return false;
    const char* model = String(F<int>(Vars(e), ME_EV_MODEL));
    return model && strstr(model, "w_c4.mdl");
}

static void BombFrame()
{
    static int level = -1;
    static float next = 0;
    int mode = GameMode();
    if (mode == 16 || CoopMode() || RoundZombieModeBa(mode))
        return;
    float now = Now();
    int lv = CheckClock();
    if (lv != level)
        level = lv, g_meBombs.clear(), next = 0;
    if (now < next && next - now <= 0.5f)
        return;
    next = now + 0.2f;
    std::vector<MeBomb> seen;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!MeIsPlantedC4(e))
            return;
        V3 at = Vec((float*)(Vars(e) + EV_ORIGIN));
        seen.push_back({ e, at });
        for (auto& b : g_meBombs)
            if (b.e == e)
                return;
        // a new one: planted just now
        void* owner = *(void**)(Vars(e) + ME_EV_OWNER);
        const char* name = MePersonName(owner);
        Log("bomb: planted by %s", name ? name : "a bot");
        if (name)
            MePostAsync(L"/api/bomb", "{\"event\":\"plant\",\"name\":" + MeJsonString(name) + "}");
    });
    g_meBombs = seen;
}

// a round is ending (TerminateRound, before the game's own call): the CTs won with a planted C4 still there = defused
static void BombRoundEnd(int status, bool ending)
{
    if (ending || status != 1 || g_meBombs.empty())
        return;
    for (auto& b : g_meBombs)
    {
        if (!MeIsPlantedC4(b.e))
            continue;
        void* best = nullptr;
        float bestD = 160.0f;
        ForEntitiesNear(b.at, 160.0f, [&](void* e) {
            int flags = F<int>(Vars(e), EV_FLAGS);
            if (!(flags & FL_CLIENT) || !Alive(e) || TeamOf(e) != 2)
                return;
            float d = Len(Vec((float*)(Vars(e) + EV_ORIGIN)) - b.at);
            if (d < bestD)
                bestD = d, best = e;
        });
        const char* name = MePersonName(best);
        Log("bomb: defused by %s", name ? name : best ? "a bot" : "nobody near it (not counted)");
        if (name)
            MePostAsync(L"/api/bomb", "{\"event\":\"defuse\",\"name\":" + MeJsonString(name) + "}");
        break;
    }
    g_meBombs.clear();
}

static void MatchEventsFrame()
{
    SoccerScoreFrame();
    BombFrame();
}
