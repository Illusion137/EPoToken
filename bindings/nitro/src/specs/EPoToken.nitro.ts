// Nitro Module spec. Consumed by `nitrogen` to generate iOS/Android bridges.
//
// This module exposes only pure-compute primitives — no HTTP, no V8 — because
// React Native ships Hermes / JSC (not V8) and embedding libcurl on mobile is
// fiddly. The TS public API (../index.ts) drives the flow with RN's fetch()
// and a user-supplied BotGuard executor (typically a hidden WebView).

import type { HybridObject } from 'react-native-nitro-modules';

export interface ChallengeResult {
    interpreterUrl: string;
    interpreterJs:  string;
    program:        string;
    globalName:     string;
}

export interface ContextOverrides {
    visitorData?:      string;
    clientVersion?:    string;
    hl?:               string;
    gl?:               string;
    userAgent?:        string;
    osName?:           string;
    osVersion?:        string;
    browserName?:      string;
    browserVersion?:   string;
    timeZone?:         string;
    utcOffsetMinutes?: number;
}

export interface Constants {
    userAgent:              string;
    requestKey:             string;
    googApiKey:             string;
    googBaseUrl:            string;
    createEndpoint:         string;
    generateItEndpoint:     string;
    innertubeApiKey:        string;
    innertubeBaseUrl:       string;
    innertubeClientName:    string;
    innertubeClientNameId:  string;
    innertubeClientVersion: string;
    ytBaseUrl:              string;
    staticVisitorId:        string;
}

export interface EPoToken extends HybridObject<{ ios: 'c++', android: 'c++' }> {
    // ---- Pure-compute primitives ------------------------------------------
    generatePlaceholderToken(identifier: string, clientState: number): string;

    generateVisitorData():       string;
    generateRandomVisitorData(): string;
    encodeVisitorData(id: string, timestamp: number): string;
    decodeVisitorDataId(visitorData: string): string;

    buildAttGetBody(opts: ContextOverrides): string;
    buildGenerateItBody(snapshot: string):   string;
    buildChallengeCreateBody():              string;

    parseAttResponse(jsonBody: string):        ChallengeResult;
    parseChallengeResponse(jsonBody: string):  ChallengeResult;
    parseGenerateItResponse(jsonBody: string): string;

    descramble(encoded: string): string;
    getConstants():              Constants;
}
