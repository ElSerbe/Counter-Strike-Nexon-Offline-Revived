// ---------------------------------------------------------------- the game's own shop, from anywhere
// The taskbar's Shop button opens the web shop (apply_patches.py), but other places still asked for the game's own
// shop page: the Duffle Bag notice ("Click OK to move to the Store Page"), item tooltips' Buy... They all go through the
// lobby's page switcher, GameUI 0x10ada090 (the object 0x104528c0 returns), vfunc 0x64 (page): page 13 is the shop.
// That shop sells the old Shop.json products for 1-3 medals, outside the battle passes. botassist hooks the vfunc:
// page 13 opens the web shop instead (CCSOMainPanel::OpenWebEvent, like the Shop button). Shift: the game's shop.
static const DWORD GAMEUI_PAGES = 0x10ada090;   // the page switcher object (static)
static const int PAGES_SLOT_SHOW = 0x64 / 4, PAGE_SHOP = 13;
typedef int(__fastcall* ShowPageFn)(void* self, void* edx, int page);
static ShowPageFn g_showPage;

static int __fastcall HookedShowPage(void* self, void* edx, int page)
{
    if (page == PAGE_SHOP && !(GetKeyState(VK_SHIFT) & 0x8000))
    {
        void* panel = *(void**)(GAMEUI_MAINPANEL + g_gameUiDelta);
        if (panel)
        {
            HttpPost(L"/api/openview", "{\"view\":\"shop\"}");
            Log("the game's shop page was asked for: the web shop opens instead (Shift: the game's)");
            ((OpenWebEventFn)(GAMEUI_OPENWEBEVENT + g_gameUiDelta))(panel, nullptr);
            return 0;
        }
    }
    return g_showPage(self, edx, page);
}

static void HookShopPage()
{
    void** obj = (void**)(GAMEUI_PAGES + g_gameUiDelta);
    void** vt = Readable(obj, 4) ? (void**)*obj : nullptr;
    if (!vt || !Readable(vt + PAGES_SLOT_SHOW, 4))
        return;
    if (vt[PAGES_SLOT_SHOW] == (void*)HookedShowPage)
        return;
    DWORD fn = (DWORD)vt[PAGES_SLOT_SHOW];
    if (fn < g_gameUiDelta + 0x10001000 || fn >= g_gameUiDelta + 0x10a00000)
    {
        Log("GameUI page switcher has an unexpected vtable: the game's shop page left as it is");
        return;
    }
    g_showPage = (ShowPageFn)fn;
    DWORD old;
    VirtualProtect(vt + PAGES_SLOT_SHOW, 4, PAGE_READWRITE, &old);
    vt[PAGES_SLOT_SHOW] = (void*)HookedShowPage;
    VirtualProtect(vt + PAGES_SLOT_SHOW, 4, old, &old);
    Log("the game's shop page opens the web shop (Shift: the game's)");
}
