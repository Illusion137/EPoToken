/**
 * EPoToken — Node.js binding.
 *
 * Mirrors potoken.node.ts exactly:
 *   C++ creates a fresh Innertube WEB session and fetches the BotGuard challenge.
 *   bgutils-js handles BotGuard JS execution in Node's own V8 — no second
 *   V8 instance, no extra dependencies.
 */

import * as addon from './build/Release/epotoken_napi.node';
import { BotGuardClient, WebPoMinter } from 'bgutils-js';
import type { IBgConfig } from 'bgutils-js';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export interface PoTokenResult {
    poToken:            string;
    placeholderPoToken: string;
    visitorData:        string;
    identifier:         string;
}

export interface ChallengeResult {
    interpreterUrl: string;
    program:        string;
    globalName:     string;
    visitorData:    string;
}

// ---------------------------------------------------------------------------
// Low-level exports (wrap native addon directly)
// ---------------------------------------------------------------------------

/**
 * Creates a fresh Innertube WEB session and fetches the BotGuard challenge.
 * Returns the challenge fields plus the session's visitor_data.
 */
export async function getAttestationChallenge(): Promise<ChallengeResult> {
    return addon.getAttestationChallenge() as Promise<ChallengeResult>;
}

/** Posts BotGuard snapshot to jnn-pa GenerateIT; returns the integrity token. */
export async function postGenerateIT(snapshot: string): Promise<string> {
    return addon.postGenerateIT(snapshot) as Promise<string>;
}

/**
 * Generates the cold-start placeholder token (pure C++, synchronous).
 * Equivalent to bgutils-js PoToken.generateColdStartToken().
 */
export function generatePlaceholderToken(
        identifier: string, clientState = 1): string {
    return addon.generatePlaceholderToken(identifier, clientState) as string;
}

// ---------------------------------------------------------------------------
// High-level: generatePoToken — mirrors potoken.node.ts flow
// ---------------------------------------------------------------------------

export async function generatePoToken(
        contentBinding?: string): Promise<PoTokenResult> {

    // 1. C++ creates a fresh Innertube session and fetches the challenge.
    //    visitor_data comes from the session, not the caller.
    const challenge = await getAttestationChallenge();
    const visitorData = challenge.visitorData;

    const binding = contentBinding ?? visitorData;

    // 2. Fetch interpreter JS in Node.js
    const interpResp = await fetch(challenge.interpreterUrl);
    if (!interpResp.ok) {
        throw new Error(
            `Failed to fetch interpreter JS: HTTP ${interpResp.status}`);
    }
    const interpreterJs = await interpResp.text();

    // 3. Run BotGuard in Node's own V8 via bgutils-js
    const webPoSignalOutput: unknown[] = [];
    const bgConfig: IBgConfig = {
        program:    challenge.program,
        globalName: challenge.globalName,
        globalObj:  globalThis,
    };
    const botGuard = await BotGuardClient.create(bgConfig, interpreterJs);
    const snapshot = await botGuard.snapshot({ webPoSignalOutput });

    // 4. C++ posts snapshot to GenerateIT
    const integrityToken = await postGenerateIT(snapshot);

    // 5. Mint PoToken via bgutils-js WebPoMinter
    const minter = await WebPoMinter.create(integrityToken, webPoSignalOutput);
    const poToken = await minter.mintAsWebsafeString(binding);

    // 6. C++ generates placeholder token
    const placeholderPoToken = generatePlaceholderToken(binding);

    return { poToken, placeholderPoToken, visitorData, identifier: binding };
}
