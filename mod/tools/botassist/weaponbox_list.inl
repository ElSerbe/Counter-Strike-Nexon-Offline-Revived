// ---------------------------------------------------------------- the game's list of weapons on the ground, per map
// mp.dll keeps every CWeaponBox in a global std::list (0x12207240: its sentinel node, then its size). CWeaponBox::Spawn
// (0x1152f020) adds the new box and, past 100 boxes, kills and removes the oldest (0x11523ac0 + UTIL_Remove); a box
// leaves the list in its UpdateOnRemove (vtable +0x100, 0x11534230). A map change frees every entity without that, and
// nothing empties the list: CSO's rooms run on dedicated servers that start afresh for each match, but here the game
// is its own server and keeps going. On the next map, once 100 boxes were reached, the "oldest" was a box of the
// previous map, freed long ago, and killing it closed the game (mp.dll+0x1523ae6 / +0x1523b7e, called from
// +0x152f10a): issues #35 (Zombie Hero, Random Weapon) and #37 (Zombie Classic, shared loadouts), where rounds hand
// out many weapons at once. So the list is emptied at the start of every map (its old nodes are left, a few bytes).
static const DWORD MP_WEAPONBOX_LIST = 0x12207240;

static void WeaponBoxListFrame()
{
    static int level = -1;
    int lv = CheckClock();
    if (lv == level || !g_mpDelta)
        return;
    level = lv;
    size_t had = 0;
    bool ok = false;
    __try
    {
        DWORD* list = (DWORD*)(MP_WEAPONBOX_LIST + g_mpDelta);
        DWORD* head = (DWORD*)list[0];
        if (head)
        {
            had = list[1];
            head[0] = (DWORD)head; // next
            head[1] = (DWORD)head; // prev
            list[1] = 0;
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (had)
        Log("weapon boxes: the game's list emptied for the new map (%u left from the last one)%s", (unsigned)had,
            ok ? "" : " - FAILED");
}
