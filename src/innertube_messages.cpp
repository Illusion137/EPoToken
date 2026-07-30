// Pure-compute Innertube + jnn-pa wire-format helpers. No HTTP, no V8.

#include "innertube_messages.h"

#include "base64.h"
#include "constants.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <random>

namespace epotoken {

using json = nlohmann::json;

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

std::string random_visitor_id() {
    // 11-character base64url-style id, matching the format Innertube hands out
    // in device_info[13]. Used only when sw.js_data is unreachable.
    static constexpr const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(0, 63);
    std::string out;
    out.reserve(11);
    for (int i = 0; i < 11; ++i) out.push_back(alphabet[dist(rng)]);
    return out;
}

const std::string& or_default(const std::string& s, const char* fallback) {
    static thread_local std::string scratch;
    if (!s.empty()) return s;
    scratch = fallback;
    return scratch;
}

} // namespace

// ---------------------------------------------------------------------------
// VisitorData
// ---------------------------------------------------------------------------

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

std::string decode_visitor_data_id(const std::string& visitor_data) {
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
        uint8_t  tag       = bytes[pos++];
        uint8_t  field_num = tag >> 3;
        uint8_t  wire_type = tag & 0x07;

        if (wire_type == 2) {
            uint64_t len = read_varint(bytes, pos);
            if (field_num == 1 && pos + len <= bytes.size()) {
                return std::string(
                    bytes.begin() + static_cast<ptrdiff_t>(pos),
                    bytes.begin() + static_cast<ptrdiff_t>(pos + len));
            }
            pos += static_cast<size_t>(len);
        } else if (wire_type == 0) {
            read_varint(bytes, pos);
        } else if (wire_type == 5) {
            pos += 4;
        } else if (wire_type == 1) {
            pos += 8;
        } else {
            break;
        }
    }
    return "";
}

std::string generate_visitor_data() {
    // Last-resort fallback when /sw.js_data is unreachable. STATIC_VISITOR_ID
    // mirrors YouTubei.js behavior so traffic patterns line up.
    const uint32_t ts = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return encode_visitor_data(constants::STATIC_VISITOR_ID, ts);
}

std::string generate_random_visitor_data() {
    // For JS hosts (WASM, RN) that need a fresh, unique visitor_data per
    // session without first calling /sw.js_data.
    const uint32_t ts = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return encode_visitor_data(random_visitor_id(), ts);
}

// ---------------------------------------------------------------------------
// Request bodies
// ---------------------------------------------------------------------------

std::string build_att_get_body(const innertube_context_overrides& opts) {
    const std::string visitor_data =
        opts.visitor_data.empty() ? generate_visitor_data() : opts.visitor_data;

    const std::string hl   = or_default(opts.hl,              "en");
    const std::string gl   = or_default(opts.gl,              "US");
    const std::string ua   = or_default(opts.user_agent,      constants::USER_AGENT);
    const std::string cv   = or_default(opts.client_version,  constants::INNERTUBE_CLIENT_VERSION);
    const std::string os_n = or_default(opts.os_name,         "Windows");
    const std::string os_v = or_default(opts.os_version,      "10.0");
    const std::string br_n = or_default(opts.browser_name,    "Chrome");
    const std::string br_v = or_default(opts.browser_version, "130.0.0.0");
    const std::string tz   = or_default(opts.time_zone,       "America/New_York");

    json context = {
        {"client", {
            {"hl",                    hl},
            {"gl",                    gl},
            {"visitorData",           visitor_data},
            {"userAgent",             ua},
            {"clientName",            constants::INNERTUBE_CLIENT_NAME},
            {"clientVersion",         cv},
            {"osName",                os_n},
            {"osVersion",             os_v},
            {"platform",              "DESKTOP"},
            {"clientFormFactor",      "UNKNOWN_FORM_FACTOR"},
            {"userInterfaceTheme",    "USER_INTERFACE_THEME_LIGHT"},
            {"browserName",           br_n},
            {"browserVersion",        br_v},
            {"screenDensityFloat",    1},
            {"screenPixelDensity",    1},
            {"screenHeightPoints",    constants::SCREEN_HEIGHT},
            {"screenWidthPoints",     constants::SCREEN_WIDTH},
            {"utcOffsetMinutes",      opts.utc_offset_minutes},
            {"timeZone",              tz},
            {"memoryTotalKbytes",     "8000000"},
            {"originalUrl",           constants::YT_BASE_URL},
            {"mainAppWebInfo", {
                {"graftUrl",                  constants::YT_BASE_URL},
                {"pwaInstallabilityStatus",   "PWA_INSTALLABILITY_STATUS_UNKNOWN"},
                {"webDisplayMode",            "WEB_DISPLAY_MODE_BROWSER"},
                {"isWebNativeShareAvailable", true}
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

    json body = {
        {"context",        context},
        {"engagementType", "ENGAGEMENT_TYPE_UNBOUND"}
    };
    return body.dump();
}

std::string build_generate_it_body(const std::string& snapshot) {
    json payload = json::array();
    payload.push_back(constants::REQUEST_KEY);
    payload.push_back(snapshot);
    return payload.dump();
}

std::string build_challenge_create_body() {
    json payload = json::array();
    payload.push_back(constants::REQUEST_KEY);
    return payload.dump();
}

// ---------------------------------------------------------------------------
// Response parsers
// ---------------------------------------------------------------------------

std::variant<std::string, challenge_error>
parse_generate_it_response(const std::string& json_body) {
    json parsed;
    try { parsed = json::parse(json_body); }
    catch (...) {
        return challenge_error{"Failed to parse GenerateIT response as JSON"};
    }
    if (!parsed.is_array() || parsed.empty() || !parsed[0].is_string()) {
        return challenge_error{
            "Could not extract integrity token from GenerateIT response (snapshot rejected): "
            + json_body
        };
    }
    return parsed[0].get<std::string>();
}

} // namespace epotoken
