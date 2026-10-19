/* Concat Corpus Builder.
 * Served by schwung-manager from the Concat module folder, so it talks to the
 * manager's /files routes on the same origin (listing, Range downloads, uploads).
 * Analysis runs in worker.js with Concat's own C code compiled to WASM; results are
 * cached in IndexedDB by path + size + mtime + range + slicing.
 * Writes <UserLibrary>/Corpora/<name>.concat (temp upload + rename). Concat on the
 * Move hot-reloads a loaded corpus file when it changes. */
'use strict';

const LIB = '/data/UserData/UserLibrary';
const CORPORA = LIB + '/Corpora';
const AUDIO_RE = /\.(wav|aif|aiff|flac|mp3)$/i;
const SR = 44100;
const NDESC = 36;
const PEAK_BINS = 1600;
const DEFAULT_SLICING = { mode: 'onset', min_ms: 20, max_ms: 2000, gate: 0.02 };   /* = Concat's knob defaults */
const PALETTE = ['#e4572e', '#29a3a3', '#f2a541', '#6d6ad6', '#76b041', '#d6457f', '#3b82c4',
                 '#b07d3a', '#8f5bd6', '#2fa36b', '#c94f4f', '#5c8a99'];

/* Descriptor layout (src/dsp/corpus.h) and the groups a .concat file can name. */
const DIM_NAMES = [];
for (let k = 0; k < 13; k++) DIM_NAMES[k] = `MFCC ${k + 1} mean`;
DIM_NAMES[13] = 'Centroid mean'; DIM_NAMES[14] = 'Flatness mean'; DIM_NAMES[15] = 'ZCR mean';
for (let k = 0; k < 13; k++) DIM_NAMES[16 + k] = `MFCC ${k + 1} std`;
DIM_NAMES[29] = 'Centroid std'; DIM_NAMES[30] = 'Flatness std'; DIM_NAMES[31] = 'ZCR std';
DIM_NAMES[32] = 'Loudness mean'; DIM_NAMES[33] = 'Loudness peak'; DIM_NAMES[34] = 'Pitch (Hz)'; DIM_NAMES[35] = 'Pitch confidence';
const GROUPS = [
  { id: 'loud', label: 'Loudness', dims: [32, 33] },
  { id: 'centroid', label: 'Brightness (spectral centroid)', dims: [13, 29] },
  { id: 'flatness', label: 'Noisiness (spectral flatness)', dims: [14, 30] },
  { id: 'zcr', label: 'Zero-crossing rate', dims: [15, 31] },
  { id: 'pitch', label: 'Pitch', dims: [34, 35] },
];
for (let k = 1; k <= 13; k++) GROUPS.push({ id: `mfcc${k}`, label: `Timbre: MFCC ${k}`, dims: [k - 1, 16 + k - 1] });

/* ── small helpers ─────────────────────────────────────────────────────────── */
const $ = (id) => document.getElementById(id);
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined) e.append(c instanceof Node ? c : String(c));
  return e;
}
const baseName = (p) => p.slice(p.lastIndexOf('/') + 1);
const dirName = (p) => p.slice(0, p.lastIndexOf('/')) || '/';
const relPath = (p) => (p.startsWith(LIB + '/') ? p.slice(LIB.length + 1) : p);
const absPath = (p) => (p.startsWith('/') ? p : `${LIB}/${p}`);
function fmtTime(s) {
  if (!isFinite(s)) return '–';
  const m = Math.floor(s / 60), r = s - m * 60;
  return m ? `${m}:${r.toFixed(1).padStart(4, '0')}` : `${r.toFixed(2)} s`;
}
function fmtNum(v) {
  const a = Math.abs(v);
  if (a === 0) return '0';
  if (a >= 1000) return v.toFixed(0);
  if (a >= 10) return v.toFixed(1);
  if (a >= 0.01) return v.toFixed(3);
  return v.toExponential(1);
}
/* fnmatch-style glob (* ? [..]), case-insensitive, like the device (FNM_CASEFOLD). */
function globRe(g) {
  let r = '';
  for (let i = 0; i < g.length; i++) {
    const c = g[i];
    if (c === '*') r += '.*';
    else if (c === '?') r += '.';
    else if (c === '[') { const j = g.indexOf(']', i + 1); if (j > i) { r += '[' + g.slice(i + 1, j).replace(/^!/, '^').replace(/\\/g, '\\\\') + ']'; i = j; } else r += '\\['; }
    else r += c.replace(/[.+^${}()|\\\]]/g, '\\$&');
  }
  return new RegExp('^' + r + '$', 'i');
}
const anyMatch = (globs, name) => (globs || []).some((g) => globRe(g).test(name));
function setStatus(msg, kind) { const s = $('status'); s.textContent = msg || ''; s.className = kind || ''; s.title = msg || ''; }
function csrf() { const m = document.cookie.match(/(?:^|;\s*)csrf_token=([^;]+)/); return m ? decodeURIComponent(m[1]) : ''; }

/* ── server I/O (schwung-manager /files) ───────────────────────────────────── */
async function listDir(path) {
  const r = await fetch('/files?path=' + encodeURIComponent(path), { credentials: 'same-origin' });
  if (!r.ok) throw new Error(`list ${path}: ${r.status}`);
  const doc = new DOMParser().parseFromString(await r.text(), 'text/html');
  const out = [];
  for (const tr of doc.querySelectorAll('tr.file-row[data-path]')) {
    const name = tr.getAttribute('data-name') || baseName(tr.getAttribute('data-path'));
    if (name.startsWith('.')) continue;
    out.push({ path: tr.getAttribute('data-path'), name, dir: tr.getAttribute('data-isdir') === 'true' });
  }
  out.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' }));
  return out;
}
const dlUrl = (p) => '/files/download?path=' + encodeURIComponent(p);
async function fileMeta(path) {
  const r = await fetch(dlUrl(path), { method: 'HEAD' });
  if (!r.ok) throw new Error(`${baseName(path)}: ${r.status}`);
  return { size: +(r.headers.get('Content-Length') || 0), mtime: r.headers.get('Last-Modified') || '' };
}
/* Range chunks: no single response runs into the manager's 60 s write timeout. */
async function download(path, onProgress) {
  const CH = 4 << 20;
  const parts = [];
  let off = 0, total = -1;
  for (;;) {
    const r = await fetch(dlUrl(path), { headers: { Range: `bytes=${off}-${off + CH - 1}` } });
    if (r.status === 416) break;                         /* empty file */
    if (r.status === 200) { const b = new Uint8Array(await r.arrayBuffer()); onProgress && onProgress(1); return b; }
    if (r.status !== 206) throw new Error(`${baseName(path)}: ${r.status}`);
    const cr = r.headers.get('Content-Range') || '';
    total = +(cr.split('/')[1] || 0);
    const b = new Uint8Array(await r.arrayBuffer());
    parts.push(b); off += b.length;
    onProgress && total && onProgress(off / total);
    if (!b.length || off >= total) break;
  }
  const out = new Uint8Array(off);
  let p = 0;
  for (const b of parts) { out.set(b, p); p += b.length; }
  return out;
}
async function postForm(url, fields, file) {
  const fd = new FormData();
  for (const [k, v] of Object.entries(fields)) fd.append(k, v);
  if (file) fd.append('file', file.blob, file.name);
  const r = await fetch(url, { method: 'POST', body: fd, headers: { 'X-CSRF-Token': csrf() }, credentials: 'same-origin' });
  if (r.status === 403) throw new Error('the Move refused the write (CSRF). Reload the page and try again.');
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r;
}

