/* Concat — corpus map View (Schwung 1.3.0 as_page).
 *
 * as_page means this is an ORDINARY knob page that also carries a drawer: the 8
 * encoders drive the level's real knobs (x/y pos, radius, density, grain size,
 * k-nearest, smooth, cutoff) and pads reach the DSP normally (Map-mode cluster
 * jumps work). The host reads our extra_keys (map_live, map_pts) every frame and
 * hands them in `o.values`, so drawPage never reads a param itself — the rule
 * that draw-path hooks cannot call getParam holds by construction, and the view
 * animates smoothly (trail, LFO, pad moves) at no per-frame cost.
 *
 * ctx is frame-scoped to the BODY BAND: (0,0) is its top-left, ctx.width/height
 * are the band's (the host draws the header/footer around us). All coords the DSP
 * sends are normalized 0..1 and scaled to the band here; y is flipped (0 = top).
 */

/* The View level's 8 encoders, keyed by param key (the map itself shows no knob
 * cells, so we name the touched one at the foot of the band). int = show whole. */
const KNOBS = {
  x_pos:      { label: 'X Position', int: false },
  y_pos:      { label: 'Y Position', int: false },
  radius:     { label: 'Radius',     int: false },
  density:    { label: 'Density',    int: true  },
  grain_size: { label: 'Grain Size', int: true  },
  k_nearest:  { label: 'K Nearest',  int: true  },
  smooth:     { label: 'Smooth',     int: false },
  cutoff:     { label: 'Filter',     int: false },
};

globalThis.canvas_overlay = {
  drawPage(ctx, o) {
    const W = ctx.width, H = ctx.height;
    if (W < 4 || H < 4) return;
    const vals = (o && o.values) || {};

    const sx = (v) => (+v) * (W - 1);
    const sy = (v) => (1 - (+v)) * (H - 1);   /* normalized y-up → pixel y-down */

    /* Scatter background: normalized "x,y;x,y;..." — one lit pixel per slice. */
    const ptsRaw = String(vals.map_pts || '');
    if (ptsRaw) {
      const pts = ptsRaw.split(';');
      for (let i = 0; i < pts.length; i++) {
        const c = pts[i].split(',');
        if (c.length < 2) continue;
        ctx.setPixel(sx(c[0]), sy(c[1]), 1);
      }
    } else {
      ctx.print(2, (H >> 1) - 3, 'load a corpus', 1);
    }

    /* Live overlay: normalized "qx,qy,qr[,tx,ty]*" (query point + played trail). */
    const live = String(vals.map_live || '0,0,0').split(',');
    if (live.length >= 3) {
      const qx = sx(live[0]), qy = sy(live[1]);
      const qr = (+live[2]) * (Math.min(W, H) * 0.5);

      /* Trail oldest→newest so the freshest sits on top; fades with age. */
      for (let i = live.length - 2; i >= 3; i -= 2) {
        const tx = sx(live[i]), ty = sy(live[i + 1]);
        const age = (live.length - 1 - i) / 2 | 0;
        if (age <= 1) ctx.fillCircle(tx, ty, 1, 1);
        else if (age <= 3) ctx.setPixel(tx, ty, 1);
      }

      if (qr >= 1) ctx.drawCircle(qx, qy, qr, 1);
      /* Query dot: filled core + outline ring for contrast over the cluster. */
      ctx.fillCircle(qx, qy, 2, 1);
      ctx.drawCircle(qx, qy, 3, 1);
    }

    /* Recipe browser (preset_browser as_page): the host hands us the selection
     * because it is not a parameter and no read would find it. Name top-left,
     * "i/n" top-right, on a cleared strip so it reads over the scatter; an
     * underline while the browser is entered (jog steps recipes). */
    const pr = o && o.preset;
    if (pr && pr.count > 0) {
      const label = String(pr.name || ('Recipe ' + (pr.index + 1)));
      const idx = (pr.index + 1) + '/' + pr.count;
      ctx.fillRect(0, 0, W, 8, 0);
      ctx.print(2, 1, label, 1);
      ctx.print(W - idx.length * 5 - 2, 1, idx, 1);
      if (pr.entered) ctx.fillRect(0, 8, W, 1, 1);
    }

    /* Touched-knob readout at the foot of the band. The map has no knob cells,
     * so name whatever the user is holding — the host passes the touched slot,
     * the page's keys, and their values. Gone the moment the knob is released. */
    const t = (o && typeof o.touched === 'number') ? o.touched : -1;
    const keys = (o && o.keys) || [];
    if (t >= 0 && t < keys.length) {
      const k = keys[t];
      const meta = KNOBS[k];
      if (meta) {
        const raw = parseFloat(vals[k]);
        let vs = '';
        if (isFinite(raw)) vs = meta.int ? String(Math.round(raw))
                                         : (Math.abs(raw) >= 10 ? raw.toFixed(1) : raw.toFixed(2));
        const line = meta.label + (vs ? '  ' + vs : '');
        ctx.fillRect(0, H - 8, W, 8, 0);
        ctx.print(2, H - 7, line, 1);
      }
    }
  },
};
