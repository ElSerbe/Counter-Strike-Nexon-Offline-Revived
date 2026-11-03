// botassist: Item Battle (mode 19; rules CSOGameRulesTDMItemRenew, vtable 0x117a54b4). Included by botassist.cpp at this point (one
// translation unit: it uses the shared helpers defined before it). Only runs in its own mode: ItemWatch returns, and DriveItem isn't
// called, unless GameMode() == MODE_ITEM.
//
// The mode (https://cso.fandom.com/wiki/Item_Battle): team deathmatch with respawns; boxes around the map give an item, kept in two slots
// (player +0x2f26 the one in hand, +0x2f27 the other, -1 none, else box id + 0x19), used with E. Box ids: 0 random, 1 Power AT4CS,
// 2 Power Ripper (behind a yellow barricade, entity fun_itemboxshield), 3 Nuclear, 4 Knockback, 5 Black hole, 6 Shield, 7 UV radar,
// 8 Invisible, 9 Teleport, 10 Rocket jump, 11 Suicide Pierrot, 12 Portal, 13 Energy beam, 14 Hurricane, 15 Missile. The boxes come from
// maps/FUN_ITEMBOX_<map>, a table only 6 maps have: gen_item_battle.py makes one for the others from their navigation meshes, and
// botassist_tables replaces the game's table.
//
// The game has no AI for any of this (bots here are stock CCSBots, the old bot manager), so:
//   - Teams: RoomBotQuota (shared, as in Super Soldier) evens the sides.
//   - DriveItem moves them (GdmPickVia/GdmWalk, Gun Deathmatch's routes): hunting, like any player here. A bot with no item goes to the
//     closest box first, one with an item only to one right next to it, and only 2 bots go for the same box. With no limit everybody went
//     to the same box, piled up around it and nobody fought. A bot that doesn't get a box in time leaves it for a minute, and a box 3
//     bots gave up on is left for all for 3 minutes (botassist.log: "box ... given up"). It breaks a barricade that covers one.
//     The stock AI keeps aiming and shooting.
//   - Items: like a player's E (rules vfunc 0x238, thiscall(player, button), and 0x23c when it's let go: the items with a target
//     are aimed while it is held). Attack items at a visible enemy, the shield, radar and invisibility when one shows up, teleport to
//     reach one far away, and rocket jump and portal when nothing is around, so the slot frees up.
//   - Weapons: botassist's room weapons would replace a Power AT4CS or Ripper (ItemHasSpecial keeps them, UpdateBuying and OnBotCommand).
static const DWORD MP_ITEM_RULES_VT = 0x117a54b4, MP_ITEM_STATES = 0x11e55518;
static const int ITEM_SLOT = 0x2f26, ITEM_SLOT2 = 0x2f27, ITEM_BOX_FIRST = 0x19, ITEM_PRESS_VFUNC = 0x238, ITEM_RELEASE_VFUNC = 0x23c, ITEM_IN_USE = 0x20;
static const int ITEM_WEAPON_SLOT = 0x1018, ITEM_WEAPON = 0x101c, ITEM_WEAPON_ID = 0xe4, ITEM_POWER_AT4 = 0x10e, ITEM_POWER_RIPPER = 0x10f, ITEM_EF_NODRAW = 0x80;
static const float ITEM_BOX_RANGE = 1500.0f, ITEM_FIGHT_RANGE = 700.0f, ITEM_USE_RANGE = 1400.0f, ITEM_USE_EVERY = 2.0f, ITEM_AIM_TIME = 0.3f, ITEM_SHIELD_RANGE = 600.0f;
static const float ITEM_IDLE_USE = 8.0f, ITEM_TELEPORT_FAR = 700.0f, ITEM_SELF_RANGE = 900.0f, ITEM_LOW_HEALTH = 50.0f;
static const float ITEM_BOX_CLOSE = 400.0f, ITEM_BOX_PICK = 0.5f, ITEM_BOX_SPEED = 180.0f, ITEM_BOX_MARGIN = 4.0f, ITEM_BOX_SKIP = 60.0f, ITEM_BOX_BAD = 180.0f;
static const int ITEM_BOX_BOTS = 2, ITEM_BOX_FAILS = 3;

static unsigned char* ItemRules()
{
    if (!g_mpDelta || GameMode() != MODE_ITEM)
        return nullptr;
    unsigned char* rules = *(unsigned char**)(MP_GAMERULES + g_mpDelta);
    return rules && Readable(rules, 0x2ac) && *(DWORD*)rules == MP_ITEM_RULES_VT + g_mpDelta ? rules : nullptr;
}