/* ── analysis cache (IndexedDB) ───────────────────────────────────────────── */
const idb = (() => {
  let dbp = null;
  const open = () => dbp || (dbp = new Promise((res) => {
    let rq;
    try { rq = indexedDB.open('concat-builder', 1); } catch (e) { res(null); return; }
    rq.onupgradeneeded = () => { rq.result.createObjectStore('analysis'); rq.result.createObjectStore('peaks'); };
    rq.onsuccess = () => res(rq.result);
    rq.onerror = () => res(null);
  }));
  const tx = async (store, mode, fn) => {
    const db = await open();
    if (!db) return undefined;
    return new Promise((res) => {
      try {
        const t = db.transaction(store, mode), s = t.objectStore(store), rq = fn(s);
        t.oncomplete = () => res(rq && rq.result); t.onerror = () => res(undefined); t.onabort = () => res(undefined);
      } catch (e) { res(undefined); }
    });
  };
  return {
    get: (store, key) => tx(store, 'readonly', (s) => s.get(key)),
    put: (store, key, val) => tx(store, 'readwrite', (s) => s.put(val, key)),
  };
})();

/* ── worker RPC ───────────────────────────────────────────────────────────── */
const worker = new Worker('worker.js');
let rpcId = 0;
const pending = new Map();
worker.onmessage = (e) => { const p = pending.get(e.data.id); if (p) { pending.delete(e.data.id); p(e.data); } };
function work(msg, transfer) {
  return new Promise((res) => { const id = ++rpcId; pending.set(id, res); worker.postMessage({ ...msg, id }, transfer || []); });
}

/* ── state ─────────────────────────────────────────────────────────────────── */
let nextId = 1;
const S = {
  name: 'Untitled',
  sources: [],          /* {id, kind:'file'|'folder', path, start, end, recurse, include[], exclude[], files[], missing, colour} */
  exclude: [],          /* top-level globs: apply to folder sources */
  slicing: { ...DEFAULT_SLICING },
  use: new Set(GROUPS.map((g) => g.id)),
  sel: null,            /* selected source id */
  dirty: false,
  loadedName: null,     /* file name it was opened from / saved as */
};
const meta = new Map();       /* path -> {size, mtime} */
const results = new Map();    /* analysis key -> {slices, desc, frames?} */
const peaksMem = new Map();   /* path|size|mtime -> {peaks, frames} */

function markDirty() { S.dirty = true; updateTitle(); saveDraft(); }
/* Unsaved work survives a reload: the draft is the .concat text plus where it came from. */
const DRAFT_KEY = 'concat-builder-draft';
function saveDraft() {
  try { localStorage.setItem(DRAFT_KEY, JSON.stringify({ doc: JSON.parse(toJSON()), loadedName: S.loadedName })); } catch (e) { /* ignore */ }
}
function clearDraft() { try { localStorage.removeItem(DRAFT_KEY); } catch (e) { /* ignore */ } }
function updateTitle() { document.title = `${S.dirty ? '• ' : ''}${S.name} · Concat Corpus Builder`; }
function colourOf(i) { return PALETTE[i % PALETTE.length]; }

/* Items = what the Move will analyse: folder sources expanded with their filters. */
function items() {
  const out = [];
  S.sources.forEach((s, si) => {
    if (s.missing) return;
    if (s.kind === 'file') { out.push({ path: s.path, start: s.start || 0, end: s.end || 0, src: si }); return; }
    for (const f of s.files || []) {
      const bn = baseName(f);
      if (s.include && s.include.length && !anyMatch(s.include, bn)) continue;
      if (anyMatch(s.exclude, bn) || anyMatch(S.exclude, bn)) continue;
      out.push({ path: f, start: 0, end: 0, src: si });
    }
  });
  return out;
}
function keyOf(it) {
  const m = meta.get(it.path) || {};
  const sl = S.slicing;
  return [it.path, m.size, m.mtime, it.start, it.end, sl.mode, sl.min_ms, sl.max_ms, sl.gate].join('|');
}

async function expandFolder(path, recurse, depth = 0, out = []) {
  if (depth > 8 || out.length > 4000) return out;
  let rows;
  try { rows = await listDir(path); } catch (e) { return out; }
  for (const r of rows) {
    if (r.dir) { if (recurse) await expandFolder(r.path, true, depth + 1, out); }
    else if (AUDIO_RE.test(r.name)) out.push(r.path);
  }
  return out;
}

async function addSource(path, isDir, at) {
  if (S.sources.some((s) => s.path === path)) { setStatus(`${baseName(path)} is already in the corpus`); return; }
  const s = { id: nextId++, kind: isDir ? 'folder' : 'file', path, start: 0, end: 0, recurse: true, include: [], exclude: [], files: [], missing: false };
  if (at === undefined) S.sources.push(s); else S.sources.splice(at, 0, s);
  S.sel = s.id;
  markDirty();
  if (isDir) { setStatus(`Listing ${baseName(path)}…`); s.files = await expandFolder(path, s.recurse); setStatus(''); }
  renderSources(); renderEditor(); scheduleAnalysis();
}

/* ── library panel ─────────────────────────────────────────────────────────── */
let libPath = LIB;
async function showLib(path) {
  libPath = path;
  const crumbs = $('crumbs'); crumbs.textContent = '';
  const rel = path === LIB ? [] : path.slice(LIB.length + 1).split('/');
  const parts = [['UserLibrary', LIB]];
  rel.forEach((p, i) => parts.push([p, LIB + '/' + rel.slice(0, i + 1).join('/')]));
  parts.forEach(([n, p], i) => { if (i) crumbs.append(' / '); crumbs.append(el('a', { onclick: () => showLib(p) }, n)); });
  $('btnUp').disabled = path === LIB;
  const lib = $('lib');
  lib.textContent = 'Loading…';
  let rows;
  try { rows = await listDir(path); } catch (e) { lib.textContent = String(e.message); return; }
  lib.textContent = '';
  const shown = rows.filter((r) => r.dir || AUDIO_RE.test(r.name));
  if (!shown.length) lib.append(el('div', { class: 'lib-row muted' }, 'No folders or audio here'));
  for (const r of shown) {
    const row = el('div', { class: 'lib-row' + (r.dir ? ' dir' : ''), draggable: 'true', title: r.path },
      el('span', { class: 'ic' }, r.dir ? '▸' : '♪'),
      el('span', { class: 'nm' }, r.name),
      r.dir ? null : el('button', { title: 'Listen', onclick: (e) => { e.stopPropagation(); playFile(r.path); } }, '▶'),
      el('button', { title: 'Add to corpus', onclick: (e) => { e.stopPropagation(); addSource(r.path, r.dir); } }, '+'));
    row.addEventListener('dblclick', () => (r.dir ? showLib(r.path) : addSource(r.path, false)));
    if (r.dir) row.addEventListener('click', () => showLib(r.path));
    row.addEventListener('dragstart', (e) => {
      e.dataTransfer.setData('application/x-concat', JSON.stringify({ path: r.path, dir: r.dir }));
      e.dataTransfer.setData('text/plain', r.path);
      e.dataTransfer.effectAllowed = 'copy';
    });
    lib.append(row);
  }
}

