// The Steam bridge needs Valve's own steam_api.dll: it's what talks to the real Steam client (Spacewar lobbies, P2P).
// Players without the game on their Steam account may run the game with a Steam emulator's DLL in Bin, and that DLL
// only knows its own network: the bridge loaded through it saw a made-up user and no other player's lobby.
// So the bridge gets the game's DLL only if Valve signed it; otherwise a copy kept next to the bridge, or the original
// from the archive.org copy of the game (that one file, not the whole zip). Nothing replaces the game's own DLL.
using System;
using System.IO;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;

namespace CsnzLauncher
{
    public static class SteamApi
    {
        public static string InGame(string game) => Path.Combine(game, "Bin", "steam_api.dll");
        // installing a release doesn't touch it (it isn't in the zip)
        public static string Kept => Path.Combine(Path.GetDirectoryName(Paths.BridgeExe), "steam_api.dll");

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct WintrustFileInfo
        {
            public uint cbStruct;
            [MarshalAs(UnmanagedType.LPWStr)] public string pcwszFilePath;
            public IntPtr hFile;
            public IntPtr pgKnownSubject;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct WintrustData
        {
            public uint cbStruct;
            public IntPtr pPolicyCallbackData, pSIPClientData;
            public uint dwUIChoice, fdwRevocationChecks, dwUnionChoice;
            public IntPtr pFile;
            public uint dwStateAction;
            public IntPtr hWVTStateData;
            public IntPtr pwszURLReference;
            public uint dwProvFlags, dwUIContext;
            public IntPtr pSignatureSettings;
        }

        [DllImport("wintrust.dll", CharSet = CharSet.Unicode)]
        static extern int WinVerifyTrust(IntPtr hwnd, [MarshalAs(UnmanagedType.LPStruct)] Guid action, ref WintrustData data);

        // Windows checks the signature against the file's contents; then the signer has to be Valve
        public static bool IsValves(string path)
        {
            if (!File.Exists(path))
                return false;
            var file = new WintrustFileInfo { cbStruct = (uint)Marshal.SizeOf(typeof(WintrustFileInfo)), pcwszFilePath = path };
            var filePtr = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WintrustFileInfo)));
            try
            {
                Marshal.StructureToPtr(file, filePtr, false);
                var data = new WintrustData
                {
                    cbStruct = (uint)Marshal.SizeOf(typeof(WintrustData)),
                    dwUIChoice = 2,      // WTD_UI_NONE
                    dwUnionChoice = 1,   // WTD_CHOICE_FILE
                    pFile = filePtr,
                };
                // WINTRUST_ACTION_GENERIC_VERIFY_V2
                if (WinVerifyTrust(new IntPtr(-1), new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE"), ref data) != 0)
                    return false;
                var subject = new X509Certificate2(X509Certificate.CreateFromSignedFile(path)).Subject;
                return subject.IndexOf("O=Valve", StringComparison.OrdinalIgnoreCase) >= 0;
            }
            catch { return false; }
            finally
            {
                Marshal.DestroyStructure(filePtr, typeof(WintrustFileInfo));
                Marshal.FreeHGlobal(filePtr);
            }
        }

        // the Goldberg Steam Emulator's DLL shipped with the mod (mod\tools\goldberg, LGPL)
        public static string Emulator => Path.Combine(Paths.Mod, "tools", "goldberg", "steam_api.dll");

        static bool SameFile(string a, string b)
        {
            try
            {
                if (new FileInfo(a).Length != new FileInfo(b).Length)
                    return false;
                byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
                if (x.Length != y.Length)
                    return false;
                for (int i = 0; i < x.Length; i++)
                    if (x[i] != y[i])
                        return false;
                return true;
            }
            catch { return false; }
        }

        // A game that isn't a Steam library install (the archive.org copy) closes at startup when the Steam account
        // doesn't own it: its Bin gets the emulator's DLL instead of Valve's. Valve's is kept for the Steam bridge first.
        // A Steam install, or another DLL someone put there on purpose, is left alone.
        public static void UseEmulatorIfNotSteam(string game, Logger log)
        {
            if (!GameLocator.IsGame(game) || !string.IsNullOrEmpty(GameLocator.BuildId(game)) || !File.Exists(Emulator))
                return;
            var inGame = InGame(game);
            if (File.Exists(inGame) && SameFile(inGame, Emulator))
                return;
            if (File.Exists(inGame) && !IsValves(inGame))
            {
                log("The game's steam_api.dll isn't Valve's nor the mod's emulator: left as it is.");
                return;
            }
            try
            {
                if (File.Exists(inGame) && !IsValves(Kept))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(Kept));
                    File.Copy(inGame, Kept, true);
                }
                File.Copy(Emulator, inGame, true);
                log("This copy of the game isn't from a Steam library: its steam_api.dll is now the Goldberg Steam Emulator's, so it " +
                    "starts without the game on your Steam account (Valve's is kept for online play).");
            }
            catch (Exception ex)
            {
                log("Couldn't put the Steam emulator's steam_api.dll in the game: " + ex.Message);
            }
        }

        // the DLL for the bridge, or null when there's no way to get Valve's
        public static string ForBridge(string game, Logger log)
        {
            var inGame = InGame(game);
            if (IsValves(inGame))
            {
                if (!IsValves(Kept))
                    try { File.Copy(inGame, Kept, true); } catch { }
                return inGame;
            }
            if (IsValves(Kept))
            {
                log("Online play: the game's steam_api.dll isn't Valve's (a Steam emulator?), so the Steam bridge uses its own copy.");
                return Kept;
            }
            log("Online play: the game's steam_api.dll isn't Valve's (a Steam emulator?). Getting Valve's for the Steam bridge from archive.org...");
            var part = Kept + ".part";
            try
            {
                var zip = ArchiveGame.Info().GetAwaiter().GetResult();
                var url = $"https://archive.org/download/{ArchiveGame.Item}/{Uri.EscapeDataString(zip.Name)}/Bin/steam_api.dll";
                using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(2) })
                {
                    http.DefaultRequestHeaders.UserAgent.ParseAdd("CSNZ-Offline-Revived-Launcher");
                    File.WriteAllBytes(part, http.GetByteArrayAsync(url).GetAwaiter().GetResult());
                }
                if (!IsValves(part))
                    throw new Exception("the file isn't signed by Valve");
                File.Delete(Kept);
                File.Move(part, Kept);
                log("Online play: Valve's steam_api.dll is in place for the Steam bridge.");
                return Kept;
            }
            catch (Exception ex)
            {
                try { File.Delete(part); } catch { }
                log("Online play is off this time: the game's steam_api.dll isn't Valve's and Valve's couldn't be downloaded (" + ex.Message + "). " +
                    "Put Valve's steam_api.dll in " + Path.GetDirectoryName(Kept) + ". Offline play works as always.");
                return null;
            }
        }
    }
}
