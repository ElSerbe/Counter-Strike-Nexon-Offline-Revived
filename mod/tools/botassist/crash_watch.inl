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

// ---------------------------------------------------------------- the game's own crashes and hangs (2026-11-26)
// Players' reports said only "the game closed" or "it stopped responding": the game writes nothing when it dies, and
// the watch above is armed only after botassist's own gun shots. Now, for the whole session:
//  - a vectored handler notes every serious exception (access violation, stack overflow, bad instruction, division by
//    zero, stack buffer overrun) raised in the game's modules, with the module and offset (and the address in the
//    module's own numbering, as the disassembler shows it), the registers and the return addresses on the stack; the
//    first one also gets a minidump. The game may catch some of them: the last ones before a crash are the cause.
//  - an unhandled-exception filter (put back every few seconds: the game sets its own) notes the fatal one, with a dump.
//  - a hang watch: when the game's window doesn't answer for 6 s (Windows' "not responding"), where its thread is, three
//    times in a stall. A map load (LoadingStall) gets one "loading" line when it ends, unless it lasts 40 s.
// All of it goes to Bin\botassist_crash.log with plain Win32 calls (no CRT, no locks: the crash may have happened in
// them), the dumps to Bin\crash-<time>.dmp (the last 3 kept). The launcher adds both to problem reports.
typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
static MiniDumpWriteDumpFn g_miniDump;
static volatile LONG g_crashLogged = 0, g_crashDumps = 0;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;
static char g_crashLogPath[MAX_PATH];

static void CrashWrite(const char* text)
{
    HANDLE h = CreateFileA(g_crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD n;
    WriteFile(h, text, lstrlenA(text), &n, NULL);
    CloseHandle(h);
}

// "mp.dll+0x1234 (0x11001234)": the second number in the module's own image base, as Ghidra shows it
static int CrashWhere(DWORD addr, char* out, int size)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) ||
        !m)
        return wnsprintfA(out, size, "0x%08lX", addr);
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, sizeof(path));
    const char* base = strrchr(path, '\\');
    DWORD off = addr - (DWORD)m, image = (DWORD)m;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    if (Readable(dos, sizeof(*dos)) && dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((BYTE*)m + dos->e_lfanew);
        if (Readable(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE)
            image = nt->OptionalHeader.ImageBase;
    }
    return wnsprintfA(out, size, "%s+0x%lx (0x%08lX)", base ? base + 1 : path, off, image + off);
}

static bool GameModule(DWORD addr)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) ||
        !m)
        return true; // code outside any module (a jump into nowhere): worth noting
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, sizeof(path));
    CharLowerA(path);
    // Windows' own and the browser's (CEF raises and catches its own) are left out
    return !strstr(path, "\\windows\\") && !strstr(path, "libcef") && !strstr(path, "chrome_elf");
}

