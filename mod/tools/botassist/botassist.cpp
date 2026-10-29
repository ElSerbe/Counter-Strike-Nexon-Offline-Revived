// botassist: companion AI for the game's bots in modes with monsters and NPCs (zombie scenario, human scenario,
// zombie shelter...). The stock bot AI only fights enemy players and ignores radio orders there.
//
// Every bot sends its view angles, movement and buttons to the engine through enginefuncs_t::pfnRunPlayerMove
// (called only from CBot::ExecuteCommand in mp.dll). botassist replaces that pointer in mp.dll's copy of the engine
// table and, while there are monsters on the map, drives friendly bots itself:
//   - moves them along the map's navigation mesh (maps/<map>.nav, the CS:CZ format) with its own A* path finding;
//   - follows the nearest human player by default, each bot keeping its own place in the formation;
//   - obeys radio orders (detours mp.dll's ClientCommand): follow me / regroup / stick together / fall back,
//     hold this position / get in position, cover me (close escort), take point / storm the front / go go go
//     (independent: hunts the monsters itself);
//   - aims at the closest visible monster and fires; pre-aims at monsters it can't see yet, watches open
//     directions instead of walls, backs off from monsters that get too close.
// The bot's own AI still buys weapons, switches and reloads.
//
// Loaded by Miles Sound System, which loads every *.asi file in the game's Bin folder at startup.
// Build (x86, VS Build Tools): cl /nologo /O2 /EHsc /MT /LD botassist.cpp /link /out:botassist.asi user32.lib
// Settings: botassist.ini next to it (see LoadSettings), log: botassist.log. Notes: README.md.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <functional>
#include <winhttp.h>
#include <intrin.h>
#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------- mp.dll (CSO build of 2026) facts, see README.md
static const DWORD MP_PREFERRED_BASE = 0x10000000;
static const DWORD MP_ENGFUNCS = 0x121d3a58;            // enginefuncs_t g_engfuncs
static const DWORD MP_GLOBALS = 0x121d3dac;             // globalvars_t *gpGlobals
static const DWORD MP_EXECUTECOMMAND_CALL = 0x113167f7; // "call [g_engfuncs + 0x1b8]" in CBot::ExecuteCommand
static const DWORD MP_CLIENTCOMMAND = 0x113ebc60;       // void ClientCommand(edict_t *)
// CBot::ClientCommand (0x113166e0) runs a command as a bot: argv[0..3] here, flag set while ClientCommand runs
static const DWORD MP_FAKECMD_ARGS = 0x12182f8c, MP_FAKECMD_FLAG = 0x12182f9c;
static const int ENG_FINDENTITYINSPHERE = 17, ENG_TRACELINE = 33, ENG_CVARGETFLOAT = 62, ENG_CVARGETSTRING = 63,
    ENG_CVARSETSTRING = 65, ENG_SERVERCOMMAND = 42, ENG_MESSAGEBEGIN = 49, ENG_MESSAGEEND = 50, ENG_WRITEBYTE = 51,
    ENG_WRITESHORT = 53, ENG_WRITECOORD = 58, ENG_CMD_ARGV = 88, ENG_LOADFILE = 100, ENG_FREEFILE = 101, ENG_RUNPLAYERMOVE = 110;
static const int PLAYER_TEAM = 0x6c; // CBasePlayer::m_iTeam (1 TR, 2 CT); CBaseEntity::pev is at +8
// entvars_t offsets (CSO adds fields; from mp.dll's gEntvarsDescription save table)
static const int EV_WEAPONMODEL = 0xd0, EV_CLASSNAME = 0x0, EV_ORIGIN = 0x8, EV_VELOCITY = 0x20, EV_PUNCHANGLE = 0x68, EV_VANGLE = 0x74, EV_ANGLES = 0x50, EV_ABSMIN = 0xd4,
    EV_ABSMAX = 0xe0, EV_HEALTH = 0x178, EV_TAKEDAMAGE = 0x190, EV_DEADFLAG = 0x194, EV_VIEW_OFS = 0x198,
    EV_FLAGS = 0x1c8, EV_TEAM = 0x1d8, EV_ARMORVALUE = 0x1e8, EV_NETNAME = 0x200, EV_CONTAINING_ENTITY = 0x238;
static const int GV_TIME = 0x0, GV_MAPNAME = 0xc, GV_STRINGBASE = 0x98;
static const int FL_CLIENT = 0x8, FL_INWATER = 0x10, FL_MONSTER = 0x20, FL_FAKECLIENT = 0x2000;
static const int IN_ATTACK = 1, IN_JUMP = 2, IN_DUCK = 4, IN_RELOAD = 0x2000;
static const float RAD = 57.29578f;

struct TraceResult
{
    int fAllSolid, fStartSolid, fInOpen, fInWater;
    float flFraction;
    float vecEndPos[3];
    float flPlaneDist;
    float vecPlaneNormal[3];
    void* pHit;
    int iHitgroup;
    char pad[64]; // room in case the engine's structure is bigger
};

typedef void* (*FindEntityInSphereFn)(void* start, const float* org, float radius);
typedef void (*TraceLineFn)(const float* v1, const float* v2, int flags, void* skip, TraceResult* tr);
typedef const char* (*CmdArgvFn)(int arg);
typedef unsigned char* (*LoadFileFn)(const char* name, int* length);
typedef void (*FreeFileFn)(void* buffer);
typedef float (*CVarGetFloatFn)(const char* name);
typedef const char* (*CVarGetStringFn)(const char* name);
typedef void (*CVarSetStringFn)(const char* name, const char* value);
typedef void (*ServerCommandFn)(const char* command);
typedef void (*MessageBeginFn)(int dest, int type, const float* origin, void* edict);
typedef void (*MessageEndFn)();
typedef void (*WriteIntFn)(int value);
typedef void (*WriteCoordFn)(float value);
static const DWORD MP_BEAM_SPRITE = 0x12183278; // short: a sprite index mp.dll uses for its own TE_BEAMPOINTS
typedef void (*RunPlayerMoveFn)(void* fakeclient, const float* angles, float fwd, float side, float up,
                                unsigned short buttons, unsigned char impulse, unsigned char msec);
typedef void (*ClientCommandFn)(void* edict);

static FindEntityInSphereFn g_FindEntityInSphere;
static MessageBeginFn g_MessageBegin;
static MessageEndFn g_MessageEnd;
static WriteIntFn g_WriteByte, g_WriteShort;
static WriteCoordFn g_WriteCoord;
static TraceLineFn g_TraceLine;
static CmdArgvFn g_CmdArgv;
static LoadFileFn g_LoadFile;
static FreeFileFn g_FreeFile;
static CVarGetFloatFn g_CVarGetFloat;
static CVarGetStringFn g_CVarGetString;
static CVarSetStringFn g_CVarSetString;
static ServerCommandFn g_ServerCommand;
typedef void* (*GetRoomFn)(); // engine function 165: the room object; its vfunc 0x188 is the game mode
typedef int (*IndexOfEdictFn)(void* edict);
typedef char* (*GetInfoKeyBufferFn)(void* edict);
typedef void (*SetClientKeyValueFn)(int index, char* infobuffer, const char* key, const char* value);
static IndexOfEdictFn g_IndexOfEdict;
typedef void* (*CreateFakeClientFn)(const char* name);
typedef void (*EdictFn)(void* edict);
static CreateFakeClientFn g_CreateFakeClient;
static EdictFn g_FreeEntPrivateData;
static GetInfoKeyBufferFn g_GetInfoKeyBuffer;
static SetClientKeyValueFn g_SetClientKeyValue;
static GetRoomFn g_GetRoom;
static int g_edictToPrivate = -1; // offset of pvPrivateData (the CBaseEntity) inside edict_t
static RunPlayerMoveFn g_RunPlayerMove;
static ClientCommandFn g_ClientCommandTrampoline;
static DWORD** g_pGlobals;
static int g_edictToVars = -1; // offset of entvars_t inside edict_t, found at run time

// ---------------------------------------------------------------- settings / log
static struct
{
    bool enabled = true;
    bool companions = true;    // take over friendly bots (else only aim/fire)
    float radius = 2500.0f;    // how far bots look for monsters
    float awareness = 1200.0f; // monsters closer than this are watched even when not visible
    float turnRate = 540.0f;   // degrees per second
    float fireCone = 6.0f;     // widest aim error (degrees) at which they fire; narrower for small/far targets
    float fleeDistance = 350.0f; // monsters closer than this make them back away
    bool logAim = true;
    float barricadeDamage = 40.0f;  // damage per second each zombie pressed against a barricade does (0: off)
    std::string barricades = "zbsbreak"; // classnames of the barricades zombies break (comma separated)
    float extremeThreat = 160.0f;  // a monster this close takes over from the current target
    std::unordered_map<std::string, int> weaponCategory; // "p_<res>.mdl" -> 1 pistol .. 5 machine gun
    std::unordered_map<int, std::string> weaponModel;     // item id -> "models/p_<res>.mdl"
    std::vector<int> coopModes{ 15, 17, 28, 52, 53, 59 }; // modes with CT spawns only: every bot plays CT
    bool roam = true;
    float objectiveRange = 1500.0f; // bots shoot objectives (breakables the players must destroy) this close
    float leash = 420.0f;
    int apiPort = 30080;            // CSNZ_Server's web API (the room's AI players: GET /api/matchbots)           // in follow mode, never farther than this from you (fighting or not)              // bots spread out around you instead of all following to the same place
    int debug = 1;             // 1: botassist_debug.log (what bots look at / shoot, entities around you); 2: also laser beams
    bool buyAnyWeapon = true;  // bots buy random weapons from the whole game (botassist_weapons.txt)
    std::vector<int> primaries, pistols, melees;
    int skill = -1;            // 0..7, -1: the room's bot difficulty
    std::string voices = "radio"; // normal (their voice chatter), radio (radio commands, shown as text), off
    bool randomCharacters = true; // any character of the game (botassist_characters.txt)
    float refillAfter = 6.0f;  // seconds of firing that empty about half of the ammo: then buy ammo in a buy zone
    float minArmor = 50.0f;    // below this armor, buy kevlar + helmet in a buy zone
    bool sameTeam = true;      // where there are monsters, every bot joins the human team (no enemy bots under the map)
    std::vector<int> characters[3]; // by team: [1] TR, [2] CT
    std::unordered_map<int, int> characterTeam; // class item id -> team
    std::unordered_map<int, int> characterClass; // class item id -> InGameID (the number joinclass takes)
    float followDistance = 180.0f;
    int zgPlayers = -1;        // Zombie Giant: room AI players that join as fake players who can be the giant (-1 all)
    bool zgPlayerGiant = true; // Zombie Giant: you can be the giant too
    float rescan = 0.15f;
    bool logTargets = true;
    bool logCommands = true;
    std::vector<std::string> ignore;
} cfg;

static char g_dir[MAX_PATH];
static CRITICAL_SECTION g_logLock;

static void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist.log", g_dir);
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(path, "a");
    if (f)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static float IniFloat(const char* ini, const char* key, const char* def)
{
    char buf[64];
    GetPrivateProfileStringA("BotAssist", key, def, buf, sizeof(buf), ini);
    return (float)atof(buf);
}

static void LoadSettings()
{
    char ini[MAX_PATH], buf[1024];
    snprintf(ini, sizeof(ini), "%sbotassist.ini", g_dir);
    cfg.enabled = GetPrivateProfileIntA("BotAssist", "Enabled", 1, ini) != 0;
    cfg.companions = GetPrivateProfileIntA("BotAssist", "Companions", 1, ini) != 0;
    cfg.radius = IniFloat(ini, "Radius", "2500");
    cfg.awareness = IniFloat(ini, "Awareness", "1200");
    cfg.turnRate = IniFloat(ini, "TurnRate", "540");
    cfg.fireCone = IniFloat(ini, "FireCone", "6");
    cfg.fleeDistance = IniFloat(ini, "FleeDistance", "350");
    cfg.logAim = GetPrivateProfileIntA("BotAssist", "LogAim", 1, ini) != 0;
    cfg.debug = GetPrivateProfileIntA("BotAssist", "Debug", 2, ini);
    cfg.barricadeDamage = (float)GetPrivateProfileIntA("BotAssist", "ZombieBarricadeDamage", 40, ini);
    GetPrivateProfileStringA("BotAssist", "Barricades", "zbsbreak", buf, sizeof(buf), ini);
    cfg.barricades = buf;
    cfg.extremeThreat = (float)GetPrivateProfileIntA("BotAssist", "ExtremeThreat", 160, ini);
    cfg.roam = GetPrivateProfileIntA("BotAssist", "Roam", 1, ini) != 0;
    cfg.objectiveRange = (float)GetPrivateProfileIntA("BotAssist", "ObjectiveRange", 1500, ini);
    cfg.leash = (float)GetPrivateProfileIntA("BotAssist", "Leash", 420, ini);
    cfg.apiPort = GetPrivateProfileIntA("BotAssist", "ApiPort", 30080, ini);
    GetPrivateProfileStringA("BotAssist", "CoopModes", "15,17,28,52,53,59", buf, sizeof(buf), ini);
    cfg.coopModes.clear();
    for (char* tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", "))
        cfg.coopModes.push_back(atoi(tok));
    cfg.buyAnyWeapon = GetPrivateProfileIntA("BotAssist", "BuyAnyWeapon", 1, ini) != 0;
    cfg.skill = GetPrivateProfileIntA("BotAssist", "Skill", -1, ini);
    GetPrivateProfileStringA("BotAssist", "Voices", "radio", buf, sizeof(buf), ini);
    cfg.voices = buf;
    cfg.randomCharacters = GetPrivateProfileIntA("BotAssist", "RandomCharacters", 1, ini) != 0;
    cfg.sameTeam = GetPrivateProfileIntA("BotAssist", "SameTeam", 1, ini) != 0;
    cfg.refillAfter = (float)GetPrivateProfileIntA("BotAssist", "RefillAfterFiring", 6, ini);
    cfg.minArmor = (float)GetPrivateProfileIntA("BotAssist", "MinArmor", 50, ini);
    // "<class item id> <team> <InGameID>" per line (1 TR, 2 CT), from Item.csv
    char chars[MAX_PATH];
    snprintf(chars, sizeof(chars), "%sbotassist_characters.txt", g_dir);
    for (auto& c : cfg.characters)
        c.clear();
    if (FILE* f = fopen(chars, "r"))
    {
        int id, team, ingame;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            ingame = 0;
            if (line[0] != ';' && sscanf(line, "%d %d %d", &id, &team, &ingame) >= 2 && (team == 1 || team == 2))
            {
                cfg.characters[team].push_back(id);
                cfg.characterTeam[id] = team;
                if (ingame > 0)
                    cfg.characterClass[id] = ingame;
            }
        }
        fclose(f);
    }
    // "<item id> <category>" per line (1 pistol, 2 shotgun, 3 SMG, 4 rifle, 5 machine gun, 11 melee), from Item.csv
    char list[MAX_PATH];
    snprintf(list, sizeof(list), "%sbotassist_weapons.txt", g_dir);
    cfg.primaries.clear();
    cfg.pistols.clear();
    cfg.melees.clear();
    if (FILE* f = fopen(list, "r"))
    {
        int id, cat;
        char line[256], res[128];
        cfg.weaponCategory.clear();
        while (fgets(line, sizeof(line), f))
        {
            if (line[0] == ';' || sscanf(line, "%d %d", &id, &cat) != 2)
                continue;
            (cat == 1 ? cfg.pistols : cat == 11 ? cfg.melees : cfg.primaries).push_back(id);
            const char* semi = strchr(line, ';');
            if (semi && sscanf(semi + 1, " %127s", res) == 1)
            {
                std::string model = std::string("models/p_") + res + ".mdl";
                for (auto& ch : model)
                    ch = (char)tolower(ch);
                cfg.weaponCategory[model] = cat;
                cfg.weaponModel[id] = model;
            }
        }
        fclose(f);
        // Counter-Strike's own weapons whose model isn't named after the item (usp45 -> p_usp.mdl...): the pistols
        // everyone spawns with must be known to drop them ("None" secondary)
        static const struct { const char* model; int cat; } aliases[] = {
            { "models/p_usp.mdl", 1 }, { "models/p_deagle.mdl", 1 }, { "models/p_elite.mdl", 1 },
            { "models/p_glock18.mdl", 1 }, { "models/p_p228.mdl", 1 }, { "models/p_fiveseven.mdl", 1 } };
        for (auto& a : aliases)
            cfg.weaponCategory.emplace(a.model, a.cat);
    }
    cfg.followDistance = IniFloat(ini, "FollowDistance", "180");
    cfg.zgPlayers = GetPrivateProfileIntA("BotAssist", "ZombieGiantPlayers", -1, ini);
    cfg.zgPlayerGiant = GetPrivateProfileIntA("BotAssist", "ZombieGiantYouCanBeGiant", 1, ini) != 0;
    cfg.logTargets = GetPrivateProfileIntA("BotAssist", "LogTargets", 1, ini) != 0;
    cfg.logCommands = GetPrivateProfileIntA("BotAssist", "LogCommands", 1, ini) != 0;
    GetPrivateProfileStringA("BotAssist", "Ignore", "hostage,airbot,turret,drone,npc_escort", buf, sizeof(buf), ini);
    cfg.ignore.clear();
    for (char* tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", "))
        cfg.ignore.push_back(tok);
}

// ---------------------------------------------------------------- small vector math
struct V3
{
    float x, y, z;
};
static V3 Vec(const float* p) { return { p[0], p[1], p[2] }; }
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Len2D(V3 a) { return sqrtf(a.x * a.x + a.y * a.y); }
static float Len(V3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static float YawOf(V3 d) { return atan2f(d.y, d.x) * RAD; }
static float PitchOf(V3 d) { return -atan2f(d.z, Len2D(d)) * RAD; } // v_angle: positive looks down
static float AngleDiff(float a, float b) { return fmodf(a - b + 540.0f, 360.0f) - 180.0f; }
static float Approach(float cur, float goal, float step)
{
    float d = AngleDiff(goal, cur);
    return cur + (d > step ? step : d < -step ? -step : d);
}

// ---------------------------------------------------------------- entities
static inline unsigned char* Vars(void* edict) { return (unsigned char*)edict + g_edictToVars; }
template <class T> static inline T& F(unsigned char* vars, int off) { return *(T*)(vars + off); }
static float Now() { return *(float*)((DWORD)*g_pGlobals + GV_TIME); }

static const char* String(int s)
{
    const char* base = *(const char**)((DWORD)*g_pGlobals + GV_STRINGBASE);
    return s ? base + s : "";
}
static const char* ClassName(void* e) { return String(F<int>(Vars(e), EV_CLASSNAME)); }
static V3 Origin(void* e) { return Vec((float*)(Vars(e) + EV_ORIGIN)); }
static V3 Eye(void* e) { return Origin(e) + Vec((float*)(Vars(e) + EV_VIEW_OFS)); }
static V3 Center(void* e)
{
    unsigned char* v = Vars(e);
    return (Vec((float*)(v + EV_ABSMIN)) + Vec((float*)(v + EV_ABSMAX))) * 0.5f;
}
static bool Alive(void* e)
{
    unsigned char* v = Vars(e);
    return F<float>(v, EV_HEALTH) > 0.0f && F<float>(v, EV_DEADFLAG) == 0.0f;
}
static bool IsIgnored(const char* classname)
{
    for (auto& s : cfg.ignore)
        if (strstr(classname, s.c_str()))
            return true;
    return false;
}
static bool IsLivingMonster(void* e)
{
    int flags = F<int>(Vars(e), EV_FLAGS);
    return (flags & FL_MONSTER) && !(flags & FL_CLIENT) && Alive(e) && F<float>(Vars(e), EV_TAKEDAMAGE) != 0.0f;
}
static void* PrivateOf(void* edict);
static bool BreakRatios(void* e, float* player, float* monster)
{
    unsigned char* p = (unsigned char*)PrivateOf(e);
    if (!p || IsBadReadPtr(p + 0xe0, 8))
        return false;
    *player = *(float*)(p + 0xe0);
    *monster = *(float*)(p + 0xe4);
    return true;
}
static bool IsObjective(void* e)
{
    unsigned char* v = Vars(e);
    if (F<float>(v, EV_HEALTH) <= 0 || F<float>(v, EV_TAKEDAMAGE) == 0.0f)
        return false;
    const char* cls = String(F<int>(v, EV_CLASSNAME));
    if (!cls)
        return false;
    if (strcmp(cls, "zbsbreak"))
        return false;
    float pr = 0, mr = 0;
    return BreakRatios(e, &pr, &mr) && pr > 0.0f && mr <= 0.0f;
}
static bool IsHuman(void* e)
{
    int flags = F<int>(Vars(e), EV_FLAGS);
    return (flags & FL_CLIENT) && !(flags & FL_FAKECLIENT);
}

// calls fn(edict) for every entity within radius; stops at the world entity the engine returns at the end
template <class Fn> static void ForEntitiesNear(V3 center, float radius, Fn fn)
{
    float c[3] = { center.x, center.y, center.z };
    void* e = nullptr;
    for (int guard = 0; guard < 2048; guard++)
    {
        e = g_FindEntityInSphere(e, c, radius);
        if (!e || !strcmp(ClassName(e), "worldspawn"))
            break;
        fn(e);
    }
}

static bool Trace(V3 from, V3 to, void* skip, int noMonsters, TraceResult& tr)
{
    float a[3] = { from.x, from.y, from.z }, b[3] = { to.x, to.y, to.z };
    memset(&tr, 0, sizeof(tr));
    g_TraceLine(a, b, noMonsters, skip, &tr);
    return tr.flFraction >= 0.99f;
}
static bool CanSee(void* bot, V3 eye, void* target)
{
    TraceResult tr;
    return Trace(eye, Center(target), bot, 0, tr) || tr.pHit == target;
}

// ---------------------------------------------------------------- navigation mesh (CS:CZ .nav, version 5)
struct NavArea
{
    unsigned id;
    float lo[3], hi[3];
    float neZ, swZ;
    V3 center;
    std::vector<int> adj[4]; // N (-y), E (+x), S (+y), W (-x): indices into g_nav.areas
};
static struct
{
    std::string map;
    std::vector<NavArea> areas;
    bool ok = false;
    int level = 0; // counts map loads: a new match on the same map is a new level too
} g_nav;

struct Reader
{
    const unsigned char* p;
    const unsigned char* end;
    bool bad = false;
    template <class T> T get()
    {
        T v{};
        if (p + sizeof(T) > end)
        {
            bad = true;
            return v;
        }
        memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    void skip(size_t n)
    {
        if (p + n > end)
            bad = true;
        else
            p += n;
    }
};

static float AreaZ(const NavArea& a, float x, float y)
{
    // the four corners: nw = lo (lo.z), se = hi (hi.z), ne (neZ), sw (swZ)
    float w = a.hi[0] - a.lo[0], h = a.hi[1] - a.lo[1];
    float u = w > 0 ? (x - a.lo[0]) / w : 0, v = h > 0 ? (y - a.lo[1]) / h : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    float north = a.lo[2] + u * (a.neZ - a.lo[2]);
    float south = a.swZ + u * (a.hi[2] - a.swZ);
    return north + v * (south - north);
}

// The level's clock starts again on every map load, also when the next match plays the same map. Watched by
// the watcher thread too, so that a match without bots (nothing calls LoadNav) still counts. Returns the
// number of map loads seen.
static CRITICAL_SECTION g_clockLock;
static float g_clock = 0;
static int g_levels = 0;
static int CheckClock()
{
    if (!g_pGlobals || !*g_pGlobals)
        return g_levels;
    EnterCriticalSection(&g_clockLock);
    float now = Now();
    if (now + 1.0f < g_clock)
        g_levels++;
    g_clock = now;
    int levels = g_levels;
    LeaveCriticalSection(&g_clockLock);
    return levels;
}

static void LoadNav()
{
    std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    int levels = CheckClock();
    bool restarted = levels != g_nav.level;
    g_nav.level = levels;
    if (map.empty() || (map == g_nav.map && !restarted))
        return;
    if (map == g_nav.map)
    {
        Log("new match on %s", map.c_str());
        return; // same map: the navigation mesh is already loaded
    }
    g_nav.map = map;
    g_nav.areas.clear();
    g_nav.ok = false;
    std::string path = "maps/" + map + ".nav";
    int len = 0;
    unsigned char* data = g_LoadFile(path.c_str(), &len);
    if (!data || len < 16)
    {
        Log("%s: no navigation mesh, bots keep their own AI", path.c_str());
        if (data)
            g_FreeFile(data);
        return;
    }
    Reader r{ data, data + len };
    unsigned magic = r.get<unsigned>(), version = r.get<unsigned>();
    if (magic != 0xFEEDFACE || version < 2 || version > 5)
    {
        Log("%s: unsupported navigation mesh (version %u)", path.c_str(), version);
        g_FreeFile(data);
        return;
    }
    if (version >= 4)
        r.get<unsigned>(); // bsp size
    if (version >= 5)
    {
        unsigned short places = r.get<unsigned short>();
        for (unsigned i = 0; i < places && !r.bad; i++)
            r.skip(r.get<unsigned short>());
    }
    unsigned count = r.get<unsigned>();
    std::vector<std::vector<unsigned>> adjIds;
    for (unsigned i = 0; i < count && !r.bad && i < 100000; i++)
    {
        NavArea a;
        a.id = r.get<unsigned>();
        r.get<unsigned char>(); // attributes
        for (int k = 0; k < 3; k++)
            a.lo[k] = r.get<float>();
        for (int k = 0; k < 3; k++)
            a.hi[k] = r.get<float>();
        a.neZ = r.get<float>();
        a.swZ = r.get<float>();
        std::vector<unsigned> ids[4];
        for (int d = 0; d < 4; d++)
        {
            unsigned n = r.get<unsigned>();
            for (unsigned k = 0; k < n && !r.bad && k < 10000; k++)
                ids[d].push_back(r.get<unsigned>());
        }
        unsigned char spots = r.get<unsigned char>();
        r.skip(spots * 17u);                      // hiding spots: id, position, flags
        unsigned char approaches = r.get<unsigned char>();
        r.skip(approaches * 14u);                 // approach areas
        unsigned encounters = r.get<unsigned>();
        for (unsigned k = 0; k < encounters && !r.bad && k < 100000; k++)
        {
            r.skip(4 + 1 + 4 + 1);
            r.skip(r.get<unsigned char>() * 5u);  // spots along the path
        }
        if (version >= 5)
            r.get<unsigned short>();              // place
        a.center = { (a.lo[0] + a.hi[0]) * 0.5f, (a.lo[1] + a.hi[1]) * 0.5f, 0 };
        a.center.z = AreaZ(a, a.center.x, a.center.y);
        g_nav.areas.push_back(a);
        for (int d = 0; d < 4; d++)
            adjIds.push_back(ids[d]);
    }
    g_FreeFile(data);
    if (r.bad || g_nav.areas.empty())
    {
        Log("%s: couldn't read the navigation mesh (%u areas, stopped at area %u)", path.c_str(), count, (unsigned)g_nav.areas.size());
        g_nav.areas.clear();
        return;
    }
    std::unordered_map<unsigned, int> index;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
        index[g_nav.areas[i].id] = (int)i;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
        for (int d = 0; d < 4; d++)
            for (unsigned id : adjIds[i * 4 + d])
            {
                auto it = index.find(id);
                if (it != index.end())
                    g_nav.areas[i].adj[d].push_back(it->second);
            }
    g_nav.ok = true;
    Log("%s: %u navigation areas loaded", path.c_str(), (unsigned)g_nav.areas.size());
}

static int AreaAt(V3 p)
{
    int best = -1;
    float bestScore = 1e30f;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        if (p.x >= a.lo[0] - 1 && p.x <= a.hi[0] + 1 && p.y >= a.lo[1] - 1 && p.y <= a.hi[1] + 1)
        {
            float dz = p.z - AreaZ(a, p.x, p.y);
            if (dz > -40.0f && dz < 120.0f && fabsf(dz) < bestScore)
            {
                bestScore = fabsf(dz);
                best = (int)i;
            }
        }
    }
    if (best >= 0)
        return best;
    for (size_t i = 0; i < g_nav.areas.size(); i++) // off the mesh: the closest area
    {
        float d = Len(g_nav.areas[i].center - p);
        if (d < bestScore)
        {
            bestScore = d;
            best = (int)i;
        }
    }
    return bestScore < 400.0f ? best : -1;
}

// point on the edge shared by area a and its neighbour b (in direction d), nudged into b
static V3 Portal(int ai, int d, int bi)
{
    const NavArea& a = g_nav.areas[ai];
    const NavArea& b = g_nav.areas[bi];
    V3 p = b.center;
    if (d == 0 || d == 2) // north / south: edge along x
    {
        float x0 = max(a.lo[0], b.lo[0]), x1 = min(a.hi[0], b.hi[0]);
        if (x1 >= x0)
            p = { (x0 + x1) * 0.5f, d == 0 ? a.lo[1] - 8.0f : a.hi[1] + 8.0f, 0 };
    }
    else
    {
        float y0 = max(a.lo[1], b.lo[1]), y1 = min(a.hi[1], b.hi[1]);
        if (y1 >= y0)
            p = { d == 1 ? a.hi[0] + 8.0f : a.lo[0] - 8.0f, (y0 + y1) * 0.5f, 0 };
    }
    p.z = AreaZ(b, p.x, p.y);
    return p;
}

static float NavWetCost(int area); // Zombie Giant: areas under water cost more

// A* over areas; returns the waypoints from the bot's area to the goal
static bool FindPath(V3 from, V3 to, std::vector<V3>& out)
{
    out.clear();
    int start = AreaAt(from), goal = AreaAt(to);
    if (start < 0 || goal < 0)
        return false;
    if (start == goal)
    {
        out.push_back(to);
        return true;
    }
    size_t n = g_nav.areas.size();
    std::vector<float> cost(n, 1e30f);
    std::vector<int> parent(n, -1), parentDir(n, -1);
    std::vector<char> closed(n, 0);
    typedef std::pair<float, int> Node;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    cost[start] = 0;
    open.push({ Len(g_nav.areas[start].center - to), start });
    int expanded = 0;
    while (!open.empty() && expanded < 6000)
    {
        int cur = open.top().second;
        open.pop();
        if (closed[cur])
            continue;
        closed[cur] = 1;
        expanded++;
        if (cur == goal)
            break;
        for (int d = 0; d < 4; d++)
            for (int nb : g_nav.areas[cur].adj[d])
            {
                if (closed[nb])
                    continue;
                float c = cost[cur] + Len(g_nav.areas[nb].center - g_nav.areas[cur].center) + NavWetCost(nb);
                if (c < cost[nb])
                {
                    cost[nb] = c;
                    parent[nb] = cur;
                    parentDir[nb] = d;
                    open.push({ c + Len(g_nav.areas[nb].center - to), nb });
                }
            }
    }
    if (parent[goal] < 0)
        return false;
    std::vector<int> chain;
    for (int a = goal; a != start && a >= 0; a = parent[a])
        chain.push_back(a);
    for (int i = (int)chain.size() - 1; i >= 0; i--)
        out.push_back(Portal(parent[chain[i]], parentDir[chain[i]], chain[i]));
    out.push_back(to);
    return true;
}

// ---------------------------------------------------------------- companions
enum Order
{
    ORDER_FOLLOW,      // default: stay near the leader
    ORDER_ESCORT,      // cover me: stay close to the leader
    ORDER_HOLD,        // hold this position
    ORDER_INDEPENDENT, // take point / storm the front: hunt monsters
};
static const char* OrderName(Order o)
{
    return o == ORDER_FOLLOW ? "follow" : o == ORDER_ESCORT ? "escort" : o == ORDER_HOLD ? "hold position" : "independent";
}

struct BotState
{
    void* edict = nullptr;
    int slot = 0;                // place in the formation
    Order order = ORDER_FOLLOW;
    V3 holdSpot{};
    float holdYaw = 0;
    void* target = nullptr;      // visible monster being shot
    void* threat = nullptr;      // closest monster, visible or not
    float nextScan = 0;
    std::vector<V3> path;
    size_t pathIndex = 0;
    V3 pathGoal{};
    float nextRepath = 0;
    V3 lastPos{};
    float lastProgress = 0;
    float unstickUntil = 0;
    float unstickSide = 1;
    float strafeSide = 1, nextStrafe = 0;
    float lastFire = -100, nextReload = 0;
    bool firedSinceReload = false;
    float fireTime = 0;          // seconds spent firing since the last ammo purchase (ammo estimate)
    float nextRefill = 0, nextKill = 0;
    int killTries = 0;
    int roomBot = -1;            // index into g_matchBots: the room AI player this bot plays (name, weapons...)
    bool ordered = false;        // a player gave this bot an order (other modes: botassist only moves it then)
    int appliedClass = 0;        // room character already given to this bot
    unsigned char* ownProfile = nullptr; // this bot's copy of its BotProfile with its room skill (see ApplyRoomSkill)
    int ownSkill = -1;
    int charTeam = 0;            // team its character belongs to (a swap to the other team gets a new one)
    bool wasAliveLoadout = false;
    float nextLoadoutCheck = 0;  // room weapons: check what it holds now and then
    int loadoutBuys = 0;         // buys of the room's weapons in this life
    int loadoutPrimary = 0, loadoutSecondary = 0, loadoutMelee = 0; // what it buys in this life
    float nextStrip = 0;         // "None" slots: drop that weapon
    int strips = 0;              // drops in this life
    int stripProbe = 0;          // next None slot to switch to (to drop what it carries there)
    float nextProbe = 0;
    int probedSlot = 0;          // the slot it was just switched to (1 / 2), and what it held before
    std::string probedFrom;
    void* commander = nullptr;   // who gave it
    bool driven = false;         // botassist drove this bot in the last command
    float nextDebug = 0, nextBeam = 0;
    float stockYaw = 0, stockPitch = 0; // angles the stock AI asked for in the last command
    int fireTick = 0;
    float idleYaw = 0, nextIdleLook = 0;
    float yaw = 0, pitch = 0;    // the aim we send
    bool aimInit = false;
    void* leader = nullptr;
    float nextLeaderScan = 0;
    bool wasAlive = false;
    float buyingSeen = 0, buyingHealth = 0; // respawns without a dead frame (UpdateBuying)
    V3 buyingOrigin = { 0, 0, 0 };
    bool newLife = false;
    float buyAt = 0, checkBuyAt = 0;
    int boughtPrimary = 0, buyTries = 0;
    std::string modelBeforeBuy;
    void* lastTarget = nullptr;
    float targetSince = 0;       // when the current target was first seen (reaction time)
    float errYaw = 0, errPitch = 0, nextErr = 0; // aim error that wanders like a human's
    float flickYaw = 0, flickPitch = 0;          // error of the first snap to a new target, fades out
    float shotYaw = 0, shotPitch = 0;            // spread between shots: a new small error after each shot
    bool wasFiring = false;
    float targetLostAt = 0;                      // the target went out of sight (kept a moment)
    float burstEnd = 0, burstPause = 0;          // long range: bursts of fire
    float lastOnTarget = -100;                   // last time the line of fire was on a monster
    std::string tapModel;                        // weapon that didn't fire while held: semi-automatic
    float holdingSince = 0, punchAtHold = 0;
    int teamFixTries = 0;
    float nextTeamFix = 0;
    V3 roamGoal{};                               // spot around the leader to stand on
    bool hasRoam = false;
    float roamUntil = 0;
    V3 roamAnchor{};                             // leader position when the spot was chosen
    float lastHp = -1;
    float hurtAt = -100;                         // last damage that didn't come from a monster
    V3 safePos{};
    bool hasSafe = false;
    float safeCheck = 0;
    float escapeUntil = 0;
    float nextRoamPick = 0;
    int shots = 0, hits = 0;     // aim self-check: shots whose line actually crosses the target
    float nextAimLog = 0;
};
static BotState g_bots[64];
static Order g_teamOrder = ORDER_FOLLOW;
static float g_lastMonsterSeen = -100;
static std::vector<std::string> g_seen;

static BotState& StateFor(void* edict)
{
    int used = 0;
    for (auto& b : g_bots)
    {
        if (b.edict == edict)
            return b;
        used += b.edict != nullptr;
    }
    for (auto& b : g_bots)
        if (!b.edict)
        {
            b = BotState();
            b.edict = edict;
            b.slot = used;
            b.order = g_teamOrder;
            return b;
        }
    g_bots[0] = BotState();
    g_bots[0].edict = edict;
    return g_bots[0];
}

static void LogTarget(void* e)
{
    if (!cfg.logTargets)
        return;
    std::string cls = ClassName(e);
    for (auto& s : g_seen)
        if (s == cls)
            return;
    if (g_seen.size() < 200)
    {
        g_seen.push_back(cls);
        Log("first target of class '%s' (health %.0f, team %d)", cls.c_str(), F<float>(Vars(e), EV_HEALTH), F<int>(Vars(e), EV_TEAM));
    }
}

static int g_monsterLevel = -1; // level where monsters were seen: botassist drives the bots there all the time
static bool MonsterMap()
{
    float now = Now();
    static int seenLevel = -1;
    if (seenLevel != g_nav.level)
    {
        seenLevel = g_nav.level;
        g_lastMonsterSeen = -100;
    }
    return (now - g_lastMonsterSeen < 15.0f && now >= g_lastMonsterSeen) || g_monsterLevel == g_nav.level;
}

static void ScanMonsters(void* bot, BotState& st, V3 eye)
{
    void* best = nullptr;
    void* nearest = nullptr;
    float bestDist = 1e30f, nearestDist = 1e30f;
    ForEntitiesNear(eye, cfg.radius, [&](void* e) {
        if (e == bot || !IsLivingMonster(e) || IsIgnored(ClassName(e)))
            return;
        g_lastMonsterSeen = Now();
        g_monsterLevel = g_nav.level;
        float d = Len(Center(e) - eye);
        if (d < nearestDist)
        {
            nearestDist = d;
            nearest = e;
        }
        if (d < bestDist && CanSee(bot, eye, e))
        {
            bestDist = d;
            best = e;
        }
    });
    // stay on the current target until it dies or stays out of sight; switch only for an extreme threat:
    // a visible monster about to hit the bot while the current one is clearly farther away
    void* cur = st.target;
    if (cur && IsLivingMonster(cur) && best) // a monster in sight always beats an objective
    {
        float curDist = Len(Center(cur) - eye);
        bool extreme = best && best != cur && bestDist < cfg.extremeThreat && curDist > bestDist * 1.8f;
        if (!extreme)
            best = cur;
    }
    else if (cur && IsLivingMonster(cur))
        best = cur;
    // no monster to shoot: objectives the players have to break, closest visible one
    if (!best && cfg.objectiveRange > 0)
    {
        float objDist = 1e30f;
        void* obj = nullptr;
        if (cur && IsObjective(cur) && CanSee(bot, eye, cur))
            obj = cur;
        else
            ForEntitiesNear(eye, cfg.objectiveRange, [&](void* e) {
                if (!IsObjective(e))
                    return;
                float d = Len(Center(e) - eye);
                if (d < objDist && CanSee(bot, eye, e))
                {
                    objDist = d;
                    obj = e;
                }
            });
        best = obj;
    }
    st.target = best;
    st.threat = nearestDist < cfg.awareness ? nearest : (best && IsLivingMonster(best) ? best : nullptr);
    if (best)
        LogTarget(best);
}

// leader: the closest living human player
static void* FindLeader(V3 from)
{
    void* best = nullptr;
    float bestDist = 1e30f;
    ForEntitiesNear(from, 16384.0f, [&](void* e) {
        if (!IsHuman(e) || !Alive(e))
            return;
        float d = Len(Origin(e) - from);
        if (d < bestDist)
        {
            bestDist = d;
            best = e;
        }
    });
    return best;
}

// formation: spread the bots around the leader so they don't stack up
static V3 FormationSpot(void* leader, int slot, float distance)
{
    static const float offsets[] = { 140, -140, 180, 100, -100, 160, -160, 120 };
    float yaw = F<float>(Vars(leader), EV_VANGLE + 4) + offsets[slot % 8];
    V3 o = Origin(leader);
    V3 spot = { o.x + cosf(yaw / RAD) * distance, o.y + sinf(yaw / RAD) * distance, o.z };
    // stay on the leader's side of walls
    TraceResult tr;
    if (!Trace(o, spot, nullptr, 1, tr))
        spot = { o.x + (tr.vecEndPos[0] - o.x) * 0.7f, o.y + (tr.vecEndPos[1] - o.y) * 0.7f, o.z };
    return spot;
}

// direction worth watching when nothing is around: the open one closest to the preferred yaw
static float OpenYaw(void* bot, V3 eye, float preferred)
{
    TraceResult tr;
    for (int i = 0; i < 8; i++)
    {
        float yaw = preferred + ((i & 1) ? 1 : -1) * ((i + 1) / 2) * 45.0f;
        V3 farPoint = { eye.x + cosf(yaw / RAD) * 400.0f, eye.y + sinf(yaw / RAD) * 400.0f, eye.z };
        if (Trace(eye, farPoint, bot, 1, tr) || tr.flFraction > 0.5f)
            return yaw;
    }
    return preferred;
}

// keeps st.path towards goal, returns the point to walk to (or false when there)
static bool NextWaypoint(void* bot, BotState& st, V3 pos, V3 goal, float arrive, V3& out)
{
    float now = Now();
    if (Len2D(goal - pos) < arrive && fabsf(goal.z - pos.z) < 64)
    {
        st.path.clear();
        return false;
    }
    if (!g_nav.ok)
    {
        out = goal; // no mesh: straight line
        return true;
    }
    if (st.path.empty() || now >= st.nextRepath || Len(goal - st.pathGoal) > 120.0f)
    {
        if (!FindPath(pos, goal, st.path))
            st.path = { goal };
        st.pathIndex = 0;
        st.pathGoal = goal;
        st.nextRepath = now + 1.0f;
    }
    while (st.pathIndex + 1 < st.path.size() && Len2D(st.path[st.pathIndex] - pos) < 48.0f)
        st.pathIndex++;
    // skip ahead to a waypoint in plain view to cut corners
    TraceResult tr;
    V3 eye = pos + V3{ 0, 0, 16 };
    for (size_t i = min(st.path.size() - 1, st.pathIndex + 3); i > st.pathIndex; i--)
        if (fabsf(st.path[i].z - pos.z) < 40 && Trace(eye, st.path[i] + V3{ 0, 0, 16 }, bot, 1, tr))
        {
            st.pathIndex = i;
            break;
        }
    out = st.path[st.pathIndex];
    return true;
}

// ---- places to stand: spread out around the leader, in the open, away from hurting places
struct Hazard
{
    V3 pos;
    float until;
};
static std::vector<Hazard> g_hazards;

static bool InHazard(V3 p)
{
    float now = Now();
    for (auto& h : g_hazards)
        if (now < h.until && Len2D(p - h.pos) < 170.0f && fabsf(p.z - h.pos.z) < 100.0f)
            return true;
    return false;
}

// open space around a point: free lines at waist height in most directions (not a corner)
static int OpenDirections(void* bot, V3 p)
{
    TraceResult tr;
    V3 a = p + V3{ 0, 0, 36 };
    int open = 0;
    for (int i = 0; i < 8; i++)
    {
        float y = i * 45.0f;
        if (Trace(a, a + V3{ cosf(y / RAD) * 110.0f, sinf(y / RAD) * 110.0f, 0 }, bot, 1, tr))
            open++;
    }
    return open;
}

// a spot near `around` (nav area centre) that is open, reachable, out of hazards and away from the other bots
static bool PickSpot(void* bot, BotState& st, V3 pos, V3 around, float minR, float maxR, V3& out)
{
    float now = Now();
    if (!g_nav.ok || g_nav.areas.empty() || now < st.nextRoamPick)
        return false;
    st.nextRoamPick = now + 1.5f; // retried at most this often
    for (int tries = 0; tries < 60; tries++)
    {
        const NavArea& a = g_nav.areas[rand() % g_nav.areas.size()];
        V3 c = a.center;
        float d = Len2D(c - around);
        if (d < minR || d > maxR || fabsf(c.z - around.z) > 160.0f || InHazard(c))
            continue;
        bool crowded = false;
        for (auto& o : g_bots)
            if (&o != &st && o.edict && o.hasRoam && Len2D(o.roamGoal - c) < 200.0f)
                crowded = true;
        if (crowded || OpenDirections(bot, c) < 6)
            continue;
        std::vector<V3> path;
        if (!FindPath(pos, c, path) || path.size() > 60)
            continue;
        out = c;
        st.nextRoamPick = now;
        return true;
    }
    return false;
}

static const char* Str(int s);

// where to shoot: upper body, led a little by the target's movement. Bosses' boxes reach far above their bodies
// (Zombie Disaster Zone's boss: the bots shot into the air above it): on a target taller than a player the bots
// aim at most 100 units above its feet, where every big monster has a body.
static const float TALL_TARGET = 150.0f, TALL_AIM = 100.0f;
static V3 AimPoint(void* target)
{
    unsigned char* v = Vars(target);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    V3 p = (lo + hi) * 0.5f;
    float height = hi.z - lo.z;
    if (height > TALL_TARGET)
    {
        p.z = lo.z + TALL_AIM;
        static std::vector<std::string> seen;
        std::string model = Str(F<int>(v, 0xc8)); // pev->model
        if (std::find(seen.begin(), seen.end(), model) == seen.end() && seen.size() < 50)
        {
            seen.push_back(model);
            Log("tall target %s: box %.0f x %.0f x %.0f, aimed %.0f above its feet", model.c_str(), hi.x - lo.x,
                hi.y - lo.y, height, TALL_AIM);
        }
        return p;
    }
    p.z += height * 0.2f;
    return p + Vec((float*)(v + EV_VELOCITY)) * 0.06f;
}

// half the target's width as seen from `dist` away, in degrees: the aim error that still hits
static float HitCone(void* target, float dist)
{
    unsigned char* v = Vars(target);
    float halfWidth = (F<float>(v, EV_ABSMAX) - F<float>(v, EV_ABSMIN)) * 0.35f;
    float cone = atan2f(halfWidth, dist > 1 ? dist : 1) * RAD;
    return cone < 1.0f ? 1.0f : cone > cfg.fireCone ? cfg.fireCone : cone;
}

static V3 Forward(float pitch, float yaw)
{
    float cp = cosf(pitch / RAD), sp = sinf(pitch / RAD), cy = cosf(yaw / RAD), sy = sinf(yaw / RAD);
    return { cp * cy, cp * sy, -sp }; // AngleVectors: positive pitch looks down
}

// a direction close to `yaw` the bot can walk without hitting a wall (knee height), or yaw itself
static float ClearYaw(void* bot, V3 pos, float yaw, float dist)
{
    TraceResult tr;
    static const float tries[] = { 0, 30, -30, 60, -60, 90, -90, 120, -120 };
    V3 knee = pos - V3{ 0, 0, 18 };
    for (float t : tries)
    {
        float y = yaw + t;
        V3 to = knee + V3{ cosf(y / RAD) * dist, sinf(y / RAD) * dist, 0 };
        if (Trace(knee, to, bot, 1, tr))
            return y;
    }
    return yaw;
}

// ---------------------------------------------------------------- the room's AI players
// The room list can hold AI players (CSNZ_Server: /addbot or the editor). When a match starts the server asks
// the game for as many bots per team and keeps their settings; each bot that joins takes the next AI player of
// its team: its name, character, weapons and skill.
struct MatchBot
{
    std::string name;
    int team = 2, primary = 0, secondary = 0, melee = 0, character = 0, skill = -1;
    std::vector<int> allowPrimary, allowSecondary, allowMelee; // weapon filter: random weapons come from these
    std::vector<int> allowCharacter; // the filter's characters (both teams): a random character comes from them
    void* edict = nullptr; // the bot playing it
    bool fakePlayer = false; // Zombie Giant: played by a fake player who can be the giant (no bot), see ZgPlayer
};
static std::vector<MatchBot> g_matchBots;
static CRITICAL_SECTION g_matchLock;
static volatile LONG g_matchFetch = 0; // 0 idle, 1 fetching, 2 done
static bool g_matchKnown = false;      // the server answered: the room's list is the bot list of this match
static int g_matchAdded[3];            // bots the game added in this match, by team
static int g_matchLevel = -1; // the level whose AI players were fetched

static int JsonInt(const std::string& obj, const char* key, int def)
{
    std::string k = std::string("\"") + key + "\":";
    size_t i = obj.find(k);
    return i == std::string::npos ? def : atoi(obj.c_str() + i + k.size());
}
static std::vector<int> JsonInts(const std::string& obj, const char* key)
{
    std::vector<int> out;
    std::string k = std::string("\"") + key + "\":[";
    size_t i = obj.find(k);
    if (i == std::string::npos)
        return out;
    const char* c = obj.c_str() + i + k.size();
    while (*c && *c != ']')
    {
        if ((*c >= '0' && *c <= '9') || *c == '-')
        {
            out.push_back(atoi(c));
            while ((*c >= '0' && *c <= '9') || *c == '-')
                c++;
        }
        else
            c++;
    }
    return out;
}
static std::string JsonStr(const std::string& obj, const char* key)
{
    std::string k = std::string("\"") + key + "\":\"";
    size_t i = obj.find(k);
    if (i == std::string::npos)
        return "";
    std::string out;
    for (i += k.size(); i < obj.size() && obj[i] != '"'; i++)
    {
        if (obj[i] == '\\' && i + 1 < obj.size())
            i++;
        out += obj[i];
    }
    return out;
}

static std::string HttpGet(const wchar_t* path)
{
    std::string body;
    HINTERNET ses = WinHttpOpen(L"botassist", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        return body;
    WinHttpSetTimeouts(ses, 2000, 2000, 3000, 3000);
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)cfg.apiPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL))
    {
        char buf[4096];
        DWORD got = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &got) && got)
            body.append(buf, got);
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return body;
}

