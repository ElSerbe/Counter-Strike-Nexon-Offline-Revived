# botassist

Companion AI for the game's bots where there are monsters and NPCs (zombie scenario zombies, human scenario
soldiers, zombie shelter...). The stock bot AI (CCSBot) only fights enemy players and ignores radio orders there.

## How it works

Every bot sends its view angles, movement and buttons to the engine through `enginefuncs_t::pfnRunPlayerMove`,
called only from `CBot::ExecuteCommand` in mp.dll. botassist replaces that pointer in mp.dll's copy of the engine
table. While monsters are on the map it drives the bots itself (elsewhere they play normally):

- path finding: reads `maps/<map>.nav` (CS:CZ format v5) through the engine's LoadFileForMe and runs A* over its
  areas; unsticks by jumping/sidestepping and repathing;
- orders: follows the nearest human by default (each bot has its own place in the formation); radio orders come
  from a detour of mp.dll's `ClientCommand` (radio1/radio2 menus + `menuselect`, or the direct commands):
  follow me / regroup / stick together / fall back = follow, cover me = close escort, hold this position / get in
  position = hold, take point / storm the front / go go go = independent (hunts the closest monster);
- combat: aims at the closest visible `FL_MONSTER` (human turn rate), fires, backs off within 160 units, strafes at
  mid range, presses reload after a fight; pre-aims at monsters within `Awareness` it can't see yet; when idle it
  watches open directions around the leader instead of walls.

It is loaded by Miles Sound System, which loads every `*.asi` in Bin at startup (`apply_patches.py apply` copies it).
It only hooks the mp.dll build it was written for (it checks the `CBot::ExecuteCommand` call site first) and logs
to `Bin/botassist.log`. Settings: `Bin/botassist.ini`.

Build (VS 2019 Build Tools, x86): `cl /nologo /O2 /EHsc /MT /LD botassist.cpp /link /out:botassist.asi user32.lib`

Source layout: `botassist.cpp` holds what every mode shares (settings, entities, navigation, scenario companions,
room AI players, buying, teams, console, hooks, the create-room UI). Each mode with its own bot behaviour lives in
`modes/`, included by botassist.cpp where it was (one translation unit, so a mode file uses the shared helpers
defined before it): `zombie_giant.inl` (mode 30), `manhunt.inl` (51), `zombie_disaster_zone.inl` (59),
`zombie4_darkness.inl` (29), `zombie2_mutation.inl` (9), `zombie_conquest.inl` (56), `zombie_humans.inl` (human bots in every PvP zombie mode). A mode file only runs in its mode (each entry point checks `GameMode()`); a change
for one mode goes in its file, so the other modes' AI stays as it was. The shared code calls into them only from
the hooks (`HookedRunPlayerMove` per bot, `HookedStartFrame` per frame) and a few mode checks in buying and teams.

## mp.dll facts (CSO build of 2026, preferred base 0x10000000)

- `g_engfuncs` copy: 0x121d3a58 (GiveFnptrsToDll copies 0xd5 dwords; CSO's table has extra functions, so indices
  differ from the HL SDK): FindEntityInSphere 17, TraceLine 33 (flags: 1 = ignore monsters, 0x100 = ignore glass;
  seen in UTIL_TraceLine at 0x1151adb0), CreateFakeClient 108, RunPlayerMove 110 (call site 0x113167f7).
- `gpGlobals`: 0x121d3dac; `time` at +0, `pStringBase` at +0x98.
- `entvars_t` (from the save table `gEntvarsDescription` at 0x11d43fc0): origin 0x8, angles 0x50, v_angle 0x74,
  absmin 0xd4, absmax 0xe0, health 0x178, takedamage 0x190, deadflag 0x194, view_ofs 0x198, button 0x1a4,
  flags 0x1c8, team 0x1d8, pContainingEntity 0x238.
- More engine functions: Cmd_Argv 88, LoadFileForMe 100, FreeFile 101; `gpGlobals->mapname` at +0xc.
- `DLL_FUNCTIONS` (static copy at 0x11d1cc48, 0x4a entries): ClientCommand is entry 23 (0x113ebc60).
- edict_t -> entvars_t is 0x84 (measured in game). Zombie scenario zombies are `monster_entity`.
- `TraceResult` is the SDK one (flFraction +0x10, pHit +0x30).
- The edict -> entvars offset is found at run time (`pev->pContainingEntity == edict`).
- ServerCommand is engine function 42 (`kick "%s"\n` at 0x11782104 is sent through it); entvars `netname` 0x200.
- Bot quota per team (around 0x11307800): the "bot team" is the one without humans; with
  `bot_auto_balance_bot_team 0` it adds bots up to `bot_max_bot_team` and kicks the extra ones (0x1131b360).
  In mode 15 that quota is already 0 and the other team still gets bots (5 of them, not from the room's enemy
  count); kicking them makes the bot manager add new ones at once. With `SameTeam=1` they run "kill" as soon as
  they spawn (kick only if kill is refused). entvars `armorvalue` 0x1e8; kevlar + helmet = item_28.
- Where monsters were seen, botassist drives the bots for the rest of the map (between waves the stock AI shot
  doors and breakables). Bots refill ammo (primammo/secammo, after RefillAfterFiring seconds of fire) and armor
  in a buy zone (func_buyzone, or within 250 units of a spawn point).

## GameUI.dll (room panel)

- The "BotSetting" button (panel member +0x280, opens Popup_BotSetting.res / Popup_ZBBotSetting.res) is made
  visible only for modes 3/4/5/24, 22, 57 and the zombie modes (0x105b4dea..0x105b4e89), and enabled from
  the room settings object's vfunc +0x174 (0x105afd67). `apply_patches.py` makes it visible and enabled always.

## Orders

Radio menus (radio1/radio2 + menuselect) and chat: `/follow`, `/hold`, `/cover`, `/free` (say or say_team).
Bots fire only when the real line of fire (view + recoil) meets a monster first, never a wall or a teammate.

## Debugging (`Debug=` in botassist.ini)

`botassist_debug.log`: every second per bot, the angles the stock AI asked for, the angles botassist sent, the
real `v_angle` after the command, and what each line hits (class, flags, health, takedamage, team, names, model);
its target/threat; every 5 s a census of the entities that can take damage within 1500 units of you, and every
change of what you look at. `Debug=2` adds lasers (TE_BEAMPOINTS: engine MessageBegin 49, MessageEnd 50,
WriteByte 51, WriteShort 53, WriteCoord 58 (float); sprite index from the short at 0x12183278).

## Barricades and aim behaviour

- Zombie scenario barricades are `zbsbreak` (CZBSBreak : CBreakable, datadesc m_playerRatio +0xe0, m_monsterRatio
  +0xe4). CZBSBreak::TakeDamage (vtable index 17, 0x11376f30, 5 args, `ret 0x14`) rewards players' hits and scales
  monster damage by m_monsterRatio - 0.1 * (upgrade level byte at gamerules+0x12c9 - 1). Offline their health
  never dropped, so botassist damages barricades touched by monsters through CBreakable::TakeDamage (0x1147b850)
  at `ZombieBarricadeDamage` per zombie per second, unless the game is already damaging that barricade.
