// Studio maps: the player's Studio maps with their share codes (publish them for everyone online, rename, delete), the maps
// other players publish through Steam (Online play) and the maps fetched from them.
// Data: CSNZ_Server /api/studio (CVoxelManager::MapsApi, manager/studionet.cpp).
(function () {
  "use strict";

  const MODES = { 0: "Original", 1: "Deathmatch", 2: "Team Deathmatch", 4: "Deathmatch (AI)", 5: "Team Deathmatch (AI)",
    8: "Zombie", 9: "Zombie Mutation", 14: "Zombie Hero", 16: "Soccer", 20: "Zombie Escape", 22: "Gun Deathmatch",
    23: "Light Original", 24: "Zombie (AI)", 38: "Studio editor", 39: "Studio play", 40: "All Star", 45: "Zombie Z",
    49: "Prop Hunt", 52: "Shelter Studio", 53: "Scenario TX", 54: "Zombie Evolution", 58: "Zombie Classic" };

  const $ = id => document.getElementById(id);
  const view = $("view");
  let data = null;
  let tab = "mine";
  let editing = null; // id of the map whose name is being edited
  let armed = null; // "delete:<id>": the next click deletes
  let watched = new Set(); // request ids this page started (their outcome is shown)
  let timer = null;

  const PREVIEW = location.port !== "30080";

  function esc(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  const mode = m => MODES[m] || "Mode " + m;
  function size(bytes) {
    if (!bytes) return "--";
    return bytes < 1048576 ? Math.max(1, Math.round(bytes / 1024)) + " KB" : (bytes / 1048576).toFixed(1) + " MB";
  }
  function when(t) {
    if (!t) return "--";
    const d = new Date(t * 1000);
    return d.toLocaleDateString("en-GB", { day: "2-digit", month: "short", year: "numeric" });
  }

  function toast(text, kind) {
    const t = $("toast");
    t.textContent = text;
    t.className = "toast " + (kind || "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.add("hidden"), 3200);
  }

  async function api(body) {
    if (PREVIEW) return previewApi(body);
    const res = await fetch("/api/studio", body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
    const json = await res.json().catch(() => ({ error: "Unexpected server response." }));
    if (!res.ok || json.error) throw new Error(json.error || "Request failed.");
    return json;
  }

  async function load(quiet) {
    try {
      const fresh = await api();
      // a refresh doesn't throw away a name being typed
      if (editing && quiet && data) { data.requests = fresh.requests; renderRequests(); }
      else { data = fresh; render(); }
    } catch (e) {
      if (!data || !quiet)
        view.innerHTML = `<div class="message">${esc(e.message)}<br>Log in with a character and open it again.</div>`;
    }
    // while a map is on its way the page asks again
    clearTimeout(timer);
    const busy = data && data.requests.some(r => watched.has(r.id) && (r.status === "waiting" || r.status === "sent"));
    timer = setTimeout(() => load(true), busy ? 1500 : 8000);
  }

  // ---------------------------------------------------------------- rendering
  function codeButton(code) {
    return `<div class="code"><div class="label">Share code</div><button type="button" data-copy="${esc(code)}" title="Copy the code">${esc(code)}</button></div>`;
  }
  function cube(title) {
    return `<div class="cube">${esc((title || "?").trim().charAt(0).toUpperCase() || "?")}</div>`;
  }

  function mapCard(m, kind) {
    const pub = kind === "mine" && m.published;
    let actions = "", tag = "", by = "";
    if (kind === "mine") {
      by = `Created ${when(m.created)} &middot; saved ${when(m.updated)}`;
      tag = m.empty ? `<span class="tag private">Not saved yet</span>` : pub ? `<span class="tag online">Online for everyone</span>` : `<span class="tag private">Code only</span>`;
      actions = `<button class="btn ${pub ? "" : "main"}" data-act="publish" data-id="${esc(m.id)}" data-on="${pub ? 0 : 1}" ${m.empty ? "disabled" : ""}>${pub ? "Unpublish" : "Publish online"}</button>
        <button class="btn" data-act="edit" data-id="${esc(m.id)}">Rename</button>
        <button class="btn danger ${armed === "delete:" + m.id ? "armed" : ""}" data-act="delete" data-id="${esc(m.id)}">${armed === "delete:" + m.id ? "Click again to delete" : "Delete"}</button>`;
    } else if (kind === "online") {
      by = `By ${esc(m.creator_nickname || "?")}${m.ownerName ? " &middot; " + esc(m.ownerName) + " (Steam)" : ""}`;
      tag = m.have ? (m.outdated ? `<span class="tag online">New version</span>` : `<span class="tag have">Downloaded</span>`) : "";
      actions = m.have && !m.outdated
        ? `<button class="btn" data-act="copy" data-id="${esc(m.id)}">Make an editable copy</button>`
        : `<button class="btn main" data-act="get" data-code="${esc(m.id)}">${m.outdated ? "Update" : "Download"}</button>`;
    } else {
      by = `By ${esc(m.creator_nickname || "?")} &middot; downloaded ${when(m.fetched)}`;
      actions = `<button class="btn" data-act="copy" data-id="${esc(m.id)}">Make an editable copy</button>
        <button class="btn danger ${armed === "delete:" + m.id ? "armed" : ""}" data-act="delete" data-id="${esc(m.id)}">${armed === "delete:" + m.id ? "Click again to remove" : "Remove"}</button>`;
    }
    const edit = editing === m.id ? `<form class="edit" data-id="${esc(m.id)}">
        <input name="title" maxlength="60" value="${esc(m.title)}" placeholder="Name">
        <textarea name="description" maxlength="300" rows="2" placeholder="Description">${esc(m.description)}</textarea>
        <div class="row"><button class="btn main" type="submit">Save</button><button class="btn" type="button" data-act="cancel">Cancel</button></div>
      </form>` : "";
    return `<div class="map ${pub ? "published" : ""}">
      <div class="top">${cube(m.title)}
        <div class="what"><div class="title">${esc(m.title || "Untitled")}</div><div class="by">${by}</div>
          ${m.description ? `<div class="desc">${esc(m.description)}</div>` : ""}</div>
        ${codeButton(m.code)}</div>
      <div class="facts"><span>Mode <b>${esc(mode(m.resource_mode))}</b></span><span>Players <b>${m.resource_max_player || "--"}</b></span><span>Size <b>${size(m.size)}</b></span></div>
      ${edit}
      <div class="actions">${actions}${tag}</div>
    </div>`;
  }

  function render() {
    if (!data) return;
    document.querySelectorAll("#tabs [data-tab]").forEach(b => b.classList.toggle("active", b.dataset.tab === tab));
    const steam = $("steam");
    steam.className = "steam" + (data.steam ? " on" : "");
    steam.textContent = data.steam ? "Steam online" : "Steam off";
    steam.title = data.steam ? "Online play is on: your published maps are listed for every player online, and their maps for you."
      : "Online play is off (or Steam isn't running): only this PC's maps. Turn on Online play in the launcher.";

    let html = "";
    if (tab === "mine") {
      html = `<p class="intro"><span class="count">${data.mine.length} / ${data.max} maps</span>
        Every map has a <b>share code</b>. Give it to a friend: they type it in the Studio browser's search in the game (or in
        <b>Share code</b> up here) and get the map from your PC while you're online. <b>Publish online</b> lists the map for
        everyone playing with Online play on. Make and edit maps in the game: Studio &gt; Create.</p>`;
      html += data.mine.length ? `<div class="maps">${data.mine.map(m => mapCard(m, "mine")).join("")}</div>`
        : `<div class="empty">No maps yet. In the game open Studio &gt; Create, pick a sample map and save it in the editor (ESC &gt; Save).</div>`;
    } else if (tab === "online") {
      html = `<p class="intro"><span class="count">${data.online.length} maps</span>
        Maps the players online right now publish. A downloaded map stays on this PC: play it from the game's Studio browser
        (it's in the lists, or search its code) with <b>Make a game room</b>. Playing one you haven't downloaded downloads it too.</p>`;
      if (!data.steam)
        html += `<div class="empty">Online play is off, so there's no online list. Turn on <b>Online play</b> in the launcher (Steam must be running).</div>`;
      else
        html += data.online.length ? `<div class="maps">${data.online.map(m => mapCard(m, "online")).join("")}</div>`
          : `<div class="empty">Nobody online has published a map yet. Publish one of yours in My maps!</div>`;
    } else {
      html = `<p class="intro"><span class="count">${data.downloaded.length} maps</span>
        Maps you got from other players. They're in the game's Studio browser lists. <b>Make an editable copy</b> puts one in your
        own studio to change it (the copy is yours).</p>`;
      html += data.downloaded.length ? `<div class="maps">${data.downloaded.map(m => mapCard(m, "downloaded")).join("")}</div>`
        : `<div class="empty">Nothing downloaded yet: try the Online maps tab, or type a share code up here.</div>`;
    }
    view.innerHTML = `<div class="pane">${html}</div>`;
    renderRequests();
  }

  function renderRequests() {
    const box = $("requests");
    const mine = data.requests.filter(r => watched.has(r.id));
    box.classList.toggle("hidden", !mine.length);
    document.body.classList.toggle("with-requests", mine.length > 0);
    box.innerHTML = mine.map(r => {
      const text = r.status === "done" ? "Got " + r.key : r.status === "failed" ? r.key + ": " + r.error : "Looking for " + r.key + " on the players' PCs...";
      return `<span class="req ${esc(r.status)}">${esc(text)}</span>`;
    }).join("");
  }

  // ---------------------------------------------------------------- actions
  async function act(body, okText) {
    try {
      const r = await api(body);
      if (okText) toast(okText, "ok");
      await load(true);
      return r;
    } catch (e) {
      toast(e.message, "error");
      return null;
    }
  }

  async function getMap(code) {
    try {
      const r = await api({ action: "get", code });
      if (r.have) {
        toast(`"${r.title}" is already on this PC: it's in the Downloaded tab or your maps.`, "ok");
        return;
      }
      watched.add(r.request);
      toast("Looking for the map on the players' PCs...");
      await load(true);
    } catch (e) {
      toast(e.message, "error");
    }
  }

  function copy(text) {
    const input = document.createElement("input");
    input.value = text;
    document.body.appendChild(input);
    input.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    input.remove();
    toast(ok ? "Copied " + text : "Share code: " + text, ok ? "ok" : "");
  }

  function close() {
    location.href = "http://127.0.0.1:1/"; // the game closes its browser window on a failed load
  }

  $("close").addEventListener("click", close);
  document.addEventListener("keydown", e => { if (e.key === "Escape" && document.activeElement.tagName !== "INPUT" && document.activeElement.tagName !== "TEXTAREA") close(); });
  $("tabs").addEventListener("click", e => {
    const b = e.target.closest("[data-tab]");
    if (b) { tab = b.dataset.tab; editing = null; armed = null; render(); }
  });
  $("codeForm").addEventListener("submit", e => {
    e.preventDefault();
    const code = $("code").value.trim();
    if (!code) { toast("Type a share code, like ABCD-1234.", "error"); return; }
    getMap(code);
  });
  view.addEventListener("submit", e => {
    const f = e.target.closest("form.edit");
    if (!f) return;
    e.preventDefault();
    editing = null;
    act({ action: "rename", id: f.dataset.id, title: f.elements.namedItem("title").value, description: f.elements.namedItem("description").value }, "Saved.");
  });
  view.addEventListener("click", e => {
    const c = e.target.closest("[data-copy]");
    if (c) { copy(c.dataset.copy); return; }
    const b = e.target.closest("[data-act]");
    if (!b || b.disabled) return;
    const id = b.dataset.id, a = b.dataset.act;
    if (a !== "delete") armed = null;
    if (a === "publish") act({ action: "publish", id, on: b.dataset.on === "1" }, b.dataset.on === "1" ? "Published: players online see it now." : "It's no longer in the online list (its code still works).");
    else if (a === "edit") { editing = id; render(); }
    else if (a === "cancel") { editing = null; render(); }
    else if (a === "delete") {
      if (armed !== "delete:" + id) { armed = "delete:" + id; render(); return; }
      armed = null;
      act({ action: "delete", id }, "Deleted.");
    }
    else if (a === "get") getMap(b.dataset.code);
    else if (a === "copy") act({ action: "copy", id }, "Copied into My maps: edit it in the game (Studio > My studio).");
  });

  // ---------------------------------------------------------------- layout preview (not served by CSNZ_Server)
  let preview = null;
  function previewApi(body) {
    if (!preview) {
      preview = { steam: true, max: 20, requests: [],
        mine: [
          { id: "a1", code: "7K2Q-M9XD", title: "Dust Blocks", description: "A blocky remake of a classic, two bomb sites.", creator_nickname: "Player", resource_mode: 39, resource_max_player: 16, updated: 1794070696, created: 1794000000, size: 48211, published: true },
          { id: "a2", code: "Q4TB-1ZR8", title: "Zombie Tower", description: "", creator_nickname: "Player", resource_mode: 24, resource_max_player: 12, updated: 1794110696, created: 1794100000, size: 1240211, published: false },
          { id: "a3", code: "H0PE-5WN2", title: "Flat map_Copy", description: "", creator_nickname: "Player", resource_mode: 39, resource_max_player: 16, updated: 0, created: 1794120000, size: 0, published: false, empty: true }],
        online: [
          { id: "b1", code: "ZX9C-0KQ4", title: "Parkour Valley", description: "Jump from roof to roof. 40 checkpoints.", creator_nickname: "Rika", ownerName: "rika_cs", resource_mode: 39, resource_max_player: 8, updated: 1794170696, size: 310000, have: false },
          { id: "b2", code: "M1N3-CR4F", title: "Mine Escape", description: "Run from the zombies through the mine.", creator_nickname: "Toni", ownerName: "Toni", resource_mode: 20, resource_max_player: 24, updated: 1794070000, size: 2210000, have: true, outdated: false }],
        downloaded: [
          { id: "b2", code: "M1N3-CR4F", title: "Mine Escape", description: "Run from the zombies through the mine.", creator_nickname: "Toni", resource_mode: 20, resource_max_player: 24, updated: 1794070000, fetched: 1794180000, size: 2210000 }] };
    }
    if (body && body.action === "get") return Promise.resolve({ ok: true, request: 1 });
    return Promise.resolve(JSON.parse(JSON.stringify(preview)));
  }

  load();
})();