static DWORD WINAPI FetchMatchBots(LPVOID)
{
    std::string json = HttpGet(L"/api/matchbots");
    std::vector<MatchBot> bots;
    size_t i = json.find("\"bots\":[");
    while (i != std::string::npos)
    {
        size_t a = json.find('{', i), b = a == std::string::npos ? a : json.find('}', a);
        if (a == std::string::npos || b == std::string::npos)
            break;
        std::string obj = json.substr(a, b - a + 1);
        MatchBot m;
        m.name = JsonStr(obj, "name");
        m.team = JsonInt(obj, "team", 2);
        m.primary = JsonInt(obj, "primary", 0);
        m.secondary = JsonInt(obj, "secondary", 0);
        m.melee = JsonInt(obj, "melee", 0);
        m.character = JsonInt(obj, "character", 0);
        m.skill = JsonInt(obj, "skill", -1);
        m.allowPrimary = JsonInts(obj, "allowPrimary");
        m.allowSecondary = JsonInts(obj, "allowSecondary");
        m.allowMelee = JsonInts(obj, "allowMelee");
        m.allowCharacter = JsonInts(obj, "allowCharacter");
        // a filter whose slot is "None" ([-1]): a bot with a random weapon there gets none
        auto none = [](int& slot, std::vector<int>& allow) {
            bool isNone = std::find(allow.begin(), allow.end(), -1) != allow.end();
            if (isNone && slot == 0)
                slot = -1;
            if (isNone)
                allow.clear();
        };
        none(m.primary, m.allowPrimary);
        none(m.secondary, m.allowSecondary);
        bots.push_back(m);
        i = b + 1;
    }
    EnterCriticalSection(&g_matchLock);
    g_matchBots = bots;
    g_matchKnown = json.find("\"bots\":[") != std::string::npos;
    LeaveCriticalSection(&g_matchLock);
    InterlockedExchange(&g_matchFetch, 2);
    Log("room AI players for this match: %d%s", (int)bots.size(), json.empty() ? " (server not reachable)" : "");
    for (auto& m : bots)
        Log("  '%s' team %d, primary %d, secondary %d, melee %d, character %d, skill %d, filter %d/%d/%d weapons, %d characters",
            m.name.c_str(), m.team, m.primary, m.secondary, m.melee, m.character, m.skill, (int)m.allowPrimary.size(),
            (int)m.allowSecondary.size(), (int)m.allowMelee.size(), (int)m.allowCharacter.size());
    return 0;
}

// a new map: get the room's AI players for this match (in the background)
static void UpdateMatchBots()
{
    if (g_nav.map.empty() || g_nav.level == g_matchLevel)
        return;
    g_matchLevel = g_nav.level;
    EnterCriticalSection(&g_matchLock);
    g_matchBots.clear();
    g_matchKnown = false;
    g_matchAdded[0] = g_matchAdded[1] = g_matchAdded[2] = 0;
    LeaveCriticalSection(&g_matchLock);
    for (auto& b : g_bots)
    {
        b.roomBot = -1;
        b.ordered = false;
        b.commander = nullptr;
        b.order = ORDER_FOLLOW;
    }
    g_teamOrder = ORDER_FOLLOW;
    InterlockedExchange(&g_matchFetch, 1);
    FetchMatchBots(NULL);
}

static void Rename(void* bot, const std::string& name)
{
    if (!g_SetClientKeyValue || !g_GetInfoKeyBuffer || !g_IndexOfEdict || name.empty())
        return;
    char* info = g_GetInfoKeyBuffer(bot);
    int index = g_IndexOfEdict(bot);
    if (info && index > 0)
        g_SetClientKeyValue(index, info, "name", name.c_str());
}

static int TeamOf(void* e);
static bool CoopMode();

// the room AI player this bot plays (assigned when its team is known), or null
static MatchBot* RoomBotOf(void* bot, BotState& st)
{
    if (g_matchFetch != 2)
        return nullptr;
    EnterCriticalSection(&g_matchLock);
    MatchBot* m = nullptr;
    if (st.roomBot >= 0 && st.roomBot < (int)g_matchBots.size() && g_matchBots[st.roomBot].edict == bot)
        m = &g_matchBots[st.roomBot];
    else
    {
        int team = TeamOf(bot);
        int pick = -1;
        for (size_t i = 0; team && i < g_matchBots.size() && pick < 0; i++)
            if (!g_matchBots[i].edict && !g_matchBots[i].fakePlayer && g_matchBots[i].team == team)
                pick = (int)i;
        for (size_t i = 0; team && i < g_matchBots.size() && pick < 0; i++)
            if (!g_matchBots[i].edict && !g_matchBots[i].fakePlayer)
                pick = (int)i;
        for (size_t i = (size_t)(pick < 0 ? g_matchBots.size() : pick); i < g_matchBots.size() && (int)i == pick; i++)
            {
                g_matchBots[i].edict = bot;
                st.roomBot = (int)i;
                m = &g_matchBots[i];
                Log("bot %d plays room AI player '%s'", st.slot, m->name.c_str());
                Rename(bot, m->name);
                break;
            }
    }
    LeaveCriticalSection(&g_matchLock);
    return m;
}

// skill 0..7 (the room's bot difficulty unless Skill= is set): turn speed, reaction time, aim error
struct Skill
{
    float turn, reaction, error;
    float flick;  // error of the first snap to a new target (degrees), fades out in `settle` seconds
    float settle;
    float spread; // spread between shots (degrees)
    float recoil; // part of the recoil the bot compensates
    float burst;  // seconds of fire per burst at long range
};
static float g_difficulty = 3;
static Skill CurrentSkill(int botSkill = -1)
{
    float d = botSkill >= 0 ? (float)botSkill : cfg.skill >= 0 ? (float)cfg.skill : g_difficulty;
    d = d < 0 ? 0 : d > 7 ? 7 : d;
    return { 360.0f + d * 90.0f, 0.5f - d * 0.05f, 1.6f - d * 0.18f, 6.0f - d * 0.7f, 0.45f - d * 0.04f,
             1.5f - d * 0.17f, 0.55f + d * 0.06f, 0.35f + d * 0.1f };
}

// automatic weapons (SMGs, rifles, machine guns: miniguns need to spin up) keep the trigger held;
// pistols and shotguns are tapped. A held weapon that never kicks is semi-automatic: tapped from then on.
static bool HoldsTrigger(void* bot, BotState& st, float now)
{
    const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    std::string model = m ? m : "";
    for (auto& ch : model)
        ch = (char)tolower(ch);
    if (!model.empty() && model == st.tapModel)
        return false;
    auto it = cfg.weaponCategory.find(model);
    int cat = it != cfg.weaponCategory.end() ? it->second : 0;
    if (cat == 1 || cat == 2)
        return false;
    return true;
}

struct Command
{
    float yaw, pitch;
    float forward = 0, side = 0;
    unsigned short buttons;
};

static void Drive(void* bot, BotState& st, Command& cmd, float dt)
{
    unsigned char* v = Vars(bot);
    float now = Now();
    V3 pos = Origin(bot), eye = Eye(bot);
    if (!st.aimInit)
    {
        st.yaw = cmd.yaw;
        st.pitch = cmd.pitch;
        st.aimInit = true;
    }
    if (now >= st.nextScan || (st.target && !IsLivingMonster(st.target) && !IsObjective(st.target)))
    {
        ScanMonsters(bot, st, eye);
        st.nextScan = now + cfg.rescan;
    }
    if (st.threat && !IsLivingMonster(st.threat))
        st.threat = nullptr;
    if (st.target)
    {
        TraceResult tr;
        bool seen = Trace(eye, AimPoint(st.target), bot, 0, tr) || tr.pHit == st.target ||
                    (tr.pHit && IsLivingMonster(tr.pHit)); // another monster in front: still there
        if (seen)
            st.targetLostAt = 0;
        else if (!st.targetLostAt)
            st.targetLostAt = now;
        else if (now - st.targetLostAt > 0.7f)
        {
            st.target = nullptr; // behind cover for a while: rescan right away
            st.targetLostAt = 0;
            st.nextScan = now;
        }
    }

    // ---- where to go
    if (now >= st.nextLeaderScan || (st.leader && (!IsHuman(st.leader) || !Alive(st.leader))))
    {
        st.leader = FindLeader(pos);
        st.nextLeaderScan = now + 0.5f;
    }
    void* leader = st.leader;
    V3 goal = pos;
    float arrive = 60.0f;
    bool wantMove = false;

    // ---- hurt by the place (a damaging trigger, outside the shelter...): remember it and get out
    {
        float hp = F<float>(v, EV_HEALTH);
        if (st.lastHp >= 0 && hp < st.lastHp - 0.5f)
        {
            bool monsterNear = false;
            ForEntitiesNear(pos, 160.0f, [&](void* e) { monsterNear |= IsLivingMonster(e) != 0; });
            if (!monsterNear)
            {
                st.hurtAt = now;
                if (!InHazard(pos))
                {
                    g_hazards.push_back({ pos, now + 30.0f });
                    static int reports = 0;
                    if (reports++ < 30)
                        Log("bot %d hurt by the place at (%.0f %.0f %.0f): leaving it", st.slot, pos.x, pos.y, pos.z);
                }
                st.escapeUntil = now + 4.0f;
                st.hasRoam = false;
            }
        }
        st.lastHp = hp;
        if (now >= st.safeCheck)
        {
            st.safeCheck = now + 1.0f;
            if (now - st.hurtAt > 2.5f && !InHazard(pos))
            {
                st.safePos = pos;
                st.hasSafe = true;
            }
        }
        if (g_hazards.size() > 64)
            g_hazards.erase(g_hazards.begin());
    }
    bool escaping = now < st.escapeUntil;
    if (escaping)
    {
        if (st.hasSafe && !InHazard(st.safePos))
            goal = st.safePos;
        else
        {
            V3 spot;
            if (PickSpot(bot, st, pos, pos, 150.0f, 700.0f, spot))
                st.safePos = goal = spot, st.hasSafe = true;
        }
        arrive = 40.0f;
        wantMove = true;
    }
    else
    switch (st.order)
    {
    case ORDER_FOLLOW:
    case ORDER_ESCORT:
        if (leader)
        {
            float dist = st.order == ORDER_ESCORT ? 90.0f : cfg.followDistance;
            V3 lp = Origin(leader);
            float leaderDist = Len(lp - pos);
            // in a fight, stay put and shoot unless the leader walks away
            bool fighting = st.target && leaderDist < dist + 120.0f;
            // the leader stays around: spread out to an open spot near them, a different one per bot, and move
            // to another one now and then; when the leader walks away, follow in formation again
            bool leaderMoved = Len2D(lp - st.roamAnchor) > 160.0f || leaderDist > cfg.leash;
            if (st.order == ORDER_FOLLOW && cfg.roam && !leaderMoved && st.hasRoam && now < st.roamUntil)
            {
                if (!fighting)
                {
                    goal = st.roamGoal;
                    arrive = 50.0f;
                    wantMove = true;
                }
                break;
            }
            st.hasRoam = false;
            if (st.order == ORDER_FOLLOW && cfg.roam && Len2D(Vec((float*)(Vars(leader) + EV_VELOCITY))) < 40.0f &&
                leaderDist < cfg.leash)
            {
                V3 spot;
                if (PickSpot(bot, st, pos, lp, 120.0f, cfg.leash - 80.0f, spot))
                {
                    st.roamGoal = spot;
                    st.hasRoam = true;
                    st.roamAnchor = lp;
                    st.roamUntil = now + 6.0f + (rand() % 800) / 100.0f;
                    break;
                }
            }
            if (!fighting || leaderDist > dist + 120.0f)
            {
                goal = FormationSpot(leader, st.slot, dist);
                if (InHazard(goal))
                    goal = st.hasSafe ? st.safePos : pos;
                arrive = leaderDist > dist * 2 ? 50.0f : 70.0f;
                wantMove = true;
            }
        }
        break;
    case ORDER_HOLD:
        goal = st.holdSpot;
        wantMove = true;
        break;
    case ORDER_INDEPENDENT:
        if (st.threat && !st.target)
        {
            goal = Origin(st.threat);
            wantMove = true;
        }
        else if (!st.threat && leader && Len(Origin(leader) - pos) > 900.0f)
        {
            goal = FormationSpot(leader, st.slot, cfg.followDistance); // nothing to hunt: rejoin
            wantMove = true;
        }
        break;
    }

    V3 waypoint;
    bool moving = wantMove && NextWaypoint(bot, st, pos, goal, arrive, waypoint);
    float moveYaw = 0;
    if (moving)
    {
        moveYaw = YawOf(waypoint - pos);
        if (waypoint.z > pos.z + 24.0f && Len2D(waypoint - pos) < 120.0f)
            cmd.buttons |= IN_JUMP;
    }

    // ---- combat movement: get away from monsters that come close, strafe at mid range
    void* danger = st.target ? st.target : st.threat;
    if (danger && !IsLivingMonster(danger))
        danger = nullptr;
    bool leashed = (st.order == ORDER_FOLLOW || st.order == ORDER_ESCORT) && leader &&
                   Len2D(Origin(leader) - pos) > cfg.leash * 0.75f;
    if (leashed && moving)
        danger = nullptr; // too far behind: catch up first, shoot while walking
    if (danger)
    {
        float d = Len2D(Center(danger) - pos);
        if (d < cfg.fleeDistance)
        {
            float away = YawOf(pos - Center(danger));
            // back off towards the leader's side rather than straight away from them
            if (leader && (st.order == ORDER_FOLLOW || st.order == ORDER_ESCORT))
            {
                float toLeader = YawOf(Origin(leader) - pos);
                float diff = AngleDiff(toLeader, away);
                if (fabsf(diff) > 60.0f)
                    away += diff > 0 ? diff - 60.0f : diff + 60.0f;
            }
            moveYaw = ClearYaw(bot, pos, away, 96.0f);
            moving = true;
        }
        else if (st.target && !moving && d < 700.0f)
        {
            if (now >= st.nextStrafe)
            {
                st.strafeSide = -st.strafeSide;
                st.nextStrafe = now + 0.6f + (rand() % 60) / 100.0f;
            }
            moveYaw = ClearYaw(bot, pos, YawOf(Center(st.target) - pos) + 90.0f * st.strafeSide, 64.0f);
            moving = true;
        }
    }
    if (moving && !(danger && Len2D(Center(danger) - pos) < cfg.fleeDistance))
        moveYaw = ClearYaw(bot, pos, moveYaw, 40.0f); // walk around obstacles

    // ---- stuck: jump and sidestep, then repath
    if (moving)
    {
        if (Len2D(pos - st.lastPos) > 24.0f)
        {
            st.lastPos = pos;
            st.lastProgress = now;
        }
        else if (now - st.lastProgress > 1.0f && now >= st.unstickUntil)
        {
            st.unstickUntil = now + 0.5f;
            st.unstickSide = (rand() & 1) ? 1.0f : -1.0f;
            st.lastProgress = now;
            st.nextRepath = 0;
            cmd.buttons |= IN_JUMP;
        }
        if (now < st.unstickUntil)
            moveYaw += 70.0f * st.unstickSide;
    }
    else
    {
        st.lastPos = pos;
        st.lastProgress = now;
    }

    // ---- where to look
    float lookYaw, lookPitch = 0;
    if (st.target)
    {
        V3 d = AimPoint(st.target) - eye;
        lookYaw = YawOf(d);
        lookPitch = PitchOf(d);
    }
    else if (st.threat && Len(Center(st.threat) - eye) < 700.0f)
    {
        V3 d = Center(st.threat) - eye; // heard, not seen: watch where it will come from
        lookYaw = YawOf(d);
        lookPitch = PitchOf(d) * 0.3f;
    }
    else if (moving && Len2D(Vec((float*)(v + EV_VELOCITY))) > 60.0f)
    {
        lookYaw = moveYaw;
    }
    else
    {
        if (now >= st.nextIdleLook)
        {
            float base = st.order == ORDER_HOLD ? st.holdYaw
                         : leader                  ? F<float>(Vars(leader), EV_VANGLE + 4)
                                                   : st.yaw;
            static const float watch[] = { 0, 70, -70, 150, -150, 110, -110, 30 };
            st.idleYaw = OpenYaw(bot, eye, base + watch[st.slot % 8] + (rand() % 40 - 20));
            st.nextIdleLook = now + 2.0f + (rand() % 200) / 100.0f;
        }
        lookYaw = st.idleYaw;
    }
    // skill: aim error that drifts, reaction time before the first shot, turn speed
    MatchBot* rb = RoomBotOf(bot, st);
    Skill skill = CurrentSkill(rb ? rb->skill : -1);
    if (now >= st.nextErr)
    {
        st.errYaw = ((rand() % 2001) / 1000.0f - 1.0f) * skill.error;
        st.errPitch = ((rand() % 2001) / 1000.0f - 1.0f) * skill.error * 0.6f;
        st.nextErr = now + 0.25f + (rand() % 30) / 100.0f;
    }
    if (st.target != st.lastTarget)
    {
        st.lastTarget = st.target;
        st.targetSince = now - (now - st.lastOnTarget < 0.5f ? skill.reaction : 0.0f); // already shooting
        // snapping to a new target overshoots or falls short a little
        float r = (rand() % 2001) / 1000.0f - 1.0f;
        st.flickYaw = (r >= 0 ? 0.5f + r * 0.5f : -0.5f + r * 0.5f) * skill.flick;
        st.flickPitch = ((rand() % 2001) / 1000.0f - 1.0f) * skill.flick * 0.4f;
    }
    // the flick error fades out as the bot settles on the target
    float fade = expf(-dt / (skill.settle > 0.05f ? skill.settle : 0.05f));
    st.flickYaw *= fade;
    st.flickPitch *= fade;
    if (st.target)
    {
        lookYaw += st.errYaw + st.flickYaw;
        lookPitch += st.errPitch + st.flickPitch;
    }
    float step = skill.turn * dt;
    if (st.target)
    {
        float dist = Len(AimPoint(st.target) - eye);
        step *= dist < 120.0f ? 4.0f : dist < 300.0f ? 2.5f : 1.6f;
    }
    st.yaw = Approach(st.yaw, lookYaw, step);
    st.pitch = Approach(st.pitch, lookPitch, step);
    // shots leave along view + punchangle (recoil): the bot compensates part of the recoil (like a player
    // pulling the mouse down) and its hand wanders a little after each shot
    V3 punch = Vec((float*)(v + EV_PUNCHANGLE));
    cmd.yaw = st.yaw - punch.y * skill.recoil + st.shotYaw;
    cmd.pitch = st.pitch - punch.x * skill.recoil + st.shotPitch;

    // ---- fire / reload: only when the crosshair is really on the target
    cmd.buttons &= ~IN_ATTACK;
    float cone = st.target ? HitCone(st.target, Len(AimPoint(st.target) - eye)) : 0;
    bool onTarget = false;
    if (st.target && now - st.targetSince >= skill.reaction && fabsf(AngleDiff(lookYaw, st.yaw)) < cone * 2.0f &&
        fabsf(AngleDiff(lookPitch, st.pitch)) < cone * 2.0f)
    {
        // where the bot means to shoot: fire only when that line meets a monster first (never a wall or a
        // teammate); the spread and the uncompensated recoil still make some shots miss, like a player's
        TraceResult tr;
        V3 shot = eye + Forward(st.pitch, st.yaw) * (Len(AimPoint(st.target) - eye) + 96.0f);
        Trace(eye, shot, bot, 0, tr);
        onTarget = tr.pHit && (tr.pHit == st.target ||
                               (IsLivingMonster(tr.pHit) && !IsIgnored(ClassName(tr.pHit))));
        st.shots++;
        st.hits += onTarget;
    }
    // long range: bursts with short pauses to let the recoil settle
    float targetDist = st.target ? Len(AimPoint(st.target) - eye) : 0;
    if (onTarget && targetDist > 600.0f)
    {
        if (now >= st.burstPause && now >= st.burstEnd)
        {
            st.burstEnd = now + skill.burst * (0.7f + (rand() % 60) / 100.0f);
            st.burstPause = st.burstEnd + 0.2f + (rand() % 25) / 100.0f;
        }
        if (now >= st.burstEnd && now < st.burstPause)
            onTarget = false;
    }
    bool firing = false;
    bool hold = HoldsTrigger(bot, st, now);
    if (onTarget)
        st.lastOnTarget = now;
    // spraying: an automatic weapon keeps firing a moment after the line leaves the target (it moves, the
    // next one is next to it), unless a wall or a teammate is right in front
    bool keepFiring = false;
    if (!onTarget && hold && st.wasFiring && st.target && now - st.lastOnTarget < 0.35f)
    {
        TraceResult tr;
        float td = Len(AimPoint(st.target) - eye);
        Trace(eye, eye + Forward(st.pitch, st.yaw) * td, bot, 0, tr);
        keepFiring = tr.flFraction > 0.6f && !(tr.pHit && IsHuman(tr.pHit)) &&
                     !(tr.pHit && (F<int>(Vars(tr.pHit), EV_FLAGS) & FL_FAKECLIENT));
    }
    if (onTarget || keepFiring)
    {
        if (hold || ++st.fireTick % 3 != 0) // tapped weapons: two commands out of three
        {
            cmd.buttons |= IN_ATTACK;
            firing = true;
        }
        if (hold && firing)
        {
            // held for a while and the weapon never kicked: semi-automatic, tap it from now on
            float kick = fabsf(punch.x) + fabsf(punch.y);
            if (!st.wasFiring)
            {
                st.holdingSince = now;
                st.punchAtHold = 0;
            }
            st.punchAtHold = st.punchAtHold > kick ? st.punchAtHold : kick;
            const char* m = String(F<int>(v, EV_WEAPONMODEL));
            std::string model = m ? m : "";
            for (auto& ch : model)
                ch = (char)tolower(ch);
            auto it = cfg.weaponCategory.find(model);
            bool machineGun = it != cfg.weaponCategory.end() && it->second == 5; // spins up first
            if (!machineGun && now - st.holdingSince > 0.8f && st.punchAtHold < 0.05f && !model.empty())
            {
                st.tapModel = model;
                Log("bot %d: %s doesn't fire while held: tapping it", st.slot, model.c_str());
            }
        }
        st.fireTime += dt;
        st.lastFire = now;
        st.firedSinceReload = true;
        if (cfg.logAim && now >= st.nextAimLog && st.shots >= 20)
        {
            Log("aim bot %d: on target %d of %d aimed frames; target %.0f units, cone %.1f, punch (%.1f %.1f)", st.slot, st.hits, st.shots,
                Len(AimPoint(st.target) - eye), cone, punch.x, punch.y);
            st.shots = st.hits = 0;
            st.nextAimLog = now + 10.0f;
        }
    }
    // a new small error after each shot
    if (firing && !st.wasFiring || (firing && (st.fireTick % 4) == 0))
    {
        st.shotYaw = ((rand() % 2001) / 1000.0f - 1.0f) * skill.spread;
        st.shotPitch = ((rand() % 2001) / 1000.0f - 1.0f) * skill.spread * 0.7f;
    }
    else if (!firing)
    {
        st.shotYaw *= 0.8f;
        st.shotPitch *= 0.8f;
    }
    st.wasFiring = firing;
    if (onTarget)
        ;
    else if (st.firedSinceReload && now - st.lastFire > 1.5f && now >= st.nextReload)
    {
        cmd.buttons |= IN_RELOAD;
        st.firedSinceReload = false;
        st.nextReload = now + 3.0f;
    }

    // ---- movement relative to where we look
    if (moving)
    {
        float rel = (moveYaw - st.yaw) / RAD;
        cmd.forward = cosf(rel) * 250.0f;
        cmd.side = -sinf(rel) * 250.0f; // sidemove: positive is right
    }
    else
    {
        cmd.forward = cmd.side = 0;
    }
}

// ---------------------------------------------------------------- buying
static DWORD g_mpDelta;

// runs a console command as the bot, like CBot::ClientCommand does
static void FakeCommand(void* bot, const char* cmd, const char* a1 = nullptr)
{
    const char** args = (const char**)(MP_FAKECMD_ARGS + g_mpDelta);
    unsigned char* flag = (unsigned char*)(MP_FAKECMD_FLAG + g_mpDelta);
    args[0] = cmd;
    args[1] = a1;
    args[2] = nullptr;
    args[3] = nullptr;
    *flag = 1;
    g_ClientCommandTrampoline(bot);
    *flag = 0;
}

static void BuyAs(void* bot, int item)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "item_%d", item);
    FakeCommand(bot, cmd);
}

