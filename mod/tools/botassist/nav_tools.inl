// botassist: console tools for the bots' way around a map. Included by botassist.cpp (one translation unit). See the README's "Navigation tools".
//
// The game still has Counter-Strike: Condition Zero's mesh editor and generator, nobody just reached them:
//   - bot_nav_edit 1 and the bot_nav_* commands work as they are (mp.dll CCSBotManager::ServerCommand 0x11318420, edit loop 0x11322fb0).
//   - The generator ("learning the map") runs in CCSBot::Update 0x1130f4e0: process mode +0x5a50 1 learn, 2 and 3 analyse, 4 save and restart the map.
//     CCSBot::Spawn 0x113041d0 only starts it when a map has no mesh, and every map ships with one: nav_generate restarts the map telling the
//     game there's no file, and starts it on the first bot.
//   - SaveNavigationMap 0x1132c4c0 writes to <game dir>\maps, which doesn't exist here (the game's files are in .nar archives), and
//     LoadNavigationMap only reads through the engine's LoadFileForMe.
// So a mesh of ours lives in Bin\botassist_nav\<map>.nav: HookedSaveNav writes there and HookedLoadFile, in mp.dll's table of engine functions, serves it to
// the game's bots and to botassist alike. The game's files stay as they are, and nav_restore goes back to them.
//
// Points of interest are botassist's own, Bin\botassist_nav\<map>.poi, one per line: "spot x y z name" or "avoid x y z radius name" (z on the floor).
//   spot: where the bots like to be (PickSpot: standing around the leader and leaving a hazard; the routes of Gun Deathmatch and the modes built on it).
//   avoid: a place the paths go around when they can (FindPath, shortcuts, PickSpot).
static const DWORD MP_SAVE_NAV = 0x1132c4c0, MP_START_LEARN = 0x11305230, MP_CCSBOT_SPAWN = 0x113041d0, MP_NAV_ANALYZE_FLAG = 0x1218293e, MP_NAV_AREA_COUNT = 0x121836d4;
static const int BOT_PROCESS_MODE = 0x5a50;
static const float POI_AVOID_RADIUS = 128.0f, POI_AVOID_COST = 3000.0f, POI_SAME_FLOOR = 120.0f, POI_FIND_RADIUS = 256.0f, POI_SHOW_RADIUS = 2500.0f;

