// CSNZ Revived online lobby: online play without Steam (and alongside it), on Cloudflare Workers.
//
// Every player's bridge (mod/tools/steambridge, Cloud.cs) keeps one WebSocket to /ws. One Durable Object ("main") holds:
//  - who is online: an id (a Steam ID, or a random id for players without Steam) proved by a secret only that PC knows
//    (the first PC to use an id binds it; the binding lapses after 30 days unused);
//  - the room list: each bridge publishes its rooms, Studio catalog, name and version ("pub"), and every other bridge
//    gets the change pushed ("host"), the whole list when it connects ("list"), and "gone" when a player leaves;
//  - signalling ("sig"): two bridges swap their addresses through here to open a direct UDP link between their PCs;
//  - the relay: a binary message [to: u64 LE][payload] is passed to that player as [from: u64 LE][payload]. It carries
//    what can't go direct: the game's TCP connections (lobby, login), and the match traffic when no direct link opens.
// Nothing is kept but the id bindings: the room list lives in memory and the bridges publish again after a restart
// ("repub"). Players' IP addresses only go to the player they're connecting to (in "sig"), never into the room list.
//
// Text messages are JSON. Client -> server:
//   {"t":"hello","id":"<u64>","secret":"<hex>","name":..,"ver":..,"proto":1}
//   {"t":"pub","data":{"rooms":"..","studio":"..",...}}            (strings; replaces what was published)
//   {"t":"sig","to":"<u64>","body":{...}}
// Server -> client:
//   {"t":"welcome","ip":"<the client's public IP>"} | {"t":"denied","why":".."}
//   {"t":"list","hosts":[{"id","name","ver","data"}...]} | {"t":"host",...one of them} | {"t":"gone","id"} | {"t":"repub"}
//   {"t":"sig","from":"<u64>","ip":"<the sender's public IP>","body":{...}} | {"t":"nopeer","id"}
// The text "ping" is answered "pong" without waking the object (the bridges' keep-alive every 30 s).

// 2 since v1.1.53. Bridges on 1 (v1.1.49-1.1.52) PINGed every host through the relay every 5 s, and every one of those
// messages counts against the Durable Objects free tier (100,000 requests a day, an incoming WebSocket message being
// 1/20 of one): with a few dozen players the lobby ran out in a couple of hours and answered 500 for the rest of the day
// (2026-11-27). They're refused with a message to update (their launcher updates by itself within the hour).
const PROTO = 2;
// the relay passes no PING (8) or PONG (9): the room list's ping only goes over direct links
const RELAY_PING = 8, RELAY_PONG = 9;
const BINDING_DAYS = 30;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === "/ws") {
      if (req.headers.get("Upgrade") !== "websocket")
        return new Response("WebSocket expected", { status: 426 });
      return env.LOBBY.get(env.LOBBY.idFromName("main")).fetch(req);
    }
    if (url.pathname === "/status")
      return env.LOBBY.get(env.LOBBY.idFromName("main")).fetch(req);
    if (url.pathname === "/")
      return new Response("Counter-Strike Nexon: Revived online lobby. https://github.com/ElSerbe/Counter-Strike-Nexon-Revived\n",
        { headers: { "content-type": "text/plain; charset=utf-8" } });
    return new Response("Not found\n", { status: 404 });
  },
};

const U64 = /^[0-9]{1,20}$/;