static bool ItemHasSpecial(void* bot)
{
    if (!ItemRules())
        return false;
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2f30) || !*(int*)(p + ITEM_WEAPON_SLOT))
        return false;
    unsigned char* weapon = *(unsigned char**)(p + ITEM_WEAPON);
    if (!weapon || !Readable(weapon, 0xe8))
        return false;
    int id = *(int*)(weapon + ITEM_WEAPON_ID);
    return id == ITEM_POWER_AT4 || id == ITEM_POWER_RIPPER;
}

// the player's item state: +0 aiming one, +0x10 can use items
static unsigned char* ItemState(void* edict)
{
    int index = g_IndexOfEdict ? g_IndexOfEdict(edict) : 0;
    unsigned char** states = *(unsigned char***)(MP_ITEM_STATES + g_mpDelta);
    if (index < 1 || !states || !Readable(states + index - 1, 4))
        return nullptr;
    unsigned char* s = states[index - 1];
    return s && Readable(s, 0x28) ? s : nullptr;
}

struct ItemBot
{
    float lastAlive = 0, useAt = 0, releaseAt = 0, fireAt = 0, quietSince = 0, boxPickAt = 0, boxUntil = 0;
    bool holding = false;
    void* box = nullptr;
    void* shield = nullptr;
    std::map<void*, float> skip; // boxes it gave up on, until when
};
static std::map<void*, ItemBot> g_itemBots;
static std::map<void*, int> g_itemBoxFails;
static std::map<void*, float> g_itemBoxBad; // boxes nobody should go for, until when

// a box is ready when it shows
static bool ItemBoxReady(void* e)
{
    const char* name = e ? ClassName(e) : nullptr;
    return name && !strcmp(name, "fun_itembox") && !(F<int>(Vars(e), EV_EFFECTS) & ITEM_EF_NODRAW);
}

// Which box the bot goes for, twice a second
static void ItemPickBox(void* bot, unsigned char* p, ItemBot& b, V3 me, float now)
{
    if (b.box && !ItemBoxReady(b.box))
        b.box = nullptr; // somebody took it
    b.shield = nullptr;
    bool room = *(signed char*)(p + ITEM_SLOT2) == -1, empty = *(signed char*)(p + ITEM_SLOT) == -1;
    float reach = !room ? 0.0f : empty ? ITEM_BOX_RANGE : ITEM_BOX_CLOSE;
    float best = reach, bestShield = ITEM_SHIELD_RANGE;
    void* box = nullptr;
    std::map<void*, int> claims;
    for (auto& kv : g_itemBots)
        if (kv.first != bot && kv.second.box)
            claims[kv.second.box]++;
    ForEntitiesNear(me, ITEM_BOX_RANGE, [&](void* e) {
        const char* name = ClassName(e);
        if (!name || strncmp(name, "fun_itembox", 11) || (F<int>(Vars(e), EV_EFFECTS) & ITEM_EF_NODRAW))
            return;
        float there = Len(Origin(e) - me);
        if (name[11] != 0)
        {
            if (there < bestShield)
            {
                bestShield = there;
                b.shield = e;
            }
            return;
        }
        auto skipped = b.skip.find(e);
        auto bad = g_itemBoxBad.find(e);
        if (there < best && claims[e] < ITEM_BOX_BOTS && (skipped == b.skip.end() || now >= skipped->second) &&
            (bad == g_itemBoxBad.end() || now >= bad->second))
        {
            best = there;
            box = e;
        }
    });
    if (!b.box && box)
    {
        b.box = box;
        b.boxUntil = now + best / ITEM_BOX_SPEED + ITEM_BOX_MARGIN;
    }
}

// the bot didn't get there: it leaves that box, and if others failed too, so does everybody
static void ItemGiveUpBox(ItemBot& b, float now)
{
    if (!b.box)
        return;
    b.skip[b.box] = now + ITEM_BOX_SKIP;
    if (++g_itemBoxFails[b.box] >= ITEM_BOX_FAILS)
    {
        g_itemBoxFails[b.box] = 0;
        g_itemBoxBad[b.box] = now + ITEM_BOX_BAD;
        V3 at = Origin(b.box);
        static int reports = 0;
        if (reports++ < 30)
            Log("Item Battle: box at %.0f %.0f %.0f given up: bots don't get to it", at.x, at.y, at.z);
    }
    b.box = nullptr;
}

typedef void(__fastcall* ItemKeyFn)(void* rules, void* edx, void* player, int button);

