#pragma once

#include "challenge.h"

#include <string>
#include <variant>

namespace epotoken {

// Mirrors YouTubei.js Session + innertube.getAttestationChallenge().
// Fetches /sw.js_data for real api_key/client_version, builds the full
// Innertube context, and POSTs to /youtubei/v1/att/get.
challenge_outcome get_attestation_challenge(const std::string& visitor_data);

// Fallback: old bgutils-js path — POSTs to jnn-pa.googleapis.com/Create.
// Used when the Innertube /att/get endpoint is unavailable.
challenge_outcome fetch_challenge(const std::string& visitor_data = "");

// POSTs the BotGuard snapshot to jnn-pa GenerateIT; returns the integrity token.
std::variant<std::string, challenge_error> post_generate_it(const std::string& snapshot);

} // namespace epotoken
