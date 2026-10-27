// Service Record: the player's statistics (all modes and per mode) and the game's medals with their progress.
// Data: CSNZ_Server /api/profile (CMedalManager); medal icons in medals/<id>.png (tools/gen_medals.py).
(function () {
  "use strict";

  const MODE_NAMES = { 0: "Original", 1: "Deathmatch", 2: "Team Deathmatch", 3: "Original (AI)", 4: "Deathmatch (AI)",
    5: "Team Deathmatch (AI)", 6: "Official", 7: "Official Tie-break", 8: "Zombie", 9: "Zombie Mutation", 12: "Challenge",
    14: "Zombie Hero", 15: "Zombie Scenario", 16: "Soccer", 17: "Human Scenario", 18: "PvPvE", 19: "Item TDM",
    20: "Zombie Escape", 22: "Gun Deathmatch", 23: "Light Original", 24: "Zombie (AI)", 27: "Snowman",
    28: "Zombie Shelter", 29: "Zombie Union", 30: "Zombie Giant", 32: "Zombie Exterminate", 33: "Stand Alone",
    35: "Zombie Official", 36: "Zombie Official Tie-break", 37: "Zombie Tag", 38: "Studio", 39: "Studio PvE",
    40: "All Star", 41: "Playroom", 42: "Season Original", 43: "Season Zombie", 44: "Season Zombie Hero",
    45: "Zombie Z", 48: "Playroom 2", 49: "Prop Hunt", 50: "Partner", 51: "Zombie Hero Escape", 52: "Shelter Studio",
    53: "Scenario TX", 54: "Zombie Evolution", 55: "Playroom 3", 56: "Zombie Team Control", 57: "Super Soldier",
    58: "Zombie Classic", 59: "Zombie Scenario Rift" };

  const $ = id => document.getElementById(id);
  const view = $("view");
  let data = null;
  let tab = "overview";
  let group = "All";
  let selectedMedal = null;

  const PREVIEW = location.port !== "30080";

  function esc(s) {
    return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }
  const num = n => Number(n || 0).toLocaleString("en-US");
  const pct = (a, b) => (b ? Math.round(a * 1000 / b) / 10 : 0);
  function hours(seconds) {
    const h = Math.floor((seconds || 0) / 3600), m = Math.floor(((seconds || 0) % 3600) / 60);
    return h ? `${num(h)}<small>h</small> ${m}<small>m</small>` : `${m}<small>m</small>`;
  }
  const kd = (k, d) => (d ? (k / d).toFixed(2) : k ? num(k) : "0.00");

  function toast(text, kind) {
    const t = $("toast");
    t.textContent = text;
    t.className = "toast " + (kind || "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.add("hidden"), 2600);
  }

  async function load() {
    try {
      if (PREVIEW) {
        data = await previewData();
      } else {
        const res = await fetch("/api/profile");
        const json = await res.json().catch(() => ({}));
        if (!res.ok || json.error) throw new Error(json.error || "The server didn't answer.");
        data = json;
      }
      render();
    } catch (e) {
      view.innerHTML = `<div class="message">${esc(e.message)}<br>Log in with a character and open it again.</div>`;
    }
  }

  // ---------------------------------------------------------------- overview
  function renderOverview() {
    const t = data.total || {};
    const earned = data.medalList.filter(m => m.earned);
    const weapons = [["Rifles", t.RifleKills], ["SMGs", t.SmgKills], ["Shotguns", t.ShotgunKills], ["Machine guns", t.MgKills],
      ["Pistols", t.PistolKills], ["Melee", t.MeleeKills], ["Grenades", t.GrenadeKills]];
    const top = Math.max(1, ...weapons.map(w => w[1] || 0));
    view.innerHTML = `<div class="overview">
      <section class="idcard">
        <div class="head"><div class="rank">${data.level}</div>
          <div class="who"><div class="nm">${esc(data.name)}</div><div class="sub">Level ${data.level} &middot; ${num(data.exp)} EXP</div></div></div>
        <div class="rows">
          <div class="row"><span>Medals (currency)</span><b>${num(data.medals)}</b></div>
          <div class="row"><span>Black badges</span><b>${num(t.BlackBadges)}</b></div>
          <div class="row"><span>Medals earned</span><b>${earned.length} / ${data.medalList.length}</b></div>
          <div class="row"><span>Days played</span><b>${num(t.DaysPlayed)}</b></div>
          <div class="row"><span>Orders completed</span><b>${num(t.OrdersDone)}</b></div>
          <div class="row"><span>Battle pass items</span><b>${num(t.Unlocks)}</b></div>
        </div>
        <div class="recent"><div class="label">Latest medals</div>
          <div class="icons">${earned.slice(-6).reverse().map(m => `<img src="medals/${m.id}.png" title="${esc(m.name)}" alt="" onerror="this.style.visibility='hidden'">`).join("") || '<span class="label" style="margin-top:4px">None yet</span>'}</div></div>
      </section>
      <section class="rec-stats">
        <div class="tiles">
          <div class="rec-stat big"><div class="label">Kills</div><div class="v">${num(t.Kills)}</div></div>
          <div class="rec-stat"><div class="label">Deaths</div><div class="v">${num(t.Deaths)}</div></div>
          <div class="rec-stat"><div class="label">K/D</div><div class="v">${kd(t.Kills || 0, t.Deaths || 0)}</div></div>
          <div class="rec-stat"><div class="label">Headshots</div><div class="v">${num(t.Headshots)}<small>${pct(t.Headshots || 0, t.Kills || 0)}%</small></div></div>
          <div class="rec-stat big"><div class="label">Matches played</div><div class="v">${num(t.Matches)}</div></div>
          <div class="rec-stat"><div class="label">Matches won</div><div class="v">${num(t.Wins)}<small>${pct(t.Wins || 0, t.Matches || 0)}%</small></div></div>
          <div class="rec-stat"><div class="label">First place</div><div class="v">${num(t.TopPlayer)}</div></div>
          <div class="rec-stat big"><div class="label">Time played</div><div class="v">${hours(t.Seconds)}</div></div>
          <div class="rec-stat"><div class="label">Best match</div><div class="v">${num(t.BestKills)}<small>kills</small></div></div>
          <div class="rec-stat"><div class="label">AI soldiers defeated</div><div class="v">${num(t.BotKills)}</div></div>
          <div class="rec-stat"><div class="label">Zombies killed</div><div class="v">${num(t.ZombieKills)}</div></div>
          <div class="rec-stat"><div class="label">Humans infected</div><div class="v">${num(t.Infections)}</div></div>
        </div>
        <div class="weapons"><div class="label">Kills by weapon</div>
          <div class="bars">${weapons.map(w => `<span class="name">${w[0]}</span><div class="bar"><span style="width:${(w[1] || 0) * 100 / top}%"></span></div><span class="n">${num(w[1])}</span>`).join("")}</div></div>
      </section>
    </div>`;
  }

  // ---------------------------------------------------------------- medals
  function medalProgress(m) {
    if (m.earned) return 1;
    const parts = m.progress.map(p => (p.need ? Math.min(1, p.have / p.need) : 1));
    return parts.length ? parts.reduce((a, b) => a + b, 0) / parts.length : 0;
  }

  function renderMedals() {
    const list = data.medalList;
    const groups = ["All", ...data.groups.filter(g => list.some(m => m.group === g))];
    if (!groups.includes(group)) group = "All";
    const shown = list.filter(m => group === "All" || m.group === group);
    if (!shown.some(m => m.id === selectedMedal)) selectedMedal = shown.length ? shown[0].id : null;
    const count = g => {
      const l = list.filter(m => g === "All" || m.group === g);
      return `${l.filter(m => m.earned).length}/${l.length}`;
    };
    view.innerHTML = `<div class="medals">
      <aside class="medal-side">
        <div class="sum"><div class="label">Medals earned</div><b>${list.filter(m => m.earned).length}</b> <span class="label">of ${list.length}</span></div>
        ${groups.map(g => `<button type="button" class="group-btn${g === group ? " active" : ""}" data-group="${esc(g)}">${esc(g)}<small>${count(g)}</small></button>`).join("")}
      </aside>
      <div id="mgrid" class="medal-grid">${shown.map(m => `
        <button type="button" class="mtile ${m.earned ? "earned" : "locked"}${m.id === selectedMedal ? " selected" : ""}" data-id="${m.id}" title="${esc(m.name)}">
          <img src="medals/${m.id}.png" alt="" loading="lazy" onerror="this.style.visibility='hidden'">
          <span class="nm">${esc(m.name)}</span>
          ${m.earned ? "" : `<div class="progress"><span style="width:${medalProgress(m) * 100}%"></span></div>`}
        </button>`).join("")}</div>
      <aside id="mdetail" class="medal-detail"></aside>
    </div>`;
    renderMedalDetail();
  }

  function renderMedalDetail() {
    const el = $("mdetail");
    const m = data.medalList.find(x => x.id === selectedMedal);
    if (!m) {
      el.innerHTML = `<div class="empty">Pick a medal to see how to earn it.</div>`;
      return;
    }
    el.className = "medal-detail" + (m.earned ? "" : " locked");
    const texts = m.text.split("; ");
    el.innerHTML = `<img class="big" src="medals/${m.id}.png" alt="" onerror="this.style.visibility='hidden'">
      <h2>${esc(m.name)}</h2>
      <div class="state${m.earned ? " ok" : ""}">${m.earned ? "Earned &middot; wear it from My Info" : esc(m.group) + " medal"}</div>
      ${m.progress.map((p, i) => `<div class="cond"><span class="t">${esc(texts[i] || texts[0] || "")}</span>
        <div class="progress"><span style="width:${m.earned ? 100 : (p.need ? Math.min(100, p.have * 100 / p.need) : 100)}%"></span></div>
        <span class="n"><span>${m.earned ? "Complete" : "Progress"}</span><span>${num(m.earned ? p.need : p.have)} / ${num(p.need)}</span></span></div>`).join("")}
      ${m.lore ? `<div class="lore">${esc(m.lore)}</div>` : ""}`;
  }

  // ---------------------------------------------------------------- modes
  function renderModes() {
    const rows = Object.entries(data.modes || {}).map(([mode, c]) => ({ mode: +mode, c }))
      .filter(r => r.c.Matches || r.c.Kills || r.c.Seconds)
      .sort((a, b) => (b.c.Seconds || 0) - (a.c.Seconds || 0));
    if (!rows.length) {
      view.innerHTML = `<div class="message">No matches yet. Statistics count from now on, mode by mode.</div>`;
      return;
    }
    view.innerHTML = `<div class="pane"><table class="modes-table">
      <thead><tr><th>Mode</th><th>Matches</th><th>Won</th><th>Win %</th><th>Kills</th><th>Deaths</th><th>K/D</th><th>Headshots</th><th>Best</th><th>Time</th></tr></thead>
      <tbody>${rows.map(({ mode, c }) => `<tr><td class="mode">${esc(MODE_NAMES[mode] || "Mode " + mode)}</td>
        <td>${num(c.Matches)}</td><td>${num(c.Wins)}</td><td>${pct(c.Wins || 0, c.Matches || 0)}%</td><td>${num(c.Kills)}</td>
        <td>${num(c.Deaths)}</td><td>${kd(c.Kills || 0, c.Deaths || 0)}</td><td>${num(c.Headshots)}</td><td>${num(c.BestKills)}</td>
        <td>${hours(c.Seconds).replace(/<\/?small>/g, "")}</td></tr>`).join("")}</tbody></table></div>`;
  }

  function render() {
    $("currency").textContent = num(data.medals);
    document.querySelectorAll("#tabs button").forEach(b => b.classList.toggle("active", b.dataset.tab === tab));
    if (tab === "medals") renderMedals();
    else if (tab === "modes") renderModes();
    else renderOverview();
  }

  // ---------------------------------------------------------------- events
  function close() {
    location.href = "http://127.0.0.1:1/"; // the game closes its browser window on a failed load
  }
  $("close").addEventListener("click", close);
  document.addEventListener("keydown", e => { if (e.key === "Escape") close(); });
  $("tabs").addEventListener("click", e => {
    const b = e.target.closest("[data-tab]");
    if (b && data) { tab = b.dataset.tab; render(); }
  });
  view.addEventListener("click", e => {
    const g = e.target.closest("[data-group]");
    if (g) { group = g.dataset.group; renderMedals(); return; }
    const m = e.target.closest(".mtile");
    if (m) {
      selectedMedal = +m.dataset.id;
      document.querySelectorAll(".mtile").forEach(x => x.classList.toggle("selected", x === m));
      renderMedalDetail();
    }
  });

  // not served by CSNZ_Server (layout preview): a made-up player with the real medal table
  async function previewData() {
    let medals = [];
    try {
      const cfg = await fetch("medals/preview.json").then(r => r.json());
      medals = cfg.Medals;
    } catch (e) { /* no medal table */ }
    const total = { Kills: 4821, Deaths: 2210, Headshots: 1093, Matches: 312, Wins: 171, TopPlayer: 64, Seconds: 151230,
      BestKills: 47, BotKills: 4402, ZombieKills: 820, Infections: 233, RifleKills: 2100, SmgKills: 640, ShotgunKills: 310,
      MgKills: 980, PistolKills: 420, MeleeKills: 290, GrenadeKills: 81, DaysPlayed: 12, OrdersDone: 37, Unlocks: 44, BlackBadges: 120 };
    return { name: "Player", level: 34, exp: 312440, medals: 1260, titles: [0, 0, 0, 0, 0], total,
      modes: { 2: { Matches: 140, Wins: 80, Kills: 2900, Deaths: 1200, Headshots: 610, BestKills: 47, Seconds: 70000 },
        8: { Matches: 60, Wins: 31, Kills: 900, Deaths: 400, Headshots: 200, BestKills: 30, Seconds: 30000 },
        15: { Matches: 20, Wins: 12, Kills: 700, Deaths: 90, Headshots: 150, BestKills: 60, Seconds: 25000 } },
      groups: [...new Set(medals.map(m => m.Group))],
      medalList: medals.map((m, i) => ({ id: m.ID, name: m.Name, group: m.Group, text: m.Text, lore: m.Lore, earned: i % 7 === 0,
        progress: m.Conditions.map(c => ({ have: Math.floor((c.Target || c.Level || c.Count || c.Medals || c.Points || 1) * ((i * 37) % 100) / 100),
          need: c.Target || c.Level || c.Count || c.Medals || c.Points || 1 })) })) };
  }

  load();
})();