static std::string CurrentMap()
{
    const char* m = g_pGlobals && *g_pGlobals ? String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME)) : nullptr;
    return m ? m : "";
}
static std::string NavToolsDir() { return std::string(g_dir) + "botassist_nav\\"; }
static std::string NavFileOf(const std::string& map) { return NavToolsDir() + map + ".nav"; }
static std::string PoiFileOf(const std::string& map) { return NavToolsDir() + map + ".poi"; }
static bool FileThere(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------- generating a mesh
enum { GEN_OFF, GEN_RESTARTING, GEN_WAITING, GEN_LEARNING };
static struct
{
    int state = GEN_OFF;
    std::string map;
    int level = 0;
    DWORD since = 0;
    bool botAsked = false;
    int tries = 0;
    DWORD nextTry = 0;
} g_gen;

static void RestartMap(const std::string& map)
{
    std::string cmd = "map " + map + "\n";
    g_ServerCommand(cmd.c_str());
}

static void GenStop(const char* why)
{
    g_gen.state = GEN_OFF;
    g_nav.map.clear(); // botassist reads its mesh again
    Con("nav_generate: %s\n", why);
    Log("nav_generate: %s", why);
}

// ---------------------------------------------------------------- the files
static LoadFileFn g_LoadFileGame;
static FreeFileFn g_FreeFileGame;
static std::set<void*> g_navBuffers;

// A mesh shipped as text (<map>.navtxt, tools/botassist/gen_nav.py: the maps whose own .nav is wrong) turned into the game's
// version 5 file: "# comment", "version 5 bspsize N areas N", then one area per line: id attr lo.x lo.y lo.z hi.x hi.y hi.z
// neZ swZ and the four lists of linked area ids (north east south west), comma separated or "-".
static bool NavFromText(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    out.clear();
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const unsigned char*)p, (const unsigned char*)p + n); };
    auto u32 = [&](unsigned v) { put(&v, 4); };
    char line[8192];
    unsigned bspSize = 0, count = 0, written = 0;
    bool header = false;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (!header)
        {
            if (sscanf(line, "version 5 bspsize %u areas %u", &bspSize, &count) != 2)
                break;
            header = true;
            u32(0xFEEDFACE);
            u32(5);
            u32(bspSize);
            unsigned short places = 0;
            put(&places, 2);
            u32(count);
            continue;
        }
        unsigned id, attr;
        float v[10];
        char lists[4][4096];
        if (sscanf(line, "%u %u %f %f %f %f %f %f %f %f %4095s %4095s %4095s %4095s", &id, &attr, &v[0], &v[1], &v[2], &v[3], &v[4],
                   &v[5], &v[6], &v[7], lists[0], lists[1], lists[2], lists[3]) != 14)
            break;
        u32(id);
        unsigned char a = (unsigned char)attr;
        put(&a, 1);
        put(v, 8 * 4);
        for (auto& l : lists)
        {
            std::vector<unsigned> ids;
            if (strcmp(l, "-"))
                for (char* tok = strtok(l, ","); tok; tok = strtok(nullptr, ","))
                    ids.push_back((unsigned)strtoul(tok, nullptr, 10));
            u32((unsigned)ids.size());
            for (unsigned x : ids)
                u32(x);
        }
        unsigned char zero = 0;
        put(&zero, 1); // hiding spots
        put(&zero, 1); // approach areas
        u32(0);        // encounter paths
        unsigned short place = 0;
        put(&place, 2);
        written++;
    }
    fclose(f);
    return header && written == count && count > 0;
}

static unsigned char* HookedLoadFile(const char* name, int* length)
{
    const char* dot = name ? strrchr(name, '.') : nullptr;
    if (dot && !_stricmp(dot, ".nav"))
    {
        const char* a = strrchr(name, '\\'), *b = strrchr(name, '/');
        const char* base = a && b ? max(a, b) + 1 : a ? a + 1 : b ? b + 1 : name;
        std::string map(base, dot - base);
        if (g_gen.state != GEN_OFF && !_stricmp(map.c_str(), g_gen.map.c_str()))
        {
            if (length)
                *length = 0;
            return nullptr; // generating: as if the map had no mesh
        }
        FILE* f = fopen(NavFileOf(map).c_str(), "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char* data = size > 0 ? (unsigned char*)malloc(size + 1) : nullptr;
            bool ok = data && fread(data, 1, size, f) == (size_t)size;
            fclose(f);
            if (ok)
            {
                data[size] = 0;
                g_navBuffers.insert(data);
                if (length)
                    *length = (int)size;
                Log("%s: read from botassist_nav (%ld bytes)", name, size);
                return data;
            }
            free(data);
            Log("%s: couldn't read botassist_nav's copy, using the game's", name);
        }
        std::vector<unsigned char> mesh;
        std::string text = NavToolsDir() + map + ".navtxt";
        if (FileThere(text))
        {
            if (NavFromText(text, mesh))
            {
                unsigned char* data = (unsigned char*)malloc(mesh.size() + 1);
                if (data)
                {
                    memcpy(data, mesh.data(), mesh.size());
                    data[mesh.size()] = 0;
                    g_navBuffers.insert(data);
                    if (length)
                        *length = (int)mesh.size();
                    Log("%s: built from botassist_nav\\%s.navtxt (%u bytes)", name, map.c_str(), (unsigned)mesh.size());
                    return data;
                }
            }
            else
                Log("%s: botassist_nav\\%s.navtxt couldn't be read, using the game's", name, map.c_str());
        }
    }
    return g_LoadFileGame(name, length);
}

