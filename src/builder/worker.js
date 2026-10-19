/* Concat Corpus Builder — analysis worker.
 * Runs cb.js (Concat's own decode + slicing + descriptor code, compiled to WASM) off
 * the page thread. One request at a time:
 *   { id, op: 'analyse', bytes, name, start, end, slicing: {mode,min_ms,max_ms,gate}, peaks, audio }
 *     -> { id, ok, frames, slices: Int32Array [start,len]*, desc: Float32Array, ndesc,
 *          peaks?: Float32Array [min,max]*N over the whole file, audio?: Float32Array }
 */
importScripts('cb.js');

let M = null;
const ready = ConcatCB().then((m) => { M = m; });

function peaksOf(heap, ptr, n, bins) {
  const out = new Float32Array(bins * 2);
  const per = n / bins;
  for (let b = 0; b < bins; b++) {
    const a = Math.floor(b * per), z = Math.max(a + 1, Math.floor((b + 1) * per));
    let lo = 0, hi = 0;
    for (let i = a; i < z && i < n; i++) {
      const v = heap[ptr + i];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    out[2 * b] = lo; out[2 * b + 1] = hi;
  }
  return out;
}

self.onmessage = async (ev) => {
  const q = ev.data;
  await ready;
  try {
    const bytes = new Uint8Array(q.bytes);
    const p = M._cb_alloc(bytes.length);
    if (!p) throw new Error('out of memory');
    M.HEAPU8.set(bytes, p);
    const name = M.stringToNewUTF8(q.name);
    const loaded = M._cb_load(p, bytes.length, name);   /* takes ownership of p */
    M._cb_free(name);
    if (!loaded) { M._cb_unload(); self.postMessage({ id: q.id, ok: false, error: "can't decode" }); return; }

    const res = { id: q.id, ok: true };
    const transfer = [];
    if (q.peaks || q.audio) {
      const n = M._cb_decode();
      res.frames = n;
      if (n > 0) {
        const ap = M._cb_audio() >> 2;
        if (q.peaks) { res.peaks = peaksOf(M.HEAPF32, ap, n, q.peaks); transfer.push(res.peaks.buffer); }
        if (q.audio) { res.audio = M.HEAPF32.slice(ap, ap + n); transfer.push(res.audio.buffer); }
      }
    }
    if (q.slicing) {
      const s = q.slicing;
      const k = M._cb_analyse(q.start || 0, q.end || 0, s.mode === 'fixed' ? 1 : 0, s.min_ms, s.max_ms, s.gate);
      if (k < 0) throw new Error("can't decode");
      const nd = M._cb_ndesc();
      const sp = M._cb_slices() >> 2, dp = M._cb_desc() >> 2;
      res.ndesc = nd;
      res.slices = M.HEAP32.slice(sp, sp + 2 * k);
      res.desc = M.HEAPF32.slice(dp, dp + k * nd);
      transfer.push(res.slices.buffer, res.desc.buffer);
    }
    M._cb_unload();
    self.postMessage(res, transfer);
  } catch (e) {
    try { M._cb_unload(); } catch (_) { /* ignore */ }
    self.postMessage({ id: q.id, ok: false, error: String(e.message || e) });
  }
};
