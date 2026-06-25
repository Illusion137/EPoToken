#include "../include/epotoken.h"

#include "base64.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <random>

// Pure C++ port of bgutils-js PoToken.generateColdStartToken().
epotoken::placeholder_outcome generate_placeholder_token(
        const std::string& identifier, uint8_t client_state) {
    using namespace epotoken;

    const std::vector<uint8_t> encoded(identifier.begin(), identifier.end());
    if (encoded.size() > 118) {
        return error{"Content binding is too long (max 118 UTF-8 bytes).", "BAD_INPUT"};
    }

    const uint32_t timestamp = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(0, 255);

    const std::array<uint8_t, 2> keys = {
        static_cast<uint8_t>(dist(rng)),
        static_cast<uint8_t>(dist(rng))
    };

    std::vector<uint8_t> header = {
        keys[0], keys[1],
        0,
        client_state,
        static_cast<uint8_t>((timestamp >> 24) & 0xFF),
        static_cast<uint8_t>((timestamp >> 16) & 0xFF),
        static_cast<uint8_t>((timestamp >>  8) & 0xFF),
        static_cast<uint8_t>( timestamp        & 0xFF),
    };

    const size_t payload_len = header.size() + encoded.size();
    std::vector<uint8_t> packet(2 + payload_len);
    packet[0] = 34; // 0x22
    packet[1] = static_cast<uint8_t>(payload_len);
    std::copy(header.begin(),  header.end(),  packet.begin() + 2);
    std::copy(encoded.begin(), encoded.end(), packet.begin() + 2 + header.size());

    for (size_t i = keys.size(); i < payload_len; ++i) {
        packet[2 + i] ^= packet[2 + (i % keys.size())];
    }

    return base64::encode(packet, /*url_safe=*/true);
}