/* ── corpus list ──────────────────────────────────────────────────────────── */
function srcSummary(s, si) {
  if (s.missing) return 'missing on the Move';
  const st = srcStats[si];
  if (s.kind === 'folder') {
    const n = items().filter((it) => it.src === si).length;
    return `folder · ${n} file${n === 1 ? '' : 's'}${st ? ` · ${st.slices} slices` : ''}`;
  }
  const m = peaksMem.get(peakKey(s.path));
  const dur = m ? m.frames / SR : NaN;
  const range = s.end > s.start ? `${fmtTime(s.start)} – ${fmtTime(s.end)}` : (s.start > 0 ? `from ${fmtTime(s.start)}` : 'whole file');
  const dup = S.sources.find((o) => o.kind === 'folder' && !o.missing && o !== s && (o.files || []).includes(s.path));
  return `${range}${isFinite(dur) ? ` of ${fmtTime(dur)}` : ''}${st ? ` · ${st.slices} slices` : ''}` +
         (dup ? ` · also in ${baseName(dup.path)} (counted twice: Skip it there)` : '');
}
let srcStats = [];
function renderSources() {
  const box = $('sources');
  box.querySelectorAll('.src').forEach((n) => n.remove());
  $('drop').style.display = S.sources.length ? 'none' : '';
  $('srcCount').textContent = S.sources.length ? `${S.sources.length} source${S.sources.length === 1 ? '' : 's'}` : '';
  S.sources.forEach((s, si) => {
    const row = el('div', { class: 'src' + (S.sel === s.id ? ' sel' : '') + (s.missing ? ' missing' : ''), title: s.path },
      el('span', { class: 'chip', style: `background:${colourOf(si)}` }),
      el('div', { class: 't' }, el('div', {}, (s.kind === 'folder' ? '▸ ' : '') + baseName(s.path)), el('div', { class: 'sub' }, srcSummary(s, si))),
      el('div', { class: 'acts' },
        s.kind === 'file' && !s.missing ? el('button', { class: 'icon', title: 'Listen to the used range', onclick: (e) => { e.stopPropagation(); playFile(s.path, s.start, s.end); } }, '▶') : null,
        el('button', { class: 'icon', title: 'Remove from the corpus (the file stays on the Move)', onclick: (e) => { e.stopPropagation(); removeSource(s.id); } }, '✕')));
    row.addEventListener('click', () => { S.sel = s.id; renderSources(); renderEditor(); });
    box.append(row);
  });
}
function removeSource(id) {
  S.sources = S.sources.filter((s) => s.id !== id);
  if (S.sel === id) S.sel = null;
  markDirty(); renderSources(); renderEditor(); scheduleAnalysis();
}
function bindDrop() {
  const box = $('sources');
  box.addEventListener('dragover', (e) => { if (e.dataTransfer.types.includes('application/x-concat')) { e.preventDefault(); box.classList.add('over'); } });
  box.addEventListener('dragleave', (e) => { if (e.target === box) box.classList.remove('over'); });
  box.addEventListener('drop', (e) => {
    box.classList.remove('over');
    const d = e.dataTransfer.getData('application/x-concat');
    if (!d) return;
    e.preventDefault();
    const { path, dir } = JSON.parse(d);
    addSource(path, dir);
  });
}

/* ── editor (selected source) ─────────────────────────────────────────────── */
let wave = null;   /* {canvas, s, frames, peaks, drag} */
function selected() { return S.sources.find((s) => s.id === S.sel) || null; }
function renderEditor() {
  const ed = $('editor');
  const s = selected();
  ed.textContent = '';
  wave = null;
  if (!s) { ed.classList.remove('on'); return; }
  ed.classList.add('on');
  ed.append(el('div', { class: 'mono muted', title: s.path }, relPath(s.path)));
  if (s.missing) { ed.append(el('div', { class: 'row' }, 'This source is not on the Move any more. Remove it, or put the file back.')); return; }
  if (s.kind === 'file') renderFileEditor(ed, s); else renderFolderEditor(ed, s);
}

function renderFileEditor(ed, s) {
  const cv = el('canvas', { id: 'wave' });
  ed.append(el('div', { class: 'row' }, cv));
  const startIn = el('input', { type: 'number', min: 0, step: 0.01, value: (s.start || 0).toFixed(2) });
  const endIn = el('input', { type: 'number', min: 0, step: 0.01, value: s.end > s.start ? s.end.toFixed(2) : '' , placeholder: 'end' });
  const apply = () => {
    const a = Math.max(0, +startIn.value || 0), b = +endIn.value || 0;
    s.start = a; s.end = b > a ? b : 0;
    markDirty(); drawWave(); renderSources(); scheduleAnalysis();
  };
  startIn.addEventListener('change', apply); endIn.addEventListener('change', apply);
  ed.append(el('div', { class: 'row' },
    el('button', { onclick: () => playFile(s.path, s.start, s.end) }, '▶ Play range'),
    el('button', { onclick: stopAudio }, '■ Stop'),
    el('label', { class: 'muted' }, 'Start ', startIn, ' s'),
    el('label', { class: 'muted' }, 'End ', endIn, ' s'),
    el('button', { onclick: () => { s.start = 0; s.end = 0; startIn.value = '0.00'; endIn.value = ''; markDirty(); drawWave(); renderSources(); scheduleAnalysis(); } }, 'Whole file'),
    el('span', { class: 'muted' }, 'Drag on the waveform to choose the part that enters the corpus.')));
  wave = { canvas: cv, s, frames: 0, peaks: null, inputs: [startIn, endIn] };
  loadPeaks(s.path).then((pk) => { if (wave && wave.s === s && pk) { wave.frames = pk.frames; wave.peaks = pk.peaks; drawWave(); renderSources(); } });
  bindWave(cv, s);
  new ResizeObserver(() => drawWave()).observe(cv);
}

