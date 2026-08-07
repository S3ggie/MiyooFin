#ifndef MIYOOFIN_HTTP_CLIENT_HPP
#define MIYOOFIN_HTTP_CLIENT_HPP

#include <string>
#include <vector>

namespace miyoofin {

/// Result of a single HTTP request. The HTTP status code is always
/// reported even for error responses so callers can distinguish
/// transport failures from HTTP-level failures (401, 403, 500, ...).
struct HttpResponse {
    long         status = 0;   ///< HTTP status code (0 if transport error)
    std::string  body;         ///< Response body (may be empty)
    bool         ok() const { return status >= 200 && status < 300; }
};

/// Result of a binary HTTP request (e.g. image download).
struct BinaryHttpResponse {
    long                    status = 0;   ///< HTTP status code (0 if transport error)
    std::vector<unsigned char> data;      ///< Raw response bytes
    bool                    truncated = false;  ///< true if response exceeded max size
    bool ok() const { return status >= 200 && status < 300 && !truncated; }
};

/// Minimal synchronous HTTP client wrapping libcurl.
/// Supports GET and POST with optional custom headers and a JSON body.
class HttpClient {
public:
    HttpClient();

    /// Perform a GET request.
    /// @return true if the HTTP status code is 200 and the body is non-empty.
    bool get(const std::string &url,
             std::string &responseBody,
             long &httpCode,
             std::string &error);

    /// Perform a POST request with the given headers and body.
    /// @param url          Full URL to fetch.
    /// @param headers      Raw header lines (e.g. "Content-Type: application/json").
    /// @param postBody     Request body (sent as-is).
    /// @param response     Output: status + body (populated on any HTTP response).
    /// @param error        Output: human-readable error on transport failure.
    /// @return true if the request completed at the HTTP level (any status).
    bool post(const std::string &url,
              const std::vector<std::string> &headers,
              const std::string &postBody,
              HttpResponse &response,
              std::string &error);

    /// Perform a binary GET request.  The response body is stored as
    /// raw bytes.  If the response exceeds maxSize bytes the transfer
    /// is aborted and truncated is set to true.
    /// @param url          Full URL to fetch.
    /// @param headers      Optional custom headers (e.g. "Authorization: ...").
    /// @param response     Output: status + raw data.
    /// @param error        Output: human-readable error on transport failure.
    /// @param maxSize      Maximum allowed response size in bytes (default 2 MB).
    /// @return true if the request completed at the HTTP level.
    bool getBinary(const std::string &url,
                   const std::vector<std::string> &headers,
                   BinaryHttpResponse &response,
                   std::string &error,
                   size_t maxSize = 2 * 1024 * 1024);

    /// Low-level request. Performs the given HTTP method with optional
    /// headers and body. On success (HTTP request completed) returns true
    /// and fills `response.status`/`response.body` regardless of status.
    /// On transport failure returns false and sets `error`.
    bool perform(const std::string &method,
                 const std::string &url,
                 const std::vector<std::string> &headers,
                 const std::string &postBody,
                 HttpResponse &response,
                 std::string &error);

    /// Set request timeout in seconds (default 5).
    void setTimeoutSec(long sec) { m_timeoutSec = sec; }

private:
    long m_timeoutSec = 5;
};

} // namespace miyoofin

#endif // MIYOOFIN_HTTP_CLIENT_HPP