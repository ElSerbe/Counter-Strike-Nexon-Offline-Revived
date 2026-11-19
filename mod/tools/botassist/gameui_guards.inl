// ---------------------------------------------------------------- GameUI: a bad picture doesn't crash the game
// The game crashed at the end of a Zombie Hero Classic match (2026-11-19, crash dump): a list of the results window
// (its layout 0x10737c20, a cell's picture from the column's map) gave an image panel an image pointer that was text
// ("anel", 0x6c656e61), and the panel's SetImage (0x10711d70, thiscall image, ret 4; image + 0xb8, then the image's
// vfunc 0x10 for its size) called through it. SetImage now gets no image instead of one that isn't an object: its
// pointer and its vtable must be readable and the vtable inside a loaded module. The cell shows no picture.
static const DWORD GAMEUI_IMAGEPANEL_SETIMAGE = 0x10711d70;
typedef void(__fastcall* SetImageFn)(void* self, void* edx, void* image);
static SetImageFn g_setImageTramp = nullptr;

static bool GuiObjectLooksValid(void* object)
{
    __try
    {
        if (!Readable(object, 4))
            return false;
        void** vt = *(void***)object;
        if (!Readable(vt, 0x18))
            return false;
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)vt, &m) || !m)
            return false;
        // the slots SetImage and painting call (size 0x10, 0x14) point into code of a module too
        HMODULE f = nullptr;
        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)vt[4], &f) && f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void __fastcall GuardedSetImage(void* self, void* edx, void* image)
{
    if (image && !GuiObjectLooksValid(image))
    {
        static int reports = 0;
        if (reports++ < 20)
        {
            HMODULE ui = GetModuleHandleA("GameUI.dll");
            DWORD from = (DWORD)_ReturnAddress();
            Log("GameUI: an image panel was given a bad picture (0x%08lX) from 0x%08lX: shown without it (no crash)",
                (DWORD)image, ui ? from - (DWORD)ui + 0x10000000 : from);
        }
        image = nullptr;
    }
    g_setImageTramp(self, edx, image);
}

static void GuardGameUiImages(HMODULE gameUi)
{
    unsigned char* fn = (unsigned char*)(GAMEUI_IMAGEPANEL_SETIMAGE - 0x10000000 + (DWORD)gameUi);
    // push ebp; mov ebp, esp; push ecx; push esi (5 bytes, no relative jumps)
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x51, 0x56, 0x8b, 0xf1 };
    if (!Readable(fn, sizeof(code)) || memcmp(fn, code, sizeof(code)))
    {
        Log("GameUI: unexpected code in the image panel's SetImage: no bad-picture guard");
        return;
    }
    g_setImageTramp = (SetImageFn)DetourCdecl(fn, 5, (void*)GuardedSetImage);
    Log("GameUI: image panels refuse bad pictures %s", g_setImageTramp ? "(no crash at the results window)" : "(NOT hooked)");
}

// ---------------------------------------------------------------- GameUI: a random map with no map to draw
// The game crashed choosing Zombie Shelter in a room of 32 (2026-11-19, crash dump: integer divide by zero at
// 0x105d82ce). GameUI's random map pick (0x105d8130(mode, ...): the MapList maps of the mode's column whose
// max_player (column 28) is at least the room's players, then rand() % count) found none: the mode's maps are for
// 4 or 8 players and room bots count as players. With none it now returns -1, what it returns when the map table
// isn't loaded (0x105d82ef): the random tile has no map instead of crashing the game.
static const DWORD GAMEUI_RANDOMMAP_COUNT = 0x105d82a7; // 8b 7d d4 8b 4d d0: mov edi, [ebp-0x2c]; mov ecx, [ebp-0x30]
static const DWORD GAMEUI_RANDOMMAP_DRAW = 0x105d82ad;  // sub edi, ecx (the count, then the draw)
static const DWORD GAMEUI_RANDOMMAP_NONE = 0x105d82ef;  // or eax, -1 and the function's end
static DWORD g_randomMapDraw, g_randomMapNone;
static volatile LONG g_randomMapEmpty;

static void __cdecl RandomMapNone()
{
    if (InterlockedIncrement(&g_randomMapEmpty) <= 10)
        Log("GameUI: the random map pick found no map for this mode and room size: no random map (no crash)");
}

static __declspec(naked) void RandomMapStub()
{
    __asm {
        mov edi, dword ptr [ebp - 0x2c] // the replaced instructions: the candidates' end and begin
        mov ecx, dword ptr [ebp - 0x30]
        cmp edi, ecx
        je none
        jmp dword ptr [g_randomMapDraw]
    none:
        pushad
        call RandomMapNone
        popad
        jmp dword ptr [g_randomMapNone] // (nothing was added: the list never allocated)
    }
}

static void GuardGameUiRandomMap(HMODULE gameUi)
{
    DWORD delta = (DWORD)gameUi - 0x10000000;
    unsigned char* site = (unsigned char*)(GAMEUI_RANDOMMAP_COUNT + delta);
    static const unsigned char code[] = { 0x8b, 0x7d, 0xd4, 0x8b, 0x4d, 0xd0, 0x2b, 0xf9 };
    if (!Readable(site, sizeof(code)) || memcmp(site, code, sizeof(code)))
    {
        Log("GameUI: unexpected code in the random map pick: no empty-list guard");
        return;
    }
    g_randomMapDraw = GAMEUI_RANDOMMAP_DRAW + delta;
    g_randomMapNone = GAMEUI_RANDOMMAP_NONE + delta;
    DWORD old;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xe9;
    *(int*)(site + 1) = (int)((unsigned char*)RandomMapStub - (site + 5));
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    Log("GameUI: the random map pick survives a mode with no map for the room's size");
}