function bindWave(cv, s) {
  const toSec = (e) => {
    const r = cv.getBoundingClientRect();
    return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * (wave.frames / SR);
  };
  let drag = null;
  cv.addEventListener('pointerdown', (e) => {
    if (!wave || !wave.frames) return;
    const t = toSec(e), dur = wave.frames / SR;
    const a = s.start || 0, b = s.end > s.start ? s.end : dur;
    const px = 6 * dur / cv.getBoundingClientRect().width;      /* grab handles within 6 px */
    drag = Math.abs(t - a) < px ? 'a' : Math.abs(t - b) < px ? 'b' : 'new';
    if (drag === 'new') { drag = { anchor: t }; s.start = t; s.end = t; }
    cv.setPointerCapture(e.pointerId);
  });
  cv.addEventListener('pointermove', (e) => {
    if (!drag || !wave) return;
    const t = toSec(e), dur = wave.frames / SR;
    if (drag === 'a') s.start = Math.min(t, (s.end > s.start ? s.end : dur) - 0.01);
    else if (drag === 'b') s.end = Math.max(t, (s.start || 0) + 0.01);
    else { s.start = Math.min(drag.anchor, t); s.end = Math.max(drag.anchor, t); }
    drawWave(); syncInputs();
  });
  cv.addEventListener('pointerup', () => {
    if (!drag || !wave) return;
    drag = null;
    const dur = wave.frames / SR;
    if (s.end - s.start < 0.02) { s.start = 0; s.end = 0; }          /* a click clears the range */
    if (s.end >= dur - 0.005) s.end = 0;                             /* to the end */
    if (s.start < 0.005) s.start = 0;
    syncInputs(); markDirty(); drawWave(); renderSources(); scheduleAnalysis();
  });
  function syncInputs() {
    const [a, b] = wave.inputs;
    a.value = (s.start || 0).toFixed(2);
    b.value = s.end > s.start ? s.end.toFixed(2) : '';
  }
}

function drawWave() {
  if (!wave) return;
  const cv = wave.canvas, dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;
  if (!W) return;
  cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const cs = getComputedStyle(document.documentElement);
  const cWave = cs.getPropertyValue('--wave').trim(), cSel = cs.getPropertyValue('--wave-sel').trim(), cInk = cs.getPropertyValue('--ink2').trim();
  g.clearRect(0, 0, W, H);
  if (!wave.peaks) { g.fillStyle = cInk; g.fillText('Loading waveform…', 10, H / 2); return; }
  const s = wave.s, dur = wave.frames / SR;
  const a = s.start || 0, b = s.end > s.start ? s.end : dur;
  const xa = (a / dur) * W, xb = (b / dur) * W;
  g.fillStyle = cSel; g.globalAlpha = 0.12; g.fillRect(xa, 0, xb - xa, H); g.globalAlpha = 1;
  const n = wave.peaks.length / 2, mid = H / 2;
  for (let x = 0; x < W; x++) {
    const i0 = Math.floor((x / W) * n), i1 = Math.max(i0 + 1, Math.floor(((x + 1) / W) * n));
    let lo = 0, hi = 0;
    for (let i = i0; i < i1 && i < n; i++) { lo = Math.min(lo, wave.peaks[2 * i]); hi = Math.max(hi, wave.peaks[2 * i + 1]); }
    g.fillStyle = x >= xa && x <= xb ? cSel : cWave;
    g.fillRect(x, mid - hi * mid * 0.95, 1, Math.max(1, (hi - lo) * mid * 0.95));
  }
  /* slice boundaries from the analysis of this range */
  const r = results.get(keyOf({ path: s.path, start: s.start || 0, end: s.end || 0 }));
  if (r) {
    g.strokeStyle = cInk; g.globalAlpha = 0.5; g.beginPath();
    for (let i = 0; i < r.slices.length; i += 2) {
      const x = ((a + r.slices[i] / SR) / dur) * W;
      g.moveTo(x + 0.5, 0); g.lineTo(x + 0.5, 6); g.moveTo(x + 0.5, H - 6); g.lineTo(x + 0.5, H);
    }
    g.stroke(); g.globalAlpha = 1;
  }
  g.fillStyle = cSel;
  for (const x of [xa, xb]) g.fillRect(Math.round(x) - 1, 0, 2, H);
  if (playing && playing.path === s.path && actx) {
    const t = playing.from + (actx.currentTime - playing.t0) * playing.rate;
    if (t <= playing.to) { g.fillStyle = cInk; g.fillRect((t / dur) * W, 0, 1, H); }
  }
}

function renderFolderEditor(ed, s) {
  const inc = el('input', { type: 'text', value: (s.include || []).join(', '), placeholder: 'e.g. *Kick*, *.wav', size: 22 });
  const exc = el('input', { type: 'text', value: (s.exclude || []).join(', '), placeholder: 'e.g. *click*', size: 22 });
  const rec = el('input', { type: 'checkbox' }); rec.checked = s.recurse !== false;
  const split = (v) => v.split(',').map((x) => x.trim()).filter(Boolean);
  const apply = async () => {
    s.include = split(inc.value); s.exclude = split(exc.value);
    if (rec.checked !== (s.recurse !== false)) { s.recurse = rec.checked; s.files = await expandFolder(s.path, s.recurse); }
    markDirty(); renderSources(); listFiles(); scheduleAnalysis();
  };
  inc.addEventListener('change', apply); exc.addEventListener('change', apply); rec.addEventListener('change', apply);
  ed.append(el('div', { class: 'row' },
    el('label', { class: 'muted' }, 'Only ', inc), el('label', { class: 'muted' }, 'Skip ', exc),
    el('label', { class: 'muted' }, rec, ' subfolders'),
    el('button', { title: 'Re-read the folder from the Move', onclick: async () => { s.files = await expandFolder(s.path, s.recurse !== false); renderSources(); listFiles(); scheduleAnalysis(); } }, 'Refresh')));
  const box = el('div', { id: 'folderFiles' });
  ed.append(box);
  function listFiles() {
    box.textContent = '';
    const used = new Set(items().filter((it) => S.sources[it.src] === s).map((it) => it.path));
    for (const f of s.files || []) {
      const on = used.has(f), bn = baseName(f);
      box.append(el('div', { class: 'lib-row' + (on ? '' : ' off'), title: f },
        el('span', { class: 'nm' }, relPath(f).slice(relPath(s.path).length + 1) || bn),
        el('button', { title: 'Listen', onclick: () => playFile(f) }, '▶'),
        on ? el('button', { title: 'Leave this file out (adds it to Skip)', onclick: () => { s.exclude = [...(s.exclude || []), bn]; exc.value = s.exclude.join(', '); markDirty(); renderSources(); listFiles(); scheduleAnalysis(); } }, '✕')
           : null));
    }
    if (!(s.files || []).length) box.append(el('div', { class: 'lib-row muted' }, 'No audio files'));
  }
  listFiles();
}

