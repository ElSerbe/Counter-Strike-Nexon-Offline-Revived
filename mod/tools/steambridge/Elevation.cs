// Whether a process runs as administrator (elevated). Steam run as administrator only lets elevated programs in
// (SteamAPI_Init fails in the others), so the bridge says so when it can't get in. The launcher has the same class.
using System;
using System.Runtime.InteropServices;

namespace SteamBridge
{
    static class Elevation
    {
        [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
        [DllImport("kernel32")] static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32")] static extern bool CloseHandle(IntPtr h);
        [DllImport("advapi32", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, int access, out IntPtr token);
        [DllImport("advapi32", SetLastError = true)]
        static extern bool GetTokenInformation(IntPtr token, int cls, out int info, int size, out int returned);

        const int QueryLimitedInformation = 0x1000, TokenQuery = 8, TokenElevation = 20;

        static bool? OfHandle(IntPtr process)
        {
            if (!OpenProcessToken(process, TokenQuery, out var token))
                return null;
            try { return GetTokenInformation(token, TokenElevation, out int e, 4, out _) ? e != 0 : (bool?)null; }
            finally { CloseHandle(token); }
        }

        // true/false; null when its token can't be read (it runs as administrator, or as another Windows user)
        public static bool? Of(int pid)
        {
            var h = OpenProcess(QueryLimitedInformation, false, pid);
            if (h == IntPtr.Zero)
                return null;
            try { return OfHandle(h); }
            finally { CloseHandle(h); }
        }

        public static bool Self() => OfHandle(GetCurrentProcess()) == true;
    }
}
