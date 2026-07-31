// Scope-aware lazy closure selector.
//
// Replaces naive token reachability (which explodes on minified code because
// single-letter local vars collide with top-level declarations). Instead we
// parse only the statements we actually reach, collect their *free* variables
// with proper lexical scoping (mirroring JsAnalyzer.findDependencies), resolve
// those against an accurate top-level name index, and recurse.
//
// Result: meriyah parses only the nsig dependency closure (~hundreds of KB),
// not the whole 2.5MB file — for both the selector pass and the downstream
// analyzer pass.

import { parseScript, type ESTree } from 'meriyah';
import { jsBuiltIns, walkAst } from './helpers.js';
import {
  findIife, splitRegions, buildNameIndex, type Region, type NameIndex
} from './pruner.js';

/** Builds the dotted string for a non-computed member chain rooted at an Identifier. */
function memberChain(node: any): string | null {
  const segs: string[] = [];
  let cur = node;
  while (cur && cur.type === 'MemberExpression') {
    if (cur.computed) return null;
    if (cur.property.type !== 'Identifier') return null;
    segs.unshift(cur.property.name);
    cur = cur.object;
  }
  if (cur && cur.type === 'Identifier') {
    segs.unshift(cur.name);
    return segs.join('.');
  }
  return null;
}

export interface RegionRefs {
  ids: Set<string>;       // free identifier names
  members: Set<string>;   // full non-computed member chains rooted at a free id
}

/** Collects free identifiers and member chains referenced in a parsed region. */
function collectFreeVars(root: ESTree.Node): RegionRefs {
  const free = new Set<string>();
  const members = new Set<string>();

  type Scope = { names: Set<string>; type: 'function' | 'block' };
  const scopeStack: Scope[] = [ { names: new Set(), type: 'block' } ];
  const currentScope = () => scopeStack[scopeStack.length - 1];
  const isInScope = (name: string) => {
    for (let i = scopeStack.length - 1; i >= 0; i--) {
      if (scopeStack[i].names.has(name)) return true;
    }
    return false;
  };

  const collectBinding = (pattern: any, target: Set<string>) => {
    if (!pattern) return;
    switch (pattern.type) {
      case 'Identifier': target.add(pattern.name); break;
      case 'ObjectPattern':
        for (const prop of pattern.properties) {
          if (prop.type === 'RestElement') collectBinding(prop.argument, target);
          else if (prop.type === 'Property') collectBinding(prop.value, target);
        }
        break;
      case 'ArrayPattern':
        for (const el of pattern.elements) if (el) collectBinding(el, target);
        break;
      case 'RestElement': collectBinding(pattern.argument, target); break;
      case 'AssignmentPattern': collectBinding(pattern.left, target); break;
    }
  };
  const collectParams = (fn: any, target: Set<string>) => {
    if (!fn?.params) return;
    for (const p of fn.params) collectBinding(p, target);
  };

  walkAst(root, {
    enter: (n: any, parent: any) => {
      switch (n.type) {
        case 'FunctionDeclaration':
        case 'FunctionExpression':
        case 'ArrowFunctionExpression': {
          const isDecl = n.type === 'FunctionDeclaration';
          const fnName = 'id' in n ? n.id?.name : undefined;
          if (isDecl && fnName) currentScope().names.add(fnName);
          const fnScope: Scope = { names: new Set(), type: 'function' };
          if (n.type === 'FunctionExpression' && fnName) fnScope.names.add(fnName);
          collectParams(n, fnScope.names);
          scopeStack.push(fnScope);
          break;
        }
        case 'BlockStatement':
          scopeStack.push({ names: new Set(), type: 'block' });
          break;
        case 'CatchClause': {
          const s = new Set<string>();
          if (n.param) collectBinding(n.param, s);
          scopeStack.push({ names: s, type: 'block' });
          break;
        }
        case 'VariableDeclaration': {
          const targetScope = n.kind === 'var'
            ? [...scopeStack].reverse().find((s) => s.type === 'function') ?? currentScope()
            : currentScope();
          for (const d of n.declarations) collectBinding(d.id, targetScope.names);
          break;
        }
        case 'ClassDeclaration':
          if (n.id?.name) currentScope().names.add(n.id.name);
          break;
        case 'LabeledStatement':
          if (n.label?.type === 'Identifier') currentScope().names.add(n.label.name);
          break;
        case 'MemberExpression': {
          // Record the full non-computed member chain (e.g. `g.o_`, `X.a.b`) so
          // the selector can resolve it to its exact declaration, mirroring
          // JsAnalyzer's member-string dependency handling.
          const chain = memberChain(n);
          if (chain) {
            const base = chain.split('.')[0];
            if (!isInScope(base) && !jsBuiltIns.has(base)) members.add(chain);
          }
          break;
        }
        case 'Identifier': {
          // Property key / member property: not a free variable of its own,
          // but the base of a member chain is (handled when we hit the base).
          if (parent?.type === 'Property' && parent.key === n && !parent.computed) return;
          if (parent?.type === 'MethodDefinition' && parent.key === n && !parent.computed) return;
          if (parent?.type === 'MemberExpression' && parent.property === n && !parent.computed) return;
          if (parent?.type === 'MetaProperty') return;
          if (isInScope(n.name) || jsBuiltIns.has(n.name)) return;
          free.add(n.name);
          break;
        }
        case 'ForStatement':
        case 'ForInStatement':
        case 'ForOfStatement':
          scopeStack.push({ names: new Set(), type: 'block' });
          break;
      }
    },
    leave: (n: any) => {
      switch (n.type) {
        case 'FunctionDeclaration':
        case 'FunctionExpression':
        case 'ArrowFunctionExpression':
        case 'BlockStatement':
        case 'CatchClause':
        case 'ForStatement':
        case 'ForInStatement':
        case 'ForOfStatement':
          if (scopeStack.length > 1) scopeStack.pop();
          break;
      }
    }
  });

  return { ids: free, members };
}

