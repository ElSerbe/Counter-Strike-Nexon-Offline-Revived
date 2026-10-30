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
- The top bar's skill list sets the skill of the bots it adds (room skill or 0..7).

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
`ZSRiftBossHealth` (percent per level, 1.5 / 5 / 1 / 4) and sets the exponent (mp 0x11945470, GameUI 0x1096ff50, read
only there) to `ZSRiftLevelExponent` (1: the same step every level). Armour and speed keep the game's rates.

The room panel redraws itself (and the tooltip) after a settings change only when the changed options include bit 59
(GameUI 0x105bcd10 ands them with the mask at 0x10aff218; the bits are the server's room flags low / lowMid / highMid /
high). A new rift level (lowMid ZSDIFFICULTY, bit 45) left the tooltip on the room's first level: botassist points
that load (0x105bcd2b) at its own mask with bit 45 added. Rift levels unlock one by one on the server (Data/ZSRift.json
`UnlockByClearing`): the highest one unlocked is the best clear + 1, sent in Packet_ZSRift type 0.