/* ── audio ─────────────────────────────────────────────────────────────────── */
let actx = null, playing = null, rafId = 0;
const audioMem = new Map();   /* path -> Float32Array (LRU, ~40 M samples) */
function ctx() { if (!actx) actx = new AudioContext(); if (actx.state === 'suspended') actx.resume(); return actx; }
function stopAudio() { if (playing) { try { playing.node.stop(); } catch (e) { /* ignore */ } playing = null; } drawWave(); }
async function decodedAudio(path) {
  if (audioMem.has(path)) { const a = audioMem.get(path); audioMem.delete(path); audioMem.set(path, a); return a; }
  setStatus(`Loading ${baseName(path)}…`);
  const bytes = await download(path);
  const r = await work({ op: 'decode', bytes: bytes.buffer, name: path, audio: true }, [bytes.buffer]);
  setStatus('');
  if (!r.ok || !r.audio) throw new Error(r.error || "can't decode");
  audioMem.set(path, r.audio);
  let total = 0;
  for (const a of audioMem.values()) total += a.length;
  for (const [k, a] of audioMem) { if (total <= 40e6 || audioMem.size <= 1) break; audioMem.delete(k); total -= a.length; }
  return r.audio;
}
async function playFrames(path, a0, a1) {
  const c = ctx();
  const audio = await decodedAudio(path);
  const from = Math.max(0, Math.min(audio.length, a0)), to = Math.max(from + 1, Math.min(audio.length, a1 || audio.length));
  stopAudio();
  const buf = c.createBuffer(1, to - from, SR);
  const ch = buf.getChannelData(0);
  ch.set(audio.subarray(from, to));
  const fade = Math.min(64, ch.length >> 2);                    /* no clicks at the cut */
  for (let i = 0; i < fade; i++) { const g = i / fade; ch[i] *= g; ch[ch.length - 1 - i] *= g; }
  const node = c.createBufferSource();
  node.buffer = buf; node.connect(c.destination); node.start();
  playing = { node, path, from: from / SR, to: to / SR, t0: c.currentTime, rate: 1 };
  node.onended = () => { if (playing && playing.node === node) { playing = null; drawWave(); } };
  cancelAnimationFrame(rafId);
  const tick = () => { if (playing && playing.node === node) { drawWave(); rafId = requestAnimationFrame(tick); } };
  tick();
}
function playFile(path, start, end) {
  playFrames(path, Math.round((start || 0) * SR), end > start ? Math.round(end * SR) : 0).catch((e) => setStatus(String(e.message || e), 'bad'));
}

/* ── analysis queue ───────────────────────────────────────────────────────── */
let runToken = 0, analysisTimer = 0;
function scheduleAnalysis() { clearTimeout(analysisTimer); analysisTimer = setTimeout(runAnalysis, 250); }
const peakKey = (path) => { const m = meta.get(path) || {}; return `${path}|${m.size}|${m.mtime}`; };
async function ensureMeta(path) {
  if (!meta.has(path)) meta.set(path, await fileMeta(path));
  return meta.get(path);
}
async function loadPeaks(path) {
  try { await ensureMeta(path); } catch (e) { return null; }
  const k = peakKey(path);
  if (peaksMem.has(k)) return peaksMem.get(k);
  const hit = await idb.get('peaks', k);
  if (hit) { peaksMem.set(k, hit); return hit; }
  const bytes = await download(path);
  const r = await work({ op: 'peaks', bytes: bytes.buffer, name: path, peaks: PEAK_BINS }, [bytes.buffer]);
  if (!r.ok) return null;
  const v = { peaks: r.peaks, frames: r.frames };
  peaksMem.set(k, v); idb.put('peaks', k, v);
  return v;
}

async function runAnalysis() {
  const token = ++runToken;
  const selIdx = S.sources.findIndex((s) => s.id === S.sel);
  const its = items().map((it, i) => [it, i]).sort((a, b) => ((b[0].src === selIdx) - (a[0].src === selIdx)) || (a[1] - b[1])).map((x) => x[0]);   /* selected source first */
  const bar = $('progress').firstElementChild;
  let done = 0, failed = 0;
  for (const it of its) {
    if (token !== runToken) return;                   /* superseded by a newer edit */
    bar.style.width = `${(done / Math.max(1, its.length)) * 100}%`;
    try {
      await ensureMeta(it.path);
      const key = keyOf(it);
      if (!results.has(key)) {
        const hit = await idb.get('analysis', key);
        if (hit) results.set(key, hit);
        else {
          setStatus(`Analysing ${done + 1}/${its.length} · ${baseName(it.path)}`);
          const bytes = await download(it.path);
          if (token !== runToken) return;
          const pk = S.sources[it.src].kind === 'file' && !peaksMem.has(peakKey(it.path)) ? PEAK_BINS : 0;   /* waveform: file sources only */
          const r = await work({ op: 'analyse', bytes: bytes.buffer, name: it.path, start: it.start, end: it.end, slicing: S.slicing, peaks: pk }, [bytes.buffer]);
          if (!r.ok) throw new Error(r.error);
          const v = { slices: r.slices, desc: r.desc };
          results.set(key, v); idb.put('analysis', key, v);
          if (r.peaks) { const pv = { peaks: r.peaks, frames: r.frames }; peaksMem.set(peakKey(it.path), pv); idb.put('peaks', peakKey(it.path), pv); }
        }
      }
    } catch (e) { failed++; console.warn(it.path, e); }
    done++;
    if (done % 8 === 0 && token === runToken) recompute();     /* the map fills in as it goes */
  }
  if (token !== runToken) return;
  bar.style.width = '0';
  if (failed) setStatus(`${failed} file${failed === 1 ? '' : 's'} could not be read`, 'bad');
  else if ($('status').textContent.startsWith('Analysing')) setStatus('');   /* keep other messages */
  recompute();
}

/* ── statistics, PCA, map ─────────────────────────────────────────────────── */
let corpus = null;   /* {n, raw: Float32Array n*NDESC, item: Int32Array, slice: Int32Array, its, proj, stats, eig} */
function percentile(sorted, p) {
  const n = sorted.length;
  if (!n) return 0;
  if (n === 1) return sorted[0];
  const idx = p * (n - 1), lo = Math.floor(idx), fr = idx - lo;
  return lo + 1 < n ? sorted[lo] + fr * (sorted[lo + 1] - sorted[lo]) : sorted[lo];
}
function jacobiEigen(A, d) {
  /* symmetric eigen-decomposition: returns {vals, vecs (column-major d*d)} */
  const a = Float64Array.from(A), v = new Float64Array(d * d);
  for (let i = 0; i < d; i++) v[i * d + i] = 1;
  for (let sweep = 0; sweep < 60; sweep++) {
    let off = 0;
    for (let p = 0; p < d; p++) for (let q = p + 1; q < d; q++) off += a[p * d + q] ** 2;
    if (off < 1e-18) break;
    for (let p = 0; p < d; p++) for (let q = p + 1; q < d; q++) {
      const apq = a[p * d + q];
      if (Math.abs(apq) < 1e-30) continue;
      const th = (a[q * d + q] - a[p * d + p]) / (2 * apq);
      const t = Math.sign(th || 1) / (Math.abs(th) + Math.sqrt(th * th + 1));
      const c = 1 / Math.sqrt(t * t + 1), s = t * c;
      for (let k = 0; k < d; k++) {
        const akp = a[k * d + p], akq = a[k * d + q];
        a[k * d + p] = c * akp - s * akq; a[k * d + q] = s * akp + c * akq;
      }
      for (let k = 0; k < d; k++) {
        const apk = a[p * d + k], aqk = a[q * d + k];
        a[p * d + k] = c * apk - s * aqk; a[q * d + k] = s * apk + c * aqk;
      }
      for (let k = 0; k < d; k++) {
        const vkp = v[k * d + p], vkq = v[k * d + q];
        v[k * d + p] = c * vkp - s * vkq; v[k * d + q] = s * vkp + c * vkq;
      }
    }
  }
  const vals = [];
  for (let i = 0; i < d; i++) vals.push(a[i * d + i]);
  return { vals, vecs: v };
}

