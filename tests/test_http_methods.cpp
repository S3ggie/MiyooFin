#include "test_support.hpp"

#include "../src/net/HttpClient.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace miyoofin;

// One-shot loopback server: answers a single request with 204 and returns its request line.
static std::string serveOnce(const std::string& method, bool useDelete)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listener, 1);
    socklen_t len = sizeof(addr);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/x";

    std::string requestLine;
    std::thread server([&] {
        const int client = ::accept(listener, nullptr, nullptr);
        char buf[1024];
        const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0)
            requestLine.assign(buf, buf + n);
        requestLine = requestLine.substr(0, requestLine.find("\r\n"));
        const char reply[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        ::send(client, reply, sizeof(reply) - 1, 0);
        ::close(client);
    });
    HttpClient client;
    client.setTimeoutSec(5);
    HttpResponse response;
    std::string error;
    client.perform(useDelete ? "DELETE" : method, url, {}, "", response, error);
    server.join();
    ::close(listener);
    return requestLine;
}

int main()
{
    CHECK_EQ(serveOnce("GET", true), "DELETE /x HTTP/1.1");
    CHECK_EQ(serveOnce("GET", false), "GET /x HTTP/1.1");
    CHECK_EQ(serveOnce("POST", false), "POST /x HTTP/1.1");
    return miyoofin_test::finish("http_methods");
}
