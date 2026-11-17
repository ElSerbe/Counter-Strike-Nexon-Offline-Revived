// botassist: Zombie Hero and its versions: 14 Zombie Hero (rules CSOGameRulesZombi3), 44 Season Zombie Hero
// (CSOGameRulesSeasonZombieHero), 45 Zombie Z (CSOGameRulesZombieZMode / ZombieZMutation) and 58 Zombie Hero Classic
// (CSOGameRulesZombi3Classic). Included by botassist.cpp at this point (one translation unit: it uses the shared
// helpers defined before it). Only acts in those modes: HeroMode(GameMode()).
//
// What the game does by itself for room bots (they are its zombie-mode bots, CSOZBBotManager / ZBBot):
//   - its behaviour tree has the hero's weapons (ZBBot_BT_BuyWeapon_HeroMode), Zombie Z's buying (BuyWeapon_ZMode)
//     and Zombie Z's mutations: ZBBot_BT_ZModeLevelUp (0x1154bda0) gains one in mode 45 ("Gain Mutation"), like the
//     player's V key (command ZBZGainMut);
//   - zombie skills (ZBBot_BT_Zombie_UsingSKill), zombie bombs, evolution, the morale boost.
// What botassist adds:
//   - zombie bots come back after dying and pick a random class among all of them, and use their skills: the same as
//     Zombie 2: Mutation (modes/zombie2_mutation.inl: ZB2Watch, ZB2BotThink), whose commands these rules keep: every
//     version's ClientCommand ends in Zombi2's (0x1144dcc0: "selzmb", "useitem zb_skill<n>"), Zombi3 0x11456a80,
//     Classic 0x10790a90, Season 0x108366e0, Zombie Z 0x10874930 / 0x107a5220 (through Zombi3's). Unless killed by a
//     headshot (or Concentrated Fire), a zombie respawns: the game's own death think decides (#CSO_ZB3_RespawnWait,
//     #CSO_ZB3_CannotRespawnByHeadShot in CBasePlayer's death code), botassist only runs it for bots;
//   - the hero keeps the mode's weapons (HeroIsHero): the room's loadout isn't bought again over them. The hero is
//     the player the rules gave a hero weapon: weapon_m134hero(2), weapon_dualkrisshero(2), weapon_herochainsaw. Its
//     health goes up when it's picked, which looked like a new life to the buying code, and a room loadout dropped the
//     hero's minigun to buy the room's rifle;
//   - a hero with the chainsaw (a melee weapon) doesn't back off from zombies (zombie_humans.inl): it goes at them.
static const int MODE_ZB3 = 14, MODE_ZB3_SEASON = 44, MODE_ZBZ = 45, MODE_ZB3_CLASSIC = 58;

static bool HeroMode(int m)
{
    return m == MODE_ZB3 || m == MODE_ZB3_SEASON || m == MODE_ZBZ || m == MODE_ZB3_CLASSIC;
}

// the hero weapon it carries ("" if none): the round's hero or heroine
static std::string HeroWeapon(void* bot)
{
    if (!HeroMode(GameMode()))
        return "";
    unsigned char* p = (unsigned char*)PrivateOf(bot);
    if (!p || !Readable(p, 0x2170) || p[0x2166])
        return ""; // a zombie
    for (int slot = 1; slot <= 4; slot++)
    {
        std::string c = SlotClass(bot, slot);
        for (auto& ch : c)
            ch = (char)tolower(ch);
        if (c.find("hero") != std::string::npos)
            return c;
    }
    return "";
}

static std::map<void*, bool> g_heroWas;

static bool HeroIsHero(void* bot)
{
    std::string w = HeroWeapon(bot);
    bool hero = !w.empty();
    bool& was = g_heroWas[bot];
    if (hero != was)
    {
        was = hero;
        static int reports = 0;
        if (reports++ < 60)
            Log("Zombie Hero: bot '%s' %s", String(F<int>(Vars(bot), EV_NETNAME)),
                hero ? ("is the hero (" + w + "): it keeps the mode's weapons").c_str() : "isn't the hero any more");
    }
    return hero;
}

// the hero with the chainsaw in hand: it must reach the zombies, not back off from them
static bool HeroWithChainsaw(void* bot)
{
    if (!HeroMode(GameMode()))
        return false;
    const char* m = String(F<int>(Vars(bot), EV_WEAPONMODEL));
    if (!m)
        return false;
    std::string held = m;
    for (auto& ch : held)
        ch = (char)tolower(ch);
    return held.find("herochainsaw") != std::string::npos;
}
