/* Concat — fullscreen map view.
 *
 * Background: a 128x64 grayscale BMP written by the analyst (dots per slice
 * darkened; draw_image thresholds it for the 1-bit OLED).
 * Overlays every frame: query dot + radius circle + fading trail of recent picks.
 * Knobs are wired directly (no labels, per the request) so you can navigate
 * without leaving the view; a small non-label value flash appears briefly.
 */

/* 8 knob mappings — turn to sweep, no label shown. Same feel as the live root
 * page but skewed toward navigation (Radius replaces Amp Atk, LFO Rate + K
 * Nearest join in). */
const KNOB_MAP = [
  { key: 'x_pos',      min: 0,    max: 1,    step: 0.02, name: 'X' },
  { key: 'y_pos',      min: 0,    max: 1,    step: 0.02, name: 'Y' },
  { key: 'radius',     min: 0,    max: 1,    step: 0.02, name: 'R' },
  { key: 'density',    min: 1,    max: 100,  step: 1,    name: 'Dns', asInt: true },
  { key: 'grain_size', min: 10,   max: 2000, step: 5,    name: 'Grn', asInt: true },
  { key: 'k_nearest',  min: 1,    max: 16,   step: 1,    name: 'kN',  asInt: true },
  { key: 'lfo_rate',   min: 0.01, max: 10,   step: 0.05, name: 'LFO' },
  { key: 'cutoff',     min: 0,    max: 1,    step: 0.02, name: 'Flt' },
];

/* Relative-encoder decoder for CC 71-78: 1..63 = CW, 65..127 = CCW. */
function decodeKnobDelta(v) {
  if (v === 0 || v === 64) return 0;
  return v < 64 ? v : -(128 - v);
}

function fmtVal(m, v) {
  if (m.asInt) return String(Math.round(v));
  return v >= 10 ? v.toFixed(1) : v.toFixed(2);
}

globalThis.canvas_overlay = {
  onOpen(ctx) {
    ctx.state = ctx.state || {};
    ctx.state.scatter_path = String(ctx.getParam('map_scatter') || '');
    ctx.state.live = '0,0,0';
    ctx.state.tick_i = 0;
    ctx.state.flash_text = '';
    ctx.state.flash_until = 0;
  },

  tick(ctx) {
    ctx.state.live = String(ctx.getParam('map_live') || '0,0,0');
    /* Scatter path only changes on Rescan — poll cheaply once a second. */
    if ((++ctx.state.tick_i % 20) === 0) {
      const p = String(ctx.getParam('map_scatter') || '');
      if (p && p !== ctx.state.scatter_path) ctx.state.scatter_path = p;
    }
  },

  draw(ctx) {
    ctx.clear();

    /* Background scatter (mono OLED: draw_image thresholds the grayscale BMP;
     * our scatter darkens dots to <60, background is 255 = white). */
    if (ctx.state.scatter_path) {
      /* threshold=128, invert=0 → pixels darker than 128 render lit. */
      try { draw_image(ctx.state.scatter_path, 0, 0, 128, 0); } catch (e) {}
    } else {
      ctx.print(4, 28, 'load a corpus', 1);
    }

    /* Live overlay: "qx,qy,qr[,tx,ty]*" */
    const parts = ctx.state.live.split(',');
    if (parts.length >= 3) {
      const qx = +parts[0] | 0, qy = +parts[1] | 0, qr = +parts[2] | 0;

      /* Trail: oldest to newest so the freshest is drawn on top. */
      for (let i = parts.length - 2; i >= 3; i -= 2) {
        const tx = +parts[i] | 0, ty = +parts[i + 1] | 0;
        const age = (parts.length - 1 - i) / 2 | 0;
        if (age <= 1) fill_circle(tx, ty, 1);
        else if (age <= 3) ctx.setPixel(tx, ty, 1);
      }

      if (qr >= 1) draw_circle(qx, qy, qr, 1);
      /* Query dot: fill 3px + outline 4px for maximum contrast on cluster. */
      fill_circle(qx, qy, 2);
      draw_circle(qx, qy, 3, 1);
    }

    /* Ephemeral value flash — a short line at the bottom (no label popup would
     * fit; just a tiny K# : value that fades after ~700ms of no knob turns). */
    if (ctx.state.flash_text && ctx.now() < ctx.state.flash_until) {
      fill_rect(0, 57, 128, 7, 0);     /* black strip so text stays readable */
      ctx.print(1, 58, ctx.state.flash_text, 1);
    }
  },

  onMidi(ctx, msg) {
    const d = msg && msg.data;
    if (!d || d.length < 3) return;
    if ((d[0] & 0xF0) !== 0xB0) return;   /* CCs only; pads still reach the DSP */

    const cc = d[1];
    if (cc >= 71 && cc <= 78) {
      const m = KNOB_MAP[cc - 71];
      const delta = decodeKnobDelta(d[2]);
      if (!delta) return;

      let cur = parseFloat(String(ctx.getParam(m.key) || '0'));
      if (!isFinite(cur)) cur = m.min;
      let nv = cur + delta * m.step;
      if (nv < m.min) nv = m.min;
      if (nv > m.max) nv = m.max;
      if (m.asInt) nv = Math.round(nv);
      ctx.setParam(m.key, String(nv));

      ctx.state.flash_text = 'K' + (cc - 70) + ' ' + m.name + ' ' + fmtVal(m, nv);
      ctx.state.flash_until = ctx.now() + 700;
    }
  },

  onClose(ctx) {},
  onExit(ctx) {},
};
