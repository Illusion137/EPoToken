#include "innertube_client.h"

#include "constants.h"
#include "http_client.h"

#include <nlohmann/json.hpp>
#include <random>
#include <string>

namespace epotoken {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Session data (mirrors YouTubei.js Session.ts)
// ---------------------------------------------------------------------------

namespace {

struct session_data {
    std::string api_key        = constants::INNERTUBE_API_KEY;
    std::string client_version = constants::INNERTUBE_CLIENT_VERSION;
    std::string visitor_data;
};

// Random 11-char cookie value matching YouTube's VISITOR_INFO1_LIVE format.
std::string random_visitor_cookie() {
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(0, 63);
    std::string id;
    id.reserve(11);
    for (int i = 0; i < 11; ++i) id += chars[dist(rng)];
    return id;
}

// Fetches /sw.js_data to extract real api_key, client_version, visitor_data.
// Any failure silently returns hardcoded defaults — the caller always has a
// usable session_data.
session_data fetch_session(const std::string& visitor_data) {
    session_data sd;
    sd.visitor_data = visitor_data;

    http::request_options opts;
    opts.method     = "GET";
    opts.timeout_ms = 10000;
    opts.headers = {
        {"user-agent",     constants::USER_AGENT},
        {"accept-language", "en-US,en;q=0.9"},
        {"cookie",         "VISITOR_INFO1_LIVE=" + random_visitor_cookie() +
                           "; CONSENT=YES+1"},
    };

    auto res = http::request(
        std::string(constants::YT_BASE_URL) + "/sw.js_data", opts);

    if (std::get_if<http::http_error>(&res)) return sd;
    const auto& resp = std::get<http::response>(res);
    if (!resp.ok()) return sd;

    // Strip JSPB safety prefix )]}'\n
    std::string body = resp.body;
    if (body.starts_with(")]}'\n"))      body = body.substr(5);
    else if (body.starts_with(")]}'\r\n")) body = body.substr(6);
    else if (body.starts_with(")]}"))    body = body.substr(4);

    try {
        // data[0][2] = device_info array per YouTubei.js Session.ts
        json data = json::parse(body);
        if (!data.is_array() || data.empty()) return sd;
        const auto& d0 = data[0];
        if (!d0.is_array() || d0.size() < 3) return sd;
        const auto& device_info = d0[2];
        if (!device_info.is_array()) return sd;

        // [13] = visitor_data (only override if caller didn't supply one)
        if (visitor_data.empty() &&
            device_info.size() > 13 && device_info[13].is_string()) {
            sd.visitor_data = device_info[13].get<std::string>();
        }
        // [16] = client_version
        if (device_info.size() > 16 && device_info[16].is_string()) {
            const auto v = device_info[16].get<std::string>();
            if (!v.empty()) sd.client_version = v;
        }
        // API key sometimes at d0[1]
        if (d0.size() > 1 && d0[1].is_string()) {
            const auto k = d0[1].get<std::string>();
            if (!k.empty()) sd.api_key = k;
        }
    } catch (...) {}

    return sd;
}

// Mirrors YouTubei.js Session.buildContext() for the WEB client.
json build_context(const session_data& sd) {
    return json{
        {"client", {
            {"hl", "en"},
            {"gl", "US"},
            {"clientName", "WEB"},
            {"clientVersion", sd.client_version},
            {"visitorData", sd.visitor_data},
            {"userAgent", constants::USER_AGENT},
            {"platform", "DESKTOP"},
            {"clientFormFactor", "UNKNOWN_FORM_FACTOR"},
            {"osName", "Windows"},
            {"osVersion", "10.0"},
            {"browserName", "Chrome"},
            {"browserVersion", "130.0.0.0"},
            {"screenDensityFloat", 1},
            {"screenHeightPoints", 1440},
            {"screenWidthPoints", 2560},
            {"utcOffsetMinutes", 0},
            {"timeZone", "America/New_York"},
            {"memoryTotalKbytes", "8000000"},
            {"mainAppWebInfo", {
                {"graftUrl", "/"},
                {"pwaInstallabilityStatus", "PWA_INSTALLABILITY_STATUS_UNKNOWN"},
                {"webDisplayMode", "WEB_DISPLAY_MODE_BROWSER"},
                {"isWebNativeShareAvailable", false}
            }}
        }},
        {"user", {{"lockedSafetyMode", false}}},
        {"request", {
            {"useSsl", true},
            {"internalExperimentFlags", json::array()},
            {"consistencyTokenJars", json::array()}
        }}
    };
}

} // namespace

// ---------------------------------------------------------------------------
// get_attestation_challenge — Innertube /att/get  (primary path)
// ---------------------------------------------------------------------------

challenge_outcome get_attestation_challenge(const std::string& visitor_data) {
    using namespace constants;

    session_data sd = fetch_session(visitor_data);
    if (sd.visitor_data.empty()) sd.visitor_data = visitor_data;

    const json body_json = {
        {"context", build_context(sd)},
        {"engagementType", "ENGAGEMENT_TYPE_UNBOUND"}
    };

    const std::string url =
        std::string(YT_BASE_URL) + "/youtubei/v1/att/get?prettyPrint=false";

    http::request_options opts;
    opts.method = "POST";
    opts.body   = body_json.dump();
    opts.headers = {
        {"content-type",             "application/json"},
        {"x-youtube-client-name",    "1"},
        {"x-youtube-client-version", sd.client_version},
        {"x-goog-api-key",           sd.api_key},
        {"x-goog-visitor-id",        sd.visitor_data},
        {"user-agent",               USER_AGENT},
        {"origin",                   "https://www.youtube.com"},
        {"referer",                  "https://www.youtube.com/"},
        {"accept-language",          "en-US,en;q=0.9"},
    };

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

    return parse_att_response(resp.body);
}

// ---------------------------------------------------------------------------
// fetch_challenge — old jnn-pa Create RPC  (fallback)
// ---------------------------------------------------------------------------

challenge_outcome fetch_challenge(const std::string& visitor_data) {
    using namespace constants;

    json payload = json::array();
    payload.push_back(REQUEST_KEY);
    if (!visitor_data.empty()) payload.push_back(visitor_data);

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

    json generate_payload = json::array();
    generate_payload.push_back(REQUEST_KEY);
    generate_payload.push_back(snapshot);

    http::request_options gen_opts;
    gen_opts.method = "POST";
    gen_opts.body   = generate_payload.dump();
    gen_opts.headers = {
        {"content-type",  "application/json+protobuf"},
        {"x-goog-api-key", GOOG_API_KEY},
        {"x-user-agent",  "grpc-web-javascript/0.1"},
        {"user-agent",    USER_AGENT},
    };

    auto gen_res = http::request(GENERATE_IT_ENDPOINT, gen_opts);
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
    try { gen_json = nlohmann::json::parse(gen_resp.body); }
    catch (...) {
        return challenge_error{"Failed to parse GenerateIT response as JSON"};
    }

    if (!gen_json.is_array() || gen_json.empty() || !gen_json[0].is_string()) {
        return challenge_error{"Could not extract integrity token from GenerateIT response"};
    }
    return gen_json[0].get<std::string>();
}

} // namespace epotoken
