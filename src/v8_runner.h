#pragma once

#include <functional>
#include <string>
#include <variant>

namespace epotoken::v8_runner {

struct run_error {
    std::string message;
};

struct snapshot_result {
    std::string snapshot;
    // The JS webPoSignalOutput array is kept alive in the V8 context between
    // snapshot and minting phases; it is referenced via __epo_state__ global.
};

struct po_token_result {
    std::string po_token;
};

using snapshot_outcome   = std::variant<snapshot_result, run_error>;
using po_token_outcome   = std::variant<po_token_result, run_error>;

// Holds all state for one PoToken generation run.
// Create on the stack; destroy after use (cleans up V8 isolate).
struct runner;

// Creates a new V8 isolate + context with full browser environment,
// loads the bgutils bundle, then runs the BotGuard interpreter script.
// Returns an opaque runner handle or an error.
std::variant<runner*, run_error> create_runner(
    const std::string& interpreter_js,
    const std::string& program,
    const std::string& global_name
);

// Runs the async snapshot phase; pumps the event loop until done.
snapshot_outcome run_snapshot(runner* r);

// Runs the async minting phase given the integrity token from C++.
po_token_outcome run_mint(runner* r,
                          const std::string& integrity_token,
                          const std::string& content_binding);

// Destroys the runner and its isolate.
void destroy_runner(runner* r);

// One-time V8 platform/ICU initialisation (call before first create_runner).
void init_v8(const char* exec_path = "");

} // namespace epotoken::v8_runner
