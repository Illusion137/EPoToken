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

// Returns true if the candidate URL is absolute (http/https/protocol-relative).
bool is_absolute_url(const std::string& url) {
    if (url.size() >= 8 && url.substr(0, 8) == "https://") return true;
    if (url.size() >= 7 && url.substr(0, 7) == "http://")  return true;
    if (url.size() >= 2 && url[0] == '/' && url[1] == '/') return true;
    return false;
}

// Normalise a protocol-relative URL to https:.
std::string normalise_url(std::string url) {
    if (url.size() >= 2 && url[0] == '/' && url[1] == '/')
        url = "https:" + url;
    return url;
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

    // Collect the first absolute URL from the trusted-resource array,
    // then fall back to the safe-script array.  Relative paths are skipped
    // because libcurl cannot fetch them.
    std::string interpreter_url;
    auto pick_url = [&](const json& arr) {
        if (!arr.is_array()) return;
        for (const auto& item : arr) {
            if (!item.is_string()) continue;
            const std::string s = item.get<std::string>();
            if (is_absolute_url(s)) { interpreter_url = normalise_url(s); return; }
        }
    };
    pick_url(r_val);
    if (interpreter_url.empty()) pick_url(e_val);

    if (interpreter_url.empty()) {
        return challenge_error{"Could not extract interpreter URL from challenge"};
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

    // YouTube Innertube JSON API uses camelCase; some mirrors use snake_case.
    auto find_obj = [&](const char* a, const char* b) -> const json* {
        auto it = resp.find(a);
        if (it != resp.end() && it->is_object()) return &*it;
        it = resp.find(b);
        if (it != resp.end() && it->is_object()) return &*it;
        return nullptr;
    };

    const json* bgc_ptr = find_obj("bgChallenge", "bg_challenge");
    if (!bgc_ptr) {
        return challenge_error{
            "No bgChallenge/bg_challenge in /att/get response: " +
            json_body.substr(0, 200)
        };
    }
    const json& bgc = *bgc_ptr;

    // Try camelCase first, fall back to snake_case.
    auto get_str = [&](const char* camel, const char* snake) -> std::string {
        auto f = bgc.find(camel);
        if (f != bgc.end() && f->is_string()) return f->get<std::string>();
        f = bgc.find(snake);
        if (f != bgc.end() && f->is_string()) return f->get<std::string>();
        return "";
    };

    std::string interpreter_url = get_str("interpreterUrl", "interpreter_url");
    if (interpreter_url.empty()) {
        return challenge_error{
            "bgChallenge.interpreterUrl is missing or empty in: " +
            bgc.dump().substr(0, 200)
        };
    }
    interpreter_url = normalise_url(interpreter_url);
    if (!is_absolute_url(interpreter_url)) {
        return challenge_error{
            "bgChallenge.interpreterUrl is not an absolute URL: " + interpreter_url
        };
    }

    const std::string program     = get_str("program",    "program");
    const std::string global_name = get_str("globalName", "global_name");
    if (program.empty() || global_name.empty()) {
        return challenge_error{
            "bgChallenge missing program or globalName: " +
            bgc.dump().substr(0, 200)
        };
    }

    return bg_challenge{interpreter_url, program, global_name};
}

} // namespace epotoken
