#include <epotoken.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string content_binding = argc >= 2 ? argv[1] : "";

    auto result = generate_po_token(content_binding);

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
