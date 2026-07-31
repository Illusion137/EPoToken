# nsig deciphering

Deciphers YouTube's stream-URL `n` query parameter (the throttling signature).
An un-deciphered `n` throttles playback to ~50 KB/s; the correct transform is
required for full-speed streaming.

This mirrors [LuanRT/YouTube.js `Player.ts`](https://github.com/LuanRT/YouTube.js/blob/main/src/core/Player.ts):
download the player (`base.js`), extract the `n` decipher function **and its full
dependency closure** via an AST analysis (`JsAnalyzer`/`JsExtractor` + matchers),
emit a small self-contained decipher script, and run it to transform `n`.

## Status

- **Time:** ~0.4 s end-to-end (was ~27 min). ✅
- **Memory:** **~44 MB combined peak RSS** (was ~154 MB): extraction ~26 MB + decipher VM ~16 MB. ✅
- Correctness: **60/60** random `n` vectors match the reference YouTube.js pipeline.

The closure selector now matches the analyzer's dependency semantics closely
(notably: a reused member like `g.r`, reassigned many times, is recorded **once**
— the analyzer's `if declaredVariables.has(name) continue`). That cut the reduced
source from ~1.2 MB → **~0.18 MB** (kept regions 6257 → 1349), so Hermes parses
only the true nsig closure. Stripped initializers are stubbed, and the timestamp
region is included without following its dependency tree.

## Architecture: native parse in C++

The AST analysis is a faithful **C++ port** of YouTube.js's
`utils/javascript/{helpers,matchers,JsAnalyzer,JsExtractor}.ts`
(`src/nsig_extract.cpp`), run over the AST produced by **Hermes's own C++ parser**
(`hermes::parser::JSParser`). base.js is parsed and analyzed natively; only the
tiny (~1 KB) decipher processor runs inside the Hermes runtime.

Why native: running a JS parser (meriyah) *inside* the interpreted Hermes VM to
parse a 2.5 MB file took **~27 min and 150 MB**. Hermes's native parser does the
same parse in **~0.7 s and ~75 MB**, so the whole file is analyzed directly — no
chunking/closure-selection needed.

| phase | where | cost | notes |
|-------|-------|------|-------|
| **extract** | C++ (`nsig_extract.cpp`) | ~0.06 s, ~26 MB | closure-selected: parses ~0.18 MB, not 2.5 MB; cache the ~180 KB `output` by player id |
| **setup + decipher** | Hermes runtime (`nsig_runner.cpp`) | ~few ms, +16 MB | per stream URL; only the ~180 KB output is parsed |

base.js changes ~weekly; the extracted script is cached by player id
(`nsig_client.cpp`), so extraction runs at most once per version.

## Correctness

The native output deciphers **byte-for-identical** results to the reference
YouTube.js pipeline: verified **40/40** random `n` values match between the C++
extractor and the original meriyah + JsAnalyzer/JsExtractor run in Node. The port
reproduces the tricky bits — predeclared-var patterns (`var …,HO,…; HO = …`),
member-chain dependencies (`g.o_`), reused prototype-alias blocks
(`g.r = Cls.prototype; g.r.method = …`), and strict side-effect stripping.

## Benchmarks

Player `02fa8099`, base.js = 2.5 MB, release Hermes, end-to-end CLI
(`epotoken_cli nsig <n> base.js` → select closure → extract → setup → decipher),
`/usr/bin/time -l`:

```
extract phase:   ~0.06 s   ~26 MB   (parses ~0.18 MB closure, not 2.5 MB)
decipher phase:  ~few ms   +16 MB   (Hermes VM + ~180 KB output)
combined:        ~0.4 s    ~44 MB peak RSS
```

Correctness: **60/60** random `n` vectors identical to the reference pipeline.

vs. the original meriyah-in-Hermes approach: **~27 min, ~154 MB**.

The whole-file native parse (no closure selection) is ~72 MB regardless of build
type — the closure selector is what makes the ~26 MB extraction possible. The
parser's AST arena is freed after extraction; `nsig_client.cpp` also calls
`malloc_trim`/`malloc_zone_pressure_relief` between phases.

## C++ API

```c++
#include <epotoken.h>

auto r = decipher_nsig("kX3F5oM-2vPqYtLa");          // discover player + fetch + extract (cached) + decipher
auto r2 = decipher_nsig(n, base_js_source);          // against a base.js you already have

if (auto* ok = std::get_if<epotoken::nsig_result>(&r))
    ; // ok->n, ok->player_id, ok->signature_timestamp
```

CLI:

```sh
epotoken_cli nsig <n> [base.js-path]     # path skips player discovery/fetch
```

## Building

The native extractor needs Hermes's **source tree** (parser + AST headers) and the
static frontend libs from a Hermes **build tree**:

```sh
cmake -B build -DBUILD_CLI=ON \
  -DHERMES_INCLUDE_DIR=<hermes>/API -DHERMES_LIBRARY=<hermes-build>/lib/libhermesvm.dylib \
  -DHERMES_SRC_ROOT=<hermes> -DHERMES_BUILD_ROOT=<hermes-build> \
  -DCMAKE_CXX_FLAGS="-I<hermes>/API/jsi -I<hermes>/public -I<hermes-build>/public"
cmake --build build -j
```

## Reference / regeneration

- `src/nsig_extract.{h,cpp}` — the native port (source of truth).
- `src/nsig_decipher.min.js` — the ~1 KB decipher processor embedded for the runtime
  (regenerate: `tools/nsig_bundle/build_decipher.mjs`).
- `tools/nsig_bundle/` — the original TypeScript pipeline (meriyah + the ported TS)
  kept for the Node correctness/memory benchmark (`benchmark.mjs`) that the C++
  port is validated against.
