// The game from archive.org, for players who can't get it from Steam any more: the Internet Archive keeps a copy of
// the Steam build this mod targets (item csnz-1362486462562801379, Steam depot manifest 1362486462562801379, the same
// files as Steam's latest version). The launcher downloads its zip (12.9 GB, resumable: a stopped download goes on
// from where it was), checks it against the MD5 archive.org lists for it, unpacks it into the chosen folder
// (Bin and Data, like Steam's folder) and deletes the zip. The mod patches that folder exactly like a Steam copy.
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace CsnzLauncher
{
    public static class ArchiveGame
    {
        public const string Item = "csnz-1362486462562801379";
        public static string PageUrl => "https://archive.org/details/" + Item;
        public static string TorrentUrl => $"https://archive.org/download/{Item}/{Item}_archive.torrent";
        // what archive.org listed on 2026-11-07; the launcher asks archive.org again before downloading
        const string KnownZip = "CSNZ 22.07.2026 – 02.32.54 UTC.zip";
        const long KnownSize = 12897139152;
        const string KnownMd5 = "96b85c1dff0ca5d62be674f8bd185bcf";
        public const long UnpackedSize = 13589447846; // Steam's size on disk for this build
        const string PartName = "csnz-archive.zip.part", ZipName = "csnz-archive.zip";

        public class ZipInfo { public string Name; public long Size; public string Md5; }

        // progress: what is being done, bytes done, bytes in all, bytes per second
        public delegate void Progress(string stage, long done, long total, double speed);

        static HttpClient Client()
        {
            var c = new HttpClient(new HttpClientHandler { AllowAutoRedirect = true }) { Timeout = Timeout.InfiniteTimeSpan };
            c.DefaultRequestHeaders.UserAgent.ParseAdd("CSNZ-Offline-Revived-Launcher");
            return c;
        }

        // the zip's name, size and MD5 from archive.org's metadata API (the values known to the launcher when it can't ask)
        public static async Task<ZipInfo> Info()
        {
            try
            {
                using (var c = Client())
                using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30)))
                {
                    var json = await (await c.GetAsync("https://archive.org/metadata/" + Item, cts.Token)).Content.ReadAsStringAsync();
                    var meta = new JavaScriptSerializer { MaxJsonLength = int.MaxValue }.Deserialize<System.Collections.Generic.Dictionary<string, object>>(json);
                    if (meta.TryGetValue("files", out var files) && files is System.Collections.ArrayList list)
                        foreach (System.Collections.Generic.Dictionary<string, object> f in list)
                        {
                            var name = f.TryGetValue("name", out var n) ? n as string : null;
                            if (name == null || !name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                                continue;
                            long.TryParse(f.TryGetValue("size", out var s) ? s as string : null, out var size);
                            var md5 = f.TryGetValue("md5", out var m) ? m as string : null;
                            if (size > 0 && !string.IsNullOrEmpty(md5))
                                return new ZipInfo { Name = name, Size = size, Md5 = md5 };
                        }
                }
            }
            catch { }
            return new ZipInfo { Name = KnownZip, Size = KnownSize, Md5 = KnownMd5 };
        }

        // a HEAD request to the zip (for --check --archive): where it redirects, its length, whether ranges are accepted
        public static async Task<string> Head(ZipInfo zip)
        {
            using (var c = Client())
            using (var req = new HttpRequestMessage(HttpMethod.Head, $"https://archive.org/download/{Item}/{Uri.EscapeDataString(zip.Name)}"))
            using (var resp = await c.SendAsync(req, HttpCompletionOption.ResponseHeadersRead))
                return $"{(int)resp.StatusCode} from {resp.RequestMessage.RequestUri.Host}, length {resp.Content.Headers.ContentLength}, ranges {string.Join(",", resp.Headers.AcceptRanges)}";
        }

        // bytes still needed on the folder's drive: the rest of the download plus the unpacked game
        public static long SpaceNeeded(string folder, ZipInfo zip)
        {
            long have = 0;
            var part = Path.Combine(folder, PartName);
            if (File.Exists(Path.Combine(folder, ZipName)))
                have = zip.Size;
            else if (File.Exists(part))
                have = new FileInfo(part).Length;
            return zip.Size - have + UnpackedSize;
        }

        public static long FreeSpace(string folder)
        {
            try { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(folder))).AvailableFreeSpace; }
            catch { return -1; }
        }

        // a download already started in this folder (it goes on from there)
        public static long Started(string folder)
        {
            var part = Path.Combine(folder, PartName);
            return File.Exists(Path.Combine(folder, ZipName)) ? -1 : File.Exists(part) ? new FileInfo(part).Length : 0;
        }

        // download (resuming), check, unpack; returns the game's folder. Cancelling keeps the part already downloaded.
        public static async Task<string> Install(string folder, ZipInfo zip, Progress progress, Logger log, CancellationToken cancel)
        {
            Directory.CreateDirectory(folder);
            var part = Path.Combine(folder, PartName);
            var done = Path.Combine(folder, ZipName);
            if (!File.Exists(done))
            {
                await Download(zip, part, progress, log, cancel);
                progress("Checking the download", 0, zip.Size, 0);
                log("Checking the download against archive.org's MD5...");
                var md5 = await Task.Run(() => Md5(part, zip.Size, progress, cancel));
                if (!md5.Equals(zip.Md5, StringComparison.OrdinalIgnoreCase))
                {
                    File.Delete(part);
                    throw new Exception("The download is damaged (its MD5 doesn't match archive.org's), so it was deleted. Press Download again to start over.");
                }
                File.Move(part, done);
                log("Download checked: it matches archive.org's copy.");
            }
            log("Unpacking the game into " + folder + "...");
            await Task.Run(() => Unpack(done, folder, progress, cancel));
            if (!GameLocator.IsGame(folder))
                throw new Exception("The archive was unpacked but has no Bin\\cstrike-online.exe.");
            try { File.Delete(done); } catch { }
            log("The game is installed in " + folder + " (the downloaded zip was deleted).");
            return folder;
        }

        static async Task Download(ZipInfo zip, string part, Progress progress, Logger log, CancellationToken cancel)
        {
            var url = $"https://archive.org/download/{Item}/{Uri.EscapeDataString(zip.Name)}";
            int failures = 0;
            while (true)
            {
                long have = File.Exists(part) ? new FileInfo(part).Length : 0;
                if (have == zip.Size)
                    return;
                if (have > zip.Size)
                {
                    File.Delete(part);
                    have = 0;
                }
                log(have > 0 ? $"Downloading from archive.org, going on from {have / 1048576} MB..." : "Downloading the game from archive.org...");
                try
                {
                    using (var c = Client())
                    using (var req = new HttpRequestMessage(HttpMethod.Get, url))
                    {
                        if (have > 0)
                            req.Headers.Range = new RangeHeaderValue(have, null);
                        using (var resp = await c.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, cancel))
                        {
                            if (resp.StatusCode == System.Net.HttpStatusCode.RequestedRangeNotSatisfiable)
                            {
                                File.Delete(part);
                                continue;
                            }
                            resp.EnsureSuccessStatusCode();
                            bool resumed = resp.StatusCode == System.Net.HttpStatusCode.PartialContent;
                            if (!resumed)
                                have = 0; // the server sent the whole file: start over
                            using (var src = await resp.Content.ReadAsStreamAsync())
                            using (var dst = new FileStream(part, resumed ? FileMode.Append : FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 20))
                            {
                                var buf = new byte[1 << 20];
                                long windowStart = have;
                                var clock = System.Diagnostics.Stopwatch.StartNew();
                                double speed = 0;
                                int n;
                                using (var idle = new CancellationTokenSource())
                                using (var both = CancellationTokenSource.CreateLinkedTokenSource(cancel, idle.Token))
                                {
                                    while (true)
                                    {
                                        // a connection that stops sending for 60 s is dropped and resumed
                                        idle.CancelAfter(TimeSpan.FromSeconds(60));
                                        var read = src.ReadAsync(buf, 0, buf.Length, both.Token);
                                        n = await read;
                                        if (n <= 0)
                                            break;
                                        await dst.WriteAsync(buf, 0, n);
                                        have += n;
                                        failures = 0;
                                        if (clock.ElapsedMilliseconds >= 1000)
                                        {
                                            double now = (have - windowStart) / clock.Elapsed.TotalSeconds;
                                            speed = speed == 0 ? now : speed * 0.7 + now * 0.3;
                                            windowStart = have;
                                            clock.Restart();
                                            progress("Downloading", have, zip.Size, speed);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                catch (Exception ex) when (!cancel.IsCancellationRequested && (ex is IOException || ex is HttpRequestException || ex is OperationCanceledException))
                {
                    if (++failures > 30)
                        throw new Exception("archive.org stopped answering (" + ex.Message + "). Press Download again later: it goes on from where it stopped.");
                    log($"The connection dropped ({ex.Message}); trying again in 10 s...");
                    await Task.Delay(10000, cancel);
                }
            }
        }

        static string Md5(string file, long total, Progress progress, CancellationToken cancel)
        {
            using (var md5 = MD5.Create())
            using (var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            {
                var buf = new byte[1 << 20];
                long done = 0, last = 0;
                int n;
                while ((n = f.Read(buf, 0, buf.Length)) > 0)
                {
                    cancel.ThrowIfCancellationRequested();
                    md5.TransformBlock(buf, 0, n, null, 0);
                    done += n;
                    if (done - last > 64 << 20)
                    {
                        last = done;
                        progress("Checking the download", done, total, 0);
                    }
                }
                md5.TransformFinalBlock(buf, 0, 0);
                return string.Concat(md5.Hash.Select(b => b.ToString("x2")));
            }
        }

        static void Unpack(string zipFile, string folder, Progress progress, CancellationToken cancel)
        {
            var root = Path.GetFullPath(folder).TrimEnd('\\') + "\\";
            using (var zip = ZipFile.OpenRead(zipFile))
            {
                long total = zip.Entries.Sum(e => e.Length), done = 0, last = 0;
                var buf = new byte[1 << 20];
                foreach (var e in zip.Entries)
                {
                    cancel.ThrowIfCancellationRequested();
                    var dest = Path.GetFullPath(Path.Combine(root, e.FullName.Replace('/', '\\')));
                    if (!dest.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                        continue; // a name that points outside the folder
                    if (e.FullName.EndsWith("/"))
                    {
                        Directory.CreateDirectory(dest);
                        continue;
                    }
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    // unpacked by an earlier, stopped run
                    if (File.Exists(dest) && new FileInfo(dest).Length == e.Length && e.Length > 0)
                    {
                        done += e.Length;
                        continue;
                    }
                    using (var src = e.Open())
                    using (var dst = new FileStream(dest, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                    {
                        int n;
                        while ((n = src.Read(buf, 0, buf.Length)) > 0)
                        {
                            dst.Write(buf, 0, n);
                            done += n;
                            if (done - last > 32 << 20)
                            {
                                last = done;
                                progress("Unpacking", done, total, 0);
                                cancel.ThrowIfCancellationRequested();
                            }
                        }
                    }
                    File.SetLastWriteTime(dest, e.LastWriteTime.DateTime);
                }
                progress("Unpacking", total, total, 0);
            }
        }
    }
}