static void HookedFreeFile(void* buffer)
{
    if (g_navBuffers.erase(buffer))
        free(buffer);
    else
        g_FreeFileGame(buffer);
}

// botassist's own mesh goes through these too
static void NavHookFiles(void** table)
{
    if (table[ENG_LOADFILE] != (void*)HookedLoadFile)
    {
        g_LoadFileGame = (LoadFileFn)table[ENG_LOADFILE];
        g_FreeFileGame = (FreeFileFn)table[ENG_FREEFILE];
        table[ENG_LOADFILE] = (void*)HookedLoadFile;
        table[ENG_FREEFILE] = (void*)HookedFreeFile;
    }
    g_LoadFile = HookedLoadFile;
    g_FreeFile = HookedFreeFile;
}

typedef int (*SaveNavFn)(char* path); // (a bool in al)
static SaveNavFn g_SaveNavTramp;

static int HookedSaveNav(char* /* <game dir>\maps\<map>.nav */)
{
    std::string map = CurrentMap(), target = NavFileOf(map);
    if (*(int*)(MP_NAV_AREA_COUNT + g_mpDelta) <= 0)
    {
        // (bot_nav_save typed while a new mesh is being learned, or a map it couldn't learn)
        Con("No mesh to save: %s stays as it was.\n", map.c_str());
        Log("navigation mesh of %s: nothing to save", map.c_str());
        return 0;
    }
    CreateDirectoryA(NavToolsDir().c_str(), nullptr);
    if (FileThere(target))
        CopyFileA(target.c_str(), (target + ".bak").c_str(), FALSE);
    char path[MAX_PATH];
    strncpy(path, target.c_str(), sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    bool ok = (g_SaveNavTramp(path) & 0xff) != 0;
    Log("navigation mesh of %s %s %s", map.c_str(), ok ? "saved to" : "NOT saved to", path);
    Con("Navigation mesh %s %s\n", ok ? "saved to" : "NOT saved to", path);
    if (ok)
    {
        if (g_gen.state != GEN_OFF)
            g_gen.state = GEN_OFF;
        g_nav.map.clear(); // the companions walk the new one from now on
    }
    return ok;
}

static void NavInstallSaveHook()
{
    if (g_SaveNavTramp)
        return;
    unsigned char* fn = (unsigned char*)(MP_SAVE_NAV + g_mpDelta);
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x53 };
    if (Readable(fn, sizeof(code)) && !memcmp(fn, code, sizeof(code)))
        g_SaveNavTramp = (SaveNavFn)DetourCdecl(fn, sizeof(code), (void*)HookedSaveNav);
    Log("navigation mesh saves: %s", g_SaveNavTramp ? "to botassist_nav" : "NOT hooked (unexpected code)");
}

// ---------------------------------------------------------------- points of interest
struct Poi
{
    bool avoid;
    V3 pos; // on the floor
    float radius;
    std::string name;
};
static std::vector<Poi> g_pois;
static std::vector<V3> g_poiSpots;
static std::vector<float> g_poiAreaCost;
static bool g_poiShow = false;

static void PoiRebuild()
{
    g_poiSpots.clear();
    for (auto& p : g_pois)
        if (!p.avoid)
            g_poiSpots.push_back(p.pos);
    g_poiAreaCost.assign(g_nav.areas.size(), 0.0f);
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        for (auto& p : g_pois)
        {
            if (!p.avoid || fabsf(a.center.z - p.pos.z) > POI_SAME_FLOOR)
                continue;
            float dx = p.pos.x < a.lo[0] ? a.lo[0] - p.pos.x : p.pos.x > a.hi[0] ? p.pos.x - a.hi[0] : 0.0f;
            float dy = p.pos.y < a.lo[1] ? a.lo[1] - p.pos.y : p.pos.y > a.hi[1] ? p.pos.y - a.hi[1] : 0.0f;
            if (dx * dx + dy * dy < p.radius * p.radius)
                g_poiAreaCost[i] = POI_AVOID_COST;
        }
    }
}

