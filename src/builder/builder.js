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
  /* A path that does not exist gets the root listing back: refuse rows from elsewhere. */
  const here = out.filter((r) => dirName(r.path) === path);
  if (out.length && !here.length) throw new Error(`${baseName(path)}: not found on the Move`);
  out.length = 0; out.push(...here);
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
  selFile: null,        /* a file inside the selected folder source, or null */
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
  /* Same order as the Move (ingest.c sorts entries by name, byte order, subfolders in
   * place): corpus order decides k-means seeds, so pad clusters match the device. */
  rows = rows.slice().sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  for (const r of rows) {
    if (r.dir) { if (recurse) await expandFolder(r.path, true, depth + 1, out); }
    else if (AUDIO_RE.test(r.name)) out.push(r.path);
  }
  return out;
}

/* Add library entries ({path, dir}) as sources, in order. Folders arrive collapsed. */
async function addSources(list) {
  const fresh = list.filter((x, i) => !S.sources.some((s) => s.path === x.path) && list.findIndex((y) => y.path === x.path) === i);
  if (!fresh.length) {
    setStatus(list.length === 1 ? `${baseName(list[0].path)} is already in the corpus` : 'Those are already in the corpus');
    return;
  }
  const added = fresh.map((x) => ({ id: nextId++, kind: x.dir ? 'folder' : 'file', path: x.path, start: 0, end: 0, recurse: true,
                                    include: [], exclude: [], files: [], missing: false, open: false }));
  S.sources.push(...added);
  S.sel = added[added.length - 1].id; S.selFile = null;
  markDirty(); renderSources(); renderEditor();
  for (const s of added.filter((x) => x.kind === 'folder')) {
    setStatus(`Listing ${baseName(s.path)}…`);
    s.files = await expandFolder(s.path, true);
  }
  setStatus(added.length > 1 ? `Added ${added.length} sources` : '');
  renderSources(); renderEditor(); scheduleAnalysis();
}
const addSource = (path, dir) => addSources([{ path, dir }]);

/* ── library panel ─────────────────────────────────────────────────────────── */
/* Click selects a sample (Ctrl/Cmd toggles, Shift extends), Ctrl+A selects every sample
 * here; dragging any selected row drags them all. Clicking a folder opens it. */
let libPath = LIB, libRows = [], libSel = new Set(), libAnchor = -1;
async function showLib(path) {
  libPath = path;
  libSel = new Set(); libAnchor = -1;
  const crumbs = $('crumbs'); crumbs.textContent = '';
  const rel = path === LIB ? [] : path.slice(LIB.length + 1).split('/');
  const parts = [['UserLibrary', LIB]];
  rel.forEach((p, i) => parts.push([p, LIB + '/' + rel.slice(0, i + 1).join('/')]));
  parts.forEach(([n, p], i) => { if (i) crumbs.append(' / '); crumbs.append(el('a', { onclick: () => showLib(p) }, n)); });
  $('btnUp').disabled = path === LIB;
  const lib = $('lib');
  lib.textContent = 'Loading…';
  paintLibSel();
  let rows;
  try { rows = await listDir(path); } catch (e) {
    if (path !== LIB && path !== libPath) return;
    if (path !== LIB) { showLib(LIB); return; }               /* a folder that went away: back to the top */
    lib.textContent = String(e.message); return;
  }
  if (path !== libPath) return;                        /* a newer navigation won */
  try { localStorage.setItem('concat-builder-lib', path); } catch (e) { /* ignore */ }
  lib.textContent = '';
  libRows = rows.filter((r) => r.dir || AUDIO_RE.test(r.name));
  if (!libRows.length) lib.append(el('div', { class: 'lib-row muted' }, 'No folders or audio here'));
  libRows.forEach((r, i) => {
    const row = el('div', { class: 'lib-row' + (r.dir ? ' dir' : ''), draggable: 'true', title: r.path, 'data-i': i },
      el('span', { class: 'ic' }, r.dir ? '▸' : '♪'),
      el('span', { class: 'nm' }, r.name),
      r.dir ? null : playBtn(r.path, 0, 0),
      el('button', { title: 'Add to corpus', onclick: (e) => { e.stopPropagation(); addSource(r.path, r.dir); } }, '+'));
    row.addEventListener('click', (e) => libClick(e, i));
    row.addEventListener('dblclick', () => { if (!r.dir) addSource(r.path, false); });
    row.addEventListener('dragstart', (e) => {
      if (!libSel.has(r.path)) { libSel = new Set([r.path]); libAnchor = i; paintLibSel(); }
      const list = libRows.filter((x) => libSel.has(x.path)).map((x) => ({ path: x.path, dir: x.dir }));
      e.dataTransfer.setData('application/x-concat', JSON.stringify({ items: list }));
      e.dataTransfer.setData('text/plain', list.map((x) => x.path).join('\n'));
      e.dataTransfer.effectAllowed = 'copy';
    });
    lib.append(row);
  });
}
function libClick(e, i) {
  const r = libRows[i];
  if (e.ctrlKey || e.metaKey) { if (libSel.has(r.path)) libSel.delete(r.path); else libSel.add(r.path); libAnchor = i; }
  else if (e.shiftKey && libAnchor >= 0) {
    const [a, b] = libAnchor < i ? [libAnchor, i] : [i, libAnchor];
    libSel = new Set(libRows.slice(a, b + 1).map((x) => x.path));
  }
  else if (r.dir) { showLib(r.path); return; }
  else { libSel = new Set([r.path]); libAnchor = i; }
  paintLibSel();
}
function libSelected() { return libRows.filter((x) => libSel.has(x.path)).map((x) => ({ path: x.path, dir: x.dir })); }
function paintLibSel() {
  for (const row of $('lib').querySelectorAll('.lib-row[data-i]')) row.classList.toggle('sel', libSel.has(libRows[+row.dataset.i].path));
  const n = libSel.size, b = $('btnAddSel');
  b.hidden = n < 2;
  b.textContent = `Add ${n}`;
}
function selectAllLib() {
  const files = libRows.filter((r) => !r.dir);
  libSel = new Set((files.length ? files : libRows).map((r) => r.path));
  libAnchor = libSel.size ? 0 : -1;
  paintLibSel();
  if (libSel.size) setStatus(`${libSel.size} selected: drag them onto the corpus, or press Add ${libSel.size}`);
}
/* ↑/↓ in the library: move through the samples and play each one. */
function libMove(d) {
  if (!libRows.length) return;
  let i = libAnchor < 0 ? (d > 0 ? 0 : libRows.length - 1) : Math.max(0, Math.min(libRows.length - 1, libAnchor + d));
  libAnchor = i; libSel = new Set([libRows[i].path]); paintLibSel();
  const row = $('lib').querySelector(`.lib-row[data-i="${i}"]`);
  if (row) row.scrollIntoView({ block: 'nearest' });
  if (!libRows[i].dir) playFile(libRows[i].path);
}

/* ── corpus list ──────────────────────────────────────────────────────────── */
function srcSummary(s, si) {
  if (s.missing) return 'missing on the Move';
  const st = srcStats[si];
  if (s.kind === 'folder') {
    const n = items().filter((it) => it.src === si).length, all = (s.files || []).length;
    return `folder · ${n === all ? `${n} file${n === 1 ? '' : 's'}` : `${n} of ${all} files`}${st ? ` · ${st.slices} slices` : ''}`;
  }
  const m = peaksMem.get(peakKey(s.path));
  const dur = m ? m.frames / SR : NaN;
  const range = s.end > s.start ? `${fmtTime(s.start)} – ${fmtTime(s.end)}` : (s.start > 0 ? `from ${fmtTime(s.start)}` : 'whole file');
  const dup = S.sources.find((o) => o.kind === 'folder' && !o.missing && o !== s && (o.files || []).includes(s.path) &&
                                    items().some((it) => it.path === s.path && S.sources[it.src] === o));
  return `${range}${isFinite(dur) ? ` of ${fmtTime(dur)}` : ''}${st ? ` · ${st.slices} slices` : ''}` +
         (dup ? ` · also in ${baseName(dup.path)} (counted twice: ✕ it there)` : '');
}
let srcStats = [];
/* A file name as an exact fnmatch pattern (brackets escape the wildcards). */
const globEscape = (name) => name.replace(/[[\]*?]/g, '[$&]');
function usedIn(s) { const si = S.sources.indexOf(s); return new Set(items().filter((it) => it.src === si).map((it) => it.path)); }

