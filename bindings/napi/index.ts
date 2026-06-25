/**
 * EPoToken — Node.js binding.
 *
 * Mirrors potoken.node.ts exactly:
 *   C++ handles Innertube HTTP (challenge + GenerateIT).
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
}

// ---------------------------------------------------------------------------
// Low-level exports (wrap native addon directly)
// ---------------------------------------------------------------------------

/** Fetches the BotGuard challenge via Innertube /att/get (C++ HTTP call). */
export async function getAttestationChallenge(
        visitorData: string): Promise<ChallengeResult> {
    return addon.getAttestationChallenge(visitorData) as Promise<ChallengeResult>;
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
        visitorData:    string,
        contentBinding?: string): Promise<PoTokenResult> {

    const binding = contentBinding ?? visitorData;

    // 1. C++ fetches Innertube challenge (falls back to jnn-pa if /att/get fails)
    const challenge = await getAttestationChallenge(visitorData);

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
