// HybridObject implementation. Wraps the cross-platform epotoken core.

#include "HybridEPoToken.hpp"

#include "epotoken.h"
#include "base64.h"
#include "challenge.h"
#include "constants.h"
#include "innertube_messages.h"

#include <stdexcept>

namespace margelo::nitro::epotoken {

namespace {

ChallengeResult to_result(::epotoken::challenge_outcome r, const char* ctx) {
    if (auto* ch = std::get_if<::epotoken::bg_challenge>(&r)) {
        return {ch->interpreter_url, ch->interpreter_js,
                ch->program, ch->global_name};
    }
    throw std::runtime_error(
        std::string(ctx) + ": " + std::get<::epotoken::challenge_error>(r).message);
}

} // namespace

// ---------------------------------------------------------------------------
// Placeholder
// ---------------------------------------------------------------------------

std::string HybridEPoToken::generatePlaceholderToken(
        const std::string& identifier, double clientState) {
    auto r = generate_placeholder_token(identifier,
                                        static_cast<uint8_t>(clientState));
    if (auto* s = std::get_if<std::string>(&r)) return *s;
    throw std::runtime_error(std::get<::epotoken::error>(r).message);
}

// ---------------------------------------------------------------------------
// Visitor data
// ---------------------------------------------------------------------------

std::string HybridEPoToken::generateVisitorData() {
    return ::epotoken::generate_visitor_data();
}

std::string HybridEPoToken::generateRandomVisitorData() {
    return ::epotoken::generate_random_visitor_data();
}

std::string HybridEPoToken::encodeVisitorData(
        const std::string& id, double timestamp) {
    return ::epotoken::encode_visitor_data(id, static_cast<uint32_t>(timestamp));
}

std::string HybridEPoToken::decodeVisitorDataId(
        const std::string& visitorData) {
    return ::epotoken::decode_visitor_data_id(visitorData);
}

// ---------------------------------------------------------------------------
// Request body builders
// ---------------------------------------------------------------------------

std::string HybridEPoToken::buildAttGetBody(const ContextOverrides& opts) {
    ::epotoken::innertube_context_overrides c;
    c.visitor_data       = opts.visitorData.value_or("");
    c.client_version     = opts.clientVersion.value_or("");
    c.hl                 = opts.hl.value_or("");
    c.gl                 = opts.gl.value_or("");
    c.user_agent         = opts.userAgent.value_or("");
    c.os_name            = opts.osName.value_or("");
    c.os_version         = opts.osVersion.value_or("");
    c.browser_name       = opts.browserName.value_or("");
    c.browser_version    = opts.browserVersion.value_or("");
    c.time_zone          = opts.timeZone.value_or("");
    c.utc_offset_minutes = static_cast<int>(opts.utcOffsetMinutes.value_or(0.0));
    return ::epotoken::build_att_get_body(c);
}

std::string HybridEPoToken::buildGenerateItBody(const std::string& snapshot) {
    return ::epotoken::build_generate_it_body(snapshot);
}

std::string HybridEPoToken::buildChallengeCreateBody() {
    return ::epotoken::build_challenge_create_body();
}

// ---------------------------------------------------------------------------
// Response parsers
// ---------------------------------------------------------------------------

ChallengeResult HybridEPoToken::parseAttResponse(const std::string& body) {
    return to_result(::epotoken::parse_att_response(body), "parseAttResponse");
}

ChallengeResult HybridEPoToken::parseChallengeResponse(const std::string& body) {
    return to_result(::epotoken::parse_challenge_response(body), "parseChallengeResponse");
}

std::string HybridEPoToken::parseGenerateItResponse(const std::string& body) {
    auto r = ::epotoken::parse_generate_it_response(body);
    if (auto* s = std::get_if<std::string>(&r)) return *s;
    throw std::runtime_error(
        "parseGenerateItResponse: " +
        std::get<::epotoken::challenge_error>(r).message);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

std::string HybridEPoToken::descramble(const std::string& encoded) {
    auto bytes = ::epotoken::base64::decode(encoded);
    for (auto& b : bytes) {
        b = static_cast<uint8_t>((static_cast<int>(b) + 97) & 0xFF);
    }
    return std::string(bytes.begin(), bytes.end());
}

Constants HybridEPoToken::getConstants() {
    using namespace ::epotoken::constants;
    return {
        USER_AGENT,
        REQUEST_KEY,
        GOOG_API_KEY,
        GOOG_BASE_URL,
        CREATE_ENDPOINT,
        GENERATE_IT_ENDPOINT,
        INNERTUBE_API_KEY,
        INNERTUBE_BASE_URL,
        INNERTUBE_CLIENT_NAME,
        INNERTUBE_CLIENT_NAME_ID,
        INNERTUBE_CLIENT_VERSION,
        YT_BASE_URL,
        STATIC_VISITOR_ID,
    };
}

// ---------------------------------------------------------------------------
// HybridObject method registration. nitrogen would emit this from the spec.
// ---------------------------------------------------------------------------

void HybridEPoToken::loadHybridMethods() {
    HybridObject::loadHybridMethods();
    registerHybrids(this, [](Prototype& proto) {
        proto.registerHybridMethod("generatePlaceholderToken",  &HybridEPoToken::generatePlaceholderToken);
        proto.registerHybridMethod("generateVisitorData",       &HybridEPoToken::generateVisitorData);
        proto.registerHybridMethod("generateRandomVisitorData", &HybridEPoToken::generateRandomVisitorData);
        proto.registerHybridMethod("encodeVisitorData",         &HybridEPoToken::encodeVisitorData);
        proto.registerHybridMethod("decodeVisitorDataId",       &HybridEPoToken::decodeVisitorDataId);
        proto.registerHybridMethod("buildAttGetBody",           &HybridEPoToken::buildAttGetBody);
        proto.registerHybridMethod("buildGenerateItBody",       &HybridEPoToken::buildGenerateItBody);
        proto.registerHybridMethod("buildChallengeCreateBody",  &HybridEPoToken::buildChallengeCreateBody);
        proto.registerHybridMethod("parseAttResponse",          &HybridEPoToken::parseAttResponse);
        proto.registerHybridMethod("parseChallengeResponse",    &HybridEPoToken::parseChallengeResponse);
        proto.registerHybridMethod("parseGenerateItResponse",   &HybridEPoToken::parseGenerateItResponse);
        proto.registerHybridMethod("descramble",                &HybridEPoToken::descramble);
        proto.registerHybridMethod("getConstants",              &HybridEPoToken::getConstants);
    });
}

} // namespace margelo::nitro::epotoken
