#include <epotoken.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// nsig subcommand:  epotoken_cli nsig <n> [base.js-path]
// Deciphers a stream-URL `n` parameter. With a path, reads base.js from disk
// (skips player discovery/fetch); otherwise fetches the current player.
static int run_nsig(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s nsig <n> [base.js-path]\n", argv[0]);
        return 2;
    }
    const std::string n = argv[2];

    epotoken::nsig_outcome result = [&]() -> epotoken::nsig_outcome {
        if (argc >= 4) {
            std::ifstream f(argv[3]);
            if (!f) return epotoken::error{std::string("cannot open ") + argv[3], "IO"};
            std::stringstream ss; ss << f.rdbuf();
            return decipher_nsig(n, ss.str());
        }
        return decipher_nsig(n);
    }();

    if (auto* err = std::get_if<epotoken::error>(&result)) {
        fprintf(stderr, "ERROR [%s]: %s\n", err->code.c_str(), err->message.c_str());
        return 1;
    }
    const auto& ok = std::get<epotoken::nsig_result>(result);
    printf("n:                   %s -> %s\n", n.c_str(), ok.n.c_str());
    printf("player_id:           %s\n", ok.player_id.c_str());
    printf("signature_timestamp: %d\n", ok.signature_timestamp);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "nsig") {
        return run_nsig(argc, argv);
    }

    const std::string content_binding = argc >= 2 ? argv[1] : "";
    const std::string interpreter_url = argc >= 3 ? argv[2] : "";

    auto result = interpreter_url.empty()
        ? generate_po_token(content_binding)
        : generate_po_token(content_binding, interpreter_url);

    if (auto* err = std::get_if<epotoken::error>(&result)) {
        fprintf(stderr, "ERROR [%s]: %s\n", err->code.c_str(), err->message.c_str());
        return 1;
    }

    const auto& ok = std::get<epotoken::po_token_result>(result);
    printf("po_token:             %s\n", ok.po_token.c_str());
    printf("placeholder_po_token: %s\n", ok.placeholder_po_token.c_str());
    printf("visitor_data:         %s\n", ok.visitor_data.c_str());
    printf("identifier:           %s\n", ok.identifier.c_str());

    const std::string id = content_binding.empty() ? ok.visitor_data : content_binding;
    auto ph = generate_placeholder_token(id);
    if (auto* s = std::get_if<std::string>(&ph)) {
        printf("standalone_placeholder: %s\n", s->c_str());
    } else {
        const auto& e = std::get<epotoken::error>(ph);
        fprintf(stderr, "placeholder error: %s\n", e.message.c_str());
    }

    return 0;
}
