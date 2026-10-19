/* Concat Corpus Builder — descriptor map on a plain 2D canvas (no libraries).
 * Points are (x,y,z) map coordinates. Orbit mode: drag to rotate, click a dot to pick
 * it. Play mode (or Shift held): press and move to play the slices under the pointer,
 * like Concat's Map mode. Wheel zooms. "Move view" looks along the third axis: the
 * 2-D plane Concat navigates on the device. A flat map (Raw X/Y) stays 2-D.
 * The dots are drawn once into a cached layer; hover, selection, grain flashes and
 * the play cursor are drawn over it, so animating them stays cheap. */
'use strict';

class Map3D {
  constructor(canvas, opts) {
    this.c = canvas;
    this.ctx = canvas.getContext('2d');
    this.onPick = opts.onPick || (() => {});
    this.onHover = opts.onHover || (() => {});
    this.onPlay = opts.onPlay || (() => {});   /* ({x, y} | null) while playing, screen px */
    this.radiusPx = opts.radiusPx || (() => 24);
    this.pts = new Float32Array(0);   /* x,y,z per point, normalised to [-1,1] */
    this.col = [];                    /* css colour per point */
    this.axes = ['PC1', 'PC2', 'PC3'];
    this.flat = false;
    this.mode = 'orbit';
    this.yaw = -0.6; this.pitch = 0.45; this.zoom = 1.0;
    this.hover = -1; this.sel = -1;
    this.screen = new Float32Array(0);
    this.order = new Uint32Array(0);
    this.flashes = new Map();         /* point -> time it was played */
    this.cursor = null;               /* {x, y} while playing */
    this.focus = null;                /* Set of points to show; the rest dim (selected source) */
    this.base = document.createElement('canvas');
    this.dirty = true;
    this.anim = 0;
    this._bind();
    new ResizeObserver(() => { this.dirty = true; this.draw(); }).observe(canvas);
  }

