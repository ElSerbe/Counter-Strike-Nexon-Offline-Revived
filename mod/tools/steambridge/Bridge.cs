// The bridge's work:
//  - hosting: a public Steam lobby (tag csnzor) with this PC's rooms (GET /api/rooms of the local server), refreshed
//    every few seconds; the players who join it get their game connections tunnelled to 127.0.0.1:30002.
//  - joining: the other lobbies' rooms go to Data/RemoteRooms.json with a local port per host (30003+); a connection
//    to that port is tunnelled to the host over Steam P2P (the client's server transfer opens it).
// P2P messages (reliable, channel 0): [type u8][connection u32][data]. OPEN asks the host to connect to its server,
// DATA carries the bytes, CLOSE ends the connection, HELLO keeps the P2P session open.
//  - matches: the room host's game is the match server and the players' games talk to it over UDP. The server gives each
//    game addresses on its own PC's bridge (ServerTransfer::NetworkFor): on a joining PC the host PC's game is
//    127.0.0.1:31000+k*2+portID (k = the tunnel), on the host PC a remote player is 127.0.0.1:33000+slot*2+portID.
//    portID 0 is a game's server port, 1 its client port; GET /api/udp gives this PC's game ports and the remote slots.
//    UDP goes over P2P channel 1: [5][direction: 0 player->host, 1 host->player][host portID][player portID][data].
//
// --loopback: one PC test. This PC's own lobby is listed as another player's, and its tunnel goes straight back to
// this PC's server (no P2P). Usage: CSNZ-SteamBridge.exe --steam-api <game Bin\steam_api.dll> --data <server Data>
//   [--api http://127.0.0.1:30080] [--log <file>] [--loopback] [--verbose]
//  - Studio maps: this PC's published maps go in its lobby too (GET /studio/net/catalog), the other lobbies' maps to
//    Data/StudioNet.json; maps the server asks for (GET /studio/net/outbox) are fetched over P2P from the PCs that have them
//    (STUDIO messages, see the Studio section) and handed to the server (POST /studio/net/result).
//
// --verbose (the launcher's "Detailed logs"): every tunnel message and match packet in the log ("detail:" lines, the
// first bytes of each packet), up to 20000 lines, and the UDP totals every 5 s.
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
        const byte Open = 1, Data = 2, Close = 3, Hello = 4, Udp = 5, Mail = 6, Studio = 7;
        const int ServerPort = 30002, FirstTunnelPort = 30003, LastTunnelPort = 30199, FirstVirtualRoom = 40000;
        const int JoinerUdpBase = 31000, HostUdpBase = 33000, UnreliableMax = 1200;

        readonly Steam steam;
        readonly string dataDir, apiUrl;
        readonly bool loopback;
        readonly Action<string> log;
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        readonly HttpClient http = new HttpClient { Timeout = TimeSpan.FromSeconds(3) };

        ulong myLobby;
        ulong pendingCreate, pendingList;
        string publishedRooms = null;
        bool publishFailLogged;
        int publishedCount = -1; // the room count last logged
        readonly DateTime started = DateTime.Now;

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
            public int LoggedRooms = -1; // the room count last logged (a change is logged)
            public List<object> Maps = new List<object>(); // the Studio maps this player publishes
            public List<object> Have = new List<object>(); // the share codes of the maps their PC fetched (it can pass them on)
            public UdpClient[] Udp; // the host PC's game, as this PC's game sees it (31000+k*2+portID)
        }

        // a player from another PC on this PC's server: their game, as this PC's game sees it (33000+slot*2+portID)
        class Remote
        {
            public ulong SteamID;
            public int Slot;
            public UdpClient[] Udp;
        }
        readonly Dictionary<ulong, Remote> remotes = new Dictionary<ulong, Remote>();
        volatile int myPort0, myPort1; // this PC's game: server and client ports (its UDP holepunch, /api/udp)
        // where this PC's game really sent its match packets from (by portID): replies go there. A packet from a port the
        // holepunch didn't announce is no longer dropped: it counts as the client port's and the reply goes back to it
        volatile int seenPort0, seenPort1;
        long udpToPeer, udpFromPeer, udpDropped;
        long reportedTo = -1, reportedFrom = -1, reportedDropped = -1;
        DateTime nextUdpReport = DateTime.MinValue;
        int udpNotes;
        void UdpNote(string text)
        {
            if (System.Threading.Interlocked.Increment(ref udpNotes) <= 60)
                log("UDP: " + text);
            else
                Detail("UDP: " + text);
        }

        // --verbose
        public bool Verbose;
        int detailLines;
        void Detail(string text)
        {
            if (!Verbose)
                return;
            int n = System.Threading.Interlocked.Increment(ref detailLines);
            if (n <= 20000)
                log("detail: " + text);
            else if (n == 20001)
                log("detail: 20000 lines written, no more details this session (the UDP totals go on)");
        }
        // the first bytes of a packet: hex, and its first byte as a letter when it is one (the game's holepunch: W, X, Y, `)
        static string Peek(byte[] data, int offset, int n)
        {
            var sb = new StringBuilder();
            if (n > 0 && data[offset] >= 0x20 && data[offset] < 0x7f)
                sb.Append('\'').Append((char)data[offset]).Append("' ");
            for (int i = 0; i < Math.Min(n, 24); i++)
                sb.Append(data[offset + i].ToString("x2"));
            if (n > 24)
                sb.Append("..");
            return sb.ToString();
        }
        static string TypeName(byte type) => type == Open ? "OPEN" : type == Data ? "DATA" : type == Close ? "CLOSE" : type == Hello ? "HELLO" : "type " + type;
        readonly Dictionary<ulong, Host> hosts = new Dictionary<ulong, Host>();

        // a tunnelled TCP connection: on the joining side it was accepted from the game, on the host side it goes to the server
        class Conn
        {
            public ulong Peer;
            public uint Id;
            public TcpClient Tcp;
            public bool Closed;
            // joining side: the game connecting to another player's server (CheckStalled)
            public string HostName;
            public long Received;
            public DateTime SentAt;
            public bool AwaitingReply;
            // host side: what came in while the connection to this PC's server was still being made (null once made)
            public List<byte[]> Pending;
            // joining side: the other server never answered, the game goes to this PC's own server (FallBackHome)
            public volatile bool FallingBack;
            public volatile TcpClient Local;
        }

        // The game connecting to another player's server (the first 32 KB, its login) waits for every answer: when that
        // server stops answering, the game spun in its TLS connect for good (a "frozen" game). After 60 s without an
        // answer the connection is closed and logged, and the game gets a disconnection instead. (25 s until v1.1.40: a
        // friend's Steam connection that delivered after 20-35 s was cut although their server had answered at once.)
        const int StallSeconds = 60;
        void CheckStalled()
        {
            var now = DateTime.UtcNow;
            foreach (var c in conns.Values.ToList())
            {
                if (c.HostName != null && c.AwaitingReply && c.Received == 0 && !c.Closed && (now - c.SentAt).TotalSeconds >= FallbackSeconds)
                {
                    FallBackHome(c);
                    continue;
                }
                if (c.HostName == null || !c.AwaitingReply || c.Received >= 32768 || (now - c.SentAt).TotalSeconds < StallSeconds)
                    continue;
                log(c.HostName + "'s server didn't answer for " + StallSeconds + " s while this PC's game was connecting to it (" +
                    c.Received + " bytes in so far): connection " + c.Id + " closed. " + LinkState(c.Peer) + ". Their PC or the Steam " +
                    "connection to it is stuck; their logs (Logs > Copy the last session's logs) say why.");
                c.Closed = true;
                Send(c.Peer, Close, c.Id, null, 0);
                CloseConn(c);
            }
        }
        // how Steam carries the traffic to another PC: straight or through Steam's relay, and what waits to be sent
        string PeerName(ulong peer) => hosts.Values.FirstOrDefault(h => h.SteamID == peer)?.Name ?? peer.ToString();
        string LinkState(ulong peer)
        {
            if (loopback || !Native.SteamAPI_ISteamNetworking_GetP2PSessionState(steam.Networking, peer, out var s))
                return "Steam connection to " + PeerName(peer) + ": no state";
            string how = s.ConnectionActive != 0 ? (s.UsingRelay != 0 ? "through Steam's relay" : "direct") :
                s.Connecting != 0 ? "still connecting" : "not connected (error " + s.SessionError + ")";
            return "Steam connection to " + PeerName(peer) + ": " + how + ", " + (s.BytesQueuedForSend / 1024) + " KB (" +
                s.PacketsQueuedForSend + " packets) waiting to be sent from this PC";
        }

        // every 2 s: the PCs with open connections or profile mail. Logged when the route changes and when this PC's sending
        // backs up (a friend's PC whose traffic arrived 20-35 s late: was it their upload, the relay, the other side?)
        readonly Dictionary<ulong, string> linkRoute = new Dictionary<ulong, string>();
        readonly Dictionary<ulong, DateTime> linkBacklogLogged = new Dictionary<ulong, DateTime>();
        void WatchLinks()
        {
            if (loopback)
                return;
            var peers = new HashSet<ulong>(conns.Values.Where(c => !c.Closed).Select(c => c.Peer));
            foreach (var m in outMail)
                if (m.Sent)
                    peers.Add(m.To);
            var now = DateTime.UtcNow;
            foreach (var peer in peers)
            {
                if (!Native.SteamAPI_ISteamNetworking_GetP2PSessionState(steam.Networking, peer, out var s) || s.ConnectionActive == 0)
                    continue;
                string route = s.UsingRelay != 0 ? "through Steam's relay" : "direct";
                if (!linkRoute.TryGetValue(peer, out var was) || was != route)
                {
                    linkRoute[peer] = route;
                    log("Steam connection to " + PeerName(peer) + ": " + route);
                }
                if (s.BytesQueuedForSend >= 8192 && (!linkBacklogLogged.TryGetValue(peer, out var at) || (now - at).TotalSeconds >= 20))
                {
                    linkBacklogLogged[peer] = now;
                    log(LinkState(peer) + ": Steam sends this PC's data to them slowly");
                }
            }
        }

        // The other player's server sent nothing at all for 40 s (not even its greeting): the game hasn't sent anything yet
        // either, so it can still be given this PC's own server instead. The server is told first (POST
        // /api/transfer/fallback): the game's login with the ticket meant for the other server then logs the player in at
        // home with their return ticket, and a message says what happened. Without this, the game sat in front of a dead
        // connection and had to be restarted.
        const int FallbackSeconds = 40, HomeServerPort = 30002;
        void FallBackHome(Conn c)
        {
            c.FallingBack = true;
            c.Closed = true;
            Send(c.Peer, Close, c.Id, null, 0);
            conns.TryRemove(Key(c.Peer, c.Id), out _);
            log(c.HostName + "'s server didn't answer for " + FallbackSeconds + " s while this PC's game was connecting to it (connection " +
                c.Id + "): the game goes back to this PC's own server. " + LinkState(c.Peer) + ".");
            new Thread(() =>
            {
                try
                {
                    var body = json.Serialize(new Dictionary<string, object> { { "host", c.HostName } });
                    http.PostAsync(apiUrl + "/api/transfer/fallback", new StringContent(body, Encoding.UTF8, "application/json")).GetAwaiter().GetResult();
                    var local = new TcpClient { NoDelay = true };
                    local.Connect("127.0.0.1", HomeServerPort);
                    c.Local = local;
                    // this PC's server -> the game
                    var buf = new byte[16384];
                    var from = local.GetStream();
                    var to = c.Tcp.GetStream();
                    int n;
                    while ((n = from.Read(buf, 0, buf.Length)) > 0)
                        to.Write(buf, 0, n);
                }
                catch (Exception ex) { log("The game couldn't be taken back to this PC's server: " + ex.Message); }
                try { c.Tcp.Close(); } catch { }
                try { c.Local?.Close(); } catch { }
            }) { IsBackground = true }.Start();
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

        // Two loops: this one (Steam: callbacks, lobbies, P2P in and out) never waits for this PC's server; ServerLoop asks the
        // server over HTTP (rooms, match ports, profile mail, Studio) and applies the answers under Steam.Lock. Before
        // v1.1.40 one loop did both under the lock, so a server slow to answer stalled the P2P traffic too: a friend's
        // game got no answer for 10-100 s (profile mail lost, logins stuck).
        public void Run(Func<bool> keepRunning)
        {
            DateTime nextList = DateTime.MinValue, nextHello = DateTime.MinValue;
            lock (Steam.Lock)
                pendingCreate = Native.SteamAPI_ISteamMatchmaking_CreateLobby(steam.Matchmaking, Steam.LobbyPublic, 250);
            new Thread(() => ServerLoop(keepRunning)) { IsBackground = true }.Start();
            while (keepRunning())
            {
                var now = DateTime.UtcNow;
                lock (Steam.Lock)
                {
                    Native.SteamAPI_RunCallbacks();
                    PollCalls();
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
                        CheckStalled();
                        WatchLinks();
                        nextHello = now.AddSeconds(2);
                    }
                    ReadP2P();
                }
                ReadLoopback();
                Thread.Sleep(15);
            }
            Shutdown();
        }

        // this PC's server, over HTTP: each answer is fetched outside Steam.Lock and applied under it
        void ServerLoop(Func<bool> keepRunning)
        {
            DateTime nextStudio = DateTime.MinValue, nextStudioPublish = DateTime.MinValue, nextMail = DateTime.MinValue,
                     nextPublish = DateTime.MinValue, nextIdentity = DateTime.MinValue, nextUdp = DateTime.MinValue;
            while (keepRunning())
            {
                try
                {
                    var now = DateTime.UtcNow;
                    if (now >= nextIdentity)
                    {
                        WriteIdentity();
                        nextIdentity = now.AddSeconds(10);
                    }
                    if (myLobby != 0 && now >= nextPublish)
                    {
                        var rooms = ApiGet("/api/rooms", out var error);
                        lock (Steam.Lock)
                            Publish(rooms, error);
                        nextPublish = DateTime.UtcNow.AddSeconds(3);
                    }
                    if (now >= nextUdp)
                    {
                        var udp = ApiGet("/api/udp", out _);
                        lock (Steam.Lock)
                            ReadUdpState(udp);
                        nextUdp = DateTime.UtcNow.AddSeconds(2);
                    }
                    if (now >= nextMail)
                    {
                        var outbox = ApiGet("/api/guest/outbox", out _);
                        lock (Steam.Lock)
                            PollMail(outbox);
                        nextMail = DateTime.UtcNow.AddMilliseconds(300);
                    }
                    if (myLobby != 0 && now >= nextStudioPublish)
                    {
                        var catalog = ApiGet("/studio/net/catalog", out _);
                        lock (Steam.Lock)
                            PublishStudio(catalog);
                        nextStudioPublish = DateTime.UtcNow.AddSeconds(10);
                    }
                    if (now >= nextStudio)
                    {
                        var requests = ApiGet("/studio/net/outbox", out _);
                        lock (Steam.Lock)
                            PollStudio(requests);
                        nextStudio = DateTime.UtcNow.AddMilliseconds(500);
                    }
                }
                catch (Exception ex) { log("Bridge (server loop): " + ex.Message); }
                Thread.Sleep(20);
            }
        }

        // GET on this PC's server: the text, or null (error says why). A slow answer is logged, once a minute at most:
        // everything online that goes through this PC's server waits for it.
        DateTime nextSlowLog = DateTime.MinValue;
        string ApiGet(string path, out Exception error)
        {
            error = null;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            try { return http.GetStringAsync(apiUrl + path).GetAwaiter().GetResult(); }
            catch (Exception ex)
            {
                error = ex.InnerException ?? ex;
                return null;
            }
            finally
            {
                if (watch.Elapsed.TotalSeconds >= 1.5 && DateTime.UtcNow >= nextSlowLog && (DateTime.Now - started).TotalSeconds > 60)
                {
                    nextSlowLog = DateTime.UtcNow.AddMinutes(1);
                    log("This PC's server took " + watch.Elapsed.TotalSeconds.ToString("0.0") + " s to answer " + path + " (" +
                        (error != null ? error.Message : "it did answer") + "): it's slow, so other players wait for this PC's rooms and profiles. " +
                        "Something may be slowing this PC down (an antivirus scanning the server, a busy disk or CPU).");
                }
            }
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
        // rooms: GET /api/rooms's answer (ServerLoop), null when it failed (error)
        void Publish(string rooms, Exception error)
        {
            if (rooms == null)
            {
                // the server isn't up (yet); said once a minute in, when it should be: other players see no rooms
                if (!publishFailLogged && (DateTime.Now - started).TotalSeconds > 60)
                {
                    publishFailLogged = true;
                    log("This PC's rooms can't be published: the local server's " + apiUrl + "/api/rooms doesn't answer (" +
                        error?.Message + "). Other players see this PC's lobby without rooms.");
                }
                return;
            }
            if (publishFailLogged)
            {
                publishFailLogged = false;
                log("This PC's rooms are published again.");
            }
            int count = System.Text.RegularExpressions.Regex.Matches(rooms, "\"entry\"").Count;
            if (count != publishedCount)
            {
                publishedCount = count;
                log(count == 0 ? "This PC has no room open: other players see no rooms of yours." :
                                 "This PC's " + count + " room" + (count == 1 ? " is" : "s are") + " visible to other players.");
            }
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
            Detail("Steam lobby list: " + count + " lobbies (this PC's own included)");
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
                    var host = h;
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
                    int k = port - FirstTunnelPort;
                    h.Udp = OpenUdp(JoinerUdpBase + k * 2, JoinerUdpBase + k * 2 + 1, (portID, from, data, n) =>
                    {
                        // this PC's game -> the host PC's game
                        int mine = MyPortID(from.Port);
                        if (mine >= 0)
                            SendUdp(loopback ? LoopHostSide : host.SteamID, 0, portID, mine, data, n);
                        else
                            Dropped("a packet from 127.0.0.1:" + from.Port + " to " + (JoinerUdpBase + k * 2 + portID));
                    });
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
                if (h.Rooms.Count != h.LoggedRooms)
                {
                    h.LoggedRooms = h.Rooms.Count;
                    log(h.Name + (h.Rooms.Count == 0 ? " has no room open (or their server doesn't publish them)" :
                                  " has " + h.Rooms.Count + " room" + (h.Rooms.Count == 1 ? "" : "s") + " open"));
                }
                ReadStudioCatalog(h, lobby);
                Native.SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(steam.Networking, owner);
            }
            // hosts gone for 30 s: their port closes
            foreach (var h in hosts.Values.Where(h => (now - h.Seen).TotalSeconds > 30).ToList())
            {
                try { h.Listener.Stop(); } catch { }
                CloseUdp(h.Udp);
                if (h.Joined && !loopback)
                    Native.SteamAPI_ISteamMatchmaking_LeaveLobby(steam.Matchmaking, h.Lobby);
                hosts.Remove(h.SteamID);
                log(h.Name + " isn't hosting anymore.");
            }
            WriteRemoteRooms();
            WriteStudioNet();
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
                        { "rid", rid }, { "port", h.Port }, { "host", h.Name ?? "" }, { "hostID", h.SteamID.ToString() }, { "entry", r["entry"] } });
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
                var c = new Conn { Peer = peer, Id = id, Tcp = tcp, HostName = h.Name ?? "The other player" };
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
                // the game waits for the server's first message: no answer at all counts as stuck too
                c.SentAt = DateTime.UtcNow;
                c.AwaitingReply = true;
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
                {
                    if (c.FallingBack)
                    {
                        // the game talks to this PC's own server now (FallBackHome): once that connection is made
                        for (int i = 0; i < 200 && c.Local == null; i++)
                            Thread.Sleep(50);
                        if (c.Local == null)
                            break;
                        c.Local.GetStream().Write(buf, 0, n);
                        continue;
                    }
                    if (c.HostName != null && !c.AwaitingReply)
                    {
                        c.SentAt = DateTime.UtcNow;
                        c.AwaitingReply = true;
                    }
                    Send(c.Peer, Data, c.Id, buf, n);
                }
            }
            catch { }
            if (!c.Closed)
            {
                c.Closed = true;
                Send(c.Peer, Close, c.Id, null, 0);
            }
            conns.TryRemove(Key(c.Peer, c.Id), out _);
            try { c.Tcp.Close(); } catch { }
            try { c.Local?.Close(); } catch { }
        }

        void Send(ulong peer, byte type, uint id, byte[] data, int n)
        {
            Detail("tunnel out to " + peer + ": " + TypeName(type) + " connection " + id + (n > 0 ? ", " + n + " bytes" : ""));
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
            while (Native.SteamAPI_ISteamNetworking_IsP2PPacketAvailable(steam.Networking, out uint usize, 1))
            {
                if (!Native.SteamAPI_ISteamNetworking_ReadP2PPacket(steam.Networking, p2pBuf, (uint)p2pBuf.Length, out usize, out ulong ufrom, 1) || usize < 4)
                    break;
                HandleUdp(ufrom, p2pBuf, (int)usize);
            }
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
            while (udpLoop.TryDequeue(out var u))
                HandleUdp(u.Item1, u.Item2, u.Item2.Length);
        }

        // a P2P message from peer
        void Handle(ulong peer, byte[] msg)
        {
            byte type = msg[0];
            uint id = BitConverter.ToUInt32(msg, 1);
            string key = Key(peer, id);
            if (type != Hello)
                Detail("tunnel in from " + peer + ": " + TypeName(type) + " connection " + id + (msg.Length > 5 ? ", " + (msg.Length - 5) + " bytes" : ""));
            switch (type)
            {
                case Mail:
                    HandleMail(peer, msg);
                    break;
                case Studio:
                    HandleStudio(peer, msg);
                    break;
                case Open:
                {
                    // a player's game connects to this PC's server: made on the connection's own thread (the P2P loop
                    // doesn't wait for this PC's server); what comes in meanwhile waits in Pending
                    var c = new Conn { Peer = peer, Id = id, Tcp = new TcpClient { NoDelay = true }, Pending = new List<byte[]>() };
                    conns[key] = c;
                    new Thread(() =>
                    {
                        var watch = System.Diagnostics.Stopwatch.StartNew();
                        try
                        {
                            c.Tcp.Connect(IPAddress.Loopback, ServerPort);
                            lock (c)
                            {
                                foreach (var p in c.Pending)
                                    c.Tcp.GetStream().Write(p, 0, p.Length);
                                c.Pending = null;
                            }
                        }
                        catch (Exception ex)
                        {
                            log("This PC's server didn't take a player's connection (" + watch.Elapsed.TotalSeconds.ToString("0.0") + " s): " + ex.Message);
                            c.Closed = true;
                            Send(peer, Close, id, null, 0);
                            CloseConn(c);
                            return;
                        }
                        log("A player (" + peer + ") connects to this PC's server, connection " + id +
                            (watch.Elapsed.TotalSeconds >= 1 ? " (the server took " + watch.Elapsed.TotalSeconds.ToString("0.0") + " s to take it)" : ""));
                        Pump(c);
                    }) { IsBackground = true }.Start();
                    break;
                }
                case Data:
                    if (conns.TryGetValue(key, out var d))
                    {
                        d.Received += msg.Length - 5;
                        d.AwaitingReply = false;
                        lock (d)
                        {
                            if (d.Pending != null)
                            {
                                var piece = new byte[msg.Length - 5];
                                Buffer.BlockCopy(msg, 5, piece, 0, piece.Length);
                                d.Pending.Add(piece);
                                break;
                            }
                        }
                        try { d.Tcp.GetStream().Write(msg, 5, msg.Length - 5); }
                        catch { CloseConn(d); }
                    }
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

        // ------------------------------------------------------------ profiles ("mail" between the servers)
        // The servers carry a player's profile to the room host's server and back (the server's guestprofile.h). The bridge
        // takes the server's mail from GET /api/guest/outbox, sends it over P2P as [6][flags u32: 1 = gzip][JSON], hands mail
        // from other PCs to POST /api/guest/inbox with the sender's Steam ID (Steam vouches for it) and answers with an ack;
        // the sender's server learns the outcome from POST /api/guest/sent. A host's lobby is joined first (the host accepts
        // only its lobby members' P2P sessions); no ack within 20 s = failed.
        class OutMail
        {
            public int Id;
            public ulong To;
            public string Text;
            public DateTime Deadline, SentAt;
            public bool Sent;
        }
        // a profile (20-200 KB of text, gzipped) over a slow Steam connection took 25 s: 20 s was too short until v1.1.40
        const int MailSeconds = 90;
        readonly List<OutMail> outMail = new List<OutMail>();
        readonly HashSet<ulong> mailPeers = new HashSet<ulong>();
        readonly HttpClient mailHttp = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };

        // text: GET /api/guest/outbox's answer (ServerLoop)
        void PollMail(string text)
        {
            if (text == null)
                return; // the server isn't up, or is older than this bridge
            Dictionary<string, object> box;
            try { box = json.Deserialize<Dictionary<string, object>>(text); }
            catch { return; }
            if (box.TryGetValue("peers", out var peers) && peers is System.Collections.IEnumerable pl)
                foreach (var p in pl)
                    if (ulong.TryParse(p?.ToString(), out ulong id) && id != 0)
                    {
                        // the PCs this server expects profiles from (its players' hosts, its guests): their P2P is welcome
                        if (mailPeers.Add(id))
                            Detail("P2P sessions from " + id + " are accepted (profiles)");
                        Native.SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(steam.Networking, id);
                    }
            if (box.TryGetValue("mail", out var mail) && mail is System.Collections.IEnumerable ml)
                foreach (Dictionary<string, object> m in ml)
                {
                    if (!ulong.TryParse(m["to"]?.ToString(), out ulong to))
                        continue;
                    outMail.Add(new OutMail { Id = Convert.ToInt32(m["id"]), To = to, Text = m["text"]?.ToString() ?? "{}", Deadline = DateTime.UtcNow.AddSeconds(MailSeconds) });
                    Detail("mail " + m["id"] + " for " + to + ", " + ((string)m["text"]).Length + " characters");
                }
            foreach (var m in outMail.ToList())
            {
                if (m.Sent)
                {
                    if (DateTime.UtcNow > m.Deadline)
                    {
                        outMail.Remove(m);
                        log("Profile mail " + m.Id + " to " + PeerName(m.To) + ": no answer in " + MailSeconds + " s. " + LinkState(m.To));
                        MailSent(m.Id, false, "no answer from the other PC");
                    }
                    continue;
                }
                // a host's server takes P2P only from its lobby's members: join it first
                var h = hosts.Values.FirstOrDefault(x => x.SteamID == m.To);
                if (h != null && !h.Joined && !loopback)
                {
                    if (h.PendingJoin == 0)
                        h.PendingJoin = Native.SteamAPI_ISteamMatchmaking_JoinLobby(steam.Matchmaking, h.Lobby);
                    if (DateTime.UtcNow > m.Deadline)
                    {
                        outMail.Remove(m);
                        MailSent(m.Id, false, "couldn't join " + h.Name + "'s Steam lobby");
                    }
                    continue;
                }
                var payload = Gzip(Encoding.UTF8.GetBytes("{\"t\":\"mail\",\"id\":" + m.Id + ",\"body\":" + m.Text + "}"));
                var msg = new byte[5 + payload.Length];
                msg[0] = Mail;
                BitConverter.GetBytes(1u).CopyTo(msg, 1);
                payload.CopyTo(msg, 5);
                Native.SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(steam.Networking, m.To);
                if (loopback)
                    loop.Enqueue(Tuple.Create(steam.MySteamID, msg));
                else
                    SendRaw(m.To, msg);
                m.Sent = true;
                m.SentAt = DateTime.UtcNow;
                m.Deadline = m.SentAt.AddSeconds(MailSeconds);
                log("Profile mail " + m.Id + " sent to " + (h?.Name ?? m.To.ToString()) + " (" + payload.Length / 1024 + " KB)");
            }
        }

        // (on its own thread: callers hold Steam.Lock)
        void MailSent(int id, bool ok, string error)
        {
            var body = json.Serialize(new Dictionary<string, object> { { "id", id }, { "ok", ok }, { "error", error ?? "" } });
            new Thread(() =>
            {
                try { mailHttp.PostAsync(apiUrl + "/api/guest/sent", new StringContent(body, Encoding.UTF8, "application/json")).GetAwaiter().GetResult(); }
                catch (Exception ex) { log("Couldn't tell this PC's server about profile mail " + id + ": " + ex.Message); }
            }) { IsBackground = true }.Start();
        }

        void HandleMail(ulong peer, byte[] msg)
        {
            string text;
            try
            {
                text = BitConverter.ToUInt32(msg, 1) == 1 ? Encoding.UTF8.GetString(Gunzip(msg, 5)) : Encoding.UTF8.GetString(msg, 5, msg.Length - 5);
            }
            catch (Exception ex)
            {
                log("Bad profile mail from " + peer + ": " + ex.Message);
                return;
            }
            // the "t" and "id" fields lead the text: read them without parsing the (big) body
            var head = text.Length > 200 ? text.Substring(0, 200) : text;
            var t = System.Text.RegularExpressions.Regex.Match(head, "^\\{\"t\":\"(\\w+)\",\"id\":(\\d+)");
            if (!t.Success)
                return;
            int id = int.Parse(t.Groups[2].Value);
            if (t.Groups[1].Value == "ack")
            {
                var m = outMail.FirstOrDefault(x => x.Id == id && (x.To == peer || loopback));
                if (m == null)
                    return;
                outMail.Remove(m);
                var ack = json.Deserialize<Dictionary<string, object>>(text);
                bool ok = ack.TryGetValue("ok", out var o) && o is bool b && b;
                string error = ack.TryGetValue("error", out var e) ? e?.ToString() : null;
                double took = (DateTime.UtcNow - m.SentAt).TotalSeconds;
                log("Profile mail " + id + " " + (ok ? "taken by" : "refused by (" + error + ")") + " " + peer + (took >= 5 ? " after " + took.ToString("0") + " s" : ""));
                MailSent(id, ok, error);
                return;
            }
            if (t.Groups[1].Value != "mail")
                return;
            int bodyAt = text.IndexOf(",\"body\":", StringComparison.Ordinal);
            string bodyText = bodyAt < 0 ? "null" : text.Substring(bodyAt + 8, text.Length - bodyAt - 9);
            string post = "{\"from\":\"" + (loopback ? steam.MySteamID : peer) + "\",\"body\":" + bodyText + "}";
            // to this PC's server on a thread of its own (the P2P loop doesn't wait for it), then the ack
            new Thread(() =>
            {
                bool taken = false;
                string why = "this PC's server didn't answer";
                try
                {
                    var resp = mailHttp.PostAsync(apiUrl + "/api/guest/inbox", new StringContent(post, Encoding.UTF8, "application/json")).GetAwaiter().GetResult();
                    var answer = json.Deserialize<Dictionary<string, object>>(resp.Content.ReadAsStringAsync().GetAwaiter().GetResult());
                    taken = answer.TryGetValue("ok", out var o) && o is bool b && b;
                    why = answer.TryGetValue("error", out var e) ? e?.ToString() : null;
                }
                catch (Exception ex) { why = "this PC's server: " + ex.Message; }
                log("Profile mail from " + peer + (taken ? " handed to this PC's server" : " not taken: " + why));
                var reply = Encoding.UTF8.GetBytes(json.Serialize(new Dictionary<string, object> { { "t", "ack" }, { "id", id }, { "ok", taken }, { "error", why ?? "" } }));
                var r = new byte[5 + reply.Length];
                r[0] = Mail;
                reply.CopyTo(r, 5);
                if (loopback)
                    loop.Enqueue(Tuple.Create(LoopHostSide, r));
                else
                    lock (Steam.Lock)
                        SendRaw(peer, r);
            }) { IsBackground = true }.Start();
        }

        static byte[] Gzip(byte[] data)
        {
            using (var m = new MemoryStream())
            {
                using (var z = new System.IO.Compression.GZipStream(m, System.IO.Compression.CompressionLevel.Optimal, true))
                    z.Write(data, 0, data.Length);
                return m.ToArray();
            }
        }

        static byte[] Gunzip(byte[] data, int offset)
        {
            using (var z = new System.IO.Compression.GZipStream(new MemoryStream(data, offset, data.Length - offset), System.IO.Compression.CompressionMode.Decompress))
            using (var m = new MemoryStream())
            {
                z.CopyTo(m);
                return m.ToArray();
            }
        }

        // ------------------------------------------------------------ Studio maps
        // Lobby data "studio0".."studio<n-1>" (n = "studioparts"): {"maps": [...], "have": [code...]} from GET /studio/net/catalog.
        // P2P (reliable, channel 0): [7][message u32][part u16][parts u16][a piece of the gzipped JSON]; big maps come in parts.
        //   {"t":"get","q":N,"key":"<share code, slot id or resource id>"}  ->  {"t":"map","q":N,"ok":true,"slot":{...},"vxl":"<base64>"}
        //   (or "ok":false). The asking PC tries the players whose lobby lists the map, the hosts it joined and the players on
        //   its server first, then everyone else.
        string publishedStudio = null;
        int nextStudioMessage = 1;
        readonly HttpClient studioHttp = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };

        class StudioRequest
        {
            public int Id;
            public string Key;
            public int Wave;
            public DateTime WaveEnds, Ends;
            public readonly HashSet<ulong> Asked = new HashSet<ulong>(), Answered = new HashSet<ulong>();
        }
        readonly Dictionary<int, StudioRequest> studioRequests = new Dictionary<int, StudioRequest>();

        class Partial
        {
            public byte[][] Parts;
            public int Got;
            public DateTime Started;
        }
        readonly Dictionary<string, Partial> partials = new Dictionary<string, Partial>();

        // catalog: GET /studio/net/catalog's answer (ServerLoop)
        void PublishStudio(string catalog)
        {
            if (catalog == null)
                return; // the server isn't up, or is older than this bridge
            if (catalog == publishedStudio)
                return;
            publishedStudio = catalog;
            var parts = new List<string>();
            for (int i = 0; i < catalog.Length; i += 7000)
                parts.Add(catalog.Substring(i, Math.Min(7000, catalog.Length - i)));
            for (int i = 0; i < parts.Count; i++)
                steam.SetLobbyData(myLobby, "studio" + i, parts[i]);
            steam.SetLobbyData(myLobby, "studioparts", parts.Count.ToString());
            try
            {
                var maps = json.Deserialize<Dictionary<string, object>>(catalog)["maps"] as System.Collections.ICollection;
                log("Studio: " + (maps?.Count ?? 0) + " published maps of this PC are in the online list");
            }
            catch { }
        }

        void ReadStudioCatalog(Host h, ulong lobby)
        {
            h.Maps = new List<object>();
            h.Have = new List<object>();
            int.TryParse(steam.LobbyData(lobby, "studioparts"), out int parts);
            if (parts <= 0)
                return;
            var text = new StringBuilder();
            for (int p = 0; p < parts; p++)
                text.Append(steam.LobbyData(lobby, "studio" + p));
            try
            {
                var c = json.Deserialize<Dictionary<string, object>>(text.ToString());
                if (c.TryGetValue("maps", out var m) && m is System.Collections.IEnumerable ml)
                    foreach (var x in ml)
                        h.Maps.Add(x);
                if (c.TryGetValue("have", out var hv) && hv is System.Collections.IEnumerable hl)
                    foreach (var x in hl)
                        h.Have.Add(x);
            }
            catch (Exception ex) { Detail("Bad Studio list from " + h.Name + ": " + ex.Message); }
        }

        void WriteStudioNet()
        {
            var list = new List<object>();
            foreach (var h in hosts.Values)
                list.Add(new Dictionary<string, object> { { "steamID", h.SteamID.ToString() }, { "name", h.Name ?? "" }, { "maps", h.Maps }, { "have", h.Have } });
            WriteJson("StudioNet.json", new Dictionary<string, object> {
                { "updated", (long)(DateTime.UtcNow - new DateTime(1970, 1, 1)).TotalSeconds }, { "hosts", list } });
        }

        static string Str(object o) => o?.ToString() ?? "";

        bool HostHasMap(Host h, string key)
        {
            string code = key.Replace("-", "").ToUpperInvariant();
            foreach (var o in h.Maps)
                if (o is Dictionary<string, object> m && (Str(m.TryGetValue("id", out var a) ? a : null) == key ||
                    Str(m.TryGetValue("resource_id", out var b) ? b : null) == key ||
                    Str(m.TryGetValue("code", out var c) ? c : null).Replace("-", "") == code))
                    return true;
            foreach (var o in h.Have)
                if (Str(o).Replace("-", "") == code)
                    return true;
            return false;
        }

        // text: GET /studio/net/outbox's answer (ServerLoop)
        void PollStudio(string text)
        {
            if (text == null)
                return;
            try
            {
                if (json.Deserialize<Dictionary<string, object>>(text)["requests"] is System.Collections.IEnumerable list)
                    foreach (Dictionary<string, object> r in list)
                    {
                        var req = new StudioRequest { Id = Convert.ToInt32(r["id"]), Key = Str(r["key"]), Ends = DateTime.UtcNow.AddSeconds(60) };
                        studioRequests[req.Id] = req;
                        log("Studio: looking for map " + req.Key + " on the other players' PCs");
                        AskWave(req);
                    }
            }
            catch (Exception ex) { log("Studio: bad request list: " + ex.Message); }
            var now = DateTime.UtcNow;
            foreach (var req in studioRequests.Values.ToList())
            {
                bool waveOver = now > req.WaveEnds || req.Asked.All(p => req.Answered.Contains(p));
                if (!waveOver)
                    continue;
                if (req.Wave == 1 && now < req.Ends)
                {
                    req.Wave = 2;
                    if (AskWave(req))
                        continue;
                }
                studioRequests.Remove(req.Id);
                log("Studio: nobody online has map " + req.Key);
                StudioResult("{\"id\":" + req.Id + ",\"ok\":false,\"error\":\"No player online has this map.\"}");
            }
            foreach (var k in partials.Where(x => (now - x.Value.Started).TotalSeconds > 120).Select(x => x.Key).ToList())
                partials.Remove(k);
        }

        // wave 1: the PCs likely to have it; wave 2: everyone else. False when nobody was left to ask.
        bool AskWave(StudioRequest req)
        {
            if (req.Wave == 0)
                req.Wave = 1;
            var who = new List<ulong>();
            if (loopback)
                who.Add(steam.MySteamID);
            else if (req.Wave == 1)
            {
                who.AddRange(hosts.Values.Where(h => HostHasMap(h, req.Key) || h.Joined).Select(h => h.SteamID));
                who.AddRange(remotes.Keys);
            }
            else
                who.AddRange(hosts.Keys);
            who = who.Distinct().Where(p => !req.Asked.Contains(p)).ToList();
            if (who.Count == 0)
            {
                req.WaveEnds = DateTime.MinValue;
                return false;
            }
            var ask = "{\"t\":\"get\",\"q\":" + req.Id + ",\"key\":" + json.Serialize(req.Key) + "}";
            foreach (var p in who)
            {
                req.Asked.Add(p);
                SendStudio(p, ask);
            }
            req.WaveEnds = DateTime.UtcNow.AddSeconds(15);
            Detail("Studio: asked " + who.Count + " PC(s) for map " + req.Key + " (round " + req.Wave + ")");
            return true;
        }

        void SendStudio(ulong peer, string text)
        {
            var data = Gzip(Encoding.UTF8.GetBytes(text));
            const int Piece = 400000;
            int parts = Math.Max(1, (data.Length + Piece - 1) / Piece);
            uint id = (uint)Interlocked.Increment(ref nextStudioMessage);
            for (int i = 0; i < parts; i++)
            {
                int n = Math.Min(Piece, data.Length - i * Piece);
                var msg = new byte[9 + n];
                msg[0] = Studio;
                BitConverter.GetBytes(id).CopyTo(msg, 1);
                BitConverter.GetBytes((ushort)i).CopyTo(msg, 5);
                BitConverter.GetBytes((ushort)parts).CopyTo(msg, 7);
                Buffer.BlockCopy(data, i * Piece, msg, 9, n);
                if (loopback)
                    loop.Enqueue(Tuple.Create(steam.MySteamID, msg));
                else
                    lock (Steam.Lock)
                    {
                        Native.SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(steam.Networking, peer);
                        SendRaw(peer, msg);
                    }
            }
        }

        void HandleStudio(ulong peer, byte[] msg)
        {
            if (msg.Length < 9)
                return;
            uint id = BitConverter.ToUInt32(msg, 1);
            int part = BitConverter.ToUInt16(msg, 5), parts = BitConverter.ToUInt16(msg, 7);
            if (parts == 0 || part >= parts || parts > 200)
                return;
            string key = peer + ":" + id;
            if (!partials.TryGetValue(key, out var pm))
                partials[key] = pm = new Partial { Parts = new byte[parts][], Started = DateTime.UtcNow };
            if (pm.Parts.Length != parts || pm.Parts[part] != null)
                return;
            pm.Parts[part] = msg.Skip(9).ToArray();
            if (++pm.Got < parts)
                return;
            partials.Remove(key);
            string text;
            try
            {
                var all = pm.Parts.SelectMany(x => x).ToArray();
                text = Encoding.UTF8.GetString(Gunzip(all, 0));
            }
            catch (Exception ex)
            {
                log("Studio: bad message from " + peer + ": " + ex.Message);
                return;
            }
            var head = text.Length > 300 ? text.Substring(0, 300) : text;
            var t = System.Text.RegularExpressions.Regex.Match(head, "^\\{\"t\":\"(\\w+)\",\"q\":(\\d+)");
            if (!t.Success)
                return;
            int q = int.Parse(t.Groups[2].Value);
            string from = hosts.TryGetValue(peer, out var fh) ? fh.Name : peer.ToString();
            if (t.Groups[1].Value == "get")
            {
                // another PC wants a map: this PC's server gives it (or says it hasn't got it)
                string wanted;
                try { wanted = Str(json.Deserialize<Dictionary<string, object>>(text)["key"]); }
                catch { return; }
                new Thread(() =>
                {
                    string answer;
                    try { answer = studioHttp.GetStringAsync(apiUrl + "/studio/net/export?key=" + Uri.EscapeDataString(wanted)).GetAwaiter().GetResult(); }
                    catch (Exception ex) { answer = "{\"ok\":false,\"error\":" + json.Serialize("this PC's server: " + ex.Message) + "}"; }
                    if (!answer.StartsWith("{"))
                        answer = "{\"ok\":false}";
                    bool ok = answer.StartsWith("{\"ok\":true");
                    if (ok)
                        log("Studio: sending map " + wanted + " to " + from + " (" + answer.Length / 1024 + " KB)");
                    SendStudio(peer, "{\"t\":\"map\",\"q\":" + q + "," + answer.Substring(1));
                }) { IsBackground = true }.Start();
                return;
            }
            if (t.Groups[1].Value != "map" || !studioRequests.TryGetValue(q, out var req) || !req.Asked.Contains(peer))
                return;
            req.Answered.Add(peer);
            if (!head.Contains("\"ok\":true"))
                return;
            // the map: to this PC's server, with who sent it
            studioRequests.Remove(q);
            int okAt = text.IndexOf("\"ok\":true", StringComparison.Ordinal);
            log("Studio: got map " + req.Key + " from " + from + " (" + text.Length / 1024 + " KB)");
            StudioResult("{\"id\":" + q + ",\"from\":" + json.Serialize(from) + "," + text.Substring(okAt));
        }

        // (on its own thread: callers hold Steam.Lock)
        void StudioResult(string body)
        {
            new Thread(() =>
            {
                try { studioHttp.PostAsync(apiUrl + "/studio/net/result", new StringContent(body, Encoding.UTF8, "application/json")).GetAwaiter().GetResult(); }
                catch (Exception ex) { log("Studio: couldn't hand the result to this PC's server: " + ex.Message); }
            }) { IsBackground = true }.Start();
        }

        // ------------------------------------------------------------ match traffic (UDP)
        delegate void UdpReceived(int portID, IPEndPoint from, byte[] data, int n);

        // two UDP sockets on 127.0.0.1 (portID 0 and 1) with a receive thread each
        UdpClient[] OpenUdp(int port0, int port1, UdpReceived received)
        {
            var socks = new UdpClient[2];
            int[] ports = { port0, port1 };
            for (int i = 0; i < 2; i++)
            {
                try
                {
                    var u = new UdpClient(new IPEndPoint(IPAddress.Loopback, ports[i]));
                    u.Client.IOControl(-1744830452, new byte[] { 0 }, null); // SIO_UDP_CONNRESET off: an ICMP reply doesn't end the socket
                    socks[i] = u;
                    int portID = i;
                    new Thread(() =>
                    {
                        var from = new IPEndPoint(IPAddress.Any, 0);
                        while (true)
                        {
                            byte[] data;
                            try { data = u.Receive(ref from); }
                            catch (ObjectDisposedException) { return; }
                            catch (SocketException) { continue; }
                            received(portID, from, data, data.Length);
                        }
                    }) { IsBackground = true }.Start();
                }
                catch (Exception ex) { log("UDP port " + ports[i] + " is busy: " + ex.Message); }
            }
            return socks;
        }

        static void CloseUdp(UdpClient[] socks)
        {
            if (socks != null)
                foreach (var u in socks)
                    try { u?.Close(); } catch { }
        }

        int MyPortID(int port)
        {
            int id = port == myPort0 && port != 0 ? 0 : port == myPort1 && port != 0 ? 1 : -1;
            if (id < 0)
            {
                if (port == 0)
                    return -1;
                if (seenPort1 != port)
                    UdpNote("this PC's game sends from port " + port + ", not one of its holepunch ports (" + myPort0 + ", " + myPort1 + "): used as its client port");
                id = 1;
            }
            if (id == 0)
                seenPort0 = port;
            else
                seenPort1 = port;
            return id;
        }
        int ReplyPort(int portID) => portID == 0 ? (seenPort0 != 0 ? seenPort0 : myPort0) : (seenPort1 != 0 ? seenPort1 : myPort1);

        void SendUdp(ulong peer, byte direction, int hostPortID, int playerPortID, byte[] data, int n)
        {
            if (System.Threading.Interlocked.Increment(ref udpToPeer) == 1)
                UdpNote("first match packet from this PC's game to " + peer + " (" + (direction == 0 ? "to their room" : "to their game in this PC's room") + ", " + n + " bytes)");
            Detail("UDP out to " + peer + " (" + (direction == 0 ? "their room's host" : "their game in this PC's room") + "), host port " + hostPortID +
                ", player port " + playerPortID + ", " + n + " bytes: " + Peek(data, 0, n));
            var msg = new byte[4 + n];
            msg[0] = Udp;
            msg[1] = direction;
            msg[2] = (byte)hostPortID;
            msg[3] = (byte)playerPortID;
            Buffer.BlockCopy(data, 0, msg, 4, n);
            if (loopback)
            {
                udpLoop.Enqueue(Tuple.Create(peer == LoopHostSide ? steam.MySteamID : LoopHostSide, msg));
                return;
            }
            lock (Steam.Lock)
                Native.SteamAPI_ISteamNetworking_SendP2PPacket(steam.Networking, peer, msg, (uint)msg.Length,
                    msg.Length <= UnreliableMax ? 0 : Steam.SendReliable, 1);
        }
        readonly ConcurrentQueue<Tuple<ulong, byte[]>> udpLoop = new ConcurrentQueue<Tuple<ulong, byte[]>>();

        // a UDP packet from the other PC: to this PC's game, from the socket that stands for the other game
        void HandleUdp(ulong peer, byte[] buf, int size)
        {
            if (buf[0] != Udp)
                return;
            int direction = buf[1], hostPortID = buf[2] & 1, playerPortID = buf[3] & 1;
            UdpClient from;
            int to;
            if (System.Threading.Interlocked.Increment(ref udpFromPeer) == 1)
                UdpNote("first match packet from " + peer + " (" + (direction == 1 ? "their game hosts the room this PC joined" : "their game joined this PC's room") + ", " + (size - 4) + " bytes)");
            if (direction == 1)
            {
                // the host PC's game -> this PC's game (this PC joined that host)
                var h = hosts.Values.FirstOrDefault(x => x.SteamID == (loopback ? steam.MySteamID : peer));
                if (h?.Udp == null)
                {
                    Dropped("a packet from host " + peer + ", whose lobby this PC doesn't know (no 31000+ ports for it)");
                    return;
                }
                from = h.Udp[hostPortID];
                to = ReplyPort(playerPortID);
            }
            else
            {
                // a remote player's game -> this PC's game (the room host)
                if (!remotes.TryGetValue(loopback ? steam.MySteamID : peer, out var r) || r.Udp == null)
                {
                    Dropped("a packet from player " + peer + ", who isn't on this PC's server (no 33000+ ports for them)");
                    return;
                }
                from = r.Udp[playerPortID];
                to = ReplyPort(hostPortID);
            }
            if (from == null || to == 0)
            {
                Dropped(from == null ? "a packet for a port this PC couldn't open" : "a packet: this PC's game ports are unknown (no holepunch to this PC's server yet)");
                return;
            }
            Detail("UDP in from " + peer + " (" + (direction == 1 ? "the host of the room this PC joined" : "a player in this PC's room") + "), host port " + hostPortID +
                ", player port " + playerPortID + ", " + (size - 4) + " bytes -> 127.0.0.1:" + to + " from :" + ((IPEndPoint)from.Client.LocalEndPoint).Port + ": " + Peek(buf, 4, size - 4));
            try { from.Send(buf.Skip(4).Take(size - 4).ToArray(), size - 4, new IPEndPoint(IPAddress.Loopback, to)); }
            catch (Exception ex) { Dropped("a packet to 127.0.0.1:" + to + ": " + ex.Message); }
        }

        void Dropped(string what)
        {
            System.Threading.Interlocked.Increment(ref udpDropped);
            UdpNote("dropped " + what);
        }

        // text: GET /api/udp's answer (ServerLoop): this PC's game ports and the remote players on this PC's server
        void ReadUdpState(string text)
        {
            var now = DateTime.UtcNow;
            if (now >= nextUdpReport && (udpToPeer != reportedTo || udpFromPeer != reportedFrom || udpDropped != reportedDropped))
            {
                nextUdpReport = now.AddSeconds(Verbose ? 5 : 15);
                reportedTo = udpToPeer;
                reportedFrom = udpFromPeer;
                reportedDropped = udpDropped;
                log("UDP: " + udpToPeer + " match packets sent to the other PC, " + udpFromPeer + " received, " + udpDropped + " dropped");
            }
            if (text == null)
                return;
            Dictionary<string, object> state;
            try { state = json.Deserialize<Dictionary<string, object>>(text); }
            catch { return; }
            if (state["myPorts"] is System.Collections.IList ports && ports.Count == 2)
            {
                int p0 = Convert.ToInt32(ports[0]), p1 = Convert.ToInt32(ports[1]);
                if (p0 != myPort0 || p1 != myPort1)
                    log("This PC's game uses UDP ports " + p0 + " (server) and " + p1 + " (client)");
                myPort0 = p0;
                myPort1 = p1;
            }
            var seen = new HashSet<ulong>();
            if (state["remotes"] is System.Collections.IEnumerable list)
                foreach (Dictionary<string, object> r in list)
                {
                    if (!ulong.TryParse(r["steamID"].ToString(), out ulong id))
                        continue;
                    int slot = Convert.ToInt32(r["slot"]);
                    seen.Add(id);
                    if (remotes.TryGetValue(id, out var old) && old.Slot == slot)
                        continue;
                    if (old != null)
                        CloseUdp(old.Udp);
                    var remote = new Remote { SteamID = id, Slot = slot };
                    remote.Udp = OpenUdp(HostUdpBase + slot * 2, HostUdpBase + slot * 2 + 1, (portID, from, data, n) =>
                    {
                        // this PC's game (the room host) -> the remote player's game
                        int mine = MyPortID(from.Port);
                        if (mine >= 0)
                            SendUdp(loopback ? steam.MySteamID : id, 1, mine, portID, data, n);
                        else
                            Dropped("a packet from 127.0.0.1:" + from.Port + " to " + (HostUdpBase + slot * 2 + portID));
                    });
                    remotes[id] = remote;
                    log("Player " + id + " plays on this PC's server: their match traffic goes through 127.0.0.1:" + (HostUdpBase + slot * 2) + "-" + (HostUdpBase + slot * 2 + 1));
                }
            foreach (var gone in remotes.Keys.Where(k => !seen.Contains(k)).ToList())
            {
                CloseUdp(remotes[gone].Udp);
                remotes.Remove(gone);
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
            {
                try { h.Listener.Stop(); } catch { }
                CloseUdp(h.Udp);
            }
            foreach (var r in remotes.Values)
                CloseUdp(r.Udp);
            lock (Steam.Lock)
            {
                if (myLobby != 0)
                    Native.SteamAPI_ISteamMatchmaking_LeaveLobby(steam.Matchmaking, myLobby);
                foreach (var h in hosts.Values.Where(h => h.Joined))
                    Native.SteamAPI_ISteamMatchmaking_LeaveLobby(steam.Matchmaking, h.Lobby);
            }
            foreach (var f in new[] { "RemoteRooms.json", "SteamIdentity.json", "StudioNet.json" })
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
                Console.WriteLine("CSNZ-SteamBridge.exe --steam-api <game Bin\\steam_api.dll> --data <server Data folder> [--api http://127.0.0.1:30080] [--log file] [--loopback] [--verbose]");
                return 2;
            }
            // Spacewar: Steam's free test app, so every player can use lobbies and P2P
            Directory.SetCurrentDirectory(exeDir);
            File.WriteAllText(Path.Combine(exeDir, "steam_appid.txt"), "480");

            Log("Steam bridge starting" + (args.Contains("--loopback") ? " (loopback test)" : "") + (args.Contains("--verbose") ? " (detailed logs)" : ""));
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
            var bridge = new Bridge(steam, dataDir, Arg("--api", "http://127.0.0.1:30080"), args.Contains("--loopback"), Log) { Verbose = args.Contains("--verbose") };
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
