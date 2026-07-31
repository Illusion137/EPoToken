// Entry point for the embedded nsig extractor bundle.
//
// Mirrors LuanRT/YouTube.js src/core/Player.ts (getPlayerData + decipher) but
// packaged as globals so a host JS engine (Hermes) can drive it from C++.
//
// Exposed globals:
//   __epo_extractNsig(playerJs)   -> JSON string { ok, output, exported, sts, error? }
//   __epo_setupNsig(output)       -> stashes exportedVars on globalThis for fast reuse
//   __epo_decipherN(n)            -> deciphered n (requires __epo_setupNsig first)

import { JsAnalyzer, type ExtractionConfig } from './JsAnalyzer.js';
import { JsExtractor } from './JsExtractor.js';
import { nsigMatcher, timestampMatcher } from './matchers.js';
import { selectClosure } from './selector.js';

const NSIG_FN = 'nsigFunction';
const TS_VAR = 'signatureTimestampVar';

function runExtraction(sourceJs: string, meta?: Record<string, unknown>): string {
  try {
    const extractions: ExtractionConfig[] = [
      { friendlyName: NSIG_FN, match: nsigMatcher },
      { friendlyName: TS_VAR, match: timestampMatcher, collectDependencies: false }
    ];

    const analyzer = new JsAnalyzer(sourceJs, { extractions });
    const extractor = new JsExtractor(analyzer);

    const result = extractor.buildScript({
      disallowSideEffectInitializers: true,
      exportRawValues: true,
      rawValueOnly: [ TS_VAR ]
    });

    const rawSts = result.exportedRawValues ? (result.exportedRawValues as any)[TS_VAR] : undefined;

    return JSON.stringify({
      ok: result.exported.includes(NSIG_FN),
      output: result.output,
      exported: result.exported,
      sts: parseInt(rawSts) || 0,
      ...meta
    });
  } catch (e: any) {
    return JSON.stringify({ ok: false, error: String(e && e.stack ? e.stack : e), ...meta });
  }
}

// Baseline: parse the entire player.js (high peak memory).
function extractNsig(playerJs: string): string {
  return runExtraction(playerJs);
}

// Memory-optimized: prune to the nsig closure first, then analyze the (much
// smaller) reduced source. meriyah never sees the full 2.5MB file.
function extractNsigChunked(playerJs: string): string {
  const p = selectClosure(playerJs);
  return runExtraction(p.reduced, {
    regionCount: p.regionCount,
    keptRegions: p.keptCount,
    parsedRegions: p.parsedRegions,
    originalBytes: p.originalBytes,
    reducedBytes: p.reducedBytes
  });
}

// Builds the per-call nsig processor. Derived from YouTube.js getNsigProcessorFn
// but reduced to the n-only path.
function nsigProcessorBody(): string {
  return `
  var exportedVars = globalThis.__epo_vars;
  var mockStreamingURL = "https://ytjs.googlevideo.com/videoplayback?expire=1234567890&" + "n=" + encodeURIComponent(n);
  var urlCtorFunction = exportedVars.nsigFunction || (function(){ throw new Error('No n/sig decipher function extracted'); });
  var urlCtor = urlCtorFunction(mockStreamingURL, "", "");
  var proto = Object.getPrototypeOf(urlCtor);
  var properties = Object.getOwnPropertyNames(proto);
  var methodBlacklist = ['constructor', 'clone', 'set', 'get'];
  for (var i = 0; i < properties.length; i++) {
    var prop = properties[i];
    if (methodBlacklist.indexOf(prop) !== -1) continue;
    if (typeof urlCtor[prop] === 'function') urlCtor[prop]();
  }
  var nResult = urlCtor.get('n');
  return nResult ? decodeURIComponent(nResult) : "";`;
}

const g: any = (typeof globalThis !== 'undefined') ? globalThis : (function(){ return this; })();

g.__epo_extractNsig = extractNsig;
g.__epo_extractNsigChunked = extractNsigChunked;

// Evaluates the extractor output once and stashes exportedVars for reuse.
g.__epo_setupNsig = function(output: string): boolean {
  // The extractor output is `const __jsExtractorGlobal = ...; const exportedVars = (IIFE);`
  // Run it and publish exportedVars to the global so decipher calls can reuse it
  // without re-parsing the (large) extracted script.
  const fn = new Function(output + '\nglobalThis.__epo_vars = exportedVars;\nreturn true;');
  return !!fn();
};

g.__epo_decipherN = function(n: string): string {
  const fn = new Function('n', nsigProcessorBody());
  return fn(n);
};

// Convenience one-shot used by standalone tests.
g.__epo_processFn = nsigProcessorBody;