async function sha256(text) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export class Lobby {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.peers = new Map(); // id -> { ws, id, name, ver, ip, data }
    // after the object slept (WebSocket hibernation) the sockets are still there, with who they are in their attachment;
    // what they published is gone, so they're asked to publish again with the first message
    for (const ws of ctx.getWebSockets()) {
      const a = ws.deserializeAttachment();
      if (a && a.id)
        this.peers.set(a.id, { ws, id: a.id, name: a.name, ver: a.ver, ip: a.ip, data: null });
    }
    this.repubPending = this.peers.size > 0;
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === "/status") {
      let hosting = 0;
      for (const p of this.peers.values())
        if (p.data) hosting++;
      return new Response(JSON.stringify({ online: this.peers.size, hosting }), { headers: { "content-type": "application/json" } });
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ ip: req.headers.get("CF-Connecting-IP") || "" });
    return new Response(null, { status: 101, webSocket: client });
  }

  send(ws, obj) {
    try { ws.send(JSON.stringify(obj)); } catch (e) { }
  }

  broadcast(obj, except) {
    const text = JSON.stringify(obj);
    for (const p of this.peers.values())
      if (p.ws !== except)
        try { p.ws.send(text); } catch (e) { }
  }

  listing(p) {
    return { id: p.id, name: p.name, ver: p.ver, data: p.data };
  }

  async webSocketMessage(ws, msg) {
    if (this.repubPending) {
      this.repubPending = false;
      this.broadcast({ t: "repub" });
    }
    const a = ws.deserializeAttachment() || {};
    if (typeof msg !== "string") {
      // the relay
      if (!a.id)
        return;
      const buf = new Uint8Array(msg);
      if (buf.length < 9 || buf[8] === RELAY_PING || buf[8] === RELAY_PONG)
        return;
      const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
      const to = view.getBigUint64(0, true).toString();
      const p = this.peers.get(to);
      if (!p) {
        this.send(ws, { t: "nopeer", id: to });
        return;
      }
      view.setBigUint64(0, BigInt(a.id), true);
      try { p.ws.send(buf); } catch (e) { }
      return;
    }
    let m;
    try { m = JSON.parse(msg); } catch (e) { return; }
    if (!m || typeof m.t !== "string")
      return;

    if (m.t === "hello") {
      const id = String(m.id || "");
      const secret = String(m.secret || "");
      if (!U64.test(id) || id === "0" || BigInt(id) > 0xffffffffffffffffn || secret.length < 32 || secret.length > 128) {
        this.send(ws, { t: "denied", why: "bad hello" });
        ws.close(4000, "bad hello");
        return;
      }
      if ((m.proto | 0) !== PROTO) {
        this.send(ws, { t: "denied", why: "This version of the mod uses another online protocol: update the mod (the launcher's Update)." });
        ws.close(4001, "protocol");
        return;
      }
      // the id belongs to the first PC that used it (its secret's hash), until it goes unused for 30 days
      const key = "id:" + id;
      const hash = await sha256(secret);
      const now = Date.now();
      const bound = await this.ctx.storage.get(key);
      if (bound && bound.hash !== hash && now - bound.seen < BINDING_DAYS * 86400000) {
        this.send(ws, { t: "denied", why: "This player id is in use by another PC." });
        ws.close(4003, "id in use");
        return;
      }
      if (!bound || bound.hash !== hash || now - bound.seen > 86400000)
        await this.ctx.storage.put(key, { hash, seen: now });
      // the same player again (a reconnect): the old socket goes
      const old = this.peers.get(id);
      if (old && old.ws !== ws)
        try { old.ws.close(4004, "replaced"); } catch (e) { }
      const name = String(m.name || "").slice(0, 64);
      const ver = String(m.ver || "").slice(0, 32);
      ws.serializeAttachment({ id, name, ver, ip: a.ip || "" });
      this.peers.set(id, { ws, id, name, ver, ip: a.ip || "", data: null });
      const hosts = [];
      for (const p of this.peers.values())
        if (p.id !== id && p.data)
          hosts.push(this.listing(p));
      this.send(ws, { t: "welcome", ip: a.ip || "" });
      this.send(ws, { t: "list", hosts });
      return;
    }

    if (!a.id)
      return;
    const me = this.peers.get(a.id);
    if (!me || me.ws !== ws)
      return;

    if (m.t === "pub") {
      const data = {};
      let size = 0;
      if (m.data && typeof m.data === "object")
        for (const [k, v] of Object.entries(m.data)) {
          const s = String(v);
          size += k.length + s.length;
          data[String(k).slice(0, 32)] = s;
        }
      if (size > 512 * 1024)
        return; // a room list and a Studio catalog are a few KB
      me.data = data;
      this.broadcast({ t: "host", ...this.listing(me) }, ws);
      return;
    }

    if (m.t === "sig") {
      const to = String(m.to || "");
      const p = this.peers.get(to);
      if (!p) {
        this.send(ws, { t: "nopeer", id: to });
        return;
      }
      const body = m.body && typeof m.body === "object" ? m.body : {};
      if (JSON.stringify(body).length > 4096)
        return;
      this.send(p.ws, { t: "sig", from: a.id, ip: me.ip, body });
      return;
    }
  }

  gone(ws) {
    const a = ws.deserializeAttachment() || {};
    if (!a.id)
      return;
    const p = this.peers.get(a.id);
    if (p && p.ws === ws) {
      this.peers.delete(a.id);
      this.broadcast({ t: "gone", id: a.id });
    }
  }

  async webSocketClose(ws, code, reason, wasClean) {
    this.gone(ws);
    try { ws.close(1000, "bye"); } catch (e) { }
  }

  async webSocketError(ws, error) {
    this.gone(ws);
  }
}
