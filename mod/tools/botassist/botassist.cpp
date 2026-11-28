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
#include <shlwapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
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
static const int ENG_FINDENTITYINSPHERE = 17, ENG_TRACELINE = 33, ENG_TRACEHULL = 36, ENG_CVARGETFLOAT = 62, ENG_CVARGETSTRING = 63,
    ENG_CVARSETSTRING = 65, ENG_SERVERCOMMAND = 42, ENG_MESSAGEBEGIN = 49, ENG_MESSAGEEND = 50, ENG_WRITEBYTE = 51,
    ENG_WRITESHORT = 53, ENG_WRITECOORD = 58, ENG_CMD_ARGV = 88, ENG_LOADFILE = 100, ENG_FREEFILE = 101, ENG_RUNPLAYERMOVE = 110;
static const int PLAYER_TEAM = 0x6c; // CBasePlayer::m_iTeam (1 TR, 2 CT); CBaseEntity::pev is at +8
// entvars_t offsets (CSO adds fields; from mp.dll's gEntvarsDescription save table)
static const int EV_WEAPONMODEL = 0xd0, EV_CLASSNAME = 0x0, EV_ORIGIN = 0x8, EV_VELOCITY = 0x20, EV_PUNCHANGLE = 0x68, EV_VANGLE = 0x74, EV_ANGLES = 0x50, EV_ABSMIN = 0xd4,
    EV_ABSMAX = 0xe0, EV_HEALTH = 0x178, EV_TAKEDAMAGE = 0x190, EV_DEADFLAG = 0x194, EV_VIEW_OFS = 0x198,
    EV_FLAGS = 0x1c8, EV_TEAM = 0x1d8, EV_ARMORVALUE = 0x1e8, EV_NETNAME = 0x200, EV_CONTAINING_ENTITY = 0x238;
static const int GV_TIME = 0x0, GV_MAPNAME = 0xc, GV_STRINGBASE = 0x98;
static const int FL_CLIENT = 0x8, FL_INWATER = 0x10, FL_MONSTER = 0x20, FL_FROZEN = 0x1000, FL_FAKECLIENT = 0x2000;
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
typedef void (*TraceHullFn)(const float* v1, const float* v2, int flags, int hull, void* skip, TraceResult* tr);
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
typedef void (*WriteStringFn)(const char* value);
static const DWORD MP_BEAM_SPRITE = 0x12183278; // short: a sprite index mp.dll uses for its own TE_BEAMPOINTS
typedef void (*RunPlayerMoveFn)(void* fakeclient, const float* angles, float fwd, float side, float up,
                                unsigned short buttons, unsigned char impulse, unsigned char msec);
typedef void (*ClientCommandFn)(void* edict);

static FindEntityInSphereFn g_FindEntityInSphere;
static MessageBeginFn g_MessageBegin;
static MessageEndFn g_MessageEnd;
static WriteIntFn g_WriteByte, g_WriteShort;
static WriteCoordFn g_WriteCoord;
static WriteStringFn g_WriteString;
static TraceLineFn g_TraceLine;
static TraceHullFn g_TraceHull;
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
    bool zombieFlee = true;    // PvP zombie modes: human bots back off from zombies close by (modes/zombie_humans.inl)
    float zombieFleeDistance = 450.0f;
    std::vector<int> primaries, pistols, melees;
    int skill = -1;            // 0..7, -1: the room's bot difficulty
    std::string voices = "radio"; // radio (radio commands, shown as text; the default), normal (voice chatter), off
    bool randomCharacters = true; // any character of the game (botassist_characters.txt)
    float refillAfter = 6.0f;  // seconds of firing that empty about half of the ammo: then buy ammo in a buy zone
    float minArmor = 50.0f;    // below this armor, buy kevlar + helmet in a buy zone
    float wbMin = 0.5f, wbMax = 4.0f; // weapon balance (weapon_balance.inl): the least and most a weapon's damage is scaled
    bool sameTeam = true;      // where there are monsters, every bot joins the human team (no enemy bots under the map)
    std::vector<int> characters[3]; // by team: [1] TR, [2] CT
    std::unordered_map<int, int> characterTeam; // class item id -> team
    std::unordered_map<int, int> characterClass; // class item id -> InGameID (the number joinclass takes)
    float followDistance = 180.0f;
    int zgPlayers = -1;        // Zombie Giant: room AI players that join as fake players who can be the giant (-1 all)
    bool zgPlayerGiant = true; // Zombie Giant: you can be the giant too
    float zsrEventDivisor = 3.0f; // Zombie Disaster Zone: events per round = rift level / this (the game's: 32)
    int zsrBotMoney = 10000;   // Zombie Disaster Zone: a bot's money on each new life, at least (0: the game's)
    // Zombie Disaster Zone: monsters' attack / health bonus per rift level above 1, in percent (negative: the game's)
    float zsrMonsterAttack = 2.6316f, zsrMonsterHealth = 15.7895f, zsrBossAttack = 1.3158f, zsrBossHealth = 10.5263f;
    float zsrExponent = 1.0f; // bonus = rate * (level - 1) ^ this (the game's: 1.85)
    float rescan = 0.15f;
    bool logTargets = true;
    bool logCommands = true;
    std::vector<std::string> ignore;
} cfg;

static char g_dir[MAX_PATH];
static bool g_aimLines = false; // the bots' aim lasers (botassist.ini AimLines; the scenario orders menu turns them on/off)
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

// weapons the game crashes on when they're fired (gen_weapons.py CRASHING): Dual Uzi W in a zombie mode (README "Weapons
// without models of their own"). Left out whatever botassist_weapons.txt and botassist_gdm_weapons.txt say: the
// launcher keeps the player's copies of those lists, so installs made before still list it.
static bool CrashingWeapon(const char* res)
{
    static const char* const kCrashing[] = { "dualuziw" };
    for (const char* c : kCrashing)
        if (!_stricmp(res, c))
            return true;
    return false;
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
    g_aimLines = GetPrivateProfileIntA("BotAssist", "AimLines", 0, ini) != 0;
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
    cfg.zombieFlee = GetPrivateProfileIntA("BotAssist", "ZombieFlee", 1, ini) != 0;
    cfg.zombieFleeDistance = (float)GetPrivateProfileIntA("BotAssist", "ZombieFleeDistance", 450, ini);
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
        int id, cat, crashing = 0;
        char line[256], res[128];
        cfg.weaponCategory.clear();
        while (fgets(line, sizeof(line), f))
        {
            if (line[0] == ';' || sscanf(line, "%d %d", &id, &cat) != 2)
                continue;
            const char* semi = strchr(line, ';');
            bool named = semi && sscanf(semi + 1, " %127s", res) == 1;
            if (named && CrashingWeapon(res))
            {
                crashing++;
                continue;
            }
            (cat == 1 ? cfg.pistols : cat == 11 ? cfg.melees : cfg.primaries).push_back(id);
            if (named)
            {
                std::string model = std::string("models/p_") + res + ".mdl";
                for (auto& ch : model)
                    ch = (char)tolower(ch);
                cfg.weaponCategory[model] = cat;
                cfg.weaponModel[id] = model;
            }
        }
        fclose(f);
        if (crashing)
            Log("botassist_weapons.txt: %d weapon(s) left out: the game crashes when they're fired (Dual Uzi W)", crashing);
        // Counter-Strike's own weapons whose model isn't named after the item (usp45 -> p_usp.mdl...): the pistols
        // everyone spawns with must be known to drop them ("None" secondary)
        static const struct { const char* model; int cat; } aliases[] = {
            { "models/p_usp.mdl", 1 }, { "models/p_deagle.mdl", 1 }, { "models/p_elite.mdl", 1 },
            { "models/p_glock18.mdl", 1 }, { "models/p_p228.mdl", 1 }, { "models/p_fiveseven.mdl", 1 } };
        for (auto& a : aliases)
            cfg.weaponCategory.emplace(a.model, a.cat);
    }
    cfg.followDistance = IniFloat(ini, "FollowDistance", "180");
    cfg.wbMin = (std::max)(0.05f, IniFloat(ini, "WeaponBalanceMin", "0.5"));
    cfg.wbMax = (std::max)(cfg.wbMin, IniFloat(ini, "WeaponBalanceMax", "4"));
    cfg.zgPlayers = GetPrivateProfileIntA("BotAssist", "ZombieGiantPlayers", -1, ini);
    cfg.zsrEventDivisor = IniFloat(ini, "ZSRiftEventDivisor", "3");
    cfg.zsrBotMoney = GetPrivateProfileIntA("BotAssist", "ZSRiftBotMoney", 10000, ini);
    cfg.zsrMonsterAttack = IniFloat(ini, "ZSRiftMonsterAttack", "2.6316");
    cfg.zsrMonsterHealth = IniFloat(ini, "ZSRiftMonsterHealth", "15.7895");
    cfg.zsrBossAttack = IniFloat(ini, "ZSRiftBossAttack", "1.3158");
    cfg.zsrBossHealth = IniFloat(ini, "ZSRiftBossHealth", "10.5263");
    cfg.zsrExponent = IniFloat(ini, "ZSRiftLevelExponent", "1");
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
// Human Scenario: see modes/human_scenario.inl
static bool HsIsEnemyClass(const char* classname);
static float HsScanRadius();
static bool HsFlying(void* e);
static bool HsClearShotAt(void* target, const struct TraceResult& tr, float lineLength, float targetDist);
static bool HsIsMissile(void* e);
static bool HsTooFar(void* e, float dist);
static float HsAimLead(void* target);
static void HsWatch();
// Zombie Shelter: see modes/zombie_shelter.inl (each returns at once outside the mode)
struct BotState;
static bool ShelterFriendlyClass(const char* cls);
static bool ShelterActive();
static bool ShelterGoal(void* bot, BotState& st, V3 pos, V3& goal, float& arrive, bool& wantMove);
static bool ShelterLook(void* bot, BotState& st, V3 eye, float& yaw, float& pitch);
static int ShelterSwing(void* bot, BotState& st, float yaw, float pitch); // 0, IN_ATTACK or IN_ATTACK2
static void ShelterBotThink(void* bot, BotState& st);
static void ShelterWatch();
static bool ShelterHoldFire(void* bot, BotState& st);
static void* ShelterTarget(void* bot, V3 eye, void* best);
static bool ShelterStandGround(void* bot); // Zombie Shelter: no backing off with the axe or at night
static bool ShelterPlayerCommand(void* player, const std::string& c);

static bool IsIgnored(const char* classname)
{
    if (ShelterFriendlyClass(classname))
        return true; // the shelter's turrets, walls... are the players'
    if (HsIsEnemyClass(classname))
        return false;
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

// bots in each other's way: ladders taken in turn, holdout places, who steps aside (the decisions, no game calls)
#include "crowding.inl"

// ---------------------------------------------------------------- navigation mesh (CS:CZ .nav, version 5)
// area attributes (CS:CZ): crouch, jump (a jump gets there), precise (no shortcuts or steering), no jump
static const unsigned char NAV_CROUCH = 1, NAV_JUMP = 2, NAV_PRECISE = 4, NAV_NO_JUMP = 8;
struct NavArea
{
    unsigned id;
    unsigned char attr = 0; // NAV_CROUCH...
    float lo[3], hi[3];
    float neZ, swZ;
    V3 center;
    std::vector<int> adj[4]; // N (-y), E (+x), S (+y), W (-x): indices into g_nav.areas
};
static unsigned g_navGen = 0; // counts changes of g_nav.areas (AreaAt's grid)
static size_t g_navBaseAreas = 0; // the mesh's own areas; after them, botassist's (the tops of ladders, BuildLadders)
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

static void PoiLoad(const std::string& map); // points of interest: see nav_tools.inl
static void PoiRebuild();

static void LoadRides(const std::string& map); // nav_tools.inl: the lifts, trains and teleports of a .navtxt mesh
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
    PoiLoad(map);
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
        a.attr = r.get<unsigned char>();
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
    g_navBaseAreas = g_nav.areas.size();
    g_navGen++;
    PoiRebuild();
    Log("%s: %u navigation areas loaded", path.c_str(), (unsigned)g_nav.areas.size());
    LoadRides(map);
}

// ---- what time botassist takes (logged every 30 s while bots play: "perf: ...")
static LARGE_INTEGER g_perfFreq;
static double g_perfMoveSec = 0, g_perfPathSec = 0, g_perfFrameSec = 0;
// the bot move hook in parts (PerfMark): where its time goes
static const char* const g_perfPartNames[] = { "upkeep", "setup", "zombies/orders", "modes", "nav", "fire" };
static double g_perfPart[6] = {};
static int g_perfMoves = 0, g_perfPaths = 0, g_perfAreaAt = 0;
static double PerfNow()
{
    LARGE_INTEGER t;
    if (!g_perfFreq.QuadPart)
        QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_perfFreq.QuadPart;
}
static void PerfMark(int part, double& since)
{
    double now = PerfNow();
    g_perfPart[part] += now - since;
    since = now;
}

// ---- which areas lie where: a grid of cells over the mesh, each with the areas over it, so AreaAt looks at a few areas
// instead of all of them (thousands, many times per bot and frame: with many bots it was most of botassist's time). Made
// again when the areas change (g_navGen: a new mesh, the ladders' and drawn links' areas) or their number does.
static struct
{
    unsigned gen = ~0u;
    size_t count = 0;
    float x0 = 0, y0 = 0, cell = 128.0f;
    int nx = 0, ny = 0;
    std::vector<std::vector<int>> cells;
} g_areaGrid;

static void AreaGridBuild()
{
    auto& G = g_areaGrid;
    G.gen = g_navGen;
    G.count = g_nav.areas.size();
    G.cells.clear();
    G.nx = G.ny = 0;
    if (g_nav.areas.empty())
        return;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (auto& a : g_nav.areas)
    {
        x0 = min(x0, a.lo[0] - 1), y0 = min(y0, a.lo[1] - 1);
        x1 = max(x1, a.hi[0] + 1), y1 = max(y1, a.hi[1] + 1);
    }
    G.cell = max(128.0f, max(x1 - x0, y1 - y0) / 512.0f);
    G.x0 = x0, G.y0 = y0;
    G.nx = (int)((x1 - x0) / G.cell) + 1;
    G.ny = (int)((y1 - y0) / G.cell) + 1;
    G.cells.assign((size_t)G.nx * G.ny, std::vector<int>());
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        int cx0 = (int)((a.lo[0] - 1 - x0) / G.cell), cx1 = (int)((a.hi[0] + 1 - x0) / G.cell);
        int cy0 = (int)((a.lo[1] - 1 - y0) / G.cell), cy1 = (int)((a.hi[1] + 1 - y0) / G.cell);
        for (int y = max(cy0, 0); y <= min(cy1, G.ny - 1); y++)
            for (int x = max(cx0, 0); x <= min(cx1, G.nx - 1); x++)
                G.cells[(size_t)y * G.nx + x].push_back((int)i);
    }
}

// the areas over the cells within `r` of p, each once at most per cell (an area may come from several cells)
template <class Fn> static void AreasNear(V3 p, float r, Fn fn)
{
    auto& G = g_areaGrid;
    if (G.gen != g_navGen || G.count != g_nav.areas.size())
        AreaGridBuild();
    if (!G.nx || !(fabsf(p.x) < 1e7f && fabsf(p.y) < 1e7f && r < 1e6f))
        return; // (nothing sensible there, and no float too big for an int)
    int cx0 = (int)floorf((p.x - r - G.x0) / G.cell), cx1 = (int)floorf((p.x + r - G.x0) / G.cell);
    int cy0 = (int)floorf((p.y - r - G.y0) / G.cell), cy1 = (int)floorf((p.y + r - G.y0) / G.cell);
    for (int y = max(cy0, 0); y <= min(cy1, G.ny - 1); y++)
        for (int x = max(cx0, 0); x <= min(cx1, G.nx - 1); x++)
            for (int i : G.cells[(size_t)y * G.nx + x])
                fn(i);
}

static int AreaAt(V3 p)
{
    g_perfAreaAt++;
    int best = -1;
    float bestScore = 1e30f;
    AreasNear(p, 0.0f, [&](int i) {
        const NavArea& a = g_nav.areas[i];
        if (p.x >= a.lo[0] - 1 && p.x <= a.hi[0] + 1 && p.y >= a.lo[1] - 1 && p.y <= a.hi[1] + 1)
        {
            // points come at floor level or at a player's origin (36 above it): the floor they belong to is the one around 18 below
            float dz = p.z - AreaZ(a, p.x, p.y), score = fabsf(dz - 18.0f);
            if (dz > -40.0f && dz < 120.0f && (score < bestScore || (score == bestScore && i < best)))
            {
                bestScore = score;
                best = i;
            }
        }
    });
    if (best >= 0)
        return best;
    AreasNear(p, 400.0f, [&](int i) { // off the mesh: the closest area (within 400 u, the cells round it)
        // to its edge: next to a big area, its centre can be thousands of units away (hs_desertstorm has one of 2500 x 5150)
        const NavArea& a = g_nav.areas[i];
        float dx = p.x < a.lo[0] ? a.lo[0] - p.x : p.x > a.hi[0] ? p.x - a.hi[0] : 0.0f;
        float dy = p.y < a.lo[1] ? a.lo[1] - p.y : p.y > a.hi[1] ? p.y - a.hi[1] : 0.0f;
        float dz = p.z - a.center.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < bestScore || (d == bestScore && i < best))
        {
            bestScore = d;
            best = i;
        }
    });
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
// points of interest: see nav_tools.inl
static float PoiAreaCost(int area);
static bool PoiAvoidAt(V3 at);
static int PoiSpotCount();
static V3 PoiSpotAt(int i);
// extra cost per nav area of the path being found, null for none (Zombie Conquest: each bot's own way, see ZConNewRoute)
static const std::vector<float>* g_navRouteCost = nullptr;
// areas this bot couldn't get into a moment ago (see NavProgress): a path through them is the last choice, not forbidden
struct NavAvoid
{
    int area;
    float until;
};
static const float NAV_AVOID_COST = 1500.0f, NAV_AVOID_TIME = 12.0f;
static const std::vector<NavAvoid>* g_navAvoid = nullptr;
static float NavAvoidCost(int area, float now)
{
    if (g_navAvoid)
        for (auto& a : *g_navAvoid)
            if (a.area == area && now < a.until && a.until - now <= NAV_AVOID_TIME) // (a new map starts the clock again)
                return NAV_AVOID_COST;
    return 0.0f;
}

// ---- places that kill: the map's trigger_hurt brushes and drops too high to survive
// botassist's own paths (A* below: companions, Manhunt, Zombie Giant, human bots backing off from zombies) treated a
// jump off a balcony like a walk, so bots took the drop instead of the stairs, and some dropped into pits whose floor
// is a trigger_hurt (7 bots in one Mutation round). The hurt brushes (solid, entvars +0x11c != SOLID_NOT; damage at
// +0x210) are listed again every 5 s (doors and buttons can turn them on and off); nav areas inside one are left out
// of every path, a drop between areas costs extra beyond a step and drops above HURT_DROP_MAX are never taken.
static const int EV_SOLID = 0x11c, EV_DMG = 0x210;
// pev->maxspeed (entvars, CSO layout). The game holds a player in place with a max speed of 1 (the freeze time, Soccer's
// kickoff) or FL_FROZEN.
static const int EV_MAXSPEED = 0x240;
static bool HeldStill(void* e)
{
    unsigned char* v = Vars(e);
    return F<float>(v, EV_MAXSPEED) <= 1.0f || (F<int>(v, EV_FLAGS) & FL_FROZEN);
}

// ---- links nobody can walk (crowding.inl's BlockedLink): a wall the mesh doesn't know about
// The meshes link areas through func_walls, doors, barricades and breakables that came later (zm_chosun: a bot at the
// foot of func_wall *214 going for a point 93 u up, 17 stuck reports in a row), and each bot learnt it alone, for
// NAV_AVOID_TIME, at a cost too small to change its way. A stuck report whose body trace hits something marks the link
// from the bot's area to the one ahead (NavProgress); from LINK_BLOCK_HITS reports on, every bot's path costs
// NAV_BLOCK_COST more through it: any other way first, the same way as before when there is none. Kept NAV_BLOCK_KEEP
// after the last report (a door NAV_BLOCK_KEEP_DOOR: they open), and no longer once what was in the way is gone (broken
// breakables are left not solid, a func_wall_toggle turns off, a freed edict gets another class).
static const float NAV_BLOCK_COST = 20000.0f, NAV_BLOCK_KEEP = 90.0f, NAV_BLOCK_KEEP_DOOR = 20.0f;
static std::vector<BlockedLink> g_navBlocked;
static std::vector<char> g_navBlockFrom; // per nav area: a blocked link starts here
static int g_navBlockLevel = -1;
static int g_navBlockReports = 0;

static void NavBlockStart()
{
    if (g_navBlockLevel == g_nav.level)
        return;
    g_navBlockLevel = g_nav.level;
    g_navBlocked.clear();
    g_navBlockFrom.clear();
    g_navBlockReports = 0;
}

static bool NavBlockDoor(const char* cls)
{
    return cls && (!strncmp(cls, "func_door", 9) || !strcmp(cls, "momentary_door"));
}

// a stuck report: the body trace from area `from` towards area `to` hit ent
static void NavBlockNote(int from, int to, void* ent, float now)
{
    NavBlockStart();
    if (from < 0 || to < 0 || from == to || !ent || from >= (int)g_nav.areas.size() || to >= (int)g_nav.areas.size())
        return;
    const char* cls = ClassName(ent);
    float keep = NavBlockDoor(cls) ? NAV_BLOCK_KEEP_DOOR : NAV_BLOCK_KEEP;
    bool was = LinkBlocked(g_navBlocked, from, to, now);
    NoteBlockedLink(g_navBlocked, from, to, now, keep, ent, F<int>(Vars(ent), EV_CLASSNAME));
    if (g_navBlockFrom.size() != g_nav.areas.size())
        g_navBlockFrom.assign(g_nav.areas.size(), 0);
    g_navBlockFrom[from] = 1;
    if (!was && LinkBlocked(g_navBlocked, from, to, now) && g_navBlockReports++ < 100)
    {
        V3 at = g_nav.areas[from].center;
        const char* model = String(F<int>(Vars(ent), 0xc8));
        Log("blocked: link %d->%d at (%.0f %.0f %.0f): %s %s in the way, every bot goes around for %.0f s", from, to, at.x,
            at.y, at.z, cls ? cls : "?", model ? model : "", keep);
    }
}

// before a path: the ones run out or whose blocker is gone leave the list
static void NavBlockPrune(float now)
{
    NavBlockStart();
    if (g_navBlocked.empty())
        return;
    bool changed = false;
    for (size_t i = g_navBlocked.size(); i-- > 0;)
    {
        BlockedLink& l = g_navBlocked[i];
        unsigned char* v = l.ent ? Vars(l.ent) : nullptr;
        bool gone = !v || F<int>(v, EV_CLASSNAME) != l.cls || !F<int>(v, EV_SOLID);
        if (gone && l.hits >= LINK_BLOCK_HITS && now < l.until && g_navBlockReports++ < 100)
            Log("blocked: link %d->%d open again (%s gone)", l.from, l.to, String(l.cls));
        if (gone || now >= l.until || now < l.last)
        {
            g_navBlocked.erase(g_navBlocked.begin() + i);
            changed = true;
        }
    }
    if (changed)
    {
        g_navBlockFrom.assign(g_nav.areas.size(), 0);
        for (const BlockedLink& l : g_navBlocked)
            if (l.from < (int)g_navBlockFrom.size())
                g_navBlockFrom[l.from] = 1;
    }
}

static float NavBlockCost(int from, int to, float now)
{
    if (from < 0 || from >= (int)g_navBlockFrom.size() || !g_navBlockFrom[from])
        return 0.0f;
    return LinkBlocked(g_navBlocked, from, to, now) ? NAV_BLOCK_COST : 0.0f;
}

static const float SAFE_DROP = 64.0f, HURT_DROP_MAX = 400.0f, DROP_COST = 4.0f;
struct HurtZone
{
    V3 lo, hi;
};
static std::vector<HurtZone> g_hurtZones;
static std::vector<char> g_navHurt; // per nav area: inside a hurt zone
static int g_hurtLevel = -1;
static float g_hurtNext = 0;

static bool InHurtZone(V3 p, float pad = 16.0f)
{
    for (auto& h : g_hurtZones)
        if (p.x > h.lo.x - pad && p.x < h.hi.x + pad && p.y > h.lo.y - pad && p.y < h.hi.y + pad &&
            p.z > h.lo.z - 72.0f && p.z < h.hi.z + pad)
            return true;
    return false;
}

static void UpdateHurtZones()
{
    float now = Now();
    if (g_hurtLevel == g_nav.level && now < g_hurtNext && now > g_hurtNext - 10.0f)
        return;
    bool newLevel = g_hurtLevel != g_nav.level;
    g_hurtLevel = g_nav.level;
    g_hurtNext = now + 5.0f;
    std::vector<HurtZone> zones;
    float minDmg = 1e9f, maxDmg = 0;
    int lasers = 0, movers = 0;
    // Besides trigger_hurt: beams that hurt (env_laser, env_beam between two points: the scenario maps' laser grids,
    // zs_contact 250 a touch, zs_defense 2500) while drawn, as small boxes along the beam; and moving or turning brushes
    // that hurt hard (crushers, saws: func_door 9999 in zs_behind...) where they are now. Bots walked into them (reports
    // 2026-11-27: "they keep walking on the lasers", "they go in saws"). A CBeam keeps its start in origin and its end in
    // angles (two point ends); EF_NODRAW: off.
    static const int EV_EFFECTS = 0x128, EV_AVELOCITY = 0x5c, EF_NODRAW = 128;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* cls = ClassName(e);
        unsigned char* v = Vars(e);
        float dmg = F<float>(v, EV_DMG);
        if (dmg <= 0)
            return;
        if (!strcmp(cls, "trigger_hurt"))
        {
            if (!F<int>(v, EV_SOLID))
                return;
            zones.push_back({ Vec((float*)(v + EV_ABSMIN)), Vec((float*)(v + EV_ABSMAX)) });
        }
        else if (!strcmp(cls, "env_laser") || !strcmp(cls, "env_beam"))
        {
            if ((F<int>(v, EV_EFFECTS) & EF_NODRAW) || dmg < 20.0f)
                return;
            V3 a = Vec((float*)(v + EV_ORIGIN)), b = Vec((float*)(v + EV_ANGLES));
            float len = Len(b - a);
            if (len < 16.0f || len > 4096.0f)
                return;
            int steps = (int)(len / 24.0f) + 1;
            for (int k = 0; k <= steps; k++)
            {
                V3 p = a + (b - a) * ((float)k / steps);
                zones.push_back({ p - V3{ 20, 20, 0 }, p + V3{ 20, 20, 0 } });
            }
            lasers++;
        }
        else if (dmg >= 100.0f && F<int>(v, EV_SOLID) &&
                 (!strncmp(cls, "func_door", 9) || !strcmp(cls, "func_rotating") || !strcmp(cls, "func_pendulum") ||
                  !strcmp(cls, "func_train") || !strcmp(cls, "func_tracktrain")))
        {
            // only while it moves or turns: a crusher at rest is a wall, a door that crushes when blocked is a door
            float sp = Len(Vec((float*)(v + EV_VELOCITY))) + Len(Vec((float*)(v + EV_AVELOCITY)));
            if (sp < 1.0f)
                return;
            zones.push_back({ Vec((float*)(v + EV_ABSMIN)) - V3{ 24, 24, 0 }, Vec((float*)(v + EV_ABSMAX)) + V3{ 24, 24, 0 } });
            movers++;
        }
        else
            return;
        minDmg = min(minDmg, dmg);
        maxDmg = max(maxDmg, dmg);
    });
    // (moving ones change place: the areas are worked out again whenever any zone did)
    bool same = zones.size() == g_hurtZones.size();
    for (size_t i = 0; same && i < zones.size(); i++)
        same = Len(zones[i].lo - g_hurtZones[i].lo) < 1.0f && Len(zones[i].hi - g_hurtZones[i].hi) < 1.0f;
    bool changed = !same || newLevel || g_navHurt.size() != g_nav.areas.size();
    g_hurtZones.swap(zones);
    if (!changed)
        return;
    g_navHurt.assign(g_nav.areas.size(), 0);
    int inside = 0;
    for (size_t i = 0; i < g_nav.areas.size(); i++)
    {
        const NavArea& a = g_nav.areas[i];
        if (InHurtZone(a.center, 0.0f))
            g_navHurt[i] = 1, inside++;
    }
    static int reports = 0, reportLevel = -1;
    if (reportLevel != g_nav.level)
        reportLevel = g_nav.level, reports = 0;
    if (reports++ < 20)
        Log("hurt zones (%s): %d boxes (%d beams, %d moving brushes; damage %.0f..%.0f), %d of %d nav areas left out of paths",
            g_nav.map.c_str(), (int)g_hurtZones.size(), lasers, movers, g_hurtZones.empty() ? 0.0f : minDmg, maxDmg, inside,
            (int)g_nav.areas.size());
}