- Targets stick until dead or out of sight for 0.7 s (a monster passing in front doesn't count); a visible monster
  within `ExtremeThreat` takes over if the current target is clearly farther away.
- Human error by skill (0..7): flick error on a new target that fades out, drifting error, spread between shots,
  partial recoil compensation, bursts beyond 600 units. Bots fire only when their intended line meets a monster.

## Teams, fire modes, movement (v8)

- Game mode: engine function 165 returns the room object; its vfunc 0x188 is the mode id (as mp.dll reads it).
  In `CoopModes` a bot on T runs "jointeam 2" + "joinclass <CT character>" (kicked after 3 refusals).
- Trigger: weapons of category 3/4/5 (botassist_weapons.txt, model `models/p_<res>.mdl`) are held; 1/2 tapped. A
  held weapon whose punchangle never moves in 0.8 s is semi-automatic and tapped from then on (machine guns
  excepted: they spin up). Spraying continues 0.35 s after the line leaves the target; no reaction delay when a
  bot switches targets while shooting.
- `Roam=1`: when the leader stands still, each bot walks to its own open nav spot 140-520 units away (6+ free
  directions out of 8, 200 units from other bots' spots), changing every 6-14 s.
- Damage with no monster within 160 units marks a 170-unit hazard for 30 s; the bot goes back to its last safe
  position (no damage for 2.5 s) and avoids hazards when choosing spots.
- Server: room request 27 (ignored before) is logged with its bytes, to find the bot dialog's packet.

## Room AI dialog, objectives, leash (v9)

- The room's AI dialog (CBotOptionDlg) sends ROOM_LOWMID_BOT with botAdd 0, and the game only adds bots with
  botAdd set: the server now sets botAdd from the counts (and 0 enemies in co-op modes). GameUI patch
  0x105bd710: the enemy list starts at 0 like the friendly one.
- zbsbreak keyvalues: `hmdmgratio` (players, CZBSBreak +0xe0) and `zbdmgratio` (zombies, +0xe4). Barricades:
  zbdmgratio > 0 (zs_defense brc_*: 1). Objectives ("red", zs_lastclue box1..5): hmdmgratio > 0, zbdmgratio 0.
  Both 0: scripted (health 1). zbstarget: what the players defend. Bots shoot visible objectives within
  `ObjectiveRange` when no monster is in sight; zombie damage on barricades is scaled by zbdmgratio.
- Followers stay within `Leash` of the leader: they only hold position to fight close to them, back off towards
  the leader's side, and stop combat moves to catch up.

## Co-op terrorists (v10)

The extra terrorist bots don't come from the room's enemy count (0 still gave 5) and refuse "jointeam 2";
kicking them made the game add new ones at once (bots kept joining and leaving). Bots are added through
CSOZBBotManager vfunc 2 AddBot(team) (0x1155cc20; vtables 0x11991964 and the voxel 0x119400e8; team 0 T, 1 CT,
2 = bot_join_team / balance). botassist replaces that slot: in `CoopModes` terrorist requests are refused and
automatic ones join CT. No more kicks in co-op modes.

## Room AI players (phase 1)

- CSNZ_Server keeps a list of AI players per room (`RoomBot`: name, level, team, primary/secondary/melee item,
  character item, skill). They are sent to the room's users as players (PlayerJoin with a fake id 0x40000000+,
  character data copied from the host with their own name/level, "ready"); kicking one from the list removes it.
  Room chat (host): `/addbot [name] [t|ct]`, `/removebot <name>|all`, `/bots`. Web API: GET/POST `/api/roombots`
  (add/remove/update/move) for the editor, GET `/api/matchbots` (last started match) for botassist.
- At match start the server sets friendly/enemy bot counts from the list (botAdd 1).
- botassist fetches `/api/matchbots` (WinHTTP, `ApiPort`) on each new map; every bot that joins takes the next AI
  player of its team: renamed through SetClientKeyValue (engine 115; IndexOfEdict 76, GetInfoKeyBuffer 113),
  its character on joinclass, its weapons when buying (melee too), its skill.
- Fixes after the first test: GetInfoKeyBuffer is engine function 112 (113 is InfoKeyValue: renaming crashed);
  the "monsters seen" time is per map (gpGlobals->time restarts with each map, an old time looked recent and the
  removal of "other team" bots ran in a zombie-escape room: kicked bots kept coming back, now removed entirely);
  the old CCSBotManager::BotAddCommand (0x11306ae0, cdecl (team, fromConsole), used by the per-team quota code) is
  detoured like the new manager's AddBot: co-op modes refuse T, auto -> CT; bot_join_team CT in co-op modes.
- Scenario-only (v12): everything about monsters (driving the bots, random weapons from the whole game, random
  characters, ammo/armor refills, barricade damage, team fixes, radio orders, debug) runs only in `CoopModes`
  (15 zombie scenario, 17 human scenario, 28 zombie shelter, 52, 53, 59). Other modes are PvP and left to the
  game; only the room's AI players (name, chosen character, chosen weapons) apply there. Zombie modes put everyone
  on CT and infected players switch to T, so a room AI player is matched to a bot of any team when none of its
  team is left. The server takes the AI players out of the room list while the match runs and sends them back
  when it ends.

## Room AI player editor (phase 2)

- `mod/web/bots/` (served by CSNZ_Server at /bots/): the room's AI players like the room list (level, name, team,
  primary weapon icon, reorder, remove, "Add bot") and an editor per bot: name (+ random), team (not in co-op
  modes), skill (room / 0..7), character, primary, secondary and melee pickers with category tabs and search.
  Data: `/api/roombots`; `catalog.json` from `tools/export_bot_catalog.py` (weapons from botassist_weapons.txt,
  melee = Item.csv category 11, characters from botassist_characters.txt; icons into web/shop/icons).
  Served by anything else (e.g. `py -m http.server` on mod/web) it shows a fake room for layout work.
- The room's "AI settings" button: botassist patches GameUI's BotSetting branch (0x105b884d -> stub -> OnCommand
  epilogue 0x105b88ad): POST /api/openview {"view":"bots"}, then CCSOMainPanel::OpenWebEvent (0x105a9020 on
  [0x10b844fc]). The shop page gets `openView` from /api/state once and goes to /bots/.
- Adding: team (CT/T, hidden in co-op modes), how many, the weapon filter of new bots, "Add" and "Fill room"
  (POST /api/roombots {action: add, count, team, preset} | {action: fill, ...}).
- Weapon filters (`/api/botpresets`, per account in CSNZ_Server's Data/BotPresets.json): sets of allowed primary,
  secondary and melee weapons; "Counter-Strike classic" (primaries 7-24, pistols 1-6, knife 161), export/import
  as a `CSOBOT1:<base64 json>` code. A bot's filter goes to botassist in /api/matchbots (allowPrimary/Secondary/
  Melee): a weapon slot left on Random is picked from the filter on every life.
- Filters also hold characters (CT and T tabs; "Counter-Strike classic" = 40-43 and 49-52), sent as
  allowCharacter. A room AI player whose character is Random joins as one of the filter's characters of its team,
  or as any character of its team (botassist_characters.txt) when the filter has none or there is no filter.
- The top bar's skill list sets the skill of the bots it adds: random by default (each new bot draws 0..7, by an update
  after adding it), room skill or 0..7. In Zombie Conquest (`MODE_SKILL_MODES`) the default is no draw: skill -1, which
  botassist plays at skill 6 there.

- A new match can load the same map again, so per-match state (room AI players, bot quota, barricades, monster
  detection) is keyed on a level counter: `gpGlobals->time` starting again (checked by LoadNav and every 500 ms by
  the watcher thread) counts as a new level.

- Characters: the game's bots never send `joinclass`. The bot manager calls HandleMenu_ChooseAppearance(player,
  class) (0x113f2cb0, cdecl; old manager call site 0x11306a3f with the bot profile's skin) while creating the bot;
  `joinclass` from anyone is refused unless the class menu is open (m_iMenu +0xe10 == 3). botassist detours it
  (6-byte prologue) and swaps in the room AI player's character, only while the game's AddBot runs.
- Buying: the item_<id> handler first asks g_pGameRules (0x121cac30) vfunc 0x2b8 (player, command), thiscall
  bool. botassist logs that answer for each room loadout purchase (other modes buy 0.2 s after spawning).

- client.dll crash while loading a match (c0000409, stack cookie): the scoreboard builder (RVA 0x1100e20) loops
  over teams 1..[this+8] (records of 0x2c bytes at this+0x2c*i: +0x34 name, +0x38 score, +0x3c/+0x40 counts) and
  formats them with an unbounded swprintf helper (RVA 0xc5aca0) into stack buffers (ebp-0x38 is 40 bytes). The
  spectators record (team 3) held 1650543732 ('tLab') in both dumps: "1650543732 (Muertes)" overflows by 2 bytes.
  botassist redirects the 10 calls of that function to bounded copies (20/50/1024 wide chars).

- Buying in PvP modes: BuyItem(player, cmd) 0x113f21a0. After g_pGameRules vfunc 0x2b8 it checks vfunc 0x36c(player,
  weapon) and, when vfunc 0xb0 is true and the player's internal-buy flag +0x18b4 is 0 (with +0x2e38 set), it returns
  1 without giving anything. The game sets +0x18b4 = 1 around its own item_<id> purchases (0x1073d0ae..0x1073d2bc);
  botassist does the same around the room loadout purchase.
- ChooseAppearance stores the class item in +0xcd4 / +0x1fb8 and a class index in +0x2156 (from class manager
  [0x121d3ec4]+0x1f8 vfunc 0x40); an index < 0x28 falls back to the team default (+0x2155 = 1).

- Character numbers: joinclass / ChooseAppearance take the class's InGameID (Item.csv column; urban 1, terror 2,
  ma_AliceBLUE 40, thief 82), not its item id. botassist_characters.txt is "<item id> <team> <InGameID>"; an item
  id falls back to the team default (+0x2155 = 1, class index 0). Seen: a player's joinclass 11 -> +0xcd4 11, index 53.
- Respawns in PvP modes can happen without any bot frame while dead: a new life is also a 1 s gap in its frames or
  its health back up by 30+.

- "None" weapon slots: a bot's own None (-1) or a weapon filter whose primary/secondary list is [-1] (the editor's
  None cell). The bot doesn't buy that slot (its AI's buy commands are dropped) and drops what it carries there:
  "cl_drop" (CSNZ has no "drop" command) throws the weapon in hand, so every 4 s it is switched to the None slots (slot1/slot2) to drop what it
  picked up; checked every 0.6 s.
- New rounds respawn bots with the game's default weapons without a dead frame or a health change: a jump of
  300+ units between two frames (to a spawn point) also counts as a new life, so the room loadout is bought again.
- Before buying the room loadout the bot empties the slot with the game's DropPrimary / DropSecondary (0x113f1750 /
  0x113f1790, cdecl player), whatever is in its hand; the thrown weapon's weaponbox can't be picked up. Its stock AI
  buys (`awp`, `deagle`, or a CSNZ weapon by resource name, e.g. `balrog7`) are dropped for a room AI player with
  its own weapons, random BuyAnyWeapon ones included: right after botassist's purchase they threw the room's weapon
  to the ground ("wanted to buy ... dropped" in botassist.log). Zombies (player +0x2166) buy nothing.
  The weaponbox thrown that way can't be picked up and its own think (+0x4c, CWeaponBox::Kill, which also removes the
  weapon inside) is brought forward to 0.5 s: 31 bots each life piled up boxes nobody could take. Freeing it with
  FL_KILLME left the weapon inside behind and a CLockOnGun got destroyed twice (crash in OnFreeEntPrivateData).
