// Shown when the game isn't on this PC: how to get it from Steam (free), and a watch that finds it once Steam has it.
using System;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace CsnzLauncher
{
    public class GetGameForm : Form
    {
        public string FoundPath { get; private set; }

        readonly Label status = new Label { AutoSize = false };
        readonly Timer watch = new Timer { Interval = 3000 };

        // preview: the window as a player without the game sees it (no watch, which would find the game here)
        public GetGameForm(bool preview = false)
        {
            Text = "Get Counter-Strike Nexon";
            ClientSize = new Size(560, 330);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = MaximizeBox = false;
            Font = new Font("Segoe UI", 9);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            bool steam = GameLocator.SteamDir() != null;
            var head = new Label { Text = "Counter-Strike Nexon isn't installed on this PC", Font = new Font("Segoe UI", 12, FontStyle.Bold), AutoSize = true, Location = new Point(14, 12) };
            var info = new Label
            {
                Location = new Point(16, 44),
                Size = new Size(528, 52),
                Text = steam
                    ? "The mod plays on your own copy of the game. It's free on Steam: install its latest version, and the launcher finds it by itself as soon as Steam has finished."
                    : "The mod plays on your own copy of the game, which is free on Steam. Steam isn't installed on this PC: get Steam first, then install the game.",
            };
            var cmdLabel = new Label { Text = "Or press Win+R and run one of these commands:", AutoSize = true, Location = new Point(16, 152) };
            var cmds = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                Font = new Font("Consolas", 10),
                Location = new Point(16, 172),
                Size = new Size(528, 48),
                Text = $"{GameLocator.InstallCommand}      (install the game){Environment.NewLine}{GameLocator.RunCommand}          (start it with Steam)",
            };

            var getSteam = new Button { Text = "Get Steam", Location = new Point(16, 104), Size = new Size(120, 32), Visible = !steam };
            var install = new Button { Text = "Install with Steam", Location = new Point(steam ? 16 : 146, 104), Size = new Size(150, 32), Enabled = steam };
            var store = new Button { Text = "Steam store page", Location = new Point(install.Right + 10, 104), Size = new Size(130, 32) };
            var copy = new Button { Text = "Copy", Location = new Point(store.Right + 10, 104), Size = new Size(70, 32) };

            status.SetBounds(16, 232, 528, 40);
            var browse = new Button { Text = "It's somewhere else...", Location = new Point(16, 284), Size = new Size(150, 32) };
            var close = new Button { Text = "Close", DialogResult = DialogResult.Cancel, Location = new Point(464, 284), Size = new Size(80, 32) };
            CancelButton = close;
            Controls.AddRange(new Control[] { head, info, getSteam, install, store, copy, cmdLabel, cmds, status, browse, close });

            getSteam.Click += (s, e) => Open(GameLocator.SteamDownload);
            install.Click += (s, e) => Open(GameLocator.InstallCommand);
            store.Click += (s, e) => Open(GameLocator.StorePage);
            copy.Click += (s, e) => { Clipboard.SetText(GameLocator.InstallCommand); status.Text = GameLocator.InstallCommand + " copied: paste it in Win+R (or a browser's address bar)."; };
            browse.Click += (s, e) =>
            {
                using (var dlg = new FolderBrowserDialog { Description = "The game's folder (it has Bin\\cstrike-online.exe)" })
                {
                    if (dlg.ShowDialog(this) != DialogResult.OK)
                        return;
                    var dir = dlg.SelectedPath;
                    if (!GameLocator.IsGame(dir) && GameLocator.IsGame(System.IO.Path.GetDirectoryName(dir)))
                        dir = System.IO.Path.GetDirectoryName(dir);
                    if (!GameLocator.IsGame(dir))
                    {
                        MessageBox.Show(this, "That folder has no Bin\\cstrike-online.exe.", Text);
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
            FormClosed += (s, e) => watch.Dispose();
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
            catch (Exception ex) { MessageBox.Show(this, "Couldn't open " + target + ": " + ex.Message, Text); }
        }
    }
}
