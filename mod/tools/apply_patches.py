"""Apply (or revert) the offline patches to the CSNZ Bin folder.

Every patch records the VA, the bytes expected in the original file, and the
replacement bytes. Before writing, the original bytes are checked, so the script
refuses to touch a file that does not match the backup.

Usage:
  python apply_patches.py <path-to-Bin> apply
  python apply_patches.py <path-to-Bin> revert   (copies files back from backup/original/Bin)

Besides the byte patches, "apply" copies the mod's own files into Bin (EXTRA_FILES) and "revert" removes them.
"""
import shutil
import sys
from pathlib import Path

import pefile

MOD = Path(__file__).resolve().parents[1]
BACKUP_BIN = MOD / "backup" / "original" / "Bin"

# mod files copied into Bin: (source, name in Bin, overwrite an existing copy). botassist.asi is loaded by Miles
# Sound System (it loads every *.asi in Bin) and lets bots fight monsters, see tools/botassist.
EXTRA_FILES = [
    (MOD / "tools" / "botassist" / "botassist.asi", "botassist.asi", True),
    (MOD / "tools" / "botassist" / "botassist.ini", "botassist.ini", False),  # keeps the player's settings
    (MOD / "tools" / "botassist" / "botassist_weapons.txt", "botassist_weapons.txt", False),
    (MOD / "tools" / "botassist" / "botassist_characters.txt", "botassist_characters.txt", False),
    (MOD / "tools" / "botassist" / "botassist_gdm_weapons.txt", "botassist_gdm_weapons.txt", False),
    # data tables botassist loads instead of the game's (gen_zsrift_waves.py: Zombie Disaster Zone's zombie mix)
    (MOD / "tools" / "botassist" / "tables" / "maps" / "ZBS_WAVE_zsrift_splinter.csv",
     "botassist_tables/maps/ZBS_WAVE_zsrift_splinter.csv", True),
    # Zombie Disaster Zone's random round events (gen_zsrift_events.py), a dedicated-server file the game lacks
    (MOD / "tools" / "botassist" / "tables" / "maps" / "ZBS_ENC_zsrift_splinter_dedi.json",
     "botassist_tables/maps/ZBS_ENC_zsrift_splinter_dedi.json", True),
]
# Item Battle's item boxes on the maps the game has no table for (gen_item_battle.py)
EXTRA_FILES += [
    (f, "botassist_tables/maps/" + f.name, True)
    for f in sorted((MOD / "tools" / "botassist" / "tables" / "maps").glob("FUN_ITEMBOX_*.csv"))
]
# All Star's character numbers (gen_all_star.py), dedicated-server files the game lacks
EXTRA_FILES += [
    (f, "botassist_tables/resource/allstar/" + f.name, True)
    for f in sorted((MOD / "tools" / "botassist" / "tables" / "resource" / "allstar").glob("*.csv"))
]
# the zombie class skills' values (gen_zombie_skills.py), dedicated-server files the game lacks
EXTRA_FILES += [
    (f, "botassist_tables/resource/zombi/ZombieSkillProperty_Dedi/" + f.name, True)
    for f in sorted((MOD / "tools" / "botassist" / "tables" / "resource" / "zombi" / "ZombieSkillProperty_Dedi").glob("*.csv"))
]

# (file, va, expected original bytes, patched bytes, note)
def _rel32(src_next: int, dst: int) -> bytes:
    return (dst - src_next).to_bytes(4, "little", signed=True)


# BlackCipher init (0x2569ef0) must still run: it initializes critical sections used later
# (e.g. 0x3f248c0). Early-returning from it caused an access violation. Instead the caller at
# 0x249a01f calls a stub at 0x256a0e0 that runs the original init and then forces success.
_STUB = 0x256A0E0
_CALLER = 0x249A01F
_INIT = 0x2569EF0

# Developer console (see the console entries in PATCHES)
_CONSOLE_FALLBACK = 0x23DA56B  # public-buffer executor: call to the whitelisted-cvar fallback
_CONSOLE_STUB = 0x1D37B20      # 32 int3 bytes between a string and a function
_PORT_CAVE = 0x1D2B3D0         # inside 59 int3 bytes at 0x1d2b3c5: the game-server socket's port check
_UDP_SET = 0x24A7780           # sets the UDP holepunch target (sockaddr_in at 0x2e1aa50) from (ip, port)
_UDP_CAVE = 0x1D454B0          # inside 2902 int3 bytes at 0x1d454aa: the same port rule for that target
_CBUF_ADDTEXT = 0x23D8710
_COMPLETION_ON_TEXT = 0x1061AF56    # GameUI: console text changed -> rebuild completion
_COMPLETION_ON_SUBMIT = 0x10618ED3  # GameUI: command added to history -> rebuild completion
_COMPLETION_WHITELIST = 0x1061B110
_COMPLETION_FULL = 0x1061B6D0

