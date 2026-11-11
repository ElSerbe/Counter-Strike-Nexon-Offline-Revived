// Acquisition Centre: battle passes bought with medals (orders and challenges are on the Missions page).
// Data comes from CSNZ_Server (/api/state, /api/unlock).
(function () {
  const view = document.getElementById("view");
  const titleEl = document.getElementById("title");
  const backBtn = document.getElementById("back");
  const tabsEl = document.getElementById("tabs");
  const medalsEl = document.getElementById("medals");
  const toastEl = document.getElementById("toast");
  const tooltipEl = document.getElementById("tooltip");
  const closeBtn = document.getElementById("close");

  let state = null;
  let tab = "passes";
  let selectedPass = null;   // id highlighted in the list
  let openPass = null;       // id of the pass being browsed
  let selectedItem = null;   // item id selected inside the open pass
  let busy = false;

  // ---------- 3D preview (viewer.js + models exported by tools/export_shop_assets.py) ----------
  let modelIndex = null;
  fetch("models/index.json").then(r => (r.ok ? r.json() : null)).then(j => { modelIndex = j; if (state) render(); }).catch(() => {});
  const stageCanvas = document.createElement("canvas");
  stageCanvas.className = "stage-canvas";
  let viewer = null, shownKey = "";

  const POSES = { Pistols: "ref_aim_onehanded", Shotguns: "ref_aim_shotgun", SMGs: "ref_aim_mp5", Rifles: "ref_aim_rifle",
    "Machine guns": "ref_aim_m249", Melee: "ref_aim_knife", Equipment: "ref_aim_grenade" };

  // what to show for an item: a character holding it, or the item's character holding the default weapon
  function previewModel(it) {
    const entry = modelIndex && modelIndex.items[it.id];
    if (!entry) return null;
    if (entry.character) return { character: entry.character, weapon: modelIndex.defaultWeapon, pose: "ref_aim_carbine" };
    const mine = modelIndex.items[state.characterItem];
    const character = (mine && mine.character) || modelIndex.defaultCharacter;
    // the weapon's own animation from the game (tools/export_shop_assets.py), else one by category
    const pose = entry.pose || (/m134/i.test(entry.weapon) ? "ref_aim_m134" : (POSES[it.category] || "ref_aim_carbine"));
    return { character, weapon: entry.weapon, pose };
  }

  function showStage(it) {
    const stage = document.getElementById("stage");
    if (!stage) { if (viewer) viewer.stop(); shownKey = ""; return; }
    const m = previewModel(it);
    if (!m || !(viewer || (viewer = createViewer(stageCanvas)))) {
      stage.classList.add("no-model");
      return;
    }
    stage.appendChild(stageCanvas);
    const key = m.character + "|" + m.weapon + "|" + m.pose;
    if (key === shownKey) return;
    shownKey = key;
    viewer.show(m.character, m.weapon, m.pose).then(ok => { if (!ok) { shownKey = ""; stage.classList.add("no-model"); } });
  }

  const esc = s => String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const medal = (small) => '<span class="medal-icon' + (small ? " small" : "") + '"></span>';
  // Bingo Tickets an order gives besides its medals (the game's own ticket picture)
  const pct = (a, b) => (b ? Math.min(100, Math.round(a * 100 / b)) : 0);
  // a rotating pass's time until its new stock: "5h 20m", "12m"
  const restock = s => { const h = Math.floor(s / 3600), m = Math.max(1, Math.floor(s % 3600 / 60)); return h ? h + "h " + m + "m" : m + "m"; };

  // Real item picture exported from the game files (tools/export_shop_assets.py), silhouette if missing.
  const itemImage = it =>
    '<img class="icon" src="icons/' + it.id + '.png" alt="" onerror="this.nextElementSibling.style.display=\'flex\';this.remove()">' +
    '<div class="fallback" style="display:none">' + itemArt(it.category) + "</div>";

  // Item sheets like the native inventory tooltips (data from iteminfo.json, written by tools/export_shop_assets.py)
  let itemInfo = {};
  fetch("iteminfo.json").then(r => (r.ok ? r.json() : {})).then(j => { itemInfo = j || {}; if (state) render(); }).catch(() => {});

  const GRADES = { 5: "SS", 4: "S", 3: "A", 2: "B", 1: "C" };
  const bar = (label, value, max, suffix) =>
    '<div class="stat-row"><span>' + label + '</span><span class="bar"><span style="width:' + Math.max(0, Math.min(100, value * 100 / max)) + '%"></span></span>' +
    '<span class="num">' + value + (suffix || "") + "</span></div>";
  const num = (label, value) => '<div class="stat-row plain"><span>' + label + '</span><span></span><span class="num">' + value + "</span></div>";

  // withDesc: tooltips show the description; the preview panel has no room for it
  function sheetHtml(it, withDesc) {
    const info = itemInfo[it.id] || {};
    let html = "";
    if (info.weapon) {
      const w = info.weapon, g = info.grades || [];
      if (g.some(x => x))
        html += '<div class="grades">' + ["Normal", "Zombie", "Scenario"].map((mode, i) =>
          '<div class="grade"><b class="g' + (g[i] || 0) + '">' + (GRADES[g[i]] || "-") + "</b><span>" + mode + "</span></div>").join("") + "</div>";
      if (w.damage || w.accuracy || w.rate)
        html += '<div class="stats-list">' +
          num("Damage", w.damage) + (w.zombieDamage ? num("Zombie damage", w.zombieDamage) : "") +
          (w.scenarioDamage ? num("Scenario damage", w.scenarioDamage) : "") +
          bar("Accuracy", w.accuracy, 100, "%") + bar("Recoil", w.recoil, 100, "%") + bar("Fire rate", w.rate, 100, "%") +
          bar("Weight", w.weight, 100, "%") + (w.knockback ? bar("Knockback", w.knockback, 100, "%") : "") +
          (w.stun ? bar("Stun", w.stun, 100, "%") : "") + "</div>";
      if (w.upgrades) html += '<div class="sheet-note">Max. upgrades: ' + w.upgrades + "</div>";
    } else if (info.character) {
      const c = info.character;
      html += '<div class="stats-list">' + bar("HP", c.hp, 30) + bar("Attack", c.attack, 30) + bar("Speed", c.speed, 30) +
        bar("Protection", c.protection, 30) + bar("Ammo", c.ammo, 30) + "</div>";
    }
    if (withDesc && info.desc) html += '<p class="desc-text">' + esc(info.desc).replace(/\n/g, "<br>") + "</p>";
    return html;
  }
  const teamBadge = it => { const t = (itemInfo[it.id] || {}).team; return t ? '<span class="team ' + t.toLowerCase() + '">' + t + "</span>" : ""; };

  // The game closes the shop window when the page fails to load (see apply_patches.py), so closing means
  // navigating to a local port nobody listens on.
  function closeShop() {
    location.href = "http://127.0.0.1:1/";
  }

  function toast(text, kind) {
    toastEl.textContent = text;
    toastEl.className = "toast " + (kind || "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => toastEl.classList.add("hidden"), 2600);
  }

  async function api(path, body) {
    const res = await fetch(path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
    const data = await res.json().catch(() => ({ error: "Unexpected server response." }));
    if (!res.ok || data.error) throw new Error(data.error || "Request failed.");
    return data;
  }

  async function load() {
    try {
      const fresh = await api("/api/state");
      // the game's browser window opened by another button: the room's "AI settings" (AI player editor), the
      // taskbar's VIP (profile) and Missions buttons
      const views = { bots: "/bots/", profile: "/profile/", missions: "/missions/" };
      if (views[fresh.openView]) {
        location.replace(views[fresh.openView]);
        return;
      }
      state = fresh;
      if (selectedPass === null && state.warbonds.length) selectedPass = state.warbonds[0].id;
      render();
    } catch (e) {
      // a failed background refresh keeps what's on screen; only the first load shows the error
      if (!state) view.innerHTML = '<div class="message">' + esc(e.message) + "<br>Log in with a character and open the shop again.</div>";
    }
  }

  const passById = id => state.warbonds.find(w => w.id === id);

  function render() {
    medalsEl.textContent = state.medals.toLocaleString("en-US");
    const inPass = openPass !== null;
    backBtn.classList.toggle("hidden", !inPass);
    tabsEl.classList.toggle("hidden", inPass);
    tabsEl.querySelectorAll("button").forEach(b => b.classList.toggle("active", b.dataset.tab === tab));
    if (inPass) renderPass(passById(openPass));
    else renderCentre();
    if (!inPass) showStage(null);
  }

  // ---------- Acquisition Centre ----------
  let listScroll = 0;
  function renderCentre() {
    titleEl.textContent = "Acquisition Centre";
    if (!state.warbonds.length) {
      view.innerHTML = '<div class="message">No battle passes available.</div>';
      return;
    }
    const pass = passById(selectedPass) || state.warbonds[0];
    const passTag = w => !w.cost ? '<span class="pass-tag free">Free</span>' : !w.purchased ? '<span class="pass-tag locked">' + medal(true) + w.cost + "</span>" : "";
    const list = state.warbonds.map(w =>
      '<button type="button" class="pass-card' + (w.id === pass.id ? " selected" : "") + (w.purchased ? "" : " not-owned") + '" data-pass="' + w.id + '">' +
        passArt(w) +
        '<div class="info"><div class="name">' + esc(w.name) + '</div>' +
        '<div class="meta"><span class="badge ' + esc(w.type.toLowerCase()) + '">' + esc(w.type) + "</span>" + passTag(w) +
        '<span>' + (w.resetsIn !== undefined ? "New in " + restock(w.resetsIn) : w.total ? w.owned + " / " + w.total : "Supplies") + '</span></div>' +
        '<div class="progress mini-progress"><span style="width:' + pct(w.owned, w.total) + '%"></span></div></div>' +
      "</button>").join("");

    const html =
      '<div class="centre"><div class="pass-list">' + list + "</div>" +
      '<section class="featured">' +
        '<div class="banner">' + passArt(pass) +
          '<div class="caption"><span class="badge ' + esc(pass.type.toLowerCase()) + '">' + esc(pass.type) + "</span>" +
          "<h2>" + esc(pass.name) + "</h2></div></div>" +
        '<p class="desc">' + esc(pass.description) + "</p>" +
        (pass.resetsIn !== undefined ? '<p class="restock">New weapons in ' + restock(pass.resetsIn) + "</p>" : "") +
        '<div class="stats">' +
          '<div class="stat grow"><div class="label">Unlocked</div><div class="value">' + pass.owned + " / " + pass.total + "</div>" +
            '<div class="progress" style="margin-top:8px"><span style="width:' + pct(pass.owned, pass.total) + '%"></span></div></div>' +
          '<div class="stat"><div class="label">Pages</div><div class="value">' + pass.pages.filter(p => p.unlocked).length + " / " + pass.pages.length + "</div></div>" +
          '<div class="stat"><div class="label">Medals spent</div><div class="value">' + medal() + pass.spent + "</div></div>" +
        "</div>" +
        '<div class="featured-actions">' +
          (pass.purchased ? "" :
            '<div class="price">' + medal() + pass.cost + "</div>" +
            '<button type="button" class="cta hold" id="unlock-pass"' + (state.medals < pass.cost ? " disabled" : "") + '><span class="fill"></span><span class="label">' +
              (state.medals < pass.cost ? "Not enough medals" : "Hold to unlock battle pass") + "</span></button>") +
          '<button type="button" class="cta' + (pass.purchased ? "" : " ghost") + '" data-open="' + pass.id + '">' + (pass.purchased ? "View battle pass" : "Preview") + "</button>" +
        "</div>" +
        (pass.purchased ? "" : '<p class="pass-note">Unlock the battle pass with medals, then unlock its items one by one.</p>') +
      "</section></div>";

    const oldList = view.querySelector(".pass-list");
    if (oldList) listScroll = oldList.scrollTop;   // kept while browsing a pass too
    view.innerHTML = html;
    view.querySelector(".pass-list").scrollTop = listScroll;
    view.querySelectorAll("[data-pass]").forEach(b => b.onclick = () => { selectedPass = +b.dataset.pass; render(); });
    view.querySelector("[data-open]").onclick = () => { openPass = pass.id; selectedItem = null; render(); };
    bindPassHold(pass, document.getElementById("unlock-pass"));
  }

  // ---------- one battle pass ----------
  let renderedPass = null;   // keeps the pages' scroll position while browsing the same pass

  let passOpen = true;   // the browsed pass has been bought (or is free)
  let quantity = 1;      // how many of a repeatable supply to buy
  const unitsCost = it => it.cost * (it.repeatable ? quantity : 1);
  const buyable = it => it && passOpen && !it.owned && it.pageUnlocked && state.medals >= unitsCost(it);
  const perUnit = it => it.repeatable ? (it.days ? " / " + it.days + (it.days === 1 ? " day" : " days") : " each") : "";

  function renderPass(pass) {
    titleEl.textContent = pass.name;
    const items = pass.pages.flatMap((p, i) => p.items.map(it => ({ ...it, page: i, pageUnlocked: p.unlocked, pageUnlock: p.unlock })));
    if (selectedItem === null && items.length) selectedItem = items[0].id;
    const sel = items.find(i => i.id === selectedItem) || items[0];
    const theme = passTheme(pass);
    passOpen = pass.purchased;

    const pages = pass.pages.map((p, i) => {
      const owned = p.items.filter(it => it.owned).length;
      const lockNote = p.unlocked ? "" :
        '<span class="lock-note">Spend ' + (p.unlock - pass.spent) + " more medals in this pass to open it</span>";
      const tiles = p.items.map(it => {
        const it2 = items.find(x => x.id === it.id);
        const isSel = sel && it.id === sel.id;
        return '<button type="button" class="tile' + (it.owned ? " owned" : "") + (it.rebuy ? " rebuy" : "") + (isSel ? " selected" : "") +
            (isSel && buyable(it2) ? " can-buy" : "") + '" data-item="' + it.id + '">' +
          '<div class="art">' + itemImage(it) + "</div>" +
          (it.owned ? '<span class="check"></span>' : "") +
          (it.rebuy ? '<span class="rebuy-tag">Buying again</span>' : "") +
          (isSel && buyable(it2) ? '<span class="hold-hint">Hold to unlock</span>' : "") +
          '<span class="charge"></span>' +
          '<div class="foot"><span class="name">' + esc(it.name || ("Item " + it.id)) + "</span>" +
          '<span class="cost">' + (it.owned ? "Owned" : medal(true) + (it.rebuy ? "<s>" + it.fullCost + "</s>" : "") + it.cost +
            (it.repeatable ? '<small>' + perUnit(it) + "</small>" : "")) + "</span></div>" +
        "</button>";
      }).join("");
      return '<section class="page' + (p.unlocked ? "" : " locked") + '">' +
        '<div class="page-head"><span class="title">' + esc(p.title || "Page " + (i + 1)) + "</span>" + lockNote +
        '<span class="count">' + (p.items.every(it => it.repeatable) ? p.items.length + " supplies" : owned + " / " + p.items.length + " unlocked") + "</span></div>" +
        '<div class="grid">' + tiles + "</div></section>";
    }).join("");

    const hero =
      '<div class="pass-hero">' + passArt(pass) +
        '<div class="caption"><span class="badge ' + esc(pass.type.toLowerCase()) + '">' + esc(pass.type) + "</span>" +
        '<p>' + esc(pass.description) + "</p>" +
        '<div class="hero-stats">' + (pass.resetsIn !== undefined ? "<span>New weapons in " + restock(pass.resetsIn) + "</span>" : "") +
          "<span>" + (pass.total ? pass.owned + " / " + pass.total + " unlocked" : "Buy as many as you want") + "</span><span>" +
          pass.pages.filter(p => p.unlocked).length + " / " + pass.pages.length + " pages open</span><span>" + medal(true) + pass.spent + " spent</span></div>" +
      "</div></div>";

    let preview = '<div class="empty">Select an item.</div>';
    if (sel) {
      let label, disabled = true, note = "", passBuy = false;
      if (!pass.purchased) {
        passBuy = true;
        disabled = state.medals < pass.cost;
        label = disabled ? "Not enough medals" : "Hold to unlock battle pass";
        note = "Unlock this battle pass (" + pass.cost + " medals) to buy its items.";
      }
      else if (sel.owned) { label = "Owned"; note = "Already in your inventory."; }
      else if (!sel.pageUnlocked) { label = "Locked"; note = "Spend " + (sel.pageUnlock - pass.spent) + " more medals in this pass to open page " + (sel.page + 1) + "."; }
      else if (state.medals < sel.cost) { label = "Not enough medals"; note = "You need " + (sel.cost - state.medals) + " more medals."; }
      else if (sel.repeatable && state.medals < unitsCost(sel)) { label = "Not enough medals"; note = "You can afford " + Math.floor(state.medals / sel.cost) + "."; }
      else if (sel.repeatable) {
        label = "Hold to buy ×" + quantity; disabled = false;
        note = sel.days ? "Each one adds " + sel.days + (sel.days === 1 ? " day" : " days") + ": " + sel.days * quantity + " days in total." : "Goes straight to your inventory.";
      }
      else if (sel.rebuy) { label = "Hold to buy again"; disabled = false; note = "No longer in your inventory: buying it again costs half."; }
      else { label = "Hold to unlock"; disabled = false; note = "Hold the button or the item. Goes straight to your inventory."; }
      // the action sits right under the name so it never needs scrolling; the sheet comes after
      preview =
        '<div class="stage" id="stage"><div class="art">' + itemImage(sel) + "</div></div>" +
        '<div class="category">' + esc(sel.category) + " &middot; " + esc(pass.pages[sel.page].title || "Page " + (sel.page + 1)) + "</div>" +
        (sel.repeatable && pass.purchased ?
          '<div class="qty"><span class="label">Quantity</span><button type="button" data-q="-1">−</button><span class="n">' + quantity +
          '</span><button type="button" data-q="1">+</button><button type="button" data-qs="5">5</button><button type="button" data-qs="10">10</button>' +
          '<button type="button" data-qs="25">25</button></div>' : "") +
        "<h3>" + esc(sel.name || ("Item " + sel.id)) + teamBadge(sel) + "</h3>" +
        '<div class="buy-row">' +
          (passBuy ? '<div class="price">' + medal() + pass.cost + "</div>" :
            sel.owned ? "" : '<div class="price">' + medal() + (sel.rebuy ? "<s>" + sel.fullCost + "</s>" : "") + unitsCost(sel) + "</div>") +
          '<button type="button" class="cta hold" id="' + (passBuy ? "unlock-pass" : "unlock") + '"' + (disabled ? " disabled" : "") + '><span class="fill"></span><span class="label">' + esc(label) + "</span></button>" +
        "</div>" +
        '<div class="note">' + esc(note) + "</div>" +
        sheetHtml(sel, false);
    }

    const oldPages = view.querySelector(".pages");
    const scroll = oldPages && renderedPass === pass.id ? oldPages.scrollTop : 0;
    view.innerHTML = '<div class="pass-view" style="--theme:' + theme.light + ";--theme-dark:" + theme.dark + ";--theme-soft:" + theme.light + '26">' +
      '<div class="pages">' + hero + pages + '</div><aside class="preview">' + preview + "</aside></div>";
    view.querySelector(".pages").scrollTop = scroll;
    renderedPass = pass.id;

    view.querySelectorAll("[data-item]").forEach(b => {
      const it = items.find(i => i.id === +b.dataset.item);
      b.onmousedown = e => {
        if (e.button !== 0) return;
        hideTooltip();
        if (selectedItem !== it.id) { selectedItem = it.id; quantity = 1; render(); }
        if (buyable(it)) startHold(pass, it);
      };
      b.onmouseenter = () => { if (!hold) showTooltip(b, it); };
      b.onmouseleave = () => { hideTooltip(); cancelHold(); };
    });
    view.querySelectorAll("[data-q], [data-qs]").forEach(b => b.onclick = () => {
      quantity = b.dataset.qs ? +b.dataset.qs : quantity + +b.dataset.q;
      quantity = Math.max(1, Math.min(99, quantity));
      render();
    });
    const unlock = document.getElementById("unlock");
    if (unlock && !unlock.disabled) bindHold(unlock, () => startHold(pass, sel));
    bindPassHold(pass, document.getElementById("unlock-pass"));
    if (sel) showStage(sel);
  }

  // ---------- hold to unlock: charge sound + glow, unlocks when the bar fills ----------
  const HOLD_MS = 1300;
  const sounds = { charge: new Audio("sounds/charge.wav"), unlock: new Audio("sounds/unlock.wav"), pass: new Audio("sounds/pass.wav") };
  sounds.charge.volume = 0.6;
  sounds.unlock.volume = 0.7;
  sounds.pass.volume = 0.8;
  let hold = null;   // { pass, item, start, frame, soundTimer }

  function holdTargets(id) {
    if (id === "pass") return [document.getElementById("unlock-pass")].filter(Boolean);
    return [view.querySelector('.tile[data-item="' + id + '"]'), document.getElementById("unlock")].filter(Boolean);
  }

  function bindHold(button, start) {
    button.onmousedown = e => { if (e.button === 0) start(); };
    button.onmouseleave = cancelHold;
    button.onkeydown = e => { if ((e.key === " " || e.key === "Enter") && !e.repeat) { e.preventDefault(); start(); } };
    button.onkeyup = e => { if (e.key === " " || e.key === "Enter") cancelHold(); };
  }

  function bindPassHold(pass, button) {
    if (button && !button.disabled) bindHold(button, () => startHold(pass, { id: "pass" }));
  }

  function startHold(pass, item) {
    if (hold || busy) return;
    hold = { pass, item, start: performance.now() };
    // only start the sound once it's clearly a hold, not a click to select
    hold.soundTimer = setTimeout(() => { try { sounds.charge.currentTime = 0; sounds.charge.play().catch(() => {}); } catch (e) {} }, 120);
    const step = now => {
      if (!hold) return;
      const t = Math.min(1, (now - hold.start) / HOLD_MS);
      holdTargets(item.id).forEach(el => { el.classList.add("charging"); el.style.setProperty("--hold", t.toFixed(3)); });
      if (t >= 1) return finishHold();
      hold.frame = requestAnimationFrame(step);
    };
    hold.frame = requestAnimationFrame(step);
  }

  function stopHoldEffects(id) {
    clearTimeout(hold.soundTimer);
    cancelAnimationFrame(hold.frame);
    sounds.charge.pause();
    holdTargets(id).forEach(el => { el.classList.remove("charging"); el.style.removeProperty("--hold"); });
  }

  function cancelHold() {
    if (!hold) return;
    stopHoldEffects(hold.item.id);
    hold = null;
  }

  function finishHold() {
    const { pass, item } = hold;
    stopHoldEffects(item.id);
    hold = null;
    const sting = item.id === "pass" ? sounds.pass : sounds.unlock;
    try { sting.currentTime = 0; sting.play().catch(() => {}); } catch (e) {}
    holdTargets(item.id).forEach(el => el.classList.add("flash"));
    if (item.id === "pass") buyPass(pass);
    else buy(pass, item);
  }

  document.addEventListener("mouseup", cancelHold);
  window.addEventListener("blur", cancelHold);

  function showTooltip(anchor, it) {
    const sheet = sheetHtml(it, true);
    tooltipEl.innerHTML = '<div class="tt-cat">' + esc(it.category) + '</div><div class="tt-name">' + esc(it.name || ("Item " + it.id)) + teamBadge(it) + "</div>" +
      (sheet || '<div class="sheet-note">' + (it.owned ? "Owned" : it.cost + " medals") + "</div>");
    tooltipEl.classList.remove("hidden");
    const r = anchor.getBoundingClientRect(), t = tooltipEl.getBoundingClientRect();
    let x = r.right + 8, y = r.top;
    if (x + t.width > innerWidth - 6) x = r.left - t.width - 8;
    if (y + t.height > innerHeight - 6) y = innerHeight - t.height - 6;
    tooltipEl.style.left = Math.max(6, x) + "px";
    tooltipEl.style.top = Math.max(6, y) + "px";
  }
  const hideTooltip = () => tooltipEl.classList.add("hidden");

  async function buy(pass, item) {
    if (busy) return;
    busy = true;
    try {
      state = await api("/api/unlock", { warbond: pass.id, item: item.id, quantity: item.repeatable ? quantity : 1 });
      toast((item.repeatable ? "Bought " + quantity + " × " : "") + (item.name || "Item") + (item.repeatable ? "" : " unlocked"), "ok");
      render();
      const tile = view.querySelector('.tile[data-item="' + item.id + '"]');
      if (tile) tile.classList.add("flash");
    } catch (e) {
      toast(e.message, "error");
    } finally {
      busy = false;
    }
  }

  async function buyPass(pass) {
    if (busy) return;
    busy = true;
    try {
      state = await api("/api/unlockpass", { warbond: pass.id });
      toast(pass.name + " unlocked", "ok");
      render();
    } catch (e) {
      toast(e.message, "error");
    } finally {
      busy = false;
    }
  }

  tabsEl.querySelectorAll("button").forEach(b => b.onclick = () => { tab = b.dataset.tab; render(); });
  backBtn.onclick = () => { cancelHold(); openPass = null; hideTooltip(); render(); };
  closeBtn.onclick = closeShop;

  load();
  setInterval(() => { if (!busy && !hold) load(); }, 15000); // medals change after matches
})();
