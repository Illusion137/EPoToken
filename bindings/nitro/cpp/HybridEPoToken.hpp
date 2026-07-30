#pragma once

// Hand-written HybridObject for the EPoToken Nitro module.
//
// `nitrogen` would normally generate a HybridEPoTokenSpec base class from the
// EPoToken.nitro.ts spec. To keep the repo self-contained we declare the same
// public surface directly — wire the generated base class in when integrating
// into a host RN app (replace this header / inherit from HybridEPoTokenSpec).

#include <NitroModules/HybridObject.hpp>

#include <optional>
#include <string>

namespace margelo::nitro::epotoken {

// Mirrors EPoToken.nitro.ts:ChallengeResult
struct ChallengeResult {
    std::string interpreterUrl;
    std::string interpreterJs;
    std::string program;
    std::string globalName;
};

// Mirrors EPoToken.nitro.ts:ContextOverrides (all optional from JS)
struct ContextOverrides {
    std::optional<std::string> visitorData;
    std::optional<std::string> clientVersion;
    std::optional<std::string> hl;
    std::optional<std::string> gl;
    std::optional<std::string> userAgent;
    std::optional<std::string> osName;
    std::optional<std::string> osVersion;
    std::optional<std::string> browserName;
    std::optional<std::string> browserVersion;
    std::optional<std::string> timeZone;
    std::optional<double>      utcOffsetMinutes;
};

// Mirrors EPoToken.nitro.ts:Constants
struct Constants {
    std::string userAgent;
    std::string requestKey;
    std::string googApiKey;
    std::string googBaseUrl;
    std::string createEndpoint;
    std::string generateItEndpoint;
    std::string innertubeApiKey;
    std::string innertubeBaseUrl;
    std::string innertubeClientName;
    std::string innertubeClientNameId;
    std::string innertubeClientVersion;
    std::string ytBaseUrl;
    std::string staticVisitorId;
};

class HybridEPoToken : public HybridObject {
public:
    explicit HybridEPoToken() : HybridObject(TAG) {}

    // ---- Placeholder ------------------------------------------------------
    std::string generatePlaceholderToken(const std::string& identifier,
                                         double clientState);

    // ---- Visitor data -----------------------------------------------------
    std::string generateVisitorData();
    std::string generateRandomVisitorData();
    std::string encodeVisitorData(const std::string& id, double timestamp);
    std::string decodeVisitorDataId(const std::string& visitorData);

    // ---- Request bodies ---------------------------------------------------
    std::string buildAttGetBody(const ContextOverrides& opts);
    std::string buildGenerateItBody(const std::string& snapshot);
    std::string buildChallengeCreateBody();

    // ---- Response parsers (throw std::runtime_error on parse failure) ----
    ChallengeResult parseAttResponse(const std::string& body);
    ChallengeResult parseChallengeResponse(const std::string& body);
    std::string     parseGenerateItResponse(const std::string& body);

    // ---- Misc -------------------------------------------------------------
    std::string descramble(const std::string& encoded);
    Constants   getConstants();

    // HybridObject plumbing (would normally come from nitrogen).
    void loadHybridMethods() override;

private:
    static constexpr auto TAG = "EPoToken";
};

} // namespace margelo::nitro::epotoken
