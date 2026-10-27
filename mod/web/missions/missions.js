// Missions: requests from the game's quest NPCs, the event of the week and the Black Market (CSNZ_Server
// /api/missions, CMissionManager), plus the daily/weekly orders and the challenge book (/api/state, CWarbondManager).
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

  const PREVIEW = location.port !== "30080";

  function esc(s) {
    return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  const num = n => Number(n || 0).toLocaleString("en-US");
  const pct = (a, b) => (b ? Math.min(100, Math.round(a * 100 / b)) : 0);
  const medal = small => '<span class="medal-icon' + (small ? " small" : "") + '"></span>';
  const badge = small => '<span class="badge-icon' + (small ? " small" : "") + '"></span>';
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
      [data, state] = await Promise.all([api("/api/missions"), api("/api/state")]);
      render();
    } catch (e) {
      if (!data) view.innerHTML = `<div class="message">${esc(e.message)}<br>Log in with a character and open it again.</div>`;
    }
  }

  async function act(body, okText) {
    if (busy) return;
    busy = true;
    try {
      data = await api("/api/missions", body);
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
  function renderMarket() {
    view.innerHTML = `<div class="market">
      <div class="market-head"><div><h2>Black Market</h2><p>Rare gear paid with Black Badges. Missions are the only way to earn them.</p></div>
        <span class="refresh">New stock in ${timeLeft(data.refreshIn)}</span></div>
      <div class="market-grid">${data.market.map(it => `
        <div class="mitem${it.kind === "Costume" ? " costume" : ""}${it.owned ? " owned" : ""}">
          <div class="art"><img src="../shop/icons/${it.itemID}.png" alt="" onerror="this.outerHTML='<span class=&quot;none&quot;>No picture</span>'"></div>
          <div class="info"><div><div class="nm" title="${esc(it.name)}">${esc(it.name)}</div><div class="kind">${esc(it.kind)}</div></div>
            <div class="buy"><span class="price">${badge(true)}${num(it.price)}</span>
              ${it.owned ? '<span class="owned-tag">Owned</span>' : `<button type="button" class="btn${confirmBuy === it.itemID ? " confirm" : ""}" data-buy="${it.itemID}"${data.badges < it.price ? " disabled" : ""}>${confirmBuy === it.itemID ? "Confirm" : "Buy"}</button>`}</div></div>
        </div>`).join("")}</div>
    </div>`;
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
    document.querySelectorAll("#tabs button").forEach(b => b.classList.toggle("active", b.dataset.tab === tab));
    if (tab === "event") renderEvent();
    else if (tab === "market") renderMarket();
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
    if (b && data) { tab = b.dataset.tab; confirmBuy = null; render(); }
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
      mock = { badges: 45, medals: 1260, refreshIn: 30000,
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
        [9655, "Agenth (Pet)", 300, "Costume"], [9586, "Coralcrown (Head)", 300, "Costume"]].map(([itemID, name, price, kind], i) => ({ itemID, name, price, kind, owned: i === 4 }));
    }
    if (path === "/api/state")
      return { medals: mock.medals, orders: [
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
  setInterval(() => { if (!busy && confirmBuy === null) load(); }, 20000); // progress changes after matches
})();
