// WASM binding via Emscripten.
//
// In the browser / WASM environment V8 cannot be embedded and raw sockets are
// unavailable. This module exports the pure-compute primitives (visitor_data,
// request body builders, response parsers, placeholder token, descramble) so
// a JS host can drive the entire PoToken flow with fetch() + bgutils-js.
//
// Flow (see bindings/wasm/index.ts for the wrapper):
//   1. JS: visitor = generateRandomVisitorData()
//   2. JS: body    = buildAttGetBody({visitorData: visitor})
//      JS: POST INNERTUBE_URL/att/get with body
//      JS: challenge = parseAttResponse(response)
//   3. JS: interpreterJs = await fetch(challenge.interpreterUrl).text()
//   4. JS: snapshot      = await bgutils.BotGuardClient.create(...).snapshot()
//   5. JS: body          = buildGenerateItBody(snapshot)
//      JS: POST GOOG_BASE_URL/.../GenerateIT with body
//      JS: token         = parseGenerateItResponse(response)
//   6. JS: poToken       = await bgutils.WebPoMinter.create(token, ...).mint()
//   7. JS: placeholder   = generatePlaceholderToken(binding)
//
// Build with Emscripten:
//   emcmake cmake -B build_wasm -DCMAKE_BUILD_TYPE=Release
//   cmake --build build_wasm --target epotoken_wasm

#include <emscripten/bind.h>

#include "../../include/epotoken.h"
#include "../../src/base64.h"
#include "../../src/challenge.h"
#include "../../src/constants.h"
#include "../../src/innertube_messages.h"

#include <string>

using namespace emscripten;

// ---------------------------------------------------------------------------
// Result types visible to JS
// ---------------------------------------------------------------------------

struct JsChallengeResult {
    bool        success;
    std::string interpreter_url;
    std::string interpreter_js;
    std::string program;
    std::string global_name;
    std::string error;
};

struct JsTokenResult {
    bool        success;
    std::string token;
    std::string error;
};

struct JsContextOverrides {
    std::string visitor_data;
    std::string client_version;
    std::string hl;
    std::string gl;
    std::string user_agent;
    std::string os_name;
    std::string os_version;
    std::string browser_name;
    std::string browser_version;
    std::string time_zone;
    int         utc_offset_minutes = 0;
};

