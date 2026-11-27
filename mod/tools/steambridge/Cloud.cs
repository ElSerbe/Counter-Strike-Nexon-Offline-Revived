// Online play without Steam: the online lobby on Cloudflare (mod/tools/lobby-worker/worker.js) and direct UDP links.
//  - One WebSocket to the lobby: this PC's id is proved with a secret kept in Data/CloudIdentity.json; this PC's rooms are
//    published there, the other players' come pushed back; reliable messages (the game's TCP connections, profiles,
//    Studio maps) go through the lobby's relay.
//  - Direct links for the match traffic: one UDP socket (IPv4 + IPv6), its public address from STUN (Cloudflare's and
//    Google's public servers), addresses swapped through the lobby ("sig"), then both PCs send probes to each other's
//    addresses at once (UDP hole punching). When no probe gets through (both behind strict NATs), the match traffic
//    goes through the relay too.
// Direct packets: ['C']['Z'][kind u8][link key u64 LE][payload]; kind 1 probe, 2 probe answer, 3 data, 4 keep-alive. The
// link key is both PCs' random link tokens XORed: only the two PCs know it (the tokens went through the lobby over TLS).
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace SteamBridge
{
    class CloudHost
    {
        public ulong ID;
        public string Name = "", Version = "";
        public Dictionary<string, string> Data = new Dictionary<string, string>();
    }

    class CloudNet
    {
        public const int Proto = 1;
        const int MaxDirect = 1200;

        readonly string url;
        public readonly ulong MyID;
        readonly string secret;
        readonly string version;
        readonly Action<string> log, detail;
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };

        // messages from other PCs (relay and direct), for the bridge's loop
        public readonly ConcurrentQueue<Tuple<ulong, byte[]>> Inbox = new ConcurrentQueue<Tuple<ulong, byte[]>>();

        // the other players online who publish (the room list); Changed counts the changes
        readonly object hostLock = new object();
        readonly Dictionary<ulong, CloudHost> hosts = new Dictionary<ulong, CloudHost>();
        public int Changed;
        public volatile bool Connected;
        public string Denied; // why the lobby refused this PC, when it did

        public CloudNet(string url, ulong myID, string secret, string version, Action<string> log, Action<string> detail)
        {
            this.url = url;
            MyID = myID;
            this.secret = secret;
            this.version = version;
            this.log = log;
            this.detail = detail;
        }

        // ------------------------------------------------------------ this PC's id
        // Players without Steam get a random id with the top byte 0x02: never a Steam ID (individual Steam IDs start 0x0110)
        public static void LoadIdentity(string dataDir, ulong steamID, out ulong id, out string secret, Action<string> log)
        {
            var path = Path.Combine(dataDir, "CloudIdentity.json");
            var js = new JavaScriptSerializer();
            ulong saved = 0;
            secret = null;
            try
            {
                if (File.Exists(path))
                {
                    var d = js.Deserialize<Dictionary<string, object>>(File.ReadAllText(path));
                    ulong.TryParse(d.TryGetValue("id", out var i) ? i?.ToString() : "", out saved);
                    secret = d.TryGetValue("secret", out var s) ? s?.ToString() : null;
                }
            }
            catch { }
            if (saved == 0 || string.IsNullOrEmpty(secret) || secret.Length < 32)
            {
                if (saved == 0)
                    saved = (RandomU64() & 0x00FFFFFFFFFFFFFFUL) | 0x0200000000000000UL;
                if (string.IsNullOrEmpty(secret) || secret.Length < 32)
                    secret = RandomHex(32);
                try
                {
                    File.WriteAllText(path, js.Serialize(new Dictionary<string, object> {
                        { "id", saved.ToString() }, { "secret", secret },
                        { "note", "This PC's id in the online lobby (used when Steam isn't open) and the secret that proves it. Keep it private." } }));
                }
                catch (Exception ex) { log("Couldn't save the online id: " + ex.Message); }
            }
            // with Steam open the Steam ID is the id (this PC's server knows it by that), proved by the same secret
            id = steamID != 0 ? steamID : saved;
        }

        static string RandomHex(int bytes)
        {
            var b = new byte[bytes];
            using (var rng = RandomNumberGenerator.Create())
                rng.GetBytes(b);
            return BitConverter.ToString(b).Replace("-", "").ToLowerInvariant();
        }

        static ulong RandomU64()
        {
            var b = new byte[8];
            using (var rng = RandomNumberGenerator.Create())
                rng.GetBytes(b);
            return BitConverter.ToUInt64(b, 0) | 1;
        }

        // ------------------------------------------------------------ the lobby connection
        ClientWebSocket ws;
        readonly BlockingCollection<Tuple<byte[], bool>> sendQueue = new BlockingCollection<Tuple<byte[], bool>>(); // (bytes, text?)
        Func<bool> keepRunning;
        Dictionary<string, string> published; // what this PC publishes (re-sent after a reconnect or "repub")
        string publishedText;

        public void Start(Func<bool> keepRunning)
        {
            this.keepRunning = keepRunning;
            try { ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12; } catch { }
            if (!NoDirect)
                OpenUdp();
            new Thread(ConnectLoop) { IsBackground = true, Name = "cloud" }.Start();
            new Thread(SendLoop) { IsBackground = true, Name = "cloud-send" }.Start();
            new Thread(TickLoop) { IsBackground = true, Name = "cloud-tick" }.Start();
        }

        void ConnectLoop()
        {
            int wait = 5;
            bool everConnected = false;
            while (keepRunning())
            {
                var started = DateTime.UtcNow;
                try
                {
                    var c = new ClientWebSocket();
                    c.Options.KeepAliveInterval = TimeSpan.FromSeconds(20);
                    c.ConnectAsync(new Uri(url), CancellationToken.None).GetAwaiter().GetResult();
                    ws = c;
                    QueueText(json.Serialize(new Dictionary<string, object> {
                        { "t", "hello" }, { "id", MyID.ToString() }, { "secret", secret }, { "name", MyName ?? "" }, { "ver", version }, { "proto", Proto } }));
                    ReceiveLoop(c);
                }
                catch (Exception ex)
                {
                    if (!everConnected && wait == 5)
                        log("Online lobby: couldn't connect (" + (ex.InnerException ?? ex).Message + "). Trying again in the background.");
                    else
                        detail("Online lobby: " + (ex.InnerException ?? ex).Message);
                }
                bool was = Connected;
                Connected = false;
                ws = null;
                lock (hostLock)
                {
                    if (hosts.Count > 0)
                    {
                        hosts.Clear();
                        Changed++;
                    }
                }
                if (was)
                {
                    everConnected = true;
                    log("Online lobby: connection lost, connecting again.");
                }
                if (Denied != null)
                    wait = 300; // refused (another PC uses this id, an old protocol): try again rarely
                else if ((DateTime.UtcNow - started).TotalSeconds > 60)
                    wait = 5;
                else
                    wait = Math.Min(wait * 2, 60);
                for (int i = 0; i < wait * 10 && keepRunning(); i++)
                    Thread.Sleep(100);
            }
            try { ws?.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None).Wait(1000); } catch { }
        }

        void ReceiveLoop(ClientWebSocket c)
        {
            var buf = new byte[65536];
            var msg = new MemoryStream();
            while (keepRunning() && c.State == WebSocketState.Open)
            {
                var r = c.ReceiveAsync(new ArraySegment<byte>(buf), CancellationToken.None).GetAwaiter().GetResult();
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    if (c.CloseStatus.HasValue && (int)c.CloseStatus.Value >= 4000)
                        detail("Online lobby closed the connection: " + c.CloseStatusDescription);
                    return;
                }
                msg.Write(buf, 0, r.Count);
                if (!r.EndOfMessage)
                    continue;
                var data = msg.ToArray();
                msg.SetLength(0);
                if (r.MessageType == WebSocketMessageType.Binary)
                {
                    if (data.Length > 8)
                    {
                        ulong from = BitConverter.ToUInt64(data, 0);
                        var m = new byte[data.Length - 8];
                        Buffer.BlockCopy(data, 8, m, 0, m.Length);
                        relayIn++;
                        Inbox.Enqueue(Tuple.Create(from, m));
                    }
                    continue;
                }
                var text = Encoding.UTF8.GetString(data);
                if (text == "pong")
                    continue;
                try { HandleText(text); }
                catch (Exception ex) { log("Online lobby: bad message (" + ex.Message + ")"); }
            }
        }

        void HandleText(string text)
        {
            var m = json.Deserialize<Dictionary<string, object>>(text);
            string t = m.TryGetValue("t", out var tv) ? tv?.ToString() : "";
            switch (t)
            {
                case "welcome":
                    Connected = true;
                    Denied = null;
                    PublicIP = m.TryGetValue("ip", out var ip) ? ip?.ToString() ?? "" : "";
                    log("Online lobby: connected as " + MyID + ". Rooms of players without Steam show up too.");
                    if (published != null)
                        QueuePub(true);
                    break;
                case "denied":
                    Denied = m.TryGetValue("why", out var why) ? why?.ToString() : "refused";
                    log("Online lobby refused this PC: " + Denied);
                    break;
                case "list":
                    lock (hostLock)
                    {
                        hosts.Clear();
                        if (m["hosts"] is System.Collections.IEnumerable list)
                            foreach (Dictionary<string, object> h in list)
                                AddHost(h);
                        Changed++;
                    }
                    break;
                case "host":
                    lock (hostLock)
                    {
                        AddHost(m);
                        Changed++;
                    }
                    break;
                case "gone":
                    if (ulong.TryParse(m["id"]?.ToString(), out ulong gone))
                        lock (hostLock)
                        {
                            hosts.Remove(gone);
                            Changed++;
                        }
                    break;
                case "repub":
                    QueuePub(true);
                    break;
                case "sig":
                    if (ulong.TryParse(m["from"]?.ToString(), out ulong from) && m["body"] is Dictionary<string, object> body)
                        OnSignal(from, m.TryGetValue("ip", out var sip) ? sip?.ToString() : "", body);
                    break;
                case "nopeer":
                    detail("Online lobby: player " + m["id"] + " isn't connected");
                    break;
            }
        }

        void AddHost(Dictionary<string, object> h)
        {
            if (!ulong.TryParse(h["id"]?.ToString(), out ulong id) || id == MyID)
                return;
            var host = new CloudHost { ID = id, Name = h["name"]?.ToString() ?? "", Version = h["ver"]?.ToString() ?? "" };
            if (h.TryGetValue("data", out var d) && d is Dictionary<string, object> data)
                foreach (var kv in data)
                    host.Data[kv.Key] = kv.Value?.ToString() ?? "";
            hosts[id] = host;
        }

        public List<CloudHost> Hosts()
        {
            lock (hostLock)
                return hosts.Values.ToList();
        }

        public string MyName;
        public bool NoDirect; // --no-direct: everything through the relay (tests)

        // what this PC publishes in the lobby (sent when it changes)
        public void Publish(Dictionary<string, string> data)
        {
            var text = json.Serialize(data);
            if (text == publishedText)
                return;
            published = new Dictionary<string, string>(data);
            publishedText = text;
            QueuePub(false);
        }

        void QueuePub(bool force)
        {
            if (!Connected || published == null)
                return;
            QueueText(json.Serialize(new Dictionary<string, object> { { "t", "pub" }, { "data", published } }));
        }

        void QueueText(string text) => sendQueue.Add(Tuple.Create(Encoding.UTF8.GetBytes(text), true));

        void SendLoop()
        {
            var nextPing = DateTime.UtcNow.AddSeconds(30);
            while (true)
            {
                if (!sendQueue.TryTake(out var item, 1000))
                {
                    if (DateTime.UtcNow >= nextPing && Connected)
                    {
                        nextPing = DateTime.UtcNow.AddSeconds(30);
                        QueueText("ping");
                    }
                    continue;
                }
                var c = ws;
                if (c == null || c.State != WebSocketState.Open)
                    continue; // dropped: the bridge's messages are re-sent by their own timeouts
                try
                {
                    c.SendAsync(new ArraySegment<byte>(item.Item1), item.Item2 ? WebSocketMessageType.Text : WebSocketMessageType.Binary, true, CancellationToken.None).GetAwaiter().GetResult();
                }
                catch (Exception ex) { detail("Online lobby: send failed (" + ex.Message + ")"); }
            }
        }

        // ------------------------------------------------------------ sending to another PC
        long relayOut, relayIn, directOut, directIn, relayDropped;

        // reliable: through the relay (ordered, never lost while connected). Otherwise straight to their PC when a direct
        // link is open, else through the relay too (dropped when the relay backs up: match packets are useless late)
        public bool Send(ulong peer, byte[] msg, bool reliable)
        {
            if (!reliable && msg.Length <= MaxDirect)
            {
                Link l;
                lock (links)
                    links.TryGetValue(peer, out l);
                if (l != null && l.Confirmed && l.Remote != null)
                {
                    SendDirect(l, 3, msg, msg.Length);
                    directOut++;
                    return true;
                }
                if (sendQueue.Count > 500)
                {
                    relayDropped++;
                    return false;
                }
            }
            if (!Connected)
                return false;
            var b = new byte[8 + msg.Length];
            BitConverter.GetBytes(peer).CopyTo(b, 0);
            Buffer.BlockCopy(msg, 0, b, 8, msg.Length);
            sendQueue.Add(Tuple.Create(b, false));
            relayOut++;
            return true;
        }

        public string Route(ulong peer)
        {
            Link l;
            lock (links)
                links.TryGetValue(peer, out l);
            if (l != null && l.Confirmed)
                return "direct";
            if (!Connected)
                return "";
            return l != null && DateTime.UtcNow < l.PunchUntil ? "connecting" : "relay";
        }

        public string State(ulong peer, string name)
        {
            Link l;
            lock (links)
                links.TryGetValue(peer, out l);
            string how = l != null && l.Confirmed ? "direct (" + Kind(l.Remote) + ")" :
                !Connected ? "the online lobby isn't connected" :
                l != null && DateTime.UtcNow < l.PunchUntil ? "through the online lobby's relay (a direct link is being tried)" :
                "through the online lobby's relay";
            return "Online connection to " + name + ": " + how + ", " + sendQueue.Count + " messages waiting to be sent from this PC";
        }

        public string Totals() => "online: " + directOut + " direct packets sent, " + directIn + " received; " + relayOut + " relay messages sent, " +
            relayIn + " received, " + relayDropped + " match packets dropped (relay backed up)";

        // ------------------------------------------------------------ direct links (UDP hole punching)
        class Link
        {
            public ulong Peer;
            public ulong MyTok = RandomU64(), TheirTok, Key;
            public List<IPEndPoint> Cands = new List<IPEndPoint>();
            public IPEndPoint Remote;
            public bool Confirmed;
            public DateTime PunchUntil, NextProbe, LastRecv, LastKeepAlive, NextOffer, WantedUntil;
            public int Tries;
            public bool LostLogged;
        }
        readonly Dictionary<ulong, Link> links = new Dictionary<ulong, Link>();
        readonly Dictionary<ulong, Link> byKey = new Dictionary<ulong, Link>();
        public Func<ulong, string> NameOf = id => id.ToString();

        Socket udp;
        int udpPort;
        DateTime udpOpened;
        public string PublicIP = "";
        IPEndPoint stunMapped;
        DateTime nextStun = DateTime.MinValue;
        readonly Dictionary<string, IPEndPoint> stunServers = new Dictionary<string, IPEndPoint>();
        byte[] stunTx;

        void OpenUdp()
        {
            try
            {
                try
                {
                    udp = new Socket(AddressFamily.InterNetworkV6, SocketType.Dgram, ProtocolType.Udp) { DualMode = true };
                    udp.Bind(new IPEndPoint(IPAddress.IPv6Any, 0));
                }
                catch
                {
                    udp = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                    udp.Bind(new IPEndPoint(IPAddress.Any, 0));
                }
                try { udp.IOControl(-1744830452, new byte[] { 0 }, null); } catch { } // SIO_UDP_CONNRESET off
                udpPort = ((IPEndPoint)udp.LocalEndPoint).Port;
                udpOpened = DateTime.UtcNow;
                new Thread(UdpReceiveLoop) { IsBackground = true, Name = "cloud-udp" }.Start();
                new Thread(AddressLoop) { IsBackground = true, Name = "cloud-addresses" }.Start();
                detail("Direct links: UDP port " + udpPort);
            }
            catch (Exception ex)
            {
                udp = null;
                log("Direct links are off this session (no UDP socket: " + ex.Message + "): match traffic goes through the online lobby's relay.");
            }
        }

        bool V6 => udp != null && udp.AddressFamily == AddressFamily.InterNetworkV6;

        static IPAddress Norm(IPAddress a) => a.IsIPv4MappedToIPv6 ? a.MapToIPv4() : a;
        // what kind of address, for the logs (players post their logs in public bug reports: no IP addresses there)
        static string Kind(IPEndPoint e)
        {
            if (e == null)
                return "?";
            var a = e.Address;
            if (IPAddress.IsLoopback(a))
                return "this PC";
            if (a.AddressFamily == AddressFamily.InterNetworkV6)
                return (a.GetAddressBytes()[0] & 0xFE) == 0xFC || a.IsIPv6LinkLocal ? "local network, IPv6" : "internet, IPv6";
            var b = a.GetAddressBytes();
            bool local = b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
            bool vpn = (b[0] == 100 && (b[1] & 0xC0) == 64) || b[0] == 26 || b[0] == 25; // CGNAT / Tailscale, Radmin, Hamachi
            return local ? "local network" : vpn ? "VPN or carrier network" : "internet";
        }
        static string Kinds(IEnumerable<IPEndPoint> list) => string.Join(", ", list.GroupBy(Kind).Select(g => g.Count() + " " + g.Key));

        static string Show(IPEndPoint e) => e == null ? "?" : (e.Address.AddressFamily == AddressFamily.InterNetworkV6 ? "[" + e.Address + "]" : e.Address.ToString()) + ":" + e.Port;

        void SendTo(byte[] b, int n, IPEndPoint to)
        {
            if (udp == null)
                return;
            try
            {
                var addr = to.Address;
                if (V6 && addr.AddressFamily == AddressFamily.InterNetwork)
                    addr = addr.MapToIPv6();
                else if (!V6 && addr.AddressFamily == AddressFamily.InterNetworkV6)
                {
                    if (!addr.IsIPv4MappedToIPv6)
                        return;
                    addr = addr.MapToIPv4();
                }
                udp.SendTo(b, n, SocketFlags.None, new IPEndPoint(addr, to.Port));
            }
            catch { }
        }

        void SendDirect(Link l, byte kind, byte[] payload, int n, IPEndPoint to = null)
        {
            var b = new byte[11 + n];
            b[0] = (byte)'C';
            b[1] = (byte)'Z';
            b[2] = kind;
            BitConverter.GetBytes(l.Key).CopyTo(b, 3);
            if (n > 0)
                Buffer.BlockCopy(payload, 0, b, 11, n);
            SendTo(b, b.Length, to ?? l.Remote);
        }

        void UdpReceiveLoop()
        {
            var buf = new byte[65536];
            while (true)
            {
                EndPoint from = V6 ? new IPEndPoint(IPAddress.IPv6Any, 0) : new IPEndPoint(IPAddress.Any, 0);
                int n;
                try { n = udp.ReceiveFrom(buf, ref from); }
                catch (ObjectDisposedException) { return; }
                catch (SocketException) { continue; }
                var src = (IPEndPoint)from;
                src = new IPEndPoint(Norm(src.Address), src.Port);
                try
                {
                    if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01 && buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42)
                        OnStun(buf, n);
                    else if (n >= 11 && buf[0] == 'C' && buf[1] == 'Z')
                        OnDirect(buf, n, src);
                }
                catch (Exception ex) { detail("Direct links: bad packet (" + Kind(src) + "): " + ex.Message); }
            }
        }

        void OnDirect(byte[] buf, int n, IPEndPoint src)
        {
            byte kind = buf[2];
            ulong key = BitConverter.ToUInt64(buf, 3);
            Link l;
            lock (links)
            {
                if (!byKey.TryGetValue(key, out l))
                    return;
                l.LastRecv = DateTime.UtcNow;
                l.LostLogged = false;
                if (kind == 1)
                    SendDirect(l, 2, null, 0, src);
                else if (kind == 2 || kind == 3)
                {
                    bool was = l.Confirmed;
                    if (!l.Confirmed || (l.Remote != null && !l.Remote.Equals(src) && kind == 3))
                        l.Remote = src;
                    l.Confirmed = true;
                    if (!was)
                        log("Direct link to " + NameOf(l.Peer) + " is open (" + Kind(src) + "): match traffic goes straight to their PC.");
                }
            }
            if (kind == 3 && n > 11)
            {
                var m = new byte[n - 11];
                Buffer.BlockCopy(buf, 11, m, 0, m.Length);
                directIn++;
                Inbox.Enqueue(Tuple.Create(l.Peer, m));
            }
        }

        // the bridge has business with this PC (a host listed, a player in this PC's room, profile mail): try a direct link
        public void Want(ulong peer)
        {
            if (udp == null || !Connected)
                return;
            var now = DateTime.UtcNow;
            if (stunMapped == null && (now - udpOpened).TotalSeconds < 4)
                return; // the public address first: an offer without it rarely gets through a NAT
            Link l;
            lock (links)
            {
                if (!links.TryGetValue(peer, out l))
                    links[peer] = l = new Link { Peer = peer };
                l.WantedUntil = now.AddSeconds(90);
                if (l.Confirmed || now < l.NextOffer)
                    return;
                // tried and failed: again after 1, 2, 4.. minutes (up to 10)
                l.NextOffer = now.AddSeconds(Math.Min(600, 60 * (1 << Math.Min(l.Tries, 4))));
                l.Tries++;
            }
            Signal(l, "offer");
        }

        void Signal(Link l, string kind)
        {
            var cands = MyCandidates();
            QueueText(json.Serialize(new Dictionary<string, object> {
                { "t", "sig" }, { "to", l.Peer.ToString() },
                { "body", new Dictionary<string, object> { { "k", kind }, { "tok", l.MyTok.ToString() }, { "cand", cands } } } }));
            detail("Direct links: " + kind + " to " + NameOf(l.Peer) + " with " + cands.Count + " addresses");
        }

        void OnSignal(ulong from, string ip, Dictionary<string, object> body)
        {
            string kind = body.TryGetValue("k", out var k) ? k?.ToString() : "";
            if (!ulong.TryParse(body.TryGetValue("tok", out var tk) ? tk?.ToString() : "", out ulong tok) || tok == 0)
                return;
            var cands = new List<IPEndPoint>();
            var ports = new HashSet<int>();
            if (body.TryGetValue("cand", out var cl) && cl is System.Collections.IEnumerable list)
                foreach (var c in list)
                {
                    var e = ParseEndPoint(c?.ToString());
                    if (e != null && cands.Count < 16)
                    {
                        cands.Add(e);
                        ports.Add(e.Port);
                    }
                }
            // the address the lobby saw them connect from, with their ports (a NAT that keeps the port)
            if (IPAddress.TryParse(ip ?? "", out var seen))
                foreach (var p in ports)
                {
                    var e = new IPEndPoint(Norm(seen), p);
                    if (!cands.Any(x => x.Equals(e)))
                        cands.Add(e);
                }
            Link l;
            bool answer;
            lock (links)
            {
                if (!links.TryGetValue(from, out l))
                    links[from] = l = new Link { Peer = from };
                if (l.TheirTok != tok)
                {
                    if (l.Key != 0)
                        byKey.Remove(l.Key);
                    l.TheirTok = tok;
                    l.Key = l.MyTok ^ tok;
                    byKey[l.Key] = l;
                    l.Confirmed = false;
                    l.Remote = null;
                }
                l.Cands = cands;
                l.PunchUntil = DateTime.UtcNow.AddSeconds(15);
                l.NextProbe = DateTime.MinValue;
                answer = kind == "offer";
            }
            detail("Direct links: " + kind + " from " + NameOf(from) + ": " + Kinds(cands));
            if (answer)
                Signal(l, "answer");
        }

        static IPEndPoint ParseEndPoint(string s)
        {
            if (string.IsNullOrEmpty(s))
                return null;
            int colon = s.LastIndexOf(':');
            if (colon <= 0 || !int.TryParse(s.Substring(colon + 1), out int port) || port <= 0 || port > 65535)
                return null;
            var host = s.Substring(0, colon).Trim('[', ']');
            if (!IPAddress.TryParse(host, out var a))
                return null;
            return new IPEndPoint(Norm(a), port);
        }

        // this PC's addresses for a direct link: the public one (STUN) and the network cards' (cached: listing the network
        // cards took 1-2 s on a player's PC, and done for each of 40 hosts it froze the bridge for a minute in v1.1.49)
        volatile List<string> localAddresses = new List<string>();
        List<string> MyCandidates()
        {
            var list = new List<string>();
            if (udp == null)
                return list;
            if (stunMapped != null)
                list.Add(Show(stunMapped));
            foreach (var a in localAddresses)
                if (!list.Contains(a))
                    list.Add(a);
            return list.Take(12).ToList();
        }

        void AddressLoop()
        {
            while (keepRunning())
            {
                var watch = System.Diagnostics.Stopwatch.StartNew();
                localAddresses = LocalAddresses();
                if (watch.Elapsed.TotalSeconds >= 3)
                    detail("Direct links: listing this PC's network cards took " + watch.Elapsed.TotalSeconds.ToString("0.0") + " s");
                for (int i = 0; i < 600 && keepRunning(); i++)
                    Thread.Sleep(100);
            }
        }

        List<string> LocalAddresses()
        {
            var list = new List<string>();
            if (udp == null)
                return list;
            try
            {
                foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                        continue;
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        var a = ua.Address;
                        if (a.AddressFamily == AddressFamily.InterNetwork)
                        {
                            if (a.ToString().StartsWith("169.254."))
                                continue;
                        }
                        else if (a.AddressFamily == AddressFamily.InterNetworkV6)
                        {
                            // global IPv6 only (2000::/3): IPv6 usually needs no NAT at all
                            if (!V6 || a.IsIPv6LinkLocal || a.IsIPv6SiteLocal || (a.GetAddressBytes()[0] & 0xE0) != 0x20)
                                continue;
                        }
                        else
                            continue;
                        var s = Show(new IPEndPoint(a, udpPort));
                        if (!list.Contains(s))
                            list.Add(s);
                    }
                }
            }
            catch { }
            return list.Take(12).ToList();
        }

        // ------------------------------------------------------------ STUN (this PC's public address)
        void Stun()
        {
            if (udp == null)
                return;
            if (stunServers.Count == 0)
                foreach (var name in new[] { "stun.cloudflare.com:3478", "stun.l.google.com:19302" })
                {
                    try
                    {
                        var parts = name.Split(':');
                        var addr = Dns.GetHostAddresses(parts[0]).FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork);
                        if (addr != null)
                            stunServers[name] = new IPEndPoint(addr, int.Parse(parts[1]));
                    }
                    catch (Exception ex) { detail("STUN: " + name + ": " + ex.Message); }
                }
            stunTx = new byte[12];
            using (var rng = RandomNumberGenerator.Create())
                rng.GetBytes(stunTx);
            var req = new byte[20];
            req[1] = 0x01; // Binding Request, no attributes
            req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;
            Buffer.BlockCopy(stunTx, 0, req, 8, 12);
            foreach (var s in stunServers.Values)
                SendTo(req, req.Length, s);
        }

        void OnStun(byte[] b, int n)
        {
            if (stunTx == null)
                return;
            for (int i = 0; i < 12; i++)
                if (b[8 + i] != stunTx[i])
                    return;
            int len = (b[2] << 8) | b[3];
            int at = 20;
            IPEndPoint found = null;
            while (at + 4 <= Math.Min(n, 20 + len))
            {
                int type = (b[at] << 8) | b[at + 1], alen = (b[at + 2] << 8) | b[at + 3];
                int v = at + 4;
                if (v + alen > n)
                    break;
                if ((type == 0x0020 || type == 0x0001) && alen >= 8 && b[v + 1] == 0x01)
                {
                    int port = (b[v + 2] << 8) | b[v + 3];
                    var ipb = new byte[] { b[v + 4], b[v + 5], b[v + 6], b[v + 7] };
                    if (type == 0x0020)
                    {
                        port ^= 0x2112;
                        ipb[0] ^= 0x21; ipb[1] ^= 0x12; ipb[2] ^= 0xA4; ipb[3] ^= 0x42;
                    }
                    found = new IPEndPoint(new IPAddress(ipb), port);
                    if (type == 0x0020)
                        break;
                }
                at = v + ((alen + 3) & ~3);
            }
            if (found == null)
                return;
            if (stunMapped == null || !stunMapped.Equals(found))
                log("Direct links: this PC's public address for match traffic is ready" + (found.Port == udpPort ? "" : " (the router changes the port)") + ".");
            stunMapped = found;
        }

        // ------------------------------------------------------------ probes, keep-alives
        void TickLoop()
        {
            while (keepRunning())
            {
                try { Tick(); }
                catch (Exception ex) { detail("Direct links: " + ex.Message); }
                Thread.Sleep(50);
            }
        }

        void Tick()
        {
            var now = DateTime.UtcNow;
            if (udp != null && now >= nextStun)
            {
                nextStun = now.AddSeconds(stunMapped == null ? ((now - udpOpened).TotalSeconds < 15 ? 1.5 : 10) : 60);
                Stun();
            }
            List<Link> all;
            lock (links)
                all = links.Values.ToList();
            foreach (var l in all)
            {
                if (l.Key == 0)
                    continue;
                if (!l.Confirmed && now < l.PunchUntil && now >= l.NextProbe)
                {
                    l.NextProbe = now.AddMilliseconds(250);
                    foreach (var c in l.Cands)
                        SendDirect(l, 1, null, 0, c);
                }
                if (l.Confirmed)
                {
                    if ((now - l.LastRecv).TotalSeconds > 30)
                    {
                        lock (links)
                        {
                            l.Confirmed = false;
                            l.Remote = null;
                            l.NextOffer = now;
                        }
                        // (a link nobody needs anymore just ends)
                        if (!l.LostLogged && now < l.WantedUntil)
                        {
                            l.LostLogged = true;
                            log("Direct link to " + NameOf(l.Peer) + " stopped answering: match traffic goes through the relay until it opens again.");
                        }
                    }
                    else if ((now - l.LastKeepAlive).TotalSeconds >= 10 && now < l.WantedUntil.AddMinutes(5))
                    {
                        l.LastKeepAlive = now;
                        SendDirect(l, 4, null, 0);
                    }
                }
            }
        }
    }
}
