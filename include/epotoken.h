#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace epotoken {

struct po_token_result {
    std::string po_token;
    std::string placeholder_po_token;
    std::string visitor_data;
    std::string identifier;
};

struct error {
    std::string message;
    std::string code;
};

using po_token_outcome     = std::variant<po_token_result, error>;
using placeholder_outcome  = std::variant<std::string, error>;

} // namespace epotoken

// Generates a full PoToken via BotGuard/V8.
// Creates a fresh Innertube WEB session internally; no visitor_data required.
// content_binding: what to mint the token for (e.g. video ID or channel ID).
//   Defaults to the session's visitor_data when omitted.
epotoken::po_token_outcome generate_po_token(
    const std::string& content_binding = ""
);

// Overload: same as above but uses a caller-supplied interpreter URL,
// bypassing the URL discovered in the /att/get challenge response. The
// program / global_name / visitor_data still come from /att/get. Useful when
// the interpreter JS is served from a cache, mirror, or pinned commit, and
// for environments (WASM, React Native) where reducing round-trips matters.
// Pass interpreter_url empty to fall back to the default discovery path.
epotoken::po_token_outcome generate_po_token(
    const std::string& content_binding,
    const std::string& interpreter_url
);

// Pure-C++ cold-start placeholder token (no BotGuard required).
// identifier must be ≤ 118 UTF-8 bytes.
epotoken::placeholder_outcome generate_placeholder_token(
    const std::string& identifier,
    uint8_t client_state = 1
);
