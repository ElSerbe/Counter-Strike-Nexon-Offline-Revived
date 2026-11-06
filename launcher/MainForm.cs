// The launcher's window: the game's folder, the installed and the latest version, Update, Play and a few tools.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace CsnzLauncher
{
    public class MainForm : Form
    {
        readonly Settings settings = Settings.Load();
        readonly TextBox gameBox = new TextBox { ReadOnly = true };
        readonly Label versionLabel = new Label { AutoSize = true };
        readonly Button updateButton = new Button { Text = "Check for updates", Width = 150, Height = 34 };
        readonly Button playButton = new Button { Text = "Play", Width = 150, Height = 34, Font = new Font(SystemFonts.DefaultFont.FontFamily, 10, FontStyle.Bold) };
        readonly Button moreButton = new Button { Text = "More ▾", Width = 100, Height = 34 };
        readonly TextBox logBox = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 9) };
        ReleaseInfo latest;
        bool busy;

        public MainForm()
        {
            Text = "Counter-Strike Nexon: Offline Revived";
            ClientSize = new Size(720, 460);
            MinimumSize = new Size(600, 380);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 9);

            var title = new Label { Text = "Counter-Strike Nexon: Offline Revived", Font = new Font("Segoe UI", 15, FontStyle.Bold), AutoSize = true, Location = new Point(14, 12) };
            var gameLabel = new Label { Text = "Game folder (Steam's CSNZ):", AutoSize = true, Location = new Point(16, 58) };
            gameBox.SetBounds(16, 78, 560, 24);
            gameBox.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right;
            var browse = new Button { Text = "Browse...", Location = new Point(586, 76), Width = 118, Anchor = AnchorStyles.Top | AnchorStyles.Right };
            versionLabel.Location = new Point(16, 112);
            updateButton.Location = new Point(16, 140);
            playButton.Location = new Point(176, 140);
            moreButton.Location = new Point(336, 140);
            logBox.SetBounds(16, 186, 688, 258);
            logBox.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right | AnchorStyles.Bottom;
            Controls.AddRange(new Control[] { title, gameLabel, gameBox, browse, versionLabel, updateButton, playButton, moreButton, logBox });

            var more = new ContextMenuStrip();
            more.Items.Add("Export shop and profile pictures from the game (Python)", null, (s, e) => _ = Run("Exporting pictures", () => Assets.Export(Game(), Log)));
            more.Items.Add("Apply the patches only", null, (s, e) => _ = Run("Patching", () => Patcher.Apply(Game(), Log)));
            more.Items.Add("Restore the original game", null, (s, e) => RestoreGame());
            more.Items.Add(new ToolStripSeparator());
            more.Items.Add("Add the hosts block", null, (s, e) => _ = Run("hosts", () => Hosts.Run("add", Log)));
            more.Items.Add("Remove the hosts block", null, (s, e) => _ = Run("hosts", () => Hosts.Run("remove", Log)));
            more.Items.Add(new ToolStripSeparator());
            more.Items.Add("GitHub token...", null, (s, e) => AskToken());
            more.Items.Add("Open the mod's folder", null, (s, e) => Process.Start("explorer.exe", "\"" + Paths.Mod + "\""));
            more.Items.Add("Open the project on GitHub", null, (s, e) => Process.Start(GitHub.PageUrl));
            moreButton.Click += (s, e) => more.Show(moreButton, new Point(0, moreButton.Height));

            browse.Click += (s, e) => BrowseGame();
            updateButton.Click += (s, e) => UpdateClicked();
            playButton.Click += (s, e) => PlayClicked();
            Shown += async (s, e) => await Startup();
            FormClosing += (s, e) => { if (busy && e.CloseReason == CloseReason.UserClosing && MessageBox.Show(this, "Something is still running. Close anyway?", Text, MessageBoxButtons.YesNo) == DialogResult.No) e.Cancel = true; };
        }

        // ------------------------------------------------------------ helpers
        void Log(string line)
        {
            if (InvokeRequired)
            {
                BeginInvoke(new Action(() => Log(line)));
                return;
            }
            logBox.AppendText(line + Environment.NewLine);
            try
            {
                Directory.CreateDirectory(Paths.LogDir);
                File.AppendAllText(Path.Combine(Paths.LogDir, "launcher.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
            }
            catch { }
        }

        string Game()
        {
            if (!GameLocator.IsGame(settings.GamePath))
                throw new Exception("Choose the game's folder first (the one with Bin\\cstrike-online.exe).");
            return settings.GamePath;
        }

        void SetBusy(bool on)
        {
            busy = on;
            updateButton.Enabled = playButton.Enabled = moreButton.Enabled = !on;
            UseWaitCursor = on;
        }

        async Task<bool> Run(string what, Action work)
        {
            if (busy)
                return false;
            SetBusy(true);
            try
            {
                await Task.Run(work);
                return true;
            }
            catch (Exception ex)
            {
                Log($"{what}: {ex.Message}");
                MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            finally
            {
                SetBusy(false);
                RefreshVersion();
            }
        }

        void RefreshVersion()
        {
            var installed = Paths.InstalledVersion();
            var text = "Installed: " + (installed != null ? "v" + installed.ToString(3) : "nothing yet");
            if (latest?.Version != null)
                text += "    Latest on GitHub: v" + latest.Version.ToString(3) + (installed == null || latest.Version > installed ? "  (update available)" : "  (up to date)");
            versionLabel.Text = text + "    Launcher v" + Paths.LauncherVersion.ToString(3);
            updateButton.Text = latest != null && (installed == null || latest.Version > installed || latest.Version > Paths.LauncherVersion) ?
                (installed == null ? "Install" : "Update") : "Check for updates";
        }

        // ------------------------------------------------------------ start
        async Task Startup()
        {
            Log($"Launcher v{Paths.LauncherVersion.ToString(3)} in {Paths.Root}");
            if (!GameLocator.IsGame(settings.GamePath))
            {
                var found = GameLocator.Find();
                if (found != null)
                {
                    settings.GamePath = found;
                    settings.Save();
                    Log("Found the game: " + found);
                }
                else
                    Log("The game wasn't found in Steam's libraries: choose its folder with Browse...");
            }
            gameBox.Text = settings.GamePath;
            RefreshVersion();
            await CheckLatest(false);
        }

        void BrowseGame()
        {
            using (var dlg = new FolderBrowserDialog { Description = "The CSNZ folder (it has Bin\\cstrike-online.exe)", SelectedPath = settings.GamePath })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK)
                    return;
                var dir = dlg.SelectedPath;
                if (!GameLocator.IsGame(dir) && GameLocator.IsGame(Path.GetDirectoryName(dir)))
                    dir = Path.GetDirectoryName(dir); // they picked Bin
                if (!GameLocator.IsGame(dir))
                {
                    MessageBox.Show(this, "That folder has no Bin\\cstrike-online.exe.", Text);
                    return;
                }
                settings.GamePath = dir;
                settings.Save();
                gameBox.Text = dir;
                Log("Game folder: " + dir);
            }
        }

        void AskToken()
        {
            using (var f = new Form { Text = "GitHub token", ClientSize = new Size(480, 150), FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, MinimizeBox = false, MaximizeBox = false })
            {
                var info = new Label { Text = "The repository is private: a token with read access to it lets the launcher download the updates.\nIt is saved encrypted for your Windows user. Leave it empty to remove it.", Location = new Point(12, 10), Size = new Size(456, 48) };
                var box = new TextBox { Location = new Point(12, 64), Width = 456, UseSystemPasswordChar = true };
                var ok = new Button { Text = "Save", DialogResult = DialogResult.OK, Location = new Point(312, 104) };
                var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Location = new Point(393, 104) };
                f.Controls.AddRange(new Control[] { info, box, ok, cancel });
                f.AcceptButton = ok;
                f.CancelButton = cancel;
                if (f.ShowDialog(this) != DialogResult.OK)
                    return;
                settings.Token = box.Text;
                settings.Save();
                Log(string.IsNullOrWhiteSpace(box.Text) ? "GitHub token removed." : "GitHub token saved.");
            }
            _ = CheckLatest(true);
        }

        // ------------------------------------------------------------ updates
        async Task CheckLatest(bool loud)
        {
            try
            {
                var token = GitHub.FindToken(settings, out var source);
                Log($"Checking GitHub for updates (token: {source})...");
                latest = await GitHub.Latest(token);
                var installed = Paths.InstalledVersion();
                Log($"Latest release: {latest.Tag}" + (installed != null && latest.Version != null && latest.Version <= installed ? " (installed)" : ""));
            }
            catch (Exception ex)
            {
                latest = null;
                Log("Update check: " + ex.Message);
                if (loud)
                    MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            RefreshVersion();
        }

        async void UpdateClicked()
        {
            if (busy)
                return;
            if (latest == null)
            {
                SetBusy(true);
                await CheckLatest(true);
                SetBusy(false);
                if (latest == null)
                    return;
            }
            var installed = Paths.InstalledVersion();
            bool modOld = installed == null || latest.Version > installed, launcherOld = latest.Version > Paths.LauncherVersion;
            if (!modOld && !launcherOld)
            {
                Log("Everything is up to date.");
                SetBusy(true);
                await CheckLatest(true);
                SetBusy(false);
                return;
            }
            if (Paths.IsDevCheckout)
            {
                MessageBox.Show(this, "This launcher sits in a Git checkout of the project: updating would overwrite the sources. Copy it to its own folder.", Text);
                return;
            }
            if (Runner.GameRunning() || Runner.ServerRunning())
            {
                MessageBox.Show(this, "Close the game (and the local server) first.", Text);
                return;
            }
            var token = GitHub.FindToken(settings, out _);
            string newLauncher = null;
            var ok = await Run("Update", () =>
            {
                if (modOld)
                {
                    var zip = latest.Find(n => n.StartsWith("CSNZ-Offline-Revived-", StringComparison.OrdinalIgnoreCase) && n.EndsWith(".zip", StringComparison.OrdinalIgnoreCase));
                    if (zip == null)
                        throw new Exception("The release has no mod package (.zip).");
                    var tmp = Path.Combine(Path.GetTempPath(), zip.Name);
                    Log($"Downloading {zip.Name} ({zip.Size / 1048576} MB)...");
                    GitHub.Download(zip, token, tmp, Log).GetAwaiter().GetResult();
                    Log("Installing...");
                    Installer.InstallZip(tmp, Log);
                    File.Delete(tmp);
                    Log($"Mod updated to {latest.Tag}.");
                }
                if (launcherOld)
                {
                    var exe = latest.Find(n => n.EndsWith(".exe", StringComparison.OrdinalIgnoreCase));
                    if (exe != null)
                    {
                        newLauncher = Application.ExecutablePath + ".new";
                        Log("Downloading the new launcher...");
                        GitHub.Download(exe, token, newLauncher, Log).GetAwaiter().GetResult();
                    }
                }
            });
            if (ok && newLauncher != null && File.Exists(newLauncher))
                SwapLauncher(newLauncher);
        }

        // the running exe can't overwrite itself: a small script waits for it to close, swaps the files and starts the new one
        void SwapLauncher(string newExe)
        {
            var exe = Application.ExecutablePath;
            var script = Path.Combine(Path.GetTempPath(), "csnz_launcher_update.cmd");
            File.WriteAllText(script,
                "@echo off\r\n" +
                $"for /l %%i in (1,1,20) do (move /y \"{newExe}\" \"{exe}\" >nul 2>&1 && goto done & timeout /t 1 /nobreak >nul)\r\n" +
                ":done\r\n" +
                $"start \"\" \"{exe}\"\r\n" +
                "del \"%~f0\"\r\n");
            Log("Restarting the launcher with its new version...");
            Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + script + "\"") { CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden, UseShellExecute = false });
            busy = false;
            Close();
        }

        // ------------------------------------------------------------ play
        async void PlayClicked()
        {
            if (busy)
                return;
            if (Paths.InstalledVersion() == null || !File.Exists(Paths.PatchList))
            {
                MessageBox.Show(this, "The mod isn't installed yet: press Install first.", Text);
                return;
            }
            if (Runner.GameRunning())
            {
                MessageBox.Show(this, "The game is already open.", Text);
                return;
            }
            string game;
            try { game = Game(); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, Text); return; }

            if (!Hosts.HasBlock())
            {
                var answer = MessageBox.Show(this,
                    "The game still points at Nexon's servers. The launcher adds a block to the Windows hosts file that sends those addresses to this PC (127.0.0.1); Windows will ask for administrator rights.\n\nAdd it now?",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                if (answer != DialogResult.Yes || !await Run("hosts", () => { if (!Hosts.Run("add", Log)) throw new Exception("The hosts block wasn't added."); }))
                    return;
            }
            if (!Assets.Present)
                Log("Note: the shop and profile pictures aren't exported yet (More > Export...). The game works without them.");

            Process server = null;
            WindowState = FormWindowState.Minimized;
            await Run("Play", () =>
            {
                Certificates.Ensure(Log);
                Patcher.Apply(game, Log);
                if (Runner.Listening(Runner.ServerPort))
                    Log("A local server is already running: using it.");
                else
                    server = Runner.StartServer(Log);
                try
                {
                    Runner.StartGame(game, Log);
                    Runner.WaitForGame();
                    Log("The game was closed.");
                }
                finally
                {
                    if (server != null && !server.HasExited)
                    {
                        System.Threading.Thread.Sleep(5000); // the server saves the account after the client leaves
                        try { server.Kill(); } catch { }
                        Log("Local server stopped.");
                    }
                }
            });
            WindowState = FormWindowState.Normal;
            Activate();
        }

        void RestoreGame()
        {
            if (Runner.GameRunning())
            {
                MessageBox.Show(this, "Close the game first.", Text);
                return;
            }
            if (MessageBox.Show(this, "Put back Steam's original game files and remove the mod's files from Bin?\n(The hosts block stays: remove it from More if you want to play online.)",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            _ = Run("Restore", () => Patcher.Restore(Game(), Log));
        }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            if (args.Length > 0 && args[0] == "--check")
            {
                // a test without the window (written to launcher-check.txt): the game's folder, the patches' state and the latest release
                var lines = new System.Collections.Generic.List<string>();
                var g = GameLocator.Find();
                lines.Add("game: " + (g ?? "not found"));
                lines.Add("installed: " + (Paths.InstalledVersion()?.ToString(3) ?? "nothing"));
                try
                {
                    if (g != null && File.Exists(Paths.PatchList))
                        lines.Add("patches: " + Patcher.Status(g));
                }
                catch (Exception ex) { lines.Add("patches: " + ex.Message); }
                try
                {
                    var r = GitHub.Latest(GitHub.FindToken(Settings.Load(), out var src)).GetAwaiter().GetResult();
                    lines.Add($"latest: {r.Tag} ({string.Join(", ", r.Assets.Select(a => a.Name))}), token {src}");
                    if (args.Contains("--download"))
                    {
                        var zip = r.Find(n => n.EndsWith(".zip", StringComparison.OrdinalIgnoreCase));
                        var tmp = Path.Combine(Path.GetTempPath(), "csnz-check-" + zip.Name);
                        GitHub.Download(zip, GitHub.FindToken(Settings.Load(), out _), tmp, l => { }).GetAwaiter().GetResult();
                        lines.Add($"downloaded {zip.Name}: {new FileInfo(tmp).Length} bytes (release says {zip.Size})");
                        File.Delete(tmp);
                    }
                }
                catch (Exception ex) { lines.Add("latest: " + ex.Message); }
                if (args.Length > 1 && args[1] == "--install" && args.Length > 2)
                {
                    Installer.InstallZip(args[2], l => lines.Add(l));
                    Certificates.Ensure(l => lines.Add(l));
                }
                File.WriteAllLines(Path.Combine(Paths.Root, "launcher-check.txt"), lines);
                return;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
