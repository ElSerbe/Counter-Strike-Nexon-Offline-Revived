// ---------------------------------------------------------------- the room's "Extra Settings" button
// GameUI's room panel runs the "DetailSetting" command (CRoomPanel OnCommand: strcmp at 0x105b877b, the dialog code
// at 0x105b87ab opens CDetailGameOptionDlgV3). It now opens CSNZ_Server's room options page instead (web/room: the
// game's own options of the mode, the bots' options, the host's defaults), like the AI settings button. Holding Shift
// while clicking it, or the server not answering, opens the game's own window as before.
static const DWORD GAMEUI_DETAILSETTING = 0x105b87ab;        // 8b 8f e4 01 00 00 (mov ecx, [edi+0x1e4])
static const DWORD GAMEUI_DETAILSETTING_NEXT = 0x105b87b1;   // the dialog code after that instruction
static const DWORD GAMEUI_DETAILSETTING_EPILOGUE = 0x105b8802; // the branch's own end (SEH frame, pops, ret 4)
static DWORD g_detailNative, g_detailDone, g_detailOpened;

static int __cdecl OnExtraSettingsButton()
{
    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        Log("room Extra Settings button (Shift): the game's own window");
        return 0;
    }
    bool told = HttpPost(L"/api/openview", "{\"view\":\"room\"}");
    Log("room Extra Settings button: %s", told ? "opening the room options page" : "server not reachable, the game's own window");
    void* panel = told ? *(void**)(GAMEUI_MAINPANEL + g_gameUiDelta) : nullptr;
    if (!panel)
        return 0;
    ((OpenWebEventFn)(GAMEUI_OPENWEBEVENT + g_gameUiDelta))(panel, nullptr);
    return 1;
}

static __declspec(naked) void ExtraSettingsStub()
{
    __asm {
        pushad
        call OnExtraSettingsButton
        mov g_detailOpened, eax
        popad
        cmp g_detailOpened, 0
        jne opened
        mov ecx, dword ptr [edi + 0x1e4] // the instruction the jump replaced
        jmp dword ptr [g_detailNative]
    opened:
        jmp dword ptr [g_detailDone]
    }
}

static void HookExtraSettingsButton()
{
    unsigned char* site = (unsigned char*)(GAMEUI_DETAILSETTING + g_gameUiDelta);
    if (!Readable(site, 6) || site[0] == 0xe9)
        return;
    static const unsigned char expect[6] = { 0x8b, 0x8f, 0xe4, 0x01, 0x00, 0x00 };
    if (memcmp(site, expect, 6))
    {
        Log("GameUI room panel has unexpected code: Extra Settings button left as it is");
        return;
    }
    g_detailNative = GAMEUI_DETAILSETTING_NEXT + g_gameUiDelta;
    g_detailDone = GAMEUI_DETAILSETTING_EPILOGUE + g_gameUiDelta;
    DWORD old;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xe9;
    *(int*)(site + 1) = (int)((unsigned char*)ExtraSettingsStub - (site + 5));
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    Log("room Extra Settings button opens the room options page (Shift: the game's own window)");
}
