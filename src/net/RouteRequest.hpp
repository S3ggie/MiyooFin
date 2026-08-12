#ifndef MIYOOFIN_ROUTE_REQUEST_HPP
#define MIYOOFIN_ROUTE_REQUEST_HPP

#include "Session.hpp"
#include <cstdio>
#include <string>

namespace miyoofin {

// Keeps persistent identity separate from the endpoint used for one request.
// Operations must return false only for failures and place transport failures
// in error with the HttpClient's "Transport: " prefix.
class RouteRequest {
public:
    explicit RouteRequest(const Session &session) : m_session(session) {}
    const std::string &primary() const { return m_session.localServerUrl.empty() ? m_session.serverUrl : m_session.localServerUrl; }
    bool usesLan() const { return !m_session.localServerUrl.empty(); }
    static bool transportFailure(const std::string &error) { return error.compare(0, 11, "Transport: ") == 0 && error.find("Callback aborted") == std::string::npos; }

    template <typename Operation>
    bool run(Operation operation, std::string &error) const {
        if (!usesLan()) { std::printf("[Route] PUBLIC\n"); return operation(m_session.serverUrl); }
        std::printf("[Route] LAN\n");
        const bool succeeded=operation(m_session.localServerUrl);
        if (succeeded || !transportFailure(error)) return succeeded;
        std::printf("[Route] LAN failed; public fallback\n");
        error.clear();
        std::printf("[Route] PUBLIC\n");
        return operation(m_session.serverUrl);
    }

    static std::string replaceBase(const std::string &url, const std::string &from, const std::string &to) {
        return url.compare(0, from.size(), from)==0 ? to+url.substr(from.size()) : url;
    }
private:
    const Session &m_session;
};
}
#endif
