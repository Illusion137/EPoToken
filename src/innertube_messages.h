#pragma once

// Pure-compute helpers for the YouTube Innertube + jnn-pa wire formats.
// No HTTP, no V8 — usable from native, WASM, and React Native builds.

#include <cstdint>
#include <string>
#include <variant>

#include "challenge.h"  // for challenge_error

namespace epotoken {

// ---------------------------------------------------------------------------
// VisitorData (protobuf-encoded { id: string, timestamp: uint32 }).
// ---------------------------------------------------------------------------

// Builds a visitor_data string with a caller-supplied 11-char id and timestamp.
// Mirrors ProtoUtils.encodeVisitorData() in YouTubei.js.
std::string encode_visitor_data(const std::string& id, uint32_t timestamp);

// Decodes a visitor_data string and returns the embedded id (field 1) — or
// empty string if the input is malformed.
std::string decode_visitor_data_id(const std::string& visitor_data);

// Generates a visitor_data using a constant id (STATIC_VISITOR_ID) + the
// current timestamp. Used as the last-resort fallback inside the native
// /att/get flow when /sw.js_data is unreachable.
std::string generate_visitor_data();

// Generates a fresh visitor_data using a random id + the current timestamp.
// Preferred for JS hosts (WASM, React Native) that drive the flow themselves
// and want a unique visitor per session.
std::string generate_random_visitor_data();

// ---------------------------------------------------------------------------
// Optional overrides for the Innertube WEB context.
// All fields default to baked-in values from constants.h when empty.
// ---------------------------------------------------------------------------

struct innertube_context_overrides {
    std::string visitor_data;     // required for /att/get (use generate_visitor_data() if none)
    std::string client_version;   // default: constants::INNERTUBE_CLIENT_VERSION
    std::string hl;               // default: "en"
    std::string gl;               // default: "US"
    std::string user_agent;       // default: constants::USER_AGENT
    std::string os_name;          // default: "Windows"
    std::string os_version;       // default: "10.0"
    std::string browser_name;     // default: "Chrome"
    std::string browser_version;  // default: "130.0.0.0"
    std::string time_zone;        // default: "America/New_York"
    int         utc_offset_minutes = 0;
};

// ---------------------------------------------------------------------------
// Request body builders (return JSON-encoded strings).
// ---------------------------------------------------------------------------

// Builds the body for POST /youtubei/v1/att/get?prettyPrint=false&alt=json
// using the supplied overrides + the Innertube WEB context defaults.
std::string build_att_get_body(const innertube_context_overrides& opts);

// Builds the body for the jnn-pa GenerateIT RPC:
//   [REQUEST_KEY, snapshot]
std::string build_generate_it_body(const std::string& snapshot);

// Builds the body for the legacy jnn-pa Create RPC fallback:
//   [REQUEST_KEY]
std::string build_challenge_create_body();

// ---------------------------------------------------------------------------
// Response parsers.
// ---------------------------------------------------------------------------

// Parses a jnn-pa GenerateIT response. Returns the integrity token on success.
// Expected shape: ["integrity-token-string", ...]
std::variant<std::string, challenge_error>
parse_generate_it_response(const std::string& json_body);

} // namespace epotoken
