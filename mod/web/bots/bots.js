// The room's AI players: list (like the room's player list), an editor for each one and weapon filters.
// Data comes from CSNZ_Server (/api/roombots, /api/botpresets); weapons and characters from catalog.json
// (tools/export_bot_catalog.py).
(function () {
  "use strict";

  const COOP_MODES = [15, 17, 28, 52, 53, 59]; // CT spawns only: no team choice
  const SKILLS = ["Very easy", "Easy", "Normal", "Normal+", "Hard", "Hard+", "Very hard", "Expert"];
  const NAMES = ["Mike", "Ana", "Viktor", "Kenji", "Lucia", "Omar", "Sasha", "Diego", "Mei", "Hans", "Nadia", "Raul",
    "Yuki", "Bruno", "Elena", "Tariq", "Ivan", "Chloe", "Marco", "Aiko", "Pablo", "Ingrid", "Dmitri", "Rosa", "Felix",
    "Leila", "Hugo", "Sora", "Mateo", "Freya", "Kofi", "Carmen", "Axel", "Nina", "Joon", "Vera", "Emil", "Iris",
    "Santi", "Greta"];
  // Counter-Strike 1.6's own weapons: pistols 1-6, shotguns..machine gun 7-24, the default knife
  const CLASSIC = { primary: range(7, 24), secondary: range(1, 6), melee: [161] };

  const $ = id => document.getElementById(id);
  let room = null;          // {host, mode, maxPlayers, players, bots: [...]}
  let presets = [];         // [{id, name, primary: [...], secondary: [...], melee: [...]}]
  let catalog = { primary: [], secondary: [], melee: [], character: [] };
  let byId = {};
  let selected = null;      // bot id
  let pickSlot = null;      // which loadout slot the picker edits
  let pickCat = "All";
  let saveTimer = null;
  let newTeam = 2;
  let newPreset = 0;
  try { newPreset = +(localStorage.getItem("bots.newPreset") || 0); } catch (e) { /* no storage */ }

  function range(a, b) { const r = []; for (let i = a; i <= b; i++) r.push(i); return r; }

  function toast(text, bad) {
    const t = $("toast");
    t.textContent = text;
    t.className = "toast" + (bad ? " bad" : "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.add("hidden"), 2600);
  }

  // not served by CSNZ_Server (layout preview): a fake room
  const PREVIEW = location.port !== "30080";
  const mock = { host: true, mode: 3, maxPlayers: 10, players: 3, bots: [
    { id: 1, name: "Mike", level: 30, team: 2, primary: 0, secondary: 0, melee: 0, character: 0, skill: -1, preset: 0 },
    { id: 2, name: "Ana", level: 18, team: 1, primary: 0, secondary: 0, melee: 0, character: 0, skill: 5, preset: 0 }] };
  let mockPresets = [];

  async function call(path, body) {
    if (PREVIEW) return previewCall(path, body);
    const res = await fetch(path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
    const json = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(json.error || "The server didn't answer.");
    return json;
  }

  function previewCall(path, body) {
    if (path === "/api/botpresets") {
      if (body && body.action === "save") {
        const p = Object.assign({}, body.preset);
        if (!p.id) p.id = 1 + Math.max(0, ...mockPresets.map(x => x.id));
        mockPresets = mockPresets.filter(x => x.id !== p.id).concat([p]);
        return { presets: JSON.parse(JSON.stringify(mockPresets)), saved: p.id };
      }
      if (body && body.action === "delete") mockPresets = mockPresets.filter(x => x.id !== body.id);
      return { presets: JSON.parse(JSON.stringify(mockPresets)) };
    }
    if (body && (body.action === "add" || body.action === "fill")) {
      const n = body.action === "fill" ? mock.maxPlayers - mock.players : body.count || 1;
      for (let i = 0; i < n && mock.bots.length + 1 < mock.maxPlayers; i++)
        mock.bots.push({ id: Date.now() + i, name: NAMES[mock.bots.length % NAMES.length], level: 20, team: body.team || 2, primary: 0, secondary: 0, melee: 0, character: 0, skill: -1, preset: body.preset || 0 });
    }
    if (body && body.action === "update") Object.assign(mock.bots.find(b => b.id === body.id), body, { action: undefined });
    if (body && body.action === "remove") mock.bots = mock.bots.filter(b => b.id !== body.id);
    if (body && body.action === "move") {
      const i = mock.bots.findIndex(b => b.id === body.id);
      const [b] = mock.bots.splice(i, 1);
      mock.bots.splice(Math.max(0, Math.min(body.index, mock.bots.length)), 0, b);
    }
    mock.players = 1 + mock.bots.length;
    return JSON.parse(JSON.stringify(mock));
  }

  const api = body => call("/api/roombots", body);

  function close() {
    location.href = "http://127.0.0.1:1/"; // the game closes its browser window on a failed load
  }

  // ---------------------------------------------------------------- list
  function isCoop() { return room && COOP_MODES.includes(room.mode); }
  function bot(id) { return room && room.bots.find(b => b.id === id); }
  function presetName(id) { const p = presets.find(x => x.id === id); return p ? p.name : ""; }

  function itemImg(id, cls) {
    const it = byId[id];
    return it && it.icon ? `<img class="${cls || ""}" src="${it.icon}" alt="">` : "";
  }

  function renderTop() {
    const full = room.players >= room.maxPlayers;
    const count = Math.max(1, Math.min(+$("count").value || 1, room.maxPlayers - room.players || 1));
    $("add").disabled = !room.host || full;
    $("fill").disabled = !room.host || full;
    $("newteam").classList.toggle("hidden", isCoop());
    document.querySelectorAll("#newteam button").forEach(x => x.classList.toggle("on", +x.dataset.team === newTeam));
    $("add").textContent = "+ Add " + (count > 1 ? count : "bot");
    $("roominfo").textContent = `Players ${room.players}/${room.maxPlayers}` + (room.host ? "" : " · only the host can edit");
    const opts = `<option value="0">Any weapon</option>` + presets.map(p => `<option value="${p.id}">${esc(p.name)}</option>`).join("");
    if (!presets.some(p => p.id === newPreset)) newPreset = 0;
    $("newpreset").innerHTML = opts;
    $("newpreset").value = String(newPreset);
  }

  function renderList() {
    renderTop();
    const list = $("slots");
    list.innerHTML = "";
    room.bots.forEach((b, i) => {
      const row = document.createElement("div");
      row.className = "slot" + (b.id === selected ? " selected" : "");
      const team = b.team === 1 ? "T" : "CT";
      const wpn = b.primary < 0 || (!b.primary && filterNone(presets.find(p => p.id === b.preset), "primary")) ? `<span class="noimg">No primary</span>` : b.primary ? itemImg(b.primary, "wpn") || `<span class="noimg">${esc(byId[b.primary] ? byId[b.primary].name : "#" + b.primary)}</span>`
                            : `<span class="noimg">${b.preset && presetName(b.preset) ? esc(presetName(b.preset)) : "Random"}</span>`;
      row.innerHTML = `
        <div class="level">${b.level}</div>
        <div class="who"><div class="nm"><span class="tag">[BOT]</span>${esc(b.name)}</div>
          <div class="sub"><span class="team-${team.toLowerCase()}">${isCoop() ? "Ally" : team}</span> &middot; ${b.skill >= 0 ? SKILLS[b.skill] : "Room skill"}</div></div>
        ${wpn}
        <div class="acts">
          <button class="icon-btn" data-act="up" title="Move up" ${i === 0 ? "disabled" : ""}>&#9650;</button>
          <button class="icon-btn" data-act="down" title="Move down" ${i === room.bots.length - 1 ? "disabled" : ""}>&#9660;</button>
          <button class="icon-btn del" data-act="del" title="Remove">&times;</button>
        </div>`;
      row.addEventListener("click", e => {
        const act = e.target.closest("[data-act]");
        if (act) {
          e.stopPropagation();
          if (act.dataset.act === "del") send({ action: "remove", id: b.id });
          else send({ action: "move", id: b.id, index: i + (act.dataset.act === "up" ? -1 : 1) });
          return;
        }
        select(b.id);
      });
      list.appendChild(row);
    });
    $("hint").textContent = room.bots.length
      ? "They play as bots when the match starts, with these names, characters and weapons. Kicking one from the room list removes it too."
      : "No AI players yet. Add bots to fill the room's free slots.";
  }

  // ---------------------------------------------------------------- editor
  function select(id) {
    selected = id;
    renderList();
    renderEditor();
  }

  function renderEditor() {
    const b = bot(selected);
    $("editor").classList.toggle("hidden", !b);
    if (!b) return;
    if (document.activeElement !== $("name")) $("name").value = b.name;
    $("teamfield").classList.toggle("hidden", isCoop());
    document.querySelectorAll("#team button").forEach(x => x.classList.toggle("on", +x.dataset.team === b.team));
    $("preset").innerHTML = `<option value="0">Any weapon</option>` + presets.map(p => `<option value="${p.id}">${esc(p.name)}</option>`).join("");
    $("preset").value = String(presets.some(p => p.id === b.preset) ? b.preset : 0);
    $("skill").value = b.skill;
    $("skilltext").textContent = b.skill >= 0 ? SKILLS[b.skill] : "room's AI difficulty";
    document.querySelectorAll(".slot-pick").forEach(btn => {
      btn.querySelector(".pick").innerHTML = slotHtml(b, btn.dataset.slot, false);
    });
    $("details").classList.toggle("hidden", !!pickSlot);
    renderPicker();
  }

  // what a loadout slot holds: a chosen item, "Random" (from the bot's filter) or "None"
  function slotHtml(b, slot, small) {
    const id = b[slot];
    const filter = presets.find(p => p.id === b.preset);
    if (id < 0) return `<span class="none">&#10005;</span><span class="pn">${slot === "melee" ? "Default knife" : "None"}</span>`;
    if (id > 0) {
      const it = byId[id];
      return `${it && it.icon ? `<img src="${it.icon}" alt="">` : ""}<span class="pn">${esc(it ? it.name : "#" + id)}</span>`;
    }
    if (filterNone(filter, slot)) return `<span class="none">&#10005;</span><span class="pn">None (${esc(filter.name)})</span>`;
    const text = filter && slot !== "character" ? `Random (${esc(filter.name)})` : "Random";
    return small ? `<span class="pn">${text}</span>` : `<span class="rnd">?</span><span class="pn">${text}</span>`;
  }

  function change(fields, now) {
    const b = bot(selected);
    if (!b || !room.host) return;
    Object.assign(b, fields);
    renderList();
    renderEditor();
    clearTimeout(saveTimer);
    saveTimer = null;
    const send_ = () => send(Object.assign({ action: "update", id: b.id }, fields));
    if (now) send_();
    else saveTimer = setTimeout(() => { saveTimer = null; send_(); }, 450);
  }

  // ---------------------------------------------------------------- picker (one weapon / character for a bot)
  function renderPicker() {
    const b = bot(selected);
    $("picker").classList.toggle("hidden", !pickSlot || !b);
    if (!pickSlot || !b) return;
    $("pickwho").textContent = "[BOT] " + b.name;
    document.querySelectorAll("#picktabs button").forEach(t => {
      t.classList.toggle("active", t.dataset.slot === pickSlot);
      t.querySelector(".pt").innerHTML = slotHtml(b, t.dataset.slot, true);
    });
    let items = catalog[pickSlot] || [];
    if (pickSlot === "character" && !isCoop())
      items = items.filter(c => !c.team || c.team === b.team);
    const cats = ["All", ...new Set(items.map(i => i.category))];
    if (pickSlot === "character") cats.length = 1;
    if (!cats.includes(pickCat)) pickCat = "All";
    $("cats").innerHTML = cats.length > 1 ? cats.map(c => `<button type="button" class="${c === pickCat ? "active" : ""}" data-cat="${esc(c)}">${esc(c)}</button>`).join("") : "";
    const q = $("search").value.trim().toLowerCase();
    const keepScroll = $("grid").scrollTop;
    const shown = items.filter(i => (pickCat === "All" || i.category === pickCat) && (!q || i.name.toLowerCase().includes(q)));
    const cur = b[pickSlot];
    const filter = presets.find(p => p.id === b.preset);
    const randomText = filterNone(filter, pickSlot) ? `Filter &ldquo;${esc(filter.name)}&rdquo;: none`
      : filter && pickSlot !== "character" ? `Random from &ldquo;${esc(filter.name)}&rdquo;` : "Random";
    let head = `<button type="button" class="cell${!cur ? " on" : ""}" data-id="0"><span class="rnd">?</span><span class="cn">${randomText}</span></button>`;
    if (pickSlot !== "character")
      head += `<button type="button" class="cell${cur < 0 ? " on" : ""}" data-id="-1" title="${pickSlot === "melee" ? "Only the default knife" : "No weapon in this slot"}"><span class="none">&#10005;</span><span class="cn">${pickSlot === "melee" ? "Default knife" : "None"}</span></button>`;
    $("grid").innerHTML = head + shown.map(i => cellHtml(i, i.id === cur ? " on" : "")).join("");
    const key = pickSlot + "|" + pickCat + "|" + q;
    if (renderPicker.lastKey === key) $("grid").scrollTop = keepScroll;
    renderPicker.lastKey = key;
  }

  function cellHtml(i, cls) {
    return `<button type="button" class="cell${cls}" data-id="${i.id}" title="${esc(i.name)}">${i.icon ? `<img src="${i.icon}" alt="" loading="lazy">` : ""}<span class="cn">${esc(i.name)}</span></button>`;
  }

  // ---------------------------------------------------------------- weapon filters
  let fmSel = null;        // preset being edited (a copy)
  let fmSlot = "primary";
  let fmCat = "All";
  let fmOnly = false;      // show only the allowed weapons
  // a filter's primary / secondary can be "None" ([-1]): its bots carry no weapon there (they drop the one they get)
  const NONE_SLOTS = ["primary", "secondary"];
  const filterNone = (p, slot) => !!p && NONE_SLOTS.includes(slot) && (p[slot] || []).includes(-1);

  function openFilters() {
    $("fm").classList.remove("hidden");
    if (!fmSel && presets.length) fmEdit(presets[0]);
    renderFilters();
  }

  function fmEdit(p) {
    fmSel = JSON.parse(JSON.stringify(p));
    fmSlot = "primary";
    fmCat = "All";
    $("fmsearch").value = "";
    $("fmname").value = fmSel.name;
  }

  function fmShown() {
    const q = $("fmsearch").value.trim().toLowerCase();
    const allowed = new Set(fmSel ? fmSel[fmSlot] : []);
    return (catalog[fmSlot] || []).filter(i => (fmCat === "All" || i.category === fmCat) && (!q || i.name.toLowerCase().includes(q)) &&
      (!fmOnly || allowed.has(i.id)));
  }

  function renderFilters() {
    $("fmlist").innerHTML = presets.map(p => `<button type="button" class="fm-item${fmSel && fmSel.id === p.id ? " on" : ""}" data-id="${p.id}">${esc(p.name)}
      <small>${filterNone(p, "primary") ? "no" : p.primary.length} primary &middot; ${filterNone(p, "secondary") ? "no" : p.secondary.length} secondary &middot; ${p.melee.length} melee</small></button>`).join("") ||
      `<p class="hint">No filters yet.</p>`;
    $("fmedit").classList.toggle("hidden", !fmSel);
    if (!fmSel) return;
    document.querySelectorAll("#fmslots button").forEach(x => x.classList.toggle("active", x.dataset.slot === fmSlot));
    const items = catalog[fmSlot] || [];
    const cats = ["All", ...new Set(items.map(i => i.category))];
    if (!cats.includes(fmCat)) fmCat = "All";
    $("fmcats").innerHTML = cats.length > 2 ? cats.map(c => `<button type="button" class="${c === fmCat ? "active" : ""}" data-cat="${esc(c)}">${esc(c)}</button>`).join("") : "";
    const q = $("fmsearch").value.trim().toLowerCase();
    const allowed = new Set(fmSel[fmSlot]);
    const keep = $("fmgrid").scrollTop;
    const none = filterNone(fmSel, fmSlot);
    const noneCell = NONE_SLOTS.includes(fmSlot)
      ? `<button type="button" class="cell${none ? " blocked" : ""}" data-id="-1" title="Bots with this filter carry no ${fmSlot} weapon"><span class="none">&#10005;</span><span class="cn">None</span></button>`
      : "";
    $("fmgrid").classList.toggle("fm-grid-none", none);
    $("fmgrid").innerHTML = noneCell + fmShown().map(i => cellHtml(i, allowed.has(i.id) ? " allowed" : "")).join("");
    const key = fmSlot + "|" + fmCat + "|" + q;
    if (renderFilters.lastKey === key) $("fmgrid").scrollTop = keep;
    renderFilters.lastKey = key;
    const n = fmSel[fmSlot].length;
    $("fmonly").classList.toggle("on", fmOnly);
    $("fmcount").textContent = none ? `No ${fmSlot} weapon: bots don't buy one and drop the one they get` : n ? `${n} ${fmSlot} weapon${n > 1 ? "s" : ""} allowed` : `No ${fmSlot} weapon chosen: bots with this filter get any ${fmSlot} weapon`;
  }

  async function presetCall(body) {
    const r = await call("/api/botpresets", body);
    presets = r.presets || [];
    return r;
  }

  async function fmSave() {
    if (!fmSel) return;
    fmSel.name = $("fmname").value.trim() || "Filter";
    try {
      const r = await presetCall({ action: "save", preset: fmSel });
      fmSel.id = r.saved;
      toast("Filter saved.");
      renderFilters();
      renderList();
      renderEditor();
    } catch (e) { toast(e.message, true); }
  }

  async function fmCreate(p) {
    try {
      const r = await presetCall({ action: "save", preset: p });
      const saved = presets.find(x => x.id === r.saved);
      if (saved) fmEdit(saved);
      renderFilters();
      renderList();
      renderEditor();
    } catch (e) { toast(e.message, true); }
  }

  // export / import: the filter as a short text code
  function encode(p) {
    return "CSOBOT1:" + btoa(JSON.stringify({ n: p.name, p: p.primary, s: p.secondary, m: p.melee }));
  }
  function decode(code) {
    const m = /CSOBOT1:([A-Za-z0-9+/=]+)/.exec(code.trim());
    if (!m) throw new Error("That isn't a weapon filter code.");
    const o = JSON.parse(atob(m[1]));
    const ids = a => (Array.isArray(a) ? a.filter(x => Number.isInteger(x)) : []);
    return { name: String(o.n || "Imported").slice(0, 40), primary: ids(o.p), secondary: ids(o.s), melee: ids(o.m) };
  }

  let codeAction = null;
  function codeBox(title, text, onOk) {
    $("codetitle").textContent = title;
    $("code").value = text;
    $("code").readOnly = !onOk;
    codeAction = onOk;
    $("codebox").classList.remove("hidden");
    $("code").focus();
    $("code").select();
  }

  // ---------------------------------------------------------------- server
  let lastJson = "";

  async function send(body) {
    try {
      room = await api(body);
      lastJson = JSON.stringify(room);
      if ((body.action === "add" || body.action === "fill") && room.bots.length) selected = room.bots[room.bots.length - 1].id;
      if (!bot(selected)) selected = room.bots.length ? room.bots[0].id : null;
      renderList();
      renderEditor();
    } catch (e) {
      toast(e.message, true);
      load();
    }
  }

  async function load() {
    try {
      const fresh = await api();
      const json = JSON.stringify(fresh);
      if (json === lastJson && room) return; // nothing changed: keep the picker's scroll and hover
      lastJson = json;
      room = fresh;
      if (!bot(selected)) selected = room.bots.length ? room.bots[0].id : null;
      renderList();
      renderEditor();
    } catch (e) {
      toast(e.message, true);
    }
  }

  function esc(s) {
    return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  // ---------------------------------------------------------------- events
  $("close").addEventListener("click", close);
  document.addEventListener("keydown", e => {
    if (e.key !== "Escape") return;
    if (!$("codebox").classList.contains("hidden")) $("codebox").classList.add("hidden");
    else if (!$("fm").classList.contains("hidden")) $("fm").classList.add("hidden");
    else if (pickSlot) { pickSlot = null; renderEditor(); }
    else close();
  });

  // adding bots
  $("newteam").addEventListener("click", e => {
    const t = e.target.closest("[data-team]");
    if (t) { newTeam = +t.dataset.team; renderTop(); }
  });
  const setCount = n => { $("count").value = String(Math.max(1, Math.min(31, n))); renderTop(); };
  $("minus").addEventListener("click", () => setCount((+$("count").value || 1) - 1));
  $("plus").addEventListener("click", () => setCount((+$("count").value || 1) + 1));
  $("count").addEventListener("input", () => { $("count").value = $("count").value.replace(/\D/g, ""); renderTop(); });
  $("count").addEventListener("blur", () => setCount(+$("count").value || 1));
  $("newpreset").addEventListener("change", () => {
    newPreset = +$("newpreset").value;
    try { localStorage.setItem("bots.newPreset", String(newPreset)); } catch (e) { /* no storage */ }
  });
  const newBot = () => ({ team: isCoop() ? 2 : newTeam, preset: newPreset });
  $("add").addEventListener("click", () => send(Object.assign({ action: "add", count: Math.max(1, +$("count").value || 1) }, newBot())));
  $("fill").addEventListener("click", () => send(Object.assign({ action: "fill" }, newBot())));

  // the selected bot
  $("name").addEventListener("input", () => change({ name: $("name").value.trim() || "Bot" }));
  $("name").addEventListener("blur", () => renderEditor());
  $("randname").addEventListener("click", () => {
    const used = new Set(room.bots.map(b => b.name));
    const free = NAMES.filter(n => !used.has(n));
    const pool = free.length ? free : NAMES;
    $("name").value = pool[Math.floor(Math.random() * pool.length)];
    change({ name: $("name").value }, true);
  });
  $("team").addEventListener("click", e => {
    const t = e.target.closest("[data-team]");
    if (!t) return;
    // a character of the other team can't be used (the game's scoreboard breaks): back to random
    const b = bot(selected), team = +t.dataset.team, ch = b && byId[b.character];
    change(ch && ch.team && ch.team !== team ? { team, character: 0 } : { team }, true);
  });
  $("preset").addEventListener("change", () => change({ preset: +$("preset").value }, true));
  $("skill").addEventListener("input", () => change({ skill: +$("skill").value }));
  const openSlot = slot => {
    pickSlot = slot;
    pickCat = "All";
    $("search").value = "";
    renderEditor();
  };
  document.querySelectorAll(".slot-pick").forEach(btn => btn.addEventListener("click", () => openSlot(btn.dataset.slot)));
  $("picktabs").addEventListener("click", e => {
    const t = e.target.closest("[data-slot]");
    if (t) openSlot(t.dataset.slot);
  });
  $("pickback").addEventListener("click", () => { pickSlot = null; renderEditor(); });
  $("cats").addEventListener("click", e => {
    const c = e.target.closest("[data-cat]");
    if (c) { pickCat = c.dataset.cat; renderPicker(); }
  });
  $("search").addEventListener("input", renderPicker);
  $("grid").addEventListener("click", e => {
    const c = e.target.closest("[data-id]");
    if (c && pickSlot) change({ [pickSlot]: +c.dataset.id }, true);
  });

  // weapon filters
  $("filters").addEventListener("click", openFilters);
  $("fmclose").addEventListener("click", () => $("fm").classList.add("hidden"));
  $("fmlist").addEventListener("click", e => {
    const it = e.target.closest("[data-id]");
    if (!it) return;
    const p = presets.find(x => x.id === +it.dataset.id);
    if (p) { fmEdit(p); renderFilters(); }
  });
  $("fmnew").addEventListener("click", () => fmCreate({ name: "New filter", primary: [], secondary: [], melee: [] }));
  $("fmclassic").addEventListener("click", () => {
    const have = slot => CLASSIC[slot].filter(id => byId[id]);
    fmCreate({ name: "Counter-Strike classic", primary: have("primary"), secondary: have("secondary"), melee: have("melee") });
  });
  $("fmimport").addEventListener("click", () => codeBox("Paste a weapon filter code", "", text => {
    try { fmCreate(decode(text)); } catch (e) { toast(e.message, true); }
  }));
  $("fmexport").addEventListener("click", () => fmSel && codeBox("Weapon filter code (copy it)", encode(fmSel), null));
  $("fmsave").addEventListener("click", fmSave);
  $("fmname").addEventListener("keydown", e => { if (e.key === "Enter") fmSave(); });
  $("fmdelete").addEventListener("click", async () => {
    if (!fmSel || !fmSel.id) return;
    try {
      await presetCall({ action: "delete", id: fmSel.id });
      fmSel = null;
      if (presets.length) fmEdit(presets[0]);
      renderFilters();
      renderList();
      renderEditor();
    } catch (e) { toast(e.message, true); }
  });
  $("fmslots").addEventListener("click", e => {
    const s = e.target.closest("[data-slot]");
    if (s) { fmSlot = s.dataset.slot; fmCat = "All"; $("fmsearch").value = ""; renderFilters(); }
  });
  $("fmcats").addEventListener("click", e => {
    const c = e.target.closest("[data-cat]");
    if (c) { fmCat = c.dataset.cat; renderFilters(); }
  });
  $("fmsearch").addEventListener("input", renderFilters);
  $("fmonly").addEventListener("click", () => { fmOnly = !fmOnly; renderFilters(); });
  $("fmallslot").addEventListener("click", () => {
    if (!fmSel) return;
    fmSel[fmSlot] = (catalog[fmSlot] || []).map(i => i.id);
    renderFilters();
  });
  $("fminvert").addEventListener("click", () => {
    if (!fmSel) return;
    const had = new Set(fmSel[fmSlot].filter(x => x !== -1));
    fmSel[fmSlot] = (catalog[fmSlot] || []).map(i => i.id).filter(id => !had.has(id));
    renderFilters();
  });
  $("fmgrid").addEventListener("click", e => {
    const c = e.target.closest("[data-id]");
    if (!c || !fmSel) return;
    const id = +c.dataset.id;
    if (id === -1) fmSel[fmSlot] = filterNone(fmSel, fmSlot) ? [] : [-1];
    else {
      const list = fmSel[fmSlot] = fmSel[fmSlot].filter(x => x !== -1);
      const at = list.indexOf(id);
      if (at >= 0) list.splice(at, 1); else list.push(id);
    }
    renderFilters();
  });
  $("fmall").addEventListener("click", () => {
    if (!fmSel) return;
    const set = new Set(fmSel[fmSlot].filter(x => x !== -1));
    fmShown().forEach(i => set.add(i.id));
    fmSel[fmSlot] = [...set];
    renderFilters();
  });
  $("fmnone").addEventListener("click", () => {
    if (!fmSel) return;
    const drop = new Set(fmShown().map(i => i.id));
    fmSel[fmSlot] = fmSel[fmSlot].filter(id => !drop.has(id));
    renderFilters();
  });
  $("codeok").addEventListener("click", () => {
    $("codebox").classList.add("hidden");
    if (codeAction) codeAction($("code").value);
  });
  $("codecancel").addEventListener("click", () => $("codebox").classList.add("hidden"));

  fetch("catalog.json").then(r => (r.ok ? r.json() : null)).then(c => {
    if (c) {
      catalog = c;
      byId = {};
      Object.values(c).forEach(list => list.forEach(i => { byId[i.id] = i; }));
    }
    if (room) { renderList(); renderEditor(); }
  }).catch(() => {});
  presetCall().then(() => { if (room) { renderList(); renderEditor(); } }).catch(() => {});
  load();
  setInterval(() => { if (document.activeElement !== $("name") && !saveTimer) load(); }, 5000);
})();