// E on the item in hand: at an enemy, or for itself. Returns whether it looks at the enemy for it
static bool ItemUse(void* bot, unsigned char* rules, unsigned char* p, ItemBot& b, GdmBot& z, void* enemy, bool seen, float d, float now, float& yaw, float& pitch)
{
    if (b.holding)
    {
        if (seen)
        {
            yaw = YawOf(Origin(enemy) - Eye(bot));
            pitch = PitchOf(Origin(enemy) - Eye(bot));
        }
        if (now >= b.releaseAt)
        {
            b.holding = false;
            ((ItemKeyFn)(*(void***)rules)[ITEM_RELEASE_VFUNC / 4])(rules, nullptr, p, ITEM_IN_USE);
        }
        return seen;
    }
    signed char held = *(signed char*)(p + ITEM_SLOT);
    unsigned char* state = ItemState(bot);
    if (held == -1 || !state || !state[0x10] || now < b.useAt)
        return false;
    int kind = held - ITEM_BOX_FIRST;
    bool attack = kind == 3 || kind == 4 || kind == 5 || kind == 11 || kind == 13 || kind == 14 || kind == 15;
    bool self = kind == 6 || kind == 7 || kind == 8;
    bool use = false, aim = false;
    if (attack)
        use = aim = seen && d < ITEM_USE_RANGE && d > 150.0f;
    else if (self)
        use = (seen && d < ITEM_SELF_RANGE) || F<float>(Vars(bot), EV_HEALTH) < ITEM_LOW_HEALTH;
    else if (kind == 9)
        use = aim = seen && d > ITEM_TELEPORT_FAR;
    else
        use = !seen && now - b.quietSince > ITEM_IDLE_USE; // rocket jump, portal, anything else: to free the slot
    if (!use)
        return false;
    b.useAt = now + ITEM_USE_EVERY;
    if (aim)
    {
        yaw = YawOf(Origin(enemy) - Eye(bot));
        pitch = PitchOf(Origin(enemy) - Eye(bot));
        // the engine takes its view from v_angle: the hook writes this frame's aim after this; a targeted item goes off when let go
    }
    b.holding = true;
    b.releaseAt = now + ITEM_AIM_TIME;
    ((ItemKeyFn)(*(void***)rules)[ITEM_PRESS_VFUNC / 4])(rules, nullptr, p, ITEM_IN_USE);
    static int reports = 0;
    if (reports++ < 80)
        Log("Item Battle: bot '%s' uses item %d (enemy %s at %.0f)", String(F<int>(Vars(bot), EV_NETNAME)), kind, seen ? "in sight" : "away", d);
    return aim;
}

