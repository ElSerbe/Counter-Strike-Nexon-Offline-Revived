// ---------------------------------------------------------------- weapon balance (zombie modes, the room's option)
// Some weapons do many times the damage of others of their type. With the room's weapon balance on (the host: AI
// Players page or /balance in the room's chat; off by default; CSNZ_Server says it in /api/matchbots only for the
// modes it applies to), the damage a human's weapon does to a zombie is scaled so that weapon's damage per second
// comes near the top of its group (botassist_balance.txt: its type, and fast or slow firing; WB_TARGET of the group
// does less): the weak ones do more, only the strongest less, between WeaponBalanceMin and WeaponBalanceMax times.
// Zombies with a lot of health (Zombie War: 22500-37500) take more besides: health / WB_HP_REF times, at most
// WB_HP_MAX. Off, nothing changes. (Until 2026-11-20 the target was the group's median, x0.25-2 and no health factor:
// the user saw zombies nearly immortal, 200-400 damage a second against 30000 health.)
//
// Where: every player's TakeDamage (CBasePlayer 0x114d7030 and the bots' overrides, vtable slot 0x44, thiscall
// inflictor, attacker entvars, damage, bits, one more; ret 0x14) and in Zombie Scenario, Zombie Shelter and Zombie
// Disaster Zone the zombie monsters' (same slot): each class's vtable is hooked when one of its entities is seen
// (g_wbOrig keeps the game's function per vtable). Only hits from a human player on a zombie (player +0x2166) or such
// a monster; grenades and mode weapons (hero's, Shelter's...: not in the table) are left alone.
//
// The damage per second of each weapon starts as a guess from the shop's numbers (gen_weapon_balance.py) and is
// measured in every zombie match, balance on or off, from the damage before scaling: a run of hits by one player with
// one weapon (gaps under WB_GAP s), the first moment's hits left out, over the run's length. The totals are kept in
// botassist_weapon_dps.txt. The guesses are first scaled to the measured numbers (the median ratio), then each
// weapon's estimate is its measured seconds plus WB_PRIOR seconds of its guess. Until a weapon has WB_SURE seconds
// measured its scale stays between WB_SOFT_MIN and WB_SOFT_MAX (the guesses can be far off).
static const DWORD MP_PLAYER_TAKEDAMAGE = 0x114d7030;
static const int VT_TAKEDAMAGE = 0x44 / 4;
static const float WB_TARGET = 0.85f, WB_HP_REF = 10000.0f, WB_HP_MAX = 3.0f;
static const int WB_EV_MAX_HEALTH = 0x1dc;
static const float WB_GAP = 2.5f, WB_PRIOR = 10.0f, WB_SURE = 30.0f, WB_SOFT_MIN = 0.75f, WB_SOFT_MAX = 2.0f,
                   WB_KEEP = 600.0f; // seconds of measures kept per weapon (older ones fade)
static const int MODE_ZOMBIE_SCENARIO_WB = 15, MODE_ZOMBIE_SHELTER_WB = 28, MODE_ZSRIFT_WB = 59;

struct WbWeapon
{
    int group = 0;
    float guess = 0;     // damage per second, from the shop (0: none)
    double dmg = 0, secs = 0; // measured
    float scale = 1;
    bool logged = false;
};
struct WbRun
{
    std::string key;
    float start = -100, last = -100;
    double dmg = 0;
};
static std::unordered_map<std::string, WbWeapon> g_wb;
static std::unordered_map<void*, TakeDamageFn> g_wbOrig; // vtable -> its TakeDamage
static WbRun g_wbRun[65];
static volatile LONG g_wbOn = 0; // this match: the room's option (FetchMatchBots)
static bool g_wbDirty = false, g_wbLoaded = false, g_wbNeedCalc = true;
static float g_wbNextHook = 0, g_wbNextSave = 0, g_wbNextCalc = 0;
static DWORD g_wbMpLo = 0, g_wbMpHi = 0;

static void WbSetMatch(int on)
{
    InterlockedExchange(&g_wbOn, on ? 1 : 0);
    Log("weapon balance: %s for this match", on ? "ON (weapon damage against zombies evened out by type)" : "off");
}

static std::string WbFile(const char* name)
{
    return std::string(g_dir) + name;
}

