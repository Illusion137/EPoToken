#include "base64.h"

namespace epotoken::base64 {

static constexpr const char* STANDARD_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(const std::vector<uint8_t>& data, bool url_safe) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    const size_t n = data.size();

    while (i < n) {
        uint32_t triple = static_cast<uint32_t>(data[i++]) << 16;
        if (i < n) triple |= static_cast<uint32_t>(data[i++]) << 8;
        if (i < n) triple |= static_cast<uint32_t>(data[i++]);

        out += STANDARD_CHARS[(triple >> 18) & 0x3F];
        out += STANDARD_CHARS[(triple >> 12) & 0x3F];
        out += STANDARD_CHARS[(triple >>  6) & 0x3F];
        out += STANDARD_CHARS[(triple >>  0) & 0x3F];
    }

    // Fix padding for partial groups
    const size_t remainder = data.size() % 3;
    if (remainder == 1) {
        out[out.size() - 1] = '=';
        out[out.size() - 2] = '=';
    } else if (remainder == 2) {
        out[out.size() - 1] = '=';
    }

    if (url_safe) {
        for (char& c : out) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        // Strip trailing '=' padding for websafe variant (bgutils u8ToBase64 with base64url=true
        // replaces + and / but keeps '=' — but YouTube's websafe variant strips it; match bgutils).
        while (!out.empty() && out.back() == '=') out.pop_back();
    }

    return out;
}

std::vector<uint8_t> decode(const std::string& input) {
    // Normalise URL-safe chars and padding dots (.→=, -→+, _→/) matching bgutils base64ToU8.
    std::string s;
    s.reserve(input.size());
    for (char c : input) {
        if      (c == '-') s += '+';
        else if (c == '_') s += '/';
        else if (c == '.') s += '=';
        else               s += c;
    }

    // Pad to multiple of 4
    while (s.size() % 4 != 0) s += '=';

    // Build decoding table
    static constexpr int8_t DEC[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);

    for (size_t i = 0; i < s.size(); i += 4) {
        const int8_t a = DEC[static_cast<uint8_t>(s[i])];
        const int8_t b = DEC[static_cast<uint8_t>(s[i+1])];
        const int8_t c = DEC[static_cast<uint8_t>(s[i+2])];
        const int8_t d = DEC[static_cast<uint8_t>(s[i+3])];

        if (a < 0 || b < 0) break; // invalid input

        const uint32_t triple =
            (static_cast<uint32_t>(a) << 18) |
            (static_cast<uint32_t>(b) << 12) |
            (c >= 0 ? static_cast<uint32_t>(c) << 6 : 0u) |
            (d >= 0 ? static_cast<uint32_t>(d)       : 0u);

        out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
        if (s[i+2] != '=') out.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
        if (s[i+3] != '=') out.push_back(static_cast<uint8_t>(triple & 0xFF));
    }

    return out;
}

} // namespace epotoken::base64
