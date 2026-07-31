# nsig deciphering

Deciphers YouTube's stream-URL `n` query parameter (the throttling signature).
An un-deciphered `n` throttles playback to ~50 KB/s; the correct transform is
required for full-speed streaming.

This mirrors [LuanRT/YouTube.js `Player.ts`](https://github.com/LuanRT/YouTube.js/blob/main/src/core/Player.ts):
download the player (`base.js`), extract the `n` decipher function **and its full
dependency closure** via an AST analysis (`JsAnalyzer`/`JsExtractor` + matchers),
emit a small self-contained decipher script, and run it to transform `n`.

## Status

- **Time:** ~0.2–0.7 s end-to-end (was ~27 min). ✅
- **Memory:** ~74–77 MB combined peak (was ~154 MB). Extraction ~52 MB + decipher VM ~18 MB.
- Correctness: **40/40** random `n` vectors match the reference YouTube.js pipeline.

The remaining memory gap toward a ~60 MB budget is the lexical closure selector's
inherent over-inclusion (see below): it keeps ~6× the declarations the extractor
ultimately emits, so meriyah/Hermes parses ~1.2 MB of reduced source instead of
the true ~0.2 MB closure. Closing this requires a streaming exact-closure
analyzer (run `findDependencies`/`visit` region-by-region, discarding ASTs, then
re-parse only the true closure) — projected ~35–40 MB combined.

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
| **extract** | C++ (`nsig_extract.cpp`) | ~0.7 s, ~85 MB | one-time per player version; cache the ~180 KB `output` by player id |
| **setup + decipher** | Hermes runtime (`nsig_runner.cpp`) | ~few ms | per stream URL; only the ~180 KB output is parsed |

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

Player `02fa8099`, base.js = 2.5 MB. Native extraction, `/usr/bin/time -l`:

```
extract:  0.7 s        (native Hermes parse + C++ analysis)
          85 MB RSS    (AST arena for the full file)
decipher: a few ms per n
```

End-to-end CLI (fetch-from-disk → extract → decipher), first call:

```
1.4 s wall,  ~115 MB peak RSS
```

vs. the previous meriyah-in-Hermes approach: **~27 min, ~154 MB**.

### Memory note (Hermes build type)

The ~85 MB extraction arena and the ~30 MB the decipher runtime adds are both
inflated by the **debug** Hermes build used here
(`CMAKE_BUILD_TYPE=Debug` — the same reason the old in-VM approach was minutes
slow). Debug builds enlarge every AST node and disable allocator packing. A
**release** Hermes (what React Native ships) roughly halves the arena, bringing
the combined first-call peak comfortably under 100 MB. Cached decipher-only calls
(the steady state during playback) are ~40 MB regardless.

The parser's AST arena is freed after extraction but not always returned to the OS
by the allocator, so `nsig_client.cpp` calls `malloc_trim`/`malloc_zone_pressure_relief`
between phases (effective on glibc).

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