// the registers and up to 16 return addresses (stack words that point into a module's code)
static void CrashDescribe(const char* what, DWORD code, DWORD at, DWORD info, const CONTEXT* c, const DWORD* stackCopy = nullptr)
{
    static char buf[4096];
    SYSTEMTIME t;
    GetLocalTime(&t);
    char where[160];
    CrashWhere(at, where, sizeof(where));
    int n = stackCopy ? wnsprintfA(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d] %s %s\r\n", t.wYear, t.wMonth,
                                   t.wDay, t.wHour, t.wMinute, t.wSecond, what, where)
                      : wnsprintfA(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d] %s: 0x%08lX at %s, address 0x%08lX, thread %lu\r\n",
                                   t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, what, code, where, info,
                                   GetCurrentThreadId());
    n += wnsprintfA(buf + n, sizeof(buf) - n,
                    "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX\r\n  stack:", c->Eax,
                    c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // a stopped thread's stack is read from its copy (2048 words at most)
    const DWORD* sp = stackCopy ? stackCopy : (const DWORD*)c->Esp;
    for (int i = 0, found = 0; i < (stackCopy ? 2048 : 1024) && found < 16 && n < (int)sizeof(buf) - 200 && Readable(sp + i, 4); i++)
    {
        DWORD v = sp[i];
        MEMORY_BASIC_INFORMATION mbi;
        if (v < 0x10000 || !VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        n += wnsprintfA(buf + n, sizeof(buf) - n, "\r\n    ");
        n += CrashWhere(v, buf + n, sizeof(buf) - n);
        found++;
    }
    lstrcpynA(buf + n, "\r\n", sizeof(buf) - n);
    CrashWrite(buf);
}

static void CrashDump(EXCEPTION_POINTERS* ep, const char* why)
{
    if (!g_miniDump || InterlockedIncrement(&g_crashDumps) > 2)
        return;
    // the last 3 dumps are kept
    char pattern[MAX_PATH];
    wnsprintfA(pattern, sizeof(pattern), "%scrash-*.dmp", g_dir);
    WIN32_FIND_DATAA fd;
    char oldest[MAX_PATH] = "";
    int count = 0;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do
        {
            count++;
            if (!oldest[0] || lstrcmpA(fd.cFileName, oldest) < 0)
                lstrcpynA(oldest, fd.cFileName, sizeof(oldest));
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    if (count >= 3 && oldest[0])
    {
        char p[MAX_PATH];
        wnsprintfA(p, sizeof(p), "%s%s", g_dir, oldest);
        DeleteFileA(p);
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char path[MAX_PATH];
    wnsprintfA(path, sizeof(path), "%scrash-%04d%02d%02d-%02d%02d%02d-%s.dmp", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, why);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    struct
    {
        DWORD ThreadId;
        EXCEPTION_POINTERS* ExceptionPointers;
        BOOL ClientPointers;
    } info = { GetCurrentThreadId(), ep, FALSE };
    // MiniDumpWithIndirectlyReferencedMemory: the stacks and the memory they point to (a few MB; the modules' data
    // sections would make it tens of MB)
    g_miniDump(GetCurrentProcess(), GetCurrentProcessId(), h, 0x40, ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(h);
    char line[MAX_PATH + 32];
    wnsprintfA(line, sizeof(line), "  dump: %s\r\n", path);
    CrashWrite(line);
}

static bool SeriousException(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION || code == 0xC0000409 /* fast fail */ ||
           code == EXCEPTION_IN_PAGE_ERROR;
}

static LONG CALLBACK GameExceptionWatch(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD at = (DWORD)ep->ExceptionRecord->ExceptionAddress;
    if (!SeriousException(code) || !GameModule(at))
        return EXCEPTION_CONTINUE_SEARCH;
    // botassist's own are caught where they happen and noted in botassist.log; the same place is noted once
    static DWORD seen[64];
    static volatile LONG seenCount = 0;
    for (LONG i = 0; i < seenCount && i < 64; i++)
        if (seen[i] == at)
            return EXCEPTION_CONTINUE_SEARCH;
    LONG slot = InterlockedIncrement(&seenCount) - 1;
    if (slot >= 64)
        return EXCEPTION_CONTINUE_SEARCH;
    seen[slot] = at;
    DWORD info = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    CrashDescribe("exception (the game may still catch it)", code, at, info, ep->ContextRecord);
    if (slot == 0)
        CrashDump(ep, "first");
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI GameCrashFilter(EXCEPTION_POINTERS* ep)
{
    if (InterlockedIncrement(&g_crashLogged) == 1)
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        DWORD info = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
        CrashDescribe("THE GAME CRASHED", code, (DWORD)ep->ExceptionRecord->ExceptionAddress, info, ep->ContextRecord);
        g_crashDumps = 0; // the fatal one always gets its dump
        CrashDump(ep, "crash");
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// the game's main window (the biggest visible top-level window of this process) and the thread that runs it
static BOOL CALLBACK FindGameWindow(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    RECT r;
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetWindowRect(w, &r) &&
        r.right - r.left >= 320)
    {
        *(HWND*)out = w;
        return FALSE;
    }
    return TRUE;
}

// Every match start stalls the game's window 7-16 s while the map loads (19 "hangs" in two days' log, nearly all of
// them that): the engine's model loading reads the .nar archives (studio models and their sequence groups 0x23f64a0,
// the map 0x23f8eb0, both through the file system's vfunc 0x5c, 0x23ef570), uploads textures (0x23f2f70) and registers
// the match's resources (CL_RegisterResources 0x23c3560). A stall whose thread is in that code or in
// filesystem_nar.dll is a load: one line when it ends, no stack, no dump; one still loading at 40 s is a hang after
// all. The studio loader keeps a 176 KB buffer on its stack (0x23f64a3), so the copy (8 KB) never reaches the frames
// above it: the loaders' own return addresses are the sign, not the match start's. 0: not a load, 1: models and
// textures, 2: the game's files.
static const DWORD HW_MODEL_CODE_LO = 0x023f2f70, HW_MODEL_CODE_HI = 0x023fb700;    // texture upload .. Mod_PointInLeaf
static const DWORD HW_REGISTER_LO = 0x023c1d20, HW_REGISTER_HI = 0x023c3a00;        // CL_RegisterResources(OnHostCrash)
static const DWORD HW_STUDIO_LOAD = 0x023f64a0;                                     // the build check: its prologue

static bool InModule(DWORD v, DWORD lo, DWORD size)
{
    return lo && v - lo < size;
}

// a module's code: its start and size (0 when the headers can't be read)
static DWORD ModuleCode(HMODULE m, DWORD& size)
{
    size = 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    if (!m || !Readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((BYTE*)m + dos->e_lfanew);
    if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    size = nt->OptionalHeader.SizeOfCode;
    return (DWORD)m + nt->OptionalHeader.BaseOfCode;
}

static int LoadingStall(DWORD eip, const DWORD* stack, DWORD words)
{
    static DWORD hw = 0, fs = 0, fsSize = 0; // fs: filesystem_nar.dll's code (return addresses, not pointers to data)
    static int hwKnown = -1; // -1 not checked yet, 0 another build (its addresses aren't these), 1 this build
    if (hwKnown < 0)
    {
        hw = (DWORD)GetModuleHandleA("hw.dll");
        static const unsigned char studio[] = { 0x55, 0x8b, 0xec, 0xb8, 0x24, 0xc0, 0x02, 0x00 }; // mov eax,0x2c024
        unsigned char* p = (unsigned char*)(hw + HW_STUDIO_LOAD - HW_BASE);
        hwKnown = hw && Readable(p, sizeof(studio)) && !memcmp(p, studio, sizeof(studio)) ? 1 : 0;
        fs = ModuleCode(GetModuleHandleA("filesystem_nar.dll"), fsSize);
    }
    int kind = InModule(eip, fs, fsSize) ? 2 : 0;
    for (DWORD i = 0; i <= words; i++)
    {
        DWORD v = i < words ? stack[i] : eip;
        if (hwKnown == 1)
        {
            DWORD a = v - hw + HW_BASE; // in hw.dll's own numbering
            if ((a >= HW_MODEL_CODE_LO && a < HW_MODEL_CODE_HI) || (a >= HW_REGISTER_LO && a < HW_REGISTER_HI))
                return 1;
        }
        if (!kind && InModule(v, fs, fsSize))
            kind = 2;
    }
    return kind;
}

// called every half second by the Watcher thread
static void CrashWatchTick()
{
    // the game (or a library it loads) sets its own filter: ours goes back in front, the other one is called after it
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(GameCrashFilter);
    if (prev && prev != GameCrashFilter)
        g_prevFilter = prev;

    static HWND window = nullptr;
    static DWORD nextFind = 0, stallSince = 0;
    static int stallSamples = 0;
    static int stallLoad = 0;         // the stall's samples so far were all a load (LoadingStall's kind), 0 if not
    static bool stallReported = false; // a HANG was written for this stall
    DWORD now = GetTickCount();
    if ((!window || !IsWindow(window)) && now >= nextFind)
    {
        window = nullptr;
        EnumWindows(FindGameWindow, (LPARAM)&window);
        nextFind = now + 5000;
    }
    if (!window)
        return;
    DWORD_PTR result;
    if (SendMessageTimeoutA(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result))
    {
        if (stallSince && stallReported)
        {
            char line[128];
            wnsprintfA(line, sizeof(line), "  the game answered again after %lu s\r\n", (now - stallSince) / 1000);
            CrashWrite(line);
        }
        else if (stallSince && stallSamples && stallLoad > 0)
        {
            // a load: when and how long, nothing more
            SYSTEMTIME t;
            GetLocalTime(&t);
            char line[192];
            wnsprintfA(line, sizeof(line),
                       "[%04d-%02d-%02d %02d:%02d:%02d] loading: the game's window didn't answer for %lu s while it "
                       "loaded %s\r\n",
                       t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (now - stallSince) / 1000,
                       stallLoad == 1 ? "models and textures" : "the game's files");
            CrashWrite(line);
        }
        stallSince = 0;
        stallSamples = 0;
        stallLoad = 0;
        stallReported = false;
        return;
    }
    if (!stallSince)
    {
        stallSince = now;
        stallLoad = -1; // no sample yet
    }
    // samples at 6, 15 and 40 s: the same place each time is where it hangs
    static const DWORD at[] = { 6000, 15000, 40000 };
    if (stallSamples >= 3 || now - stallSince < at[stallSamples])
        return;
    stallSamples++;
    DWORD tid = GetWindowThreadProcessId(window, nullptr);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th)
        return;
    // the thread is stopped only while its registers and stack are copied: it may hold the heap's or the CRT's
    // locks, so nothing here allocates or logs until it runs again
    static DWORD stackCopy[2048];
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    DWORD words = 0;
    bool got = false;
    if (SuspendThread(th) != (DWORD)-1)
    {
        got = GetThreadContext(th, &c) != 0;
        if (got)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)c.Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
            {
                DWORD avail = ((DWORD)mbi.BaseAddress + mbi.RegionSize - c.Esp) / 4;
                words = avail < 2048 ? avail : 2048;
                memcpy(stackCopy, (void*)c.Esp, words * 4);
            }
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    if (!got)
        return;
    // a load stays quiet until the last sample (40 s): a load that long, or a stall that was something else, is a hang
    int load = LoadingStall(c.Eip, stackCopy, words);
    if (stallLoad != 0) // still a load while every sample is one; models and textures if any sample was
        stallLoad = !load ? 0 : stallLoad < 0 ? load : (stallLoad == 1 || load == 1) ? 1 : 2;
    if (stallLoad > 0 && stallSamples < 3)
        return;
    char what[128];
    wnsprintfA(what, sizeof(what), "HANG: the game's window hasn't answered for %lu s%s; its thread is at",
               (now - stallSince) / 1000, stallLoad > 0 ? " (still loading)" : "");
    if (words)
        CrashDescribe(what, 0, c.Eip, 0, &c, stackCopy); // the copied stack: same values, read after the thread runs again
    stallReported = true;
    // one dump per stall: at the 15 s sample, or at the first one written after it
    static DWORD dumpedFor = 0;
    if (stallSamples >= 2 && dumpedFor != stallSince)
    {
        dumpedFor = stallSince;
        CrashDump(nullptr, "hang");
    }
}

static void InstallGameCrashWatch()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    wnsprintfA(g_crashLogPath, sizeof(g_crashLogPath), "%sbotassist_crash.log", g_dir);
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    g_miniDump = dbg ? (MiniDumpWriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    AddVectoredExceptionHandler(0, GameExceptionWatch); // after the game's own vectored handlers
    g_prevFilter = SetUnhandledExceptionFilter(GameCrashFilter);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[160];
    wnsprintfA(line, sizeof(line), "\r\n[%04d-%02d-%02d %02d:%02d:%02d] ===== game started, crash watch on (botassist %s)\r\n",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, __DATE__);
    CrashWrite(line);
    Log("crash watch: the game's crashes and hangs go to botassist_crash.log%s", g_miniDump ? " (with dumps)" : "");
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
