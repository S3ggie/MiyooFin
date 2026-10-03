// HttpClient: credentials never follow a redirect to another server, and response sizes are
// bounded. Uses loopback servers on two ports (two ports = two origins).
#include "test_support.hpp"

#include "../src/net/HttpClient.hpp"
#include <curl/curl.h>

#include <arpa/inet.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace miyoofin;

namespace {

class Server
{
  public:
    using Handler = std::function<std::string(const std::string& request)>;
    explicit Server(Handler handler) : m_handler(std::move(handler))
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(m_fd, 8);
        socklen_t len = sizeof(addr);
        ::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { run(); });
    }
    ~Server()
    {
        m_stop = true;
        ::shutdown(m_fd, SHUT_RDWR);
        ::close(m_fd);
        m_thread.join();
    }
    std::string base() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port);
    }
    int hits() const
    {
        return m_hits.load();
    }
    std::string lastRequest()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last;
    }

  private:
    void run()
    {
        while (!m_stop) {
            const int client = ::accept(m_fd, nullptr, nullptr);
            if (client < 0)
                return;
            char buf[4096];
            const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                const std::string request(buf, buf + n);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_last = request;
                }
                ++m_hits;
                const std::string reply = m_handler(request);
                std::size_t sent = 0;
                while (sent < reply.size()) {
                    const ssize_t w =
                        ::send(client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                    if (w <= 0)
                        break;
                    sent += static_cast<std::size_t>(w);
                }
            }
            ::close(client);
        }
    }
    Handler m_handler;
    int m_fd = -1;
    unsigned short m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_hits{0};
    std::mutex m_mutex;
    std::string m_last;
    std::thread m_thread;
};

std::string redirectTo(const std::string& location)
{
    return "HTTP/1.1 302 Found\r\nLocation: " + location +
           "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

std::string ok(const std::string& body)
{
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

const std::vector<std::string> kToken = {"X-Emby-Token: secret-token"};

void testOriginRules()
{
    CHECK_EQ(urlOrigin("https://Host.example/path?q=1"), "https://host.example:443");
    CHECK_EQ(urlOrigin("http://host.example:8096/x"), "http://host.example:8096");
    CHECK_EQ(urlOrigin("http://user:pw@host.example/x"), "http://host.example:80");
    CHECK(urlOrigin("not a url").empty());

    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "http://a:1/y"));
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "/relative"));
    CHECK(redirectKeepsCredentialsSafe("http://a/x", "https://a/y")); // http -> https, same host
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "http://a:2/y"));   // other port
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "http://b:1/y"));   // other host
    CHECK(!redirectKeepsCredentialsSafe("https://a/x", "http://a/y"));      // https -> http
    CHECK(!redirectKeepsCredentialsSafe("http://a:8096/x", "https://a/y")); // port changes
    CHECK(!redirectKeepsCredentialsSafe("https://a/x", "https://a.evil.example/y"));
}

// Locations that change origin without containing "://", and chains that get there in steps.
void testRelativeLookingRedirectsCannotChangeOrigin()
{
    // scheme-relative and path-confusion forms
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "//b:2/stolen"));
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "//b/stolen"));
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "//a:2/stolen")); // same host, other port
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "//a:1/same"));    // same origin
    CHECK(redirectKeepsCredentialsSafe("http://a/x", "//a/same"));        // default ports agree
    CHECK(redirectKeepsCredentialsSafe("http://a:80/x", "//a:443/up") ==
          false); // scheme-relative keeps http
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "/\\b:2/stolen")); // backslash tricks
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "\\\\b:2/stolen"));
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x",
                                        "http://a:1@b:2/stolen")); // user-info is not the host
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "http:b:2/stolen")); // no authority marker
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "ftp://a:1/x"));     // other scheme
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x",
                                        "//b:2/x\r\nX-Evil: 1"));           // control characters
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "//%62:2/stolen")); // encoded host
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "//b:2x/stolen"));  // malformed port
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "/next?y=1#z"));
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "next/page"));
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "?q=1"));
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "../up"));
    CHECK(redirectKeepsCredentialsSafe("http://a:1/x", "http://a:1/abs"));
    CHECK(redirectKeepsCredentialsSafe("http://a/x", "https://a/up")); // upgrade of the same host
    CHECK(!redirectKeepsCredentialsSafe("http://a/x", "https://b/up"));
    // The resolved target is what libcurl will request: it must agree with libcurl's own resolver
    // on the origin of every form above.
    const char* const bases[] = {"http://a:1/x/y", "https://a.example/p?q"};
    const char* const locations[] = {
        "//b:2/s",    "//a:1/s",          "/p",  "p",     "../p", "?z", "#f",
        "http://c/d", "https://c:8443/d", "//c", "///c/d"};
    for (const char* base : bases)
        for (const char* location : locations) {
            CURLU* u = curl_url();
            if (curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK &&
                curl_url_set(u, CURLUPART_URL, location, 0) == CURLUE_OK) {
                char* resolved = nullptr;
                CHECK(curl_url_get(u, CURLUPART_URL, &resolved, 0) == CURLUE_OK);
                // Where the two resolvers disagree the guard may only be stricter (it refuses
                // oddities such as "///c/d"), never target a different server than libcurl will.
                const std::string mine = resolveRedirectTarget(base, location);
                CHECK(mine.empty() || urlOrigin(mine) == urlOrigin(resolved));
                if (std::string(location) != "///c/d")
                    CHECK(!mine.empty()); // every ordinary form does resolve
                curl_free(resolved);
            }
            curl_url_cleanup(u);
        }
}

