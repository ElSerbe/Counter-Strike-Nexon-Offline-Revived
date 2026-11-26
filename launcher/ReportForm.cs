// A problem report: the player writes what happened and how it happens again; the launcher adds the versions, Windows,
// where the game came from and the last match's mode and map (from the game's logs), puts the logs in a folder of
// their own and opens GitHub's form with all of that filled in. GitHub takes files only from the browser (dragged into
// the form): the folder opens next to it with the files selected. Opened from More > Report a problem, and offered
// when the game closed without its log saying so (a crash).
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace CsnzLauncher
{
    class ReportForm : Form
    {
        readonly TextBox what = new TextBox();
        readonly string game;

        public ReportForm(string game, bool crashed)
        {
            this.game = game;
            Text = "Report a problem";
            BackColor = Theme.Panel;
            ForeColor = Theme.Text;
            Font = Theme.UiFont(10);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(620, 470);

            var head = new Label
            {
                Text = (crashed ? "The game closed unexpectedly. Would you like to report it on GitHub?\n\n" : "") +
                       "What happened, and how can it happen again? (the steps, as exactly as you can)",
                Location = new Point(16, 14), Size = new Size(588, crashed ? 64 : 34),
            };
            what.Multiline = true;
            what.ScrollBars = ScrollBars.Vertical;
            what.BackColor = Color.FromArgb(14, 17, 21);
            what.ForeColor = Theme.Text;
            what.BorderStyle = BorderStyle.FixedSingle;
            what.Location = new Point(16, head.Bottom + 6);
            what.Size = new Size(588, 200);
            var info = new Label
            {
                Text = "Added for you: " + Details().Replace("\n", "; ") + "\n\nYour browser opens GitHub's form (a free GitHub account is needed) " +
                       "and a folder with your logs: drag the files into the form's Logs box.",
                Location = new Point(16, what.Bottom + 10), Size = new Size(588, 110), ForeColor = Theme.Muted,
            };
            var send = new FlatButton { Text = "Report on GitHub", Size = new Size(190, 38), Location = new Point(414, 418) };
            var cancel = new FlatButton { Text = crashed ? "Not now" : "Cancel", Size = new Size(120, 38), Location = new Point(284, 418) };
            send.Click += (s, e) => { Send(); Close(); };
            cancel.Click += (s, e) => Close();
            Controls.AddRange(new Control[] { head, what, info, send, cancel });
        }

        // versions, Windows, where the game came from, the last match (mode from botassist.log, map from Error.log)
        string Details()
        {
            var lines = new System.Collections.Generic.List<string>
            {
                "Launcher v" + Paths.LauncherVersion.ToString(3) + ", mod " + (Paths.InstalledVersion()?.ToString(3) ?? "not installed"),
                LogBundle.WindowsName(),
            };
            if (GameLocator.IsGame(game))
                lines.Add("Game: " + (string.IsNullOrEmpty(GameLocator.BuildId(game)) ? "archive.org copy" : "Steam, build " + GameLocator.BuildId(game)));
            string mode = Last(Path.Combine(game ?? "", "Bin", "botassist.log"), @"mode (\d+)");
            string map = Last(Path.Combine(game ?? "", "Bin", "Error.log"), "cso_connect \"([^\"]+)\"");
            if (mode != null || map != null)
                lines.Add("Last match: " + (mode != null ? "mode " + mode : "mode ?") + (map != null ? ", map " + map : ""));
            return string.Join("\n", lines);
        }

        static string Last(string file, string pattern)
        {
            try
            {
                if (!File.Exists(file))
                    return null;
                string text;
                using (var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    f.Seek(Math.Max(0, f.Length - 2000000), SeekOrigin.Begin);
                    text = new StreamReader(f).ReadToEnd();
                }
                var m = Regex.Matches(text, pattern);
                return m.Count > 0 ? m[m.Count - 1].Groups[1].Value : null;
            }
            catch { return null; }
        }

        // the logs in a folder of their own (mod\logs\report-<time>): the game's, the local server's, the Steam bridge's,
        // the launcher's and the summary of the last session
        string CollectLogs()
        {
            var dir = Path.Combine(Paths.LogDir, "report-" + DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss"));
            Directory.CreateDirectory(dir);
            void Copy(string from, string name)
            {
                try
                {
                    if (!File.Exists(from))
                        return;
                    using (var src = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                    using (var dst = File.Create(Path.Combine(dir, name)))
                    {
                        // the end of a big log is what matters (GitHub takes files up to 25 MB)
                        src.Seek(Math.Max(0, src.Length - 8000000), SeekOrigin.Begin);
                        src.CopyTo(dst);
                    }
                }
                catch { }
            }
            var bin = Path.Combine(game ?? "", "Bin");
            Copy(Path.Combine(bin, "Error.log"), "game-Error.log.txt");
            Copy(Path.Combine(Paths.Mod, "server", "server.log"), "server.log.txt");
            Copy(Path.Combine(Paths.LogDir, "steambridge.log"), "steambridge.log.txt");
            Copy(Path.Combine(Paths.LogDir, "launcher.log"), "launcher.log.txt");
            Copy(Path.Combine(bin, "botassist.log"), "botassist.log.txt");
            Copy(LogBundle.FilePath, "last-session.txt");
            Copy(LogBundle.CrashLog(game), "botassist_crash.log.txt");
            // botassist's dumps of the crash or hang: zipped (GitHub doesn't take .dmp files)
            var dumps = LogBundle.RecentDumps(game);
            if (dumps.Length > 0)
                try
                {
                    using (var zip = System.IO.Compression.ZipFile.Open(Path.Combine(dir, "crash-dumps.zip"), System.IO.Compression.ZipArchiveMode.Create))
                        foreach (var d in dumps)
                            System.IO.Compression.ZipFileExtensions.CreateEntryFromFile(zip, d, Path.GetFileName(d));
                }
                catch { }
            return dir;
        }

        void Send()
        {
            var text = what.Text.Trim();
            var title = "[Problem] " + (text.Length == 0 ? "" : text.Split('\n')[0].Trim());
            if (title.Length > 90)
                title = title.Substring(0, 90) + "...";
            if (text.Length > 4000)
                text = text.Substring(0, 4000);
            string dir = null;
            try { dir = CollectLogs(); } catch { }
            var url = GitHub.PageUrl + "/issues/new?template=bug_report.yml" +
                "&title=" + Uri.EscapeDataString(title) +
                "&what=" + Uri.EscapeDataString(text) +
                "&details=" + Uri.EscapeDataString(Details());
            try
            {
                if (dir != null)
                {
                    var first = Directory.GetFiles(dir).FirstOrDefault();
                    Process.Start("explorer.exe", first != null ? "/select,\"" + first + "\"" : "\"" + dir + "\"");
                }
                Process.Start(url);
            }
            catch (Exception ex)
            {
                DarkBox.Show(this, "Couldn't open the browser (" + ex.Message + "). The form is at " + GitHub.PageUrl + "/issues/new/choose", Text);
            }
        }

        // the game's own log says when it closes normally ("Log File Closed" after its last "Log File Opened")
        public static bool GameCrashed(string game)
        {
            try
            {
                var log = Path.Combine(game, "Bin", "Error.log");
                if (!File.Exists(log))
                    return false;
                string text;
                using (var f = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                    text = new StreamReader(f).ReadToEnd();
                int opened = text.LastIndexOf("Log File Opened", StringComparison.Ordinal);
                return opened >= 0 && text.IndexOf("Log File Closed", opened, StringComparison.Ordinal) < 0;
            }
            catch { return false; }
        }
    }
}
