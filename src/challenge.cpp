#include "challenge.h"

#include "base64.h"
#include "constants.h"
#include "http_client.h"

#include <nlohmann/json.hpp>

namespace epotoken {

using json = nlohmann::json;

namespace {

// Mirrors bgutils descramble()/B(): base64url-decode then add 97 (mod 256) per byte.
// The encoding scheme subtracts 97 from each char before base64-encoding.
std::string descramble(const std::string& encoded) {
    std::vector<uint8_t> bytes = base64::decode(encoded);
    for (auto& b : bytes) {
        b = static_cast<uint8_t>((static_cast<int>(b) + 97) & 0xFF);
    }
    return std::string(bytes.begin(), bytes.end());
}

// Mirrors bgutils parseChallengeData()/C().
challenge_outcome parse_inner(const json& outer) {
    json inner_arr = json::array();

    if (outer.size() > 1 && outer[1].is_string()) {
        const std::string raw = outer[1].get<std::string>();
        const std::string descrambled = descramble(raw);
        try {
            inner_arr = json::parse(descrambled);
        } catch (...) {
            return challenge_error{"Failed to parse descrambled challenge JSON"};
        }
    } else if (!outer.empty() && outer[0].is_array()) {
        inner_arr = outer[0];
    } else {
        return challenge_error{"Unexpected challenge response format"};
    }

    if (!inner_arr.is_array() || inner_arr.size() < 6) {
        return challenge_error{"Challenge inner array too short"};
    }

    // Layout: [messageId, safeScriptArr, trustedUrlArr, interpreterHash, program, globalName, ?, blob]
    // e = inner_arr[1], r = inner_arr[2], program = inner_arr[4], globalName = inner_arr[5]
    const auto& e_val = inner_arr[1]; // array containing safe script URL strings
    const auto& r_val = inner_arr[2]; // array containing trusted resource URL strings

    std::string interpreter_url;
    if (r_val.is_array()) {
        for (const auto& item : r_val) {
            if (item.is_string() && !item.get<std::string>().empty()) {
                interpreter_url = item.get<std::string>();
                break;
            }
        }
    }

    if (interpreter_url.empty()) {
        // Fallback: try the safe script array
        if (e_val.is_array()) {
            for (const auto& item : e_val) {
                if (item.is_string() && !item.get<std::string>().empty()) {
                    interpreter_url = item.get<std::string>();
                    break;
                }
            }
        }
    }

    if (interpreter_url.empty()) {
        return challenge_error{"Could not extract interpreter URL from challenge"};
    }

    // Protocol-relative URL → prepend https:
    if (interpreter_url.size() >= 2 && interpreter_url[0] == '/' && interpreter_url[1] == '/') {
        interpreter_url = "https:" + interpreter_url;
    }

    if (!inner_arr[4].is_string()) {
        return challenge_error{"Challenge program field is not a string"};
    }
    if (!inner_arr[5].is_string()) {
        return challenge_error{"Challenge globalName field is not a string"};
    }

    return bg_challenge{
        interpreter_url,
        inner_arr[4].get<std::string>(),
        inner_arr[5].get<std::string>(),
    };
}

} // namespace

challenge_outcome parse_challenge_response(const std::string& json_body) {
    json outer;
    try {
        outer = json::parse(json_body);
    } catch (...) {
        return challenge_error{"Failed to parse challenge HTTP response as JSON"};
    }

    if (!outer.is_array() || outer.empty()) {
        return challenge_error{"Challenge response is not a JSON array"};
    }

    return parse_inner(outer);
}

challenge_outcome fetch_challenge(const std::string& visitor_data) {
    using namespace constants;

    json payload = json::array();
    payload.push_back(REQUEST_KEY);
    if (!visitor_data.empty()) {
        payload.push_back(visitor_data);
    }

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

} // namespace epotoken