- Create-room mode grid (GameUI CModeSelectDialogV2 and its _Select / _Check, vtables 0x108d287c / 0x108d2c74 /
  0x108d3074): tiles are made in code (0x102e5cc0), one per mode of the list hw.dll builds from the server's
  GameModeList.csv; 0x102e6610 leaves out modes with mode_fun_order > 0 (GameUI 0x104341a0), which go to "Other
  modes" (99). CSNZ_Server's GameModeList.csv now has every one of those in the grid (fun order 0, ui order 100 +
  old fun order), so botassist hooks OnThink (slot 104) / OnMouseWheeled (117) of those dialogs, adds a vgui
  ScrollBar (ctor 0x1071f8c0, 0xd0 bytes; slots 159 SetValue, 160 GetValue, 161 SetRange, 163 SetRangeWindow,
  171 SetButtonPressedScrollValue) and moves / hides the tile rows (vector +0x1bc..+0x1c0, 0x34-byte entries).
- Zombie Giant (mode 30, mp.dll CSOGameRulesZombieBoss, vtable 0x117a7a4c, parent CSOGameRulesDM): players are
  counted with 0x1091c670(player, 0), which skips bots (player vfunc 0x21c = is bot). Slot 60 (0x1085f660) ends
  the match when one or no human is left; the giant is picked among human CTs (0x10861d50); 0x1085f850 keeps
  3 bots per giant (adds T bots, kicks the rest: the "kick" seen in the log) and bots spawn as br_bot soldiers
  (0x10860740: famas / m32).
  botassist (HookZombieGiant): the bot classes' vfunc 0x21c (CBot 0x1193f25c, CCSBot 0x1193dcc8, ZBBot 0x1198f294,
  CPlayRoomBot 0x1179b8a4; 0x106626c0 "return 1") answers 0 for room AI players only in the end-of-match check
  (0x1091c670 called from 0x1085f660), the soldier count (0x1085f850) and the kick pick (0x1131a210): the match goes
  on with one human and room bots are never kicked. Room bots all join CT; bot requests with 0x1085f850 on the stack
  (its soldiers) pass the room quota. Rules slot 98 (0x10860740) gives room bots the parent's equipment (0x10743240)
  instead of the br_bot model and famas / m32.
  Room bots can't be the giant: humans here are ZombieBossPlayer (0x3778 bytes, giant state at +0x3704..+0x3770,
  cast by 0x10922d00), bots are CCSBot (0x5aa0, own fields from +0x3700). The first version answered "not a bot" to
  all the mode's code; the mode then read bot memory as giant state and crashed (0x10637b80 via 0x1091c9f0 when the
  player picked a weapon).
  Room bots count as the mode's soldiers (0x1085f850 counts bots): any bot it asks for becomes a room AI player
  on CT, extra ones are refused, so no bots pile up round after round.
- Zombie Giant fake players (ZgPlayer, `ZombieGiantPlayers=` in botassist.ini, default -1: every room AI player): the last room AI players
  of the list join as fake *players* so the giant rotates among you and them: engine pfnCreateFakeClient (table
  index 108), pfnFreeEntPrivateData (70), the player factory 0x113ef1d0 (DLL function; ZombieBossPlayer, vtable
  0x11958834, 0x3778 bytes, in mode 30), ClientPutInServer 0x113efbb0, "*bot 1" in the info key buffer for the
  scoreboard, then jointeam 2 / joinclass. botassist drives them once per frame (from the bots' RunPlayerMove,
  calling the engine's pfnRunPlayerMove itself): soldiers buy their weapon, keep 450..850 u from the giant,
  strafe and shoot; the giant follows the navigation mesh to the closest human, sends "BossAttack1/2" when close
  and a random "BossSkill_1".."4" every 6..14 s, sprints (IN_ATTACK2) when far, "BossJump" when stuck.
  The giant's commands: rules ClientCommand (slot 11, 0x10861490) -> ZombieBossPlayer 0x10923b60 -> pawn handler
  0x1090cb50 (also "zg_gainstamina", "zg_gainevol", "zg_freez") -> name to state 0x1090db80 -> 0x1090e1f0 (stamina,
  cool down). Pawns: ZombieBossPawn::Goliath / Titan / Revenant / Dione / Angra / Human...; giant types +0x3710
  1, 2, 4, 6 (T) and 0, 3, 5 (CT); 0x10861d50 picks the giants among the CT humans who haven't been one yet.
  The pick sorts by +0x3708 (times giant) then +0x370c (gpGlobals->time when they joined / were last the giant),
  so the first to join (you) was always first: ZgUpdateTeam writes a random value there for those not yet giant.
  Room bots on CT in this mode stand still with their stock AI: DriveZgBot gives them the soldiers' logic
  (ZgThink: gather at the team's average position on the way, then 450..850 u from the giant, strafe, shoot).
  "jointeam 2" left the fake players as spectators (team 3; the command only works with the team menu open), and
  the game dropped them later: they now join like the bot manager's bots (+0x6c team, +0xe10 1, +0xcc0 4,
  HandleMenu_ChooseTeam 0x113f2f00(player, team), HandleMenu_ChooseAppearance 0x113f2cb0(player, class)).
  ZombieGiantYouCanBeGiant=0 (default): your +0x3708 (times giant) is kept at 1000 so the fake players go first;
  the game resets it only when a player joins (rules slot 105 -> 0x1091ac40).
  Fake players were kicked for idling ~2.5 min after joining (CBasePlayer think 0x114c6b7b: mp_autokick, time since
  +0xccc last movement, skipped for IsBot): botassist keeps +0xccc at the current time. Fake players and driven
  bots also press the movement keys (IN_FORWARD / IN_BACK / IN_MOVELEFT / IN_MOVERIGHT) like a client does.
  botassist_debug.log: "ZG '<name>'" / "ZG bot '<name>'" lines every 15 s (position, speed, movement, target).
  Bots and fake players stood still at maxspeed 1 (pev +0x240; ClientPutInServer's joining speed, never reset for
  them in this mode): ZgUnfreeze calls pfnSetClientMaxspeed (engine function 107) with 240 once your own maxspeed
  is >= 100 (the freeze is over).
- Zombie Giant debugging (botassist.log): "CLIENT put in server / disconnects" (detours of 0x113efbb0 and
  ClientDisconnect 0x113ef8c0, with the return addresses on the stack), "SERVER COMMAND" (every kick, and the
  first 40 commands), "ZG state" (team / giant type / next giant of every player when it changes);
  botassist_debug.log: "ZG census" every 15 s (team, class, health, flags, maxspeed, speed, buttons).
- Zombie Giant pick: the sort call 0x1086208a (push 0x1085f810) uses ZgGiantOrder, a random key per candidate drawn
  once per pick (you last only with ZombieGiantYouCanBeGiant=0). The game's own order put the first to join (you)
  first; making your +0x3708 1000 made its comparator inconsistent (you still came first).
- A fake player made the giant was dropped by hw.dll 10 s after the pick, at the transformation: "Reliable channel
  overflowed" (rva 0x786a06: netchan.message overflowed -> SV_DropClient; nothing ever sends a fake client's
  reliable channel, and the transformation sends it a burst). MessageBegin (engine function 49) turns MSG_ONE to a
  fake player into MSG_ONE_UNRELIABLE. Fake players are also driven from the end of StartFrame (0x113f8a80) so they
  play with no bots in the room.
- Fake players were still dropped by the engine when they died or became the giant. SV_DropClient (hw.dll rva
  0x71ba90; client_t name at +0xa3e0) is detoured: every drop is logged ("ENGINE DROPS", reason and exact caller)
  and a fake player is kept unless it is a level change, the end of the session, or botassist's own kick.
  (The stack lists in "CLIENT disconnects" are unreliable: stale return addresses.)
- Fake players buy like the bots: each is linked to its room AI player (g_matchBots edict + BotState roomBot) and
  runs UpdateBuying (room weapons, filters, None, rebuy each round) except as the giant.
- The real cause of the drops: fake players moved to the terrorists (picked as the giant) were kicked every frame by
  hw.dll's CGameRoomManager::KickPlayer(player index) (vtable 0x2c186f4 slot 10 -> 0x247aa10 -> SV_DropClient
  "Kicked"; the room doesn't know them). The slot is replaced: the caller is logged once ("ROOM KICK"), fake
  players aren't kicked. "ENGINE DROPS" lines are logged once per name and reason.
- Zombie Giant money: the room's weapons cost more than the money the mode starts with (+0xca8, read by 0x114b76d0
  against the price from rules vfunc 0x400); UpdateBuying raises it to 16000 before buying the room's weapons there.
  Not enough: the fake players still kept the mode's AWP (the buy changes nothing for them). When a buy fails there,
  the room's primary is given directly: "slot1", "cl_drop" if a primary is in hand, GiveNamedItem 0x114bac10
  (thiscall player, "weapon_<resource>", 1).
- The kick mark: respawn (0x113fa200, also the "respawn" command) counts the respawns of players the room doesn't
  know (+0x2f54) and marks every 3rd in the bit set 0x1218bce8; StartFrame (0x113f8bfa) then calls the room
  manager's KickPlayer. Fake players' counter is kept at 0.
- Debugging: "ZG picked" every 0.5 s for a fake player picked as the giant (team, type, next, timer +0x3724,
  height, vertical speed, +0xe10...); "fake player '<name>' runs:" what the game makes it run.
- Why picked fake players never became the giant: the transformation (rules slot 116, 0x10860e50) moves the picked
  player to the terrorists and runs "respawn"; the type is only applied by the mode's equipment (slot 98,
  0x10860740 -> 0x1091c9f0: type = next, next = 0). Once fake players were linked to their room AI player (for the
  room's weapons), IsRoomBotEdict counted them as room bots and HookedZgEquip gave them the parent's equipment: they
  stayed terrorists with type 0 and next set, the giant count (0x1085f930: giants alive, or next set and team 2)
  never saw them, and the game picked two more every 10 s. Fake players are no longer room bots there.
- The giant spawns on one of the rules' giant spots (0x1085fa90: vector at rules +0x260..+0x264, only spots with no
  human near). The stuck fake players sat on those spots, so none was left for the real giant: the random index
  read past the list and you spawned outside the map. "ZG giant '<name>' spawned N units from the closest of M giant
  spots" checks every giant spawn.
