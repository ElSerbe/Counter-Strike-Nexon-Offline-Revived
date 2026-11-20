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
static void PrismSwordFrame();
static void KnifeFixFrame()
{
    PrismSwordFrame();
    if (g_knifeFix != 1)
        return;
    float now = Now();
    if (now < g_knifeFixNext && now > g_knifeFixNext - 10.0f)
        return;
    g_knifeFixNext = now + 3.0f;
    FixShellKnives(g_mpDelta);
}

// ---------------------------------------------------------------- the Miracle Prism Sword's animations
// With the Frostblade's helper the server sends the Frostblade's animation numbers (CBasePlayerWeapon::SendWeaponAnim
// 0x1152d030, thiscall weapon, anim, skiplocal: pev weaponanim +0x248 and SVC_WEAPONANIM), and on v_magicknife.mdl
// they're other moves (the user, 2026-11-20: no recovery after a slash, the right click didn't look like the Prism
// Sword). Frostblade (helper 0x11864abc): 0 idle, 1 slash (one swing every 2.03 s), 2 draw; right click held: 9 start,
// 3, 5, 4, 6 the charge steps; released: 7 the charged swing, 8 the full charge's (state 5). v_magicknife.mdl (30 fps):
// 0 idle, 1-4 slash_1..4 (0.53 s), 5 slash_end (0.87 s), 6 draw, 7 charge_start, 8 charge_finish, 9 charge_idle1,
// 10 charge_idle2, 11 charge_attack1, 12 charge_attack2, 13 stab. botassist detours SendWeaponAnim: for a player whose
// view model is v_magicknife.mdl the number is translated; the slashes take turns (1..4) and slash_end follows each
// half a second later, unless something else was played meanwhile.
static const DWORD MP_SEND_WEAPON_ANIM = 0x1152d030;
static const int PL_ACTIVE_ITEM = 0x1024, EV_VIEWMODEL = 0xcc, EV_WEAPONANIM = 0x248, WPN_PLAYER_OFS = 0xd0;
static const int MK_SLASH_END = 5, MK_DRAW = 6;
typedef void(__fastcall* SendWeaponAnimFn)(void* weapon, void* edx, int anim, int skiplocal);
static SendWeaponAnimFn g_sendWeaponAnim;

struct PrismSwing
{
    void* weapon = nullptr;
    int anim = -1;          // the slash sent
    float endAt = 0, lastSlash = -100;
    int combo = 0;
};
static PrismSwing g_prism[33];

static int PrismPlayerIndex(unsigned char* player)
{
    unsigned char* pev = player ? *(unsigned char**)(player + 8) : nullptr;
    if (!pev || !g_IndexOfEdict)
        return -1;
    int i = g_IndexOfEdict(*(void**)(pev + EV_CONTAINING_ENTITY));
    return i >= 1 && i <= 32 ? i : -1;
}

static bool HoldsPrismSword(unsigned char* player)
{
    unsigned char* pev = player && Readable(player + 8, 4) ? *(unsigned char**)(player + 8) : nullptr;
    if (!pev || !Readable(pev + EV_VIEWMODEL, 4))
        return false;
    const char* vm = String(*(int*)(pev + EV_VIEWMODEL));
    return vm && strstr(vm, "v_magicknife.mdl") != nullptr;
}

static int PrismAnim(unsigned char* player, void* weapon, int anim)
{
    static const int map[10] = { 0, 1, MK_DRAW, 9, 10, 8, 10, 11, 12, 7 };
    if (anim < 0 || anim > 9)
        return anim;
    int i = PrismPlayerIndex(player);
    if (anim != 1)
    {
        if (i > 0)
            g_prism[i].weapon = nullptr; // no slash_end after this one
        return map[anim];
    }
    if (i <= 0)
        return 1;
    PrismSwing& s = g_prism[i];
    float now = Now();
    s.combo = (now - s.lastSlash < 2.6f && now >= s.lastSlash) ? s.combo % 4 + 1 : 1;
    s.lastSlash = now;
    s.weapon = weapon;
    s.anim = s.combo;
    s.endAt = now + 0.5f;
    return s.combo;
}

static void __fastcall HookedSendWeaponAnim(void* weapon, void* edx, int anim, int skiplocal)
{
    __try
    {
        unsigned char* player = *(unsigned char**)((unsigned char*)weapon + WPN_PLAYER_OFS);
        if (player && HoldsPrismSword(player))
            anim = PrismAnim(player, weapon, anim);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_sendWeaponAnim(weapon, edx, anim, skiplocal);
}

// every frame: each slash is followed by slash_end, as long as the sword is still in hand and nothing else was played
static void PrismSwordFrame()
{
    if (!g_sendWeaponAnim)
        return;
    float now = Now();
    for (int i = 1; i <= 32; i++)
    {
        PrismSwing& s = g_prism[i];
        if (!s.weapon)
            continue;
        if (now < s.endAt && now > s.endAt - 5.0f)
            continue;
        void* weapon = s.weapon;
        s.weapon = nullptr;
        __try
        {
            unsigned char* player = *(unsigned char**)((unsigned char*)weapon + WPN_PLAYER_OFS);
            unsigned char* pev = player ? *(unsigned char**)(player + 8) : nullptr;
            if (!pev || *(void**)(player + PL_ACTIVE_ITEM) != weapon || *(int*)(pev + EV_WEAPONANIM) != s.anim ||
                !HoldsPrismSword(player))
                continue;
            g_sendWeaponAnim(weapon, nullptr, MK_SLASH_END, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

static void FixPrismSwordAnims(DWORD delta)
{
    unsigned char* fn = (unsigned char*)(MP_SEND_WEAPON_ANIM + delta);
    const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x83, 0x7d, 0x0c, 0x00 }; // push ebp; mov ebp, esp; cmp [ebp+0xc], 0
    if (!Readable(fn, sizeof(code)) || fn[0] == 0xe9)
        return;
    if (memcmp(fn, code, sizeof(code)))
    {
        Log("Prism Sword animations: mp.dll isn't the expected one, left as they are");
        return;
    }
    g_sendWeaponAnim = (SendWeaponAnimFn)DetourCdecl(fn, sizeof(code), (void*)HookedSendWeaponAnim);
    Log("Prism Sword animations: the Frostblade's moves are played with the Prism Sword's own (4 slashes + recovery, charge)");
}