function recompute() {
  const its = items();
  const per = its.map((it) => results.get(keyOf(it)) || null);
  let n = 0;
  per.forEach((r) => { if (r) n += r.slices.length / 2; });
  srcStats = S.sources.map(() => ({ slices: 0, secs: 0 }));
  const raw = new Float32Array(n * NDESC), item = new Int32Array(n), slice = new Int32Array(n);
  let k = 0;
  per.forEach((r, ii) => {
    if (!r) return;
    const m = r.slices.length / 2;
    raw.set(r.desc.subarray(0, m * NDESC), k * NDESC);
    for (let j = 0; j < m; j++) { item[k + j] = ii; slice[k + j] = j; srcStats[its[ii].src].secs += r.slices[2 * j + 1] / SR; }
    srcStats[its[ii].src].slices += m;
    k += m;
  });
  const used = new Array(NDESC).fill(false);
  for (const g of GROUPS) if (S.use.has(g.id)) g.dims.forEach((d) => { used[d] = true; });

  /* per-dim statistics (raw) + RobustScaling exactly as concat_robust_fit */
  const stats = [];
  const col = new Float32Array(n);
  for (let d = 0; d < NDESC; d++) {
    for (let i = 0; i < n; i++) col[i] = raw[i * NDESC + d];
    col.sort();
    const q1 = percentile(col, 0.25), q3 = percentile(col, 0.75);
    const lo = percentile(col, 0.01), hi = percentile(col, 0.99);
    const hist = new Array(24).fill(0);
    for (let i = 0; i < n; i++) { const t = hi > lo ? (col[i] - lo) / (hi - lo) : 0.5; hist[Math.max(0, Math.min(23, Math.floor(t * 24)))]++; }
    stats.push({ min: n ? col[0] : 0, max: n ? col[n - 1] : 0, median: percentile(col, 0.5), q1, q3, iqr: Math.max(q3 - q1, 1e-6), hist });
  }
  const qn = new Float32Array(n * NDESC);
  for (let i = 0; i < n; i++) for (let d = 0; d < NDESC; d++)
    qn[i * NDESC + d] = used[d] ? (raw[i * NDESC + d] - stats[d].median) / stats[d].iqr : 0;

  /* PCA over the query space (what Concat's Learned map uses: PC1, PC2) */
  let proj = new Float32Array(n * 3), eig = null;
  if (n >= 4) {
    const mean = new Float64Array(NDESC), C = new Float64Array(NDESC * NDESC);
    for (let i = 0; i < n; i++) for (let d = 0; d < NDESC; d++) mean[d] += qn[i * NDESC + d];
    for (let d = 0; d < NDESC; d++) mean[d] /= n;
    const row = new Float64Array(NDESC);
    for (let i = 0; i < n; i++) {
      for (let d = 0; d < NDESC; d++) row[d] = qn[i * NDESC + d] - mean[d];
      for (let p = 0; p < NDESC; p++) { const rp = row[p]; if (!rp) continue; for (let q = p; q < NDESC; q++) C[p * NDESC + q] += rp * row[q]; }
    }
    for (let p = 0; p < NDESC; p++) for (let q = p; q < NDESC; q++) { C[p * NDESC + q] /= n; C[q * NDESC + p] = C[p * NDESC + q]; }
    const { vals, vecs } = jacobiEigen(C, NDESC);
    const order = vals.map((v, i) => i).sort((a, b) => vals[b] - vals[a]);
    const total = vals.reduce((s, v) => s + Math.max(0, v), 0) || 1;
    eig = { share: order.slice(0, 3).map((i) => Math.max(0, vals[i]) / total), axes: order.slice(0, 3).map((i) => { const a = new Float64Array(NDESC); for (let d = 0; d < NDESC; d++) a[d] = vecs[d * NDESC + i]; return a; }), total };
    for (let i = 0; i < n; i++) for (let c = 0; c < 3; c++) {
      let s = 0;
      for (let d = 0; d < NDESC; d++) s += (qn[i * NDESC + d] - mean[d]) * eig.axes[c][d];
      proj[i * 3 + c] = s;
    }
    /* each descriptor's share of the map: loading^2 weighted by axis variance */
    eig.contrib = new Float64Array(NDESC);
    for (let c = 0; c < 3; c++) for (let d = 0; d < NDESC; d++) eig.contrib[d] += eig.share[c] * eig.axes[c][d] ** 2;
  }
  corpus = { n, raw, item, slice, its, proj, stats, eig, used };
  const colours = new Array(n);
  for (let i = 0; i < n; i++) colours[i] = colourOf(its[item[i]].src);
  map.setPoints(proj, colours);
  $('mapEmpty').style.display = n ? 'none' : '';
  renderSummary(); renderDesc(); renderSources(); drawWave();
}

function renderSummary() {
  const sm = $('summary'); sm.textContent = '';
  if (!corpus) return;
  const secs = srcStats.reduce((s, x) => s + x.secs, 0);
  const files = new Set(corpus.its.filter((it) => results.has(keyOf(it))).map((it) => it.path)).size;
  const total = new Set(corpus.its.map((it) => it.path)).size;
  const usedN = corpus.used.filter(Boolean).length;
  const mb = (secs * SR * 2) / 1048576;
  sm.append(...[
    el('span', {}, el('b', {}, corpus.n.toLocaleString()), ' slices'),
    el('span', {}, el('b', {}, files < total ? `${files}/${total}` : files), ' files'),
    el('span', {}, el('b', {}, fmtTime(secs)), ' of audio'),
    el('span', { title: 'Pool memory on the Move at 44.1 kHz mono int16. The default Pool Cap is 128 MB.' }, el('b', {}, mb.toFixed(1)), ' MB pool'),
    el('span', {}, el('b', {}, `${usedN}/${NDESC}`), ' descriptors'),
    corpus.eig ? el('span', { title: 'Variance captured by the first two axes = how faithful the Move\'s 2-D map is' }, el('b', {}, `${Math.round((corpus.eig.share[0] + corpus.eig.share[1]) * 100)}%`), ' on the Move map') : null].filter(Boolean));
}

