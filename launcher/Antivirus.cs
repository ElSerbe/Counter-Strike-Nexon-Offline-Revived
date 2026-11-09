// Antivirus warnings: what the mod's programs do, how to check them and how to let them through, shown once on the
// first start (More shows it again), and when an antivirus has taken one of the mod's programs away.
// The launcher never changes the antivirus' settings itself (adding exclusions is what malware does): it opens Windows
// Security and gives the folders to add.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Windows.Forms;

namespace CsnzLauncher
{
    public static class Antivirus
    {
        // the mod's own programs (under mod\), the ones an antivirus may quarantine
        static readonly string[] Programs =
        {
            @"tools\botassist\botassist.asi",
            @"server\CSNZ_Server\bin\CSNZ_Server.exe",
            @"server\CSNZ_Server\bin\libgcc_s_seh-1.dll",
            @"server\CSNZ_Server\bin\libstdc++-6.dll",
            @"server\CSNZ_Server\bin\libwinpthread-1.dll",
            @"tools\steambridge\out\CSNZ-SteamBridge.exe",
            @"tools\nartool\nartool.exe",
            @"tools\goldberg\steam_api.dll",
        };

        // installed programs that aren't there any more (most likely quarantined)
        public static string[] Missing()
        {
            if (Paths.InstalledVersion() == null || Paths.IsDevCheckout)
                return new string[0];
            return Programs.Where(p => !File.Exists(Path.Combine(Paths.Mod, p))).Select(p => Path.GetFileName(p)).ToArray();
        }

        public static string LauncherSha256()
        {
            try
            {
                using (var sha = SHA256.Create())
                using (var f = File.OpenRead(Application.ExecutablePath))
                    return string.Concat(sha.ComputeHash(f).Select(b => b.ToString("x2")));
            }
            catch { return null; }
        }
    }

    public class AntivirusForm : Form
    {
        const int W = 600, Pad = 18;

        // missing: the programs an antivirus took away (null: the first start's explanation); offerDownload: the mod can
        // be downloaded again (the latest release isn't older than the installed one)
        public AntivirusForm(string[] missing, string gamePath, bool offerDownload)
        {
            bool removed = missing != null && missing.Length > 0;
            Text = removed ? "Your antivirus removed part of the mod" : "About antivirus warnings";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = MaximizeBox = false;
            ShowInTaskbar = false;
            Font = Theme.UiFont(13);
            BackColor = Theme.Back;
            ForeColor = Theme.Text;

            int y = 10;
            Label Add(string text, Font font = null, Color? color = null, int gap = 8)
            {
                var l = new Label { Text = text, AutoSize = false, Font = font ?? Font, ForeColor = color ?? Theme.Text, UseMnemonic = false };
                var h = TextRenderer.MeasureText(text, l.Font, new Size(W - 2 * Pad, int.MaxValue), TextFormatFlags.WordBreak).Height + 2;
                l.SetBounds(Pad, y, W - 2 * Pad, h);
                Controls.Add(l);
                y += h + gap;
                return l;
            }

            Add(removed ? "YOUR ANTIVIRUS REMOVED PART OF THE MOD" : "IS THIS SAFE? ABOUT ANTIVIRUS WARNINGS", Theme.Display(22), Theme.Accent, 6);
            if (removed)
            {
                Add("These files of the mod aren't there any more (an antivirus usually moves them to its quarantine):\n   " +
                    string.Join("\n   ", missing), color: Theme.Warn);
                Add("Restore them from the antivirus' quarantine (or download the mod again below), then add the folders below as " +
                    "exceptions so it doesn't happen again.");
            }
            Add("Some antivirus programs, Microsoft Defender among them, may warn about this launcher or the mod's programs. " +
                "It's a false alarm: the programs are new, they aren't signed with a paid certificate and few people have " +
                "downloaded them yet, so the antivirus guesses (Defender's \"Wacatac.B!ml\" means a machine-learning guess, " +
                "not a known virus). Everything is open source and built from the code on GitHub.");
            Add("What each program does:\n" +
                "  •  the launcher: downloads the mod from GitHub, patches your game (keeping a backup of every file) and adds the " +
                "hosts block, the only step that asks for administrator rights\n" +
                "  •  botassist.asi: the bots' AI, loaded by the game\n" +
                "  •  CSNZ_Server.exe: the local server;  CSNZ-SteamBridge.exe: Online play through Steam\n" +
                "  •  nartool.exe: reads the item pictures out of your game's own files", color: Theme.Muted);
            Add("If your antivirus blocks or removes one of them, add these two folders as exceptions (in Windows Security: " +
                "Virus & threat protection settings > Exclusions > Add an exclusion > Folder). There's no need to turn the antivirus off.");

            void FolderRow(string label, string path)
            {
                var l = new Label { Text = label, AutoSize = false, ForeColor = Theme.Muted };
                l.SetBounds(Pad, y + 5, 118, 22);
                var box = new TextBox { ReadOnly = true, TabStop = false, Text = path ?? "(not found yet)" };
                Theme.Dark(box);
                box.SetBounds(Pad + 120, y + 2, W - 2 * Pad - 120 - 78, 26);
                var copy = new FlatButton { Text = "Copy", Enabled = path != null };
                copy.SetBounds(W - Pad - 70, y, 70, 30);
                copy.Click += (s, e) => { try { Clipboard.SetText(path); copy.Text = "Copied"; } catch { } };
                Controls.AddRange(new Control[] { l, box, copy });
                y += 38;
            }
            FolderRow("The mod's folder", Paths.Root);
            FolderRow("The game's Bin", GameLocator.IsGame(gamePath) ? Path.Combine(gamePath, "Bin") : null);
            y += 6;

            var sha = Antivirus.LauncherSha256();
            var security = new FlatButton { Text = "Open Windows Security" };
            security.SetBounds(Pad, y, 180, 34);
            security.Click += (s, e) => Open("windowsdefender://threatsettings");
            var vt = new FlatButton { Text = "This launcher on VirusTotal", Enabled = sha != null };
            vt.SetBounds(security.Right + 10, y, 200, 34);
            vt.Click += (s, e) => Open("https://www.virustotal.com/gui/file/" + sha);
            var code = new FlatButton { Text = "Source code" };
            code.SetBounds(vt.Right + 10, y, W - Pad - vt.Right - 10, 34);
            code.Click += (s, e) => Open(GitHub.PageUrl);
            Controls.AddRange(new Control[] { security, vt, code });
            y += 42;
            if (sha != null)
                Add("This launcher's SHA-256: " + sha + " (VirusTotal says \"not found\" until someone has scanned this version).",
                    Theme.UiFont(11), Theme.Muted);

            y += 4;
            var ok = new FlatButton { Text = removed ? "Close" : "Got it", Primary = !removed || !offerDownload, DialogResult = DialogResult.OK };
            ok.SetBounds(W - Pad - 110, y, 110, 36);
            Controls.Add(ok);
            AcceptButton = CancelButton = ok;
            if (removed && offerDownload)
            {
                var again = new FlatButton { Text = "Download the mod again", Primary = true, DialogResult = DialogResult.Retry };
                again.SetBounds(ok.Left - 10 - 210, y, 210, 36);
                Controls.Add(again);
                AcceptButton = again;
            }
            ClientSize = new Size(W, y + 36 + Pad);
            ActiveControl = (Control)AcceptButton; // not the folder boxes (their text would show selected)
        }

        void Open(string target)
        {
            try { Process.Start(target); }
            catch (Exception ex) { DarkBox.Show(this, "Couldn't open " + target + ": " + ex.Message, Text); }
        }
    }
}
