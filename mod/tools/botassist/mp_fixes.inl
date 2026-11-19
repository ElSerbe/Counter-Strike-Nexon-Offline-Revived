// ---------------------------------------------------------------- mp.dll fixes (every mode)
// Divine Titan EX: drawing it (CDivineTitan's function at 0x10c6a970, weapon id 0x23e) creates its drone entity by
// name, "divinetitanex_drone", and keeps it at +0x23c; this mp.dll exports no such entity (only "divinetitan_drone",
// the plain Divine Titan's), so the creation gave nothing and the game crashed writing to it (0x10c6aa1b, a Zombie
// Hero round start with 31 bots, crash dump 2026-11-19). Both places that name it (0x10c6a9d8 mov edx, 0x10c75949
// push) get the plain one's drone instead.
static const DWORD MP_DTEX_SITES[] = { 0x10c6a9d8, 0x10c75949 };
static const DWORD MP_DTEX_DRONE = 0x11840810, MP_DT_DRONE = 0x11840824;

static void FixDivineTitanEx(DWORD delta)
{
    int fixed = 0;
    for (DWORD siteVa : MP_DTEX_SITES)
    {
        unsigned char* site = (unsigned char*)(siteVa + delta);
        if (!Readable(site, 5) || (site[0] != 0xba && site[0] != 0x68))
            continue;
        DWORD* imm = (DWORD*)(site + 1);
        if (*imm == MP_DT_DRONE + delta)
        {
            fixed++;
            continue;
        }
        if (*imm != MP_DTEX_DRONE + delta)
            continue;
        DWORD old;
        VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old);
        *imm = MP_DT_DRONE + delta;
        VirtualProtect(imm, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        fixed++;
    }
    Log("Divine Titan EX: %d of 2 places use divinetitan_drone (divinetitanex_drone doesn't exist: drawing it crashed)",
        fixed);
}