- The room's primary given directly is then selected with "weapon_<resource>" (dropping the AWP doesn't always work:
  weapon_chainmg was given but the AWP stayed in hand).
- Zombie Giant only: the hooks every mode passes through (StartFrame, client connect/disconnect and server command
  logging, engine drops, room kicks, reliable messages) act only in mode 30 or while its fake players are still
  connected (ZgActive); in the other modes they call the game's code unchanged.
- Giant commands per pawn (vfunc +0x20 maps the command to a monster state; pawn vtables Human 0x117c18c4, Dione
  0x117c18e8 type 1, Titan 0x117c191c type 2, Revenant 0x117c1950 type 4, Angra 0x117c1984 type 6, Goliath
  0x117c19b8 type 3, Comanche 0x117c19dc type 5): BossAttack1/2, BossJump, BossSkill_1..3; BossSkill_4 only once
  evolved (pawn +0x14). Evolving: 10 points ("zg_gainevol", pawn +0x10) and "use" pressed (0x1090d290: player
  +0xec0 & IN_USE). The giant AI presses use every 2 s.
- Giant AI: the secondary attack (mostly ranged: poison, grenade, fireball) is also thrown at visible humans
  350..2200 units away every 5..12 s, aimed a little high with the distance. zg_splash has 136 zg_breakable (props
  only giants break) and 34 func_breakable: a giant that barely moves for 0.4 s with one within 120 units in front
  faces it and hits it (BossAttack1) until it breaks (6 s at most). Logged: "is blocked by", "broke", "throws".
- Giant states (registered in 0x1091b060; the classes' RTTI names): Dione Attack1 3 light, Attack2 4 heavy (whirl),
  Skill1 5 rolling, Skill2 6 poison missile, Skill3 7 tentacle, Skill4 0x2a air strike (evolved: 0x26..0x29);
  Titan 10/11/0xc rolling/0xd rocket/0xe mine/0x2f artillery; Revenant 0x14/0x15 own heavy/0x16 rolling/0x17
  fireball/0x18 inferno/0x34 hellfire; Angra (dragon) 0x20/0x21/0x22 own rolling/0x23 breath/0x24 tentacle/0x39
  poison fog. BossJump (CommonState::StateJump) leaps towards the movement keys held.
- Pawn (0x109201f0 fastcall player): +0xc stamina, +0x10 evolution points, +0x14 evolved, +0x14+slot ready,
  +0x28+4*slot cool down end. State data 0x1090c770(state): [0] slot, [5] cool down, [6] stamina cost (checked
  by 0x1090e1f0). Current state: state manager (player +0x3738) vfunc +0x14, 0 idle.
- Giant AI (ZgGiantCombat): per giant kit (role of each move: melee, heavy, mid, charge, projectile, area, around
  itself, cone); while idle it uses a skill as soon as it's ready and affordable and the target fits its role,
  otherwise attacks; aimed moves wait until it faces the target. Presses use only with 10 evolution points.
- Water (ZgScanWorld): func_water and func_ladder volumes per match; navigation areas under water cost 2000 more;
  soldiers' goals and the rally point move to the closest dry area; a soldier in the water swims to the closest
  ladder leaving it and climbs it (pitch -70, forward), or heads for dry ground.
- Fix: the state manager is an object inside the player (vtable at player +0x3738), not a pointer: reading it
  through a pointer faulted, so ZgGiantCombat never ran (giants only broke props). If the pawn can't be read the
  giant falls back to plain attacks and random skills ("the pawn couldn't be read").
- Water, second try: zg_splash's navigation mesh has almost no areas in the deep channel, and a giant standing in
  it has its origin above the surface, so its position didn't count as wet and soldiers walked in after it. Water
  is now judged at the feet (absmin.z) and, for the next step, at the floor found by a trace down: soldiers don't
  step, back off or strafe into it, and their goals move to dry ground.
- Stuck: a jump, then 1.5 s sideways, then another target for 10 s ("can't get to"). A breakable it gave up on is
  ignored for 20 s; broken ones (solid 0, entvars +0x11c) are skipped.
- Ammunition (ZgAmmo): player +0x101c active weapon; weapon +0xe4 id, +0x158 primary ammo type, +0x160 clip; player
  +0x1034 reserve per ammo type. Reload when empty or below half between fights. Each weapon held for the first
  time in a life gets its ammunition: "primammo"/"secammo" (money topped up), and if that gives nothing the reserve
  is set to three clips (30..300). Empty: pistol ("slot2"), then knife ("slot3"). The zs_supply boxes need a charge
  earned by dealing damage, so the soldiers don't go there (they stood at them pressing use).

## Console: cheats and bot_add

- sv_cheats on during a match: botassist checks it every 2 s and tells CSNZ_Server once per match (POST
  /api/cheats). The server flags the match; its medals (match, orders completed by it, level-ups) are cut by
  Data/Warbonds.json Medals.CheatPenalty (60%), with a chat message.

- Nexon's hw.dll registers god, noclip, notarget, fly, kill (and pause, setpause, unpause, save) with an empty
  handler (`ret`). botassist writes its own handlers into those entries of the engine's command list (head at hw rva
  0x14aa068; entries {next, name, function, flags}) and adds `give` through AddServerCommand (engine function 146).
  They act on your player (the first client without FL_FAKECLIENT, PEntityOfEntIndex = engine function 77); all but
  kill need `sv_cheats 1`. Toggles are applied again every server frame (from the StartFrame detour) until turned off
  or a new map starts: god = FL_GODMODE + takedamage 0, noclip = movetype 8, fly = movetype 5, notarget =
  FL_NOTARGET; kill = mp.dll's ClientKill (DLL function 20, 0x113efaf0). `give` takes weapon_x / item_x / ammo_x,
  a bare name (weapon_ is added) or an item id of botassist_weapons.txt (GiveNamedItem 0x114bac10). Messages go to
  the console through Con_Printf (hw rva 0x6e0d70); entvars movetype is at 0x118.
- `bot_add`, `bot_add_t`, `bot_add_ct`: mp.dll registers every bot_* command with the same handler (0x11317980 ->
  bot manager ServerCommand -> AddBot(2 / 0 / 1)). Their entries get a wrapper that sets g_consoleBotAdd, and
  RoomBotQuota lets those bots in past the room's AI players.
- The command list is checked every 2 s of server time (mp.dll's commands are registered when its bot manager starts).

## Room skill in PvP, characters half men / half women

- The game's own AI (PvP modes) aims by the bot's BotProfile: CBot::m_profile at player +0x3700 (profile +0x04
  aggression, +0x08 skill 0..1, +0x0c teamwork, +0x64 reaction time = file value - 0.1, +0x68 attack delay; parser
  at mp.dll 0x11319b70). Profiles are shared, so a room AI player with a skill gets its own copy (ApplyRoomSkill):
  skill 0..7 -> skill 0.2..1, reaction 0.4..0, attack delay 0.7..0. Logged as "room skill N (profile ...)".
- Sex of a character: ChooseAppearance asks the class table ([0x121d3ec4] + 0x1f8, vfunc 0x50(class number),
  thiscall) whether the class is male (player +0x1fb5 bit 0 / +0x2155: male voice and sounds). Random characters
  alternate per team so each team gets as many men as women (PickCharacter).
- Team swaps in the middle of a match (mp.dll 0x114d66e1) give the default character of the new team (class table
  vfunc 0x24(team) -> player +0xcd4, male, player vfunc 0x220 = model). A bot whose class becomes that default
  after a team change gets a character of its new team (room character, filter, or random half men / half women)
  the same way (ApplyTeamCharacter). Not in zombie modes (infection changes team and model).
- Room AI players without weapons or filter buy any weapon of the game (BuyAnyWeapon=1) in every mode, melee
  included (botassist_weapons.txt category 11, from gen_weapons.py).
- sv_cheats: hw.dll sets it to 1 at start-up with -dev, and it stays on between matches; botassist sets it to 0
  when each match starts, so only turning it on during the match flags the match (logs the command line once).

## Taskbar VIP and Missions buttons

- CSOTaskbar::OnCommand "OpenVIPInfo" (0x1059795e) and "OpenQuestDlg" (0x10597287) open main panel pages 0x15 / 0xf
  through the page manager (0x104528c0, vfunc 0x64). botassist turns their "call 0x104528c0" (0x10597976 /
  0x105972a3) into a jump to a stub that posts /api/openview {"view": "profile" | "missions"} and opens the game's
  browser window (OpenWebEvent), then returns past the page call (0x10597984 / 0x105972b1).
- The pages: mod/web/profile (statistics and medals, CSNZ_Server CMedalManager, Data/Medals.json from
  tools/gen_medals.py) and mod/web/missions (requests, weekly event, Black Market, orders, challenges;
  CMissionManager, Data/Missions.json from tools/gen_missions.py).

## Table overrides: Zombie Disaster Zone's zombie mix
mp.dll loads its data tables (Zombie Scenario's `maps/ZBS_*_<map>`, `resource/...`) through 0x1146cb10; the `.cso`
files are CSV encrypted with TEA (fixed key, hw.dll 0x02758c80; `tools/csodec.py` decrypts and encrypts them).
botassist detours hw.dll's `.cso` loader (0x027583e0, every table load ends there; mp.dll's tables are
`StringTableWrapper`s with the table at +4): `Bin/botassist_tables/<table>.csv` (e.g.
`maps/ZBS_WAVE_zsrift_splinter.csv`), when it exists, goes to hw.dll's CSV parser (0x02758ba0) instead. Plain files
read whole through SharedDataMgr vfunc 0x4c8 (0x025b3c70) are served from `Bin/botassist_tables/<path>` the same way.

Zombie Disaster Zone's own waves spawn only zombie types 1 and 2 every round, though its type table
(`maps/zbs_zt_zsrift_splinter`) lists the scenario's special zombies. `gen_zsrift_waves.py` rebuilds the wave table
from the game's with a mix that grows each round (speed, psycho, healer, heavy, boomer, deimos, climber, light,
revival, golden speed) and the boss's escort mixed too; `apply_patches.py` copies it into Bin.

