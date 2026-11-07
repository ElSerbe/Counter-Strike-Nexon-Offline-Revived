// Shown when the game isn't on this PC: how to get it from Steam (free), with a watch that finds it once Steam has it,
// and, when Steam doesn't offer it any more, a download of the same version from archive.org (ArchiveGame).
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows.Forms;

namespace CsnzLauncher
{
    public class GetGameForm : Form
    {
        public string FoundPath { get; private set; }

        readonly Label status = new Label { AutoSize = false, ForeColor = Theme.Muted };
        readonly System.Windows.Forms.Timer watch = new System.Windows.Forms.Timer { Interval = 3000 };
        readonly TextBox folderBox = new TextBox();
        readonly FlatButton download = new FlatButton { Text = "Download from archive.org", Primary = true };
        readonly FlatButton folderButton = new FlatButton { Text = "Browse" };
        readonly Bar bar = new Bar();
        readonly Label progressText = new Label { AutoSize = false, ForeColor = Theme.Muted };
        readonly Logger log;
        CancellationTokenSource cancel;
        ArchiveGame.ZipInfo zipInfo;

        // preview: the window as a player without the game sees it (no watch, which would find the game here)
        public GetGameForm(bool preview = false, Logger log = null)
        {
            this.log = log ?? (l => { });
            Text = "Get Counter-Strike Nexon";
            ClientSize = new Size(560, 556);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = MaximizeBox = false;
            Font = Theme.UiFont(13);
            BackColor = Theme.Back;
            ForeColor = Theme.Text;

            bool steam = GameLocator.SteamDir() != null;
            var head = new Label { Text = "COUNTER-STRIKE NEXON ISN'T INSTALLED ON THIS PC", Font = Theme.Display(22), ForeColor = Theme.Accent, AutoSize = true, Location = new Point(14, 10) };
            var info = new Label
            {
                Location = new Point(16, 44),
                Size = new Size(528, 52),
                Text = steam
                    ? "The mod plays on your own copy of the game. It's free on Steam: install its latest version, and the launcher finds it by itself as soon as Steam has finished."
                    : "The mod plays on your own copy of the game, which is free on Steam. Steam isn't installed on this PC: get Steam first, then install the game.",
            };
            var cmdLabel = new Label { Text = "Or press Win+R and run one of these commands:", AutoSize = true, ForeColor = Theme.Muted, Location = new Point(16, 146) };
            var cmds = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                Font = new Font("Consolas", 10),
                BackColor = Theme.Field,
                ForeColor = Theme.Text,
                BorderStyle = BorderStyle.FixedSingle,
                Location = new Point(16, 166),
                Size = new Size(528, 48),
                Text = $"{GameLocator.InstallCommand}      (install the game){Environment.NewLine}{GameLocator.RunCommand}          (start it with Steam)",
            };

            var getSteam = new FlatButton { Text = "Get Steam", Location = new Point(16, 104), Size = new Size(120, 32), Visible = !steam };
            var install = new FlatButton { Text = "Install with Steam", Location = new Point(steam ? 16 : 146, 104), Size = new Size(150, 32), Enabled = steam };
            var store = new FlatButton { Text = "Steam store page", Location = new Point(install.Right + 10, 104), Size = new Size(130, 32) };
            var copy = new FlatButton { Text = "Copy", Location = new Point(store.Right + 10, 104), Size = new Size(70, 32) };
            status.SetBounds(16, 222, 528, 36);

            // archive.org
            var line = new Label { BackColor = Theme.PanelLine, Location = new Point(16, 264), Size = new Size(528, 1) };
            var archiveHead = new Label { Text = "STEAM DOESN'T GIVE YOU THE GAME? GET IT FROM ARCHIVE.ORG", Font = Theme.Display(18), ForeColor = Theme.Accent, AutoSize = true, Location = new Point(14, 274) };
            var archiveInfo = new Label
            {
                Location = new Point(16, 302),
                Size = new Size(528, 52),
                Text = "The Internet Archive keeps a copy of the same Steam version the mod is made for. The download is 12.9 GB " +
                       "(it can be stopped and goes on later from where it was) and installing needs about 27 GB free on the drive.",
            };
            var folderLabel = new Label { Text = "Install the game in this folder:", AutoSize = true, ForeColor = Theme.Muted, Location = new Point(16, 358) };
            folderBox.SetBounds(16, 378, 430, 26);
            Theme.Dark(folderBox);
            folderBox.Text = Path.Combine(Paths.Root, "Counter-Strike Nexon");
            folderButton.SetBounds(456, 376, 88, 30);
            download.SetBounds(16, 414, 230, 36);
            download.Font = Theme.Display(17);
            var page = new FlatButton { Text = "archive.org page", Location = new Point(256, 414), Size = new Size(140, 36) };
            var torrent = new FlatButton { Text = "Torrent", Location = new Point(406, 414), Size = new Size(138, 36) };
            bar.SetBounds(16, 460, 528, 10);
            progressText.SetBounds(16, 474, 528, 34);
            progressText.Text = DownloadedSoFar();

