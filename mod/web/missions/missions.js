// Missions: requests from the game's quest NPCs, the event of the week and the Black Market (CSNZ_Server
// /api/missions, CMissionManager), the Rift Exchange (/api/riftexchange, CZSRiftManager), plus the daily/weekly
// orders and the challenge book (/api/state, CWarbondManager).
(function () {
  "use strict";

  const $ = id => document.getElementById(id);
  const view = $("view");
  let data = null;   // /api/missions
  let state = null;  // /api/state (orders)
  let tab = "requests";
  let selected = null; // request id shown in the briefing
  let busy = false;
  let confirmBuy = null;
  let marketPick = null; // Black Market item shown in the preview
  let rift = null;       // /api/riftexchange (null: an older server without it)
  let riftPick = null;   // Rift Exchange item shown in the preview
  let riftConfirm = null; // "buy:<id>" / "hold:<id>" waiting for the second click

  const PREVIEW = location.port !== "30080";

  function esc(s) {
    return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  const num = n => Number(n || 0).toLocaleString("en-US");
  const pct = (a, b) => (b ? Math.min(100, Math.round(a * 100 / b)) : 0);
  const medal = small => '<span class="medal-icon' + (small ? " small" : "") + '"></span>';
  const badge = small => '<span class="badge-icon' + (small ? " small" : "") + '"></span>';
  const ticket = small => '<span class="ticket-icon' + (small ? " small" : "") + '"></span>';
  const rewards = (b, m) => '<span class="rewards">' + (b ? `<span class="b">${badge(true)}${b}</span>` : "") + (m ? `<span class="m">${medal(true)}${m}</span>` : "") + "</span>";
  function timeLeft(seconds) {
    const d = Math.floor(seconds / 86400), h = Math.floor(seconds % 86400 / 3600), m = Math.floor(seconds % 3600 / 60);
    return d ? d + "d " + h + "h" : h ? h + "h " + m + "m" : m + "m";
  }
  // several requirements: the average of their parts
  const share = progress => (progress.length ? progress.reduce((a, p) => a + (p.need ? Math.min(1, p.have / p.need) : 1), 0) / progress.length : 0);
  const progressText = progress => progress.map(p => num(p.have) + " / " + num(p.need)).join(" · ");

  function toast(text, kind) {
    const t = $("toast");
    t.textContent = text;
    t.className = "toast " + (kind || "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.add("hidden"), 2600);
  }

  async function api(path, body) {
    if (PREVIEW) return preview(path, body);
    const res = await fetch(path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
    const json = await res.json().catch(() => ({ error: "Unexpected server response." }));
    if (!res.ok || json.error) throw new Error(json.error || "Request failed.");
    return json;
  }

  async function load() {
    try {
      [data, state, rift] = await Promise.all([api("/api/missions"), api("/api/state"), api("/api/riftexchange").catch(() => null)]);
      render();
    } catch (e) {
      if (!data) view.innerHTML = `<div class="message">${esc(e.message)}<br>Log in with a character and open it again.</div>`;
    }
  }

  async function act(body, okText) {
    if (busy) return;
    busy = true;
    try {
      if (body.rift) rift = await api("/api/riftexchange", body);
      else data = await api("/api/missions", body);
      if (okText) toast(okText, "ok");
      if (body.action === "claim" || body.action === "claimEvent" || body.action === "claimBonus") state = await api("/api/state");
      render();
    } catch (e) {
      toast(e.message, "error");
    } finally {
      busy = false;
    }
  }

  // ---------------------------------------------------------------- requests
  function reqCard(r, kind) {
    const done = kind === "active" && r.done;
    const side = kind === "active"
      ? (r.done ? `<button type="button" class="btn" data-claim="${r.id}">Claim</button>` : `<button type="button" class="btn ghost" data-drop="${r.id}">Drop</button>`)
      : `<button type="button" class="btn" data-accept="${r.id}">Accept</button>`;
    return `<div class="req${done ? " done" : ""}${r.id === selected ? " selected" : ""}" data-req="${r.id}">
      <img src="npc/${esc(r.portrait)}.png" alt="" onerror="this.style.visibility='hidden'">
      <div class="body"><span class="npc">${esc(r.npc)}</span><span class="title">${esc(r.title)}</span><span class="goal">${esc(r.text)}</span>
        ${kind === "active" ? `<div class="progress"><span style="width:${r.done ? 100 : share(r.progress) * 100}%"></span></div>` : ""}</div>
      <div class="side">${rewards(r.badges, r.medals)}${side}
        ${kind === "active" ? `<span class="npc">${r.done ? "Ready" : esc(progressText(r.progress))}</span>` : ""}</div>
    </div>`;
  }

  function renderRequests() {
    const rq = data.requests;
    const all = rq.active.concat(rq.offers);
    if (!all.some(r => r.id === selected)) selected = all.length ? all[0].id : null;
    const slots = [];
    for (let i = rq.active.length; i < rq.slots; i++) slots.push('<div class="slot-empty">Free slot: accept a request</div>');
    const pick = all.find(r => r.id === selected);
    view.innerHTML = `<div class="requests">
      <div class="req-lists">
        <section><h2 class="section-title">Your requests<small>${rq.active.length} / ${rq.slots}</small></h2>
          <div class="event-list">${rq.active.map(r => reqCard(r, "active")).join("")}${slots.join("")}</div></section>
        <section><h2 class="section-title">Offered today<small>New offers in ${timeLeft(data.refreshIn)}</small></h2>
          <div class="event-list">${rq.offers.map(r => reqCard(r, "offer")).join("") || '<div class="slot-empty">No more offers today</div>'}</div></section>
      </div>
      <aside class="briefing">${pick ? `
        <img class="portrait" src="npc/${esc(pick.portrait)}.png" alt="" onerror="this.style.display='none'">
        <div><div class="from">From ${esc(pick.npc)}</div><h2>${esc(pick.title)}</h2></div>
        ${pick.story ? `<div class="story">${esc(pick.story)}</div>` : ""}
        <div class="goal">${esc(pick.text)}</div>
        <div>${rewards(pick.badges, pick.medals)}</div>` : '<div class="empty">No requests today.</div>'}</aside>
    </div>`;
  }

  // ---------------------------------------------------------------- event
  function renderEvent() {
    const ev = data.event;
    if (!ev) {
      view.innerHTML = '<div class="message">No event this week.</div>';
      return;
    }
    const allClaimed = ev.missions.every(m => m.claimed);
    view.innerHTML = `<div class="event">
      <div class="event-head"><h2>${esc(ev.name)}</h2><span class="ends">Ends in ${timeLeft(ev.endsIn)}</span><p>${esc(ev.description)}</p></div>
      <div class="event-list">${ev.missions.map((m, i) => `
        <div class="emission${m.claimed ? " claimed" : m.done ? " ready" : ""}">
          <div><div class="text">${esc(m.text)}</div><div class="progress"><span style="width:${m.done ? 100 : share(m.progress) * 100}%"></span></div>
            <div class="sub">${m.claimed ? "Claimed" : m.done ? "Complete" : esc(progressText(m.progress))}</div></div>
          ${rewards(m.badges, m.medals)}
          ${m.claimed ? '<span class="sub">Done</span>' : `<button type="button" class="btn" data-event="${i}"${m.done ? "" : " disabled"}>Claim</button>`}
        </div>`).join("")}</div>
      <div class="bonus"><div class="t"><b>Event bonus</b><span>Claim every mission of the event for an extra reward.</span></div>
        ${rewards(ev.bonusBadges, ev.bonusMedals)}
        ${ev.bonusClaimed ? '<span class="sub">Claimed</span>' : `<button type="button" class="btn" data-bonus="1"${allClaimed ? "" : " disabled"}>Claim bonus</button>`}</div>
    </div>`;
  }

  // ---------------------------------------------------------------- black market
  // 3D preview like the shop's battle passes (../shop/viewer.js, models from tools/export_shop_assets.py): costumes
  // worn by the player's character, zombie skins as the zombie, weapons in the character's hands
  let modelIndex = null;
  fetch("../shop/models/index.json").then(r => (r.ok ? r.json() : null)).then(j => { modelIndex = j; if (data && tab === "market") renderMarket(); }).catch(() => {});
  const stageCanvas = document.createElement("canvas");
  stageCanvas.className = "stage-canvas";
  let viewer = null, shownKey = "";

  function previewModel(it) {
    const entry = modelIndex && modelIndex.items[it.itemID];
    if (!entry) return null;
    const mine = state && modelIndex.items[state.characterItem];
    const character = (mine && mine.character && !mine.zombie && mine.character) || modelIndex.defaultCharacter;
    if (entry.costume) return { character, weapon: modelIndex.defaultWeapon, pose: "ref_aim_carbine", costume: entry.costume };
    if (entry.zombie) return { character: entry.character, weapon: null, pose: "ref_aim_knife" };
    if (entry.character) return { character: entry.character, weapon: modelIndex.defaultWeapon, pose: "ref_aim_carbine" };
    return { character, weapon: entry.weapon, pose: entry.pose || "ref_aim_carbine" };
  }

  function showStage(it) {
    const stage = $("stage");
    if (!stage) return;
    const m = previewModel(it);
    if (!m || !(viewer || (viewer = window.createViewer && createViewer(stageCanvas, "../shop/models/")))) {
      stage.classList.add("no-model");
      return;
    }
    stage.appendChild(stageCanvas);
    const key = [m.character, m.weapon, m.pose, m.costume].join("|");
    if (key === shownKey) return;
    shownKey = key;
    viewer.show(m.character, m.weapon, m.pose, m.costume).then(ok => { if (!ok) { shownKey = ""; stage.classList.add("no-model"); } });
  }

  const buyButton = it => it.owned ? '<span class="owned-tag">Owned</span>'
    : `<button type="button" class="btn${confirmBuy === it.itemID ? " confirm" : ""}" data-buy="${it.itemID}"${data.badges < it.price ? " disabled" : ""}>${confirmBuy === it.itemID ? "Confirm" : "Buy"}</button>`;

  function renderMarket() {
    if (!data.market.some(it => it.itemID === marketPick))
      marketPick = (data.market.find(it => it.kind === "Costume") || data.market[0] || {}).itemID;
    const pick = data.market.find(it => it.itemID === marketPick);
    const scroll = view.querySelector(".market-main") ? view.querySelector(".market-main").scrollTop : 0;
    view.innerHTML = `<div class="market"><div class="market-main">
      <div class="market-head"><div><h2>Black Market</h2><p>Rare gear paid with Black Badges. Missions are the only way to earn them.</p></div>
        <span class="refresh">New stock in ${timeLeft(data.refreshIn)}</span></div>
      <div class="market-grid">${data.market.map(it => `
        <div class="mitem${it.kind === "Costume" ? " costume" : ""}${it.owned ? " owned" : ""}${it.itemID === marketPick ? " selected" : ""}" data-pick="${it.itemID}">
          <div class="art"><img src="../shop/icons/${it.itemID}.png" alt="" onerror="this.outerHTML='<span class=&quot;none&quot;>No picture</span>'"></div>
          <div class="info"><div><div class="nm" title="${esc(it.name)}">${esc(it.name)}</div><div class="kind">${esc(it.kind)}</div></div>
            <div class="buy"><span class="price">${badge(true)}${num(it.price)}</span>${buyButton(it)}</div></div>
        </div>`).join("")}</div></div>
      <aside class="preview">${pick ? `
        <div class="stage" id="stage"><div class="art"><img src="../shop/icons/${pick.itemID}.png" alt="" onerror="this.remove()"></div></div>
        <div class="category">${esc(pick.kind)}${previewModel(pick) && previewModel(pick).costume ? " &middot; worn by your character" : ""}</div>
        <h3>${esc(pick.name)}</h3>
        <div class="buy-row"><div class="price">${badge()}${num(pick.price)}</div>${buyButton(pick)}</div>
        <div class="note">Drag the model to turn it.</div>` : '<div class="empty">Nothing in stock.</div>'}</aside>
    </div>`;
    view.querySelector(".market-main").scrollTop = scroll;
    if (pick) showStage(pick);
  }

  // ---------------------------------------------------------------- rift exchange
  // cosmetics paid with Rift Tickets (clearing Zombie Disaster Zone); a new stock every day, and an item can be
  // held for more days (one day per hold, up to rules.maxHoldDays) so there's time to earn its tickets
  function riftButtons(it) {
    if (it.owned) return '<span class="owned-tag">Owned</span>';
    const buy = "buy:" + it.itemID, hold = "hold:" + it.itemID;
    const holdsFull = !it.held && rift.held.length >= rift.rules.maxHolds;
    const canHold = (it.held ? it.canExtend : !holdsFull) && rift.tickets >= it.holdCost;
    const holdTitle = it.held && !it.canExtend ? `Held for the most days already (${rift.rules.maxHoldDays})`
      : holdsFull ? `You can hold ${rift.rules.maxHolds} items at most` : `Keep it one more day for ${it.holdCost} tickets`;
    return `<span class="actions">
      <button type="button" class="btn hold${riftConfirm === hold ? " confirm" : ""}" data-rhold="${it.itemID}" title="${esc(holdTitle)}"${canHold ? "" : " disabled"}>${riftConfirm === hold ? "Confirm" : it.held ? "+1 day" : "Hold"}</button>
      <button type="button" class="btn${riftConfirm === buy ? " confirm" : ""}" data-rbuy="${it.itemID}"${rift.tickets < it.price ? " disabled" : ""}>${riftConfirm === buy ? "Confirm" : "Buy"}</button></span>`;
  }

  const heldText = it => (it.daysLeft > 0 ? `Held · ${it.daysLeft} more day${it.daysLeft === 1 ? "" : "s"}` : "Held · last day");

  function riftCard(it) {
    return `<div class="mitem rift-item${it.held ? " held" : ""}${it.owned ? " owned" : ""}${it.itemID === riftPick ? " selected" : ""}" data-rpick="${it.itemID}">
      <div class="art"><img src="../shop/icons/${it.itemID}.png" alt="" onerror="this.outerHTML='<span class=&quot;none&quot;>No picture</span>'"></div>
      <div class="info"><div><div class="nm" title="${esc(it.name)}">${esc(it.name)}</div>
        <div class="kind">${esc(it.kind)}</div>${it.held ? `<div class="held-tag">${heldText(it)}</div>` : ""}</div>
        <div class="buy"><span class="price">${ticket(true)}${num(it.price)}</span>${riftButtons(it)}</div></div>
    </div>`;
  }

  function renderRift() {
    if (!rift) {
      view.innerHTML = '<div class="message">The Rift Exchange needs the updated CSNZ_Server.</div>';
      return;
    }
    const held = rift.held.map(it => Object.assign({ held: true }, it));
    const all = held.concat(rift.stock);
    if (!all.some(it => it.itemID === riftPick)) riftPick = (all.find(it => !it.owned) || all[0] || {}).itemID;
    const pick = all.find(it => it.itemID === riftPick);
    const r = rift.rules;
    const atMax = r.clearTickets + r.ticketsPerLevel * (r.maxDifficulty - 1);
    const scroll = view.querySelector(".market-main") ? view.querySelector(".market-main").scrollTop : 0;
    view.innerHTML = `<div class="market rift"><div class="market-main">
      <div class="market-head"><div><h2>Rift Exchange</h2>
        <p>Cosmetics paid with Rift Tickets. Clear Zombie Disaster Zone to earn them: ${r.clearTickets} at level 1, ${r.ticketsPerLevel} more for each level above (${atMax} at level ${r.maxDifficulty}).${rift.bestClear ? ` Your best clear: level ${rift.bestClear}.` : ""}</p></div>
        <span class="refresh">New stock in ${timeLeft(rift.refreshIn)}</span></div>
      <section class="rift-section"><h2 class="section-title">Held for you<small>${held.length} / ${r.maxHolds} · up to ${r.maxHoldDays} days each</small></h2>
        ${held.length ? `<div class="market-grid">${held.map(riftCard).join("")}</div>` : `<div class="rift-empty">Hold an item of today's stock to keep it for another day</div>`}</section>
      <section class="rift-section"><h2 class="section-title">Today's stock<small>${rift.stock.length} items</small></h2>
        ${rift.stock.length ? `<div class="market-grid">${rift.stock.map(riftCard).join("")}</div>` : '<div class="rift-empty">Nothing left to sell you</div>'}</section>
      </div>
      <aside class="preview">${pick ? `
        <div class="stage" id="stage"><div class="art"><img src="../shop/icons/${pick.itemID}.png" alt="" onerror="this.remove()"></div></div>
        <div class="category">${esc(pick.kind)}${previewModel(pick) && previewModel(pick).costume ? " &middot; worn by your character" : ""}</div>
        <h3>${esc(pick.name)}</h3>
        <div class="buy-row"><div class="price">${ticket()}${num(pick.price)}</div>${riftButtons(pick)}</div>
        <div class="hold-row"><span>${pick.held ? heldText(pick) : "Leaves at midnight unless held"}</span><span>Hold: ${ticket(true)} ${num(pick.holdCost)} / day</span></div>
        <div class="note">Holding keeps an item in your exchange for <b>one more day</b> per hold, up to ${r.maxHoldDays} days. Drag the model to turn it.</div>` : '<div class="empty">Nothing in stock.</div>'}</aside>
    </div>`;
    view.querySelector(".market-main").scrollTop = scroll;
    if (pick) showStage(pick);
  }

  // ---------------------------------------------------------------- orders and challenges (from the shop)
  const tickets = o => (o.tickets ? '<span class="ticket-reward" title="Bingo Ticket"><img src="../shop/icons/2179.png" alt="">+' + o.tickets + "</span>" : "");

  function renderOrders() {
    const column = (period, label) => {
      const list = state.orders.filter(o => o.period === period);
      if (!list.length) return "";
      return "<div><h2>" + label + "<small>Resets in " + timeLeft(list[0].resetsIn) + "</small></h2>" +
        list.map(o => '<div class="order' + (o.completed ? " done" : "") + '">' +
          '<div class="row"><span class="text">' + esc(o.text) + '</span><span class="reward">' + medal(true) + "+" + o.reward + tickets(o) + "</span></div>" +
          '<div class="progress"><span style="width:' + pct(o.progress, o.target) + '%"></span></div>' +
          '<div class="sub">' + (o.completed ? "Completed" : o.progress + " / " + o.target) + "</div></div>").join("") + "</div>";
    };
    view.innerHTML = '<div class="orders">' + column("Daily", "Daily orders") + column("Weekly", "Weekly orders") + "</div>";
  }

  function renderChallenges() {
    const list = state.orders.filter(o => o.period === "Challenge");
    const done = list.filter(o => o.completed);
    const earned = done.reduce((sum, o) => sum + o.reward, 0);
    const total = list.reduce((sum, o) => sum + o.reward, 0);
    const groups = [];
    list.forEach(o => { let g = groups.find(x => x.name === o.group); if (!g) groups.push(g = { name: o.group || "General", items: [] }); g.items.push(o); });
    const card = o => '<div class="challenge' + (o.completed ? " done" : "") + '">' +
      '<div class="row"><span class="text">' + esc(o.text) + '</span><span class="reward">' + medal(true) + o.reward + tickets(o) + "</span></div>" +
      '<div class="progress"><span style="width:' + pct(o.progress, o.target) + '%"></span></div>' +
      '<div class="sub">' + (o.completed ? "Completed" : num(o.progress) + " / " + num(o.target)) + "</div></div>";
    view.innerHTML = '<div class="challenges">' +
      '<div class="book-head"><div><h2>Challenge book</h2><p>Permanent goals. Each one pays out once.</p></div>' +
      '<div class="book-stats"><div><span class="label">Completed</span><span class="value">' + done.length + " / " + list.length + "</span></div>" +
      '<div><span class="label">Medals earned</span><span class="value">' + medal() + earned + " / " + total + "</span></div></div>" +
      '<div class="progress book-progress"><span style="width:' + pct(done.length, list.length) + '%"></span></div></div>' +
      groups.map(g => '<section class="book-group"><h3>' + esc(g.name) + "<small>" + g.items.filter(o => o.completed).length + " / " + g.items.length + "</small></h3>" +
        '<div class="book-grid">' + g.items.map(card).join("") + "</div></section>").join("") + "</div>";
  }

  function render() {
    $("badges").textContent = num(data.badges);
    $("medals").textContent = num(data.medals);
    $("tickets").textContent = rift ? num(rift.tickets) : "--";
    document.querySelectorAll("#tabs button").forEach(b => b.classList.toggle("active", b.dataset.tab === tab));
    if (tab === "event") renderEvent();
    else if (tab === "market") renderMarket();
    else if (tab === "rift") renderRift();
    else if (tab === "orders") renderOrders();
    else if (tab === "challenges") renderChallenges();
    else renderRequests();
  }

  // ---------------------------------------------------------------- events
  function close() {
    location.href = "http://127.0.0.1:1/"; // the game closes its browser window on a failed load
  }
  $("close").addEventListener("click", close);
  document.addEventListener("keydown", e => { if (e.key === "Escape") close(); });
  $("tabs").addEventListener("click", e => {
    const b = e.target.closest("[data-tab]");
    if (b && data) { tab = b.dataset.tab; confirmBuy = riftConfirm = null; render(); }
  });
  view.addEventListener("click", e => {
    const t = e.target;
    const accept = t.closest("[data-accept]"), drop = t.closest("[data-drop]"), claim = t.closest("[data-claim]");
    const evClaim = t.closest("[data-event]"), bonus = t.closest("[data-bonus]"), buy = t.closest("[data-buy]");
    if (accept) return act({ action: "accept", id: +accept.dataset.accept }, "Request accepted.");
    if (drop) return act({ action: "abandon", id: +drop.dataset.drop }, "Request dropped.");
    if (claim) return act({ action: "claim", id: +claim.dataset.claim }, "Reward claimed.");
    if (evClaim) return act({ action: "claimEvent", index: +evClaim.dataset.event }, "Reward claimed.");
    if (bonus) return act({ action: "claimBonus" }, "Event bonus claimed.");
    if (buy) {
      const id = +buy.dataset.buy;
      if (confirmBuy !== id) {
        confirmBuy = id;
        renderMarket();
        clearTimeout(act.confirmTimer);
        act.confirmTimer = setTimeout(() => { confirmBuy = null; if (tab === "market") renderMarket(); }, 3000);
        return;
      }
      confirmBuy = null;
      return act({ action: "buy", itemID: id }, "Bought. It's in your inventory.");
    }
    const rbuy = t.closest("[data-rbuy]"), rhold = t.closest("[data-rhold]");
    if (rbuy || rhold) {
      const id = +(rbuy ? rbuy.dataset.rbuy : rhold.dataset.rhold), key = (rbuy ? "buy:" : "hold:") + id;
      if (riftConfirm !== key) {
        riftConfirm = key;
        renderRift();
        clearTimeout(act.riftTimer);
        act.riftTimer = setTimeout(() => { riftConfirm = null; if (tab === "rift") renderRift(); }, 3000);
        return;
      }
      riftConfirm = null;
      return rbuy ? act({ rift: true, action: "buy", itemID: id }, "Bought. It's in your inventory.")
        : act({ rift: true, action: "hold", itemID: id }, "Held for one more day.");
    }
    const rpick = t.closest("[data-rpick]");
    if (rpick) {
      riftPick = +rpick.dataset.rpick;
      return renderRift();
    }
    const pickItem = t.closest("[data-pick]");
    if (pickItem) {
      marketPick = +pickItem.dataset.pick;
      return renderMarket();
    }
    const req = t.closest("[data-req]");
    if (req) {
      selected = +req.dataset.req;
      renderRequests();
    }
  });

  // ---------------------------------------------------------------- layout preview (not served by CSNZ_Server)
  let mock = null;
  function preview(path, body) {
    if (!mock) {
      mock = { badges: 45, medals: 1260, refreshIn: 30000, characterItem: 51,
        requests: { slots: 3, active: [], offers: [] }, market: [], event: null };
      const npcs = [["DoctorA", "Senior Researcher Soi"], ["Victor", "Victor"], ["Evelyn", "Evelyn"], ["Jacob", "Jacob"], ["Zim", "Zim"]];
      const titles = ["Glorious Victor!", "Necessity", "C4 Plan Support", "Zombie Sweep", "First Steps 3", "Virus Host"];
      const goals = ["Win 3 matches", "Score 5 headshots", "Plant or defuse 3 bombs in Original modes", "Kill 100 monsters in Zombie Scenario", "Play for 1 hours", "Infect 10 humans in Zombie (Original, Mutation, Hero)"];
      titles.forEach((t, i) => {
        const r = { id: i + 1, title: t, npc: npcs[i % 5][1], portrait: npcs[i % 5][0].toLowerCase(), story: "The world ultimately belongs to the victor.\nThat is the only thing I learned while working in various governments and organizations.", text: goals[i], badges: 3 + i, medals: 10 + i * 2 };
        if (i < 2) mock.requests.active.push(Object.assign({ progress: [{ have: i ? 5 : 1, need: i ? 5 : 3 }], done: i === 1 }, r));
        else mock.requests.offers.push(r);
      });
      mock.event = { id: 1, name: "Operation Headhunter", description: "Command wants marksmen. Every clean headshot this week counts toward the operation.", endsIn: 300000,
        bonusBadges: 25, bonusMedals: 100, bonusClaimed: false,
        missions: [{ text: "Score 50 headshots", badges: 10, medals: 30, progress: [{ have: 50, need: 50 }], done: true, claimed: true },
          { text: "Score 150 headshots", badges: 15, medals: 40, progress: [{ have: 72, need: 150 }], done: false, claimed: false },
          { text: "Score 200 rifle kills", badges: 10, medals: 30, progress: [{ have: 200, need: 200 }], done: true, claimed: false },
          { text: "Finish first 3 times", badges: 15, medals: 40, progress: [{ have: 1, need: 3 }], done: false, claimed: false }] };
      mock.market = [[9423, "Sflaser (S25-3)", 30, "Weapon"], [9426, "Ironfan (S25-3)", 30, "Weapon"], [9425, "Python (S25-3)", 30, "Weapon"],
        [9424, "M 249ex (S25-3)", 30, "Weapon"], [9108, "Balrog 7 (S24-3)", 30, "Weapon"], [9023, "Skull 2 (S24-2)", 30, "Weapon"],
        [9655, "Agenth (Pet)", 300, "Costume"], [9586, "Coralcrown (Head)", 300, "Costume"], [8928, "Beveragecan (Pelvis)", 300, "Costume"],
        [9413, "Orientalinkfan (Back)", 300, "Costume"], [8584, "Boomerskin (S21-4)", 30, "Costume"]].map(([itemID, name, price, kind], i) => ({ itemID, name, price, kind, owned: i === 4 }));
    }
    if (path === "/api/riftexchange") {
      if (!mock.rift) {
        const kinds = [[707, "Dinosaur Head", "Head", 45], [575, "Fierce Penguin", "Back", 50], [473, "Cat Tail", "Pelvis", 35],
          [8597, "S21-4 Pet Costume A", "Pet", 60], [8030, "Evolution Level 3 Jiang Shi", "Zombie Skin", 55], [949, "Paladin Crown", "Head", 45],
          [8626, "Tttiger 22", "Tattoo", 25], [8364, "Tin Man's Helmet", "Head", 45], [9124, "Turtle (Back)", "Back", 50]];
        const items = kinds.map(([itemID, name, kind, price]) => ({ itemID, name, kind, price, holdCost: Math.max(2, Math.round(price / 10)), owned: false }));
        mock.rift = { tickets: 64, refreshIn: 30000, bestClear: 12, rules: { clearTickets: 3, ticketsPerLevel: 1, maxDifficulty: 20, maxHolds: 3, maxHoldDays: 7 },
          held: [Object.assign({ daysLeft: 3, heldDays: 3, canExtend: true }, items[8])], stock: items.slice(0, 8) };
      }
      const r = mock.rift;
      if (body && body.action === "buy") {
        const it = r.stock.concat(r.held).find(m => m.itemID === body.itemID);
        r.tickets -= it.price; it.owned = true;
      }
      if (body && body.action === "hold") {
        let it = r.held.find(m => m.itemID === body.itemID);
        if (!it) {
          it = Object.assign({ daysLeft: 0, heldDays: 0, canExtend: true }, r.stock.find(m => m.itemID === body.itemID));
          r.held.push(it);
          r.stock = r.stock.filter(m => m.itemID !== body.itemID);
        }
        it.daysLeft++; it.heldDays++; it.canExtend = it.heldDays < 7; r.tickets -= it.holdCost;
      }
      return JSON.parse(JSON.stringify(r));
    }
    if (path === "/api/state")
      return { medals: mock.medals, characterItem: mock.characterItem, orders: [
        { period: "Daily", text: "Play 3 matches", reward: 10, progress: 1, target: 3, completed: false, resetsIn: 30000 },
        { period: "Weekly", text: "Win 10 matches", reward: 40, progress: 10, target: 10, completed: true, resetsIn: 300000 },
        { period: "Challenge", group: "Combat", text: "Score 1,000 kills", reward: 50, progress: 420, target: 1000, completed: false }] };
    if (body && body.action === "accept") {
      const i = mock.requests.offers.findIndex(r => r.id === body.id);
      mock.requests.active.push(Object.assign({ progress: [{ have: 0, need: 3 }], done: false }, mock.requests.offers.splice(i, 1)[0]));
    }
    if (body && body.action === "claim") {
      const i = mock.requests.active.findIndex(r => r.id === body.id);
      const r = mock.requests.active.splice(i, 1)[0];
      mock.badges += r.badges; mock.medals += r.medals;
    }
    if (body && body.action === "buy") {
      const it = mock.market.find(m => m.itemID === body.itemID);
      mock.badges -= it.price; it.owned = true;
    }
    return JSON.parse(JSON.stringify(mock));
  }

  load();
  setInterval(() => { if (!busy && confirmBuy === null && riftConfirm === null) load(); }, 20000); // progress changes after matches
})();