static void WbLoad()
{
    g_wbLoaded = true;
    g_wb.clear();
    if (FILE* f = fopen(WbFile("botassist_balance.txt").c_str(), "r"))
    {
        char line[256], key[128];
        int group;
        float guess;
        while (fgets(line, sizeof(line), f))
            if (line[0] != ';' && sscanf(line, "%127s %d %f", key, &group, &guess) == 3)
            {
                WbWeapon& w = g_wb[key];
                w.group = group;
                w.guess = guess;
            }
        fclose(f);
    }
    int measured = 0;
    if (FILE* f = fopen(WbFile("botassist_weapon_dps.txt").c_str(), "r"))
    {
        char line[256], key[128];
        double dmg, secs;
        while (fgets(line, sizeof(line), f))
            if (line[0] != ';' && sscanf(line, "%127s %lf %lf", key, &dmg, &secs) == 3 && secs > 0)
            {
                WbWeapon& w = g_wb[key];
                w.dmg = dmg;
                w.secs = secs;
                measured++;
            }
        fclose(f);
    }
    Log("weapon balance: %d weapons in botassist_balance.txt, %d measured before", (int)g_wb.size(), measured);
}

static void WbSave()
{
    g_wbDirty = false;
    std::vector<std::pair<std::string, const WbWeapon*>> list;
    for (auto& kv : g_wb)
        if (kv.second.secs > 0)
            list.push_back({ kv.first, &kv.second });
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first < b.first; });
    FILE* f = fopen(WbFile("botassist_weapon_dps.txt").c_str(), "w");
    if (!f)
        return;
    fprintf(f, "; weapon balance: damage to zombies measured per weapon (before scaling): <class> <damage> <seconds>\n"
               "; written by botassist; delete it to measure again from scratch\n");
    for (auto& e : list)
        fprintf(f, "%s %.0f %.2f\n", e.first.c_str(), e.second->dmg, e.second->secs);
    fclose(f);
    // with the balance on: the scales now of the most used weapons (the first-use lines come before any measure)
    static int reports = 0;
    if (!g_wbOn || reports++ >= 30)
        return;
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.second->secs > b.second->secs; });
    std::string s;
    char b[96];
    for (size_t i = 0; i < list.size() && i < 12; i++)
    {
        const WbWeapon& w = *list[i].second;
        snprintf(b, sizeof(b), "%s%s x%.2f (%.0f/s, %.0f s)", i ? ", " : "", list[i].first.c_str() + 7, w.scale,
                 w.dmg / w.secs, w.secs);
        s += b;
    }
    Log("weapon balance now: %s", s.c_str());
}

