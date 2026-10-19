/* Concat Corpus Builder — 3D descriptor map on a plain 2D canvas (no libraries).
 * Points are PCA coordinates (x,y,z). Drag to orbit, wheel to zoom, click a point to
 * pick it. "Move view" looks along PC3, which is the 2-D Learned map Concat
 * navigates on the device (PC1 across, PC2 up). */
'use strict';

class Map3D {
  constructor(canvas, opts) {
    this.c = canvas;
    this.ctx = canvas.getContext('2d');
    this.onPick = opts.onPick || (() => {});
    this.onHover = opts.onHover || (() => {});
    this.pts = new Float32Array(0);   /* x,y,z per point, normalised to [-1,1] */
    this.col = [];                    /* css colour per point */
    this.yaw = -0.6; this.pitch = 0.45; this.zoom = 1.0;
    this.hover = -1; this.sel = -1;
    this.screen = new Float32Array(0);
    this.order = new Uint32Array(0);
    this._bind();
    new ResizeObserver(() => this.draw()).observe(canvas);
  }

  setPoints(xyz, colours) {
    const n = xyz.length / 3;
    /* Each axis min..max -> -1..1, like the Move maps its Learned axes to 0..1, so the
     * Move view shows the same spread you navigate with X/Y on the device. */
    const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < n; i++) for (let c = 0; c < 3; c++) { const v = xyz[3 * i + c]; if (v < lo[c]) lo[c] = v; if (v > hi[c]) hi[c] = v; }
    this.pts = new Float32Array(xyz.length);
    for (let i = 0; i < n; i++) for (let c = 0; c < 3; c++) {
      const r = hi[c] - lo[c];
      this.pts[3 * i + c] = r > 1e-12 ? ((xyz[3 * i + c] - lo[c]) / r) * 2 - 1 : 0;
    }
    this.col = colours;
    this.screen = new Float32Array(n * 3);
    this.order = new Uint32Array(n);
    this.hover = -1;
    if (this.sel >= n) this.sel = -1;
    this.draw();
  }

  moveView() { this.yaw = 0; this.pitch = 0; this.draw(); }   /* PC1 across, PC2 up */
  orbitView() { this.yaw = -0.6; this.pitch = 0.45; this.draw(); }
  select(i) { this.sel = i; this.draw(); }

  _bind() {
    let drag = null;
    this.c.addEventListener('pointerdown', (e) => {
      drag = { x: e.clientX, y: e.clientY, yaw: this.yaw, pitch: this.pitch, moved: false };
      this.c.setPointerCapture(e.pointerId);
    });
    this.c.addEventListener('pointermove', (e) => {
      if (drag) {
        const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
        if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
        this.yaw = drag.yaw + dx * 0.008;
        this.pitch = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, drag.pitch + dy * 0.008));
        this.draw();
        return;
      }
      const i = this._hit(e);
      if (i !== this.hover) { this.hover = i; this.draw(); this.onHover(i); }
    });
    this.c.addEventListener('pointerup', (e) => {
      const d = drag; drag = null;
      if (d && !d.moved) { const i = this._hit(e); if (i >= 0) { this.sel = i; this.draw(); this.onPick(i); } }
    });
    this.c.addEventListener('pointerleave', () => { if (this.hover >= 0) { this.hover = -1; this.draw(); this.onHover(-1); } });
    this.c.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.zoom = Math.max(0.4, Math.min(8, this.zoom * Math.exp(-e.deltaY * 0.0015)));
      this.draw();
    }, { passive: false });
  }

  _hit(e) {
    const r = this.c.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    let best = -1, bd = 64;   /* 8 px radius */
    const s = this.screen, n = s.length / 3;
    for (let i = 0; i < n; i++) {
      const dx = s[3 * i] - x, dy = s[3 * i + 1] - y, d = dx * dx + dy * dy;
      if (d < bd) { bd = d; best = i; }
    }
    return best;
  }

  draw() {
    const c = this.c, dpr = window.devicePixelRatio || 1;
    const W = c.clientWidth, H = c.clientHeight;
    if (!W || !H) return;
    if (c.width !== Math.round(W * dpr) || c.height !== Math.round(H * dpr)) {
      c.width = Math.round(W * dpr); c.height = Math.round(H * dpr);
    }
    const g = this.ctx;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    const cs = getComputedStyle(c);
    const axis = cs.getPropertyValue('--map-axis').trim() || '#555';
    const ink = cs.getPropertyValue('--map-ink').trim() || '#ddd';

    const cy = Math.cos(this.yaw), sy = Math.sin(this.yaw), cp = Math.cos(this.pitch), sp = Math.sin(this.pitch);
    const scale = Math.min(W, H) * 0.27 * this.zoom, ox = W / 2, oy = H / 2;   /* a rotated cube still fits */
    const proj = (x, y, z, out, o) => {
      /* yaw around the vertical (PC2) axis, then pitch */
      const x1 = x * cy - z * sy, z1 = x * sy + z * cy;
      const y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
      const persp = 3 / (3 + z2);
      out[o] = ox + x1 * scale * persp;
      out[o + 1] = oy - y2 * scale * persp;
      out[o + 2] = z2;
    };

    if (!this.pts.length) return;   /* empty: the page shows a hint instead */

    /* axes */
    const a = new Float32Array(6);
    g.lineWidth = 1; g.strokeStyle = axis; g.fillStyle = axis; g.font = '11px system-ui, sans-serif';
    [[1, 0, 0, 'PC1'], [0, 1, 0, 'PC2'], [0, 0, 1, 'PC3']].forEach(([x, y, z, l]) => {
      proj(-x, -y, -z, a, 0); proj(x, y, z, a, 3);
      g.beginPath(); g.moveTo(a[0], a[1]); g.lineTo(a[3], a[4]); g.stroke();
      g.fillText(l, a[3] + 3, a[4] - 3);
    });

    const n = this.pts.length / 3, s = this.screen;
    for (let i = 0; i < n; i++) proj(this.pts[3 * i], this.pts[3 * i + 1], this.pts[3 * i + 2], s, 3 * i);
    for (let i = 0; i < n; i++) this.order[i] = i;
    this.order.sort((p, q) => s[3 * q + 2] - s[3 * p + 2]);   /* far first */
    const r = n > 3000 ? 1.6 : n > 800 ? 2.2 : 3;
    for (let k = 0; k < n; k++) {
      const i = this.order[k];
      g.fillStyle = this.col[i] || ink;
      g.globalAlpha = 0.55 + 0.45 * (1 - (s[3 * i + 2] + 1.5) / 3);
      g.beginPath(); g.arc(s[3 * i], s[3 * i + 1], r, 0, 6.2832); g.fill();
    }
    g.globalAlpha = 1;
    for (const [i, rad] of [[this.hover, r + 4], [this.sel, r + 6]]) {
      if (i < 0 || i >= n) continue;
      g.strokeStyle = ink; g.lineWidth = 1.5;
      g.beginPath(); g.arc(s[3 * i], s[3 * i + 1], rad, 0, 6.2832); g.stroke();
    }
  }
}
window.Map3D = Map3D;
