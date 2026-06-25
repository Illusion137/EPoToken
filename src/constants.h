#pragma once

namespace epotoken::constants {

// ---------------------------------------------------------------------------
// jnn-pa (bgutils-js) — BotGuard Create / GenerateIT RPC
// ---------------------------------------------------------------------------
inline constexpr const char* GOOG_API_KEY  = "AIzaSyDyT5W0Jh49F30Pqqtyfdf7pDLFKLJoAnw";
inline constexpr const char* GOOG_BASE_URL = "https://jnn-pa.googleapis.com";
inline constexpr const char* REQUEST_KEY   = "O43z0dpjhgX20SCx4KAo";

inline constexpr const char* CREATE_ENDPOINT =
    "https://jnn-pa.googleapis.com/$rpc/google.internal.waa.v1.Waa/Create";
inline constexpr const char* GENERATE_IT_ENDPOINT =
    "https://jnn-pa.googleapis.com/$rpc/google.internal.waa.v1.Waa/GenerateIT";

// ---------------------------------------------------------------------------
// Innertube WEB client — mirrors Constants.CLIENTS.WEB in YouTubei.js
// ---------------------------------------------------------------------------
inline constexpr const char* YT_BASE_URL = "https://www.youtube.com";
// https://www.youtube.com/youtubei/v1  (PRODUCTION_1 + API_VERSION)
inline constexpr const char* INNERTUBE_BASE_URL =
    "https://www.youtube.com/youtubei/v1";

inline constexpr const char* INNERTUBE_API_KEY     = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
inline constexpr const char* INNERTUBE_CLIENT_NAME = "WEB";
// CLIENT_NAME_IDS['WEB'] = '1' in Constants.ts
inline constexpr const char* INNERTUBE_CLIENT_NAME_ID = "1";
// Constants.CLIENTS.WEB.VERSION
inline constexpr const char* INNERTUBE_CLIENT_VERSION = "2.20260623.01.00";

// 11-char static visitor ID used as a local fallback when no visitor_data
// is available and sw.js_data is unreachable (mirrors STATIC_VISITOR_ID).
inline constexpr const char* STATIC_VISITOR_ID = "6zpwvWUNAco";

// ---------------------------------------------------------------------------
// Browser simulation (navigator / screen globals in V8 env)
// ---------------------------------------------------------------------------
inline constexpr const char* USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36";

inline constexpr int SCREEN_WIDTH        = 2560;
inline constexpr int SCREEN_HEIGHT       = 1440;
inline constexpr int HARDWARE_CONCURRENCY = 8;

} // namespace epotoken::constants
