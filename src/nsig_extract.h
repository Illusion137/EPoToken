#pragma once

#include <string>

// Native C++ nsig extractor.
//
// Replaces the in-Hermes meriyah + JsAnalyzer/JsExtractor pipeline with a
// native port that parses base.js using Hermes's own C++ parser
// (hermes::parser::JSParser) and runs the dependency analysis + emission in
// C++. Parsing the full 2.5 MB base.js this way is ~0.6 s and ~75 MB (vs.
// minutes / 150 MB running the JS parser inside the interpreted VM), so no
// closure-selection/chunking is needed — the whole file is analyzed directly.
//
// Faithful port of LuanRT/YouTube.js:
//   src/utils/javascript/{JsAnalyzer,JsExtractor,matchers,helpers}.ts
namespace epotoken::nsig_extract {

struct result {
    bool        ok = false;
    std::string output;   // self-contained decipher script (run in Hermes)
    int         sts = 0;  // signatureTimestamp
    std::string error;
};

// Parses base.js and emits the nsig decipher script + signature timestamp.
result extract(const std::string& player_js);

} // namespace epotoken::nsig_extract