// The buy command (item_<id>, handler around 0x113f2310) first asks the game rules: g_pGameRules (0x121cac30)
// vfunc 0x2b8 (player, command), thiscall, returns bool. 1 allowed, 0 refused, -1 unknown.
static const DWORD MP_GAMERULES = 0x121cac30;
static void* PrivateOf(void* edict);
static int BuyGate(void* bot, int item, DWORD* fnOut)
{
    __try
    {
        void* rules = *(void**)(MP_GAMERULES + g_mpDelta);
        void* player = PrivateOf(bot);
        if (!rules || !player)
            return -1;
        typedef bool(__fastcall * CanBuyFn)(void* self, void* edx, void* player, const char* cmd);
        CanBuyFn fn = (CanBuyFn)(*(void***)rules)[0x2b8 / 4];
        *fnOut = (DWORD)fn - g_mpDelta;
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "item_%d", item);
        return fn(rules, nullptr, player, cmd) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static int GameMode();

// Zombie Giant doesn't sell the room's weapons to the fake players (the buy changes nothing; they keep the mode's
// AWP): the primary is given like the mode gives its soldiers theirs, GiveNamedItem (0x114bac10, thiscall
// player, "weapon_<resource>", 1), after dropping the one in hand
static const DWORD MP_GIVENAMEDITEM = 0x114bac10;
typedef void(__fastcall* GiveNamedItemFn)(void* player, void* edx, const char* name, int flag);

static bool GiveRoomPrimary(void* bot, int item)
{
    auto it = cfg.weaponModel.find(item);
    void* pl = PrivateOf(bot);
    if (it == cfg.weaponModel.end() || !pl || it->second.size() <= 13)
        return false;
    std::string cls = "weapon_" + it->second.substr(9, it->second.size() - 13); // models/p_<res>.mdl
    FakeCommand(bot, "slot1");
    const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    std::string held = m ? m : "";
    for (auto& ch : held)
        ch = (char)tolower(ch);
    auto cat = cfg.weaponCategory.find(held);
    if (cat != cfg.weaponCategory.end() && cat->second >= 2 && cat->second <= 5 && held != it->second)
        FakeCommand(bot, "cl_drop");
    ((GiveNamedItemFn)(MP_GIVENAMEDITEM + g_mpDelta))(pl, nullptr, cls.c_str(), 1);
    FakeCommand(bot, cls.c_str()); // "weapon_<res>" selects it, even when the old one couldn't be dropped
    const char* now = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    static int reports = 0;
    if (reports++ < 40)
        Log("'%s': %s given directly (was holding %s, now %s)", String(F<int>(Vars(bot), EV_NETNAME)), cls.c_str(),
            held.c_str(), now ? now : "");
    return true;
}

// CSNZ's drop command is "cl_drop" (it throws the weapon in hand), thrown behind the bot: the drop goes where the
// player faces, and thrown ahead the bot and the ones it fights walked onto it, picked it up and threw it again
static void DropBehind(void* bot)
{
    float* va = (float*)(Vars(bot) + EV_VANGLE);
    float* an = (float*)(Vars(bot) + EV_ANGLES);
    float saved[6] = { va[0], va[1], va[2], an[0], an[1], an[2] };
    va[0] = an[0] = 0;
    va[1] = an[1] = AngleDiff(va[1] + 180.0f, 0.0f);
    FakeCommand(bot, "cl_drop");
    memcpy(va, saved, sizeof(float) * 3);
    memcpy(an, saved + 3, sizeof(float) * 3);
}

// The room's weapon only fits a free slot: the modes that hand out a weapon at spawn (an AWP in some deathmatch
// rooms) kept it, and the bought one stayed on the ground. Before buying, the bot throws away what it carries in
// that slot (slot1: categories 2-5, slot2: 1); an empty slot selects nothing and nothing is thrown.
static void FreeSlot(void* bot, const char* slot, int minCat, int maxCat)
{
    FakeCommand(bot, slot);
    const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    std::string held = m ? m : "";
    for (auto& ch : held)
        ch = (char)tolower(ch);
    auto cat = cfg.weaponCategory.find(held);
    if (cat != cfg.weaponCategory.end() && cat->second >= minCat && cat->second <= maxCat)
        DropBehind(bot);
}

// a moment after spawning (after the bot's own CS purchases), buy a random weapon from the whole game
static void UpdateBuying(void* bot, BotState& st)
{
    float now = Now();
    bool alive = Alive(bot);
    // A bot that respawns in these modes may never run a frame while dead: a gap in its frames, or its health
    // back up (a respawn restores it), is a new life too
    float hp = F<float>(Vars(bot), EV_HEALTH);
    // a new round (the game respawns everyone with its default weapons, no dead frame, full health): the bot
    // jumps to a spawn point, farther than it can move in a frame
    V3 at = Origin(bot);
    bool teleported = st.buyingSeen > 0 && now - st.buyingSeen < 0.5f && Len(at - st.buyingOrigin) > 300.0f;
    bool respawned = alive && st.wasAlive && st.buyingSeen > 0 &&
                     (now - st.buyingSeen > 1.0f || now < st.buyingSeen || hp > st.buyingHealth + 30.0f || teleported);
    st.buyingSeen = now;
    st.buyingHealth = hp;
    st.buyingOrigin = at;
    st.newLife = alive && (!st.wasAlive || respawned);
    if (st.newLife)
    {
        // other modes: right away, while the bot is still in its spawn's buy zone
        st.buyAt = now + (CoopMode() ? 1.5f : 0.2f);
        st.buyTries = 0;
    }
    st.wasAlive = alive;
    if (!alive || !g_ClientCommandTrampoline)
        return;
    MatchBot* rb = RoomBotOf(bot, st);
    bool scenario = CoopMode();
    // room AI players buy their own loadout in every mode: chosen weapons, else random ones from their filter, else
    // (BuyAnyWeapon) any weapon of the game, not only Counter-Strike's
    bool chosen = rb && (rb->primary != 0 || rb->secondary != 0 || rb->melee != 0 || !rb->allowPrimary.empty() ||
                         !rb->allowSecondary.empty() || !rb->allowMelee.empty() ||
                         (cfg.buyAnyWeapon && !cfg.primaries.empty()));
    // a slot: the room's weapon, -1 for "None", else a random one from the bot's filter, else 0
    auto pick = [](int fixed, const std::vector<int>& allow) {
        return fixed != 0 ? fixed : allow.empty() ? 0 : allow[rand() % allow.size()];
    };
    // the same, falling back on any weapon of the slot (BuyAnyWeapon)
    auto pickAny = [&](int fixed, const std::vector<int>& allow, const std::vector<int>& any) {
        int id = pick(fixed, allow);
        return id != 0 || !cfg.buyAnyWeapon || any.empty() ? id : any[rand() % any.size()];
    };
    // "None": the bot drops its weapon of that slot (the game gives a pistol, some modes a primary, and bots pick
    // up what others drop). CSNZ's drop command is "cl_drop" (mp.dll 0x113ee0f4: game rules vfunc 0x11c), it throws
    // the weapon in hand, so the bot is switched to that slot now and then.
    if (rb && (rb->primary < 0 || rb->secondary < 0) && now >= st.nextStrip && st.strips < 200)
    {
        st.nextStrip = now + 0.6f;
        const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
        std::string held = m ? m : "";
        for (auto& ch : held)
            ch = (char)tolower(ch);
        auto cat = cfg.weaponCategory.find(held);
        int c = cat == cfg.weaponCategory.end() ? 0 : cat->second;
        // a weapon botassist doesn't know that came up when switching to a None slot is that slot's weapon
        if (c == 0 && st.probedSlot && !held.empty() && held != st.probedFrom && held.find("knife") == std::string::npos)
            c = st.probedSlot == 2 ? 1 : 4;
        st.probedSlot = 0;
        if ((rb->primary < 0 && c >= 2 && c <= 5) || (rb->secondary < 0 && c == 1))
        {
            DropBehind(bot);
            st.strips++;
            st.stripProbe = 0; // it may carry the other slot's one too
            static int reports = 0;
            if (reports++ < 40)
                Log("bot %d '%s' drops %s (its room slot is None)", st.slot, rb->name.c_str(), held.c_str());
        }
        else if (now >= st.nextProbe)
        {
            const char* probes[2];
            int n = 0;
            if (rb->primary < 0)
                probes[n++] = "slot1";
            if (rb->secondary < 0)
                probes[n++] = "slot2";
            if (st.stripProbe < n)
            {
                st.probedSlot = probes[st.stripProbe][4] - '0';
                st.probedFrom = held;
                FakeCommand(bot, probes[st.stripProbe++]); // nothing happens if it has none
            }
            else
            {
                st.stripProbe = 0;
                st.nextProbe = now + 4.0f;
            }
        }
    }
    if (!scenario && !chosen)
    {
        st.buyAt = 0; // other modes: the game's own buying, unless the room chose weapons for this bot
        return;
    }
    if (st.newLife)
    {
        st.loadoutBuys = 0;
        st.strips = 0;
        st.stripProbe = 0;
        st.nextProbe = 0;
    }
    st.wasAliveLoadout = alive;
    // other modes: the bot must hold the room's primary (or carry it: checked when a primary is in its hands)
    if (!scenario && rb && st.loadoutPrimary > 0 && !st.buyAt && st.nextLoadoutCheck && now >= st.nextLoadoutCheck)
    {
        st.nextLoadoutCheck = now + 2.5f;
        const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
        std::string held = m ? m : "";
        for (auto& ch : held)
            ch = (char)tolower(ch);
        auto want = cfg.weaponModel.find(st.loadoutPrimary);
        auto cat = cfg.weaponCategory.find(held);
        bool primaryInHand = cat != cfg.weaponCategory.end() && cat->second >= 2 && cat->second <= 5;
        bool ok = want == cfg.weaponModel.end() || held == want->second;
        if (!ok && primaryInHand && st.loadoutBuys < 6)
        {
            Log("bot %d '%s' holds %s instead of the room's %s: buying it again", st.slot, rb->name.c_str(), held.c_str(),
                want->second.c_str());
            st.buyAt = now;
        }
    }
    if (scenario && (!cfg.buyAnyWeapon || cfg.primaries.empty()) && !chosen)
        return;
    if (st.buyAt && now >= st.buyAt)
    {
        st.buyAt = 0;
        if (!scenario)
        {
            // the room's choices only; checked again in a moment
            st.buyTries = 99;
            st.nextLoadoutCheck = now + 1.0f;
            // a new life: new random weapons from the filter; buying again (the game's AI changed it): the same
            if (st.loadoutBuys == 0)
            {
                st.loadoutPrimary = pickAny(rb->primary, rb->allowPrimary, cfg.primaries);
                st.loadoutSecondary = pickAny(rb->secondary, rb->allowSecondary, cfg.pistols);
                st.loadoutMelee = pickAny(rb->melee, rb->allowMelee, cfg.melees);
            }
            st.loadoutBuys++;
            {
                int item = st.loadoutPrimary > 0 ? st.loadoutPrimary : st.loadoutSecondary > 0 ? st.loadoutSecondary
                                                                                                 : st.loadoutMelee;
                DWORD fn = 0;
                int gate = item > 0 ? BuyGate(bot, item, &fn) : -1;
                static int reports = 0;
                if (reports++ < 30)
                    Log("bot %d: the game rules (0x%08lx) %s item_%d", st.slot, fn,
                        gate == 1 ? "allow" : gate == 0 ? "REFUSE" : "can't tell about", item);
            }
            const char* before = String(F<int>(Vars(bot), EV_WEAPONMODEL));
            st.modelBeforeBuy = before ? before : "";
            st.boughtPrimary = st.loadoutPrimary;
            st.checkBuyAt = st.loadoutPrimary > 0 ? now + 0.6f : 0; // only a primary can be checked (in hand)
            // The buy handler (0x113f21a0) in these modes only records an item_<id> purchase for later (returns
            // "bought" and gives nothing) unless the player's internal-buy flag +0x18b4 is set: the game sets it
            // itself around its own item_<id> purchases (0x1073d0ae .. 0x1073d2bc). Same here.
            unsigned char* pl = (unsigned char*)PrivateOf(bot);
            unsigned char wasInternal = pl ? pl[0x18b4] : 0;
            if (pl)
                pl[0x18b4] = 1;
            // Zombie Giant starts everyone with little money (+0xca8, read by 0x114b76d0 against the price from
            // game rules vfunc 0x400): the room's weapons always cost too much there, so the bot gets enough
            if (pl && GameMode() == 30 && *(int*)(pl + 0xca8) < 16000)
                *(int*)(pl + 0xca8) = 16000;
            if (st.loadoutPrimary > 0)
                FreeSlot(bot, "slot1", 2, 5);
            if (st.loadoutSecondary > 0)
                FreeSlot(bot, "slot2", 1, 1);
            FakeCommand(bot, "client_buy_open");
            if (st.loadoutPrimary > 0)
                BuyAs(bot, st.loadoutPrimary);
            if (st.loadoutSecondary > 0)
                BuyAs(bot, st.loadoutSecondary);
            if (st.loadoutMelee > 0)
                BuyAs(bot, st.loadoutMelee);
            FakeCommand(bot, "primammo"); // full ammo for what it bought
            FakeCommand(bot, "primammo");
            FakeCommand(bot, "secammo");
            FakeCommand(bot, "client_buy_close");
            if (pl)
                pl[0x18b4] = wasInternal;
            {
                static int reports = 0;
                if (pl && reports++ < 30)
                    Log("bot %d spawned with character %d (class index %d, default flag %d)", st.slot,
                        *(int*)(pl + 0xcd4), *(unsigned short*)(pl + 0x2156), pl[0x2155]);
            }
            // in hand: what it bought; no primary: its pistol (a random one keeps the game's), else the knife
            const char* hand = st.loadoutPrimary > 0                                   ? "slot1"
                               : st.loadoutSecondary > 0                               ? "slot2"
                               : st.loadoutPrimary < 0 && st.loadoutSecondary == 0     ? "slot2"
                               : st.loadoutPrimary < 0 || st.loadoutMelee > 0          ? "slot3"
                                                                                       : nullptr;
            if (hand)
                FakeCommand(bot, hand);
            st.nextStrip = now + 0.5f;
            Log("bot %d buys the room's weapons: %d %d %d", st.slot, st.loadoutPrimary, st.loadoutSecondary, st.loadoutMelee);
            return;
        }
        int roomPrimary = rb && st.buyTries == 0 ? pick(rb->primary, rb->allowPrimary) : 0;
        if (rb && rb->primary < 0)
            roomPrimary = -1;
        else if (rb && !rb->allowPrimary.empty() && roomPrimary <= 0)
            roomPrimary = rb->allowPrimary[rand() % rb->allowPrimary.size()]; // retries stay within the filter
        st.boughtPrimary = roomPrimary > 0 ? roomPrimary : roomPrimary < 0 ? 0 : cfg.primaries[rand() % cfg.primaries.size()];
        const char* model = String(F<int>(Vars(bot), EV_WEAPONMODEL));
        st.modelBeforeBuy = model ? model : "";
        st.buyTries++;
        FakeCommand(bot, "client_buy_open");
        if (st.boughtPrimary > 0)
            BuyAs(bot, st.boughtPrimary);
        int roomSecondary = rb ? pick(rb->secondary, rb->allowSecondary) : 0;
        if (roomSecondary >= 0 && (roomSecondary > 0 || !cfg.pistols.empty()))
            BuyAs(bot, roomSecondary > 0 ? roomSecondary : cfg.pistols[rand() % cfg.pistols.size()]);
        int roomMelee = rb ? pickAny(rb->melee, rb->allowMelee, cfg.melees) : 0;
        if (roomMelee > 0)
            BuyAs(bot, roomMelee);
        FakeCommand(bot, "item_28"); // kevlar + helmet
        FakeCommand(bot, "primammo");
        FakeCommand(bot, "primammo");
        FakeCommand(bot, "secammo");
        FakeCommand(bot, "client_buy_close");
        st.fireTime = 0;
        FakeCommand(bot, "slot1");
        st.checkBuyAt = now + 0.5f;
    }
    if (st.checkBuyAt && now >= st.checkBuyAt)
    {
        st.checkBuyAt = 0;
        const char* model = String(F<int>(Vars(bot), EV_WEAPONMODEL));
        std::string held = model ? model : "";
        // nothing changed (the item isn't sold here, no money...): try another weapon a few times. In the other modes
        // the room's primary must be in hand (holding another one, e.g. the spawn's, is a failure too)
        auto want = cfg.weaponModel.find(st.boughtPrimary);
        bool wrong = want != cfg.weaponModel.end() && _stricmp(held.c_str(), want->second.c_str());
        bool failed = (held == st.modelBeforeBuy && (want == cfg.weaponModel.end() || wrong)) || (!scenario && wrong);
        static int reports = 0;
        if (reports++ < 40)
            Log("bot %d bought item %d, now holding %s%s", st.slot, st.boughtPrimary, held.c_str(),
                failed ? " (no change: trying another)" : "");
        if (failed && !scenario && GameMode() == 30 && st.boughtPrimary > 0 && GiveRoomPrimary(bot, st.boughtPrimary))
            st.nextLoadoutCheck = now + 1.0f;
        else if (failed && !scenario && st.loadoutBuys < 4)
            st.buyAt = now + 0.4f; // the same loadout again
        else if (failed && scenario && st.buyTries < 4)
            st.buyAt = now + 0.3f;
    }
}

// ---------------------------------------------------------------- voices / difficulty / characters / teams
static float g_nextCvarCheck = 0;
static void* PrivateOf(void* edict);

// Monster modes have no enemy team, but the bot manager still fills the "bot team" up to bot_max_bot_team
// (the room's enemy bots): those bots spawn under the map and the round never ends. Where there are monsters
// the quota of the other team is forced to 0, so the bot manager kicks them; stragglers are kicked by name.
// The quota is given back on the next map unless the room set it again.
static std::string g_teamsMap, g_savedMaxBotTeam, g_savedBalanceBotTeam;
static int g_wrongTeamChecks = 0;
static int g_humanTeam = 0;
static float g_nextKick = 0;

static int TeamOf(void* e)
{
    void* player = PrivateOf(e);
    return player ? *(int*)((unsigned char*)player + PLAYER_TEAM) : 0;
}

static void SetCvar(const char* name, const char* value)
{
    const char* cur = g_CVarGetString(name);
    if (cur && !_stricmp(cur, value))
        return;
    g_CVarSetString(name, value);
    Log("%s %s -> %s", name, cur ? cur : "?", value);
}

static bool CoopMode();

static void UpdateTeams(void* anyPlayer)
{
    std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
    if (map != g_teamsMap)
    {
        // new map: give the quota back if it is still the one we forced
        if (!g_savedMaxBotTeam.empty())
        {
            const char* cur = g_CVarGetString("bot_max_bot_team");
            if (cur && !strcmp(cur, "0"))
                SetCvar("bot_max_bot_team", g_savedMaxBotTeam.c_str());
            cur = g_CVarGetString("bot_auto_balance_bot_team");
            if (cur && !strcmp(cur, "0"))
                SetCvar("bot_auto_balance_bot_team", g_savedBalanceBotTeam.c_str());
        }
        g_savedMaxBotTeam.clear();
        g_savedBalanceBotTeam.clear();
        g_teamsMap = map;
        g_wrongTeamChecks = 0;
    }
    if (!cfg.sameTeam || !CoopMode())
        return;
    if (g_savedMaxBotTeam.empty())
    {
        const char* a = g_CVarGetString("bot_max_bot_team");
        const char* b = g_CVarGetString("bot_auto_balance_bot_team");
        g_savedMaxBotTeam = a && *a ? a : "0";
        g_savedBalanceBotTeam = b && *b ? b : "0";
    }
    SetCvar("bot_auto_balance_bot_team", "0");
    SetCvar("bot_max_bot_team", "0");
    SetCvar("bot_join_team", "CT");

    // the human team (the bots of the other team kill themselves as soon as they spawn, see HookedRunPlayerMove)
    int humanTeam = 0;
    std::vector<void*> wrong;
    ForEntitiesNear(Origin(anyPlayer), 16384.0f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS);
        if (!(flags & FL_CLIENT))
            return;
        int team = TeamOf(e);
        if (!(flags & FL_FAKECLIENT) && (team == 1 || team == 2))
            humanTeam = team;
        else if (flags & FL_FAKECLIENT)
            wrong.push_back(e);
    });
    g_humanTeam = humanTeam;
    if (!humanTeam)
        return;
    wrong.erase(std::remove_if(wrong.begin(), wrong.end(), [&](void* e) {
        int team = TeamOf(e);
        return team == humanTeam || (team != 1 && team != 2);
    }), wrong.end());
    if (wrong.empty())
    {
        g_wrongTeamChecks = 0;
        return;
    }
    ++g_wrongTeamChecks;
}

// a bot of the other team where there are monsters: it spawned under the map (no spawn points for that team)
// and keeps the round from ending. Kicking doesn't help (the bot manager adds another one), so it dies.
typedef int(__fastcall* RoomIntFn)(void* self, void* edx);
static int g_gameMode = -1;
static float g_nextModeCheck = 0;

static DWORD SafeGameMode(int* out)
{
    __try
    {
        void* room = g_GetRoom ? g_GetRoom() : nullptr;
        *out = room ? (*(RoomIntFn**)room)[0x188 / 4](room, nullptr) : -1;
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }
}

static int GameMode()
{
    float now = Now();
    if (now >= g_nextModeCheck || now < g_nextModeCheck - 10.0f)
    {
        g_nextModeCheck = now + 3.0f;
        int mode = -1;
        if (SafeGameMode(&mode))
            mode = -1;
        if (mode != g_gameMode)
            Log("game mode %d%s", mode, mode >= 0 ? "" : " (unknown)");
        g_gameMode = mode;
    }
    return g_gameMode;
}

static bool CoopMode()
{
    int mode = GameMode();
    for (int m : cfg.coopModes)
        if (m == mode)
            return true;
    return false;
}

// Scenario maps only have CT spawns (the T ones are outside the map). A bot the game puts on T in a co-op
// mode switches to CT ("jointeam 2" + a CT character), so the bot count doesn't change and the bot manager
// doesn't add another one.
static void FixCoopTeam(void* bot, BotState& st)
{
    float now = Now();
    if (now < st.nextTeamFix || TeamOf(bot) != 1)
        return;
    st.nextTeamFix = now + 2.0f;
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    if (st.teamFixTries++ < 3)
    {
        Log("bot '%s' is a terrorist in a co-op mode: switching to CT", name ? name : "?");
        FakeCommand(bot, "jointeam", "2");
        char cls[16] = "1";
        if (!cfg.characters[2].empty())
            snprintf(cls, sizeof(cls), "%d", cfg.characters[2][rand() % cfg.characters[2].size()]);
        FakeCommand(bot, "joinclass", cls);
        return;
    }
    // no kicks: the game added another bot at once, so bots kept joining and leaving; new terrorists are
    // refused at the source instead (HookedAddBot)
}

static void RemoveWrongTeamBot(void* bot, BotState& st)
{
    float now = Now();
    if (!g_humanTeam || !Alive(bot) || now < st.nextKill)
        return;
    int team = TeamOf(bot);
    if (team == g_humanTeam || (team != 1 && team != 2))
        return;
    st.nextKill = now + 1.0f;
    st.killTries++;
    const char* name = String(F<int>(Vars(bot), EV_NETNAME));
    static int reports = 0;
    if (reports++ < 20)
        Log("bot '%s' is on team %d (humans play team %d): kill", name ? name : "?", team, g_humanTeam);
    FakeCommand(bot, "kill");
    // "kill" refused (some modes): kick as a last resort, at most every 10 s
    if (st.killTries > 5 && name && *name && !strchr(name, '"') && now >= g_nextKick)
    {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "kick \"%s\"\n", name);
        g_ServerCommand(cmd);
        g_nextKick = now + 10.0f;
        Log("kicked bot '%s' (kill didn't work)", name);
    }
}

// a buy zone around the bot: a func_buyzone it stands in, or a spawn point close by (CS's fallback zones)
static bool InBuyZone(void* bot)
{
    V3 o = Origin(bot);
    bool in = false;
    ForEntitiesNear(o, 400.0f, [&](void* e) {
        if (in)
            return;
        const char* cls = ClassName(e);
        if (!cls)
            return;
        unsigned char* v = Vars(e);
        if (!strcmp(cls, "func_buyzone"))
        {
            V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
            in = o.x >= lo.x - 16 && o.x <= hi.x + 16 && o.y >= lo.y - 16 && o.y <= hi.y + 16 && o.z >= lo.z - 40 &&
                 o.z <= hi.z + 40;
        }
        else if (!strcmp(cls, "info_player_start") || !strcmp(cls, "info_player_deathmatch"))
            in = Len(Origin(e) - o) < 250.0f;
    });
    return in;
}

// about half the ammo spent (estimated from the time spent firing) or little armor: refill in a buy zone
static void UpdateRefill(void* bot, BotState& st)
{
    float now = Now();
    if (now < st.nextRefill || st.target || !Alive(bot) || !g_ClientCommandTrampoline)
        return;
    st.nextRefill = now + 2.0f;
    bool lowAmmo = st.fireTime >= cfg.refillAfter;
    bool lowArmor = F<float>(Vars(bot), EV_ARMORVALUE) < cfg.minArmor;
    if ((!lowAmmo && !lowArmor) || !InBuyZone(bot))
        return;
    FakeCommand(bot, "client_buy_open");
    if (lowAmmo)
    {
        for (int i = 0; i < 4; i++)
            FakeCommand(bot, "primammo");
        FakeCommand(bot, "secammo");
        FakeCommand(bot, "secammo");
        st.fireTime = 0;
    }
    if (lowArmor)
        FakeCommand(bot, "item_28"); // kevlar + helmet
    FakeCommand(bot, "client_buy_close");
    st.nextRefill = now + 8.0f;
    static int reports = 0;
    if (reports++ < 30)
        Log("bot %d refills in a buy zone:%s%s (armor now %.0f)", st.slot, lowAmmo ? " ammo" : "", lowArmor ? " armor" : "",
            F<float>(Vars(bot), EV_ARMORVALUE));
}

static void UpdateCvars(void* anyPlayer)
{
    float now = Now();
    if (now < g_nextCvarCheck && now >= g_nextCvarCheck - 10.0f)
        return;
    g_nextCvarCheck = now + 3.0f;
    float difficulty = g_CVarGetFloat("bot_difficulty");
    if (difficulty != g_difficulty)
        Log("bot_difficulty %.0f", difficulty);
    g_difficulty = difficulty;
    const char* want = cfg.voices == "off" ? "off" : cfg.voices == "normal" ? "normal" : "radio";
    const char* cur = g_CVarGetString("bot_chatter");
    if (!cur || _stricmp(cur, want))
    {
        g_CVarSetString("bot_chatter", want);
        Log("bot_chatter %s -> %s", cur ? cur : "?", want);
    }
    UpdateTeams(anyPlayer);
}

// the CBaseEntity behind an edict: pvPrivateData, whose pev (+8) is the edict's entvars
static void* PrivateOf(void* edict)
{
    if (g_edictToPrivate < 0)
    {
        for (int k = 0x60; k < g_edictToVars; k += 4)
        {
            void* p = *(void**)((unsigned char*)edict + k);
            if (p && !IsBadReadPtr(p, 12) && *(void**)((unsigned char*)p + 8) == Vars(edict))
            {
                g_edictToPrivate = k;
                Log("edict_t -> private data offset: 0x%x", k);
                break;
            }
        }
        if (g_edictToPrivate < 0)
            return nullptr;
    }
    return *(void**)((unsigned char*)edict + g_edictToPrivate);
}

static char g_classArg[16];

// a bot choosing its character ("joinclass <class item id>"): give it any character of its team instead
// HandleMenu_ChooseAppearance / joinclass take the class's InGameID (Item.csv), not its item id: an item id there
// falls back to the team's default character
static int CharacterClassNumber(int id)
{
    auto it = cfg.characterClass.find(id);
    return it == cfg.characterClass.end() ? 0 : it->second;
}

static bool CharacterFits(int id, int team)
{
    auto it = cfg.characterTeam.find(id);
    return it == cfg.characterTeam.end() || it->second == team;
}

// a character's sex, as the game knows it (female voices and sounds): ChooseAppearance asks the class table
// ([0x121d3ec4] + 0x1f8, vfunc 0x50(class number), thiscall) whether the class is male. 1 male, 0 female, -1 unknown.
static const DWORD MP_CLASS_OWNER = 0x121d3ec4;
typedef bool(__fastcall* ClassIsMaleFn)(void* table, void* edx, int classNumber);
static int ClassSex(int number)
{
    __try
    {
        void* owner = *(void**)(MP_CLASS_OWNER + g_mpDelta);
        void* table = owner ? *(void**)((unsigned char*)owner + 0x1f8) : nullptr;
        if (!table)
            return -1;
        ClassIsMaleFn isMale = (ClassIsMaleFn)(*(void***)table)[0x50 / 4];
        return isMale(table, nullptr, number) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static int CharacterSex(int id)
{
    static std::unordered_map<int, int> known;
    auto it = known.find(id);
    if (it != known.end())
        return it->second;
    int number = CharacterClassNumber(id);
    int sex = number && g_mpDelta ? ClassSex(number) : -1;
    if (sex >= 0)
        known[id] = sex;
    return sex;
}

// a random character from the pool, as many men as women: each team's next bot gets the sex its team has fewer of
// in this match (a tie: either), then any character of that sex
static int g_sexLevel = -1;
static int g_sexPicks[3][2]; // [team][female, male]
static int PickCharacter(const std::vector<int>& pool, int team)
{
    std::vector<int> bySex[2];
    for (int id : pool)
    {
        int sex = CharacterSex(id);
        if (sex >= 0)
            bySex[sex].push_back(id);
    }
    if (bySex[0].empty() || bySex[1].empty())
        return pool[rand() % pool.size()];
    if (g_sexLevel != g_levels)
    {
        g_sexLevel = g_levels;
        memset(g_sexPicks, 0, sizeof(g_sexPicks));
        static int reports = 0;
        if (reports++ < 3)
            Log("characters for team %d: %d men, %d women (picked half and half)", team, (int)bySex[1].size(),
                (int)bySex[0].size());
    }
    int* picks = g_sexPicks[team == 1 ? 1 : 2];
    int sex = picks[0] < picks[1] ? 0 : picks[1] < picks[0] ? 1 : rand() % 2;
    picks[sex]++;
    return bySex[sex][rand() % bySex[sex].size()];
}

// the game's bots buy with Counter-Strike's aliases
static int BuyAliasSlot(const char* cmd)
{
    static const char* const primaries[] = { "m3", "xm1014", "tmp", "mac10", "mp5", "mp5navy", "ump45", "p90", "famas",
                                             "galil", "ak47", "m4a1", "sg552", "aug", "scout", "awp", "g3sg1", "sg550",
                                             "m249", "defender", "clarion", "krieg552", "bullpup", "smg", "c90", "d3au1",
                                             "krieg550", "magnum", "cv47", "mp", "autoshotgun", "12gauge", "mac" };
    static const char* const pistols[] = { "glock", "glock18", "usp", "p228", "deagle", "elites", "elite", "fn57",
                                           "fiveseven", "km45", "9x19mm", "228compact", "nighthawk" };
    for (auto a : primaries)
        if (!_stricmp(cmd, a))
            return 1;
    for (auto a : pistols)
        if (!_stricmp(cmd, a))
            return 2;
    return 0;
}

// the character a bot joins as: its room AI player's (if of its team); a room AI player on Random gets one of its
// filter's characters of its team, or any character of its team when the filter has none; other bots get a random
// one in co-op modes, else 0. Returns the class number the game takes (InGameID).
static int BotCharacter(void* bot, int team)
{
    LoadNav();
    UpdateMatchBots();
    MatchBot* rb = RoomBotOf(bot, StateFor(bot));
    int id = 0;
    if (rb && rb->character > 0 && CharacterFits(rb->character, team))
        id = rb->character;
    else
    {
        if (rb && rb->character > 0)
            Log("bot '%s': character %d is of the other team (bot on team %d): not used", rb->name.c_str(),
                rb->character, team);
        if (team != 1 && team != 2)
            return 0;
        std::vector<int> pool;
        if (rb)
            for (int c : rb->allowCharacter)
                if (CharacterFits(c, team) && CharacterClassNumber(c))
                    pool.push_back(c);
        if (pool.empty() && cfg.randomCharacters && (rb || CoopMode()))
            pool = cfg.characters[team];
        if (pool.empty())
            return 0;
        id = PickCharacter(pool, team);
    }
    StateFor(bot).appliedClass = id;
    int number = CharacterClassNumber(id);
    static int reports = 0;
    if (reports++ < 40)
        Log("bot '%s' joins as character %d (class number %d, team %d)", rb ? rb->name.c_str() : "?", id, number,
            team);
    return number; // 0: unknown here, the game's own choice stays
}

static bool IsZgFakeEdict(void* e);

// returns true to drop the command
static bool OnBotCommand(void* bot)
{
    const char** args = (const char**)(MP_FAKECMD_ARGS + g_mpDelta);
    unsigned char* flag = (unsigned char*)(MP_FAKECMD_FLAG + g_mpDelta);
    if (!*flag || !args[0])
        return false;
    // what the game makes a Zombie Giant fake player run
    static const char* const equipment[] = { "buy_spawn", "vesthelm", "vest", "primammo", "secammo", "defuser",
                                             "hegren", "flash", "sgren", "nvgs", "slot1", "slot2", "cl_drop" };
    bool routine = false;
    for (const char* q : equipment)
        routine |= !_stricmp(args[0], q);
    if (IsZgFakeEdict(bot) && !routine && BuyAliasSlot(args[0]) == 0 && _strnicmp(args[0], "item_", 5) &&
        _strnicmp(args[0], "weapon_", 7))
    {
        static int reports = 0;
        if (reports++ < 150)
            Log("fake player '%s' runs: %s %s", String(F<int>(Vars(bot), EV_NETNAME)), args[0], args[1] ? args[1] : "");
    }
    // what the game's bots send (to learn their buying commands): each command once
    static std::vector<std::string> seen;
    if (seen.size() < 80 && std::find(seen.begin(), seen.end(), std::string(args[0])) == seen.end())
    {
        seen.push_back(args[0]);
        Log("bot command: %s %s", args[0], args[1] ? args[1] : "");
    }
    if (int slot = BuyAliasSlot(args[0]))
    {
        // a room AI player with weapons of its own (chosen, a filter or None) doesn't buy the game's ones
        MatchBot* rb = RoomBotOf(bot, StateFor(bot));
        bool own = rb && (slot == 1 ? rb->primary != 0 || !rb->allowPrimary.empty()
                                    : rb->secondary != 0 || !rb->allowSecondary.empty());
        if (own)
        {
            static int reports = 0;
            if (reports++ < 20)
                Log("bot '%s' wanted to buy '%s': not its room loadout, dropped", rb->name.c_str(), args[0]);
            return true;
        }
        return false;
    }
    if (_stricmp(args[0], "joinclass"))
        return false;
    void* player = PrivateOf(bot);
    int id = BotCharacter(bot, player ? *(int*)((unsigned char*)player + PLAYER_TEAM) : 0);
    if (id <= 0)
        return false;
    snprintf(g_classArg, sizeof(g_classArg), "%d", id);
    args[1] = g_classArg;
    return false;
}

static const char* Str(int s);

// ---------------------------------------------------------------- zombies against barricades
// Zombie scenario barricades (CZBSBreak, "zbsbreak") scale the damage monsters do by their m_monsterRatio
// (+0xe4, set by the map) minus 10% per upgrade level; offline they never lost any health, so zombies stuck
// at one never got through. Monsters pressed against a barricade damage it here, through
// CBreakable::TakeDamage (0x1147b850, the base class: no ratio), so it breaks and fires its targets like normal.
// If the game damages the barricade itself, botassist leaves it alone.
static const DWORD MP_BREAKABLE_TAKEDAMAGE = 0x1147b850;
typedef int(__fastcall* TakeDamageFn)(void* self, void* edx, void* inflictor, void* attacker, float damage, int bits,
                                      int extra);
struct BarricadeState
{
    float lastHealth = -1;
    bool gameDamages = false;
};
static std::unordered_map<void*, BarricadeState> g_barricades;
static int g_barricadeLevel = -1;
static float g_nextBarricadeCheck = 0;

static bool IsBarricadeClass(const char* cls)
{
    if (!cls || !*cls)
        return false;
    size_t n = strlen(cls);
    const std::string& list = cfg.barricades;
    for (size_t i = 0; i <= list.size();)
    {
        size_t j = list.find(',', i);
        if (j == std::string::npos)
            j = list.size();
        std::string item = list.substr(i, j - i);
        while (!item.empty() && item[0] == ' ')
            item.erase(0, 1);
        while (!item.empty() && item.back() == ' ')
            item.pop_back();
        if (item.size() == n && !_stricmp(item.c_str(), cls))
            return true;
        i = j + 1;
    }
    return false;
}

// no C++ objects here: __try needs a plain function
static DWORD SafeTakeDamage(TakeDamageFn fn, void* self, void* attacker, float damage)
{
    __try
    {
        fn(self, nullptr, attacker, attacker, damage, 4 /* DMG_SLASH */, 0);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return GetExceptionCode();
    }
}

static void UpdateBarricades(void* anyBot)
{
    float now = Now();
    if (cfg.barricadeDamage <= 0 || (now < g_nextBarricadeCheck && now >= g_nextBarricadeCheck - 5.0f))
        return;
    const float interval = 0.5f;
    g_nextBarricadeCheck = now + interval;
    if (g_barricadeLevel != g_nav.level)
    {
        g_barricades.clear();
        g_barricadeLevel = g_nav.level;
    }
    std::vector<void*> list;
    ForEntitiesNear(Origin(anyBot), 16384.0f, [&](void* e) {
        unsigned char* v = Vars(e);
        if (F<float>(v, EV_HEALTH) > 0 && F<float>(v, EV_TAKEDAMAGE) != 0.0f && IsBarricadeClass(ClassName(e)))
            list.push_back(e);
    });
    TakeDamageFn takeDamage = (TakeDamageFn)(MP_BREAKABLE_TAKEDAMAGE + g_mpDelta);
    for (void* b : list)
    {
        unsigned char* bv = Vars(b);
        BarricadeState& bs = g_barricades[b];
        float hp = F<float>(bv, EV_HEALTH);
        if (bs.lastHealth >= 0 && hp < bs.lastHealth - 0.5f && !bs.gameDamages)
        {
            bs.gameDamages = true; // something else (the game) damages it: nothing to fix here
            Log("barricade '%s' takes damage from the game: botassist leaves it alone", Str(F<int>(bv, 0x1fc)));
        }
        // only what zombies may break (map keyvalue zbdmgratio > 0), scaled by that ratio
        float playerRatio = 0, monsterRatio = 0;
        if (bs.gameDamages || !BreakRatios(b, &playerRatio, &monsterRatio) || monsterRatio <= 0.0f)
            continue;
        V3 lo = Vec((float*)(bv + EV_ABSMIN)), hi = Vec((float*)(bv + EV_ABSMAX));
        V3 c = (lo + hi) * 0.5f;
        float reach = Len(hi - lo) * 0.5f + 64.0f;
        void* attacker = nullptr;
        int zombies = 0;
        ForEntitiesNear(c, reach, [&](void* m) {
            if (!IsLivingMonster(m) || IsIgnored(ClassName(m)))
                return;
            unsigned char* mv = Vars(m);
            V3 mlo = Vec((float*)(mv + EV_ABSMIN)), mhi = Vec((float*)(mv + EV_ABSMAX));
            const float t = 24.0f; // touching, give or take
            if (mlo.x > hi.x + t || mhi.x < lo.x - t || mlo.y > hi.y + t || mhi.y < lo.y - t || mlo.z > hi.z + t ||
                mhi.z < lo.z - t)
                return;
            zombies++;
            attacker = m;
        });
        if (!zombies)
        {
            bs.lastHealth = hp;
            continue;
        }
        float damage = cfg.barricadeDamage * interval * zombies * monsterRatio;
        void* self = PrivateOf(b);
        if (!self)
            continue;
        DWORD code = SafeTakeDamage(takeDamage, self, Vars(attacker), damage);
        if (code)
        {
            Log("barricade damage failed (exception 0x%08lX): turned off", code);
            cfg.barricadeDamage = 0;
            return;
        }
        bs.lastHealth = Alive(b) ? F<float>(bv, EV_HEALTH) : 0;
        static int reports = 0;
        if (reports++ < 40)
        {
            float pr = 0, mr = 0;
            BreakRatios(b, &pr, &mr);
            Log("barricade '%s' (player ratio %.2f, monster ratio %.2f): %d zombie(s) hit it, health %.0f -> %.0f",
                Str(F<int>(bv, 0x1fc)), pr, mr, zombies, hp, F<float>(bv, EV_HEALTH));
        }
    }
}

// ---------------------------------------------------------------- debug
static void DLog(const char* fmt, ...)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist_debug.log", g_dir);
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(path, "a");
    if (f)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static const char* Str(int s)
{
    const char* r = s ? String(s) : nullptr;
    return r && *r ? r : "-";
}

// one entity in a few words: class, flags, health, takedamage, team, solid/movetype, names, model
static std::string Describe(void* e)
{
    if (!e)
        return "nothing";
    unsigned char* v = Vars(e);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s [flags 0x%x hp %.0f/%.0f takedmg %.0f dead %.0f team %d solid %d move %d name '%s' target '%s' model '%s'%s%s]",
             Str(F<int>(v, EV_CLASSNAME)), F<int>(v, EV_FLAGS), F<float>(v, EV_HEALTH), F<float>(v, 0x1dc),
             F<float>(v, EV_TAKEDAMAGE), F<float>(v, EV_DEADFLAG), F<int>(v, EV_TEAM), F<int>(v, 0x11c),
             F<int>(v, 0x118), Str(F<int>(v, 0x1fc)), Str(F<int>(v, 0x1f8)), Str(F<int>(v, 0xc8)),
             IsLivingMonster(e) ? " MONSTER" : "", IsIgnored(Str(F<int>(v, EV_CLASSNAME))) ? " IGNORED" : "");
    float pr, mr;
    if (!strcmp(Str(F<int>(v, EV_CLASSNAME)), "zbsbreak") && BreakRatios(e, &pr, &mr))
    {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, " (player ratio %.2f, monster ratio %.2f%s)", pr, mr,
                 IsObjective(e) ? ", OBJECTIVE" : "");
    }
    else if (IsObjective(e))
        strncat(buf, " (OBJECTIVE)", sizeof(buf) - strlen(buf) - 1);
    return buf;
}

