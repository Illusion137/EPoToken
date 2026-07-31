#include "nsig_client.h"

#include "../include/epotoken.h"
#include "constants.h"
#include "http_client.h"
#include "nsig_extract.h"
#include "nsig_runner.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
namespace { void memdbg(const char* w) {
    if (!std::getenv("EPO_NSIG_MEMDBG")) return;
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    double curMB = 0;
#if defined(__APPLE__)
    double peakMB = ru.ru_maxrss / 1048576.0;
    mach_task_basic_info info; mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &cnt) == KERN_SUCCESS)
        curMB = info.resident_size / 1048576.0;
#else
    double peakMB = ru.ru_maxrss / 1024.0;
#endif
    std::fprintf(stderr, "[memdbg] %s  curRSS=%.1f MB  peakRSS=%.1f MB\n", w, curMB, peakMB);
} }

namespace epotoken {

namespace {

// A cached, extracted decipher script for one player version.
struct cached_player {
    std::string output;
    int         sts = 0;
};

std::mutex& cache_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, cached_player>& player_cache() {
    static std::unordered_map<std::string, cached_player> c;
    return c;
}

// Returns freed heap (the parser's AST arena) to the OS so the one-time
// extraction footprint doesn't stack with the decipher runtime's GC heap.
void release_freed_memory() {
#if defined(__APPLE__)
    malloc_zone_pressure_relief(malloc_default_zone(), 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

// Extracts `url`-style substring between two literals (mirrors YouTube.js
// getStringBetweenStrings for player id discovery).
std::string between(const std::string& s, const std::string& a, const std::string& b) {
    auto i = s.find(a);
    if (i == std::string::npos) return {};
    i += a.size();
    auto j = s.find(b, i);
    if (j == std::string::npos) return {};
    return s.substr(i, j - i);
}

// Runs extraction for a player source, memoized by `key`.
std::variant<cached_player, nsig_error> get_or_extract(
        const std::string& key, const std::string& player_js) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        auto it = player_cache().find(key);
        if (it != player_cache().end()) return it->second;
    }

    // Native extraction: parse base.js with Hermes's C++ parser and run the
    // dependency analysis in C++ (~0.7s, ~85MB for the full 2.5MB file).
    auto res = nsig_extract::extract(player_js);
    if (!res.ok)
        return nsig_error{res.error.empty() ? "nsig extraction failed" : res.error};

    cached_player cp{res.output, res.sts};
    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        player_cache()[key] = cp;
    }
    // The parser's AST arena (~72 MB for a 2.5 MB base.js) is freed now that
    // `res` was built; hand it back to the OS so it doesn't stack with the
    // decipher runtime's heap.
    memdbg("after extract");
    release_freed_memory();
    memdbg("after release");
    return cp;
}

epotoken::nsig_outcome decipher_with_cached(
        const std::string& n, const std::string& player_id, const cached_player& cp) {
    auto rr = nsig_runner::create_runner();
    if (auto* err = std::get_if<nsig_runner::run_error>(&rr))
        return error{"nsig runtime: " + err->message, "NSIG"};
    auto* runner = std::get<nsig_runner::runner*>(rr);
    struct guard { nsig_runner::runner* r; ~guard() { nsig_runner::destroy_runner(r); } } g{runner};

    auto s = nsig_runner::setup(runner, cp.output);
    if (auto* err = std::get_if<nsig_runner::run_error>(&s))
        return error{err->message, "NSIG"};

    auto d = nsig_runner::decipher(runner, n);
    if (auto* err = std::get_if<nsig_runner::run_error>(&d))
        return error{err->message, "NSIG"};

    memdbg("after decipher");
    return nsig_result{std::get<std::string>(d), player_id, cp.sts};
}

} // namespace

std::variant<std::string, nsig_error> discover_player_id() {
    http::request_options opts;
    opts.headers = {{"user-agent", constants::USER_AGENT}};
    auto res = http::request(std::string(constants::YT_BASE_URL) + "/iframe_api", opts);
    if (auto* err = std::get_if<http::http_error>(&res))
        return nsig_error{"iframe_api fetch: " + err->message};
    const auto& resp = std::get<http::response>(res);
    if (!resp.ok())
        return nsig_error{"iframe_api HTTP " + std::to_string(resp.status)};

    // Body contains a URL like ".../player/<id>/..." — extract <id>.
    std::string id = between(resp.body, "\\/player\\/", "\\/");
    if (id.empty()) id = between(resp.body, "/player/", "/");
    if (id.empty()) return nsig_error{"could not locate player id in iframe_api"};
    return id;
}

std::variant<std::string, nsig_error> fetch_player_js(const std::string& player_id) {
    const std::string url = std::string(constants::YT_BASE_URL) +
        "/s/player/" + player_id + "/player_es6.vflset/en_US/base.js";
    http::request_options opts;
    opts.headers = {{"user-agent", constants::USER_AGENT}};
    opts.timeout_ms = 60000;
    auto res = http::request(url, opts);
    if (auto* err = std::get_if<http::http_error>(&res))
        return nsig_error{"base.js fetch: " + err->message};
    const auto& resp = std::get<http::response>(res);
    if (!resp.ok())
        return nsig_error{"base.js HTTP " + std::to_string(resp.status)};
    return resp.body;
}

} // namespace epotoken

// ---------------------------------------------------------------------------
// Public API (declared in epotoken.h)
// ---------------------------------------------------------------------------

epotoken::nsig_outcome decipher_nsig(const std::string& n, const std::string& player_js) {
    using namespace epotoken;
    // Cache key: a cheap hash of the player source.
    const std::string key = "h" + std::to_string(std::hash<std::string>{}(player_js));
    auto cp = get_or_extract(key, player_js);
    if (auto* err = std::get_if<nsig_error>(&cp)) return error{err->message, "NSIG"};
    return decipher_with_cached(n, key, std::get<cached_player>(cp));
}

epotoken::nsig_outcome decipher_nsig(const std::string& n) {
    using namespace epotoken;
    auto pid = discover_player_id();
    if (auto* err = std::get_if<nsig_error>(&pid)) return error{err->message, "NSIG"};
    const std::string& player_id = std::get<std::string>(pid);

    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        auto it = player_cache().find(player_id);
        if (it != player_cache().end())
            return decipher_with_cached(n, player_id, it->second);
    }

    auto pjs = fetch_player_js(player_id);
    if (auto* err = std::get_if<nsig_error>(&pjs)) return error{err->message, "NSIG"};

    auto cp = get_or_extract(player_id, std::get<std::string>(pjs));
    if (auto* err = std::get_if<nsig_error>(&cp)) return error{err->message, "NSIG"};
    return decipher_with_cached(n, player_id, std::get<cached_player>(cp));
}
