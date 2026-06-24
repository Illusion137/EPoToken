#include "../include/epotoken.h"

#include "base64.h"
#include "challenge.h"
#include "constants.h"
#include "http_client.h"
#include "v8_runner.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <random>

// ---------------------------------------------------------------------------
// generate_placeholder_token
// Pure C++ port of bgutils-js PoToken.generateColdStartToken().
// ---------------------------------------------------------------------------

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

    // Header layout:
    //   [key0, key1,  0 (masked val always 0),  client_state,
    //    ts>>24, ts>>16, ts>>8, ts&0xFF]
    std::vector<uint8_t> header = {
        keys[0], keys[1],
        0,
        client_state,
        static_cast<uint8_t>((timestamp >> 24) & 0xFF),
        static_cast<uint8_t>((timestamp >> 16) & 0xFF),
        static_cast<uint8_t>((timestamp >>  8) & 0xFF),
        static_cast<uint8_t>( timestamp        & 0xFF),
    };

    // Packet: [0x22, payload_len, ...header, ...identifier_bytes]
    const size_t payload_len = header.size() + encoded.size();
    std::vector<uint8_t> packet(2 + payload_len);
    packet[0] = 34; // 0x22
    packet[1] = static_cast<uint8_t>(payload_len);
    std::copy(header.begin(),  header.end(),  packet.begin() + 2);
    std::copy(encoded.begin(), encoded.end(), packet.begin() + 2 + header.size());

    // XOR: payload[i] ^= payload[i % key_length]  (starting at i = key_length)
    const size_t key_len = keys.size(); // 2
    for (size_t i = key_len; i < payload_len; ++i) {
        packet[2 + i] ^= packet[2 + (i % key_len)];
    }

    return base64::encode(packet, /*url_safe=*/true);
}

// ---------------------------------------------------------------------------
// generate_po_token
// Full BotGuard flow: fetch challenge → interpreter → snapshot → GenerateIT → mint.
// ---------------------------------------------------------------------------

epotoken::po_token_outcome generate_po_token(
        const std::string& visitor_data,
        const std::string& content_binding_in) {
    using namespace epotoken;
    using namespace epotoken::constants;

    const std::string content_binding =
        content_binding_in.empty() ? visitor_data : content_binding_in;

    if (content_binding.empty() && visitor_data.empty()) {
        return error{
            "No identifier provided: both visitor_data and content_binding are empty.",
            "CRITICAL"
        };
    }

    // -----------------------------------------------------------------------
    // 1. Fetch BotGuard challenge
    // -----------------------------------------------------------------------
    auto challenge_res = fetch_challenge(visitor_data);
    if (auto* err = std::get_if<challenge_error>(&challenge_res)) {
        return error{err->message, "CRITICAL"};
    }
    const auto& challenge = std::get<bg_challenge>(challenge_res);

    // -----------------------------------------------------------------------
    // 2. Fetch BotGuard interpreter JavaScript
    // -----------------------------------------------------------------------
    http::request_options fetch_opts;
    fetch_opts.method = "GET";
    fetch_opts.headers = {
        {"user-agent", USER_AGENT},
        {"referer",    "https://www.youtube.com/"},
    };

    auto interp_res = http::request(challenge.interpreter_url, fetch_opts);
    if (auto* err = std::get_if<http::http_error>(&interp_res)) {
        return error{"Failed to fetch interpreter JS: " + err->message, "CRITICAL"};
    }
    const auto& interp_resp = std::get<http::response>(interp_res);
    if (!interp_resp.ok()) {
        return error{
            "Interpreter JS fetch returned HTTP " + std::to_string(interp_resp.status),
            "CRITICAL"
        };
    }
    const std::string interpreter_js = interp_resp.body;
    if (interpreter_js.empty()) {
        return error{"Interpreter JS is empty", "CRITICAL"};
    }

    // -----------------------------------------------------------------------
    // 3. Create V8 runner (sets up browser env, loads bgutils, runs interpreter)
    // -----------------------------------------------------------------------
    auto runner_res = v8_runner::create_runner(
        interpreter_js, challenge.program, challenge.global_name);

    if (auto* err = std::get_if<v8_runner::run_error>(&runner_res)) {
        return error{"V8 setup failed: " + err->message, "CRITICAL"};
    }
    auto* runner = std::get<v8_runner::runner*>(runner_res);

    // RAII cleanup
    struct runner_guard {
        v8_runner::runner* r;
        ~runner_guard() { v8_runner::destroy_runner(r); }
    } guard{runner};

    // -----------------------------------------------------------------------
    // 4. Run BotGuard snapshot (async, pumps event loop)
    // -----------------------------------------------------------------------
    auto snap_res = v8_runner::run_snapshot(runner);
    if (auto* err = std::get_if<v8_runner::run_error>(&snap_res)) {
        return error{err->message, "CRITICAL"};
    }
    const std::string snapshot = std::get<v8_runner::snapshot_result>(snap_res).snapshot;

    // -----------------------------------------------------------------------
    // 5. POST snapshot to GenerateIT to obtain integrity token
    // -----------------------------------------------------------------------
    nlohmann::json generate_payload = nlohmann::json::array();
    generate_payload.push_back(REQUEST_KEY);
    generate_payload.push_back(snapshot);

    http::request_options gen_opts;
    gen_opts.method = "POST";
    gen_opts.body   = generate_payload.dump();
    gen_opts.headers = {
        {"content-type",  "application/json+protobuf"},
        {"x-goog-api-key", GOOG_API_KEY},
        {"x-user-agent",  "grpc-web-javascript/0.1"},
        {"user-agent",    USER_AGENT},
    };

    auto gen_res = http::request(GENERATE_IT_ENDPOINT, gen_opts);
    if (auto* err = std::get_if<http::http_error>(&gen_res)) {
        return error{"GenerateIT HTTP error: " + err->message, "CRITICAL"};
    }
    const auto& gen_resp = std::get<http::response>(gen_res);
    if (!gen_resp.ok()) {
        return error{
            "GenerateIT returned HTTP " + std::to_string(gen_resp.status),
            "CRITICAL"
        };
    }

    nlohmann::json gen_json;
    try {
        gen_json = nlohmann::json::parse(gen_resp.body);
    } catch (...) {
        return error{"Failed to parse GenerateIT response as JSON", "CRITICAL"};
    }

    if (!gen_json.is_array() || gen_json.empty() || !gen_json[0].is_string()) {
        return error{"Could not get integrity token from GenerateIT response", "CRITICAL"};
    }
    const std::string integrity_token = gen_json[0].get<std::string>();

    // -----------------------------------------------------------------------
    // 6. Mint PoToken in V8 using WebPoMinter + webPoSignalOutput
    // -----------------------------------------------------------------------
    auto mint_res = v8_runner::run_mint(runner, integrity_token, content_binding);
    if (auto* err = std::get_if<v8_runner::run_error>(&mint_res)) {
        return error{err->message, "CRITICAL"};
    }
    const std::string po_token = std::get<v8_runner::po_token_result>(mint_res).po_token;

    // -----------------------------------------------------------------------
    // 7. Generate placeholder token (cold-start; may fail if binding > 118 bytes)
    // -----------------------------------------------------------------------
    std::string placeholder;
    auto ph_res = generate_placeholder_token(content_binding);
    if (auto* s = std::get_if<std::string>(&ph_res)) {
        placeholder = *s;
    }
    // If identifier is too long the placeholder is simply left empty,
    // matching the try/catch behaviour in potoken.node.ts.

    return po_token_result{
        po_token,
        placeholder,
        visitor_data,
        content_binding,
    };
}
