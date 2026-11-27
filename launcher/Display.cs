// The player's field of view and frame rate (More > Field of view / Uncapped frame rate): written to Bin\botassist.ini
// [Display] before the game starts; botassist puts them in the game's console variables (default_fov, fps_max) once
// the game is up. 0 = leave the game's own value.
// Also Windows display scaling (DpiAware below).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;

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

        // Windows display scaling over 100% (#40): cstrike-online.exe doesn't say it handles DPI, so Windows tells it the
        // screen is smaller (1536x864 at 125% on a 1920x1080 screen) and stretches its picture by the scale. At the
        // desktop's own resolution the stretched picture is bigger than the screen: lobby and match zoomed in, edges cut
        // off. The exe's "Override high DPI scaling behavior: Application" (Properties > Compatibility) leaves the game
        // its real pixels; it's the HIGHDPIAWARE flag in the player's compatibility layers, set here before every start.
        // A DPI choice the player made there (System: DPIUNAWARE, System (Enhanced): GDIDPISCALING) is kept, and so are
        // their other flags (RUNASADMIN...). The game runs elevated as the same Windows user, so it reads these settings.
        const string Layers = @"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers";
        const string DpiAwareFlag = "HIGHDPIAWARE";
        static readonly string[] DpiFlags = { DpiAwareFlag, "DPIUNAWARE", "GDIDPISCALING" };

        static string GameExe(string game) => Path.GetFullPath(Path.Combine(game, "Bin", "cstrike-online.exe"));

        // the value's flags without the "~" Windows puts first
        static List<string> Flags(string value) =>
            (value ?? "").Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries).Where(f => f != "~").ToList();

        static void Store(RegistryKey key, string exe, List<string> flags)
        {
            if (flags.Count == 0)
                key.DeleteValue(exe, false);
            else
                key.SetValue(exe, "~ " + string.Join(" ", flags));
        }

        public static void DpiAware(string game, Logger log)
        {
            var exe = GameExe(game);
            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(Layers))
                {
                    var flags = Flags(key.GetValue(exe) as string);
                    if (flags.Any(f => DpiFlags.Contains(f, StringComparer.OrdinalIgnoreCase)))
                        return; // set already (by an earlier start, or the player's own choice)
                    flags.Add(DpiAwareFlag);
                    Store(key, exe, flags);
                    log("Windows display scaling: the game draws at the screen's own size (high DPI override set to Application).");
                }
            }
            catch (Exception ex)
            {
                log("Windows display scaling: the high DPI override couldn't be set (" + ex.Message + "); with scaling over 100% the game may look zoomed in.");
            }
        }

        // Unpatch: the flag DpiAware added goes, the player's other flags stay
        public static void DpiAwareOff(string game, Logger log)
        {
            var exe = GameExe(game);
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(Layers, true))
                {
                    if (key == null)
                        return;
                    var flags = Flags(key.GetValue(exe) as string);
                    if (flags.RemoveAll(f => f.Equals(DpiAwareFlag, StringComparison.OrdinalIgnoreCase)) == 0)
                        return;
                    Store(key, exe, flags);
                    log("  the game's high DPI override removed");
                }
            }
            catch { }
        }
    }
}
