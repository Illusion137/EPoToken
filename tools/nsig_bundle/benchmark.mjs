// nsig extractor benchmark: correctness (chunked vs full) + peak memory.
//
//   node --expose-gc benchmark.mjs <path-to-base.js>
//
// Loads the built bundle (dist/nsig_extractor.min.js) and compares the
// memory-optimized "chunked" closure selector against the baseline full-file
// parse, verifying they produce identical deciphered n values.

import { readFileSync } from 'fs';

const bundlePath = new URL('./dist/nsig_extractor.min.js', import.meta.url);
const playerPath = process.argv[2];
if (!playerPath) {
  console.error('usage: node --expose-gc benchmark.mjs <path-to-base.js>');
  process.exit(2);
}

(0, eval)(readFileSync(bundlePath, 'utf8'));
const player = readFileSync(playerPath, 'utf8');

function peak(fn) {
  if (global.gc) global.gc();
  const before = process.memoryUsage();
  const t0 = Date.now();
  const out = fn();
  const t1 = Date.now();
  const after = process.memoryUsage();
  return {
    out,
    ms: t1 - t0,
    heapMB: (after.heapUsed - before.heapUsed) / 1048576,
    rssMB: after.rss / 1048576
  };
}

// 40 deterministic pseudo-random n values.
const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
let seed = 987654321;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff);
const testN = Array.from({ length: 40 }, () =>
  Array.from({ length: 16 }, () => chars[rnd() % chars.length]).join(''));

function decipherAll(output) {
  globalThis.__epo_setupNsig(output);
  return testN.map((n) => globalThis.__epo_decipherN(n));
}

console.log(`player.js: ${(player.length / 1024 | 0)} KB\n`);

const full = peak(() => JSON.parse(globalThis.__epo_extractNsig(player)));
const fullOut = decipherAll(full.out.output);
console.log('FULL (parse whole file):');
console.log(`  ok=${full.out.ok}  sts=${full.out.sts}  outputKB=${full.out.output.length / 1024 | 0}`);
console.log(`  extract: ${full.ms}ms  heap +${full.heapMB.toFixed(1)}MB  peakRSS ${full.rssMB.toFixed(1)}MB\n`);

const chunk = peak(() => JSON.parse(globalThis.__epo_extractNsigChunked(player)));
const chunkOut = decipherAll(chunk.out.output);
console.log('CHUNKED (closure selector):');
console.log(`  ok=${chunk.out.ok}  sts=${chunk.out.sts}  outputKB=${chunk.out.output.length / 1024 | 0}`);
console.log(`  regions kept ${chunk.out.keptRegions}/${chunk.out.regionCount}  reduced ${chunk.out.reducedBytes / 1024 | 0}KB (from ${player.length / 1024 | 0}KB)`);
console.log(`  extract: ${chunk.ms}ms  heap +${chunk.heapMB.toFixed(1)}MB  peakRSS ${chunk.rssMB.toFixed(1)}MB\n`);

const mism = testN.filter((_, i) => fullOut[i] !== chunkOut[i]);
console.log(`correctness: ${testN.length - mism.length}/${testN.length} n-values match ${mism.length === 0 ? '✓' : '✗ FAIL'}`);
console.log(`heap reduction: ${(100 * (1 - chunk.heapMB / full.heapMB)).toFixed(0)}%   rss reduction: ${(100 * (1 - chunk.rssMB / full.rssMB)).toFixed(0)}%`);
if (mism.length) { console.log('mismatches:', mism.slice(0, 5)); process.exit(1); }
