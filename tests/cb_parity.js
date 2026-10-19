// Corpus Builder parity, WASM side (node): same output format as tests/cb_parity.c.
const fs = require('fs');
const ConcatCB = require(process.argv[2]);           // path to src/builder/cb.js
ConcatCB().then((M) => {
  const file = process.argv[3];
  const a = process.argv.length > 5 ? +process.argv[4] : 0, b = process.argv.length > 5 ? +process.argv[5] : 0;
  const bytes = fs.readFileSync(file);
  const p = M._cb_alloc(bytes.length);
  M.HEAPU8.set(bytes, p);
  const name = M.stringToNewUTF8(file);
  if (!M._cb_load(p, bytes.length, name)) { console.log('decode failed'); return; }
  const out = [];
  out.push('frames ' + M._cb_decode());
  const n = M._cb_analyse(a, b, 0, 20, 2000, 0.02), nd = M._cb_ndesc();
  out.push('slices ' + n);
  const sl = M._cb_slices() >> 2, de = M._cb_desc() >> 2;
  for (let i = 0; i < n; i++) {
    let line = M.HEAP32[sl + 2 * i] + ' ' + M.HEAP32[sl + 2 * i + 1];
    for (let d = 0; d < nd; d++) line += ' ' + M.HEAPF32[de + i * nd + d].toPrecision(7);
    out.push(line);
  }
  console.log(out.join('\n'));
});