static bool NavAreaHurts(int i) { return i >= 0 && i < (int)g_navHurt.size() && g_navHurt[i]; }

// walking toward yaw from pos (feet-ish origin): would the bot drop more than maxDrop or into a hurt zone within
// `ahead` units? The ground is probed at a few points along the way.
static bool DropAhead(void* bot, V3 pos, float yaw, float ahead, float maxDrop)
{
    TraceResult tr;
    V3 dir{ cosf(yaw / RAD), sinf(yaw / RAD), 0 };
    V3 feet = pos - V3{ 0, 0, 36 };
    for (float d = 24.0f; d <= ahead; d += 24.0f)
    {
        V3 at = pos + dir * d;
        if (!Trace(pos, at, bot, 1, tr))
            return false; // a wall first: no drop that way
        V3 down = at - V3{ 0, 0, 36 + maxDrop };
        if (Trace(at, down, bot, 1, tr))
            return true; // no floor within maxDrop
        V3 floor{ tr.vecEndPos[0], tr.vecEndPos[1], tr.vecEndPos[2] };
        if (InHurtZone(floor + V3{ 0, 0, 8 }, 0.0f) || floor.z < feet.z - maxDrop)
            return true;
    }
    return false;
}

// ---- does a player's body fit? A line has no width: it passes a corner the 32 units of a player catch on, it doesn't see the clip
// brushes that only block players, and with "ignore monsters" it goes through every solid that isn't a brush (the engine skips all
// but SOLID_BSP). The hull is the crouching player's (32 x 32 x 36, the same width), raised to the upper half of a standing body, so
// steps and ramps pass under it and what's lower is for jumping (LedgeAhead). People and monsters move out of the way: they don't count.
static const int HULL_DUCK = 3;
static const float BODY_LIFT = 18.0f, BODY_LOOK = 32.0f, STAIRS_LIFT = 40.0f, STAIRS_RISE = 48.0f;

static void HullLine(V3 from, V3 to, int noMonsters, void* skip, TraceResult& tr)
{
    float a[3] = { from.x, from.y, from.z }, b[3] = { to.x, to.y, to.z };
    memset(&tr, 0, sizeof(tr));
    g_TraceHull(a, b, noMonsters, HULL_DUCK, skip, &tr);
}

// how far along from -> to (player origins) the body gets, 0..1
static float BodyFraction(void* bot, V3 from, V3 to, TraceResult& tr)
{
    if (!g_TraceHull)
    {
        Trace(from, to, bot, 1, tr);
        return tr.flFraction;
    }
    V3 a = from + V3{ 0, 0, BODY_LIFT }, b = to + V3{ 0, 0, BODY_LIFT };
    HullLine(a, b, 0, bot, tr);
    bool inside = tr.fStartSolid || tr.fAllSolid;
    if (inside || (tr.flFraction < 1.0f && tr.pHit && (F<int>(Vars(tr.pHit), EV_FLAGS) & (FL_CLIENT | FL_MONSTER))))
        HullLine(a, b, 1, bot, tr);
    if (tr.fStartSolid || tr.fAllSolid)
        Trace(from, to, bot, 1, tr); // squeezed under something: all a hull can say is "stuck"
    return tr.flFraction;
}

static bool BodyClear(void* bot, V3 from, V3 to)
{
    TraceResult tr;
    return BodyFraction(bot, from, to, tr) >= 0.99f;
}

static V3 YawDir(float yaw) { return { cosf(yaw / RAD), sinf(yaw / RAD), 0 }; }

// the body can step towards yaw. Stairs and ramps rise into a level hull (the front of the box is 48 units ahead at the end of the
// look): above them it's free and the floor there is within reach, unlike a wall.
static bool BodyStep(void* bot, V3 pos, float yaw, float look = BODY_LOOK)
{
    V3 to = pos + YawDir(yaw) * look;
    if (BodyClear(bot, pos, to))
        return true;
    V3 up{ 0, 0, STAIRS_LIFT }, lift{ 0, 0, BODY_LIFT };
    TraceResult tr;
    if (g_TraceHull)
    {
        HullLine(pos + up + lift, to + up + lift, 1, bot, tr);
        if (tr.fStartSolid || tr.fAllSolid)
            return false; // a low ceiling: no room above to tell
    }
    if (!BodyClear(bot, pos + up, to + up))
        return false;
    if (Trace(to + up, to - V3{ 0, 0, 36 }, bot, 0, tr))
        return true;
    return tr.vecEndPos[2] - (pos.z - 36.0f) <= STAIRS_RISE;
}

// room for the body towards yaw, in units
static float BodyRoom(void* bot, V3 pos, float yaw, float len)
{
    TraceResult tr;
    return BodyFraction(bot, pos, pos + YawDir(yaw) * len, tr) * len;
}

// ---- ladders. This game's .nav files have none (they end after the areas), so they come from the map's func_ladder brushes:
// the area at the foot (floor within reach of the brush's bottom, at most 64 units beside it) and the area at the top (floor
// near the brush's top) are linked both ways for the A* below. A path through one gets two waypoints: where to step on and,
// flagged with the ladder, where to step off; NavTraverse climbs it (looking up or down and walking into it).
struct NavLadder
{
    V3 lo, hi;
    int bottom = -1, top = -1;
    V3 bottomPoint{}, topPoint{};
};
static std::vector<NavLadder> g_ladders;

// ---- rides: what carries players across the meshes gen_nav.py builds (the .navtxt's "deck" and "ride" lines, LoadRides):
// a lift (func_door going up or down, func_plat) or a train (func_train along its path) has an area of its own at each stop,
// its deck, linked by walking to the floor beside it; a ride joins two decks, or a teleport's trigger to its destination. A
// path through one gets two waypoints: where to get on and, flagged NAV_RIDE_FLAG + ride, where it gets off; NavRideStep
// waits at the edge until the deck is there, steps on, stays on, and NextWaypoint neither repaths nor counts it stuck meanwhile.
struct NavDeck
{
    int area = -1;
    std::string cls;
    V3 size{}, at{};  // the mover's size; the middle of its floor at this stop
    float sink = 0;   // its floor below its top
};
struct NavRide
{
    int kind = 0;     // 0 lift, 1 train, 2 teleport
    int from = -1, to = -1, deckFrom = -1, deckTo = -1;
    V3 on{}, off{};
};
static std::vector<NavDeck> g_navDecks;
static std::vector<NavRide> g_navRides;
static const int NAV_RIDE_FLAG = 10000; // and NAV_LINK_FLAG: the links drawn with wp_link
static const int NAV_LINK_FLAG = 20000;
static const float RIDE_COST = 300.0f;
// the ride the bot is on or about to take: its waypoint is the way off (flagged), or the way on just before it; 0: none
static int RideFlagAhead(const std::vector<int>& flags, size_t index)
{
    if (index < flags.size() && flags[index] >= NAV_RIDE_FLAG && flags[index] < NAV_LINK_FLAG)
        return flags[index];
    if (index + 1 < flags.size() && flags[index] == 0 && flags[index + 1] >= NAV_RIDE_FLAG && flags[index + 1] < NAV_LINK_FLAG)
        return flags[index + 1];
    return 0;
}
// ---- links the player draws (wp_add / wp_link, nav_tools.inl; Bin\botassist_nav\<map>.links): points on the floor joined two
// by two, each way a path can take: walking straight, jumping, crouching, climbing or dropping from one to the other. A point
// belongs to the mesh's area under it, or to a small area of its own where the mesh has none (the top of a tractor), so a
// chain of points can lead where the mesh doesn't. A path through one gets two waypoints: its near point and, flagged
// NAV_LINK_FLAG + 2 x link (+ 1 the other way), its far one; NavTraverse goes straight at it the way the link says.
enum { LINK_WALK, LINK_JUMP, LINK_CROUCH, LINK_LADDER, LINK_DROP, LINK_KINDS };
struct NavLinkPoint
{
    V3 pos{};       // on the floor
    int area = -1;  // BuildLinkAreas
};
struct NavLink
{
    int a = -1, b = -1, kind = LINK_WALK;
    bool oneway = false; // only from a to b (a drop always)
};
static std::vector<NavLinkPoint> g_linkPoints;
static std::vector<NavLink> g_links;

static int g_laddersLevel = -1;
static std::vector<int> g_pathFlagsOut; // FindPath: per waypoint, 1 + ladder index when it is reached by that ladder, else 0

// the point of area a closest to p, pulled `into` units towards the area's centre
static V3 AreaPointNear(const NavArea& a, V3 p, float into)
{
    V3 q{ p.x < a.lo[0] ? a.lo[0] : p.x > a.hi[0] ? a.hi[0] : p.x, p.y < a.lo[1] ? a.lo[1] : p.y > a.hi[1] ? a.hi[1] : p.y, 0 };
    V3 c = a.center - q;
    c.z = 0;
    float l = Len2D(c);
    if (l > 1.0f)
        q = q + c * ((into < l ? into : l) / l);
    q.z = AreaZ(a, q.x, q.y);
    return q;
}

static float AreaGap2D(const NavArea& a, V3 lo, V3 hi)
{
    float dx = a.hi[0] < lo.x ? lo.x - a.hi[0] : a.lo[0] > hi.x ? a.lo[0] - hi.x : 0.0f;
    float dy = a.hi[1] < lo.y ? lo.y - a.hi[1] : a.lo[1] > hi.y ? a.lo[1] - hi.y : 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

// a small area of botassist's own on the platform a ladder leads to, when the mesh has none there (the watchtowers of Zombie
// Shelter: buildings of the match): floor beside the ladder's top on one of its four sides, with room to stand
static int LadderTopArea(const NavLadder& l, float bottomFloor)
{
    V3 mid{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, 0 };
    float hx = (l.hi.x - l.lo.x) * 0.5f, hy = (l.hi.y - l.lo.y) * 0.5f;
    const V3 sides[4] = { { hx + 28.0f, 0, 0 }, { -hx - 28.0f, 0, 0 }, { 0, hy + 28.0f, 0 }, { 0, -hy - 28.0f, 0 } };
    for (const V3& off : sides)
    {
        V3 at{ mid.x + off.x, mid.y + off.y, l.hi.z + 48.0f };
        TraceResult tr;
        if (Trace(at, at - V3{ 0, 0, 120 }, nullptr, 1, tr) || tr.fStartSolid || tr.fAllSolid)
            continue;
        float fz = tr.vecEndPos[2];
        if (fz < l.hi.z - 72.0f || fz > l.hi.z + 24.0f || fz < bottomFloor + 64.0f)
            continue;
        V3 feet{ at.x, at.y, fz + 2.0f };
        if (!Trace(feet, feet + V3{ 0, 0, 70 }, nullptr, 1, tr))
            continue; // no room for a body
        NavArea a;
        a.id = 0xF0000000u + (unsigned)g_nav.areas.size();
        a.lo[0] = at.x - 20.0f, a.lo[1] = at.y - 20.0f, a.lo[2] = fz;
        a.hi[0] = at.x + 20.0f, a.hi[1] = at.y + 20.0f, a.hi[2] = fz;
        a.neZ = a.swZ = fz;
        a.center = { at.x, at.y, fz };
        g_nav.areas.push_back(a);
        return (int)g_nav.areas.size() - 1;
    }
    return -1;
}

// each drawn point's area: the mesh's floor under it (within 24 u), else a small one of its own
static void BuildLinkAreas()
{
    int made = 0;
    for (auto& p : g_linkPoints)
    {
        p.area = -1;
        float best = 24.0f;
        for (size_t i = 0; i < g_nav.areas.size(); i++)
        {
            const NavArea& a = g_nav.areas[i];
            if (p.pos.x < a.lo[0] - 1 || p.pos.x > a.hi[0] + 1 || p.pos.y < a.lo[1] - 1 || p.pos.y > a.hi[1] + 1)
                continue;
            float dz = fabsf(p.pos.z - AreaZ(a, p.pos.x, p.pos.y));
            if (dz < best)
            {
                best = dz;
                p.area = (int)i;
            }
        }
        if (p.area >= 0)
            continue;
        NavArea a;
        a.id = 0xE0000000u + (unsigned)g_nav.areas.size();
        a.lo[0] = p.pos.x - 16.0f, a.lo[1] = p.pos.y - 16.0f, a.lo[2] = p.pos.z;
        a.hi[0] = p.pos.x + 16.0f, a.hi[1] = p.pos.y + 16.0f, a.hi[2] = p.pos.z;
        a.neZ = a.swZ = p.pos.z;
        a.center = p.pos;
        g_nav.areas.push_back(a);
        p.area = (int)g_nav.areas.size() - 1;
        made++;
    }
    static int reports = 0;
    if (!g_linkPoints.empty() && reports++ < 40)
        Log("links (%s): %d points, %d links, %d points off the mesh (an area of their own)", g_nav.map.c_str(),
            (int)g_linkPoints.size(), (int)g_links.size(), made);
}

// one entry per func_ladder of the map, in the map's order (FindPath's flags point into it), linked or not; a ladder whose top
// has no floor yet is looked at again every 20 s (built later)
static float g_laddersRetry = 0;

static void BuildLadders()
{
    float now = Now();
    bool retry = g_laddersLevel == g_nav.level && now >= g_laddersRetry && now < g_laddersRetry + 60.0f;
    if ((g_laddersLevel == g_nav.level && !retry) || !g_nav.ok)
        return;
    bool firstTime = g_laddersLevel != g_nav.level;
    g_laddersLevel = g_nav.level;
    g_laddersRetry = 0;
    if (g_nav.areas.size() > g_navBaseAreas)
        g_nav.areas.resize(g_navBaseAreas);
    g_ladders.clear();
    int seen = 0, made = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* cls = ClassName(e);
        if (!cls || strcmp(cls, "func_ladder"))
            return;
        seen++;
        unsigned char* v = Vars(e);
        g_ladders.push_back(NavLadder{});
        NavLadder& l = g_ladders.back();
        l.lo = Vec((float*)(v + EV_ABSMIN));
        l.hi = Vec((float*)(v + EV_ABSMAX));
        if (l.hi.z - l.lo.z < 48.0f)
            return;
        V3 mid{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, 0 };
        float bestB = 1e9f, bestT = 1e9f;
        for (size_t i = 0; i < g_nav.areas.size(); i++)
        {
            const NavArea& a = g_nav.areas[i];
            float gap = AreaGap2D(a, l.lo, l.hi);
            if (gap > 64.0f)
                continue;
            float floor = AreaPointNear(a, mid, 0.0f).z;
            // a scrap of floor linked to nothing (beside a pillar: Hitchhiking's two ladders behind the gate had one at each
            // foot, so no path went up them) only when there's no linked floor about as close
            float lone = a.adj[0].empty() && a.adj[1].empty() && a.adj[2].empty() && a.adj[3].empty() ? 48.0f : 0.0f;
            if (floor > l.lo.z - 40.0f && floor < l.lo.z + 40.0f && gap + fabsf(floor - l.lo.z) + lone < bestB)
            {
                bestB = gap + fabsf(floor - l.lo.z) + lone;
                l.bottom = (int)i;
            }
            if (floor > l.hi.z - 72.0f && floor < l.hi.z + 24.0f && gap + fabsf(floor - l.hi.z) + lone < bestT)
            {
                bestT = gap + fabsf(floor - l.hi.z) + lone;
                l.top = (int)i;
            }
        }
        if (l.bottom >= 0 && (l.top < 0 || l.top == l.bottom))
        {
            l.top = LadderTopArea(l, AreaPointNear(g_nav.areas[l.bottom], mid, 0.0f).z);
            made += l.top >= 0;
        }
        if (l.bottom < 0 || l.top < 0 || l.bottom == l.top)
        {
            l.bottom = l.top = -1;
            g_laddersRetry = now + 20.0f;
            return;
        }
        l.bottomPoint = AreaPointNear(g_nav.areas[l.bottom], mid, 0.0f);
        l.topPoint = AreaPointNear(g_nav.areas[l.top], mid, 24.0f);
        if (l.topPoint.z - l.bottomPoint.z < 40.0f)
            l.bottom = l.top = -1; // the two floors are about level: no climbing there
    });
    BuildLinkAreas();
    g_navGen++; // the areas after the mesh's were made again
    int linked = 0;
    for (auto& l : g_ladders)
        linked += l.bottom >= 0;
    static int reports = 0;
    if (seen && (firstTime || made) && reports++ < 40)
        Log("ladders (%s): %d of %d func_ladder linked into the navigation mesh (%d to a platform botassist added)", g_nav.map.c_str(),
            linked, seen, made);
}

// A* over areas; returns the waypoints from the bot's area to the goal. partial: when nothing connects to the goal, the way to the
// area closest to it (not reached: false without it)
static bool FindPathImpl(V3 from, V3 to, std::vector<V3>& out, bool partial);
static bool FindPath(V3 from, V3 to, std::vector<V3>& out, bool partial = false)
{
    double t0 = PerfNow();
    bool ok = FindPathImpl(from, to, out, partial);
    g_perfPathSec += PerfNow() - t0;
    g_perfPaths++;
    return ok;
}
static bool FindPathImpl(V3 from, V3 to, std::vector<V3>& out, bool partial)
{
    out.clear();
    g_pathFlagsOut.clear();
    int start = AreaAt(from), goal = AreaAt(to);
    if (start < 0 || goal < 0)
        return false;
    if (start == goal)
    {
        out.push_back(to);
        g_pathFlagsOut.push_back(0);
        return true;
    }
    BuildLadders();
    float pathNow = Now();
    NavBlockPrune(pathNow);
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
                if (closed[nb] || (NavAreaHurts(nb) && nb != goal))
                    continue;
                // dropping down: the height between the two floors at their shared edge
                V3 edge = Portal(cur, d, nb);
                float drop = AreaZ(g_nav.areas[cur], edge.x, edge.y) - edge.z;
                if (drop > HURT_DROP_MAX)
                    continue;
                float c = cost[cur] + Len(g_nav.areas[nb].center - g_nav.areas[cur].center) + NavWetCost(nb) +
                          (g_navRouteCost && nb < (int)g_navRouteCost->size() ? (*g_navRouteCost)[nb] : 0.0f) +
                          (drop > SAFE_DROP ? (drop - SAFE_DROP) * DROP_COST : 0.0f) + NavAvoidCost(nb, pathNow) + PoiAreaCost(nb) +
                          NavBlockCost(cur, nb, pathNow);
                if (c < cost[nb])
                {
                    cost[nb] = c;
                    parent[nb] = cur;
                    parentDir[nb] = d;
                    open.push({ c + Len(g_nav.areas[nb].center - to), nb });
                }
            }
        // a ride from this area (parentDir 1000 + ride)
        for (size_t ri = 0; ri < g_navRides.size(); ri++)
        {
            const NavRide& r = g_navRides[ri];
            if (r.from != cur || r.to < 0 || closed[r.to] || (NavAreaHurts(r.to) && r.to != goal))
                continue;
            float c = cost[cur] + Len(r.off - r.on) * 0.25f + RIDE_COST + NavAvoidCost(r.to, pathNow) + PoiAreaCost(r.to) +
                      NavBlockCost(cur, r.to, pathNow);
            if (c < cost[r.to])
            {
                cost[r.to] = c;
                parent[r.to] = cur;
                parentDir[r.to] = 1000 + (int)ri;
                open.push({ c + Len(g_nav.areas[r.to].center - to), r.to });
            }
        }
        // a drawn link from this area (parentDir 2000 + 2 x link, + 1 the other way)
        for (size_t li = 0; li < g_links.size(); li++)
        {
            const NavLink& k = g_links[li];
            for (int back = 0; back < 2; back++)
            {
                if (back && (k.oneway || k.kind == LINK_DROP))
                    break;
                const NavLinkPoint& p = g_linkPoints[back ? k.b : k.a];
                const NavLinkPoint& q = g_linkPoints[back ? k.a : k.b];
                int nb = q.area;
                if (p.area != cur || nb < 0 || nb == cur || closed[nb] || (NavAreaHurts(nb) && nb != goal))
                    continue;
                float c = cost[cur] + Len(q.pos - p.pos) + 16.0f + NavAvoidCost(nb, pathNow) + PoiAreaCost(nb) +
                          NavBlockCost(cur, nb, pathNow);
                if (c < cost[nb])
                {
                    cost[nb] = c;
                    parent[nb] = cur;
                    parentDir[nb] = 2000 + (int)li * 2 + back;
                    open.push({ c + Len(g_nav.areas[nb].center - to), nb });
                }
            }
        }
        // a ladder from this area (parentDir 4 + 2 x ladder, + 1 going down)
        for (size_t li = 0; li < g_ladders.size(); li++)
        {
            const NavLadder& l = g_ladders[li];
            if (l.bottom < 0)
                continue;
            int nb = l.bottom == cur ? l.top : l.top == cur ? l.bottom : -1;
            if (nb < 0 || closed[nb] || (NavAreaHurts(nb) && nb != goal))
                continue;
            float c = cost[cur] + Len(l.topPoint - l.bottomPoint) * 1.5f + 64.0f + NavAvoidCost(nb, pathNow) + PoiAreaCost(nb) +
                      NavBlockCost(cur, nb, pathNow);
            if (c < cost[nb])
            {
                cost[nb] = c;
                parent[nb] = cur;
                parentDir[nb] = 4 + (int)li * 2 + (nb == l.bottom ? 1 : 0);
                open.push({ c + Len(g_nav.areas[nb].center - to), nb });
            }
        }
    }
    bool reached = parent[goal] >= 0;
    if (!reached)
    {
        if (!partial)
            return false;
        int closest = -1;
        float closestDist = Len(g_nav.areas[start].center - to);
        for (size_t i = 0; i < n; i++)
            if (closed[i] && (int)i != start && Len(g_nav.areas[i].center - to) < closestDist)
            {
                closestDist = Len(g_nav.areas[i].center - to);
                closest = (int)i;
            }
        if (closest < 0)
            return false;
        goal = closest;
    }
    std::vector<int> chain;
    for (int a = goal; a != start && a >= 0; a = parent[a])
        chain.push_back(a);
    for (int i = (int)chain.size() - 1; i >= 0; i--)
    {
        int d = parentDir[chain[i]];
        if (d < 4)
        {
            out.push_back(Portal(parent[chain[i]], d, chain[i]));
            g_pathFlagsOut.push_back(0);
            continue;
        }
        if (d >= 2000)
        {
            const NavLink& k = g_links[(d - 2000) / 2];
            bool back = (d - 2000) % 2 == 1;
            out.push_back(g_linkPoints[back ? k.b : k.a].pos); // its near point
            g_pathFlagsOut.push_back(0);
            out.push_back(g_linkPoints[back ? k.a : k.b].pos); // its far point
            g_pathFlagsOut.push_back(NAV_LINK_FLAG + (d - 2000));
            continue;
        }
        if (d >= 1000)
        {
            const NavRide& r = g_navRides[d - 1000];
            out.push_back(r.on); // where to get on
            g_pathFlagsOut.push_back(0);
            out.push_back(r.off); // where it gets off
            g_pathFlagsOut.push_back(NAV_RIDE_FLAG + (d - 1000));
            continue;
        }
        const NavLadder& l = g_ladders[(d - 4) / 2];
        bool down = (d - 4) % 2 == 1;
        out.push_back(down ? l.topPoint : l.bottomPoint); // where to step on
        g_pathFlagsOut.push_back(0);
        out.push_back(down ? l.bottomPoint : l.topPoint); // where to step off: climbed to
        g_pathFlagsOut.push_back(1 + (d - 4) / 2);
    }
    out.push_back(reached ? to : g_nav.areas[goal].center);
    g_pathFlagsOut.push_back(0);
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
    float fireAt = -100;         // HoldFire: the last time the stock AI pressed attack
    float holdLog = -100;
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
    // getting around (NavProgress, WalkYaw)
    V3 navAnchor{};              // where it was when the clock started: leaving this circle is progress
    float navAnchorAt = 0, navLastCall = -100;
    int navStuck = 0;            // times in a row it stayed in the circle
    std::vector<V3> navCrumbs;   // places it came through: the way back is known to work
    std::vector<NavAvoid> navAvoid;
    V3 navBackTo{};
    float navBackUntil = 0;
    float navNextCheck = 0;      // shortcuts and crouching are looked at a few times a second
    float duckUntil = 0;         // crouching: a low area of the mesh
    std::vector<int> pathFlags;  // per waypoint of path: 1 + ladder index when reached by climbing it (FindPath)
    float gapJumpAt = -100;      // NavTraverse: the last jump over a gap or onto a ledge
    float rideUntil = 0;         // NavRideStep: waiting for or on a lift, train or teleport: no repath, not stuck
    bool climbing = false;       // NavTraverse: on its way up or down a ladder
    float ladderWaitSince = -1, ladderWaitLast = -100; // LadderQueueWait: waiting for its turn at a ladder since
    float ladderWaitUntil = 0;   // waiting: no repath, not stuck (NextWaypoint, NavProgress)
    int ladderSkip = -1;         // a ladder it gave up waiting for: taken without waiting till ladderSkipUntil
    float ladderSkipUntil = 0;
    float navHeldLog = -100;     // NavProgress: when it last logged that the game holds this bot still
    int onLink = 0;             // NavTraverse: the drawn link it is taking (its path flag), 0: none
    float steerSide = 1, steerUntil = 0; // keeps going around an obstacle by the same side
    float jumpAt = -100;
    float strafeSide = 1, nextStrafe = 0;
    float lastFire = -100, nextReload = 0;
    bool firedSinceReload = false;
    float fireTime = 0;          // seconds spent firing since the last ammo purchase (ammo estimate)
    float nextRefill = 0, nextKill = 0;
    int killTries = 0;
    int roomBot = -1;            // index into g_matchBots: the room AI player this bot plays (name, weapons...)
    bool ordered = false;        // a player gave this bot an order (other modes: botassist only moves it then)
    int appliedClass = 0;        // room character already given to this bot
    float charFixAt = -100;      // ApplyTeamCharacter: last put back (not more than every 2 s)
    unsigned char* ownProfile = nullptr; // this bot's copy of its BotProfile with its room skill (see ApplyRoomSkill)
    int ownSkill = -1;
    int charTeam = 0;            // team its character belongs to (a swap to the other team gets a new one)
    bool wasAliveLoadout = false;
    float nextLoadoutCheck = 0;  // room weapons: check what it holds now and then
    int loadoutBuys = 0;         // buys of the room's weapons in this life
    int loadoutPrimary = 0, loadoutSecondary = 0, loadoutMelee = 0; // what it buys in this life
    bool nonePrimary = false, noneSecondary = false; // None drawn from its filter for this life: it drops what it gets
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

// another bot's state, if botassist keeps one for it (none is made)
static BotState* FindBotState(void* edict)
{
    for (auto& b : g_bots)
        if (edict && b.edict == edict)
            return &b;
    return nullptr;
}

// the order two bots in each other's way go in (crowding.inl): the formation slot, then the edict
static long long CrowdKey(const BotState& st, void* bot)
{
    return ((long long)st.slot << 32) | (long long)(DWORD)bot;
}
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

static void* HsMissileFirst(void* bot, V3 eye, void* best);
static bool HsNoChase(void* threat, void* target, V3 eye);

static void* FocusTarget(void* bot, V3 eye, void* best); // modes/scenario_orders.inl: the players' marked target