static void PoiLoad(const std::string& map)
{
    g_pois.clear();
    FILE* f = fopen(PoiFileOf(map).c_str(), "r");
    if (f)
    {
        char line[256], kind[16], name[64];
        while (fgets(line, sizeof(line), f))
        {
            Poi p{};
            name[0] = 0;
            if (sscanf(line, "%15s", kind) != 1)
                continue;
            p.avoid = !strcmp(kind, "avoid");
            if (p.avoid ? sscanf(line, "%*s %f %f %f %f %63s", &p.pos.x, &p.pos.y, &p.pos.z, &p.radius, name) < 4
                        : strcmp(kind, "spot") || sscanf(line, "%*s %f %f %f %63s", &p.pos.x, &p.pos.y, &p.pos.z, name) < 3)
                continue;
            p.name = name;
            g_pois.push_back(p);
        }
        fclose(f);
        Log("%s: %d points of interest", PoiFileOf(map).c_str(), (int)g_pois.size());
    }
    PoiRebuild();
}

static bool PoiSave()
{
    std::string path = PoiFileOf(CurrentMap());
    CreateDirectoryA(NavToolsDir().c_str(), nullptr);
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
    {
        Con("Can't write %s\n", path.c_str());
        return false;
    }
    for (auto& p : g_pois)
        if (p.avoid)
            fprintf(f, "avoid %.1f %.1f %.1f %.1f %s\n", p.pos.x, p.pos.y, p.pos.z, p.radius, p.name.c_str());
        else
            fprintf(f, "spot %.1f %.1f %.1f %s\n", p.pos.x, p.pos.y, p.pos.z, p.name.c_str());
    fclose(f);
    return true;
}

static float PoiAreaCost(int area)
{
    return area >= 0 && area < (int)g_poiAreaCost.size() ? g_poiAreaCost[area] : 0.0f;
}
static bool PoiAvoidAt(V3 at)
{
    for (auto& p : g_pois)
        if (p.avoid && Len2D(at - p.pos) < p.radius && fabsf(at.z - p.pos.z) < POI_SAME_FLOOR)
            return true;
    return false;
}
static int PoiSpotCount() { return (int)g_poiSpots.size(); }
static V3 PoiSpotAt(int i) { return g_poiSpots[i]; }

// ---------------------------------------------------------------- console
static void* NavToolsPlayer()
{
    void* e = YourEdict();
    if (!e)
        Con("Only in a match you host.\n");
    return e;
}

static void CmdPoiAdd()
{
    void* e = NavToolsPlayer();
    if (!e)
        return;
    std::string kind = g_CmdArgv(1) ? g_CmdArgv(1) : "";
    if (kind != "spot" && kind != "avoid")
    {
        Con("Usage: poi_add spot [name] | poi_add avoid [radius] [name]\n");
        return;
    }
    Poi p{};
    p.avoid = kind == "avoid";
    p.pos = Origin(e) - V3{ 0, 0, 36 };
    p.radius = POI_AVOID_RADIUS;
    const char* a = g_CmdArgv(2);
    if (p.avoid && a && atof(a) > 0)
    {
        p.radius = (float)atof(a);
        a = g_CmdArgv(3);
    }
    p.name = a ? a : "";
    g_pois.push_back(p);
    PoiRebuild();
    if (PoiSave())
        Con("Point %d: %s at (%.0f %.0f %.0f)%s\n", (int)g_pois.size() - 1, kind.c_str(), p.pos.x, p.pos.y, p.pos.z, p.avoid ? ", paths go around it" : ", bots will come here");
}