## Zombie Disaster Zone's random events
Each round the mode draws random events from `maps/ZBS_ENC_<map>_dedi.json` (mp.dll 0x107633d0, jsoncons): stage
hazards (hurricanes, magnetic fields, high gravity, petrifying statues, fire bombs, airstrikes, vigilant eye), special
zombies joining the round, rounds of one kind of zombie, weapon / speed penalties and zombie HP / attack / speed
modifiers. Nexon kept that file on its dedicated servers, so the mode had none of them offline.
`gen_zsrift_events.py` writes one (its docstring has the format), served through the file overrides. The number of
events per round is rift level / 32 in the game (made for levels up to 100); `ZSRiftEventDivisor` (botassist.ini,
3 by default) replaces 32. Bots get at least `ZSRiftBotMoney` on each new life there, since the mode pays once per
round and they come back with nothing.

## Zombie Disaster Zone's Rift Tickets
botassist clears every round of the rift itself (see above), so it counts them: when the match's cleared rounds reach
the round count of the rift (zombie round manager +0x290, the boss round included) it posts
`/api/zsrift/clear {"difficulty", "rounds"}` to CSNZ_Server (tried 5 times, 3 s apart). The server pays everyone in
the room Rift Tickets for the room's rift level, once per match (Data/ZSRift.json `ClearTickets`, `TicketsPerLevel`),
spent at the Rift Exchange tab of the Missions page. Log line: "ZSRift: rift cleared at level N: told the server".

## Zombie Disaster Zone's monster strength per rift level
mp.dll's monster setup (0x11360de0) multiplies a monster's attack, health, armour and speed by
1 + rate * (level - 1) ^ 1.85, the rates of its kind coming from `resource/ZSRift/ZSRIFT_Monster` (hw SharedDataMgr
vfunc 0x8ac(type): attack, health, armour, speed; type 0 common zombies, 1 and 2 special ones, 3 bosses). The room's
difficulty tooltip (GameUI 0x105b4c50) uses the same formula. At level 20 that was +325% attack and +2321% health.
botassist hooks that vfunc to hand out `ZSRiftMonsterAttack` / `ZSRiftMonsterHealth` / `ZSRiftBossAttack` /
`ZSRiftBossHealth` (percent per level: +50% / +300% / +25% / +200% at level 20) and sets the exponent (mp 0x11945470, GameUI 0x1096ff50, read
only there) to `ZSRiftLevelExponent` (1: the same step every level). Armour and speed keep the game's rates.

The room panel redraws itself (and the tooltip) after a settings change only when the changed options include bit 59
(GameUI 0x105bcd10 ands them with the mask at 0x10aff218; the bits are the server's room flags low / lowMid / highMid /
high). A new rift level (lowMid ZSDIFFICULTY, bit 45) left the tooltip on the room's first level: botassist points
that load (0x105bcd2b) at its own mask with bit 45 added. Rift levels unlock one by one on the server (Data/ZSRift.json
`UnlockByClearing`): the highest one unlocked is the best clear + 1, sent in Packet_ZSRift type 0.

## Manhunt (mode 51, ZHE) with room bots
One random player is the zombie (CSOGameRulesZHE 0x107a6290 shuffles every living client, bots included); the others
repair generators (`esc_generator`, CZHEGenerator: one of the 3 spots of each of the 7 groups is used, the others stay
EF_NODRAW) and leave through an exit (`esc_exitzone`: always lets humans out, but the map's doors `exit_door1..4` block
it until the rules fire `ms_exit_active`, a multisource with subtotal 5 that opens two exits at random; an exit is
open for the bots once its door has moved). The round waits ("#CSO_WaitEnemy", rules vfunc 0x104 = 0x107a65a0) until
everyone in the room is in game, so every room AI player joins CT even when the game asks for terrorists to balance
the teams (the zombie is drawn from the CT players). The stock bot AI knows none of it, so botassist plays every bot
there (`DriveZheBot`):
- humans repair the closest generator nobody else takes, pick up teammates who are down and heal hurt ones (when
  calm), one bot per teammate while the others carry on, holding +use the way the game accepts it (below); they let
  go while a generator overheats (state 5) or after it blew (6). Healing only works on someone standing still (the
  human module counts its helpers, +0x24 list / +0x28 count), so a bot being healed or picked up stays still and
  looks at its helper. When the zombie is heard (within 550 u, 900 u while running) or seen within 1100 u they run,
  the knife out, to a nav spot out of its sight (a trace from the zombie blocked, the way there not past it) and keep
  going until there; seen again, cut off or no way there: another spot; arrived with the zombie still on their heels
  (seen or heard in the last 3 s, or within 700 u): on to another spot; arrived and out of its sight: they hide
  crouched for 4 to 9 s, watching where it was (hiding, only a zombie within 700 u moves them), then go back to work. Down, they crawl
  away from it or towards a teammate; with the exits open they run out. The exit zones only let out what classifies
  as a player (CZHEExitZone::ExitTouch 0x108ea1e0 wants vfunc 0x30 == 2; CCSBot says 15), so botassist lets a bot
  out itself once its box is in an open exit's zone: the human module's escape 0x108017a0 (state 5, spectator flags,
  the message), then g_pGameRules vfunc 0x100 (what ExitTouch calls next); escaped bots stay still;
- the zombie sees in a 140 degree cone (up to 3000 u, crouched humans only up to 1100 u, anyone within 350 u) and
  reacts after 0.4 s; it also hears humans running (not crouched, over 170 u/s) within 900 u and goes to search there. It patrols a wide, random round, at a random walking pace, stopping at each spot to look around:
  mostly a ring 700 to 2400 u around a random human (it never knows exactly where), 20 % of the legs closer (300 to
  900 u), 15 % anywhere; 85 % of the legs keep 900 u off the humans who are down, and after downing someone it keeps
  1100 u off that place for 15 to 30 s, so the others can pick them up;
  generators being repaired are heard now and then (more often close by), an overheating one always: it goes there
  and smashes it if it has progress. It chases whoever it sees, sometimes the long way round (a detour point), and
  aims with an error (it can miss); it keeps after their real position for 1.5 s once out of sight, then goes where
  they were (20 % of the time a wrong guess nearby), looks around and stays on their trail for 14 to 24 s: every 2.5 to
  5 s it hears them (their exact position, when within 900 u), picks up the trail (a spot within 450 u of them) or
  takes a wrong turn, before it gives up. botassist.log tells when it spots, hears and gives up.