// what a line from `eye` along (pitch, yaw) hits first
static std::string LookingAt(void* self, V3 eye, float pitch, float yaw, float* dist)
{
    TraceResult tr;
    V3 end = eye + Forward(pitch, yaw) * 4096.0f;
    Trace(eye, end, self, 0, tr);
    *dist = tr.flFraction * 4096.0f;
    if (tr.flFraction >= 1.0f)
        return "nothing (open space)";
    return Describe(tr.pHit);
}

// laser from a to b, visible to everyone (TE_BEAMPOINTS); life in tenths of a second
static void Beam(V3 a, V3 b, int r, int g, int bl, int life)
{
    short sprite = *(short*)(MP_BEAM_SPRITE + g_mpDelta);
    if (sprite <= 0 || !g_MessageBegin)
        return;
    g_MessageBegin(0 /* MSG_BROADCAST */, 23 /* SVC_TEMPENTITY */, nullptr, nullptr);
    g_WriteByte(0); // TE_BEAMPOINTS
    g_WriteCoord(a.x);
    g_WriteCoord(a.y);
    g_WriteCoord(a.z);
    g_WriteCoord(b.x);
    g_WriteCoord(b.y);
    g_WriteCoord(b.z);
    g_WriteShort(sprite);
    g_WriteByte(0);    // start frame
    g_WriteByte(10);   // frame rate
    g_WriteByte(life); // life
    g_WriteByte(8);    // width
    g_WriteByte(0);    // noise
    g_WriteByte(r);
    g_WriteByte(g);
    g_WriteByte(bl);
    g_WriteByte(220);  // brightness
    g_WriteByte(0);    // scroll speed
    g_MessageEnd();
}

// after the command ran: where the bot really looks (v_angle) and what that line hits
static void DebugBot(void* bot, BotState& st, const float* sent, unsigned short buttons)
{
    float now = Now();
    if (!cfg.debug || !Alive(bot))
        return;
    unsigned char* v = Vars(bot);
    V3 eye = Eye(bot);
    V3 va = Vec((float*)(v + EV_VANGLE)), punch = Vec((float*)(v + EV_PUNCHANGLE));
    if (cfg.debug >= 2 && now >= st.nextBeam)
    {
        st.nextBeam = now + 0.2f;
        TraceResult tr;
        V3 end = eye + Forward(va.x + punch.x, va.y + punch.y) * 4096.0f;
        Trace(eye, end, bot, 0, tr);
        V3 hit = eye + (end - eye) * tr.flFraction;
        bool firing = (buttons & IN_ATTACK) != 0;
        // red: firing, green: has a target, white: no target
        Beam(eye, hit, firing ? 255 : st.target ? 40 : 200, firing ? 40 : st.target ? 255 : 200, firing ? 40 : st.target ? 40 : 200, 2);
        if (st.target) // yellow: from the bot to the point it wants to hit
            Beam(eye, AimPoint(st.target), 255, 220, 0, 2);
    }
    if (now < st.nextDebug)
        return;
    st.nextDebug = now + 1.0f;
    float dView = 0, dSent = 0;
    std::string view = LookingAt(bot, eye, va.x + punch.x, va.y + punch.y, &dView);
    std::string want = LookingAt(bot, eye, sent[0] + punch.x, sent[1] + punch.y, &dSent);
    const char* name = String(F<int>(v, EV_NETNAME));
    DLog("bot %d '%s' team %d hp %.0f weapon %s | driven %d order %d | stock (%.1f %.1f) sent (%.1f %.1f) v_angle (%.1f %.1f) "
         "punch (%.1f %.1f) fixangle %d | buttons 0x%x%s",
         st.slot, name ? name : "?", TeamOf(bot), F<float>(v, EV_HEALTH), Str(F<int>(v, EV_WEAPONMODEL)), st.driven,
         (int)st.order, st.stockPitch, st.stockYaw, sent[0], sent[1], va.x, va.y, punch.x, punch.y, F<int>(v, 0xb0),
         buttons, (buttons & IN_ATTACK) ? " ATTACK" : "");
    DLog("    view line hits (%.0f u): %s", dView, view.c_str());
    if (fabsf(AngleDiff(va.y, sent[1])) > 0.5f || fabsf(va.x - sent[0]) > 0.5f)
        DLog("    sent line hits (%.0f u): %s", dSent, want.c_str());
    if (st.target)
        DLog("    target (%.0f u, visible %d): %s", Len(AimPoint(st.target) - eye), CanSee(bot, eye, st.target),
             Describe(st.target).c_str());
    else if (st.threat)
        DLog("    threat, not visible (%.0f u): %s", Len(Center(st.threat) - eye), Describe(st.threat).c_str());
}

// every few seconds: what the human looks at, and every entity that can take damage around them
static float g_nextCensus = 0;
static std::string g_lastHumanLook;

static void DebugCensus(void* anyBot)
{
    float now = Now();
    if (!cfg.debug)
        return;
    void* human = FindLeader(Origin(anyBot));
    if (!human)
        return;
    unsigned char* hv = Vars(human);
    V3 eye = Eye(human);
    V3 va = Vec((float*)(hv + EV_VANGLE));
    float d = 0;
    std::string look = LookingAt(human, eye, va.x, va.y, &d);
    if (look != g_lastHumanLook)
    {
        g_lastHumanLook = look;
        DLog("YOU look at (%.0f u): %s", d, look.c_str());
    }
    if (now < g_nextCensus && now >= g_nextCensus - 10.0f)
        return;
    g_nextCensus = now + 5.0f;
    struct Item
    {
        float dist;
        void* e;
    };
    std::vector<Item> items;
    ForEntitiesNear(eye, 1500.0f, [&](void* e) {
        if (e == human)
            return;
        unsigned char* v = Vars(e);
        const char* cls = Str(F<int>(v, EV_CLASSNAME));
        int flags = F<int>(v, EV_FLAGS);
        bool interesting = F<float>(v, EV_TAKEDAMAGE) != 0.0f || (flags & (FL_MONSTER | FL_CLIENT)) ||
                           strstr(cls, "break") || strstr(cls, "monster") || strstr(cls, "npc") || strstr(cls, "zombie");
        if (interesting)
            items.push_back({ Len(Center(e) - eye), e });
    });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
    DLog("CENSUS around you (%s, %d entities that can take damage within 1500 u):", g_nav.map.c_str(), (int)items.size());
    int n = 0;
    for (auto& it : items)
    {
        if (n++ >= 20)
            break;
        TraceResult tr;
        bool visible = Trace(eye, Center(it.e), human, 0, tr) || tr.pHit == it.e;
        DLog("    %5.0f u %s %s", it.dist, visible ? "VISIBLE" : "hidden ", Describe(it.e).c_str());
    }
}

// ---------------------------------------------------------------- bot manager
// CSOZBBotManager (and its voxel variant) add bots through vfunc 2, AddBot(team): 0 T, 1 CT, 2 by bot_join_team
// / balance ("bot_add_t" passes 0, "bot_add_ct" 1, "bot_add" 2). In the co-op modes the T spawns are outside the
// map: terrorists are refused, automatic ones join CT.
static const DWORD MP_ZBBOTMGR_VTABLES[] = { 0x11991964, 0x119400e8 };
static const DWORD MP_ZBBOTMGR_ADDBOT = 0x1155cc20;
typedef int(__fastcall* AddBotFn)(void* self, void* edx, int team);
static AddBotFn g_AddBot;

// The game fills rooms with bots on its own (bot quota per team: 4 + 5 by default, even with no AI players in
// the room). Bots are added only for the room's AI players: per team, as many as the room has. team: 0 T, 1 CT,
// 2 any; returns the team to add (0/1) or -1 to refuse.
static const int MODE_ZOMBIE_GIANT = 30; // Zombie Giant: see HookZombieGiant
static void MarkZgPlayers();
static int g_consoleBotAdd = 0; // inside "bot_add" / "bot_add_t" / "bot_add_ct" typed in the console

static int RoomBotQuota(int team, const char* which)
{
    if (!cfg.enabled)
        return team;
    if (g_consoleBotAdd)
    {
        Log("console bot_add (%s, team %d): added past the room's AI players", which, team);
        return team;
    }
    LoadNav();
    UpdateMatchBots();
    if (g_matchFetch != 2 || !g_matchKnown)
        return team; // server not reachable: the game decides
    MarkZgPlayers();
    int want[3] = { 0, 0, 0 };
    EnterCriticalSection(&g_matchLock);
    for (auto& m : g_matchBots)
        if (!m.fakePlayer)
            want[m.team == 1 ? 1 : 2]++;
    LeaveCriticalSection(&g_matchLock);
    if (CoopMode() || GameMode() == MODE_ZOMBIE_GIANT)
    {
        want[2] += want[1]; // co-op, Zombie Giant (the giant comes from the CT players): everyone on CT
        want[1] = 0;
    }
    if (CoopMode() && team == 0)
        team = 2; // Zombie Disaster Zone asks for its bots as terrorists: they join CT like the others
    if (GameMode() == MODE_ZOMBIE_GIANT)
        team = 1; // its soldier bots (asked for as T or CT) are the room's AI players
    // team index 0 (T) -> 1, 1 (CT) -> 2
    int t = team == 0 ? 1 : team == 1 ? 2 : (want[2] - g_matchAdded[2] >= want[1] - g_matchAdded[1] ? 2 : 1);
    if (g_matchAdded[t] >= want[t])
    {
        int other = t == 1 ? 2 : 1;
        if (team == 2 && g_matchAdded[other] < want[other])
            t = other;
        else
        {
            static int reports = 0, reportLevel = -1;
            if (reportLevel != g_nav.level)
                reportLevel = g_nav.level, reports = 0;
            if (reports++ < 10)
                Log("the game asked for a bot (%s, team %d) beyond the room's AI players (%d T, %d CT): refused", which,
                    team, want[1], want[2]);
            return -1;
        }
    }
    g_matchAdded[t]++;
    Log("the game adds a bot (%s): %s %d of %d", which, t == 1 ? "T" : "CT", g_matchAdded[t], want[t]);
    return t == 1 ? 0 : 1;
}

static int g_creatingBot = 0; // inside the game's AddBot

static int __fastcall HookedAddBot(void* self, void* edx, int team)
{
    team = RoomBotQuota(team, "new manager");
    if (team < 0)
        return 0;
    if (cfg.enabled && cfg.sameTeam && CoopMode())
    {
        static int reports = 0;
        if (team == 0)
        {
            if (reports++ < 10)
                Log("co-op mode: the game asked for a terrorist bot, refused");
            return 0;
        }
        if (team == 2)
            team = 1;
    }
    g_creatingBot++;
    int r = g_AddBot(self, edx, team);
    g_creatingBot--;
    return r;
}

static bool Readable(const void* p, size_t n);

static const DWORD MP_BOTADDCOMMAND = 0x11306ae0;
typedef int (*BotAddCommandFn)(int team, int fromConsole);
static BotAddCommandFn g_BotAddCommandTrampoline;

static int HookedBotAddCommand(int team, int fromConsole)
{
    team = RoomBotQuota(team, "old manager");
    if (team < 0)
        return 0;
    if (cfg.enabled && cfg.sameTeam && CoopMode())
    {
        static int reports = 0;
        if (team == 0)
        {
            if (reports++ < 10)
                Log("co-op mode: the game asked for a terrorist bot (old manager), refused");
            return 0;
        }
        if (team == 2)
            team = 1;
    }
    g_creatingBot++;
    int r = g_BotAddCommandTrampoline(team, fromConsole);
    g_creatingBot--;
    return r;
}

// push ebp; mov ebp, esp; push ecx; cmp byte ptr [abs32], 0 (11 bytes) run from a trampoline
static void DetourBotAddCommand(DWORD delta)
{
    unsigned char* fn = (unsigned char*)(MP_BOTADDCOMMAND + delta);
    if (!Readable(fn, 11) || fn[0] == 0xe9)
        return;
    static const unsigned char expect[] = { 0x55, 0x8b, 0xec, 0x51, 0x80, 0x3d };
    if (memcmp(fn, expect, sizeof(expect)) || fn[10] != 0x00)
    {
        Log("BotAddCommand has unexpected code: not hooked");
        return;
    }
    unsigned char* tramp = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return;
    memcpy(tramp, fn, 11);
    tramp[11] = 0xe9;
    *(int*)(tramp + 12) = (int)((fn + 11) - (tramp + 16));
    g_BotAddCommandTrampoline = (BotAddCommandFn)tramp;
    DWORD old;
    VirtualProtect(fn, 11, PAGE_EXECUTE_READWRITE, &old);
    unsigned char patch[11] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(int*)(patch + 1) = (int)((unsigned char*)HookedBotAddCommand - (fn + 5));
    memcpy(fn, patch, 11);
    VirtualProtect(fn, 11, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 11);
    Log("old bot manager hooked too");
}

// The game's bots never send "joinclass": the bot manager picks their character while creating them with a direct
// call to HandleMenu_ChooseAppearance(player, class) (0x113f2cb0, cdecl; old manager call site 0x11306a3f, class =
// the bot profile's skin). Humans only reach it through "joinclass" while the class menu is open (m_iMenu +0xe10
// == 3; otherwise the command is refused, so a joinclass sent to a bot in game does nothing). A bot being created
// gets its room AI player's character there. Other callers (zombie infection, mode changes) are left alone.
static const DWORD MP_CHOOSEAPPEARANCE = 0x113f2cb0, MP_CHOOSEAPPEARANCE_BOTSITE = 0x11306a44;
typedef int (*ChooseAppearanceFn)(void* player, int cls);
static ChooseAppearanceFn g_ChooseAppearanceTrampoline;

static int RoomAppearance(void* player, int cls, bool creating)
{
    __try
    {
        if (!cfg.enabled || !creating || !player)
            return cls;
        unsigned char* pev = *(unsigned char**)((unsigned char*)player + 8);
        if (!pev || !(F<int>(pev, EV_FLAGS) & FL_FAKECLIENT))
            return cls;
        void* edict = *(void**)(pev + EV_CONTAINING_ENTITY);
        if (!edict)
            return cls;
        if (g_edictToVars < 0)
        {
            g_edictToVars = (int)(pev - (unsigned char*)edict);
            Log("edict_t -> entvars_t offset: 0x%x (from a new bot)", g_edictToVars);
        }
        int id = BotCharacter(edict, *(int*)((unsigned char*)player + PLAYER_TEAM));
        return id > 0 ? id : cls;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return cls;
    }
}

static int HookedChooseAppearance(void* player, int cls)
{
    bool creating = g_creatingBot > 0 || (DWORD)_ReturnAddress() == MP_CHOOSEAPPEARANCE_BOTSITE + g_mpDelta;
    int asked = RoomAppearance(player, cls, creating);
    int r = g_ChooseAppearanceTrampoline(player, asked);
    if (creating && asked != cls && player)
    {
        // what the game made of it: +0xcd4 / +0x1fb8 the class item, +0x2156 the class index, +0x2155 set when it
        // fell back to the team's default character (an id the class manager didn't know)
        unsigned char* p = (unsigned char*)player;
        Log("bot appearance: profile skin %d -> asked %d; game stored %d, class index %d, default flag %d", cls, asked,
            *(int*)(p + 0xcd4), *(unsigned short*)(p + 0x2156), p[0x2155]);
    }
    return r;
}

// push ebp; mov ebp, esp; sub esp, 8 (6 bytes) run from a trampoline
static void DetourChooseAppearance(DWORD delta)
{
    unsigned char* fn = (unsigned char*)(MP_CHOOSEAPPEARANCE + delta);
    if (!Readable(fn, 6) || fn[0] == 0xe9)
        return;
    static const unsigned char expect[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x08 };
    if (memcmp(fn, expect, sizeof(expect)))
    {
        Log("HandleMenu_ChooseAppearance has unexpected code: room characters off");
        return;
    }
    unsigned char* tramp = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return;
    memcpy(tramp, fn, 6);
    tramp[6] = 0xe9;
    *(int*)(tramp + 7) = (int)((fn + 6) - (tramp + 11));
    g_ChooseAppearanceTrampoline = (ChooseAppearanceFn)tramp;
    DWORD old;
    VirtualProtect(fn, 6, PAGE_EXECUTE_READWRITE, &old);
    unsigned char patch[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
    *(int*)(patch + 1) = (int)((unsigned char*)HookedChooseAppearance - (fn + 5));
    memcpy(fn, patch, 6);
    VirtualProtect(fn, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 6);
    Log("bot characters hooked: new bots join as their room AI player's character");
}

// ---------------------------------------------------------------- Zombie Giant (mode 30) with room bots
// CSOGameRulesZombieBoss (vtable 0x117a7a4c) only counts humans: player vfunc 0x21c ("is a bot", 1 in the bot
// classes' 0x106626c0) makes 0x1091c670 skip bots, so a lone player ends the match (0x1085f660: one human left).
// The mode keeps bots of its own (br_bot soldiers, 3 per giant: 0x1085f850 adds them through the bot manager and
// kicks extra bots, 0x1131a210 picks the bot to kick). Room bots count as these soldiers: the mode asks for no more
// bots (any it asks for become room AI players, extra ones are refused) and never kicks them.
// Humans in this mode are ZombieBossPlayer objects (0x3778 bytes, giant state from +0x3710), bots are CCSBot
// (their own fields from +0x3700): any code that treats a bot as a human there reads and writes bot memory as
// giant state and crashes. So room bots can't be the giant; they answer "not a bot" only where the mode counts
// players (end of match) or picks the bot to kick, and play as the human side.
static const DWORD MP_BOT_VTABLES[] = { 0x1193f25c, 0x1193dcc8, 0x1198f294, 0x1179b8a4 }; // CBot, CCSBot, ZBBot, CPlayRoomBot
static const DWORD MP_ISBOT_TRUE = 0x106626c0;
static const DWORD MP_ZG_KICK_FN = 0x1131a210, MP_ZG_KICK_END = 0x1131a340;
static const DWORD MP_ZG_ISHUMAN_FN = 0x1091c670, MP_ZG_ISHUMAN_END = 0x1091c6b0;
static const DWORD MP_ZG_ENDCHECK_RET = 0x1085f6b8; // 0x1085f660 after its call to 0x1091c670
static const DWORD MP_ZG_RULES_VTABLE = 0x117a7a4c, MP_ZG_EQUIP_SLOT = 98; // 0x10860740: br_bot model + famas/m32
static const DWORD MP_DM_EQUIP = 0x10743240; // the parent's (CSOGameRulesDM) spawn equipment
static DWORD g_zgDelta;

static bool IsRoomBotEdict(void* edict)
{
    for (auto& b : g_bots)
        if (b.edict == edict)
        {
            if (b.roomBot < 0)
                return false;
            EnterCriticalSection(&g_matchLock);
            // a fake player plays its room AI player (weapons) but is a player: the mode's equipment (rules slot 98,
            // 0x10860740) is what turns it into the giant it was picked as (0x1091c9f0: type = next)
            bool ok = b.roomBot < (int)g_matchBots.size() && g_matchBots[b.roomBot].edict == edict &&
                      !g_matchBots[b.roomBot].fakePlayer;
            LeaveCriticalSection(&g_matchLock);
            return ok;
        }
    return false;
}

static bool IsRoomBotPlayer(unsigned char* self)
{
    unsigned char* pev = self ? *(unsigned char**)(self + 8) : nullptr;
    void* edict = pev ? *(void**)(pev + 0x238) : nullptr;
    return edict && IsRoomBotEdict(edict);
}

// mp.dll omits frame pointers: look for a return address in the stack above this call
static bool StackHasReturn(DWORD lo, DWORD hi, int depth)
{
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    DWORD* top = (DWORD*)((NT_TIB*)NtCurrentTeb())->StackBase;
    for (int i = 0; i < depth && sp + i < top; i++)
    {
        DWORD a = sp[i] - g_zgDelta;
        if (a >= lo && a < hi)
            return true;
    }
    return false;
}

static int __fastcall HookedIsBot(unsigned char* self, void*)
{
    if (g_gameMode != MODE_ZOMBIE_GIANT)
        return 1;
    DWORD ret = (DWORD)_ReturnAddress() - g_zgDelta;
    bool counting = (ret >= MP_ZG_KICK_FN && ret < MP_ZG_KICK_END)
        || (ret >= MP_ZG_ISHUMAN_FN && ret < MP_ZG_ISHUMAN_END && StackHasReturn(MP_ZG_ENDCHECK_RET, MP_ZG_ENDCHECK_RET + 1, 64));
    return counting && IsRoomBotPlayer(self) ? 0 : 1;
}

// room bots spawn with the normal equipment (their loadout is bought afterwards), not as br_bot soldiers
typedef void(__thiscall* EquipFn)(void* rules, void* player);
static EquipFn g_zgEquip, g_dmEquip;
static void __fastcall HookedZgEquip(void* rules, void*, unsigned char* player)
{
    if (IsRoomBotPlayer(player))
        g_dmEquip(rules, player);
    else
        g_zgEquip(rules, player);
}

static void PatchSlot(void** slot, void* fn)
{
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, 4, old, &old);
}

static void HookZombieGiant(DWORD delta)
{
    g_zgDelta = delta;
    int n = 0;
    for (DWORD vt : MP_BOT_VTABLES)
    {
        void** slot = (void**)(vt + delta) + 135;
        if (!Readable(slot, 4) || *slot != (void*)(MP_ISBOT_TRUE + delta))
            continue;
        PatchSlot(slot, (void*)HookedIsBot);
        n++;
    }
    void** equip = (void**)(MP_ZG_RULES_VTABLE + delta) + MP_ZG_EQUIP_SLOT;
    if (Readable(equip, 4) && *equip == (void*)(0x10860740 + delta))
    {
        g_zgEquip = (EquipFn)*equip;
        g_dmEquip = (EquipFn)(MP_DM_EQUIP + delta);
        PatchSlot(equip, (void*)HookedZgEquip);
    }
    Log("Zombie Giant: room bots count as players (%d bot classes hooked, equipment %s)", n, g_zgEquip ? "hooked" : "NOT hooked");
}

// ---------------------------------------------------------------- Zombie Giant: fake players who can be the giant
// Bots can't be the giant (see above), so the last ZombieGiantPlayers= of the room's AI players (botassist.ini) join
// as fake *players*: pfnCreateFakeClient, the game's player factory (DLL function 0x113ef1d0: a ZombieBossPlayer in
// this mode) and ClientPutInServer (0x113efbb0), like the bot manager does for its bots but without a bot class.
// The mode counts them as humans, so the giant rotates among you and them (0x10861d50). Nothing runs them, so
// botassist does, once per frame from the bots' RunPlayerMove: as soldiers they keep their distance from the giant
// and shoot it; as the giant they walk to the closest human on the navigation mesh, attack ("BossAttack1/2") and now
// and then use a random skill ("BossSkill_1".."4"): the giant's commands (rules ClientCommand 0x10861490 ->
// 0x10923b60 -> the pawn's handler 0x1090cb50, which checks the stamina and the cool down).
static const DWORD MP_CREATE_PLAYER = 0x113ef1d0, MP_PUT_IN_SERVER = 0x113efbb0;
// "jointeam" only works while the team menu is open: like the bot manager (0x113069d1), set the team, the menu
// state (+0xe10 1, +0xcc0 4) and call HandleMenu_ChooseTeam(player, team) and HandleMenu_ChooseAppearance
static const DWORD MP_CHOOSETEAM = 0x113f2f00;
typedef int (*ChooseTeamFn)(void* player, int team);
static const DWORD MP_ZOMBIEBOSSPLAYER_VT = 0x11958834;
static const int ENG_CREATEFAKECLIENT = 108, ENG_FREEENTPRIVATEDATA = 70;
static const int ZG_GIANT_TYPE = 0x3710; // ZombieBossPlayer: 1, 2, 4, 6 giants; 0, 3, 5 humans
static const int IN_ATTACK2 = 0x800, IN_USE = 0x20, IN_FORWARD = 0x8, IN_BACK = 0x10, IN_MOVELEFT = 0x200, IN_MOVERIGHT = 0x400;
static const int PLAYER_LAST_MOVEMENT = 0xccc;
// Joining players get maxspeed 1 (ClientPutInServer, like Counter-Strike's freeze) until the game gives them their
// real speed; in this mode the bots and fake players never got it (speed 0..1 while walking). pev->maxspeed is at
// +0x240 (entvars maxspeed, CSO layout); engine function 107 is pfnSetClientMaxspeed(edict, speed).
static const int EV_MAXSPEED = 0x240, ENG_SETCLIENTMAXSPEED = 107;
static const int EV_SOLID = 0x11c; // entvars solid (CSO layout: the standard one + 0x10 up to here)
typedef void (*SetClientMaxspeedFn)(void* edict, float speed);
static SetClientMaxspeedFn g_SetClientMaxspeed;
static float g_zgYourMaxspeed = 0; // yours: once you can move, the freeze is over // idle kick (mp_autokick, 0x114c6b7b) after the rules' idle time

struct ZgPlayer
{
    std::string name;
    int character = 0;
    void* edict = nullptr;
    bool tried = false, wasAlive = false;
    void* target = nullptr;
    std::vector<V3> path;
    size_t wp = 0;
    float yaw = 0, pitch = 0;
    float buyAt = 0, deadSince = 0, nextRespawn = 0, nextTarget = 0, nextPath = 0, nextAttack = 0, nextSkill = 0;
    float nextStrafe = 0, nextFire = 0, sprintUntil = 0, nextSprint = 0, stuckCheck = 0;
    float nextReport = 0, lastThink = 0, lastFwd = 0, lastSide = 0, nextPickReport = 0;
    float nextRanged = 0, aimUntil = 0, nextBreakScan = 0, breakUntil = 0, nextUse = 0, slowSince = 0;
    float busySince = 0, climbUntil = 0;
    int kitType = 0, pending = -1, ladder = -1; // the giant's kit, an aimed move waiting to face the target
    bool pendingSkill = false, simpleGiant = false;
    void* ignoreBreak = nullptr; // a breakable it couldn't break (for a while)
    void* avoidTarget = nullptr; // a target it couldn't reach (for a while)
    float ignoreBreakUntil = 0, avoidUntil = 0, detourUntil = 0, detourYaw = 0, stuckRoundsSince = 0;
    int stuckRounds = 0;
    float nextReload = 0, stockAt = 0, nextSlot = 0;
    int stockId = 0, stockBefore = 0, emptyStage = 0;
    std::vector<int> stocked;             // weapons whose ammunition this life already got
    std::unordered_map<int, int> maxClip; // the fullest clip seen per weapon id
    void* breakTarget = nullptr; // a zg_breakable / func_breakable in the giant's way
    int stuckCount = 0;
    bool lastMove = false;
    V3 stuckFrom{};
    int strafe = 1;
};
static std::vector<ZgPlayer> g_zgPlayers;
static std::vector<std::string> g_zgKickNames; // fake players botassist kicks itself

// everything Zombie Giant only acts in that mode, or while its fake players are still connected (they leave on the
// next map): the other modes' bots never see it
static bool ZgActive() { return g_gameMode == MODE_ZOMBIE_GIANT || !g_zgPlayers.empty(); }
static int g_zgLevel = -1, g_zgMarkLevel = -1;
static float g_zgFrame = -1, g_zgStart = 0;

static float Rnd() { return (float)rand() / RAND_MAX; }

// the last room AI players of the list are played by fake players (once per match, before bots take them)
static void MarkZgPlayers()
{
    if (cfg.zgPlayers == 0 || !g_CreateFakeClient || GameMode() != MODE_ZOMBIE_GIANT || g_matchFetch != 2 ||
        !g_matchKnown || g_zgMarkLevel == g_nav.level)
        return;
    g_zgMarkLevel = g_nav.level;
    int n = 0;
    EnterCriticalSection(&g_matchLock);
    for (int i = (int)g_matchBots.size() - 1; i >= 0 && (cfg.zgPlayers < 0 || n < cfg.zgPlayers); i--)
        if (!g_matchBots[i].edict)
        {
            g_matchBots[i].fakePlayer = true;
            n++;
        }
    LeaveCriticalSection(&g_matchLock);
    if (n)
        Log("Zombie Giant: %d room AI players join as players who can be the giant", n);
}

static bool IsBotClass(void* pl)
{
    DWORD vt = pl ? *(DWORD*)pl - g_zgDelta : 0;
    for (DWORD b : MP_BOT_VTABLES)
        if (vt == b)
            return true;
    return false;
}

// a new match: fake players still connected from the last one leave (the server normally drops them)
static void ZgNewLevel()
{
    for (auto& z : g_zgPlayers)
    {
        if (!z.edict)
            continue;
        unsigned char* v = Vars(z.edict);
        void* pl = PrivateOf(z.edict);
        const char* name = String(F<int>(v, EV_NETNAME));
        if (pl && (F<int>(v, EV_FLAGS) & FL_CLIENT) && !IsBotClass(pl) && name && z.name == name && !strchr(name, '"'))
        {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "kick \"%s\"\n", name);
            g_zgKickNames.push_back(name);
            g_ServerCommand(cmd);
            Log("Zombie Giant: '%s' from the last match leaves", name);
        }
    }
    g_zgPlayers.clear();
}

static void CreateZgPlayer(ZgPlayer& z)
{
    z.tried = true;
    void* e = g_CreateFakeClient(z.name.c_str());
    if (!e)
    {
        Log("Zombie Giant: couldn't add the player '%s' (server full?)", z.name.c_str());
        return;
    }
    g_FreeEntPrivateData(e);
    ((EdictFn)(MP_CREATE_PLAYER + g_mpDelta))(e);
    void* pl = PrivateOf(e);
    if (!pl || *(DWORD*)pl != MP_ZOMBIEBOSSPLAYER_VT + g_mpDelta)
    {
        Log("Zombie Giant: '%s' isn't a Zombie Giant player (vtable 0x%08lx): removed", z.name.c_str(),
            pl ? *(DWORD*)pl - g_mpDelta : 0);
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "kick \"%s\"\n", z.name.c_str());
        g_zgKickNames.push_back(z.name);
        g_ServerCommand(cmd);
        return;
    }
    char* info = g_GetInfoKeyBuffer ? g_GetInfoKeyBuffer(e) : nullptr;
    if (info)
        g_SetClientKeyValue(g_IndexOfEdict(e), info, "*bot", "1"); // the scoreboard shows it like the bots
    ((EdictFn)(MP_PUT_IN_SERVER + g_mpDelta))(e);
    F<int>(Vars(e), EV_FLAGS) |= FL_FAKECLIENT;
    unsigned char* p = (unsigned char*)pl;
    *(int*)(p + PLAYER_TEAM) = 2;
    *(int*)(p + 0xe10) = 1;
    *(int*)(p + 0xcc0) = 4;
    int joined = ((ChooseTeamFn)(MP_CHOOSETEAM + g_mpDelta))(pl, 2);
    int id = z.character > 0 && CharacterFits(z.character, 2) ? z.character
             : !cfg.characters[2].empty()                      ? cfg.characters[2][rand() % cfg.characters[2].size()]
                                                               : 0;
    int number = id ? CharacterClassNumber(id) : 0;
    char cls[16];
    snprintf(cls, sizeof(cls), "%d", number > 0 ? number : 1);
    if (joined)
        ((ChooseAppearanceFn)(MP_CHOOSEAPPEARANCE + g_mpDelta))(pl, number > 0 ? number : 1);
    z.edict = e;
    EnterCriticalSection(&g_matchLock);
    for (size_t i = 0; i < g_matchBots.size(); i++)
        if (g_matchBots[i].fakePlayer && !g_matchBots[i].edict && g_matchBots[i].name == z.name)
        {
            g_matchBots[i].edict = e;
            StateFor(e).roomBot = (int)i;
            break;
        }
    LeaveCriticalSection(&g_matchLock);
    Log("Zombie Giant: '%s' joined as a player who can be the giant (team choice %s, now team %d, character %d, class %s)",
        z.name.c_str(), joined ? "accepted" : "REFUSED", *(int*)(p + PLAYER_TEAM), id, cls);
}

// the closest living player of the other team, visible ones first
static void* ZgTarget(void* e, int team, void* avoid = nullptr)
{
    V3 me = Eye(e);
    void* best = nullptr;
    float bestScore = 1e30f;
    ForEntitiesNear(me, 8000.0f, [&](void* o) {
        if (o == e || o == avoid || !(F<int>(Vars(o), EV_FLAGS) & FL_CLIENT) || !Alive(o))
            return;
        int t = TeamOf(o);
        if ((t != 1 && t != 2) || t == team)
            return;
        float d = Len(Center(o) - me);
        float score = CanSee(e, me, o) ? d : d * 1.5f + 500.0f;
        if (score < bestScore)
        {
            bestScore = score;
            best = o;
        }
    });
    return best;
}

// the yaw to walk at towards a point, along the navigation mesh
static bool ZgPathYaw(ZgPlayer& z, V3 me, V3 goal, float now, float reach, float& yaw)
{
    if (now >= z.nextPath || z.wp >= z.path.size())
    {
        if (!g_nav.ok || !FindPath(me, goal, z.path))
            z.path.assign(1, goal);
        z.wp = 0;
        z.nextPath = now + 1.0f;
    }
    while (z.wp + 1 < z.path.size() && Len2D(z.path[z.wp] - me) < reach)
        z.wp++;
    V3 d = z.path[z.wp] - me;
    if (Len2D(d) < 8.0f)
        return false;
    yaw = YawOf(d);
    return true;
}

// the human side (room bots and fake players) fights together: where they gather, refreshed twice a second
static V3 g_zgRally{};
static int g_zgAllies = 0;
static float g_zgNextRally = 0;
static std::unordered_map<void*, ZgPlayer> g_zgBots; // room bots on the human side, driven like the soldiers

static bool IsZgPlayerClass(void* pl) { return pl && *(DWORD*)pl == MP_ZOMBIEBOSSPLAYER_VT + g_mpDelta; }

static void ZgWatchClient(void* e, unsigned char* pl, float now, bool census);

static bool ZgSwimming(void* e);
static V3 ZgDry(V3 p);
static V3 ZgFeet(void* e);

static void ZgUpdateTeam(float now)
{
    if (now < g_zgNextRally)
        return;
    g_zgNextRally = now + 0.5f;
    static float nextCensus = 0;
    bool census = now >= nextCensus || now < nextCensus - 20.0f;
    if (census)
        nextCensus = now + 15.0f;
    V3 sum{};
    int n = 0;
    float c[3] = { 0, 0, 0 };
    void* e = nullptr;
    for (int guard = 0; guard < 2048; guard++)
    {
        e = g_FindEntityInSphere(e, c, 16384.0f);
        if (!e || !strcmp(ClassName(e), "worldspawn"))
            break;
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            continue;
        unsigned char* pl = (unsigned char*)PrivateOf(e);
        if (!pl)
            continue;
        ZgWatchClient(e, pl, now, census);
        if (IsHuman(e))
            g_zgYourMaxspeed = Alive(e) ? F<float>(Vars(e), EV_MAXSPEED) : g_zgYourMaxspeed;
        if (Alive(e) && *(int*)(pl + PLAYER_TEAM) == 2 && !ZgSwimming(e))
        {
            sum = sum + ZgFeet(e);
            n++;
        }
    }
    g_zgAllies = n;
    if (n)
        g_zgRally = ZgDry(sum * (1.0f / n));
}

// a soldier left at the joining speed (1) once you can move gets a rifleman's speed
static void ZgUnfreeze(void* e, const char* who)
{
    float speed = F<float>(Vars(e), EV_MAXSPEED);
    if (!g_SetClientMaxspeed || speed <= 0.0f || speed >= 50.0f || g_zgYourMaxspeed < 100.0f)
        return;
    g_SetClientMaxspeed(e, 240.0f);
    static int reports = 0;
    if (reports++ < 20)
        Log("Zombie Giant: %s '%s' had maxspeed %.0f (you %.0f): set to 240 -> %.0f", who,
            String(F<int>(Vars(e), EV_NETNAME)), speed, g_zgYourMaxspeed, F<float>(Vars(e), EV_MAXSPEED));
}

// What only the giants can break (zg_breakable: forklifts, containers..., 136 of them on zg_splash) and the
// ordinary func_breakable stop a giant that walks into them. When the giant hardly moves and one is right in front
// of it (within its reach, towards where it walks), it faces it and hits it (BossAttack1) until it breaks.
static bool ZgBreakable(void* o)
{
    const char* cls = ClassName(o);
    unsigned char* v = Vars(o);
    return cls && (!strcmp(cls, "zg_breakable") || !strcmp(cls, "func_breakable")) && F<float>(v, EV_HEALTH) > 0.0f &&
           F<float>(v, EV_TAKEDAMAGE) != 0.0f && F<int>(v, EV_SOLID) != 0; // broken ones are left not solid
}

