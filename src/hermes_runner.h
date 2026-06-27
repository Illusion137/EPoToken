#pragma once

#include <string>
#include <variant>

namespace epotoken::hermes_runner {

struct run_error       { std::string message; };
struct snapshot_result { std::string snapshot; };
struct po_token_result { std::string po_token; };

using snapshot_outcome = std::variant<snapshot_result, run_error>;
using po_token_outcome = std::variant<po_token_result, run_error>;

struct runner;

// Creates a Hermes runtime with full browser environment, loads the BotGuard
// bundle, then evaluates the interpreter script.
std::variant<runner*, run_error> create_runner(
    const std::string& interpreter_js,
    const std::string& program,
    const std::string& global_name);

// Runs the async snapshot phase; pumps the event loop until done.
snapshot_outcome run_snapshot(runner* r);

// Runs the async minting phase given the integrity token.
po_token_outcome run_mint(runner* r,
                          const std::string& integrity_token,
                          const std::string& content_binding);

// Destroys the runner and its Hermes runtime.
void destroy_runner(runner* r);

// No-op: Hermes requires no one-time global initialisation.
inline void init_hermes(const char* = "") {}

} // namespace epotoken::hermes_runner