static void ScanMonsters(void* bot, BotState& st, V3 eye)
{
    void* best = nullptr;
    void* nearest = nullptr;
    float bestDist = 1e30f, nearestDist = 1e30f;
    // the nearest one in sight: sight is checked nearest first and stops at the first seen. It was checked for each
    // monster nearer than the best so far, in the engine's order: with dozens of monsters, every bot ~7 times a second
    // (cfg.rescan), that was most of botassist's time in Zombie Scenario (up to 300 ms a second, frame drops on slower
    // PCs; reports 2026-11-27). Same pick as before.
    static std::vector<std::pair<float, void*>> found;
    found.clear();
    ForEntitiesNear(eye, HsScanRadius(), [&](void* e) {
        if (e == bot || !IsLivingMonster(e) || IsIgnored(ClassName(e)))
            return;
        g_lastMonsterSeen = Now();
        g_monsterLevel = g_nav.level;
        float d = Len(Center(e) - eye);
        if (HsTooFar(e, d))
            return;
        if (d < nearestDist)
        {
            nearestDist = d;
            nearest = e;
        }
        found.push_back({ d, e });
    });
    std::sort(found.begin(), found.end(), [](const std::pair<float, void*>& a, const std::pair<float, void*>& b) { return a.first < b.first; });
    for (auto& f : found)
        if (CanSee(bot, eye, f.second))
        {
            bestDist = f.first;
            best = f.second;
            break;
        }
    // stay on the current target until it dies or stays out of sight; switch only for an extreme threat:
    // a visible monster about to hit the bot while the current one is clearly farther away
    void* cur = st.target;
    if (cur && IsLivingMonster(cur) && best)
    {
        float curDist = Len(Center(cur) - eye);
        bool extreme = best && best != cur && bestDist < cfg.extremeThreat && curDist > bestDist * 1.8f;
        if (!extreme)
            best = cur;
    }
    else if (cur && IsLivingMonster(cur))
        best = cur;
    // objectives the players have to break (zbsbreak: the doors, gates and machines that open the way), the closest
    // visible one: when no monster is within OBJECTIVE_MONSTER_NEAR (it waited for no monster at all, which in Zombie
    // Scenario is almost never, so the way stayed shut; reports 2026-11-27). A monster that close comes first.
    static const float OBJECTIVE_MONSTER_NEAR = 600.0f;
    float monsterDist = best && IsLivingMonster(best) ? Len(Center(best) - eye) : 1e30f;
    if (nearestDist < monsterDist)
        monsterDist = nearestDist; // (one not in sight but this close is a threat too)
    if (monsterDist >= OBJECTIVE_MONSTER_NEAR && cfg.objectiveRange > 0)
    {
        float objDist = 1e30f;
        void* obj = nullptr;
        if (cur && IsObjective(cur) && CanSee(bot, eye, cur))
            obj = cur;
        else
        {
            found.clear(); // (nearest first, as above)
            ForEntitiesNear(eye, cfg.objectiveRange, [&](void* e) {
                if (IsObjective(e))
                    found.push_back({ Len(Center(e) - eye), e });
            });
            std::sort(found.begin(), found.end(), [](const std::pair<float, void*>& a, const std::pair<float, void*>& b) { return a.first < b.first; });
            for (auto& f : found)
                if (CanSee(bot, eye, f.second))
                {
                    objDist = f.first;
                    obj = f.second;
                    break;
                }
        }
        if (obj)
            best = obj;
    }
    best = HsMissileFirst(bot, eye, best);
    best = ShelterTarget(bot, eye, best); // Zombie Shelter Team Match: the other team
    best = FocusTarget(bot, eye, best);   // "shoot my target"
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
    // nothing to stand on (the leader is by a ledge): the bot would take the long way to the floor below
    for (float f : { 1.0f, 0.5f })
    {
        V3 s = o + (spot - o) * f;
        if (!Trace(s, s - V3{ 0, 0, 100 }, nullptr, 1, tr))
            return s;
    }
    return o;
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

// a waypoint where a body stands: the points of a path are on the floor, the last one is whatever the caller asked for
static V3 PathBody(const BotState& st, size_t i)
{
    V3 p = st.path[i];
    p.z += i + 1 == st.path.size() ? 18.0f : 36.0f;
    return p;
}

// floor all the way (no shortcut over a pit or through a hurt zone)
static bool FloorAlong(void* bot, V3 from, V3 to)
{
    TraceResult tr;
    V3 d = to - from;
    float len = Len2D(d);
    for (float s = 48.0f; s < len; s += 48.0f)
    {
        V3 at = from + d * (s / len);
        if (Trace(at, at - V3{ 0, 0, 100 }, bot, 1, tr))
            return false;
        if (InHurtZone(V3{ tr.vecEndPos[0], tr.vecEndPos[1], tr.vecEndPos[2] + 8.0f }, 0.0f) || PoiAvoidAt(V3{ tr.vecEndPos[0], tr.vecEndPos[1], tr.vecEndPos[2] }))
            return false;
    }
    return true;
}

static bool CoopUnstick(void* bot, BotState& st, V3 pos); // scenario_orders.inl
static bool HuntGoal(void* bot, BotState& st, V3 pos, V3& goal); // scenario_orders.inl
static void HsTargetBots(DWORD delta);                          // modes/human_scenario.inl
static bool CoopMode();

// Every mode that walks a path goes through here, so this is where a bot that isn't getting anywhere is noticed. Moving doesn't count
// as getting anywhere: it has to leave a circle in time (a bot rubbing a corner or bouncing off it moves all the time). Then the area it
// was going into costs a lot for a while, so the new path takes another way if there is one, and the second time it also walks back
// to where it came from before trying again.
static const float NAV_STUCK_TIME = 1.5f, NAV_STUCK_MOVE = 64.0f, NAV_CRUMB_STEP = 96.0f, NAV_BACK_TIME = 1.2f, NAV_BACK_MIN = 80.0f;
static const float NAV_ASIDE = 64.0f, NAV_ASIDE_TIME = 1.0f;

// a living player in the way to wp: the body checks (BodyFraction) look through people, so a bot pressed against
// another one read as "nothing in the way of its body" and turned away from an area that was free
static void* PlayerInTheWay(void* bot, V3 pos, V3 wp)
{
    V3 to{ wp.x, wp.y, pos.z };
    float d = Len2D(to - pos);
    if (d < 1.0f)
        return nullptr;
    V3 dir = (to - pos) * (1.0f / d);
    void* found = nullptr;
    if (g_TraceHull)
    {
        TraceResult tr;
        V3 lift{ 0, 0, BODY_LIFT };
        HullLine(pos + lift, pos + dir * (std::min)(d, NAV_ASIDE) + lift, 0, bot, tr);
        if (tr.pHit && tr.pHit != bot && (F<int>(Vars(tr.pHit), EV_FLAGS) & FL_CLIENT) && Alive(tr.pHit))
            found = tr.pHit;
    }
    if (!found)
    {
        // pressed against each other (the hull starts inside them): the nearest one on the side it's going
        float best = 48.0f;
        ForEntitiesNear(pos, 48.0f, [&](void* e) {
            if (e == bot || !(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT) || !Alive(e))
                return;
            V3 o = Origin(e) - pos;
            float od = Len2D(o);
            if (od < best && fabsf(o.z) < 72.0f && o.x * dir.x + o.y * dir.y > 0.0f)
            {
                best = od;
                found = e;
            }
        });
    }
    return found;
}

// steps out of the way for NAV_ASIDE_TIME: to a free side, else back (NextWaypoint walks to navBackTo meanwhile)
static void StepAside(void* bot, BotState& st, V3 pos, V3 wp, float now)
{
    V3 to{ wp.x, wp.y, pos.z };
    float d = Len2D(to - pos);
    V3 dir = d > 1.0f ? (to - pos) * (1.0f / d) : V3{ 1, 0, 0 };
    V3 perp{ -dir.y, dir.x, 0 };
    float first = st.unstickSide;
    V3 aside = pos - dir * NAV_ASIDE;
    for (float s : { first, -first })
        if (BodyClear(bot, pos, pos + perp * (s * NAV_ASIDE)))
        {
            aside = pos + perp * (s * NAV_ASIDE);
            st.unstickSide = s;
            break;
        }
    st.navBackTo = aside;
    st.navBackUntil = now + NAV_ASIDE_TIME;
}

// botassist.log's perf lines, for comparing builds: stuck reports in the last 30 s by what was in the way (the
// "stuck:" lines stop at 300 a map, these don't), and how many of them were bots the game held still
static int g_stuckCounts[4]; // a player, something, nothing; held still (of those)
// the same for ladders (the "takes a ladder" lines stop at 60 a map, the waits at 80): climbs started, waits for a
// turn, waits given up (LadderQueueWait)
static int g_ladderCounts[4]; // (and turns lost: LadderTurnLost)

static void NavProgress(void* bot, BotState& st, V3 pos, V3 goal, float now)
{
    // waiting for its turn at a ladder (LadderQueueWait; NavTraverse keeps it waiting), or held where it is by the game
    // (Soccer's kickoff: 9 bots counted "stuck" every 1.5 s for 15 s, turned off their way and walked back): not stuck
    bool held = HeldStill(bot);
    if (now < st.ladderWaitUntil || held)
    {
        st.navLastCall = now;
        st.navAnchor = pos;
        st.navAnchorAt = now;
        if (held && st.navStuck)
            st.navStuck = 0;
        static int heldReports = 0, heldLevel = -1;
        if (heldLevel != g_nav.level)
            heldLevel = g_nav.level, heldReports = 0;
        if (held && heldReports < 3 && now - st.navHeldLog > 30.0f)
        {
            st.navHeldLog = now;
            heldReports++;
            Log("stuck check: bot %d held still (max speed %.0f, flags 0x%x): not counted", st.slot,
                F<float>(Vars(bot), EV_MAXSPEED), F<int>(Vars(bot), EV_FLAGS));
        }
        return;
    }
    if (!st.navCrumbs.empty() && Len(pos - st.navCrumbs.back()) > 400.0f)
        st.navCrumbs.clear(); // respawned
    if (st.navCrumbs.empty() || Len2D(pos - st.navCrumbs.back()) > NAV_CRUMB_STEP)
    {
        st.navCrumbs.push_back(pos);
        if (st.navCrumbs.size() > 8)
            st.navCrumbs.erase(st.navCrumbs.begin());
    }
    bool paused = now - st.navLastCall > 0.5f || now < st.navLastCall; // wasn't walking
    st.navLastCall = now;
    bool moved = Len2D(pos - st.navAnchor) > NAV_STUCK_MOVE || fabsf(pos.z - st.navAnchor.z) > NAV_STUCK_MOVE; // (a ladder: up or down)
    if (paused || moved)
    {
        st.navAnchor = pos;
        st.navAnchorAt = now;
        if (moved)
            st.navStuck = 0;
        return;
    }
    if (now - st.navAnchorAt < NAV_STUCK_TIME)
        return;
    st.navAnchorAt = now;
    st.navStuck++;
    st.nextRepath = 0;
    if (st.navStuck >= 12 && CoopMode() && CoopUnstick(bot, st, pos))
        return; // ~20 s stuck in a co-op mode (a Zombie Shelter bot that spawned on a roof): put on the ground nearby
    V3 wp = st.pathIndex < st.path.size() ? st.path[st.pathIndex] : goal;
    // another player in the way (crowding.inl's WayDecision). Moving, it will be out of the way soon: the area ahead is
    // fine, so no other way and no walking back; of two bots the one with the higher key steps aside and the other keeps
    // going, for a person (or a bot botassist doesn't drive) it steps aside. Standing still (holding its holdout place,
    // waiting at a ladder), it won't move: it's gone around like a wall, the area ahead avoided and, the second time,
    // walking back. Moving: a driven bot that walked its path within 0.5 s and isn't waiting at a ladder, anyone else
    // going faster than 30 u/s.
    void* blocker = PlayerInTheWay(bot, pos, wp);
    int way = WAY_KEEP_GOING;
    if (blocker)
    {
        BotState* o = (F<int>(Vars(blocker), EV_FLAGS) & FL_FAKECLIENT) ? FindBotState(blocker) : nullptr;
        bool moving = o ? now >= o->navLastCall && now - o->navLastCall < 0.5f && now >= o->ladderWaitUntil
                        : Len2D(Vec((float*)(Vars(blocker) + EV_VELOCITY))) > 30.0f;
        way = WayDecision(o != nullptr, moving, CrowdKey(st, bot), o ? CrowdKey(*o, blocker) : 0);
        if (way == WAY_STEP_ASIDE)
            StepAside(bot, st, pos, wp, now);
    }
    bool aroundIt = !blocker || way == WAY_GO_AROUND; // (a wall, or someone standing there)
    int ahead = aroundIt ? AreaAt(wp) : -1, here = AreaAt(pos);
    // what its body runs into on its way (no player in the way): a wall or door the mesh links through marks that link for
    // every bot (NavBlockNote)
    TraceResult tr = {};
    float got = 1.0f;
    if (!blocker)
    {
        got = BodyFraction(bot, pos, V3{ wp.x, wp.y, pos.z }, tr);
        if (got < 0.99f && tr.pHit && ahead >= 0 && here >= 0 && ahead != here)
            NavBlockNote(here, ahead, tr.pHit, now);
    }
    g_stuckCounts[blocker ? 0 : got >= 0.99f || !tr.pHit ? 2 : 1]++;
    if (HeldStill(bot))
        g_stuckCounts[3]++; // (never: a held bot returns above; counted for the same line as build A)
    if (ahead >= 0 && ahead != here)
    {
        for (size_t i = st.navAvoid.size(); i-- > 0;)
            if (now >= st.navAvoid[i].until || st.navAvoid[i].until - now > NAV_AVOID_TIME || st.navAvoid[i].area == ahead)
                st.navAvoid.erase(st.navAvoid.begin() + i);
        if (st.navAvoid.size() >= 8)
            st.navAvoid.erase(st.navAvoid.begin());
        st.navAvoid.push_back({ ahead, now + NAV_AVOID_TIME });
    }
    if (st.navStuck >= 2 && aroundIt)
        for (size_t i = st.navCrumbs.size(); i-- > 0;)
            if (Len2D(st.navCrumbs[i] - pos) >= NAV_BACK_MIN)
            {
                st.navBackTo = st.navCrumbs[i];
                st.navBackUntil = now + NAV_BACK_TIME;
                break;
            }
    // botassist.log: where and against what, the next thing to look at when a bot stays stuck
    static int reports = 0, reportLevel = -1;
    if (reportLevel != g_nav.level)
        reportLevel = g_nav.level, reports = 0;
    if (blocker && reports++ < 300)
        Log("stuck: bot %d at (%.0f %.0f %.0f) going to (%.0f %.0f %.0f) by (%.0f %.0f %.0f), %d in a row: %s '%s' in the way, %s",
            st.slot, pos.x, pos.y, pos.z, goal.x, goal.y, goal.z, wp.x, wp.y, wp.z, st.navStuck,
            (F<int>(Vars(blocker), EV_FLAGS) & FL_FAKECLIENT) ? "bot" : "player", String(F<int>(Vars(blocker), EV_NETNAME)),
            way == WAY_GO_AROUND ? "standing still: goes around" : way == WAY_STEP_ASIDE ? "steps aside"
                                                                         : "keeps going (the other one steps aside)");
    else if (reports++ < 300)
    {
        const char* model = tr.pHit ? String(F<int>(Vars(tr.pHit), 0xc8)) : nullptr;
        // (max speed: HeldStill reads it; a bot the game holds some other way would show here, not stuck yet reported)
        Log("stuck: bot %d at (%.0f %.0f %.0f) going to (%.0f %.0f %.0f) by (%.0f %.0f %.0f), %d in a row: %s %s (max speed %.0f)", st.slot, pos.x, pos.y, pos.z, goal.x, goal.y, goal.z, wp.x, wp.y, wp.z,
            st.navStuck, got >= 0.99f || !tr.pHit ? "nothing in the way of its body" : ClassName(tr.pHit), got >= 0.99f || !model ? "" : model,
            F<float>(Vars(bot), EV_MAXSPEED));
    }
}

// keeps st.path towards goal, returns the point to walk to (or false when there)
// partial: with no way to the goal, as close as it can get. Without it a one-point path means "no way": Conquest and the human bots
// of the zombie modes read it like that to choose another goal, so only who follows a player asks for this.
static bool NextWaypoint(void* bot, BotState& st, V3 pos, V3 goal, float arrive, V3& out, bool partial = false)
{
    float now = Now();
    if (Len2D(goal - pos) < arrive && fabsf(goal.z - pos.z) < 64)
    {
        st.path.clear();
        return false;
    }
    bool riding = now < st.rideUntil && RideFlagAhead(st.pathFlags, st.pathIndex) != 0;
    if (g_nav.ok && !riding)
        NavProgress(bot, st, pos, goal, now);
    if (now < st.navBackUntil && st.navBackUntil - now <= NAV_BACK_TIME && Len2D(st.navBackTo - pos) > 32.0f)
    {
        out = st.navBackTo;
        return true;
    }
    if (!g_nav.ok)
    {
        out = goal; // no mesh: straight line
        return true;
    }
    bool ladderWait = now < st.ladderWaitUntil; // (waiting for its turn at a ladder: the path through it stays)
    if (!riding && (st.path.empty() || (now >= st.nextRepath && !ladderWait) || Len(goal - st.pathGoal) > 120.0f))
    {
        g_navAvoid = &st.navAvoid;
        if (!FindPath(pos, goal, st.path, partial))
        {
            st.path = { goal };
            g_pathFlagsOut.assign(1, 0);
        }
        st.pathFlags = g_pathFlagsOut;
        if (st.pathFlags.size() != st.path.size())
            st.pathFlags.assign(st.path.size(), 0);
        g_navAvoid = nullptr;
        st.pathIndex = 0;
        st.pathGoal = goal;
        st.nextRepath = now + 1.0f;
        st.navNextCheck = 0;
    }
    // close to this one: on to the next, unless that's round a corner the body wouldn't clear from here (or the end of a ladder
    // it hasn't climbed yet: right above or below it, the ladder still has to be climbed)
    while (st.pathIndex + 1 < st.path.size() && Len2D(st.path[st.pathIndex] - pos) < 48.0f)
    {
        if (st.pathIndex < st.pathFlags.size() && st.pathFlags[st.pathIndex] && fabsf(pos.z - 36.0f - st.path[st.pathIndex].z) > 24.0f)
            break;
        if (Len2D(st.path[st.pathIndex] - pos) >= 20.0f && !BodyClear(bot, pos, PathBody(st, st.pathIndex + 1)))
            break;
        st.pathIndex++;
    }
    if (now >= st.navNextCheck || now < st.navNextCheck - 1.0f)
    {
        st.navNextCheck = now + 0.1f;
        // a shortcut to a later waypoint the whole body can walk to in a straight line
        size_t last = min(st.path.size() - 1, st.pathIndex + 3);
        for (size_t k = st.pathIndex; k <= last; k++)
            if (k < st.pathFlags.size() && st.pathFlags[k])
            {
                last = k > st.pathIndex ? k - 1 : st.pathIndex; // never past a ladder
                break;
            }
        // (in a precise area of the mesh, none: it keeps to the waypoints)
        int hereArea = AreaAt(pos);
        if (hereArea >= 0 && (g_nav.areas[hereArea].attr & NAV_PRECISE))
            last = st.pathIndex;
        for (size_t i = last; i > st.pathIndex; i--)
        {
            V3 body = PathBody(st, i);
            if (fabsf(body.z - pos.z) < 40 && BodyClear(bot, pos, body) && FloorAlong(bot, pos, body))
            {
                st.pathIndex = i;
                break;
            }
        }
        // the mesh's low areas are for crouching
        // (and the one after this waypoint: a waypoint at the mouth of a low passage isn't passed while the bot stands, its
        // body doesn't fit the way to the next one, so the bot has to crouch before it). One higher up (a duct in a wall) is
        // jumped into standing and crouched in the air: crouching first, the jump wouldn't reach it.
        int bodyFlags = F<int>(Vars(bot), EV_FLAGS);
        float feetZ = pos.z - ((bodyFlags & 0x4000 /* FL_DUCKING */) ? 18.0f : 36.0f);
        bool grounded = (bodyFlags & 0x200 /* FL_ONGROUND */) != 0;
        auto low = [&](int area, float z) {
            return area >= 0 && (g_nav.areas[area].attr & NAV_CROUCH) && (!grounded || z <= feetZ + 20.0f);
        };
        V3 at = st.path[st.pathIndex];
        int here = AreaAt(pos), next = Len2D(at - pos) < 96.0f ? AreaAt(at) : -1;
        int after = st.pathIndex + 1 < st.path.size() && Len2D(st.path[st.pathIndex + 1] - pos) < 128.0f
                        ? AreaAt(st.path[st.pathIndex + 1]) : -1;
        if (low(here, feetZ) || low(next, at.z) || (after >= 0 && low(after, st.path[st.pathIndex + 1].z)))
            st.duckUntil = now + 0.3f;
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
    // the map's points of interest first: somebody chose them
    int spots = PoiSpotCount(), firstSpot = spots ? rand() % spots : 0;
    for (int tries = 0; tries < 60 + spots; tries++)
    {
        bool chosen = tries < spots;
        V3 c = chosen ? PoiSpotAt((firstSpot + tries) % spots) : g_nav.areas[rand() % g_nav.areas.size()].center;
        float d = Len2D(c - around);
        if (d < minR || d > maxR || fabsf(c.z - around.z) > 160.0f || InHazard(c) || PoiAvoidAt(c))
            continue;
        bool crowded = false;
        for (auto& o : g_bots)
            if (&o != &st && o.edict && o.hasRoam && Len2D(o.roamGoal - c) < 200.0f)
                crowded = true;
        if (crowded || (!chosen && OpenDirections(bot, c) < 6))
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
    if (height > TALL_TARGET && !HsFlying(target))
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
    return p + Vec((float*)(v + EV_VELOCITY)) * HsAimLead(target);
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
        if (Trace(knee, to, bot, 1, tr) && BodyStep(bot, pos, y))
            return y;
    }
    return yaw;
}

// ClearYaw for walking a nav path: a line at knee height also hits stairs and ramps going up (40 u ahead they have
// risen above it), which sent the bot sideways instead of up them. It looks with the whole body (BodyStep), and goes round
// an obstacle by the side with more room, staying on that side: choosing again each frame had it bouncing off the middle.
static float WalkYaw(void* bot, V3 pos, float yaw, float dist)
{
    float look = dist < BODY_LOOK ? dist : BODY_LOOK;
    if (BodyStep(bot, pos, yaw, look))
        return yaw;
    BotState& st = StateFor(bot);
    float now = Now(), side = st.steerSide;
    if (now < st.duckUntil && st.duckUntil - now < 1.0f)
        return yaw; // into a low area of the mesh: to a standing body that's a wall
    if (now >= st.steerUntil || st.steerUntil - now > 1.0f)
        side = BodyRoom(bot, pos, yaw + 60.0f, 160.0f) >= BodyRoom(bot, pos, yaw - 60.0f, 160.0f) ? 1.0f : -1.0f;
    static const float tries[] = { 30, 60, 90, 120 };
    for (float t : tries)
        for (float s : { side, -side })
            if (BodyStep(bot, pos, yaw + s * t, look))
            {
                st.steerSide = s;
                st.steerUntil = now + 0.6f;
                return yaw + s * t;
            }
    return yaw;
}

// something to jump onto right ahead: blocked just above a step's height (stairs pass), free at head height
static bool LedgeAhead(void* bot, V3 pos, float yaw)
{
    TraceResult tr;
    V3 dir{ cosf(yaw / RAD), sinf(yaw / RAD), 0 };
    V3 knee = pos - V3{ 0, 0, 14 }, head = pos + V3{ 0, 0, 20 };
    return !Trace(knee, knee + dir * 24.0f, bot, 1, tr) && Trace(head, head + dir * 32.0f, bot, 1, tr);
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
    bool gone = false;       // a person took its place in this match: kicked, and not added again (KickForJoiners)
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

static void WbSetMatch(int on); // weapon_balance.inl: the room's weapon balance for this match

// The room's arms restriction (CSO_WeaponRestrictType_<n>, /api/matchbots "weaponLimit"): the weapons botassist buys
// for bots keep to it. They ignored it (a Pistol Match with rifles, reports 2026-11-23): the game's buy menu keeps
// players to it, botassist's buy commands didn't. 0 none, 1 no AWP, 2 Classic, 3 Sniper Match, 4 Pistol Match, 5 Knife
// Match, 6 no flash bang, 7 AWP Match, 8 Grenade Match, 9 New Classic, 10 no AWP + no flash, 11 Classic (no flash),
// 12 New Classic (no flash), 13 Shotgun Battle. New Classic isn't known (left open); pistols and melee stay in the
// Sniper, AWP and Shotgun matches.
static volatile LONG g_roomWeaponLimit = 0;
static bool SniperRes(const std::string& res)
{
    static const char* const kSnipers[] = { "awp", "svd", "m95", "m24", "m40", "m82", "as50", "sl8", "sg550", "g3sg1", "scout",
                                            "xm2010", "trg42", "l115", "wa2000", "dragunov", "vsk94", "sv98", "bendita",
                                            "railcannon", "crossbow", "bow" };
    for (const char* s : kSnipers)
        if (!res.compare(0, strlen(s), s))
            return true;
    return false;
}
static bool RoomAllowsWeapon(int id)
{
    int limit = (int)g_roomWeaponLimit;
    if (id <= 0 || limit <= 0 || limit == 6 || limit == 9 || limit == 12)
        return true;
    auto m = cfg.weaponModel.find(id);
    if (m == cfg.weaponModel.end() || m->second.size() <= 13)
        return limit == 1 || limit == 10; // (not in the list: only the AWP bans can let it through)
    std::string res = m->second.substr(9, m->second.size() - 13); // models/p_<res>.mdl, lower case
    auto c = cfg.weaponCategory.find(m->second);
    int cat = c != cfg.weaponCategory.end() ? c->second : 0;
    bool awp = !res.compare(0, 3, "awp");
    switch (limit)
    {
    case 1:
    case 10: return !awp;
    case 2:
    case 11: return cat == 11 || id <= 24; // Counter-Strike's own weapons (Item.csv 1-24)
    case 3: return cat == 1 || cat == 11 || (cat == 4 && SniperRes(res));
    case 4: return cat == 1 || cat == 11;
    case 5:
    case 8: return cat == 11;
    case 7: return cat == 1 || cat == 11 || awp;
    case 13: return cat == 1 || cat == 2 || cat == 11;
    }
    return true;
}
// a random one of the list that the room allows, 0 none
static int AllowedFrom(const std::vector<int>& list)
{
    std::vector<int> ok;
    for (int x : list)
        if (RoomAllowsWeapon(x))
            ok.push_back(x);
    return ok.empty() ? 0 : ok[rand() % ok.size()];
}
#include "room_options.inl"

static DWORD WINAPI FetchMatchBots(LPVOID)
{
    std::string json = HttpGet(L"/api/matchbots");
    std::vector<MatchBot> bots;
    size_t i = json.find("\"bots\":[");
    if (i != std::string::npos)
        i += 8;
    while (i != std::string::npos)
    {
        // the list ends at its "]": the room's "options":{...} after it was read as one more bot, a nameless one
        // with a random look and weapons (in every match since the Extra Settings page)
        while (i < json.size() && (json[i] == ',' || json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t'))
            i++;
        if (i >= json.size() || json[i] != '{')
            break;
        size_t a = i, b = json.find('}', a);
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
        // a filter whose slot is only "None" ([-1]): a bot with a random weapon there gets none. With weapons too, None
        // stays in the list as one more draw: with 4 weapons and None, about 1 bot in 5 gets none (a new draw each life)
        auto none = [](int& slot, std::vector<int>& allow) {
            bool onlyNone = !allow.empty() && std::all_of(allow.begin(), allow.end(), [](int x) { return x == -1; });
            if (onlyNone && slot == 0)
                slot = -1;
            if (onlyNone)
                allow.clear();
        };
        none(m.primary, m.allowPrimary);
        none(m.secondary, m.allowSecondary);
        none(m.melee, m.allowMelee); // (melee None: only the default knife)
        bots.push_back(m);
        i = b + 1;
    }
    EnterCriticalSection(&g_matchLock);
    g_matchBots = bots;
    g_matchKnown = json.find("\"bots\":[") != std::string::npos;
    LeaveCriticalSection(&g_matchLock);
    WbSetMatch(JsonInt(json, "weaponBalance", 0));
    InterlockedExchange(&g_roomWeaponLimit, JsonInt(json, "weaponLimit", 0));
    if (g_roomWeaponLimit)
        Log("room arms restriction %d: bots buy only weapons it allows", (int)g_roomWeaponLimit);
    SetRoomOptions(json);
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
    // every bot's state goes: the last match's bots are gone, and their edicts may be too (2026-11-20: after a Gun
    // Deathmatch with 18 bots, Zombie Shelter's bots became bots 18-20 and every order read the old edicts: access
    // violations, and the bots followed no order and did no job)
    for (auto& b : g_bots)
        b = BotState();
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
static bool PtnIsPartner(void* bot); // Partner: see modes/partner.inl
static bool PtnCreatingPartner();

static MatchBot* RoomBotOf(void* bot, BotState& st)
{
    if (g_matchFetch != 2 || PtnIsPartner(bot))
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
            if (!g_matchBots[i].edict && !g_matchBots[i].fakePlayer && !g_matchBots[i].gone && g_matchBots[i].team == team)
                pick = (int)i;
        for (size_t i = 0; team && i < g_matchBots.size() && pick < 0; i++)
            if (!g_matchBots[i].edict && !g_matchBots[i].fakePlayer && !g_matchBots[i].gone)
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
    // ordered to follow / cover someone: that player, not just the nearest one
    if (st.ordered && (st.order == ORDER_FOLLOW || st.order == ORDER_ESCORT) && st.commander && IsHuman(st.commander) &&
        Alive(st.commander) && TeamOf(st.commander) == TeamOf(bot))
        leader = st.commander;
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
    else if ((!st.ordered || st.order == ORDER_INDEPENDENT) && ShelterGoal(bot, st, pos, goal, arrive, wantMove))
        ; // Zombie Shelter: the mode's own tasks (gathering, warehouse, repairs...)
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
        if (st.threat && !st.target && !HsNoChase(st.threat, st.target, eye))
        {
            goal = Origin(st.threat);
            wantMove = true;
        }
        else if (!st.target && !st.threat && HuntGoal(bot, st, pos, goal))
            wantMove = true; // nothing in sight: towards the nearest monster of the map (Human Scenario's are far away)
        else if ((!st.threat || HsNoChase(st.threat, st.target, eye)) && leader && Len(Origin(leader) - pos) > 900.0f)
        {
            goal = FormationSpot(leader, st.slot, cfg.followDistance); // nothing to hunt: rejoin
            wantMove = true;
        }
        break;
    }

    V3 waypoint;
    bool moving = wantMove && NextWaypoint(bot, st, pos, goal, arrive, waypoint, true);
    bool walkingPath = moving; // (strafing and backing off below aren't: they don't count as being stuck)
    float moveYaw = 0;
    if (moving)
    {
        moveYaw = YawOf(waypoint - pos); // (jumps up to a waypoint above, over gaps, ladders: NavTraverse)
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
        if (d < cfg.fleeDistance && !HsFlying(danger) && !ShelterStandGround(bot))
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
    if (moving && !(danger && !HsFlying(danger) && Len2D(Center(danger) - pos) < cfg.fleeDistance && !ShelterStandGround(bot)))
    {
        moveYaw = WalkYaw(bot, pos, moveYaw, 40.0f); // walk around obstacles
        // a low ledge or a crate ahead: over it, crouching in the air to clear it
        if (LedgeAhead(bot, pos, moveYaw) && (now - st.jumpAt > 0.7f || now < st.jumpAt) && !DropAhead(bot, pos, moveYaw, 120.0f, SAFE_DROP))
        {
            st.jumpAt = now;
            cmd.buttons |= IN_JUMP;
        }
    }
    if (now - st.jumpAt > 0.1f && now - st.jumpAt < 0.5f)
        cmd.buttons |= IN_DUCK;

    // ---- stuck: jump and sidestep, then repath (only on the way somewhere: a bot strafing side to side while it shoots
    // hardly moves, and it jumped all the time as if it were stuck)
    if (moving && walkingPath)
    {
        if (Len2D(pos - st.lastPos) > 24.0f || fabsf(pos.z - st.lastPos.z) > 24.0f || st.climbing)
        {
            st.lastPos = pos;
            st.lastProgress = now;
        }
        else if (now - st.lastProgress > 1.0f && now >= st.unstickUntil)
        {
            st.unstickUntil = now + 0.5f;
            st.unstickSide = BodyRoom(bot, pos, moveYaw + 70.0f, 96.0f) >= BodyRoom(bot, pos, moveYaw - 70.0f, 96.0f) ? 1.0f : -1.0f;
            st.lastProgress = now;
            st.nextRepath = 0;
            st.jumpAt = now;
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
    else if (ShelterLook(bot, st, eye, lookYaw, lookPitch))
        ; // Zombie Shelter: at the resource or building it hits
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
        float aimDist = Len(AimPoint(st.target) - eye);
        V3 shot = eye + Forward(st.pitch, st.yaw) * (aimDist + 96.0f);
        Trace(eye, shot, bot, 0, tr);
        onTarget = (tr.pHit && (tr.pHit == st.target ||
                                (IsLivingMonster(tr.pHit) && !IsIgnored(ClassName(tr.pHit))))) ||
                   HsClearShotAt(st.target, tr, aimDist + 96.0f, aimDist);
        st.shots++;
        st.hits += onTarget;
    }
    // long range: bursts with short pauses to let the recoil settle
    float targetDist = st.target ? Len(AimPoint(st.target) - eye) : 0;
    if (onTarget && targetDist > 600.0f && !HsIsMissile(st.target))
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
    bool meleeTooFar = ShelterHoldFire(bot, st); // Zombie Shelter: the axe doesn't reach from afar
    if (meleeTooFar)
        onTarget = false;
    if (onTarget)
        st.lastOnTarget = now;
    // spraying: an automatic weapon keeps firing a moment after the line leaves the target (it moves, the
    // next one is next to it), unless a wall or a teammate is right in front
    bool keepFiring = false;
    if (!onTarget && !meleeTooFar && hold && st.wasFiring && st.target && now - st.lastOnTarget < 0.35f)
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
    if (int swing = ShelterSwing(bot, st, st.yaw, st.pitch))
        cmd.buttons = (unsigned short)((cmd.buttons & ~(IN_ATTACK | IN_RELOAD)) | swing); // Zombie Shelter: the axe (heavy on barricades)

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
// The Zombie Hero Classic copies of weapons (item names ending in "zhc", e.g. janus3zhc) are drawn with the
// original's model: a bot holding models/p_janus3.mdl holds the room's janus3zhc. Taken as a wrong weapon, it was
// bought again up to 6 times each life (one weapon thrown on the ground each time) in 24-bot rooms (logs 2026-11-25).
static bool SameWeaponModel(const std::string& held, const std::string& want)
{
    auto base = [](const std::string& m) {
        std::string s = m;
        size_t dot = s.rfind(".mdl");
        if (dot != std::string::npos && dot >= 3 && !_strnicmp(s.c_str() + dot - 3, "zhc", 3))
            s.erase(dot - 3, 3);
        return s;
    };
    return !_stricmp(base(held).c_str(), base(want).c_str());
}

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

// The room's weapon only fits a free slot: the modes that hand out a weapon at spawn (an AWP in some rooms, e.g.
// Zombie 4: Darkness) kept it, and the bought one fell to the ground (the buy handler doesn't make room). Before
// buying, the bot throws away what its slot holds with the game's own DropPrimary / DropSecondary (0x113f1750 /
// 0x113f1790, cdecl player: DropPlayerItem 0x114b4e60 by the class name of that slot's item), whatever is in its
// hand: "slot1" + cl_drop missed whenever the switch hadn't happened yet. Its weaponbox can't be picked up (touch
// +0x50 cleared), or the bot and the ones around it grabbed it again, and goes away in a moment through its own think
// (+0x4c, CWeaponBox::Kill, which DropPlayerItem schedules for much later): Kill removes the weapon inside too. Freeing
// the box with FL_KILLME instead left that weapon behind and some (CLockOnGun) were destroyed twice: a crash.
static const DWORD MP_DROP_PRIMARY = 0x113f1750, MP_DROP_SECONDARY = 0x113f1790;
static bool Readable(const void* p, size_t n);

// CSNZ's player and item pointers are 8-byte handles {control block, pointer}, valid when the control block's +4 is
// set: the inventory has 6 slots 16 bytes apart from player +0xfb8 (1 primary, 2 pistol, 3 knife, 4 grenades), each
// item's next one at +0xd8, the item in hand at +0x1018; the player's reserve ammo per type at +0x1034.
static const int PLAYER_ITEMS = 0xfb8, PLAYER_ACTIVE_ITEM = 0x1018, ITEM_NEXT = 0xd8, PLAYER_AMMO = 0x1034;

static unsigned char* ItemHandle(unsigned char* h)
{
    unsigned char* c = *(unsigned char**)h;
    if (!c || !Readable(c + 4, 4) || !*(int*)(c + 4))
        return nullptr;
    unsigned char* p = *(unsigned char**)(h + 4);
    return p && Readable(p, 0x170) ? p : nullptr;
}

// the CSO match object ([0x121d3ec4]: +0x27c started, +0x27d over, +0x214 ...)
static const DWORD MP_CSO_RULES = 0x121d3ec4;

// CBasePlayer::PlayerDeathThink (0x114c3cb0, thiscall player): what the game runs each frame for a dead player
// (respawn when the rules allow it); the modes whose bots got stuck dead call it themselves
static const DWORD MP_PLAYER_DEATH_THINK = 0x114c3cb0;
static bool CallPlayerDeathThink(void* player)
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

// class name of the first item in an inventory slot (player +0xfb8 + 16 * slot: 8-byte handles), "" when empty
static bool Readable(const void* p, size_t n);
static const char* SlotClassRaw(void* bot, int slot)
{
    __try
    {
        unsigned char* pl = (unsigned char*)PrivateOf(bot);
        unsigned char* it = pl ? ItemHandle(pl + 0xfb8 + 16 * slot) : nullptr;
        unsigned char* pev = it ? *(unsigned char**)(it + 8) : nullptr;
        return pev && Readable(pev, 4) ? String(*(int*)pev) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}
static std::string SlotClass(void* bot, int slot)
{
    const char* c = SlotClassRaw(bot, slot);
    return c ? c : "";
}

static bool CallDropSlot(void* player, int slot)
{
    __try
    {
        typedef void(__cdecl * DropFn)(void* player);
        ((DropFn)((slot == 1 ? MP_DROP_PRIMARY : MP_DROP_SECONDARY) + g_mpDelta))(player);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// a drop that faulted inside mp.dll (0xC0000005) leaves the game's own state half-updated: from then on, for the rest
// of the match, the game's drop isn't called again and bots only get the room's weapons into empty slots (Zombie Hero
// Classic, 2026-11-26: 44 failed drops in one second from 25 bots re-buying, then the game crashed)
static int g_dropFailLevel = -1;

// true when the slot is free; false when it holds something that wasn't dropped: the drop faulted, or drops are off
// after a fault this match
static bool FreeSlot(void* bot, int slot)
{
    std::string had = SlotClass(bot, slot);
    void* pl = PrivateOf(bot);
    if (had.empty() || !pl)
        return true;
    if (g_dropFailLevel == g_nav.level)
    {
        static int reports = 0;
        if (reports++ < 30)
            Log("bot '%s' keeps its %s: drops are off after a fault", String(F<int>(Vars(bot), EV_NETNAME)), had.c_str());
        return false;
    }
    std::vector<void*> before;
    V3 me = Origin(bot);
    ForEntitiesNear(me, 300.0f, [&](void* e) {
        const char* c = ClassName(e);
        if (c && !strcmp(c, "weaponbox"))
            before.push_back(e);
    });
    float* va = (float*)(Vars(bot) + EV_VANGLE);
    float* an = (float*)(Vars(bot) + EV_ANGLES);
    float saved[6] = { va[0], va[1], va[2], an[0], an[1], an[2] };
    va[0] = an[0] = 0;
    va[1] = an[1] = AngleDiff(va[1] + 180.0f, 0.0f);
    bool ok = CallDropSlot(pl, slot);
    memcpy(va, saved, sizeof(float) * 3);
    memcpy(an, saved + 3, sizeof(float) * 3);
    ForEntitiesNear(me, 300.0f, [&](void* e) {
        const char* c = ClassName(e);
        unsigned char* p = (unsigned char*)PrivateOf(e);
        if (c && p && !strcmp(c, "weaponbox") && std::find(before.begin(), before.end(), e) == before.end())
        {
            *(void**)(p + 0x50) = nullptr; // CWeaponBox touch
            if (*(void**)(p + 0x4c))       // its think (Kill) runs in a moment
                F<float>(Vars(e), 0x114) = Now() + 0.5f;
            static int reports = 0;
            if (reports++ < 3)
                Log("thrown weaponbox: think 0x%08lx, removed in 0.5 s", *(DWORD*)(p + 0x4c) ? *(DWORD*)(p + 0x4c) - g_mpDelta : 0);
        }
    });
    std::string left = SlotClass(bot, slot);
    static int reports = 0;
    if (!left.empty() || reports++ < 30)
        Log("bot '%s' throws away its %s to buy the room's weapon%s", String(F<int>(Vars(bot), EV_NETNAME)), had.c_str(),
            !ok ? " - the drop FAILED" : left.empty() ? "" : (" - still carries " + left).c_str());
    if (!ok && g_dropFailLevel != g_nav.level)
    {
        g_dropFailLevel = g_nav.level;
        Log("a weapon drop faulted in the game: room loadouts are no longer bought over the bots' weapons this match");
    }
    return ok;
}

// What SameWeaponModel (above) doesn't cover: weapons with no model of their own beyond the "zhc" copies.
// scar556limited is drawn with p_scar.mdl and buffm4a1zhc with p_buffm4.mdl, so neither the models nor the models
// without "zhc" match, and those bots dropped and bought the room's weapon again every check, though they had it.
// These count as the room's primary too: that weapon in slot 1 (weapon_<res>), or a held model whose name starts the
// room's one (3 characters at least). When slot 1 holds the base weapon itself, though, it isn't the room's variant: a
// plain M4A1 (weapon_m4a1, p_m4a1.mdl) isn't the room's M4A1 Dragon (p_m4a1dragon.mdl). Case doesn't matter.
static bool RoomPrimaryMatches(std::string held, std::string want, std::string slot1Class)
{
    for (std::string* s : { &held, &want, &slot1Class })
        for (auto& ch : *s)
            ch = (char)tolower((unsigned char)ch);
    static const size_t pre = 9, post = 4; // "models/p_" ... ".mdl"
    if (want.size() <= pre + post || held.size() <= pre + post || want.compare(0, pre, "models/p_") ||
        held.compare(0, pre, "models/p_"))
        return false;
    std::string wantRes = want.substr(pre, want.size() - pre - post);
    std::string heldRes = held.substr(pre, held.size() - pre - post);
    if (slot1Class == "weapon_" + wantRes)
        return true;
    if (slot1Class == "weapon_" + heldRes)
        return false;
    return heldRes.size() >= 3 && wantRes.size() > heldRes.size() && !wantRes.compare(0, heldRes.size(), heldRes);
}

// the room's primary is what the bot holds: the same model or its "zhc" copy (SameWeaponModel), or one of the cases
// RoomPrimaryMatches adds
static bool HoldsRoomPrimary(void* bot, const std::string& held, const std::string& want)
{
    return SameWeaponModel(held, want) || RoomPrimaryMatches(held, want, SlotClass(bot, 1));
}

// a moment after spawning (after the bot's own CS purchases), buy a random weapon from the whole game
// Zombie Disaster Zone pays money once per round, and a player who dies keeps the weapon in hand but bots come back
// with nothing (not even the knife): they buy again on each new life, so they get at least ZSRiftBotMoney then
// (player +0xca8, the money 0x114b76d0 reads)
static void ZsrBotMoney(void* bot)
{
    unsigned char* pl = (unsigned char*)PrivateOf(bot);
    if (pl && GameMode() == 59 && cfg.zsrBotMoney > 0 && *(int*)(pl + 0xca8) < cfg.zsrBotMoney)
        *(int*)(pl + 0xca8) = cfg.zsrBotMoney;
}

static const int MODE_ZHE = 51; // Manhunt: see DriveZheBot
static const int MODE_ZCON = 56; // Zombie Conquest: see modes/zombie_conquest.inl
static const int MODE_ZWAR = 32; // Zombie War: see modes/zombie_war.inl
static const int MODE_GDM = 22; // Gun Deathmatch: see modes/gun_deathmatch.inl
static const int MODE_SS = 57; // Super Soldier: see modes/super_soldier.inl
static const int MODE_PTN = 50; // Partner: see modes/partner.inl
static const int MODE_BEAST = 27; // Beast: see modes/beast.inl
static bool BeastIsBeast(void* bot);
static const int MODE_ITEM = 19; // Item Battle: see modes/item_battle.inl
static bool ItemHasSpecial(void* bot);
static const int MODE_ALLSTAR = 40; // All Star: see modes/all_star.inl
static void InstallAllStarTables(DWORD delta);
static bool HeroMode(int m); // Zombie Hero and its versions: see modes/zombie_hero.inl
static bool HeroIsHero(void* bot);
static void InstallBeastBotClass(DWORD delta);

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
    // a zombie (player +0x2166, every zombie mode) carries no guns: nothing to buy
    unsigned char* me = (unsigned char*)PrivateOf(bot);
    if (me && me[0x2166])
    {
        st.buyAt = 0;
        st.checkBuyAt = 0;
        return;
    }
    if (HeroIsHero(bot))
    {
        // Zombie Hero: the hero keeps the mode's weapons (its health going up when picked looked like a new life, and
        // the room's loadout dropped the hero's minigun)
        st.buyAt = 0;
        st.checkBuyAt = 0;
        st.nextLoadoutCheck = 0;
        return;
    }
    if (GameMode() == MODE_ZHE)
        return; // Manhunt: everyone starts with bare hands, weapons come from the map's boxes (customwb)
    if (GameMode() == MODE_ZCON)
        return; // Zombie Conquest: only the mode's weapon sets can be bought (ZConBotThink)
    if (GameMode() == MODE_ZWAR)
        return; // Zombie War: humans play the weapon picked before the round (ZWarBuyPick)
    if (GameMode() == MODE_GDM || GameMode() == MODE_SS)
        return; // Gun Deathmatch: the rules give each level's weapon. Super Soldier: its bots buy by Expert
    if (PtnIsPartner(bot))
        return; // Partner: a partner's weapons go by its player's money (ZBBot_BT_BuyWeapon_Partner)
    if (BeastIsBeast(bot))
        return; // Beast: only claws
    if (ItemHasSpecial(bot))
        return; // Item Battle: a Power AT4CS or Ripper from a box stays
    if (GameMode() == MODE_ALLSTAR)
        return; // All Star: each character has its own weapons
    MatchBot* rb = RoomBotOf(bot, st);
    bool scenario = CoopMode();
    // room AI players buy their own loadout in every mode: chosen weapons, else random ones from their filter, else
    // (BuyAnyWeapon) any weapon of the game, not only Counter-Strike's
    bool chosen = rb && (rb->primary != 0 || rb->secondary != 0 || rb->melee != 0 || !rb->allowPrimary.empty() ||
                         !rb->allowSecondary.empty() || !rb->allowMelee.empty() ||
                         (cfg.buyAnyWeapon && !cfg.primaries.empty()));
    // A gun the room chose is bought only when botassist_weapons.txt lists it: the guns kept away from bots there
    // (gen_weapons.py CRASHING: Dual Uzi W crashes the game when fired in a zombie mode) count as not chosen, also in
    // rooms saved before. Every gun of the room's AI player editor is in the list; melee isn't checked (the editor
    // offers some the list leaves out: bare hands, crowbar...). Without the list, nothing is refused.
    // (and the room's arms restriction allows it: RoomAllowsWeapon)
    auto gunListed = [&](int id) {
        return id <= 0 || ((cfg.weaponModel.empty() || cfg.weaponModel.count(id) != 0) && RoomAllowsWeapon(id));
    };
    // a slot: the room's weapon, -1 for "None", else a random one from the bot's filter, else 0
    auto pick = [&](int fixed, const std::vector<int>& allow, bool gun) {
        if (fixed != 0 && (!gun || gunListed(fixed)))
            return fixed;
        if (fixed != 0)
        {
            static int reports = 0;
            if (reports++ < 20)
                Log("room weapon item_%d isn't in botassist_weapons.txt (kept away from bots): another one instead", fixed);
        }
        std::vector<int> ok;
        for (int x : allow)
            if (!gun || gunListed(x))
                ok.push_back(x);
        return ok.empty() ? 0 : ok[rand() % ok.size()];
    };
    // the same, falling back on any weapon of the slot (BuyAnyWeapon)
    auto pickAny = [&](int fixed, const std::vector<int>& allow, const std::vector<int>& any, bool gun) {
        int id = pick(fixed, allow, gun);
        return id != 0 || !cfg.buyAnyWeapon || any.empty() ? id : AllowedFrom(any);
    };
    // "None": the bot drops its weapon of that slot (the game gives a pistol, some modes a primary, and bots pick
    // up what others drop). CSNZ's drop command is "cl_drop" (mp.dll 0x113ee0f4: game rules vfunc 0x11c), it throws
    // the weapon in hand, so the bot is switched to that slot now and then.
    bool noPrimary = rb && (rb->primary < 0 || (rb->primary == 0 && st.nonePrimary));
    bool noSecondary = rb && (rb->secondary < 0 || (rb->secondary == 0 && st.noneSecondary));
    if (rb && (noPrimary || noSecondary) && now >= st.nextStrip && st.strips < 200)
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
        if ((noPrimary && c >= 2 && c <= 5) || (noSecondary && c == 1))
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
            if (noPrimary)
                probes[n++] = "slot1";
            if (noSecondary)
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
        bool ok = want == cfg.weaponModel.end() || HoldsRoomPrimary(bot, held, want->second);
        if (!ok && primaryInHand && st.loadoutBuys < 6 && g_dropFailLevel != g_nav.level)
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
                st.loadoutPrimary = pickAny(rb->primary, rb->allowPrimary, cfg.primaries, true);
                st.loadoutSecondary = pickAny(rb->secondary, rb->allowSecondary, cfg.pistols, true);
                st.loadoutMelee = pickAny(rb->melee, rb->allowMelee, cfg.melees, false);
                st.nonePrimary = st.loadoutPrimary < 0;
                st.noneSecondary = st.loadoutSecondary < 0;
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
            ZsrBotMoney(bot);
            // a slot whose weapon couldn't be dropped (the drop faulted, or drops are off after a fault) keeps it: the
            // room's weapon for that slot isn't bought, the others are. After slot 1's drop faults, FreeSlot(2) returns
            // before calling the game again
            bool buyPrimary = st.loadoutPrimary > 0 && FreeSlot(bot, 1);
            bool buySecondary = st.loadoutSecondary > 0 && FreeSlot(bot, 2);
            bool refused = (st.loadoutPrimary > 0 && !buyPrimary) || (st.loadoutSecondary > 0 && !buySecondary);
            if (refused)
            {
                st.loadoutBuys = 99; // no more checks or rebuys for this bot this life
                st.nextLoadoutCheck = 0;
            }
            if (!buyPrimary)
                st.checkBuyAt = 0;
            if (refused && !buyPrimary && !buySecondary && st.loadoutMelee <= 0)
            {
                if (pl)
                    pl[0x18b4] = wasInternal;
                return;
            }
            FakeCommand(bot, "client_buy_open");
            if (buyPrimary)
                BuyAs(bot, st.loadoutPrimary);
            if (buySecondary)
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
            Log("bot %d buys the room's weapons: %d %d %d", st.slot, buyPrimary ? st.loadoutPrimary : 0,
                buySecondary ? st.loadoutSecondary : 0, st.loadoutMelee);
            return;
        }
        int roomPrimary = rb && st.buyTries == 0 ? pick(rb->primary, rb->allowPrimary, true) : 0;
        if (rb && (rb->primary < 0 || (st.buyTries > 0 && st.nonePrimary)))
            roomPrimary = -1; // (None drawn for this life stays on the retries)
        else if (rb && !rb->allowPrimary.empty() && roomPrimary == 0)
        {
            std::vector<int> real; // retries stay within the filter's weapons
            for (int x : rb->allowPrimary)
                if (x > 0 && gunListed(x))
                    real.push_back(x);
            if (!real.empty())
                roomPrimary = real[rand() % real.size()];
        }
        if (rb && st.buyTries == 0)
            st.nonePrimary = roomPrimary < 0;
        st.boughtPrimary = roomPrimary > 0 ? roomPrimary : roomPrimary < 0 ? 0 : AllowedFrom(cfg.primaries);
        const char* model = String(F<int>(Vars(bot), EV_WEAPONMODEL));
        st.modelBeforeBuy = model ? model : "";
        st.buyTries++;
        if (st.buyTries == 1)
            ZsrBotMoney(bot);
        FakeCommand(bot, "client_buy_open");
        if (st.boughtPrimary > 0)
            BuyAs(bot, st.boughtPrimary);
        int roomSecondary = rb ? (st.buyTries > 1 && st.noneSecondary ? -1 : pick(rb->secondary, rb->allowSecondary, true)) : 0;
        if (rb && st.buyTries == 1)
            st.noneSecondary = roomSecondary < 0;
        if (roomSecondary == 0)
            roomSecondary = AllowedFrom(cfg.pistols);
        if (roomSecondary > 0)
            BuyAs(bot, roomSecondary);
        int roomMelee = rb ? pickAny(rb->melee, rb->allowMelee, cfg.melees, false) : 0;
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
        bool wrong = want != cfg.weaponModel.end() && !HoldsRoomPrimary(bot, held, want->second);
        bool failed = (held == st.modelBeforeBuy && (want == cfg.weaponModel.end() || wrong)) || (!scenario && wrong);
        static int reports = 0;
        if (reports++ < 40)
            Log("bot %d bought item %d, now holding %s%s", st.slot, st.boughtPrimary, held.c_str(),
                failed ? " (no change: trying another)" : "");
        if (failed && !scenario && GameMode() == 30 && st.boughtPrimary > 0 && GiveRoomPrimary(bot, st.boughtPrimary))
            st.nextLoadoutCheck = now + 1.0f;
        else if (failed && !scenario && st.loadoutBuys < 4 && g_dropFailLevel != g_nav.level)
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
// The bot manager (mp 0x1130789f) kicks the human team's bots past bot_max_human_team (4 by default): in Zombie
// Scenario only the last 4 of the room's AI players stayed (5 of 9, 6 of 10 kicked). Where everyone plays on one
// team the cap is raised to the room's AI players (RaiseHumanTeamCap) and given back on the next map.
static std::string g_savedMaxHumanTeam, g_raisedMaxHumanTeam, g_raisedMap;

static void SetCvar(const char* name, const char* value);

static void RaiseHumanTeamCap(int bots)
{
    const char* cur = g_CVarGetString("bot_max_human_team");
    if (cur && atoi(cur) >= bots)
        return;
    if (g_savedMaxHumanTeam.empty())
        g_savedMaxHumanTeam = cur && *cur ? cur : "0";
    char v[16];
    snprintf(v, sizeof(v), "%d", bots);
    SetCvar("bot_max_human_team", v);
    g_raisedMaxHumanTeam = v;
    g_raisedMap = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
}
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
static bool CoopOneTeam();

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
        // (raised for this map's bots already, before this check saw the new map: kept)
        if (!g_savedMaxHumanTeam.empty() && g_raisedMap != map)
        {
            const char* cur = g_CVarGetString("bot_max_human_team");
            if (cur && g_raisedMaxHumanTeam == cur)
                SetCvar("bot_max_human_team", g_savedMaxHumanTeam.c_str());
            g_savedMaxHumanTeam.clear();
            g_raisedMaxHumanTeam.clear();
        }
        g_savedMaxBotTeam.clear();
        g_savedBalanceBotTeam.clear();
        g_teamsMap = map;
        g_wrongTeamChecks = 0;
    }
    if (!cfg.sameTeam || !CoopOneTeam())
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

// a co-op mode played by one team: not Zombie Shelter's Team Match maps (*_pvp), where both teams have bots
static bool CoopOneTeam()
{
    if (!CoopMode())
        return false;
    if (GameMode() == 28)
    {
        std::string map = String(*(int*)((DWORD)*g_pGlobals + GV_MAPNAME));
        if (map.size() >= 4 && map.compare(map.size() - 4, 4, "_pvp") == 0)
            return false;
    }
    return true;
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

#include "safe_kick.inl" // a bot is hidden from monsters a second before it's kicked (issue #53)

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
        KickSoon(bot, name);
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

// ---------------------------------------------------------------- a person takes an AI player's place
// A person joining this match while the room is full takes an AI player's place (CSNZ_Server's JoinRoom): the server
// names it in GET /api/matchkicks (asked every 3 s, on a thread of its own); here, on the game's thread, the bot that
// plays it is kicked, and the room's list no longer counts it, so the game's bot manager doesn't add another.
static std::vector<std::string> g_kickNames;
static volatile LONG g_kickFetching = 0;
static float g_nextKickPoll = 0;

static DWORD WINAPI FetchKicks(LPVOID)
{
    std::string json = HttpGet(L"/api/matchkicks");
    std::vector<std::string> names;
    size_t i = json.find("\"kicks\":[");
    if (i != std::string::npos)
    {
        i += 9;
        size_t end = json.find(']', i);
        while (end != std::string::npos)
        {
            size_t a = json.find('"', i);
            if (a == std::string::npos || a > end)
                break;
            size_t b = json.find('"', a + 1);
            if (b == std::string::npos)
                break;
            names.push_back(json.substr(a + 1, b - a - 1));
            i = b + 1;
        }
    }
    if (!names.empty())
    {
        EnterCriticalSection(&g_matchLock);
        g_kickNames.insert(g_kickNames.end(), names.begin(), names.end());
        LeaveCriticalSection(&g_matchLock);
    }
    InterlockedExchange(&g_kickFetching, 0);
    return 0;
}

static void KickForJoiners()
{
    if (!g_ServerCommand || g_matchFetch != 2)
        return;
    float now = Now();
    if (now >= g_nextKickPoll || now < g_nextKickPoll - 10.0f)
    {
        g_nextKickPoll = now + 3.0f;
        if (InterlockedCompareExchange(&g_kickFetching, 1, 0) == 0)
        {
            HANDLE h = CreateThread(NULL, 0, FetchKicks, NULL, 0, NULL);
            if (h)
                CloseHandle(h);
            else
                InterlockedExchange(&g_kickFetching, 0);
        }
    }
    std::vector<std::pair<void*, std::string>> kick;
    EnterCriticalSection(&g_matchLock);
    for (auto& n : g_kickNames)
        for (auto& m : g_matchBots)
            if (!m.gone && !m.fakePlayer && m.name == n)
            {
                m.gone = true;
                const char* net = m.edict ? String(F<int>(Vars(m.edict), EV_NETNAME)) : nullptr;
                kick.push_back({ net && *net ? m.edict : nullptr, net && *net ? net : m.name });
                break;
            }
    g_kickNames.clear();
    LeaveCriticalSection(&g_matchLock);
    for (auto& k : kick)
    {
        if (k.second.empty() || strchr(k.second.c_str(), '"'))
            continue;
        // hidden from monsters first, kicked a second later (safe_kick.inl: issue #53)
        KickSoon(k.first, k.second.c_str());
        Log("a person takes AI player '%s''s place in this match: kicked in a second", k.second.c_str());
    }
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
    const std::string& voices = Voices();
    const char* want = voices == "off" || GameMode() == MODE_ZHE ? "off" : voices == "normal" ? "normal" : "radio";
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

// a buy command the stock AI sends: a Counter-Strike alias, or a CSNZ weapon by its resource name (models/p_<name>.mdl
// in botassist_weapons.txt: category 1 pistol -> slot 2, 2-5 -> slot 1)
static int StockBuySlot(const char* cmd)
{
    if (int slot = BuyAliasSlot(cmd))
        return slot;
    std::string model = "models/p_" + std::string(cmd) + ".mdl";
    for (auto& ch : model)
        ch = (char)tolower(ch);
    auto cat = cfg.weaponCategory.find(model);
    return cat == cfg.weaponCategory.end() ? 0 : cat->second == 1 ? 2 : cat->second >= 2 && cat->second <= 5 ? 1 : 0;
}

// zombie modes where every player starts human (CT) and the game turns some into zombies: the room's T / CT of an AI
// player only picks its character there (a terrorist's look), it joins the humans like everyone. (A bot asked for as
// T joined the zombies' side at once, and the game took it out at the round's start: half the room was gone.)
static bool HumansStartMode()
{
    static const int modes[] = { 8, 9, 14, 20, 24, 29, 35, 36, 43, 44, 45, 54, 58 };
    int m = GameMode();
    for (int x : modes)
        if (x == m)
            return true;
    return false;
}

// the character a bot joins as: its room AI player's (if of its team); a room AI player on Random gets one of its
// filter's characters of its team, or any character of its team when the filter has none; other bots get a random
// one in co-op modes, else 0. Returns the class number the game takes (InGameID).
static int BotCharacter(void* bot, int team)
{
    LoadNav();
    UpdateMatchBots();
    MatchBot* rb = RoomBotOf(bot, StateFor(bot));
    if (rb && HumansStartMode() && (rb->team == 1 || rb->team == 2))
        team = rb->team; // on the humans' side, with the look of the room's team
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
    // Manhunt: the stock AI's radio ("hold this position", "wait for my go"...) is dropped; botassist's bots say what
    // fits (ZheRadio, sent past this hook)
    if (GameMode() == MODE_ZHE && (!_strnicmp(args[0], "radio", 5) || !_stricmp(args[0], "menuselect")))
        return true;
    if (int slot = StockBuySlot(args[0]))
    {
        // a room AI player with weapons of its own (chosen, a filter, None, or random ones with BuyAnyWeapon) doesn't
        // buy the game's ones: the stock AI buys its AWP and Deagle (or a CSNZ weapon by name) right after
        // botassist's purchase, and the game's buy throws away what the slot held, the room's weapon
        // (Super Soldier: the game's AI buys by Expert. Gun Deathmatch: the rules give each level's weapon with the
        // internal-buy flag set, and only those stay)
        if (GameMode() == MODE_SS)
            return false;
        if (BeastIsBeast(bot) || ItemHasSpecial(bot))
            return true;
        if (GameMode() == MODE_GDM)
        {
            unsigned char* me = (unsigned char*)PrivateOf(bot);
            return !(me && Readable(me, 0x18b8) && me[0x18b4]);
        }
        MatchBot* rb = RoomBotOf(bot, StateFor(bot));
        bool own = rb && (slot == 1 ? rb->primary != 0 || !rb->allowPrimary.empty() ||
                                          (cfg.buyAnyWeapon && !cfg.primaries.empty())
                                    : rb->secondary != 0 || !rb->allowSecondary.empty() ||
                                          (cfg.buyAnyWeapon && !cfg.pistols.empty()));
        if (own)
        {
            static int reports = 0;
            if (reports++ < 40)
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
// (It used to leave a barricade alone for good once something else took health from it, taken for the game's own zombie
// damage: a stray bullet or a blast did that, and most barricades of a match were never broken by zombies again. And it
// only ran while there were bots: the frame hook runs it now.)
static const DWORD MP_BREAKABLE_TAKEDAMAGE = 0x1147b850;
typedef int(__fastcall* TakeDamageFn)(void* self, void* edx, void* inflictor, void* attacker, float damage, int bits,
                                      int extra);
struct BarricadeState
{
    float lastHealth = -1;
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

// The zombies' melee: Zombie Scenario's monsters (monster_entity) think through CZBSEliteMonsterImprov (vtable
// 0x117cf2a0, at the monster's +0x1294; its owner monster at +0x1d4c). Its attack (vfunc 0x1ac, 0x10a50190) swings at
// its target (+0x1d2c, an entity pointer) when it's within reach and in front: the attack animation (vfunc 0x1a4)
// and the game's melee hit (0x1076fde0). Its target is a player; one behind a barricade is out of reach, so the
// zombies only pushed against it. A zombie touching a barricade gets that barricade as its target now, so it swings
// at it (botassist's damage above still does the breaking). When the barricade goes (broken, removed), a zombie still
// aiming at it gets no target (ZbsTargetGuard, every frame, before the monsters think) and picks a player again.
static const DWORD MP_ZBS_IMPROV_VT = 0x117cf2a0;
static const int MONSTER_IMPROV = 0x1294, IMPROV_TARGET = 0x1d2c, IMPROV_OWNER = 0x1d4c;
struct ZbsAimed
{
    void* improv;
    void* monster; // edict
    void* barricade; // edict
    void* barricadePriv;
};
static std::vector<ZbsAimed> g_zbsAimed;

static unsigned char* ZbsImprov(void* monsterEdict)
{
    unsigned char* m = (unsigned char*)PrivateOf(monsterEdict);
    if (!m || !Readable(m + MONSTER_IMPROV, 4))
        return nullptr;
    unsigned char* imp = *(unsigned char**)(m + MONSTER_IMPROV);
    if (!imp || !Readable(imp, IMPROV_OWNER + 4) || *(DWORD*)imp != MP_ZBS_IMPROV_VT + g_mpDelta ||
        *(unsigned char**)(imp + IMPROV_OWNER) != m)
        return nullptr;
    return imp;
}

static bool EdictGone(void* e, void* priv)
{
    return !e || *(int*)e /* free */ || PrivateOf(e) != priv;
}

static void ZbsTargetGuard()
{
    static float last = 0;
    float now = Now();
    if (now + 1.0f < last)
        g_zbsAimed.clear(); // a new map: those entities are gone with the old one
    last = now;
    for (size_t i = 0; i < g_zbsAimed.size();)
    {
        ZbsAimed& a = g_zbsAimed[i];
        if (!Readable(a.improv, IMPROV_OWNER + 4) || !Readable(a.monster, 4) || !Readable(a.barricade, 4))
        {
            g_zbsAimed.erase(g_zbsAimed.begin() + i);
            continue;
        }
        bool monsterGone = EdictGone(a.monster, *(void**)((unsigned char*)a.improv + IMPROV_OWNER));
        bool barricadeGone = EdictGone(a.barricade, a.barricadePriv) || F<float>(Vars(a.barricade), EV_HEALTH) <= 0;
        if (!monsterGone && !barricadeGone)
        {
            i++;
            continue;
        }
        if (!monsterGone && *(void**)((unsigned char*)a.improv + IMPROV_TARGET) == a.barricadePriv)
            *(void**)((unsigned char*)a.improv + IMPROV_TARGET) = nullptr;
        g_zbsAimed.erase(g_zbsAimed.begin() + i);
    }
}

static void ZbsAimAtBarricade(void* monster, void* barricade)
{
    unsigned char* imp = ZbsImprov(monster);
    void* bp = PrivateOf(barricade);
    if (!imp || !bp)
        return;
    void* cur = *(void**)(imp + IMPROV_TARGET);
    if (cur == bp)
        return;
    *(void**)(imp + IMPROV_TARGET) = bp;
    bool known = false;
    for (auto& a : g_zbsAimed)
        if (a.improv == imp)
        {
            a.monster = monster;
            a.barricade = barricade;
            a.barricadePriv = bp;
            known = true;
        }
    if (!known)
        g_zbsAimed.push_back({ imp, monster, barricade, bp });
    static int reports = 0;
    if (reports++ < 20)
        Log("barricade: zombie %s attacks barricade '%s' (it was after %s)", ClassName(monster),
            Str(F<int>(Vars(barricade), 0x1fc)), cur ? "a player" : "nothing");
}

static void UpdateBarricades()
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
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
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
        // only what zombies may break (map keyvalue zbdmgratio > 0), scaled by that ratio
        float playerRatio = 0, monsterRatio = 0;
        if (!BreakRatios(b, &playerRatio, &monsterRatio) || monsterRatio <= 0.0f)
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
            const float t = 40.0f; // touching, give or take (a zombie's arms reach a little past its box)
            if (mlo.x > hi.x + t || mhi.x < lo.x - t || mlo.y > hi.y + t || mhi.y < lo.y - t || mlo.z > hi.z + t ||
                mhi.z < lo.z - t)
                return;
            zombies++;
            attacker = m;
            __try { ZbsAimAtBarricade(m, b); } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
    if ((!cfg.debug && !g_aimLines) || !Alive(bot))
        return;
    unsigned char* v = Vars(bot);
    V3 eye = Eye(bot);
    V3 va = Vec((float*)(v + EV_VANGLE)), punch = Vec((float*)(v + EV_PUNCHANGLE));
    if (g_aimLines && now >= st.nextBeam)
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
    if (!cfg.debug || now < st.nextDebug)
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
    if (PtnCreatingPartner())
    {
        Log("the game adds a player's partner (%s, team %d)", which, team);
        return team;
    }
    LoadNav();
    UpdateMatchBots();
    if (g_matchFetch != 2 || !g_matchKnown)
        return team; // server not reachable: the game decides
    MarkZgPlayers();
    // Zombie Conquest and Zombie War merge the lobby's teams: the room lists its AI players on the host's side, while the
    // game asks for bots on both sides to balance them (T are the zombies). The list only says how many: each bot joins
    // the side asked for (RoomBotOf takes any free AI player), the emptier one when the game doesn't say.
    // (Partner: PtnWatch asks for each side itself)
    if (GameMode() == MODE_ZCON || GameMode() == MODE_ZWAR || GameMode() == MODE_SS || GameMode() == MODE_PTN ||
        GameMode() == MODE_BEAST || GameMode() == MODE_ITEM)
    {
        int total = 0;
        EnterCriticalSection(&g_matchLock);
        for (auto& m : g_matchBots)
            if (!m.fakePlayer && !m.gone)
                total++;
        LeaveCriticalSection(&g_matchLock);
        if (g_matchAdded[1] + g_matchAdded[2] >= total)
            return -1;
        // (Super Soldier keeps asking for CT while the list lasts: there the sides are simply kept even)
        // (Beast and Item Battle too: their bot manager asks for each side, and the room's list has them all on one)
        bool even = GameMode() == MODE_SS || GameMode() == MODE_BEAST || GameMode() == MODE_ITEM;
        int t = !even && team == 0 ? 1 : !even && team == 1 ? 2 : g_matchAdded[1] <= g_matchAdded[2] ? 1 : 2;
        g_matchAdded[t]++;
        Log("the game adds a bot (%s): %s, %d T and %d CT of %d", which, t == 1 ? "T (zombies)" : "CT (humans)",
            g_matchAdded[1], g_matchAdded[2], total);
        return t == 1 ? 0 : 1;
    }
    int want[3] = { 0, 0, 0 };
    EnterCriticalSection(&g_matchLock);
    for (auto& m : g_matchBots)
        if (!m.fakePlayer && !m.gone)
            want[m.team == 1 ? 1 : 2]++;
    LeaveCriticalSection(&g_matchLock);
    // Manhunt starts when everyone in the room is in (humans on CT, the zombie is drawn from them); the game asks
    // for its bots as terrorists to balance the teams
    bool oneTeam = GameMode() == MODE_ZOMBIE_GIANT || GameMode() == MODE_ZHE || HumansStartMode();
    if (CoopOneTeam() || oneTeam)
    {
        want[2] += want[1]; // co-op, Zombie Giant (the giant comes from the CT players), Manhunt: everyone on CT
        want[1] = 0;
    }
    if (CoopOneTeam() && team == 0)
        team = 2; // Zombie Disaster Zone asks for its bots as terrorists: they join CT like the others
    if (oneTeam)
        team = 1; // Zombie Giant's soldier bots, Manhunt's players, the zombie modes' humans: the room's AI players
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
    if (CoopOneTeam() || oneTeam)
        RaiseHumanTeamCap(want[2]); // (before the bot is in: the bot manager would kick it on its next check)
    Log("the game adds a bot (%s): %s %d of %d", which, t == 1 ? "T" : "CT", g_matchAdded[t], want[t]);
    return t == 1 ? 0 : 1;
}

static int g_creatingBot = 0; // inside the game's AddBot

static int __fastcall HookedAddBot(void* self, void* edx, int team)
{
    team = RoomBotQuota(team, "new manager");
    if (team < 0)
        return 0;
    if (cfg.enabled && cfg.sameTeam && CoopOneTeam())
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
    if (cfg.enabled && cfg.sameTeam && CoopOneTeam())
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

// redirects a function's first len bytes (whole instructions, no relative jumps) to hook; returns a trampoline that
// runs them and goes on with the rest (the original)
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

#include "modes/zombie_giant.inl"

#include "modes/manhunt.inl"

// ---------------------------------------------------------------- table overrides
// The games' data tables (Zombie Scenario's maps/ZBS_*_<map>, resource/...) are CSV files encrypted with TEA (.cso).
// mp.dll asks for them through 0x1146cb10(table, name, encrypted); all of them end in hw.dll's .cso loader 0x027583e0
// (thiscall table, "<name>.cso", returns rows && columns): it reads the file, decrypts it (0x02758c80, the only other
// caller is unrelated) and hands the text to the CSV parser 0x02758ba0 (thiscall table, text, size). botassist
// detours that loader: a file botassist_tables/<name>.csv next to botassist.asi goes to the parser instead.
// Zombie Disaster Zone's waves come from there (tables/maps/ZBS_WAVE_zsrift_splinter.csv, gen_zsrift_waves.py).
static const DWORD HW_LOADCSO = 0x027583e0, HW_PARSETABLE = 0x02758ba0, HW_BASE = 0x1d00000;
typedef bool(__thiscall* LoadCsoFn)(void* table, const char* file);
typedef void(__thiscall* ParseTableFn)(void* table, const char* text, int size);
static LoadCsoFn g_LoadCsoTramp;

// botassist_tables\<name> next to botassist.asi, read whole
static bool ReadOverride(const char* name, std::string& text)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbotassist_tables\\%s", g_dir, name);
    for (char* c = path; *c; c++)
        if (*c == '/')
            *c = '\\';
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
        text.append(buf, n);
    fclose(f);
    return !text.empty();
}

static bool LoadTableOverride(void* table, const char* file)
{
    size_t len = file ? strlen(file) : 0;
    if (len < 5 || (_stricmp(file + len - 4, ".cso") && _stricmp(file + len - 4, ".csv")))
        return false;
    std::string raw, text;
    if (!ReadOverride((std::string(file, len - 4) + ".csv").c_str(), raw))
        return false;
    // the parser splits rows on "\r\n" only (the game's tables have them; a file with bare "\n" became one column of
    // 2535 cells and the rift spawned nothing)
    for (size_t i = 0; i < raw.size(); i++)
    {
        if (raw[i] == '\n' && (i == 0 || raw[i - 1] != '\r'))
            text += '\r';
        text += raw[i];
    }
    ((DWORD*)table)[2] = ((DWORD*)table)[3] = 0; // rows, columns: what the .cso loader clears first
    ((ParseTableFn)(g_hwLo + HW_PARSETABLE - HW_BASE))(table, text.c_str(), (int)text.size());
    Log("table %s: loaded from botassist_tables (%lu rows, %lu columns)", file, ((DWORD*)table)[2], ((DWORD*)table)[3]);
    return true;
}

static bool __fastcall HookedLoadCso(void* table, void*, const char* file)
{
    if (LoadTableOverride(table, file))
        return ((DWORD*)table)[2] && ((DWORD*)table)[3];
    return g_LoadCsoTramp(table, file);
}

// the same loader for plain .csv tables (All Star's AllStar_Status-Dedi.csv, which the game never shipped)
static const DWORD HW_LOADCSV = 0x027584f0;
static LoadCsoFn g_LoadCsvTramp;
static bool __fastcall HookedLoadCsv(void* table, void*, const char* file)
{
    if (LoadTableOverride(table, file))
        return ((DWORD*)table)[2] && ((DWORD*)table)[3];
    return g_LoadCsvTramp(table, file);
}

static void InstallTableOverrides()
{
    if (g_LoadCsoTramp)
        return; // hw.dll stays loaded across matches
    if (!g_hwLo)
        ModuleRange("hw.dll", g_hwLo, g_hwHi);
    unsigned char* csv = (unsigned char*)(g_hwLo + HW_LOADCSV - HW_BASE);
    static const unsigned char csvCode[] = { 0x55, 0x8b, 0xec, 0x53, 0x56, 0x8b, 0xf1 };
    if (g_hwLo && Readable(csv, sizeof(csvCode)) && !memcmp(csv, csvCode, sizeof(csvCode)))
        g_LoadCsvTramp = (LoadCsoFn)DetourCdecl(csv, sizeof(csvCode), (void*)HookedLoadCsv);
    Log("table overrides for .csv: %s", g_LoadCsvTramp ? "on" : "NOT hooked (unexpected code)");
    unsigned char* fn = (unsigned char*)(g_hwLo + HW_LOADCSO - HW_BASE);
    unsigned char* parse = (unsigned char*)(g_hwLo + HW_PARSETABLE - HW_BASE);
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x51, 0x53, 0x8b, 0xd9 };
    static const unsigned char parseCode[] = { 0x55, 0x8b, 0xec, 0x56, 0x57, 0x8b, 0x7d, 0x0c, 0x8b, 0xf1 };
    if (g_hwLo && Readable(fn, sizeof(code)) && !memcmp(fn, code, sizeof(code)) && Readable(parse, sizeof(parseCode)) &&
        !memcmp(parse, parseCode, sizeof(parseCode)))
        g_LoadCsoTramp = (LoadCsoFn)DetourCdecl(fn, sizeof(code), (void*)HookedLoadCso);
    Log("table overrides: %s", g_LoadCsoTramp ? "botassist_tables\\<table>.csv replaces the game's table" : "NOT hooked (unexpected code)");
}

// Plain data files the game reads whole through SharedDataMgr (hw.dll vtable 0x2c05d3c) vfunc 0x4c8 (0x025b3c70,
// stdcall: std::string path, std::string out: open, size, read into out). Zombie Disaster Zone's random events come
// from maps/ZBS_ENC_<map>_dedi.json that way (mp.dll 0x107633d0), a file Nexon kept on its dedicated servers: without
// it the mode has no hazards, special zombies or modifiers. botassist_tables\<path> is read instead when it exists
// (tables/maps/ZBS_ENC_zsrift_splinter_dedi.json). The output string grows with the game's own append (0x01ea1d30,
// thiscall string, count, char), as the original does, so its memory stays the game's.
struct MsvcString
{
    union
    {
        char buf[16];
        char* ptr;
    };
    unsigned size, capacity;
    char* data() { return capacity > 15 ? ptr : buf; }
};
static const DWORD HW_SHAREDDATA_VT = 0x2c05d3c, HW_READPLAIN = 0x025b3c70, HW_STRING_APPEND = 0x01ea1d30;
typedef void(__stdcall* ReadPlainFn)(MsvcString* path, MsvcString* out);
typedef void(__thiscall* StringAppendFn)(MsvcString* s, unsigned count, char ch);
static ReadPlainFn g_ReadPlain;

static void __stdcall HookedReadPlain(MsvcString* path, MsvcString* out)
{
    std::string text;
    if (path && out && ReadOverride(path->data(), text))
    {
        if (out->size < text.size())
            ((StringAppendFn)(g_hwLo + HW_STRING_APPEND - HW_BASE))(out, (unsigned)text.size() - out->size, 0);
        else
            out->size = (unsigned)text.size();
        memcpy(out->data(), text.data(), text.size());
        out->data()[text.size()] = 0;
        Log("file %s: read from botassist_tables (%u bytes)", path->data(), (unsigned)text.size());
        return;
    }
    g_ReadPlain(path, out);
}

static void InstallFileOverrides()
{
    if (g_ReadPlain || !g_hwLo)
        return;
    void** slot = (void**)(g_hwLo + HW_SHAREDDATA_VT - HW_BASE + 0x4c8);
    static const unsigned char append[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x10, 0x53, 0x8b, 0x5d, 0x08 };
    unsigned char* app = (unsigned char*)(g_hwLo + HW_STRING_APPEND - HW_BASE);
    if (Readable(slot, 4) && *slot == (void*)(g_hwLo + HW_READPLAIN - HW_BASE) && Readable(app, sizeof(append)) &&
        !memcmp(app, append, sizeof(append)))
    {
        g_ReadPlain = (ReadPlainFn)*slot;
        PatchSlot(slot, (void*)HookedReadPlain);
    }
    Log("file overrides: %s", g_ReadPlain ? "botassist_tables\\<file> replaces the game's plain data files" : "NOT hooked (unexpected code)");
}

// The zombie class skills' values (every zombie mode): hw.dll's ZombieSkillTable (SharedDataMgr at 0x3f25310, +0x1778,
// vtable 0x2c21ac4: a std::map keyed by mode, evolution level and round) is filled by its vfunc 1 (0x26af410) from 32
// files resource/zombi/ZombieSkillProperty_Dedi/ZombieSkillProperty_<skill>.csv, but only on a dedicated server
// (byte 0x3f26248). Offline it stayed empty: "useitem zb_skill0" found no row and did nothing (no zombie could use its
// skill) and the skills that read their row every frame without checking crashed the game (Stamper, Banshee, Flying
// zombie: mp+0xa28d63, mp+0x94791a). botassist runs the loader past that check (0x26af420) once the file overrides
// are on; the files come from botassist_tables (gen_zombie_skills.py). A missing column throws a C++ exception.
static const DWORD HW_SHAREDDATA = 0x3f25310, HW_ZBSKILL_TABLE_VT = 0x2c21ac4, HW_ZBSKILL_LOAD = 0x26af420;
static unsigned g_zbSkillRows;

static void LoadZombieSkillTable()
{
    static bool tried = false;
    if (tried || !g_hwLo || !g_ReadPlain)
        return;
    unsigned char* mgr = nullptr;
    unsigned char* table = nullptr;
    unsigned char* load = (unsigned char*)(g_hwLo + HW_ZBSKILL_LOAD - HW_BASE);
    static const unsigned char code[] = { 0x53, 0x56, 0x8b, 0xd9, 0x8b, 0x73, 0x04 }; // push ebx; push esi; mov ebx,ecx..
    __try
    {
        mgr = *(unsigned char**)(g_hwLo + HW_SHAREDDATA - HW_BASE);
        if (mgr)
            table = mgr + 0x1778;
        if (!table || *(DWORD*)table != g_hwLo + HW_ZBSKILL_TABLE_VT - HW_BASE || memcmp(load, code, sizeof(code)))
        {
            Log("zombie skill table: %s", mgr ? "NOT loaded (unexpected code)" : "not created yet");
            tried = mgr != nullptr;
            return;
        }
        tried = true;
        g_zbSkillRows = *(unsigned*)(table + 8);
        if (!g_zbSkillRows)
        {
            ((void(__thiscall*)(void*))load)(table);
            g_zbSkillRows = *(unsigned*)(table + 8);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("zombie skill table: exception 0x%08lX while loading (a file with a missing column?)", GetExceptionCode());
        g_zbSkillRows = table ? *(unsigned*)(table + 8) : 0;
        return;
    }
    Log("zombie skill table: %u rows%s", g_zbSkillRows,
        g_zbSkillRows ? "" : " - zombie skills won't work (botassist_tables\\resource\\zombi\\ZombieSkillProperty_Dedi missing?)");
}

// How many random events a Zombie Disaster Zone round gets (0x10769630): rift level / a tweakable value (a vector of
// floats at 0x11e438f8, 32 from mp.dll's static init 0x1004f7f0, no callback), at most 10, the remainder a chance of
// one more. 32 was made for rift levels up to 100; ZSRiftEventDivisor replaces it.
static const DWORD MP_ZSRIFT_EVENT_DIVISOR = 0x11e438f8;
static void SetZsrEventDivisor(DWORD delta)
{
    float** vec = (float**)(MP_ZSRIFT_EVENT_DIVISOR + delta);
    bool ok = cfg.zsrEventDivisor > 0 && Readable(vec, 8) && vec[0] && vec[1] > vec[0] && Readable(vec[0], 4) &&
              *vec[0] == 32.0f;
    if (ok)
        *vec[0] = cfg.zsrEventDivisor;
    Log("ZSRift: events per round = rift level / %.1f%s", ok ? cfg.zsrEventDivisor : 32.0f, ok ? "" : " (game's value kept)");
}

// How much stronger Zombie Disaster Zone's monsters get with the rift level. mp.dll's monster setup (0x11360de0)
// multiplies a monster's attack, health, armour and speed by 1 + rate * (level - 1) ^ 1.85, with its kind's rates from
// resource/ZSRift/ZSRIFT_Monster (hw SharedDataMgr vfunc 0x8ac(type) -> float attack, health, armor, speed; type 0 the
// common zombies, 1 and 2 the special ones, 3 the bosses). The room's difficulty tooltip (GameUI 0x105b4c50) shows the
// same formula for types 0 and 3: +325% attack and +2321% health at level 20. botassist makes the growth linear (the
// 1.85, read only there: mp 0x11945470, GameUI 0x1096ff50, becomes ZSRiftLevelExponent) and hands out the ini's
// attack and health rates (percent per level); armour and speed keep the game's rates.
static const DWORD HW_SDM_MONSTER_RATES = 0x2590e20, MP_ZSRIFT_EXPONENT = 0x11945470, GAMEUI_ZSRIFT_EXPONENT = 0x1096ff50;
typedef float*(__thiscall* MonsterRatesFn)(void* sdm, unsigned type);
static MonsterRatesFn g_MonsterRates;

static float* __fastcall HookedMonsterRates(void* sdm, void*, unsigned type)
{
    float* r = g_MonsterRates(sdm, type);
    if (r && type <= 3)
    {
        float attack = type == 3 ? cfg.zsrBossAttack : cfg.zsrMonsterAttack;
        float health = type == 3 ? cfg.zsrBossHealth : cfg.zsrMonsterHealth;
        if (attack >= 0)
            r[0] = attack / 100;
        if (health >= 0)
            r[1] = health / 100;
    }
    return r;
}

static void InstallZsrMonsterRates()
{
    if (g_MonsterRates)
        return; // hw.dll stays loaded
    if (!g_hwLo)
        ModuleRange("hw.dll", g_hwLo, g_hwHi);
    void** slot = (void**)(g_hwLo + HW_SHAREDDATA_VT - HW_BASE + 0x8ac);
    if (g_hwLo && Readable(slot, 4) && *slot == (void*)(g_hwLo + HW_SDM_MONSTER_RATES - HW_BASE))
    {
        g_MonsterRates = (MonsterRatesFn)*slot;
        PatchSlot(slot, (void*)HookedMonsterRates);
    }
    Log("ZSRift: monster bonus per level: attack %.1f%%, health %.1f%%, boss attack %.1f%%, boss health %.1f%%%s",
        cfg.zsrMonsterAttack, cfg.zsrMonsterHealth, cfg.zsrBossAttack, cfg.zsrBossHealth,
        g_MonsterRates ? "" : " NOT set (unexpected code)");
}

static void SetZsrExponent(double* c, const char* where)
{
    bool ok = cfg.zsrExponent > 0 && Readable(c, 8) && (*c == 1.85 || *c == (double)cfg.zsrExponent);
    if (ok)
    {
        DWORD old;
        VirtualProtect(c, 8, PAGE_READWRITE, &old);
        *c = cfg.zsrExponent;
        VirtualProtect(c, 8, old, &old);
    }
    Log("ZSRift: %s: monster bonus = rate * (level - 1) ^ %.2f%s", where, ok ? cfg.zsrExponent : 1.85,
        ok ? "" : " (game's value kept)");
}

// The room panel redraws itself (GameUI 0x105b4c50, the difficulty tooltip with the monsters' bonuses too) after a
// room settings change only when the changed options include bit 59: 0x105bcd10 ands them with the mask at 0x10aff218
// (movups xmm1, [mask] at 0x105bcd2b). The bits are the server's room setting flags (low, lowMid, highMid, high), so a
// new rift level (lowMid ZSDIFFICULTY, bit 45) left the tooltip on the level the room was made with. botassist points
// the instruction at its own mask with bit 45 added.
static const DWORD GAMEUI_REDRAW_MASK_LOAD = 0x105bcd2b, GAMEUI_REDRAW_MASK = 0x10aff218;
static DWORD g_redrawMask[4];

// Create room dialog: "Random Map" sent the wrong mode in Item Battle and the other fun modes. Which mode the dialog creates comes from the
// selected map tile (GameUI 0x10571030, thiscall dialog, flag: tile +0x14) or, when that is 0, from the selected mode tab. Fun modes' maps
// carry their mode in the tile, but the random tile has none, so the tab's mode came out (Zombie Mutant from Item Battle).
// Now a tile without a mode takes it from the other tiles of the list when they have one.
static const DWORD GAMEUI_SELECTED_MODE = 0x10571030;
typedef int(__fastcall* SelectedModeFn)(unsigned char* dialog, void* edx, char flag);
static SelectedModeFn g_SelectedModeTramp;

static int __fastcall HookedSelectedMode(unsigned char* dialog, void* edx, char flag)
{
    __try
    {
        unsigned char* grid = *(unsigned char**)(dialog + 0x1b8);
        unsigned char* first = *(unsigned char**)(grid + 0x164);
        unsigned char* last = *(unsigned char**)(grid + 0x168);
        unsigned selected = *(unsigned*)(grid + 0x170);
        unsigned count = first && last > first ? (unsigned)(last - first) / 0x50 : 0;
        if (selected < count && *(int*)(first + selected * 0x50 + 0x14) == 0)
            for (unsigned i = 0; i < count; i++)
            {
                int mode = *(int*)(first + i * 0x50 + 0x14);
                if (mode > 0)
                    return mode;
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return g_SelectedModeTramp(dialog, edx, flag);
}

static void FixRandomMapMode(HMODULE gameUi)
{
    if (g_SelectedModeTramp)
        return;
    unsigned char* fn = (unsigned char*)(GAMEUI_SELECTED_MODE + ((DWORD)gameUi - 0x10000000));
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x6a, 0xff };
    if (!Readable(fn, sizeof(code)) || memcmp(fn, code, sizeof(code)))
    {
        Log("GameUI create room dialog has unexpected code: Random Map left as it is");
        return;
    }
    g_SelectedModeTramp = (SelectedModeFn)DetourCdecl(fn, sizeof(code), (void*)HookedSelectedMode);
    Log("GameUI create room dialog: Random Map keeps a fun mode %s", g_SelectedModeTramp ? "on" : "NOT hooked");
}

static void FixRiftTooltip(HMODULE gameUi)
{
    DWORD delta = (DWORD)gameUi - 0x10000000; // GameUI.dll's preferred base
    FixRandomMapMode(gameUi);
    SetZsrExponent((double*)(GAMEUI_ZSRIFT_EXPONENT + delta), "difficulty tooltip");
    unsigned char* p = (unsigned char*)(GAMEUI_REDRAW_MASK_LOAD + delta);
    DWORD* mask = (DWORD*)(GAMEUI_REDRAW_MASK + delta);
    if (!Readable(p, 7) || p[0] != 0x0f || p[1] != 0x10 || p[2] != 0x0d || *(DWORD**)(p + 3) != mask ||
        !Readable(mask, 16))
    {
        Log("GameUI room panel has unexpected code: difficulty tooltip left as it is");
        return;
    }
    memcpy(g_redrawMask, mask, sizeof(g_redrawMask));
    g_redrawMask[1] |= 1u << (59 - 32) | 1u << (45 - 32);
    DWORD old;
    VirtualProtect(p + 3, 4, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD**)(p + 3) = g_redrawMask;
    VirtualProtect(p + 3, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    Log("GameUI room panel: redrawn after a new rift level too (difficulty tooltip)");
}

// The classic crossbow (item 8977, CCrossBowCls) crashed the game on every shot: its bolt class
// ("crossbowcls_bolt", vfunc 0x344 0x10c43cc0) isn't in this mp.dll, so CBaseEntity::Create returns null and its
// attack (0x10c495a0, from CCrossbow's 0x1100de40) writes through it (mp+0xc49926). Two vtables have that slot:
// CCrossBowCls (0x1183991c, the "weapon_crossbowcls" entity) and CSimpleWpn<CCrossBowCls> (0x118391fc, what the
// item factory 0x10c43c20 makes when the weapon is bought by item id, as the bots' room loadouts do); both fire
// CCrossbow's "crossbow_bolt" (0x1100ead0) instead. Every mode.
static const DWORD MP_CROSSBOWCLS_VTABLES[] = { 0x1183991c, 0x118391fc };
static const DWORD MP_CROSSBOWCLS_BOLT = 0x10c43cc0, MP_CROSSBOW_BOLT = 0x1100ead0;
static void FixCrossbowCls(DWORD delta)
{
    int patched = 0;
    for (DWORD vt : MP_CROSSBOWCLS_VTABLES)
    {
        void** slot = (void**)(vt + delta + 0x344);
        if (Readable(slot, 4) && *slot == (void*)(MP_CROSSBOWCLS_BOLT + delta))
        {
            PatchSlot(slot, (void*)(MP_CROSSBOW_BOLT + delta));
            patched++;
        }
    }
    Log("classic crossbow: %s (%d of 2 vtables)", patched == 2 ? "fires crossbow_bolt (its own bolt class doesn't exist)"
                                                              : "NOT fully patched (unexpected code)", patched);
}

// Zombie class skills that read a per-round row (zombie modes such as Zombie 2: Mutation): FUN_10948aa0 (thiscall
// obj, player: classes 14 and 15) and FUN_10948f90 (thiscall obj, player: class 1) look up a row in a game table
// through the engine's data manager ([0x121d3cfc]() -> vfunc 0x1c8 -> vfunc 8 (table CSO match +0x4f0, the player's
// level 0x114b9ed0, round CSO +0x4cc + 1)) and use it without checking: offline the row can be missing (null), and
// picking such a class crashed the game (mp+0xa28d63, from the player's think). They are skipped while it's missing.
// The cause was the empty table itself (LoadZombieSkillTable fills it now); this stays as a fallback.
static const DWORD MP_ZB_SKILL_THINK_A = 0x10948aa0, MP_ZB_SKILL_THINK_B = 0x10948f90, MP_PLAYER_LEVEL = 0x114b9ed0,
                   MP_DATA_MANAGER = 0x121d3cfc;
typedef void(__fastcall* ZbSkillThinkFn)(void* obj, void* edx, void* player);
static ZbSkillThinkFn g_ZbSkillThinkA, g_ZbSkillThinkB;

// the row those skills read, null when the table has none (or on any fault)
static void* ZbSkillRow(void* player)
{
    __try
    {
        typedef void*(__cdecl * GetMgrFn)();
        typedef void*(__thiscall * SubFn)(void*);
        typedef void*(__thiscall * RowFn)(void*, int, int, int);
        typedef int(__thiscall * LevelFn)(void*);
        unsigned char* cso = *(unsigned char**)(MP_CSO_RULES + g_mpDelta);
        GetMgrFn getMgr = *(GetMgrFn*)(MP_DATA_MANAGER + g_mpDelta);
        if (!cso || !getMgr)
            return nullptr;
        void* mgr = getMgr();
        void* sub = mgr ? ((SubFn)(*(void***)mgr)[0x1c8 / 4])(mgr) : nullptr;
        if (!sub)
            return nullptr;
        int level = ((LevelFn)(MP_PLAYER_LEVEL + g_mpDelta))(player);
        return ((RowFn)(*(void***)sub)[8 / 4])(sub, *(int*)(cso + 0x4f0), level, *(int*)(cso + 0x4cc) + 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static bool ZbSkillRowMissing(void* player, int cls, const char* which)
{
    unsigned char* p = (unsigned char*)player;
    if (!p || !p[0x2166] || ZbSkillRow(player))
        return false;
    static int reports = 0;
    if (reports++ < 10)
        Log("zombie class %d skill (%s): the game table has no row for this level / round: skipped instead of crashing",
            cls, which);
    return true;
}

static void __fastcall HookedZbSkillThinkA(void* obj, void* edx, void* player)
{
    unsigned char* p = (unsigned char*)player;
    int cls = p ? (signed char)p[0x218e] : -1;
    if ((cls == 14 || cls == 15) && ZbSkillRowMissing(player, cls, "0x10948aa0"))
        return;
    g_ZbSkillThinkA(obj, edx, player);
}

static void __fastcall HookedZbSkillThinkB(void* obj, void* edx, void* player)
{
    unsigned char* p = (unsigned char*)player;
    int cls = p ? (signed char)p[0x218e] : -1;
    if (cls == 1 && ZbSkillRowMissing(player, cls, "0x10948f90"))
        return;
    g_ZbSkillThinkB(obj, edx, player);
}

static void GuardZbSkillThinks(DWORD delta)
{
    unsigned char* a = (unsigned char*)(MP_ZB_SKILL_THINK_A + delta);
    unsigned char* b = (unsigned char*)(MP_ZB_SKILL_THINK_B + delta);
    static const unsigned char codeA[] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xc0 };       // push ebp; mov ebp,esp; and esp,-40
    static const unsigned char codeB[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c };       // push ebp; mov ebp,esp; sub esp,0c
    if (Readable(a, sizeof(codeA)) && !memcmp(a, codeA, sizeof(codeA)))
        g_ZbSkillThinkA = (ZbSkillThinkFn)DetourCdecl(a, sizeof(codeA), (void*)HookedZbSkillThinkA);
    if (Readable(b, sizeof(codeB)) && !memcmp(b, codeB, sizeof(codeB)))
        g_ZbSkillThinkB = (ZbSkillThinkFn)DetourCdecl(b, sizeof(codeB), (void*)HookedZbSkillThinkB);
    Log("zombie class skills: %s", g_ZbSkillThinkA && g_ZbSkillThinkB ? "a missing table row no longer crashes"
                                                                       : "NOT guarded (unexpected code)");
}

static void InstallFrameHook(DWORD delta);
static void ScFrame();                   // modes/soccer.inl: the hands only in mode 16
static void InstallScHooks(DWORD delta); // modes/soccer.inl: bots count as their team's players in mode 16
static void InstallZtHooks(DWORD delta); // modes/zombie_tag.inl: what each player looks like, bumps into and carries in mode 37

#include "mp_fixes.inl"
#include "model_aliases.inl"
#include "knife_fixes.inl"
#include "crash_watch.inl"
#include "gun_fixes.inl"
#include "grenade_fixes.inl"
#include "scifi_weapons.inl"
#include "studio_guns.inl" // Studio's rifle, pistol and minigun outside Studio: exploding objects and blocks

static void HookBotManager(DWORD delta)
{
    FixDivineTitanEx(delta);
    FixArcanaII(delta);
    InstallCrashWatch();
    FixSciFiGuns();
    FixGunPrices();
    FixGrenades();
    FixStudioGuns();
    FixPrismSwordAnims(delta);
    GuardZbSkillThinks(delta);
    FixCrossbowCls(delta);
    HsTargetBots(delta);
    InstallTableOverrides();
    InstallFileOverrides();
    LoadZombieSkillTable();
    SetZsrEventDivisor(delta);
    InstallZsrMonsterRates();
    SetZsrExponent((double*)(MP_ZSRIFT_EXPONENT + delta), "mp.dll");
    DetourBotAddCommand(delta);
    HookZombieGiant(delta);
    InstallZgDebug(delta);
    InstallZgPlayers(delta);
    InstallFrameHook(delta);
    InstallZtHooks(delta);
    InstallScHooks(delta);
    InstallBeastBotClass(delta);
    InstallAllStarTables(delta);
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
static const DWORD MP_SV_CHEATS_COPY = 0x1220f318; // the game rules' sv_cheats, read when a player's HUD starts (0x11414c5e)
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

#include "nav_tools.inl"

// ---------------------------------------------------------------- console: bot_stop, bot_resume, bot_team, bot_scramble
// For debugging. "bot_stop [t|ct]" freezes the bots of a team (no team: both): no moving, jumping or shooting, applied
// last in HookedRunPlayerMove, so every mode's driving is overridden; "bot_resume [t|ct]" lets them go. It lasts until
// resumed (new maps too). "bot_team t|ct" moves every bot to that team, "bot_team" alone swaps each bot to the other one;
// "bot_scramble" deals the bots out at random so both teams have as many players as they can (humans count, they stay).
// A switch is the bot's own "jointeam" + "joinclass" (a character of the new team): a living bot dies and joins the
// other team, as a player would.
// mp.dll has a cvar "bot_stop" (the game's bots, all of them, 1 / 0) and the engine won't add a command named like a cvar,
// so botassist's "bot_stop" is put at the head of the engine's command list itself: commands are looked up before cvars.
static bool g_botStop[3]; // by team: [1] T, [2] CT

static bool BotStopped(void* bot)
{
    int team = TeamOf(bot);
    return team >= 1 && team <= 2 && g_botStop[team];
}

// the team argument: 1 T, 2 CT, 0 none (both), -1 not a team
static int ConsoleTeamArg(const char* cmd)
{
    const char* a = g_CmdArgv(1);
    if (!a || !*a)
        return 0;
    if (!_stricmp(a, "t") || !_stricmp(a, "tr") || !_stricmp(a, "terrorist") || !_stricmp(a, "terrorists") || !strcmp(a, "1"))
        return 1;
    if (!_stricmp(a, "ct") || !_stricmp(a, "cts") || !strcmp(a, "2"))
        return 2;
    Con("%s: the team is t or ct (nothing: both)\n", cmd);
    return -1;
}

static const char* ConsoleTeamName(int team)
{
    return team == 1 ? "T" : team == 2 ? "CT" : "both teams";
}

static void ConsoleBotStop(bool stop)
{
    const char* cmd = stop ? "bot_stop" : "bot_resume";
    int team = ConsoleTeamArg(cmd);
    if (team < 0)
        return;
    for (int t = 1; t <= 2; t++)
        if (!team || team == t)
            g_botStop[t] = stop;
    Con("%s: the bots of %s %s\n", cmd, ConsoleTeamName(team), stop ? "are stopped (bot_resume to let them go)" : "play again");
    Log("console: %s %s (T %s, CT %s)", cmd, ConsoleTeamName(team), g_botStop[1] ? "stopped" : "playing",
        g_botStop[2] ? "stopped" : "playing");
}
static void CmdBotStop() { ConsoleBotStop(true); }
static void CmdBotResume() { ConsoleBotStop(false); }

static std::vector<void*> ConsoleBots()
{
    std::vector<void*> bots;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS);
        int team = TeamOf(e);
        if ((flags & FL_CLIENT) && (flags & FL_FAKECLIENT) && (team == 1 || team == 2))
            bots.push_back(e);
    });
    return bots;
}

static void ConsoleMoveBot(void* bot, int team)
{
    char t[4], cls[16] = "1";
    snprintf(t, sizeof(t), "%d", team);
    if (!cfg.characters[team].empty())
        snprintf(cls, sizeof(cls), "%d", cfg.characters[team][rand() % cfg.characters[team].size()]);
    FakeCommand(bot, "jointeam", t);
    FakeCommand(bot, "joinclass", cls);
}

static void CmdBotTeam()
{
    int team = ConsoleTeamArg("bot_team");
    if (team < 0)
        return;
    int moved = 0;
    for (void* b : ConsoleBots())
    {
        int now = TeamOf(b), want = team ? team : 3 - now;
        if (now != want)
            ConsoleMoveBot(b, want), moved++;
    }
    Con("bot_team: %d bots %s\n", moved, team ? (team == 1 ? "moved to T" : "moved to CT") : "swapped to the other team");
    Log("console: bot_team %s: %d bots", team ? ConsoleTeamName(team) : "(swap)", moved);
}

static void CmdBotScramble()
{
    std::vector<void*> bots = ConsoleBots();
    int humans[3] = {};
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        int flags = F<int>(Vars(e), EV_FLAGS), team = TeamOf(e);
        if ((flags & FL_CLIENT) && !(flags & FL_FAKECLIENT) && (team == 1 || team == 2))
            humans[team]++;
    });
    for (size_t i = bots.size(); i > 1; i--)
        std::swap(bots[i - 1], bots[rand() % i]);
    int total = (int)bots.size() + humans[1] + humans[2];
    int wantT = total / 2 + ((total % 2) && (rand() % 2) ? 1 : 0); // an odd one out goes to a random side
    int botsT = wantT - humans[1];
    botsT = botsT < 0 ? 0 : botsT > (int)bots.size() ? (int)bots.size() : botsT;
    int moved = 0;
    for (size_t i = 0; i < bots.size(); i++)
    {
        int want = (int)i < botsT ? 1 : 2;
        if (TeamOf(bots[i]) != want)
            ConsoleMoveBot(bots[i], want), moved++;
    }
    Con("bot_scramble: %d bots on T, %d on CT (%d moved)\n", botsT, (int)bots.size() - botsT, moved);
    Log("console: bot_scramble: %d bots T, %d CT, %d moved (humans T %d, CT %d)", botsT, (int)bots.size() - botsT, moved,
        humans[1], humans[2]);
}

static void BotControlCommands()
{
    static CmdFunction stopNode = { nullptr, "bot_stop", CmdBotStop, 0 };
    static const struct { const char* name; void (*fn)(); } cmds[] = {
        { "bot_resume", CmdBotResume }, { "bot_team", CmdBotTeam }, { "bot_scramble", CmdBotScramble } };
    CmdFunction** head = (CmdFunction**)(g_hwLo + HW_CMD_LIST);
    bool haveStop = false, have[sizeof(cmds) / sizeof(cmds[0])] = {};
    int n = 0;
    for (CmdFunction* c = *head; c && n < 4096; c = c->next, n++)
    {
        if (!Readable(c, sizeof(*c)) || !Readable(c->name, 1))
            return;
        haveStop |= c == &stopNode;
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
            have[i] |= !_stricmp(c->name, cmds[i].name);
    }
    if (!haveStop)
    {
        stopNode.next = *head; // ahead of everything, the cvar bot_stop included
        *head = &stopNode;
    }
    auto add = (AddServerCommandFn)((void**)(MP_ENGFUNCS + g_mpDelta))[ENG_ADDSERVERCOMMAND];
    if (add)
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
            if (!have[i])
                add(cmds[i].name, cmds[i].fn);
    if (!haveStop)
        Log("console: bot_stop, bot_resume, bot_team and bot_scramble set up");
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
    NavAddCommands();
    BotControlCommands();
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
        // mp.dll keeps its own copy, taken when the game rules were made (0x11539743), before this frame: with it still
        // on, joining players were told the match used sv_cheats ("using sv_cheats" just for adding bots, report
        // 2026-11-25). It follows the cvar now.
        __try
        {
            float* cached = (float*)(MP_SV_CHEATS_COPY + g_mpDelta);
            if (*cached != 0.0f && g_CVarGetFloat && g_CVarGetFloat("sv_cheats") == 0.0f)
            {
                *cached = 0.0f;
                Log("console: mp.dll's copy of sv_cheats set to 0 too");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
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

#include "modes/zombie_disaster_zone.inl"

#include "modes/zombie4_darkness.inl"

#include "modes/zombie_humans.inl"
#include "match_teams.inl" // each player's side for the server's rounds won
#include "weaponbox_list.inl" // the game's list of weapons on the ground emptied at each map (issues #35, #37)
#include "tutorial.inl"    // Tutorial 1 and 2 finished: the tutorial window's bits
#include "modes/zombie_holdout.inl"

#include "modes/zombie2_mutation.inl"

#include "modes/zombie_hero.inl"
#include "weapon_balance.inl"

#include "modes/zombie_conquest.inl"

#include "modes/zombie_war.inl"

#include "modes/zombie_escape.inl"

#include "modes/gun_deathmatch.inl"

#include "modes/super_soldier.inl"

#include "modes/partner.inl"

#include "modes/beast.inl"

#include "modes/item_battle.inl"

#include "modes/all_star.inl"

#include "modes/human_scenario.inl"

#include "modes/zombie_tag.inl"

#include "modes/soccer.inl"

#include "modes/zombie_shelter.inl"

static void GiveOrder(Order order, void* by); // (below)
#include "modes/scenario_orders.inl"

// ---------------------------------------------------------------- the frame hook (every mode)
// The end of mp.dll's StartFrame (DLL function 0x113f8a80) runs botassist's per-frame work: the console, the command
// file, and each mode's watcher (each returns at once outside its mode).
static const DWORD MP_STARTFRAME = 0x113f8a80;
static void (*g_StartFrameTramp)();
static void ConsoleFrame();
static void CommandFile();

// every 30 s with bots playing: how many bot moves, and the time botassist took in them, in paths and in the frame hook
static void PerfReport()
{
    static double next = 0, since = 0;
    double now = PerfNow();
    if (!since)
        since = now, next = now + 30.0;
    if (now < next)
        return;
    double span = now - since;
    if (g_perfMoves > 0 && span > 1.0)
    {
        static int reports = 0;
        if (reports++ < 400)
            Log("perf: %.0f bot moves/s, botassist %.1f ms/s in them (%.0f us each), of which paths %.1f ms/s (%.0f/s); "
                "AreaAt %.0f/s; frame hook %.1f ms/s; %d areas, mode %d", g_perfMoves / span, g_perfMoveSec * 1000.0 / span,
                g_perfMoveSec * 1e6 / g_perfMoves, g_perfPathSec * 1000.0 / span, g_perfPaths / span, g_perfAreaAt / span,
                g_perfFrameSec * 1000.0 / span, (int)g_nav.areas.size(), GameMode());
        if (reports < 400)
        {
            std::string parts;
            char b[64];
            for (int i = 0; i < 6; i++)
            {
                snprintf(b, sizeof(b), "%s%s %.1f", i ? ", " : "", g_perfPartNames[i], g_perfPart[i] * 1000.0 / span);
                parts += b;
            }
            Log("perf: bot move hook by part (ms/s): %s", parts.c_str());
            Log("perf: stuck reports %d (a player in the way %d, something %d, nothing %d; %d of them held still)",
                g_stuckCounts[0] + g_stuckCounts[1] + g_stuckCounts[2], g_stuckCounts[0], g_stuckCounts[1], g_stuckCounts[2],
                g_stuckCounts[3]);
            Log("perf: ladders: %d climbs, %d waits for a turn, %d gave up waiting, %d turns lost", g_ladderCounts[0],
                g_ladderCounts[1], g_ladderCounts[2], g_ladderCounts[3]);
        }
    }
    for (double& d : g_perfPart)
        d = 0;
    g_perfMoves = g_perfPaths = g_perfAreaAt = 0;
    for (int& c : g_stuckCounts)
        c = 0;
    for (int& c : g_ladderCounts)
        c = 0;
    g_perfMoveSec = g_perfPathSec = g_perfFrameSec = 0;
    since = now;
    next = now + 30.0;
}

static void HookedStartFrame()
{
    g_StartFrameTramp();
    double perfStart = PerfNow();
    __try
    {
        WeaponBoxListFrame(); // first: before anything this frame hands out weapons
        ConsoleFrame();
        CommandFile();
        ZSRiftWatch();
        Z4Watch();
        ZB2Watch();
        ZConWatch();
        ZWarWatch();
        GdmWatch();
        SSWatch();
        PtnWatch();
        BeastWatch();
        ShelterWatch();
        ItemWatch();
        AsWatch();
        HsWatch();
        ZtFrame();
        ScFrame();
        NavToolsFrame();
        ZgFrame();
        WbFrame();
        KnifeFixFrame();
        GrenadeFixFrame();
        SfFrame();
        StudioGunsFrame();
        CrowdReport();
        MatchTeamsFrame();
        TutorialFrame();
        PendingKicksFrame();
        if (CoopMode())
        {
            UpdateBarricades(); // zombies against barricades, bots or not
            ZbsTargetGuard();
        }
        else
            g_zbsAimed.clear();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("exception 0x%08lX in the frame hook (ignored)", GetExceptionCode());
    }
    g_perfFrameSec += PerfNow() - perfStart;
    PerfReport();
}

static void InstallRadioFilter(DWORD delta); // below
static void InstallFrameHook(DWORD delta)
{
    if (g_StartFrameTramp)
        return;
    unsigned char* sf = (unsigned char*)(MP_STARTFRAME + delta);
    static const unsigned char sfCode[] = { 0x8b, 0x0d };
    if (!memcmp(sf, sfCode, 2))
        g_StartFrameTramp = (void (*)())DetourCdecl(sf, 6, (void*)HookedStartFrame);
    Log("frame hook: %s", g_StartFrameTramp ? "on" : "NOT hooked (unexpected code)");
    InstallRadioFilter(delta);
}

// ---------------------------------------------------------------- bots' radio: no "Get in position and wait for my go"
// CBasePlayer::Radio 0x114c7700 (thiscall: message id "%!MRAD_...", text "#...", pitch, show icon; ret 0x10). The game's bots
// send "%!MRAD_GETINPOS" (0x113f40c7, 0x113f67ff) again and again in every mode: a bot's is dropped, a player's goes through.
typedef void(__fastcall* RadioFn)(void* player, void* edx, const char* id, const char* text, int pitch, int icon);
static RadioFn g_RadioTramp;

static void __fastcall HookedRadio(void* player, void* edx, const char* id, const char* text, int pitch, int icon)
{
    __try
    {
        if (cfg.enabled && id && !_stricmp(id, "%!MRAD_GETINPOS"))
        {
            unsigned char* pev = Readable((unsigned char*)player + 8, 4) ? *(unsigned char**)((unsigned char*)player + 8) : nullptr;
            if (pev && Readable(pev + EV_FLAGS, 4) && (*(int*)(pev + EV_FLAGS) & FL_FAKECLIENT))
            {
                static int reports = 0;
                if (reports++ < 3)
                    Log("radio: a bot's \"Get in position and wait for my go\" dropped");
                return;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_RadioTramp(player, edx, id, text, pitch, icon);
}

static void InstallRadioFilter(DWORD delta)
{
    if (g_RadioTramp)
        return;
    unsigned char* fn = (unsigned char*)(MP_PLAYER_RADIO + delta);
    static const unsigned char code[] = { 0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68 }; // push ebp; mov ebp, esp; push -1; push imm32
    if (Readable(fn, 10) && !memcmp(fn, code, sizeof(code)))
        g_RadioTramp = (RadioFn)DetourCdecl(fn, 10, (void*)HookedRadio);
    Log("bots' radio filter: %s", g_RadioTramp ? "on (no \"Get in position\")" : "NOT hooked (unexpected code)");
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
    if (!NextWaypoint(bot, st, pos, goal, arrive, wp, true))
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
    moveYaw = WalkYaw(bot, pos, moveYaw, 40.0f);
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
// Skill 0..7 -> the game's own templates in testbotprofile.db (Easy, Fair, Normal, Tough, Hard, VeryHard, Expert,
// Elite). It was a straight line from skill 0.2 / attack delay 0.7 s to skill 1 / reaction 0 / delay 0: Very Easy aimed
// better than the game's Easy and the top level better than its Elite ("one-shots on Very Easy", reports 2026-11-26).
static const float ROOM_SKILL[8][3] = {
    // skill, reaction (file value - 0.1), attack delay
    { 0.00f, 0.40f, 1.50f }, // Easy
    { 0.25f, 0.30f, 1.00f }, // Fair
    { 0.50f, 0.30f, 0.70f }, // Normal
    { 0.60f, 0.20f, 0.35f }, // Tough
    { 0.75f, 0.15f, 0.00f }, // Hard
    { 0.80f, 0.15f, 0.00f }, // VeryHard
    { 0.90f, 0.10f, 0.00f }, // Expert
    { 1.00f, 0.10f, 0.00f }, // Elite
};
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
        const float* t = ROOM_SKILL[skill < 0 ? 0 : skill > 7 ? 7 : skill];
        *(float*)(bs.ownProfile + 8) = t[0];
        *(float*)(bs.ownProfile + 0x64) = t[1];
        *(float*)(bs.ownProfile + 0x68) = t[2];
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
    if (!rb)
        return;
    // Zombie Conquest: a room AI player without a skill of its own plays hard whatever the room's difficulty
    int skill = rb->skill < 0 && GameMode() == MODE_ZCON ? ZCON_SKILL : rb->skill;
    if (skill < 0)
        return;
    unsigned char* player = (unsigned char*)PrivateOf(bot);
    if (!player || IsBadReadPtr(player + PLAYER_BOT_PROFILE, 4))
        return;
    unsigned char* cur = *(unsigned char**)(player + PLAYER_BOT_PROFILE);
    if (cur && cur == bs.ownProfile && bs.ownSkill == skill)
        return;
    if (!bs.ownProfile)
        bs.ownProfile = new unsigned char[PROFILE_COPY];
    float was[3] = {};
    if (!SetRoomProfile(player, bs, skill, was))
    {
        static int fails = 0;
        if (fails++ < 5)
            Log("bot '%s': no bot profile at +0x%x: room skill not applied", rb->name.c_str(), PLAYER_BOT_PROFILE);
        bs.ownSkill = skill; // don't try every frame
        return;
    }
    bs.ownSkill = skill;
    Log("bot %d '%s': room skill %d (profile skill %.2f -> %.2f, reaction %.2f -> %.2f, attack delay %.2f -> %.2f)", bs.slot,
        rb->name.c_str(), skill, was[0], *(float*)(bs.ownProfile + 8), was[1], *(float*)(bs.ownProfile + 0x64), was[2],
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
    unsigned char* player = (unsigned char*)PrivateOf(bot);
    if (!player || IsBadReadPtr(player + 0xcd4, 4) || ZombieMode(GameMode()))
    {
        bs.charTeam = team;
        return;
    }
    float now = Now();
    if (team == bs.charTeam && (bs.appliedClass <= 0 || now - bs.charFixAt < 2.0f))
        return;
    int current = *(int*)(player + 0xcd4);
    int def = TeamDefaultClass(team);
    if (def <= 0 || current != def)
        return; // not swapped yet (or it already has a character of its own)
    MatchBot* rb = RoomBotOf(bot, bs);
    int id = 0;
    if (team == bs.charTeam)
    {
        // same team, back on the team's default character (a new round of Team Deathmatch did it): its own again
        int want = bs.appliedClass;
        if (!CharacterFits(want, team) || !CharacterClassNumber(want) || CharacterClassNumber(want) == def)
            return;
        id = want;
    }
    else if (rb && rb->character > 0 && CharacterFits(rb->character, team) && CharacterClassNumber(rb->character))
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
    bool swapped = team != bs.charTeam;
    bs.charTeam = team;
    bs.charFixAt = now;
    if (!number || !SetPlayerClass(player, number, sex != 0))
        return;
    bs.appliedClass = id;
    static int reports = 0;
    if (reports++ < 60)
        Log("bot %d '%s' %s team %d: character %d (class %d) instead of the team's default %d", bs.slot,
            rb ? rb->name.c_str() : "?", swapped ? "swapped to" : "put back on its own character on", team, id, number, def);
}

// ---------------------------------------------------------------- hooks
// ---------------------------------------------------------------- crossing the map: ladders and jumps
// Runs for every bot after its mode's driver (they all walk paths through NextWaypoint), so climbing and jumping work the
// same in every mode:
//  - a ladder of the path (FindPath's flagged waypoint): walk into it, look up (or down) and hold forward until the floor of
//    the other end; at the top, over the lip towards where it steps off
//  - a ledge too high to walk up, up to 60 units (a crouch jump: the legs tucked in the air clear 18 more than a jump), on the
//    way to a waypoint above: jump
//  - a gap: no floor (or a fall) just ahead and the waypoint across it, at most 240 away and not above a jump: a running jump
//  - the mesh's "jump" areas: jump into them; "no jump" areas: no jumping at all
//  - every jump tucks the legs in the air (duck 0.1 to 0.5 s after it)
static const int NAV_EV_MOVETYPE = 0x118, NAV_MOVETYPE_FLY = 5, FL_ONGROUND = 0x200, FL_DUCKING = 0x4000;
static const float JUMP_RISE = 60.0f, GAP_JUMP_MAX = 240.0f;

// the floor under a point (z), or -1e9 when there's none within depth; hurts: it is in a hurt zone
static float FloorUnder(void* bot, V3 at, float depth, bool& hurts)
{
    TraceResult tr;
    hurts = false;
    if (Trace(at, at - V3{ 0, 0, depth }, bot, 1, tr) || tr.fStartSolid || tr.fAllSolid)
        return -1e9f;
    V3 f{ tr.vecEndPos[0], tr.vecEndPos[1], tr.vecEndPos[2] };
    hurts = InHurtZone(f + V3{ 0, 0, 8 }, 0.0f);
    return f.z;
}

static void NavJump(BotState& st, unsigned short& buttons, float now, const char* why, V3 pos)
{
    buttons |= IN_JUMP;
    st.jumpAt = now;
    st.gapJumpAt = now;
    static int reports = 0, reportLevel = -1;
    if (reportLevel != g_nav.level)
        reportLevel = g_nav.level, reports = 0;
    if (reports++ < 60)
        Log("bot %d jumps (%s) at (%.0f %.0f %.0f)", st.slot, why, pos.x, pos.y, pos.z);
}

// the mover of a deck: the entity of its class and size closest to `around` (its bounds are the model's grown by 1 on each side)
static void* DeckEntity(const NavDeck& d, V3 around, float radius)
{
    void* best = nullptr;
    float bestDist = 1e30f;
    ForEntitiesNear(around, radius, [&](void* e) {
        if (d.cls != ClassName(e))
            return;
        unsigned char* v = Vars(e);
        V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
        V3 s = hi - lo;
        if (fabsf(s.x - d.size.x) > 6.0f || fabsf(s.y - d.size.y) > 6.0f || fabsf(s.z - d.size.z) > 6.0f)
            return;
        float dist = Len((lo + hi) * 0.5f - around);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = e;
        }
    });
    return best;
}
static V3 DeckFloor(const NavDeck& d, void* e) // the middle of the mover's floor where it is now
{
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    return V3{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, hi.z - 1.0f - d.sink };
}
static bool OnDeck(const NavDeck& d, void* e, V3 feet)
{
    unsigned char* v = Vars(e);
    V3 lo = Vec((float*)(v + EV_ABSMIN)), hi = Vec((float*)(v + EV_ABSMAX));
    float floor = hi.z - 1.0f - d.sink;
    return feet.x > lo.x && feet.x < hi.x && feet.y > lo.y && feet.y < hi.y && feet.z > floor - 12.0f && feet.z < floor + 30.0f;
}

// one frame of a ride: true when it moved the bot (NavTraverse stops there)
static bool NavRideStep(void* bot, BotState& st, const NavRide& r, V3 pos, V3 feet, float& fwd, float& side, unsigned short& buttons)
{
    float now = Now();
    float viewYaw = F<float>(Vars(bot), EV_VANGLE + 4);
    auto walkTo = [&](V3 p, float speed) {
        float rel = (YawOf(p - pos) - viewYaw) / RAD;
        fwd = cosf(rel) * speed;
        side = -sinf(rel) * speed;
    };
    static int reports = 0, reportLevel = -1;
    if (reportLevel != g_nav.level)
        reportLevel = g_nav.level, reports = 0;
    st.rideUntil = now + 1.0f;
    if (r.kind == 2)
    {
        // a teleport: into the trigger; on the other side NextWaypoint is at the way off already
        walkTo(r.on, 250.0f);
        if (Len2D(r.on - pos) < 24.0f)
            walkTo(r.on + (r.on - pos) * 4.0f, 250.0f);
        return true;
    }
    if (r.deckFrom < 0 || r.deckFrom >= (int)g_navDecks.size())
        return false;
    const NavDeck& from = g_navDecks[r.deckFrom];
    void* mover = DeckEntity(from, pos, 2000.0f);
    if (!mover)
        return false; // gone (a train that left for good): the path goes elsewhere next time
    V3 deck = DeckFloor(from, mover);
    if (OnDeck(from, mover, feet))
    {
        // aboard: to its middle, and stay
        if (Len2D(deck - pos) > 24.0f)
            walkTo(deck, 120.0f);
        else
            fwd = side = 0;
        buttons &= ~(IN_JUMP | IN_DUCK);
        if (reports++ < 40 && Len2D(deck - pos) <= 24.0f && Len(deck - from.at) < 8.0f)
            Log("bot %d is aboard the %s at (%.0f %.0f %.0f), going to (%.0f %.0f %.0f)", st.slot, from.cls.c_str(), deck.x,
                deck.y, deck.z, r.off.x, r.off.y, r.off.z);
        return true;
    }
    bool here = Len2D(deck - from.at) < 12.0f && fabsf(deck.z - from.at.z) < 10.0f;
    float edge = max(fabsf(pos.x - from.at.x) - from.size.x * 0.5f, fabsf(pos.y - from.at.y) - from.size.y * 0.5f);
    if (here)
    {
        // it's at this stop: on board (a jump if its floor is higher than this one)
        walkTo(from.at, 250.0f);
        if (from.at.z > feet.z + 18.0f && edge < 48.0f && now - st.jumpAt > 0.8f)
        {
            st.jumpAt = now;
            buttons |= IN_JUMP;
        }
        return true;
    }
    // not here yet: wait by its edge (the shaft or the track is no floor)
    if (edge > 72.0f)
        walkTo(from.at, 200.0f);
    else
        fwd = side = 0;
    return true;
}

// ---- ladders in turn (crowding.inl decides): every bot whose path went up a ladder walked into it at once, so five bots
// on zm_chosun's ladders pushed into each other, the ones climbing up into the ones coming down. A bot within
// LADDER_QUEUE of a ladder on its path waits while someone is on it the other way (or stopped, or still at this end),
// or another bot walking up to it is nearer: LADDER_WAIT_BACK from the ladder, out of the way of whoever gets off.
// After LadderWaitLimit (8 s, 3 s more for each bot ahead of it, at most 20) it takes another way if there is one (the
// ladder's ends cost NAV_AVOID_COST), else this ladder without waiting. One at the foot that doesn't get on for
// LADDER_TURN_TIME (zm_chosun: a bot tried to get on at the top 12 times in 15 s) loses its turn, and the next one goes.
static const float LADDER_QUEUE = 200.0f, LADDER_WAIT_BACK = 96.0f;
struct LadderApproach
{
    void* bot;
    int ladder;
    float dist;
    long long key;
    float at;
    bool waiting = false;    // it waited for its turn last move
    float best = 0, bestAt = 0; // its nearest to the ladder, and when it last got LADDER_CLOSER nearer (or waited)
    bool lostLogged = false;
};
static std::vector<LadderApproach> g_ladderQueue; // bots walking up to a ladder, refreshed each move

// 2D distance from p to the ladder's brush
static float LadderGap(const NavLadder& l, V3 p)
{
    float dx = p.x < l.lo.x ? l.lo.x - p.x : p.x > l.hi.x ? p.x - l.hi.x : 0.0f;
    float dy = p.y < l.lo.y ? l.lo.y - p.y : p.y > l.hi.y ? p.y - l.hi.y : 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

// true: the bot waits this move (fwd/side set)
static bool LadderQueueWait(void* bot, BotState& st, V3 pos, V3 feet, const float* aim, float& fwd, float& side,
                            unsigned short& buttons, float now)
{
    int ladder = -1;
    bool up = false;
    size_t last = (std::min)(st.path.size() - 1, st.pathIndex + 2);
    for (size_t k = st.pathIndex; k <= last && k < st.pathFlags.size(); k++)
    {
        int f = st.pathFlags[k];
        if (f > 0 && f <= (int)g_ladders.size())
        {
            ladder = f - 1;
            up = st.path[k].z > feet.z;
            break;
        }
        if (f)
            break; // a ride or a drawn link comes first
    }
    if (now - st.ladderWaitLast > 0.5f || now < st.ladderWaitLast)
        st.ladderWaitSince = -1; // (it wasn't waiting a moment ago)
    if (ladder < 0 || (ladder == st.ladderSkip && now < st.ladderSkipUntil))
        return false;
    const NavLadder& l = g_ladders[ladder];
    float entryZ = up ? l.lo.z : l.hi.z;
    float gap = LadderGap(l, pos);
    if (gap > LADDER_QUEUE || fabsf(feet.z - entryZ) > 100.0f)
        return false;
    long long key = CrowdKey(st, bot);
    std::vector<LadderOther> others;
    // on it: anyone climbing (movetype fly) within the ladder's brush
    V3 c{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, (l.lo.z + l.hi.z) * 0.5f };
    ForEntitiesNear(c, Len(l.hi - l.lo) * 0.5f + 64.0f, [&](void* e) {
        unsigned char* v = Vars(e);
        if (e == bot || !(F<int>(v, EV_FLAGS) & FL_CLIENT) || !Alive(e) || F<int>(v, NAV_EV_MOVETYPE) != NAV_MOVETYPE_FLY)
            return;
        V3 o = Origin(e);
        if (LadderGap(l, o) > 24.0f || o.z < l.lo.z - 40.0f || o.z > l.hi.z + 76.0f)
            return;
        float vz = ((float*)(v + EV_VELOCITY))[2];
        others.push_back({ true, vz > 20.0f ? 1 : vz < -20.0f ? -1 : 0, o.z - 36.0f, 0.0f, 0 });
    });
    // walking up to it: the other bots here that were in the line a moment ago (an entry is kept LADDER_KEEP for its
    // clock, through a try to get on); one at the foot that isn't getting on has lost its turn (crowding.inl's
    // LadderTurnLost)
    for (size_t i = g_ladderQueue.size(); i-- > 0;)
    {
        LadderApproach& a = g_ladderQueue[i];
        if (a.bot == bot)
            continue;
        if (now < a.at || now - a.at > LADDER_KEEP)
        {
            g_ladderQueue.erase(g_ladderQueue.begin() + i);
            continue;
        }
        if (a.ladder == ladder && now - a.at <= 0.3f)
            others.push_back({ false, 0, 0.0f, a.dist, a.key, LadderTurnLost(a.waiting, a.dist, a.bestAt, now) });
    }
    // this bot's own entry: new, for another ladder, or back after more than LADDER_KEEP starts its clock (back sooner,
    // from a try to get on, it keeps it: LadderClockKeeps); waiting, or LADDER_CLOSER nearer than its nearest, starts it
    // again
    LadderApproach* me = nullptr;
    for (LadderApproach& a : g_ladderQueue)
        if (a.bot == bot)
            me = &a;
    if (!me)
    {
        g_ladderQueue.push_back({ bot, ladder, gap, key, now });
        me = &g_ladderQueue.back();
        me->best = gap, me->bestAt = now;
    }
    else if (!LadderClockKeeps(me->ladder, ladder, me->at, now))
    {
        *me = { bot, ladder, gap, key, now };
        me->best = gap, me->bestAt = now;
    }
    me->dist = gap, me->key = key, me->at = now;
    if (me->waiting || gap < me->best - LADDER_CLOSER)
        me->best = gap, me->bestAt = now;
    if (!LadderTurnLost(me->waiting, gap, me->bestAt, now))
        me->lostLogged = false;
    else if (!me->lostLogged)
    {
        me->lostLogged = true;
        g_ladderCounts[3]++;
        static int lostReports = 0, lostLevel = -1;
        if (lostLevel != g_nav.level)
            lostLevel = g_nav.level, lostReports = 0;
        if (lostReports++ < 40)
            Log("bot %d lost its turn at ladder %d (%s): at its foot %.0f s without getting on, the next one goes", st.slot,
                ladder, up ? "up" : "down", LADDER_TURN_TIME);
    }
    me->waiting = LadderMustWait(up ? 1 : -1, gap, key, entryZ, others);
    if (!me->waiting)
        return false;
    static int reports = 0, reportLevel = -1;
    if (reportLevel != g_nav.level)
        reportLevel = g_nav.level, reports = 0;
    if (st.ladderWaitSince < 0)
    {
        st.ladderWaitSince = now;
        g_ladderCounts[1]++;
        int on = 0;
        for (const LadderOther& o : others)
            on += o.on;
        if (reports++ < 80)
            Log("bot %d waits for its turn at ladder %d (%s, %.0f u from it): %d on it, %d walking up to it", st.slot, ladder,
                up ? "up" : "down", gap, on, (int)others.size() - on);
    }
    st.ladderWaitLast = now;
    float limit = LadderWaitLimit(LadderAhead(gap, key, others)); // (longer the more are ahead of it)
    if (now - st.ladderWaitSince > limit)
    {
        // waited long enough: another way if there is one (the ends of this ladder cost more for a while), else this one
        for (int area : { l.bottom, l.top })
            if (area >= 0)
            {
                if (st.navAvoid.size() >= 8)
                    st.navAvoid.erase(st.navAvoid.begin());
                st.navAvoid.push_back({ area, now + NAV_AVOID_TIME });
            }
        g_ladderCounts[2]++;
        float waited = now - st.ladderWaitSince;
        st.ladderSkip = ladder;
        st.ladderSkipUntil = now + NAV_AVOID_TIME;
        st.ladderWaitSince = -1;
        st.ladderWaitUntil = 0;
        st.nextRepath = 0;
        if (reports++ < 80)
        {
            // who it waited for: the ones ahead of it in line (distance, waiting or not) and how many were on it
            std::string ahead;
            int on = 0;
            for (const LadderOther& o : others)
                on += o.on;
            for (const LadderApproach& a : g_ladderQueue)
            {
                if (a.bot == bot || a.ladder != ladder || now - a.at > 0.3f)
                    continue;
                bool lost = LadderTurnLost(a.waiting, a.dist, a.bestAt, now);
                if (lost || !(a.dist + LADDER_TIE < gap || (fabsf(a.dist - gap) <= LADDER_TIE && a.key < key)))
                    continue;
                BotState* o = FindBotState(a.bot);
                char b[64];
                snprintf(b, sizeof(b), "%sbot %d %.0f u%s", ahead.empty() ? "" : ", ", o ? o->slot : -1, a.dist,
                         a.waiting ? " waiting" : "");
                ahead += b;
            }
            Log("bot %d gave up waiting for ladder %d after %.0f s: another way, or this ladder without waiting (ahead: %s; %d on "
                "it)", st.slot, ladder, waited, ahead.empty() ? "nobody" : ahead.c_str(), on);
        }
        return false;
    }
    st.ladderWaitUntil = now + 0.3f;
    fwd = side = 0;
    buttons &= ~IN_JUMP;
    if (gap < LADDER_WAIT_BACK)
    {
        // too close: out of the way of whoever gets off (away from the ladder, if the floor goes on)
        float yaw = YawOf(pos - c);
        if (!DropAhead(bot, pos, yaw, 72.0f, SAFE_DROP))
        {
            float rel = AngleDiff(yaw, aim[1]) / RAD;
            fwd = cosf(rel) * 150.0f;
            side = -sinf(rel) * 150.0f;
        }
    }
    return true;
}

static void NavTraverse(void* bot, BotState& st, float* aim, float& fwd, float& side, unsigned short& buttons)
{
    unsigned char* v = Vars(bot);
    float now = Now();
    int flags = F<int>(v, EV_FLAGS);
    bool onLadder = F<int>(v, NAV_EV_MOVETYPE) == NAV_MOVETYPE_FLY;
    if (now - st.jumpAt > 0.1f && now - st.jumpAt < 0.5f && !onLadder)
        buttons |= IN_DUCK; // tucked legs: a crouch jump
    if (!g_nav.ok || st.path.empty() || st.pathIndex >= st.path.size() || now - st.navLastCall > 0.3f || now < st.navLastCall)
        return;
    V3 pos = Origin(bot);
    V3 feet = pos - V3{ 0, 0, (flags & FL_DUCKING) ? 18.0f : 36.0f };
    V3 wp = st.path[st.pathIndex];
    int flag = st.pathIndex < st.pathFlags.size() ? st.pathFlags[st.pathIndex] : 0;
    float* va = (float*)(v + EV_VANGLE);
    float* ang = (float*)(v + EV_ANGLES);

    // ---- a lift, a train, a teleport
    int ride = RideFlagAhead(st.pathFlags, st.pathIndex);
    if (ride && (flag == ride || Len2D(wp - pos) < 300.0f) && ride - NAV_RIDE_FLAG < (int)g_navRides.size() &&
        NavRideStep(bot, st, g_navRides[ride - NAV_RIDE_FLAG], pos, feet, fwd, side, buttons))
        return;
    // ---- a link drawn with wp_link: straight at its far point, jumping, crouching or climbing as it says
    if (flag >= NAV_LINK_FLAG && (flag - NAV_LINK_FLAG) / 2 < (int)g_links.size())
    {
        const NavLink& k = g_links[(flag - NAV_LINK_FLAG) / 2];
        bool back = ((flag - NAV_LINK_FLAG) & 1) != 0;
        V3 from = g_linkPoints[back ? k.b : k.a].pos;
        int kind = k.kind;
        if (back && kind == LINK_JUMP && wp.z < from.z - 18.0f)
            kind = LINK_WALK; // a jump up taken the other way: off the ledge
        static const char* const names[] = { "walk", "jump", "crouch", "ladder", "drop" };
        static int reports = 0, reportLevel = -1;
        if (reportLevel != g_nav.level)
            reportLevel = g_nav.level, reports = 0;
        if (st.onLink != flag && reports++ < 80)
            Log("bot %d takes a drawn %s link from (%.0f %.0f %.0f) to (%.0f %.0f %.0f)", st.slot, names[kind], from.x, from.y, from.z,
                wp.x, wp.y, wp.z);
        st.onLink = flag;
        float yaw = YawOf(wp - pos);
        if (kind == LINK_LADDER)
        {
            bool up = wp.z > feet.z;
            float pitch = onLadder ? (up ? -60.0f : 60.0f) : (up ? -20.0f : 30.0f);
            if (onLadder && up && feet.z > wp.z - 24.0f)
                pitch = -40.0f; // at the top: over the lip
            aim[0] = va[0] = pitch;
            aim[1] = va[1] = AngleDiff(yaw, 0.0f);
            va[2] = 0;
            ang[0] = pitch / -3.0f;
            ang[1] = va[1];
            fwd = 250.0f;
            side = 0;
            buttons &= ~(IN_JUMP | IN_DUCK);
            st.duckUntil = 0;
            st.climbing = true;
            return;
        }
        float rel = AngleDiff(yaw, aim[1]) / RAD;
        fwd = cosf(rel) * 250.0f;
        side = -sinf(rel) * 250.0f;
        if (kind != LINK_JUMP)
            buttons &= ~IN_JUMP;
        if (kind == LINK_CROUCH)
        {
            buttons |= IN_DUCK;
            st.duckUntil = now + 0.3f;
        }
        if (kind == LINK_JUMP && (flags & FL_ONGROUND) && now - st.jumpAt > 0.8f)
        {
            // at the near point (or just past it, on the way), or against the ledge it leads up to
            V3 ab = wp - from, ap = pos - from;
            float abl = Len2D(ab), along = abl > 1.0f ? (ap.x * ab.x + ap.y * ab.y) / abl : 0.0f;
            bool takeoff = Len2D(ap) < 32.0f || (along > 0.0f && along < abl * 0.5f && Len2D(ap) < 80.0f);
            bool ledge = wp.z > feet.z + 16.0f && Len2D(wp - pos) < 72.0f;
            if (takeoff || ledge)
                NavJump(st, buttons, now, "along a drawn link", pos);
        }
        return;
    }
    st.onLink = 0;
    // ---- a ladder someone else is using, or will first: wait for the turn
    if (!onLadder && LadderQueueWait(bot, st, pos, feet, aim, fwd, side, buttons, now))
        return;
    // ---- a ladder
    if (flag > 0 && flag <= (int)g_ladders.size() && fabsf(feet.z - wp.z) > 24.0f)
    {
        const NavLadder& l = g_ladders[flag - 1];
        bool up = wp.z > feet.z;
        V3 mid{ (l.lo.x + l.hi.x) * 0.5f, (l.lo.y + l.hi.y) * 0.5f, pos.z };
        float dx = pos.x < l.lo.x ? l.lo.x - pos.x : pos.x > l.hi.x ? pos.x - l.hi.x : 0.0f;
        float dy = pos.y < l.lo.y ? l.lo.y - pos.y : pos.y > l.hi.y ? pos.y - l.hi.y : 0.0f;
        float gap = sqrtf(dx * dx + dy * dy);
        float yaw = YawOf(mid - pos), pitch;
        if (onLadder || gap < 40.0f)
        {
            if (up && feet.z > l.hi.z - 24.0f)
            {
                yaw = YawOf(wp - pos); // at the top: over the lip
                pitch = -40.0f;
            }
            else
                pitch = up ? -60.0f : 60.0f;
        }
        else
            pitch = up ? -20.0f : 30.0f;
        aim[0] = va[0] = pitch;
        aim[1] = va[1] = AngleDiff(yaw, 0.0f);
        va[2] = 0;
        ang[0] = pitch / -3.0f;
        ang[1] = va[1];
        fwd = 250.0f;
        side = 0;
        buttons &= ~(IN_JUMP | IN_DUCK);
        st.duckUntil = 0;
        static int reports = 0, reportLevel = -1;
        if (reportLevel != g_nav.level)
            reportLevel = g_nav.level, reports = 0;
        if (!st.climbing)
            g_ladderCounts[0]++;
        if (!st.climbing && reports++ < 60)
            Log("bot %d takes a ladder %s at (%.0f %.0f %.0f) to (%.0f %.0f %.0f)", st.slot, up ? "up" : "down", pos.x, pos.y, pos.z,
                wp.x, wp.y, wp.z);
        st.climbing = true;
        return;
    }
    st.climbing = false;
    if (onLadder)
        return;
    int here = AreaAt(pos);
    if (here >= 0 && (g_nav.areas[here].attr & NAV_NO_JUMP))
    {
        buttons &= ~IN_JUMP;
        return;
    }
    if (!(flags & FL_ONGROUND) || (fabsf(fwd) < 1.0f && fabsf(side) < 1.0f) || now - st.gapJumpAt < 0.8f || now - st.jumpAt < 0.6f)
        return;
    // the way it walks: its view plus the move keys (sidemove positive is right)
    float moveYaw = va[1] + atan2f(-side, fwd) * RAD;
    V3 dir = YawDir(moveYaw);
    float toWp = fabsf(AngleDiff(YawOf(wp - pos), moveYaw));
    bool hurts = false;

    // a ledge to climb with a crouch jump
    if (wp.z > feet.z + 12.0f && toWp < 60.0f)
        for (float d : { 20.0f, 36.0f })
        {
            V3 probe = pos + dir * d;
            probe.z = feet.z + JUMP_RISE + 8.0f;
            float fz = FloorUnder(bot, probe, JUMP_RISE + 72.0f, hurts);
            if (fz < -1e8f || hurts)
                break;
            float rise = fz - feet.z;
            if (rise > 20.0f && rise <= JUMP_RISE)
            {
                TraceResult tr;
                if (Trace(pos + V3{ 0, 0, 28 }, pos + V3{ 0, 0, 28 } + dir * (d + 16.0f), bot, 1, tr)) // room for the head
                {
                    NavJump(st, buttons, now, "onto a ledge", pos);
                    return;
                }
            }
            if (rise > 4.0f)
                break; // something lower was in the way first
        }

    // a gap with the waypoint across it
    float speed = Len2D(Vec((float*)(v + EV_VELOCITY)));
    float horiz = Len2D(wp - pos);
    if (speed > 160.0f && toWp < 30.0f && horiz > 56.0f && horiz < GAP_JUMP_MAX && wp.z < feet.z + 40.0f && wp.z > feet.z - 120.0f)
    {
        V3 ahead = pos + dir * 28.0f;
        float fz = FloorUnder(bot, ahead, 36.0f + 96.0f, hurts);
        if (fz < -1e8f || fz < feet.z - 64.0f || hurts)
        {
            bool landHurts = false;
            float lz = FloorUnder(bot, wp + V3{ 0, 0, 36 }, 100.0f, landHurts);
            TraceResult tr;
            if (lz > -1e8f && !landHurts && Trace(pos + V3{ 0, 0, 16 }, wp + V3{ 0, 0, 40 }, bot, 1, tr))
            {
                NavJump(st, buttons, now, "over a gap", pos);
                return;
            }
        }
    }

    // the mesh marks the next area as one to jump into
    int next = AreaAt(wp);
    if (next >= 0 && next != here && (g_nav.areas[next].attr & NAV_JUMP) && horiz < 56.0f && wp.z > feet.z + 10.0f && toWp < 60.0f)
        NavJump(st, buttons, now, "into a jump area", pos);
}

// ---- holding the trigger (every mode)
// The stock AI taps the trigger: short bursts, released between them. A minigun (M134, Gatling, Vulcanus...) only
// fires once its barrel has spun up with the trigger held, so bots spun it, let go, spun it again and never shot; the
// automatic weapons only gave short bursts. HoldFire keeps attack pressed a moment after the AI lets go (HOLD_AUTO, a
// spin-up weapon HOLD_SPIN) while an enemy is still in front of the aim (HOLD_CONE degrees, HOLD_RANGE units) and the
// weapon in hand has rounds in its clip. Only automatic weapons: SMGs and machine guns, rifles that aren't snipers
// (botassist_weapons.txt category by the model in hand) and the spin-up ones by name. Semi-automatic and bolt guns
// need the release between shots: they're left as the AI fires them. Not for zombies or the reload key.
static const float HOLD_AUTO = 0.45f, HOLD_SPIN = 1.5f, HOLD_CONE = 14.0f, HOLD_RANGE = 3000.0f;

static bool HoldSpinWeapon(const std::string& m)
{
    static const char* const spin[] = { "m134", "gatling", "vulcan", "minigun", "m61", "mg3", "minigatling", "gungnir" };
    for (const char* s : spin)
        if (m.find(s) != std::string::npos)
            return true;
    return false;
}

static bool HoldSniper(const std::string& m)
{
    static const char* const semi[] = { "awp", "scout", "sg550", "g3sg1", "sl8", "m24", "m400", "svd", "dragunov",
                                        "barrett", "m95", "m82", "xm2010", "as50", "trg", "m200", "tac", "crossbow",
                                        "bow", "sniper", "kar98", "m1garand", "mosin", "railgun", "deagle", "anaconda" };
    for (const char* s : semi)
        if (m.find(s) != std::string::npos)
            return true;
    return false;
}

static void HoldFire(void* bot, BotState& st, const float* aim, unsigned short& buttons)
{
    float now = Now();
    if (buttons & IN_ATTACK)
    {
        st.fireAt = now;
        return;
    }
    if (buttons & IN_RELOAD)
        return;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || p[0x2166] || !Alive(bot))
        return;
    const char* model = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    if (!model || !*model)
        return;
    std::string m = model;
    for (auto& ch : m)
        ch = (char)tolower(ch);
    bool spin = HoldSpinWeapon(m);
    float window = spin ? HOLD_SPIN : HOLD_AUTO;
    if (now - st.fireAt > window)
        return;
    if (!spin)
    {
        auto cat = cfg.weaponCategory.find(m);
        int c = cat != cfg.weaponCategory.end() ? cat->second : 0;
        if (!(c == 3 || c == 5 || (c == 4 && !HoldSniper(m))))
            return;
    }
    // rounds in the clip of the weapon in hand (an empty one reloads instead)
    unsigned char* w = ItemHandle(p + PLAYER_ACTIVE_ITEM);
    if (!w || !Readable(w + 0x160, 4) || *(int*)(w + 0x160) <= 0)
        return;
    // an enemy still in front of the aim
    V3 eye = Eye(bot);
    int team = TeamOf(bot);
    bool ahead = false;
    ForEntitiesNear(eye, HOLD_RANGE, [&](void* e) {
        if (ahead || e == bot)
            return;
        int flags = F<int>(Vars(e), EV_FLAGS);
        bool enemy = (flags & FL_CLIENT) ? Alive(e) && TeamOf(e) != team : IsLivingMonster(e);
        if (!enemy)
            return;
        V3 to = Center(e) - eye;
        if (fabsf(AngleDiff(YawOf(to), aim[1])) < HOLD_CONE && fabsf(PitchOf(to) - aim[0]) < HOLD_CONE + 6.0f)
            ahead = true;
    });
    if (!ahead)
        return;
    buttons |= IN_ATTACK;
    if (now - st.holdLog > 30.0f)
    {
        st.holdLog = now;
        static int reports = 0;
        if (reports++ < 40)
            Log("bot '%s' holds the trigger of its %s (%s)", String(F<int>(Vars(bot), EV_NETNAME)), model,
                spin ? "spin-up weapon" : "automatic");
    }
}

static void HookedRunPlayerMove(void* fakeclient, const float* angles, float fwd, float side, float up,
                                unsigned short buttons, unsigned char impulse, unsigned char msec)
{
    float aim[3] = { angles[0], angles[1], angles[2] };
    BotState* dbg = nullptr;
    double perfStart = PerfNow(), perfPart = perfStart;
    __try
    {
        if (cfg.enabled && fakeclient && g_edictToVars >= 0)
        {
            unsigned char* v = Vars(fakeclient);
            UpdateCvars(fakeclient);
            LoadNav();
            UpdateHurtZones();
            UpdateMatchBots();
            KickForJoiners();
            UpdateZgPlayers();
            PerfMark(0, perfPart);
            if (F<int>(v, EV_FLAGS) & FL_FAKECLIENT)
            {
                BotState& bs = StateFor(fakeclient);
                PtnBotThink(fakeclient);
                BeastBotThink(fakeclient);
                // (Partner: a partner keeps the character it was chosen for. Beast: a beast keeps its model)
                if (!PtnIsPartner(fakeclient) && !BeastIsBeast(fakeclient))
                {
                    ApplyRoomCharacter(fakeclient, bs);
                    ApplyTeamCharacter(fakeclient, bs);
                }
                ApplyRoomSkill(fakeclient, bs);
                if (cfg.sameTeam && CoopOneTeam())
                    FixCoopTeam(fakeclient, bs);
                UpdateBuying(fakeclient, bs);
                ShelterBotThink(fakeclient, bs);
                if (CoopMode() && MonsterMap())
                {
                    UpdateRefill(fakeclient, bs);
                }
            }
            PerfMark(1, perfPart);
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
                bool monsters = scenario && (MonsterMap() || ShelterActive()); // the shelter: its tasks even with no zombie around
                if (scenario && !monsters && Now() >= st.nextScan)
                {
                    ScanMonsters(fakeclient, st, Eye(fakeclient));
                    st.nextScan = Now() + 1.0f;
                    monsters = st.threat != nullptr;
                }
                float stockYaw = aim[1];
                if (!scenario && ZombieCloakAim(fakeclient, aim, buttons))
                {
                    // the stock AI's move was meant for its own view: keep its direction on the ground for the new
                    // view (turned to a zombie, a bot backing off kept "backwards" and ran into it)
                    float speed = sqrtf(fwd * fwd + side * side);
                    if (speed > 1.0f)
                    {
                        float moveYaw = stockYaw + atan2f(-side, fwd) * RAD;
                        float rel = (moveYaw - aim[1]) / RAD;
                        fwd = cosf(rel) * speed;
                        side = -sinf(rel) * speed;
                    }
                    float* va = (float*)(v + EV_VANGLE); // fixangle: see below
                    float* ang = (float*)(v + EV_ANGLES);
                    va[0] = aim[0];
                    va[1] = aim[1];
                    va[2] = 0;
                    ang[0] = aim[0] / -3.0f;
                    ang[1] = aim[1];
                    ang[2] = 0;
                }
                if (!scenario && !PtnIsPartner(fakeclient))
                {
                    float f = fwd, sd = side;
                    unsigned short bt = buttons;
                    if (OrderedMove(fakeclient, st, aim[1], f, sd, bt))
                    {
                        fwd = f;
                        side = sd;
                        buttons = bt;
                    }
                    // zombies close: backing off comes before orders
                    f = fwd, sd = side, bt = buttons;
                    if (ZombieFlee(fakeclient, st, aim[1], f, sd, bt))
                    {
                        fwd = f;
                        side = sd;
                        buttons = bt;
                    }
                }
                PerfMark(2, perfPart);
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
                if (!scenario && GameMode() == MODE_ZHE)
                {
                    float y = aim[1], p = aim[0], f = fwd, sd = side;
                    unsigned short bt = buttons;
                    if (DriveZheBot(fakeclient, y, p, f, sd, bt))
                    {
                        aim[0] = p;
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
                if (!scenario && GameMode() == MODE_ZE)
                {
                    float y = aim[1], p = aim[0], f = fwd, sd = side;
                    unsigned short bt = buttons;
                    bool setView = false;
                    if (ZeMove(fakeclient, st, y, p, setView, f, sd, bt))
                    {
                        fwd = f;
                        side = sd;
                        buttons = bt;
                        if (setView)
                        {
                            aim[0] = p < -89.0f ? -89.0f : p > 89.0f ? 89.0f : p;
                            aim[1] = AngleDiff(y, 0.0f);
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
                }
                if (!scenario && (GameMode() == MODE_ZTAG || GameMode() == MODE_SOCCER))
                {
                    float y = aim[1], p = aim[0], f = fwd, sd = side;
                    unsigned short bt = buttons;
                    if (GameMode() == MODE_ZTAG ? DriveZtBot(fakeclient, y, p, f, sd, bt) : DriveSoccerBot(fakeclient, y, p, f, sd, bt))
                    {
                        aim[0] = p;
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
                if (!scenario && (GameMode() == MODE_BEAST || GameMode() == MODE_ITEM))
                {
                    float y = aim[1], p = aim[0], f = fwd, sd = side;
                    unsigned short bt = buttons;
                    if (GameMode() == MODE_BEAST ? DriveBeast(fakeclient, st, y, p, f, sd, bt) : DriveItem(fakeclient, st, y, p, f, sd, bt))
                    {
                        aim[0] = p;
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
                if (!scenario && GameMode() == MODE_Z4)
                    Z4BotSkills(fakeclient);
                if (!scenario && (GameMode() == MODE_ZB2 || HeroMode(GameMode())))
                    ZB2BotThink(fakeclient); // (Zombie Hero's versions too: modes/zombie_hero.inl)
                if (!scenario && (GameMode() == MODE_ZCON || GameMode() == MODE_ZWAR || GameMode() == MODE_GDM ||
                                  GameMode() == MODE_SS || GameMode() == MODE_PTN))
                {
                    if (GameMode() == MODE_ZCON)
                        ZConBotThink(fakeclient, st);
                    else if (GameMode() == MODE_ZWAR)
                    {
                        ZB2BotThink(fakeclient);
                        ZWarBuyPick(fakeclient);
                    }
                    float y = aim[1], f = fwd, sd = side;
                    unsigned short bt = buttons;
                    bool setView = false;
                    if (GameMode() == MODE_ZCON   ? ZConMove(fakeclient, st, y, setView, f, sd, bt)
                        : GameMode() == MODE_ZWAR ? ZWarMove(fakeclient, st, y, setView, f, sd, bt)
                        : GameMode() == MODE_PTN  ? PtnMove(fakeclient, st, y, setView, f, sd, bt)
                                                  : GdmMove(fakeclient, st, y, setView, f, sd, bt))
                    {
                        fwd = f;
                        side = sd;
                        buttons = bt;
                        if (setView)
                        {
                            aim[1] = y;
                            float* ang = (float*)(v + EV_ANGLES);
                            ang[1] = aim[1];
                        }
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
    PerfMark(3, perfPart);
    if (dbg)
        __try { NavTraverse(fakeclient, *dbg, aim, fwd, side, buttons); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    PerfMark(4, perfPart);
    if (dbg && Now() < dbg->duckUntil && dbg->duckUntil - Now() < 1.0f)
        buttons |= IN_DUCK;
    if (dbg)
        __try { HoldFire(fakeclient, *dbg, aim, buttons); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    PerfMark(5, perfPart);
    // bot_stop: last, over every mode's driving
    __try
    {
        if (cfg.enabled && fakeclient && g_edictToVars >= 0 && (F<int>(Vars(fakeclient), EV_FLAGS) & FL_FAKECLIENT) &&
            BotStopped(fakeclient))
        {
            fwd = side = up = 0;
            buttons = 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_perfMoveSec += PerfNow() - perfStart;
    g_perfMoves++;
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
    int n = 0, all = 0;
    int team = TeamOf(by);
    void* answer = nullptr;
    std::vector<void*> mates;
    for (auto& b : g_bots)
        if (b.edict && (F<int>(Vars(b.edict), EV_FLAGS) & FL_FAKECLIENT) && (!team || TeamOf(b.edict) == team))
            mates.push_back(b.edict);
    all = (int)mates.size();
    std::vector<void*> reach = OrderReach(by, team, mates); // the room's option: every bot, or the N nearest
    // the room's chances: a bot it reaches may not obey, one further away may obey anyway (each keeps what it was doing)
    std::vector<void*> obey;
    void* refuser = nullptr;
    int farOk = 0, refused = 0;
    for (void* e : mates)
    {
        bool reached = std::find(reach.begin(), reach.end(), e) != reach.end();
        if (OrderObeyed(reached))
        {
            obey.push_back(e);
            farOk += !reached;
        }
        else if (reached)
        {
            refused++;
            if (!refuser && Alive(e))
                refuser = e;
        }
    }
    if ((int)obey.size() == all)
        g_teamOrder = order;
    for (auto& b : g_bots)
    {
        if (!b.edict || std::find(obey.begin(), obey.end(), b.edict) == obey.end())
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
    Log("order: %s (%d of %d bots: %d of them further away; %d near ones said no)", OrderName(order), n, all, farOk, refused);
    if (Voices() != "off" && answer)
        FakeCommand(answer, "roger");
    if (Voices() != "off" && refuser)
        FakeCommand(refuser, "negative");
}

// true: the command stops here (botassist's own menu took it)
static bool OnPlayerCommand(void* player)
{
    const char* cmd = g_CmdArgv(0);
    if (!cmd || !*cmd)
        return false;
    std::string c = cmd;
    for (auto& ch : c)
        ch = (char)tolower(ch);
    if (ShelterPlayerCommand(player, c)) // Zombie Shelter: the orders menu
        return true;
    if (ScenarioPlayerCommand(player, c)) // the scenario modes: the orders menu
        return true;
    if (SoccerPlayerCommand(player, c)) // Soccer: the orders menu
        return true;
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
        if (GameMode() != MODE_PTN) // there they are the partner's orders
            g_openMenu = c[5] - '0';
        return false;
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
        return false;
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
            return false;
    }
    if (c == "followme" || c == "regroup" || c == "sticktog" || c == "fallback")
        GiveOrder(ORDER_FOLLOW, player);
    else if (c == "coverme")
        GiveOrder(ORDER_ESCORT, player);
    else if (c == "holdpos" || c == "getinpos")
        GiveOrder(ORDER_HOLD, player);
    else if (c == "takepoint" || c == "stormfront" || c == "go")
        GiveOrder(ORDER_INDEPENDENT, player);
    return false;
}

static void HookedClientCommand(void* player)
{
    __try
    {
        if (cfg.enabled && player && g_edictToVars >= 0 && IsHuman(player))
        {
            if (OnPlayerCommand(player))
                return; // botassist's own menu took it
        }
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
// VirtualQuery is a system call, and botassist checks memory before nearly every read of the game's objects: hundreds
// of times per bot move (31 bots: 0.5-0.75 ms per move, a third of a second of every second). The readable regions it
// finds are remembered for READABLE_KEEP_MS (per thread): the game's heap and module regions don't go away that fast,
// and the risky reads are under __try anyway.
static const DWORD READABLE_KEEP_MS = 100;
static bool Readable(const void* p, size_t n)
{
    struct Region
    {
        const char* lo;
        const char* hi;
        DWORD at;
    };
    static thread_local Region known[32] = {};
    static thread_local int next = 0;
    const char* a = (const char*)p;
    DWORD now = GetTickCount();
    for (const Region& r : known)
        if (a >= r.lo && a < r.hi && n <= (size_t)(r.hi - a) && now - r.at < READABLE_KEEP_MS)
            return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const char* lo = (const char*)mbi.BaseAddress;
    const char* hi = lo + mbi.RegionSize;
    if (a + n > hi)
        return false;
    known[next] = { lo, hi, now };
    next = (next + 1) % 32;
    return true;
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
    g_TraceHull = (TraceHullFn)table[ENG_TRACEHULL];
    g_CmdArgv = (CmdArgvFn)table[ENG_CMD_ARGV];
    NavHookFiles(table);
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
    g_WriteString = (WriteStringFn)table[59]; // pfnWriteString (mp.dll's ShowMenu writes its text with it)
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
    FixShellKnives(delta);
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

#include "gameui_guards.inl"

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

#include "room_settings_button.inl"
#include "family_button.inl"
#include "shop_redirect.inl"

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
    // the medals' texts and a few others CSNZ_Server changes (the tutorial's rewards, the Point Coupon): all CSO_ tokens
    if (_strnicmp(token, "CSO_", 4) != 0)
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

// The room's panels show the room's AI difficulty (GameUI 0x10442bd0 and 0x10446080: "#CSO_Bot_Difficulty_<n>"), whatever skill
// each AI player has on the AI players page. Asked from those two, the answer is the bots' own: their common skill, or
// "Random" if they differ; only bots with the room's skill keep the room's text. The server sends the room's bot settings
// when the list changes, which makes the panel ask again (the answer is kept 300 ms, it may ask several times).
static const wchar_t* RoomBotLevelText(void* self, const char* token, void* from)
{
    static const char PREFIX[] = "CSO_Bot_Difficulty_";
    if (*token == '#')
        token++;
    if (_strnicmp(token, PREFIX, sizeof(PREFIX) - 1) || !isdigit((unsigned char)token[sizeof(PREFIX) - 1]))
        return nullptr;
    HMODULE ui = GetModuleHandleA("GameUI.dll");
    DWORD at = ui ? (DWORD)from - (DWORD)ui + 0x10000000 : 0;
    if (!(at >= 0x10442bd0 && at < 0x10443100) && !(at >= 0x10446080 && at < 0x10446800))
        return nullptr;
    static DWORD askedAt = 0;
    static int level = -1; // -1 the room's, -2 random, else the bots' common skill
    DWORD tick = GetTickCount();
    if (!askedAt || tick - askedAt > 300)
    {
        askedAt = tick;
        std::string json = HttpGet(L"/api/roombots");
        int common = -1, seen = 0;
        bool differ = false;
        for (size_t i = json.find("\"skill\":"); i != std::string::npos; i = json.find("\"skill\":", i + 1))
        {
            int skill = atoi(json.c_str() + i + 8);
            differ |= seen && skill != common;
            common = skill;
            seen++;
        }
        level = !seen ? -1 : differ ? -2 : common >= 0 && common < 8 ? common : -1;
    }
    if (level == -1)
        return nullptr;
    if (level == -2)
        return L"Random";
    char own[40];
    snprintf(own, sizeof(own), "#%s%d", PREFIX, level);
    return ((LocFindFn)g_locFind[1])(self, nullptr, own);
}

static const wchar_t* __fastcall LocFind4(void* self, void*, const char* token)
{
    const wchar_t* text = QuestText(token);
    if (!text && token)
        text = RoomBotLevelText(self, token, _ReturnAddress());
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
    Log("game texts: %d texts replaced (the Medals tab's offline conditions, the tutorial's rewards...)", (int)g_questTexts->size());
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

// The lobby's Decoder window (GameUI CGachaponDecoderDlg) lists a decoder when the name in its item row (ItemBoxName,
// item vfunc 0x74) is in hw's item box registry (0x2e2adc0: a vector of boxes at +4/+8, each with its name as a
// std::string at +4), which the server's Metadata ItemBox fills (one box per decoder id, hw 0x255fb40), and, for the ones
// the player owns, when the item's vfunc 0x78 is true (and its id was over 954: patched, apply_patches.py). Logged once
// per change of the registry, with each decoder's name and vfunc 0x78, so the next test tells which condition still fails.
static const DWORD HW_ITEMBOX_REGISTRY = 0x2e2adc0, HW_ITEM_TABLE = 0x3f24638, HW_ITEM_TABLE_LOADED = 0x3f24628;
static const char* StdStringAt(unsigned char* s)
{
    unsigned size = *(unsigned*)(s + 0x10), cap = *(unsigned*)(s + 0x14);
    const char* p = cap > 15 ? *(const char**)s : (const char*)s;
    return size < 256 && Readable(p, size + 1) ? p : "?";
}
// appends to a fixed buffer (no C++ objects: the caller runs under __try)
static void Cat(char* out, size_t cap, const char* fmt, ...)
{
    size_t len = strlen(out);
    if (len + 1 >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + len, cap - len, fmt, ap);
    va_end(ap);
}
static int DecoderDiagRaw(char* out, size_t cap)
{
    __try
    {
        unsigned char* reg = (unsigned char*)(g_hwLo + HW_ITEMBOX_REGISTRY - HW_BASE);
        unsigned char** b = *(unsigned char***)(reg + 4);
        unsigned char** e = *(unsigned char***)(reg + 8);
        int n = b && e > b && e - b < 4096 ? (int)(e - b) : 0;
        for (int i = 0; i < n && i < 120; i++)
            if (Readable(b[i], 0x24))
                Cat(out, cap, "%s%s", i ? ", " : "", StdStringAt(b[i] + 4));
        unsigned char* loaded = (unsigned char*)(g_hwLo + HW_ITEM_TABLE_LOADED - HW_BASE);
        if (!n || (!*(int*)loaded && !*(int*)(loaded + 4)))
            return n;
        static const int ids[] = { 165,  173,  396,  533,  552,  555,  576,  774,  803,  806,  871,  872,  886,  955,  956,
                                   957,  958,  975,  976,  985,  2198, 2599, 8097, 8261, 8292, 8293, 8308, 8309, 8310, 8311,
                                   8312, 8453, 8616, 8617, 8898, 9053, 9084, 9116, 9144, 9184, 9330, 9633, 9706, 9707 };
        typedef void*(__thiscall * GetFn)(void*, int);
        typedef const char*(__thiscall * NameFn)(void*);
        typedef char(__thiscall * FlagFn)(void*);
        void* table = (void*)(g_hwLo + HW_ITEM_TABLE - HW_BASE);
        GetFn get = (GetFn)(*(void***)table)[0xc / 4];
        Cat(out, cap, " | decoders (id name vfunc78):");
        for (int id : ids)
        {
            void* item = get(table, id);
            if (!item)
            {
                Cat(out, cap, " %d -;", id);
                continue;
            }
            void** vt = *(void***)item;
            const char* name = ((NameFn)vt[0x74 / 4])(item);
            char flag = ((FlagFn)vt[0x78 / 4])(item);
            Cat(out, cap, " %d %s %d;", id, name && Readable(name, 1) ? name : "?", (int)flag);
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Cat(out, cap, " (exception)");
        return -1;
    }
}
static void DecoderDiag()
{
    static int last = 0, reports = 0;
    if (!g_hwLo)
        ModuleRange("hw.dll", g_hwLo, g_hwHi);
    if (!g_hwLo || reports >= 6)
        return;
    __try
    {
        unsigned char* reg = (unsigned char*)(g_hwLo + HW_ITEMBOX_REGISTRY - HW_BASE);
        unsigned char** b = *(unsigned char***)(reg + 4);
        unsigned char** e = *(unsigned char***)(reg + 8);
        int n = b && e > b ? (int)(e - b) : 0;
        if (n == last)
            return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    static char text[6000];
    text[0] = 0;
    int n = DecoderDiagRaw(text, sizeof(text));
    last = n;
    reports++;
    Log("decoder window: %d item boxes registered: %s", n, text);
}

// ---------------------------------------------------------------- the player's field of view and frame rate
// botassist.ini [Display] (the launcher's More > Field of view / Uncapped frame rate): Fov -> default_fov (the client's
// field of view when the server sets none), FpsMax -> fps_max (the game's is 101; it raises anything under 60 to 60).
// 0 = the game's own. Typed into hw.dll's command buffer (Cbuf_AddText 0x23d8710) a while after the game started and
// again when a match's server starts (its configs may set them back). The console takes them too.
static void ApplyDisplay()
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sbotassist.ini", g_dir);
    int fov = GetPrivateProfileIntA("Display", "Fov", 0, ini);
    int fps = GetPrivateProfileIntA("Display", "FpsMax", 0, ini);
    HMODULE hw = GetModuleHandleA("hw.dll");
    if (!hw || (fov <= 0 && fps <= 0))
        return;
    typedef void (*CbufAddTextFn)(const char* text);
    auto addText = (CbufAddTextFn)((DWORD)hw - 0x1D00000 + 0x23D8710);
    char cmd[96] = "";
    if (fov >= 60 && fov <= 150)
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), "default_fov %d\n", fov);
    if (fps >= 60 && fps <= 1000)
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), "fps_max %d\n", fps);
    if (!*cmd)
        return;
    __try { addText(cmd); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    Log("display: field of view %d, fps_max %d (botassist.ini [Display])", fov, fps);
}

// The frame rate stuck at 64-65 for many players, uncapped or not (reports 2026-11-25/26): the engine waits between
// frames with Sleep, and Sleep lasts at least one tick of the system timer, 15.6 ms (64 per second) unless the
// process asks for 1 ms ticks. Windows 10 2004 and later honour that per process only, and Windows 11 drops it for a
// process whose window isn't visible unless it opts out (ProcessPowerThrottling, IGNORE_TIMER_RESOLUTION).
static void AskForFineTimer()
{
    HMODULE winmm = LoadLibraryA("winmm.dll");
    typedef UINT(WINAPI * TimeBeginPeriodFn)(UINT);
    auto begin = winmm ? (TimeBeginPeriodFn)GetProcAddress(winmm, "timeBeginPeriod") : nullptr;
    UINT r = begin ? begin(1) : (UINT)-1;
    struct
    {
        ULONG Version, ControlMask, StateMask;
    } throttling = { 1, 0x4 /* PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION */, 0 /* off: keep honouring it */ };
    typedef BOOL(WINAPI * SetProcessInformationFn)(HANDLE, int, void*, DWORD);
    auto setInfo = (SetProcessInformationFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    BOOL t = setInfo ? setInfo(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &throttling, sizeof(throttling)) : FALSE;
    Log("timer: 1 ms ticks %s%s (the frame rate is no longer held at 64)", r == 0 ? "on" : "NOT on",
        t ? ", kept while the window is hidden" : "");
}

static DWORD WINAPI Watcher(LPVOID)
{
    AskForFineTimer();
    HMODULE hooked = NULL;
    HMODULE uiHooked = NULL;
    HMODULE clientFixed = NULL;
    DWORD uiSince = 0;
    bool displayDone = false;
    InstallGameCrashWatch(); // crash_watch.inl: the game's crashes and hangs, for problem reports
    for (;;)
    {
        CrashWatchTick();
        if (!displayDone && GetModuleHandleA("GameUI.dll"))
        {
            if (!uiSince)
                uiSince = GetTickCount();
            else if (GetTickCount() - uiSince > 15000)
            {
                ApplyDisplay();
                displayDone = true;
            }
        }
        InstallModelAliases(); // weapons this client has no model for
        HMODULE client = GetModuleHandleA("client.dll");
        if (client && client != clientFixed)
        {
            clientFixed = client;
            FixScoreboard(client);
        }
        HMODULE ui = GetModuleHandleA("GameUI.dll");
        if (ui && uiHooked == ui && cfg.enabled)
            HookShopPage(); // (its vtable may be set after GameUI loads: tried until it's hooked)
        if (ui && ui != uiHooked && cfg.enabled)
        {
            uiHooked = ui;
            HookAiSettingsButton(ui);
            HookExtraSettingsButton();
            HookViewBenefitsButton();
            GuardGameUiImages(ui);
            GuardGameUiRandomMap(ui);
            HookTaskbarButton(GAMEUI_VIP_PAGE, GAMEUI_VIP_RETURN, (void*)VipStub, &g_vipReturn, "VIP");
            HookTaskbarButton(GAMEUI_QUEST_PAGE, GAMEUI_QUEST_RETURN, (void*)MissionsStub, &g_questReturn, "Missions");
            HookModeDialog();
            InstallZsrMonsterRates();
            FixRiftTooltip(ui);
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
            ApplyDisplay(); // a match's server: its configs may have set them back
        }
        else if (!mp)
            hooked = NULL;
        if (mp && mp == hooked)
        {
            CheckClock();
            FixBuyMenuPrices(); // gun_fixes.inl: the buy menu shows the new prices
        }
        if (cfg.enabled)
            DecoderDiag();
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
