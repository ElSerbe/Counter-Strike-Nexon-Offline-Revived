// ---------------------------------------------------------------- what closes a match (diagnostics)
// 2026-11-20: the Gravity Repulsor's first shot sent the player back to the room ("the connection to the server was
// closed") twice; every step of botassist's shot finished and nothing was logged. Two watchers, for the log:
//  - hw.dll's Host_Error (0x2419150, cdecl fmt, ...; "Host_Error: %s"): its message goes to botassist.log before the
//    engine drops the match.
//  - a vectored exception handler, armed for a few seconds after a botassist gun shot (CrashWatchArm): every
//    exception then (first chance, even the ones the game catches) is logged with its module and offset, and the
//    return addresses on the stack that fall in game modules.
static const DWORD HW_HOST_ERROR = 0x02419150;
typedef void (*HostErrorFn)(const char* fmt, ...);
static HostErrorFn g_hostError;
static volatile DWORD g_crashWatchUntil = 0;
static volatile LONG g_crashWatchLogs = 0;

static void __cdecl HookedHostError(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "(null)", ap);
    va_end(ap);
    Log("Host_Error: %s", msg);
    g_hostError("%s", msg);
}

static bool ModuleOf(DWORD addr, char* name, size_t size, DWORD& offset)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) ||
        !m)
        return false;
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, sizeof(path));
    const char* base = strrchr(path, '\\');
    snprintf(name, size, "%s", base ? base + 1 : path);
    offset = addr - (DWORD)m;
    return true;
}

static LONG CALLBACK CrashWatchHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (GetTickCount() > g_crashWatchUntil || code == 0x406D1388 /* thread name */ || code == 0x40010006 ||
        code == 0x4001000A /* debug strings */ || InterlockedIncrement(&g_crashWatchLogs) > 12)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD at = (DWORD)ep->ExceptionRecord->ExceptionAddress, off = 0;
    char mod[64] = "?";
    ModuleOf(at, mod, sizeof(mod), off);
    DWORD info = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    char stack[512] = "";
    size_t used = 0;
    DWORD* sp = (DWORD*)ep->ContextRecord->Esp;
    for (int i = 0, found = 0; i < 256 && found < 8 && Readable(sp + i, 4); i++)
    {
        char m2[64];
        DWORD o2;
        DWORD v = sp[i];
        if (v > 0x10000 && ModuleOf(v, m2, sizeof(m2), o2) && (!_stricmp(m2, "mp.dll") || !_stricmp(m2, "hw.dll") ||
                                                              !_stricmp(m2, "client.dll") || !_stricmp(m2, "botassist.asi")))
        {
            used += snprintf(stack + used, sizeof(stack) - used, " %s+0x%lx", m2, o2);
            found++;
            if (used >= sizeof(stack) - 40)
                break;
        }
    }
    Log("exception watch: 0x%08lX at %s+0x%lx (address 0x%08lX); stack:%s", code, mod, off, info, stack);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void CrashWatchArm()
{
    g_crashWatchUntil = GetTickCount() + 4000;
}

static void InstallCrashWatch()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    AddVectoredExceptionHandler(1, CrashWatchHandler);
    if (!g_hwLo)
        ModuleRange("hw.dll", g_hwLo, g_hwHi);
    unsigned char* fn = g_hwLo ? (unsigned char*)(g_hwLo + HW_HOST_ERROR - HW_BASE) : nullptr;
    const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x04, 0x04, 0x00, 0x00 }; // push ebp; ...; sub esp, 0x404
    if (fn && Readable(fn, sizeof(code)) && !memcmp(fn, code, sizeof(code)))
    {
        g_hostError = (HostErrorFn)DetourCdecl(fn, sizeof(code), (void*)HookedHostError);
        Log("crash watch: Host_Error messages and the exceptions after botassist's gun shots go to this log");
    }
    else
        Log("crash watch: hw.dll isn't the expected one, Host_Error not watched");
}
