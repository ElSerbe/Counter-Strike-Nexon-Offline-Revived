// The launcher's work, without the window: settings, GitHub releases, the game's folder, the patches, the hosts block,
// the server certificate, the local server and the game. Every step logs through a callback.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.NetworkInformation;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace CsnzLauncher
{
    public delegate void Logger(string line);

    public static class Paths
    {
        public static readonly string Root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        public static string Mod => Path.Combine(Root, "mod");
        public static string SettingsFile => Path.Combine(Root, "launcher.json");
        public static string LogDir => Path.Combine(Mod, "logs");
        public static string VersionFile => Path.Combine(Mod, "VERSION");
        public static string PatchList => Path.Combine(Mod, "patches.json");
        public static string ServerBin => Path.Combine(Mod, "server", "CSNZ_Server", "bin");
        public static string BackupBin => Path.Combine(Mod, "backup", "original", "Bin");
        public static string KeptSettings => Path.Combine(Mod, "backup", "settings");
        // a Git checkout of the project: updating would overwrite the sources
        public static bool IsDevCheckout => Directory.Exists(Path.Combine(Root, ".git"));

        public static Version InstalledVersion()
        {
            try { return ParseVersion(File.ReadAllText(VersionFile)); } catch { return null; }
        }
        public static Version ParseVersion(string s)
        {
            s = (s ?? "").Trim().TrimStart('v', 'V');
            return Version.TryParse(s, out var v) ? v : null;
        }
        public static Version LauncherVersion => typeof(Paths).Assembly.GetName().Version;
    }

    // ------------------------------------------------------------ settings (launcher.json next to the exe)
    public class Settings
    {
        public string GamePath = "";
        public string TokenProtected = ""; // a GitHub token, encrypted for this Windows user (DPAPI)

        static readonly JavaScriptSerializer Json = new JavaScriptSerializer();

        public static Settings Load()
        {
            try
            {
                if (File.Exists(Paths.SettingsFile))
                    return Json.Deserialize<Settings>(File.ReadAllText(Paths.SettingsFile)) ?? new Settings();
            }
            catch { }
            return new Settings();
        }
        public void Save() => File.WriteAllText(Paths.SettingsFile, Json.Serialize(this));

        public string Token
        {
            get
            {
                if (string.IsNullOrEmpty(TokenProtected))
                    return "";
                try
                {
                    var raw = ProtectedData.Unprotect(Convert.FromBase64String(TokenProtected), null, DataProtectionScope.CurrentUser);
                    return Encoding.UTF8.GetString(raw);
                }
                catch { return ""; }
            }
            set
            {
                TokenProtected = string.IsNullOrWhiteSpace(value) ? "" :
                    Convert.ToBase64String(ProtectedData.Protect(Encoding.UTF8.GetBytes(value.Trim()), null, DataProtectionScope.CurrentUser));
            }
        }
    }

    // ------------------------------------------------------------ GitHub releases
    public class ReleaseAsset
    {
        public string Name, ApiUrl;
        public long Size;
    }
    public class ReleaseInfo
    {
        public string Tag, Notes;
        public Version Version;
        public List<ReleaseAsset> Assets = new List<ReleaseAsset>();
        public ReleaseAsset Find(Func<string, bool> match) => Assets.FirstOrDefault(a => match(a.Name));
    }

    public static class GitHub
    {
        public const string Owner = "ElSerbe", Repo = "Counter-Strike-Nexon-Offline-Revived";
        public static string PageUrl => $"https://github.com/{Owner}/{Repo}";

        static GitHub()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        }

        // the token for the private repository: the one saved in the launcher, GH_TOKEN / GITHUB_TOKEN, or the GitHub CLI's
        public static string FindToken(Settings settings, out string source)
        {
            source = "none";
            var t = settings.Token;
            if (!string.IsNullOrEmpty(t)) { source = "saved in the launcher"; return t; }
            foreach (var name in new[] { "GH_TOKEN", "GITHUB_TOKEN" })
            {
                t = Environment.GetEnvironmentVariable(name);
                if (!string.IsNullOrEmpty(t)) { source = name; return t.Trim(); }
            }
            try
            {
                var psi = new ProcessStartInfo("gh", "auth token") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
                using (var p = Process.Start(psi))
                {
                    var output = p.StandardOutput.ReadToEnd().Trim();
                    p.WaitForExit(5000);
                    if (p.ExitCode == 0 && output.Length > 10) { source = "GitHub CLI (gh)"; return output; }
                }
            }
            catch { }
            return "";
        }

        static HttpClient Client(string token, bool redirects = true)
        {
            var c = new HttpClient(new HttpClientHandler { AllowAutoRedirect = redirects }) { Timeout = TimeSpan.FromMinutes(30) };
            c.DefaultRequestHeaders.UserAgent.ParseAdd("CSNZ-Offline-Revived-Launcher/" + Paths.LauncherVersion);
            if (!string.IsNullOrEmpty(token))
                c.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
            return c;
        }

        public static async Task<ReleaseInfo> Latest(string token)
        {
            using (var c = Client(token))
            {
                var req = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{Owner}/{Repo}/releases/latest");
                req.Headers.Accept.ParseAdd("application/vnd.github+json");
                var res = await c.SendAsync(req);
                if (res.StatusCode == HttpStatusCode.NotFound)
                    throw new Exception(string.IsNullOrEmpty(token)
                        ? "The repository is private: the launcher needs a GitHub token (More > GitHub token...) or a signed-in GitHub CLI."
                        : "No release found (or this token can't read the repository).");
                if (res.StatusCode == HttpStatusCode.Unauthorized)
                    throw new Exception("GitHub refused the token (expired or wrong). Set a new one in More > GitHub token...");
                res.EnsureSuccessStatusCode();
                var json = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(await res.Content.ReadAsStringAsync());
                var info = new ReleaseInfo { Tag = json["tag_name"] as string, Notes = json.ContainsKey("body") ? json["body"] as string : "" };
                info.Version = Paths.ParseVersion(info.Tag);
                foreach (Dictionary<string, object> a in (System.Collections.IEnumerable)json["assets"])
                    info.Assets.Add(new ReleaseAsset { Name = (string)a["name"], ApiUrl = (string)a["url"], Size = Convert.ToInt64(a["size"]) });
                return info;
            }
        }

        // a release asset: the API answers with a redirect to a signed download link, which must be fetched without the token
        public static async Task Download(ReleaseAsset asset, string token, string path, Logger log)
        {
            string url;
            using (var c = Client(token, false))
            {
                var req = new HttpRequestMessage(HttpMethod.Get, asset.ApiUrl);
                req.Headers.Accept.ParseAdd("application/octet-stream");
                var res = await c.SendAsync(req);
                if ((int)res.StatusCode >= 300 && (int)res.StatusCode < 400 && res.Headers.Location != null)
                    url = res.Headers.Location.ToString();
                else
                {
                    res.EnsureSuccessStatusCode();
                    using (var f = File.Create(path))
                        await res.Content.CopyToAsync(f);
                    return;
                }
            }
            using (var c = Client(null))
            using (var res = await c.GetAsync(url, HttpCompletionOption.ResponseHeadersRead))
            {
                res.EnsureSuccessStatusCode();
                using (var input = await res.Content.ReadAsStreamAsync())
                using (var f = File.Create(path))
                {
                    var buf = new byte[1 << 16];
                    long done = 0, next = 0;
                    int n;
                    while ((n = await input.ReadAsync(buf, 0, buf.Length)) > 0)
                    {
                        await f.WriteAsync(buf, 0, n);
                        done += n;
                        if (done >= next)
                        {
                            log($"  {asset.Name}: {done / 1048576} of {Math.Max(1, asset.Size / 1048576)} MB");
                            next = done + 16 * 1048576;
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ installing a release of the mod
    public static class Installer
    {
        // the player's state: kept when an update brings its own copy
        static readonly string[] KeepIfPresent =
        {
            "mod/server/CSNZ_Server/bin/Data/BotPresets.json",
            "mod/server/CSNZ_Server/bin/Data/BingoState.json",
            "mod/server/CSNZ_Server/bin/Data/ZSRiftState.json",
        };

        public static void InstallZip(string zip, Logger log)
        {
            int written = 0, kept = 0;
            using (var archive = ZipFile.OpenRead(zip))
            {
                foreach (var entry in archive.Entries)
                {
                    var name = entry.FullName.Replace('\\', '/');
                    if (!name.StartsWith("mod/") || name.Contains(".."))
                        continue;
                    var dest = Path.Combine(Paths.Root, name.Replace('/', '\\'));
                    if (name.EndsWith("/"))
                    {
                        Directory.CreateDirectory(dest);
                        continue;
                    }
                    if (File.Exists(dest) && KeepIfPresent.Contains(name, StringComparer.OrdinalIgnoreCase))
                    {
                        kept++;
                        continue;
                    }
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    entry.ExtractToFile(dest, true);
                    written++;
                }
            }
            log($"Installed {written} files ({kept} of your own kept).");
        }
    }

    // ------------------------------------------------------------ the game's folder
    public static class GameLocator
    {
        // Counter-Strike Nexon on Steam
        public const int AppId = 273110;
        public static string StorePage => $"https://store.steampowered.com/app/{AppId}/";
        public static string InstallCommand => $"steam://install/{AppId}";
        public static string RunCommand => $"steam://run/{AppId}";
        public static string VerifyCommand => $"steam://validate/{AppId}";
        public const string SteamDownload = "https://store.steampowered.com/about/";

        public static bool IsGame(string dir) => !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "Bin", "cstrike-online.exe"));

        // Steam's own folder, or null when Steam isn't installed
        public static string SteamDir()
        {
            foreach (var (hive, key, value) in new[] {
                (Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath"),
                (Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                (Registry.LocalMachine, @"SOFTWARE\Valve\Steam", "InstallPath") })
            {
                try
                {
                    using (var k = hive.OpenSubKey(key))
                        if (k?.GetValue(value) is string s && File.Exists(Path.Combine(s, "steam.exe")))
                            return s.Replace('/', '\\');
                }
                catch { }
            }
            return null;
        }

        // every Steam library folder (Steam's own and the ones in libraryfolders.vdf)
        public static List<string> Libraries()
        {
            var libs = new List<string>();
            var steam = SteamDir();
            if (steam == null)
                return libs;
            libs.Add(steam);
            var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
            try
            {
                if (File.Exists(vdf))
                    foreach (var line in File.ReadAllLines(vdf))
                    {
                        var parts = line.Trim().Split('"');
                        if (parts.Length >= 4 && parts[1] == "path")
                            libs.Add(parts[3].Replace(@"\\", @"\"));
                    }
            }
            catch { }
            return libs.Distinct(StringComparer.OrdinalIgnoreCase).Where(Directory.Exists).ToList();
        }

        // a "key" "value" line of a Steam .acf file
        static string AcfValue(string acf, string key)
        {
            foreach (var line in File.ReadAllLines(acf))
            {
                var parts = line.Trim().Split('"');
                if (parts.Length >= 4 && parts[1].Equals(key, StringComparison.OrdinalIgnoreCase))
                    return parts[3];
            }
            return null;
        }

        // Steam knows the game (its appmanifest) but its files aren't all there yet: Steam is still downloading it
        public static string Downloading()
        {
            foreach (var lib in Libraries())
            {
                var acf = Path.Combine(lib, "steamapps", $"appmanifest_{AppId}.acf");
                try
                {
                    if (File.Exists(acf))
                    {
                        var dir = Path.Combine(lib, "steamapps", "common", AcfValue(acf, "installdir") ?? "CSNZ");
                        if (!IsGame(dir) || AcfValue(acf, "StateFlags") != "4")
                            return dir;
                    }
                }
                catch { }
            }
            return null;
        }

        // Steam's build number of the installed game, for the log
        public static string BuildId(string game)
        {
            try
            {
                var acf = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(game)), $"appmanifest_{AppId}.acf");
                return File.Exists(acf) ? AcfValue(acf, "buildid") : null;
            }
            catch { return null; }
        }

        public static string Find()
        {
            var libs = Libraries();
            // the app's manifest says which folder Steam installed it in
            foreach (var lib in libs)
            {
                var acf = Path.Combine(lib, "steamapps", $"appmanifest_{AppId}.acf");
                try
                {
                    if (File.Exists(acf))
                    {
                        var dir = Path.Combine(lib, "steamapps", "common", AcfValue(acf, "installdir") ?? "CSNZ");
                        if (IsGame(dir))
                            return dir;
                    }
                }
                catch { }
            }
            foreach (var lib in libs)
            {
                var dir = Path.Combine(lib, "steamapps", "common", "CSNZ");
                if (IsGame(dir))
                    return dir;
            }
            // Steam's uninstall entry for the game (app 273110)
            foreach (var key in new[] { @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 273110",
                                        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 273110" })
            {
                try
                {
                    using (var k = Registry.LocalMachine.OpenSubKey(key))
                        if (k?.GetValue("InstallLocation") is string s && IsGame(s))
                            return s;
                }
                catch { }
            }
            return null;
        }
    }

    // ------------------------------------------------------------ the client patches (patches.json, made by tools/release/build_release.py)
    public class PatchList
    {
        public class Patch { public string file, orig, @new, note; public long offset; public string va; public List<string> prev = new List<string>(); }
        public class Extra { public string src, dst; public bool overwrite; }
        public string version;
        public List<Patch> patches = new List<Patch>();
        public List<Extra> files = new List<Extra>();

        public static PatchList Load() => new JavaScriptSerializer { MaxJsonLength = int.MaxValue }.Deserialize<PatchList>(File.ReadAllText(Paths.PatchList));
    }

    public static class Patcher
    {
        // Outdated: an older release's bytes at a place whose patch changed since (rewritten by Apply)
        public enum State { Original, Patched, Different, Outdated }

        static byte[] Hex(string s)
        {
            var b = new byte[s.Length / 2];
            for (int i = 0; i < b.Length; i++)
                b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
            return b;
        }

        static State Check(FileStream f, PatchList.Patch p)
        {
            var orig = Hex(p.orig);
            var neu = Hex(p.@new);
            var cur = new byte[orig.Length];
            f.Seek(p.offset, SeekOrigin.Begin);
            if (f.Read(cur, 0, cur.Length) != cur.Length)
                return State.Different;
            if (cur.SequenceEqual(orig))
                return State.Original;
            // patched: the new bytes first, the original ones after them
            if (cur.Take(neu.Length).SequenceEqual(neu) && cur.Skip(neu.Length).SequenceEqual(orig.Skip(neu.Length)))
                return State.Patched;
            foreach (var old in (p.prev ?? new List<string>()).Select(Hex))
                if (cur.Take(old.Length).SequenceEqual(old) && cur.Skip(old.Length).SequenceEqual(orig.Skip(old.Length)))
                    return State.Outdated;
            return State.Different;
        }

        static Dictionary<string, List<State>> Survey(string bin, PatchList list)
        {
            var states = new Dictionary<string, List<State>>(StringComparer.OrdinalIgnoreCase);
            foreach (var group in list.patches.GroupBy(p => p.file, StringComparer.OrdinalIgnoreCase))
            {
                var path = Path.Combine(bin, group.Key);
                var s = new List<State>();
                if (!File.Exists(path))
                    s.Add(State.Different);
                else
                    using (var f = File.OpenRead(path))
                        foreach (var p in group)
                            s.Add(Check(f, p));
                states[group.Key] = s;
            }
            return states;
        }

        // copies of the untouched files, for "Restore the original game" (refreshed after a Steam update)
        static void Backup(string bin, Dictionary<string, List<State>> states, Logger log)
        {
            Directory.CreateDirectory(Paths.BackupBin);
            foreach (var kv in states)
            {
                if (!kv.Value.All(s => s == State.Original))
                    continue;
                var src = Path.Combine(bin, kv.Key);
                var dst = Path.Combine(Paths.BackupBin, kv.Key);
                var fi = new FileInfo(src);
                var bi = new FileInfo(dst);
                if (bi.Exists && bi.Length == fi.Length && bi.LastWriteTimeUtc == fi.LastWriteTimeUtc)
                    continue;
                File.Copy(src, dst, true);
                log($"  backed up {kv.Key}");
            }
        }

        public static string Status(string game)
        {
            var list = PatchList.Load();
            var states = Survey(Path.Combine(game, "Bin"), list);
            int patched = states.Values.Sum(v => v.Count(s => s == State.Patched)), original = states.Values.Sum(v => v.Count(s => s == State.Original));
            int different = states.Values.Sum(v => v.Count(s => s == State.Different));
            return different > 0 ? $"{different} patch places don't match this game version" : original == 0 ? "patched" : patched == 0 ? "original" : "partly patched";
        }

        // the game as a whole, for the window
        public enum GameState { NoMod, Original, Patched, Partly, Mismatch, Outdated }

        public static GameState Check(string game)
        {
            if (!File.Exists(Paths.PatchList))
                return GameState.NoMod;
            var list = PatchList.Load();
            var bin = Path.Combine(game, "Bin");
            var all = Survey(bin, list).Values.SelectMany(v => v).ToList();
            if (all.Contains(State.Different))
            {
                // a file the mod patched but Steam changed since: Restore can still put the backup back only if it's ours
                return GameState.Mismatch;
            }
            bool filesIn = list.files.Where(x => x.overwrite).All(x => File.Exists(Path.Combine(bin, x.dst.Replace('/', '\\'))));
            if (all.Contains(State.Outdated))
                return GameState.Outdated;
            if (all.All(s => s == State.Patched))
                return filesIn ? GameState.Patched : GameState.Partly;
            if (all.All(s => s == State.Original))
                return list.files.Any(x => x.overwrite && File.Exists(Path.Combine(bin, x.dst.Replace('/', '\\')))) ? GameState.Partly : GameState.Original;
            return GameState.Partly;
        }

        public static void Apply(string game, Logger log)
        {
            var bin = Path.Combine(game, "Bin");
            var list = PatchList.Load();
            var states = Survey(bin, list);
            var bad = states.Where(kv => kv.Value.Contains(State.Different)).Select(kv => kv.Key).ToList();
            if (bad.Count > 0)
                throw new Exception("These game files don't match the mod: " + string.Join(", ", bad) +
                    ".\nSteam probably updated the game. Check for an update of the mod (it has to be made for the new game version).");
            Backup(bin, states, log);
            int done = 0;
            foreach (var group in list.patches.GroupBy(p => p.file, StringComparer.OrdinalIgnoreCase))
            {
                using (var f = new FileStream(Path.Combine(bin, group.Key), FileMode.Open, FileAccess.ReadWrite))
                    foreach (var p in group)
                    {
                        var s = Check(f, p);
                        if (s != State.Original && s != State.Outdated)
                            continue;
                        var neu = Hex(p.@new);
                        f.Seek(p.offset, SeekOrigin.Begin);
                        f.Write(neu, 0, neu.Length);
                        done++;
                    }
            }
            int copied = 0;
            foreach (var x in list.files)
            {
                var src = Path.Combine(Paths.Mod, x.src.Replace('/', '\\'));
                var dst = Path.Combine(bin, x.dst.Replace('/', '\\'));
                var kept = Path.Combine(Paths.KeptSettings, x.dst.Replace('/', '\\'));
                if (!x.overwrite && !File.Exists(dst) && File.Exists(kept))
                    src = kept; // the player's settings from before an unpatch
                if (!File.Exists(src) || (File.Exists(dst) && !x.overwrite))
                    continue;
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.Copy(src, dst, true);
                copied++;
            }
            log($"Game patched: {done} new patch places, {list.patches.Count - done} already in place, {copied} mod files copied into Bin.");
        }

        // Steam's files back: the backups of the patched files, the mod's files out of Bin. Returns false when a patched file
        // had no backup (then Steam's "Verify integrity of game files" fetches it again).
        public static bool Restore(string game, Logger log)
        {
            var bin = Path.Combine(game, "Bin");
            var list = PatchList.Load();
            var states = Survey(bin, list);
            bool complete = true;
            foreach (var kv in states)
            {
                var file = kv.Key;
                if (kv.Value.All(s => s == State.Original))
                    continue; // untouched already
                var b = Path.Combine(Paths.BackupBin, file);
                if (!kv.Value.Contains(State.Patched) && !kv.Value.Contains(State.Outdated))
                {
                    // not our bytes: Steam changed this file since the mod's release, an old backup would only break it
                    log($"  {file} isn't patched by this mod (Steam updated it?): left as it is");
                    continue;
                }
                if (File.Exists(b))
                {
                    File.Copy(b, Path.Combine(bin, file), true);
                    log($"  restored {file}");
                }
                else
                {
                    log($"  no backup of {file}: Steam's \"Verify integrity of game files\" puts it back");
                    complete = false;
                }
            }
            foreach (var x in list.files)
            {
                var dst = Path.Combine(bin, x.dst.Replace('/', '\\'));
                if (!File.Exists(dst))
                    continue;
                if (!x.overwrite)
                {
                    // the player's settings (botassist.ini...): kept aside for the next patch
                    var keep = Path.Combine(Paths.KeptSettings, x.dst.Replace('/', '\\'));
                    Directory.CreateDirectory(Path.GetDirectoryName(keep));
                    File.Copy(dst, keep, true);
                }
                File.Delete(dst);
            }
            var tables = Path.Combine(bin, "botassist_tables");
            if (Directory.Exists(tables))
                Directory.Delete(tables, true);
            log(complete ? "The game is back to Steam's files." : "The mod's files are out of Bin; Steam has to put back the files without a backup.");
            return complete;
        }
    }

    // ------------------------------------------------------------ hosts block (the game's Nexon addresses -> 127.0.0.1)
    public static class Hosts
    {
        static string HostsFile => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers", "etc", "hosts");
        public static bool HasBlock()
        {
            try { return File.ReadAllText(HostsFile).Contains("# >>> CSNZ offline mod (begin)"); } catch { return false; }
        }
        // runs tools\hosts_block.ps1 elevated: Windows asks the player first
        public static bool Run(string action, Logger log)
        {
            var script = Path.Combine(Paths.Mod, "tools", "hosts_block.ps1");
            var psi = new ProcessStartInfo("powershell.exe", $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\" {action}")
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
            };
            try
            {
                using (var p = Process.Start(psi))
                {
                    p.WaitForExit();
                    log($"hosts file: {action} {(p.ExitCode == 0 ? "done" : "failed")}");
                    return p.ExitCode == 0;
                }
            }
            catch (System.ComponentModel.Win32Exception)
            {
                log("hosts file: Windows' permission prompt was cancelled.");
                return false;
            }
        }
    }

    // ------------------------------------------------------------ the server's TLS certificate (made on this PC, never shipped)
    public static class Certificates
    {
        public static void Ensure(Logger log)
        {
            var dir = Path.Combine(Paths.ServerBin, "Data", "Certs");
            var cert = Path.Combine(dir, "server-cert.pem");
            var key = Path.Combine(dir, "server-key.pem");
            if (File.Exists(cert) && File.Exists(key))
                return;
            Directory.CreateDirectory(dir);
            using (var rsa = new RSACryptoServiceProvider(2048))
            {
                var req = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                using (var c = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(20)))
                    File.WriteAllText(cert, Pem("CERTIFICATE", c.Export(X509ContentType.Cert)));
                File.WriteAllText(key, Pem("RSA PRIVATE KEY", Pkcs1(rsa.ExportParameters(true))));
            }
            log("Made the local server's certificate (self-signed, this PC only).");
        }

        static string Pem(string label, byte[] der)
        {
            var b64 = Convert.ToBase64String(der);
            var sb = new StringBuilder($"-----BEGIN {label}-----\n");
            for (int i = 0; i < b64.Length; i += 64)
                sb.Append(b64.Substring(i, Math.Min(64, b64.Length - i))).Append('\n');
            return sb.Append($"-----END {label}-----\n").ToString();
        }

        // RSAPrivateKey (PKCS#1) in DER
        static byte[] Pkcs1(RSAParameters k)
        {
            var body = new List<byte>();
            foreach (var part in new[] { new byte[] { 0 }, k.Modulus, k.Exponent, k.D, k.P, k.Q, k.DP, k.DQ, k.InverseQ })
                body.AddRange(DerInteger(part));
            return DerTag(0x30, body.ToArray());
        }
        static byte[] DerInteger(byte[] v)
        {
            int i = 0;
            while (i < v.Length - 1 && v[i] == 0)
                i++;
            var trimmed = v.Skip(i).ToArray();
            if ((trimmed[0] & 0x80) != 0)
                trimmed = new byte[] { 0 }.Concat(trimmed).ToArray();
            return DerTag(0x02, trimmed);
        }
        static byte[] DerTag(byte tag, byte[] content)
        {
            var len = new List<byte>();
            if (content.Length < 0x80)
                len.Add((byte)content.Length);
            else
            {
                var bytes = BitConverter.GetBytes(content.Length).Reverse().SkipWhile(b => b == 0).ToArray();
                len.Add((byte)(0x80 | bytes.Length));
                len.AddRange(bytes);
            }
            return new[] { tag }.Concat(len).Concat(content).ToArray();
        }
    }

    // ------------------------------------------------------------ playing: the local server and the game
    public static class Runner
    {
        public const int ServerPort = 30002;

        public static bool Listening(int port) =>
            IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpListeners().Any(e => e.Port == port);
        public static bool GameRunning() => Process.GetProcessesByName("cstrike-online").Length > 0;
        public static bool ServerRunning() => Process.GetProcessesByName("CSNZ_Server").Length > 0;

        // CSNZ_Server with its console input kept open (it spins when the input hits its end), output in server\server.log
        public static Process StartServer(Logger log)
        {
            var exe = Path.Combine(Paths.ServerBin, "CSNZ_Server.exe");
            var psi = new ProcessStartInfo(exe)
            {
                WorkingDirectory = Paths.ServerBin,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            var p = Process.Start(psi);
            var logFile = Path.Combine(Paths.Mod, "server", "server.log");
            var writer = new StreamWriter(logFile, false, Encoding.UTF8) { AutoFlush = true };
            p.OutputDataReceived += (s, e) => { if (e.Data != null) lock (writer) writer.WriteLine(e.Data); };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (writer) writer.WriteLine(e.Data); };
            p.Exited += (s, e) => { lock (writer) writer.Dispose(); };
            p.EnableRaisingEvents = true;
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            for (int i = 0; i < 60 && !Listening(ServerPort); i++)
            {
                if (p.HasExited)
                    throw new Exception("The local server stopped right away: see mod\\server\\server.log.");
                Thread.Sleep(500);
            }
            if (!Listening(ServerPort))
                throw new Exception("The local server didn't open port 30002: see mod\\server\\server.log.");
            log("Local server started.");
            return p;
        }

        // the game asks Windows for administrator rights itself; its built-in browser gets a closed proxy so it can't reach the internet
        public static void StartGame(string game, Logger log)
        {
            var bin = Path.Combine(game, "Bin");
            var psi = new ProcessStartInfo(Path.Combine(bin, "cstrike-online.exe"), "--proxy-server=http://127.0.0.1:9")
            {
                WorkingDirectory = bin,
                UseShellExecute = true,
                Verb = "runas",
            };
            try { Process.Start(psi); }
            catch (System.ComponentModel.Win32Exception) { throw new Exception("The game didn't start (Windows' permission prompt was cancelled)."); }
            log("Game started.");
        }

        public static void WaitForGame()
        {
            for (int i = 0; i < 60 && !GameRunning(); i++)
                Thread.Sleep(1000);
            while (GameRunning())
                Thread.Sleep(2000);
        }
    }

    // ------------------------------------------------------------ the web pages' pictures and models, exported from the player's game (needs Python)
    public static class Assets
    {
        public static bool Present => Directory.Exists(Path.Combine(Paths.Mod, "web", "shop", "icons")) &&
                                      Directory.EnumerateFiles(Path.Combine(Paths.Mod, "web", "shop", "icons")).Any();

        static readonly string[] Scripts = { "export_shop_assets.py", "export_bingo_icons.py", "export_bot_catalog.py", "gen_medals.py", "gen_missions.py" };

        static int Run(string file, string args, Logger log, string game = null)
        {
            var psi = new ProcessStartInfo(file, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = Path.Combine(Paths.Mod, "tools"),
            };
            if (game != null)
                psi.EnvironmentVariables["CSNZ_GAME"] = game;
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            using (var p = Process.Start(psi))
            {
                p.OutputDataReceived += (s, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) log("  " + e.Data); };
                p.ErrorDataReceived += (s, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) log("  " + e.Data); };
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                return p.ExitCode;
            }
        }

        static string FindPython(out string prefix)
        {
            foreach (var (exe, pre) in new[] { ("py", "-3 "), ("python", ""), ("python3", "") })
            {
                try
                {
                    if (Run(exe, pre + "-c \"import sys; assert sys.version_info >= (3, 8)\"", _ => { }) == 0)
                    {
                        prefix = pre;
                        return exe;
                    }
                }
                catch { }
            }
            prefix = "";
            return null;
        }

        public static void Export(string game, Logger log)
        {
            var py = FindPython(out var pre);
            if (py == null)
                throw new Exception("Python 3 isn't installed (https://www.python.org/downloads/, tick \"Add python.exe to PATH\").\n" +
                                    "Without these pictures the in-game shop and profile pages show no item images; everything else works.");
            if (Run(py, pre + "-c \"import PIL, pefile\"", _ => { }) != 0)
            {
                log("Installing the Python packages Pillow and pefile...");
                if (Run(py, pre + "-m pip install --user --disable-pip-version-check pillow pefile", log) != 0)
                    throw new Exception("pip couldn't install Pillow and pefile.");
            }
            foreach (var script in Scripts)
            {
                log($"Running {script} (it can take a few minutes)...");
                if (Run(py, pre + "\"" + script + "\"", log, game) != 0)
                    throw new Exception(script + " failed: see the lines above.");
            }
            log("Shop, profile and mission pictures exported from your game.");
        }
    }
}
