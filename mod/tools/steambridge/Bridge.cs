// The bridge's work:
//  - hosting: a public Steam lobby (tag csnzor) with this PC's rooms (GET /api/rooms of the local server), refreshed
//    every few seconds; the players who join it get their game connections tunnelled to 127.0.0.1:30002.
//  - joining: the other lobbies' rooms go to Data/RemoteRooms.json with a local port per host (30003+); a connection
//    to that port is tunnelled to the host over Steam P2P (the client's server transfer opens it).
// P2P messages (reliable, channel 0): [type u8][connection u32][data]. OPEN asks the host to connect to its server,
// DATA carries the bytes, CLOSE ends the connection, HELLO keeps the P2P session open.
//
// --loopback: one PC test. This PC's own lobby is listed as another player's, and its tunnel goes straight back to
// this PC's server (no P2P). Usage: CSNZ-SteamBridge.exe --steam-api <game Bin\steam_api.dll> --data <server Data>
//   [--api http://127.0.0.1:30080] [--log <file>] [--loopback]
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace SteamBridge
{
    class Bridge
    {
        const string Tag = "csnzor", Protocol = "1";
        const byte Open = 1, Data = 2, Close = 3, Hello = 4;
        const int ServerPort = 30002, FirstTunnelPort = 30003, LastTunnelPort = 30199, FirstVirtualRoom = 40000;

        readonly Steam steam;
        readonly string dataDir, apiUrl;
        readonly bool loopback;
        readonly Action<string> log;
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        readonly HttpClient http = new HttpClient { Timeout = TimeSpan.FromSeconds(3) };

        ulong myLobby;
        ulong pendingCreate, pendingList;
        string publishedRooms = null;

        class Host
        {
            public ulong SteamID, Lobby;
            public string Name;
            public int Port;
            public TcpListener Listener;
            public bool Joined;
            public ulong PendingJoin;
            public DateTime Seen;
            public List<Dictionary<string, object>> Rooms = new List<Dictionary<string, object>>();
        }
        readonly Dictionary<ulong, Host> hosts = new Dictionary<ulong, Host>();

        // a tunnelled TCP connection: on the joining side it was accepted from the game, on the host side it goes to the server
        class Conn
        {
            public ulong Peer;
            public uint Id;
            public TcpClient Tcp;
            public bool Closed;
        }
        readonly ConcurrentDictionary<string, Conn> conns = new ConcurrentDictionary<string, Conn>();
        static string Key(ulong peer, uint id) => peer + ":" + id;
        int nextConn = 1;

        // loopback: what would go over P2P, in both directions
        readonly ConcurrentQueue<Tuple<ulong, byte[]>> loop = new ConcurrentQueue<Tuple<ulong, byte[]>>();
        const ulong LoopHostSide = 1; // the host side's "peer" in loopback (the joining side's peer is MySteamID)

        public Bridge(Steam steam, string dataDir, string apiUrl, bool loopback, Action<string> log)
        {
            this.steam = steam;
            this.dataDir = dataDir;
            this.apiUrl = apiUrl.TrimEnd('/');
            this.loopback = loopback;
            this.log = log;
        }

        public void Run(Func<bool> keepRunning)
        {
            DateTime nextList = DateTime.MinValue, nextPublish = DateTime.MinValue, nextIdentity = DateTime.MinValue, nextHello = DateTime.MinValue;
            lock (Steam.Lock)
                pendingCreate = Native.SteamAPI_ISteamMatchmaking_CreateLobby(steam.Matchmaking, Steam.LobbyPublic, 250);
            while (keepRunning())
            {
                var now = DateTime.UtcNow;
                lock (Steam.Lock)
                {
                    Native.SteamAPI_RunCallbacks();
                    PollCalls();
                    if (now >= nextIdentity)
                    {
                        WriteIdentity();
                        nextIdentity = now.AddSeconds(10);
                    }
                    if (myLobby != 0 && now >= nextPublish)
                    {
                        Publish();
                        nextPublish = now.AddSeconds(3);
                    }
                    if (pendingList == 0 && now >= nextList)
                    {
                        Native.SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(steam.Matchmaking, Steam.Utf8Z(Tag), Steam.Utf8Z(Protocol), Steam.ComparisonEqual);
                        Native.SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(steam.Matchmaking, Steam.DistanceWorldwide);
                        Native.SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(steam.Matchmaking, 50);
                        pendingList = Native.SteamAPI_ISteamMatchmaking_RequestLobbyList(steam.Matchmaking);
                        nextList = now.AddSeconds(5);
                    }
                    if (myLobby != 0 && now >= nextHello)
                    {
                        KeepSessions();
                        nextHello = now.AddSeconds(2);
                    }
                    ReadP2P();
                }
                ReadLoopback();
                Thread.Sleep(15);
            }
            Shutdown();
        }

        // ------------------------------------------------------------ call results
        void PollCalls()
        {
            try
            {
                if (pendingCreate != 0)
                {
                    var r = steam.CallResult(pendingCreate, Steam.CbLobbyCreated, 16);
                    if (r != null)
                    {
                        pendingCreate = 0;
                        if (BitConverter.ToInt32(r, 0) != 1)
                            log("Couldn't create the Steam lobby (result " + BitConverter.ToInt32(r, 0) + ")");
                        else
                        {
                            myLobby = BitConverter.ToUInt64(r, 8);
                            steam.SetLobbyData(myLobby, Tag, Protocol);
                            steam.SetLobbyData(myLobby, "owner", steam.MySteamID.ToString());
                            steam.SetLobbyData(myLobby, "name", steam.MyName);
                            log("Steam lobby " + myLobby + " is open: this PC's rooms are visible to other players.");
                        }
                    }
                }
                if (pendingList != 0)
                {
                    var r = steam.CallResult(pendingList, Steam.CbLobbyMatchList, 4);
                    if (r != null)
                    {
                        pendingList = 0;
                        ReadLobbyList(BitConverter.ToInt32(r, 0));
                    }
                }
                foreach (var h in hosts.Values.Where(h => h.PendingJoin != 0).ToList())
                {
                    var r = steam.CallResult(h.PendingJoin, Steam.CbLobbyEnter, 24);
                    if (r != null)
                    {
                        h.PendingJoin = 0;
                        h.Joined = BitConverter.ToInt32(r, 16) == 1;
                        log(h.Joined ? "Joined " + h.Name + "'s Steam lobby." : "Couldn't join " + h.Name + "'s Steam lobby (" + BitConverter.ToInt32(r, 16) + ")");
                    }
                }
            }
            catch (Exception ex)
            {
                log(ex.Message);
                pendingCreate = pendingList = 0;
            }
        }

        // ------------------------------------------------------------ hosting
        void Publish()
        {
            string rooms;
            try { rooms = http.GetStringAsync(apiUrl + "/api/rooms").GetAwaiter().GetResult(); }
            catch { return; } // the server isn't up (yet)
            if (rooms == publishedRooms)
                return;
            publishedRooms = rooms;
            // a lobby value holds up to 8 KB: the room list in pieces
            var parts = new List<string>();
            for (int i = 0; i < rooms.Length; i += 7000)
                parts.Add(rooms.Substring(i, Math.Min(7000, rooms.Length - i)));
            for (int i = 0; i < parts.Count; i++)
                steam.SetLobbyData(myLobby, "rooms" + i, parts[i]);
            steam.SetLobbyData(myLobby, "parts", parts.Count.ToString());
        }

        // the players in this lobby: their P2P sessions are accepted, and a HELLO keeps them open
        void KeepSessions()
        {
            int n = Native.SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(steam.Matchmaking, myLobby);
            for (int i = 0; i < n; i++)
            {
                ulong m = Native.SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(steam.Matchmaking, myLobby, i);
                if (m == 0 || m == steam.MySteamID)
                    continue;
                Native.SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(steam.Networking, m);
                SendRaw(m, new byte[] { Hello, 0, 0, 0, 0 });
            }
            foreach (var h in hosts.Values.Where(h => h.Joined))
            {
                Native.SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(steam.Networking, h.SteamID);
                SendRaw(h.SteamID, new byte[] { Hello, 0, 0, 0, 0 });
            }
        }

        // ------------------------------------------------------------ the other players' rooms
        void ReadLobbyList(int count)
        {
            var now = DateTime.UtcNow;
            for (int i = 0; i < count; i++)
            {
                ulong lobby = Native.SteamAPI_ISteamMatchmaking_GetLobbyByIndex(steam.Matchmaking, i);
                if (!ulong.TryParse(steam.LobbyData(lobby, "owner"), out ulong owner) || owner == 0)
                    continue;
                if (owner == steam.MySteamID && !loopback)
                    continue;
                if (!hosts.TryGetValue(owner, out var h))
                {
                    int port = FreePort();
                    if (port == 0)
                        continue;
                    h = new Host { SteamID = owner, Port = port };
                    try
                    {
                        h.Listener = new TcpListener(IPAddress.Loopback, port);
                        h.Listener.Start();
                    }
                    catch (Exception ex)
                    {
                        log("Port " + port + " is busy: " + ex.Message);
                        continue;
                    }
                    hosts[owner] = h;
                    log("Found " + steam.LobbyData(lobby, "name") + "'s lobby: their rooms go through 127.0.0.1:" + port);
                    var host = h;
                    new Thread(() => AcceptLoop(host)) { IsBackground = true }.Start();
                }
                h.Lobby = lobby;
                h.Name = steam.LobbyData(lobby, "name");
                h.Seen = now;
                h.Rooms.Clear();
                int.TryParse(steam.LobbyData(lobby, "parts"), out int parts);
                var text = new StringBuilder();
                for (int p = 0; p < parts; p++)
                    text.Append(steam.LobbyData(lobby, "rooms" + p));
                try
                {
                    if (text.Length > 0 && json.Deserialize<Dictionary<string, object>>(text.ToString())["rooms"] is System.Collections.IEnumerable list)
                        foreach (Dictionary<string, object> r in list)
                            h.Rooms.Add(r);
                }
                catch (Exception ex) { log("Bad room list from " + h.Name + ": " + ex.Message); }
            }
            // hosts gone for 30 s: their port closes
            foreach (var h in hosts.Values.Where(h => (now - h.Seen).TotalSeconds > 30).ToList())
            {
                try { h.Listener.Stop(); } catch { }
                if (h.Joined && !loopback)
                    Native.SteamAPI_ISteamMatchmaking_LeaveLobby(steam.Matchmaking, h.Lobby);
                hosts.Remove(h.SteamID);
                log(h.Name + " isn't hosting anymore.");
            }
            WriteRemoteRooms();
        }

        int FreePort()
        {
            for (int p = FirstTunnelPort; p <= LastTunnelPort; p++)
                if (!hosts.Values.Any(h => h.Port == p))
                    return p;
            return 0;
        }

        void WriteRemoteRooms()
        {
            var rooms = new List<object>();
            foreach (var h in hosts.Values)
                foreach (var r in h.Rooms)
                {
                    int rid = Convert.ToInt32(r["id"]);
                    rooms.Add(new Dictionary<string, object> {
                        { "vid", FirstVirtualRoom + (h.Port - FirstTunnelPort) * 100 + rid % 100 },
                        { "rid", rid }, { "port", h.Port }, { "host", h.Name ?? "" }, { "entry", r["entry"] } });
                }
            WriteJson("RemoteRooms.json", new Dictionary<string, object> { { "rooms", rooms } });
        }

        void WriteIdentity()
        {
            WriteJson("SteamIdentity.json", new Dictionary<string, object> {
                { "steamID", steam.MySteamID.ToString() }, { "name", steam.MyName },
                { "updated", (long)(DateTime.UtcNow - new DateTime(1970, 1, 1)).TotalSeconds } });
        }

        void WriteJson(string name, object value)
        {
            var path = Path.Combine(dataDir, name);
            var tmp = path + ".tmp";
            try
            {
                File.WriteAllText(tmp, json.Serialize(value), new UTF8Encoding(false));
                if (File.Exists(path))
                    File.Delete(path);
                File.Move(tmp, path);
            }
            catch (Exception ex) { log("Couldn't write " + name + ": " + ex.Message); }
        }

        // ------------------------------------------------------------ tunnels
        // the game connects to a host's port: the connection goes to that host
        void AcceptLoop(Host h)
        {
            while (true)
            {
                TcpClient tcp;
                try { tcp = h.Listener.AcceptTcpClient(); }
                catch { return; }
                tcp.NoDelay = true;
                uint id = (uint)Interlocked.Increment(ref nextConn);
                ulong peer = loopback ? LoopHostSide : h.SteamID;
                var c = new Conn { Peer = peer, Id = id, Tcp = tcp };
                conns[Key(peer, id)] = c;
                lock (Steam.Lock)
                {
                    if (!loopback && !h.Joined && h.PendingJoin == 0)
                        h.PendingJoin = Native.SteamAPI_ISteamMatchmaking_JoinLobby(steam.Matchmaking, h.Lobby);
                }
                // the host accepts the P2P session of its lobby's members: wait for the lobby (up to 10 s)
                for (int i = 0; i < 200 && !loopback && !h.Joined; i++)
                    Thread.Sleep(50);
                log("Game connection " + id + " to " + h.Name + "'s server" + (loopback ? " (loopback)" : ""));
                Send(peer, Open, id, null, 0);
                new Thread(() => Pump(c)) { IsBackground = true }.Start();
            }
        }

        // TCP -> P2P
        void Pump(Conn c)
        {
            var buf = new byte[16384];
            try
            {
                var s = c.Tcp.GetStream();
                int n;
                while ((n = s.Read(buf, 0, buf.Length)) > 0)
                    Send(c.Peer, Data, c.Id, buf, n);
            }
            catch { }
            if (!c.Closed)
            {
                c.Closed = true;
                Send(c.Peer, Close, c.Id, null, 0);
            }
            conns.TryRemove(Key(c.Peer, c.Id), out _);
            try { c.Tcp.Close(); } catch { }
        }

        void Send(ulong peer, byte type, uint id, byte[] data, int n)
        {
            var msg = new byte[5 + n];
            msg[0] = type;
            BitConverter.GetBytes(id).CopyTo(msg, 1);
            if (n > 0)
                Buffer.BlockCopy(data, 0, msg, 5, n);
            if (loopback)
            {
                // the joining side talks to LoopHostSide; the host side answers the "peer" MySteamID
                loop.Enqueue(Tuple.Create(peer == LoopHostSide ? steam.MySteamID : LoopHostSide, msg));
                return;
            }
            lock (Steam.Lock)
                SendRaw(peer, msg);
        }

        void SendRaw(ulong peer, byte[] msg)
        {
            if (!Native.SteamAPI_ISteamNetworking_SendP2PPacket(steam.Networking, peer, msg, (uint)msg.Length, Steam.SendReliable, 0))
                log("P2P send to " + peer + " failed");
        }

        readonly byte[] p2pBuf = new byte[1 << 20];
        void ReadP2P()
        {
            while (Native.SteamAPI_ISteamNetworking_IsP2PPacketAvailable(steam.Networking, out uint size, 0))
            {
                if (!Native.SteamAPI_ISteamNetworking_ReadP2PPacket(steam.Networking, p2pBuf, (uint)p2pBuf.Length, out size, out ulong from, 0) || size < 5)
                    break;
                var msg = new byte[size];
                Buffer.BlockCopy(p2pBuf, 0, msg, 0, (int)size);
                Handle(from, msg);
            }
        }

        void ReadLoopback()
        {
            while (loop.TryDequeue(out var m))
                Handle(m.Item1, m.Item2);
        }

        // a P2P message from peer
        void Handle(ulong peer, byte[] msg)
        {
            byte type = msg[0];
            uint id = BitConverter.ToUInt32(msg, 1);
            string key = Key(peer, id);
            switch (type)
            {
                case Open:
                    // a player's game connects to this PC's server
                    try
                    {
                        var tcp = new TcpClient();
                        tcp.NoDelay = true;
                        tcp.Connect(IPAddress.Loopback, ServerPort);
                        var c = new Conn { Peer = peer, Id = id, Tcp = tcp };
                        conns[key] = c;
                        new Thread(() => Pump(c)) { IsBackground = true }.Start();
                        log("A player (" + peer + ") connects to this PC's server, connection " + id);
                    }
                    catch (Exception ex)
                    {
                        log("This PC's server didn't take the connection: " + ex.Message);
                        Send(peer, Close, id, null, 0);
                    }
                    break;
                case Data:
                    if (conns.TryGetValue(key, out var d))
                        try { d.Tcp.GetStream().Write(msg, 5, msg.Length - 5); }
                        catch { CloseConn(d); }
                    break;
                case Close:
                    if (conns.TryGetValue(key, out var x))
                    {
                        x.Closed = true;
                        CloseConn(x);
                    }
                    break;
            }
        }

        void CloseConn(Conn c)
        {
            conns.TryRemove(Key(c.Peer, c.Id), out _);
            try { c.Tcp.Close(); } catch { }
        }

        void Shutdown()
        {
            foreach (var c in conns.Values.ToList())
                CloseConn(c);
            foreach (var h in hosts.Values)
                try { h.Listener.Stop(); } catch { }
            lock (Steam.Lock)
            {
                if (myLobby != 0)
                    Native.SteamAPI_ISteamMatchmaking_LeaveLobby(steam.Matchmaking, myLobby);
                foreach (var h in hosts.Values.Where(h => h.Joined))
                    Native.SteamAPI_ISteamMatchmaking_LeaveLobby(steam.Matchmaking, h.Lobby);
            }
            foreach (var f in new[] { "RemoteRooms.json", "SteamIdentity.json" })
                try { File.Delete(Path.Combine(dataDir, f)); } catch { }
        }
    }

    static class Program
    {
        static int Main(string[] args)
        {
            string Arg(string name, string def = null)
            {
                int i = Array.IndexOf(args, name);
                return i >= 0 && i + 1 < args.Length ? args[i + 1] : def;
            }
            var exeDir = AppDomain.CurrentDomain.BaseDirectory;
            var logPath = Arg("--log", Path.Combine(exeDir, "steambridge.log"));
            var logLock = new object();
            void Log(string line)
            {
                var text = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " " + line;
                Console.WriteLine(text);
                lock (logLock)
                    try { File.AppendAllText(logPath, text + Environment.NewLine); } catch { }
            }

            var steamApi = Arg("--steam-api");
            var dataDir = Arg("--data");
            if (steamApi == null || dataDir == null)
            {
                Console.WriteLine("CSNZ-SteamBridge.exe --steam-api <game Bin\\steam_api.dll> --data <server Data folder> [--api http://127.0.0.1:30080] [--log file] [--loopback]");
                return 2;
            }
            // Spacewar: Steam's free test app, so every player can use lobbies and P2P
            Directory.SetCurrentDirectory(exeDir);
            File.WriteAllText(Path.Combine(exeDir, "steam_appid.txt"), "480");

            Log("Steam bridge starting" + (args.Contains("--loopback") ? " (loopback test)" : ""));
            var steam = Steam.Start(steamApi, Log);
            if (steam == null)
            {
                Log("Online play is off for this session; offline play works as always.");
                return 1;
            }
            Log("Steam user " + steam.MyName + " (" + steam.MySteamID + ")");

            bool running = true;
            Console.CancelKeyPress += (s, e) => { e.Cancel = true; running = false; };
            // stops by itself when the stop file appears (the launcher's way to end it cleanly)
            var stopFile = Path.Combine(dataDir, "SteamBridge.stop");
            try { File.Delete(stopFile); } catch { }
            var bridge = new Bridge(steam, dataDir, Arg("--api", "http://127.0.0.1:30080"), args.Contains("--loopback"), Log);
            try
            {
                bridge.Run(() => running && !File.Exists(stopFile));
            }
            catch (Exception ex)
            {
                Log("Bridge error: " + ex);
            }
            try { File.Delete(stopFile); } catch { }
            lock (Steam.Lock)
                Native.SteamAPI_Shutdown();
            Log("Steam bridge stopped.");
            return 0;
        }
    }
}
