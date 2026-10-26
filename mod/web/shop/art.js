// Category silhouettes for items without an icon, and the battle pass banners and colours.
(function () {
  const fill = "#c9c4b6";

  // simple side-view silhouettes on a 200x100 canvas
  const shapes = {
    Pistols: '<path d="M40 30h92l6 6v10h-8v6h-30l-6 10 6 22h-22l-6-20-8-4V46H40z"/>',
    Shotguns: '<path d="M8 40h120l6-6h50v14h-48l-8 6H96l-6 8 4 16H76l-6-16-8-2v-6H8z"/>',
    SMGs: '<path d="M30 34h96l8-6h30v12h-26v10h-30l-8 8 4 24H88l-4-22-10-2-6 22H52l4-28H30z"/>',
    Rifles: '<path d="M4 42h40l8-8h86l6-6h44v12h-40v10H114l-6 4 6 26H96l-8-22h-16l-4 10H44l4-12H4z"/>',
    "Machine guns": '<path d="M4 38h120l6-8h60v16h-56l-4 8h-22v6l-10 4 4 22H86l-2-20h-18v14H52V54H4z"/><path d="M90 60h16v26H90z"/>',
    Melee: '<path d="M14 54 150 24l36 6-34 14L30 64z"/><path d="M24 50l-10 4 4 12 14-4z"/>',
    Equipment: '<circle cx="100" cy="58" r="28"/><path d="M90 22h20v10H90zM110 24l22-10 4 8-22 10z"/>',
    Characters: '<circle cx="100" cy="30" r="18"/><path d="M62 96c0-26 16-44 38-44s38 18 38 44z"/>',
    Costumes: '<path d="M58 70c0-26 18-46 42-46s42 20 42 46l18 8H40z"/>',
    Items: '<path d="M60 30l40-16 40 16v44l-40 16-40-16z"/><path fill="#0b0e11" d="M100 46 64 32l-2 4 38 14z"/>'
  };

  window.itemArt = function (category) {
    const shape = shapes[category] || shapes.Items;
    return '<svg viewBox="0 0 200 100" xmlns="http://www.w3.org/2000/svg"><g fill="' + fill + '">' + shape + '</g></svg>';
  };

  function hash(text) {
    let h = 2166136261;
    for (let i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
    return h >>> 0;
  }

  const palettes = {
    Season: [["#2a2410", "#f4d23a"], ["#13202b", "#f4d23a"], ["#241612", "#ff8a3d"]],
    Event: [["#0f1f2c", "#9ad8ff"], ["#1d1028", "#c48bff"], ["#0f2418", "#7dd87a"]]
  };

  // The colours of a battle pass: [dark, light]. Used by its banner and as the theme inside the pass.
  window.passTheme = function (pass) {
    if (pass.theme && pass.theme.length === 2) return { dark: pass.theme[0], light: pass.theme[1] };   // Warbonds.json "Theme"
    const set = palettes[pass.type] || palettes.Season;
    const [dark, light] = set[hash(pass.name) % set.length];
    return { dark, light };
  };

  // Cover for passes without promotional art: their best weapons' real icons fanned out on the right.
  function cover(pass) {
    const items = pass.pages.flatMap(p => p.items).filter(it => it.category !== "Characters")
      .sort((a, b) => (b.fullCost || b.cost) - (a.fullCost || a.cost)).slice(0, 3);
    const spots = [["right:2%;top:16%;width:58%;transform:rotate(-8deg)", 3], ["right:20%;top:44%;width:46%;transform:rotate(5deg)", 2],
      ["right:-6%;top:52%;width:40%;transform:rotate(-2deg)", 1]];
    return items.map((it, i) =>
      '<img class="cover-icon" src="icons/' + it.id + '.png" alt="" style="' + spots[i][0] + ';z-index:' + spots[i][1] + '" onerror="this.remove()">').join("");
  }

  // Banner for a battle pass: its promotional art from the game when there is one, otherwise a cover made
  // from its items over the pass colours.
  window.passArt = function (pass) {
    const { dark, light } = passTheme(pass);
    const h = hash(pass.name);
    const angle = 20 + (h % 40);
    const generated =
      'background:' +
      'repeating-linear-gradient(' + angle + 'deg, rgba(255,255,255,.035) 0 2px, transparent 2px 14px),' +
      'radial-gradient(circle at 75% 40%, ' + light + '40, transparent 60%),' +
      'linear-gradient(135deg, ' + dark + ', #07090b 80%);';
    const content = pass.image
      ? '<img class="promo" src="/' + pass.image + '" alt="" onerror="this.remove()">'
      : cover(pass);
    return '<div class="art" style="' + generated + '">' + content + '</div>';
  };
})();
