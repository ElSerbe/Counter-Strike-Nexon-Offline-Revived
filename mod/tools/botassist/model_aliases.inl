// ---------------------------------------------------------------- weapons this client has no model for
// A few weapons of Item.csv name models the game files don't have (murasame5, sfboomerang1, y24s2mg3...): drawn, they
// were invisible (a player hit someone with an empty hand). (The weapon skins - Trans-Gear, Halloween, the "b"
// editions - have no model of their own either, but hw.dll's skin table shows them on the base model: not here.) The
// game opens every file through FileSystem_Nar.dll's IFileSystem ("VFileSystem009"). Nexon's has two more functions
// before Open than Valve's: Open(name, options, pathID) is vtable slot 12 (+0x30, thiscall, ret 0xc; nartool uses it
// too). botassist hooks only that one: a models/v_, p_ or w_<res>.mdl that isn't there, for a <res> of
// botassist_model_aliases.txt (gen_model_aliases.py), is opened as the alias's model instead. Only when the real file
// is missing: nothing changes for the others. (The first build hooked slots 8 and 10, Valve's FileExists and Open -
// other functions here: slot 8's result was cut to a bool and the game crashed on reaching the lobby.)
static const int FS_SLOT_OPEN = 12;
typedef void* (__fastcall* FsOpenFn)(void* self, void* edx, const char* name, const char* options, const char* pathID);
static FsOpenFn g_fsOpen = nullptr;
static std::unordered_map<std::string, std::string> g_modelAliases; // "y19s4hammer" -> "hammer"
// Files GameUI reads itself (its own CSV reader, not the hw.dll loaders of the table overrides): a file of
// botassist_tablesesource\clan\ next to botassist.asi is opened instead of the game's, by its full path (the Family
// Achievements' offline targets: tables/resource/clan/ClanAchieve.csv, gen_family_achievements.py)
static std::unordered_map<std::string, std::string> g_fileOverrides; // "resource/clan/clanachieve.csv" -> full path

static void LoadFileOverrides()
{
    const char* dirs[] = { "resource\\clan" };
    for (const char* d : dirs)
    {
        WIN32_FIND_DATAA fd;
        std::string base = std::string(g_dir) + "botassist_tables\\" + d + "\\";
        HANDLE h = FindFirstFileA((base + "*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::string rel = std::string(d) + "/" + fd.cFileName;
            for (auto& ch : rel)
                ch = ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
            g_fileOverrides[rel] = base + fd.cFileName;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

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
    if (name && !g_fileOverrides.empty() && options && options[0] == 'r')
    {
        std::string n = name;
        for (auto& ch : n)
            ch = ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
        auto it = g_fileOverrides.find(n);
        if (it != g_fileOverrides.end())
        {
            void* o = g_fsOpen(self, edx, it->second.c_str(), options, pathID);
            static int reports = 0;
            if (reports++ < 20)
                Log("file %s: %s", name, o ? "opened from botassist_tables" : "botassist_tables' copy couldn't be opened, the game's is used");
            if (o)
                return o;
        }
    }
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

static void InstallModelAliases()
{
    static bool done = false;
    if (done)
        return;
    HMODULE fs = GetModuleHandleA("FileSystem_Nar.dll");
    if (!fs)
        return;
    done = true;
    LoadFileOverrides();
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
    if (!vt || !Readable(vt + FS_SLOT_OPEN, 4) || (g_modelAliases.empty() && g_fileOverrides.empty()) || (DWORD)vt[FS_SLOT_OPEN] < lo ||
        (DWORD)vt[FS_SLOT_OPEN] >= hi)
    {
        Log("model aliases: %d weapons, file system %s: not hooked", (int)g_modelAliases.size(), iface ? "unexpected" : "not found");
        return;
    }
    if (vt[FS_SLOT_OPEN] == (void*)HookedFsOpen)
        return;
    g_fsOpen = (FsOpenFn)vt[FS_SLOT_OPEN];
    DWORD old;
    VirtualProtect(vt + FS_SLOT_OPEN, sizeof(void*), PAGE_READWRITE, &old);
    vt[FS_SLOT_OPEN] = (void*)HookedFsOpen;
    VirtualProtect(vt + FS_SLOT_OPEN, sizeof(void*), old, &old);
    Log("model aliases: %d weapons without models of their own show their base weapon's; %d files of botassist_tables opened instead of the game's",
        (int)g_modelAliases.size(), (int)g_fileOverrides.size());
}