function renderSources() {
  const box = $('sources');
  box.querySelectorAll('.src').forEach((n) => n.remove());
  $('drop').style.display = S.sources.length ? 'none' : '';
  $('srcCount').textContent = S.sources.length ? `${S.sources.length} source${S.sources.length === 1 ? '' : 's'}` : '';
  S.sources.forEach((s, si) => {
    const isFolder = s.kind === 'folder' && !s.missing;
    const tog = isFolder ? el('button', { class: 'tog', title: s.open ? 'Hide the files' : 'Show the files',
      onclick: (e) => { e.stopPropagation(); s.open = !s.open; renderSources(); } }, s.open ? '▾' : '▸') : null;
    const row = el('div', { class: 'src' + (S.sel === s.id && !S.selFile ? ' sel' : '') + (s.missing ? ' missing' : ''), title: s.path },
      el('span', { class: 'chip', style: `background:${colourOf(si)}` }),
      el('div', { class: 't' }, el('div', {}, tog, baseName(s.path)), el('div', { class: 'sub' }, srcSummary(s, si))),
      el('div', { class: 'acts' },
        s.kind === 'file' && !s.missing ? playBtn(s.path, s.start, s.end) : null,
        el('button', { class: 'icon', title: 'Remove from the corpus (the file stays on the Move)', onclick: (e) => { e.stopPropagation(); removeSource(s.id); } }, '✕')));
    row.addEventListener('click', () => { S.sel = s.id; S.selFile = null; renderSources(); renderEditor(); });
    row.addEventListener('dblclick', () => { if (isFolder) { s.open = !s.open; renderSources(); } });
    box.append(row);
    if (!isFolder || !s.open) return;
    const used = usedIn(s), base = relPath(s.path);
    for (const f of s.files || []) {
      const on = used.has(f), bn = baseName(f), mine = (s.exclude || []).includes(globEscape(bn));
      const child = el('div', { class: 'src child' + (S.sel === s.id && S.selFile === f ? ' sel' : '') + (on ? '' : ' off'), title: f },
        el('span'),
        el('div', { class: 't' }, el('div', {}, relPath(f).slice(base.length + 1) || bn)),
        el('div', { class: 'acts' },
          playBtn(f, 0, 0),
          on ? el('button', { class: 'icon', title: 'Leave this file out of the corpus', onclick: (e) => { e.stopPropagation(); skipFile(s, f); } }, '✕')
             : el('button', { class: 'icon', title: mine ? 'Put this file back' : 'Left out by a pattern: edit Only / Skip below', disabled: !mine,
                              onclick: (e) => { e.stopPropagation(); unskipFile(s, f); } }, '+')));
      child.addEventListener('click', () => { S.sel = s.id; S.selFile = f; renderSources(); renderEditor(); });
      box.append(child);
    }
    if (!(s.files || []).length) box.append(el('div', { class: 'src child off' }, el('span'), el('div', { class: 't muted' }, 'No audio files'), el('span')));
  });
  const sel = box.querySelector('.src.sel');
  if (sel) sel.scrollIntoView({ block: 'nearest' });
}
function skipFile(s, f) {
  s.exclude = [...(s.exclude || []), globEscape(baseName(f))];
  markDirty(); renderSources(); renderEditor(); scheduleAnalysis();
}
function unskipFile(s, f) {
  const g = globEscape(baseName(f));
  s.exclude = (s.exclude || []).filter((x) => x !== g);
  if (!usedIn(s).has(f)) setStatus(`${baseName(f)} is still left out by a pattern (Only / Skip)`);
  markDirty(); renderSources(); renderEditor(); scheduleAnalysis();
}
function removeSource(id) {
  S.sources = S.sources.filter((s) => s.id !== id);
  if (S.sel === id) { S.sel = null; S.selFile = null; }
  markDirty(); renderSources(); renderEditor(); scheduleAnalysis();
}
function bindDrop() {
  /* The whole corpus column takes drops, not just the list. */
  const col = $('corpusCol'), box = $('sources');
  col.addEventListener('dragover', (e) => { if (e.dataTransfer.types.includes('application/x-concat')) { e.preventDefault(); box.classList.add('over'); } });
  col.addEventListener('dragleave', (e) => { if (!col.contains(e.relatedTarget)) box.classList.remove('over'); });
  col.addEventListener('drop', (e) => {
    box.classList.remove('over');
    const d = e.dataTransfer.getData('application/x-concat');
    if (!d) return;
    e.preventDefault();
    const p = JSON.parse(d);
    addSources(p.items || [{ path: p.path, dir: p.dir }]);
  });
}

/* ↑/↓ in the corpus list: step through sources and the files of open folders,
 * playing each sample; → opens a folder, ← closes it (or goes back to it). */
function navRows() {
  const out = [];
  for (const s of S.sources) {
    out.push({ s, f: null });
    if (s.kind === 'folder' && s.open && !s.missing) for (const f of s.files || []) out.push({ s, f });
  }
  return out;
}
function navMove(d) {
  const rows = navRows();
  if (!rows.length) return;
  const at = rows.findIndex((r) => r.s.id === S.sel && r.f === (S.selFile || null));
  const r = rows[at < 0 ? (d > 0 ? 0 : rows.length - 1) : Math.max(0, Math.min(rows.length - 1, at + d))];
  S.sel = r.s.id; S.selFile = r.f;
  renderSources(); renderEditor();
  if (r.f) playFile(r.f);
  else if (r.s.kind === 'file' && !r.s.missing) playFile(r.s.path, r.s.start, r.s.end);
}
function navOpen(open) {
  const s = selected();
  if (!s || s.kind !== 'folder') return;
  if (!open && S.selFile) S.selFile = null;
  else s.open = open;
  renderSources(); renderEditor();
}

/* ── editor (selected source, or a file inside a folder source) ───────────── */
let wave = null;   /* {canvas, s, frames, peaks, inputs, preview} */
function selected() { return S.sources.find((s) => s.id === S.sel) || null; }
/* The selected source (or file in a folder) lights up on the map. */
function updateFocus() {
  if (!corpus || !map) return;
  const s = selected();
  if (!s) { map.setFocus(null); return; }
  const si = S.sources.indexOf(s), set = new Set();
  for (let i = 0; i < corpus.n; i++) {
    const it = corpus.its[corpus.item[i]];
    if (it.src === si && (!S.selFile || it.path === S.selFile)) set.add(i);
  }
  map.setFocus(set);
}
function renderEditor() {
  updateFocus();
  const ed = $('editor');
  const s = selected();
  ed.textContent = '';
  wave = null;
  if (!s) { ed.classList.remove('on'); return; }
  ed.classList.add('on');
  if (S.selFile && s.kind === 'folder') { renderMemberEditor(ed, s, S.selFile); return; }
  ed.append(el('div', { class: 'mono muted', title: s.path }, relPath(s.path)));
  if (s.missing) { ed.append(el('div', { class: 'row' }, 'This source is not on the Move any more. Remove it, or put the file back.')); return; }
  if (s.kind === 'file') renderFileEditor(ed, s); else renderFolderEditor(ed, s);
}

