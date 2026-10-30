// botassist: Zombie Disaster Zone (mode 59, ZS Rift). Included by botassist.cpp at this point (one translation unit: it uses the shared helpers
// defined before it). Only runs in its own mode: ZSRiftWatch returns unless GameMode() == 59; the kill-money hook patches only the ZSRift vtable.

// Zombie Disaster Zone (mode 59): CSOGameRulesZSRift (vtable 0x1178504c, g_pGameRules = [0x121cac30]) and the
// object its code reads the round state from ([0x121d3ec4]: +0x27c freeze period, +0x27d, +0x214). Logged when
// they change: rules +0x21c phase (1 -> 2 when the vote ends), +0x224 vote phase, +0x23c / +0x240 vote times,
// +0x284 / +0x288, and how many monsters are alive
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

// A player who dies while a teammate lives becomes a spectator (0x114d5de0) until the rift's respawn time (player
// +0xf74): the rules' death think (vfunc 0x118, 0x10768180) turns dead flag 2 into 5 once it passes (not while the
// buy menu is open, player +0xe10 14) and respawns on the next one. Bots that died that way stayed spectators: one
// still dead 3 s after its respawn time goes through CBasePlayer::PlayerDeathThink (0x114c3cb0) from here.
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
        bool ok = CallPlayerDeathThink(p);
        if (ok && F<int>(v, EV_DEADFLAG) == 5)
            ok = CallPlayerDeathThink(p);
        Log("ZSRift: bot '%s' still dead after its respawn time (%s): death think%s -> %s", String(F<int>(v, EV_NETNAME)),
            before.c_str(), ok ? "" : " FAILED", ZSRiftDeadState(e).c_str());
    });
}

// The last round cleared: CSNZ_Server pays the room's players their Rift Tickets (POST /api/zsrift/clear, body
// {"difficulty": rift level, "rounds": rounds cleared}); a failed call is tried again from a new thread.
static volatile LONG g_zsrClearSending = 0;
static int g_zsrClearDifficulty = 0, g_zsrClearRounds = 0;
static DWORD WINAPI ReportZSRiftClear(LPVOID)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"difficulty\":%d,\"rounds\":%d}", g_zsrClearDifficulty, g_zsrClearRounds);
    for (int attempt = 0; attempt < 5; attempt++)
    {
        std::string reply;
        if (HttpPost(L"/api/zsrift/clear", body, &reply))
        {
            Log("ZSRift: rift cleared at level %d: told the server (%s)", g_zsrClearDifficulty, reply.substr(0, 200).c_str());
            InterlockedExchange(&g_zsrClearSending, 0);
            return 0;
        }
        Sleep(3000);
    }
    Log("ZSRift: rift cleared at level %d, but the server isn't reachable: no Rift Tickets", g_zsrClearDifficulty);
    InterlockedExchange(&g_zsrClearSending, 0);
    return 0;
}

static void ZSRiftWatch()
{
    static float next = 0;
    static std::string last;
    static void* clearsRules = nullptr; // the rules object the count below belongs to (a new one each map)
    static int clears = 0;              // rounds this match cleared (botassist clears every one of them)
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
        clears = 0;
        Log("ZSRift: the match didn't start by itself (room-match check 0x318 off): started it%s", ok ? "" : " - FAILED");
    }
    // clear the round once its kills reach the target
    unsigned char* zsm = *(unsigned char**)(cso + 0xc);
    int target = zsm && Readable(zsm, 0x1300) ? ZSRiftTarget(zsm) : -1;
    if (cso[0x27c] == 1 && !cso[0x27d] && !*(int*)(cso + 4) && *(DWORD*)r == 0x1178504C + g_mpDelta && target > 0 &&
        *(int*)(r + 0x284) >= target && !zsm[0x12c8])
    {
        bool ok = ZSRiftClearRound(zsm);
        if (r != clearsRules)
        {
            clearsRules = r;
            clears = 0;
        }
        clears += ok;
        int total = *(int*)(zsm + 0x290);
        Log("ZSRift: round %d cleared (%d of %d kills; %d rounds played of %d; %d cleared)%s", *(short*)(zsm + 0x288),
            *(int*)(r + 0x284), target, *(int*)(cso + 0x4cc), total, clears, ok ? "" : " - FAILED");
        // the boss round was the last one: the rift is cleared
        if (ok && total > 0 && clears == total && InterlockedCompareExchange(&g_zsrClearSending, 1, 0) == 0)
        {
            g_zsrClearDifficulty = v214;
            g_zsrClearRounds = clears;
            HANDLE t = CreateThread(nullptr, 0, ReportZSRiftClear, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
            else
                InterlockedExchange(&g_zsrClearSending, 0);
        }
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