static float WbMedian(std::vector<float>& v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

// every weapon's scale from the guesses and the measures
static void WbCalc()
{
    g_wbNeedCalc = false;
    std::vector<float> ratios;
    for (auto& kv : g_wb)
        if (kv.second.guess > 0 && kv.second.secs >= WB_SURE / 3)
            ratios.push_back((float)(kv.second.dmg / kv.second.secs) / kv.second.guess);
    float k = ratios.size() >= 5 ? WbMedian(ratios) : 1.0f; // the guesses in measured numbers
    std::unordered_map<int, std::vector<float>> groups;
    std::unordered_map<std::string, float> est;
    for (auto& kv : g_wb)
    {
        const WbWeapon& w = kv.second;
        float e = 0;
        if (w.guess > 0)
            e = (float)((w.guess * k * WB_PRIOR + w.dmg) / (WB_PRIOR + w.secs));
        else if (w.secs >= WB_SURE / 3)
            e = (float)(w.dmg / w.secs);
        if (e > 0 && w.group)
        {
            est[kv.first] = e;
            groups[w.group].push_back(e);
        }
    }
    std::unordered_map<int, float> target;
    for (auto& g : groups)
    {
        std::vector<float>& v = g.second;
        std::sort(v.begin(), v.end());
        target[g.first] = v[(std::min)(v.size() - 1, (size_t)(v.size() * WB_TARGET))];
    }
    for (auto& kv : g_wb)
    {
        WbWeapon& w = kv.second;
        auto e = est.find(kv.first);
        float t = w.group ? target[w.group] : 0;
        if (e == est.end() || t <= 0)
        {
            w.scale = 1;
            continue;
        }
        float sure = (float)(std::min)(1.0, w.secs / WB_SURE);
        float lo = WB_SOFT_MIN + (cfg.wbMin - WB_SOFT_MIN) * sure, hi = WB_SOFT_MAX + (cfg.wbMax - WB_SOFT_MAX) * sure;
        w.scale = (std::max)(lo, (std::min)(hi, t / e->second));
    }
}

static bool WbZombieMonsterMode()
{
    int m = GameMode();
    return m == MODE_ZOMBIE_SCENARIO_WB || m == MODE_ZOMBIE_SHELTER_WB || m == MODE_ZSRIFT_WB;
}

static bool WbInMp(const void* p)
{
    return (DWORD)p >= g_wbMpLo && (DWORD)p < g_wbMpHi;
}

// the class name of the weapon in a player's hand ("" when none)
static std::string WbHeld(unsigned char* player)
{
    unsigned char* it = ItemHandle(player + PLAYER_ACTIVE_ITEM);
    unsigned char* pev = it && Readable(it + 8, 4) ? *(unsigned char**)(it + 8) : nullptr;
    const char* cls = pev && Readable(pev, 4) ? String(*(int*)pev) : nullptr;
    std::string s = cls ? cls : "";
    for (auto& ch : s)
        ch = (char)tolower(ch);
    return s;
}

// the damage of one hit, measured and (balance on) scaled
static float WbDamage(void* self, void* inflictor, void* attacker, float damage)
{
    if (damage <= 0 || !attacker || !Readable(attacker, EV_CONTAINING_ENTITY + 4))
        return damage;
    unsigned char* victim = (unsigned char*)self;
    unsigned char* vpev = Readable(victim + 8, 4) ? *(unsigned char**)(victim + 8) : nullptr;
    if (!vpev || !Readable(vpev, EV_CONTAINING_ENTITY + 4))
        return damage;
    int vflags = *(int*)(vpev + EV_FLAGS);
    bool zombie = (vflags & FL_CLIENT) ? Readable(victim + 0x2166, 1) && victim[0x2166]
                                       : (vflags & FL_MONSTER) && WbZombieMonsterMode();
    if (!zombie)
        return damage;
    void* aed = *(void**)((unsigned char*)attacker + EV_CONTAINING_ENTITY);
    unsigned char* ap = aed ? (unsigned char*)PrivateOf(aed) : nullptr;
    if (!ap || !(F<int>(Vars(aed), EV_FLAGS) & FL_CLIENT) || !Readable(ap, 0x2170) || ap[0x2166])
        return damage; // not a human player's
    if (inflictor && inflictor != attacker && Readable(inflictor, 4))
    {
        const char* icls = String(*(int*)inflictor);
        if (icls && strstr(icls, "grenade"))
            return damage; // a grenade (the hand may hold anything by then)
    }
    std::string key = WbHeld(ap);
    auto it = g_wb.find(key);
    if (key.empty() || it == g_wb.end() || !it->second.group)
        return damage; // a mode weapon: never balanced, not measured

    // the measure: runs of hits
    int idx = g_IndexOfEdict ? g_IndexOfEdict(aed) : 0;
    float now = Now();
    if (idx > 0 && idx < 65)
    {
        WbRun& r = g_wbRun[idx];
        if (r.key == key && now - r.last <= WB_GAP && now >= r.start)
        {
            if (now > r.start + 0.001f)
                r.dmg += damage;
            r.last = now;
        }
        else
        {
            if (!r.key.empty() && r.last - r.start >= 0.5f && r.dmg > 0) // shorter runs: too noisy (12910/s over 0.3 s seen)
            {
                WbWeapon& w = g_wb[r.key];
                w.dmg += r.dmg;
                w.secs += r.last - r.start;
                if (w.secs > WB_KEEP)
                {
                    w.dmg *= WB_KEEP / w.secs;
                    w.secs = WB_KEEP;
                }
                g_wbDirty = g_wbNeedCalc = true;
            }
            r.key = key;
            r.start = r.last = now;
            r.dmg = 0;
        }
    }

    if (!g_wbOn)
        return damage;
    WbWeapon& w = it->second;
    float maxHp = Readable(vpev + WB_EV_MAX_HEALTH, 4) ? *(float*)(vpev + WB_EV_MAX_HEALTH) : 0;
    float hpScale = (std::max)(1.0f, (std::min)(WB_HP_MAX, maxHp / WB_HP_REF));
    if (!w.logged)
    {
        w.logged = true;
        static int reports = 0;
        if (reports++ < 60)
            Log("weapon balance: %s does x%.2f to zombies (group %d, %.0f s measured), x%.2f more on this %.0f-health zombie",
                key.c_str(), w.scale, w.group, w.secs, hpScale, maxHp);
    }
    return damage * w.scale * hpScale;
}

static float SafeWbDamage(void* self, void* inflictor, void* attacker, float damage)
{
    __try
    {
        return WbDamage(self, inflictor, attacker, damage);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int faults = 0;
        if (faults++ < 5)
            Log("weapon balance: exception 0x%08lX (damage left as it was)", GetExceptionCode());
        return damage;
    }
}

static void SafeGdmOnDamage(void* victim, void* attacker, float damage); // modes/gun_deathmatch.inl: hits per weapon

static int __fastcall WbTakeDamage(void* self, void* edx, void* inflictor, void* attacker, float damage, int bits, int extra)
{
    auto it = g_wbOrig.find(*(void**)self);
    if (it == g_wbOrig.end())
        return 0; // can't happen: only hooked vtables get here
    SafeGdmOnDamage(self, attacker, damage);
    return it->second(self, edx, inflictor, attacker, SafeWbDamage(self, inflictor, attacker, damage), bits, extra);
}

static void WbHookVtable(void* entityPrivate)
{
    void** vt = *(void***)entityPrivate;
    if (!WbInMp(vt) || g_wbOrig.count(vt) || !Readable(vt + VT_TAKEDAMAGE, 4))
        return;
    void* fn = vt[VT_TAKEDAMAGE];
    if (fn == (void*)WbTakeDamage || !WbInMp(fn))
        return;
    g_wbOrig[vt] = (TakeDamageFn)fn;
    DWORD old;
    VirtualProtect(vt + VT_TAKEDAMAGE, 4, PAGE_READWRITE, &old);
    vt[VT_TAKEDAMAGE] = (void*)WbTakeDamage;
    VirtualProtect(vt + VT_TAKEDAMAGE, 4, old, &old);
    Log("weapon balance: TakeDamage of vtable 0x%08lX hooked (%s)", (DWORD)vt - g_mpDelta,
        fn == (void*)(MP_PLAYER_TAKEDAMAGE + g_mpDelta) ? "players" : "its own");
}

// every frame (the frame hook): new classes hooked, the scales worked out again, the measures saved
static void WbFrame()
{
    if (!g_FindEntityInSphere)
        return;
    if (!g_wbMpHi)
    {
        HMODULE mp = GetModuleHandleA("mp.dll");
        if (!mp)
            return;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((unsigned char*)mp + ((IMAGE_DOS_HEADER*)mp)->e_lfanew);
        g_wbMpLo = (DWORD)mp;
        g_wbMpHi = (DWORD)mp + nt->OptionalHeader.SizeOfImage;
    }
    if (!g_wbLoaded)
        WbLoad();
    float now = Now();
    if (now < g_wbNextHook - 5.0f)
        g_wbNextHook = g_wbNextSave = g_wbNextCalc = 0; // a new map: the clock started again
    if (now >= g_wbNextHook)
    {
        g_wbNextHook = now + 2.0f;
        bool monsters = WbZombieMonsterMode();
        ForEntitiesNear(V3{ 0, 0, 0 }, 65536.0f, [&](void* e) {
            int flags = F<int>(Vars(e), EV_FLAGS);
            if (!((flags & FL_CLIENT) || (monsters && (flags & FL_MONSTER))))
                return;
            void* p = PrivateOf(e);
            if (p && Readable(p, 4))
                WbHookVtable(p);
        });
    }
    if (g_wbNeedCalc && now >= g_wbNextCalc)
    {
        g_wbNextCalc = now + 10.0f;
        WbCalc();
    }
    if (g_wbDirty && now >= g_wbNextSave)
    {
        g_wbNextSave = now + 60.0f;
        WbSave();
    }
}
