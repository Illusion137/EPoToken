#pragma once

#include "challenge.h"

#include <string>
#include <variant>

namespace epotoken {

// Bundles the BotGuard challenge with the visitor_data from the fresh session
// created during the /att/get call — both are needed by the caller.
struct attestation_result {
    bg_challenge challenge;
    std::string  visitor_data;
};
using attestation_outcome = std::variant<attestation_result, challenge_error>;

// Creates a fresh Innertube WEB session (fetches /sw.js_data) then POSTs to
// /att/get. Returns the challenge together with the session's visitor_data.
attestation_outcome get_attestation_challenge();

// Fallback: jnn-pa Create RPC, used when /att/get fails.
challenge_outcome fetch_challenge();

// Generates a fresh protobuf-encoded visitor_data (random 11-char id + now).
std::string generate_visitor_data();

// POSTs the BotGuard snapshot to jnn-pa GenerateIT; returns the integrity token.
std::variant<std::string, challenge_error> post_generate_it(const std::string& snapshot);

} // namespace epotoken
