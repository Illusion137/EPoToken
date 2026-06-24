#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace epotoken::base64 {

// Standard base64 encode. If url_safe=true, uses - _ instead of + /
// and omits = padding (websafe variant used by bgutils).
std::string encode(const std::vector<uint8_t>& data, bool url_safe = false);

// Decodes both standard and URL-safe base64 (handles - _ . as in bgutils base64ToU8).
std::vector<uint8_t> decode(const std::string& input);

} // namespace epotoken::base64
