#pragma once

#include <string>
#include <variant>

namespace epotoken {

struct bg_challenge {
    std::string interpreter_url;  // https:-prefixed
    std::string program;
    std::string global_name;
};

struct challenge_error {
    std::string message;
};

using challenge_outcome = std::variant<bg_challenge, challenge_error>;

// Parses the jnn-pa Create RPC JSON response (bgutils-js format).
// Outer array where [1] is base64url-scrambled inner array.
challenge_outcome parse_challenge_response(const std::string& json_body);

// Parses the Innertube /att/get IGetChallengeResponse JSON object.
// Expects {"bg_challenge":{"interpreter_url":"...","program":"...","global_name":"..."}}.
challenge_outcome parse_att_response(const std::string& json_body);

} // namespace epotoken
