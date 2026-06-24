#include "http_client.h"

#include <algorithm>
#include <cstring>
#include <curl/curl.h>

namespace epotoken::http {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* hdrs = static_cast<std::map<std::string, std::string>*>(userdata);
    const std::string line(buffer, size * nitems);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key  = line.substr(0, colon);
        std::string val  = line.substr(colon + 1);
        // Trim whitespace
        auto trim = [](std::string& s) {
            const auto f = s.find_first_not_of(" \t\r\n");
            const auto l = s.find_last_not_of(" \t\r\n");
            s = (f == std::string::npos) ? "" : s.substr(f, l - f + 1);
        };
        trim(key); trim(val);
        // Lowercase key
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        (*hdrs)[key] = val;
    }
    return size * nitems;
}

} // namespace

outcome request(const std::string& url, const request_options& opts) {
    CURL* curl = curl_easy_init();
    if (!curl) return http_error{"curl_easy_init() failed"};

    std::string body_buf;
    std::map<std::string, std::string> resp_headers;
    response resp;

    curl_slist* header_list = nullptr;
    for (const auto& [k, v] : opts.headers) {
        header_list = curl_slist_append(header_list, (k + ": " + v).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, opts.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

    if (opts.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, opts.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(opts.body.size()));
    } else if (opts.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, opts.method.c_str());
    }

    const CURLcode rc = curl_easy_perform(curl);

    curl_slist_free_all(header_list);

    if (rc != CURLE_OK) {
        const char* msg = curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        return http_error{std::string("curl: ") + msg};
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    resp.status  = http_code;
    resp.headers = std::move(resp_headers);
    resp.body    = std::move(body_buf);
    return resp;
}

} // namespace epotoken::http