Weapons: everyone starts with bare hands (botassist doesn't hand out the room's weapons in this mode). The map's
item boxes (`customwb`, resource/ZHE/zheweaponbox_1..4.json: guns such as the MAC-10, M14 EBR, Glock, Deagle, XM1014,
M60, cannon; the zhebox tools; smoke, flash, sticky bomb) open when walked into (CCustomWeaponBox::CustomWBTouch
0x108e9bb0). Calm humans open the close ones, and farther ones while they lack a gun with ammunition or a grenade.
The zombie can't die but shots slow it down: with it in sight within 700 u the bots take out a gun with
ammunition and shoot it while running backwards (reloading when the clip is empty), throw a grenade at it now and then
(beyond 200 u; it waits 0.9 s after taking it out, its deploy, before pulling the pin), and otherwise carry the knife (faster on foot). An empty gun (clip and reserve) is thrown away off to
the side behind them, a little up, away from where they run, and its weapon box loses its touch so nobody picks the
empty gun up again. The items come from CBasePlayer::DropPlayerItem (0x114b4e60): CSNZ keeps entity pointers as
8-byte handles {control block, pointer} (valid while the block's second dword is set); m_rgpPlayerItems is 6 slots
16 bytes apart from player +0xfb8 (1 primary, 2 pistol, 3 knife, 4 grenades), the chain through the item's m_pNext
handle at +0xd8, the active item's handle at +0x1018. botassist_debug.log lists what each bot carries every 30 s.

Radio: the stock AI's chatter is off (bot_chatter off, its radio commands dropped); the bots say "Enemy spotted"
when they see the zombie, "Taking fire" when hurt, "Need backup" when down, "Affirmative" when they go to pick
someone up or heal them, "Sector clear" when their generator is done and "Go go go" when the exits open (one message every
2.5 s at most).

Using things the way the game checks it: CBasePlayer::PlayerUse (0x114c4ea0) picks, among the usable entities whose
box is within 64 u of the player's origin, the one the view (v_angle) points at best from the eyes, so the bots aim at
the middle of the box. While +use is held, the generator (0x108e9380) and the human module (0x10803140, also hurt
teammates) keep a helper only with no attack button and when 0x11516f90 passes: the target's vfunc 0x128 position
seen from the player's, normalised in 3D, then only x/y against pev->angles, within the cone *(float*)[0x11e49458]
degrees (90 on zhe_heorgen). Too close to something lower (a teammate down) fails it, so the bots step back.
The generator only takes the press while aimed, so +use goes down once the view is on the box; a bot missing from
the generator's repairers (+0x18c, player indexes) after 0.8 s lets go and presses again. When +use doesn't take,
botassist_debug.log gets the numbers once ("+use on the ... does nothing").

Facts: player module by id 0x1087f310 (thiscall player, id; 9 = CZHEHumanModule, state +0x18: 0 healthy, 1 / 2 hurt,
3 down, 5 escaped); CBasePlayer +0x2166 = the zombie; CZHEGenerator +0x1bc state (0..3 progress, 4 repaired,
5 overheating, 6 blown), +0x190 players repairing, +0x1c4 gauge; entvars +0x1fc targetname.
Log lines: "Manhunt: ..." in botassist.log, each bot's task every 10 s in botassist_debug.log.
`resource/ZombieHorrorEscape/ZHE_RuleBonus_Dedi.csv` (score bonuses, a dedicated-server file) is missing offline.

## Zombie 4: Darkness (mode 29, ZB4)

Rules: CSOGameRulesZombie4Reboot (vtable 0x11968564). Day and night alternate (rules +0x6e8, 1 = night, copied to
each player's +0x2eec).

- A zombie killed at night falls groggy (player +0x2eef, 0x11465b00 from CBasePlayer::Killed; it takes no damage) and
  gets up 5 s later (rules death think, vfunc 0x118). Killed by day it dies and respawns 7 s later (rules vfunc 0x1d4)
  through CBasePlayer::PlayerDeathThink 0x114c3cb0 and respawn 0x113fa200 (cdecl pev, copy corpse).
- It dies for good only when the lethal hit, by day and not a grenade, comes from a human with Concentrated Fire on
  (attacker +0x2ef8): the rules' damage hook (vfunc 0x1b8, 0x11462340) sets the victim's +0x2ee8 = 1 and the "can
  respawn" check 0x114bdf00 then says no.
- botassist watches the dead zombies (`Z4Watch`, every 0.5 s from StartFrame). One still down 9 s (groggy) / 11 s
  (dead) after dying, and not killed for good, gets the game's death think twice and then respawn (only while the
  round runs: CSO match +0x27c / +0x27d). botassist.log: "ZB4: zombie ... down", "still down ... ->", "is up again".
- Zombies are tougher than stock: health +30 %, armor x2. The rules' MakeZombie (vfunc 0x28c, 0x11461f40, thiscall
  rules, player, infector, class) gives a first zombie the match's zombie health (CSO match +0x218) and 1000 armor,
  an infected human half its infector's health (at least 2000) and armor (at least 500), x the class night factor.
  botassist wraps it: a first zombie's base health (player +0x2e8c: respawns, groggy and night start from it),
  health, max health and armor are scaled; an infected one inherits half of its scaled infector, so only the floors
  are scaled (2600 health, 1000 armor). botassist.log: "ZB4: ... turns zombie".
- Human skills are client commands handled by the rules' ClientCommand (vfunc 0x24, 0x11461460): `zb4 AdrShot`
  (key 6, Concentrated Fire: adrenaline +0x2f10 >= 30 of 100, burns it until it's gone) and `zb4 Kick` (key 5: a
  trace along the view knocks a zombie back; on a groggy zombie it takes some of its max health and gives
  adrenaline). The zombies' W+W sprint is 0x114616b0 (+0x2eee).
- Human bots (`Z4BotSkills`, every 0.25 s): Concentrated Fire on the nearest zombie in sight by day when it's hurt
  (half health or 1500 or less) or the gauge is nearly full and it's close; at night only to save themselves (full
  gauge, zombie within 350 u). They kick a zombie in their face (half the time) or a groggy one at their feet, with
  the view turned to it for the command and given back. Logged as "ZB4: bot ... Concentrated Fire" (botassist.log)
  and "ZB4 bot ...: kicks" (botassist_debug.log).

## Human bots back off from zombies (PvP zombie modes)

`modes/zombie_humans.inl`, every mode where zombies are players (not co-op/monster modes, Zombie Giant or Manhunt):
a human bot (player +0x2166 clear) with a zombie in sight within `ZombieFleeDistance` (botassist.ini, 450; any
zombie within 200 u even unseen) keeps the stock AI's aim and fire but moves away from them, to a nav area 250 to
900 u off in the direction away from the zombies (scored by how far it leads from them), picked again every 1.5 to
2.5 s, once reached, when the zombies come round, or when stuck; a jump now and then with one within 160 u.
`ZombieFlee=0` turns it off. botassist_debug.log: "zombie flee: bot ... backs off".

Backing off used to kill bots: the path took the drop instead of the stairs, a bot with no path ran straight off a
ledge, and several fell into pits whose floor is a `trigger_hurt` (7 in one Mutation round). Now the retreat spot is
never in a hurt zone, the bot only follows a real nav path to it (else it moves at most a short way straight off,
with drops up to 120 u), and before each step the ground 72 u ahead is probed (`DropAhead`: no floor within the
allowed drop, or a hurt zone below): it turns away, or stands and fights if no way is safe. Jumps only on safe ground.
botassist_debug.log: "zombie flee: bot ... stops at a drop / hurt zone".

Cloaked zombies: the stock AI stops attacking an enemy whose cloaking module (3) is active (FUN_112f34a0, 0x10880320) and
doesn't see one with alpha under 1 (`CCSBot::IsVisible`, 0x11311ff0), so a cloaked zombie bit bots untouched.
`ZombieCloakAim` (these modes plus Conquest and Zombie War): a cloaked zombie in sight within 300 u, with no visible one
closer, is aimed at and shot by botassist after 0.25 s. Backing off needs nothing new: it goes by line of sight.
botassist.log: "zombie flee: bot ... shoots at the cloaked zombie".

## Zombie Conquest (mode 56) with room bots

`modes/zombie_conquest.inl` (rules CSOGameRulesZombiConquest, vtable 0x117919e0; bases, safe areas and weapon sets come
from the game's `resource/ZombiConquest/ConquestInfo.json`, 11 maps: chapel, skyscraper, hacienda, farero, wintershelter,
assault...). T are zombies, CT humans, and they swap every round.
- No zombies: the lobby's teams are merged in this mode, so the room lists every AI player on the host's side, and
  `RoomBotQuota` refused the game's requests for T bots. In this mode the list only says how many bots: each one joins
  the side the game asks for. botassist.log: "the game adds a bot (...): T (zombies), n T and m CT of k".
- No respawn: as in Zombie 4 and Mutation nothing runs a dead bot's PlayerDeathThink once it is respawnable. `ZConWatch`
  (frame hook) runs it for humans and zombies; the game keeps its own wait; after 30 s a forced respawn.
  botassist.log: "ZCon: ... still down".
- Human bots' weapons: only the chosen set can be bought (rules vfunc 0x36c), so the room's weapons are skipped (shared
  `UpdateBuying` returns in this mode) and each life a bot takes a random set of the current base: handlers 0x107af350
  (set) and 0x1080ea30 (ability, from the second base) called directly, because those two commands read their argument
  straight from the engine, not from a bot's fake command; then `item_<id>` for each weapon. botassist.log: "ZCon: human
  bot ... takes weapon set".
- Zombie bots draw a class (`selzmb random`: only those the base allows); both sides use skills (`useitem`) and awakening
  (`ZBCon_UseSpecial` with the gauge, module +0x18, at 100).
- Bots play hard: a room AI player without a skill of its own gets skill 6 of 7 here (`ZCON_SKILL`, shared
  `ApplyRoomSkill`), whatever the room's difficulty. botassist.log: "room skill 6".
- How a base works (FUN_107aa6e0): humans inside and no zombie capture it; a zombie with them stops the clock; no human
  left starts a capture over, or takes a captured base back to free after a while. It scores while a human is inside.
- `ZConMove` replaces only the movement (the stock AI keeps aiming and shooting), always along the nav mesh. The stock AI
  goes straight at a human it sees, so zombies stayed against rails and glass with one across a gap.
  - Both sides go in squads (`ZConSquadThink`; humans one, zombies two from 6 bots on): one way and one rally point on it,
    700 u short of the base for zombies and 500 u for humans. A bot goes there first, a new life too; those there leave
    together once 60% of those waiting have come, or after 8 s (zombies) or 5 s (humans). 25 s waiting at most. A new base
    gathers everyone again. botassist.log: "ZCon: ... squad n gathers at", "squad n attacks: k together".
  - Humans go to the active base and move around in it, close to each other. Once it's captured and two others are inside,
    30% of their spots are around it (up to 180 u out). A zombie close comes first: they back off shooting (`ZombieFleeMove`; within
    300 u inside the base, 420 u outside, each bot a bit different), out of the base if need be, and come back.
  - Zombies go at a human in sight or within 400 u if the way there is under 1500 u (or 2.5 times the distance), and on
    the way to the rally point only at one within 500 u; else to the base and around it, 60% of the time to the closest
    human.
  - Each squad has its own way (`ZConSquadRally`, renewed about every 30 s): often the shortest, or with a stretch of it
    costing more in the A* (`g_navRouteCost`, shared, null elsewhere): the second zombie squad always tries, the first
    40% of the time, the humans 30%. The same rule was checked offline, per bot, on six of the maps' nav meshes (chapel, skyscraper, hacienda, farero,
    wintershelter, assault): a median of 4 different routes in 12 draws per trip (only 1 on some trips), typically 14%
    longer, 2.1 times at most.
  - Stuck: a jump and a step aside, a step back, then that way counts as blocked for this bot. A team mate right ahead
    is passed on one side. Not during the freeze at the start of a round (maxspeed 1): counted as stuck, the ways out of
    the spawn ended up marked as blocked. botassist.log: "ZCon: ... stuck at".
  - `WalkYaw` and `LedgeAhead` (shared, also used by the zombie flee in every PvP zombie mode): `ClearYaw`'s line at knee
    height also hits stairs going up and turned the bot aside; a wall now has to block at head height, and a real ledge
    gets a jump (with the legs up).

