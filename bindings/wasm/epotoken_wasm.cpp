// WASM binding via Emscripten.
//
// In the browser / WASM environment V8 cannot be embedded.
// This module exports only the pure-compute functions (challenge parsing,
// placeholder token generation) and the descramble helper.
// The JS host is responsible for all HTTP calls and BotGuard JS execution
// (using bgutils-js natively in the browser).
//
// Build with Emscripten:
//   emcmake cmake -B build_wasm -DCMAKE_BUILD_TYPE=Release
//   cmake --build build_wasm --target epotoken_wasm
//
// Or directly:
//   em++ -std=c++20 -O2 --bind \
//     bindings/wasm/epotoken_wasm.cpp \
//     src/base64.cpp src/challenge.cpp src/placeholder.cpp \
//     -I include -I src -I <path-to-nlohmann-json> \
//     -s MODULARIZE=1 -s EXPORT_NAME="EPoToken" \
//     -s ENVIRONMENT=web,node \
//     -o epotoken.js

#include <emscripten/bind.h>

#include "../../include/epotoken.h"
#include "../../src/base64.h"
#include "../../src/challenge.h"

#include <string>

using namespace emscripten;

// ---------------------------------------------------------------------------
// Result types visible to JS
// ---------------------------------------------------------------------------

struct JsChallengeResult {
    bool        success;
    std::string interpreter_url;
    std::string program;
    std::string global_name;
    std::string error;
};

struct JsTokenResult {
    bool        success;
    std::string token;
    std::string error;
};

// ---------------------------------------------------------------------------
// Exported functions
// ---------------------------------------------------------------------------

// Parse jnn-pa Create RPC response (bgutils-js format).
JsChallengeResult wasm_parse_challenge_response(const std::string& json_body) {
    auto r = epotoken::parse_challenge_response(json_body);
    if (auto* ch = std::get_if<epotoken::bg_challenge>(&r)) {
        return {true, ch->interpreter_url, ch->program, ch->global_name, {}};
    }
    return {false, {}, {}, {}, std::get<epotoken::challenge_error>(r).message};
}

// Parse Innertube /att/get IGetChallengeResponse.
JsChallengeResult wasm_parse_att_response(const std::string& json_body) {
    auto r = epotoken::parse_att_response(json_body);
    if (auto* ch = std::get_if<epotoken::bg_challenge>(&r)) {
        return {true, ch->interpreter_url, ch->program, ch->global_name, {}};
    }
    return {false, {}, {}, {}, std::get<epotoken::challenge_error>(r).message};
}

// Generate placeholder/cold-start token (pure C++, no network).
JsTokenResult wasm_generate_placeholder_token(
        const std::string& identifier, int client_state) {
    auto r = generate_placeholder_token(identifier,
                                        static_cast<uint8_t>(client_state));
    if (auto* s = std::get_if<std::string>(&r)) {
        return {true, *s, {}};
    }
    return {false, {}, std::get<epotoken::error>(r).message};
}

// bgutils descramble helper: base64url-decode then add 97 per byte.
// Useful when working with the raw jnn-pa challenge format from JS.
std::string wasm_descramble(const std::string& encoded) {
    auto bytes = epotoken::base64::decode(encoded);
    for (auto& b : bytes) {
        b = static_cast<uint8_t>((static_cast<int>(b) + 97) & 0xFF);
    }
    return std::string(bytes.begin(), bytes.end());
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(epotoken) {
    value_object<JsChallengeResult>("ChallengeResult")
        .field("success",        &JsChallengeResult::success)
        .field("interpreterUrl", &JsChallengeResult::interpreter_url)
        .field("program",        &JsChallengeResult::program)
        .field("globalName",     &JsChallengeResult::global_name)
        .field("error",          &JsChallengeResult::error);

    value_object<JsTokenResult>("TokenResult")
        .field("success", &JsTokenResult::success)
        .field("token",   &JsTokenResult::token)
        .field("error",   &JsTokenResult::error);

    function("parseChallengeResponse",  &wasm_parse_challenge_response);
    function("parseAttResponse",        &wasm_parse_att_response);
    function("generatePlaceholderToken",&wasm_generate_placeholder_token);
    function("descramble",              &wasm_descramble);
}