# GameUI.dll survey dialog (character select): see the GameUI entries in PATCHES
_SURVEY_NOT_OK = 0x105A219F    # non-"OK" branch of CUserSurveyDlg::OnCommand
_SURVEY_EPILOGUE = 0x105A21A7  # OnCommand epilogue right after that branch's call
_SURVEY_CAVE = 0x101A889B      # 21 int3 bytes after a noreturn call
_SURVEY_ANCHOR = _SURVEY_NOT_OK + 5  # return address pushed by the stub's call $+5
_SURVEY_PAD = 0x105A21B9       # int3 padding right after OnCommand's ret 4 (7 bytes)

# Web shop page served by CSNZ_Server (see the GameUI shop entries in PATCHES)
SHOP_URL = "http://127.0.0.1:30080/shop"
_SHOP_URL_VA = 0x10978C80      # "https://nxpay.nexonclub.com/..." (69 chars + NUL)

PATCHES = [
    (
        "hw.dll",
        _CALLER,
        b"\xe8" + _rel32(_CALLER + 5, _INIT),              # call 0x2569ef0 (original)
        b"\xe8" + _rel32(_CALLER + 5, _STUB),              # call stub
        "BlackCipher init: caller uses stub that runs original init then reports success",
    ),
    (
        "hw.dll",
        _STUB,
        b"\xcc" * 8,                                        # int3 padding (checked before writing)
        b"\xe8" + _rel32(_STUB + 5, _INIT) + b"\xb0\x01\xc3",  # call 0x2569ef0; mov al,1; ret
        "BlackCipher stub: original init, then return 1",
    ),
    # Anti-cheat (NGClient.aes ordinals, which start BlackCipher; it phones home to psm-log.ngs.nexon.com and
    # S3) is never started or called. NGClient is virtualized, so instead of guessing how it behaves when
    # uninitialized, hw.dll simply stops calling it. hw.dll is loaded at another base, so every call
    # "ff 15 [abs]" keeps its relocated operand: only the 2 opcode bytes become "jmp short +4" over it.
    (
        "hw.dll",
        0x2569F3B,
        bytes.fromhex("6a006a0168"),        # push 0; push 1; push ... (args of NGClient #1 = start)
        b"\xe9" + _rel32(0x2569F40, 0x2569FD9),  # jmp to the init's success exit (critical section already set up)
        "Anti-cheat: NGClient never started (init returns success right after its critical section)",
    ),
    (
        "hw.dll",
        0x2569FF6,
        bytes.fromhex("ff15"),              # call [NGClient #2] (stop)
        bytes.fromhex("eb04"),
        "Anti-cheat: NGClient stop not called",
    ),
    (
        "hw.dll",
        0x2569EAE,
        bytes.fromhex("ff15"),              # call [NGClient #4] (3 args, cleaned by the add esp,0xc after it)
        bytes.fromhex("eb04"),
        "Anti-cheat: NGClient #4 not called",
    ),
    (
        "hw.dll",
        0x256A1DB,
        bytes.fromhex("ff15"),              # call [NGClient #5] (query; #6 is only called when it returns true)
        bytes.fromhex("eb04"),
        "Anti-cheat: NGClient #5 not called",
    ),
    (
        "hw.dll",
        0x256A1E1,
        bytes.fromhex("84c00f84cd000000"),  # test al,al; je 0x256a2b6
        b"\xe9" + _rel32(0x256A1E6, 0x256A2B6) + b"\x90\x90\x90",  # always skip (#6 never called)
        "Anti-cheat: NGClient #5/#6 branch always skipped",
    ),
    # hw.dll's only URL downloader (WinINet InternetOpenUrlA at 0x23ecb38) fetched the shop/lobby promotion
    # images from Nexon's S3 bucket; the URL list comes from packed resources, not from the binaries. Every
    # download now takes the downloader's own "open failed" path (error 6) before any connection is made.
    (
        "hw.dll",
        0x23ECB22,
        bytes.fromhex("56ffb5f0ee"),        # push esi; push [ebp-0x1110] (first 4 of its 6 bytes)
        b"\xe9" + _rel32(0x23ECB27, 0x23ECB48),  # jmp to: push 6; jmp error handler
        "Web downloads (promotion images from Nexon S3) always fail locally, nothing is fetched",
    ),
    (
        "NGClient.aes",
        0x5E5CE0C0,
        bytes.fromhex("558bec837d0c0175"),  # DllMain CRT startup: push ebp; mov ebp,esp; cmp [ebp+0xc],1; jne
        bytes.fromhex("b801000000c20c00"),  # mov eax,1; ret 0xc  (loads as inert data; no TLS callbacks)
        "Anti-cheat removed: NGClient.aes (static import of hw.dll) runs no code at load, nothing calls it",
    ),
    # Login request at hw.dll 0x24f74b0 is NOT patched anymore: the fake "login success" stopped the
    # client from sending its login packet to CSNZ_Server. Original bytes: 55 8b ec 53 56.
    (
        "hw.dll",
        0x24F7860,
        bytes.fromhex("558bec6aff"),            # push ebp; mov ebp,esp; push -1
        bytes.fromhex("c20400"),                # ret 4  (server-message handler, vtable slot 1 of 0x2bef250: no popups)
        "Server message handler (ServerDown popup etc.) suppressed",
    ),
    (
        "hw.dll",
        0x23088D5,
        bytes.fromhex("7535"),                  # jne 0x230890c  (server check failed -> popup + quit path)
        bytes.fromhex("eb35"),                  # jmp 0x230890c  (always take the success path)
        "Server availability check: failure branch (popup + quit at 0x23088d7) skipped",
    ),
    (
        "NexonAnalytics32.dll",
        0x1015E2FF,
        bytes.fromhex("558bec837d0c0175"),  # DllMain prologue: push ebp; mov ebp,esp; cmp [ebp+0xc],1; jne
        bytes.fromhex("b801000000c20c00"),  # mov eax,1; ret 0xc  (DllMain returns TRUE; no analytics init)
        "NexonAnalytics32 DllMain: skip load-time analytics init (LoadLibrary hang from Log File Opened)",
    ),
    (
        "hw.dll",
        0x23089C1,
        bytes.fromhex("755e"),              # jne 0x2308a21
        bytes.fromhex("eb5e"),              # jmp 0x2308a21
        "Server text MessageBoxW (site 0x23089e7) always skipped",
    ),
    (
        "hw.dll",
        0x24F669C,
        bytes.fromhex("8bf083feff7545"),    # mov esi,eax; cmp esi,-1; jne 0x24f66e8  (inet_addr result / DNS fallback)
        bytes.fromhex("be7f000001eb45"),    # mov esi,0x0100007f; jmp 0x24f66e8  (always 127.0.0.1, string still released)
        "Host resolver 0x24f6670 always returns 127.0.0.1 (server list mixes external IPs and unresolvable names)",
    ),
    (
        "hw.dll",
        0x2754F80,
        bytes.fromhex("8b450852ff710489410c668b450c6689410a"),  # mov eax,[ebp+8]; push edx; push [ecx+4]; mov [ecx+0c],eax; mov ax,[ebp+0c]; mov [ecx+0a],ax
        # push edx; push [ecx+4]; mov dword [ecx+0c],127.0.0.1; call the port check below; nop; nop
        bytes.fromhex("52ff7104c7410c7f000001") + b"\xe8" + _rel32(0x2754F90, _PORT_CAVE) + b"\x90\x90",
        "Game-server socket: always 127.0.0.1; port 30002 unless a server transfer asks for one of the mod's ports "
        "(30002-30199, the Steam bridge). Callers: the first connect (port 0 without -lobbyport) and Packet_Transfer",
    ),
    (
        "hw.dll",
        _PORT_CAVE,
        b"\xcc" * 31,                       # int3 padding (checked before writing)
        bytes.fromhex(
            "668b450c"      # mov ax,[ebp+0c]      the port asked for (network order)
            "86c4"          # xchg al,ah
            "663d3275"      # cmp ax,30002
            "720a"          # jb force
            "663df775"      # cmp ax,30199
            "7704"          # ja force
            "86c4"          # xchg al,ah           back to network order
            "eb04"          # jmp store
            "66b87532"      # force: mov ax,htons(30002)
            "6689410a"      # store: mov [ecx+0a],ax
            "c3"),          # ret
        "Game-server socket port check (code cave in int3 padding)",
    ),
    (
        # The game's UDP holepunch goes to a sockaddr_in at 0x2e1aa50 set by 0x24a7780(ip, port) after each connect
        # (0x24f698e the first, 0x2672f5d a server transfer). The first connect passes port 0 (no -lobbyport): the
        # holepunch went to port 0 and never reached CSNZ_Server, so neither the server nor the Steam bridge knew the
        # game's UDP ports (players from another PC hung at "joining"). Only after a transfer did it reach 30002.
        "hw.dll",
        _UDP_SET + 0x20,
        bytes.fromhex("5dc3cccccc"),        # pop ebp; ret; padding
        b"\xe9" + _rel32(_UDP_SET + 0x25, _UDP_CAVE),  # jmp to the port check (the stores before it are untouched)
        "UDP holepunch target: same port rule as the game-server socket (port 0 -> 30002)",
    ),
    (
        "hw.dll",
        _UDP_CAVE,
        b"\xcc" * 37,                       # int3 padding (checked before writing)
        bytes.fromhex(
            "668b450c"      # mov ax,[ebp+0c]        the port asked for (network order)
            "86c4"          # xchg al,ah
            "663d3275"      # cmp ax,30002
            "7206"          # jb force
            "663df775"      # cmp ax,30199
            "7611"          # jbe done               one of the mod's ports: kept
            "e800000000"    # force: call next
            "5a"            # next: pop edx          (edx = this address, wherever hw.dll is loaded)
            "8b92") + _rel32(_UDP_CAVE + 23, _UDP_SET + 0x1C) + bytes.fromhex(  # mov edx,[relocated slot of the port's address]
            "66c7027532"    # mov word [edx],htons(30002)
            "5d"            # done: pop ebp
            "c3"),          # ret
        "UDP holepunch target port check (code cave in int3 padding)",
    ),
    (
        "hw.dll",
        0x2B7F4B4,
        b"NexonAnalytics32.dll",           # LoadLibraryA name used only by hw.dll's analytics init (0x2051f51)
        b"NexonAnalyticsXX.dll",           # missing DLL -> hw takes its own "NexonAnalytics_Init Failed" path
        "NexonAnalytics never loaded: with its DllMain stubbed, its zeroed critical sections crashed the client "
        "(RtlpWaitOnCriticalSection) right after connecting",
    ),
    (
        "hw.dll",
        0x24F69EE,
        bytes.fromhex("753c"),              # jne 0x24f6a2c  (analytics init ok -> continue)
        bytes.fromhex("eb3c"),              # jmp 0x24f6a2c  (never show the "NexonAnalytics Init Failed" fatal error)
        "Analytics removed: init failure is no longer fatal (no analytics at all in this build)",
    ),
    # Developer console with every command, as in CS 1.6. GameUI's console sends the typed line through
    # cl_enginefuncs +0x29c (0x23a8bc0) to Nexon's "public" command buffer, whose executor (0x23da3a0) only knows
    # the Pcmd list (vxl_do, cl_dt, cl_ut) and, failing that, a short cvar whitelist (0x23e33a0: cl_showfps,
    # cl_headname...). The engine's own buffer (Cbuf_AddText 0x23d8710, run by Cbuf_Execute 0x23d8760) has every
    # command, alias and cvar and forwards the rest to the server. Instead of the whitelist, the executor now hands
    # the line (its buffer at [ebp-0x404]) to that buffer. The stub only uses relative calls (hw.dll is relocated).
    (
        "hw.dll",
        _CONSOLE_FALLBACK,
        b"\xe8" + _rel32(_CONSOLE_FALLBACK + 5, 0x23E33A0),  # call the whitelisted-cvar fallback
        b"\xe8" + _rel32(_CONSOLE_FALLBACK + 5, _CONSOLE_STUB),
        "Console: lines the public command list doesn't know go to the engine's full command buffer",
    ),
    (
        "hw.dll",
        _CONSOLE_STUB,
        b"\xcc" * 24,                                       # int3 padding between a string and a function
        bytes.fromhex("8d85fcfbffff50")                     # lea eax,[ebp-0x404]; push eax  (the executor's line)
        + b"\xe8" + _rel32(_CONSOLE_STUB + 12, _CBUF_ADDTEXT)  # call Cbuf_AddText
        + bytes.fromhex("6a0a54")                           # push 0x0a; push esp  ("\n" on the stack)
        + b"\xe8" + _rel32(_CONSOLE_STUB + 20, _CBUF_ADDTEXT)  # call Cbuf_AddText
        + bytes.fromhex("83c40cc3"),                        # add esp,0xc; ret
        "Console stub: Cbuf_AddText(line); Cbuf_AddText(\"\\n\")",
    ),
    # The console's completion list (CGameConsolePanel, both __thiscall(text), ret 4) was built from that cvar
    # whitelist (0x1061b110, via cl_enginefuncs +0x2a0); Valve's builder over every command (+0x19c..+0x1a4) and
    # every cvar (+0x198, next at +0x14) is still in GameUI (0x1061b6d0) but unused. Both callers now use it.
    (
        "GameUI.dll",
        _COMPLETION_ON_TEXT,
        b"\xe8" + _rel32(_COMPLETION_ON_TEXT + 5, _COMPLETION_WHITELIST),
        b"\xe8" + _rel32(_COMPLETION_ON_TEXT + 5, _COMPLETION_FULL),
        "Console completion while typing: every command and cvar",
    ),
    (
        "GameUI.dll",
        _COMPLETION_ON_SUBMIT,
        b"\xe8" + _rel32(_COMPLETION_ON_SUBMIT + 5, _COMPLETION_WHITELIST),
        b"\xe8" + _rel32(_COMPLETION_ON_SUBMIT + 5, _COMPLETION_FULL),
        "Console completion after a command (history): every command and cvar",
    ),
    # Character-select survey (CUserSurveyDlg::OnCommand 0x105a1fc0): any button other than "OK" quits the
    # game instead of closing the dialog, so the player can't stay stuck logged out. The quit call is the one
    # GameUI uses for "QuitNoConfirm": engine ClientCmd at [0x10b844c8]+0x54 with "quit\n" at 0x10938a40.
    # The game maps GameUI.dll at a different base than 0x10000000, so the stub can't use absolute addresses:
    # it takes its own runtime address with call/pop and reaches both globals relative to it. No gap is big
    # enough, so it is split: the call site (8 bytes) -> the int3 padding after OnCommand (5) -> a 21-byte gap.
    (
        "GameUI.dll",
        _SURVEY_NOT_OK,
        bytes.fromhex("568bcbe8794f1600"),  # push esi; mov ecx,ebx; call 0x10707120 (BaseClass::OnCommand -> closes)
        bytes.fromhex("e800000000"          # call $+5          (pushes the runtime address of _SURVEY_ANCHOR)
                      "58")                 # pop eax           (eax = _SURVEY_ANCHOR, relocated)
        + b"\xeb" + bytes([_SURVEY_PAD - (_SURVEY_ANCHOR + 3)]),  # jmp short to the padding
        "Survey dialog: Cancel/Close quits the game (character select can't be dismissed)",
    ),
    (
        "GameUI.dll",
        _SURVEY_PAD,
        b"\xcc" * 5,
        b"\xe9" + _rel32(_SURVEY_PAD + 5, _SURVEY_CAVE),  # jmp quit stub
        "Survey dialog: hop from OnCommand's padding to the quit stub",
    ),
    (
        "GameUI.dll",
        _SURVEY_CAVE,
        b"\xcc" * 21,
        b"\x8b\x88" + (0x10B844C8 - _SURVEY_ANCHOR).to_bytes(4, "little", signed=True)  # mov ecx,[eax+d] (engine funcs)
        + b"\x05" + (0x10938A40 - _SURVEY_ANCHOR).to_bytes(4, "little", signed=True)    # add eax,d ("quit\n")
        + bytes.fromhex("50"                 # push eax
                        "ff5154"             # call [ecx+0x54]   (ClientCmd, cdecl)
                        "59")                # pop ecx           (drop the argument)
        + b"\xe9" + _rel32(_SURVEY_CAVE + 21, _SURVEY_EPILOGUE),  # jmp back to OnCommand's epilogue
        "Survey dialog: position-independent quit stub (engine ClientCmd \"quit\")",
    ),
    # Shop rework (medals / battle passes): the taskbar's Shop button opens the CEF web event window
    # (CWebEventDlg) instead of the native shop, and that window loads our local shop page.
    # CSOTaskbar::OnCommand("openShopDlg") (0x10596d40): after the connection check, the 0x10597428 branch now
    # calls CCSOMainPanel::OpenWebEvent (0x105a9020) on the main panel stored in [0x10b844fc]. GameUI is mapped
    # at another base, so the absolute operand sits exactly where the original "mov eax,[0x10b844c8]" had its
    # relocation (+1): the loader rebases our address the same way.
    (
        "GameUI.dll",
        0x10597426,
        bytes.fromhex("747a"),              # je 0x105974a2 (native shop page 13)
        bytes.fromhex("9090"),              # always take the branch below
        "Shop button: skip the native shop page",
    ),
    (
        "GameUI.dll",
        0x10597428,
        bytes.fromhex("a1c844b8108b8090020000ffd08bc88b10"),
        b"\xa1" + (0x10B844FC).to_bytes(4, "little")       # mov eax, [main panel]   (relocated slot)
        + b"\x8b\xc8"                                        # mov ecx, eax
        + b"\xe8" + _rel32(0x10597434, 0x105A9020)          # call CCSOMainPanel::OpenWebEvent
        + b"\xe9" + _rel32(0x10597439, 0x1059748E),         # jmp OnCommand epilogue
        "Shop button: open the web shop window (CWebEventDlg) instead",
    ),
    # CWebEventDlg::CreateWebViewPanel loads LoadURL("about:blank", 11); point it at our shop URL, stored over
    # the Nexon cash-charge URL (unused offline, single reference). The pointer keeps its relocation slot.
    (
        "GameUI.dll",
        _SHOP_URL_VA,
        b"https://nxpay.nexonclub.com/NxChargeCash/NxCash.asp?NXID=%s&CHID=NCSO\0",
        SHOP_URL.encode() + b"\0" * (70 - len(SHOP_URL)),
        "Web shop URL (replaces the Nexon cash-charge URL)",
    ),
    (
        "GameUI.dll",
        0x103F5E53,
        bytes.fromhex("c700d4e58810c740040b000000"),       # mov [eax],"about:blank"; mov [eax+4],11
        b"\xc7\x00" + _SHOP_URL_VA.to_bytes(4, "little")
        + b"\xc7\x40\x04" + len(SHOP_URL).to_bytes(4, "little"),
        "Web event window loads the web shop URL instead of about:blank",
    ),
    # Close button of the web shop: the in-game browser reports a failed page load to the panel that owns it as
    # the vgui command "OnLoadError". It now sends "CloseModal" (what the window's own close button sends), so the
    # page closes the window by navigating to an address that refuses the connection (http://127.0.0.1:1/).
    # The other web views in the game only fail on Nexon pages that don't exist offline anyway.
    (
        "hw.dll",
        0x2B6E088,
        b"OnLoadError\0",
        b"CloseModal\0\0",
        "Web shop Close button: browser load errors close the window (CloseModal)",
    ),
    # CWebEventDlg::CreateWebViewPanel only loads its URL when it creates the web view; reopening the shop reused
    # the old page (e.g. the closing error page). Jump to the LoadURL call when the view already exists.
    (
        "GameUI.dll",
        0x103F5C8F,
        bytes.fromhex("0f85d0010000"),      # jne 0x103f5e65 (view exists -> return)
        b"\x0f\x85" + _rel32(0x103F5C95, 0x103F5E48),  # jne 0x103f5e48 (view exists -> load the shop URL again)
        "Web shop reloads every time it opens",
    ),
    # Room panel "BotSetting" button: shown only in modes 3/4/5/24, 22, 57 and the zombie modes (visibility =
    # a chain of cmove on the mode id, then SetVisible). AI teammates work in every mode (botassist.asi), so the
    # button is always shown: "movzx ecx, cl" before the last cmove becomes "mov cl, 1".
    (
        "GameUI.dll",
        0x105B4E6A,
        bytes.fromhex("0fb6c9"),            # movzx ecx, cl
        bytes.fromhex("b10190"),            # mov cl, 1; nop
        "Room bot settings button shown in every mode",
    ),
    # ...and enabled: SetEnabled(roomSettings->vfunc 0x174 != 0) becomes SetEnabled(1).
    (
        "GameUI.dll",
        0x105AFD79,
        bytes.fromhex("84c00f95c2"),        # test al, al; setne dl
        bytes.fromhex("b201909090"),        # mov dl, 1
        "Room bot settings button always enabled",
    ),
    # CBotOptionDlg fills "EnemyBotNumber" from 1 (the friendly list starts at 0): start it at 0 too, so the
    # co-op modes can have no enemy bots (item n = n enemies, read back through the "Number" key).
    (
        "GameUI.dll",
        0x105BD710,
        bytes.fromhex("bf01000000"),        # mov edi, 1
        bytes.fromhex("bf00000000"),        # mov edi, 0
        "Bot settings dialog: enemy AI count can be 0",
    ),
    # Bingo window (CMileageBingoDlg constructor 0x1035f8f0..): the Steam version hides the even / odd / special open
    # buttons and their labels (FindChildControl(name)->SetVisible(0), vfunc 0xac); they are shown again
    # ("push 0" -> "push 1"). CSNZ_Server answers all four (Data/Bingo.json costs).
    (
        "GameUI.dll",
        0x1035F791,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: EvenNumOpenLabel shown",
    ),
    (
        "GameUI.dll",
        0x1035F7AF,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: EvenNumOpenDescriptionLabel shown",
    ),
    (
        "GameUI.dll",
        0x1035F7CD,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: EvenNumOpenButton shown",
    ),
    (
        "GameUI.dll",
        0x1035F7EB,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: OddNumOpenLabel shown",
    ),
    (
        "GameUI.dll",
        0x1035F809,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: OddNumOpenDescriptionLabel shown",
    ),
    (
        "GameUI.dll",
        0x1035F827,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: OddNumOpenButton shown",
    ),
    (
        "GameUI.dll",
        0x1035F845,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: SpecialOpenLabel shown",
    ),
    (
        "GameUI.dll",
        0x1035F863,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: SpecialOpenButton shown",
    ),
    (
        "GameUI.dll",
        0x1035F877,
        bytes.fromhex("6a00ff90ac000000"),  # push 0; call [eax+0xac] (SetVisible)
        bytes.fromhex("6a01"),              # push 1
        "Bingo window: SpecialOpenDescriptionLabel shown",
    ),
    # GameUI.dll connection-state checks (0x105964e6, 0x104f1f67) are NOT patched anymore: forcing
    # "connected" skipped the real connect, so the client never sent its login to CSNZ_Server.
    # Original bytes at both sites: ff d0 (call eax).
]

