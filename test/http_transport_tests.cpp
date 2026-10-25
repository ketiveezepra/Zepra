/**
 * @file http_transport_tests.cpp
 * @brief HTTP/1.1 response framing, keep-alive reuse, and the nxhttp body fixes
 *
 * The framing tests feed canned bytes to the reader; the client tests talk to
 * a small server on 127.0.0.1, so none of this needs the internet.
 */

#include <gtest/gtest.h>
#include "http_client.hpp"
#include "http_response_reader.hpp"
#include "nxhttp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Zepra::Networking;

// =============================================================================
// Framing (canned bytes)
// =============================================================================

namespace {

// Serves the given bytes in pieces of at most `step` bytes, then EOF
HttpReadFn cannedReader(const std::string& bytes, size_t step, size_t* consumed = nullptr) {
    auto pos = std::make_shared<size_t>(0);
    return [bytes, step, pos, consumed](char* buf, size_t len) -> long {
        size_t n = std::min({len, step, bytes.size() - *pos});
        std::copy_n(bytes.data() + *pos, n, buf);
        *pos += n;
        if (consumed) *consumed = *pos;
        return static_cast<long>(n);
    };
}

std::string bodyOf(const HttpWireResponse& r) {
    return std::string(r.body.begin(), r.body.end());
}

} // namespace

TEST(HttpFramingTests, ContentLengthStopsAtBodyEnd) {
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(wire, 3), false, r));
    EXPECT_EQ(r.statusCode, 200);
    EXPECT_EQ(bodyOf(r), "hello");
    EXPECT_TRUE(r.complete);
    EXPECT_TRUE(r.keepAlive);
}

TEST(HttpFramingTests, DoesNotReadPastTheResponse) {
    // A reader that would block (here: fail) if asked for more than the response
    std::string wire = "HTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nok";
    size_t served = 0;
    bool askedForMore = false;
    HttpReadFn read = [&](char* buf, size_t len) -> long {
        if (served == wire.size()) { askedForMore = true; return -1; }
        size_t n = std::min(len, wire.size() - served);
        std::copy_n(wire.data() + served, n, buf);
        served += n;
        return static_cast<long>(n);
    };
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(read, false, r));
    EXPECT_EQ(bodyOf(r), "ok");
    EXPECT_FALSE(askedForMore);
}

TEST(HttpFramingTests, ChunkedBodyIsDecoded) {
    std::string wire =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;ext=1\r\nhello\r\n"
        "7\r\n, world\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n";
    for (size_t step : {1u, 4u, 4096u}) {
        HttpWireResponse r;
        ASSERT_TRUE(readHttpResponse(cannedReader(wire, step), false, r)) << step;
        EXPECT_EQ(bodyOf(r), "hello, world") << step;
        EXPECT_TRUE(r.complete);
        EXPECT_TRUE(r.keepAlive);
    }
}

TEST(HttpFramingTests, TruncatedChunkedBodyIsIncomplete) {
    std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\na\r\nhello";
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(wire, 8), false, r));
    EXPECT_EQ(bodyOf(r), "hello");
    EXPECT_FALSE(r.complete);
    EXPECT_FALSE(r.keepAlive);
}

TEST(HttpFramingTests, BodyWithoutLengthEndsAtClose) {
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the end";
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(wire, 5), false, r));
    EXPECT_EQ(bodyOf(r), "until the end");
    EXPECT_TRUE(r.complete);
    EXPECT_FALSE(r.keepAlive);
}

TEST(HttpFramingTests, NoBodyForHeadAnd204And304) {
    std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n";
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(head, 64), true, r));
    EXPECT_TRUE(r.body.empty());
    EXPECT_TRUE(r.keepAlive);

    for (const char* status : {"204 No Content", "304 Not Modified"}) {
        std::string wire = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 10\r\n\r\n";
        ASSERT_TRUE(readHttpResponse(cannedReader(wire, 64), false, r));
        EXPECT_TRUE(r.body.empty()) << status;
        EXPECT_TRUE(r.keepAlive) << status;
    }
}

TEST(HttpFramingTests, InterimResponsesAreSkipped) {
    std::string wire =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n"
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(wire, 7), false, r));
    EXPECT_EQ(r.statusCode, 201);
    EXPECT_EQ(bodyOf(r), "ok");
    EXPECT_EQ(r.header("Link"), nullptr);
}

