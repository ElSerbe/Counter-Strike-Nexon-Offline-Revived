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
        readonly Button patchButton = new Button { Text = "Patch the game", Width = 170, Height = 34 };
        readonly Label gameState = new Label { AutoSize = true, Font = new Font("Segoe UI", 9, FontStyle.Bold) };
        readonly LinkLabel getGame = new LinkLabel { Text = "Not installed? Get it free on Steam", AutoSize = true, Visible = false };
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
            gameState.Location = new Point(16, 108);
            getGame.Location = new Point(480, 108);
            getGame.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            versionLabel.Location = new Point(16, 132);
            updateButton.Location = new Point(16, 160);
            playButton.Location = new Point(176, 160);
            patchButton.Location = new Point(336, 160);
            moreButton.Location = new Point(516, 160);
            logBox.SetBounds(16, 206, 688, 238);
            logBox.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right | AnchorStyles.Bottom;
            Controls.AddRange(new Control[] { title, gameLabel, gameBox, browse, gameState, getGame, versionLabel, updateButton, playButton, patchButton, moreButton, logBox });

            var more = new ContextMenuStrip();
            more.Items.Add("Export shop and profile pictures from the game (Python)", null, (s, e) => _ = Run("Exporting pictures", () => Assets.Export(Game(), Log)));
            more.Items.Add("Verify the game's files with Steam", null, (s, e) => OpenSteam(GameLocator.VerifyCommand));
            more.Items.Add("Get the game from Steam...", null, (s, e) => GetGame());
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
            patchButton.Click += (s, e) => PatchClicked();
            getGame.LinkClicked += (s, e) => GetGame();
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
            updateButton.Enabled = playButton.Enabled = moreButton.Enabled = patchButton.Enabled = !on;
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
            RefreshGameState();
        }

        // the game's line under its folder, and the Patch / Unpatch button
        Patcher.GameState RefreshGameState()
        {
            bool has = GameLocator.IsGame(settings.GamePath);
            getGame.Visible = !has;
            if (!has)
            {
                gameState.Text = "Game: not found";
                gameState.ForeColor = Color.Firebrick;
                patchButton.Text = "Patch the game";
                patchButton.Enabled = false;
                return Patcher.GameState.NoMod;
            }
            Patcher.GameState st;
            try { st = Patcher.Check(settings.GamePath); }
            catch { st = Patcher.GameState.NoMod; }
            switch (st)
            {
                case Patcher.GameState.NoMod:
                    gameState.Text = "Game: found (install the mod to patch it)";
                    gameState.ForeColor = SystemColors.ControlText;
                    break;
                case Patcher.GameState.Original:
                    gameState.Text = "Game: original (Steam's files, not patched)";
                    gameState.ForeColor = SystemColors.ControlText;
                    break;
                case Patcher.GameState.Patched:
                    gameState.Text = "Game: patched for offline play";
                    gameState.ForeColor = Color.ForestGreen;
                    break;
                case Patcher.GameState.Outdated:
                    gameState.Text = "Game: patched by an older release (Patch or Play updates it)";
                    gameState.ForeColor = Color.DarkOrange;
                    break;
                case Patcher.GameState.Partly:
                    gameState.Text = "Game: partly patched";
                    gameState.ForeColor = Color.DarkOrange;
                    break;
                default:
                    gameState.Text = "Game: doesn't match this mod (Steam updated it?)";
                    gameState.ForeColor = Color.Firebrick;
                    break;
            }
            bool unpatch = st == Patcher.GameState.Patched || st == Patcher.GameState.Partly || st == Patcher.GameState.Mismatch;
            patchButton.Text = unpatch ? "Unpatch (original game)" : "Patch the game";
            patchButton.Enabled = !busy && st != Patcher.GameState.NoMod;
            return st;
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
                    Log("The game wasn't found in Steam's libraries.");
            }
            gameBox.Text = settings.GamePath;
            if (GameLocator.IsGame(settings.GamePath))
                Log("Game: " + settings.GamePath + (GameLocator.BuildId(settings.GamePath) is string b ? $" (Steam build {b})" : ""));
            RefreshVersion();
            if (!GameLocator.IsGame(settings.GamePath))
                GetGame();
            await CheckLatest(false);
        }

        // the game isn't on this PC: how to get it from Steam, and a watch that picks it up once installed
        void GetGame()
        {
            using (var f = new GetGameForm())
            {
                if (f.ShowDialog(this) != DialogResult.OK)
                    return;
                settings.GamePath = f.FoundPath;
                settings.Save();
                gameBox.Text = f.FoundPath;
                Log("Found the game: " + f.FoundPath);
                RefreshVersion();
            }
        }

        void OpenSteam(string command)
        {
            try { Process.Start(command); }
            catch (Exception ex) { MessageBox.Show(this, "Steam didn't open: " + ex.Message, Text); }
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
                RefreshVersion();
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
            if (!GameLocator.IsGame(settings.GamePath))
            {
                GetGame();
                return;
            }
            var game = settings.GamePath;

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

        // Patch the game / Unpatch it back to Steam's files
        async void PatchClicked()
        {
            if (busy)
                return;
            if (Runner.GameRunning())
            {
                MessageBox.Show(this, "Close the game first.", Text);
                return;
            }
            var st = RefreshGameState();
            if (st == Patcher.GameState.Original || st == Patcher.GameState.Outdated)
            {
                await Run("Patching", () => Patcher.Apply(Game(), Log));
                return;
            }
            if (MessageBox.Show(this, "Unpatch the game: put back Steam's original files and take the mod's files out of Bin?\n\n" +
                    "Your mod settings are kept for the next time you patch (or press Play). The hosts block stays: remove it from More if you want to play online.",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            bool complete = true;
            if (await Run("Unpatch", () => complete = Patcher.Restore(Game(), Log)) && !complete &&
                MessageBox.Show(this, "Some patched files had no backup. Let Steam verify the game's files to download them again?",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                OpenSteam(GameLocator.VerifyCommand);
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
                // --game <folder> tests on another copy of the game; --patch / --unpatch change it
                int gi = Array.IndexOf(args, "--game");
                var g = gi > 0 && gi + 1 < args.Length ? args[gi + 1] : GameLocator.Find();
                lines.Add("game: " + (g ?? "not found"));
                lines.Add("installed: " + (Paths.InstalledVersion()?.ToString(3) ?? "nothing"));
                try
                {
                    if (g != null && File.Exists(Paths.PatchList))
                    {
                        lines.Add("patches: " + Patcher.Status(g) + ", game state " + Patcher.Check(g) + ", Steam build " + GameLocator.BuildId(g));
                        if (args.Contains("--patch"))
                        {
                            Patcher.Apply(g, l => lines.Add(l));
                            lines.Add("after patch: " + Patcher.Check(g));
                        }
                        if (args.Contains("--unpatch"))
                        {
                            lines.Add("restore complete: " + Patcher.Restore(g, l => lines.Add(l)));
                            lines.Add("after unpatch: " + Patcher.Check(g));
                        }
                    }
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
            if (args.Contains("--preview-get-game"))
            {
                Application.Run(new GetGameForm(true));
                return;
            }
            Application.Run(new MainForm());
        }
    }
}
