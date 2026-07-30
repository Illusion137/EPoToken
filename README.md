# EPoToken

C++ implementation of YouTube's PoToken flow. Bindings for Node.js (N-API),
browsers (WASM), and React Native (Nitro Modules).

The native core uses V8 + libcurl. Non-native bindings expose pure-compute
primitives and let the JS host handle HTTP and BotGuard.

## Layout

```
include/epotoken.h    Public C++ API
src/                  Core (V8, libcurl, parsers, helpers)
cli/main.cpp          CLI for sanity-checking the native flow
bindings/wasm/        Browser / Deno WASM binding
bindings/napi/        Node.js N-API addon
bindings/nitro/       React Native Nitro Module
```

## 1. C++

### Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_CLI=ON
cmake --build build -j
```

Requires CMake 3.16+, C++20, libcurl, V8, and `nlohmann/json` (auto-fetched).

### API

```c++
#include <epotoken.h>

auto r = generate_po_token("VIDEO_ID");

// Overload with pre-fetched / mirrored interpreter URL.
auto r2 = generate_po_token("VIDEO_ID", "https://my-cdn/interp.js");

if (auto* ok = std::get_if<epotoken::po_token_result>(&r)) {
    // ok->po_token, ok->placeholder_po_token, ok->visitor_data, ok->identifier
}

// Cold-start placeholder. Pure compute, no network.
auto p = generate_placeholder_token("VIDEO_ID");
```

### CLI

```sh
./build/epotoken_cli
./build/epotoken_cli dQw4w9WgXcQ
./build/epotoken_cli dQw4w9WgXcQ https://my-cdn/interp.js
```

## 2. Node.js

HTTP runs in C++ via libcurl. BotGuard runs in Node's V8 via `bgutils-js`.

```sh
cd bindings/napi
npm install
npm run build
```

```ts
import { generatePoToken } from 'epotoken-napi';

const { poToken, placeholderPoToken, visitorData, identifier } =
    await generatePoToken('VIDEO_ID');
```

Low-level:

```ts
import {
    getAttestationChallenge,
    postGenerateIT,
    generatePlaceholderToken,
} from 'epotoken-napi';
```

## 3. WASM

No sockets, no V8. WASM exports primitives; JS does `fetch()` and BotGuard.

```sh
cd bindings/wasm
npm install
npm run build
```

```ts
import createModule from './epotoken.js';
import { createEPoToken } from 'epotoken-wasm';

const wasm = await createModule();
const ep   = createEPoToken(wasm);

const { poToken, placeholderPoToken } = await ep.generatePoToken({
    contentBinding: 'VIDEO_ID',
    interpreterUrl: 'https://my-cdn/interp.js',  // optional
});
```

Primitives:

```ts
ep.wasm.generateRandomVisitorData();
ep.wasm.buildAttGetBody({ visitorData: 'V' });
ep.wasm.parseAttResponse(body);
ep.wasm.buildGenerateItBody(snapshot);
ep.wasm.parseGenerateItResponse(body);
ep.wasm.generatePlaceholderToken('VIDEO_ID', 1);
ep.wasm.getConstants();
```

## 4. React Native (Nitro)

RN uses Hermes / JSC, not V8. Native exposes primitives; JS does `fetch()` and
delegates BotGuard to a caller-supplied executor (typically a hidden
`react-native-webview` running `bgutils-js`).

```sh
yarn add react-native-epotoken react-native-nitro-modules
cd ios && pod install
```

```ts
import {
    generatePoToken,
    generatePoTokenWithInterpreter,
    type BotGuardExecutor,
} from 'react-native-epotoken';

const executor: BotGuardExecutor = {
    async snapshot({ interpreterJs, program, globalName }) {
        // run bgutils-js in a hidden WebView, return { snapshot, handle }
    },
    async mintPoToken(handle, integrityToken, contentBinding) {
        // mint via the same WebView, keyed by handle
    },
};

const { poToken } = await generatePoToken(executor, { contentBinding: 'VIDEO_ID' });

await generatePoTokenWithInterpreter(
    executor,
    'https://my-cdn/interp.js',
    { contentBinding: 'VIDEO_ID' },
);
```

Low-level:

```ts
import { native, generatePlaceholderToken, generateVisitorData } from 'react-native-epotoken';
```

## Picking a binding

|                            | C++       | Node          | WASM            | React Native        |
|----------------------------|-----------|---------------|-----------------|---------------------|
| HTTP                       | libcurl   | libcurl       | host `fetch()`  | host `fetch()`      |
| BotGuard                   | V8        | Node V8       | bgutils-js      | caller executor     |
| Placeholder token          | yes       | yes           | yes             | yes                 |
| `interpreter_url` override | yes       | low-level     | yes             | yes                 |

## Publishing

Both npm packages bundle the C++ core via `scripts/sync-core.js`, which copies
the repo's `include/` and `src/` into `bindings/<binding>/cpp/core/` so the
published tarball is self-contained.

One-time setup: add an `NPM_TOKEN` repo secret (npm automation token with
publish scope).

### epotoken-napi

1. Bump version: `cd bindings/napi && npm version patch` (or minor/major).
2. Push the tag: `git push origin napi-v<version>`.
3. `.github/workflows/publish-napi.yml` runs prebuildify on Linux x64/arm64
   and macOS x64/arm64, stages prebuilds, builds TS, and `npm publish`es.
   Consumers get prebuilt binaries via `node-gyp-build`; if no prebuild
   matches their platform, `node-gyp` builds from source (requires libcurl).

Local prebuild for the current platform: `npm run prebuild`.
Manual publish (skips CI): `npm run prepublishOnly && npm publish`.

### react-native-epotoken

1. Bump version: `cd bindings/nitro && npm version patch`.
2. Push the tag: `git push origin nitro-v<version>`.
3. `.github/workflows/publish-nitro.yml` syncs core, runs nitrogen, builds
   TS, packs, and publishes. RN consumers compile the C++ during their
   `pod install` / Android build.

Manual publish: `npm run prepublishOnly && npm publish`.

## Notes

- Strings in `src/constants.h` (client version, API keys, endpoints) may need
  bumping when YouTube rotates them.
- `src/bg_full_bundle.min.js` is embedded into a header at build time and is
  only used by the native V8 path.
- `bindings/*/cpp/core/` is gitignored; it's populated by sync-core during
  `npm install` (dev) or `prepublishOnly` (publish).