# Telemetry removed at the source: every export the game uses returns at once, so nothing is collected or sent.
# nxgsm.dll (Nexon GameLogManager, imported by hw.dll): stage/error logs to http://eu.gamelog.nexon.com/200036.html.
# All exports are cdecl and return a bool the game ignores -> xor eax,eax; inc eax; ret.
# Both DLLs are loaded at another base, so a stub must not overlap a relocated absolute address (the loader
# adds the delta on top of it): FinalizeGameLogManager starts with mov eax,[abs], so it only gets a 1-byte ret.
_NXGSM_STUBS = [
    (0x10005190, "558bec83", "33c040c3", "InitializeGameLogManagerA"),
    (0x10005260, "558bec83", "33c040c3", "InitializeGameLogManagerW"),
    (0x10005360, "558bec51", "33c040c3", "WriteStageLogA"),
    (0x100053D0, "558bec51", "33c040c3", "WriteStageLogW"),
    (0x10005440, "558bec51", "33c040c3", "WriteErrorLogA"),
    (0x100054B0, "558bec51", "33c040c3", "WriteErrorLogW"),
    (0x10005600, "558bec51", "33c040c3", "SetUserSN / SetNexonSN"),
    (0x10005330, "a1", "c3", "FinalizeGameLogManager"),
]
# CrashReporter.dll (Nexon crash reporter, imported by cstrike-online.exe and hw.dll): fetches
# /prod/collection-settings from an AWS API host, collects session info and uploads dumps/screenshots via
# CrashReportClient.exe. Exports are stdcall and return 0 with the right stack cleanup. IsInit is left alone:
# it reads a flag only Init's worker sets, so it stays 0 and both callers skip the rest of their setup.
_CRASHREPORTER_STUBS = [
    (0x10010A20, "558bec8b45", 8, "_CrashReporter_Init@8"),
    (0x10010AC0, "6a006a", 0, "_CrashReporter_PlayTimeRecorderInit@0"),
    (0x10010CB0, "558bec6aff", 4, "_CrashReporter_RegisterCallback_BeforeDump@4"),
    (0x10010D50, "558bec8b4d", 4, "_CrashReporter_RegisterClientModule@4"),
    (0x10010B10, "558bec8b4d", 8, "_CrashReporter_RegisterLogFile@8"),
    (0x10010D90, "558bec83e4", 4, "_CrashReporter_SetClientExit@4"),
    (0x100110B0, "558bec83e4", 8, "_CrashReporter_SetIntInfo@8"),
    (0x10010D60, "558bec83e4", 4, "_CrashReporter_SetReceiveServerRegion@4"),
    (0x10010F40, "558bec83e4", 8, "_CrashReporter_SetStrInfoA@8"),
    (0x10010F10, "558bec83e4", 8, "_CrashReporter_SetStrInfoW@8"),
]
# Lobby promotion banners: hw.dll downloads bannerinfo.json and the CEF lobby page from Nexon's S3 bucket, and
# that page pulls in third-party hosts. The URLs now point at 127.0.0.1 (nothing listens on 443, so the
# request fails at once). Same buffer size, NUL padded.
def _url(old: str, new: str, wide: bool) -> tuple:
    enc = "utf-16-le" if wide else "ascii"
    orig, repl = old.encode(enc), new.encode(enc)
    assert len(repl) <= len(orig)
    return orig, repl + b"\0" * (len(orig) - len(repl))


