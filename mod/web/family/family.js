// The family page: level, EXP, Family Points, bonuses, members, chronicle and the game's Family achievements.
// Data: CSNZ_Server's /api/family (manager/familymanager.cpp). Opened by the family window's "View Benefits" button
// (botassist.asi), which in this client opened the missions window.
(function () {
  "use strict";

  const $ = id => document.getElementById(id);
  const CATEGORIES = [
    { id: -1, label: "All" }, { id: 1, label: "Original" }, { id: 2, label: "Zombie" }, { id: 3, label: "Scenario" },
    { id: 4, label: "Deathmatch" }, { id: 0, label: "Family" }, { id: 9, label: "Event" }];
  const GRADES = { 0: "Master", 1: "Admin", 2: "Regular", 3: "Associate" };
  let data = null;
  let tab = -1;
  try { tab = Number(localStorage.getItem("family.tab") || -1); } catch (e) { tab = -1; }

  // not served by CSNZ_Server (layout preview): a made-up family
  const PREVIEW = location.port !== "30080";
  const mock = {
    inFamily: true, name: "QSAE", master: "test1", level: 3, maxLevel: 50, expInto: 1630, expNeed: 4608, exp: 8670,
    points: 412, bonus: 10, baseExpBonus: 5, basePointBonus: 5, levelMedals: 100, score: 40, done: 3,
    rules: { expMatch: 20, expWin: 15, expKill: 1, expMaxKills: 60, pointsMatch: 5, pointsWin: 5, killsPerPoint: 5 },
    members: [{ name: "test1", grade: 0, online: true }, { name: "Omen", grade: 2, online: false }],
    chronicle: [{ date: 20261119, text: "Complete the Trap" }, { date: 20261120, text: "Family Level 3" }],
    weekEnds: 20261123,
    weekly: [
      { id: 1, text: "Play 40 matches as a family", target: 40, progress: 40, done: true, claimed: false, medals: 60, tickets: 2, familyExp: 400, familyPoints: 80, items: [] },
      { id: 3, text: "Get 800 kills as a family", target: 800, progress: 312, done: false, claimed: false, medals: 100, tickets: 0, familyExp: 600, familyPoints: 120, items: [] },
      { id: 10, text: "Earn 3,000 Family EXP this week", target: 3000, progress: 3000, done: true, claimed: true, medals: 150, tickets: 3, familyExp: 0, familyPoints: 150, items: [{ id: 396, name: "Code A Decoder", count: 1, days: 0 }] }],
    achievements: [
      { id: 26, key: "cso_zbs_trap", name: "Complete the Trap", text: "Clear the mission with your family\n(every real player in the match from the family)", category: 3, parent: 30, target: 1, progress: 1, done: 20261119, score: 10, medals: 30, buff: 0, items: [{ id: 624, name: "Mid-Level Power Amplifier", count: 1, days: 30 }], children: false },
      { id: 30, key: "cso_zbs_season1", name: "Complete Zombie Scen. S1", text: "", category: 3, parent: 0, target: 6, progress: 1, done: 0, score: 70, medals: 210, buff: 0, items: [{ id: 173, name: "Advanced Decoder", count: 3, days: 0 }], children: true },
      { id: 4, key: "cso_ori_ctwinner", name: "CT Winner", text: "Win Original 100 times as CT\n(every real player in the match from the family)", category: 1, parent: 6, target: 100, progress: 37, done: 0, score: 10, medals: 30, buff: 0, items: [{ id: 676, name: "Item Duration Extender", count: 20, days: 0 }], children: false },
      { id: 3, key: "cso_ori_ruler", name: "Original Ruler", text: "", category: 1, parent: 0, target: 2, progress: 0, done: 0, score: 20, medals: 60, buff: 10, items: [], children: true }]
  };

  function toast(text, bad) {
    const t = $("toast");
    t.textContent = text;
    t.className = "toast" + (bad ? " bad" : "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.add("hidden"), 2600);
  }

  function close() {
    location.href = "http://127.0.0.1:1/"; // the game closes its browser window on a failed load
  }

  const fmt = n => Number(n || 0).toLocaleString("en-US");
  const date = d => d ? String(d).replace(/^(\d{4})(\d{2})(\d{2})$/, "$3/$2/$1") : "";

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function renderHead() {
    $("famname").textContent = data.name + (data.master ? "  ·  master " + data.master : "");
    $("level").textContent = data.level;
    $("levelof").textContent = "of " + data.maxLevel;
    const pct = data.expNeed ? Math.min(100, 100 * data.expInto / data.expNeed) : 100;
    $("levelfill").style.width = pct + "%";
    $("levelexp").textContent = data.expNeed ? fmt(data.expInto) + " / " + fmt(data.expNeed) + " EXP" : "Highest level";
    $("levelnext").textContent = fmt(data.exp) + " Family EXP in all";
    $("points").textContent = fmt(data.points);
    $("achieved").textContent = data.done + " / " + data.achievements.length;
    $("score").textContent = fmt(data.score);
    $("members").textContent = data.members.length;
    $("bexp").textContent = "+" + (data.baseExpBonus + data.bonus) + "% (family +" + data.baseExpBonus + "%, achievements +" + data.bonus + "%)";
    $("bpoint").textContent = "+" + (data.basePointBonus + data.bonus) + "% (family +" + data.basePointBonus + "%, achievements +" + data.bonus + "%)";
    $("blevel").textContent = data.expNeed ? fmt(data.levelMedals) + " medals for every member online at level " + (data.level + 1) : "Highest level reached";
    const r = data.rules;
    $("rules").innerHTML = "";
    const lines = [
      ["Each match", "+" + r.expMatch + " EXP, +" + r.pointsMatch + " points"],
      ["A win", "+" + r.expWin + " EXP, +" + r.pointsWin + " points"],
      ["Kills", "+" + r.expKill + " EXP each (up to " + r.expMaxKills + "), +1 point every " + r.killsPerPoint],
      ["Achievements", "their EXP and the bonuses above"]];
    lines.forEach(([k, v]) => { const d = el("div"); d.appendChild(el("b", "", k + ": ")); d.appendChild(document.createTextNode(v)); $("rules").appendChild(d); });
  }

  function renderMembers() {
    const list = $("memberlist");
    list.innerHTML = "";
    data.members.slice().sort((a, b) => (b.online - a.online) || (a.grade - b.grade)).forEach(m => {
      const row = el("div", "member");
      row.appendChild(el("span", "dot" + (m.online ? " on" : "")));
      row.appendChild(el("span", "", m.name));
      row.appendChild(el("span", "grade", GRADES[m.grade] || ""));
      list.appendChild(row);
    });
    const chron = $("chronicle");
    chron.innerHTML = "";
    if (!data.chronicle.length)
      chron.appendChild(el("div", "empty", "Nothing yet: achievements and new levels show here."));
    data.chronicle.slice().reverse().forEach(c => {
      const row = el("div", "chron");
      row.appendChild(el("span", "chron-d", date(c.date)));
      row.appendChild(el("span", "", c.text));
      chron.appendChild(row);
    });
  }

  function renderTabs() {
    const tabs = $("tabs");
    tabs.innerHTML = "";
    CATEGORIES.forEach(c => {
      const n = c.id < 0 ? data.achievements.length : data.achievements.filter(a => a.category === c.id).length;
      if (!n) return;
      const b = el("button", c.id === tab ? "on" : "", c.label);
      b.type = "button";
      b.addEventListener("click", () => {
        tab = c.id;
        try { localStorage.setItem("family.tab", String(tab)); } catch (e) { /* (no storage) */ }
        renderTabs();
        renderAchievements();
      });
      tabs.appendChild(b);
    });
  }

  function rewardText(a) {
    const parts = a.items.map(i => i.name + (i.count > 1 ? " x" + i.count : "") + (i.days ? " (" + i.days + " days)" : ""));
    parts.push(fmt(a.medals) + " medals");
    if (a.buff) parts.push("Family EXP/Point bonus +" + a.buff + "%");
    return parts.join(", ");
  }

  function renderWeekly() {
    $("weekends").textContent = data.weekEnds ? "New missions on " + date(data.weekEnds) : "";
    const list = $("weeklist");
    list.innerHTML = "";
    (data.weekly || []).forEach(m => {
      const card = el("div", "week" + (m.done ? " done" : "") + (m.claimed ? " claimed" : ""));
      const top = el("div", "ach-top");
      top.appendChild(el("span", "ach-name", m.text));
      card.appendChild(top);
      const bar = el("div", "ach-bar");
      const fill = el("div", "ach-fill");
      fill.style.width = Math.min(100, 100 * m.progress / m.target) + "%";
      bar.appendChild(fill);
      card.appendChild(bar);
      const parts = [fmt(m.medals) + " medals"];
      if (m.tickets) parts.push(m.tickets + " Bingo Ticket" + (m.tickets === 1 ? "" : "s"));
      m.items.forEach(i => parts.push(i.name + (i.count > 1 ? " x" + i.count : "") + (i.days ? " (" + i.days + " days)" : "")));
      const rw = el("div", "ach-reward");
      rw.appendChild(el("span", "", "You get: "));
      rw.appendChild(document.createTextNode(parts.join(", ")));
      card.appendChild(rw);
      if (m.familyExp || m.familyPoints) {
        const fam = el("div", "ach-reward");
        fam.appendChild(el("span", "", "The family gets: "));
        fam.appendChild(document.createTextNode([m.familyExp ? fmt(m.familyExp) + " Family EXP" : "", m.familyPoints ? fmt(m.familyPoints) + " Family Points" : ""].filter(Boolean).join(", ")));
        card.appendChild(fam);
      }
      const foot = el("div", "week-foot");
      foot.appendChild(el("span", "ach-prog", fmt(m.progress) + " / " + fmt(m.target)));
      if (m.claimed) foot.appendChild(el("span", "week-state", "Claimed"));
      else if (m.done) {
        const b = el("button", "cta", "Claim");
        b.type = "button";
        b.addEventListener("click", () => claim(m.id, b));
        foot.appendChild(b);
      } else foot.appendChild(el("span", "week-state", "In progress"));
      card.appendChild(foot);
      list.appendChild(card);
    });
  }

  async function claim(id, button) {
    button.disabled = true;
    try {
      let fresh;
      if (PREVIEW) {
        mock.weekly.forEach(m => { if (m.id === id) m.claimed = true; });
        fresh = Object.assign(JSON.parse(JSON.stringify(mock)), { claimed: true });
      } else {
        const res = await fetch("/api/family", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ action: "claim", id }) });
        fresh = await res.json();
      }
      if (fresh.error) toast(fresh.error, true);
      else toast(fresh.decoder ? "Reward claimed, and a " + fresh.decoder + "!" : "Reward claimed!");
      if (fresh.inFamily) {
        data = fresh;
        renderHead();
        renderWeekly();
      }
    } catch (e) {
      toast("The server didn't answer.", true);
      button.disabled = false;
    }
  }

  function renderAchievements() {
    const list = $("achlist");
    list.innerHTML = "";
    const byId = {};
    data.achievements.forEach(a => { byId[a.id] = a; });
    data.achievements
      .filter(a => tab < 0 || a.category === tab)
      .sort((a, b) => (!!a.done - !!b.done) || (b.progress / b.target - a.progress / a.target) || a.id - b.id)
      .forEach(a => {
        const card = el("div", "ach" + (a.done ? " done" : "") + (a.children ? " group" : ""));
        const top = el("div", "ach-top");
        top.appendChild(el("span", "ach-name", a.name));
        top.appendChild(el("span", "ach-score", a.score + " pts"));
        card.appendChild(top);
        let text = a.text;
        if (a.children)
          text = "Complete: " + data.achievements.filter(x => x.parent === a.id).map(x => x.name).join(", ");
        if (text) card.appendChild(el("div", "ach-text", text));
        const bar = el("div", "ach-bar");
        const fill = el("div", "ach-fill");
        fill.style.width = Math.min(100, 100 * a.progress / a.target) + "%";
        bar.appendChild(fill);
        card.appendChild(bar);
        const prog = el("div", "ach-prog");
        prog.appendChild(el("span", "", fmt(a.progress) + " / " + fmt(a.target)));
        prog.appendChild(el("span", "", a.done ? "Done " + date(a.done) : (byId[a.parent] ? "Part of " + byId[a.parent].name : "")));
        card.appendChild(prog);
        const rw = el("div", "ach-reward");
        rw.appendChild(el("span", "", "Reward: "));
        rw.appendChild(document.createTextNode(rewardText(a)));
        card.appendChild(rw);
        list.appendChild(card);
      });
  }

  async function load() {
    try {
      if (PREVIEW) data = JSON.parse(JSON.stringify(mock));
      else {
        const res = await fetch("/api/family");
        data = await res.json();
      }
    } catch (e) {
      toast("The server didn't answer.", true);
      return;
    }
    if (!data || !data.inFamily) {
      $("main").classList.add("hidden");
      $("nofamily").classList.remove("hidden");
      return;
    }
    renderHead();
    renderMembers();
    renderWeekly();
    renderTabs();
    renderAchievements();
  }

  $("close").addEventListener("click", close);
  document.addEventListener("keydown", e => { if (e.key === "Escape") close(); });
  load();
})();
