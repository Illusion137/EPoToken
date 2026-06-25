#pragma once

namespace epotoken::constants {

// jnn-pa (bgutils-js) keys — used for BotGuard Create / GenerateIT RPC.
inline constexpr const char* GOOG_API_KEY  = "AIzaSyDyT5W0Jh49F30Pqqtyfdf7pDLFKLJoAnw";
inline constexpr const char* GOOG_BASE_URL = "https://jnn-pa.googleapis.com";
inline constexpr const char* REQUEST_KEY   = "O43z0dpjhgX20SCx4KAo";

// Innertube (YouTubei.js) — used for /att/get attestation endpoint.
inline constexpr const char* INNERTUBE_API_KEY =
    "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
inline constexpr const char* INNERTUBE_CLIENT_VERSION = "2.20241107.01.00";
inline constexpr const char* YT_BASE_URL = "https://www.youtube.com";

// Matches the user-agent used by bgutils-js / the JSDOM setup in potoken.node.ts.
inline constexpr const char* USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36";

// Endpoint builders (mirror buildURL() from bgutils-js helpers.ts)
inline constexpr const char* CREATE_ENDPOINT =
    "https://jnn-pa.googleapis.com/$rpc/google.internal.waa.v1.Waa/Create";
inline constexpr const char* GENERATE_IT_ENDPOINT =
    "https://jnn-pa.googleapis.com/$rpc/google.internal.waa.v1.Waa/GenerateIT";

// Simulated browser geometry fed to navigator / screen globals
inline constexpr int SCREEN_WIDTH  = 1920;
inline constexpr int SCREEN_HEIGHT = 1080;
inline constexpr int HARDWARE_CONCURRENCY = 8;

} // namespace epotoken::constants