static bool DriveItem(void* bot, BotState& st, float& yaw, float& pitch, float& fwd, float& side, unsigned short& buttons)
{
    if (st.ordered && st.order != ORDER_INDEPENDENT)
        return false;
    unsigned char* rules = ItemRules();
    unsigned char* p = rules ? (unsigned char*)PrivateOf(bot) : nullptr;
    if (!p || !Readable(p, 0x2f30) || !g_nav.ok)
        return false;
    GdmBot& z = g_gdmBots[bot]; // there: GdmPickTarget and GdmPickVia read it to spread the team out
    ItemBot& b = g_itemBots[bot];
    float now = Now();
    V3 me = Origin(bot), eye = Eye(bot);
    int team = *(int*)(p + 0x6c);
    z.team = team;
    if (F<float>(Vars(bot), EV_MAXSPEED) <= 1.0f || (F<int>(Vars(bot), EV_FLAGS) & FL_FROZEN))
    {
        st.lastPos = me;
        st.lastProgress = now;
        return false;
    }
    if (now - b.lastAlive > 1.0f)
    {
        z.hasVia = false;
        z.viaAt = now + (rand() % 3 ? 0.0f : 6.0f);
        b.holding = false;
        b.quietSince = now;
        b.box = nullptr;
    }
    b.lastAlive = now;
    if (now >= z.retargetAt || !z.target)
    {
        z.retargetAt = now + GDM_RETARGET;
        GdmPickTarget(bot, z, me, eye, team);
    }
    if (z.target && (!(F<int>(Vars(z.target), EV_FLAGS) & FL_CLIENT) || !Alive(z.target)))
        z.target = nullptr;
    bool seen = false;
    float d = 1e9f;
    V3 at = me;
    if (z.target)
    {
        at = Origin(z.target);
        d = Len(at - me);
        seen = CanSee(bot, eye, z.target);
        if (seen)
        {
            z.lastSeen = now;
            b.quietSince = now;
        }
    }
    bool aimed = ItemUse(bot, rules, p, b, z, z.target, seen, d, now, yaw, pitch);
    V3 goal = at;
    bool go = z.target != nullptr;
    if (seen && d < ITEM_FIGHT_RANGE)
    {
        if (d < GDM_CLOSE)
        {
            if (now >= z.strafeAt)
            {
                z.strafeAt = now + GDM_STRAFE * (0.6f + Rnd() * 0.8f);
                z.strafe = rand() % 2 ? 1 : -1;
            }
            float around = YawOf(at - me) + z.strafe * (d < GDM_TOO_CLOSE ? 120.0f : 70.0f);
            goal = me + V3{ cosf(around / RAD), sinf(around / RAD), 0 } * 120.0f;
        }
    }
    else
    {
        // nothing close to fight: a box if it should get one (ItemPickBox), else hunting; a barricade on the way gets shot
        if (now >= b.boxPickAt)
        {
            b.boxPickAt = now + ITEM_BOX_PICK;
            ItemPickBox(bot, p, b, me, now);
        }
        if (b.box && now >= b.boxUntil)
            ItemGiveUpBox(b, now);
        void* shield = b.shield;
        if (b.box)
        {
            goal = Origin(b.box);
            go = true;
        }
        else if (z.target)
        {
            if (!z.hasVia && now >= z.viaAt)
                GdmPickVia(bot, z, me, at);
            if (z.hasVia && (Len2D(z.via - me) < GDM_VIA_REACHED || now >= z.viaUntil))
            {
                z.hasVia = false;
                z.viaAt = now + 3.0f + Rnd() * 7.0f;
            }
            if (z.hasVia)
                goal = z.via;
        }
        if (shield && !seen && !aimed)
        {
            V3 center = Origin(shield);
            TraceResult tr;
            if (Trace(eye, center, bot, 1, tr) || tr.pHit == shield)
            {
                yaw = YawOf(center - eye);
                pitch = PitchOf(center - eye);
                aimed = true;
                if (now >= b.fireAt)
                {
                    buttons |= IN_ATTACK;
                    if (now >= b.fireAt + 1.2f)
                        b.fireAt = now + 0.15f;
                }
            }
        }
    }
    if (!go)
        return aimed; // nothing to move for: only its look
    V3 wp;
    if (!NextWaypoint(bot, st, me, goal, 32.0f, wp))
        wp = goal;
    float wayYaw = YawOf(wp - me), maxDrop = ZCON_FREE_DROP;
    if (me.z - 36.0f - wp.z + 32.0f > maxDrop)
        maxDrop = min(me.z - 36.0f - wp.z + 32.0f, HURT_DROP_MAX);
    bool setView = false;
    float view = yaw;
    if (aimed)
        z.lastSeen = now; // it keeps looking at what it aims at
    bool walked = GdmWalk(bot, st, z, me, now, wayYaw, wayYaw, maxDrop, view, setView, fwd, side, buttons);
    if (setView)
    {
        yaw = view;
        pitch = 0;
    }
    if (z.stuckNow && z.stuck >= 3)
        ItemGiveUpBox(b, now);
    if (z.stuckNow && z.stuck >= 4)
    {
        z.hasVia = false;
        z.target = nullptr;
        static int reports = 0;
        if (reports++ < 30)
            Log("Item Battle: '%s' stuck at %.0f %.0f %.0f going to %.0f %.0f %.0f", String(F<int>(Vars(bot), EV_NETNAME)), me.x, me.y, me.z, goal.x, goal.y, goal.z);
    }
    return walked;
}

static void ItemWatch()
{
    static float next = 0;
    if (!ItemRules() || !g_pGlobals || !*g_pGlobals || g_edictToVars < 0)
        return;
    float now = Now();
    if (now >= next - 60.0f && now < next)
        return;
    next = now + 15.0f;
    static int reports = 0;
    if (reports >= 100)
        return;
    reports++;
    int alive[3] = { 0, 0, 0 }, dead = 0, boxes = 0, ready = 0, holding = 0;
    ForEntitiesNear(V3{ 0, 0, 0 }, 1e6f, [&](void* e) {
        const char* name = ClassName(e);
        if (name && !strcmp(name, "fun_itembox"))
        {
            boxes++;
            ready += (F<int>(Vars(e), EV_EFFECTS) & ITEM_EF_NODRAW) ? 0 : 1;
        }
        if (!(F<int>(Vars(e), EV_FLAGS) & FL_CLIENT))
            return;
        int team = TeamOf(e);
        if (team != 1 && team != 2)
            return;
        if (!Alive(e))
            dead++;
        else
            alive[team]++;
        unsigned char* q = (unsigned char*)PrivateOf(e);
        if (q && Readable(q, 0x2f30) && *(signed char*)(q + ITEM_SLOT) != -1)
            holding++;
    });
    Log("Item Battle: %d T and %d CT alive, %d dead; %d of %d boxes ready, %d players with an item", alive[1], alive[2], dead, ready, boxes, holding);
}