function renderDesc() {
  const box = $('desc'); box.textContent = '';
  if (!corpus) return;
  const t = el('table');
  t.append(el('tr', {}, el('th', {}, 'Descriptor'), el('th', {}, 'Use'), el('th', {}, 'Min'), el('th', {}, 'Median'), el('th', {}, 'IQR'), el('th', {}, 'Max'),
    el('th', {}, 'Distribution'), el('th', { title: 'Share of the 3-D map this descriptor shapes' }, 'Map share')));
  const maxC = corpus.eig ? Math.max(...corpus.eig.contrib) || 1 : 1;
  for (const g of GROUPS) {
    const on = S.use.has(g.id);
    const cb = el('input', { type: 'checkbox' }); cb.checked = on;
    cb.addEventListener('change', () => {
      if (cb.checked) S.use.add(g.id); else S.use.delete(g.id);
      if (!S.use.size) { S.use.add(g.id); cb.checked = true; setStatus('A corpus needs at least one descriptor'); return; }
      markDirty(); recompute();
    });
    t.append(el('tr', { class: 'grp' + (on ? '' : ' off') }, el('td', {}, g.label), el('td', {}, cb), el('td', { colspan: 6 })));
    for (const d of g.dims) {
      const st = corpus.stats[d];
      const hc = el('canvas', { width: 96, height: 18 });
      drawHist(hc, st.hist, on);
      const c = corpus.eig ? corpus.eig.contrib[d] : 0;
      t.append(el('tr', { class: on ? '' : 'off' },
        el('td', { class: 'muted' }, ' ' + DIM_NAMES[d]),
        el('td'), el('td', {}, fmtNum(st.min)), el('td', {}, fmtNum(st.median)), el('td', {}, fmtNum(st.iqr)), el('td', {}, fmtNum(st.max)),
        el('td', {}, hc),
        el('td', {}, on && corpus.eig ? el('span', { class: 'share', style: `width:${Math.round((c / maxC) * 60)}px` }) : '', on && corpus.eig ? ` ${(c * 100).toFixed(1)}%` : '')));
    }
  }
  box.append(t);
}
function drawHist(cv, hist, on) {
  const g = cv.getContext('2d'), W = cv.width, H = cv.height, m = Math.max(...hist, 1);
  const cs = getComputedStyle(document.documentElement);
  g.fillStyle = cs.getPropertyValue(on ? '--accent' : '--ink2').trim();
  const w = W / hist.length;
  hist.forEach((v, i) => { const h = (v / m) * H; g.fillRect(i * w, H - h, w - 1, h); });
}

/* map interaction: hover names the slice, click plays it */
let map;
function sliceInfo(i) {
  const it = corpus.its[corpus.item[i]], r = results.get(keyOf(it)), j = corpus.slice[i];
  const st = r.slices[2 * j], len = r.slices[2 * j + 1];
  return { it, from: Math.round((it.start || 0) * SR) + st, len };
}
function onMapHover(i) {
  const tip = $('mapTip');
  if (i < 0 || !corpus) { tip.textContent = corpus && corpus.n ? 'Drag to rotate · wheel to zoom · click a dot to hear it' : ''; return; }
  const s = sliceInfo(i);
  tip.textContent = `${relPath(s.it.path)} · ${fmtTime(s.from / SR)} · ${Math.round((s.len / SR) * 1000)} ms`;
}
function onMapPick(i) {
  if (!corpus) return;
  const s = sliceInfo(i);
  playFrames(s.it.path, s.from, s.from + s.len).catch((e) => setStatus(String(e.message || e), 'bad'));
  const si = s.it.src, src = S.sources[si];
  if (src && S.sel !== src.id) { S.sel = src.id; renderSources(); renderEditor(); }
}

