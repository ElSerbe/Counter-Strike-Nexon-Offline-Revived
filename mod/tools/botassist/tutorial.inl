// ---------------------------------------------------------------- the tutorial window's missions (Tutorial button)
// Tutorial 1 and 2 are the maps tuto_0 and tuto_0a. Each ends with a trigger_once that fires "endmm" (the "comp"
// message, a fade and trigger_endsection "_oem_end_training"). The game only ever asks the server for the window's bits
// (UpdateInfo RequestUpdateTutorial, always 0), so finishing them never counted and gave no reward (issues #3, #26).
// When that last trigger_once fires (it removes itself) or the player stands in it, this tells the server, which sets
// the window's bit and gives its reward (POST /api/tutorial, CMissionManager::OnTutorialUpdate). The Zombie Scenario
// tutorial (zs_zombieux) counts on the server when it's won, the Mission Chain when a mission request is done.
static const int EV_TARGET_TUTO = 0x1f8;

static volatile LONG g_tutoPosting = 0;

static DWORD WINAPI PostTutorialDone(LPVOID arg)
{
    char body[32];
    snprintf(body, sizeof(body), "{\"bit\":%d}", (int)(INT_PTR)arg);
    bool ok = HttpPost(L"/api/tutorial", body);
    Log("tutorial: bit %d finished, %s", (int)(INT_PTR)arg, ok ? "the server was told" : "the server didn't answer");
    InterlockedExchange(&g_tutoPosting, 0);
    return 0;
}

static void TutorialFrame()
{
    static int level = -1, bit = -1;
    static std::string lastMap;
    static void* endTrigger = nullptr;
    static V3 lo, hi;
    static bool done = false;
    static float next = 0;
    float now = Now();
    // (a new map or the same one again: the clock goes back; g_nav.level only moves with bots, the tutorial has none)
    int lv = CheckClock();
    const char* mapName = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    std::string map = mapName ? mapName : "";
    for (auto& ch : map)
        ch = (char)tolower((unsigned char)ch);
    if (lv != level || map != lastMap)
    {
        level = lv;
        lastMap = map;
        endTrigger = nullptr;
        done = false;
        next = 0;
        bit = map == "tuto_0" ? 1 : map == "tuto_0a" ? 2 : -1;
    }
    if (bit < 0 || done || (now < next && next - now <= 0.5f))
        return;
    next = now + 0.25f;
    if (!endTrigger)
    {
        ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
            const char* target = String(F<int>(Vars(e), EV_TARGET_TUTO));
            if (!endTrigger && !strcmp(ClassName(e), "trigger_once") && target && !strcmp(target, "endmm"))
            {
                endTrigger = e;
                lo = Vec((float*)(Vars(e) + EV_ABSMIN));
                hi = Vec((float*)(Vars(e) + EV_ABSMAX));
            }
        });
        if (endTrigger)
            Log("tutorial: map for bit %d, its end at (%.0f %.0f %.0f)-(%.0f %.0f %.0f)", bit, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        return;
    }
    // fired: the edict is freed or reused by something else
    const char* cls = ClassName(endTrigger);
    const char* target = String(F<int>(Vars(endTrigger), EV_TARGET_TUTO));
    bool fired = !cls || strcmp(cls, "trigger_once") || !target || strcmp(target, "endmm");
    void* you = YourEdict();
    if (!fired && you)
    {
        V3 o = Origin(you);
        fired = o.x >= lo.x - 16 && o.x <= hi.x + 16 && o.y >= lo.y - 16 && o.y <= hi.y + 16 && o.z >= lo.z - 72 && o.z <= hi.z + 72;
    }
    if (!fired || g_tutoPosting)
        return;
    done = true;
    InterlockedExchange(&g_tutoPosting, 1);
    HANDLE h = CreateThread(NULL, 0, PostTutorialDone, (LPVOID)(INT_PTR)bit, 0, NULL);
    if (h)
        CloseHandle(h);
    else
    {
        InterlockedExchange(&g_tutoPosting, 0);
        done = false;
    }
}
