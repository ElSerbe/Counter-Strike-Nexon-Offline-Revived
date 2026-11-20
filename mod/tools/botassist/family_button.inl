// ---------------------------------------------------------------- the family window's "View Benefits" button
// GameUI's family info panel runs "ShowBenefits" (0x101e3f10: strcmp, then 0x101e488a: the main panel's section 2,
// the missions / season pass window, which in this client has nothing about the family). It now opens CSNZ_Server's
// family page instead (web/family: level, EXP, points, bonuses, members, chronicle, the Family achievements), like the
// room's Extra Settings button. Holding Shift, or the server not answering, does what the game did.
static const DWORD GAMEUI_SHOWBENEFITS = 0x101e488a;          // e8 31 e0 26 00 (call 0x104528c0)
static const DWORD GAMEUI_SHOWBENEFITS_CALL = 0x104528c0;     // the call that instruction made
static const DWORD GAMEUI_SHOWBENEFITS_NEXT = 0x101e488f;     // the game's code after it
static const DWORD GAMEUI_SHOWBENEFITS_END = 0x101e48dd;      // the command's end (SEH frame, pops, ret)
static DWORD g_benefitsCall, g_benefitsNext, g_benefitsEnd, g_benefitsOpened;

static int __cdecl OnViewBenefitsButton()
{
    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        Log("family View Benefits button (Shift): the game's own window");
        return 0;
    }
    bool told = HttpPost(L"/api/openview", "{\"view\":\"family\"}");
    Log("family View Benefits button: %s", told ? "opening the family page" : "server not reachable, the game's own window");
    void* panel = told ? *(void**)(GAMEUI_MAINPANEL + g_gameUiDelta) : nullptr;
    if (!panel)
        return 0;
    ((OpenWebEventFn)(GAMEUI_OPENWEBEVENT + g_gameUiDelta))(panel, nullptr);
    return 1;
}

static __declspec(naked) void ViewBenefitsStub()
{
    __asm {
        pushad
        call OnViewBenefitsButton
        mov g_benefitsOpened, eax
        popad
        cmp g_benefitsOpened, 0
        jne opened
        call dword ptr [g_benefitsCall] // the call the jump replaced
        jmp dword ptr [g_benefitsNext]
    opened:
        jmp dword ptr [g_benefitsEnd]
    }
}

static void HookViewBenefitsButton()
{
    unsigned char* site = (unsigned char*)(GAMEUI_SHOWBENEFITS + g_gameUiDelta);
    if (!Readable(site, 5) || site[0] == 0xe9)
        return;
    int rel = site[0] == 0xe8 ? *(int*)(site + 1) : 0;
    if (site[0] != 0xe8 || (DWORD)(site + 5 + rel) != GAMEUI_SHOWBENEFITS_CALL + g_gameUiDelta)
    {
        Log("GameUI family panel has unexpected code: View Benefits button left as it is");
        return;
    }
    g_benefitsCall = GAMEUI_SHOWBENEFITS_CALL + g_gameUiDelta;
    g_benefitsNext = GAMEUI_SHOWBENEFITS_NEXT + g_gameUiDelta;
    g_benefitsEnd = GAMEUI_SHOWBENEFITS_END + g_gameUiDelta;
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xe9;
    *(int*)(site + 1) = (int)((unsigned char*)ViewBenefitsStub - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("family View Benefits button opens the family page (Shift: the game's own window)");
}