function attachWave(ed, src, inputs) {
  const cv = el('canvas', { id: 'wave' });
  ed.append(el('div', { class: 'row' }, cv));
  wave = { canvas: cv, s: src, frames: 0, peaks: null, inputs, preview: !!src.preview };
  loadPeaks(src.path).then((pk) => { if (wave && wave.s === src && pk) { wave.frames = pk.frames; wave.peaks = pk.peaks; drawWave(); renderSources(); } });
  bindWave(cv, src);
  new ResizeObserver(() => drawWave()).observe(cv);
}

function renderFileEditor(ed, s) {
  const startIn = el('input', { type: 'number', min: 0, step: 0.01, value: (s.start || 0).toFixed(2) });
  const endIn = el('input', { type: 'number', min: 0, step: 0.01, value: s.end > s.start ? s.end.toFixed(2) : '', placeholder: 'end' });
  attachWave(ed, s, [startIn, endIn]);
  const apply = () => {
    const a = Math.max(0, +startIn.value || 0), b = +endIn.value || 0;
    s.start = a; s.end = b > a ? b : 0;
    markDirty(); drawWave(); renderSources(); scheduleAnalysis();
  };
  startIn.addEventListener('change', apply); endIn.addEventListener('change', apply);
  ed.append(el('div', { class: 'row' },
    playBtn(s.path, s.start, s.end, { cls: '', label: 'Play range' }),
    el('button', { onclick: stopAudio }, '■ Stop'),
    el('label', { class: 'muted' }, 'Start ', startIn, ' s'),
    el('label', { class: 'muted' }, 'End ', endIn, ' s'),
    el('button', { onclick: () => { s.start = 0; s.end = 0; startIn.value = '0.00'; endIn.value = ''; markDirty(); drawWave(); renderSources(); scheduleAnalysis(); } }, 'Whole file'),
    el('span', { class: 'muted' }, 'Drag on the waveform to choose the part that enters the corpus. Click a slice to hear it.')));
}

/* A file inside a folder source: hear it, skip it, or pull it out to trim it. */
function renderMemberEditor(ed, s, f) {
  const on = usedIn(s).has(f);
  ed.append(el('div', { class: 'mono muted', title: f }, relPath(f)));
  attachWave(ed, { path: f, start: 0, end: 0, preview: true }, null);
  ed.append(el('div', { class: 'row' },
    playBtn(f, 0, 0, { cls: '', label: 'Play' }),
    el('button', { onclick: stopAudio }, '■ Stop'),
    on ? el('button', { onclick: () => skipFile(s, f) }, '✕ Leave out')
       : el('button', { onclick: () => unskipFile(s, f) }, '+ Put back'),
    el('button', { title: 'Adds this sample as its own source (so you can trim it) and leaves it out of the folder',
                   onclick: () => trimOnItsOwn(s, f) }, 'Trim on its own'),
    el('span', { class: 'muted' }, `In ${baseName(s.path)}. Click a slice to hear it; ↑/↓ steps through the files.`)));
}
function trimOnItsOwn(s, f) {
  if (usedIn(s).has(f)) s.exclude = [...(s.exclude || []), globEscape(baseName(f))];
  let own = S.sources.find((o) => o.kind === 'file' && o.path === f);
  if (!own) {
    own = { id: nextId++, kind: 'file', path: f, start: 0, end: 0, recurse: true, include: [], exclude: [], files: [], missing: false, open: false };
    S.sources.splice(S.sources.indexOf(s) + 1, 0, own);
  }
  S.sel = own.id; S.selFile = null;
  markDirty(); renderSources(); renderEditor(); scheduleAnalysis();
}

/* The slice of `s`'s analysed range under time t (s), as absolute frames. */
function sliceAt(s, t) {
  const r = results.get(keyOf({ path: s.path, start: s.start || 0, end: s.end || 0 }));
  if (!r) return null;
  const a = s.start || 0, rel = Math.round((t - a) * SR);
  for (let i = 0; i < r.slices.length; i += 2)
    if (rel >= r.slices[i] && rel < r.slices[i] + r.slices[i + 1]) return { from: Math.round(a * SR) + r.slices[i], len: r.slices[i + 1] };
  return null;
}
function playSliceAt(s, t) {
  const h = sliceAt(s, t);
  if (h) playFrames(s.path, h.from, h.from + h.len).catch(playErr);
  return !!h;
}

