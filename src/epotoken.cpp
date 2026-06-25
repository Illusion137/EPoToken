#include "../include/epotoken.h"

#include "constants.h"
#include "http_client.h"
#include "innertube_client.h"
#include "v8_runner.h"

// ---------------------------------------------------------------------------
// generate_po_token
// Creates a fresh Innertube WEB session, fetches the BotGuard challenge via
// /att/get, runs BotGuard in V8, and mints the PoToken.
// Falls back to the legacy jnn-pa Create endpoint if /att/get fails.
// ---------------------------------------------------------------------------

epotoken::po_token_outcome generate_po_token(const std::string& content_binding_in) {
    using namespace epotoken;
    using namespace epotoken::constants;

    // -----------------------------------------------------------------------
    // 1. Create fresh session + fetch BotGuard challenge via Innertube /att/get.
    //    Returns both the challenge and the session's visitor_data.
    // -----------------------------------------------------------------------
    std::string visitor_data;
    challenge_outcome challenge_res;

    auto att_res = get_attestation_challenge();
    if (auto* att = std::get_if<attestation_result>(&att_res)) {
        visitor_data = att->visitor_data;
        challenge_res = att->challenge;
    } else {
        // /att/get failed — fall back to legacy jnn-pa Create RPC.
        // Generate a local visitor_data so the result is still populated.
        visitor_data = generate_visitor_data();
        challenge_res = fetch_challenge();
    }

    if (auto* err = std::get_if<challenge_error>(&challenge_res)) {
        return error{err->message, "CRITICAL"};
    }
    const auto& challenge = std::get<bg_challenge>(challenge_res);

    // content_binding defaults to visitor_data when not specified
    const std::string content_binding =
        content_binding_in.empty() ? visitor_data : content_binding_in;

    // -----------------------------------------------------------------------
    // 2. Fetch BotGuard interpreter JavaScript
    // -----------------------------------------------------------------------
    http::request_options fetch_opts;
    fetch_opts.method = "GET";
    fetch_opts.headers = {
        {"user-agent", USER_AGENT},
        {"referer",    "https://www.youtube.com/"},
    };

    fprintf(stderr, "DEBUG: interpreter_url='%s'\n", challenge.interpreter_url.c_str());
    fprintf(stderr, "DEBUG: program_len=%zu  global_name='%s'\n",
            challenge.program.size(), challenge.global_name.c_str());

    auto interp_res = http::request(challenge.interpreter_url, fetch_opts);
    if (auto* err = std::get_if<http::http_error>(&interp_res)) {
        return error{"Failed to fetch interpreter JS: " + err->message, "CRITICAL"};
    }
    const auto& interp_resp = std::get<http::response>(interp_res);
    if (!interp_resp.ok()) {
        return error{
            "Interpreter JS fetch returned HTTP " +
            std::to_string(interp_resp.status), "CRITICAL"
        };
    }
    const std::string interpreter_js = interp_resp.body;
    if (interpreter_js.empty()) {
        return error{"Interpreter JS is empty", "CRITICAL"};
    }

    // -----------------------------------------------------------------------
    // 3. Create V8 runner (browser env + bgutils bundle + interpreter)
    // -----------------------------------------------------------------------
    auto runner_res = v8_runner::create_runner(
        interpreter_js, challenge.program, challenge.global_name);

    if (auto* err = std::get_if<v8_runner::run_error>(&runner_res)) {
        return error{"V8 setup failed: " + err->message, "CRITICAL"};
    }
    auto* runner = std::get<v8_runner::runner*>(runner_res);

    struct runner_guard {
        v8_runner::runner* r;
        ~runner_guard() { v8_runner::destroy_runner(r); }
    } guard{runner};

    // -----------------------------------------------------------------------
    // 4. Run BotGuard snapshot
    // -----------------------------------------------------------------------
    auto snap_res = v8_runner::run_snapshot(runner);
    if (auto* err = std::get_if<v8_runner::run_error>(&snap_res)) {
        return error{err->message, "CRITICAL"};
    }
    const std::string snapshot =
        std::get<v8_runner::snapshot_result>(snap_res).snapshot;

    // -----------------------------------------------------------------------
    // 5. POST snapshot to jnn-pa GenerateIT → integrity token
    // -----------------------------------------------------------------------
    auto gen_res = post_generate_it(snapshot);
    if (auto* err = std::get_if<challenge_error>(&gen_res)) {
        return error{err->message, "CRITICAL"};
    }
    const std::string integrity_token = std::get<std::string>(gen_res);

    // -----------------------------------------------------------------------
    // 6. Mint PoToken in V8 using WebPoMinter
    // -----------------------------------------------------------------------
    auto mint_res = v8_runner::run_mint(runner, integrity_token, content_binding);
    if (auto* err = std::get_if<v8_runner::run_error>(&mint_res)) {
        return error{err->message, "CRITICAL"};
    }
    const std::string po_token =
        std::get<v8_runner::po_token_result>(mint_res).po_token;

    // -----------------------------------------------------------------------
    // 7. Generate placeholder token
    // -----------------------------------------------------------------------
    std::string placeholder;
    auto ph_res = generate_placeholder_token(content_binding);
    if (auto* s = std::get_if<std::string>(&ph_res)) {
        placeholder = *s;
    }

    return po_token_result{
        po_token,
        placeholder,
        visitor_data,
        content_binding,
    };
}