export interface SelectResult {
  reduced: string;
  iifeParam: string;
  regionCount: number;
  keptCount: number;
  originalBytes: number;
  reducedBytes: number;
  parsedRegions: number;
  keptNames?: Set<string>;
}

/**
 * Selects the accurate nsig/timestamp dependency closure and returns a reduced
 * program the stock JsAnalyzer/JsExtractor can consume.
 */
export function selectClosure(src: string): SelectResult {
  const iife = findIife(src);
  if (!iife) {
    return { reduced: src, iifeParam: '', regionCount: 0, keptCount: 0, originalBytes: src.length, reducedBytes: src.length, parsedRegions: 0 };
  }

  const regions: Region[] = splitRegions(src, iife.bodyStart, iife.bodyEnd);
  const index: NameIndex = buildNameIndex(regions);

  // Prototype-method index: base identifier -> regions declaring `Base.prototype.*`.
  // The stock extractor emits *all* prototype methods of a depended-upon class,
  // so when a class identifier is reached we pull its prototype attachments too.
  const protoToRegions = new Map<string, number[]>();
  for (let r = 0; r < regions.length; r++) {
    for (const d of regions[r].decls) {
      const pi = d.indexOf('.prototype.');
      if (pi > 0) {
        const base = d.slice(0, pi);
        const arr = protoToRegions.get(base);
        if (arr) arr.push(r); else protoToRegions.set(base, [ r ]);
      }
    }
  }

  // Prototype-alias index. YouTube attaches methods through a reused alias var:
  //   g.r = hO.prototype; g.r.add = function(){...}; g.r.ceil = ...
  //   g.r = qB.prototype; g.r.foo = ...            (alias reassigned per class)
  // The stock analyzer links each `g.r.*` block back to the class in the
  // preceding `g.r = Class.prototype`, and emits them when that class is
  // depended. We mirror it (over-approximating: any class aliased through `g.r`
  // pulls all `g.r.*` members — a safe superset): detect `Alias = Base.prototype`
  // (Alias may itself be a member like `g.r`) and, when `Base` is reached, pull
  // every alias-assignment region plus every `Alias.*` member region.
  const ALIAS_RE = /^(?:var\s+)?([A-Za-z_$][\w$.]*?)\s*=\s*([A-Za-z_$][\w$.]*)\.prototype\s*$/;
  // Per alias expression, the ordered list of `Alias = Base.prototype` assignments.
  const aliasAssigns = new Map<string, { idx: number; base: string }[]>();
  const aliasExprs = new Set<string>();
  for (let r = 0; r < regions.length; r++) {
    const text = src.slice(regions[r].start, regions[r].end).trim();
    if (text.indexOf('.prototype') === -1) continue;
    const m = ALIAS_RE.exec(text);
    if (m) {
      const alias = m[1];
      const base = m[2];
      aliasExprs.add(alias);
      const arr = aliasAssigns.get(alias);
      if (arr) arr.push({ idx: r, base }); else aliasAssigns.set(alias, [ { idx: r, base } ]);
    }
  }

  // Attribute each `Alias.*` member region to the class in the nearest preceding
  // `Alias = Class.prototype` assignment (exactly how the analyzer binds them),
  // giving precise class -> {assignment region, member regions} mappings.
  const ownerAssignRegions = new Map<string, number[]>();
  const ownerMemberRegions = new Map<string, number[]>();
  const pushMap = (map: Map<string, number[]>, key: string, idx: number) => {
    const arr = map.get(key);
    if (arr) arr.push(idx); else map.set(key, [ idx ]);
  };
  if (aliasExprs.size > 0) {
    for (const [ , assigns ] of aliasAssigns) {
      for (const a of assigns) pushMap(ownerAssignRegions, a.base, a.idx);
    }
    for (let r = 0; r < regions.length; r++) {
      for (const d of regions[r].decls) {
        for (const ae of aliasExprs) {
          if (d.length > ae.length && d.startsWith(ae) && d.charCodeAt(ae.length) === 46 /* . */) {
            const assigns = aliasAssigns.get(ae)!;
            // nearest preceding assignment of this alias
            let owner: string | null = null;
            for (let k = assigns.length - 1; k >= 0; k--) {
              if (assigns[k].idx < r) { owner = assigns[k].base; break; }
            }
            if (owner) pushMap(ownerMemberRegions, owner, r);
            break;
          }
        }
      }
    }
  }

  const kept = new Set<number>();
  const worklist: number[] = [];
  let parsedRegions = 0;

  const includeRegions = (arr: number[] | undefined) => {
    if (!arr) return;
    for (const idx of arr) if (!kept.has(idx)) { kept.add(idx); worklist.push(idx); }
  };

  // Seed with nsig candidates and the timestamp region.
  for (let r = 0; r < regions.length; r++) {
    const text = src.slice(regions[r].start, regions[r].end);
    const isNsig = text.indexOf('alr') !== -1 && text.indexOf('yes') !== -1;
    const isTs = text.indexOf('signatureTimestamp') !== -1;
    if (isNsig || isTs) {
      if (!kept.has(r)) { kept.add(r); worklist.push(r); }
    }
  }

  // Pull prototype methods attached to class `key` via a `key.prototype` alias.
  const includeAliasesOf = (key: string) => {
    includeRegions(ownerAssignRegions.get(key)); // `Alias = key.prototype` region(s)
    includeRegions(ownerMemberRegions.get(key)); // `Alias.*` members bound to key
  };

  // Resolve a bare identifier: its own declaration + its prototype methods/aliases.
  const includeIdentifier = (nm: string) => {
    includeRegions(index.nameToRegions.get(nm));
    includeRegions(protoToRegions.get(nm));
    includeAliasesOf(nm);
  };

  // Resolve a member chain to its exact declaration, trying progressively
  // shorter prefixes (mirrors JsAnalyzer's longest-declared-member lookup).
  const includeMember = (chain: string) => {
    let cur = chain;
    for (;;) {
      const exact = index.nameToRegions.get(cur);
      if (exact) { includeRegions(exact); includeAliasesOf(cur); return; }
      const dot = cur.lastIndexOf('.');
      if (dot <= 0) break;
      cur = cur.slice(0, dot);
    }
    // No declared member prefix — fall back to the base identifier.
    includeIdentifier(chain.split('.')[0]);
  };

  while (worklist.length > 0) {
    const r = worklist.pop()!;
    const region = regions[r];
    const text = src.slice(region.start, region.end).trim();
    if (!text) continue;

    let refs: RegionRefs;
    try {
      const ast = parseScript(text, { ranges: false, loc: false, module: false });
      parsedRegions++;
      refs = collectFreeVars(ast);
    } catch {
      // If a region can't be parsed in isolation, skip dep analysis for it.
      // It is still kept (already in `kept`); its own deps may be missed, but
      // this is extremely rare for top-level base.js statements.
      continue;
    }

    for (const chain of refs.members) includeMember(chain);
    for (const nm of refs.ids) {
      if (nm === iife.param) continue;
      includeIdentifier(nm);
    }
  }

  const keptSorted = Array.from(kept).sort((a, b) => a - b);
  const keptNames = new Set<string>();
  const parts: string[] = [];
  parts.push('(function(' + iife.param + '){var window=this;');
  for (const idx of keptSorted) {
    for (const d of regions[idx].decls) keptNames.add(d);
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
    reducedBytes: reduced.length,
    parsedRegions,
    keptNames
  };
}
