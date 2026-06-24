#pragma once

#include <string>
#include <variant>

namespace epotoken {

struct bg_challenge {
    std::string interpreter_url;  // trusted resource URL (https: prepended if //-relative)
    std::string program;
    std::string global_name;
};

struct challenge_error {
    std::string message;
};

using challenge_outcome = std::variant<bg_challenge, challenge_error>;

// Fetches and parses the BotGuard challenge from jnn-pa.googleapis.com.
// Mirrors Challenge.create() + parseChallengeData() from bgutils-js.
challenge_outcome fetch_challenge(const std::string& visitor_data = "");

// Parses a raw JSON response body (already fetched).
challenge_outcome parse_challenge_response(const std::string& json_body);

} // namespace epotoken
