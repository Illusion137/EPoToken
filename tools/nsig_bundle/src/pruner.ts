// Lexical pruner: reduces a ~2.5MB player.js down to just the top-level
// statement regions reachable from the nsig / signatureTimestamp seeds, WITHOUT
// building a full AST. The reduced source is then handed to the real JsAnalyzer
// /JsExtractor, which run their exact algorithm on a fraction of the input.
//
// This is the memory-optimization layer for low-end devices: meriyah only ever
// parses the closure (~hundreds of KB) instead of the whole 2.5MB file, cutting
// peak heap by ~10x.
//
// Correctness rests on over-approximation: token reachability is a *superset* of
// the true dependency closure, so the downstream analyzer always sees every
// declaration it needs. It can never pull in *too little*.

const KEYWORDS = new Set([
  'break', 'case', 'catch', 'class', 'const', 'continue', 'debugger', 'default',
  'delete', 'do', 'else', 'export', 'extends', 'finally', 'for', 'function', 'if',
  'import', 'in', 'instanceof', 'new', 'return', 'super', 'switch', 'this', 'throw',
  'try', 'typeof', 'var', 'void', 'while', 'with', 'yield', 'let', 'static', 'await',
  'true', 'false', 'null', 'undefined'
]);

function isIdStart(c: number): boolean {
  return (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c === 95 || c === 36;
}
function isIdPart(c: number): boolean {
  return isIdStart(c) || (c >= 48 && c <= 57);
}
function isWs(c: number): boolean {
  return c === 32 || c === 9 || c === 10 || c === 13 || c === 12 || c === 11;
}

// A statement region within the IIFE body.
export interface Region {
  start: number;
  end: number;
  decls: string[];   // names/members this region declares
  bases: string[];   // base identifiers of member declarations (e.g. 'A' for 'A.b')
}

export interface NameIndex {
  nameToRegions: Map<string, number[]>;
  baseToRegions: Map<string, number[]>;
}

/** Builds declared-name and base-identifier indexes over the statement regions. */
export function buildNameIndex(regions: Region[]): NameIndex {
  const nameToRegions = new Map<string, number[]>();
  const baseToRegions = new Map<string, number[]>();
  const add = (map: Map<string, number[]>, key: string, idx: number) => {
    const arr = map.get(key);
    if (arr) arr.push(idx); else map.set(key, [ idx ]);
  };
  for (let r = 0; r < regions.length; r++) {
    for (const d of regions[r].decls) {
      add(nameToRegions, d, r);
      const base = d.split(/[.\[]/)[0];
      if (base) add(baseToRegions, base, r);
    }
    for (const b of regions[r].bases) add(baseToRegions, b, r);
  }
  return { nameToRegions, baseToRegions };
}

export interface PruneResult {
  reduced: string;
  iifeParam: string;
  regionCount: number;
  keptCount: number;
  originalBytes: number;
  reducedBytes: number;
}

/**
 * Finds the outer IIFE `(function(param){ ... })(...)`, returns the param name
 * and the [bodyStart, bodyEnd) range of the function body (exclusive of braces).
 */
export function findIife(src: string): { param: string; bodyStart: number; bodyEnd: number } | null {
  const n = src.length;
  let i = 0;
  // Scan for the first "(function" at statement depth (skip strings/comments).
  while (i < n) {
    const c = src.charCodeAt(i);
    // Skip comments / strings so we don't match inside them.
    if (c === 47 /* / */) {
      const c2 = src.charCodeAt(i + 1);
      if (c2 === 47) { i += 2; while (i < n && src.charCodeAt(i) !== 10) i++; continue; }
      if (c2 === 42) { i += 2; while (i < n && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
    }
    if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); continue; }
    if (c === 40 /* ( */ && src.startsWith('function', i + 1)) {
      // Found "(function". Parse param list.
      let j = i + 1 + 8; // after "function"
      while (j < n && isWs(src.charCodeAt(j))) j++;
      // optional function name
      if (isIdStart(src.charCodeAt(j))) { while (j < n && isIdPart(src.charCodeAt(j))) j++; while (j < n && isWs(src.charCodeAt(j))) j++; }
      if (src.charCodeAt(j) !== 40) { i++; continue; } // expected '('
      j++;
      while (j < n && isWs(src.charCodeAt(j))) j++;
      let param = '';
      if (isIdStart(src.charCodeAt(j))) { const s = j; while (j < n && isIdPart(src.charCodeAt(j))) j++; param = src.slice(s, j); }
      // skip to ')'
      while (j < n && src.charCodeAt(j) !== 41) j++;
      j++;
      while (j < n && isWs(src.charCodeAt(j))) j++;
      if (src.charCodeAt(j) !== 123) { i++; continue; } // expected '{'
      const bodyStart = j + 1;
      const bodyEnd = matchBrace(src, j);
      if (bodyEnd < 0) return null;
      return { param, bodyStart, bodyEnd };
    }
    i++;
  }
  return null;
}

// Given index of an opening quote, returns index just past the closing quote.
function skipString(src: string, i: number): number {
  const n = src.length;
  const q = src.charCodeAt(i);
  i++;
  if (q === 96) {
    // template literal (handle ${ } nesting)
    while (i < n) {
      const c = src.charCodeAt(i);
      if (c === 92) { i += 2; continue; }
      if (c === 96) return i + 1;
      if (c === 36 && src.charCodeAt(i + 1) === 123) {
        i += 2;
        let depth = 1;
        while (i < n && depth > 0) {
          const d = src.charCodeAt(i);
          if (d === 123) depth++;
          else if (d === 125) depth--;
          else if (d === 34 || d === 39 || d === 96) { i = skipString(src, i); continue; }
          else if (d === 47 && src.charCodeAt(i + 1) === 47) { while (i < n && src.charCodeAt(i) !== 10) i++; continue; }
          else if (d === 47 && src.charCodeAt(i + 1) === 42) { i += 2; while (i < n && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
          i++;
        }
        continue;
      }
      i++;
    }
    return i;
  }
  while (i < n) {
    const c = src.charCodeAt(i);
    if (c === 92) { i += 2; continue; }
    if (c === q) return i + 1;
    i++;
  }
  return i;
}

// Returns index of the matching '}' for the '{' at index `open`, or -1.
function matchBrace(src: string, open: number): number {
  const n = src.length;
  let i = open + 1;
  let depth = 1;
  let prevSig = 123; // treat as after '{' -> regex allowed
  while (i < n) {
    const c = src.charCodeAt(i);
    if (isWs(c)) { i++; continue; }
    if (c === 47) {
      const c2 = src.charCodeAt(i + 1);
      if (c2 === 47) { i += 2; while (i < n && src.charCodeAt(i) !== 10) i++; continue; }
      if (c2 === 42) { i += 2; while (i < n && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
      // regex vs divide
      if (regexAllowed(prevSig)) { i = skipRegex(src, i); prevSig = 47; continue; }
      prevSig = 47; i++; continue;
    }
    if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); prevSig = 34; continue; }
    if (c === 123) { depth++; i++; prevSig = 123; continue; }
    if (c === 125) { depth--; i++; if (depth === 0) return i - 1; prevSig = 125; continue; }
    prevSig = c;
    i++;
  }
  return -1;
}

// Heuristic: is a regex literal allowed after the previous significant char?
// (Char-based; used only by the intra-region head helpers where regexes before
// the first '=' are vanishingly rare.)
function regexAllowed(prev: number): boolean {
  if (prev === 0) return true;
  if (isIdPart(prev)) return false;         // identifier/number -> divide
  if (prev === 41 || prev === 93) return false; // ) ]  -> divide
  return true;
}

// Keywords after which a `/` begins a regex literal, not division.
const REGEX_KW = new Set([
  'return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void', 'do',
  'else', 'throw', 'case', 'yield', 'await'
]);

// Previous-token classification for regex detection in the main scanner.
// 0 = regex-allowed (start / punctuation / regex-allowing keyword)
// 1 = value (identifier / number / string / regex / ')' / ']') -> division
const enum PrevTok { RegexOk = 0, Value = 1 }

function skipRegex(src: string, i: number): number {
  const n = src.length;
  i++; // past '/'
  let inClass = false;
  while (i < n) {
    const c = src.charCodeAt(i);
    if (c === 92) { i += 2; continue; }
    if (c === 91) inClass = true;
    else if (c === 93) inClass = false;
    else if (c === 47 && !inClass) { i++; break; }
    else if (c === 10) break; // unterminated
    i++;
  }
  while (i < n && isIdPart(src.charCodeAt(i))) i++; // flags
  return i;
}

/**
 * Splits the IIFE body into top-level statement regions using a single robust
 * linear scan. Statements are separated by depth-0 ';' and by a depth-0 '}' that
 * closes a block/label (which has no trailing ';'). Strings, template literals,
 * comments and regex literals are skipped; regex-vs-division is disambiguated via
 * the previous *token* (keyword-aware), which is essential for `return/.../` etc.
 * in minified code. Over-merging two statements is harmless (the region stays
 * valid JS); the goal is only to keep any single region small.
 */
export function splitRegions(src: string, bodyStart: number, bodyEnd: number): Region[] {
  const regions: Region[] = [];
  let i = bodyStart;
  let stmtStart = bodyStart;
  let depth = 0;
  let prev: PrevTok = PrevTok.RegexOk;

  const push = (end: number) => {
    const region = makeRegion(src, stmtStart, end);
    if (region) regions.push(region);
  };

  while (i < bodyEnd) {
    const c = src.charCodeAt(i);
    if (isWs(c)) { i++; continue; }
    // Comments.
    if (c === 47) {
      const c2 = src.charCodeAt(i + 1);
      if (c2 === 47) { i += 2; while (i < bodyEnd && src.charCodeAt(i) !== 10) i++; continue; }
      if (c2 === 42) { i += 2; while (i < bodyEnd && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
      // regex or division
      if (prev === PrevTok.RegexOk) { i = skipRegex(src, i); prev = PrevTok.Value; continue; }
      prev = PrevTok.RegexOk; i++; continue; // '/' as operator -> regex allowed after
    }
    // Strings / templates.
    if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); prev = PrevTok.Value; continue; }
    // Identifiers / keywords.
    if (isIdStart(c)) {
      const s = i;
      i++;
      while (i < bodyEnd && isIdPart(src.charCodeAt(i))) i++;
      const word = src.slice(s, i);
      prev = REGEX_KW.has(word) ? PrevTok.RegexOk : PrevTok.Value;
      continue;
    }
    // Numbers.
    if (c >= 48 && c <= 57) {
      i++;
      while (i < bodyEnd) { const d = src.charCodeAt(i); if (isIdPart(d) || d === 46) i++; else break; }
      prev = PrevTok.Value;
      continue;
    }
    // Brackets.
    if (c === 40 || c === 91 || c === 123) { depth++; i++; prev = PrevTok.RegexOk; continue; }
    if (c === 41 || c === 93) { if (depth > 0) depth--; i++; prev = PrevTok.Value; continue; }
    if (c === 125 /* } */) {
      if (depth > 0) depth--;
      i++;
      if (depth === 0) {
        // A '}' returning to depth 0 ends a block/label/function-decl statement
        // when it is not continued by an operator, member access, call, or ';'.
        let k = i;
        while (k < bodyEnd) {
          const d = src.charCodeAt(k);
          if (isWs(d)) { k++; continue; }
          if (d === 47 && src.charCodeAt(k + 1) === 47) { k += 2; while (k < bodyEnd && src.charCodeAt(k) !== 10) k++; continue; }
          if (d === 47 && src.charCodeAt(k + 1) === 42) { k += 2; while (k < bodyEnd && !(src.charCodeAt(k) === 42 && src.charCodeAt(k + 1) === 47)) k++; k += 2; continue; }
          break;
        }
        const nx = k < bodyEnd ? src.charCodeAt(k) : 0;
        const continues = nx === 59 /*;*/ || nx === 41 /*)*/ || nx === 93 /*]*/ || nx === 125 /*}*/ ||
          nx === 44 /*,*/ || nx === 46 /*.*/ || nx === 40 /*(*/ || nx === 91 /*[*/ ||
          nx === 42 /* * */ || nx === 43 /*+*/ || nx === 45 /*-*/ || nx === 37 /*%*/ ||
          nx === 60 /*<*/ || nx === 62 /*>*/ || nx === 61 /*=*/ || nx === 33 /*!*/ ||
          nx === 38 /*&*/ || nx === 124 /*|*/ || nx === 94 /*^*/ || nx === 63 /*?*/ ||
          nx === 58 /*:*/ || nx === 47 /* / */;
        if (!continues) {
          push(i);
          stmtStart = i;
        }
      }
      prev = PrevTok.RegexOk;
      continue;
    }
    if (c === 59 /* ; */) {
      if (depth === 0) {
        push(i);
        i++;
        stmtStart = i;
        prev = PrevTok.RegexOk;
        continue;
      }
      i++; prev = PrevTok.RegexOk; continue;
    }
    // Any other punctuation -> regex allowed next.
    prev = PrevTok.RegexOk;
    i++;
  }
  push(bodyEnd);
  return regions;
}

// Matches a bracket/brace/paren starting at `open`, returns index just past close.
function matchDelim(src: string, open: number, limit: number): number {
  const openCh = src.charCodeAt(open);
  const closeCh = openCh === 40 ? 41 : openCh === 91 ? 93 : 125;
  const n = Math.min(src.length, limit);
  let i = open + 1;
  let depth = 1;
  let prevSig = openCh;
  while (i < n) {
    const c = src.charCodeAt(i);
    if (isWs(c)) { i++; continue; }
    if (c === 47) {
      const c2 = src.charCodeAt(i + 1);
      if (c2 === 47) { i += 2; while (i < n && src.charCodeAt(i) !== 10) i++; continue; }
      if (c2 === 42) { i += 2; while (i < n && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
      if (regexAllowed(prevSig)) { i = skipRegex(src, i); prevSig = 47; continue; }
      prevSig = 47; i++; continue;
    }
    if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); prevSig = 34; continue; }
    if (c === 40 || c === 91 || c === 123) { i = matchDelim(src, i, limit); prevSig = 41; continue; }
    if (c === 41 || c === 93 || c === 125) { i++; if (c === closeCh) { depth--; if (depth === 0) return i; } prevSig = c; continue; }
    prevSig = c;
    i++;
  }
  return i;
}

/** Extracts declared names from the head of a statement region. */
function makeRegion(src: string, start: number, end: number): Region | null {
  // trim leading whitespace/comments
  let s = start;
  while (s < end) {
    const c = src.charCodeAt(s);
    if (isWs(c)) { s++; continue; }
    if (c === 47 && src.charCodeAt(s + 1) === 47) { s += 2; while (s < end && src.charCodeAt(s) !== 10) s++; continue; }
    if (c === 47 && src.charCodeAt(s + 1) === 42) { s += 2; while (s < end && !(src.charCodeAt(s) === 42 && src.charCodeAt(s + 1) === 47)) s++; s += 2; continue; }
    break;
  }
  if (s >= end) return null;

  const decls: string[] = [];
  const bases: string[] = [];

  const word = readWord(src, s, end);
  if (word === 'var' || word === 'let' || word === 'const') {
    collectVarNames(src, s + word.length, end, decls);
  } else if (word === 'function') {
    let j = s + 8;
    while (j < end && isWs(src.charCodeAt(j))) j++;
    const name = readWord(src, j, end);
    if (name) decls.push(name);
  } else {
    // assignment: LHS = ... ; capture member/identifier LHS
    const eq = findTopEq(src, s, end);
    if (eq > 0) {
      const lhs = src.slice(s, eq).trim();
      if (/^[A-Za-z_$][\w$.\[\]'" ]*$/.test(lhs)) {
        const member = lhs.replace(/\s+/g, '');
        decls.push(member);
        const base = member.split(/[.\[]/)[0];
        if (base && base !== member) bases.push(base);
      }
    }
  }

  return { start, end, decls, bases };
}

function readWord(src: string, i: number, end: number): string {
  if (i >= end || !isIdStart(src.charCodeAt(i))) return '';
  const s = i;
  while (i < end && isIdPart(src.charCodeAt(i))) i++;
  return src.slice(s, i);
}

// Collects declarator names in `var a=.., b=.., c` (split on depth-0 commas).
function collectVarNames(src: string, i: number, end: number, out: string[]): void {
  while (i < end) {
    while (i < end && isWs(src.charCodeAt(i))) i++;
    const name = readWord(src, i, end);
    if (name) out.push(name);
    // advance to depth-0 comma or end
    while (i < end) {
      const c = src.charCodeAt(i);
      if (isWs(c)) { i++; continue; }
      if (c === 47 && src.charCodeAt(i + 1) === 47) { i += 2; while (i < end && src.charCodeAt(i) !== 10) i++; continue; }
      if (c === 47 && src.charCodeAt(i + 1) === 42) { i += 2; while (i < end && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
      if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); continue; }
      if (c === 40 || c === 91 || c === 123) { i = matchDelim(src, i, end); continue; }
      if (c === 47) { i = skipRegex(src, i); continue; }
      if (c === 44) { i++; break; } // comma -> next declarator
      i++;
    }
  }
}

// Finds a top-level '=' (assignment, not ==/===/!=/<=/>=) within [s,end).
function findTopEq(src: string, s: number, end: number): number {
  let i = s;
  let prevSig = 0;
  while (i < end) {
    const c = src.charCodeAt(i);
    if (isWs(c)) { i++; continue; }
    if (c === 47 && src.charCodeAt(i + 1) === 47) { i += 2; while (i < end && src.charCodeAt(i) !== 10) i++; continue; }
    if (c === 47 && src.charCodeAt(i + 1) === 42) { i += 2; while (i < end && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
    if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); prevSig = 34; continue; }
    if (c === 40 || c === 91 || c === 123) { i = matchDelim(src, i, end); prevSig = 41; continue; }
    if (c === 47) { if (regexAllowed(prevSig)) { i = skipRegex(src, i); prevSig = 47; continue; } prevSig = 47; i++; continue; }
    if (c === 61 /* = */) {
      const next = src.charCodeAt(i + 1);
      if (next === 61) { i += 2; prevSig = 61; continue; } // == / ===
      if (prevSig === 33 || prevSig === 60 || prevSig === 62) { i++; continue; } // != <= >=
      return i;
    }
    prevSig = c;
    i++;
  }
  return -1;
}

// Collects identifier tokens used in a region [start,end), skipping strings/comments/regex.
function collectUsedTokens(src: string, start: number, end: number, out: Set<string>): void {
  let i = start;
  let prevSig = 0;
  while (i < end) {
    const c = src.charCodeAt(i);
    if (c === 47) {
      const c2 = src.charCodeAt(i + 1);
      if (c2 === 47) { i += 2; while (i < end && src.charCodeAt(i) !== 10) i++; continue; }
      if (c2 === 42) { i += 2; while (i < end && !(src.charCodeAt(i) === 42 && src.charCodeAt(i + 1) === 47)) i++; i += 2; continue; }
      if (regexAllowed(prevSig)) { i = skipRegex(src, i); prevSig = 47; continue; }
      prevSig = 47; i++; continue;
    }
    if (c === 34 || c === 39 || c === 96) { i = skipString(src, i); prevSig = 34; continue; }
    if (isIdStart(c)) {
      const sName = i;
      while (i < end && isIdPart(src.charCodeAt(i))) i++;
      const name = src.slice(sName, i);
      if (!KEYWORDS.has(name)) out.add(name);
      prevSig = 65;
      continue;
    }
    if (!isWs(c)) prevSig = c;
    i++;
  }
}

/**
 * Prunes the player source to the reachable closure around the nsig / timestamp
 * seeds and returns a reduced program that the real analyzer can consume.
 */
export function prunePlayer(src: string): PruneResult {
  const iife = findIife(src);
  if (!iife) {
    // Fall back to the whole source if the IIFE can't be located.
    return { reduced: src, iifeParam: '', regionCount: 0, keptCount: 0, originalBytes: src.length, reducedBytes: src.length };
  }

  const regions = splitRegions(src, iife.bodyStart, iife.bodyEnd);

  // Index: declared name -> region indices; base identifier -> region indices.
  const nameToRegions = new Map<string, number[]>();
  const baseToRegions = new Map<string, number[]>();
  const add = (map: Map<string, number[]>, key: string, idx: number) => {
    const arr = map.get(key);
    if (arr) arr.push(idx); else map.set(key, [ idx ]);
  };
  for (let r = 0; r < regions.length; r++) {
    for (const d of regions[r].decls) {
      add(nameToRegions, d, r);
      const base = d.split(/[.\[]/)[0];
      if (base) add(baseToRegions, base, r);
    }
    for (const b of regions[r].bases) add(baseToRegions, b, r);
  }

  // Seeds: nsig candidates (contain 'alr' and 'yes') + timestamp ('signatureTimestamp').
  const kept = new Set<number>();
  const worklist: number[] = [];
  for (let r = 0; r < regions.length; r++) {
    const text = src.slice(regions[r].start, regions[r].end);
    if ((text.indexOf('alr') !== -1 && text.indexOf('yes') !== -1) || text.indexOf('signatureTimestamp') !== -1) {
      if (!kept.has(r)) { kept.add(r); worklist.push(r); }
    }
  }

  // BFS over token reachability.
  while (worklist.length > 0) {
    const r = worklist.pop()!;
    const used = new Set<string>();
    collectUsedTokens(src, regions[r].start, regions[r].end, used);
    for (const tok of used) {
      const viaName = nameToRegions.get(tok);
      if (viaName) for (const idx of viaName) if (!kept.has(idx)) { kept.add(idx); worklist.push(idx); }
      const viaBase = baseToRegions.get(tok);
      if (viaBase) for (const idx of viaBase) if (!kept.has(idx)) { kept.add(idx); worklist.push(idx); }
    }
  }

  // Emit reduced source in original order.
  const keptSorted = Array.from(kept).sort((a, b) => a - b);
  const parts: string[] = [];
  parts.push('(function(' + iife.param + '){var window=this;');
  for (const idx of keptSorted) {
    parts.push(src.slice(regions[idx].start, regions[idx].end).trim() + ';');
  }
  parts.push('})(this);');
  const reduced = parts.join('\n');

  return {
    reduced,
    iifeParam: iife.param,
    regionCount: regions.length,
    keptCount: keptSorted.length,
    originalBytes: src.length,
    reducedBytes: reduced.length
  };
}
