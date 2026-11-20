// The room's Extra Settings page: the game's options of the room's mode, the bots' options, the zombie modes' options
// and the host's defaults for new rooms. Data: CSNZ_Server's /api/roomsettings (manager/roomoptions.cpp); every change
// is sent at once. Opened by the room's Extra Settings button (botassist.asi).
(function () {
  "use strict";

  const SKILLS = ["Very easy", "Easy", "Normal", "Normal+", "Hard", "Hard+", "Very hard", "Expert"];
  const $ = id => document.getElementById(id);
  let room = null;

  function toast(text, bad) {
    const t = $("toast");
    t.textContent = text;
    t.className = "toast" + (bad ? " bad" : "");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => t.classList.add("hidden"), 2600);
  }

  // not served by CSNZ_Server (layout preview): a fake room
  const PREVIEW = location.port !== "30080";
  const mock = {
    host: true, waiting: true, mode: 14, coop: false, holdoutMode: true, watchBackMode: true, balanceMode: true,
    weaponBalance: false, newBotSkill: -2, options: { orderCount: 0, obey: 100, farChance: 0, groups: true, walkers: true, watchBack: true, voices: "radio" },
    defaults: null,
    native: [
      { key: "freezeTime", label: "Freeze time", help: "Seconds everyone waits at the start of a round.", toggle: false, value: 0, locked: "", choices: [0, 1, 2, 3, 4, 5].map(i => ({ value: i, label: i + " s" })) },
      { key: "buyTime", label: "Buy time", help: "Seconds the buy menu stays open after a round starts.", toggle: false, value: 90, locked: "", choices: [60, 90, 120].map(i => ({ value: i, label: i + " s" })) },
      { key: "displayNickname", label: "Display nickname", help: "Whose names show over their heads.", toggle: false, value: 1, locked: "", choices: [{ value: 0, label: "Do not display" }, { value: 1, label: "Display allies only" }, { value: 2, label: "Display all nicknames" }] },
      { key: "teamBalance", label: "Team balance", help: "Moves players between teams.", toggle: true, value: 1, locked: "This mode keeps its own team balance.", choices: [] },
      { key: "friendlyFire", label: "Friendly fire", help: "Shots hurt your own team.", toggle: true, value: 0, locked: "", choices: [] },
      { key: "flashlight", label: "Allow flashlight", help: "Players can use the flashlight.", toggle: true, value: 1, locked: "", choices: [] },
      { key: "zbRespawn", label: "Zombies respawn", help: "Zombies always come back: no permanent zombie death.", toggle: true, value: 1, locked: "", choices: [] },
      { key: "zbAutoHunting", label: "Zombie auto play", help: "A zombie player who stays idle is played by the game.", toggle: true, value: 0, locked: "", choices: [] }]
  };

  function previewCall(body) {
    if (!body) return JSON.parse(JSON.stringify(mock));
    if (body.action === "saveDefaults") {
      const n = {};
      mock.native.forEach(o => { n[o.key] = o.value; });
      mock.defaults = { native: n, options: Object.assign({}, mock.options), newBotSkill: mock.newBotSkill, weaponBalance: mock.weaponBalance };
      return Object.assign(JSON.parse(JSON.stringify(mock)), { saved: true });
    }
    if (body.action === "forgetDefaults") mock.defaults = null;
    if (body.options) Object.assign(mock.options, body.options);
    if (body.newBotSkill !== undefined) mock.newBotSkill = body.newBotSkill;
    if (body.weaponBalance !== undefined) mock.weaponBalance = body.weaponBalance;
    if (body.native) mock.native.forEach(o => { if (body.native[o.key] !== undefined && !o.locked) o.value = body.native[o.key]; });
    return JSON.parse(JSON.stringify(mock));
  }

  async function api(body) {
    if (PREVIEW) return previewCall(body);
    const res = await fetch("/api/roomsettings", body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
    const json = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(json.error || "The server didn't answer.");
    return json;
  }

  function close() {
    location.href = "http://127.0.0.1:1/"; // the game closes its browser window on a failed load
  }

  function canEdit() { return room && room.host && room.waiting !== false; }

  function onOff(el, value, enabled) {
    el.querySelectorAll("button").forEach(b => {
      b.classList.toggle("on", +b.dataset.v === (value ? 1 : 0));
      b.disabled = !enabled;
    });
  }

  function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]); }

  function renderNative() {
    const edit = canEdit();
    $("native").innerHTML = room.native.length ? room.native.map(o => {
      const off = !edit || !!o.locked;
      const control = o.toggle
        ? `<span class="onoff" data-key="${o.key}"><button type="button" data-v="0"${off ? " disabled" : ""} class="${o.value ? "" : "on"}">Off</button><button type="button" data-v="1"${off ? " disabled" : ""} class="${o.value ? "on" : ""}">On</button></span>`
        : `<select class="select" data-key="${o.key}"${off ? " disabled" : ""}>${o.choices.map(c => `<option value="${c.value}"${c.value === o.value ? " selected" : ""}>${esc(c.label)}</option>`).join("")}</select>`;
      return `<div class="row${o.locked ? " off" : ""}"><div class="row-text"><span class="row-name">${esc(o.label)}</span>` +
        `<span class="row-help">${esc(o.help)}${o.locked ? ` <span class="note">${esc(o.locked)}</span>` : ""}</span></div>${control}</div>`;
    }).join("") : `<p class="card-help">This mode has no options to change here.</p>`;
  }

  function render() {
    const edit = canEdit();
    $("roominfo").textContent = !room.host ? "Only the host can change these" : room.waiting === false ? "The match is running" : "Changes apply at once";
    renderNative();

    if (!$("newskill").options.length)
      $("newskill").innerHTML = `<option value="-2">Random skill</option><option value="-1">Room AI difficulty</option>` +
        SKILLS.map((s, i) => `<option value="${i}">${s}</option>`).join("");
    $("newskill").value = String(room.newBotSkill);
    $("newskill").disabled = !edit;
    const oc = String(room.options.orderCount);
    if (![...$("ordercount").options].some(o => o.value === oc))
      $("ordercount").insertAdjacentHTML("beforeend", `<option value="${oc}">The ${oc} nearest</option>`);
    $("ordercount").value = oc;
    $("ordercount").disabled = !edit;
    const pick = (id, v, dflt) => {
      const s = String(v ?? dflt);
      if (![...$(id).options].some(o => o.value === s))
        $(id).insertAdjacentHTML("beforeend", `<option value="${s}">${s}%</option>`);
      $(id).value = s;
      $(id).disabled = !edit;
    };
    pick("obey", room.options.obey, 100);
    pick("farchance", room.options.farChance, 0);
    $("farrow").classList.toggle("hidden", !room.options.orderCount);
    $("voices").value = room.options.voices || "radio";
    $("voices").disabled = !edit;

    // only the options this mode has (the others keep their value for the modes that have them)
    onOff($("balance"), room.weaponBalance, edit && room.balanceMode);
    onOff($("groups"), room.options.groups, edit);
    onOff($("walkers"), room.options.walkers, edit);
    onOff($("watchback"), room.options.watchBack, edit);
    $("balancerow").classList.toggle("hidden", !room.balanceMode);
    $("groupsrow").classList.toggle("hidden", !room.holdoutMode);
    $("walkersrow").classList.toggle("hidden", !room.holdoutMode);
    $("watchrow").classList.toggle("hidden", !room.watchBackMode);
    $("zombiecard").classList.toggle("hidden", !(room.holdoutMode || room.watchBackMode || room.balanceMode));

    $("forget").disabled = !room.defaults;
    $("defstate").textContent = room.defaults ? "New rooms you create start with your saved defaults." : "No saved defaults: new rooms start with the game's.";
  }

  async function send(body) {
    try {
      const fresh = await api(Object.assign({ action: "set" }, body));
      room = fresh;
      if (fresh.refused && fresh.refused.length) toast("This mode doesn't allow that value.", true);
    } catch (e) {
      toast(e.message, true);
    }
    render();
  }

  async function load() {
    try {
      room = await api();
      render();
    } catch (e) {
      $("roominfo").textContent = e.message;
    }
  }

  // ---------------------------------------------------------------- events
  $("native").addEventListener("change", e => {
    const key = e.target.dataset.key;
    if (key) send({ native: { [key]: +e.target.value } });
  });
  $("native").addEventListener("click", e => {
    const b = e.target.closest(".onoff button");
    if (!b || b.disabled) return;
    send({ native: { [b.parentElement.dataset.key]: +b.dataset.v } });
  });
  $("newskill").addEventListener("change", () => send({ newBotSkill: +$("newskill").value }));
  $("ordercount").addEventListener("change", () => send({ options: { orderCount: +$("ordercount").value } }));
  $("obey").addEventListener("change", () => send({ options: { obey: +$("obey").value } }));
  $("farchance").addEventListener("change", () => send({ options: { farChance: +$("farchance").value } }));
  $("voices").addEventListener("change", () => send({ options: { voices: $("voices").value } }));
  const flag = (id, make) => $(id).addEventListener("click", e => {
    const b = e.target.closest("button");
    if (b && !b.disabled) send(make(b.dataset.v === "1"));
  });
  flag("balance", on => ({ weaponBalance: on }));
  flag("groups", on => ({ options: { groups: on } }));
  flag("walkers", on => ({ options: { walkers: on } }));
  flag("watchback", on => ({ options: { watchBack: on } }));

  $("save").addEventListener("click", async () => {
    try {
      room = await api({ action: "saveDefaults" });
      toast("Saved: new rooms you create start like this one.");
    } catch (e) {
      toast(e.message, true);
    }
    render();
  });
  $("forget").addEventListener("click", async () => {
    try {
      room = await api({ action: "forgetDefaults" });
      toast("Your defaults are gone: new rooms start with the game's.");
    } catch (e) {
      toast(e.message, true);
    }
    render();
  });
  $("close").addEventListener("click", close);
  document.addEventListener("keydown", e => { if (e.key === "Escape") close(); });

  load();
  setInterval(() => { if (document.visibilityState === "visible" && !document.activeElement.matches("select")) load(); }, 4000);
})();