  setPoints(xyz, colours, axes, flat) {
    const n = xyz.length / 3;
    /* Each axis min..max -> -1..1, like the Move maps its axes to 0..1, so the Move
     * view shows the same spread you navigate with X/Y on the device. */
    const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < n; i++) for (let c = 0; c < 3; c++) { const v = xyz[3 * i + c]; if (v < lo[c]) lo[c] = v; if (v > hi[c]) hi[c] = v; }
    this.pts = new Float32Array(xyz.length);
    for (let i = 0; i < n; i++) for (let c = 0; c < 3; c++) {
      const r = hi[c] - lo[c];
      this.pts[3 * i + c] = r > 1e-12 ? ((xyz[3 * i + c] - lo[c]) / r) * 2 - 1 : 0;
    }
    this.col = colours;
    if (axes) this.axes = axes;
    if (flat !== undefined && flat !== this.flat) { this.flat = flat; if (flat) { this.yaw = 0; this.pitch = 0; } }
    this.screen = new Float32Array(n * 3);
    this.order = new Uint32Array(n);
    this.hover = -1;
    this.flashes.clear();
    if (this.sel >= n) this.sel = -1;
    this.dirty = true;
    this.draw();
  }
  setColours(colours) { this.col = colours; this.dirty = true; this.draw(); }
  setFocus(set) { this.focus = set && set.size ? set : null; this.dirty = true; this.draw(); }
  setMode(m) { this.mode = m; this.c.style.cursor = m === 'play' ? 'crosshair' : ''; }
  moveView() { this.yaw = 0; this.pitch = 0; this.dirty = true; this.draw(); }   /* axis 1 across, axis 2 up */
  orbitView() { if (this.flat) return; this.yaw = -0.6; this.pitch = 0.45; this.dirty = true; this.draw(); }
  select(i) { this.sel = i; this.draw(); }
  refresh() { this.dirty = true; this.draw(); }   /* theme change */

  /* Up to k points within r px of (x,y), nearest first; else the single nearest. */
  nearest(x, y, r, k) {
    const s = this.screen, n = s.length / 3, r2 = r * r;
    const hits = [];
    let best = -1, bd = Infinity;
    for (let i = 0; i < n; i++) {
      const dx = s[3 * i] - x, dy = s[3 * i + 1] - y, d = dx * dx + dy * dy;
      if (d < r2) hits.push([d, i]);
      if (d < bd) { bd = d; best = i; }
    }
    if (!hits.length) return best >= 0 ? [best] : [];
    hits.sort((a, b) => a[0] - b[0]);
    return hits.slice(0, k).map((h) => h[1]);
  }

  /* The k dots nearest (x,y) on screen, nearest first, and the distance to the k-th. */
  nearestK(x, y, k) {
    const s = this.screen, n = s.length / 3, bd = [], bi = [];
    for (let i = 0; i < n; i++) {
      const dx = s[3 * i] - x, dy = s[3 * i + 1] - y, d = dx * dx + dy * dy;
      if (bd.length < k || d < bd[bd.length - 1]) {
        let p = bd.length < k ? bd.length : k - 1;
        while (p > 0 && bd[p - 1] > d) { bd[p] = bd[p - 1]; bi[p] = bi[p - 1]; p--; }
        bd[p] = d; bi[p] = i;
      }
    }
    return { idx: bi, r: bd.length ? Math.sqrt(bd[bd.length - 1]) : 0 };
  }

  flash(i) { this.flashes.set(i, performance.now()); this._animate(); }

  _animate() {
    if (this.anim) return;
    const step = () => {
      this.anim = 0;
      this.draw();
      if (this.flashes.size || this.cursor) this.anim = requestAnimationFrame(step);
    };
    this.anim = requestAnimationFrame(step);
  }

  _local(e) { const r = this.c.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top }; }

  _bind() {
    let drag = null;
    this.c.addEventListener('pointerdown', (e) => {
      this.c.setPointerCapture(e.pointerId);
      if (this.mode === 'play' || e.shiftKey) {
        drag = { play: true };
        this.cursor = this._local(e);
        this.onPlay(this.cursor);
        this._animate();
        return;
      }
      drag = { x: e.clientX, y: e.clientY, yaw: this.yaw, pitch: this.pitch, moved: false };
    });
    this.c.addEventListener('pointermove', (e) => {
      if (drag && drag.play) { this.cursor = this._local(e); this.onPlay(this.cursor); return; }
      if (drag) {
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
        if (!this.flat && drag.moved) {
          this.yaw = drag.yaw + dx * 0.008;
          this.pitch = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, drag.pitch + dy * 0.008));
          this.dirty = true;
          this.draw();
        }
        return;
      }
      const i = this._hit(e);
      if (i !== this.hover) { this.hover = i; this.draw(); this.onHover(i); }
    });
    const end = (e) => {
      const d = drag; drag = null;
      if (d && d.play) { this.cursor = null; this.onPlay(null); this.draw(); return; }
      if (d && !d.moved && e.type === 'pointerup') { const i = this._hit(e); if (i >= 0) { this.sel = i; this.draw(); this.onPick(i); } }
    };
    this.c.addEventListener('pointerup', end);
    this.c.addEventListener('pointercancel', end);
    this.c.addEventListener('pointerleave', () => { if (this.hover >= 0) { this.hover = -1; this.draw(); this.onHover(-1); } });
    this.c.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.zoom = Math.max(0.4, Math.min(8, this.zoom * Math.exp(-e.deltaY * 0.0015)));
      this.dirty = true;
      this.draw();
    }, { passive: false });
  }

  _hit(e) {
    const { x, y } = this._local(e);
    const near = this.nearest(x, y, 8, 1);
    if (!near.length) return -1;
    const i = near[0], dx = this.screen[3 * i] - x, dy = this.screen[3 * i + 1] - y;
    return dx * dx + dy * dy <= 64 ? i : -1;
  }

  _renderBase(W, H, dpr) {
    const b = this.base;
    if (b.width !== Math.round(W * dpr) || b.height !== Math.round(H * dpr)) { b.width = Math.round(W * dpr); b.height = Math.round(H * dpr); }
    const g = b.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    const cs = getComputedStyle(this.c);
    const axis = cs.getPropertyValue('--map-axis').trim() || '#555';
    const ink = cs.getPropertyValue('--map-ink').trim() || '#ddd';
    const cy = Math.cos(this.yaw), sy = Math.sin(this.yaw), cp = Math.cos(this.pitch), sp = Math.sin(this.pitch);
    const scale = Math.min(W, H) * (this.flat ? 0.44 : 0.27) * this.zoom, ox = W / 2, oy = H / 2;
    const proj = (x, y, z, out, o) => {
      const x1 = x * cy - z * sy, z1 = x * sy + z * cy;      /* yaw around axis 2, then pitch */
      const y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
      const persp = this.flat ? 1 : 3 / (3 + z2);
      out[o] = ox + x1 * scale * persp;
      out[o + 1] = oy - y2 * scale * persp;
      out[o + 2] = z2;
    };
    if (!this.pts.length) return;

    const a = new Float32Array(6);
    g.lineWidth = 1; g.strokeStyle = axis; g.fillStyle = axis; g.font = '11px system-ui, sans-serif';
    const ax = this.flat ? [[1, 0, 0], [0, 1, 0]] : [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
    ax.forEach(([x, y, z], k) => {
      proj(-x, -y, -z, a, 0); proj(x, y, z, a, 3);
      g.beginPath(); g.moveTo(a[0], a[1]); g.lineTo(a[3], a[4]); g.stroke();
      const lab = this.axes[k] || '';
      if (this.flat && k === 0) g.fillText(lab, a[3] - g.measureText(lab).width, a[4] - 4);
      else g.fillText(lab, a[3] + 3, a[4] - 3);
    });

    const n = this.pts.length / 3, s = this.screen;
    for (let i = 0; i < n; i++) proj(this.pts[3 * i], this.pts[3 * i + 1], this.pts[3 * i + 2], s, 3 * i);
    for (let i = 0; i < n; i++) this.order[i] = i;
    if (!this.flat) this.order.sort((p, q) => s[3 * q + 2] - s[3 * p + 2]);   /* far first */
    if (this.focus) { const f = this.focus; this.order.sort((p, q) => (f.has(p) ? 1 : 0) - (f.has(q) ? 1 : 0)); }   /* focused on top */
    const r = this._dotR(n);
    for (let k = 0; k < n; k++) {
      const i = this.order[k];
      g.fillStyle = this.col[i] || ink;
      const a = this.flat ? 0.8 : 0.55 + 0.45 * (1 - (s[3 * i + 2] + 1.5) / 3);
      g.globalAlpha = this.focus && !this.focus.has(i) ? a * 0.15 : a;
      g.beginPath(); g.arc(s[3 * i], s[3 * i + 1], r, 0, 6.2832); g.fill();
    }
    g.globalAlpha = 1;
  }
  _dotR(n) { return n > 3000 ? 1.6 : n > 800 ? 2.2 : 3; }

  draw() {
    const c = this.c, dpr = window.devicePixelRatio || 1;
    const W = c.clientWidth, H = c.clientHeight;
    if (!W || !H) return;
    if (c.width !== Math.round(W * dpr) || c.height !== Math.round(H * dpr)) {
      c.width = Math.round(W * dpr); c.height = Math.round(H * dpr); this.dirty = true;
    }
    if (this.dirty) { this._renderBase(W, H, dpr); this.dirty = false; }
    const g = this.ctx;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.clearRect(0, 0, c.width, c.height);
    g.drawImage(this.base, 0, 0);
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    const cs = getComputedStyle(c);
    const ink = cs.getPropertyValue('--map-ink').trim() || '#ddd';
    const hot = cs.getPropertyValue('--accent').trim() || '#f97316';
    const n = this.pts.length / 3, s = this.screen, r = this._dotR(n);

    /* grain flashes: a ring that grows and fades over 350 ms */
    const now = performance.now();
    for (const [i, t] of this.flashes) {
      const age = (now - t) / 350;
      if (age >= 1 || i >= n) { this.flashes.delete(i); continue; }
      g.globalAlpha = 1 - age;
      g.fillStyle = this.col[i] || hot;
      g.beginPath(); g.arc(s[3 * i], s[3 * i + 1], r + 1.5 + age * 6, 0, 6.2832); g.fill();
      g.strokeStyle = ink; g.lineWidth = 1;
      g.beginPath(); g.arc(s[3 * i], s[3 * i + 1], r + 3 + age * 9, 0, 6.2832); g.stroke();
    }
    g.globalAlpha = 1;
    if (this.cursor) {
      g.strokeStyle = ink; g.lineWidth = 1; g.setLineDash([3, 3]);
      g.beginPath(); g.arc(this.cursor.x, this.cursor.y, this.radiusPx(), 0, 6.2832); g.stroke();
      g.setLineDash([]);
    }
    for (const [i, rad] of [[this.hover, r + 4], [this.sel, r + 6]]) {
      if (i < 0 || i >= n) continue;
      g.strokeStyle = ink; g.lineWidth = 1.5;
      g.beginPath(); g.arc(s[3 * i], s[3 * i + 1], rad, 0, 6.2832); g.stroke();
    }
  }
}
window.Map3D = Map3D;