TEST(HttpFramingTests, ConnectionCloseAndHttp10AreNotReused) {
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", 64), false, r));
    EXPECT_FALSE(r.keepAlive);

    ASSERT_TRUE(readHttpResponse(cannedReader(
        "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", 64), false, r));
    EXPECT_FALSE(r.keepAlive);

    ASSERT_TRUE(readHttpResponse(cannedReader(
        "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 0\r\n\r\n", 64), false, r));
    EXPECT_TRUE(r.keepAlive);
}

TEST(HttpFramingTests, BadContentLengthIsRejected) {
    HttpWireResponse r;
    EXPECT_FALSE(readHttpResponse(cannedReader(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 64), false, r));
    EXPECT_FALSE(readHttpResponse(cannedReader(
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", 64), false, r));
    // Identical repeated values are allowed
    EXPECT_TRUE(readHttpResponse(cannedReader(
        "HTTP/1.1 200 OK\r\nContent-Length: 2, 2\r\n\r\nok", 64), false, r));
    EXPECT_EQ(bodyOf(r), "ok");
}

TEST(HttpFramingTests, BytesAfterTheResponseBlockReuse) {
    HttpWireResponse r;
    ASSERT_TRUE(readHttpResponse(cannedReader(
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokEXTRA", 64), false, r));
    EXPECT_EQ(bodyOf(r), "ok");
    EXPECT_FALSE(r.keepAlive);
}

TEST(HttpFramingTests, GarbageAndEmptyStreamFail) {
    HttpWireResponse r;
    EXPECT_FALSE(readHttpResponse(cannedReader("", 64), false, r));
    EXPECT_EQ(r.bytesReceived, 0u);
    EXPECT_FALSE(readHttpResponse(cannedReader("SSH-2.0-OpenSSH\r\n\r\n", 64), false, r));
}

// =============================================================================
// Loopback server
// =============================================================================

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
static void closeSock(sock_t s) { close(s); }
#endif

// Answers each request with handler(requestNumber, requestLine); an empty
// answer closes the connection without responding.
class LoopbackServer {
public:
    using Handler = std::function<std::string(int, const std::string&)>;

    explicit LoopbackServer(Handler handler) : handler_(std::move(handler)) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listener_, 16);
        socklen_t len = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~LoopbackServer() {
        stopping_ = true;
        // Wake the accept() with a connection of our own, then end every
        // connection the client may still hold open in its pool
        sock_t wake = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        thread_.join();
        closeSock(wake);
        closeSock(listener_);
        // Winsock's shutdown() doesn't wake a blocked recv(); the client closing does
        HttpClient::closeIdleConnections();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (sock_t c : clients_) shutdown(c, 2);  // SD_BOTH / SHUT_RDWR
        }
        for (auto& t : workers_) t.join();
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    int connections() const { return connections_; }

private:
    void run() {
        for (;;) {
            sock_t client = accept(listener_, nullptr, nullptr);
            if (stopping_) {
                if (client != static_cast<sock_t>(-1)) closeSock(client);
                return;
            }
            if (client == static_cast<sock_t>(-1)) return;
            connections_++;
            std::lock_guard<std::mutex> lock(mutex_);
            clients_.push_back(client);
            workers_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(sock_t client) {
        std::string buf;
        char tmp[4096];
        for (;;) {
            size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                int n = recv(client, tmp, sizeof(tmp), 0);
                if (n <= 0) { closeSock(client); return; }
                buf.append(tmp, n);
            }
            std::string requestLine = buf.substr(0, buf.find("\r\n"));
            buf.erase(0, end + 4);
            std::string answer = handler_(++requests_, requestLine);
            if (answer.empty()) { closeSock(client); return; }
            send(client, answer.data(), static_cast<int>(answer.size()), 0);
            if (answer.find("Connection: close") != std::string::npos) {
                closeSock(client);
                return;
            }
        }
    }

    Handler handler_;
    sock_t listener_;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> connections_{0};
    std::atomic<int> requests_{0};
    std::mutex mutex_;
    std::thread thread_;
    std::vector<std::thread> workers_;
    std::vector<sock_t> clients_;
};

std::string okResponse(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

HttpResponse fetch(const std::string& url) {
    HttpClientConfig config;
    config.useCookies = false;
    config.readTimeoutMs = 5000;
    HttpClient client(config);  // a new client per request, like the browser's httpGet
    return client.send(HttpRequest(HttpMethod::GET, url));
}

std::string bodyOf(const HttpResponse& r) {
    return std::string(r.body().begin(), r.body().end());
}

} // namespace

class HttpClientLoopbackTests : public ::testing::Test {
protected:
    void SetUp() override { HttpClient::closeIdleConnections(); }
    void TearDown() override { HttpClient::closeIdleConnections(); }
};

TEST_F(HttpClientLoopbackTests, KeepAliveReusesOneConnection) {
    LoopbackServer server([](int n, const std::string&) {
        if (n % 2) return okResponse("response " + std::to_string(n));
        return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "9\r\nresponse \r\n1\r\n") + std::to_string(n) + "\r\n0\r\n\r\n";
    });
    for (int i = 1; i <= 4; ++i) {
        HttpResponse r = fetch(server.url("/page"));
        ASSERT_FALSE(r.hasError()) << r.error();
        EXPECT_EQ(bodyOf(r), "response " + std::to_string(i));
    }
    EXPECT_EQ(server.connections(), 1);
}

TEST_F(HttpClientLoopbackTests, ConnectionCloseIsHonoured) {
    LoopbackServer server([](int, const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok");
    });
    EXPECT_EQ(bodyOf(fetch(server.url("/"))), "ok");
    EXPECT_EQ(bodyOf(fetch(server.url("/"))), "ok");
    EXPECT_EQ(server.connections(), 2);
}

TEST_F(HttpClientLoopbackTests, StaleConnectionIsRetried) {
    // The server answers the first request, then drops the kept-alive
    // connection when the second one arrives, without answering it
    LoopbackServer server([](int n, const std::string&) {
        return n == 2 ? std::string() : okResponse("answer " + std::to_string(n));
    });
    EXPECT_EQ(bodyOf(fetch(server.url("/"))), "answer 1");
    HttpResponse r = fetch(server.url("/"));
    ASSERT_FALSE(r.hasError()) << r.error();
    EXPECT_EQ(bodyOf(r), "answer 3");
    EXPECT_EQ(server.connections(), 2);
}

TEST_F(HttpClientLoopbackTests, RelativeRedirectsAreFollowedEveryTime) {
    LoopbackServer server([](int, const std::string& line) {
        if (line.rfind("GET /old", 0) == 0) {
            return std::string("HTTP/1.1 302 Found\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n");
        }
        return okResponse("moved");
    });
    // The redirect counter used to be a static that never reset
    for (int i = 0; i < 15; ++i) {
        HttpResponse r = fetch(server.url("/old"));
        ASSERT_EQ(bodyOf(r), "moved") << "request " << i;
    }
}

// =============================================================================
// nxhttp C client (chunked responses and bodies without a length)
// =============================================================================

TEST(NxHttpClientBodyTests, ChunkedBodyIsDecoded) {
    LoopbackServer server([](int, const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                           "4\r\nnx-h\r\n3\r\nttp\r\n0\r\n\r\n");
    });
    NxHttpError err = NX_HTTP_OK;
    NxHttpResponse* res = nx_http_get(server.url("/").c_str(), &err);
    ASSERT_NE(res, nullptr) << nx_http_error_string(err);
    EXPECT_EQ(nx_http_response_status(res), 200);
    EXPECT_STREQ(nx_http_response_body_string(res), "nx-http");
    nx_http_response_free(res);
}

TEST(NxHttpClientBodyTests, BodyWithoutLengthIsRead) {
    std::string body(20000, 'z');
    LoopbackServer server([&](int, const std::string&) {
        return "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + body;
    });
    NxHttpError err = NX_HTTP_OK;
    NxHttpResponse* res = nx_http_get(server.url("/").c_str(), &err);
    ASSERT_NE(res, nullptr) << nx_http_error_string(err);
    EXPECT_EQ(nx_http_response_body_len(res), body.size());
    nx_http_response_free(res);
}
