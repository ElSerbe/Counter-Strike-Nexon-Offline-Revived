// The player's field of view and frame rate (More > Field of view / Uncapped frame rate): written to Bin\botassist.ini
// [Display] before the game starts; botassist puts them in the game's console variables (default_fov, fps_max) once
// the game is up. 0 = leave the game's own value.
using System.IO;
using System.Runtime.InteropServices;

namespace CsnzLauncher
{
    static class Display
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern bool WritePrivateProfileString(string section, string key, string value, string file);

        public static void Write(string game, Settings settings, Logger log)
        {
            var ini = Path.Combine(game, "Bin", "botassist.ini");
            try
            {
                WritePrivateProfileString("Display", "Fov", settings.Fov.ToString(), ini);
                WritePrivateProfileString("Display", "FpsMax", settings.FpsUncapped ? "1000" : "0", ini);
                if (settings.Fov != 0 || settings.FpsUncapped)
                    log("Display: " + (settings.Fov != 0 ? "field of view " + settings.Fov : "the game's field of view") +
                        (settings.FpsUncapped ? ", frame rate uncapped" : ""));
            }
            catch { }
        }
    }
}