static void CmdPoiDel()
{
    void* e = NavToolsPlayer();
    if (!e)
        return;
    const char* a = g_CmdArgv(1);
    int index = -1;
    if (a && *a)
        index = atoi(a);
    else
    {
        float best = POI_FIND_RADIUS;
        V3 me = Origin(e) - V3{ 0, 0, 36 };
        for (size_t i = 0; i < g_pois.size(); i++)
            if (Len(g_pois[i].pos - me) < best)
            {
                best = Len(g_pois[i].pos - me);
                index = (int)i;
            }
    }
    if (index < 0 || index >= (int)g_pois.size())
    {
        Con(a && *a ? "No point with that number: poi_list shows them.\n" : "No point near you. poi_del <number> removes any of them.\n");
        return;
    }
    g_pois.erase(g_pois.begin() + index);
    PoiRebuild();
    if (PoiSave())
        Con("Point %d removed; the numbers after it moved down.\n", index);
}

static void CmdPoiList()
{
    void* e = NavToolsPlayer();
    V3 me = e ? Origin(e) : V3{ 0, 0, 0 };
    Con("%s: %d points of interest\n", CurrentMap().c_str(), (int)g_pois.size());
    for (size_t i = 0; i < g_pois.size(); i++)
    {
        auto& p = g_pois[i];
        Con("  %d %s (%.0f %.0f %.0f)%s %s, %.0f u from you\n", (int)i, p.avoid ? "avoid" : "spot", p.pos.x, p.pos.y, p.pos.z, p.avoid ? " radius" : "", p.name.c_str(), Len(p.pos - me));
    }
}