void testCredentialsStayOnTheirServer()
{
    Server target([](const std::string&) { return ok("target"); });
    Server origin([&](const std::string&) { return redirectTo(target.base() + "/stolen"); });

    // A request carrying the token is refused at the redirect; the other server sees nothing.
    {
        HttpClient client;
        HttpResponse response;
        std::string error;
        CHECK(!client.perform("GET", origin.base() + "/Items", kToken, "", response, error));
        CHECK(error.find("Refused a redirect") != std::string::npos);
        CHECK(target.hits() == 0);
    }
    // Same for the binary and file download paths.
    {
        HttpClient client;
        BinaryHttpResponse response;
        std::string error;
        CHECK(!client.getBinary(origin.base() + "/img", kToken, response, error, 1024));
        CHECK(error.find("Refused a redirect") != std::string::npos);
        CHECK(target.hits() == 0);
        std::string error2;
        CHECK(!client.downloadToFile(origin.base() + "/seg", kToken, "http-sec-dl.part", error2));
        CHECK(error2.find("Refused a redirect") != std::string::npos);
        CHECK(target.hits() == 0);
        ::unlink("http-sec-dl.part");
    }
    // Without credentials a redirect is still followed (nothing to protect).
    {
        HttpClient client;
        HttpResponse response;
        std::string error;
        CHECK(client.perform("GET", origin.base() + "/public", {}, "", response, error));
        CHECK(response.body == "target" && target.hits() == 1);
    }
}

// The same, end to end: every Location form below is sent by a real server; the other server and
// every transfer path must see nothing.
void testSchemeRelativeAndChainedRedirectsAreRefused()
{
    std::atomic<int> targetPort{0};
    Server target([](const std::string&) { return ok("target"); });
    targetPort = std::stoi(target.base().substr(target.base().rfind(':') + 1));
    const std::vector<std::string> forms = {
        "//127.0.0.1:" + std::to_string(targetPort.load()) + "/stolen",
        "//localhost:" + std::to_string(targetPort.load()) + "/stolen",
        "/\\127.0.0.1:" + std::to_string(targetPort.load()) + "/stolen",
    };
    for (const std::string& form : forms) {
        Server origin([&](const std::string&) { return redirectTo(form); });
        HttpClient client;
        HttpResponse response;
        std::string error;
        CHECK(!client.perform("GET", origin.base() + "/Items", kToken, "", response, error));
        BinaryHttpResponse bin;
        std::string error2;
        CHECK(!client.getBinary(origin.base() + "/img", kToken, bin, error2, 1024));
        std::string error3;
        CHECK(!client.downloadToFile(origin.base() + "/seg", kToken, "http-sec-dl2.part", error3));
        ::unlink("http-sec-dl2.part");
        // The raw-curl path the HLS segment downloader uses (and its LAN-fallback handle).
        CURL* curl = curl_easy_init();
        RedirectGuard guard;
        guard.attach(curl, origin.base() + "/segment", kToken);
        curl_easy_setopt(curl, CURLOPT_URL, (origin.base() + "/segment").c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(
            curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) { return s * n; });
        struct curl_slist* headers = curl_slist_append(nullptr, kToken[0].c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        CHECK(curl_easy_perform(curl) != CURLE_OK && guard.refused);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        CHECK(target.hits() == 0); // never reached, by any path
    }
    // A chain: same-origin hop first, then the escape.
    Server chain([&](const std::string& request) {
        if (request.compare(0, 8, "GET /hop") == 0)
            return redirectTo("//127.0.0.1:" + std::to_string(targetPort.load()) + "/stolen");
        return redirectTo("/hop");
    });
    HttpClient client;
    HttpResponse response;
    std::string error;
    CHECK(!client.perform("GET", chain.base() + "/Items", kToken, "", response, error));
    CHECK(target.hits() == 0);
    CHECK(target.lastRequest().find("secret-token") == std::string::npos);
}

void testSameOriginRedirectKeepsTheToken()
{
    Server server([](const std::string& request) {
        if (request.compare(0, 9, "GET /next") == 0)
            return ok("arrived");
        return redirectTo("/next");
    });
    HttpClient client;
    HttpResponse response;
    std::string error;
    CHECK(client.perform("GET", server.base() + "/start", kToken, "", response, error));
    CHECK(response.body == "arrived");
    CHECK(server.lastRequest().find("secret-token") != std::string::npos); // sent to its own server
}

void testResponseSizesAreBounded()
{
    // Text: more than the cap is a stable error, not an unbounded buffer.
    Server big(
        [](const std::string&) { return ok(std::string(kMaxTextResponseBytes + 4096, 'x')); });
    HttpClient client;
    HttpResponse response;
    std::string error;
    CHECK(!client.perform("GET", big.base() + "/huge", {}, "", response, error));
    CHECK_EQ(error, "Response too large");
    CHECK(response.transportCode == CURLE_FILESIZE_EXCEEDED);
    CHECK(response.body.empty());

    // Binary: over the limit is reported as truncated (with the real HTTP status), not as a
    // generic transport failure.
    Server image([](const std::string&) { return ok(std::string(8192, 'i')); });
    BinaryHttpResponse bin;
    std::string error2;
    CHECK(client.getBinary(image.base() + "/pic", {}, bin, error2, 1024));
    CHECK(bin.truncated && bin.status == 200 && bin.data.empty() && !bin.ok());
    // Within the limit it is unchanged.
    BinaryHttpResponse small;
    CHECK(client.getBinary(image.base() + "/pic", {}, small, error2, 16384));
    CHECK(small.ok() && small.data.size() == 8192);
}

} // namespace

int main()
{
    testOriginRules();
    testRelativeLookingRedirectsCannotChangeOrigin();
    testSchemeRelativeAndChainedRedirectsAreRefused();
    testCredentialsStayOnTheirServer();
    testSameOriginRedirectKeepsTheToken();
    testResponseSizesAreBounded();
    return miyoofin_test::finish("http_security");
}
