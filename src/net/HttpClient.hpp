#ifndef MIYOOFIN_HTTP_CLIENT_HPP
#define MIYOOFIN_HTTP_CLIENT_HPP

#include <string>

namespace miyoofin {

/// Minimal synchronous HTTP client wrapping libcurl.
/// Only supports GET requests with a timeout.
class HttpClient {
public:
    HttpClient();

    /// Perform a GET request.
    /// @param url          Full URL to fetch.
    /// @param responseBody  Output: response body if HTTP 200.
    /// @param httpCode      Output: the HTTP status code (e.g. 200, 404).
    /// @param error         Output: human-readable error on failure.
    /// @return true if the HTTP status code is 200 and the body is non-empty.
    bool get(const std::string &url,
             std::string &responseBody,
             long &httpCode,
             std::string &error);

    /// Set request timeout in seconds (default 5).
    void setTimeoutSec(long sec) { m_timeoutSec = sec; }

private:
    long m_timeoutSec = 5;
};

} // namespace miyoofin

#endif // MIYOOFIN_HTTP_CLIENT_HPP