## Zombie War (mode 32) with room bots

`modes/zombie_war.inl` (rules CSOGameRulesZombi3ExRenewal, vtable 0x117aaecc, derived from Zombie 2's). T are zombies, CT
humans, in even teams, and a bitten human dies. A round ends when a side has nobody alive, or by time; the sides swap at half
time; zombies wait for a countdown at the start and get stronger in the last 30 s.
- No zombies: as in Conquest the room lists every AI player on the host's side and `RoomBotQuota` refused the T bots, so
  the round ended and restarted over and over. Same fix: each bot joins the side the game asks for.
- The ban/pick menu before each round (`BanPickSystem::Supervisor`, pointer at 0x11e5fb90): humans pick the weapon they
  play and zombies ban one, in turns, with 11 s for each player and then a draw. A bot never answers, so it took 11 s per bot.
  `ZWarWatch` ends a bot's turn at once, so the game draws for it, and shortens the pause before a bot's turn to 0.3 s.
  botassist.log: "ZWar: bot ... ends its turn". The chat's "selection time ... is over" for each bot comes with the draw.
- Human bots' weapons: a player's client buys the pick when the menu closes (`item_<id>`, ammo, grenade, armor); a bot has no
  client. `ZWarBuyPick` does the same for human bots, and shared `UpdateBuying` skips this mode, so the room's weapons
  and filters don't apply here. botassist.log: "ZWar: human bot ... buys its pick".
- Zombie bots stay still for the humans' preparation (20 s from the round's start). Nobody counts as stuck during the menu:
  `ZConMoveAt` also stops for FL_FROZEN (shared with Conquest).
- `ZB2Watch` and `ZB2BotThink` (the same rules as Mutation): dead zombie bots go through the game's death think, zombie bots
  draw a class, both sides use skills.
- `ZWarMove`: Conquest's movement (`ZConMoveAt`) with an objective that moves. Humans hold together around a nav area near
  them, a new one 400 to 900 u off every 35 to 55 s. Zombies gather at their squads' rally points and fall on where the
  humans are. The room's skill applies. botassist.log: "ZWar: n humans and m zombies alive".

## Gun Deathmatch (mode 22) with room bots

`modes/gun_deathmatch.inl` (rules CSOGameRulesGunDeath, vtable 0x11961354). Two teams, respawns, a new weapon every few kills
from strong to weak: 18 levels, 60 kills to the Glock 18, then an HE grenade whose kill wins. Levels from
`resource/GDMWeaponList.cso` (`GDMWeaponListCash.cso` with Lucky Gunman), which name weapon ids (Item.csv column 5, not item ids).
- More weapons: those tables name only 2 or 3 weapons a level. `GdmWatch` rewrites the server's rows (rules +0x2e0 and
  +0x2ec, 0x24 bytes each: level, type, kills, 3 ids, 3 chances) every 2 s with 3 weapons of the level's kind (`GDM_TIERS`, the
  game's order) from `botassist_gdm_weapons.txt` (`gen_gdm_weapons.py`), so any weapon can come at any level up. The last level
  stays the Glock 18. Delete lines from that file to keep a weapon out.
- Bots' weapons: the rules run the buy command as the player with the internal-buy flag (+0x18b4) set. botassist dropped
  that for room AI players and bought the room's weapons (1443 purchases in one test), so bots didn't play the levels. Now
  `UpdateBuying` skips the mode and only the stock AI's own purchases are dropped. botassist.log: "GDM: bot ... reaches level".
- `GdmMove`: bots hunt (the stock AI keeps aiming and shooting): by the nav mesh to the closest enemy, in sight first, and round
  it when close (320 u), with the same safety as Conquest (`ZConSafeYaw`, `WalkYaw`, `LedgeAhead`).
- Routes: enemies a teammate goes for count as 500 u farther each, and most bots go through a place of their own on the way
  (`GdmPickVia`: far from teammates and from where they go, not too far off), so they don't become one group.

## Super Soldier (mode 57) with room bots

`modes/super_soldier.inl` (rules CSOGameRulesSuperSoldier, vtable 0x11789d2c). Team deathmatch with Experts, skills and S.V.P.
boxes. The game has its own AI for it (`resource/supersoldier/supersoldier_bot_bt.xml`, `supersoldier_bot.json`: buying by
Expert, skills, targets), so botassist leaves that alone; movement is `GdmMove` (routes of their own), since their Move node took
everybody to the same target in one group.
- The match waits for players on both sides (0x10778350) and the room lists every AI player on the host's side, so T bots were
  refused. `RoomBotQuota` keeps the sides even here, whatever side the game asks for.
- `UpdateBuying` skips the mode and the AI's purchases aren't dropped. botassist.log: "SS: n T and m CT alive; Experts: ...".

## Partner (mode 50) with room bots

`modes/partner.inl` (rules CSOGameRulesPartner, vtable 0x11781bfc). Bomb defuse where each player has an AI partner with its
own orders (Z menu). The game does all of that (`resource/partner/partner_bot_bt.xml`, one branch per order); what blocked it:
- Partners come through AddBot and `RoomBotQuota` refused them. Rules +0x4f8 holds the owner while one is created
  (`PtnCreatingPartner`), and `PtnIsPartner` keeps them out of `RoomBotOf`, buying and bot orders.
- The mode's bot module never asks for bots, so the room's AI players never came and the round never started. `PtnWatch` adds them
  once a player has a team, keeping 2 client slots per player. A list with one side empty is split between the sides.
- `PtnBotThink` gives room bots the orders a player would (they run the same tree): the carrier plants, the closest one picks
  up the C4 or defuses, the others go to the sites, hold and hunt. Each bot is its own owner: the rules' PlayerKilled skips the base
  code when `ownerIndex` isn't a valid player (no death notice, no score). Their abilities come from their room skill.
