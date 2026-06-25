// Mirrors the YouTubei.js session initialization + getAttestationChallenge flow.
//
// Key references (LuanRT/YouTube.js):
//   src/core/Session.ts   — #getSessionData(), #buildContext(), #getVisitorID()
//   src/utils/ProtoUtils.ts — encodeVisitorData(), decodeVisitorData()
//   src/utils/Constants.ts  — CLIENTS.WEB, CLIENT_NAME_IDS, URLS
//   src/utils/HTTPClient.ts — #setupCommonHeaders()

#include "innertube_client.h"

#include "base64.h"
#include "constants.h"
#include "http_client.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace epotoken {

using json = nlohmann::json;

// ===========================================================================
// VisitorData protobuf helpers
// Mirrors ProtoUtils.encodeVisitorData / decodeVisitorData in YouTubei.js.
//
// Proto schema:
//   message VisitorData {
//     string id        = 1;   // wire type 2 (length-delimited)
//     uint32 timestamp = 2;   // wire type 0 (varint)
//   }
// ===========================================================================

namespace {

void write_varint(std::vector<uint8_t>& buf, uint64_t val) {
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val) byte |= 0x80;
        buf.push_back(byte);
    } while (val);
}

uint64_t read_varint(const std::vector<uint8_t>& data, size_t& pos) {
    uint64_t val   = 0;
    int      shift = 0;
    while (pos < data.size()) {
        uint8_t b = data[pos++];
        val |= static_cast<uint64_t>(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return val;
}

// Encodes { id, timestamp } as a protobuf, then base64url-encodes the result.
// Mirrors: encodeURIComponent(u8ToBase64(proto).replace(+,-).replace(/,_))
// We drop the encodeURIComponent() since the string is used in JSON bodies and
// headers — not in URL query strings — and the url_safe base64 has no =/+/ chars.
std::string encode_visitor_data(const std::string& id, uint32_t timestamp) {
    std::vector<uint8_t> buf;

    // Field 1: string (tag = (1 << 3) | 2 = 0x0A)
    buf.push_back(0x0A);
    write_varint(buf, id.size());
    for (unsigned char c : id) buf.push_back(c);

    // Field 2: uint32 (tag = (2 << 3) | 0 = 0x10)
    buf.push_back(0x10);
    write_varint(buf, timestamp);

    return base64::encode(buf, /*url_safe=*/true);
}

// Decodes a visitor_data string and returns the embedded id field.
// Mirrors: decodeVisitorData(visitor_data).id
std::string decode_visitor_data_id(const std::string& visitor_data) {
    // Handle %xx URL-encoding that encodeURIComponent() may have added.
    std::string raw;
    raw.reserve(visitor_data.size());
    for (size_t i = 0; i < visitor_data.size(); ++i) {
        if (visitor_data[i] == '%' && i + 2 < visitor_data.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int hi = hex(visitor_data[i+1]);
            int lo = hex(visitor_data[i+2]);
            if (hi >= 0 && lo >= 0) {
                raw += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        raw += visitor_data[i];
    }

    auto bytes = base64::decode(raw);

    size_t pos = 0;
    while (pos < bytes.size()) {
        uint8_t  tag        = bytes[pos++];
        uint8_t  field_num  = tag >> 3;
        uint8_t  wire_type  = tag & 0x07;

        if (wire_type == 2) {                        // length-delimited
            uint64_t len = read_varint(bytes, pos);
            if (field_num == 1 && pos + len <= bytes.size()) {
                return std::string(
                    bytes.begin() + static_cast<ptrdiff_t>(pos),
                    bytes.begin() + static_cast<ptrdiff_t>(pos + len));
            }
            pos += static_cast<size_t>(len);
        } else if (wire_type == 0) {                 // varint
            read_varint(bytes, pos);
        } else if (wire_type == 5) {                 // 32-bit fixed
            pos += 4;
        } else if (wire_type == 1) {                 // 64-bit fixed
            pos += 8;
        } else {
            break;                                   // unknown wire type
        }
    }
    return "";
}

// ===========================================================================
// Session data — all fields extracted from sw.js_data and used in context
// ===========================================================================

struct session_data {
    // HTTP / API credentials
    std::string api_key        = constants::INNERTUBE_API_KEY;
    std::string client_version = constants::INNERTUBE_CLIENT_VERSION;

    // Innertube visitor identity
    std::string visitor_data;    // for context.client.visitorData
    std::string visitor_id;      // 11-char id → VISITOR_INFO1_LIVE cookie

    // Locale (device_info[0], [1])
    std::string hl = "en";
    std::string gl = "US";

    // Network (device_info[3])
    std::string remote_host;

    // Device (device_info[11], [12])
    std::string device_make;
    std::string device_model;

    // Platform (device_info[17], [18])
    std::string os_name        = "Windows";
    std::string os_version     = "10.0";

    // Browser (device_info[86], [87])
    std::string browser_name    = "Chrome";
    std::string browser_version = "130.0.0.0";

    // Timezone (device_info[79])
    std::string time_zone = "America/New_York";

    // Experiment tracking (device_info[103], [107])
    std::string device_experiment_id;
    std::string rollout_token;

    // Config data (device_info[61][last])
    std::string app_install_data;
};

// ---------------------------------------------------------------------------
// Fetch and parse /sw.js_data to populate session_data.
//
// YouTube assigns visitor_data in the response at device_info[13].
// We do NOT pre-generate any visitor ID — we read whatever YouTube gives us.
//
// JSPB response structure (YouTube.js Session.ts #getSessionData):
//   data          = JSON.parse(text.replace /^\)\]\}'//)
//   ytcfg         = data[0][2]
//   [[device_info], api_key] = ytcfg   →   device_info = ytcfg[0][0],
//                                           api_key     = ytcfg[1]
//   device_info indices:
//     [0]  hl, [1]  gl, [3]  remote_host, [11] device_make, [12] device_model,
//     [13] visitor_data, [16] client_version, [17] os_name, [18] os_version,
//     [61] config_info (last elem = app_install_data),
//     [79] time_zone, [86] browser_name, [87] browser_version,
//     [103] device_experiment_id, [107] rollout_token
// ---------------------------------------------------------------------------
session_data fetch_session() {
    using namespace constants;

    session_data sd;

    // GET /sw.js_data — no VISITOR_INFO1_LIVE cookie on the initial request.
    // YouTube will include a visitor_data for this session in device_info[13].
    http::request_options opts;
    opts.method     = "GET";
    opts.timeout_ms = 15000;
    opts.headers = {
        {"user-agent",     USER_AGENT},
        {"accept",         "*/*"},
        {"accept-language","en-US,en;q=0.9"},
        {"referer",        std::string(YT_BASE_URL) + "/sw.js"},
    };

    auto res = http::request(std::string(YT_BASE_URL) + "/sw.js_data", opts);
    const http::response* resp = std::get_if<http::response>(&res);
    if (!resp || !resp->ok()) return sd;  // leave visitor_data empty; caller handles fallback

    // ------------------------------------------------------------------
    // Strip JSPB safety prefix  )]}'\n  or  )]}'\r\n  or  )]}'
    // ------------------------------------------------------------------
    std::string body = resp->body;
    if      (body.starts_with(")]}'\r\n")) body = body.substr(6);
    else if (body.starts_with(")]}'\n"))   body = body.substr(5);
    else if (body.starts_with(")]}"))      body = body.substr(4);

    // ------------------------------------------------------------------
    // Parse JSPB
    // ------------------------------------------------------------------
    try {
        json data = json::parse(body);
        if (!data.is_array() || data.empty()) return sd;

        const json& d0 = data[0];
        if (!d0.is_array() || d0.size() < 3) return sd;

        // ytcfg = d0[2]
        const json& ytcfg = d0[2];
        if (!ytcfg.is_array() || ytcfg.size() < 2) return sd;

        // [[device_info], api_key] = ytcfg
        //   ytcfg[0] = [device_info]  → device_info = ytcfg[0][0]
        //   ytcfg[1] = api_key
        const json& ytcfg0 = ytcfg[0];
        if (!ytcfg0.is_array() || ytcfg0.empty()) return sd;

        const json& device_info = ytcfg0[0];
        if (!device_info.is_array()) return sd;

        if (ytcfg[1].is_string()) {
            std::string k = ytcfg[1].get<std::string>();
            if (!k.empty()) sd.api_key = k;
        }

        auto str_at = [&](size_t idx) -> std::string {
            return (device_info.size() > idx && device_info[idx].is_string())
                ? device_info[idx].get<std::string>() : "";
        };

        // Locale
        if (auto v = str_at(0); !v.empty()) sd.hl = v;
        if (auto v = str_at(1); !v.empty()) sd.gl = v;

        // Network
        sd.remote_host = str_at(3);

        // Device
        sd.device_make  = str_at(11);
        sd.device_model = str_at(12);

        // visitor_data: YouTube-assigned, decode to get the embedded id
        if (auto v = str_at(13); !v.empty()) {
            sd.visitor_data = v;
            sd.visitor_id   = decode_visitor_data_id(v);
        }

        // Client version (WEB-specific)
        if (auto v = str_at(16); !v.empty()) sd.client_version = v;

        // OS
        if (auto v = str_at(17); !v.empty()) sd.os_name    = v;
        if (auto v = str_at(18); !v.empty()) sd.os_version = v;

        // config_info → app_install_data (device_info[61][last])
        if (device_info.size() > 61 && device_info[61].is_array()) {
            const json& ci = device_info[61];
            if (!ci.empty() && ci.back().is_string())
                sd.app_install_data = ci.back().get<std::string>();
        }

        // Timezone
        if (auto v = str_at(79);  !v.empty()) sd.time_zone = v;

        // Browser
        if (auto v = str_at(86);  !v.empty()) sd.browser_name    = v;
        if (auto v = str_at(87);  !v.empty()) sd.browser_version = v;

        // Experiment tracking
        sd.device_experiment_id = str_at(103);
        sd.rollout_token        = str_at(107);

    } catch (...) {}

    return sd;
}

// ---------------------------------------------------------------------------
// Build full Innertube WEB context — mirrors Session.ts #buildContext().
// ---------------------------------------------------------------------------
json build_context(const session_data& sd) {
    // utcOffsetMinutes: -Math.floor(new Date().getTimezoneOffset())
    // getTimezoneOffset() returns (UTC - local) in minutes; negate for (local - UTC).
    // We default to 0 (UTC) since computing the system offset from a string tz is
    // non-trivial; callers on real hardware will get the server's TZ anyway.
    const int utc_offset_minutes = 0;

    json context = {
        {"client", {
            {"hl",                    sd.hl.empty()             ? "en"              : sd.hl},
            {"gl",                    sd.gl.empty()             ? "US"              : sd.gl},
            {"visitorData",           sd.visitor_data},
            {"userAgent",             constants::USER_AGENT},
            {"clientName",            constants::INNERTUBE_CLIENT_NAME},
            {"clientVersion",         sd.client_version},
            {"osName",                sd.os_name.empty()        ? "Windows"         : sd.os_name},
            {"osVersion",             sd.os_version.empty()     ? "10.0"            : sd.os_version},
            {"platform",              "DESKTOP"},
            {"clientFormFactor",      "UNKNOWN_FORM_FACTOR"},
            {"userInterfaceTheme",    "USER_INTERFACE_THEME_LIGHT"},
            {"browserName",           sd.browser_name.empty()   ? "Chrome"          : sd.browser_name},
            {"browserVersion",        sd.browser_version.empty()? "130.0.0.0"       : sd.browser_version},
            {"screenDensityFloat",    1},
            {"screenPixelDensity",    1},
            {"screenHeightPoints",    constants::SCREEN_HEIGHT},
            {"screenWidthPoints",     constants::SCREEN_WIDTH},
            {"utcOffsetMinutes",      utc_offset_minutes},
            {"timeZone",              sd.time_zone.empty()      ? "America/New_York" : sd.time_zone},
            {"memoryTotalKbytes",     "8000000"},
            {"originalUrl",           constants::YT_BASE_URL},
            {"mainAppWebInfo", {
                {"graftUrl",                    constants::YT_BASE_URL},
                {"pwaInstallabilityStatus",     "PWA_INSTALLABILITY_STATUS_UNKNOWN"},
                {"webDisplayMode",              "WEB_DISPLAY_MODE_BROWSER"},
                {"isWebNativeShareAvailable",   true}
            }}
        }},
        {"user", {
            {"enableSafetyMode", false},
            {"lockedSafetyMode", false}
        }},
        {"request", {
            {"useSsl", true},
            {"internalExperimentFlags", json::array()}
        }}
    };

    // Optional fields — only include when present
    if (!sd.remote_host.empty())
        context["client"]["remoteHost"] = sd.remote_host;
    if (!sd.device_make.empty())
        context["client"]["deviceMake"] = sd.device_make;
    if (!sd.device_model.empty())
        context["client"]["deviceModel"] = sd.device_model;
    if (!sd.rollout_token.empty())
        context["client"]["rolloutToken"] = sd.rollout_token;
    if (!sd.device_experiment_id.empty())
        context["client"]["deviceExperimentId"] = sd.device_experiment_id;
    if (!sd.app_install_data.empty())
        context["client"]["configInfo"] = {{"appInstallData", sd.app_install_data}};

    return context;
}

// ---------------------------------------------------------------------------
// Common Innertube request headers (mirrors HTTPClient.ts #setupCommonHeaders)
// ---------------------------------------------------------------------------
http::request_options innertube_post_opts(
        const session_data& sd, const std::string& body_str) {
    http::request_options opts;
    opts.method = "POST";
    opts.body   = body_str;
    opts.headers = {
        // Content negotiation
        {"content-type",             "application/json"},
        {"accept",                   "*/*"},
        {"accept-language",          sd.hl.empty() ? "en-US,en;q=0.9"
                                                   : sd.hl + "," + sd.hl.substr(0,2) + ";q=0.9"},
        // Innertube client identification (HTTPClient.ts #setupCommonHeaders)
        {"x-youtube-client-name",    constants::INNERTUBE_CLIENT_NAME_ID},
        {"x-youtube-client-version", sd.client_version},
        {"x-goog-visitor-id",        sd.visitor_data},
        // Server-side context (set on non-browser / server environments)
        {"user-agent",               constants::USER_AGENT},
        {"origin",                   constants::YT_BASE_URL},
        {"referer",                  std::string(constants::YT_BASE_URL) + "/"},
    };
    return opts;
}

} // namespace (anonymous)

// ===========================================================================
// Public API
// ===========================================================================

// ---------------------------------------------------------------------------
// generate_visitor_data — creates a fresh protobuf-encoded visitor_data string.
// Callers can use this as a fallback when /att/get fails and they still need
// a valid visitor_data for the result.
// ---------------------------------------------------------------------------

std::string generate_visitor_data() {
    // Only used as a last-resort fallback when /sw.js_data is unreachable.
    // STATIC_VISITOR_ID is the same constant YouTubei.js uses in this scenario.
    const uint32_t ts = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return encode_visitor_data(constants::STATIC_VISITOR_ID, ts);
}

// ---------------------------------------------------------------------------
// get_attestation_challenge — creates a fresh Innertube WEB session then
// calls /att/get. Mirrors innertube.getAttestationChallenge('ENGAGEMENT_TYPE_UNBOUND').
// Returns both the challenge and the session visitor_data.
// ---------------------------------------------------------------------------

attestation_outcome get_attestation_challenge() {
    // 1. Create a fresh session: fetches /sw.js_data for real api_key,
    //    client_version, device_info, and the YouTube-assigned visitor_data.
    session_data sd = fetch_session();

    // Ensure visitor_data is set (absolute fallback using STATIC_VISITOR_ID).
    if (sd.visitor_data.empty()) {
        const uint32_t ts = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        sd.visitor_data = encode_visitor_data(constants::STATIC_VISITOR_ID, ts);
        sd.visitor_id   = constants::STATIC_VISITOR_ID;
    }

    // 2. Build the Innertube WEB request body
    const json body_json = {
        {"context",        build_context(sd)},
        {"engagementType", "ENGAGEMENT_TYPE_UNBOUND"}
    };
    const std::string body_str = body_json.dump();

    // 3. POST /youtubei/v1/att/get?prettyPrint=false&alt=json
    const std::string url =
        std::string(constants::INNERTUBE_BASE_URL) + "/att/get?prettyPrint=false&alt=json";

    auto opts = innertube_post_opts(sd, body_str);
    auto result = http::request(url, opts);

    if (auto* err = std::get_if<http::http_error>(&result)) {
        return challenge_error{"HTTP error calling /att/get: " + err->message};
    }
    const auto& resp = std::get<http::response>(result);
    if (!resp.ok()) {
        return challenge_error{
            "/att/get returned HTTP " + std::to_string(resp.status) +
            ": " + resp.body.substr(0, 200)
        };
    }

    auto ch = parse_att_response(resp.body);
    if (auto* c = std::get_if<bg_challenge>(&ch)) {
        return attestation_result{*c, sd.visitor_data};
    }
    return std::get<challenge_error>(ch);
}

// ---------------------------------------------------------------------------
// fetch_challenge — legacy jnn-pa Create RPC  (fallback)
// ---------------------------------------------------------------------------

challenge_outcome fetch_challenge() {
    using namespace constants;

    json payload = json::array();
    payload.push_back(REQUEST_KEY);

    http::request_options opts;
    opts.method = "POST";
    opts.body   = payload.dump();
    opts.headers = {
        {"content-type",  "application/json+protobuf"},
        {"x-goog-api-key", GOOG_API_KEY},
        {"x-user-agent",  "grpc-web-javascript/0.1"},
        {"user-agent",    USER_AGENT},
    };

    auto result = http::request(CREATE_ENDPOINT, opts);
    if (auto* err = std::get_if<http::http_error>(&result)) {
        return challenge_error{"HTTP error fetching challenge: " + err->message};
    }
    const auto& resp = std::get<http::response>(result);
    if (!resp.ok()) {
        return challenge_error{
            "Challenge fetch returned HTTP " + std::to_string(resp.status)
        };
    }
    return parse_challenge_response(resp.body);
}

// ---------------------------------------------------------------------------
// post_generate_it — jnn-pa GenerateIT RPC
// ---------------------------------------------------------------------------

std::variant<std::string, challenge_error>
post_generate_it(const std::string& snapshot) {
    using namespace constants;

    json payload = json::array();
    payload.push_back(REQUEST_KEY);
    payload.push_back(snapshot);

    http::request_options opts;
    opts.method = "POST";
    opts.body   = payload.dump();
    opts.headers = {
        {"content-type",  "application/json+protobuf"},
        {"x-goog-api-key", GOOG_API_KEY},
        {"x-user-agent",  "grpc-web-javascript/0.1"},
        {"user-agent",    USER_AGENT},
    };

    auto gen_res = http::request(GENERATE_IT_ENDPOINT, opts);
    if (auto* err = std::get_if<http::http_error>(&gen_res)) {
        return challenge_error{"GenerateIT HTTP error: " + err->message};
    }
    const auto& gen_resp = std::get<http::response>(gen_res);
    if (!gen_resp.ok()) {
        return challenge_error{
            "GenerateIT returned HTTP " + std::to_string(gen_resp.status)
        };
    }

    json gen_json;
    try { gen_json = json::parse(gen_resp.body); }
    catch (...) {
        return challenge_error{"Failed to parse GenerateIT response as JSON"};
    }
    if (!gen_json.is_array() || gen_json.empty() || !gen_json[0].is_string()) {
        return challenge_error{"Could not extract integrity token from GenerateIT response"};
    }
    return gen_json[0].get<std::string>();
}

} // namespace epotoken
