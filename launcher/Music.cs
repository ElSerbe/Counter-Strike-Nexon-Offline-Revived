// The launcher's background music: the game's own lobby theme (Bin\CSOnline.mp3 in the player's Steam copy, so no
// music ships with the launcher), looped at 10% volume through Windows' MCI. FadeOut lowers it to silence and stops.
// MCI wants every command on the thread that opened the file: all of this runs on the window's thread.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace CsnzLauncher
{
    class Music : IDisposable
    {
        const string Alias = "csnzbgm";
        const int Volume = 100; // MCI's scale is 0-1000: 10%
        [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
        static extern int mciSendString(string command, StringBuilder ret, int retLen, IntPtr callback);
        public static string LastError = "";
        static int Mci(string cmd)
        {
            int e = mciSendString(cmd, null, 0, IntPtr.Zero);
            if (e != 0)
                LastError = $"MCI error {e} on \"{cmd.Split(' ')[0]}\"";
            return e;
        }

        readonly Timer fade = new Timer { Interval = 40 };
        bool open;
        int level;
        Action faded;

        public bool Playing { get; private set; }
        public event EventHandler Changed;

        public Music()
        {
            fade.Tick += (s, e) =>
            {
                level -= Volume / 40; // about 1.6 s down to silence
                if (level > 0)
                {
                    Mci($"setaudio {Alias} volume to {level}");
                    return;
                }
                fade.Stop();
                Stop();
                var then = faded;
                faded = null;
                then?.Invoke();
            };
        }

        public static string FileIn(string game) =>
            string.IsNullOrEmpty(game) ? null : Path.Combine(game, "Bin", "CSOnline.mp3");

        // starts the theme from the game's folder; false when the file isn't there or Windows can't play it
        public bool Play(string game)
        {
            if (Playing)
                return true;
            var file = FileIn(game);
            if (file == null || !File.Exists(file))
                return false;
            fade.Stop();
            faded = null;
            Close();
            if (Mci($"open \"{file}\" type mpegvideo alias {Alias}") != 0)
                return false;
            open = true;
            level = Volume;
            Mci($"setaudio {Alias} volume to {level}");
            if (Mci($"play {Alias} repeat") != 0)
            {
                Close();
                return false;
            }
            Playing = true;
            Changed?.Invoke(this, EventArgs.Empty);
            return true;
        }

        // lowers the volume to nothing, stops, then runs done (right away when nothing plays)
        public void FadeOut(Action done = null)
        {
            if (!Playing)
            {
                done?.Invoke();
                return;
            }
            faded = (Action)Delegate.Combine(faded, done);
            fade.Start();
        }

        public void Stop()
        {
            fade.Stop();
            Close();
            if (Playing)
            {
                Playing = false;
                Changed?.Invoke(this, EventArgs.Empty);
            }
        }

        void Close()
        {
            if (!open)
                return;
            Mci($"stop {Alias}");
            Mci($"close {Alias}");
            open = false;
        }

        public void Dispose()
        {
            Stop();
            fade.Dispose();
        }
    }
}
