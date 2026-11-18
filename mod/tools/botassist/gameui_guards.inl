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
