#pragma once

#include <string>
#include <variant>

// Internal nsig helpers. The public API (decipher_nsig) lives in epotoken.h.
namespace epotoken {

struct nsig_error { std::string message; };

// Discovers the current player id from the iframe API.
std::variant<std::string, nsig_error> discover_player_id();

// Fetches base.js for a player id.
std::variant<std::string, nsig_error> fetch_player_js(const std::string& player_id);

} // namespace epotoken
