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
// visitor_data:     YouTube visitor data string from an Innertube session.
// content_binding:  What to mint the token for; defaults to visitor_data when empty.
epotoken::po_token_outcome generate_po_token(
    const std::string& visitor_data,
    const std::string& content_binding = ""
);

// Pure-C++ cold-start placeholder token (no BotGuard required).
// identifier must be ≤ 118 UTF-8 bytes.
epotoken::placeholder_outcome generate_placeholder_token(
    const std::string& identifier,
    uint8_t client_state = 1
);