            var browse = new FlatButton { Text = "It's somewhere else...", Location = new Point(16, 512), Size = new Size(150, 32) };
            var close = new FlatButton { Text = "Close", DialogResult = DialogResult.Cancel, Location = new Point(464, 512), Size = new Size(80, 32) };
            CancelButton = close;
            Controls.AddRange(new Control[] { head, info, getSteam, install, store, copy, cmdLabel, cmds, status, line, archiveHead, archiveInfo,
                                              folderLabel, folderBox, folderButton, download, page, torrent, bar, progressText, browse, close });

            getSteam.Click += (s, e) => Open(GameLocator.SteamDownload);
            install.Click += (s, e) => Open(GameLocator.InstallCommand);
            store.Click += (s, e) => Open(GameLocator.StorePage);
            copy.Click += (s, e) => { Clipboard.SetText(GameLocator.InstallCommand); status.Text = GameLocator.InstallCommand + " copied: paste it in Win+R (or a browser's address bar)."; };
            page.Click += (s, e) => Open(ArchiveGame.PageUrl);
            torrent.Click += (s, e) => Open(ArchiveGame.TorrentUrl);
            folderButton.Click += (s, e) =>
            {
                using (var dlg = new FolderBrowserDialog { Description = "A folder for the game (an empty one, or a new one inside it is made)", ShowNewFolderButton = true })
                {
                    if (dlg.ShowDialog(this) != DialogResult.OK)
                        return;
                    var dir = dlg.SelectedPath;
                    // a folder with other things in it gets a folder of its own
                    if (!GameLocator.IsGame(dir) && ArchiveGame.Started(dir) == 0 && Directory.EnumerateFileSystemEntries(dir).Any())
                        dir = Path.Combine(dir, "Counter-Strike Nexon");
                    folderBox.Text = dir;
                    progressText.Text = DownloadedSoFar();
                }
            };
            download.Click += (s, e) => DownloadClicked();
            browse.Click += (s, e) =>
            {
                using (var dlg = new FolderBrowserDialog { Description = "The game's folder (it has Bin\\cstrike-online.exe)" })
                {
                    if (dlg.ShowDialog(this) != DialogResult.OK)
                        return;
                    var dir = dlg.SelectedPath;
                    if (!GameLocator.IsGame(dir) && GameLocator.IsGame(Path.GetDirectoryName(dir)))
                        dir = Path.GetDirectoryName(dir);
                    if (!GameLocator.IsGame(dir))
                    {
                        DarkBox.Show(this, "That folder has no Bin\\cstrike-online.exe.", Text);
                        return;
                    }
                    Done(dir);
                }
            };