struct JsConstants {
    std::string user_agent;
    std::string request_key;
    std::string goog_api_key;
    std::string goog_base_url;
    std::string create_endpoint;
    std::string generate_it_endpoint;
    std::string innertube_api_key;
    std::string innertube_base_url;
    std::string innertube_client_name;
    std::string innertube_client_name_id;
    std::string innertube_client_version;
    std::string yt_base_url;
    std::string static_visitor_id;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

JsChallengeResult to_js(epotoken::challenge_outcome r) {
    if (auto* ch = std::get_if<epotoken::bg_challenge>(&r)) {
        return {true, ch->interpreter_url, ch->interpreter_js,
                ch->program, ch->global_name, {}};
    }
    return {false, {}, {}, {}, {},
            std::get<epotoken::challenge_error>(r).message};
}

// ---------------------------------------------------------------------------
// Exported functions
// ---------------------------------------------------------------------------

// Parse jnn-pa Create RPC response (bgutils-js legacy format).
JsChallengeResult wasm_parse_challenge_response(const std::string& body) {
    return to_js(epotoken::parse_challenge_response(body));
}

// Parse Innertube /att/get IGetChallengeResponse.
JsChallengeResult wasm_parse_att_response(const std::string& body) {
    return to_js(epotoken::parse_att_response(body));
}

// Parse jnn-pa GenerateIT response → integrity token.
JsTokenResult wasm_parse_generate_it_response(const std::string& body) {
    auto r = epotoken::parse_generate_it_response(body);
    if (auto* s = std::get_if<std::string>(&r)) return {true, *s, {}};
    return {false, {}, std::get<epotoken::challenge_error>(r).message};
}

// Generate placeholder/cold-start token (pure C++, no network).
JsTokenResult wasm_generate_placeholder_token(
        const std::string& identifier, int client_state) {
    auto r = generate_placeholder_token(identifier,
                                        static_cast<uint8_t>(client_state));
    if (auto* s = std::get_if<std::string>(&r)) return {true, *s, {}};
    return {false, {}, std::get<epotoken::error>(r).message};
}

// VisitorData helpers.
std::string wasm_generate_visitor_data()        { return epotoken::generate_visitor_data(); }
std::string wasm_generate_random_visitor_data() { return epotoken::generate_random_visitor_data(); }

std::string wasm_encode_visitor_data(const std::string& id, double ts) {
    return epotoken::encode_visitor_data(id, static_cast<uint32_t>(ts));
}

std::string wasm_decode_visitor_data_id(const std::string& vd) {
    return epotoken::decode_visitor_data_id(vd);
}

// Request body builders.
std::string wasm_build_att_get_body(const JsContextOverrides& opts) {
    epotoken::innertube_context_overrides c;
    c.visitor_data        = opts.visitor_data;
    c.client_version      = opts.client_version;
    c.hl                  = opts.hl;
    c.gl                  = opts.gl;
    c.user_agent          = opts.user_agent;
    c.os_name             = opts.os_name;
    c.os_version          = opts.os_version;
    c.browser_name        = opts.browser_name;
    c.browser_version     = opts.browser_version;
    c.time_zone           = opts.time_zone;
    c.utc_offset_minutes  = opts.utc_offset_minutes;
    return epotoken::build_att_get_body(c);
}

std::string wasm_build_generate_it_body(const std::string& snapshot) {
    return epotoken::build_generate_it_body(snapshot);
}

std::string wasm_build_challenge_create_body() {
    return epotoken::build_challenge_create_body();
}

// bgutils descramble helper: base64url-decode then add 97 per byte.
std::string wasm_descramble(const std::string& encoded) {
    auto bytes = epotoken::base64::decode(encoded);
    for (auto& b : bytes) {
        b = static_cast<uint8_t>((static_cast<int>(b) + 97) & 0xFF);
    }
    return std::string(bytes.begin(), bytes.end());
}

// Returns the baked-in defaults so the JS wrapper doesn't need to hard-code
// any URLs / API keys that may drift from constants.h over time.
JsConstants wasm_constants() {
    using namespace epotoken::constants;
    return {
        USER_AGENT,
        REQUEST_KEY,
        GOOG_API_KEY,
        GOOG_BASE_URL,
        CREATE_ENDPOINT,
        GENERATE_IT_ENDPOINT,
        INNERTUBE_API_KEY,
        INNERTUBE_BASE_URL,
        INNERTUBE_CLIENT_NAME,
        INNERTUBE_CLIENT_NAME_ID,
        INNERTUBE_CLIENT_VERSION,
        YT_BASE_URL,
        STATIC_VISITOR_ID,
    };
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(epotoken) {
    value_object<JsChallengeResult>("ChallengeResult")
        .field("success",        &JsChallengeResult::success)
        .field("interpreterUrl", &JsChallengeResult::interpreter_url)
        .field("interpreterJs",  &JsChallengeResult::interpreter_js)
        .field("program",        &JsChallengeResult::program)
        .field("globalName",     &JsChallengeResult::global_name)
        .field("error",          &JsChallengeResult::error);

    value_object<JsTokenResult>("TokenResult")
        .field("success", &JsTokenResult::success)
        .field("token",   &JsTokenResult::token)
        .field("error",   &JsTokenResult::error);

    value_object<JsContextOverrides>("ContextOverrides")
        .field("visitorData",       &JsContextOverrides::visitor_data)
        .field("clientVersion",     &JsContextOverrides::client_version)
        .field("hl",                &JsContextOverrides::hl)
        .field("gl",                &JsContextOverrides::gl)
        .field("userAgent",         &JsContextOverrides::user_agent)
        .field("osName",            &JsContextOverrides::os_name)
        .field("osVersion",         &JsContextOverrides::os_version)
        .field("browserName",       &JsContextOverrides::browser_name)
        .field("browserVersion",    &JsContextOverrides::browser_version)
        .field("timeZone",          &JsContextOverrides::time_zone)
        .field("utcOffsetMinutes",  &JsContextOverrides::utc_offset_minutes);

    value_object<JsConstants>("Constants")
        .field("userAgent",              &JsConstants::user_agent)
        .field("requestKey",             &JsConstants::request_key)
        .field("googApiKey",             &JsConstants::goog_api_key)
        .field("googBaseUrl",            &JsConstants::goog_base_url)
        .field("createEndpoint",         &JsConstants::create_endpoint)
        .field("generateItEndpoint",     &JsConstants::generate_it_endpoint)
        .field("innertubeApiKey",        &JsConstants::innertube_api_key)
        .field("innertubeBaseUrl",       &JsConstants::innertube_base_url)
        .field("innertubeClientName",    &JsConstants::innertube_client_name)
        .field("innertubeClientNameId",  &JsConstants::innertube_client_name_id)
        .field("innertubeClientVersion", &JsConstants::innertube_client_version)
        .field("ytBaseUrl",              &JsConstants::yt_base_url)
        .field("staticVisitorId",        &JsConstants::static_visitor_id);

    function("parseChallengeResponse",   &wasm_parse_challenge_response);
    function("parseAttResponse",         &wasm_parse_att_response);
    function("parseGenerateItResponse",  &wasm_parse_generate_it_response);
    function("generatePlaceholderToken", &wasm_generate_placeholder_token);
    function("generateVisitorData",      &wasm_generate_visitor_data);
    function("generateRandomVisitorData",&wasm_generate_random_visitor_data);
    function("encodeVisitorData",        &wasm_encode_visitor_data);
    function("decodeVisitorDataId",      &wasm_decode_visitor_data_id);
    function("buildAttGetBody",          &wasm_build_att_get_body);
    function("buildGenerateItBody",      &wasm_build_generate_it_body);
    function("buildChallengeCreateBody", &wasm_build_challenge_create_body);
    function("descramble",               &wasm_descramble);
    function("getConstants",             &wasm_constants);
}
