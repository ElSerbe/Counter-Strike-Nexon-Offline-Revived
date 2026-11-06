// The game's steam_api.dll (Steamworks SDK of about 2017: SteamClient017, SteamMatchMaking009, SteamNetworking005,
// SteamUtils008, SteamUser019, SteamFriends015). Its flat C functions take the interface pointer first, pass and return
// CSteamID as a uint64, and are cdecl. Call results are polled with ISteamUtils, no callbacks needed.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace SteamBridge
{
    static class Native
    {
        const string Dll = "steam_api";
        const CallingConvention C = CallingConvention.Cdecl;

        [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern IntPtr LoadLibraryW(string path);

        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool SteamAPI_Init();
        [DllImport(Dll, CallingConvention = C)] public static extern void SteamAPI_Shutdown();
        [DllImport(Dll, CallingConvention = C)] public static extern void SteamAPI_RunCallbacks();
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamClient();
        [DllImport(Dll, CallingConvention = C)] public static extern int SteamAPI_GetHSteamUser();
        [DllImport(Dll, CallingConvention = C)] public static extern int SteamAPI_GetHSteamPipe();

        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamClient_GetISteamMatchmaking(IntPtr self, int user, int pipe, string version);
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamClient_GetISteamNetworking(IntPtr self, int user, int pipe, string version);
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamClient_GetISteamUtils(IntPtr self, int pipe, string version);
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamClient_GetISteamUser(IntPtr self, int user, int pipe, string version);
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamClient_GetISteamFriends(IntPtr self, int user, int pipe, string version);

        [DllImport(Dll, CallingConvention = C)] public static extern ulong SteamAPI_ISteamUser_GetSteamID(IntPtr self);
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamFriends_GetPersonaName(IntPtr self);

        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamUtils_IsAPICallCompleted(IntPtr self, ulong call, [MarshalAs(UnmanagedType.I1)] out bool failed);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamUtils_GetAPICallResult(IntPtr self, ulong call, IntPtr result, int size, int expected, [MarshalAs(UnmanagedType.I1)] out bool failed);

        [DllImport(Dll, CallingConvention = C)] public static extern ulong SteamAPI_ISteamMatchmaking_CreateLobby(IntPtr self, int type, int maxMembers);
        [DllImport(Dll, CallingConvention = C)] public static extern ulong SteamAPI_ISteamMatchmaking_JoinLobby(IntPtr self, ulong lobby);
        [DllImport(Dll, CallingConvention = C)] public static extern void SteamAPI_ISteamMatchmaking_LeaveLobby(IntPtr self, ulong lobby);
        [DllImport(Dll, CallingConvention = C)] public static extern ulong SteamAPI_ISteamMatchmaking_RequestLobbyList(IntPtr self);
        [DllImport(Dll, CallingConvention = C)] public static extern void SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(IntPtr self, byte[] key, byte[] value, int comparison);
        [DllImport(Dll, CallingConvention = C)] public static extern void SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(IntPtr self, int distance);
        [DllImport(Dll, CallingConvention = C)] public static extern void SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(IntPtr self, int max);
        [DllImport(Dll, CallingConvention = C)] public static extern ulong SteamAPI_ISteamMatchmaking_GetLobbyByIndex(IntPtr self, int index);
        [DllImport(Dll, CallingConvention = C)] public static extern IntPtr SteamAPI_ISteamMatchmaking_GetLobbyData(IntPtr self, ulong lobby, byte[] key);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamMatchmaking_SetLobbyData(IntPtr self, ulong lobby, byte[] key, byte[] value);
        [DllImport(Dll, CallingConvention = C)] public static extern int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(IntPtr self, ulong lobby);
        [DllImport(Dll, CallingConvention = C)] public static extern ulong SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(IntPtr self, ulong lobby, int index);

        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamNetworking_SendP2PPacket(IntPtr self, ulong remote, byte[] data, uint size, int sendType, int channel);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamNetworking_IsP2PPacketAvailable(IntPtr self, out uint size, int channel);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamNetworking_ReadP2PPacket(IntPtr self, byte[] dest, uint destSize, out uint size, out ulong remote, int channel);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(IntPtr self, ulong remote);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamNetworking_AllowP2PPacketRelay(IntPtr self, [MarshalAs(UnmanagedType.I1)] bool allow);
        [DllImport(Dll, CallingConvention = C)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(IntPtr self, ulong remote);
    }

    // one Steam session; every call is made under Steam.Lock
    class Steam
    {
        public static readonly object Lock = new object();
        public IntPtr Matchmaking, Networking, Utils, User, Friends;
        public ulong MySteamID;
        public string MyName;

        public const int LobbyPublic = 2, SendReliable = 2, ComparisonEqual = 0, DistanceWorldwide = 3;
        public const int CbLobbyEnter = 504, CbLobbyMatchList = 510, CbLobbyCreated = 513;

        public static byte[] Utf8Z(string s) => Encoding.UTF8.GetBytes((s ?? "") + "\0");

        public static string FromUtf8(IntPtr p)
        {
            if (p == IntPtr.Zero)
                return "";
            int n = 0;
            while (Marshal.ReadByte(p, n) != 0)
                n++;
            var b = new byte[n];
            Marshal.Copy(p, b, 0, n);
            return Encoding.UTF8.GetString(b);
        }

        // loads the game's steam_api.dll (Bin of the game) and starts Steam for app 480 (steam_appid.txt in the bridge's folder)
        public static Steam Start(string steamApiPath, Action<string> log)
        {
            if (Native.LoadLibraryW(steamApiPath) == IntPtr.Zero)
            {
                log("Couldn't load " + steamApiPath + " (error " + Marshal.GetLastWin32Error() + ")");
                return null;
            }
            if (!Native.SteamAPI_Init())
            {
                log("SteamAPI_Init failed: Steam isn't running, or isn't logged in.");
                return null;
            }
            var s = new Steam();
            var client = Native.SteamClient();
            int user = Native.SteamAPI_GetHSteamUser(), pipe = Native.SteamAPI_GetHSteamPipe();
            s.Matchmaking = Native.SteamAPI_ISteamClient_GetISteamMatchmaking(client, user, pipe, "SteamMatchMaking009");
            s.Networking = Native.SteamAPI_ISteamClient_GetISteamNetworking(client, user, pipe, "SteamNetworking005");
            s.Utils = Native.SteamAPI_ISteamClient_GetISteamUtils(client, pipe, "SteamUtils008");
            s.User = Native.SteamAPI_ISteamClient_GetISteamUser(client, user, pipe, "SteamUser019");
            s.Friends = Native.SteamAPI_ISteamClient_GetISteamFriends(client, user, pipe, "SteamFriends015");
            if (s.Matchmaking == IntPtr.Zero || s.Networking == IntPtr.Zero || s.Utils == IntPtr.Zero || s.User == IntPtr.Zero || s.Friends == IntPtr.Zero)
            {
                log("Steam didn't give one of its interfaces.");
                Native.SteamAPI_Shutdown();
                return null;
            }
            s.MySteamID = Native.SteamAPI_ISteamUser_GetSteamID(s.User);
            s.MyName = FromUtf8(Native.SteamAPI_ISteamFriends_GetPersonaName(s.Friends));
            Native.SteamAPI_ISteamNetworking_AllowP2PPacketRelay(s.Networking, true);
            return s;
        }

        // a finished call result, or null while it's still running; failed results throw
        public byte[] CallResult(ulong call, int callbackId, int size)
        {
            if (!Native.SteamAPI_ISteamUtils_IsAPICallCompleted(Utils, call, out bool failed))
                return null;
            var buf = Marshal.AllocHGlobal(size);
            try
            {
                if (!Native.SteamAPI_ISteamUtils_GetAPICallResult(Utils, call, buf, size, callbackId, out failed) || failed)
                    throw new Exception("Steam call " + callbackId + " failed");
                var data = new byte[size];
                Marshal.Copy(buf, data, 0, size);
                return data;
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
            }
        }

        public string LobbyData(ulong lobby, string key) => FromUtf8(Native.SteamAPI_ISteamMatchmaking_GetLobbyData(Matchmaking, lobby, Utf8Z(key)));
        public void SetLobbyData(ulong lobby, string key, string value) => Native.SteamAPI_ISteamMatchmaking_SetLobbyData(Matchmaking, lobby, Utf8Z(key), Utf8Z(value));
    }
}
