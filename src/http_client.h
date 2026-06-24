#pragma once

#include <map>
#include <string>
#include <variant>

namespace epotoken::http {

struct request_options {
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::string body;
    long timeout_ms = 30000;
};

struct response {
    long status = 0;
    std::map<std::string, std::string> headers;
    std::string body;

    bool ok() const { return status >= 200 && status < 300; }
};

struct http_error {
    std::string message;
};

using outcome = std::variant<response, http_error>;

// Synchronous HTTP request via libcurl.
outcome request(const std::string& url, const request_options& opts = {});

} // namespace epotoken::http
