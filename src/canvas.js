/* Concat — fullscreen map view (canvas overlay for the "View" menu entry).
 *
 * Draws a static scatter of the corpus (a 128x64 8-bit BMP written by the
 * analyst thread, one per corpus / rescan) as the background, then overlays
 * per-frame:
 *   - the current query position as a filled dot,
 *   - a hollow circle at the query radius,
 *   - the last few played slices as a fading trail.
 *
 * All heavy pixel work is offloaded to the BMP (drawn once via draw_image).
 * Per-frame the module returns ~60 bytes of CSV state via get_param("map_live"),
 * which is cheap enough to serve on the SPI callback every repaint.
 */

globalThis.canvas_overlay = {
  onOpen(ctx) {
    ctx.state = ctx.state || {};
    ctx.state.scatter_path = String(ctx.getParam('map_scatter') || '');
    ctx.state.live = '0,0,0';
    ctx.state.tick_i = 0;
  },

  tick(ctx) {
    /* Re-read the live state cheaply. Scatter path only refreshes every ~1s. */
    ctx.state.live = String(ctx.getParam('map_live') || '0,0,0');
    if ((++ctx.state.tick_i % 20) === 0) {
      const p = String(ctx.getParam('map_scatter') || '');
      if (p && p !== ctx.state.scatter_path) ctx.state.scatter_path = p;
    }
  },

  draw(ctx) {
    ctx.clear();

    /* Background: the corpus scatter. If missing, hint the user. */
    if (ctx.state.scatter_path) {
      /* draw_image(path, x, y, threshold, invert) — threshold=128 turns the
       * grayscale into 1-bit for the OLED; invert=1 so dark corpus dots read
       * as lit pixels on the dark screen. */
      try { draw_image(ctx.state.scatter_path, 0, 0, 128, 1); } catch (e) {}
    } else {
      ctx.print(4, 28, 'no corpus loaded', 1);
    }

    /* Parse "qx,qy,qr[,tx,ty]*" from map_live. */
    const parts = ctx.state.live.split(',');
    if (parts.length < 3) return;
    const qx = +parts[0] | 0, qy = +parts[1] | 0, qr = +parts[2] | 0;

    /* Fading trail: draw the most recent slices smallest+dimmest first. */
    for (let i = parts.length - 2; i >= 3; i -= 2) {
      const tx = +parts[i] | 0, ty = +parts[i + 1] | 0;
      const age = (parts.length - 1 - i) / 2 | 0;
      if (age <= 1) fill_circle(tx, ty, 1);              /* freshest: solid */
      else if (age <= 3) ctx.setPixel(tx, ty, 1);        /* middle: single pixel */
      /* older: skip (implicit fade) */
    }

    /* Radius circle. */
    if (qr >= 1) draw_circle(qx, qy, qr, 1);

    /* Query dot: filled + outlined for high contrast against the scatter. */
    fill_circle(qx, qy, 2);
    draw_circle(qx, qy, 3, 1);
  },

  onMidi(ctx, msg) {
    /* Pass through pad hits so the view is playable in Notes mode without
     * leaving. Do nothing else — the host still runs its own MIDI routing. */
  },

  onClose(ctx) {},
  onExit(ctx) {},
};