/* ── .concat files ─────────────────────────────────────────────────────────── */
function useList() {
  if (S.use.size === GROUPS.length) return null;            /* all: leave the key out */
  const out = [];
  for (const g of GROUPS.slice(0, 5)) if (S.use.has(g.id)) out.push(g.id);
  let k = 1;
  while (k <= 13) {
    if (!S.use.has(`mfcc${k}`)) { k++; continue; }
    let e = k;
    while (e < 13 && S.use.has(`mfcc${e + 1}`)) e++;
    out.push(e > k ? `mfcc${k}-${e}` : `mfcc${k}`);
    k = e + 1;
  }
  return out;
}
function toJSON() {
  const doc = { concat: 1, name: S.name, sources: [] };
  for (const s of S.sources) {
    const o = { path: relPath(s.path) };
    if (s.kind === 'file') { if (s.start > 0) o.start = +s.start.toFixed(3); if (s.end > s.start) o.end = +s.end.toFixed(3); }
    else {
      if (s.include && s.include.length) o.include = s.include;
      if (s.exclude && s.exclude.length) o.exclude = s.exclude;
      if (s.recurse === false) o.recurse = false;
    }
    doc.sources.push(o);
  }
  if (S.exclude.length) doc.exclude = S.exclude;
  const use = useList();
  if (use) doc.descriptors = { use };
  doc.slicing = { mode: S.slicing.mode, min_ms: S.slicing.min_ms, max_ms: S.slicing.max_ms, gate: S.slicing.gate };
  return JSON.stringify(doc, null, 2) + '\n';
}
function safeName(n) { return n.trim().replace(/[\/\\:*?"<>|]/g, '-').replace(/^\.+/, '').slice(0, 60) || 'Untitled'; }

async function save() {
  const name = safeName($('name').value);
  $('name').value = name; S.name = name;
  if (!S.sources.length) { setStatus('Add at least one source first', 'bad'); return; }
  const file = `${name}.concat`;
  try {
    setStatus('Saving…');
    const have = await listDir(LIB).catch(() => []);
    if (!have.some((r) => r.dir && r.name === 'Corpora')) await postForm('/files/mkdir', { path: LIB, name: 'Corpora' });
    if (S.loadedName !== file) {
      const exists = (await listDir(CORPORA).catch(() => [])).some((r) => r.name === file);
      if (exists && !confirm(`${file} already exists on the Move. Replace it?`)) { setStatus(''); return; }
    }
    const tmp = `${file}.tmp`;
    await postForm('/files/upload', { path: CORPORA }, { blob: new Blob([toJSON()], { type: 'application/json' }), name: tmp });
    await postForm('/files/rename', { path: `${CORPORA}/${tmp}`, name: file });   /* atomic replace */
    const check = await listDir(CORPORA);
    if (!check.some((r) => r.name === file)) throw new Error('the file did not appear on the Move');
    S.loadedName = file; S.dirty = false; clearDraft(); updateTitle();
    setStatus(`Saved Corpora/${file}. On the Move: Source → Corpora → ${file}`, 'ok');
  } catch (e) { setStatus(`Save failed: ${e.message || e}`, 'bad'); }
}

async function openDialog() {
  const dlg = $('dlgOpen'), list = $('openList');
  list.textContent = 'Loading…';
  dlg.showModal();
  let rows = [];
  try { rows = (await listDir(CORPORA)).filter((r) => !r.dir && /\.concat$/i.test(r.name)); } catch (e) { /* no folder yet */ }
  list.textContent = '';
  if (!rows.length) list.append(el('div', { class: 'muted' }, 'No corpus files yet. Build one and press Save.'));
  for (const r of rows)
    list.append(el('div', { class: 'lib-row', onclick: () => { dlg.close(); openCorpus(r.path); } },
      el('span', { class: 'ic' }, '◆'), el('span', { class: 'nm' }, r.name.replace(/\.concat$/i, ''))));
}
async function applyDoc(doc, fallbackName) {
  if ((doc.concat || 1) !== 1) throw new Error(`corpus file version ${doc.concat}`);
  S.name = doc.name || fallbackName;
  S.sources = [];
  S.exclude = Array.isArray(doc.exclude) ? doc.exclude : [];
  S.slicing = { ...DEFAULT_SLICING, ...(doc.slicing || {}) };
  S.use = new Set(GROUPS.map((g) => g.id));
  if (doc.descriptors && Array.isArray(doc.descriptors.use)) {
    S.use = new Set();
    for (const u of doc.descriptors.use) {
      const m = /^mfcc(\d+)(?:-(\d+))?$/i.exec(u);
      if (m) { for (let k = +m[1]; k <= +(m[2] || m[1]); k++) if (k >= 1 && k <= 13) S.use.add(`mfcc${k}`); }
      else if (u.toLowerCase() === 'mfcc') { for (let k = 1; k <= 13; k++) S.use.add(`mfcc${k}`); }
      else if (GROUPS.some((g) => g.id === u.toLowerCase())) S.use.add(u.toLowerCase());
    }
    if (!S.use.size) S.use = new Set(GROUPS.map((g) => g.id));
  }
  for (const src of doc.sources || []) {
    const p = absPath(typeof src === 'string' ? src : src.path || '');
    const isFile = AUDIO_RE.test(p);
    const s = { id: nextId++, kind: isFile ? 'file' : 'folder', path: p, start: +src.start || 0, end: +src.end || 0,
                recurse: src.recurse !== false, include: src.include || [], exclude: src.exclude || [], files: [], missing: false };
    if (isFile) { try { await ensureMeta(p); } catch (e) { s.missing = true; } }
    else { s.missing = !(await listDir(p).then(() => true, () => false)); if (!s.missing) s.files = await expandFolder(p, s.recurse); }
    S.sources.push(s);
  }
  S.sel = null;
  $('name').value = S.name; syncSlicingInputs();
  renderSources(); renderEditor(); recompute(); scheduleAnalysis();
  return S.sources.filter((s) => s.missing).length;
}
async function openCorpus(path) {
  if (S.dirty && !confirm('Discard unsaved changes?')) return;
  try {
    setStatus(`Opening ${baseName(path)}…`);
    const doc = JSON.parse(new TextDecoder().decode(await download(path)));
    const miss = await applyDoc(doc, baseName(path).replace(/\.concat$/i, ''));
    S.loadedName = baseName(path); S.dirty = false; clearDraft(); updateTitle();
    setStatus(miss ? `${miss} source${miss === 1 ? '' : 's'} missing on the Move` : `Opened ${baseName(path)}`, miss ? 'bad' : 'ok');
  } catch (e) { setStatus(`Can't open: ${e.message || e}`, 'bad'); }
}
async function restoreDraft() {
  let d = null;
  try { d = JSON.parse(localStorage.getItem(DRAFT_KEY) || 'null'); } catch (e) { /* ignore */ }
  if (!d || !d.doc || !(d.doc.sources || []).length) return;
  try {
    await applyDoc(d.doc, 'Untitled');
    S.loadedName = d.loadedName || null; S.dirty = true; updateTitle();
    setStatus('Restored your unsaved corpus from the last session');
  } catch (e) { clearDraft(); }
}
function newCorpus() {
  if (S.dirty && !confirm('Discard unsaved changes?')) return;
  Object.assign(S, { name: 'Untitled', sources: [], exclude: [], slicing: { ...DEFAULT_SLICING }, use: new Set(GROUPS.map((g) => g.id)), sel: null, dirty: false, loadedName: null });
  clearDraft();
  $('name').value = S.name; syncSlicingInputs(); updateTitle();
  renderSources(); renderEditor(); recompute(); setStatus('');
}

/* ── slicing controls ─────────────────────────────────────────────────────── */
function syncSlicingInputs() {
  $('slMode').value = S.slicing.mode; $('slMin').value = S.slicing.min_ms; $('slMax').value = S.slicing.max_ms; $('slGate').value = S.slicing.gate;
}
function bindSlicing() {
  const apply = () => {
    const mode = $('slMode').value === 'fixed' ? 'fixed' : 'onset';
    let mn = Math.round(+$('slMin').value || 20), mx = Math.round(+$('slMax').value || 2000), gate = +$('slGate').value;
    mn = Math.max(5, Math.min(2000, mn)); mx = Math.max(mn, Math.min(2000, mx));
    gate = isFinite(gate) ? Math.max(0, Math.min(1, gate)) : 0.02;
    S.slicing = { mode, min_ms: mn, max_ms: mx, gate };
    syncSlicingInputs(); markDirty(); scheduleAnalysis();
  };
  for (const id of ['slMode', 'slMin', 'slMax', 'slGate']) $(id).addEventListener('change', apply);
}

/* ── init ─────────────────────────────────────────────────────────────────── */
function init() {
  map = new Map3D($('map'), { onPick: onMapPick, onHover: onMapHover });
  $('btnUp').addEventListener('click', () => showLib(dirName(libPath).length < LIB.length ? LIB : dirName(libPath)));
  $('btnNew').addEventListener('click', newCorpus);
  $('btnOpen').addEventListener('click', openDialog);
  $('openCancel').addEventListener('click', () => $('dlgOpen').close());
  $('btnSave').addEventListener('click', save);
  $('name').addEventListener('change', () => { S.name = safeName($('name').value); $('name').value = S.name; markDirty(); });
  $('btnMoveView').addEventListener('click', () => map.moveView());
  $('btnOrbit').addEventListener('click', () => map.orbitView());
  const tab = (mapOn) => {
    $('tabMap').classList.toggle('on', mapOn); $('tabDesc').classList.toggle('on', !mapOn);
    $('mapWrap').hidden = !mapOn; $('desc').hidden = mapOn;
    $('btnMoveView').hidden = !mapOn; $('btnOrbit').hidden = !mapOn;
    if (mapOn) map.draw();
  };
  $('tabMap').addEventListener('click', () => tab(true));
  $('tabDesc').addEventListener('click', () => tab(false));
  $('btnTheme').addEventListener('click', () => {
    const r = document.documentElement, dark = r.dataset.theme ? r.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
    r.dataset.theme = dark ? 'light' : 'dark';
    try { localStorage.setItem('concat-builder-theme', r.dataset.theme); } catch (e) { /* ignore */ }
    map.draw(); drawWave(); renderDesc();
  });
  try { const t = localStorage.getItem('concat-builder-theme'); if (t) document.documentElement.dataset.theme = t; } catch (e) { /* ignore */ }
  window.addEventListener('beforeunload', (e) => { if (S.dirty) { e.preventDefault(); e.returnValue = ''; } });
  window.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); save(); }
    if (e.key === ' ' && e.target === document.body) { e.preventDefault(); stopAudio(); }
  });
  bindDrop(); bindSlicing(); syncSlicingInputs(); updateTitle();
  showLib(LIB + '/Samples').catch(() => showLib(LIB));
  recompute();
  onMapHover(-1);
  restoreDraft();
}
init();
