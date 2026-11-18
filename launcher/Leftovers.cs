using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace CsnzLauncher
{
    // ------------------------------------------------------------ the game's helpers left running after it
    // The game starts helpers from its Bin folder: CefSubProcess.exe (its built-in browser), CrashReportClient.exe
    // (after a crash), wow_helper.exe, FFmpeg.exe. One can outlive cstrike-online.exe, most of all when it crashes, and
    // Steam then shows the game as still running. The game runs with administrator rights and so do its helpers: the
    // launcher can see where they run from (QueryFullProcessImageName with limited query rights) but can't close them,
    // so it starts itself again with administrator rights ("--close-leftovers <game>"; Windows asks the player first)
    // and that copy closes them. Only processes whose exe is in the game's Bin folder, by exact PID.
    public static class Leftovers
    {
        const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder name, ref int size);

        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        static string ImagePath(int pid)
        {
            IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (h == IntPtr.Zero)
                return null;
            try
            {
                var sb = new StringBuilder(1024);
                int size = sb.Capacity;
                return QueryFullProcessImageName(h, 0, sb, ref size) ? sb.ToString(0, size) : null;
            }
            finally
            {
                CloseHandle(h);
            }
        }

        // the processes running an exe of the game's Bin folder
        public static List<Process> Find(string game)
        {
            var found = new List<Process>();
            if (string.IsNullOrEmpty(game))
                return found;
            string bin = Path.GetFullPath(Path.Combine(game, "Bin")).TrimEnd('\\') + "\\";
            foreach (var p in Process.GetProcesses())
            {
                string path = ImagePath(p.Id);
                if (path != null && path.StartsWith(bin, StringComparison.OrdinalIgnoreCase))
                    found.Add(p);
                else
                    p.Dispose();
            }
            return found;
        }

        static string Describe(IEnumerable<Process> list) =>
            string.Join(", ", list.Select(p => $"{p.ProcessName}.exe (PID {p.Id})"));

        // closes them; returns how many are still running
        public static int Close(string game, Logger log)
        {
            int left = 0;
            foreach (var p in Find(game))
            {
                using (p)
                {
                    try
                    {
                        p.Kill();
                        p.WaitForExit(5000);
                        log?.Invoke($"Closed the game's {p.ProcessName}.exe (PID {p.Id}), left running after the game.");
                    }
                    catch (Exception ex) when (ex is Win32Exception || ex is InvalidOperationException)
                    {
                        if (!p.HasExited)
                            left++;
                    }
                }
            }
            return left;
        }

        // after the game: its helpers get a few seconds to end by themselves, then they're closed
        public static void CloseAfterGame(string game, Logger log)
        {
            List<Process> left = null;
            for (int i = 0; i < 6; i++)
            {
                left?.ForEach(p => p.Dispose());
                left = Find(game);
                if (left.Count == 0)
                    return;
                Thread.Sleep(1000);
            }
            log($"Still running after the game: {Describe(left)}. Steam shows the game as running while they are: closing them.");
            left.ForEach(p => p.Dispose());
            if (Close(game, log) == 0)
                return;
            // they run with administrator rights, like the game
            var psi = new ProcessStartInfo(System.Windows.Forms.Application.ExecutablePath, $"--close-leftovers \"{game}\"")
            {
                UseShellExecute = true,
                Verb = "runas",
                WorkingDirectory = Paths.Root,
            };
            try
            {
                using (var p = Process.Start(psi))
                {
                    p.WaitForExit(30000);
                }
            }
            catch (Win32Exception)
            {
                log("Windows' permission prompt was cancelled: close them from the Task Manager (Ctrl+Shift+Esc).");
                return;
            }
            var still = Find(game);
            log(still.Count == 0 ? "The game's left-over processes are closed."
                                 : $"Couldn't close {Describe(still)}: close them from the Task Manager (Ctrl+Shift+Esc).");
            still.ForEach(p => p.Dispose());
        }
    }
}
