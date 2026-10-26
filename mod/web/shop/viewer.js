// 3D preview of a character holding a weapon, from models exported by tools/mdl_export.py.
// Models are unposed GoldSrc meshes (vertices in bone space) + bone matrices; the weapon's bones follow the
// character's bones with the same name, like the engine does with attached (p_) models.
(function () {
  const cache = {};
  const loadJson = name => cache[name] || (cache[name] = fetch("models/" + name + ".json").then(r => (r.ok ? r.json() : null)).catch(() => null));

  // 3x4 row-major matrices
  const mul = (a, b) => {
    const o = new Array(12);
    for (let r = 0; r < 3; r++)
      for (let c = 0; c < 4; c++)
        o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + (c === 3 ? a[r * 4 + 3] : 0);
    return o;
  };

  function weaponMatrices(weapon, charBones) {
    const out = [];
    weapon.bones.forEach((b, i) => {
      out[i] = charBones[b.name] || (b.parent >= 0 ? mul(out[b.parent], weapon.local[i]) : weapon.local[i]);
    });
    return out;
  }

  // GoldSrc (x forward, y left, z up) -> GL (x right, y up, z towards the camera); the model faces the camera
  function skin(mesh, mats) {
    const n = mesh.vbones.length, pos = new Float32Array(n * 3), nor = new Float32Array(n * 3);
    for (let i = 0; i < n; i++) {
      const m = mats[mesh.vbones[i]], k = mats[mesh.nbones[i]];
      const x = mesh.positions[i * 3], y = mesh.positions[i * 3 + 1], z = mesh.positions[i * 3 + 2];
      const gx = m[0] * x + m[1] * y + m[2] * z + m[3], gy = m[4] * x + m[5] * y + m[6] * z + m[7], gz = m[8] * x + m[9] * y + m[10] * z + m[11];
      pos[i * 3] = gy; pos[i * 3 + 1] = gz; pos[i * 3 + 2] = gx;
      const a = mesh.normals[i * 3], b = mesh.normals[i * 3 + 1], c = mesh.normals[i * 3 + 2];
      const nx = k[0] * a + k[1] * b + k[2] * c, ny = k[4] * a + k[5] * b + k[6] * c, nz = k[8] * a + k[9] * b + k[10] * c;
      nor[i * 3] = ny; nor[i * 3 + 1] = nz; nor[i * 3 + 2] = nx;
    }
    return { pos, nor, uv: new Float32Array(mesh.uvs) };
  }

  const VS = "attribute vec3 p;attribute vec3 n;attribute vec2 t;uniform mat4 mvp;uniform mat4 rot;varying vec2 vt;varying vec3 vn;" +
    "void main(){vt=t;vn=(rot*vec4(n,0.)).xyz;gl_Position=mvp*vec4(p,1.);}";
  const FS = "precision mediump float;uniform sampler2D tex;uniform float masked;varying vec2 vt;varying vec3 vn;" +
    "void main(){vec4 c=texture2D(tex,vt);if(masked>.5&&c.a<.5)discard;vec3 l=normalize(vec3(-.4,.6,.8));" +
    "float d=max(dot(normalize(vn),l),0.);float rim=pow(1.-max(normalize(vn).z,0.),2.)*.25;" +
    "gl_FragColor=vec4(c.rgb*(.5+.65*d)+vec3(.95,.82,.25)*rim,1.);}";

  function perspective(fov, aspect, near, far) {
    const f = 1 / Math.tan(fov / 2), nf = 1 / (near - far);
    return [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) * nf, -1, 0, 0, 2 * far * near * nf, 0];
  }
  function mat4mul(a, b) {
    const o = new Array(16);
    for (let c = 0; c < 4; c++)
      for (let r = 0; r < 4; r++)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  }
  const rotY = a => [Math.cos(a), 0, -Math.sin(a), 0, 0, 1, 0, 0, Math.sin(a), 0, Math.cos(a), 0, 0, 0, 0, 1];
  const translate = (x, y, z) => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1];

  window.createViewer = function (canvas) {
    const gl = canvas.getContext("webgl", { antialias: true, alpha: true, premultipliedAlpha: false });
    if (!gl) return null;
    const prog = gl.createProgram();
    [[gl.VERTEX_SHADER, VS], [gl.FRAGMENT_SHADER, FS]].forEach(([type, src]) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      gl.attachShader(prog, s);
    });
    gl.linkProgram(prog);
    gl.useProgram(prog);
    const loc = name => gl.getAttribLocation(prog, name), uni = name => gl.getUniformLocation(prog, name);
    const texCache = {};
    function texture(file) {
      if (texCache[file]) return texCache[file];
      const t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([90, 90, 90, 255]));
      const img = new Image();
      img.onload = () => {
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
        draw();
      };
      img.src = "models/" + file;
      [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T].forEach(p => gl.texParameteri(gl.TEXTURE_2D, p, gl.CLAMP_TO_EDGE));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      return (texCache[file] = t);
    }

    let parts = [], center = [0, 36, 0], radius = 40, angle = 0, dragging = false, lastX = 0, token = 0, raf = 0;

    function draw() {
      const w = canvas.clientWidth, h = canvas.clientHeight;
      if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
      gl.viewport(0, 0, w, h);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      gl.enable(gl.DEPTH_TEST);
      const rot = rotY(angle);
      const view = translate(0, 0, -radius * 3.6);
      const mvp = mat4mul(perspective(0.6, w / Math.max(1, h), 1, 2000), mat4mul(view, mat4mul(rot, translate(-center[0], -center[1], -center[2]))));
      gl.uniformMatrix4fv(uni("mvp"), false, mvp);
      gl.uniformMatrix4fv(uni("rot"), false, rot);
      for (const p of parts) {
        [["p", p.pos, 3], ["n", p.nor, 3], ["t", p.uv, 2]].forEach(([name, buf, size]) => {
          gl.bindBuffer(gl.ARRAY_BUFFER, buf);
          gl.enableVertexAttribArray(loc(name));
          gl.vertexAttribPointer(loc(name), size, gl.FLOAT, false, 0, 0);
        });
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, texture(p.texture));
        gl.uniform1i(uni("tex"), 0);
        gl.uniform1f(uni("masked"), p.masked ? 1 : 0);
        gl.drawArrays(gl.TRIANGLES, 0, p.count);
      }
    }

    function loop() {
      if (!dragging) angle += 0.004;
      draw();
      raf = requestAnimationFrame(loop);
    }

    canvas.onmousedown = e => { dragging = true; lastX = e.clientX; };
    window.addEventListener("mouseup", () => { dragging = false; });
    window.addEventListener("mousemove", e => { if (dragging) { angle += (e.clientX - lastX) * 0.01; lastX = e.clientX; } });

    return {
      // character: model name (e.g. "gign"), weapon: p_ model name or null, pose: ref_aim_* sequence
      async show(character, weapon, pose) {
        const my = ++token;
        const [ch, wp] = await Promise.all([loadJson(character), weapon ? loadJson(weapon) : null]);
        if (my !== token) return false;
        if (!ch) return false;
        const poseMats = ch.poses[pose] || ch.poses.ref_aim_carbine || Object.values(ch.poses)[0];
        const byName = {};
        ch.bones.forEach((b, i) => { byName[b.name] = poseMats[i]; });
        const meshes = ch.meshes.map(m => [m, poseMats]);
        if (wp) {
          const wm = weaponMatrices(wp, byName);
          wp.meshes.forEach(m => meshes.push([m, wm]));
        }
        const min = [1e9, 1e9, 1e9], max = [-1e9, -1e9, -1e9];
        parts.forEach(p => [p.pos, p.nor, p.uv].forEach(b => gl.deleteBuffer(b)));
        parts = meshes.map(([m, mats]) => {
          const s = skin(m, mats);
          for (let i = 0; i < s.pos.length; i += 3)
            for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], s.pos[i + k]); max[k] = Math.max(max[k], s.pos[i + k]); }
          const buf = data => { const b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b); gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW); return b; };
          return { pos: buf(s.pos), nor: buf(s.nor), uv: buf(s.uv), count: s.pos.length / 3, texture: m.texture, masked: m.masked };
        });
        center = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
        radius = Math.max(max[1] - min[1], max[0] - min[0]) / 2;
        angle = 0.35;
        cancelAnimationFrame(raf);
        loop();
        return true;
      },
      stop() { cancelAnimationFrame(raf); }
    };
  };
})();
