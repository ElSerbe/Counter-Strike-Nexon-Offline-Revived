// The Steam bridge needs Valve's own steam_api.dll: it's what talks to the real Steam client (Spacewar lobbies, P2P).
// Players without the game on their Steam account may run the game with a Steam emulator's DLL in Bin, and that DLL
// only knows its own network: the bridge loaded through it saw a made-up user and no other player's lobby.
// So the bridge gets the game's DLL only if Valve signed it; otherwise a copy kept next to the bridge, or the original
// from the archive.org copy of the game (that one file, not the whole zip). Nothing replaces the game's own DLL.
using System;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
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

        // The Goldberg Steam Emulator's DLL (LGPL, https://gitlab.com/Mr_Goldberg/goldberg_emulator) isn't shipped with the
        // mod: the launcher downloads its author's release zip from GitLab when a game needs it and takes the 32-bit
        // steam_api.dll at the zip's root, which has to be exactly this one (SHA-256). Kept in mod\tools\goldberg.
        public static string Emulator => Path.Combine(Paths.Mod, "tools", "goldberg", "steam_api.dll");
        public const string EmulatorZipUrl = "https://gitlab.com/Mr_Goldberg/goldberg_emulator/uploads/2524331e488ec6399c396cf48bbe9903/Goldberg_Lan_Steam_Emu_v0.2.5.zip";
        const string EmulatorSha256 = "d45e99b9f588670a9cf8316243e98d64bce96303915ec3fa0dc2bfdb42099a45"; // v0.2.5, built 2019-08-12
        // Goldberg builds the launcher put in a game before: the one shipped with the mod in v1.1.12 to v1.1.15 (2023-05-09)
        static readonly string[] OlderEmulators = { "eaeaf26f60229ae4d1a9aae07d03ab4e93ea599dd883eb810d6adebcf0e52e14" };

        static string Sha256Of(string path)
        {
            try
            {
                using (var sha = SHA256.Create())
                using (var f = File.OpenRead(path))
                    return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
            }
            catch { return ""; }
        }

        static bool IsEmulator(string path)
        {
            var h = Sha256Of(path);
            return h == EmulatorSha256 || Array.IndexOf(OlderEmulators, h) >= 0;
        }

        // the emulator's DLL in mod\tools\goldberg, downloaded from GitLab when it isn't there yet
        public static bool EnsureEmulator(Logger log)
        {
            if (Sha256Of(Emulator) == EmulatorSha256)
                return true;
            log("Getting the Goldberg Steam Emulator (v0.2.5, 5 MB) from gitlab.com...");
            var part = Emulator + ".part";
            try
            {
                byte[] zip;
                using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) })
                {
                    http.DefaultRequestHeaders.UserAgent.ParseAdd("CSNZ-Offline-Revived-Launcher");
                    zip = http.GetByteArrayAsync(EmulatorZipUrl).GetAwaiter().GetResult();
                }
                Directory.CreateDirectory(Path.GetDirectoryName(Emulator));
                using (var archive = new ZipArchive(new MemoryStream(zip), ZipArchiveMode.Read))
                {
                    var entry = archive.GetEntry("steam_api.dll") ?? throw new Exception("the zip has no steam_api.dll");
                    using (var from = entry.Open())
                    using (var to = File.Create(part))
                        from.CopyTo(to);
                }
                if (Sha256Of(part) != EmulatorSha256)
                    throw new Exception("its steam_api.dll isn't the expected one");
                if (File.Exists(Emulator))
                    File.Delete(Emulator);
                File.Move(part, Emulator);
                log("The Goldberg Steam Emulator's steam_api.dll is ready.");
                return true;
            }
            catch (Exception ex)
            {
                try { File.Delete(part); } catch { }
                log("Couldn't get the Goldberg Steam Emulator (" + ex.Message + "). Without the game on your Steam account, this copy " +
                    "of the game may close at startup: put Goldberg's steam_api.dll in its Bin by hand (" + EmulatorZipUrl + ").");
                return false;
            }
        }

        // A game that isn't a Steam library install (the archive.org copy) closes at startup when the Steam account
        // doesn't own it: its Bin gets the emulator's DLL instead of Valve's. Valve's is kept for the Steam bridge first.
        // A Steam install, or another DLL someone put there on purpose, is left alone.
        public static void UseEmulatorIfNotSteam(string game, Logger log)
        {
            if (!GameLocator.IsGame(game) || !string.IsNullOrEmpty(GameLocator.BuildId(game)))
                return;
            var inGame = InGame(game);
            if (File.Exists(inGame) && !IsValves(inGame))
            {
                if (!IsEmulator(inGame))
                    log("The game's steam_api.dll isn't Valve's nor the mod's emulator: left as it is.");
                return;
            }
            if (!EnsureEmulator(log))
                return;
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
