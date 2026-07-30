/**
 * EPoToken — React Native (Nitro) binding.
 *
 * The native module exposes pure-compute primitives. The TS wrapper here
 * drives the network flow with fetch() and delegates BotGuard execution to
 * a caller-supplied executor (typically a hidden react-native-webview that
 * loads bgutils-js).
 */

import { NitroModules } from 'react-native-nitro-modules';
import type { EPoToken, ContextOverrides } from './specs/EPoToken.nitro';

export type { ChallengeResult, ContextOverrides, Constants } from './specs/EPoToken.nitro';

const ep: EPoToken = NitroModules.createHybridObject<EPoToken>('EPoToken');

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

export interface PoTokenResult {
    poToken:            string;
    placeholderPoToken: string;
    visitorData:        string;
    identifier:         string;
}

export interface BotGuardSnapshotInput {
    interpreterJs: string;
    program:       string;
    globalName:    string;
}

export interface BotGuardSnapshotOutput {
    snapshot: string;
    /**
     * Opaque handle the executor uses to mint poTokens once it has the
     * integrity token. Implementations typically stash a webPoSignalOutput
     * array under a request id and look it up in mintPoToken.
     */
    handle:   unknown;
}

/**
 * Caller-supplied BotGuard executor. On RN this is normally backed by a
 * hidden WebView running bgutils-js, but it can be anything that fulfills
 * the contract.
 */
export interface BotGuardExecutor {
    snapshot(input: BotGuardSnapshotInput): Promise<BotGuardSnapshotOutput>;
    mintPoToken(
        handle: unknown,
        integrityToken: string,
        contentBinding: string,
    ): Promise<string>;
}

export interface GeneratePoTokenOptions {
    /** Defaults to the session's visitor_data. */
    contentBinding?: string;
    /**
     * Overrides the interpreter URL returned by /att/get. Useful when the
     * interpreter JS is served from a cache, mirror, or pinned commit, and
     * when the WebView already has the interpreter loaded.
     */
    interpreterUrl?: string;
    /** Pre-fetched interpreter JS (skips the network fetch entirely). */
    interpreterJs?:  string;
    /** Overrides applied to the /att/get Innertube WEB context. */
    context?:        ContextOverrides;
    /** Custom fetch implementation (defaults to globalThis.fetch). */
    fetch?:          typeof fetch;
}

// ---------------------------------------------------------------------------
// Low-level re-exports (mirror the napi/wasm bindings)
// ---------------------------------------------------------------------------

export const native = ep;

export const generatePlaceholderToken = (id: string, clientState = 1): string =>
    ep.generatePlaceholderToken(id, clientState);

export const generateVisitorData = (): string =>
    ep.generateRandomVisitorData();

// ---------------------------------------------------------------------------
// High-level: generatePoToken — same shape as the napi/wasm wrappers
// ---------------------------------------------------------------------------

export async function generatePoToken(
        executor: BotGuardExecutor,
        opts: GeneratePoTokenOptions = {}): Promise<PoTokenResult> {

    const doFetch    = opts.fetch ?? globalThis.fetch.bind(globalThis);
    const constants  = ep.getConstants();

    // 1. Build the /att/get body — fresh random visitor unless overridden.
    const ctx: ContextOverrides = { ...(opts.context ?? {}) };
    if (!ctx.visitorData) ctx.visitorData = ep.generateRandomVisitorData();
    const visitorData = ctx.visitorData!;

    const attBody = ep.buildAttGetBody(ctx);
    const attUrl  =
        `${constants.innertubeBaseUrl}/att/get?prettyPrint=false&alt=json`;

    const attResp = await doFetch(attUrl, {
        method:  'POST',
        headers: {
            'content-type':              'application/json',
            'accept':                    '*/*',
            'x-youtube-client-name':     constants.innertubeClientNameId,
            'x-youtube-client-version':  ctx.clientVersion ?? constants.innertubeClientVersion,
            'x-goog-visitor-id':         visitorData,
            'user-agent':                ctx.userAgent ?? constants.userAgent,
            'origin':                    constants.ytBaseUrl,
            'referer':                   `${constants.ytBaseUrl}/`,
        },
        body: attBody,
    });
    if (!attResp.ok) {
        throw new Error(`/att/get returned HTTP ${attResp.status}`);
    }
    const challenge = ep.parseAttResponse(await attResp.text());

    // 2. Get the interpreter JS.
    let interpreterJs: string;
    if (opts.interpreterJs) {
        interpreterJs = opts.interpreterJs;
    } else {
        const url = opts.interpreterUrl || challenge.interpreterUrl;
        if (!url && challenge.interpreterJs) {
            interpreterJs = challenge.interpreterJs;
        } else {
            if (!url) throw new Error('No interpreter URL or embedded JS available');
            const r = await doFetch(url, {
                headers: {
                    'user-agent': ctx.userAgent ?? constants.userAgent,
                    'referer':    `${constants.ytBaseUrl}/`,
                },
            });
            if (!r.ok) {
                throw new Error(`Failed to fetch interpreter JS: HTTP ${r.status}`);
            }
            interpreterJs = await r.text();
        }
    }
    if (!interpreterJs) throw new Error('Interpreter JS is empty');

    // 3. Run BotGuard via the caller-supplied executor.
    const { snapshot, handle } = await executor.snapshot({
        interpreterJs,
        program:    challenge.program,
        globalName: challenge.globalName,
    });

    // 4. POST GenerateIT.
    const genBody = ep.buildGenerateItBody(snapshot);
    const genResp = await doFetch(constants.generateItEndpoint, {
        method:  'POST',
        headers: {
            'content-type':    'application/json+protobuf',
            'x-goog-api-key':  constants.googApiKey,
            'x-user-agent':    'grpc-web-javascript/0.1',
            'user-agent':      ctx.userAgent ?? constants.userAgent,
        },
        body: genBody,
    });
    if (!genResp.ok) {
        throw new Error(`GenerateIT returned HTTP ${genResp.status}`);
    }
    const integrityToken = ep.parseGenerateItResponse(await genResp.text());

    // 5. Mint the PoToken via the executor.
    const binding = opts.contentBinding ?? visitorData;
    const poToken = await executor.mintPoToken(handle, integrityToken, binding);

    // 6. Placeholder token (synchronous; native).
    const placeholderPoToken = ep.generatePlaceholderToken(binding, 1);

    return { poToken, placeholderPoToken, visitorData, identifier: binding };
}

// ---------------------------------------------------------------------------
// Overload: same flow but starts from a pre-known interpreter URL. Useful
// when the WebView already has bgutils-js loaded against a pinned commit and
// the caller doesn't want /att/get's URL discovery to drift.
// ---------------------------------------------------------------------------
export function generatePoTokenWithInterpreter(
        executor: BotGuardExecutor,
        interpreterUrl: string,
        opts: Omit<GeneratePoTokenOptions, 'interpreterUrl'> = {})
        : Promise<PoTokenResult> {
    return generatePoToken(executor, { ...opts, interpreterUrl });
}
