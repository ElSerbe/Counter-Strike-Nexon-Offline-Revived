// ---------------------------------------------------------------- weapons this client has no model for
// A few weapons of Item.csv name models the game files don't have (murasame5, sfboomerang1, y24s2mg3...): drawn, they
// were invisible (a player hit someone with an empty hand). (The weapon skins - Trans-Gear, Halloween, the "b"
// editions - have no model of their own either, but hw.dll's skin table shows them on the base model: not here.) The
// game opens every file through FileSystem_Nar.dll's IFileSystem ("VFileSystem009": vtable slot 8 FileExists(name),
// slot 10 Open(name, options, pathID), thiscall). botassist hooks both: a models/v_, p_ or w_<res>.mdl that isn't
// there, for a <res> of botassist_model_aliases.txt (gen_model_aliases.py), is opened as the alias's model instead
// (the plain Halloween Hammer's hammer). Only when the real file is missing: nothing changes for the others.
typedef void* (__fastcall* FsOpenFn)(void* self, void* edx, const char* name, const char* options, const char* pathID);
typedef bool(__fastcall* FsExistsFn)(void* self, void* edx, const char* name);
static FsOpenFn g_fsOpen = nullptr;
static FsExistsFn g_fsExists = nullptr;
static std::unordered_map<std::string, std::string> g_modelAliases; // "y19s4hammer" -> "hammer"

// the alias path of a missing weapon model ("" when it isn't one)
static std::string ModelAlias(const char* name)
{
    if (!name || g_modelAliases.empty())
        return "";
    std::string n = name;
    for (auto& ch : n)
        ch = ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
    size_t at = n.find("models/");
    if (at == std::string::npos || n.size() < at + 13 || n.compare(n.size() - 4, 4, ".mdl"))
        return "";
    size_t p = at + 7;
    char kind = n[p];
    if ((kind != 'v' && kind != 'p' && kind != 'w') || n[p + 1] != '_')
        return "";
    std::string res = n.substr(p + 2, n.size() - 4 - (p + 2));
    auto it = g_modelAliases.find(res);
    if (it == g_modelAliases.end())
        return "";
    return n.substr(0, p + 2) + it->second + ".mdl";
}

static void* __fastcall HookedFsOpen(void* self, void* edx, const char* name, const char* options, const char* pathID)
{
    void* h = g_fsOpen(self, edx, name, options, pathID);
    if (h || !options || options[0] != 'r')
        return h;
    std::string alias = ModelAlias(name);
    if (alias.empty())
        return h;
    h = g_fsOpen(self, edx, alias.c_str(), options, pathID);
    static int reports = 0;
    if (reports++ < 60)
        Log("model aliases: %s isn't in the game files: %s shown instead%s", name, alias.c_str(), h ? "" : " (not there either)");
    return h;
}

static bool __fastcall HookedFsExists(void* self, void* edx, const char* name)
{
    if (g_fsExists(self, edx, name))
        return true;
    std::string alias = ModelAlias(name);
    return !alias.empty() && g_fsExists(self, edx, alias.c_str());
}

static void InstallModelAliases()
{
    static bool done = false;
    if (done)
        return;
    HMODULE fs = GetModuleHandleA("FileSystem_Nar.dll");
    if (!fs)
        return;
    done = true;
    if (FILE* f = fopen((std::string(g_dir) + "botassist_model_aliases.txt").c_str(), "r"))
    {
        char line[256], a[128], b[128];
        while (fgets(line, sizeof(line), f))
            if (line[0] != ';' && sscanf(line, "%127s %127s", a, b) == 2)
                g_modelAliases[a] = b;
        fclose(f);
    }
    typedef void*(__cdecl * CreateInterfaceFn)(const char*, int*);
    CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(fs, "CreateInterface");
    void* iface = ci ? ci("VFileSystem009", nullptr) : nullptr;
    void** vt = iface && Readable(iface, 4) ? *(void***)iface : nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((unsigned char*)fs + ((IMAGE_DOS_HEADER*)fs)->e_lfanew);
    DWORD lo = (DWORD)fs, hi = lo + nt->OptionalHeader.SizeOfImage;
    if (!vt || !Readable(vt + 10, 4) || g_modelAliases.empty() || (DWORD)vt[10] < lo || (DWORD)vt[10] >= hi ||
        (DWORD)vt[8] < lo || (DWORD)vt[8] >= hi)
    {
        Log("model aliases: %d weapons, file system %s: not hooked", (int)g_modelAliases.size(), iface ? "unexpected" : "not found");
        return;
    }
    if (vt[10] == (void*)HookedFsOpen)
        return;
    g_fsOpen = (FsOpenFn)vt[10];
    g_fsExists = (FsExistsFn)vt[8];
    DWORD old;
    VirtualProtect(vt + 8, 3 * sizeof(void*), PAGE_READWRITE, &old);
    vt[8] = (void*)HookedFsExists;
    vt[10] = (void*)HookedFsOpen;
    VirtualProtect(vt + 8, 3 * sizeof(void*), old, &old);
    Log("model aliases: %d weapons without models of their own show their base weapon's", (int)g_modelAliases.size());
}