static void ZgBreakThrough(void* e, ZgPlayer& z, V3 me, bool move, float moveYaw, float now, float& lookYaw,
                           float& lookPitch)
{
    unsigned char* v = Vars(e);
    float speed = Len2D(Vec((float*)(v + EV_VELOCITY)));
    if (!move || speed > 90.0f)
        z.slowSince = now;
    if (z.breakTarget && (now >= z.breakUntil || !ZgBreakable(z.breakTarget)))
    {
        static int reports = 0;
        if (reports++ < 30)
            Log("Zombie Giant: '%s' (giant) %s the %s in its way", String(F<int>(v, EV_NETNAME)),
                ZgBreakable(z.breakTarget) ? "gives up on" : "broke", ClassName(z.breakTarget));
        if (ZgBreakable(z.breakTarget))
        {
            z.ignoreBreak = z.breakTarget; // it doesn't break (or isn't really in the way): not again soon
            z.ignoreBreakUntil = now + 20.0f;
        }
        z.breakTarget = nullptr;
        z.nextPath = 0;
    }
    if (!z.breakTarget && move && now - z.slowSince > 0.4f && now >= z.nextBreakScan)
    {
        z.nextBreakScan = now + 0.3f;
        V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
        float radius = (hi.x - lo.x) * 0.5f;
        V3 c = (lo + hi) * 0.5f;
        float best = 1e30f;
        ForEntitiesNear(c, radius + 400.0f, [&](void* o) {
            if (o == e || !ZgBreakable(o) || (o == z.ignoreBreak && now < z.ignoreBreakUntil))
                return;
            unsigned char* ov = Vars(o);
            V3 olo = Vec((float*)(ov + EV_ABSMIN)), ohi = Vec((float*)(ov + EV_ABSMAX));
            V3 oc = (olo + ohi) * 0.5f;
            if (ohi.z < lo.z + 8.0f || olo.z > hi.z) // under its feet or over its head
                return;
            // the gap between the two boxes, sideways
            float gx = fmaxf(0.0f, fmaxf(olo.x - hi.x, lo.x - ohi.x));
            float gy = fmaxf(0.0f, fmaxf(olo.y - hi.y, lo.y - ohi.y));
            float gap = sqrtf(gx * gx + gy * gy);
            if (gap > 120.0f || fabsf(AngleDiff(YawOf(oc - c), moveYaw)) > 70.0f)
                return;
            if (gap < best)
            {
                best = gap;
                z.breakTarget = o;
            }
        });
        if (z.breakTarget)
        {
            z.breakUntil = now + 6.0f;
            static int reports = 0;
            if (reports++ < 30)
                Log("Zombie Giant: '%s' (giant) is blocked by a %s (health %.0f, %.0f units away): breaking it",
                    String(F<int>(v, EV_NETNAME)), ClassName(z.breakTarget), F<float>(Vars(z.breakTarget), EV_HEALTH), best);
        }
    }
    if (!z.breakTarget)
        return;
    V3 oc = Center(z.breakTarget), eye = Eye(e);
    lookYaw = YawOf(oc - eye);
    lookPitch = PitchOf(oc - eye) * 0.5f;
    if (fabsf(AngleDiff(lookYaw, z.yaw)) < 25.0f && now >= z.nextAttack)
    {
        FakeCommand(e, "BossAttack1");
        z.nextAttack = now + 0.9f + Rnd() * 0.4f;
    }
}

// ---------------------------------------------------------------- Zombie Giant: water and ladders
// The middle of the Zombie Giant maps is a water channel (zg_splash: func_water from z -256 up to -92, with a
// trigger_push current) left only by a few ladders (func_ladder), which the navigation mesh doesn't link: soldiers
// who went in stayed there. The water and the ladders are read from the map's entities once per match; the soldiers'
// paths avoid the water, their goals (the giant, the group) are moved to the closest dry ground, and a soldier in the
// water swims to the closest ladder out and climbs it (looking up, walking forward).
struct ZgBox
{
    V3 lo, hi;
};
static std::vector<ZgBox> g_zgWater, g_zgLadders;
static std::vector<char> g_navWet; // per navigation area: its floor is under water (FindPath avoids it)
static int g_zgWorldLevel = -1, g_navWetLevel = -1;

static bool InWaterBox(const ZgBox& w, V3 p, float below)
{
    return p.x >= w.lo.x && p.x <= w.hi.x && p.y >= w.lo.y && p.y <= w.hi.y && p.z <= w.hi.z - below &&
           p.z >= w.lo.z - 80.0f;
}

// a point on the floor (feet) under water
static bool ZgWet(V3 p)
{
    for (auto& w : g_zgWater)
        if (InWaterBox(w, p, 4.0f))
            return true;
    return false;
}

static V3 ZgFeet(void* e)
{
    V3 o = Origin(e);
    o.z = F<float>(Vars(e), EV_ABSMIN + 8);
    return o;
}

// the floor `dist` units away towards yaw is under water (or a drop into it)
static bool ZgWetAhead(void* e, float yaw, float dist)
{
    if (g_zgWater.empty())
        return false;
    V3 me = Origin(e);
    V3 p = me + V3{ cosf(yaw / RAD), sinf(yaw / RAD), 0 } * dist;
    TraceResult tr;
    Trace(p, p + V3{ 0, 0, -800.0f }, e, 1, tr);
    return ZgWet(Vec(tr.vecEndPos));
}

static void ZgScanWorld()
{
    if (g_zgWorldLevel == g_nav.level)
        return;
    g_zgWorldLevel = g_nav.level;
    g_zgWater.clear();
    g_zgLadders.clear();
    ForEntitiesNear({ 0, 0, 0 }, 32768.0f, [&](void* o) {
        const char* cls = ClassName(o);
        bool water = cls && !strcmp(cls, "func_water"), ladder = cls && !strcmp(cls, "func_ladder");
        if (!water && !ladder)
            return;
        unsigned char* v = Vars(o);
        ZgBox b{ Vec((float*)(v + EV_ABSMIN)), Vec((float*)(v + EV_ABSMAX)) };
        (water ? g_zgWater : g_zgLadders).push_back(b);
    });
    g_navWet.assign(g_nav.areas.size(), 0);
    int wet = 0;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
        wet += g_navWet[i] = ZgWet(g_nav.areas[i].center);
    g_navWetLevel = g_nav.level;
    Log("Zombie Giant: %d water volumes, %d ladders, %d of %d navigation areas under water", (int)g_zgWater.size(),
        (int)g_zgLadders.size(), wet, (int)g_nav.areas.size());
}

// extra path cost of an area under water (only while the Zombie Giant water is known)
static float NavWetCost(int area)
{
    return g_navWetLevel == g_nav.level && area < (int)g_navWet.size() && g_navWet[area] ? 2000.0f : 0.0f;
}

// the closest dry ground to a point under water (the point itself when it's dry)
static V3 ZgDry(V3 p)
{
    if (!ZgWet(p) || g_navWet.size() != g_nav.areas.size())
        return p;
    float best = 1e30f;
    V3 out = p;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        if (g_navWet[i])
            continue;
        float d = Len(g_nav.areas[i].center - p);
        if (d < best)
        {
            best = d;
            out = g_nav.areas[i].center;
        }
    }
    return out;
}

static bool ZgSwimming(void* e)
{
    return (F<int>(Vars(e), EV_FLAGS) & FL_INWATER) || ZgWet(ZgFeet(e));
}

// a soldier in the water: to the closest ladder that leaves it (or the closest dry ground), and up the ladder.
// true: fwd / look set
static bool ZgLeaveWater(void* e, ZgPlayer& z, float now, float& moveYaw, float& lookYaw, float& lookPitch, bool& move)
{
    V3 me = Origin(e);
    bool swimming = ZgSwimming(e);
    bool climbing = now < z.climbUntil;
    if (!swimming && !climbing)
    {
        z.ladder = -1;
        return false;
    }
    if (z.ladder < 0 || z.ladder >= (int)g_zgLadders.size())
    {
        float best = 1e30f;
        for (size_t i = 0; i < g_zgLadders.size(); i++)
        {
            const ZgBox& l = g_zgLadders[i];
            V3 foot{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, l.lo.z };
            bool fromWater = false;
            for (auto& w : g_zgWater) // its foot is in the water, its top above the surface
                fromWater |= InWaterBox(w, foot, 0.0f) && l.hi.z > w.hi.z;
            float d = Len2D(foot - me);
            if (fromWater && d < best)
            {
                best = d;
                z.ladder = (int)i;
            }
        }
        static int reports = 0;
        if (reports++ < 30)
            Log("Zombie Giant: '%s' is in the water: %s", String(F<int>(Vars(e), EV_NETNAME)),
                z.ladder >= 0 ? "heading for a ladder" : "no ladder out, heading for dry ground");
    }
    move = true;
    if (z.ladder < 0)
    {
        moveYaw = YawOf(ZgDry(me) - me);
        lookYaw = moveYaw;
        lookPitch = 0;
        return true;
    }
    const ZgBox& l = g_zgLadders[z.ladder];
    V3 c{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, me.z };
    float gx = fmaxf(0.0f, fmaxf(l.lo.x - me.x, me.x - l.hi.x)), gy = fmaxf(0.0f, fmaxf(l.lo.y - me.y, me.y - l.hi.y));
    moveYaw = YawOf(c - me);
    lookYaw = moveYaw;
    if (sqrtf(gx * gx + gy * gy) < 40.0f || climbing)
    {
        // on the ladder: look up and walk into it; a little more once out of the water to step off at the top
        lookPitch = -70.0f;
        if (me.z < l.hi.z - 8.0f)
            z.climbUntil = now + 0.8f;
    }
    else
        lookPitch = 0;
    return true;
}

// ---------------------------------------------------------------- Zombie Giant: each giant's moves
// The giant's commands become monster states (pawn vfunc +0x20; the states are registered in 0x1091b060):
//   Dione (type 1): Attack1 light, Attack2 heavy (a whirl), Skill1 rolling charge, Skill2 poison missile,
//                   Skill3 tentacles, Skill4 (evolved) air strike
//   Titan (2):      Attack1 light, Attack2 heavy, Skill1 rolling charge, Skill2 rockets, Skill3 mines,
//                   Skill4 (evolved) artillery
//   Revenant (4):   Attack1 light, Attack2 its heavy attack, Skill1 rolling charge, Skill2 fireball, Skill3 inferno
//                   (around itself), Skill4 (evolved) hellfire from the sky
//   Angra (6):      the dragon: Attack1 light, Attack2 heavy, Skill1 its rolling attack (comes down at the target),
//                   Skill2 breath, Skill3 tentacles, Skill4 (evolved) poison fog
// The pawn (0x109201f0, fastcall player: the pawn of the current type) holds the stamina (+0xc), the evolution
// points (+0x10, 10 to evolve), the evolved flag (+0x14) and, per skill slot 1..4, "ready" (+0x14 + slot) and the
// cool down's end (+0x28 + 4 * slot). Each state's data (0x1090c770(state): [0] skill slot, [5] cool down,
// [6] stamina cost) is what 0x1090e1f0 checks before switching to it. The current state is the state manager's
// (player +0x3738, an object inside the player) vfunc +0x14; 0 is idle. The giant uses a skill as soon as it's ready, it can pay for it and the
// target fits it, and attacks in between.
enum ZgRole
{
    ZG_MELEE,      // within reach
    ZG_HEAVY,      // within reach, slower and stronger
    ZG_MID,        // aimed, 250..900
    ZG_CHARGE,     // aimed, 280..1000: closes the gap
    ZG_PROJECTILE, // aimed, 350..2600
    ZG_AREA,       // aimed at the target, 200..2200
    ZG_SELF,       // around the giant: humans close by
    ZG_CONE,       // aimed, up to 750
};
struct ZgMove
{
    const char* cmd;
    int state, evolved; // -1: not available
    ZgRole role;
};
struct ZgKit
{
    int type;
    const char* name;
    ZgMove moves[6]; // Attack1, Attack2, Skill1..4
};
static const ZgKit g_zgKits[] = {
    { 1, "Dione", { { "BossAttack1", 3, 0x26, ZG_MELEE }, { "BossAttack2", 4, 0x27, ZG_MID }, { "BossSkill_1", 5, 5, ZG_CHARGE },
                    { "BossSkill_2", 6, 0x28, ZG_PROJECTILE }, { "BossSkill_3", 7, 0x29, ZG_AREA },
                    { "BossSkill_4", -1, 0x2a, ZG_AREA } } },
    { 2, "Titan", { { "BossAttack1", 10, 0x2b, ZG_MELEE }, { "BossAttack2", 11, 0x2c, ZG_HEAVY }, { "BossSkill_1", 0xc, 0xc, ZG_CHARGE },
                    { "BossSkill_2", 0xd, 0x2d, ZG_PROJECTILE }, { "BossSkill_3", 0xe, 0x2e, ZG_SELF },
                    { "BossSkill_4", -1, 0x2f, ZG_AREA } } },
    { 4, "Revenant", { { "BossAttack1", 0x14, 0x30, ZG_MELEE }, { "BossAttack2", 0x15, 0x31, ZG_MID },
                       { "BossSkill_1", 0x16, 0x16, ZG_CHARGE }, { "BossSkill_2", 0x17, 0x32, ZG_PROJECTILE },
                       { "BossSkill_3", 0x18, 0x33, ZG_SELF }, { "BossSkill_4", -1, 0x34, ZG_AREA } } },
    { 6, "Angra", { { "BossAttack1", 0x20, 0x35, ZG_MELEE }, { "BossAttack2", 0x21, 0x36, ZG_HEAVY },
                    { "BossSkill_1", 0x22, 0x22, ZG_CHARGE }, { "BossSkill_2", 0x23, 0x37, ZG_CONE },
                    { "BossSkill_3", 0x24, 0x38, ZG_AREA }, { "BossSkill_4", -1, 0x39, ZG_SELF } } },
};
static const DWORD MP_ZG_PAWN = 0x109201f0, MP_ZG_STATEINFO = 0x1090c770;
static const DWORD MP_GIANT_PAWN_VTABLES[] = { 0x117c18e8, 0x117c191c, 0x117c1950, 0x117c1984 };
typedef unsigned char* (__fastcall* PawnOfFn)(void* player);
typedef int* (*StateInfoFn)(int state);
typedef int(__fastcall* CurrentStateFn)(void* manager, void* edx);

struct ZgPawn
{
    bool ok = false;
    float stamina = 0;
    int evolution = 0, state = 0;
    bool evolved = false;
    bool ready[5] = {};
};