            watch.Tick += (s, e) => Look();
            if (preview)
                status.Text = "Waiting for the game: this window closes by itself once Steam has installed it.";
            else
                Shown += (s, e) => { Look(); watch.Start(); };
            FormClosing += (s, e) =>
            {
                if (cancel == null)
                    return;
                if (DarkBox.Show(this, "Stop the download? What's downloaded is kept: next time it goes on from there.", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                {
                    e.Cancel = true;
                    return;
                }
                cancel.Cancel();
            };
            FormClosed += (s, e) => watch.Dispose();
        }

        string DownloadedSoFar()
        {
            try
            {
                long have = ArchiveGame.Started(folderBox.Text);
                return have < 0 ? "The download is complete in this folder: Download unpacks it."
                     : have > 0 ? $"{Gb(have)} of 12.9 GB already downloaded in this folder: Download goes on from there."
                     : "";
            }
            catch { return ""; }
        }

        static string Gb(long b) => (b / 1073741824.0).ToString("0.0") + " GB";

        async void DownloadClicked()
        {
            if (cancel != null)
            {
                cancel.Cancel(); // the button says Stop while downloading
                return;
            }
            string folder;
            try { folder = Path.GetFullPath(folderBox.Text.Trim()); }
            catch
            {
                DarkBox.Show(this, "That isn't a folder path.", Text);
                return;
            }
            if (GameLocator.IsGame(folder))
            {
                Done(folder);
                return;
            }
            watch.Stop();
            download.Enabled = false;
            progressText.Text = "Asking archive.org about the download...";
            zipInfo = zipInfo ?? await ArchiveGame.Info();
            download.Enabled = true;
            long need = ArchiveGame.SpaceNeeded(folder, zipInfo), free = ArchiveGame.FreeSpace(folder);
            if (free >= 0 && free < need)
            {
                DarkBox.Show(this, $"The drive of {folder} has {Gb(free)} free, and installing needs {Gb(need)}. Choose a folder on another drive, or make room.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                progressText.Text = DownloadedSoFar();
                watch.Start();
                return;
            }
            cancel = new CancellationTokenSource();
            download.Text = "Stop";
            folderBox.Enabled = folderButton.Enabled = false;
            log($"Getting the game from archive.org ({ArchiveGame.Item}) into {folder}");
            try
            {
                var dir = await ArchiveGame.Install(folder, zipInfo, (stage, done, total, speed) => Ui(() => ShowProgress(stage, done, total, speed)),
                                                    l => { log(l); Ui(() => progressText.Text = l); }, cancel.Token);
                cancel = null;
                Done(dir);
            }
            catch (OperationCanceledException)
            {
                if (IsDisposed)
                    return;
                log("archive.org download stopped (it goes on from there next time).");
                progressText.Text = "Stopped. " + DownloadedSoFar();
            }
            catch (Exception ex)
            {
                log("archive.org: " + ex.Message);
                if (IsDisposed)
                    return;
                progressText.Text = ex.Message;
                DarkBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            finally
            {
                cancel = null;
                if (!IsDisposed && DialogResult != DialogResult.OK)
                {
                    download.Text = "Download from archive.org";
                    folderBox.Enabled = folderButton.Enabled = true;
                    watch.Start();
                }
            }
        }

        // runs on the window's thread, and not at all once the window is gone
        void Ui(Action a)
        {
            try
            {
                if (!IsDisposed && IsHandleCreated)
                    BeginInvoke(a);
            }
            catch { }
        }

        void ShowProgress(string stage, long done, long total, double speed)
        {
            if (IsDisposed)
                return;
            bar.Value = total > 0 ? (float)done / total : 0;
            var text = $"{stage}: {Gb(done)} of {Gb(total)} ({(total > 0 ? done * 100 / total : 0)}%)";
            if (speed > 0)
            {
                var left = TimeSpan.FromSeconds((total - done) / speed);
                text += $", {speed / 1048576:0.0} MB/s, about " + (left.TotalHours >= 1 ? $"{(int)left.TotalHours} h {left.Minutes} min" : $"{Math.Max(1, (int)left.TotalMinutes)} min") + " left";
            }
            progressText.Text = text;
        }

        void Look()
        {
            var found = GameLocator.Find();
            if (found != null)
            {
                Done(found);
                return;
            }
            var downloading = GameLocator.Downloading();
            status.Text = downloading != null
                ? "Steam is installing it (" + downloading + "). This window closes by itself when it's done."
                : "Waiting for the game: this window closes by itself once Steam has installed it.";
        }

        void Done(string dir)
        {
            watch.Stop();
            FoundPath = dir;
            DialogResult = DialogResult.OK;
            Close();
        }

        void Open(string target)
        {
            try { Process.Start(target); }
            catch (Exception ex) { DarkBox.Show(this, "Couldn't open " + target + ": " + ex.Message, Text); }
        }

        // a thin teal progress bar
        class Bar : Control
        {
            float value;
            public float Value { get => value; set { this.value = Math.Max(0, Math.Min(1, value)); Invalidate(); } }
            public Bar() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); }
            protected override void OnPaint(PaintEventArgs e)
            {
                e.Graphics.Clear(Theme.Field);
                using (var b = new SolidBrush(Theme.Accent))
                    e.Graphics.FillRectangle(b, 0, 0, Width * value, Height);
                using (var p = new Pen(Theme.PanelLine))
                    e.Graphics.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
            }
        }
    }
}