_PROMO_URLS = [
    (0x2C06C60, "https://nxe-cz-prod-content.s3.eu-central-1.amazonaws.com/csnz/game/promotion/",
     "https://127.0.0.1/csnz/game/promotion/", False),
    (0x2B7AA68, "https://nxe-cz-prod-content.s3.eu-central-1.amazonaws.com/csnz/game/promotion/guide/",
     "https://127.0.0.1/csnz/game/promotion/guide/", True),
    (0x2C1BF80, "https://s3.eu-central-1.amazonaws.com/nxe-cz-prod-content/csnz/game/promotion/%s/bannerinfo.json",
     "https://127.0.0.1/csnz/game/promotion/%s/bannerinfo.json", True),
]
PATCHES += [
    ("hw.dll", va, *_url(old, new, wide), f"Promotion content from Nexon S3 -> 127.0.0.1 ({new})")
    for va, old, new, wide in _PROMO_URLS
]
PATCHES += [
    ("nxgsm.dll", va, bytes.fromhex(orig), bytes.fromhex(new), f"GameLog telemetry off: {name} returns at once")
    for va, orig, new, name in _NXGSM_STUBS
]
PATCHES += [
    ("CrashReporter.dll", va, bytes.fromhex(orig),
     bytes.fromhex("33c0c3" if args == 0 else f"33c0c2{args:02x}00"), f"Crash reporter off: {name} returns 0")
    for va, orig, args, name in _CRASHREPORTER_STUBS
]