static void CmdPoiClear()
{
    std::string path = PoiFileOf(CurrentMap());
    if (FileThere(path))
        MoveFileExA(path.c_str(), (path + ".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_pois.clear();
    PoiRebuild();
    Con("Points of this map removed (a copy stays in %s.old).\n", path.c_str());
}

static void CmdPoiShow()
{
    g_poiShow = !g_poiShow;
    Con("Points of interest %s: green = spot, red = avoid.\n", g_poiShow ? "shown" : "hidden");
}

static void CmdNavGenerate()
{
    if (!NavToolsPlayer())
        return;
    if (g_gen.state != GEN_OFF)
    {
        Con("Already generating. Leaving the match cancels it.\n");
        return;
    }
    g_gen = {};
    g_gen.state = GEN_RESTARTING;
    g_gen.map = CurrentMap();
    g_gen.level = CheckClock();
    g_gen.since = GetTickCount();
    g_nav.map.clear();
    Con("Generating a new mesh for %s: the map restarts, and it takes a while (a progress bar shows it). It starts from where a bot spawns,\n", g_gen.map.c_str());
    Con("so it only covers what can be walked from there. It restarts the map again when it's done. Use a room without monsters: they need the mesh.\n");
    Log("nav_generate: %s", g_gen.map.c_str());
    RestartMap(g_gen.map);
}

static void CmdNavRestore()
{
    if (!NavToolsPlayer())
        return;
    std::string map = CurrentMap(), path = NavFileOf(map);
    if (!FileThere(path))
    {
        Con("%s already uses the game's mesh.\n", map.c_str());
        return;
    }
    MoveFileExA(path.c_str(), (path + ".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    Con("Back to the game's mesh (yours stays in %s.old). The map restarts.\n", path.c_str());
    Log("nav_restore: %s", map.c_str());
    g_nav.map.clear();
    RestartMap(map);
}

static void CmdNavInfo()
{
    std::string map = CurrentMap();
    Con("%s: %d areas, from %s; %d points of interest\n", map.c_str(), (int)g_nav.areas.size(), FileThere(NavFileOf(map)) ? "botassist_nav" : "the game", (int)g_pois.size());
    void* e = YourEdict();
    if (e && g_nav.ok)
    {
        int a = AreaAt(Origin(e));
        if (a >= 0)
            Con("You are on area %u, %.0f x %.0f%s\n", g_nav.areas[a].id, g_nav.areas[a].hi[0] - g_nav.areas[a].lo[0], g_nav.areas[a].hi[1] - g_nav.areas[a].lo[1], g_nav.areas[a].attr & NAV_CROUCH ? ", crouch" : "");
    }
}

// the game registers it and then doesn't handle it: Update starts the analysis when this flag is set
static void CmdNavAnalyze()
{
    *(unsigned char*)(MP_NAV_ANALYZE_FLAG + g_mpDelta) = 1;
    Con("Analysing the mesh for the game's bots (they need a bot in the match). It saves and restarts the map when it's done.\n");
}

static void CmdNavBinds()
{
    static const char* const binds[][2] = { { "F5", "bot_nav_mark" }, { "F6", "bot_nav_connect" }, { "F7", "bot_nav_disconnect" }, { "F8", "bot_nav_split" },
        { "F9", "bot_nav_merge" }, { "F10", "bot_nav_delete" }, { "F11", "bot_nav_begin_area" }, { "F12", "bot_nav_end_area" } };
    char cmd[128];
    for (auto& b : binds)
    {
        snprintf(cmd, sizeof(cmd), "bind \"%s\" \"%s\"\n", b[0], b[1]);
        g_ServerCommand(cmd);
        Con("  %s = %s\n", b[0], b[1]);
    }
}

static void CmdNavHelp()
{
    Con("Bots' way around the map. Files: Bin\\botassist_nav\\<map>.nav and .poi\n");
    Con("Points of interest (at your feet):\n");
    Con("  poi_add spot [name]            a place bots like to stand\n");
    Con("  poi_add avoid [radius] [name]  a place their paths go around (radius 128)\n");
    Con("  poi_list, poi_del [number], poi_clear, poi_show (draws them)\n");
    Con("Editing the mesh:\n");
    Con("  bot_nav_edit 1 shows the areas; aim at one, then:\n");
    Con("  bot_nav_mark, then aim at another: bot_nav_connect / bot_nav_disconnect / bot_nav_merge / bot_nav_splice\n");
    Con("  bot_nav_split, bot_nav_delete, bot_nav_crouch, bot_nav_jump, bot_nav_no_jump\n");
    Con("  bot_nav_begin_area, walk, bot_nav_end_area: a new area\n");
    Con("  bot_nav_corner_select, bot_nav_corner_raise, bot_nav_corner_lower, bot_nav_warp\n");
    Con("  nav_binds puts the most used ones on F5-F12 (it replaces what those keys did)\n");
    Con("  bot_nav_save saves it; companions use it at once, the game's bots after a restart\n");
    Con("  bot_nav_analyze updates what the game's bots know about it, saves and restarts\n");
    Con("Whole mesh: nav_generate makes a new one, nav_restore goes back to the game's, nav_info tells which one is in use\n");
}

// run from InstallConsoleCommands: names that aren't in the command list yet, and the game's empty bot_nav_analyze
static void NavAddCommands()
{
    NavInstallSaveHook();
    static const struct { const char* name; void (*fn)(); } cmds[] = {
        { "poi_add", CmdPoiAdd }, { "poi_del", CmdPoiDel }, { "poi_list", CmdPoiList }, { "poi_clear", CmdPoiClear }, { "poi_show", CmdPoiShow },
        { "nav_generate", CmdNavGenerate }, { "nav_restore", CmdNavRestore }, { "nav_info", CmdNavInfo }, { "nav_binds", CmdNavBinds }, { "nav_help", CmdNavHelp } };
    void (*botCommand)() = (void (*)())(MP_BOT_COMMAND + g_mpDelta);
    bool have[sizeof(cmds) / sizeof(cmds[0])] = {};
    int n = 0;
    for (CmdFunction* c = *(CmdFunction**)(g_hwLo + HW_CMD_LIST); c && n < 4096; c = c->next, n++)
    {
        if (!Readable(c, sizeof(*c)) || !Readable(c->name, 1))
            return;
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
            have[i] |= !_stricmp(c->name, cmds[i].name);
        if (!_stricmp(c->name, "bot_nav_analyze") && c->fn == botCommand)
            c->fn = CmdNavAnalyze;
    }
    auto add = (AddServerCommandFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_ADDSERVERCOMMAND];
    if (add)
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
            if (!have[i])
                add(cmds[i].name, cmds[i].fn);
}

// ---------------------------------------------------------------- every frame
static void PoiDraw()
{
    static DWORD next = 0;
    DWORD now = GetTickCount();
    if (!g_poiShow || (int)(now - next) < 0)
        return;
    next = now + 1000;
    void* e = YourEdict();
    if (!e)
        return;
    V3 me = Origin(e);
    int beams = 0;
    for (auto& p : g_pois)
    {
        if (Len(p.pos - me) > POI_SHOW_RADIUS || beams > 40)
            continue;
        if (!p.avoid)
        {
            Beam(p.pos, p.pos + V3{ 0, 0, 72 }, 0, 255, 0, 11);
            beams++;
            continue;
        }
        V3 last = p.pos + V3{ p.radius, 0, 8 };
        for (int i = 1; i <= 8; i++)
        {
            float y = i * 45.0f;
            V3 at = p.pos + V3{ cosf(y / RAD) * p.radius, sinf(y / RAD) * p.radius, 8 };
            Beam(last, at, 255, 0, 0, 11);
            last = at;
            beams++;
        }
    }
}

static void GenFrame()
{
    if (g_gen.state == GEN_OFF || !g_pGlobals || !*g_pGlobals)
        return;
    int level = CheckClock();
    DWORD now = GetTickCount();
    if (g_gen.state == GEN_RESTARTING)
    {
        if (level != g_gen.level)
        {
            g_gen.state = GEN_WAITING;
            g_gen.level = level;
            g_gen.since = now;
        }
        else if (now - g_gen.since > 30000)
            GenStop("the map didn't restart, nothing changed");
        return;
    }
    if (level != g_gen.level)
    {
        GenStop("the match ended before it finished, nothing saved");
        return;
    }
    if (g_gen.state != GEN_WAITING)
        return;
    if (now - g_gen.since > 90000)
    {
        GenStop("no bot to start from, nothing changed. Restart the match to get the mesh back");
        return;
    }
    if (g_edictToVars < 0 || (int)(now - g_gen.nextTry) < 0)
        return;
    g_gen.nextTry = now + 1000;
    auto entOf = (EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX];
    int maxClients = *(int*)((DWORD)*g_pGlobals + GV_MAXCLIENTS);
    bool anyBot = false;
    for (int i = 1; i <= maxClients && i <= 64; i++)
    {
        void* e = entOf(i);
        if (!e || !PrivateOf(e) || !(F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT) || !Alive(e))
            continue;
        anyBot = true;
        unsigned char* bot = (unsigned char*)PrivateOf(e);
        void** vtable = *(void***)bot;
        if (!Readable(vtable, 4) || vtable[0] != (void*)(MP_CCSBOT_SPAWN + g_mpDelta))
            continue; // not one of the game's CS bots (a fake player)
        if (*(int*)(bot + BOT_PROCESS_MODE) == 0)
            ((void(__fastcall*)(void*))(MP_START_LEARN + g_mpDelta))(bot);
        if (*(int*)(bot + BOT_PROCESS_MODE) != 0)
        {
            g_gen.state = GEN_LEARNING;
            Con("Learning %s from where a bot stands (%.0f %.0f %.0f)...\n", g_gen.map.c_str(), Origin(e).x, Origin(e).y, Origin(e).z);
            Log("nav_generate: learning from (%.0f %.0f %.0f)", Origin(e).x, Origin(e).y, Origin(e).z);
            return;
        }
        if (++g_gen.tries >= 20)
            GenStop("the game refused every start position, nothing changed. Restart the match to get the mesh back");
        return;
    }
    if (!anyBot && !g_gen.botAsked && now - g_gen.since > 8000)
    {
        g_gen.botAsked = true;
        g_ServerCommand("bot_add\n");
    }
}

static void NavToolsFrame()
{
    PoiDraw();
    GenFrame();
}
