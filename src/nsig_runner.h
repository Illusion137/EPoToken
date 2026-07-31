#pragma once

#include <string>
#include <variant>

// Runs the tiny embedded decipher bundle inside a Hermes runtime. nsig
// extraction is done natively in C++ (src/nsig_extract.cpp); this runtime only
// evaluates the extracted decipher script and transforms `n`.
//
//   setup(output) once, then decipher(n) many times — the per-URL hot path.
namespace epotoken::nsig_runner {

struct run_error { std::string message; };

using decipher_outcome = std::variant<std::string, run_error>;

struct runner;

// Creates a Hermes runtime and evaluates the embedded decipher bundle.
std::variant<runner*, run_error> create_runner();

// Loads a previously extracted `output` script so decipher() can run against it.
std::variant<std::monostate, run_error> setup(runner* r, const std::string& output);

// Transforms an `n` query-parameter value using the loaded decipher script.
decipher_outcome decipher(runner* r, const std::string& n);

void destroy_runner(runner* r);

} // namespace epotoken::nsig_runner