- botassist's own radio menu stays closed in this mode: radio1..3 are the partner's orders.
- Room bots have partners too (`PtnPartners`, the game's 0x10744560, class drawn by team), named "Class (Owner)"; a bot's partner
  falls with it, as a player's does. Human partners get that name as well.
- `PtnMove`: the tree's path following has no unsticking and bots spawn packed, so room bots and partners walk with `GdmWalk`
  (extracted from `GdmMove`) unless an enemy is in sight within 1000 u or they reached the end of the way (planting, defusing).
- CTs split between the bomb sites and hold them; Ts go for the round's site, some hunting.
- Radio: `PtnSay` calls the game's own radio (0x114c7700, message and sound), at most once in 9 s a bot and 2.5 s overall.
- Score list: client.dll hid in this mode every player whose first info field is 0 (meant for partners), which is everybody
  offline, so the list was empty. `apply_patches.py` removes the check in its three places (0x029FFA42, 0x02A01007, 0x02A0311A).
- Pre-round: the game's default config has mp_freezetime 0; `PtnWatch` sets 6 s, and mp_buytime 0.75 if shorter.
- botassist.log: "Partner: n T and m CT alive, p players, q partners, bomb state s", "the game adds a player's partner".

## Beast (mode 27) with room bots

`modes/beast.inl` (rules CSOGameRulesSnowman, vtable 0x117a35d4; "Snowman" in the game's files). Teams, 3 s respawn; ampoules at each
respawn and from bodies; 3 of them and E make a beast. On sm_blizzard a team wins by destroying the other's core, open for 20 s
once its controller falls; DM_port and zm_requiem are team deathmatch. The game has no AI for it (stock CCSBots):
- `RoomBotQuota` evens the sides, since the room lists its AI players on one and T bots were refused.
- `InstallBeastBotClass`: crystals (ampoules) and cores only take players of class 2 and a CCSBot's is 15, so bots stood on the
  crystals without getting them and couldn't hurt a core. CCSBot's vfunc 0x30 answers 2 in this mode only.
- `BeastBotThink` transforms bots like a player's E (rules vfunc 0x1f0): low health, a beast near, or after 6-20 s with 3 ampoules.
- `BeastIsBeast` keeps beasts out of the room's weapons and character (`UpdateBuying`, `OnBotCommand`, the bot hook).
- `DriveBeast`: humans fetch ampoules close by; roles by player index (2 of 5 attack the enemy controller, 2 hunt, 1 defends its own
  controller; with the barrier down all but the defenders go for the core). Objectives get shot within 700 u or punched up close.
  Beasts run at enemies, punch, stomp (attack2) and roll (double W through the buttons). Routes and unsticking are Gun Deathmatch's
  (`GdmPickVia`, `GdmWalk`). botassist.log: "Beast: n T and m CT alive..., core and controller health", "turns into a beast", "stuck at".

## Item Battle (mode 19) with room bots

`modes/item_battle.inl` (rules CSOGameRulesTDMItemRenew, vtable 0x117a54b4). Team deathmatch with item boxes; two item slots, used with E.
- Maps: the list is MapList.csv's `tdm_item` column. A box table (`maps/FUN_ITEMBOX_<map>`) exists for only 6 maps, so `gen_item_battle.py`
  writes one for the others in `tables/maps` (`botassist_tables` replaces the game's table): open spots on the nav mesh's main part (96 units
  wide, not dead ends, one level when possible), spread out, 36 units over the floor like the game's, 2 special boxes near the center. `gen_item_battle_maps.txt` lists the 70 maps; delete a
  line and regenerate to leave one out. The server's `IsMapValid` takes that column for this mode.
- The game has no AI for it (stock CCSBots). `RoomBotQuota` evens the sides. `DriveItem`: a bot with no item goes to the closest box, one with an item only to one within 400 u,
  2 bots a box at most, with a time limit (leaves it 60 s; 3 failures leave it for all, 180 s), shooting barricades on the way, else hunting (Gun Deathmatch's `GdmPickVia`, `GdmWalk`). `ItemUse` presses E as a player does (rules vfuncs 0x238 and 0x23c): attack
  items at a visible enemy, shield, radar and invisibility when one shows up, teleport toward a far one, rocket jump and portal when idle.
- `ItemHasSpecial` keeps a Power AT4CS or Ripper from being replaced by room weapons. botassist.log: "Item Battle: ... boxes ready", "uses item".

## All Star (mode 40) with room bots

`modes/all_star.inl` (rules CSOGameRulesAllStar, vtable 0x1179d918). Team deathmatch where each life is one of 20 characters.
- `resource/allstar/AllStar_Status-Dedi.csv` and `AllStar_Skill-Dedi.csv` are only read on a dedicated server and never shipped: characters had
  0 health and no model. `InstallAllStarTables` removes the check (0x1082bfb4, 0x1082bd0b) and `gen_all_star.py` writes the files (`tables/resource/allstar`);
  hw.dll's plain .csv loader (0x27584f0) is detoured like the .cso one, so they come from `botassist_tables`.
  Nexon's numbers are unknown: health and armor follow a video of the mode, and the attacks each role. Third person view: on for all but the soldiers
  (1, 2, 3, 6) and light zombies (4, 5, whose arms the video shows). The camera is client.dll's MPToCL 0xbf: cam_idealyaw, cam_idealdist, pitch, offset.
  The Skill table isn't multipliers: `SKILLS` in the generator says what each pawn does with its numbers (damage, radius, speed, seconds).
  The rules' +0x2a8 (room option, vfunc 0x28c) makes characters go by team and kills (0x1082a7c0): patched off. Normally each player draws at 0x1082c760.
  The explosion radius of `BaseMissile` (Revenant's fire, XT-300's rockets) is in meters: x39.37 (0x10911f70). 250 there reached the whole map.
  Hector has 2 skins, red and blue: column 20 is the skin. Fake players fail 0x114bd6a0, so the rules don't give them speed and gravity (they kept
  -1 with the zombies' knife, 0 with the bazooka, and stood still): `AsFixSpeed`. `AsGround` lowers the loaded models that float (kart, ball,
  chicken, snowman: lowest point z -18 against the others' -36), through GetModelPtr, whose data the client shares here. The light zombies' knife
  does no damage, cause unknown: `AsClaw` hits with the pawns' melee function when they press attack. `AsUnstack`: spawn points run out with 32 players.
- Characters are state of `AllStarPlayer`, which bots aren't, so room AI players join as fake players, Zombie Giant's way (`CreateZgPlayer`, now on the emptier
  side here). `DriveAllStarPlayer`, run from `AsWatch` (no stock bot runs to call it): hunting with `GdmPickVia`/`GdmWalk`, `AS_CLASSES` says how each class fights, melee characters run at the enemy,
  others circle, aim by room skill, skills through 0x10664940. botassist.log: "All Star: n T and m CT alive...; characters", "spawns as character", "stuck at".

## Human Scenario (mode 17) with room bots

`modes/human_scenario.inl` (rules CSOGameRulesHumanScen, vtable 0x11962378). It runs on the zombie scenario's code and is in `CoopModes`, so the companion AI
plays it. Soldiers come from the maps' `zombiespawn` points as monsters. The aircraft of `hs_blaze` are `airbot_drone` and `airbot_osprey`, which the
`Ignore` list leaves out (friendly in other modes), so bots didn't shoot them and weren't even driven until something else appeared. The shared code asks
the mode's functions: `HsIsEnemyClass` (`IsIgnored`; the homing missile stays ignored), `HsScanRadius` (6000 here against `Radius`), `HsFlying` (`AimPoint`,
no backing away), `HsClearShotAt` (firing). botassist.log: "Human Scenario: enemy <class> <model>, health..., bots shoot it".

## Create room: Random Map in fun modes

The dialog takes the mode from the selected map tile (+0x14) or, if 0, from the mode tab (GameUI 0x10571030). The random tile has none, so a fun mode became the tab's
mode (Zombie Mutant from Item Battle). `HookedSelectedMode` takes it from the list's other tiles. The server accepts any map id for mode 19, since the host draws the map.

## The room's AI difficulty text

The room's panels show the room's difficulty (`#CSO_Bot_Difficulty_<n>`), not what the AI players page set. `RoomBotLevelText`
(on botassist's ILocalize hook, only for calls from GameUI 0x10442bd0 and 0x10446080) answers with the bots' common skill or
"Random" if they differ. Bots that keep the room's skill keep the room's text. It reads `GET /api/roombots`.

## Deadly places in botassist's paths (shared)

`UpdateHurtZones` lists the map's solid `trigger_hurt` brushes with damage (entvars solid +0x11c, dmg +0x210, from
mp.dll's entvars field table) every 5 s, and marks the nav areas whose centre lies in one. The shared A* (`FindPath`:
companions, Manhunt, Zombie Giant, zombie flee) leaves those areas out (unless one is the goal), never takes a drop
above 400 u (about half a player's health) and charges 4 per unit of drop beyond 64 u, so stairs or a ramp win over
a jump off a balcony. botassist.log: "hurt zones (<map>): N trigger_hurt ..., M of K nav areas left out of paths".

## Classic crossbow crash (every mode)

The classic crossbow (item 8977) fires "crossbowcls_bolt", a class this mp.dll doesn't have: CBaseEntity::Create
returns null and the attack (0x10c495a0) writes through it (crash at mp+0xc49926, from the bot's ItemPostFrame).
Two vtables carry that bolt name in slot 0x344: CCrossBowCls (0x1183991c, the weapon_crossbowcls entity) and
CSimpleWpn<CCrossBowCls> (0x118391fc, what the item factory 0x10c43c20 makes when it's bought by item id, as the
bots' room loadouts do). botassist points both to CCrossbow's "crossbow_bolt" (0x1100ead0) at startup.

## Zombie class skills: the skill table (every zombie mode)

Zombie skills read their values from hw.dll's ZombieSkillTable (the engine's data manager `[0x121d3cfc]()` =
SharedDataMgr 0x3f25310, vfunc 0x1c8 -> +0x1778, vtable 0x2c21ac4; vfunc 8 looks a row up by mode, evolution level
0x114b9ed0 and round, with fallbacks down to mode 14 / level 0 / round 0). Its loader (vfunc 1, 0x26af410) reads 32
files `resource/zombi/ZombieSkillProperty_Dedi/ZombieSkillProperty_<skill>.csv`, only on a dedicated server, and the
files were never shipped: offline the table was empty. So `useitem zb_skill0` did nothing (no zombie, player or bot,
could use its skill), and the skill thinks that read their row every frame without checking crashed the game when
such a class was picked (FUN_10948aa0 classes 14/15 at mp+0xa28d63, FUN_10947710 the Flying zombie at mp+0x94791a).

`LoadZombieSkillTable` (at startup, after the file overrides) runs the loader past its dedicated-server check
(0x26af420), and the files come from `botassist_tables` (`gen_zombie_skills.py` writes them to
`tables/resource/zombi/ZombieSkillProperty_Dedi`; apply_patches.py copies them). Each file needs every column the
game reads (a missing one throws); the generator's docstring lists which class uses which skill. Nexon's values are
unknown: the generator follows the wiki where it can and uses moderate values elsewhere. botassist.log: "zombie skill
table: N rows" (96 expected: 32 skills x 3 evolution levels).

The earlier guard stays as a fallback: FUN_10948aa0 and FUN_10948f90 are detoured (thiscall obj, player) and skipped
while their row is missing ("zombie class N skill ... skipped" in botassist.log).

## Zombie 2: Mutation (mode 9) with room bots

`modes/zombie2_mutation.inl` (rules CSOGameRulesZombi2, ClientCommand vtable slot 0x2c = 0x1144dcc0):
- Zombie bots stayed dead: as in Zombie 4, nothing runs a dead bot's CBasePlayer::PlayerDeathThink once it is
  "respawnable" (dead flag 3). `ZB2Watch` (frame hook, every 0.25 s) runs it for a bot zombie respawnable for a
  second or still dead 10 s after dying; the game decides the rest. botassist.log: "ZB2: zombie ... still down".
- A new zombie bot sends `selzmb random` (rules vfunc 0x454 -> 0x4e0 = 0x1144f3c0, once per zombie life: player
  +0x2e39). The draw only takes classes the player owns (player +0x2171 + class): a new player owns 0, 12, 13 and
  18..21, and the draw leaves out 12, 13, 18..20 and 22..26, so bots were always Regular or class 21. Bots get all 27
  flags first (once the skill table has rows); the mode's own class rules still apply.
- `useitem zb_skill0` (zombie's G skill; a quarter of the time `zb_skill1`, the second skill some classes have) with
  a human in sight within 700 u, `useitem hm_skill0` (human's first skill) with a zombie within 350 u, every 4 to 8 s
  half of the time (0x1144f710 -> 0x11450190 checks the class, cooldowns and costs). botassist.log: "ZB2: bot ...
  uses".

The frame hook (StartFrame 0x113f8a80 detour, `HookedStartFrame`) is shared code in botassist.cpp: it runs the
console, the command file and each mode's watcher (ZSRiftWatch, Z4Watch, ZB2Watch, ZgFrame), each of which returns
at once outside its mode.
