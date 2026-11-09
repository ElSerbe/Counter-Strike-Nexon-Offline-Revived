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
        readonly Header header = new Header { Dock = DockStyle.Top, Height = 214 };
        readonly Card gameCard = new Card { Title = "Game" };
        readonly Card logCard = new Card { Title = "Activity" };
        readonly TextBox gameBox = new TextBox { ReadOnly = true };
        readonly Label versionLabel = new Label { AutoSize = true };
        readonly FlatButton updateButton = new FlatButton { Text = "Check for updates" };
        readonly FlatButton playButton = new FlatButton { Text = "Play", Primary = true, Sub = "Starts the local server and the game" };
        readonly FlatButton moreButton = new FlatButton { Text = "More  \u25BE" };
        readonly FlatButton logsButton = new FlatButton { Text = "Logs  \u25BE" };
        readonly FlatButton picturesButton = new FlatButton { Text = "Pictures" };
        readonly ToolTip tips = new ToolTip();
        readonly Toggle detailBox = new Toggle { Text = "Detailed logs: the server and the Steam bridge log every packet (for bug reports)" };
        readonly FlatButton patchButton = new FlatButton { Text = "Patch game" };
        readonly FlatButton browseButton = new FlatButton { Text = "Browse" };
        readonly Toggle onlineBox = new Toggle { Text = "Online play through Steam: other players' rooms in Game lobby, yours in theirs" };
        readonly Label gameState = new Label { AutoSize = true };
        readonly LinkLabel getGame = new LinkLabel { Text = "Not installed? Get it free on Steam", AutoSize = true, Visible = false };
        readonly TextBox logBox = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.None, Font = new Font("Consolas", 9) };
        const string DiscordUrl = "https://discord.gg/fjxubwyhYv";
        const string PatreonUrl = "https://patreon.com/ltsYuri";
        readonly Music music = new Music();
        readonly bool autoPlay;
        ReleaseInfo latest;
        bool busy;

        // autoPlay (--play, the desktop shortcut): plays as soon as the checks are done and closes with the game
        public MainForm(bool autoPlay = false)
        {
            this.autoPlay = autoPlay;
            Text = "Counter-Strike Nexon: Offline Revived";
            FormBorderStyle = FormBorderStyle.None;
            ClientSize = new Size(980, 640);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Back;
            ForeColor = Theme.Text;
            Font = Theme.UiFont(13);
            DoubleBuffered = true;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            header.Credits = "Offline port and rework by ItsYuri  \u00B7  Local server: CSNZ_Server by Jusic, Hardee, NekoMeow, Smilex_Gamer and xRiseless";
            header.LogoClicked += (s, e) => Process.Start(DiscordUrl);
            header.DiscordClicked += (s, e) => Process.Start(DiscordUrl);
            header.PatreonClicked += (s, e) => Process.Start(PatreonUrl);
            // the speaker stops the music for now (or starts it again); More turns it off for good
            header.MusicClicked += (s, e) =>
            {
                if (music.Playing)
                    music.Stop();
                else if (!music.Play(settings.GamePath))
                    Log("No music: the game's Bin\\CSOnline.mp3 wasn't found or Windows can't play it (" + Music.LastError + ").");
            };
            music.Changed += (s, e) => header.MusicOn = music.Playing;
            FormClosed += (s, e) => music.Dispose();

            // the game card
            gameCard.SetBounds(24, 222, 600, 206);
            var folderLabel = new Label { Text = "Steam's Counter-Strike Nexon folder", AutoSize = true, ForeColor = Theme.Muted, Location = new Point(18, 42) };
            gameBox.SetBounds(18, 62, 466, 26);
            Theme.Dark(gameBox);
            gameBox.Font = Theme.UiFont(13);
            browseButton.SetBounds(494, 60, 88, 30);
            browseButton.Font = Theme.Display(15);
            gameState.Location = new Point(18, 100);
            gameState.Font = Theme.UiFont(13, FontStyle.Bold);
            getGame.Location = new Point(360, 100);
            getGame.LinkColor = getGame.ActiveLinkColor = Theme.Accent;
            getGame.VisitedLinkColor = Theme.Accent;
            versionLabel.Location = new Point(18, 122);
            versionLabel.ForeColor = Theme.Muted;
            onlineBox.SetBounds(16, 144, 570, 26);
            onlineBox.BackColor = Theme.Panel;
            detailBox.SetBounds(16, 172, 570, 26);
            detailBox.BackColor = Theme.Panel;
            foreach (Control c in new Control[] { folderLabel, gameState, getGame, versionLabel })
                c.BackColor = Theme.Panel;
            gameCard.Controls.AddRange(new Control[] { folderLabel, gameBox, browseButton, gameState, getGame, versionLabel, onlineBox, detailBox });

            // the actions
            playButton.SetBounds(648, 222, 308, 100);
            playButton.Font = Theme.Display(34);
            updateButton.SetBounds(648, 330, 150, 44);
            patchButton.SetBounds(806, 330, 150, 44);
            moreButton.SetBounds(648, 384, 98, 44);
            logsButton.SetBounds(753, 384, 98, 44);
            picturesButton.SetBounds(858, 384, 98, 44);
            tips.SetToolTip(picturesButton, "Exports the shop's item pictures and 3D previews, the medal icons and the mission pictures from your game\n" +
                                            "for the in-game web pages. Installs Python for your Windows user first if it isn't there.");
            foreach (var bt in new[] { updateButton, patchButton, moreButton, logsButton, picturesButton })
                bt.Font = Theme.Display(16);

            // the log
            logCard.SetBounds(24, 440, 932, 178);
            logBox.SetBounds(16, 40, 900, 124);
            Theme.Dark(logBox);
            logBox.BorderStyle = BorderStyle.None;
            logBox.BackColor = Theme.Panel;
            logBox.ForeColor = Color.FromArgb(170, 184, 184);
            logCard.Controls.Add(logBox);

            Controls.AddRange(new Control[] { gameCard, playButton, updateButton, patchButton, moreButton, logsButton, picturesButton, logCard, header });
            picturesButton.Click += (s, e) => PicturesClicked();
            onlineBox.Checked = settings.OnlinePlay;
            onlineBox.CheckedChanged += (s, e) => { settings.OnlinePlay = onlineBox.Checked; settings.Save(); onlineBox.Invalidate(); };
            detailBox.Checked = settings.DetailedLogs;
            detailBox.CheckedChanged += (s, e) =>
            {
                settings.DetailedLogs = detailBox.Checked;
                settings.Save();
                detailBox.Invalidate();
                Log(detailBox.Checked ? "Detailed logs on: the next Play logs every packet (bigger log files)." : "Detailed logs off.");
            };

            // the last session's logs: launcher, Steam bridge, server and game in one text
            var logsMenu = new ContextMenuStrip { Renderer = new DarkMenuRenderer(), BackColor = Theme.Panel, ForeColor = Theme.Text, ShowImageMargin = false, Font = Theme.UiFont(13) };
            logsMenu.Items.Add("Copy the last session's logs", null, (s, e) => CopyLogs());
            logsMenu.Items.Add("Open the last session's logs", null, (s, e) => OpenLogs());
            logsMenu.Items.Add("Open the logs folder", null, (s, e) => { Directory.CreateDirectory(Paths.LogDir); Process.Start("explorer.exe", "\"" + Paths.LogDir + "\""); });
            logsButton.Click += (s, e) => logsMenu.Show(logsButton, new Point(0, logsButton.Height));

            var more = new ContextMenuStrip { Renderer = new DarkMenuRenderer(), BackColor = Theme.Panel, ForeColor = Theme.Text, ShowImageMargin = false, Font = Theme.UiFont(13) };
            more.Items.Add("Create a desktop shortcut that starts the game", null, (s, e) => CreateShortcut());
            more.Items.Add("Verify the game's files with Steam", null, (s, e) => OpenSteam(GameLocator.VerifyCommand));
            more.Items.Add("Get the game (Steam or archive.org)...", null, (s, e) => GetGame());
            more.Items.Add("Antivirus warnings: is this safe?", null, (s, e) => ShowAntivirusInfo());
            more.Items.Add(new ToolStripSeparator());
            more.Items.Add("Add the hosts block", null, (s, e) => _ = Run("hosts", () => Hosts.Run("add", Log)));
            more.Items.Add("Remove the hosts block", null, (s, e) => _ = Run("hosts", () => Hosts.Run("remove", Log)));
            more.Items.Add(new ToolStripSeparator());
            more.Items.Add("Open the mod's folder", null, (s, e) => Process.Start("explorer.exe", "\"" + Paths.Mod + "\""));
            more.Items.Add("Open the project on GitHub", null, (s, e) => Process.Start(GitHub.PageUrl));
            more.Items.Add("Join the Discord", null, (s, e) => Process.Start(DiscordUrl));
            more.Items.Add("Support the project on Patreon", null, (s, e) => Process.Start(PatreonUrl));
            more.Items.Add(new ToolStripSeparator());
            var musicItem = more.Items.Add("", null, (s, e) =>
            {
                settings.MusicOff = !settings.MusicOff;
                settings.Save();
                if (settings.MusicOff)
                {
                    music.Stop();
                    Log("Launcher music turned off: it won't play any more (More turns it back on).");
                }
                else
                {
                    Log("Launcher music turned on.");
                    StartMusic();
                }
            });
            more.Opening += (s, e) => musicItem.Text = settings.MusicOff ? "Turn the launcher music on" : "Turn the launcher music off";
            moreButton.Click += (s, e) => more.Show(moreButton, new Point(0, moreButton.Height));

            browseButton.Click += (s, e) => BrowseGame();
            updateButton.Click += (s, e) => UpdateClicked();
            playButton.Click += (s, e) => PlayClicked();
            patchButton.Click += (s, e) => PatchClicked();
            getGame.LinkClicked += (s, e) => GetGame();
            Shown += async (s, e) => await Startup();
            FormClosing += (s, e) => { if (busy && e.CloseReason == CloseReason.UserClosing && DarkBox.Show(this, "Something is still running. Close anyway?", Text, MessageBoxButtons.YesNo) == DialogResult.No) e.Cancel = true; };
        }

        // a thin frame around the borderless window
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using (var p = new Pen(Theme.PanelLine))
                e.Graphics.DrawRectangle(p, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
        }

        // a shortcut on the desktop to this launcher in --play mode, with its icon
        void CreateShortcut()
        {
            try
            {
                var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                var link = Path.Combine(desktop, "Counter-Strike Nexon Offline Revived.lnk");
                var shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
                dynamic sc = shell.GetType().InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { link });
                sc.TargetPath = Application.ExecutablePath;
                sc.Arguments = "--play";
                sc.WorkingDirectory = Paths.Root;
                sc.IconLocation = Application.ExecutablePath + ",0";
                sc.Description = "Counter-Strike Nexon: Offline Revived";
                sc.Save();
                Log("Desktop shortcut created: it opens the launcher and starts the game.");
                DarkBox.Show(this, "\"Counter-Strike Nexon Offline Revived\" is on your desktop: it starts the game directly.", Text);
            }
            catch (Exception ex)
            {
                DarkBox.Show(this, "Couldn't create the shortcut: " + ex.Message, Text);
            }
        }

        // the game's lobby theme from its own folder, unless More turned it off (or the shortcut goes straight to the game)
        void StartMusic()
        {
            if (settings.MusicOff || autoPlay || busy || Runner.GameRunning() || music.Playing)
                return;
            if (GameLocator.IsGame(settings.GamePath) && !music.Play(settings.GamePath))
                Log("No music: the game's Bin\\CSOnline.mp3 wasn't found or Windows can't play it (" + Music.LastError + ").");
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
            updateButton.Enabled = playButton.Enabled = moreButton.Enabled = patchButton.Enabled = picturesButton.Enabled = !on;
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
                DarkBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
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
                gameState.ForeColor = Theme.Bad;
                patchButton.Text = "Patch game";
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
                    gameState.ForeColor = Theme.Text;
                    break;
                case Patcher.GameState.Original:
                    gameState.Text = "Game: original (Steam's files, not patched)";
                    gameState.ForeColor = Theme.Text;
                    break;
                case Patcher.GameState.Patched:
                    gameState.Text = "Game: patched for offline play";
                    gameState.ForeColor = Theme.Good;
                    break;
                case Patcher.GameState.Outdated:
                    gameState.Text = "Game: patched by an older release (Patch or Play updates it)";
                    gameState.ForeColor = Theme.Warn;
                    break;
                case Patcher.GameState.Partly:
                    gameState.Text = "Game: partly patched";
                    gameState.ForeColor = Theme.Warn;
                    break;
                default:
                    gameState.Text = "Game: doesn't match this mod (Steam updated it?)";
                    gameState.ForeColor = Theme.Bad;
                    break;
            }
            bool unpatch = st == Patcher.GameState.Patched || st == Patcher.GameState.Partly || st == Patcher.GameState.Mismatch;
            patchButton.Text = unpatch ? "Unpatch game" : "Patch game";
            patchButton.Enabled = !busy && st != Patcher.GameState.NoMod;
            return st;
        }

        // ------------------------------------------------------------ start
        async Task Startup()
        {
            Log($"Launcher v{Paths.LauncherVersion.ToString(3)} in {Paths.Root}");
            // the previous launcher, renamed by an update (SwapLauncher)
            try { File.Delete(Application.ExecutablePath + ".old"); } catch { }
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
            StartMusic();
            if (!GameLocator.IsGame(settings.GamePath))
                GetGame();
            await CheckLatest(false);
            if (!autoPlay && !settings.AntivirusInfoShown)
                ShowAntivirusInfo();
            else if (!autoPlay)
                await CheckRemoved();
            if (autoPlay)
            {
                // the desktop shortcut: straight into the game, the launcher closes with it
                if (await PlayAsync())
                    Close();
            }
        }

        // the game isn't on this PC: how to get it from Steam, and a watch that picks it up once installed
        void GetGame()
        {
            using (var f = new GetGameForm(false, Log))
            {
                if (f.ShowDialog(this) != DialogResult.OK)
                    return;
                settings.GamePath = f.FoundPath;
                settings.Save();
                gameBox.Text = f.FoundPath;
                Log("Found the game: " + f.FoundPath);
                RefreshVersion();
                StartMusic();
            }
        }

        void OpenSteam(string command)
        {
            try { Process.Start(command); }
            catch (Exception ex) { DarkBox.Show(this, "Steam didn't open: " + ex.Message, Text); }
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
                    DarkBox.Show(this, "That folder has no Bin\\cstrike-online.exe.", Text);
                    return;
                }
                settings.GamePath = dir;
                settings.Save();
                gameBox.Text = dir;
                Log("Game folder: " + dir);
                RefreshVersion();
                StartMusic();
            }
        }

        // ------------------------------------------------------------ pictures
        // the in-game web pages' pictures and 3D previews, made from the player's own game by the mod's Python scripts.
        // No Python: python.org's newest installer, for this Windows user (asked first)
        async void PicturesClicked()
        {
            if (busy)
                return;
            if (Paths.InstalledVersion() == null)
            {
                DarkBox.Show(this, "The mod isn't installed yet: press Install first.", Text);
                return;
            }
            if (!GameLocator.IsGame(settings.GamePath))
            {
                GetGame();
                return;
            }
            if (Assets.Present && DarkBox.Show(this, "The pictures are already exported. Export them again (a few minutes)?", Text,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            bool install = false;
            SetBusy(true);
            bool hasPython = await Task.Run(() => Assets.HasPython());
            SetBusy(false);
            if (!hasPython)
            {
                if (DarkBox.Show(this,
                        "The pictures are made by Python scripts, and Python isn't installed on this PC.\n\n" +
                        "Download the latest Python from python.org (about 30 MB) and install it for your Windows user? " +
                        "No administrator rights are needed; it shows up in Windows' Installed apps like any program.",
                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                    return;
                install = true;
            }
            settings.PicturesAsked = true;
            settings.Save();
            if (await ExportPictures(install))
                DarkBox.Show(this, "The pictures are ready: the in-game shop, profile and missions pages show them now.", Text);
        }

        // Python first when `install`, then the mod's scripts; true when the pictures are ready
        Task<bool> ExportPictures(bool install)
        {
            var game = settings.GamePath;
            return Run("Pictures", () =>
            {
                if (install)
                    Assets.InstallPython(Log);
                Assets.Export(game, Log);
            });
        }

        public static string PicturesQuestion(bool hasPython) =>
            "Extract the item pictures and 3D models from your game?\n\n" +
            "The in-game shop, the rewards, the profile's medals and the missions show every item with its picture and 3D preview. " +
            "They're made from your own game's files (a few minutes) and stay through updates.\n\n" +
            (hasPython ? "" : "Python isn't on this PC: the launcher first downloads the latest one from python.org (about 30 MB) and installs it " +
                              "for your Windows user, without administrator rights.\n\n") +
            "Not now: the game works without them, and the Pictures button extracts them any time. This question won't come back.";

        public static string[] PicturesButtons(bool beforePlay) => new[] { beforePlay ? "Extract, then play" : "Extract now", "Not now" };

        // Asked once, after the mod is installed or before the first Play: the pictures from the player's game, or not now.
        // Not again after that answer (the Pictures button stays), nor when they're already there (updates keep them).
        async Task OfferPictures(bool beforePlay)
        {
            if (busy || settings.PicturesAsked || Assets.Present || Paths.InstalledVersion() == null || !GameLocator.IsGame(settings.GamePath))
                return;
            SetBusy(true);
            bool hasPython = await Task.Run(() => Assets.HasPython());
            SetBusy(false);
            var answer = DarkBox.Show(this, PicturesQuestion(hasPython), "Item pictures", MessageBoxButtons.YesNo, MessageBoxIcon.Question,
                                      PicturesButtons(beforePlay));
            settings.PicturesAsked = true;
            settings.Save();
            if (answer != DialogResult.Yes)
            {
                Log("Item pictures: not now (the Pictures button extracts them any time).");
                return;
            }
            if (await ExportPictures(!hasPython))
                Log("The item pictures are ready: the in-game shop, profile and missions pages show them.");
            else
                Log("The item pictures weren't extracted: press Pictures to try again.");
        }

        // ------------------------------------------------------------ logs
        string BuildLogs()
        {
            try { return LogBundle.Build(settings); }
            catch (Exception ex)
            {
                DarkBox.Show(this, "Couldn't gather the logs: " + ex.Message, Text);
                return null;
            }
        }

        void CopyLogs()
        {
            var text = BuildLogs();
            if (text == null)
                return;
            try
            {
                Clipboard.SetText(text);
                Log($"The last session's logs are copied ({text.Length / 1024} KB): paste them in a message, or send {LogBundle.FilePath}.");
            }
            catch (Exception ex)
            {
                DarkBox.Show(this, "Couldn't copy them (" + ex.Message + "). They are in " + LogBundle.FilePath, Text);
            }
        }

        void OpenLogs()
        {
            if (BuildLogs() == null)
                return;
            try { Process.Start("notepad.exe", "\"" + LogBundle.FilePath + "\""); }
            catch (Exception ex) { DarkBox.Show(this, "Couldn't open " + LogBundle.FilePath + ": " + ex.Message, Text); }
        }

        // ------------------------------------------------------------ updates
        async Task CheckLatest(bool loud)
        {
            try
            {
                var token = GitHub.FindToken(settings, out _);
                Log("Checking GitHub for updates...");
                latest = await GitHub.Latest(token);
                var installed = Paths.InstalledVersion();
                Log($"Latest release: {latest.Tag}" + (installed != null && latest.Version != null && latest.Version <= installed ? " (installed)" : ""));
            }
            catch (Exception ex)
            {
                latest = null;
                Log("Update check: " + ex.Message);
                if (loud)
                    DarkBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
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
                DarkBox.Show(this, "This launcher sits in a Git checkout of the project: updating would overwrite the sources. Copy it to its own folder.", Text);
                return;
            }
            if (!CloseRunning("Updating"))
                return;
            var token = GitHub.FindToken(settings, out _);
            string newLauncher = null;
            var ok = await Run("Update", () =>
            {
                if (modOld)
                    DownloadMod(token);
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
            else if (ok && await CheckRemoved())
                await OfferPictures(false); // the new launcher asks before its first Play instead
        }

        // the latest release's mod package, downloaded and installed over the mod (the player's own files are kept)
        void DownloadMod(string token)
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
            Log($"Mod installed: {latest.Tag}.");
        }

        // ------------------------------------------------------------ antivirus
        // The explanation (first start, More), or, when an antivirus took some of the mod's programs away, the same
        // window with them listed and a "Download the mod again". True when nothing is missing (any more).
        async Task<bool> CheckRemoved()
        {
            var missing = Antivirus.Missing();
            if (missing.Length == 0)
                return true;
            Log("Missing from the mod (an antivirus removed them?): " + string.Join(", ", missing));
            var installed = Paths.InstalledVersion();
            bool offer = latest?.Version != null && installed != null && latest.Version >= installed;
            DialogResult answer;
            using (var f = new AntivirusForm(missing, settings.GamePath, offer))
                answer = f.ShowDialog(this);
            if (answer != DialogResult.Retry || !CloseRunning("Downloading the mod again"))
                return false;
            var token = GitHub.FindToken(settings, out _);
            if (!await Run("Download", () => DownloadMod(token)))
                return false;
            missing = Antivirus.Missing();
            if (missing.Length == 0)
                return true;
            Log("Still missing after downloading them again: " + string.Join(", ", missing));
            DarkBox.Show(this, "The antivirus removed them again:\n   " + string.Join("\n   ", missing) +
                               "\n\nAdd the mod's folder as an exception in your antivirus (the window before shows it), then download them again.",
                         Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return false;
        }

        void ShowAntivirusInfo()
        {
            using (var f = new AntivirusForm(null, settings.GamePath, false))
                f.ShowDialog(this);
            settings.AntivirusInfoShown = true;
            settings.Save();
        }

        // The running exe can't be overwritten, but it can be renamed: it becomes .old (deleted by the new launcher when it
        // starts), the new one takes its name and is started. No script and no hidden console.
        void SwapLauncher(string newExe)
        {
            var exe = Application.ExecutablePath;
            var old = exe + ".old";
            try
            {
                if (File.Exists(old))
                    File.Delete(old);
                File.Move(exe, old);
                try { File.Move(newExe, exe); }
                catch { File.Move(old, exe); throw; }
            }
            catch (Exception ex)
            {
                Log("The new launcher couldn't replace this one (" + ex.Message + "): it's in " + newExe);
                DarkBox.Show(this, "The mod is updated, but the new launcher couldn't replace this one (" + ex.Message + ").\n\n" +
                                   "It was downloaded to:\n" + newExe + "\n\nClose this launcher and rename it to " + Path.GetFileName(exe) + ".", Text);
                return;
            }
            Log("Restarting the launcher with its new version...");
            Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = Paths.Root, UseShellExecute = false });
            busy = false;
            Close();
        }

        // ------------------------------------------------------------ play
        async void PlayClicked() => await PlayAsync();

        async Task<bool> PlayAsync()
        {
            if (busy)
                return false;
            if (Paths.InstalledVersion() == null || !File.Exists(Paths.PatchList))
            {
                DarkBox.Show(this, "The mod isn't installed yet: press Install first.", Text);
                return false;
            }
            if (Runner.GameRunning())
            {
                DarkBox.Show(this, "The game is already open.", Text);
                return false;
            }
            if (!GameLocator.IsGame(settings.GamePath))
            {
                GetGame();
                return false;
            }
            if (!await CheckRemoved())
                return false;
            var game = settings.GamePath;

            if (!Hosts.HasBlock())
            {
                var answer = DarkBox.Show(this,
                    "The game still points at Nexon's servers. The launcher adds a block to the Windows hosts file that sends those addresses to this PC (127.0.0.1); Windows will ask for administrator rights.\n\nAdd it now?",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                if (answer != DialogResult.Yes || !await Run("hosts", () => { if (!Hosts.Run("add", Log)) throw new Exception("The hosts block wasn't added."); }))
                    return false;
            }
            await OfferPictures(true);
            if (!Assets.Present)
                Log("Note: the shop and profile pictures aren't exported yet (the Pictures button). The game works without them.");

            // the music fades out while the game starts (the window minimizes; the fade keeps running)
            music.FadeOut();
            Process server = null, bridge = null;
            bool online = settings.OnlinePlay, verbose = settings.DetailedLogs;
            // where the Logs button's "last session" starts
            Log($"{LogBundle.PlayMark} {DateTime.Now:yyyy-MM-dd HH:mm:ss}: mod v{Paths.InstalledVersion()?.ToString(3)}, online play {(online ? "on" : "off")}, detailed logs {(verbose ? "on" : "off")} ===");
            WindowState = FormWindowState.Minimized;
            bool played = await Run("Play", () =>
            {
                Certificates.Ensure(Log);
                Patcher.Apply(game, Log);
                SteamApi.UseEmulatorIfNotSteam(game, Log); // the archive.org copy: starts without the game on the Steam account
                if (verbose)
                    Log("Game state after patching: " + Patcher.Check(game) + ", Steam build " + GameLocator.BuildId(game) + ", hosts block " + (Hosts.HasBlock() ? "yes" : "no"));
                if (Runner.Listening(Runner.ServerPort))
                    Log("A local server is already running: using it" + (verbose ? " (its detailed logs depend on how it was started)." : "."));
                else
                    server = Runner.StartServer(Log, verbose);
                if (online)
                    bridge = Runner.StartBridge(game, Log, verbose);
                try
                {
                    Runner.StartGame(game, Log);
                    Runner.WaitForGame();
                    Log("The game was closed.");
                }
                finally
                {
                    Runner.StopBridge(bridge, Log);
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
            return played;
        }

        // The game, the local server or the Steam bridge still running (the last two have no window, so a left-over
        // one is invisible): name them and offer to close them. True when nothing of ours runs any more.
        bool CloseRunning(string what)
        {
            var running = Runner.OursRunning();
            if (running.Length == 0)
                return true;
            var names = string.Join("\n", running.Select(p => $"   {p.ProcessName}.exe   (PID {p.Id})"));
            if (DarkBox.Show(this, $"{what} needs the game, the local server and the Steam bridge closed. These are still running (the server and the bridge have no window):\n\n{names}\n\nWould you like to close them now?",
                    "Close the processes?", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                return false;
            Runner.CloseOurs(Log);
            if (Runner.OursRunning().Length == 0)
                return true;
            DarkBox.Show(this, "Some of them couldn't be closed. Close them from the Task Manager (Ctrl+Shift+Esc), then try again.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return false;
        }

        // Patch the game / Unpatch it back to Steam's files
        async void PatchClicked()
        {
            if (busy)
                return;
            if (!CloseRunning("Patching"))
                return;
            var st = RefreshGameState();
            if (st == Patcher.GameState.Original || st == Patcher.GameState.Outdated)
            {
                await Run("Patching", () => Patcher.Apply(Game(), Log));
                return;
            }
            if (DarkBox.Show(this, "Unpatch the game: put back Steam's original files and take the mod's files out of Bin?\n\n" +
                    "Your mod settings are kept for the next time you patch (or press Play). The hosts block stays: remove it from More if you want to play online.",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            bool complete = true;
            if (await Run("Unpatch", () => complete = Patcher.Restore(Game(), Log)) && !complete &&
                DarkBox.Show(this, "Some patched files had no backup. Let Steam verify the game's files to download them again?",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                OpenSteam(GameLocator.VerifyCommand);
        }
    }

    static class Program
    {
        // Opened from the browser's download bar, the launcher runs from a new temporary copy each time (Chrome/Edge:
        // Downloads\scoped_dir..., else %TEMP%): the mod got installed there and every start asked to install it again.
        // It offers a folder of its own (Documents\CSNZ Offline Revived by default), copies itself there and starts that copy.
        static bool MoveOutOfTemporaryFolder()
        {
            var root = Paths.Root;
            var temp = Path.GetFullPath(Path.GetTempPath()).TrimEnd('\\');
            bool temporary = Path.GetFileName(root).StartsWith("scoped_dir", StringComparison.OrdinalIgnoreCase) ||
                             root.StartsWith(temp, StringComparison.OrdinalIgnoreCase);
            if (!temporary)
                return false;
            var target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "CSNZ Offline Revived");
            var answer = DarkBox.Show(
                "The launcher was opened from a temporary folder (the browser's downloads bar):\n" + root + "\n\n" +
                "Everything it installs there is lost, and next time it asks to install again.\n\n" +
                "Yes: move it to " + target + " and open it from there (a desktop shortcut is made too).\n" +
                "No: choose another folder.\nCancel: keep running from here.",
                "Counter-Strike Nexon: Offline Revived", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
            if (answer == DialogResult.Cancel)
                return false;
            if (answer == DialogResult.No)
            {
                using (var d = new FolderBrowserDialog { Description = "An empty folder for the launcher (the mod is installed next to it)", ShowNewFolderButton = true })
                {
                    if (d.ShowDialog() != DialogResult.OK)
                        return false;
                    target = d.SelectedPath;
                }
            }
            try
            {
                Directory.CreateDirectory(target);
                var exe = Path.Combine(target, "CSNZ-Offline-Revived-Launcher.exe");
                File.Copy(Application.ExecutablePath, exe, true);
                foreach (var keep in new[] { "launcher.json" })
                    if (File.Exists(Path.Combine(root, keep)) && !File.Exists(Path.Combine(target, keep)))
                        File.Copy(Path.Combine(root, keep), Path.Combine(target, keep));
                try
                {
                    var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                    var shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
                    dynamic sc = shell.GetType().InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell,
                        new object[] { Path.Combine(desktop, "Counter-Strike Nexon Offline Revived Launcher.lnk") });
                    sc.TargetPath = exe;
                    sc.WorkingDirectory = target;
                    sc.IconLocation = exe + ",0";
                    sc.Save();
                }
                catch { }
                Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = target });
                return true;
            }
            catch (Exception ex)
            {
                DarkBox.Show("Couldn't move the launcher: " + ex.Message, "Counter-Strike Nexon: Offline Revived");
                return false;
            }
        }

        [STAThread]
        static void Main(string[] args)
        {
            // the hosts block, run by the launcher itself with administrator rights (Hosts.Run starts it)
            if (args.Length == 2 && args[0] == "--hosts")
                Environment.Exit(Hosts.Edit(args[1]));
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
                if (args.Contains("--python"))
                    try
                    {
                        lines.Add("python found: " + Assets.HasPython());
                        var (v, url) = Assets.LatestPython();
                        lines.Add($"latest python installer: {v} {url}");
                    }
                    catch (Exception ex) { lines.Add("python: " + ex.Message); }
                if (args.Contains("--archive"))
                    try
                    {
                        // archive.org's answer about the game's zip, and whether its server resumes downloads (HEAD: nothing is downloaded)
                        var z = ArchiveGame.Info().GetAwaiter().GetResult();
                        lines.Add($"archive.org zip: {z.Name}, {z.Size} bytes, md5 {z.Md5}");
                        lines.Add("archive.org head: " + ArchiveGame.Head(z).GetAwaiter().GetResult());
                    }
                    catch (Exception ex) { lines.Add("archive.org: " + ex.Message); }
                int ht = Array.IndexOf(args, "--hosts-test");
                if (ht > 0 && ht + 1 < args.Length)
                {
                    // the hosts editing on a copy: add, add again (one block), remove
                    var f = args[ht + 1];
                    lines.Add("hosts add: " + Hosts.Edit("add", f) + ", blocks " + (File.ReadAllText(f).Split(new[] { "CSNZ offline mod (begin)" }, StringSplitOptions.None).Length - 1));
                    lines.Add("hosts add again: " + Hosts.Edit("add", f) + ", blocks " + (File.ReadAllText(f).Split(new[] { "CSNZ offline mod (begin)" }, StringSplitOptions.None).Length - 1));
                    File.Copy(f, f + ".added", true);
                    lines.Add("hosts remove: " + Hosts.Edit("remove", f) + ", blocks " + (File.ReadAllText(f).Split(new[] { "CSNZ offline mod (begin)" }, StringSplitOptions.None).Length - 1));
                }
                int sa = Array.IndexOf(args, "--steam-api");
                if (sa > 0)
                {
                    // --steam-api <dll>...: is each one Valve's; --bridge-dll <game>: which one the bridge would get
                    for (int i = sa + 1; i < args.Length && !args[i].StartsWith("--"); i++)
                        lines.Add($"steam api {args[i]}: Valve's {SteamApi.IsValves(args[i])}");
                }
                int ue = Array.IndexOf(args, "--use-emulator");
                if (ue > 0 && ue + 1 < args.Length)
                {
                    // --use-emulator <game>: what Play does to a game that isn't a Steam library install
                    SteamApi.UseEmulatorIfNotSteam(args[ue + 1], l => lines.Add(l));
                    lines.Add("game's dll now: Valve's " + SteamApi.IsValves(SteamApi.InGame(args[ue + 1])) + ", kept copy Valve's " + SteamApi.IsValves(SteamApi.Kept));
                }
                int bd = Array.IndexOf(args, "--bridge-dll");
                if (bd > 0 && bd + 1 < args.Length)
                    lines.Add("bridge dll: " + SteamApi.ForBridge(args[bd + 1], l => lines.Add(l)));
                if (args.Contains("--logs"))
                    try { lines.Add($"logs: {LogBundle.Build(Settings.Load()).Length} characters in {LogBundle.FilePath}"); }
                    catch (Exception ex) { lines.Add("logs: " + ex); }
                File.WriteAllLines(Path.Combine(Paths.Root, "launcher-check.txt"), lines);
                return;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            int pd = Array.IndexOf(args, "--preview-dialog");
            if (pd >= 0)
            {
                // the close-the-processes question as a player sees it; with a .png after it, drawn into that file
                var text = "Updating needs the game, the local server and the Steam bridge closed. These are still running (the server and the bridge have no window):\n\n" +
                    "   CSNZ_Server.exe   (PID 37796)\n\nWould you like to close them now?";
                if (pd + 1 < args.Length)
                    DarkBox.Render(args[pd + 1], text, "Close the processes?", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                else
                    DarkBox.Show(text, "Close the processes?", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                return;
            }
            int pa = Array.IndexOf(args, "--preview-antivirus");
            if (pa >= 0)
            {
                // the antivirus window; "removed" after it: as when files were quarantined
                var missing = args.Contains("removed") ? new[] { "botassist.asi", "nartool.exe" } : null;
                var png = args.FirstOrDefault(a => a.EndsWith(".png", StringComparison.OrdinalIgnoreCase));
                using (var f = new AntivirusForm(missing, Settings.Load().GamePath, missing != null) { StartPosition = FormStartPosition.CenterScreen, ShowInTaskbar = true })
                {
                    if (png == null)
                    {
                        f.ShowDialog();
                        return;
                    }
                    // with a .png: drawn into that file, off screen
                    f.StartPosition = FormStartPosition.Manual;
                    f.Location = new Point(-5000, -5000);
                    f.Show();
                    Application.DoEvents();
                    using (var bmp = new Bitmap(f.Width, f.Height))
                    {
                        f.DrawToBitmap(bmp, new Rectangle(Point.Empty, f.Size));
                        bmp.Save(png);
                    }
                }
                return;
            }
            int pp = Array.IndexOf(args, "--preview-pictures");
            if (pp >= 0)
            {
                // the item pictures question (without Python, before Play); with a .png after it, drawn into that file
                if (pp + 1 < args.Length)
                    DarkBox.Render(args[pp + 1], MainForm.PicturesQuestion(false), "Item pictures", MessageBoxButtons.YesNo, MessageBoxIcon.Question, MainForm.PicturesButtons(true));
                else
                    DarkBox.Show(MainForm.PicturesQuestion(false), "Item pictures", MessageBoxButtons.YesNo, MessageBoxIcon.Question, MainForm.PicturesButtons(true));
                return;
            }
            if (!args.Contains("--play") && MoveOutOfTemporaryFolder())
                return;
            if (args.Contains("--preview-get-game"))
            {
                Application.Run(new GetGameForm(true));
                return;
            }
            Application.Run(new MainForm(args.Contains("--play")));
        }
    }
}
