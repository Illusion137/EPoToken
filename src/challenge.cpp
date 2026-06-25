#include "challenge.h"

#include "base64.h"

#include <nlohmann/json.hpp>

namespace epotoken {

using json = nlohmann::json;

namespace {

// Mirrors bgutils descramble()/B(): base64url-decode then add 97 (mod 256) per byte.
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
    const auto& e_val = inner_arr[1]; // safe script URL strings
    const auto& r_val = inner_arr[2]; // trusted resource URL strings

    std::string interpreter_url;
    if (r_val.is_array()) {
        for (const auto& item : r_val) {
            if (item.is_string() && !item.get<std::string>().empty()) {
                interpreter_url = item.get<std::string>();
                break;
            }
        }
    }
    if (interpreter_url.empty() && e_val.is_array()) {
        for (const auto& item : e_val) {
            if (item.is_string() && !item.get<std::string>().empty()) {
                interpreter_url = item.get<std::string>();
                break;
            }
        }
    }
    if (interpreter_url.empty()) {
        return challenge_error{"Could not extract interpreter URL from challenge"};
    }

    if (interpreter_url.size() >= 2 &&
        interpreter_url[0] == '/' && interpreter_url[1] == '/') {
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
    try { outer = json::parse(json_body); }
    catch (...) {
        return challenge_error{"Failed to parse challenge HTTP response as JSON"};
    }
    if (!outer.is_array() || outer.empty()) {
        return challenge_error{"Challenge response is not a JSON array"};
    }
    return parse_inner(outer);
}

challenge_outcome parse_att_response(const std::string& json_body) {
    json resp;
    try { resp = json::parse(json_body); }
    catch (...) {
        return challenge_error{"Failed to parse /att/get response as JSON"};
    }

    if (!resp.is_object()) {
        return challenge_error{"/att/get response is not a JSON object"};
    }

    auto it = resp.find("bg_challenge");
    if (it == resp.end() || !it->is_object()) {
        return challenge_error{
            "No bg_challenge in /att/get response: " + json_body.substr(0, 120)
        };
    }

    const json& bgc = *it;

    auto get_str = [&](const char* key) -> std::string {
        auto f = bgc.find(key);
        return (f != bgc.end() && f->is_string()) ? f->get<std::string>() : "";
    };

    std::string interpreter_url = get_str("interpreter_url");
    if (interpreter_url.empty()) {
        return challenge_error{"bg_challenge.interpreter_url is missing or empty"};
    }
    if (interpreter_url.size() >= 2 &&
        interpreter_url[0] == '/' && interpreter_url[1] == '/') {
        interpreter_url = "https:" + interpreter_url;
    }

    const std::string program     = get_str("program");
    const std::string global_name = get_str("global_name");
    if (program.empty() || global_name.empty()) {
        return challenge_error{"bg_challenge missing program or global_name"};
    }

    return bg_challenge{interpreter_url, program, global_name};
}

} // namespace epotoken