function bindWave(cv, s) {
  const toSec = (e) => {
    const r = cv.getBoundingClientRect();
    return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * (wave.frames / SR);
  };
  let drag = null;
  /* hover shades the slice under the pointer */
  cv.addEventListener('pointermove', (e) => {
    if (drag || !wave || !wave.frames) return;
    const h = sliceAt(s, toSec(e)), k = h ? h.from : -1;
    if (k !== wave.hoverKey) { wave.hoverKey = k; wave.hover = h; drawWave(); }
  });
  cv.addEventListener('pointerleave', () => { if (wave && wave.hover) { wave.hover = null; wave.hoverKey = -1; drawWave(); } });
  if (s.preview) {                                     /* folder member: click plays the slice (or from there) */
    cv.addEventListener('pointerdown', (e) => {
      if (!wave || !wave.frames) return;
      const t = toSec(e);
      if (!playSliceAt(s, t)) playFile(s.path, t, 0);
    });
    return;
  }
  cv.addEventListener('pointerdown', (e) => {
    if (!wave || !wave.frames) return;
    const t = toSec(e), dur = wave.frames / SR;
    const a = s.start || 0, b = s.end > s.start ? s.end : dur;
    const px = 6 * dur / cv.getBoundingClientRect().width;      /* grab handles within 6 px */
    const prev = { start: s.start, end: s.end };
    drag = Math.abs(t - a) < px ? { kind: 'a', prev } : Math.abs(t - b) < px ? { kind: 'b', prev } : { kind: 'new', anchor: t, prev };
    if (drag.kind === 'new') { s.start = t; s.end = t; }
    wave.hover = null;
    cv.setPointerCapture(e.pointerId);
  });
  cv.addEventListener('pointermove', (e) => {
    if (!drag || !wave) return;
    const t = toSec(e), dur = wave.frames / SR;
    if (drag.kind === 'a') s.start = Math.min(t, (s.end > s.start ? s.end : dur) - 0.01);
    else if (drag.kind === 'b') s.end = Math.max(t, (s.start || 0) + 0.01);
    else { s.start = Math.min(drag.anchor, t); s.end = Math.max(drag.anchor, t); }
    drawWave(); syncInputs();
  });
  cv.addEventListener('pointerup', (e) => {
    if (!drag || !wave) return;
    const d = drag; drag = null;
    const dur = wave.frames / SR;
    if (d.kind === 'new' && s.end - s.start < 0.02) {           /* a click: keep the range, hear the slice */
      s.start = d.prev.start; s.end = d.prev.end;
      syncInputs(); drawWave();
      playSliceAt(s, toSec(e));
      return;
    }
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
  if (!wave.preview) { g.fillStyle = cSel; g.globalAlpha = 0.12; g.fillRect(xa, 0, xb - xa, H); g.globalAlpha = 1; }
  const n = wave.peaks.length / 2, mid = H / 2;
  for (let x = 0; x < W; x++) {
    const i0 = Math.floor((x / W) * n), i1 = Math.max(i0 + 1, Math.floor(((x + 1) / W) * n));
    let lo = 0, hi = 0;
    for (let i = i0; i < i1 && i < n; i++) { lo = Math.min(lo, wave.peaks[2 * i]); hi = Math.max(hi, wave.peaks[2 * i + 1]); }
    g.fillStyle = !wave.preview && x >= xa && x <= xb ? cSel : cWave;
    g.fillRect(x, mid - hi * mid * 0.95, 1, Math.max(1, (hi - lo) * mid * 0.95));
  }
  if (wave.hover) {                                     /* the slice under the pointer */
    const x0 = (wave.hover.from / SR / dur) * W, x1 = ((wave.hover.from + wave.hover.len) / SR / dur) * W;
    g.fillStyle = cInk; g.globalAlpha = 0.18; g.fillRect(x0, 0, Math.max(1, x1 - x0), H); g.globalAlpha = 1;
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
  if (!wave.preview) for (const x of [xa, xb]) g.fillRect(Math.round(x) - 1, 0, 2, H);
  if (playing && playing.path === s.path && actx) {
    const t = playing.from + (actx.currentTime - playing.t0);
    if (t <= playing.to) { g.fillStyle = cInk; g.fillRect((t / dur) * W, 0, 1, H); }
  } else if (paused && paused.path === s.path) {
    g.fillStyle = cInk; g.fillRect((paused.pos / dur) * W, 0, 1, H);
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
    markDirty(); renderSources(); scheduleAnalysis();
  };
  inc.addEventListener('change', apply); exc.addEventListener('change', apply); rec.addEventListener('change', apply);
  ed.append(el('div', { class: 'row' },
    el('label', { class: 'muted' }, 'Only ', inc), el('label', { class: 'muted' }, 'Skip ', exc),
    el('label', { class: 'muted' }, rec, ' subfolders'),
    el('button', { title: 'Re-read the folder from the Move', onclick: async () => { s.files = await expandFolder(s.path, s.recurse !== false); renderSources(); scheduleAnalysis(); } }, 'Refresh')),
    el('div', { class: 'row muted' }, s.open ? 'Click a file above to see it; ↑/↓ steps through the files and plays each one.'
                                             : el('span', {}, el('button', { onclick: () => { s.open = true; renderSources(); renderEditor(); } }, '▸ Show the files'), ' to hear them one by one and leave some out.')));
}

/* ── audio ─────────────────────────────────────────────────────────────────── */
/* One transport for the whole page. A play button is keyed by file + range: pressing it
 * while that sound plays pauses it, pressing it again resumes from there. Only Stop
 * (button, Esc) or reaching the end goes back to the start. */
let actx = null, rafId = 0;
let playing = null;   /* {node, key, path, from, to, t0} seconds; the sound now playing */
let paused = null;    /* {key, path, pos, to} seconds; where a paused sound resumes */
const audioMem = new Map();   /* path -> Float32Array (LRU, ~40 M samples) */
function ctx() { if (!actx) actx = new AudioContext(); if (actx.state === 'suspended') actx.resume(); return actx; }
const playKey = (path, start, end) => `${path}|${+(start || 0).toFixed(3)}|${end > start ? +end.toFixed(3) : 0}`;
function halt() {   /* silence the node without touching transport state */
  if (playing) { const n = playing.node; playing = null; try { n.onended = null; n.stop(); } catch (e) { /* ignore */ } }
}
function stopAudio() { halt(); paused = null; refreshPlayButtons(); drawWave(); }
function pauseAudio() {
  if (!playing) return;
  const pos = playing.from + (actx.currentTime - playing.t0);
  paused = playing.key ? { key: playing.key, path: playing.path, pos: Math.min(pos, playing.to), to: playing.to } : null;
  halt(); refreshPlayButtons(); drawWave();
}
const AUDIO_MEM_CAP = 64e6;   /* decoded samples kept for playback (~256 MB) */
async function decodedAudio(path, quiet) {
  if (audioMem.has(path)) { const a = audioMem.get(path); audioMem.delete(path); audioMem.set(path, a); return a; }
  if (!quiet) setStatus(`Loading ${baseName(path)}…`);
  const bytes = await download(path);
  const r = await work({ op: 'decode', bytes: bytes.buffer, name: path, audio: true }, [bytes.buffer]);
  if (!quiet) setStatus('');
  if (!r.ok || !r.audio) throw new Error(r.error || "can't decode");
  audioMem.set(path, r.audio);
  let total = 0;
  for (const a of audioMem.values()) total += a.length;
  for (const [k, a] of audioMem) { if (total <= AUDIO_MEM_CAP || audioMem.size <= 1) break; audioMem.delete(k); total -= a.length; }
  return r.audio;
}
async function playFrames(path, a0, a1, key) {
  const c = ctx();
  const audio = await decodedAudio(path);
  const from = Math.max(0, Math.min(audio.length, a0)), to = Math.max(from + 1, Math.min(audio.length, a1 || audio.length));
  halt();
  paused = null;
  const buf = c.createBuffer(1, to - from, SR);
  const ch = buf.getChannelData(0);
  ch.set(audio.subarray(from, to));
  const fade = Math.min(64, ch.length >> 2);                    /* no clicks at the cut */
  for (let i = 0; i < fade; i++) { const g = i / fade; ch[i] *= g; ch[ch.length - 1 - i] *= g; }
  const node = c.createBufferSource();
  node.buffer = buf; node.connect(c.destination); node.start();
  playing = { node, key: key || null, path, from: from / SR, to: to / SR, t0: c.currentTime };
  node.onended = () => { if (playing && playing.node === node) { playing = null; refreshPlayButtons(); drawWave(); } };   /* end: back to the start */
  refreshPlayButtons();
  cancelAnimationFrame(rafId);
  const tick = () => { if (playing && playing.node === node) { drawWave(); rafId = requestAnimationFrame(tick); } };
  tick();
}
const playErr = (e) => setStatus(String(e.message || e), 'bad');
/* Play from the start (arrow keys, waveform clicks): no pause/resume. */
function playFile(path, start, end) {
  playFrames(path, Math.round((start || 0) * SR), end > start ? Math.round(end * SR) : 0, playKey(path, start, end)).catch(playErr);
}
/* What a play button does. */
function togglePlay(path, start, end) {
  const key = playKey(path, start, end);
  if (playing && playing.key === key) { pauseAudio(); return; }
  if (paused && paused.key === key) {
    playFrames(path, Math.round(paused.pos * SR), Math.round(paused.to * SR), key).catch(playErr);
    return;
  }
  playFile(path, start, end);
}
/* A play button that follows the transport: ▶ / ⏸ (labelled variants for the editor). */
function playBtn(path, start, end, opts = {}) {
  const b = el('button', { class: opts.cls || 'icon', 'data-play': playKey(path, start, end), 'data-label': opts.label || '' ,
    onclick: (e) => { e.stopPropagation(); togglePlay(path, start, end); } });
  paintPlayBtn(b);
  return b;
}
function paintPlayBtn(b) {
  const on = playing && playing.key === b.dataset.play, lab = b.dataset.label;
  b.textContent = on ? (lab ? '⏸︎ Pause' : '⏸︎') : (lab ? `▶︎ ${lab}` : '▶︎');   /* FE0E: text glyphs, not emoji */
  b.title = on ? 'Pause' : (paused && paused.key === b.dataset.play ? 'Resume' : 'Listen');
  b.classList.toggle('on', !!on);
}
function refreshPlayButtons() { for (const b of document.querySelectorAll('button[data-play]')) paintPlayBtn(b); }

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
          const label = `Analysing ${done + 1}/${its.length} · ${baseName(it.path)}`;
          setStatus(label);
          const bytes = await download(it.path, (p) => { if (p < 1 && token === runToken) setStatus(`${label} · loading ${Math.round(p * 100)}%`); });
          if (token === runToken) setStatus(label);
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
  const clusters = kmeans(qn, n);
  corpus = { n, raw, qn, item, slice, its, proj, stats, eig, used, clusters };
  updateMap();
  updateFocus();
  $('mapEmpty').style.display = n ? 'none' : '';
  renderSummary(); renderDesc(); renderSources(); drawWave();
}

/* Concat's Pool Cap: 128 MB by default, 512 MB max (free RAM can lower it on the day). */
function poolMeter(mb) {
  const CAP = 128, MAX = 512;
  const cls = mb <= CAP ? '' : mb <= MAX ? 'warn' : 'over';
  const pct = Math.min(100, (mb / (mb <= CAP ? CAP : MAX)) * 100);
  const need = Math.ceil(mb + 1);
  const title = mb <= CAP ? "Fits Concat's default Pool Cap (128 MB): every slice stays playable."
    : mb <= MAX ? `Above the default 128 MB. Set Pool Cap (Corpus page) to at least ${need} MB to keep every slice; otherwise Concat keeps an even subset.`
    : "Above Concat's 512 MB maximum: Concat keeps an even subset of the slices. Trim sources or leave some out.";
  const txt = mb <= CAP ? ' of 128 MB pool' : mb <= MAX ? ` pool: Pool Cap ≥ ${need} MB` : ' pool: over 512 MB';
  return el('span', { class: 'pool ' + cls, title }, el('b', {}, `${mb.toFixed(1)} MB`), el('span', { class: 'meter' }, el('i', { style: `width:${pct}%` })), txt);
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
    poolMeter(mb),
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
  if (i < 0 || !corpus) {
    tip.textContent = !corpus || !corpus.n ? '' : map.mode === 'play' ? 'Press and drag on the map to play it · wheel to zoom'
      : `${map.flat ? '' : 'Drag to rotate · '}Shift+drag to play · wheel to zoom · click a dot to hear it`;
    return;
  }
  const s = sliceInfo(i);
  tip.textContent = `${relPath(s.it.path)} · ${fmtTime(s.from / SR)} · ${Math.round((s.len / SR) * 1000)} ms`;
}
function onMapPick(i) {
  if (!corpus) return;
  const s = sliceInfo(i);
  playFrames(s.it.path, s.from, s.from + s.len).catch((e) => setStatus(String(e.message || e), 'bad'));
  const src = S.sources[s.it.src];
  if (!src) return;
  const f = src.kind === 'folder' ? s.it.path : null;
  if (S.sel !== src.id || S.selFile !== f) { S.sel = src.id; S.selFile = f; if (f) src.open = true; renderSources(); renderEditor(); }
}

/* ── map views, colours, and playing the map ─────────────────────────────── */
/* Page preferences (not part of the corpus): kept per browser. */
const PREFS_KEY = 'concat-builder-prefs';
const P = { view: 'learned', rawX: 0, rawY: 1, colorBy: 'source', density: 20, grain: 120, spread: 4 };
function loadPrefs() { try { Object.assign(P, JSON.parse(localStorage.getItem(PREFS_KEY) || '{}')); } catch (e) { /* ignore */ } }
function savePrefs() { try { localStorage.setItem(PREFS_KEY, JSON.stringify(P)); } catch (e) { /* ignore */ } }

/* Concat's X/Y Descriptor menu and the dims it maps to (desc_index in concat.c). */
const RAW_AXES = [['Bright', 13], ['Pitch', 34], ['Loud', 32], ['Noisy', 14], ['Rolloff', 15], ['MFCC1', 1], ['MFCC2', 2], ['MFCC3', 3]];
const COLOR_BY = [['source', 'Source'], ['cluster', 'Pad clusters'], ['32', 'Loudness'], ['13', 'Brightness'], ['14', 'Noisiness'], ['34', 'Pitch'], ['15', 'Zero crossings']];
const CLUSTER_COLS = ['#e6194b', '#3cb44b', '#d4a106', '#4363d8', '#f58231', '#911eb4', '#2bb3b3', '#d633c8',
                      '#86b300', '#e08a8a', '#008080', '#9b7fd4', '#9a6324', '#7a7a00', '#800000', '#3a9e7a'];

/* Concat's pad clusters (corpus_kmeans in corpus_engine.c): K = 16, 16 iterations,
 * seeds = evenly spaced slices in corpus order, empty clusters reseeded the same way,
 * float32 sums. Holding a pad in Map mode jumps to cluster (note % K). */
function kmeans(qn, n) {
  const K = Math.min(16, n), N = NDESC;
  if (!K) return null;
  const cen = new Float32Array(K * N), asg = new Int32Array(n), cnt = new Int32Array(K);
  for (let k = 0; k < K; k++) { const r = Math.floor((k * n) / K); cen.set(qn.subarray(r * N, (r + 1) * N), k * N); }
  const dist = (i, k) => { let d = 0; for (let j = 0; j < N; j++) { const e = qn[i * N + j] - cen[k * N + j]; d = Math.fround(d + Math.fround(e * e)); } return d; };
  const assign = () => {
    for (let i = 0; i < n; i++) {
      let best = 0, bd = Infinity;
      for (let k = 0; k < K; k++) { const d = dist(i, k); if (d < bd) { bd = d; best = k; } }
      asg[i] = best;
    }
  };
  for (let it = 0; it < 16; it++) {
    assign();
    cen.fill(0); cnt.fill(0);
    for (let i = 0; i < n; i++) { const k = asg[i]; cnt[k]++; for (let j = 0; j < N; j++) cen[k * N + j] += qn[i * N + j]; }
    for (let k = 0; k < K; k++) {
      if (cnt[k] > 0) { const inv = Math.fround(1 / cnt[k]); for (let j = 0; j < N; j++) cen[k * N + j] *= inv; }
      else { const r = ((Math.imul(it, 2654435761 | 0) + Math.imul(k, 40503)) >>> 0) % n; cen.set(qn.subarray(r * N, (r + 1) * N), k * N); }
    }
  }
  assign();                                                  /* members, for colours */
  const size = new Int32Array(K);
  for (let i = 0; i < n; i++) size[asg[i]]++;
  /* what a held pad plays: the slices nearest the centroid (full descriptor space) */
  const near = [];
  for (let k = 0; k < K; k++) {
    const bd = [], bi = [];
    for (let i = 0; i < n; i++) {
      const d = dist(i, k);
      if (bd.length < 16 || d < bd[bd.length - 1]) {
        let q = bd.length < 16 ? bd.length : 15;
        while (q > 0 && bd[q - 1] > d) { bd[q] = bd[q - 1]; bi[q] = bi[q - 1]; q--; }
        bd[q] = d; bi[q] = i;
      }
    }
    near.push(bi);
  }
  return { K, asg, size, near };
}
/* Move pads: notes 68..99, bottom row 68..75 up to the top row 92..99. */
const padCluster = (note, K) => ((note % K) + K) % K;
const RAMP = [[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98], [253, 231, 37]];   /* viridis-like, readable both themes */
function rampColour(t) {
  const x = Math.max(0, Math.min(1, t)) * (RAMP.length - 1), i = Math.min(RAMP.length - 2, Math.floor(x)), f = x - i;
  const c = RAMP[i].map((v, k) => Math.round(v + f * (RAMP[i + 1][k] - v)));
  return `rgb(${c[0]},${c[1]},${c[2]})`;
}

function mapColours() {
  const n = corpus.n, out = new Array(n);
  if (P.colorBy === 'source') { for (let i = 0; i < n; i++) out[i] = colourOf(corpus.its[corpus.item[i]].src); return out; }
  if (P.colorBy === 'cluster') { const a = corpus.clusters ? corpus.clusters.asg : null; for (let i = 0; i < n; i++) out[i] = a ? CLUSTER_COLS[a[i] % 16] : '#888'; return out; }
  const d = +P.colorBy, idx = new Uint32Array(n);
  for (let i = 0; i < n; i++) idx[i] = i;
  idx.sort((a, b) => corpus.raw[a * NDESC + d] - corpus.raw[b * NDESC + d]);
  for (let r = 0; r < n; r++) out[idx[r]] = rampColour(n > 1 ? r / (n - 1) : 0.5);   /* by rank: every colour gets used */
  return out;
}

function renderLegend() {
  const lg = $('legend'); lg.textContent = '';
  if (!corpus || !corpus.n) return;
  const notes = [];
  if (P.view === 'raw') {
    for (const k of [P.rawX, P.rawY]) if (!corpus.used[RAW_AXES[k][1]]) notes.push(`${RAW_AXES[k][0]} is not used by this corpus: flat on the Move too`);
  }
  if (P.colorBy === 'cluster' && corpus.clusters) lg.append(padGrid());
  else if (P.colorBy !== 'source') {
    const d = +P.colorBy, st = corpus.stats[d], name = COLOR_BY.find((c) => c[0] === P.colorBy)[1];
    const bar = el('div', { class: 'bar' });
    bar.style.background = `linear-gradient(90deg, ${RAMP.map((c, i) => `rgb(${c}) ${(i / (RAMP.length - 1)) * 100}%`).join(', ')})`;
    lg.append(el('div', {}, name), bar, el('div', { class: 'ends' }, el('span', {}, fmtNum(st.min)), el('span', {}, fmtNum(st.max))));
  }
  for (const t of notes) lg.append(el('div', { style: 'color: var(--bad)' }, t));
}

function updateMap() {
  for (const e of document.querySelectorAll('.rawOnly')) e.hidden = P.view !== 'raw';
  for (const e of document.querySelectorAll('.learnedOnly')) e.hidden = P.view === 'raw';
  if (!corpus) return;
  const n = corpus.n;
  if (P.view === 'raw') {
    const dx = RAW_AXES[P.rawX][1], dy = RAW_AXES[P.rawY][1], xyz = new Float32Array(n * 3);
    for (let i = 0; i < n; i++) { xyz[3 * i] = corpus.qn[i * NDESC + dx]; xyz[3 * i + 1] = corpus.qn[i * NDESC + dy]; }
    map.setPoints(xyz, mapColours(), [RAW_AXES[P.rawX][0], RAW_AXES[P.rawY][0], ''], true);
  } else {
    map.setPoints(corpus.proj, mapColours(), ['PC1', 'PC2', 'PC3'], false);
  }
  renderLegend();
}

function bindMapBar() {
  const fill = (sel, opts, val) => { sel.textContent = ''; opts.forEach(([v, l]) => sel.append(el('option', { value: v }, l))); sel.value = String(val); };
  fill($('rawX'), RAW_AXES.map((a, i) => [i, a[0]]), P.rawX);
  fill($('rawY'), RAW_AXES.map((a, i) => [i, a[0]]), P.rawY);
  fill($('colorBy'), COLOR_BY, P.colorBy);
  $('mapAxes').value = P.view;
  const on = (id, fn) => $(id).addEventListener('change', () => { fn($(id).value); savePrefs(); updateMap(); });
  on('mapAxes', (v) => { P.view = v; });
  on('rawX', (v) => { P.rawX = +v; });
  on('rawY', (v) => { P.rawY = +v; });
  on('colorBy', (v) => { P.colorBy = v; if (v === 'cluster' && corpus && corpus.clusters) preload(clusterPaths([...Array(corpus.clusters.K).keys()]), true); });
  const slider = (id, key, fmt) => {
    const inp = $(id), out = $(id + 'V');
    inp.value = P[key]; out.textContent = fmt(P[key]);
    inp.addEventListener('input', () => { P[key] = +inp.value; out.textContent = fmt(P[key]); savePrefs(); });
  };
  slider('pDensity', 'density', (v) => `${v}/s`);
  slider('pGrain', 'grain', (v) => `${v} ms`);
  slider('pSpread', 'spread', (v) => String(v));
  $('btnPlay').addEventListener('click', () => setPlayMode(map.mode !== 'play'));
  updateMap();
}

/* Playing the map: Concat's Map mode in the browser. While the pointer is down, grains
 * fire at Density per second; each picks a random slice among the Spread (K) nearest
 * dots to the pointer, avoiding the last 4 played (Concat's Repeat Avoid default), with
 * a Hann window and Concat's default pan spread. Audio is decoded on demand. */
const G = { pos: null, cluster: null, timer: 0, next: 0, recent: [], spreadPx: 24, out: null, tap: null, loading: new Set(), lastTip: -1 };
let hann = null;
const bufOf = new WeakMap();   /* decoded Float32Array -> AudioBuffer */
function grainOut() {
  if (!G.out) {
    const c = ctx(), comp = c.createDynamicsCompressor(), g = c.createGain(), tap = c.createGain();
    comp.threshold.value = -14; comp.ratio.value = 4; g.gain.value = 0.7;
    g.connect(comp); comp.connect(tap); tap.connect(c.destination);
    G.out = g; G.tap = tap;                                    /* the recorder listens to tap */
  }
  return G.out;
}
/* One background queue decodes corpus audio for playing: requests from the pointer or
 * a pad go first, the rest of the corpus fills in behind until the memory cap. */
const preQ = [];
let preRunning = false;
function preload(paths, front) {
  const fresh = paths.filter((p) => !audioMem.has(p) && !G.loading.has(p));
  for (const p of fresh) { const at = preQ.indexOf(p); if (at >= 0) preQ.splice(at, 1); }
  if (front) preQ.unshift(...fresh); else preQ.push(...fresh);
  runPreload();
}
async function runPreload() {
  if (preRunning) return;
  preRunning = true;
  while (preQ.length) {
    let total = 0;
    for (const a of audioMem.values()) total += a.length;
    if (total > AUDIO_MEM_CAP * 0.9 && !G.timer) break;          /* full: only on-demand loads from here */
    const p = preQ.shift();
    if (audioMem.has(p) || G.loading.has(p)) continue;
    G.loading.add(p);
    if (G.timer) $('mapTip').textContent = `Loading audio: ${baseName(p)}…`;
    try { await decodedAudio(p, true); } catch (e) { /* skip unreadable */ }
    G.loading.delete(p);
  }
  preRunning = false;
}
function wantAudio(path) { if (!audioMem.has(path) && !G.loading.has(path)) preload([path], true); }
function clusterPaths(clusters) {
  const seen = new Set();
  for (const c of clusters) for (const i of corpus.clusters.near[c]) seen.add(sliceInfo(i).it.path);
  return [...seen];
}
function playGrain(i, when) {
  const s = sliceInfo(i), arr = audioMem.get(s.it.path);
  if (!arr) { wantAudio(s.it.path); return false; }
  const c = ctx();
  const dur = Math.min(P.grain / 1000, s.len / SR);
  if (dur < 0.005) return false;
  let buf = bufOf.get(arr);
  if (!buf) { buf = c.createBuffer(1, arr.length, SR); buf.copyToChannel(arr, 0); bufOf.set(arr, buf); }
  if (!hann) { hann = new Float32Array(64); for (let k = 0; k < 64; k++) hann[k] = 0.5 - 0.5 * Math.cos((2 * Math.PI * k) / 63); }
  const src = c.createBufferSource(), g = c.createGain();
  src.buffer = buf;
  g.gain.setValueAtTime(0, when);
  g.gain.setValueCurveAtTime(hann, when, dur);
  src.connect(g);
  if (c.createStereoPanner) { const p = c.createStereoPanner(); p.pan.value = (Math.random() * 2 - 1) * 0.3; g.connect(p); p.connect(grainOut()); }
  else g.connect(grainOut());
  src.start(when, s.from / SR, dur);
  return true;
}
function grainTick() {
  if ((!G.pos && G.cluster === null) || !corpus || !corpus.n) return;
  const c = ctx(), now = c.currentTime;
  if (G.next < now) G.next = now + 0.005;
  let near;
  if (G.cluster !== null && corpus.clusters) near = { idx: corpus.clusters.near[G.cluster].slice(0, P.spread) };   /* held pad */
  else { near = map.nearestK(G.pos.x, G.pos.y, P.spread); G.spreadPx = Math.max(6, near.r); }
  if (!near.idx.length) return;
  let guard = 0;
  while (G.next < now + 0.08 && guard++ < 16) {                /* schedule 80 ms ahead on the audio clock */
    const pool = near.idx.filter((i) => !G.recent.includes(i));
    const pick = (pool.length ? pool : near.idx)[Math.floor(Math.random() * (pool.length || near.idx.length))];
    if (playGrain(pick, G.next)) {
      map.flash(pick);
      G.recent.push(pick); if (G.recent.length > 4) G.recent.shift();
      if (pick !== G.lastTip) { G.lastTip = pick; onMapHover(pick); }
    }
    G.next += 1 / P.density;
  }
}
function grainsRun() {
  const want = !!G.pos || G.cluster !== null;
  if (want && !G.timer) {
    ctx();
    G.next = 0; G.recent = [];
    G.timer = setInterval(grainTick, 15);
    grainTick();
  } else if (!want && G.timer) {
    clearInterval(G.timer); G.timer = 0;
    onMapHover(-1);
  }
}
function onMapPlay(pos) { G.pos = pos; grainsRun(); }
/* Hold a pad in the grid: Concat's Map-mode cluster jump. */
function padPress(c) {
  G.cluster = c;
  const set = new Set();
  corpus.clusters.asg.forEach((k, i) => { if (k === c) set.add(i); });
  map.setFocus(set);
  wantClusterAudio(c);
  grainsRun();
}
function padRelease() { if (G.cluster === null) return; G.cluster = null; updateFocus(); grainsRun(); }
function wantClusterAudio(c) { preload(clusterPaths([c]), true); }
function padGrid() {
  const K = corpus.clusters.K, box = el('div', { class: 'pads', title: 'Concat\'s pads in Map mode: hold one to jump to its cluster. Hold here to hear it.' });
  box.append(el('div', {}, 'Pad clusters (Map mode)'));
  const grid = el('div', { class: 'grid' });
  for (let row = 3; row >= 0; row--) for (let col = 0; col < 8; col++) {
    const note = 68 + row * 8 + col, c = padCluster(note, K);
    const cell = el('button', { class: 'pad', style: `background:${CLUSTER_COLS[c % 16]}`,
      title: `Pad ${note}: cluster ${c + 1}, ${corpus.clusters.size[c]} slices. Hold to hear it.` }, String(c + 1));
    cell.addEventListener('pointerdown', (e) => { e.preventDefault(); cell.setPointerCapture(e.pointerId); padPress(c); });
    cell.addEventListener('pointerup', padRelease);
    cell.addEventListener('pointercancel', padRelease);
    grid.append(cell);
  }
  box.append(grid);
  return box;
}
/* Recording: everything played on the map (drag, Shift+drag, pads), tapped after the
 * compressor and kept as float stereo at the audio context's rate, then a 16-bit WAV
 * to download or save to the Move (UserLibrary/Recordings/Corpus Builder).
 * ScriptProcessor rather than AudioWorklet: the Move serves this page over plain http,
 * where browsers do not offer AudioWorklet. */
const REC = { node: null, on: false, chunks: [], frames: 0, timer: 0, blob: null, secs: 0, savedPath: null };
const REC_MAX_S = 300;
function recNode() {
  if (REC.node) return REC.node;
  const c = ctx();
  grainOut();
  const sp = c.createScriptProcessor(4096, 2, 2);
  sp.onaudioprocess = (e) => {
    for (let ch = 0; ch < e.outputBuffer.numberOfChannels; ch++) e.outputBuffer.getChannelData(ch).fill(0);
    if (!REC.on) return;
    const ib = e.inputBuffer, L = ib.getChannelData(0).slice(), R = (ib.numberOfChannels > 1 ? ib.getChannelData(1) : ib.getChannelData(0)).slice();
    REC.chunks.push([L, R]); REC.frames += L.length;               /* silence too: timing is kept */
  };
  const mute = c.createGain(); mute.gain.value = 0;
  G.tap.connect(sp); sp.connect(mute); mute.connect(c.destination);   /* keeps it running */
  REC.node = sp;
  return sp;
}
function startRec() {
  try { recNode(); } catch (e) { setStatus('Recording is not supported in this browser', 'bad'); return; }
  REC.chunks = []; REC.frames = 0; REC.on = true;
  const b = $('btnRec'), t0 = performance.now();
  b.classList.add('rec');
  REC.timer = setInterval(() => {
    const sec = (performance.now() - t0) / 1000;
    b.textContent = `■ ${fmtClock(sec)}`;
    if (sec >= REC_MAX_S) stopRec();
  }, 200);
  if (map.mode !== 'play') setPlayMode(true);
  setStatus(`Recording: play the map (up to ${REC_MAX_S / 60} min). R or ■ stops.`);
}
function stopRec() {
  if (!REC.on) return;
  clearInterval(REC.timer);
  const b = $('btnRec'); b.classList.remove('rec'); b.textContent = '● Rec';
  setTimeout(() => { REC.on = false; finishTake(); }, 150);   /* let the last grain tail in */
}
const fmtClock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
function finishTake() {
  if (!REC.frames) { setStatus('Nothing was recorded'); return; }
  const sr = ctx().sampleRate;
  /* trim silence at both ends (the take starts at Rec, not at the first grain) */
  let first = -1, last = -1, pos = 0;
  for (const [L, R] of REC.chunks) {
    for (let k = 0; k < L.length; k++, pos++) if (Math.abs(L[k]) > 1e-4 || Math.abs(R[k]) > 1e-4) { if (first < 0) first = pos; last = pos; }
  }
  if (first < 0) { setStatus('The take was silent: play the map while recording'); return; }
  first = Math.max(0, first - Math.round(sr * 0.01)); last = Math.min(pos - 1, last + Math.round(sr * 0.25));
  const n = last - first + 1, data = new DataView(new ArrayBuffer(44 + n * 4));
  const str = (o, t) => { for (let k = 0; k < t.length; k++) data.setUint8(o + k, t.charCodeAt(k)); };
  str(0, 'RIFF'); data.setUint32(4, 36 + n * 4, true); str(8, 'WAVEfmt '); data.setUint32(16, 16, true);
  data.setUint16(20, 1, true); data.setUint16(22, 2, true); data.setUint32(24, sr, true); data.setUint32(28, sr * 4, true);
  data.setUint16(32, 4, true); data.setUint16(34, 16, true); str(36, 'data'); data.setUint32(40, n * 4, true);
  let o = 44; pos = 0;
  const q = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
  for (const [L, R] of REC.chunks) for (let k = 0; k < L.length; k++, pos++) {
    if (pos < first || pos > last) continue;
    data.setInt16(o, q(L[k]), true); data.setInt16(o + 2, q(R[k]), true); o += 4;
  }
  REC.chunks = [];
  REC.blob = new Blob([data.buffer], { type: 'audio/wav' });
  REC.secs = n / sr; REC.savedPath = null;
  const d = new Date(), pad2 = (x) => String(x).padStart(2, '0');
  $('takeName').value = `${S.name} take ${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}${pad2(d.getMinutes())}`;
  $('takeInfo').textContent = `${fmtTime(REC.secs)} · stereo · ${sr} Hz · ${(REC.blob.size / 1048576).toFixed(1)} MB`;
  $('takeSave').textContent = 'Save to the Move';
  $('dlgTake').showModal();
  setStatus('');
}
let takeNode = null;
async function playTake() {
  if (takeNode) { try { takeNode.stop(); } catch (e) { /* ignore */ } takeNode = null; $('takePlay').textContent = '▶︎ Listen'; return; }
  const c = ctx(), buf = await c.decodeAudioData(await REC.blob.arrayBuffer());
  takeNode = c.createBufferSource(); takeNode.buffer = buf; takeNode.connect(c.destination); takeNode.start();
  $('takePlay').textContent = '■ Stop';
  takeNode.onended = () => { takeNode = null; $('takePlay').textContent = '▶︎ Listen'; };
}
function downloadTake() {
  const a = el('a', { href: URL.createObjectURL(REC.blob), download: `${safeName($('takeName').value)}.wav` });
  document.body.append(a); a.click(); a.remove();
}
async function saveTake() {
  if (REC.savedPath) { addSource(REC.savedPath, false); $('dlgTake').close(); return; }   /* second press: add it */
  const name = `${safeName($('takeName').value)}.wav`, dir = `${LIB}/Recordings/Corpus Builder`;
  try {
    setStatus('Saving the take…');
    if (!(await listDir(LIB).catch(() => [])).some((r) => r.dir && r.name === 'Recordings')) await postForm('/files/mkdir', { path: LIB, name: 'Recordings' });
    if (!(await listDir(`${LIB}/Recordings`).catch(() => [])).some((r) => r.dir && r.name === 'Corpus Builder')) await postForm('/files/mkdir', { path: `${LIB}/Recordings`, name: 'Corpus Builder' });
    await postForm('/files/upload', { path: dir }, { blob: REC.blob, name: `${name}.tmp` });
    await postForm('/files/rename', { path: `${dir}/${name}.tmp`, name });
    REC.savedPath = `${dir}/${name}`;
    $('takeSave').textContent = 'Add it to the corpus';
    setStatus(`Saved Recordings/Corpus Builder/${name}`, 'ok');
  } catch (e) { setStatus(`Save failed: ${e.message || e}`, 'bad'); }
}
function bindTake() {
  $('btnRec').addEventListener('click', () => (REC.on ? stopRec() : startRec()));
  $('takePlay').addEventListener('click', () => playTake().catch(playErr));
  $('takeDownload').addEventListener('click', downloadTake);
  $('takeSave').addEventListener('click', saveTake);
  $('takeDiscard').addEventListener('click', () => { if (takeNode) playTake(); REC.blob = null; $('dlgTake').close(); });
  $('dlgTake').addEventListener('close', () => { if (takeNode) playTake(); });
}

/* Decode the corpus files in the background so the first grains are not silent:
 * the files with the most slices first. */
function preloadCorpusAudio() {
  if (!corpus) return;
  const counts = new Map();
  for (let i = 0; i < corpus.n; i++) { const p = corpus.its[corpus.item[i]].path; counts.set(p, (counts.get(p) || 0) + 1); }
  preload([...counts.keys()].sort((a, b) => counts.get(b) - counts.get(a)), false);
}
function setPlayMode(on) {
  map.setMode(on ? 'play' : 'orbit');
  const b = $('btnPlay');
  b.classList.toggle('on', on);
  b.textContent = on ? '● Playing: drag on the map' : '▶︎ Play the map';
  onMapHover(-1);
  if (on) { ctx(); preloadCorpusAudio(); }
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
  S.sel = null; S.selFile = null;
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
  Object.assign(S, { name: 'Untitled', sources: [], exclude: [], slicing: { ...DEFAULT_SLICING }, use: new Set(GROUPS.map((g) => g.id)), sel: null, selFile: null, dirty: false, loadedName: null });
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
  loadPrefs();
  map = new Map3D($('map'), { onPick: onMapPick, onHover: onMapHover, onPlay: onMapPlay, radiusPx: () => G.spreadPx });
  bindMapBar();
  bindTake();
  $('btnHelp').addEventListener('click', () => $('dlgHelp').showModal());
  $('helpClose').addEventListener('click', () => $('dlgHelp').close());
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
    $('mapBar').hidden = !mapOn;
    if (mapOn) map.draw();
  };
  $('tabMap').addEventListener('click', () => tab(true));
  $('tabDesc').addEventListener('click', () => tab(false));
  $('btnTheme').addEventListener('click', () => {
    const r = document.documentElement, dark = r.dataset.theme ? r.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
    r.dataset.theme = dark ? 'light' : 'dark';
    try { localStorage.setItem('concat-builder-theme', r.dataset.theme); } catch (e) { /* ignore */ }
    map.refresh(); updateMap(); drawWave(); renderDesc();
  });
  try { const t = localStorage.getItem('concat-builder-theme'); if (t) document.documentElement.dataset.theme = t; } catch (e) { /* ignore */ }
  window.addEventListener('beforeunload', (e) => { if (S.dirty) { e.preventDefault(); e.returnValue = ''; } });
  /* Keys act on the panel last clicked: library or corpus list. */
  let panel = 'lib';
  $('libCol').addEventListener('pointerdown', () => { panel = 'lib'; });
  $('corpusCol').addEventListener('pointerdown', () => { panel = 'src'; });
  $('rightCol').addEventListener('pointerdown', () => { panel = 'map'; });
  $('btnAddSel').addEventListener('click', () => { const l = libSelected(); if (l.length) addSources(l); });
  window.addEventListener('keydown', (e) => {
    const mod = e.ctrlKey || e.metaKey;
    if (mod && e.key === 's') { e.preventDefault(); save(); return; }
    if (e.target.closest('input, textarea, select, dialog')) return;     /* typing: native keys */
    if (mod && (e.key === 'a' || e.key === 'A')) { e.preventDefault(); panel = 'lib'; selectAllLib(); return; }
    if (e.key === 'Escape') {
      libSel = new Set(); paintLibSel(); stopAudio();
      if (S.sel !== null) { S.sel = null; S.selFile = null; renderSources(); renderEditor(); }
      return;
    }
    if (!mod && (e.key === 'p' || e.key === 'P')) { setPlayMode(map.mode !== 'play'); return; }
    if (!mod && (e.key === 'r' || e.key === 'R')) { if (REC.on) stopRec(); else startRec(); return; }
    if (e.key === '?') { $('dlgHelp').showModal(); return; }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const d = e.key === 'ArrowDown' ? 1 : -1;
      if (panel === 'src') navMove(d); else if (panel === 'lib') libMove(d);
      return;
    }
    if (panel === 'src' && (e.key === 'ArrowRight' || e.key === 'ArrowLeft')) { e.preventDefault(); navOpen(e.key === 'ArrowRight'); return; }
    if (panel === 'lib' && e.key === 'Enter') { const l = libSelected(); if (l.length) addSources(l); return; }
    if (e.key === ' ') {
      e.preventDefault();
      if (panel === 'lib') { const r = libRows[libAnchor]; if (r && !r.dir) togglePlay(r.path, 0, 0); else if (playing) pauseAudio(); return; }
      const s = selected();
      if (s && S.selFile) togglePlay(S.selFile, 0, 0);
      else if (s && s.kind === 'file' && !s.missing) togglePlay(s.path, s.start, s.end);
      else if (playing) pauseAudio();
    }
  });
  bindDrop(); bindSlicing(); syncSlicingInputs(); updateTitle();
  let lp = null;
  try { lp = localStorage.getItem('concat-builder-lib'); } catch (e) { /* ignore */ }
  showLib(lp && lp.startsWith(LIB) ? lp : LIB + '/Samples');
  recompute();
  onMapHover(-1);
  restoreDraft();
}
init();
