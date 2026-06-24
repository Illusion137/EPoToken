#include <epotoken.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: epotoken_cli <visitor_data> [content_binding]\n");
        return 1;
    }

    const std::string visitor_data    = argv[1];
    const std::string content_binding = argc >= 3 ? argv[2] : "";

    // ------------------------------------------------------------------
    // Full PoToken
    // ------------------------------------------------------------------
    auto result = generate_po_token(visitor_data, content_binding);

    if (auto* err = std::get_if<epotoken::error>(&result)) {
        fprintf(stderr, "ERROR [%s]: %s\n", err->code.c_str(), err->message.c_str());
        return 1;
    }

    const auto& ok = std::get<epotoken::po_token_result>(result);
    printf("po_token:             %s\n", ok.po_token.c_str());
    printf("placeholder_po_token: %s\n", ok.placeholder_po_token.c_str());
    printf("visitor_data:         %s\n", ok.visitor_data.c_str());
    printf("identifier:           %s\n", ok.identifier.c_str());

    // ------------------------------------------------------------------
    // Stand-alone placeholder token
    // ------------------------------------------------------------------
    const std::string id = content_binding.empty() ? visitor_data : content_binding;
    auto ph = generate_placeholder_token(id);
    if (auto* s = std::get_if<std::string>(&ph)) {
        printf("standalone_placeholder: %s\n", s->c_str());
    } else {
        const auto& e = std::get<epotoken::error>(ph);
        fprintf(stderr, "placeholder error: %s\n", e.message.c_str());
    }

    return 0;
}
