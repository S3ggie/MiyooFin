// HttpClient: credentials never follow a redirect to another server, and response sizes are
// bounded. Uses loopback servers on two ports (two ports = two origins).
#include "test_support.hpp"

#include "../src/net/HttpClient.hpp"

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
                    const ssize_t w = ::send(client, reply.data() + sent, reply.size() - sent,
                                             MSG_NOSIGNAL);
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
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "http://a:2/y")); // other port
    CHECK(!redirectKeepsCredentialsSafe("http://a:1/x", "http://b:1/y")); // other host
    CHECK(!redirectKeepsCredentialsSafe("https://a/x", "http://a/y"));    // https -> http
    CHECK(!redirectKeepsCredentialsSafe("http://a:8096/x", "https://a/y")); // port changes
    CHECK(!redirectKeepsCredentialsSafe("https://a/x", "https://a.evil.example/y"));
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
    Server big([](const std::string&) {
        return ok(std::string(kMaxTextResponseBytes + 4096, 'x'));
    });
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
    testCredentialsStayOnTheirServer();
    testSameOriginRedirectKeepsTheToken();
    testResponseSizesAreBounded();
    return miyoofin_test::finish("http_security");
}