# Partner (mode 50): the score list only counts and shows players whose first info field isn't 0, to hide the partners of the
# official game's players. Nobody here has that field, so the list was empty. Now it skips that check in the three places:
# everybody is listed, partners too, like the other modes.
PATCHES += [
    ("client.dll", 0x029FFA42, bytes.fromhex("750d"), bytes.fromhex("eb0d"), "Partner: score list rows show everybody"),
    ("client.dll", 0x02A01007, bytes.fromhex("7506"), bytes.fromhex("eb06"), "Partner: score list team totals count everybody"),
    ("client.dll", 0x02A0311A, bytes.fromhex("7506"), bytes.fromhex("eb06"), "Partner: score list team counts count everybody"),
]


# what older releases wrote at a place whose patch changed since: replaced by the current bytes, not an abort
PREVIOUS = {
    ("hw.dll", 0x2754F80): [bytes.fromhex("52ff7104c7410c7f00000166c7410a753290")],  # 1.0.x: port always 30002
}


def file_offset(path: Path, va: int) -> int:
    pe = pefile.PE(str(path), fast_load=True)
    return pe.get_offset_from_rva(va - pe.OPTIONAL_HEADER.ImageBase)


def apply(bin_dir: Path) -> None:
    for name, va, orig, new, note in PATCHES:
        target = bin_dir / name
        off = file_offset(target, va)
        with open(target, "r+b") as f:
            f.seek(off)
            cur = f.read(len(orig))
            # already patched: the new bytes sit at the start, untouched original bytes after them
            if cur[:len(new)] == new and cur[len(new):] == orig[len(new):]:
                print(f"[skip] {name} 0x{va:x} already patched ({note})")
                continue
            older = any(cur[:len(p)] == p and cur[len(p):] == orig[len(p):] for p in PREVIOUS.get((name, va), []))
            if cur != orig and not older:
                sys.exit(f"[abort] {name} 0x{va:x}: bytes {cur.hex()} != expected {orig.hex()}")
            f.seek(off)
            f.write(new)
        print(f"[{'upd' if older else 'ok'}]  {name} 0x{va:x}: {orig.hex()} -> {new.hex()} ({note})")
    for src, name, overwrite in EXTRA_FILES:
        dst = bin_dir / name
        if dst.exists() and not overwrite:
            print(f"[skip] {name} already present")
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        print(f"[ok]   {name} copied")


def revert(bin_dir: Path) -> None:
    for name in sorted({p[0] for p in PATCHES}):
        shutil.copy2(BACKUP_BIN / name, bin_dir / name)
        print(f"[restored] {name} from backup")
    for _, name, _ in EXTRA_FILES:
        if (bin_dir / name).exists():
            (bin_dir / name).unlink()
            print(f"[removed] {name}")


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[2] not in ("apply", "revert"):
        sys.exit(__doc__)
    bin_dir = Path(sys.argv[1])
    apply(bin_dir) if sys.argv[2] == "apply" else revert(bin_dir)
