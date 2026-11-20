// ---------------------------------------------------------------- melee weapons with no server code
// mp.dll keeps one "knife helper" per knife index (Item.csv InGameID - 4000 + 2; models in the table at 0x11c81610) in a
// std::map filled at start (0x11096b30: registrar, index, helper; map at 0x120bbfcc). A few helpers of this client's
// mp.dll are empty shells: their Deploy (vtable slot 2) is the shared do-nothing 0x106581d0, so the weapon never gets a
// model and never hits (the server code of those weapons was only on Nexon's dedicated servers). Seen 2026-11-20: the
// Miracle Prism Sword (magicknife, index 70) was invisible and silent in every mode, the player's pev viewmodel and
// weaponmodel left empty. Shells: MagicKnife (70), MagicKnifeZF (127, no model in the files), Tyrantmace,
// Y25S1RuneBlade (neither sold).
// A helper is {vtable, its weapon's data (a *_prop object: +0x18 the knife index its models and the client's weapon
// come from), ...}. The fix gives index 70 a copy of the Frostblade's helper (the Prism Sword's sibling, 8383/8384,
// same base type "sfsword") whose data copy says index 70: the server plays the Frostblade's attacks with the Prism
// Sword's models, and client.dll (which has the full MagicKnife) its animations and sounds. If anything doesn't look
// as expected, the Y25S2 SF Sword's helper is used as it is (works, but looks and is named "Beam Sword Universe": first
// version, seen working in game 2026-11-20).
static const DWORD MP_KNIFE_REGISTER = 0x11096b30, MP_KNIFE_REGISTRAR = 0x11fb95fc, MP_KNIFE_STUB_DEPLOY = 0x106581d0;
static const DWORD MP_HELPER_MAGICKNIFE = 0x11c0fa50;
static const DWORD MP_HELPER_FROSTBLADE = 0x11c11b14, MP_VT_FROSTBLADE = 0x11864abc, MP_VT_FROSTBLADE_PROP = 0x11864a04;
static const DWORD MP_HELPER_SFSWORD25 = 0x11ffb454, MP_VT_SFSWORD25 = 0x1186cddc;
static const int KNIFE_PROP_INDEX = 0x18, KNIFE_PROP_SIZE = 0xba0; // (the Runeblade's data ends where the Frostblade's starts)
typedef void*(__fastcall* KnifeRegisterFn)(void* registrar, void* edx, int index, void* helper);

static bool KnifeHelperIsShell(DWORD helper, DWORD delta)
{
    DWORD* obj = (DWORD*)(helper + delta);
    if (!Readable(obj, 4))
        return false;
    DWORD* vt = (DWORD*)*obj;
    return vt && Readable(vt + 2, 4) && vt[2] == MP_KNIFE_STUB_DEPLOY + delta;
}

// a copy of the Frostblade's helper whose data says knife index `index` (nullptr when it isn't as expected)
static void* CloneFrostbladeHelper(DWORD delta, int index)
{
    DWORD* src = (DWORD*)(MP_HELPER_FROSTBLADE + delta);
    if (!Readable(src, 12) || src[0] != MP_VT_FROSTBLADE + delta)
        return nullptr;
    BYTE* data = (BYTE*)src[1];
    if (!data || !Readable(data, KNIFE_PROP_SIZE) || *(DWORD*)data != MP_VT_FROSTBLADE_PROP + delta ||
        *(int*)(data + KNIFE_PROP_INDEX) != 69)
        return nullptr;
    BYTE* copy = (BYTE*)VirtualAlloc(nullptr, KNIFE_PROP_SIZE + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!copy)
        return nullptr;
    memcpy(copy, data, KNIFE_PROP_SIZE);
    int moved = 0; // pointers into the object itself follow the copy
    for (int i = 0; i + 4 <= KNIFE_PROP_SIZE; i += 4)
    {
        DWORD v = *(DWORD*)(copy + i);
        if (v >= (DWORD)data && v < (DWORD)data + KNIFE_PROP_SIZE)
        {
            *(DWORD*)(copy + i) = v - (DWORD)data + (DWORD)copy;
            moved++;
        }
    }
    *(int*)(copy + KNIFE_PROP_INDEX) = index;
    DWORD* helper = (DWORD*)(copy + KNIFE_PROP_SIZE + 16);
    memcpy(helper, src, 12);
    helper[1] = (DWORD)copy;
    Log("melee fix: Frostblade helper copied for knife %d (%d pointers into its data moved)", index, moved);
    return helper;
}

static int g_knifeFix = 0; // 0 not yet, 1 the Y25S2 stand-in (retried each frame until the clone works), 2 the clone
static float g_knifeFixNext = 0;

static void FixShellKnives(DWORD delta)
{
    if (g_knifeFix == 2)
        return;
    const BYTE expect[] = { 0x55, 0x8b, 0xec, 0x6a, 0xff }; // the register function's start
    BYTE* fn = (BYTE*)(MP_KNIFE_REGISTER + delta);
    if (!Readable(fn, sizeof(expect)) || memcmp(fn, expect, sizeof(expect)) ||
        (g_knifeFix == 0 && !KnifeHelperIsShell(MP_HELPER_MAGICKNIFE, delta)))
    {
        Log("melee fix: mp.dll isn't the expected one, the Miracle Prism Sword is left as it is");
        g_knifeFix = 2;
        return;
    }
    void* helper = nullptr;
    const char* how = "the Frostblade's attacks, its own models";
    __try { helper = CloneFrostbladeHelper(delta, 70); } __except (EXCEPTION_EXECUTE_HANDLER) { helper = nullptr; }
    if (!helper && g_knifeFix == 1)
        return; // (the stand-in is already there; the Frostblade's data isn't made yet)
    if (!helper)
    {
        DWORD* sub = (DWORD*)(MP_HELPER_SFSWORD25 + delta);
        if (!Readable(sub, 4) || *sub != MP_VT_SFSWORD25 + delta)
        {
            Log("melee fix: no helper to give the Miracle Prism Sword, left as it is");
            return;
        }
        helper = sub;
        how = "the Y25S2 SF Sword (looks and is named Beam Sword Universe)";
    }
    __try
    {
        ((KnifeRegisterFn)fn)((void*)(MP_KNIFE_REGISTRAR + delta), nullptr, 70, helper);
        g_knifeFix = helper == (void*)(MP_HELPER_SFSWORD25 + delta) ? 1 : 2;
        Log("melee fix: knife 70, Miracle Prism Sword (magicknife), had no server code: it plays as %s", how);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("melee fix: knife 70 couldn't be given another helper (exception 0x%08lx)", GetExceptionCode());
    }
}

// every frame (the frame hook): until the clone is in, tried again every few seconds (the Frostblade's data object is
// made the first time the game asks for it)
static void KnifeFixFrame()
{
    if (g_knifeFix != 1)
        return;
    float now = Now();
    if (now < g_knifeFixNext && now > g_knifeFixNext - 10.0f)
        return;
    g_knifeFixNext = now + 3.0f;
    FixShellKnives(g_mpDelta);
}