static bool ReadZgPawn(unsigned char* pl, ZgPawn& out)
{
    __try
    {
        unsigned char* pawn = ((PawnOfFn)(MP_ZG_PAWN + g_mpDelta))(pl);
        if (!pawn)
            return false;
        DWORD vt = *(DWORD*)pawn - g_mpDelta;
        bool giant = false;
        for (DWORD g : MP_GIANT_PAWN_VTABLES)
            giant |= vt == g;
        if (!giant)
            return false;
        out.stamina = *(float*)(pawn + 0xc);
        out.evolution = *(int*)(pawn + 0x10);
        out.evolved = pawn[0x14] != 0;
        for (int s = 1; s <= 4; s++)
            out.ready[s] = pawn[0x14 + s] != 0;
        unsigned char* manager = pl + 0x3738; // the state manager is part of the player (its vtable is there)
        out.state = ((CurrentStateFn)(*(void***)manager)[0x14 / 4])(manager, nullptr);
        out.ok = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// can the giant switch to this state now: its skill slot ready and the stamina to pay for it
static bool ZgAffordable(const ZgPawn& p, int state)
{
    __try
    {
        int* info = ((StateInfoFn)(MP_ZG_STATEINFO + g_mpDelta))(state);
        int slot = info[0];
        if (slot >= 1 && slot <= 4 && !p.ready[slot])
            return false;
        return *(float*)&info[6] <= p.stamina;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// dist: from the giant's eyes; flat: sideways only (the giant is tall: a human at its feet is far from its eyes)
static bool ZgRoleFits(ZgRole role, float dist, float flat, bool visible, int near450, int near700)
{
    switch (role)
    {
    case ZG_MELEE: return visible && flat < 320.0f;
    case ZG_HEAVY: return visible && flat < 360.0f;
    case ZG_MID: return visible && dist > 250.0f && dist < 900.0f;
    case ZG_CHARGE: return visible && dist > 280.0f && dist < 1000.0f;
    case ZG_PROJECTILE: return visible && dist > 350.0f && dist < 2600.0f;
    case ZG_AREA: return visible && dist > 200.0f && dist < 2200.0f;
    case ZG_SELF: return near450 >= 1 || near700 >= 2;
    case ZG_CONE: return visible && flat < 750.0f;
    }
    return false;
}

static bool ZgAimed(ZgRole role) { return role != ZG_SELF; }

// the giant's fighting: picks a move (skills first), turns to the target if the move is aimed, and uses it
static void ZgGiantCombat(void* e, ZgPlayer& z, unsigned char* pl, float dist, bool visible, V3 tp, V3 eye, float now,
                          float& lookYaw, float& lookPitch, unsigned short& buttons)
{
    int type = *(int*)(pl + ZG_GIANT_TYPE);
    const ZgKit* kit = nullptr;
    for (auto& k : g_zgKits)
        if (k.type == type)
            kit = &k;
    ZgPawn p;
    const char* name = String(F<int>(Vars(e), EV_NETNAME));
    float flat = Len2D(tp - eye);
    if (!kit || !ReadZgPawn(pl, p))
    {
        // the pawn couldn't be read: plain attacks within reach and a random skill now and then
        if (!z.simpleGiant)
        {
            z.simpleGiant = true;
            Log("Zombie Giant: '%s' (type %d): the pawn couldn't be read, simple attacks", name, type);
        }
        if (visible && flat < 320.0f && now >= z.nextAttack)
        {
            FakeCommand(e, Rnd() < 0.7f ? "BossAttack1" : "BossAttack2");
            z.nextAttack = now + 0.8f + Rnd() * 0.5f;
        }
        else if (visible && dist < 2200.0f && now >= z.nextSkill)
        {
            char skill[16];
            snprintf(skill, sizeof(skill), "BossSkill_%d", 1 + rand() % 3);
            FakeCommand(e, skill);
            z.nextSkill = now + 3.0f + Rnd() * 3.0f;
        }
        return;
    }
    if (z.kitType != type)
    {
        z.kitType = type;
        z.pending = -1;
        Log("Zombie Giant: '%s' is %s (stamina %.0f, evolution %d%s)", name, kit->name, p.stamina, p.evolution,
            p.evolved ? ", evolved" : "");
    }
    // evolving: with 10 points the pawn evolves when "use" is pressed (0x1090d290: player +0xec0, the buttons
    // pressed this frame, & IN_USE), which unlocks Skill4 and stronger moves
    if (!p.evolved && p.evolution >= 10 && now >= z.nextUse)
    {
        buttons |= IN_USE;
        z.nextUse = now + 1.0f;
        static int reports = 0;
        if (reports++ < 20)
            Log("Zombie Giant: '%s' (%s) presses use to evolve", name, kit->name);
    }
    // busy with a move: wait (unless a state seems stuck)
    if (p.state != 0)
    {
        if (z.busySince <= 0)
            z.busySince = now;
        if (now - z.busySince < 5.0f)
            return;
    }
    else
        z.busySince = 0;
    int near450 = 0, near700 = 0;
    ForEntitiesNear(Origin(e), 700.0f, [&](void* o) {
        if (o == e || !(F<int>(Vars(o), EV_FLAGS) & FL_CLIENT) || !Alive(o) || TeamOf(o) != 2)
            return;
        float d = Len(Center(o) - Center(e));
        near450 += d < 450.0f;
        near700 += d < 700.0f;
    });
    // a pending aimed move: fire once facing the target, give up after a moment
    if (z.pending >= 0)
    {
        const ZgMove& m = kit->moves[z.pending];
        if (!visible || now >= z.aimUntil)
            z.pending = -1;
        else if (fabsf(AngleDiff(lookYaw, z.yaw)) < (m.role == ZG_PROJECTILE ? 6.0f : 12.0f))
        {
            FakeCommand(e, m.cmd);
            static int reports = 0;
            if (reports++ < 80)
                Log("Zombie Giant: '%s' (%s) uses %s at %.0f units (stamina %.0f)", name, kit->name, m.cmd, dist, p.stamina);
            z.pending = -1;
            z.nextAttack = now + (z.pendingSkill ? 0.4f : 0.7f + Rnd() * 0.4f);
            if (!z.pendingSkill)
                z.nextRanged = now + 4.0f + Rnd() * 5.0f;
        }
        return;
    }
    if (now < z.nextAttack)
        return;
    // the skills that fit, at random
    int choice = -1, fits = 0;
    for (int i = 2; i < 6; i++)
    {
        const ZgMove& m = kit->moves[i];
        int state = p.evolved ? m.evolved : m.state;
        if (state < 0 || !ZgRoleFits(m.role, dist, flat, visible, near450, near700) || !ZgAffordable(p, state))
            continue;
        if (rand() % ++fits == 0)
            choice = i;
    }
    bool skill = choice >= 0;
    if (!skill)
    {
        // attacks: within reach the light one mostly; Attack2 from afar now and then when it's a mid-range one
        const ZgMove& a1 = kit->moves[0];
        const ZgMove& a2 = kit->moves[1];
        bool a2fits = ZgRoleFits(a2.role, dist, flat, visible, near450, near700) &&
                      (a2.role != ZG_MID || flat < 330.0f || now >= z.nextRanged);
        if (ZgRoleFits(a1.role, dist, flat, visible, near450, near700))
            choice = a2fits && Rnd() < 0.35f ? 1 : 0;
        else if (a2fits)
            choice = 1;
        if (choice >= 0 && !ZgAffordable(p, p.evolved ? kit->moves[choice].evolved : kit->moves[choice].state))
            choice = -1;
    }
    if (choice < 0)
        return;
    const ZgMove& m = kit->moves[choice];
    if (ZgAimed(m.role) && flat > 330.0f)
    {
        z.pending = choice; // turn first (ZgThink looks at the target while one is pending)
        z.pendingSkill = skill;
        z.aimUntil = now + 1.2f;
        return;
    }
    FakeCommand(e, m.cmd);
    if (skill)
    {
        static int reports = 0;
        if (reports++ < 80)
            Log("Zombie Giant: '%s' (%s) uses %s at %.0f units (stamina %.0f, %d humans close)", name, kit->name,
                m.cmd, dist, p.stamina, near700);
    }
    z.nextAttack = now + (skill ? 0.4f : 0.7f + Rnd() * 0.4f);
}

// ---------------------------------------------------------------- Zombie Giant: ammunition
// The soldiers' weapon (from CBasePlayerWeapon::UpdateClientData 0x11533920 and the AmmoX loop in 0x114df514):
// player +0x101c the active item; weapon +0xe4 its id, +0x158 the primary ammo type, +0x160 the clip; player
// +0x1034 the reserve per ammo type (0x102 types).
// Each weapon a soldier holds for the first time in a life gets its ammunition bought ("primammo" / "secammo",
// the mode's money is topped up); when the buy gives nothing (the room's weapons handed over directly have none),
// the reserve is filled directly: three clips. A soldier reloads when its clip is empty, and between fights when
// it's below half. With nothing left it takes its pistol, then its knife. (The zs_supply boxes need a charge earned
// by dealing damage: the soldiers don't go there.)
struct ZgGun
{
    bool ok = false;
    int id = 0, clip = 0, reserve = 0;
    int* reservePtr = nullptr;
};

static ZgGun ZgWeapon(void* e)
{
    ZgGun g;
    unsigned char* pl = (unsigned char*)PrivateOf(e);
    if (!pl || !Readable(pl + 0x101c, 4))
        return g;
    unsigned char* w = *(unsigned char**)(pl + 0x101c);
    if (!w || !Readable(w + 0xe4, 4) || !Readable(w + 0x158, 12))
        return g;
    int type = *(int*)(w + 0x158);
    g.clip = *(int*)(w + 0x160);
    g.id = *(int*)(w + 0xe4);
    if (type <= 0 || type >= 0x102 || g.clip < 0 || !Readable(pl + 0x1034 + 4 * type, 4))
        return g; // a knife or a grenade
    g.reservePtr = (int*)(pl + 0x1034 + 4 * type);
    g.reserve = *g.reservePtr;
    g.ok = true;
    return g;
}

// reloading and stocking up (buttons, commands); holdFire while the clip is empty
static void ZgAmmo(void* e, ZgPlayer& z, float now, bool fighting, unsigned short& buttons, bool& holdFire)
{
    ZgGun g = ZgWeapon(e);
    if (!g.ok)
        return;
    const char* name = String(F<int>(Vars(e), EV_NETNAME));
    int& top = z.maxClip[g.id];
    top = g.clip > top ? g.clip : top;
    // a weapon held for the first time in this life: its ammunition
    if (std::find(z.stocked.begin(), z.stocked.end(), g.id) == z.stocked.end())
    {
        int want = top * 3 < 30 ? 30 : top * 3 > 300 ? 300 : top * 3;
        if (z.stockId != g.id)
        {
            z.stockId = g.id;
            z.stockAt = now + 0.6f;
            z.stockBefore = g.reserve;
            unsigned char* pl = (unsigned char*)PrivateOf(e);
            if (pl && *(int*)(pl + 0xca8) < 16000)
                *(int*)(pl + 0xca8) = 16000;
            if (g.reserve < want)
                for (int i = 0; i < 3; i++)
                {
                    FakeCommand(e, "primammo");
                    FakeCommand(e, "secammo");
                }
        }
        else if (now >= z.stockAt)
        {
            bool bought = g.reserve > z.stockBefore;
            if (g.reserve < want)
                *g.reservePtr = want; // nothing to buy (or not enough): handed over like the room's weapons
            z.stocked.push_back(g.id);
            z.stockId = 0;
            static int reports = 0;
            if (reports++ < 40)
                Log("Zombie Giant: '%s' ammunition for weapon %d: %d -> %d (%s)", name, g.id, z.stockBefore, *g.reservePtr,
                    bought ? "bought" : "given");
            g.reserve = *g.reservePtr;
        }
    }
    holdFire = g.clip == 0;
    if (g.reserve > 0 && now >= z.nextReload && (g.clip == 0 || (!fighting && g.clip < top / 2)))
    {
        buttons |= IN_RELOAD;
        z.nextReload = now + (g.clip == 0 ? 0.6f : 2.5f);
    }
    if (g.clip == 0 && g.reserve == 0 && now >= z.nextSlot)
    {
        // empty: the pistol, then the knife
        z.emptyStage++;
        FakeCommand(e, z.emptyStage == 1 ? "slot2" : "slot3");
        z.nextSlot = now + 1.0f;
        static int reports = 0;
        if (reports++ < 30)
            Log("Zombie Giant: '%s' is out of ammunition (weapon %d): %s", name, g.id,
                z.emptyStage == 1 ? "the pistol" : "the knife");
    }
}

// one frame of a Zombie Giant fighter: the giant hunts the closest human, soldiers gather and shoot the giant
static void ZgThink(void* e, ZgPlayer& z, bool giant, int team, float now, float dt, float& fwd, float& side,
                    unsigned short& buttons)
{
    if (now >= z.nextTarget || (z.target && !Alive(z.target)))
    {
        z.target = ZgTarget(e, team, now < z.avoidUntil ? z.avoidTarget : nullptr);
        if (!z.target)
            z.target = ZgTarget(e, team);
        z.nextTarget = now + 0.4f;
    }
    V3 me = Origin(e), eye = Eye(e);
    float lookYaw = z.yaw, lookPitch = 0, moveYaw = z.yaw;
    bool move = false;
    bool leaving = !giant && !g_zgWater.empty() && ZgLeaveWater(e, z, now, moveYaw, lookYaw, lookPitch, move);
    bool holdFire = false;
    if (!giant)
        ZgAmmo(e, z, now, z.target && CanSee(e, eye, z.target), buttons, holdFire);
    if (z.target && !leaving)
    {
        V3 tp = Center(z.target);
        float dist = Len(tp - eye);
        bool visible = CanSee(e, eye, z.target);
        if (giant)
        {
            move = dist > 140.0f && ZgPathYaw(z, me, Origin(z.target), now, 90.0f, moveYaw);
            // aimed moves (ZgGiantCombat) turn the giant to the target first, a little high the farther it is
            bool aiming = z.pending >= 0 && visible;
            V3 aimAt = tp;
            aimAt.z += dist > 600.0f ? (dist - 600.0f) * 0.06f : 0.0f;
            lookYaw = (dist < 600.0f && visible) || aiming ? YawOf(aimAt - eye) : move ? moveYaw : z.yaw;
            lookPitch = (dist < 600.0f && visible) || aiming ? PitchOf(aimAt - eye) : 0;
            ZgGiantCombat(e, z, (unsigned char*)PrivateOf(e), dist, visible, tp, eye, now, lookYaw, lookPitch, buttons);
            if (dist > 1500.0f && now >= z.nextSprint)
            {
                z.nextSprint = now + 4.0f;
                if (Rnd() < 0.3f)
                    z.sprintUntil = now + 2.0f;
            }
            if (now < z.sprintUntil)
                buttons |= IN_ATTACK2;
        }
        else
        {
            // soldiers stay with the group (rally point) on the way, then keep 450..850 units from the giant,
            // strafing, and shoot it
            bool engaged = visible && dist < 1100.0f;
            float fromGroup = g_zgAllies > 1 ? Len2D(g_zgRally - me) : 0.0f;
            V3 goal = ZgDry(ZgFeet(z.target)); // a giant in the water: from the bank
            if (!engaged && fromGroup > 500.0f && Len(g_zgRally - Origin(z.target)) < Len(me - Origin(z.target)) + 300.0f)
                move = ZgPathYaw(z, me, g_zgRally, now, 40.0f, moveYaw);
            else if (!engaged || dist > 850.0f)
                move = ZgPathYaw(z, me, goal, now, 40.0f, moveYaw);
            else if (dist < 450.0f)
            {
                moveYaw = YawOf(me - Origin(z.target));
                move = !ZgWetAhead(e, moveYaw, 150.0f); // backing off, but not into the water
            }
            lookYaw = visible ? YawOf(tp - eye) : move ? moveYaw : z.yaw;
            lookPitch = visible ? PitchOf(tp - eye) : 0;
            if (now >= z.nextStrafe)
            {
                z.strafe = -z.strafe;
                z.nextStrafe = now + 0.8f + Rnd() * 1.4f;
            }
            if (visible && !holdFire && dist < 3000.0f && fabsf(AngleDiff(lookYaw, z.yaw)) < 8.0f && now >= z.nextFire)
            {
                buttons |= IN_ATTACK;
                z.nextFire = now + 0.12f;
            }
            if (engaged && ZgWetAhead(e, z.yaw - 90.0f * z.strafe, 120.0f)) // strafing, not into the water
                z.strafe = -z.strafe;
            if (engaged)
                side += z.strafe * 250.0f;
        }
    }
    else if (!giant && !leaving && g_zgAllies > 1 && Len2D(g_zgRally - me) > 300.0f)
        move = ZgPathYaw(z, me, g_zgRally, now, 40.0f, moveYaw); // nobody to fight: gather
    if (giant)
        ZgBreakThrough(e, z, me, move, moveYaw, now, lookYaw, lookPitch);
    if (now < z.detourUntil)
    {
        moveYaw = z.detourYaw; // around what blocked it
        move = true;
    }
    if (!giant && move && !leaving && ZgWetAhead(e, moveYaw, 70.0f))
    {
        move = false; // the next step is a drop into the water: another way
        z.nextPath = 0;
    }
    float turn = (giant ? 240.0f : 420.0f) * dt;
    z.yaw = Approach(z.yaw, move && !z.target && !z.breakTarget ? moveYaw : lookYaw, turn);
    float k = dt * 6.0f > 1.0f ? 1.0f : dt * 6.0f;
    z.pitch += (lookPitch - z.pitch) * k;
    if (move)
    {
        float rel = AngleDiff(moveYaw, z.yaw) / RAD;
        fwd += cosf(rel) * 400.0f;
        side += -sinf(rel) * 400.0f;
    }
    // the movement keys too, like a player's client: the mode's movement code reads them
    buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
    if (fwd > 50.0f)
        buttons |= IN_FORWARD;
    else if (fwd < -50.0f)
        buttons |= IN_BACK;
    if (side > 50.0f)
        buttons |= IN_MOVERIGHT;
    else if (side < -50.0f)
        buttons |= IN_MOVELEFT;
    z.lastFwd = fwd;
    z.lastSide = side;
    z.lastMove = move;
    // stuck twice in a row (2 s): a jump and another way round
    if (now >= z.stuckCheck)
    {
        if (move && Len2D(me - z.stuckFrom) < 30.0f && fabsf(me.z - z.stuckFrom.z) < 30.0f) // climbing isn't stuck
            z.stuckCount++;
        else
            z.stuckCount = 0;
        if (z.stuckCount >= 2 && !(giant && z.breakTarget)) // (a giant breaking what's in the way isn't stuck)
        {
            // stuck again and again: a jump, then a way round sideways, then another target
            if (now - z.stuckRoundsSince > 12.0f)
            {
                z.stuckRounds = 0;
                z.stuckRoundsSince = now;
            }
            z.stuckRounds++;
            if (z.stuckRounds == 1)
            {
                if (giant)
                    FakeCommand(e, "BossJump");
                else
                    buttons |= IN_JUMP;
            }
            else if (z.stuckRounds == 2 || !z.target)
            {
                z.detourYaw = z.yaw + (Rnd() < 0.5f ? 100.0f : -100.0f);
                z.detourUntil = now + 1.5f;
            }
            else
            {
                z.avoidTarget = z.target;
                z.avoidUntil = now + 10.0f;
                z.nextTarget = 0;
                z.stuckRounds = 0;
                static int reports = 0;
                if (reports++ < 30)
                    Log("Zombie Giant: '%s' can't get to '%s': another target for a while",
                        String(F<int>(Vars(e), EV_NETNAME)), String(F<int>(Vars(z.target), EV_NETNAME)));
            }
            z.strafe = -z.strafe;
            z.nextPath = 0;
            z.stuckCount = 0;
        }
        z.stuckFrom = me;
        z.stuckCheck = now + 1.0f;
    }
}

static void ZgSpawned(void* e, ZgPlayer& z, float now)
{
    z.wasAlive = true;
    z.buyAt = now + 0.7f;
    z.path.clear();
    z.target = nullptr;
    z.nextSkill = now + 4.0f + Rnd() * 6.0f;
    z.nextRanged = now + 3.0f + Rnd() * 5.0f;
    z.aimUntil = 0;
    z.breakTarget = nullptr;
    z.pending = -1;
    z.kitType = 0;
    z.busySince = 0;
    z.ladder = -1;
    z.climbUntil = 0;
    z.stocked.clear();
    z.stockId = 0;
    z.emptyStage = 0;
    z.detourUntil = 0;
    z.stuckRounds = 0;
    z.slowSince = now;
    z.yaw = F<float>(Vars(e), EV_VANGLE + 4);
    z.pitch = 0;
    z.stuckFrom = Origin(e);
    z.stuckCheck = now + 1.0f;
}

static void DriveZgPlayer(ZgPlayer& z, float now, float dt)
{
    void* e = z.edict;
    unsigned char* v = Vars(e);
    unsigned char* pl = (unsigned char*)PrivateOf(e);
    if (!pl || !(F<int>(v, EV_FLAGS) & FL_CLIENT))
    {
        Log("Zombie Giant: '%s' left the game (flags 0x%x, private %p)", z.name.c_str(), F<int>(v, EV_FLAGS), pl);
        z.edict = nullptr;
        return;
    }
    if (!IsZgPlayerClass(pl))
    {
        static int reports = 0;
        if (reports++ < 10)
            Log("Zombie Giant: '%s' is now a 0x%08lx (not a Zombie Giant player): not driven", z.name.c_str(),
                *(DWORD*)pl - g_mpDelta);
        return;
    }
    float fwd = 0, side = 0;
    unsigned short buttons = 0;
    *(float*)(pl + PLAYER_LAST_MOVEMENT) = now; // never idle-kicked
    // respawn (0x113fa200) counts the respawns of players the room doesn't know (+0x2f54) and marks every 3rd one
    // for a kick (0x1218bce8, done by StartFrame through the room manager)
    *(int*)(pl + 0x2f54) = 0;
    if (!(*(int*)(pl + PLAYER_TEAM) == 1 && *(int*)(pl + ZG_GIANT_TYPE) != 0))
        UpdateBuying(e, StateFor(e)); // the room's weapons, like the bots (not as the giant)
    int team = *(int*)(pl + PLAYER_TEAM);
    int type = *(int*)(pl + ZG_GIANT_TYPE);
    bool giant = team == 1 && (type == 1 || type == 2 || type == 4 || type == 6);
    if (!Alive(e))
    {
        if (z.wasAlive)
            z.deadSince = now;
        z.wasAlive = false;
        if (now - z.deadSince > 3.0f && now >= z.nextRespawn)
        {
            FakeCommand(e, "respawn");
            z.nextRespawn = now + 3.0f;
        }
    }
    else
    {
        if (!z.wasAlive)
        {
            ZgSpawned(e, z, now);
            static int reports = 0;
            if (reports++ < 30)
                Log("Zombie Giant: '%s' spawns as %s (type %d, team %d)", z.name.c_str(), giant ? "the giant" : "a soldier",
                    type, team);
        }
        if (!giant)
            ZgUnfreeze(e, "fake player");
        ZgThink(e, z, giant, team, now, dt, fwd, side, buttons);
        int next = *(int*)(pl + 0x3718);
        if ((team == 1 || next) && now >= z.nextPickReport)
        {
            z.nextPickReport = now + 0.5f;
            Log("ZG picked '%s': team %d, type %d, next %d, timer %.1f (now %.1f), z %.0f, vz %.0f, health %.0f, deadflag %.0f, "
                "flags 0x%x, +0xe10 %d, +0x118 %d, +0x120 %d, +0x3704 %d",
                z.name.c_str(), team, type, next, *(float*)(pl + 0x3724), now, Origin(e).z,
                F<float>(v, EV_VELOCITY + 8), F<float>(v, EV_HEALTH), F<float>(v, EV_DEADFLAG), F<int>(v, EV_FLAGS),
                *(int*)(pl + 0xe10), F<int>(v, 0x118), F<int>(v, 0x120), pl[0x3704]);
        }
        if (now >= z.nextReport)
        {
            z.nextReport = now + 15.0f;
            DLog("ZG '%s' %s at %.0f %.0f %.0f (speed %.0f, move %d fwd %.0f side %.0f), target %s (team %d, %.0f u), "
                 "%d allies", z.name.c_str(), giant ? "giant" : "soldier", Origin(e).x, Origin(e).y, Origin(e).z,
                 Len2D(Vec((float*)(v + EV_VELOCITY))), z.lastMove, z.lastFwd, z.lastSide,
                 z.target ? String(F<int>(Vars(z.target), EV_NETNAME)) : "none", z.target ? TeamOf(z.target) : 0,
                 z.target ? Len(Origin(z.target) - Origin(e)) : 0.0f, g_zgAllies);
        }
    }
    float ang[3] = { z.pitch, AngleDiff(z.yaw, 0.0f), 0 };
    float* va = (float*)(v + EV_VANGLE);
    float* an = (float*)(v + EV_ANGLES);
    va[0] = ang[0];
    va[1] = ang[1];
    va[2] = 0;
    an[0] = ang[0] / -3.0f;
    an[1] = ang[1];
    an[2] = 0;
    int ms = (int)(dt * 1000.0f);
    g_RunPlayerMove(e, ang, fwd, side, 0, buttons, 0, (unsigned char)(ms < 1 ? 1 : ms > 100 ? 100 : ms));
}

// a room bot on the human side in Zombie Giant: the stock AI stands still there, so botassist drives it like the
// fake soldiers (true: fwd / side / buttons / aim replaced)
static bool DriveZgBot(void* bot, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (GameMode() != MODE_ZOMBIE_GIANT || TeamOf(bot) != 2 || !Alive(bot))
    {
        auto it = g_zgBots.find(bot);
        if (it != g_zgBots.end())
            it->second.wasAlive = false;
        return false;
    }
    float now = Now();
    ZgPlayer& z = g_zgBots[bot];
    if (!z.wasAlive || now < z.lastThink - 1.0f || now - z.lastThink > 2.0f)
        ZgSpawned(bot, z, now);
    float dt = now - z.lastThink;
    dt = dt <= 0 || dt > 0.25f ? 0.02f : dt;
    z.lastThink = now;
    float f = 0, s = 0;
    unsigned short b = 0;
    ZgUnfreeze(bot, "bot");
    ZgThink(bot, z, false, 2, now, dt, f, s, b);
    yaw = AngleDiff(z.yaw, 0.0f);
    pitch = z.pitch;
    fwd = f;
    side = s;
    buttons = (unsigned short)((buttons & ~(IN_ATTACK | IN_JUMP | IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT)) | b);
    if (now >= z.nextReport)
    {
        z.nextReport = now + 15.0f;
        DLog("ZG bot '%s' at %.0f %.0f %.0f (speed %.0f, move %d fwd %.0f side %.0f), target %s (%.0f u)",
             String(F<int>(Vars(bot), EV_NETNAME)), Origin(bot).x, Origin(bot).y, Origin(bot).z,
             Len2D(Vec((float*)(Vars(bot) + EV_VELOCITY))), z.lastMove, f, s,
             z.target ? String(F<int>(Vars(z.target), EV_NETNAME)) : "none",
             z.target ? Len(Origin(z.target) - Origin(bot)) : 0.0f);
    }
    return true;
}

// once per frame, from the bots' RunPlayerMove
static void UpdateZgPlayers()
{
    float now = Now();
    if (now == g_zgFrame)
        return;
    float dt = g_zgFrame < 0 || now < g_zgFrame || now - g_zgFrame > 0.25f ? 0.02f : now - g_zgFrame;
    g_zgFrame = now;
    if (g_zgLevel != g_nav.level)
    {
        ZgNewLevel();
        g_zgBots.clear();
        g_zgNextRally = 0;
        g_zgLevel = g_nav.level;
        g_zgStart = now;
    }
    if (GameMode() != MODE_ZOMBIE_GIANT || g_edictToVars < 0)
        return;
    ZgScanWorld();
    ZgUpdateTeam(now);
    if (!g_CreateFakeClient || !g_FreeEntPrivateData)
        return;
    MarkZgPlayers();
    if (g_zgPlayers.empty() && g_zgMarkLevel == g_nav.level)
    {
        EnterCriticalSection(&g_matchLock);
        for (auto& m : g_matchBots)
            if (m.fakePlayer)
            {
                ZgPlayer z;
                z.name = m.name;
                z.character = m.character;
                g_zgPlayers.push_back(z);
            }
        LeaveCriticalSection(&g_matchLock);
    }
    if (now - g_zgStart > 2.0f)
        for (auto& z : g_zgPlayers)
            if (!z.tried)
            {
                CreateZgPlayer(z);
                break; // one per frame
            }
    for (auto& z : g_zgPlayers)
        if (z.edict)
            DriveZgPlayer(z, now, dt);
}

// ---------------------------------------------------------------- Zombie Giant debugging (botassist.log)
// Who connects and disconnects (detours of ClientPutInServer 0x113efbb0 and ClientDisconnect 0x113ef8c0, with the
// return addresses on the stack: "mp:<address at mp.dll's preferred base>", "hw+<offset>"), every server command
// with "kick", the giant changes of every Zombie Giant player, and every 15 s each player's movement state.
static const DWORD MP_CLIENTDISCONNECT = 0x113ef8c0;
static DWORD g_hwLo, g_hwHi, g_mpLo, g_mpHi;
static EdictFn g_ClientDisconnectTramp, g_PutInServerTramp;
static ServerCommandFn g_ServerCommandOrig;
static std::unordered_map<void*, int> g_zgSeenState; // edict -> team | type << 8 | next type << 16
static float g_zgNextCensus = 0;

static void ModuleRange(const char* name, DWORD& lo, DWORD& hi)
{
    HMODULE m = GetModuleHandleA(name);
    if (!m)
        return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    lo = (DWORD)m;
    hi = lo + nt->OptionalHeader.SizeOfImage;
}

// return addresses into mp.dll / hw.dll found on the stack (a call instruction just before them)
static void StackTrace(char* out, size_t size)
{
    out[0] = 0;
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    DWORD* top = (DWORD*)((NT_TIB*)NtCurrentTeb())->StackBase;
    size_t used = 0;
    int n = 0;
    for (int i = 0; i < 4096 && sp + i < top && n < 16; i++)
    {
        DWORD a = sp[i];
        bool mp = a >= g_mpLo + 6 && a < g_mpHi, hw = a >= g_hwLo + 6 && a < g_hwHi;
        if (!mp && !hw)
            continue;
        const BYTE* p = (const BYTE*)a;
        bool call = p[-5] == 0xe8 || (p[-6] == 0xff && (p[-5] & 0x38) == 0x10) ||
                    (p[-2] == 0xff && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xff && (p[-2] & 0x38) == 0x10);
        if (!call)
            continue;
        int w = mp ? snprintf(out + used, size - used, " mp:%08lx", a - g_mpDelta)
                   : snprintf(out + used, size - used, " hw+%lx", a - g_hwLo);
        if (w <= 0 || used + w >= size)
            break;
        used += w;
        n++;
    }
}

static const char* ZgWho(void* e)
{
    for (auto& z : g_zgPlayers)
        if (z.edict == e)
            return "fake player";
    void* pl = PrivateOf(e);
    if (IsBotClass(pl))
        return "bot";
    if (pl && (F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) && !(F<int>(Vars(e), EV_FLAGS) & FL_FAKECLIENT))
        return "you";
    return "?";
}

static void LogClientEvent(const char* what, void* e)
{
    char trace[512];
    StackTrace(trace, sizeof(trace));
    unsigned char* v = Vars(e);
    void* pl = PrivateOf(e);
    unsigned char* p = (unsigned char*)pl;
    Log("CLIENT %s: '%s' (%s, flags 0x%x, team %d, class 0x%08lx, giant %d next %d) at %.1f; stack:%s", what,
        String(F<int>(v, EV_NETNAME)), ZgWho(e), F<int>(v, EV_FLAGS), p ? *(int*)(p + PLAYER_TEAM) : -1,
        p ? *(DWORD*)p - g_mpDelta : 0, p && IsZgPlayerClass(p) ? *(int*)(p + ZG_GIANT_TYPE) : -1,
        p && IsZgPlayerClass(p) ? *(int*)(p + 0x3718) : -1, Now(), trace);
}

static void HookedClientDisconnect(void* e)
{
    __try { if (e && g_edictToVars >= 0 && ZgActive()) LogClientEvent("disconnects", e); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ClientDisconnectTramp(e);
}

static void HookedPutInServer(void* e)
{
    g_PutInServerTramp(e);
    __try { if (e && g_edictToVars >= 0 && ZgActive()) LogClientEvent("put in server", e); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void LogServerCommand(const char* cmd)
{
    char trace[512];
    StackTrace(trace, sizeof(trace));
    std::string c = cmd;
    while (!c.empty() && (c.back() == '\n' || c.back() == '\r'))
        c.pop_back();
    Log("SERVER COMMAND '%s'; stack:%s", c.c_str(), trace);
}

static void HookedServerCommand(const char* cmd)
{
    __try
    {
        static int reports = 0;
        if (cmd && ZgActive() && (strstr(cmd, "kick") || reports < 40))
        {
            reports++;
            LogServerCommand(cmd);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ServerCommandOrig(cmd);
}

// pev flags worth naming in the census
static std::string FlagNames(int f)
{
    static const struct { int bit; const char* name; } names[] = {
        { 1, "FLY" }, { 2, "SWIM" }, { 8, "CLIENT" }, { 0x10, "INWATER" }, { 0x20, "MONSTER" }, { 0x40, "GODMODE" },
        { 0x80, "NOTARGET" }, { 0x200, "ONGROUND" }, { 0x1000, "FROZEN" }, { 0x2000, "FAKECLIENT" }, { 0x4000, "DUCKING" },
        { 0x40000, "IMMUNE_WATER" }, { 0x100000, "PROXY" }, { 0x200000, "ALWAYSTHINK" }, { 0x40000000, "KILLME" },
        { (int)0x80000000, "DORMANT" } };
    std::string s;
    for (auto& n : names)
        if (f & n.bit)
            s += std::string(s.empty() ? "" : "|") + n.name;
    return s;
}

// giant changes of everyone, and a movement census every 15 s (from ZgUpdateTeam, for each client)
static void ZgWatchClient(void* e, unsigned char* pl, float now, bool census)
{
    unsigned char* v = Vars(e);
    int team = *(int*)(pl + PLAYER_TEAM);
    bool zg = IsZgPlayerClass(pl);
    int type = zg ? *(int*)(pl + ZG_GIANT_TYPE) : 0, next = zg ? *(int*)(pl + 0x3718) : 0;
    int state = (team & 0xff) | (type & 0xff) << 8 | (next & 0xff) << 16;
    auto it = g_zgSeenState.find(e);
    if (it == g_zgSeenState.end() || it->second != state)
    {
        Log("ZG state '%s' (%s): team %d, giant type %d, next %d (times giant %d, last %.1f) at %.1f, position %.0f %.0f %.0f",
            String(F<int>(v, EV_NETNAME)), ZgWho(e), team, type, next, zg ? *(int*)(pl + 0x3708) : 0,
            zg ? *(float*)(pl + 0x370c) : 0.0f, now, Origin(e).x, Origin(e).y, Origin(e).z);
        g_zgSeenState[e] = state;
        // the giant spawns on one of the rules' giant spots (0x1085fa90: rules +0x260..+0x264, those with no human
        // near); with none left it reads past the list and the giant lands outside the map
        unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
        if (rules && (!Readable(rules, 4) || *(DWORD*)rules != MP_ZG_RULES_VTABLE + g_mpDelta))
            rules = nullptr;
        if ((type == 1 || type == 2 || type == 4 || type == 6) && rules && Readable(rules + 0x260, 8))
        {
            void** spot = *(void***)(rules + 0x260);
            void** end = *(void***)(rules + 0x264);
            float best = 1e30f;
            int n = 0;
            for (; spot && spot < end && n < 256 && Readable(spot, 4); spot++, n++)
            {
                unsigned char* sp = (unsigned char*)*spot;
                unsigned char* spev = sp && Readable(sp + 8, 4) ? *(unsigned char**)(sp + 8) : nullptr;
                if (spev && Readable(spev + 8, 12))
                {
                    float d = Len2D(Vec((float*)(spev + 8)) - Origin(e));
                    best = d < best ? d : best;
                }
            }
            Log("ZG giant '%s' spawned %.0f units from the closest of %d giant spots%s", String(F<int>(v, EV_NETNAME)), best,
                n, best > 200.0f ? ": OUTSIDE THE SPAWN POINTS" : "");
        }
    }
    if (census)
        DLog("ZG census '%s' (%s): team %d, %s, health %.0f, weapon %s, flags %s, maxspeed %.0f, speed %.0f, buttons 0x%x, at %.0f %.0f %.0f",
             String(F<int>(v, EV_NETNAME)), ZgWho(e), team, zg ? "ZombieBossPlayer" : IsBotClass(pl) ? "bot class" : "other class",
             F<float>(v, EV_HEALTH), String(F<int>(v, EV_WEAPONMODEL)), FlagNames(F<int>(v, EV_FLAGS)).c_str(), F<float>(v, 0x240),
             Len2D(Vec((float*)(v + EV_VELOCITY))), F<int>(v, 0x1a4), Origin(e).x, Origin(e).y, Origin(e).z);
}

static void* DetourCdecl(unsigned char* fn, int len, void* hook)
{
    unsigned char* tramp = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return nullptr;
    memcpy(tramp, fn, len);
    tramp[len] = 0xe9;
    *(int*)(tramp + len + 1) = (int)((fn + len) - (tramp + len + 5));
    DWORD old;
    VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xe9;
    *(int*)(fn + 1) = (int)((unsigned char*)hook - (fn + 5));
    for (int i = 5; i < len; i++)
        fn[i] = 0x90;
    VirtualProtect(fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, len);
    return tramp;
}

static void InstallZgDebug(DWORD delta)
{
    ModuleRange("hw.dll", g_hwLo, g_hwHi);
    ModuleRange("mp.dll", g_mpLo, g_mpHi);
    unsigned char* disc = (unsigned char*)(MP_CLIENTDISCONNECT + delta);
    unsigned char* put = (unsigned char*)(MP_PUT_IN_SERVER + delta);
    static const unsigned char discCode[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c };
    static const unsigned char putCode[] = { 0x53, 0x8b, 0xdc, 0x83, 0xec, 0x08 };
    if (!memcmp(disc, discCode, 6))
        g_ClientDisconnectTramp = (EdictFn)DetourCdecl(disc, 6, (void*)HookedClientDisconnect);
    if (!memcmp(put, putCode, 6))
        g_PutInServerTramp = (EdictFn)DetourCdecl(put, 6, (void*)HookedPutInServer);
    void** table = (void**)(MP_ENGFUNCS + delta);
    if (table[ENG_SERVERCOMMAND] != (void*)HookedServerCommand)
    {
        g_ServerCommandOrig = (ServerCommandFn)table[ENG_SERVERCOMMAND];
        DWORD old;
        VirtualProtect(&table[ENG_SERVERCOMMAND], 4, PAGE_READWRITE, &old);
        table[ENG_SERVERCOMMAND] = (void*)HookedServerCommand;
        VirtualProtect(&table[ENG_SERVERCOMMAND], 4, old, &old);
    }
    Log("debug: client connect/disconnect %s, server commands logged (hw.dll %08lx..%08lx)",
        g_ClientDisconnectTramp && g_PutInServerTramp ? "logged" : "NOT hooked (unexpected code)", g_hwLo, g_hwHi);
}

// ---------------------------------------------------------------- Zombie Giant: who becomes the giant, and staying in
// The game sorts the candidates (the human CTs) with 0x1085f810 (times giant +0x3708, then +0x370c: the time they
// joined or were last the giant) and takes the first ones: you, the first to join, were always first. The sort call
// (0x1086208a: push 0x1085f810) gets ZgGiantOrder instead: a random draw for each pick (the same keys during one
// sort, so the order stays consistent), you last when ZombieGiantYouCanBeGiant=0.
// A fake player that became the giant was dropped ~10 s later, when the mode transformed it: it sends the new giant
// many reliable messages at once, and a fake client's reliable channel (never sent) overflowed: hw.dll drops
// overflowed clients ("Reliable channel overflowed", rva 0x786a06). Reliable messages to fake players go unreliable
// (MSG_ONE -> MSG_ONE_UNRELIABLE): the unreliable buffer just discards what doesn't fit.
// The fake players are driven from the end of mp.dll's StartFrame (DLL function 0x113f8a80) too, so they play
// when the room has no bots left (every room AI player is a fake player here).
static const DWORD MP_GIANT_SORT_PUSH = 0x1086208a, MP_GIANT_ORDER = 0x1085f810, MP_STARTFRAME = 0x113f8a80;
static const int MSG_ONE = 1, MSG_ONE_UNRELIABLE = 8;
static std::unordered_map<void*, float> g_zgGiantKeys;
static float g_zgKeysAt = -100;
static MessageBeginFn g_MessageBeginOrig;
static void (*g_StartFrameTramp)();

static float ZgGiantKey(unsigned char* pl)
{
    auto it = g_zgGiantKeys.find(pl);
    if (it != g_zgGiantKeys.end())
        return it->second;
    float key = Rnd();
    unsigned char* pev = pl ? *(unsigned char**)(pl + 8) : nullptr;
    int flags = pev ? F<int>(pev, EV_FLAGS) : 0;
    bool you = (flags & FL_CLIENT) && !(flags & FL_FAKECLIENT);
    if (you && !cfg.zgPlayerGiant)
        key += 2.0f;
    g_zgGiantKeys[pl] = key;
    return key;
}

static bool __cdecl ZgGiantOrder(unsigned char* a, unsigned char* b)
{
    float now = Now();
    if (now - g_zgKeysAt > 0.5f || now < g_zgKeysAt)
    {
        g_zgGiantKeys.clear(); // a new pick: a new draw
        g_zgKeysAt = now;
        Log("Zombie Giant: drawing the giant at random");
    }
    return ZgGiantKey(a) < ZgGiantKey(b);
}

static bool IsZgFakeEdict(void* e)
{
    for (auto& z : g_zgPlayers)
        if (z.edict == e)
            return true;
    return false;
}

static void HookedMessageBegin(int dest, int type, const float* origin, void* ed)
{
    if (dest == MSG_ONE && ed && !g_zgPlayers.empty() && IsZgFakeEdict(ed))
    {
        dest = MSG_ONE_UNRELIABLE;
        static int reports = 0;
        if (reports++ < 3)
            Log("Zombie Giant: reliable messages to fake players go unreliable (message %d)", type);
    }
    g_MessageBeginOrig(dest, type, origin, ed);
}

static void ConsoleFrame();
static void ZSRiftWatch();
static void CommandFile();

static void HookedStartFrame()
{
    g_StartFrameTramp();
    __try
    {
        ConsoleFrame();
        CommandFile();
        ZSRiftWatch();
        if (cfg.enabled && g_edictToVars >= 0 && g_pGlobals && *g_pGlobals && (GameMode(), ZgActive()))
        {
            LoadNav();
            UpdateMatchBots();
            UpdateZgPlayers();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("exception 0x%08lX in the frame hook (ignored)", GetExceptionCode());
    }
}

static void InstallZgPlayers(DWORD delta)
{
    unsigned char* push = (unsigned char*)(MP_GIANT_SORT_PUSH + delta);
    bool order = push[0] == 0x68 && *(DWORD*)(push + 1) == MP_GIANT_ORDER + delta;
    if (order)
    {
        DWORD old;
        VirtualProtect(push + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        *(DWORD*)(push + 1) = (DWORD)ZgGiantOrder;
        VirtualProtect(push + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), push, 5);
    }
    void** table = (void**)(MP_ENGFUNCS + delta);
    if (table[ENG_MESSAGEBEGIN] != (void*)HookedMessageBegin)
    {
        g_MessageBeginOrig = (MessageBeginFn)table[ENG_MESSAGEBEGIN];
        DWORD old;
        VirtualProtect(&table[ENG_MESSAGEBEGIN], 4, PAGE_READWRITE, &old);
        table[ENG_MESSAGEBEGIN] = (void*)HookedMessageBegin;
        VirtualProtect(&table[ENG_MESSAGEBEGIN], 4, old, &old);
    }
    unsigned char* sf = (unsigned char*)(MP_STARTFRAME + delta);
    static const unsigned char sfCode[] = { 0x8b, 0x0d };
    if (!memcmp(sf, sfCode, 2))
        g_StartFrameTramp = (void (*)())DetourCdecl(sf, 6, (void*)HookedStartFrame);
    Log("Zombie Giant: giant drawn at random %s, fake players' reliable messages unreliable, frame hook %s",
        order ? "on" : "NOT hooked (unexpected code)", g_StartFrameTramp ? "on" : "NOT hooked");
}

// ---------------------------------------------------------------- Zombie Giant: fake players stay in the game
// hw.dll dropped the fake players when they died or became the giant (SV_DropClient, rva 0x71ba90: client_t*,
// crash, reason format...; the client's name is at client_t +0xa3e0). Every drop is logged with its reason and the
// exact caller; a fake player is only let go on a level change, at the end of the session or when botassist kicks it.
static const DWORD HW_DROPCLIENT = 0x71ba90;
static const int HW_CLIENT_NAME = 0xa3e0;
typedef void (*DropClientFn)(void* cl, int crash, const char* fmt, DWORD a1, DWORD a2, DWORD a3, DWORD a4);
static DropClientFn g_DropClientTramp;

static bool ZgKeepClient(void* cl, const char* fmt, DWORD ret)
{
    const char* name = (const char*)cl + HW_CLIENT_NAME;
    bool fake = false;
    for (auto& z : g_zgPlayers)
        fake |= z.edict && z.name == name;
    auto own = std::find(g_zgKickNames.begin(), g_zgKickNames.end(), std::string(name));
    bool ours = own != g_zgKickNames.end();
    if (ours)
        g_zgKickNames.erase(own);
    std::string reason = fmt ? fmt : "";
    bool allowed = !fake || ours || reason.find("level change") != std::string::npos ||
                   reason.find("Session End") != std::string::npos || reason.find("Client leave") != std::string::npos ||
                   reason.find("shutting down") != std::string::npos;
    // each name and reason once (some callers retry every frame)
    static std::vector<std::string> seen;
    std::string key = std::string(name) + "|" + reason;
    bool fresh = std::find(seen.begin(), seen.end(), key) == seen.end();
    if (fresh && seen.size() < 200)
        seen.push_back(key);
    if (allowed || fresh)
        Log("ENGINE DROPS '%s' (%s): reason '%s', from hw+%lx%s", name, fake ? "fake player" : "client", reason.c_str(),
            ret - g_hwLo, allowed ? "" : ": KEPT (a fake player)");
    return !allowed;
}

// Zombie Disaster Zone: two of the room's three bots left within a second of joining, with nothing on the game's
// side. In that mode the bots' drops and room kicks are logged with their caller, and the bots stay like Zombie
// Giant's fake players (gone only on a level change, at the end of the session or when they leave the room).
static const int MODE_ZSRIFT = 59;
static void* BotWhere(std::function<bool(void*)> match)
{
    void* found = nullptr;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        if (!found && (F<int>(Vars(e), EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) == (FL_CLIENT | FL_FAKECLIENT) && match(e))
            found = e;
    });
    return found;
}

static bool ZsrKeepBot(void* cl, const char* fmt, DWORD ret)
{
    const char* name = (const char*)cl + HW_CLIENT_NAME;
    if (!BotWhere([&](void* e) { return !strcmp(String(F<int>(Vars(e), EV_NETNAME)), name); }))
        return false;
    std::string reason = fmt ? fmt : "";
    bool allowed = reason.find("level change") != std::string::npos || reason.find("Session End") != std::string::npos ||
                   reason.find("Client leave") != std::string::npos || reason.find("shutting down") != std::string::npos;
    static std::vector<std::string> seen;
    std::string key = std::string(name) + "|" + reason;
    if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 200)
    {
        seen.push_back(key);
        Log("ZSRift: the engine drops bot '%s': reason '%s', from hw+%lx%s", name, reason.c_str(), ret - g_hwLo,
            allowed ? "" : ": KEPT");
    }
    return !allowed;
}

static void HookedDropClient(void* cl, int crash, const char* fmt, DWORD a1, DWORD a2, DWORD a3, DWORD a4)
{
    DWORD ret = (DWORD)_ReturnAddress();
    bool keep = false;
    __try { keep = cl && ZgActive() && ZgKeepClient(cl, fmt, ret); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { keep = keep || (cl && GameMode() == MODE_ZSRIFT && ZsrKeepBot(cl, fmt, ret)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!keep)
        g_DropClientTramp(cl, crash, fmt, a1, a2, a3, a4);
}

// The fake players picked as the giant (moved to the terrorists) were kicked every frame through hw.dll's
// CGameRoomManager::KickPlayer(player index) (vtable 0x2c186f4 slot 10 -> 0x247aa10 -> SV_DropClient "Kicked"):
// the room doesn't know them. That slot is replaced: the caller is logged once, fake players are not kicked.
static const DWORD HW_ROOMMANAGER_VT = 0x2c186f4 - 0x1d00000, HW_ROOMKICK_SLOT = 10;
typedef void(__fastcall* RoomKickFn)(void* self, void* edx, int index);
static RoomKickFn g_RoomKick;

static bool ZgRoomKickBlocked(int index, DWORD ret)
{
    void* e = nullptr;
    for (auto& z : g_zgPlayers)
        if (z.edict && g_IndexOfEdict && g_IndexOfEdict(z.edict) == index)
            e = z.edict;
    static std::vector<DWORD> seen;
    if (std::find(seen.begin(), seen.end(), ret) == seen.end() && seen.size() < 50)
    {
        seen.push_back(ret);
        char from[32];
        if (ret >= g_mpLo && ret < g_mpHi)
            snprintf(from, sizeof(from), "mp:%08lx", ret - g_mpDelta);
        else if (ret >= g_hwLo && ret < g_hwHi)
            snprintf(from, sizeof(from), "hw+%lx", ret - g_hwLo);
        else
            snprintf(from, sizeof(from), "%08lx", ret);
        Log("ROOM KICK player %d (%s) from %s%s", index, e ? String(F<int>(Vars(e), EV_NETNAME)) : "not a fake player",
            from, e ? ": refused (a fake player)" : "");
    }
    return e != nullptr;
}

static bool ZsrRoomKickBlocked(int index, DWORD ret)
{
    void* bot = BotWhere([&](void* e) { return g_IndexOfEdict && g_IndexOfEdict(e) == index; });
    static std::vector<DWORD> seen;
    if (bot && std::find(seen.begin(), seen.end(), ret) == seen.end() && seen.size() < 50)
    {
        seen.push_back(ret);
        Log("ZSRift: the room kicks bot '%s' (player %d) from %s+%lx: refused", String(F<int>(Vars(bot), EV_NETNAME)),
            index, ret >= g_mpLo && ret < g_mpHi ? "mp" : "hw", ret >= g_mpLo && ret < g_mpHi ? ret - g_mpDelta : ret - g_hwLo);
    }
    return bot != nullptr;
}

static void __fastcall HookedRoomKick(void* self, void* edx, int index)
{
    DWORD ret = (DWORD)_ReturnAddress();
    bool blocked = false;
    __try { blocked = ZgActive() && ZgRoomKickBlocked(index, ret); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { blocked = blocked || (GameMode() == MODE_ZSRIFT && ZsrRoomKickBlocked(index, ret)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!blocked)
        g_RoomKick(self, edx, index);
}

static void InstallZgDropGuard()
{
    if (g_hwLo)
    {
        void** slot = (void**)(g_hwLo + HW_ROOMMANAGER_VT) + HW_ROOMKICK_SLOT;
        if (Readable(slot, 4) && *slot == (void*)(g_hwLo + 0x2601990 - 0x1d00000))
        {
            g_RoomKick = (RoomKickFn)*slot;
            PatchSlot(slot, (void*)HookedRoomKick);
        }
        Log("Zombie Giant: room kicks %s", g_RoomKick ? "watched, fake players kept" : "NOT hooked (unexpected code)");
    }
    if (!g_hwLo)
        return;
    unsigned char* fn = (unsigned char*)(g_hwLo + HW_DROPCLIENT);
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x04, 0x06, 0x00, 0x00 };
    if (!memcmp(fn, code, sizeof(code)))
        g_DropClientTramp = (DropClientFn)DetourCdecl(fn, sizeof(code), (void*)HookedDropClient);
    Log("Zombie Giant: engine drops %s", g_DropClientTramp ? "logged, fake players kept" : "NOT hooked (unexpected code)");
}

// The classic crossbow (item 8977, CCrossBowCls) crashed the game on every shot: its bolt class
// ("crossbowcls_bolt", vfunc 0x344 0x10c43cc0) isn't in this mp.dll, so CBaseEntity::Create returns null and its
// attack (0x1100de40, CCrossbow's) writes through it (mp+0xc49926). It fires CCrossbow's "crossbow_bolt"
// (0x1100ead0) instead.
static const DWORD MP_CROSSBOWCLS_VTABLE = 0x1183991c, MP_CROSSBOWCLS_BOLT = 0x10c43cc0, MP_CROSSBOW_BOLT = 0x1100ead0;
static void FixCrossbowCls(DWORD delta)
{
    void** slot = (void**)(MP_CROSSBOWCLS_VTABLE + delta + 0x344);
    bool ok = Readable(slot, 4) && *slot == (void*)(MP_CROSSBOWCLS_BOLT + delta);
    if (ok)
        PatchSlot(slot, (void*)(MP_CROSSBOW_BOLT + delta));
    Log("classic crossbow: %s", ok ? "fires crossbow_bolt (its own bolt class doesn't exist)" : "NOT patched (unexpected code)");
}

static void HookBotManager(DWORD delta)
{
    FixCrossbowCls(delta);
    DetourBotAddCommand(delta);
    HookZombieGiant(delta);
    InstallZgDebug(delta);
    InstallZgPlayers(delta);
    InstallZgDropGuard();
    DetourChooseAppearance(delta);
    for (DWORD vt : MP_ZBBOTMGR_VTABLES)
    {
        void** slot = (void**)(vt + delta) + 2;
        if (!Readable(slot, 4))
            continue;
        if (*slot == (void*)HookedAddBot)
            continue;
        if (*slot != (void*)(MP_ZBBOTMGR_ADDBOT + delta))
        {
            Log("bot manager vtable 0x%08lX: unexpected AddBot, not hooked", vt);
            continue;
        }
        g_AddBot = (AddBotFn)*slot;
        DWORD old;
        VirtualProtect(slot, 4, PAGE_READWRITE, &old);
        *slot = (void*)HookedAddBot;
        VirtualProtect(slot, 4, old, &old);
    }
    if (g_AddBot)
        Log("bot manager hooked: no terrorist bots in co-op modes");
}

// ---------------------------------------------------------------- console: cheats and bot_add
// Nexon's hw.dll keeps god, noclip, notarget, fly and kill registered but empty (each handler is a bare "ret").
// botassist puts its own handlers in those entries of the engine's command list (head at hw rva 0x14aa068, entries
// {next, name, function, flags}) and adds "give" (engine function 146, AddServerCommand). Like in CS 1.6 they act on
// your player in the server the game hosts, and all but kill need sv_cheats 1. A toggle is applied every server frame
// (the game resets movetype and takedamage on respawn) until it is turned off or a new map starts.
// "bot_add", "bot_add_t" and "bot_add_ct" (mp.dll's bot command handler 0x11317980) go past the room's AI players:
// g_consoleBotAdd is set while they run. The list is checked every 2 s (game commands come and go with mp.dll).
static const DWORD HW_CMD_LIST = 0x31aa068 - 0x1d00000, HW_CON_PRINTF = 0x23e0d70 - 0x1d00000;
static const DWORD MP_BOT_COMMAND = 0x11317980, MP_CLIENTKILL = 0x113efaf0;
static const int ENG_PENTITYOFENTINDEX = 77, ENG_ADDSERVERCOMMAND = 146, GV_MAXCLIENTS = 0x90, EV_MOVETYPE = 0x118;
static const int FL_GODMODE = 0x40, FL_NOTARGET = 0x80, MOVETYPE_WALK = 3, MOVETYPE_FLY = 5, MOVETYPE_NOCLIP = 8;
static const float DAMAGE_NO = 0.0f, DAMAGE_AIM = 2.0f;
struct CmdFunction
{
    CmdFunction* next;
    const char* name;
    void (*fn)();
    int flags;
};
typedef void* (*EntityOfIndexFn)(int index);
typedef void (*ConPrintfFn)(const char* fmt, ...);
typedef void (*AddServerCommandFn)(const char* name, void (*fn)());
static struct
{
    bool god, noclip, notarget, fly;
    float lastFrame = -1; // gpGlobals->time of the last server frame (it starts again with each map)
    DWORD lastTick = 0;   // GetTickCount of the last server frame: is a match running?
} g_cheats;
static float g_consoleNextCheck = -1;
static void CalibrateEdict(void* edict);
static bool HttpPost(const wchar_t* path, const char* body, std::string* reply = nullptr);
static volatile LONG g_cheatsReported = 0; // 0 not yet in this match, 1 sending or told

// sv_cheats is on: CSNZ_Server cuts this match's medals (POST /api/cheats); a failed call is tried again later
static DWORD WINAPI ReportCheats(LPVOID)
{
    bool told = HttpPost(L"/api/cheats", "{}");
    Log("sv_cheats is on: %s", told ? "the server cuts this match's medals" : "server not reachable, trying again");
    if (!told)
        InterlockedExchange(&g_cheatsReported, 0);
    return 0;
}

static void Con(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_hwLo)
        ((ConPrintfFn)(g_hwLo + HW_CON_PRINTF))("%s", buf);
}

// your player: the first client that isn't a bot or a fake player
static void* YourEdict()
{
    if (!g_hwLo || !g_mpDelta || !g_pGlobals || !*g_pGlobals || GetTickCount() - g_cheats.lastTick > 2000)
        return nullptr;
    void* found = nullptr;
    __try
    {
        auto entOf = (EntityOfIndexFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_PENTITYOFENTINDEX];
        int maxClients = *(int*)((DWORD)*g_pGlobals + GV_MAXCLIENTS);
        for (int i = 1; i <= maxClients && i <= 64 && !found; i++)
        {
            void* e = entOf(i);
            if (!e)
                continue;
            if (g_edictToVars < 0)
                CalibrateEdict(e);
            if (g_edictToVars < 0)
                return nullptr;
            int flags = F<int>(Vars(e), EV_FLAGS);
            if ((flags & FL_CLIENT) && !(flags & FL_FAKECLIENT) && PrivateOf(e))
                found = e;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        found = nullptr;
    }
    return found;
}

static void* CheatTarget(bool cheat)
{
    void* e = YourEdict();
    if (!e)
        Con("Only in a match you host.\n");
    else if (cheat && g_CVarGetFloat && g_CVarGetFloat("sv_cheats") == 0.0f)
    {
        Con("Cheats are off: type sv_cheats 1 first.\n");
        e = nullptr;
    }
    return e;
}

static void SetMovetype(void* e)
{
    F<int>(Vars(e), EV_MOVETYPE) = g_cheats.noclip ? MOVETYPE_NOCLIP : g_cheats.fly ? MOVETYPE_FLY : MOVETYPE_WALK;
}

static void CmdGod()
{
    void* e = CheatTarget(true);
    if (!e)
        return;
    g_cheats.god = !g_cheats.god;
    unsigned char* v = Vars(e);
    if (g_cheats.god)
    {
        F<int>(v, EV_FLAGS) |= FL_GODMODE;
        F<float>(v, EV_TAKEDAMAGE) = DAMAGE_NO;
    }
    else
    {
        F<int>(v, EV_FLAGS) &= ~FL_GODMODE;
        if (F<float>(v, EV_TAKEDAMAGE) == DAMAGE_NO)
            F<float>(v, EV_TAKEDAMAGE) = DAMAGE_AIM;
    }
    Con("godmode %s\n", g_cheats.god ? "ON" : "OFF");
    Log("console: godmode %s", g_cheats.god ? "on" : "off");
}

static void CmdNoclip()
{
    void* e = CheatTarget(true);
    if (!e)
        return;
    g_cheats.noclip = !g_cheats.noclip;
    SetMovetype(e);
    Con("noclip %s\n", g_cheats.noclip ? "ON" : "OFF");
    Log("console: noclip %s", g_cheats.noclip ? "on" : "off");
}

static void CmdFly()
{
    void* e = CheatTarget(true);
    if (!e)
        return;
    g_cheats.fly = !g_cheats.fly;
    SetMovetype(e);
    Con("flymode %s\n", g_cheats.fly ? "ON" : "OFF");
    Log("console: flymode %s", g_cheats.fly ? "on" : "off");
}

static void CmdNotarget()
{
    void* e = CheatTarget(true);
    if (!e)
        return;
    g_cheats.notarget = !g_cheats.notarget;
    if (g_cheats.notarget)
        F<int>(Vars(e), EV_FLAGS) |= FL_NOTARGET;
    else
        F<int>(Vars(e), EV_FLAGS) &= ~FL_NOTARGET;
    Con("notarget %s\n", g_cheats.notarget ? "ON" : "OFF");
    Log("console: notarget %s", g_cheats.notarget ? "on" : "off");
}

// mp.dll's ClientKill (DLL function 20)
static void CmdKill()
{
    void* e = CheatTarget(false);
    if (!e)
        return;
    unsigned char* fn = (unsigned char*)(MP_CLIENTKILL + g_mpDelta);
    if (fn[0] != 0x55 || fn[1] != 0x8b || fn[2] != 0xec)
    {
        Con("kill: unexpected game code\n");
        return;
    }
    Log("console: kill");
    ((EdictFn)fn)(e);
}

static bool GiveItem(void* player, const char* name)
{
    __try
    {
        ((GiveNamedItemFn)(MP_GIVENAMEDITEM + g_mpDelta))(player, nullptr, name, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// give <weapon_x | item_x | ammo_x | x (weapon_x) | item id from botassist_weapons.txt>
static void CmdGive()
{
    void* e = CheatTarget(true);
    if (!e)
        return;
    const char* a = g_CmdArgv(1);
    if (!a || !*a)
    {
        Con("Usage: give <name> (weapon_ak47, ak47, item_kevlar...) or give <item id>\n");
        return;
    }
    std::string name = a;
    for (auto& ch : name)
        ch = (char)tolower(ch);
    if (name.find_first_not_of("0123456789") == std::string::npos)
    {
        auto it = cfg.weaponModel.find(atoi(name.c_str()));
        if (it == cfg.weaponModel.end() || it->second.size() <= 13)
        {
            Con("give: no weapon with item id %s\n", name.c_str());
            return;
        }
        name = "weapon_" + it->second.substr(9, it->second.size() - 13); // models/p_<res>.mdl
    }
    else if (name.compare(0, 7, "weapon_") && name.compare(0, 5, "item_") && name.compare(0, 5, "ammo_"))
        name = "weapon_" + name;
    static std::vector<char*> names; // the entity's classname may keep pointing at it
    char* keep = nullptr;
    for (char* s : names)
        if (name == s)
            keep = s;
    if (!keep)
        names.push_back(keep = _strdup(name.c_str()));
    void* pl = PrivateOf(e);
    bool ok = pl && GiveItem(pl, keep);
    Con(ok ? "give %s\n" : "give %s: failed\n", keep);
    Log("console: give %s%s", keep, ok ? "" : " (failed)");
}

static void ConsoleBotAdd()
{
    g_consoleBotAdd++;
    ((void (*)())(MP_BOT_COMMAND + g_mpDelta))();
    g_consoleBotAdd--;
}

static bool IsEmptyHandler(void (*fn)())
{
    unsigned char* p = (unsigned char*)fn;
    return Readable(p, 3) && (p[0] == 0xc3 || (p[0] == 0xc2 && p[1] == 0 && p[2] == 0));
}

static void InstallConsoleCommands()
{
    static const struct { const char* name; void (*fn)(); } cheats[] = {
        { "god", CmdGod }, { "noclip", CmdNoclip }, { "notarget", CmdNotarget }, { "fly", CmdFly }, { "kill", CmdKill } };
    static const char* botAdds[] = { "bot_add", "bot_add_t", "bot_add_ct" };
    void (*botCommand)() = (void (*)())(MP_BOT_COMMAND + g_mpDelta);
    bool give = false;
    int n = 0, patched = 0;
    for (CmdFunction* c = *(CmdFunction**)(g_hwLo + HW_CMD_LIST); c && n < 4096; c = c->next, n++)
    {
        if (!Readable(c, sizeof(*c)) || !Readable(c->name, 1))
            break;
        give |= !_stricmp(c->name, "give");
        for (auto& ch : cheats)
            if (!_stricmp(c->name, ch.name) && c->fn != ch.fn && IsEmptyHandler(c->fn))
                c->fn = ch.fn, patched++;
        for (auto b : botAdds)
            if (!_stricmp(c->name, b) && c->fn == botCommand)
                c->fn = ConsoleBotAdd, patched++;
    }
    if (!give)
    {
        auto add = (AddServerCommandFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_ADDSERVERCOMMAND];
        if (add)
            add("give", CmdGive), patched++;
    }
    if (patched)
        Log("console: %d commands set up (god, noclip, notarget, fly, kill, give, bot_add*)", patched);
}

// every server frame: keeps the toggles on your player, checks the command list every 2 s
static void ConsoleFrame()
{
    if (!g_hwLo || !g_mpDelta)
        return;
    float now = Now();
    if (now < g_cheats.lastFrame - 1.0f || g_cheats.lastFrame < 0) // a new map (or the first one)
    {
        // every match starts without cheats (the engine turns sv_cheats on by itself with -dev, and it stays on from
        // an earlier match): the player has to turn it on again in this one
        if (g_CVarGetFloat && g_CVarSetString && g_CVarGetFloat("sv_cheats") != 0.0f)
        {
            g_CVarSetString("sv_cheats", "0");
            Log("console: new match, sv_cheats 0 (it was on; command line: %.200s)", GetCommandLineA());
        }
        if (g_cheats.god || g_cheats.noclip || g_cheats.notarget || g_cheats.fly)
            Log("console: new map, cheats off");
        g_cheats.god = g_cheats.noclip = g_cheats.notarget = g_cheats.fly = false;
        g_consoleNextCheck = -1;
        InterlockedExchange(&g_cheatsReported, 0);
    }
    g_cheats.lastFrame = now;
    g_cheats.lastTick = GetTickCount();
    if (g_consoleNextCheck < 0 || now >= g_consoleNextCheck)
    {
        g_consoleNextCheck = now + 2.0f;
        InstallConsoleCommands();
        if (g_CVarGetFloat && g_CVarGetFloat("sv_cheats") != 0.0f && !InterlockedExchange(&g_cheatsReported, 1))
        {
            HANDLE h = CreateThread(NULL, 0, ReportCheats, NULL, 0, NULL);
            if (h)
                CloseHandle(h);
            else
                InterlockedExchange(&g_cheatsReported, 0);
        }
    }
    if (!(g_cheats.god || g_cheats.noclip || g_cheats.notarget || g_cheats.fly))
        return;
    void* e = YourEdict();
    if (!e || !Alive(e))
        return;
    unsigned char* v = Vars(e);
    if (g_cheats.god)
    {
        F<int>(v, EV_FLAGS) |= FL_GODMODE;
        F<float>(v, EV_TAKEDAMAGE) = DAMAGE_NO;
    }
    if (g_cheats.notarget)
        F<int>(v, EV_FLAGS) |= FL_NOTARGET;
    if (g_cheats.noclip || g_cheats.fly)
        SetMovetype(e);
}

// ---------------------------------------------------------------- diagnostics
// botassist_cmd.txt next to botassist.asi: each line runs as a server console command (then the file is deleted),
// so a test can be driven without input to the (elevated) game window
static void CommandFile()
{
    static DWORD next = 0;
    if (GetTickCount() < next)
        return;
    next = GetTickCount() + 1000;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist_cmd.txt", g_dir);
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    std::vector<std::string> cmds;
    while (fgets(line, sizeof(line), f))
    {
        std::string c = line;
        while (!c.empty() && (c.back() == '\n' || c.back() == '\r' || c.back() == ' '))
            c.pop_back();
        if (!c.empty())
            cmds.push_back(c);
    }
    fclose(f);
    DeleteFileA(path);
    for (auto& c : cmds)
    {
        Log("command file: %s", c.c_str());
        if (g_ServerCommand)
            g_ServerCommand((c + "\n").c_str());
    }
}

// Zombie Disaster Zone (mode 59): CSOGameRulesZSRift (vtable 0x1178504c, g_pGameRules = [0x121cac30]) and the
// object its code reads the round state from ([0x121d3ec4]: +0x27c freeze period, +0x27d, +0x214). Logged when
// they change: rules +0x21c phase (1 -> 2 when the vote ends), +0x224 vote phase, +0x23c / +0x240 vote times,
// +0x284 / +0x288, and how many monsters are alive
static const DWORD MP_CSO_RULES = 0x121d3ec4;
static std::string VtName(unsigned char* obj)
{
    if (!obj || !Readable(obj, 4))
        return "null";
    DWORD vt = *(DWORD*)obj;
    char buf[160];
    const char* name = "?";
    DWORD* col = Readable((void*)(vt - 4), 4) ? *(DWORD**)(vt - 4) : nullptr;
    if (col && Readable(col, 16))
    {
        unsigned char* td = (unsigned char*)col[3];
        if (td && Readable(td + 8, 4))
            name = (const char*)(td + 8);
    }
    snprintf(buf, sizeof(buf), "0x%08lX %.60s", vt - g_mpDelta, name);
    return buf;
}

// the match's interface (mp 0x121d3cec returns it): vfunc 0x188 game mode, 0x318 asked by CSOGameRules::Think
// before its start countdown
static void MatchIface(int& mode, int& v318, int& v214)
{
    __try
    {
        typedef void* (*IfaceFn)();
        IfaceFn get = *(IfaceFn*)(0x121d3cec + g_mpDelta);
        void* m = get ? get() : nullptr;
        if (m)
        {
            typedef int(__thiscall * IntFn)(void*);
            typedef char(__thiscall * BoolFn)(void*);
            void** vt = *(void***)m;
            mode = ((IntFn)vt[0x188 / 4])(m);
            v318 = ((BoolFn)vt[0x318 / 4])(m);
            v214 = ((IntFn)vt[0x214 / 4])(m); // the damage code's encounter level
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log("match interface %p (hw +0x%lX), vtable hw +0x%lX: 0x188 at +0x%lX, 0x214 at +0x%lX, 0x318 at +0x%lX", m,
                    (DWORD)m - g_hwLo, (DWORD)vt - g_hwLo, (DWORD)vt[0x188 / 4] - g_hwLo, (DWORD)vt[0x214 / 4] - g_hwLo,
                    (DWORD)vt[0x318 / 4] - g_hwLo);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// CSOGameRulesZSRift doesn't start the match the usual way: its CheckWinConditions (rules vfunc 0x100) and
// "enough players" check (0x104) are stubs, so nothing says "Game Commencing" and the first real round (the rift's
// vote, buy time, waves) never begins. Offline the room-match branch of CSOGameRules::Think doesn't run either
// (match interface vfunc 0x318 off), and after 5 minutes it ends the match. Started like CSOGameRules'
// CheckWinConditions (0x11411850) does: the "#Game_Commencing" message (0x11498f80), round +0x534 1, and
// TerminateRound(3 s, draw) (0x11491680, cdecl), whose restart makes the first round.
static const DWORD MP_ROUND_MESSAGE = 0x11498f80, MP_TERMINATE_ROUND = 0x11491680;
static bool ZSRiftStart(unsigned char* cso)
{
    __try
    {
        typedef void(__cdecl * MessageFn)(const char*, int);
        typedef void(__cdecl * TerminateFn)(float, int);
        ((MessageFn)(MP_ROUND_MESSAGE + g_mpDelta))("#Game_Commencing", 10);
        ((TerminateFn)(MP_TERMINATE_ROUND + g_mpDelta))(3.0f, 3);
        cso[0x534] = 1;
        cso[0x30b] = 1; // complete reset: the restart doesn't count as a round played (+0x4cc), the rift's 5 do
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// the rules' player stat levels (std::map at +0x288: user ID -> 9 levels, node key +0x10, levels +0x14)
static void ZSRiftLevels(unsigned char* rules, char* out, size_t size)
{
    out[0] = 0;
    __try
    {
        unsigned char* head = *(unsigned char**)(rules + 0x288);
        unsigned char* stack[64];
        int sp = 0;
        unsigned char* node = *(unsigned char**)(head + 4);
        size_t len = 0;
        while ((node && !node[0xd]) || sp)
        {
            while (node && !node[0xd] && sp < 64)
            {
                stack[sp++] = node;
                node = *(unsigned char**)node;
            }
            node = stack[--sp];
            int* lv = (int*)(node + 0x14);
            len += snprintf(out + len, size - len, " user %d: %d %d %d %d %d %d %d %d %d;", *(int*)(node + 0x10), lv[0],
                            lv[1], lv[2], lv[3], lv[4], lv[5], lv[6], lv[7], lv[8]);
            if (len >= size - 1)
                return;
            node = *(unsigned char**)(node + 8);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        snprintf(out, size, " (unreadable)");
    }
}

// The rift's rounds never end here. CSOGameRulesZSRift's monster-kill handler (rules vfunc 0x334, 0x10767a20)
// counts the round's kills at rules +0x284, capped at the round's target (short +0xc of the round entry 0x1135bbc0
// returns from the zombie round manager [[0x121d3ec4] + 0xc]), but nothing compares the two, and its
// CheckWinConditions (0x100) is a stub. The other zombie modes clear a round with the manager's RoundEnd(2 = the
// humans win, 0) (0x1135f190, thiscall, once per round: manager +0x12c8): for mode 59 it counts the round won and
// either ends the match after the last one ("#CSO_CongAllRoundClear") or calls TerminateRound(.., 1); the next
// round's start pays the rift's round money.
static const DWORD MP_ZS_ROUND_ENTRY = 0x1135bbc0, MP_ZS_ROUND_END = 0x1135f190;
static int ZSRiftTarget(unsigned char* zsm)
{
    __try
    {
        typedef unsigned char*(__fastcall * EntryFn)(void*);
        unsigned char* e = ((EntryFn)(MP_ZS_ROUND_ENTRY + g_mpDelta))(zsm);
        return e && Readable(e + 0xc, 2) ? *(short*)(e + 0xc) : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static bool ZSRiftClearRound(unsigned char* zsm)
{
    __try
    {
        typedef void(__thiscall * EndFn)(void*, int, char);
        ((EndFn)(MP_ZS_ROUND_END + g_mpDelta))(zsm, 2, 0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A player who dies while a teammate lives becomes a spectator (0x114d5de0) until the rift's respawn time (player
// +0xf74): the rules' death think (vfunc 0x118, 0x10768180) turns dead flag 2 into 5 once it passes (not while the
// buy menu is open, player +0xe10 14) and respawns on the next one. Bots that died that way stayed spectators: one
// still dead 3 s after its respawn time goes through CBasePlayer::PlayerDeathThink (0x114c3cb0) from here.
static const DWORD MP_PLAYER_DEATH_THINK = 0x114c3cb0;
static bool ZSRiftDeathThink(void* player)
{
    __try
    {
        typedef void(__thiscall * ThinkFn)(void*);
        ((ThinkFn)(MP_PLAYER_DEATH_THINK + g_mpDelta))(player);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Money per kill: the rift pays only at the start of each round (its StartMoney / RoundMoney stats), so a monster's
// killer also gets ZSRIFT_KILL_MONEY through the rules' monster-kill handler (ZSRift vtable slot 0x334, 0x10767a20:
// rules, monster, killer player), with the game's own money getter / setter (0x114b76d0 / 0x114cce40, thiscall on
// the player, as its upgrade purchases use them; the money on the HUD follows).
static const int ZSRIFT_KILL_MONEY = 5, ZSRIFT_MAX_MONEY = 999999;
static const DWORD MP_ZSRIFT_VTABLE = 0x1178504C, MP_ZSRIFT_MONSTER_KILLED = 0x10767a20, MP_GET_MONEY = 0x114b76d0,
                   MP_SET_MONEY = 0x114cce40;
typedef void(__fastcall* MonsterKilledFn)(void* rules, void* edx, void* monster, void* killer);
static MonsterKilledFn g_ZsrMonsterKilled;

static void ZsrPayKill(void* killer)
{
    __try
    {
        typedef int(__thiscall * GetFn)(void*);
        typedef void(__thiscall * SetFn)(void*, int);
        int money = ((GetFn)(MP_GET_MONEY + g_mpDelta))(killer);
        if (money >= 0 && money < ZSRIFT_MAX_MONEY)
            ((SetFn)(MP_SET_MONEY + g_mpDelta))(killer, min(money + ZSRIFT_KILL_MONEY, ZSRIFT_MAX_MONEY));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void __fastcall HookedZsrMonsterKilled(void* rules, void* edx, void* monster, void* killer)
{
    g_ZsrMonsterKilled(rules, edx, monster, killer);
    if (killer)
        ZsrPayKill(killer);
}

static void HookZsrKillMoney()
{
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    void** slot = (void**)(MP_ZSRIFT_VTABLE + g_mpDelta + 0x334);
    if (Readable(slot, 4) && *slot == (void*)(MP_ZSRIFT_MONSTER_KILLED + g_mpDelta))
    {
        g_ZsrMonsterKilled = (MonsterKilledFn)*slot;
        PatchSlot(slot, (void*)HookedZsrMonsterKilled);
    }
    Log("ZSRift: %s", g_ZsrMonsterKilled ? "each monster kill pays its killer" : "kill money NOT hooked (unexpected code)");
}

static std::string ZSRiftDeadState(void* e)
{
    unsigned char* v = Vars(e);
    unsigned char* p = (unsigned char*)PrivateOf(e);
    char buf[200];
    snprintf(buf, sizeof(buf), "dead flag %d, health %.0f, respawn at %.1f, buy menu +0xe10 %d, joining +0xcc0 %d, "
             "observer +0xc7c %d / iuser1 %d, next think %.1f, team %d",
             F<int>(v, EV_DEADFLAG), F<float>(v, EV_HEALTH), p ? *(float*)(p + 0xf74) : -1.0f, p ? *(int*)(p + 0xe10) : -1,
             p ? *(int*)(p + 0xcc0) : -1, p ? *(int*)(p + 0xc7c) : -1, F<int>(v, 0x274), F<float>(v, 0x114),
             p ? *(int*)(p + 0x6c) : -1);
    return buf;
}

static void ZSRiftRespawnBots(float now)
{
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        if ((F<int>(v, EV_FLAGS) & (FL_CLIENT | FL_FAKECLIENT)) != (FL_CLIENT | FL_FAKECLIENT))
            return;
        int dead = F<int>(v, EV_DEADFLAG);
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if ((dead != 2 && dead != 5) || !p || *(float*)(p + 0xf74) <= 0 || now < *(float*)(p + 0xf74) + 3.0f)
            return;
        std::string before = ZSRiftDeadState(e);
        if (*(int*)(p + 0xe10) == 14)
            *(int*)(p + 0xe10) = 0;
        bool ok = ZSRiftDeathThink(p);
        if (ok && F<int>(v, EV_DEADFLAG) == 5)
            ok = ZSRiftDeathThink(p);
        Log("ZSRift: bot '%s' still dead after its respawn time (%s): death think%s -> %s", String(F<int>(v, EV_NETNAME)),
            before.c_str(), ok ? "" : " FAILED", ZSRiftDeadState(e).c_str());
    });
}

static void ZSRiftWatch()
{
    static float next = 0;
    static std::string last;
    if (!g_mpDelta || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0 || GameMode() != 59)
        return;
    float now = Now();
    if (now >= next - 10.0f && now < next)
        return;
    next = now + 1.0f;
    unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
    unsigned char* r = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    if (!r || !Readable(r, 0x2d0) || !cso || !Readable(cso, 0x560))
        return;
    HookZsrKillMoney();
    int monsters = 0, players = 0;
    std::map<std::string, int> classes;
    std::string health;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        unsigned char* v = Vars(e);
        int flags = F<int>(v, EV_FLAGS);
        char h[96];
        if (flags & FL_CLIENT)
        {
            players++;
            snprintf(h, sizeof(h), " '%s' %.0f/%.0f", String(F<int>(v, EV_NETNAME)), F<float>(v, EV_HEALTH),
                     F<float>(v, EV_ARMORVALUE));
            health += h;
        }
        else if ((flags & FL_MONSTER) && F<float>(v, EV_HEALTH) > 0)
        {
            if (monsters++ < 3)
            {
                snprintf(h, sizeof(h), " %s %.0f", ClassName(e), F<float>(v, EV_HEALTH));
                health += h;
            }
            classes[ClassName(e)]++;
        }
    });
    int mode = -1, v318 = -1, v214 = -1;
    MatchIface(mode, v318, v214);
    // start the match once the intro time is over (+4 0) when the game won't
    static float introOver = -1;
    if (cso[0x27c] || cso[0x27d] || cso[0x534] || *(int*)(cso + 4) || v318 > 0 || *(DWORD*)r != 0x1178504C + g_mpDelta)
        introOver = -1;
    else if (introOver < 0 || now < introOver)
        introOver = now;
    else if (now - introOver >= 3.0f)
    {
        introOver = -1;
        bool ok = ZSRiftStart(cso);
        Log("ZSRift: the match didn't start by itself (room-match check 0x318 off): started it%s", ok ? "" : " - FAILED");
    }
    // clear the round once its kills reach the target
    unsigned char* zsm = *(unsigned char**)(cso + 0xc);
    int target = zsm && Readable(zsm, 0x1300) ? ZSRiftTarget(zsm) : -1;
    if (cso[0x27c] == 1 && !cso[0x27d] && !*(int*)(cso + 4) && *(DWORD*)r == 0x1178504C + g_mpDelta && target > 0 &&
        *(int*)(r + 0x284) >= target && !zsm[0x12c8])
    {
        bool ok = ZSRiftClearRound(zsm);
        Log("ZSRift: round %d cleared (%d of %d kills; %d rounds played of %d)%s", *(short*)(zsm + 0x288),
            *(int*)(r + 0x284), target, *(int*)(cso + 0x4cc), *(int*)(zsm + 0x290), ok ? "" : " - FAILED");
    }
    if (cso[0x27c] == 1)
        ZSRiftRespawnBots(now);
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "rules %s, round %s: started(+27c) %d over(+27d) %d +4 %d +240 %.1f start(+248) %.1f freeze(+250) %d +254 "
             "%d +274 %d +2de %d +534 %d +556 %d +214 %d; rules phase(+21c) %d vote(+224) %d +23c %.1f +240 %.1f +284 %d "
             "+20c %.1f +211 %d +218 %d; iface mode %d 0x318 %d 0x214 %d; %d monsters, %d clients; round %d, target %d, "
             "%d played of %d",
             VtName(r).c_str(), cso == r ? "= rules" : VtName(cso).c_str(), cso[0x27c], cso[0x27d], *(int*)(cso + 4),
             *(float*)(cso + 0x240), *(float*)(cso + 0x248), *(int*)(cso + 0x250), *(int*)(cso + 0x254),
             *(int*)(cso + 0x274), cso[0x2de], cso[0x534], cso[0x556], cso[0x214], *(int*)(r + 0x21c),
             *(int*)(r + 0x224), *(float*)(r + 0x23c), *(float*)(r + 0x240), *(int*)(r + 0x284), *(float*)(r + 0x20c),
             r[0x211], r[0x218], mode, v318, v214, monsters, players, zsm ? *(short*)(zsm + 0x288) : -1, target,
             *(int*)(cso + 0x4cc), zsm ? *(int*)(zsm + 0x290) : -1);
    // logged when the round or the rift's phase changes (not on every kill)
    char key[96];
    snprintf(key, sizeof(key), "%d %d %d %d %d %d %d %d %d %d", cso[0x27c], cso[0x27d], *(int*)(cso + 4), cso[0x534],
             *(int*)(r + 0x21c), *(int*)(r + 0x224), v318, v214, target, *(int*)(cso + 0x4cc));
    std::string state = key;
    if (state != last)
    {
        std::string names;
        for (auto& c : classes)
            names += " " + c.first + " x" + std::to_string(c.second);
        char levels[600];
        ZSRiftLevels(r, levels, sizeof(levels));
        Log("ZSRift %.1f: %s%s; levels%s; health/armor%s", now, buf, names.c_str(), levels, health.c_str());
        last = state;
    }
}

// ---------------------------------------------------------------- orders in the other modes
// follow / cover me: back to a formation spot near the commander when it got farther than that; hold: stay on
// the spot (it can still turn and shoot); go / take point: the game's AI again. Movement is given relative to
// the view the game's AI chose this command.
static bool OrderedMove(void* bot, BotState& st, float viewYaw, float& fwd, float& side, unsigned short& buttons)
{
    if (!st.ordered || st.order == ORDER_INDEPENDENT)
        return false;
    V3 pos = Origin(bot);
    V3 goal;
    float arrive;
    if (st.order == ORDER_HOLD)
    {
        goal = st.holdSpot;
        arrive = 48.0f;
    }
    else
    {
        void* leader = st.commander;
        if (!leader || !IsHuman(leader) || !Alive(leader) || TeamOf(leader) != TeamOf(bot))
            return false;
        float dist = st.order == ORDER_ESCORT ? 100.0f : cfg.followDistance;
        if (Len(Origin(leader) - pos) < dist + 90.0f)
            return false; // close enough: the game's AI does the rest
        goal = FormationSpot(leader, st.slot, dist);
        arrive = 60.0f;
    }
    V3 wp;
    if (!NextWaypoint(bot, st, pos, goal, arrive, wp))
    {
        if (st.order == ORDER_HOLD)
        {
            fwd = side = 0; // on the spot: stay
            return true;
        }
        return false;
    }
    float moveYaw = YawOf(wp - pos);
    if (wp.z > pos.z + 24.0f && Len2D(wp - pos) < 120.0f)
        buttons |= IN_JUMP;
    moveYaw = ClearYaw(bot, pos, moveYaw, 40.0f);
    float now = Now();
    if (Len2D(pos - st.lastPos) > 24.0f)
    {
        st.lastPos = pos;
        st.lastProgress = now;
    }
    else if (now - st.lastProgress > 1.0f)
    {
        st.lastProgress = now;
        st.nextRepath = 0;
        buttons |= IN_JUMP;
    }
    float rel = (moveYaw - viewYaw) / RAD;
    fwd = cosf(rel) * 250.0f;
    side = -sinf(rel) * 250.0f;
    return true;
}

// a bot that took its room AI player after choosing its character gets the room's character now
static void ApplyRoomCharacter(void* bot, BotState& bs)
{
    MatchBot* rb = RoomBotOf(bot, bs);
    // with the appearance hook the character is chosen when the bot is created (joinclass in game is refused)
    if (!rb || rb->character <= 0 || bs.appliedClass == rb->character || !g_ClientCommandTrampoline ||
        g_ChooseAppearanceTrampoline)
        return;
    if (!CharacterFits(rb->character, TeamOf(bot)))
    {
        bs.appliedClass = rb->character; // don't try again
        Log("bot '%s': character %d is of the other team: not used", rb->name.c_str(), rb->character);
        return;
    }
    unsigned char* v = Vars(bot);
    bs.appliedClass = rb->character;
    char id[16];
    snprintf(id, sizeof(id), "%d", rb->character);
    const char* before = String(F<int>(v, 0xc8));
    std::string was = before ? before : "";
    FakeCommand(bot, "joinclass", id);
    const char* after = String(F<int>(v, 0xc8));
    Log("bot %d '%s': room character %d (model %s -> %s)", bs.slot, rb->name.c_str(), rb->character, was.c_str(),
        after ? after : "");
}

// The game's own AI (every PvP mode) aims and reacts by the bot's BotProfile (CBot::m_profile, player +0x3700;
// profiles are shared by every bot with the same name). A room AI player with a skill gets its own copy:
//   +0x08 skill 0..1 (aim error: 1 = none), +0x64 reaction time (BotProfile.db value - 0.1), +0x68 attack delay.
// Skill 0..7 -> skill 0.2..1, reaction 0.4..0 s, attack delay 0.7..0 s: Expert aims and reacts like CZ's best.
static const int PLAYER_BOT_PROFILE = 0x3700;
static const int PROFILE_COPY = 0x200;
static bool SetRoomProfile(unsigned char* player, BotState& bs, int skill, float* was)
{
    __try
    {
        unsigned char** slot = (unsigned char**)(player + PLAYER_BOT_PROFILE);
        unsigned char* cur = *slot;
        if (!cur)
            return false;
        if (cur != bs.ownProfile)
        {
            float s = *(float*)(cur + 8), r = *(float*)(cur + 0x64);
            if (!(s >= 0.0f && s <= 1.0f) || !(r >= -1.0f && r <= 5.0f))
                return false; // not a BotProfile
            memcpy(bs.ownProfile, cur, PROFILE_COPY);
        }
        was[0] = *(float*)(bs.ownProfile + 8);
        was[1] = *(float*)(bs.ownProfile + 0x64);
        was[2] = *(float*)(bs.ownProfile + 0x68);
        float d = (float)(skill > 7 ? 7 : skill);
        *(float*)(bs.ownProfile + 8) = 0.2f + d * (0.8f / 7.0f);
        *(float*)(bs.ownProfile + 0x64) = 0.4f - d * (0.4f / 7.0f);
        *(float*)(bs.ownProfile + 0x68) = (7.0f - d) * 0.1f;
        *slot = bs.ownProfile;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void ApplyRoomSkill(void* bot, BotState& bs)
{
    MatchBot* rb = RoomBotOf(bot, bs);
    if (!rb || rb->skill < 0)
        return;
    unsigned char* player = (unsigned char*)PrivateOf(bot);
    if (!player || IsBadReadPtr(player + PLAYER_BOT_PROFILE, 4))
        return;
    unsigned char* cur = *(unsigned char**)(player + PLAYER_BOT_PROFILE);
    if (cur && cur == bs.ownProfile && bs.ownSkill == rb->skill)
        return;
    if (!bs.ownProfile)
        bs.ownProfile = new unsigned char[PROFILE_COPY];
    float was[3] = {};
    if (!SetRoomProfile(player, bs, rb->skill, was))
    {
        static int fails = 0;
        if (fails++ < 5)
            Log("bot '%s': no bot profile at +0x%x: room skill not applied", rb->name.c_str(), PLAYER_BOT_PROFILE);
        bs.ownSkill = rb->skill; // don't try every frame
        return;
    }
    bs.ownSkill = rb->skill;
    Log("bot %d '%s': room skill %d (profile skill %.2f -> %.2f, reaction %.2f -> %.2f, attack delay %.2f -> %.2f)", bs.slot,
        rb->name.c_str(), rb->skill, was[0], *(float*)(bs.ownProfile + 8), was[1], *(float*)(bs.ownProfile + 0x64), was[2],
        *(float*)(bs.ownProfile + 0x68));
}

// Modes that swap the teams in the middle of the match give everyone the default character of the new team
// (mp.dll 0x114d66e1: class number = class table vfunc 0x24(team), male, then player vfunc 0x220 sets the model).
// A bot that ends up on the other team with that default character gets a character of its new team instead:
// its room character if it fits, else one from its filter or any (half men, half women). Zombie modes are left
// alone (infection changes the team and the model).
static bool ZombieMode(int mode)
{
    static const int zombie[] = { 8, 9, 14, 15, 17, 20, 24, 28, 29, 30, 32, 35, 36, 37, 43, 44, 45, 51, 52, 54, 56,
                                  57, 58, 59 };
    for (int z : zombie)
        if (mode == z)
            return true;
    return false;
}

typedef int(__fastcall* TeamDefaultClassFn)(void* table, void* edx, int team);
typedef void(__fastcall* PlayerVoidFn)(void* player, void* edx);
// the class number of the team's default character, or -1
static int TeamDefaultClass(int team)
{
    __try
    {
        void* owner = *(void**)(MP_CLASS_OWNER + g_mpDelta);
        void* table = owner ? *(void**)((unsigned char*)owner + 0x1f8) : nullptr;
        return table ? ((TeamDefaultClassFn)(*(void***)table)[0x24 / 4])(table, nullptr, team) : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// what the team swap does, with our class: class number (+0xcd4, +0x1fb8), sex (+0x1fb5 bit 0, +0x2155), model
static bool SetPlayerClass(unsigned char* player, int number, int male)
{
    __try
    {
        *(int*)(player + 0xcd4) = number;
        *(int*)(player + 0x1fb8) = number;
        player[0x1fb5] = (unsigned char)((player[0x1fb5] & ~1) | (male ? 1 : 0));
        player[0x2155] = (unsigned char)(male ? 1 : 0);
        ((PlayerVoidFn)(*(void***)player)[0x220 / 4])(player, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void ApplyTeamCharacter(void* bot, BotState& bs)
{
    int team = TeamOf(bot);
    if (team != 1 && team != 2)
        return;
    if (!bs.charTeam)
    {
        bs.charTeam = team; // the character it was created with
        return;
    }
    if (team == bs.charTeam)
        return;
    unsigned char* player = (unsigned char*)PrivateOf(bot);
    if (!player || IsBadReadPtr(player + 0xcd4, 4) || ZombieMode(GameMode()))
    {
        bs.charTeam = team;
        return;
    }
    int current = *(int*)(player + 0xcd4);
    int def = TeamDefaultClass(team);
    if (def <= 0 || current != def)
        return; // not swapped yet (or it already has a character of its own)
    bs.charTeam = team;
    MatchBot* rb = RoomBotOf(bot, bs);
    int id = 0;
    if (rb && rb->character > 0 && CharacterFits(rb->character, team) && CharacterClassNumber(rb->character))
        id = rb->character;
    else
    {
        std::vector<int> pool;
        if (rb)
            for (int c : rb->allowCharacter)
                if (CharacterFits(c, team) && CharacterClassNumber(c))
                    pool.push_back(c);
        if (pool.empty() && cfg.randomCharacters)
            pool = cfg.characters[team];
        if (pool.empty())
            return;
        id = PickCharacter(pool, team);
    }
    int number = CharacterClassNumber(id);
    int sex = CharacterSex(id);
    if (!number || !SetPlayerClass(player, number, sex != 0))
        return;
    bs.appliedClass = id;
    static int reports = 0;
    if (reports++ < 60)
        Log("bot %d '%s' swapped to team %d: character %d (class %d) instead of the team's default %d", bs.slot,
            rb ? rb->name.c_str() : "?", team, id, number, def);
}

// ---------------------------------------------------------------- hooks
static void HookedRunPlayerMove(void* fakeclient, const float* angles, float fwd, float side, float up,
                                unsigned short buttons, unsigned char impulse, unsigned char msec)
{
    float aim[3] = { angles[0], angles[1], angles[2] };
    BotState* dbg = nullptr;
    __try
    {
        if (cfg.enabled && fakeclient && g_edictToVars >= 0)
        {
            unsigned char* v = Vars(fakeclient);
            UpdateCvars(fakeclient);
            LoadNav();
            UpdateMatchBots();
            UpdateZgPlayers();
            if (F<int>(v, EV_FLAGS) & FL_FAKECLIENT)
            {
                BotState& bs = StateFor(fakeclient);
                ApplyRoomCharacter(fakeclient, bs);
                ApplyRoomSkill(fakeclient, bs);
                ApplyTeamCharacter(fakeclient, bs);
                if (cfg.sameTeam && CoopMode())
                    FixCoopTeam(fakeclient, bs);
                UpdateBuying(fakeclient, bs);
                if (CoopMode() && MonsterMap())
                {
                    UpdateRefill(fakeclient, bs);
                    UpdateBarricades(fakeclient);
                }
            }
            if ((F<int>(v, EV_FLAGS) & FL_FAKECLIENT) && Alive(fakeclient))
            {
                LoadNav();
                BotState& st = StateFor(fakeclient);
                st.stockPitch = aim[0];
                st.stockYaw = aim[1];
                st.driven = false;
                dbg = &st;
                DebugCensus(fakeclient);
                // take the bots over only in the scenario modes (monsters); elsewhere they play normally
                bool scenario = CoopMode();
                bool monsters = scenario && MonsterMap();
                if (scenario && !monsters && Now() >= st.nextScan)
                {
                    ScanMonsters(fakeclient, st, Eye(fakeclient));
                    st.nextScan = Now() + 1.0f;
                    monsters = st.threat != nullptr;
                }
                if (!scenario)
                {
                    float f = fwd, sd = side;
                    unsigned short bt = buttons;
                    if (OrderedMove(fakeclient, st, aim[1], f, sd, bt))
                    {
                        fwd = f;
                        side = sd;
                        buttons = bt;
                    }
                }
                if (!scenario && GameMode() == MODE_ZOMBIE_GIANT)
                {
                    float y = aim[1], p = aim[0], f = fwd, sd = side;
                    unsigned short bt = buttons;
                    if (DriveZgBot(fakeclient, y, p, f, sd, bt))
                    {
                        aim[0] = p < -89.0f ? -89.0f : p > 89.0f ? 89.0f : p;
                        aim[1] = y;
                        fwd = f;
                        side = sd;
                        buttons = bt;
                        st.driven = true;
                        float* va = (float*)(v + EV_VANGLE); // fixangle: see below
                        float* ang = (float*)(v + EV_ANGLES);
                        va[0] = aim[0];
                        va[1] = aim[1];
                        va[2] = 0;
                        ang[0] = aim[0] / -3.0f;
                        ang[1] = aim[1];
                        ang[2] = 0;
                    }
                }
                if (monsters)
                {
                    Command cmd{ aim[1], aim[0], fwd, side, buttons };
                    float dt = msec ? msec / 1000.0f : 0.016f;
                    if (cfg.companions)
                    {
                        Drive(fakeclient, st, cmd, dt);
                        fwd = cmd.forward;
                        side = cmd.side;
                    }
                    else
                    {
                        float keepFwd = fwd, keepSide = side;
                        Drive(fakeclient, st, cmd, dt);
                        fwd = keepFwd;
                        side = keepSide;
                    }
                    aim[0] = cmd.pitch < -89.0f ? -89.0f : cmd.pitch > 89.0f ? 89.0f : cmd.pitch;
                    aim[1] = AngleDiff(cmd.yaw, 0.0f); // -180..180
                    buttons = cmd.buttons;
                    st.driven = true;
                    // bots run with pev->fixangle set, so the engine ignores the command's view angles and keeps
                    // pev->v_angle (what the stock AI chose): that is where the bot looks and shoots. Write ours there,
                    // like CBot::ExecuteCommand does with its own (model angles: pitch / -3)
                    float* va = (float*)(v + EV_VANGLE);
                    float* ang = (float*)(v + EV_ANGLES);
                    va[0] = aim[0];
                    va[1] = aim[1];
                    va[2] = 0;
                    ang[0] = aim[0] / -3.0f;
                    ang[1] = aim[1];
                    ang[2] = 0;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("exception 0x%08lX in the bot hook (ignored)", GetExceptionCode());
    }
    g_RunPlayerMove(fakeclient, aim, fwd, side, up, buttons, impulse, msec);
    if (dbg && CoopMode()) // debugging is about the scenario modes
        __try { DebugBot(fakeclient, *dbg, aim, buttons); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// radio menus: radio1 = cover me, take point, hold position, regroup, follow me, taking fire;
// radio2 = go, fall back, stick together, get in position, storm the front, report in
static int g_openMenu = 0;
static std::vector<std::string> g_seenCommands;

static void GiveOrder(Order order, void* by)
{
    g_teamOrder = order;
    int n = 0;
    int team = TeamOf(by);
    void* answer = nullptr;
    for (auto& b : g_bots)
    {
        if (!b.edict || !(F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT))
            continue;
        if (team && TeamOf(b.edict) != team)
            continue;
        if (!answer && Alive(b.edict))
            answer = b.edict;
        b.order = order;
        b.ordered = true;
        b.commander = by;
        b.path.clear();
        if (order == ORDER_HOLD)
        {
            b.holdSpot = Origin(b.edict);
            b.holdYaw = F<float>(Vars(by), EV_VANGLE + 4);
        }
        n++;
    }
    Log("order: %s (%d bots)", OrderName(order), n);
    if (cfg.voices != "off" && answer)
        FakeCommand(answer, "roger");
}

static void OnPlayerCommand(void* player)
{
    const char* cmd = g_CmdArgv(0);
    if (!cmd || !*cmd)
        return;
    std::string c = cmd;
    for (auto& ch : c)
        ch = (char)tolower(ch);
    if (c == "joinclass")
    {
        // what a player's character choice looks like, next to what botassist gives the bots
        const char* a1 = g_CmdArgv(1);
        void* pl = PrivateOf(player);
        unsigned char* p = (unsigned char*)pl;
        Log("player joinclass %s (now: +0xcd4 %d, class index +0x2156 %d, default flag +0x2155 %d)", a1 ? a1 : "",
            p ? *(int*)(p + 0xcd4) : -1, p ? *(unsigned short*)(p + 0x2156) : -1, p ? p[0x2155] : -1);
    }
    else if (cfg.logCommands && (c == "menuselect" || c.compare(0, 5, "radio") == 0))
    {
        const char* a1 = g_CmdArgv(1);
        Log("player command: %s %s (open menu %d)", c.c_str(), a1 ? a1 : "", g_openMenu);
    }
    else if (cfg.logCommands && g_seenCommands.size() < 60)
    {
        bool known = false;
        for (auto& s : g_seenCommands)
            known |= s == c;
        if (!known)
        {
            g_seenCommands.push_back(c);
            Log("player command: %s", c.c_str());
        }
    }
    if (c == "radio1" || c == "radio2" || c == "radio3")
    {
        g_openMenu = c[5] - '0';
        return;
    }
    // chat orders: "/follow", "/hold", "/cover", "/free" (also without the slash, in any case)
    if (c == "say" || c == "say_team")
    {
        const char* a = g_CmdArgv(1);
        std::string t = a ? a : "";
        for (auto& ch : t)
            ch = (char)tolower(ch);
        while (!t.empty() && (t[0] == '/' || t[0] == '!' || t[0] == ' ' || t[0] == '"'))
            t.erase(0, 1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '"'))
            t.pop_back();
        if (t == "follow" || t == "followme" || t == "regroup")
            GiveOrder(ORDER_FOLLOW, player);
        else if (t == "cover" || t == "coverme")
            GiveOrder(ORDER_ESCORT, player);
        else if (t == "hold" || t == "holdpos" || t == "stay")
            GiveOrder(ORDER_HOLD, player);
        else if (t == "free" || t == "go" || t == "takepoint")
            GiveOrder(ORDER_INDEPENDENT, player);
        return;
    }
    int pick = -1;
    if (c == "menuselect")
    {
        const char* a = g_CmdArgv(1);
        pick = a ? atoi(a) : -1;
        int menu = g_openMenu;
        g_openMenu = 0;
        if (menu == 1)
            c = pick == 1 ? "coverme" : pick == 2 ? "takepoint" : pick == 3 ? "holdpos" : pick == 4 ? "regroup" : pick == 5 ? "followme" : "";
        else if (menu == 2)
            c = pick == 1 ? "go" : pick == 2 ? "fallback" : pick == 3 ? "sticktog" : pick == 4 ? "getinpos" : pick == 5 ? "stormfront" : "";
        else
            return;
    }
    if (c == "followme" || c == "regroup" || c == "sticktog" || c == "fallback")
        GiveOrder(ORDER_FOLLOW, player);
    else if (c == "coverme")
        GiveOrder(ORDER_ESCORT, player);
    else if (c == "holdpos" || c == "getinpos")
        GiveOrder(ORDER_HOLD, player);
    else if (c == "takepoint" || c == "stormfront" || c == "go")
        GiveOrder(ORDER_INDEPENDENT, player);
}

static void HookedClientCommand(void* player)
{
    __try
    {
        if (cfg.enabled && player && g_edictToVars >= 0 && IsHuman(player))
            OnPlayerCommand(player);
        else if (cfg.enabled && player && g_edictToVars >= 0 && (F<int>(Vars(player), EV_FLAGS) & FL_FAKECLIENT))
        {
            if (OnBotCommand(player))
                return; // dropped
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_ClientCommandTrampoline(player);
}

// ---------------------------------------------------------------- installation
static bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

// entvars_t lives inside edict_t; pev->pContainingEntity points back to the edict
static void CalibrateEdict(void* edict)
{
    for (int k = 0; k <= 0x400; k += 4)
    {
        unsigned char* v = (unsigned char*)edict + k;
        if (Readable(v + EV_CONTAINING_ENTITY, 4) && *(void**)(v + EV_CONTAINING_ENTITY) == edict)
        {
            g_edictToVars = k;
            Log("edict_t -> entvars_t offset: 0x%x", k);
            return;
        }
    }
}

static void CalibratingRunPlayerMove(void* fakeclient, const float* angles, float fwd, float side, float up,
                                     unsigned short buttons, unsigned char impulse, unsigned char msec)
{
    if (g_edictToVars < 0 && fakeclient)
        __try { CalibrateEdict(fakeclient); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HookedRunPlayerMove(fakeclient, angles, fwd, side, up, buttons, impulse, msec);
}

static void CalibratingClientCommand(void* player)
{
    if (g_edictToVars < 0 && player)
        __try { CalibrateEdict(player); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HookedClientCommand(player);
}

// ClientCommand starts with "push ebp; mov ebp, esp; push -1; push imm32" (10 bytes): those run from a trampoline
static bool DetourClientCommand(unsigned char* fn)
{
    if (fn[0] == 0xe9) // already detoured by an earlier load of this mp.dll
        return true;
    if (!(fn[0] == 0x55 && fn[1] == 0x8b && fn[2] == 0xec && fn[3] == 0x6a && fn[4] == 0xff && fn[5] == 0x68))
    {
        Log("ClientCommand has unexpected code; radio orders disabled");
        return false;
    }
    unsigned char* tramp = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return false;
    memcpy(tramp, fn, 10);
    tramp[10] = 0xe9;
    *(int*)(tramp + 11) = (int)((fn + 10) - (tramp + 15));
    g_ClientCommandTrampoline = (ClientCommandFn)tramp;
    DWORD old;
    VirtualProtect(fn, 10, PAGE_EXECUTE_READWRITE, &old);
    unsigned char patch[10] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(int*)(patch + 1) = (int)((unsigned char*)CalibratingClientCommand - (fn + 5));
    memcpy(fn, patch, 10);
    VirtualProtect(fn, 10, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 10);
    return true;
}

static bool Install(HMODULE mp)
{
    DWORD base = (DWORD)mp;
    DWORD delta = base - MP_PREFERRED_BASE;
    void** table = (void**)(MP_ENGFUNCS + delta);
    unsigned char* site = (unsigned char*)(MP_EXECUTECOMMAND_CALL + delta);

    // only touch the mp.dll this was written for: CBot::ExecuteCommand must call g_engfuncs.pfnRunPlayerMove here
    if (!Readable(site, 6) || site[0] != 0xff || site[1] != 0x15 || *(void***)(site + 2) != &table[ENG_RUNPLAYERMOVE])
    {
        Log("mp.dll at 0x%08lx is not the expected build; bots left unchanged", base);
        return false;
    }
    if (!table[ENG_RUNPLAYERMOVE] || !table[ENG_TRACELINE] || !table[ENG_FINDENTITYINSPHERE] || !table[ENG_CMD_ARGV] ||
        !table[ENG_LOADFILE] || !table[ENG_FREEFILE] || !table[ENG_CVARGETFLOAT] || !table[ENG_CVARGETSTRING] ||
        !table[ENG_CVARSETSTRING] || !table[ENG_SERVERCOMMAND])
        return false; // engine functions not handed over yet (GiveFnptrsToDll)

    g_FindEntityInSphere = (FindEntityInSphereFn)table[ENG_FINDENTITYINSPHERE];
    g_TraceLine = (TraceLineFn)table[ENG_TRACELINE];
    g_CmdArgv = (CmdArgvFn)table[ENG_CMD_ARGV];
    g_LoadFile = (LoadFileFn)table[ENG_LOADFILE];
    g_FreeFile = (FreeFileFn)table[ENG_FREEFILE];
    g_CVarGetFloat = (CVarGetFloatFn)table[ENG_CVARGETFLOAT];
    g_CVarGetString = (CVarGetStringFn)table[ENG_CVARGETSTRING];
    g_CVarSetString = (CVarSetStringFn)table[ENG_CVARSETSTRING];
    g_ServerCommand = (ServerCommandFn)table[ENG_SERVERCOMMAND];
    g_GetRoom = (GetRoomFn)table[165];
    g_IndexOfEdict = (IndexOfEdictFn)table[76];
    g_GetInfoKeyBuffer = (GetInfoKeyBufferFn)table[112];
    g_SetClientKeyValue = (SetClientKeyValueFn)table[115];
    g_CreateFakeClient = (CreateFakeClientFn)table[ENG_CREATEFAKECLIENT];
    g_FreeEntPrivateData = (EdictFn)table[ENG_FREEENTPRIVATEDATA];
    g_SetClientMaxspeed = (SetClientMaxspeedFn)table[ENG_SETCLIENTMAXSPEED];
    g_MessageBegin = (MessageBeginFn)table[ENG_MESSAGEBEGIN];
    g_MessageEnd = (MessageEndFn)table[ENG_MESSAGEEND];
    g_WriteByte = (WriteIntFn)table[ENG_WRITEBYTE];
    g_WriteShort = (WriteIntFn)table[ENG_WRITESHORT];
    g_WriteCoord = (WriteCoordFn)table[ENG_WRITECOORD];
    if (table[ENG_RUNPLAYERMOVE] != (void*)CalibratingRunPlayerMove)
        g_RunPlayerMove = (RunPlayerMoveFn)table[ENG_RUNPLAYERMOVE];
    g_pGlobals = (DWORD**)(MP_GLOBALS + delta);
    g_mpDelta = delta;

    bool orders = DetourClientCommand((unsigned char*)(MP_CLIENTCOMMAND + delta));
    DWORD old;
    VirtualProtect(&table[ENG_RUNPLAYERMOVE], 4, PAGE_READWRITE, &old);
    InterlockedExchangePointer(&table[ENG_RUNPLAYERMOVE], (void*)CalibratingRunPlayerMove);
    VirtualProtect(&table[ENG_RUNPLAYERMOVE], 4, old, &old);
    Log("hooked mp.dll at 0x%08lx: companions %s, radio orders %s, follow distance %.0f, buying from %d weapons",
        base, cfg.companions ? "on" : "off (aim only)", orders ? "on" : "off", cfg.followDistance,
        cfg.buyAnyWeapon ? (int)(cfg.primaries.size() + cfg.pistols.size()) : 0);
    HookBotManager(delta);
    return true;
}

// ---------------------------------------------------------------- the room's "AI settings" button
// GameUI's room panel runs the "BotSetting" command (CRoomPanel OnCommand, 0x105b8816: strcmp, then the dialog
// code at 0x105b884d). It now tells CSNZ_Server to show the AI player editor and opens the game's browser window
// (CCSOMainPanel::OpenWebEvent 0x105a9020 on the main panel [0x10b844fc], like the patched Shop button); the
// shop page sees the request in /api/state and goes to /bots/.
static const DWORD GAMEUI_PREFERRED_BASE = 0x10000000;
static const DWORD GAMEUI_BOTSETTING = 0x105b884d;  // a1 c8 44 b8 10 (mov eax, [0x10b844c8])
static const DWORD GAMEUI_ONCOMMAND_EPILOGUE = 0x105b88ad;
static const DWORD GAMEUI_OPENWEBEVENT = 0x105a9020;
static const DWORD GAMEUI_MAINPANEL = 0x10b844fc;
static DWORD g_gameUiDelta;
static DWORD g_botSettingReturn;

static bool HttpPost(const wchar_t* path, const char* body, std::string* reply)
{
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"botassist", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        return false;
    WinHttpSetTimeouts(ses, 1000, 1000, 2000, 2000);
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)cfg.apiPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    if (req && WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1L, (LPVOID)body, (DWORD)strlen(body),
                                  (DWORD)strlen(body), 0) &&
        WinHttpReceiveResponse(req, NULL))
    {
        ok = true;
        char buf[4096];
        DWORD got = 0;
        while (reply && WinHttpReadData(req, buf, sizeof(buf), &got) && got)
            reply->append(buf, got);
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

typedef void(__fastcall* OpenWebEventFn)(void* mainPanel, void* edx);

static void __cdecl OnAiSettingsButton()
{
    bool told = HttpPost(L"/api/openview", "{\"view\":\"bots\"}");
    Log("room AI settings button: opening the AI player editor%s", told ? "" : " (server not reachable)");
    void* panel = *(void**)(GAMEUI_MAINPANEL + g_gameUiDelta);
    if (panel)
        ((OpenWebEventFn)(GAMEUI_OPENWEBEVENT + g_gameUiDelta))(panel, nullptr);
}

// The taskbar's VIP and Missions buttons (CSOTaskbar::OnCommand "OpenVIPInfo" / "OpenQuestDlg") open a main panel
// page (0x15 / 0xf, page manager 0x104528c0 vfunc 0x64) the offline game has nothing for: they open the game's
// browser window instead, on the profile page (statistics, medals) and the missions page.
static const DWORD GAMEUI_VIP_PAGE = 0x10597976, GAMEUI_VIP_RETURN = 0x10597984;
static const DWORD GAMEUI_QUEST_PAGE = 0x105972a3, GAMEUI_QUEST_RETURN = 0x105972b1;
static const DWORD GAMEUI_PAGE_MANAGER = 0x104528c0;
static DWORD g_vipReturn, g_questReturn;

static void OpenWebView(const char* view)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"view\":\"%s\"}", view);
    bool told = HttpPost(L"/api/openview", body);
    Log("taskbar: opening the %s page%s", view, told ? "" : " (server not reachable)");
    void* panel = *(void**)(GAMEUI_MAINPANEL + g_gameUiDelta);
    if (panel)
        ((OpenWebEventFn)(GAMEUI_OPENWEBEVENT + g_gameUiDelta))(panel, nullptr);
}
static void __cdecl OnVipButton() { OpenWebView("profile"); }
static void __cdecl OnMissionsButton() { OpenWebView("missions"); }

static __declspec(naked) void VipStub()
{
    __asm {
        pushad
        call OnVipButton
        popad
        jmp dword ptr [g_vipReturn]
    }
}

static __declspec(naked) void MissionsStub()
{
    __asm {
        pushad
        call OnMissionsButton
        popad
        jmp dword ptr [g_questReturn]
    }
}

// jumped to from the BotSetting branch: run the C++ side, then leave OnCommand through its own epilogue
static __declspec(naked) void BotSettingStub()
{
    __asm {
        pushad
        call OnAiSettingsButton
        popad
        jmp dword ptr [g_botSettingReturn]
    }
}

// ---------------------------------------------------------------- client.dll scoreboard
// The scoreboard builder (client.dll RVA 0x1100e20) formats every team row with an unbounded swprintf helper
// (RVA 0xc5aca0: count 0x7fffffff) into small stack buffers. The spectators row (team 3) can carry an
// uninitialised score (seen: 1650543732, the text "tLab"), and "1650543732 (Muertes)" plus its terminator is
// 42 bytes in a 40-byte buffer: the stack cookie fails and the game closes while loading the match. Those calls
// go through bounded copies (the row shows a cut text instead).
static int __cdecl ScoreFmt20(wchar_t* buf, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf_s(buf, 20, _TRUNCATE, fmt, ap);
    va_end(ap);
    return r;
}
static int __cdecl ScoreFmt50(wchar_t* buf, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf_s(buf, 50, _TRUNCATE, fmt, ap);
    va_end(ap);
    return r;
}
static int __cdecl ScoreFmt1024(wchar_t* buf, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    return r;
}

static void FixScoreboard(HMODULE client)
{
    const DWORD helper = 0xc5aca0;
    struct Site
    {
        DWORD rva;
        void* fn;
    } sites[] = {
        { 0x110153e, (void*)ScoreFmt1024 }, // ebp-0x914 (team name)
        { 0x11015af, (void*)ScoreFmt20 },   // ebp-0x38: 0x28 bytes
        { 0x1101760, (void*)ScoreFmt20 },
        { 0x11017f1, (void*)ScoreFmt20 },
        { 0x110183d, (void*)ScoreFmt20 },
        { 0x11016f0, (void*)ScoreFmt50 },   // ebp-0x114: 0x64 bytes
        { 0x1101724, (void*)ScoreFmt50 },
        { 0x11018dd, (void*)ScoreFmt50 },   // ebp-0xb0: 0x64 bytes
        { 0x1101911, (void*)ScoreFmt50 },
        { 0x11019ce, (void*)ScoreFmt50 },
    };
    unsigned char* base = (unsigned char*)client;
    for (auto& s : sites)
    {
        unsigned char* p = base + s.rva;
        if (!Readable(p, 5) || p[0] != 0xe8 || p + 5 + *(int*)(p + 1) != base + helper)
        {
            Log("client.dll scoreboard: unexpected code at +0x%lx, not fixed", s.rva);
            return;
        }
    }
    for (auto& s : sites)
    {
        unsigned char* p = base + s.rva;
        DWORD old;
        VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        *(int*)(p + 1) = (int)((unsigned char*)s.fn - (p + 5));
        VirtualProtect(p + 1, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), base + 0x1100e20, 0x1000);
    Log("client.dll scoreboard: team rows formatted with bounds (no crash on a bad spectators score)");
}

static void HookAiSettingsButton(HMODULE gameUi)
{
    g_gameUiDelta = (DWORD)gameUi - GAMEUI_PREFERRED_BASE;
    unsigned char* site = (unsigned char*)(GAMEUI_BOTSETTING + g_gameUiDelta);
    if (!Readable(site, 5))
        return;
    if (site[0] == 0xe9)
        return; // already hooked
    DWORD expectAbs = 0x10b844c8 + g_gameUiDelta;
    if (site[0] != 0xa1 || *(DWORD*)(site + 1) != expectAbs)
    {
        Log("GameUI room panel has unexpected code: AI settings button left as it is");
        return;
    }
    g_botSettingReturn = GAMEUI_ONCOMMAND_EPILOGUE + g_gameUiDelta;
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xe9;
    *(int*)(site + 1) = (int)((unsigned char*)BotSettingStub - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("room AI settings button opens the AI player editor");
}

// "call <page manager>" in the button's branch becomes "jmp stub"; the stub jumps back past the page call
static void HookTaskbarButton(DWORD siteVa, DWORD returnVa, void* stub, DWORD* returnSlot, const char* what)
{
    unsigned char* site = (unsigned char*)(siteVa + g_gameUiDelta);
    if (!Readable(site, 5) || site[0] == 0xe9)
        return;
    if (site[0] != 0xe8 || site + 5 + *(int*)(site + 1) != (unsigned char*)(GAMEUI_PAGE_MANAGER + g_gameUiDelta))
    {
        Log("GameUI taskbar has unexpected code: %s button left as it is", what);
        return;
    }
    *returnSlot = returnVa + g_gameUiDelta;
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xe9;
    *(int*)(site + 1) = (int)((unsigned char*)stub - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("taskbar %s button opens its web page", what);
}

// ---------------------------------------------------------------- create-room mode list: scrolling
// The "Other modes" entries are in the mode grid now (CSNZ_Server Data/GameModeList.csv: mode_fun_order 0), more
// rows than CModeSelectDialogV2 has room for. Its tiles are made in code (0x102e5cc0: slot s at
// (s % 4) * 224, (s / 4) * 58 below the first row) and kept in a vector at +0x1bc / +0x1c0 of 0x34-byte entries
// (+0x20 background, +0x24 icon, +0x2c label, +0x30 check button: only grid tiles have one; the 4 hot modes come
// first). The dialog gets a vgui ScrollBar and the mouse wheel; rows outside the window are hidden.
// Panel vtable: 43 SetVisible, 104 OnThink, 117 OnMouseWheeled (159 slots); ScrollBar adds 159 SetValue,
// 160 GetValue, 161 SetRange, 163 SetRangeWindow, 171 SetButtonPressedScrollValue.
static const DWORD GAMEUI_MODEDLG_VTABLES[] = { 0x108d287c, 0x108d2c74, 0x108d3074 }; // V2, _Select, _Check
static const DWORD GAMEUI_NEW = 0x107b1428;            // operator new (cdecl)
static const DWORD GAMEUI_SCROLLBAR_CTOR = 0x1071f8c0; // ScrollBar(Panel* parent, const char* name, bool vertical)
static const DWORD GAMEUI_SETBOUNDS = 0x106f84e0;      // Panel::SetBounds(x, y, w, h)
static const int MODE_ROW = 58;
static void* g_ipanel;                                  // vgui2 IPanel (VGUI_Panel007)
static void* g_modeThink[3];
static void* g_modeWheel[3];

typedef void(__fastcall* PanelVoidFn)(void*, void*);
typedef void(__fastcall* PanelIntArgFn)(void*, void*, int);
typedef void(__fastcall* Panel2IntFn)(void*, void*, int, int);
typedef int(__fastcall* PanelGetIntFn)(void*, void*);
typedef void(__fastcall* PanelBoundsFn)(void*, void*, int, int, int, int);
typedef void*(__fastcall* ScrollBarCtorFn)(void*, void*, void*, const char*, bool);
typedef void*(__cdecl* GameNewFn)(size_t);
typedef void(__fastcall* IPanelPosFn)(void*, void*, DWORD, int*, int*);
typedef void(__fastcall* IPanelSetPosFn)(void*, void*, DWORD, int, int);
typedef bool(__fastcall* IPanelBoolFn)(void*, void*, DWORD);

static void** Vt(void* obj) { return *(void***)obj; }
static DWORD VPanelOf(void* panel) { return *(DWORD*)((unsigned char*)panel + 0x28); }
static void IPanelGetPos(void* p, int& x, int& y) { ((IPanelPosFn)Vt(g_ipanel)[3])(g_ipanel, nullptr, VPanelOf(p), &x, &y); }
static void IPanelSetPos(void* p, int x, int y) { ((IPanelSetPosFn)Vt(g_ipanel)[2])(g_ipanel, nullptr, VPanelOf(p), x, y); }
static void IPanelGetSize(void* p, int& w, int& h) { ((IPanelPosFn)Vt(g_ipanel)[5])(g_ipanel, nullptr, VPanelOf(p), &w, &h); }
static bool IPanelVisible(void* p) { return ((IPanelBoolFn)Vt(g_ipanel)[19])(g_ipanel, nullptr, VPanelOf(p)); }
static void SetVisibleV(void* p, bool on) { ((PanelIntArgFn)Vt(p)[43])(p, nullptr, on ? 1 : 0); }

struct ModeTile
{
    void* panel[4];   // background, icon, label, check button
    int x[4], y[4];   // where the dialog put them
    bool hidden[4];   // hidden by the scrolling
};
static struct
{
    unsigned char* dlg = nullptr;
    unsigned char* begin = nullptr;
    int entries = 0;
    void* firstPanel = nullptr;
    std::vector<ModeTile> tiles;
    int rows = 0, shown = 0, top = -1;
    void* bar = nullptr;
} g_modes;

static int ModeVtableIndex(void* dlg)
{
    for (int i = 0; i < 3; i++)
        if (*(DWORD*)dlg == GAMEUI_MODEDLG_VTABLES[i] + g_gameUiDelta)
            return i;
    return 0;
}

static void ModeScrollApply(int top)
{
    g_modes.top = top;
    for (size_t i = 0; i < g_modes.tiles.size(); i++)
    {
        ModeTile& t = g_modes.tiles[i];
        int row = (int)i / 4;
        bool in = row >= top && row < top + g_modes.shown;
        for (int k = 0; k < 4; k++)
        {
            if (!t.panel[k])
                continue;
            IPanelSetPos(t.panel[k], t.x[k], t.y[k] - top * MODE_ROW);
            if (k < 3)
                SetVisibleV(t.panel[k], in);
            else if (!in && IPanelVisible(t.panel[k]))
            {
                SetVisibleV(t.panel[k], false); // the check button is only shown in some dialogs: keep that
                t.hidden[k] = true;
            }
            else if (in && t.hidden[k])
            {
                SetVisibleV(t.panel[k], true);
                t.hidden[k] = false;
            }
        }
    }
}

// the dialog (re)built its tiles: read them, add the scroll bar when they don't fit
static void ModeScrollBuild(unsigned char* dlg, unsigned char* begin, int entries)
{
    if (g_modes.dlg != dlg)
        g_modes.bar = nullptr; // another dialog object: its own scroll bar
    g_modes.dlg = dlg;
    g_modes.begin = begin;
    g_modes.entries = entries;
    g_modes.firstPanel = entries ? *(void**)(begin + 0x20) : nullptr;
    g_modes.tiles.clear();
    g_modes.top = -1;
    for (int i = 0; i < entries; i++)
    {
        unsigned char* e = begin + i * 0x34;
        if (!*(void**)(e + 0x30) || !*(void**)(e + 0x20))
            continue; // a hot mode
        ModeTile t = {};
        static const int offs[4] = { 0x20, 0x24, 0x2c, 0x30 };
        for (int k = 0; k < 4; k++)
        {
            t.panel[k] = *(void**)(e + offs[k]);
            if (t.panel[k])
                IPanelGetPos(t.panel[k], t.x[k], t.y[k]);
        }
        g_modes.tiles.push_back(t);
    }
    if (g_modes.tiles.empty())
        return;
    int w = 0, h = 0;
    IPanelGetSize(dlg, w, h);
    int firstY = g_modes.tiles[0].y[0];
    g_modes.rows = ((int)g_modes.tiles.size() + 3) / 4;
    g_modes.shown = max(1, (h - firstY - 8) / MODE_ROW);
    bool scroll = g_modes.rows > g_modes.shown;
    if (scroll && !g_modes.bar)
    {
        void* mem = ((GameNewFn)(GAMEUI_NEW + g_gameUiDelta))(0xd0);
        g_modes.bar = mem ? ((ScrollBarCtorFn)(GAMEUI_SCROLLBAR_CTOR + g_gameUiDelta))(mem, nullptr, dlg, "ModeScrollBar", true)
                          : nullptr;
        if (g_modes.bar)
            ((PanelBoundsFn)(GAMEUI_SETBOUNDS + g_gameUiDelta))(g_modes.bar, nullptr, w - 24, firstY, 16,
                                                                  g_modes.shown * MODE_ROW - 10);
    }
    if (g_modes.bar)
    {
        void** v = Vt(g_modes.bar);
        ((Panel2IntFn)v[161])(g_modes.bar, nullptr, 0, g_modes.rows);         // SetRange
        ((PanelIntArgFn)v[163])(g_modes.bar, nullptr, g_modes.shown);          // SetRangeWindow
        ((PanelIntArgFn)v[171])(g_modes.bar, nullptr, 1);                      // SetButtonPressedScrollValue
        ((PanelIntArgFn)v[159])(g_modes.bar, nullptr, 0);                      // SetValue
        SetVisibleV(g_modes.bar, scroll);
    }
    Log("mode list: %d modes in %d rows, %d shown%s", (int)g_modes.tiles.size(), g_modes.rows, g_modes.shown,
        scroll ? ": scroll bar added" : "");
    ModeScrollApply(0);
}

static void ModeScrollUpdate(unsigned char* dlg, int wheel)
{
    __try
    {
        unsigned char* begin = *(unsigned char**)(dlg + 0x1bc);
        unsigned char* end = *(unsigned char**)(dlg + 0x1c0);
        int entries = begin && end > begin ? (int)((end - begin) / 0x34) : 0;
        if (entries > 400)
            return;
        void* first = entries ? *(void**)(begin + 0x20) : nullptr;
        if (dlg != g_modes.dlg || begin != g_modes.begin || entries != g_modes.entries || first != g_modes.firstPanel)
            ModeScrollBuild(dlg, begin, entries);
        if (!g_modes.bar || g_modes.rows <= g_modes.shown)
            return;
        void** v = Vt(g_modes.bar);
        if (wheel)
            ((PanelIntArgFn)v[159])(g_modes.bar, nullptr, ((PanelGetIntFn)v[160])(g_modes.bar, nullptr) - wheel);
        int top = ((PanelGetIntFn)v[160])(g_modes.bar, nullptr);
        top = max(0, min(top, g_modes.rows - g_modes.shown));
        if (top != g_modes.top)
            ModeScrollApply(top);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int reports = 0;
        if (reports++ < 3)
            Log("mode list: scrolling failed (exception)");
    }
}

static void __fastcall ModeDlgThink(unsigned char* dlg, void*)
{
    ((PanelVoidFn)g_modeThink[ModeVtableIndex(dlg)])(dlg, nullptr);
    ModeScrollUpdate(dlg, 0);
}

static void __fastcall ModeDlgWheel(unsigned char* dlg, void*, int delta)
{
    ModeScrollUpdate(dlg, delta); // the dialog itself does nothing with the wheel (Panel's passes it to the parent)
}

static void HookModeDialog()
{
    HMODULE vgui = GetModuleHandleA("vgui2.dll");
    typedef void*(__cdecl * CreateInterfaceFn)(const char*, int*);
    CreateInterfaceFn ci = vgui ? (CreateInterfaceFn)GetProcAddress(vgui, "CreateInterface") : nullptr;
    g_ipanel = ci ? ci("VGUI_Panel007", nullptr) : nullptr;
    if (!g_ipanel)
    {
        Log("mode list: vgui2 IPanel not found, no scrolling");
        return;
    }
    for (int i = 0; i < 3; i++)
    {
        void** vt = (void**)(GAMEUI_MODEDLG_VTABLES[i] + g_gameUiDelta);
        if (!Readable(vt + 117, 4) || vt[104] == (void*)ModeDlgThink)
            return;
        g_modeThink[i] = vt[104];
        g_modeWheel[i] = vt[117];
        DWORD old;
        VirtualProtect(vt + 104, 14 * 4, PAGE_READWRITE, &old);
        vt[104] = (void*)ModeDlgThink;
        vt[117] = (void*)ModeDlgWheel;
        VirtualProtect(vt + 104, 14 * 4, old, &old);
    }
    Log("mode list: the create-room mode grid scrolls");
}

// ---------------------------------------------------------------- medal texts
// My Info's Medals tab shows each medal (an honor quest of CSNZ_Server) with the game's texts CSO_Quest<2000+id>_Desc1
// / _Cond<n>, which describe the original online conditions. CSNZ_Server's offline ones (GET /api/questtexts)
// replace them: vgui2's ILocalize (VGUI_Localize003, CSO build) finds strings with vtable slots 3 and 4 (both
// (const char* token) -> wchar_t*, "#" optional); both are hooked and answer these tokens first. The table is
// built once and never changed afterwards, so the game's threads can read it freely.
static std::map<std::string, std::wstring>* g_questTexts;
static void* g_locFind[2];
typedef const wchar_t*(__fastcall* LocFindFn)(void* self, void* edx, const char* token);

static const wchar_t* QuestText(const char* token)
{
    std::map<std::string, std::wstring>* texts = g_questTexts;
    if (!texts || !token)
        return nullptr;
    if (*token == '#')
        token++;
    if (_strnicmp(token, "CSO_Quest", 9) != 0)
        return nullptr;
    std::string key(token);
    for (char& c : key)
        c = (char)tolower((unsigned char)c);
    auto it = texts->find(key);
    return it == texts->end() ? nullptr : it->second.c_str();
}

static const wchar_t* __fastcall LocFind3(void* self, void*, const char* token)
{
    const wchar_t* text = QuestText(token);
    return text ? text : ((LocFindFn)g_locFind[0])(self, nullptr, token);
}

static const wchar_t* __fastcall LocFind4(void* self, void*, const char* token)
{
    const wchar_t* text = QuestText(token);
    return text ? text : ((LocFindFn)g_locFind[1])(self, nullptr, token);
}

// a flat JSON object of strings ({"key":"text",...}) -> lowercase key -> UTF-16 text
static std::map<std::string, std::wstring>* ParseTexts(const std::string& json)
{
    auto* out = new std::map<std::string, std::wstring>();
    size_t i = 0;
    auto readString = [&](std::string& s) -> bool {
        i = json.find('"', i);
        if (i == std::string::npos)
            return false;
        for (i++; i < json.size() && json[i] != '"'; i++)
        {
            if (json[i] != '\\' || i + 1 >= json.size())
            {
                s += json[i];
                continue;
            }
            char e = json[++i];
            if (e == 'n')
                s += '\n';
            else if (e == 't')
                s += '\t';
            else if (e == 'u' && i + 4 < json.size())
            {
                unsigned cp = strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp < 0x80)
                    s += (char)cp;
                else if (cp < 0x800)
                    s += (char)(0xc0 | (cp >> 6)), s += (char)(0x80 | (cp & 0x3f));
                else
                    s += (char)(0xe0 | (cp >> 12)), s += (char)(0x80 | ((cp >> 6) & 0x3f)), s += (char)(0x80 | (cp & 0x3f));
            }
            else
                s += e;
        }
        i++;
        return true;
    };
    for (;;)
    {
        std::string key, value;
        if (!readString(key))
            break;
        size_t colon = json.find(':', i);
        if (colon == std::string::npos)
            break;
        i = colon + 1;
        if (!readString(value))
            break;
        int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), &w[0], n);
        for (char& c : key)
            c = (char)tolower((unsigned char)c);
        (*out)[key] = w;
    }
    return out;
}

static DWORD WINAPI QuestTexts(LPVOID)
{
    typedef void*(__cdecl * CreateInterfaceFn)(const char*, int*);
    void** loc = nullptr;
    for (int i = 0; i < 600 && !loc; i++)
    {
        HMODULE vgui = GetModuleHandleA("vgui2.dll");
        CreateInterfaceFn ci = vgui ? (CreateInterfaceFn)GetProcAddress(vgui, "CreateInterface") : nullptr;
        loc = ci ? (void**)ci("VGUI_Localize003", nullptr) : nullptr;
        if (!loc)
            Sleep(1000);
    }
    if (!loc)
    {
        Log("medal texts: vgui2 ILocalize not found");
        return 0;
    }
    std::string json;
    for (int i = 0; i < 120 && json.size() < 2; i++)
    {
        json = HttpGet(L"/api/questtexts");
        if (json.size() < 2)
            Sleep(5000);
    }
    if (json.size() < 2)
    {
        Log("medal texts: CSNZ_Server didn't answer /api/questtexts, the Medals tab keeps the game's texts");
        return 0;
    }
    g_questTexts = ParseTexts(json);

    void** vt = *(void***)loc;
    if (vt[3] == (void*)LocFind3)
        return 0;
    g_locFind[0] = vt[3];
    g_locFind[1] = vt[4];
    DWORD old;
    VirtualProtect(vt + 3, 2 * sizeof(void*), PAGE_READWRITE, &old);
    vt[3] = (void*)LocFind3;
    vt[4] = (void*)LocFind4;
    VirtualProtect(vt + 3, 2 * sizeof(void*), old, &old);
    Log("medal texts: %d texts of the Medals tab replaced by the offline conditions", (int)g_questTexts->size());
    return 0;
}

// Steam achievements: CSNZ_Server sends the game's Steam packet (192) only to a client whose Steam API is up (the
// game's handler has no ISteamUserStats otherwise and crashes). The login doesn't say it (user name + password), so
// botassist checks SteamAPI_GetHSteamPipe like the game does and tells the server (POST /api/steam, answered once a
// character is logged in on this computer); again now and then, for a new login.
static DWORD WINAPI ReportSteam(LPVOID)
{
    bool told = false;
    for (;;)
    {
        Sleep(told ? 30000 : 5000);
        HMODULE api = GetModuleHandleA("steam_api.dll");
        typedef int(__cdecl * PipeFn)();
        PipeFn pipe = api ? (PipeFn)GetProcAddress(api, "SteamAPI_GetHSteamPipe") : nullptr;
        if (!pipe || !pipe())
            continue;
        std::string reply;
        bool ok = HttpPost(L"/api/steam", "{\"steam\":true}", &reply) && reply.find("\"ok\":true") != std::string::npos;
        if (ok && !told)
            Log("Steam: the Steam API is up, CSNZ_Server sends the Steam achievements");
        told = ok;
    }
}

static DWORD WINAPI Watcher(LPVOID)
{
    HMODULE hooked = NULL;
    HMODULE uiHooked = NULL;
    HMODULE clientFixed = NULL;
    for (;;)
    {
        HMODULE client = GetModuleHandleA("client.dll");
        if (client && client != clientFixed)
        {
            clientFixed = client;
            FixScoreboard(client);
        }
        HMODULE ui = GetModuleHandleA("GameUI.dll");
        if (ui && ui != uiHooked && cfg.enabled)
        {
            uiHooked = ui;
            HookAiSettingsButton(ui);
            HookTaskbarButton(GAMEUI_VIP_PAGE, GAMEUI_VIP_RETURN, (void*)VipStub, &g_vipReturn, "VIP");
            HookTaskbarButton(GAMEUI_QUEST_PAGE, GAMEUI_QUEST_RETURN, (void*)MissionsStub, &g_questReturn, "Missions");
            HookModeDialog();
        }
        HMODULE mp = GetModuleHandleA("mp.dll");
        if (mp && mp != hooked)
        {
            g_edictToVars = -1;
            g_edictToPrivate = -1;
            for (auto& b : g_bots)
                b = BotState();
            g_nav.map.clear();
            if (Install(mp))
                hooked = mp;
        }
        else if (!mp)
            hooked = NULL;
        if (mp && mp == hooked)
            CheckClock();
        Sleep(500);
    }
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Miles may unload ASI files it doesn't recognise: keep this one loaded for the whole session
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&DllMain, &pinned);
        DisableThreadLibraryCalls(self);
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_matchLock);
        InitializeCriticalSection(&g_clockLock);
        GetModuleFileNameA(self, g_dir, sizeof(g_dir));
        char* slash = strrchr(g_dir, '\\');
        if (slash)
            slash[1] = 0;
        LoadSettings();
        Log("botassist loaded (%s)", cfg.enabled ? "enabled" : "disabled in botassist.ini");
        CreateThread(NULL, 0, Watcher, NULL, 0, NULL);
        if (cfg.enabled)
        {
            CreateThread(NULL, 0, QuestTexts, NULL, 0, NULL);
            CreateThread(NULL, 0, ReportSteam, NULL, 0, NULL);
        }
    }
    return TRUE;
}
